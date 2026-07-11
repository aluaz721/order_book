cmake_minimum_required(VERSION 3.14)
project(order_book
    VERSION 0.1.0
    DESCRIPTION "My Limit Order Book implementation"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
)
FetchContent_MakeAvailable(googletest)

enable_testing()

file(GLOB_RECURSE ORDER_BOOK_SOURCES ../cpp/src/*.cpp)

add_library(order_book STATIC ${ORDER_BOOK_SOURCES})
target_include_directories(order_book PUBLIC ../cpp/include)
target_compile_features(order_book PUBLIC cxx_std_17)

add_executable(test_tree_order_book ../cpp/tests/book/test_tree_order_book.cpp)
target_link_libraries(test_tree_order_book PRIVATE order_book GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(test_tree_order_book)