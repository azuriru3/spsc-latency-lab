cmake_minimum_required(VERSION 3.16)
project(spsc_latency_lab CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
    add_compile_options(/O2 /W4)
else()
    add_compile_options(-O3 -Wall -Wextra -Wno-interference-size)
endif()

find_package(Threads REQUIRED)

add_executable(bench_latency src/bench_latency.cpp)
target_include_directories(bench_latency PRIVATE include)
target_link_libraries(bench_latency PRIVATE Threads::Threads)

enable_testing()
add_executable(test_correctness tests/test_correctness.cpp)
target_include_directories(test_correctness PRIVATE include)
target_link_libraries(test_correctness PRIVATE Threads::Threads)
add_test(NAME correctness COMMAND test_correctness)
