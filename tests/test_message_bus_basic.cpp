#include "test_support.h"

struct Point {
    double x, y;
    bool operator==(const Point& o) const { return x == o.x && y == o.y; }
};

TEST_F(MessageBusTest, BasicPubSub) {
    std::promise<int> promise;
    auto future = promise.get_future();

    bus.subscribe<int>("test/int", [&promise](const int& val) { promise.set_value(val); });
    bus.publish<int>("test/int", 42);

    ASSERT_EQ(future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_EQ(future.get(), 42);
}

TEST_F(MessageBusTest, StringMessage) {
    std::promise<std::string> promise;
    auto future = promise.get_future();

    bus.subscribe<std::string>("test/str",
                               [&promise](const std::string& val) { promise.set_value(val); });

    bus.publish<std::string>("test/str", "hello");

    ASSERT_EQ(future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_EQ(future.get(), "hello");
}

TEST_F(MessageBusTest, CustomType) {
    std::promise<Point> promise;
    auto future = promise.get_future();

    bus.subscribe<Point>("geom/point", [&promise](const Point& p) { promise.set_value(p); });

    bus.publish<Point>("geom/point", {3.0, 4.0});

    ASSERT_EQ(future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    auto result = future.get();
    EXPECT_EQ(result.x, 3.0);
    EXPECT_EQ(result.y, 4.0);
}

TEST_F(MessageBusTest, MultipleSubscribers) {
    std::promise<int> p1, p2;
    auto f1 = p1.get_future();
    auto f2 = p2.get_future();

    bus.subscribe<int>("multi", [&p1](const int& v) { p1.set_value(v); });
    bus.subscribe<int>("multi", [&p2](const int& v) { p2.set_value(v); });

    bus.publish<int>("multi", 99);

    ASSERT_EQ(f1.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    ASSERT_EQ(f2.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_EQ(f1.get(), 99);
    EXPECT_EQ(f2.get(), 99);
}

TEST_F(MessageBusTest, Unsubscribe) {
    std::atomic<int> count{0};
    auto id = bus.subscribe<int>("unsub", [&count](const int&) { count.fetch_add(1); });

    bus.publish<int>("unsub", 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(count.load(), 1);

    bus.unsubscribe(id);

    bus.publish<int>("unsub", 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(count.load(), 1);
}

TEST_F(MessageBusTest, MultipleTopics) {
    std::promise<int> p_int;
    std::promise<std::string> p_str;
    auto f_int = p_int.get_future();
    auto f_str = p_str.get_future();

    bus.subscribe<int>("topic/a", [&p_int](const int& v) { p_int.set_value(v); });
    bus.subscribe<std::string>("topic/b", [&p_str](const std::string& v) { p_str.set_value(v); });

    bus.publish<int>("topic/a", 10);
    bus.publish<std::string>("topic/b", "world");

    ASSERT_EQ(f_int.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    ASSERT_EQ(f_str.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_EQ(f_int.get(), 10);
    EXPECT_EQ(f_str.get(), "world");
}

TEST_F(MessageBusTest, TypeMismatch) {
    bus.subscribe<int>("typed", [](const int&) {});
    EXPECT_THROW(bus.subscribe<std::string>("typed", [](const std::string&) {}),
                 std::runtime_error);
}

TEST_F(MessageBusTest, PublishBeforeSubscribe) {
    bus.publish<int>("late", 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::promise<int> promise;
    auto future = promise.get_future();
    bus.subscribe<int>("late", [&promise](const int& v) { promise.set_value(v); });

    bus.publish<int>("late", 2);
    ASSERT_EQ(future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_EQ(future.get(), 2);
}

TEST_F(MessageBusTest, QueueFullReturnsFalse) {
    MessageBus small_bus(4);
    int published = 0;
    for (int i = 0; i < 100; ++i) {
        if (!small_bus.publish<int>("full", i))
            break;
        ++published;
    }
    EXPECT_LT(published, 100);
    small_bus.stop();
}

TEST_F(MessageBusTest, StartStopIdempotent) {
    bus.start();
    bus.start();
    bus.stop();
    bus.stop();
    bus.stop();
}

TEST_F(MessageBusTest, StopWithoutStart) {
    MessageBus fresh_bus;
    fresh_bus.stop();
}

TEST_F(MessageBusTest, UnsubscribeInvalidId) {
    bus.unsubscribe(999999);
}

TEST_F(MessageBusTest, HandlerExceptionIsolation) {
    std::atomic<int> good_count{0};
    bus.subscribe<int>("except", [](const int&) { throw std::runtime_error("boom"); });
    bus.subscribe<int>("except", [&good_count](const int&) { good_count.fetch_add(1); });

    bus.publish<int>("except", 1);
    bus.publish<int>("except", 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(good_count.load(), 2);
}

TEST_F(MessageBusTest, ConcurrentPublish) {
    constexpr int THREADS = 4;
    constexpr int PER_THREAD = 100;

    std::atomic<int> received{0};
    bus.subscribe<int>("concurrent", [&received](const int&) { received.fetch_add(1); });

    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < PER_THREAD; ++i) {
                while (!bus.publish<int>("concurrent", t * PER_THREAD + i)) {
                    std::this_thread::yield();
                }
            }
        });
    }
    for (auto& th : threads)
        th.join();

    for (int i = 0; i < 100 && received.load() < THREADS * PER_THREAD; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(received.load(), THREADS * PER_THREAD);
}

TEST_F(MessageBusTest, PublishWithStringView) {
    std::promise<int> promise;
    auto future = promise.get_future();

    std::string_view topic_sv = "sv/test";
    bus.subscribe<int>(topic_sv, [&](const int& v) { promise.set_value(v); });
    bus.publish<int>(topic_sv, 77);

    ASSERT_EQ(future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_EQ(future.get(), 77);
}

TEST_F(MessageBusTest, PublishWithCharLiteral) {
    std::promise<int> promise;
    auto future = promise.get_future();

    bus.subscribe<int>("lit/test", [&](const int& v) { promise.set_value(v); });
    bus.publish<int>("lit/test", 88);

    ASSERT_EQ(future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_EQ(future.get(), 88);
}

TEST_F(MessageBusTest, ConcurrentSubscribeSameTopic) {
    constexpr int THREADS = 16;
    constexpr int ROUNDS = 5;
    for (int r = 0; r < ROUNDS; ++r) {
        std::string topic = "concurrent/sub/" + std::to_string(r);
        std::atomic<int> count{0};
        std::vector<std::thread> threads;
        std::vector<SubscriptionId> ids(THREADS);

        for (int t = 0; t < THREADS; ++t) {
            threads.emplace_back([&, t] {
                ids[t] = bus.subscribe<int>(topic, [&](const int&) { count.fetch_add(1); });
            });
        }
        for (auto& th : threads)
            th.join();

        bus.publish<int>(topic, 42);
        for (int i = 0; i < 100 && count.load() < THREADS; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        EXPECT_EQ(count.load(), THREADS);
    }
}