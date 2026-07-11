# Limit Order Book Engine

## Tech Stack
### Languages & Compilers
![C++20](https://img.shields.io/badge/C++-20-00599C.svg?logo=c%2B%2B&logoColor=white)
![Python](https://img.shields.io/badge/Python-3.11-blue.svg?logo=python&logoColor=white)
![JavaScript]
![CMake](https://img.shields.io/badge/CMake-064F8C.svg?logo=cmake&logoColor=white)
![GCC](https://img.shields.io/badge/GCC-11+-blue.svg?logo=gnu&logoColor=white)
![Clang](https://img.shields.io/badge/Clang-14+-orange.svg?logo=llvm&logoColor=white)

### Containers & Release
![Docker](https://img.shields.io/badge/Docker-Container-blue.svg?logo=docker)

### Systems & Infra
![Linux](https://img.shields.io/badge/Linux-OS-FCC624.svg?logo=linux&logoColor=black)
![Ubuntu](https://img.shields.io/badge/Ubuntu-26.04-E95420.svg?logo=ubuntu&logoColor=white)
![macOS](https://img.shields.io/badge/macOS-Profiler-999999.svg?logo=apple&logoColor=white)


## Repo Layout
```text
cpp/
  include/order_book/
    book/
        price_level_interface.hpp
        array_price_level.hpp
        linked_list_price_level.hpp
        order_book_interface.hpp
        tree_order_book.hpp
        vector_order_book.hpp
    core/
        event.hpp
        order.hpp
        price_utils.hpp
    engine/
        simulation_config.hpp
        simulation_engine.hpp
    matching/
        fifo_matcher.hpp
        matching_algorithm.hpp
        pro_rata_matcher.hpp
    orders/
        order_validators.hpp
        stop_order_manager.hpp
    sources/
        order_source.hpp
        interactive_source.hpp
        historical_replayer.hpp
        strategy_source.hpp
  src/
    book/
        array_price_level.cpp
        linked_list_price_level.cpp
        tree_order_book.cpp
        vector_order_book.cpp
    engine/
        simulation_engine.cpp
    matching/
        fifo_matcher.cpp
    orders/
        stop_order_manager.cpp
    sources/
        interactive_source.cpp
  tests/
    book/
        test_tree_order_book.cpp
  benchmarks/
```

## Architecture

## Engine API
Supported order types: Market, Market limit, Limit, Stop, Stop Limit, Immediate-or-Cancel (IOC), Fill-or-Kill (FOK) 

## Build and Run

### Run without containerization

### Docker

Build and run the app with Docker Compose:

```bash
docker compose up --build
```

- Backend: http://localhost:8000
- Frontend: http://localhost:5173

### GitHub Actions

### Unit Tests
63 Google Test tests to validate core engine functionality.

```bash
cmake
ctest --test-dir build
```

### Benchmark (CSV + Histogram)
```bash
./build/cpp/bench_tool --msgs 2000000 --warmup 50000 \
  --out-csv docs/bench.csv --hist docs/hist.csv
```
Example output:
```text
msgs=2000000, time=0.156s, rate=12854458.3 msgs/s
latency_us: p50=0.04 p90=0.08 p99=0.08 p99.9=0.12
```
See `docs/bench.md`, `docs/bench.csv`, and `docs/hist.csv` for reproducible results.


### Replay from Snapshot
