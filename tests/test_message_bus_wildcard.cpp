#include "test_support.h"

TEST_F(MessageBusTest, InvalidWildcardHashNotLast) {
    EXPECT_THROW(bus.subscribe<int>("a/#/b", [](const int&) {}), std::runtime_error);
}

TEST_F(MessageBusTest, ValidWildcardPatterns) {
    EXPECT_NO_THROW(bus.subscribe<int>("#", [](const int&) {}));
    EXPECT_NO_THROW(bus.subscribe<int>("sensor/#", [](const int&) {}));
    EXPECT_NO_THROW(bus.subscribe<int>("sensor/*/temp", [](const int&) {}));
    EXPECT_NO_THROW(bus.subscribe<int>("a/*/c/#", [](const int&) {}));
}

TEST_F(MessageBusTest, WildcardSingleLevel) {
    std::atomic<int> count{0};
    bus.subscribe<int>("sensor/*/temp", [&](const int&) { count.fetch_add(1); });

    bus.publish<int>("sensor/1/temp", 10);
    bus.publish<int>("sensor/2/temp", 20);
    bus.publish<int>("sensor/1/humidity", 30);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(count.load(), 2);
}

TEST_F(MessageBusTest, WildcardMultiLevel) {
    std::atomic<int> count{0};
    bus.subscribe<int>("system/#", [&](const int&) { count.fetch_add(1); });

    bus.publish<int>("system/cpu", 1);
    bus.publish<int>("system/mem/used", 2);
    bus.publish<int>("system/disk/sda/read", 3);
    bus.publish<int>("other/thing", 4);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(count.load(), 3);
}

TEST_F(MessageBusTest, WildcardUnsubscribe) {
    std::atomic<int> count{0};
    auto id = bus.subscribe<int>("event/#", [&](const int&) { count.fetch_add(1); });

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

    bus.subscribe<int>("data/temp", [&](const int&) { exact_count.fetch_add(1); });
    bus.subscribe<int>("data/*", [&](const int&) { wild_count.fetch_add(1); });

    bus.publish<int>("data/temp", 42);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_EQ(exact_count.load(), 1);
    EXPECT_EQ(wild_count.load(), 1);
}