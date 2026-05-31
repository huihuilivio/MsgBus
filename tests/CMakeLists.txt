include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
# Prevent overriding parent project's compiler/linker settings on Windows
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(msgbus_tests
    test_topic_registry.cpp
    test_message_ptr.cpp
    test_object_pool.cpp
    test_lock_free_queue.cpp
    test_topic_matcher.cpp
    test_topic_slot.cpp
    test_wildcard_trie.cpp
    test_message_bus_basic.cpp
    test_message_bus_wildcard.cpp
    test_message_bus_policies.cpp
    test_message_bus_coroutine.cpp
    test_message_bus_multi_dispatcher.cpp
)
target_link_libraries(msgbus_tests PRIVATE msgbus GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(msgbus_tests)

# Performance benchmark
add_executable(msgbus_bench bench_message_bus.cpp)
target_link_libraries(msgbus_bench PRIVATE msgbus)
