package main

import (
	"encoding/json"
	"fmt"
	"log"
	"net/url"
	"os"
	"os/signal"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/alecthomas/kingpin/v2"
	"github.com/gorilla/websocket"
)

var (
	clioHosts = kingpin.Arg("hosts", "Clio nodes IP:port, comma separated list (i.e. 192.168.1.1:51233,192.168.1.2:51233)").Required().String()

	outputFile *os.File
)

const maxAllowedDelay = int64(3000)

type Event int

const (
	Connecting Event = iota + 1
	Connected
	Disconnected
	Reconnected
	ReconnectFailed
	Stalled
	Resumed
)

type Stat struct {
	event     Event
	node      string
	timestamp int64
	data      string
}

type Message struct {
	SigningTime int64 `json:"signing_time"`
}

func currentTimestamp() int64 {
	return time.Now().UnixMilli()
}

func writeStat(stat Stat) {
	log.Printf("[%v from %s at %d] %s\n", stat.event, stat.node, stat.timestamp, stat.data)

	_, err := outputFile.WriteString(fmt.Sprintf("%d,%d,%s,%s\n", stat.event, stat.timestamp, stat.node, stat.data))
	if err != nil {
		log.Println("failed to write csv record:", err)
	}
}

func parseTimestamp(msg []byte) (int64, error) {
	var out Message
	err := json.Unmarshal(msg, &out)
	if err != nil {
		return 0, err
	}

	return out.SigningTime, nil
}

func monitor(addr string, wg *sync.WaitGroup, stats *chan (Stat)) {
	var shouldRestart atomic.Bool
	shouldRestart.Store(true)

	defer wg.Done()

	interrupt := make(chan os.Signal, 1)
	signal.Notify(interrupt, os.Interrupt)

	u := url.URL{Scheme: "ws", Host: addr, Path: "/"}
	log.Printf("connecting to %s", u.String())

	c, _, err := websocket.DefaultDialer.Dial(u.String(), nil)
	if err != nil {
		log.Fatal("dial:", err)
	} else {
		*stats <- Stat{
			event:     Connected,
			node:      addr,
			timestamp: currentTimestamp(),
			data:      "connected",
		}
	}
	defer c.Close()

	done := make(chan struct{})

	var startTs atomic.Int64
	startTs.Store(currentTimestamp())

	var lastMessageTs atomic.Int64
	lastMessageTs.Store(startTs.Load())

	var previousInnerTs atomic.Int64
	previousInnerTs.Store(0)

	var stalledTs atomic.Int64
	stalledTs.Store(0)

	var isStalled atomic.Bool
	isStalled.Store(false)

	go func() {
		defer close(done)
		for {
			_, msg, err := c.ReadMessage()
			if err != nil {
				serr, ok := err.(*websocket.CloseError)
				if ok && serr.Code != 1000 {
					shouldRestart.Store(true)
				}

				*stats <- Stat{
					event:     Disconnected,
					node:      addr,
					timestamp: currentTimestamp(),
					data:      fmt.Sprint("disconnected: ", err),
				}
				log.Printf("[%s] read: %v\n", addr, err)

			tryAgain:
				if !shouldRestart.Load() {
					return
				} else {
					c, _, err = websocket.DefaultDialer.Dial(u.String(), nil)
					if err != nil {
						log.Println("reconnect dial:", err)
						time.Sleep(time.Second)

						*stats <- Stat{
							event:     ReconnectFailed,
							node:      addr,
							timestamp: currentTimestamp(),
							data:      fmt.Sprint("reconnect failed: ", err),
						}
						goto tryAgain
					} else {
						*stats <- Stat{
							event:     Reconnected,
							node:      addr,
							timestamp: currentTimestamp(),
							data:      "reconnected",
						}
						shouldRestart.Store(false)
					}
				}
			}

			// parse message and get latest timestamp out of the packet
			innerTs, _ := parseTimestamp(msg)

			lastMessageTs.Store(currentTimestamp())
			if isStalled.Load() {
				*stats <- Stat{
					event:     Resumed,
					node:      addr,
					timestamp: currentTimestamp(),
					data:      fmt.Sprintf("resumed after %d seconds. message ts %d vs last received %d", (lastMessageTs.Load()-stalledTs.Load())/1000, innerTs, previousInnerTs.Load()),
				}

				startTs.Store(lastMessageTs.Load())
				isStalled.Store(false)
			}

			previousInnerTs.Store(innerTs)
		}
	}()

	// subscribe to validations
	err = c.WriteMessage(websocket.TextMessage, []byte("{\"command\":\"subscribe\",\"streams\":[\"validations\"]}"))
	if err != nil {
		log.Fatalf("[%s] could not send subscription message: %v\n", addr, err)
	}

	ticker := time.NewTicker(time.Second)
	defer ticker.Stop()

	for {
		select {
		case <-done:
			if !shouldRestart.Load() {
				return
			}
		case <-ticker.C:
			if currentTimestamp()-lastMessageTs.Load() > maxAllowedDelay {
				if !isStalled.Load() {
					*stats <- Stat{
						event:     Stalled,
						node:      addr,
						timestamp: currentTimestamp(),
						data:      fmt.Sprintf("stalled after %d seconds. latest received message ts was %d", (lastMessageTs.Load()-startTs.Load())/1000, previousInnerTs.Load()),
					}

					isStalled.Store(true)
					stalledTs.Store(lastMessageTs.Load())
				}
			}
		case <-interrupt:
			if shouldRestart.Load() {
				// we are currently trying to reconnect so
				shouldRestart.Store(false) // stop retrying
			} else {
				// try disconnect gracefully
				err := c.WriteMessage(websocket.CloseMessage, websocket.FormatCloseMessage(websocket.CloseNormalClosure, ""))
				if err != nil {
					log.Printf("[%s] write close: %v\n", addr, err)
					return
				}

				select {
				case <-done:
				case <-time.After(time.Second):
				}
			}

			return
		}
	}
}

func main() {
	log.SetOutput(os.Stdout)
	kingpin.Parse()

	var err error
	outputFile, err = os.OpenFile("output.csv", os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0644)
	if err != nil {
		log.Fatal(err)
		return
	}
	defer outputFile.Close()

	var wg sync.WaitGroup
	stats := make(chan Stat)
	defer close(stats)

	go func() {
		inter := make(chan os.Signal, 1)
		signal.Notify(inter, os.Interrupt)

		for {
			select {
			case <-inter:
				return
			case stat := <-stats:
				writeStat(stat)
			}
		}
	}()

	for _, host := range strings.Split(*clioHosts, ",") {
		go monitor(host, &wg, &stats)
		wg.Add(1)
	}

	wg.Wait()
	log.Println("All done")
}
