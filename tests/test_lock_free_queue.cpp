#include "test_support.h"

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
    LockFreeQueue<int> q(4);
    EXPECT_TRUE(q.try_enqueue(1));
    EXPECT_TRUE(q.try_enqueue(2));
    EXPECT_TRUE(q.try_enqueue(3));
    EXPECT_TRUE(q.try_enqueue(4));
    EXPECT_FALSE(q.try_enqueue(5));
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

TEST(LockFreeQueueTest, MPMCConcurrent) {
    LockFreeQueue<int> q(4096);
    constexpr int PRODUCERS = 4;
    constexpr int CONSUMERS = 2;
    constexpr int PER_PRODUCER = 500;
    constexpr int TOTAL = PRODUCERS * PER_PRODUCER;

    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};
    std::atomic<long long> sum{0};

    std::vector<std::thread> threads;
    for (int p = 0; p < PRODUCERS; ++p) {
        threads.emplace_back([&, p] {
            for (int i = 0; i < PER_PRODUCER; ++i) {
                int val = p * PER_PRODUCER + i + 1;
                while (!q.try_enqueue(val)) {
                    std::this_thread::yield();
                }
                produced.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (int c = 0; c < CONSUMERS; ++c) {
        threads.emplace_back([&] {
            while (consumed.load(std::memory_order_relaxed) < TOTAL) {
                int val;
                if (q.try_dequeue(val)) {
                    sum.fetch_add(val, std::memory_order_relaxed);
                    consumed.fetch_add(1, std::memory_order_relaxed);
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }
    for (auto& t : threads)
        t.join();

    long long expected = 0;
    for (int i = 1; i <= TOTAL; ++i)
        expected += i;
    EXPECT_EQ(consumed.load(), TOTAL);
    EXPECT_EQ(sum.load(), expected);
}

TEST(LockFreeQueueTest, CapacityRounding) {
    LockFreeQueue<int> q(3);
    EXPECT_TRUE(q.try_enqueue(1));
    EXPECT_TRUE(q.try_enqueue(2));
    EXPECT_TRUE(q.try_enqueue(3));
    EXPECT_TRUE(q.try_enqueue(4));
    EXPECT_FALSE(q.try_enqueue(5));
}

TEST(LockFreeQueueTest, HighContentionMPMC) {
    LockFreeQueue<int> q(4);
    constexpr int PRODUCERS = 8;
    constexpr int CONSUMERS = 8;
    constexpr int PER_PRODUCER = 1000;
    constexpr int TOTAL = PRODUCERS * PER_PRODUCER;

    std::atomic<long long> sum{0};
    std::atomic<int> consumed{0};
    std::vector<std::thread> threads;

    for (int p = 0; p < PRODUCERS; ++p) {
        threads.emplace_back([&, p] {
            for (int i = 0; i < PER_PRODUCER; ++i) {
                int val = p * PER_PRODUCER + i + 1;
                while (!q.try_enqueue(val)) {
                    std::this_thread::yield();
                }
            }
        });
    }
    for (int c = 0; c < CONSUMERS; ++c) {
        threads.emplace_back([&] {
            while (consumed.load(std::memory_order_relaxed) < TOTAL) {
                int val;
                if (q.try_dequeue(val)) {
                    sum.fetch_add(val, std::memory_order_relaxed);
                    consumed.fetch_add(1, std::memory_order_relaxed);
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }
    for (auto& t : threads)
        t.join();

    long long expected = 0;
    for (int i = 1; i <= TOTAL; ++i)
        expected += i;
    EXPECT_EQ(consumed.load(), TOTAL);
    EXPECT_EQ(sum.load(), expected);
}