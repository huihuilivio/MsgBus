include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
# Prevent overriding parent project's compiler/linker settings on Windows
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(msgbus_tests test_message_bus.cpp)
target_link_libraries(msgbus_tests PRIVATE msgbus GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(msgbus_tests)

# Performance benchmark
add_executable(msgbus_bench bench_message_bus.cpp)
target_link_libraries(msgbus_bench PRIVATE msgbus)
