cmake_minimum_required(VERSION 3.20)
project(MsgBus VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Header-only library
add_library(msgbus INTERFACE)
target_include_directories(msgbus INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# GCC needs -fcoroutines for C++20 coroutine support
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(msgbus INTERFACE -fcoroutines)
endif()

# Examples
option(MSGBUS_BUILD_EXAMPLES "Build examples" ON)
if(MSGBUS_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Tests
option(MSGBUS_BUILD_TESTS "Build tests" ON)
if(MSGBUS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
