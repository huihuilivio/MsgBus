#include "test_support.h"

TEST_F(MultiDispatcherTest, BasicPubSub) {
    std::promise<int> promise;
    auto future = promise.get_future();

    bus.subscribe<int>("multi/test", [&](const int& v) { promise.set_value(v); });
    bus.publish<int>("multi/test", 99);

    ASSERT_EQ(future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_EQ(future.get(), 99);
}

TEST_F(MultiDispatcherTest, ConcurrentMultiTopic) {
    constexpr int TOPICS = 8;
    constexpr int MSGS = 100;

    std::atomic<int> received{0};
    for (int t = 0; t < TOPICS; ++t) {
        bus.subscribe<int>("mt/" + std::to_string(t), [&](const int&) { received.fetch_add(1); });
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
    for (auto& th : producers)
        th.join();

    for (int i = 0; i < 200 && received.load() < TOPICS * MSGS; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(received.load(), TOPICS * MSGS);
}

TEST_F(MultiDispatcherTest, WildcardWithMultiDispatcher) {
    std::atomic<int> count{0};
    bus.subscribe<int>("sensor/#", [&](const int&) { count.fetch_add(1); });

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

TEST_F(MultiDispatcherTest, SameTopicOrdering) {
    constexpr int N = 200;
    std::vector<int> received;
    received.reserve(N);
    std::mutex mtx;

    bus.subscribe<int>("order/test", [&](const int& v) {
        std::lock_guard<std::mutex> lock(mtx);
        received.push_back(v);
    });

    for (int i = 0; i < N; ++i) {
        while (!bus.publish<int>("order/test", i)) {
            std::this_thread::yield();
        }
    }

    for (int i = 0; i < 200 && static_cast<int>(received.size()) < N; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::lock_guard<std::mutex> lock(mtx);
    ASSERT_EQ(static_cast<int>(received.size()), N);
    for (int i = 0; i < N; ++i) {
        EXPECT_EQ(received[i], i) << "Out of order at index " << i;
    }
}

TEST_F(MultiDispatcherTest, AutoDispatcherCount) {
    MessageBus auto_bus(65536, 0);
    EXPECT_GE(auto_bus.dispatcher_count(), 1u);
}

TEST_F(MultiDispatcherTest, StopDrainsAllMessages) {
    std::atomic<int> received{0};
    bus.subscribe<int>("drain/test", [&](const int&) { received.fetch_add(1); });

    constexpr int N = 200;
    for (int i = 0; i < N; ++i) {
        while (!bus.publish<int>("drain/test", i)) {
            std::this_thread::yield();
        }
    }

    bus.stop();

    EXPECT_EQ(received.load(), N);
}

TEST_F(MultiDispatcherTest, RestartAfterStop) {
    bus.stop();
    bus.start();

    std::promise<int> promise;
    auto future = promise.get_future();

    bus.subscribe<int>("restart/test", [&](const int& v) { promise.set_value(v); });
    bus.publish<int>("restart/test", 55);

    ASSERT_EQ(future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_EQ(future.get(), 55);
}

TEST_F(MultiDispatcherTest, HighVolumeStopDrain) {
    constexpr int NUM_TOPICS = 16;
    constexpr int PER_TOPIC = 50;
    std::atomic<int> received{0};
    for (int t = 0; t < NUM_TOPICS; ++t) {
        bus.subscribe<int>("drain/" + std::to_string(t),
                           [&](const int&) { received.fetch_add(1, std::memory_order_relaxed); });
    }

    std::vector<std::thread> producers;
    for (int t = 0; t < NUM_TOPICS; ++t) {
        producers.emplace_back([&, t] {
            for (int i = 0; i < PER_TOPIC; ++i) {
                while (!bus.publish<int>("drain/" + std::to_string(t), i))
                    std::this_thread::yield();
            }
        });
    }
    for (auto& th : producers)
        th.join();

    bus.stop();
    EXPECT_EQ(received.load(), NUM_TOPICS * PER_TOPIC);
}

TEST(MultiDispatcherDrainTest, RouterDrainWithPendingMessages) {
    MessageBus bus(65536, 4);
    bus.start();

    std::atomic<int> received{0};
    for (int t = 0; t < 8; ++t) {
        bus.subscribe<int>("route/" + std::to_string(t), [&](const int&) {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            received.fetch_add(1, std::memory_order_relaxed);
        });
    }

    for (int i = 0; i < 2000; ++i) {
        bus.publish<int>("route/" + std::to_string(i % 8), i);
    }

    bus.stop();
    EXPECT_GT(received.load(), 0);
}