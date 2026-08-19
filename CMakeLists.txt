cmake_minimum_required(VERSION 3.16)
project(rate_limiter VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

include(FetchContent)

# JSON library for HTTP payloads
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(json)

# Modern C++ HTTP Server library
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.15.3
)
FetchContent_MakeAvailable(httplib)

# YAML parser for config.yaml
FetchContent_Declare(
    yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG 0.8.0
)
FetchContent_MakeAvailable(yaml-cpp)

# Google Test framework
enable_testing()
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
FetchContent_MakeAvailable(googletest)

# Core library source files
set(LIB_SOURCES
    src/config.cpp
    src/token_bucket.cpp
    src/rate_limiter.cpp
    src/server.cpp
)

add_library(rate_limiter_lib ${LIB_SOURCES})

target_include_directories(rate_limiter_lib PUBLIC src)

target_link_libraries(rate_limiter_lib PUBLIC
    nlohmann_json::nlohmann_json
    httplib::httplib
    yaml-cpp::yaml-cpp
    Threads::Threads
)

# Main HTTP service executable
add_executable(rate_limiter src/main.cpp)
target_link_libraries(rate_limiter PRIVATE rate_limiter_lib)

# Unit test binary
add_executable(rate_limiter_test tests/rate_limiter_test.cpp)
target_link_libraries(rate_limiter_test PRIVATE rate_limiter_lib GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(rate_limiter_test)

# Benchmark binary
add_executable(benchmark benchmarks/benchmark.cpp)
target_link_libraries(benchmark PRIVATE rate_limiter_lib)