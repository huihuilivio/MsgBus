#include "test_support.h"

TEST(FullPolicyTest, ReturnFalseDefault) {
    MessageBus bus(4);
    EXPECT_EQ(bus.policy(), FullPolicy::ReturnFalse);
    int published = 0;
    for (int i = 0; i < 100; ++i) {
        if (!bus.publish<int>("full", i))
            break;
        ++published;
    }
    EXPECT_LT(published, 100);
}

TEST(FullPolicyTest, DropNewestAlwaysReturnsTrue) {
    MessageBus bus(4, 1, FullPolicy::DropNewest);
    EXPECT_EQ(bus.policy(), FullPolicy::DropNewest);
    for (int i = 0; i < 100; ++i) {
        EXPECT_TRUE(bus.publish<int>("drop_newest", i));
    }
}

TEST(FullPolicyTest, DropOldestAlwaysReturnsTrue) {
    MessageBus bus(4, 1, FullPolicy::DropOldest);
    EXPECT_EQ(bus.policy(), FullPolicy::DropOldest);
    for (int i = 0; i < 100; ++i) {
        EXPECT_TRUE(bus.publish<int>("drop_oldest", i));
    }
}

TEST(FullPolicyTest, DropOldestKeepsNewest) {
    MessageBus bus(4, 1, FullPolicy::DropOldest);
    bus.start();

    std::vector<int> received;
    std::mutex mu;
    std::condition_variable done_cv;

    bus.subscribe<int>("topic", [&](const int& v) {
        std::lock_guard<std::mutex> lk(mu);
        received.push_back(v);
    });

    for (int i = 0; i < 20; ++i) {
        bus.publish<int>("topic", i);
    }

    for (int i = 0; i < 200; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        std::lock_guard<std::mutex> lk(mu);
        if (received.size() >= 4)
            break;
    }
    bus.stop();

    std::lock_guard<std::mutex> lk(mu);
    EXPECT_GE(received.size(), 1u);
    if (!received.empty()) {
        EXPECT_GE(received.back(), 16);
    }
}

