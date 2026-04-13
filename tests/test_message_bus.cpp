#include "msgbus/message_bus.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "msgbus/topic_matcher.h"

using namespace msgbus;

// ---------- LockFreeQueue Tests ----------

TEST(LockFreeQueueTest, EnqueueDequeue) {
    LockFreeQueue<int> q(8);
    EXPECT_TRUE(q.try_enqueue(42));
    int val = 0;
    EXPECT_TRUE(q.try_dequeue(val));
    EXPECT_EQ(val, 42);
}

TEST(LockFreeQueueTest, FIFO) {
    LockFreeQueue<int> q(16);
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(q.try_enqueue(i));
    }
    for (int i = 0; i < 10; ++i) {
        int val = -1;
        EXPECT_TRUE(q.try_dequeue(val));
        EXPECT_EQ(val, i);
    }
}

TEST(LockFreeQueueTest, FullQueue) {
    LockFreeQueue<int> q(4); // rounds up to 4
    EXPECT_TRUE(q.try_enqueue(1));
    EXPECT_TRUE(q.try_enqueue(2));
    EXPECT_TRUE(q.try_enqueue(3));
    EXPECT_TRUE(q.try_enqueue(4));
    EXPECT_FALSE(q.try_enqueue(5)); // full
}

TEST(LockFreeQueueTest, EmptyQueue) {
    LockFreeQueue<int> q(4);
    int val = 0;
    EXPECT_FALSE(q.try_dequeue(val));
}

