//------------------------------------------------------------------------------
/*
    This file is part of clio: https://github.com/XRPLF/clio
    Copyright (c) 2023, the clio developers.

    Permission to use, copy, modify, and distribute this software for any
    purpose with or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL,  DIRECT,  INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <util/Fixtures.h>

#include <data/CassandraBackend.h>
#include <data/cassandra/Handle.h>
#include <data/cassandra/SettingsProvider.h>
#include <data/cassandra/impl/ExecutionStrategy.h>

#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/json.hpp>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <future>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

using namespace util;

struct PlaygroundTest : public NoLoggerFixture
{
};

namespace test::detail {
class Tracker
{
    using DataType = std::tuple<std::size_t, std::chrono::steady_clock::time_point>;
    std::thread t_;
    std::mutex mtx_;
    std::atomic_bool stopping_ = false;
    DataType first_ = {0, std::chrono::steady_clock::now()};
    DataType latest_ = {0, std::chrono::steady_clock::now()};

public:
    Tracker()
        : t_([this] {
            while (!stopping_)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{100});
                std::lock_guard l(mtx_);
                if (std::chrono::steady_clock::now() - std::get<1>(latest_) >= std::chrono::seconds{1})
                {
                    std::cout << "detected lock: " << std::get<0>(first_) << "; " << std::get<0>(latest_) << std::endl;
                }
            }
        })
    {
    }

    ~Tracker()
    {
        stopping_ = true;
        if (t_.joinable())
            t_.join();
    }

    void
    track(std::size_t id)
    {
        std::lock_guard l(mtx_);
        latest_ = {id, std::chrono::steady_clock::now()};
    }

    void
    first(std::size_t id)
    {
        std::lock_guard l(mtx_);
        first_ = {id, std::chrono::steady_clock::now()};
    }
};

template <class FnType>
auto
synchronous(FnType&& func)
{
    boost::asio::io_context ctx;

    using R = typename boost::result_of<FnType(boost::asio::yield_context)>::type;
    if constexpr (!std::is_same<R, void>::value)
    {
        R res;
        boost::asio::spawn(
            ctx, [_ = boost::asio::make_work_guard(ctx), &func, &res](auto yield) { res = func(yield); });

        ctx.run();
        return res;
    }
    else
    {
        boost::asio::spawn(ctx, [_ = boost::asio::make_work_guard(ctx), &func](auto yield) { func(yield); });
        ctx.run();
    }
}

class Future
{
    using CbType = std::function<void(std::string)>;
    using CbPtrType = std::unique_ptr<CbType>;

    CbPtrType cb_;

public:
    Future(CbType&& cb, auto& ctx) : cb_{std::make_unique<CbType>(std::move(cb))}
    {
        boost::asio::post(ctx, [cb = *cb_] {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
            cb("pls send halp :D");
        });
    }
    Future(Future const&) = delete;
    Future(Future&&) = default;
};

}  // namespace test::detail

auto
readFake(test::detail::Tracker& track, auto& ctx, boost::asio::yield_context yield)
{
    static std::size_t _id = 0u;
    auto future = std::optional<test::detail::Future>{};

    auto init = [&track, &future, &ctx, id = _id++]<typename Self>(Self& self) {
        auto executor = boost::asio::get_associated_executor(self);
        auto sself = std::make_shared<Self>(std::move(self));

        auto ft = test::detail::Future{
            [_ = boost::asio::make_work_guard(executor), executor, &track, sself = std::move(sself), id](auto&& data) {
                boost::asio::post(executor, [&track, data = std::move(data), sself = std::move(sself), id]() mutable {
                    track.track(id);
                    sself->complete(std::move(data));
                });
            },
            ctx};
        future.emplace(std::move(ft));  // make sure we move like in real code
    };

    auto res = boost::asio::async_compose<decltype(yield), void(std::string)>(
        init, yield, boost::asio::get_associated_executor(yield));

    return res;
}

auto
readReal(test::detail::Tracker& track, auto& handle, auto const& statement, boost::asio::yield_context yield)
{
    static std::size_t _id = 0u;
    auto future = std::optional<data::cassandra::FutureWithCallback>{};

    auto init = [&track, &handle, &statement, &future, id = _id++]<typename Self>(Self& self) {
        auto executor = boost::asio::get_associated_executor(self);

        // using this instead fixes/masks the issue:
        // auto executor = boost::asio::system_executor{};

        auto sself = std::make_shared<Self>(std::move(self));

        future.emplace(handle.asyncExecute(
            statement, [_ = boost::asio::make_work_guard(executor), &track, sself = std::move(sself), id](auto&& data) {
                track.first(id);
                auto executor = boost::asio::get_associated_executor(*sself);
                boost::asio::post(
                    executor,
                    [_ = boost::asio::make_work_guard(executor),
                     &track,
                     data = std::move(data),
                     sself = std::move(sself),
                     id]() mutable {
                        track.track(id);
                        sself->complete(std::move(data));
                    });
            }));
    };

    auto res = boost::asio::async_compose<decltype(yield), void(data::cassandra::ResultOrError)>(
        init, yield, boost::asio::get_associated_executor(yield));

    return res;
}

TEST_F(PlaygroundTest, Real)
{
    using namespace data::cassandra;
    constexpr static auto contactPoints = "127.0.0.1";
    constexpr static auto keyspace = "test";

    Config cfg{boost::json::parse(fmt::format(
        R"JSON({{
            "contact_points": "{}",
            "keyspace": "{}",
            "replication_factor": 1,
            "max_write_requests_outstanding": 1000,
            "max_read_requests_outstanding": 100000,
            "threads": 4
        }})JSON",
        contactPoints,
        keyspace))};
    SettingsProvider settingsProvider{cfg, 0};
    auto settings = settingsProvider.getSettings();
    auto handle = Handle{settings};

    if (auto const res = handle.connect(); not res)
        throw std::runtime_error("Could not connect to Cassandra: " + res.error());

    auto schema = Schema{settingsProvider};
    if (auto res = handle.execute(schema.createKeyspace); not res)
        throw std::runtime_error("oops: " + res.error());
    if (auto res = handle.executeEach(schema.createSchema); not res)
        throw std::runtime_error("oops: " + res.error());
    schema.prepareStatements(handle);

    auto statement = schema->selectLedgerRange.bind();

    test::detail::Tracker track;
    std::atomic_uint32_t callCount = 0u;

    constexpr auto TOTAL = 10'000u;

    // emulate ETL monitor loop
    auto t = std::thread([&track, &handle, &statement, &callCount] {
        for (auto i = 0u; i < TOTAL; ++i)
        {
            // turns coroutine code into a synchronous call. this is how ETL monitoring works today
            test::detail::synchronous([&track, &handle, &statement, &callCount](auto yield) {
                auto res = readReal(track, handle, statement, yield);
                ++callCount;

                if (callCount % 500 == 0)
                    std::cout << " + calls: " << callCount.load() << std::endl;
            });
        }
    });

    if (t.joinable())
        t.join();

    EXPECT_TRUE(callCount == TOTAL);
    std::cout << "done." << std::endl;
}

TEST_F(PlaygroundTest, Fake)
{
    constexpr auto TOTAL = 10'000u;

    boost::asio::thread_pool pool{4};
    test::detail::Tracker track;
    std::atomic_uint32_t callCount = 0u;

    // emulate ETL monitor loop
    auto t = std::thread([&track, &pool, &callCount] {
        for (auto i = 0u; i < TOTAL; ++i)
        {
            // turns coroutine code into a synchronous call. this is how ETL monitoring works today
            test::detail::synchronous([&track, &pool, &callCount](auto yield) {
                auto res = readFake(track, pool, yield);
                ++callCount;

                if (callCount % 500 == 0)
                    std::cout << " + calls: " << callCount.load() << std::endl;
            });
        }
    });

    if (t.joinable())
        t.join();

    pool.join();

    EXPECT_TRUE(callCount == TOTAL);
    std::cout << "done." << std::endl;
}