TEST(FullPolicyTest, BlockReleasesOnDequeue) {
    MessageBus bus(4, 1, FullPolicy::Block);
    bus.start();

    std::atomic<int> received{0};
    bus.subscribe<int>("block",
                       [&](const int&) { received.fetch_add(1, std::memory_order_relaxed); });

    constexpr int N = 50;
    std::thread publisher([&] {
        for (int i = 0; i < N; ++i) {
            EXPECT_TRUE(bus.publish<int>("block", i));
        }
    });

    publisher.join();
    for (int i = 0; i < 200 && received.load() < N; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    bus.stop();
    EXPECT_EQ(received.load(), N);
}

TEST(FullPolicyTest, BlockTimeoutReturnsOnTimeout) {
    MessageBus bus(4, 1, FullPolicy::BlockTimeout, std::chrono::milliseconds{50});
    int succeeded = 0;
    for (int i = 0; i < 20; ++i) {
        if (bus.publish<int>("timeout", i))
            ++succeeded;
    }
    EXPECT_GT(succeeded, 0);
    EXPECT_LT(succeeded, 20);
}

TEST(FullPolicyTest, BlockWakesOnStop) {
    MessageBus bus(4, 1, FullPolicy::Block);
    bus.start();

    std::atomic<int> received{0};
    bus.subscribe<int>("stop_wake",
                       [&](const int&) { received.fetch_add(1, std::memory_order_relaxed); });

    MessageBus slow_bus(4, 1, FullPolicy::Block);
    slow_bus.start();
    std::atomic<bool> handler_running{false};
    std::atomic<bool> handler_release{false};
    slow_bus.subscribe<int>("block_topic", [&](const int&) {
        handler_running.store(true);
        while (!handler_release.load(std::memory_order_acquire))
            std::this_thread::yield();
    });

    slow_bus.publish<int>("block_topic", 0);
    while (!handler_running.load())
        std::this_thread::yield();

    for (int i = 0; i < 4; ++i) {
        slow_bus.publish<int>("block_topic", i + 1);
    }

    std::atomic<bool> publish_done{false};
    std::thread publisher([&] {
        slow_bus.publish<int>("block_topic", 999);
        publish_done.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(publish_done.load());

    handler_release.store(true, std::memory_order_release);
    slow_bus.stop();
    publisher.join();
    EXPECT_TRUE(publish_done.load());
}

TEST(FullPolicyTest, DropOldestMultiProducer) {
    MessageBus bus(64, 1, FullPolicy::DropOldest);
    bus.start();

    std::atomic<int> received{0};
    bus.subscribe<int>("mp", [&](const int&) { received.fetch_add(1, std::memory_order_relaxed); });

    constexpr int THREADS = 4;
    constexpr int PER_THREAD = 500;
    std::vector<std::thread> producers;
    for (int t = 0; t < THREADS; ++t) {
        producers.emplace_back([&, t] {
            for (int i = 0; i < PER_THREAD; ++i) {
                bus.publish<int>("mp", t * PER_THREAD + i);
            }
        });
    }
    for (auto& th : producers)
        th.join();

    for (int i = 0; i < 200 && received.load() < PER_THREAD; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    bus.stop();

    EXPECT_GT(received.load(), 0);
}

TEST(FullPolicyTest, DropOldestRetryLoop) {
    MessageBus bus(2, 1, FullPolicy::DropOldest);
    bus.start();

    std::atomic<int> received{0};
    bus.subscribe<int>("retry", [&](const int&) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        received.fetch_add(1, std::memory_order_relaxed);
    });

    constexpr int THREADS = 4;
    constexpr int PER_THREAD = 50;
    std::vector<std::thread> producers;
    for (int t = 0; t < THREADS; ++t) {
        producers.emplace_back([&] {
            for (int i = 0; i < PER_THREAD; ++i) {
                bus.publish<int>("retry", i);
            }
        });
    }
    for (auto& th : producers)
        th.join();

    bus.stop();
    EXPECT_GT(received.load(), 0);
}

TEST(FullPolicyTest, BlockWaitsAndDrains) {
    MessageBus bus(2, 1, FullPolicy::Block);
    bus.start();

    std::atomic<int> received{0};
    bus.subscribe<int>("block_drain", [&](const int&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        received.fetch_add(1, std::memory_order_relaxed);
    });

    constexpr int N = 30;
    std::vector<std::thread> publishers;
    for (int t = 0; t < 3; ++t) {
        publishers.emplace_back([&, t] {
            for (int i = 0; i < N / 3; ++i) {
                EXPECT_TRUE(bus.publish<int>("block_drain", t * 10 + i));
            }
        });
    }
    for (auto& th : publishers)
        th.join();

    bus.stop();
    EXPECT_EQ(received.load(), N);
}

TEST(FullPolicyTest, BlockTimeoutWaitsAndDrains) {
    MessageBus bus(4, 1, FullPolicy::BlockTimeout, std::chrono::milliseconds{5000});
    bus.start();

    std::atomic<bool> handler_gate{false};
    std::atomic<int> received{0};
    bus.subscribe<int>("bt_drain", [&](const int&) {
        if (received.load(std::memory_order_relaxed) == 0) {
            while (!handler_gate.load(std::memory_order_acquire))
                std::this_thread::yield();
        }
        received.fetch_add(1, std::memory_order_relaxed);
    });

    for (int i = 0; i < 5; ++i) {
        bus.publish<int>("bt_drain", i);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::atomic<bool> pub_done{false};
    std::thread publisher([&] {
        bus.publish<int>("bt_drain", 99);
        pub_done.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    handler_gate.store(true, std::memory_order_release);
    publisher.join();
    EXPECT_TRUE(pub_done.load());

    bus.stop();
    EXPECT_GT(received.load(), 0);
}

TEST(DropCallbackTest, DropNewestNotifiesCallback) {
    MessageBus bus(4, 1, FullPolicy::DropNewest);
    std::vector<int> dropped_values;
    std::vector<std::string> dropped_topics;
    std::mutex drop_mu;

    for (int i = 0; i < 20; ++i) {
        bus.publish<int>("drop/newest", i, [&](std::string_view topic, const int& val) {
            std::lock_guard<std::mutex> lk(drop_mu);
            dropped_topics.emplace_back(topic);
            dropped_values.push_back(val);
        });
    }

    EXPECT_FALSE(dropped_values.empty());
    for (const auto& t : dropped_topics) {
        EXPECT_EQ(t, "drop/newest");
    }
}

TEST(DropCallbackTest, DropOldestNotifiesCallback) {
    MessageBus bus(4, 1, FullPolicy::DropOldest);
    std::vector<int> dropped_values;
    std::mutex drop_mu;

    for (int i = 0; i < 20; ++i) {
        bus.publish<int>("drop/oldest", i,
                         [&drop_mu, &dropped_values](std::string_view, const int& val) {
                             std::lock_guard<std::mutex> lk(drop_mu);
                             dropped_values.push_back(val);
                         });
    }

    EXPECT_FALSE(dropped_values.empty());
    for (int val : dropped_values) {
        EXPECT_GE(val, 0);
        EXPECT_LT(val, 20);
    }
}

TEST(DropCallbackTest, NoCallbackWithoutDropPolicy) {
    MessageBus bus(4, 1, FullPolicy::ReturnFalse);
    bool callback_fired = false;

    for (int i = 0; i < 20; ++i) {
        bus.publish<int>("no/drop", i, [&callback_fired](std::string_view, const int&) {
            callback_fired = true;
        });
    }

    EXPECT_FALSE(callback_fired);
}

TEST(DropCallbackTest, NoCallbackWhenQueueNotFull) {
    MessageBus bus(64, 1, FullPolicy::DropNewest);
    bus.start();

    std::atomic<int> received{0};
    bus.subscribe<int>("not/full", [&](const int&) { received.fetch_add(1); });

    bool callback_fired = false;
    for (int i = 0; i < 10; ++i) {
        bus.publish<int>("not/full", i, [&callback_fired](std::string_view, const int&) {
            callback_fired = true;
        });
    }

    for (int i = 0; i < 100 && received.load() < 10; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    bus.stop();

    EXPECT_FALSE(callback_fired);
    EXPECT_EQ(received.load(), 10);
}

TEST(DropCallbackTest, CallbackReceivesCorrectTopic) {
    MessageBus bus(2, 1, FullPolicy::DropNewest);
    std::string captured_topic;
    int captured_val = -1;
    std::mutex mu;
    std::condition_variable cv;
    bool dropped = false;

    bus.publish<int>("topic/a", 1);
    bus.publish<int>("topic/a", 2);

    bus.publish<int>("topic/b", 99, [&](std::string_view topic, const int& val) {
        std::lock_guard<std::mutex> lk(mu);
        captured_topic = topic;
        captured_val = val;
        dropped = true;
        cv.notify_one();
    });

    EXPECT_TRUE(dropped);
    EXPECT_EQ(captured_topic, "topic/b");
    EXPECT_EQ(captured_val, 99);
}

TEST(DropCallbackTest, PublishWithoutCallbackStillWorks) {
    MessageBus bus(4, 1, FullPolicy::DropNewest);
    for (int i = 0; i < 20; ++i) {
        EXPECT_TRUE(bus.publish<int>("no/cb", i));
    }
}

TEST(DropCallbackTest, DropOldestMultiProducerWithCallback) {
    MessageBus bus(16, 1, FullPolicy::DropOldest);
    bus.start();

    std::atomic<int> drop_count{0};
    bus.subscribe<int>("mp/drop", [&](const int&) {});

    constexpr int THREADS = 4;
    constexpr int PER_THREAD = 200;
    std::vector<std::thread> producers;
    for (int t = 0; t < THREADS; ++t) {
        producers.emplace_back([&, t] {
            for (int i = 0; i < PER_THREAD; ++i) {
                bus.publish<int>("mp/drop", t * PER_THREAD + i, [&](std::string_view, const int&) {
                    drop_count.fetch_add(1, std::memory_order_relaxed);
                });
            }
        });
    }
    for (auto& th : producers)
        th.join();

    bus.stop();
    EXPECT_GT(drop_count.load(), 0);
}