TEST(LockFreeQueueTest, ConcurrentEnqueueDequeue) {
    LockFreeQueue<int> q(1024);
    constexpr int N = 1000;
    std::atomic<int> sum{0};

    std::thread producer([&] {
        for (int i = 1; i <= N; ++i) {
            while (!q.try_enqueue(i)) {
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&] {
        int count = 0;
        while (count < N) {
            int val;
            if (q.try_dequeue(val)) {
                sum.fetch_add(val, std::memory_order_relaxed);
                ++count;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(sum.load(), N * (N + 1) / 2);
}

// ---------- MessageBus Basic Tests ----------

class MessageBusTest : public ::testing::Test {
protected:
    MessageBus bus;

    void SetUp() override { bus.start(); }
    void TearDown() override { bus.stop(); }
};

TEST_F(MessageBusTest, BasicPubSub) {
    std::promise<int> promise;
    auto future = promise.get_future();

    bus.subscribe<int>("test/int", [&promise](const int& val) {
        promise.set_value(val);
    });

    bus.publish<int>("test/int", 42);

    ASSERT_EQ(future.wait_for(std::chrono::seconds(1)),
              std::future_status::ready);
    EXPECT_EQ(future.get(), 42);
}

TEST_F(MessageBusTest, StringMessage) {
    std::promise<std::string> promise;
    auto future = promise.get_future();

    bus.subscribe<std::string>("test/str",
        [&promise](const std::string& val) {
            promise.set_value(val);
        });

    bus.publish<std::string>("test/str", "hello");

    ASSERT_EQ(future.wait_for(std::chrono::seconds(1)),
              std::future_status::ready);
    EXPECT_EQ(future.get(), "hello");
}

struct Point {
    double x, y;
    bool operator==(const Point& o) const { return x == o.x && y == o.y; }
};

TEST_F(MessageBusTest, CustomType) {
    std::promise<Point> promise;
    auto future = promise.get_future();

    bus.subscribe<Point>("geom/point", [&promise](const Point& p) {
        promise.set_value(p);
    });

    bus.publish<Point>("geom/point", {3.0, 4.0});

    ASSERT_EQ(future.wait_for(std::chrono::seconds(1)),
              std::future_status::ready);
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

    ASSERT_EQ(f1.wait_for(std::chrono::seconds(1)),
              std::future_status::ready);
    ASSERT_EQ(f2.wait_for(std::chrono::seconds(1)),
              std::future_status::ready);
    EXPECT_EQ(f1.get(), 99);
    EXPECT_EQ(f2.get(), 99);
}

TEST_F(MessageBusTest, Unsubscribe) {
    std::atomic<int> count{0};
    auto id = bus.subscribe<int>("unsub",
        [&count](const int&) { count.fetch_add(1); });

    bus.publish<int>("unsub", 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(count.load(), 1);

    bus.unsubscribe(id);

    bus.publish<int>("unsub", 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(count.load(), 1); // should not increase
}

TEST_F(MessageBusTest, MultipleTopics) {
    std::promise<int> p_int;
    std::promise<std::string> p_str;
    auto f_int = p_int.get_future();
    auto f_str = p_str.get_future();

    bus.subscribe<int>("topic/a", [&p_int](const int& v) {
        p_int.set_value(v);
    });
    bus.subscribe<std::string>("topic/b",
        [&p_str](const std::string& v) { p_str.set_value(v); });

    bus.publish<int>("topic/a", 10);
    bus.publish<std::string>("topic/b", "world");

    ASSERT_EQ(f_int.wait_for(std::chrono::seconds(1)),
              std::future_status::ready);
    ASSERT_EQ(f_str.wait_for(std::chrono::seconds(1)),
              std::future_status::ready);
    EXPECT_EQ(f_int.get(), 10);
    EXPECT_EQ(f_str.get(), "world");
}

TEST_F(MessageBusTest, TypeMismatch) {
    bus.subscribe<int>("typed", [](const int&) {});
    EXPECT_THROW(
        bus.subscribe<std::string>("typed", [](const std::string&) {}),
        std::runtime_error);
}

TEST_F(MessageBusTest, PublishBeforeSubscribe) {
    // Message published before any subscriber — should be dropped silently
    bus.publish<int>("late", 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::promise<int> promise;
    auto future = promise.get_future();
    bus.subscribe<int>("late", [&promise](const int& v) {
        promise.set_value(v);
    });

    bus.publish<int>("late", 2);
    ASSERT_EQ(future.wait_for(std::chrono::seconds(1)),
              std::future_status::ready);
    EXPECT_EQ(future.get(), 2);
}

// ---------- Concurrent publish test ----------

TEST_F(MessageBusTest, ConcurrentPublish) {
    constexpr int THREADS = 4;
    constexpr int PER_THREAD = 100;

    std::atomic<int> received{0};
    bus.subscribe<int>("concurrent",
        [&received](const int&) { received.fetch_add(1); });

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
    for (auto& th : threads) th.join();

    // Wait for all messages to be dispatched
    for (int i = 0; i < 100 && received.load() < THREADS * PER_THREAD; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(received.load(), THREADS * PER_THREAD);
}

// ---------- Coroutine Tests ----------

struct Task {
    struct promise_type {
        Task get_return_object() { return {}; }
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };
};

TEST_F(MessageBusTest, CoroutineAsyncWait) {
    std::promise<int> promise;
    auto future = promise.get_future();

    auto coro = [&]() -> Task {
        auto val = co_await bus.async_wait<int>("coro/test");
        promise.set_value(val);
    };
    coro();

    bus.publish<int>("coro/test", 123);

    ASSERT_EQ(future.wait_for(std::chrono::seconds(1)),
              std::future_status::ready);
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

    ASSERT_EQ(future.wait_for(std::chrono::seconds(1)),
              std::future_status::ready);
    EXPECT_EQ(future.get(), "coroutine!");
}

// ---------- Topic Matcher Tests ----------

TEST(TopicMatcherTest, ExactMatch) {
    EXPECT_TRUE(topicMatches("a/b/c", "a/b/c"));
    EXPECT_FALSE(topicMatches("a/b/c", "a/b/d"));
    EXPECT_FALSE(topicMatches("a/b", "a/b/c"));
}

TEST(TopicMatcherTest, SingleLevelWildcard) {
    EXPECT_TRUE(topicMatches("sensor/*/temp", "sensor/1/temp"));
    EXPECT_TRUE(topicMatches("sensor/*/temp", "sensor/abc/temp"));
    EXPECT_FALSE(topicMatches("sensor/*/temp", "sensor/1/2/temp"));
    EXPECT_FALSE(topicMatches("sensor/*/temp", "sensor/1/humidity"));
}

TEST(TopicMatcherTest, MultiLevelWildcard) {
    EXPECT_TRUE(topicMatches("sensor/#", "sensor/1/temp"));
    EXPECT_TRUE(topicMatches("sensor/#", "sensor"));
    EXPECT_TRUE(topicMatches("sensor/#", "sensor/a/b/c/d"));
    EXPECT_TRUE(topicMatches("#", "anything/at/all"));
    EXPECT_FALSE(topicMatches("sensor/#", "other/1"));
}

TEST(TopicMatcherTest, MixedWildcards) {
    EXPECT_TRUE(topicMatches("a/*/c/#", "a/b/c/d/e"));
    EXPECT_TRUE(topicMatches("a/*/c/#", "a/x/c"));
    EXPECT_FALSE(topicMatches("a/*/c/#", "a/b/d"));
}

TEST(TopicMatcherTest, IsWildcard) {
    EXPECT_TRUE(isWildcard("sensor/*"));
    EXPECT_TRUE(isWildcard("sensor/#"));
    EXPECT_TRUE(isWildcard("*/temp"));
    EXPECT_FALSE(isWildcard("sensor/temp"));
    EXPECT_FALSE(isWildcard("plain"));
}

// ---------- Wildcard Subscription Tests ----------

TEST_F(MessageBusTest, WildcardSingleLevel) {
    std::atomic<int> count{0};
    bus.subscribe<int>("sensor/*/temp", [&](const int&) {
        count.fetch_add(1);
    });

    bus.publish<int>("sensor/1/temp", 10);
    bus.publish<int>("sensor/2/temp", 20);
    bus.publish<int>("sensor/1/humidity", 30); // should NOT match

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(count.load(), 2);
}

TEST_F(MessageBusTest, WildcardMultiLevel) {
    std::atomic<int> count{0};
    bus.subscribe<int>("system/#", [&](const int&) {
        count.fetch_add(1);
    });

    bus.publish<int>("system/cpu", 1);
    bus.publish<int>("system/mem/used", 2);
    bus.publish<int>("system/disk/sda/read", 3);
    bus.publish<int>("other/thing", 4); // should NOT match

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(count.load(), 3);
}

TEST_F(MessageBusTest, WildcardUnsubscribe) {
    std::atomic<int> count{0};
    auto id = bus.subscribe<int>("event/#", [&](const int&) {
        count.fetch_add(1);
    });

    bus.publish<int>("event/click", 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(count.load(), 1);

    bus.unsubscribe(id);

    bus.publish<int>("event/click", 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(count.load(), 1);
}

TEST_F(MessageBusTest, WildcardAndExactCoexist) {
    std::atomic<int> exact_count{0};
    std::atomic<int> wild_count{0};

    bus.subscribe<int>("data/temp", [&](const int&) {
        exact_count.fetch_add(1);
    });
    bus.subscribe<int>("data/*", [&](const int&) {
        wild_count.fetch_add(1);
    });

    bus.publish<int>("data/temp", 42);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_EQ(exact_count.load(), 1);
    EXPECT_EQ(wild_count.load(), 1);
}

// ---------- Multi-Dispatcher Tests ----------

class MultiDispatcherTest : public ::testing::Test {
protected:
    MessageBus bus{65536, 4}; // 4 worker threads

    void SetUp() override { bus.start(); }
    void TearDown() override { bus.stop(); }
};

TEST_F(MultiDispatcherTest, BasicPubSub) {
    std::promise<int> promise;
    auto future = promise.get_future();

    bus.subscribe<int>("multi/test", [&](const int& v) {
        promise.set_value(v);
    });
    bus.publish<int>("multi/test", 99);

    ASSERT_EQ(future.wait_for(std::chrono::seconds(1)),
              std::future_status::ready);
    EXPECT_EQ(future.get(), 99);
}

TEST_F(MultiDispatcherTest, ConcurrentMultiTopic) {
    constexpr int TOPICS = 8;
    constexpr int MSGS = 100;

    std::atomic<int> received{0};
    for (int t = 0; t < TOPICS; ++t) {
        bus.subscribe<int>("mt/" + std::to_string(t), [&](const int&) {
            received.fetch_add(1);
        });
    }

    std::vector<std::thread> producers;
    for (int t = 0; t < TOPICS; ++t) {
        producers.emplace_back([&, t] {
            std::string topic = "mt/" + std::to_string(t);
            for (int i = 0; i < MSGS; ++i) {
                while (!bus.publish<int>(topic, i)) {
                    std::this_thread::yield();
                }
            }
        });
    }
    for (auto& th : producers) th.join();

    for (int i = 0; i < 200 && received.load() < TOPICS * MSGS; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(received.load(), TOPICS * MSGS);
}

TEST_F(MultiDispatcherTest, WildcardWithMultiDispatcher) {
    std::atomic<int> count{0};
    bus.subscribe<int>("sensor/#", [&](const int&) {
        count.fetch_add(1);
    });

    bus.publish<int>("sensor/a", 1);
    bus.publish<int>("sensor/b/c", 2);

    for (int i = 0; i < 100 && count.load() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(count.load(), 2);
}

TEST_F(MultiDispatcherTest, DispatcherCount) {
    EXPECT_EQ(bus.dispatcher_count(), 4u);
}
