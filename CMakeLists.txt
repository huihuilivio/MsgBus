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

# GCC/Clang may need -fcoroutines for C++20 coroutine support
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(msgbus INTERFACE -fcoroutines)
endif()

# Code coverage (GCC/Clang only)
option(MSGBUS_COVERAGE "Enable code coverage instrumentation" OFF)
if(MSGBUS_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(msgbus INTERFACE --coverage -fprofile-arcs -ftest-coverage -fprofile-update=atomic)
        target_link_options(msgbus INTERFACE --coverage)
    else()
        message(WARNING "Code coverage is only supported with GCC/Clang")
    endif()
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
