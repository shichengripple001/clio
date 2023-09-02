## Cassandra/scylladb

Run local scylladb via docker: `docker run --rm -p 9042:9042 --name clio-db -d scylladb/scylla --memory 16G`

Allow scylladb to startup, takes a few minutes.

Alternatively you can run cassandra: `docker run --rm --name clio-db -d -p 9042:9042 cassandra:4.0.4`

### Setup data to read from DB

cqlsh into the db with `docker exec -it clio-db cqlsh`

Then create the keyspace and table like so:
```
create keyspace test with replication = {'class': 'SimpleStrategy', 'replication_factor': '1'} and durable_writes = true;
create table test.ledger_range (is_latest boolean primary key, sequence bigint);
update test.ledger_range set sequence = 2 where is_latest = true;
update test.ledger_range set sequence = 1 where is_latest = false;
```

## Setup conan

Clio needs conan 1.x, take a look at the `Conan configuration` section in the main README.md. Here are the important bits: 

### Conan configuration

Clio does not require anything but default settings in your (`~/.conan/profiles/default`) Conan profile. It's best to have no extra flags specified. 

on Linux:
```
[settings]
os=Linux
os_build=Linux
arch=x86_64
arch_build=x86_64
compiler=gcc
compiler.version=11
compiler.libcxx=libstdc++11
build_type=Release
compiler.cppstd=20
```

### Artifactory

1. Make sure artifactory is setup with Conan
```sh
conan remote add --insert 0 conan-non-prod http://18.143.149.228:8081/artifactory/api/conan/conan-non-prod
```
Now you should be able to download prebuilt `xrpl` package on some platforms.

2. Remove old packages you may have cached: 
```sh 
conan remove -f xrpl
conan remove -f cassandra-cpp-driver
```

## Back to code

### Building the project
```
mkdir build && cd build
conan install .. --output-folder . --build missing --settings build_type=Release -o tests=True
cmake -DCMAKE_TOOLCHAIN_FILE:FILEPATH=build/generators/conan_toolchain.cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --parallel 8
```

### unittests/Playground.cpp

I have added two tests, one queries the real DB (and also gets stuck in the process) and the other one is faking it and works fine everytime (for me at least).

I added the `_ = boost::asio::make_work_guard(executor)` because i had crashes on linux before if i don't. 

### Running the tests

`./clio_tests --gtest_filter="Play*Fake"` runs the fake that works.
`./clio_tests --gtest_filter="Play*Real"` for the real thing that does not work so well :)

