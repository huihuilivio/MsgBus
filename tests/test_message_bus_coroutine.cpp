#include "test_support.h"

TEST_F(MessageBusTest, CoroutineAsyncWait) {
    std::promise<int> promise;
    auto future = promise.get_future();

    auto coro = [&]() -> Task {
        auto val = co_await bus.async_wait<int>("coro/test");
        promise.set_value(val);
    };
    coro();

    bus.publish<int>("coro/test", 123);

    ASSERT_EQ(future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_EQ(future.get(), 123);
}

TEST_F(MessageBusTest, CoroutineAsyncWaitString) {
    std::promise<std::string> promise;
    auto future = promise.get_future();

    auto coro = [&]() -> Task {
        auto val = co_await bus.async_wait<std::string>("coro/str");
        promise.set_value(val);
    };
    coro();

    bus.publish<std::string>("coro/str", "coroutine!");

    ASSERT_EQ(future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_EQ(future.get(), "coroutine!");
}

TEST_F(MessageBusTest, CoroutineAwaitableDestroyedBeforeMessage) {
    {
        auto task = [&]() -> DestroyableTask {
            co_await bus.async_wait<int>("coro/no_msg");
        };
        auto t = task();
    }

    std::promise<int> p;
    auto f = p.get_future();
    bus.subscribe<int>("coro/after", [&](const int& v) { p.set_value(v); });
    bus.publish<int>("coro/after", 42);
    ASSERT_EQ(f.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_EQ(f.get(), 42);
}

TEST(CoroutineTest, AsyncWaitDuplicateFireGuard) {
    MessageBus bus(65536, 4);
    bus.start();

    std::atomic<int> fire_count{0};
    std::promise<int> promise;
    auto future = promise.get_future();

    auto coro = [&]() -> Task {
        auto val = co_await bus.async_wait<int>("dup/#");
        fire_count.fetch_add(1, std::memory_order_relaxed);
        promise.set_value(val);
    };
    coro();

    for (int i = 0; i < 10; ++i) {
        bus.publish<int>("dup/" + std::to_string(i), i);
    }

    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(fire_count.load(), 1);
    bus.stop();
}