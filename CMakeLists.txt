cmake_minimum_required(VERSION 3.15)
project(order_book
    VERSION 0.1.0
    DESCRIPTION "My Limit Order Book implementation"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ORDER_BOOK_BUILD_TESTS "Build the GoogleTest suite" ON)
option(ORDER_BOOK_BUILD_BENCHMARKS "Build the Google Benchmark suite" ON)
option(ORDER_BOOK_BUILD_PYTHON "Build the pybind11 Python bindings" ON)

include(FetchContent)

if(ORDER_BOOK_BUILD_TESTS)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
  )
  FetchContent_MakeAvailable(googletest)
endif()

if(ORDER_BOOK_BUILD_BENCHMARKS)
  FetchContent_Declare(
    benchmark
    URL https://github.com/google/benchmark/archive/v1.7.1.zip
  )
  FetchContent_MakeAvailable(benchmark)
endif()

enable_testing()

file(GLOB_RECURSE ORDER_BOOK_SOURCES cpp/src/*.cpp)

add_library(order_book STATIC ${ORDER_BOOK_SOURCES})
target_include_directories(order_book PUBLIC cpp/include)
target_compile_features(order_book PUBLIC cxx_std_17)
set_target_properties(order_book PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(ORDER_BOOK_BUILD_TESTS)
  add_executable(test_tree_order_book cpp/tests/book/test_tree_order_book.cpp)
  target_link_libraries(test_tree_order_book PRIVATE order_book GTest::gtest_main)

  add_executable(test_historical_replayer cpp/tests/sources/test_historical_replayer.cpp)
  target_link_libraries(test_historical_replayer PRIVATE order_book GTest::gtest_main)

  include(GoogleTest)
  gtest_discover_tests(test_tree_order_book)
  gtest_discover_tests(test_historical_replayer)
endif()

if(ORDER_BOOK_BUILD_BENCHMARKS)
  add_executable(bench_order_book cpp/benchmarks/bench_order_book.cpp)
  target_link_libraries(bench_order_book PRIVATE order_book benchmark)
endif()

if(ORDER_BOOK_BUILD_PYTHON)
  FetchContent_Declare(
    pybind11
    URL https://github.com/pybind/pybind11/archive/refs/tags/v3.0.4.zip
  )
  FetchContent_MakeAvailable(pybind11)

  # Built as order_book/_order_book.*.so — the private compiled submodule
  # imported by python/order_book/__init__.py. The DESTINATION here (relative
  # to the wheel's platlib root) must match that package layout so it lands
  # next to __init__.py, py.typed, and features/ (copied verbatim by
  # scikit-build-core's wheel.packages — see pyproject.toml).
  pybind11_add_module(order_book_python bindings/python/order_book_bindings.cpp)
  target_link_libraries(order_book_python PRIVATE order_book)
  set_target_properties(order_book_python PROPERTIES OUTPUT_NAME _order_book)
  install(TARGETS order_book_python LIBRARY DESTINATION order_book)
endif()
