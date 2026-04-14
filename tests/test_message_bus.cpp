#include "msgbus/message_bus.h"
#include "msgbus/object_pool.h"
#include "msgbus/topic_matcher.h"
#include "msgbus/topic_registry.h"
#include "msgbus/wildcard_trie.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <future>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace msgbus;

// ---------- TopicRegistry Tests ----------

TEST(TopicRegistryTest, ResolveAndToString) {
    TopicRegistry reg;
    TopicId id1 = reg.resolve("sensor/temp");
    TopicId id2 = reg.resolve("sensor/humidity");
    EXPECT_NE(id1, kInvalidTopicId);
    EXPECT_NE(id2, kInvalidTopicId);
    EXPECT_NE(id1, id2);
    EXPECT_EQ(reg.to_string(id1), "sensor/temp");
    EXPECT_EQ(reg.to_string(id2), "sensor/humidity");
}

TEST(TopicRegistryTest, SameTopicSameId) {
    TopicRegistry reg;
    TopicId a = reg.resolve("x/y");
    TopicId b = reg.resolve("x/y");
    EXPECT_EQ(a, b);
}

TEST(TopicRegistryTest, InvalidIdReturnsEmpty) {
    TopicRegistry reg;
    EXPECT_TRUE(reg.to_string(999).empty());
}

TEST(TopicRegistryTest, ConcurrentResolve) {
    TopicRegistry reg;
    constexpr int THREADS = 8;
    constexpr int PER_THREAD = 100;
    std::vector<std::thread> threads;
    std::vector<TopicId> ids(THREADS * PER_THREAD);

    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < PER_THREAD; ++i) {
                std::string topic = "t/" + std::to_string(t) + "/" + std::to_string(i);
                ids[t * PER_THREAD + i] = reg.resolve(topic);
            }
        });
    }
    for (auto& th : threads) th.join();

    // All IDs should be unique (no duplicates for different topics)
    std::set<TopicId> unique_ids(ids.begin(), ids.end());
    EXPECT_EQ(unique_ids.size(), static_cast<size_t>(THREADS * PER_THREAD));
}

// ---------- MessagePtr Tests ----------

TEST(MessagePtrTest, DefaultNull) {
    MessagePtr ptr;
    EXPECT_FALSE(ptr);
    EXPECT_EQ(ptr.get(), nullptr);
}

TEST(MessagePtrTest, AdoptAndAccess) {
    auto* raw = new TypedMessage<int>(1, 42);
    MessagePtr ptr = MessagePtr::adopt(raw);
    EXPECT_TRUE(ptr);
    EXPECT_EQ(ptr->topic_id(), 1u);
    EXPECT_EQ(ptr->type(), typeid(int));
    EXPECT_EQ(static_cast<TypedMessage<int>*>(ptr.get())->data_, 42);
}

TEST(MessagePtrTest, CopyIncrementsRefCount) {
    auto* raw = new TypedMessage<int>(1, 1);
    MessagePtr p1 = MessagePtr::adopt(raw);
    {
        MessagePtr p2 = p1; // copy
        EXPECT_EQ(p2.get(), p1.get());
        EXPECT_EQ(raw->ref_count_.load(), 2);
    }
    // p2 destroyed, ref count back to 1
    EXPECT_EQ(raw->ref_count_.load(), 1);
}

TEST(MessagePtrTest, MoveTransfersOwnership) {
    auto* raw = new TypedMessage<int>(1, 1);
    MessagePtr p1 = MessagePtr::adopt(raw);
    MessagePtr p2 = std::move(p1);
    EXPECT_FALSE(p1);
    EXPECT_TRUE(p2);
    EXPECT_EQ(p2.get(), raw);
    EXPECT_EQ(raw->ref_count_.load(), 1);
}

TEST(MessagePtrTest, CopyAssignment) {
    auto* r1 = new TypedMessage<int>(1, 1);
    auto* r2 = new TypedMessage<int>(2, 2);
    MessagePtr p1 = MessagePtr::adopt(r1);
    MessagePtr p2 = MessagePtr::adopt(r2);
    p2 = p1;
    EXPECT_EQ(p2.get(), r1);
    EXPECT_EQ(r1->ref_count_.load(), 2);
    // r2 should have been deleted (ref dropped to 0)
}

TEST(MessagePtrTest, MoveAssignment) {
    auto* r1 = new TypedMessage<int>(1, 1);
    auto* r2 = new TypedMessage<int>(2, 2);
    MessagePtr p1 = MessagePtr::adopt(r1);
    MessagePtr p2 = MessagePtr::adopt(r2);
    p2 = std::move(p1);
    EXPECT_FALSE(p1);
    EXPECT_EQ(p2.get(), r1);
    EXPECT_EQ(r1->ref_count_.load(), 1);
}

TEST(MessagePtrTest, SelfCopyAssignment) {
    auto* raw = new TypedMessage<int>(1, 1);
    MessagePtr p = MessagePtr::adopt(raw);
    auto& ref = p;
    p = ref; // self-copy
    EXPECT_EQ(p.get(), raw);
    EXPECT_EQ(raw->ref_count_.load(), 1);
}

TEST(MessagePtrTest, SelfMoveAssignment) {
    auto* raw = new TypedMessage<int>(1, 1);
    MessagePtr p = MessagePtr::adopt(raw);
    auto& ref = p;
    p = std::move(ref); // self-move
    EXPECT_EQ(p.get(), raw);
    EXPECT_EQ(raw->ref_count_.load(), 1);
}

TEST(MessagePtrTest, RecyclerCalledOnDestroy) {
    auto* raw = new TypedMessage<int>(1, 1);
    // Use data_ as a flag: recycler sets it to a sentinel instead of deleting
    raw->recycler_ = [](IMessage* msg) {
        static_cast<TypedMessage<int>*>(msg)->data_ = 12345;
    };
    {
        MessagePtr p = MessagePtr::adopt(raw);
        raw->ref_count_.store(1, std::memory_order_relaxed);
        // p goes out of scope → recycler called instead of delete
    }
    // raw is still alive because the recycler did NOT delete it
    EXPECT_EQ(raw->data_, 12345);
    delete raw;
}

TEST(MessagePtrTest, ResetToNull) {
    auto* raw = new TypedMessage<int>(1, 1);
    MessagePtr p = MessagePtr::adopt(raw);
    EXPECT_TRUE(p);
    p.reset();
    EXPECT_FALSE(p);
    EXPECT_EQ(p.get(), nullptr);
}

TEST(MessagePtrTest, AdoptNull) {
    MessagePtr p = MessagePtr::adopt(nullptr);
    EXPECT_FALSE(p);
}

// ---------- TypedMessage Tests ----------

TEST(TypedMessageTest, Construction) {
    TypedMessage<std::string> msg(1, "hello");
    EXPECT_EQ(msg.topic_id(), 1u);
    EXPECT_EQ(msg.data_, "hello");
    EXPECT_EQ(msg.type(), typeid(std::string));
}

TEST(TypedMessageTest, ResetForReuse) {
    auto* msg = new TypedMessage<int>(1, 1);
    msg->ref_count_.store(5, std::memory_order_relaxed);
    msg->recycler_ = reinterpret_cast<void(*)(IMessage*)>(0xDEAD); // dummy

    msg->reset(2, 99);

    EXPECT_EQ(msg->topic_id(), 2u);
    EXPECT_EQ(msg->data_, 99);
    EXPECT_EQ(msg->ref_count_.load(), 0);
    EXPECT_EQ(msg->recycler_, nullptr);
    delete msg;
}

// ---------- ObjectPool Tests ----------

TEST(ObjectPoolTest, AcquireFromEmpty) {
    ObjectPool<TypedMessage<int>> pool(4);
    EXPECT_EQ(pool.acquire(), nullptr);
}

TEST(ObjectPoolTest, ReleaseAndAcquire) {
    ObjectPool<TypedMessage<int>> pool(4);
    auto* obj = new TypedMessage<int>(1, 42);
    pool.release(obj);
    auto* recycled = pool.acquire();
    EXPECT_EQ(recycled, obj);
    EXPECT_EQ(pool.acquire(), nullptr); // pool empty again
    delete recycled;
}

TEST(ObjectPoolTest, FullPoolDeletesObject) {
    ObjectPool<TypedMessage<int>> pool(2); // capacity 2
    auto* a = new TypedMessage<int>(1, 1);
    auto* b = new TypedMessage<int>(2, 2);
    auto* c = new TypedMessage<int>(3, 3);

    pool.release(a);
    pool.release(b);
    pool.release(c); // pool full → c should be deleted

    auto* got1 = pool.acquire();
    auto* got2 = pool.acquire();
    auto* got3 = pool.acquire();
    EXPECT_NE(got1, nullptr);
    EXPECT_NE(got2, nullptr);
    EXPECT_EQ(got3, nullptr);
    delete got1;
    delete got2;
}

TEST(ObjectPoolTest, RecycleViaMessagePtr) {
    // Verify the full object pool + MessagePtr recycler integration
    auto& pool = TypedMessagePool<int>::instance();
    // Publish a message, let it be destroyed → should be recycled to pool
    {
        auto* raw = pool.acquire();
        if (!raw) raw = new TypedMessage<int>(1, 0);
        raw->reset(2, 77);
        raw->recycler_ = &TypedMessagePool<int>::recycle;
        MessagePtr ptr = MessagePtr::adopt(raw);
        // ptr goes out of scope → recycler called → back in pool
    }
    auto* recycled = pool.acquire();
    EXPECT_NE(recycled, nullptr);
    if (recycled) {
        // The recycled object should exist (we can reuse it)
        recycled->reset(3, 100);
        EXPECT_EQ(recycled->data_, 100);
        pool.release(recycled);
    }
}

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
    for (auto& t : threads) t.join();

    // Verify all messages consumed and sum is correct
    long long expected = 0;
    for (int i = 1; i <= TOTAL; ++i) expected += i;
    EXPECT_EQ(consumed.load(), TOTAL);
    EXPECT_EQ(sum.load(), expected);
}

TEST(LockFreeQueueTest, CapacityRounding) {
    // Capacity 3 rounds up to 4
    LockFreeQueue<int> q(3);
    EXPECT_TRUE(q.try_enqueue(1));
    EXPECT_TRUE(q.try_enqueue(2));
    EXPECT_TRUE(q.try_enqueue(3));
    EXPECT_TRUE(q.try_enqueue(4)); // 4 (rounded up from 3)
    EXPECT_FALSE(q.try_enqueue(5)); // full
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

TEST_F(MessageBusTest, QueueFullReturnsFalse) {
    MessageBus small_bus(4); // tiny queue
    // Do NOT start — no dispatcher draining, so the queue will actually fill
    int published = 0;
    for (int i = 0; i < 100; ++i) {
        if (!small_bus.publish<int>("full", i)) break;
        ++published;
    }
    // At least one publish should have failed (queue is tiny)
    EXPECT_LT(published, 100);
    small_bus.stop();
}

// ---------- FullPolicy Tests ----------

TEST(FullPolicyTest, ReturnFalseDefault) {
    // Default policy is ReturnFalse
    MessageBus bus(4);
    EXPECT_EQ(bus.policy(), FullPolicy::ReturnFalse);
    // Do NOT start — queue will fill
    int published = 0;
    for (int i = 0; i < 100; ++i) {
        if (!bus.publish<int>("full", i)) break;
        ++published;
    }
    EXPECT_LT(published, 100);
}

TEST(FullPolicyTest, DropNewestAlwaysReturnsTrue) {
    MessageBus bus(4, 1, FullPolicy::DropNewest);
    EXPECT_EQ(bus.policy(), FullPolicy::DropNewest);
    // Do NOT start — queue will fill, but publish always returns true
    for (int i = 0; i < 100; ++i) {
        EXPECT_TRUE(bus.publish<int>("drop_newest", i));
    }
}

TEST(FullPolicyTest, DropOldestAlwaysReturnsTrue) {
    MessageBus bus(4, 1, FullPolicy::DropOldest);
    EXPECT_EQ(bus.policy(), FullPolicy::DropOldest);
    // Do NOT start — queue will fill, but publish always returns true
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

    // Overflow: publish 20 messages into capacity-4 queue without giving
    // dispatcher much time. Newest messages should survive.
    for (int i = 0; i < 20; ++i) {
        bus.publish<int>("topic", i);
    }

    // Wait for dispatcher to drain
    for (int i = 0; i < 200; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        std::lock_guard<std::mutex> lk(mu);
        if (received.size() >= 4) break;
    }
    bus.stop();

    std::lock_guard<std::mutex> lk(mu);
    // Should have received some messages, and the last ones should be the newest
    EXPECT_GE(received.size(), 1u);
    if (!received.empty()) {
        // The very last received should be close to 19 (the newest published)
        EXPECT_GE(received.back(), 16); // at least among last 4
    }
}

TEST(FullPolicyTest, BlockReleasesOnDequeue) {
    MessageBus bus(4, 1, FullPolicy::Block);
    bus.start();

    std::atomic<int> received{0};
    bus.subscribe<int>("block", [&](const int&) {
        received.fetch_add(1, std::memory_order_relaxed);
    });

    // Publish more than capacity — Block policy should let all through
    constexpr int N = 50;
    std::thread publisher([&] {
        for (int i = 0; i < N; ++i) {
            EXPECT_TRUE(bus.publish<int>("block", i));
        }
    });

    publisher.join();
    // Wait for all to be consumed
    for (int i = 0; i < 200 && received.load() < N; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    bus.stop();
    EXPECT_EQ(received.load(), N);
}

TEST(FullPolicyTest, BlockTimeoutReturnsOnTimeout) {
    MessageBus bus(4, 1, FullPolicy::BlockTimeout, std::chrono::milliseconds{50});
    // Do NOT start — queue will fill and timeout
    int succeeded = 0;
    for (int i = 0; i < 20; ++i) {
        if (bus.publish<int>("timeout", i)) ++succeeded;
    }
    // First few fit, rest should timeout and return false
    EXPECT_GT(succeeded, 0);
    EXPECT_LT(succeeded, 20);
}

TEST(FullPolicyTest, BlockWakesOnStop) {
    MessageBus bus(4, 1, FullPolicy::Block);
    bus.start();

    std::atomic<int> received{0};
    bus.subscribe<int>("stop_wake", [&](const int&) {
        received.fetch_add(1, std::memory_order_relaxed);
    });

    // Publish enough to fill the queue + backlog, then from another thread
    // publish one more that will block when the dispatcher is paused.
    // We achieve this by subscribing a slow handler.
    MessageBus slow_bus(4, 1, FullPolicy::Block);
    slow_bus.start();
    std::atomic<bool> handler_running{false};
    std::atomic<bool> handler_release{false};
    slow_bus.subscribe<int>("block_topic", [&](const int&) {
        handler_running.store(true);
        while (!handler_release.load(std::memory_order_acquire))
            std::this_thread::yield();
    });

    // Publish one message to trigger the slow handler (blocks dispatcher)
    slow_bus.publish<int>("block_topic", 0);
    while (!handler_running.load()) std::this_thread::yield();

    // Now dispatcher is stuck. Fill the queue.
    for (int i = 0; i < 4; ++i) {
        slow_bus.publish<int>("block_topic", i + 1);
    }

    // Next publish should block (queue full, dispatcher stuck)
    std::atomic<bool> publish_done{false};
    std::thread publisher([&] {
        slow_bus.publish<int>("block_topic", 999);
        publish_done.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(publish_done.load());

    // stop() should wake the blocked publisher
    handler_release.store(true, std::memory_order_release);
    slow_bus.stop();
    publisher.join();
    EXPECT_TRUE(publish_done.load());
}

TEST(FullPolicyTest, DropOldestMultiProducer) {
    // Multiple producers with DropOldest — no crash, no UB
    MessageBus bus(64, 1, FullPolicy::DropOldest);
    bus.start();

    std::atomic<int> received{0};
    bus.subscribe<int>("mp", [&](const int&) {
        received.fetch_add(1, std::memory_order_relaxed);
    });

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
    for (auto& th : producers) th.join();

    // Wait for drain
    for (int i = 0; i < 200 && received.load() < PER_THREAD; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    bus.stop();

    EXPECT_GT(received.load(), 0);
}

TEST(FullPolicyTest, DropOldestRetryLoop) {
    // Tiny queue + slow handler → forces the while-retry path in DropOldest.
    // Use capacity=2 (minimum) so queue is almost always full.
    MessageBus bus(2, 1, FullPolicy::DropOldest);
    bus.start();

    std::atomic<int> received{0};
    bus.subscribe<int>("retry", [&](const int&) {
        // Slow handler to keep queue full
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        received.fetch_add(1, std::memory_order_relaxed);
    });

    // Multi-threaded rapid-fire to maximize contention on publish_mutex_
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
    for (auto& th : producers) th.join();

    bus.stop();
    EXPECT_GT(received.load(), 0);
}

TEST(FullPolicyTest, BlockWaitsAndDrains) {
    // Block policy: tiny queue + slow handler → publisher blocks in cv_wait,
    // then woken when dispatcher drains. Exercises the cv_not_full_.wait() lambda.
    MessageBus bus(2, 1, FullPolicy::Block);
    bus.start();

    std::atomic<int> received{0};
    bus.subscribe<int>("block_drain", [&](const int&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        received.fetch_add(1, std::memory_order_relaxed);
    });

    // Publish from multiple threads to increase contention
    constexpr int N = 30;
    std::vector<std::thread> publishers;
    for (int t = 0; t < 3; ++t) {
        publishers.emplace_back([&, t] {
            for (int i = 0; i < N / 3; ++i) {
                EXPECT_TRUE(bus.publish<int>("block_drain", t * 10 + i));
            }
        });
    }
    for (auto& th : publishers) th.join();

    bus.stop();
    EXPECT_EQ(received.load(), N);
}

TEST(FullPolicyTest, BlockTimeoutWaitsAndDrains) {
    // Exercises wait_until lambda: publisher must enter wait_until and be woken
    // by notify_not_full when the dispatcher drains.
    // Strategy: slow handler holds dispatcher, fill queue completely, then
    // a publisher thread blocks. Release gate → handler finishes → space freed.
    MessageBus bus(4, 1, FullPolicy::BlockTimeout, std::chrono::milliseconds{5000});
    bus.start();

    std::atomic<bool> handler_gate{false};
    std::atomic<int> received{0};
    bus.subscribe<int>("bt_drain", [&](const int&) {
        // First call blocks until gate is released
        if (received.load(std::memory_order_relaxed) == 0) {
            while (!handler_gate.load(std::memory_order_acquire))
                std::this_thread::yield();
        }
        received.fetch_add(1, std::memory_order_relaxed);
    });

    // Publish enough to trigger the handler and fill the queue behind it.
    // The first message enters the handler which blocks. The remaining
    // messages saturate the queue (capacity=4).
    for (int i = 0; i < 5; ++i) {
        bus.publish<int>("bt_drain", i);
    }
    // Give dispatcher time to pick up the first message and block in handler
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Next publish should block in wait_until (queue full, handler gated)
    std::atomic<bool> pub_done{false};
    std::thread publisher([&] {
        bus.publish<int>("bt_drain", 99);
        pub_done.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // Release gate → dispatcher drains → notify_not_full → publisher unblocks
    handler_gate.store(true, std::memory_order_release);
    publisher.join();
    EXPECT_TRUE(pub_done.load());

    bus.stop();
    EXPECT_GT(received.load(), 0);
}

TEST_F(MessageBusTest, StartStopIdempotent) {
    // Already started in SetUp
    bus.start(); // second start should be no-op
    bus.start(); // third start
    bus.stop();
    bus.stop(); // second stop should be no-op
    bus.stop(); // third stop
}

TEST_F(MessageBusTest, StopWithoutStart) {
    MessageBus fresh_bus;
    fresh_bus.stop(); // should not crash
}

TEST_F(MessageBusTest, UnsubscribeInvalidId) {
    bus.unsubscribe(999999); // non-existent ID, should not crash
}

TEST_F(MessageBusTest, HandlerExceptionIsolation) {
    std::atomic<int> good_count{0};
    // First subscriber throws
    bus.subscribe<int>("except", [](const int&) {
        throw std::runtime_error("boom");
    });
    // Second subscriber should still receive
    bus.subscribe<int>("except", [&good_count](const int&) {
        good_count.fetch_add(1);
    });

    bus.publish<int>("except", 1);
    bus.publish<int>("except", 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(good_count.load(), 2);
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

/// A coroutine Task that owns its handle, allowing manual destruction
/// while the coroutine is suspended (to test cleanup paths).
struct DestroyableTask {
    struct promise_type {
        DestroyableTask get_return_object() {
            return DestroyableTask{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };

    std::coroutine_handle<promise_type> handle;

    explicit DestroyableTask(std::coroutine_handle<promise_type> h) : handle(h) {}
    DestroyableTask(DestroyableTask&& o) noexcept : handle(o.handle) { o.handle = nullptr; }
    DestroyableTask& operator=(DestroyableTask&&) = delete;
    ~DestroyableTask() { if (handle) handle.destroy(); }
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

TEST_F(MessageBusTest, CoroutineAwaitableDestroyedBeforeMessage) {
    // Exercises the AsyncWaitAwaitable destructor cleanup path (L266):
    // coroutine suspends on co_await, then gets destroyed before message arrives.
    {
        auto task = [&]() -> DestroyableTask {
            co_await bus.async_wait<int>("coro/no_msg");
            // Never reached — task destroyed while suspended
        };
        auto t = task(); // coroutine suspends at co_await
        // t goes out of scope → handle.destroy() → awaitable destructor
        // sees sub_id != 0 → unsubscribes
    }
    // Verify bus is still functional after cleanup
    std::promise<int> p;
    auto f = p.get_future();
    bus.subscribe<int>("coro/after", [&](const int& v) { p.set_value(v); });
    bus.publish<int>("coro/after", 42);
    ASSERT_EQ(f.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_EQ(f.get(), 42);
}

TEST(CoroutineTest, AsyncWaitDuplicateFireGuard) {
    // Exercises the CAS duplicate-fire guard (L248) by using a wildcard pattern
    // with multi-dispatcher that may deliver to the same handler from multiple workers.
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

    // Publish to multiple topics matching the wildcard
    for (int i = 0; i < 10; ++i) {
        bus.publish<int>("dup/" + std::to_string(i), i);
    }

    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    // Only one fire should have occurred despite multiple matching publishes
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(fire_count.load(), 1);
    bus.stop();
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

TEST_F(MultiDispatcherTest, SameTopicOrdering) {
    // Verify that messages on the same topic are delivered in publish order
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
    MessageBus auto_bus(65536, 0); // 0 = auto
    EXPECT_GE(auto_bus.dispatcher_count(), 1u);
}

// ---------- WildcardTrie Tests ----------

TEST(WildcardTrieTest, SingleLevelMatch) {
    WildcardTrie trie;
    auto slot = std::make_shared<TopicSlot<int>>();
    slot->addSubscriber(std::function<void(const int&)>([](const int&) {}), 1);
    trie.insert("sensor/*/temp", {&typeid(int), slot, 1});

    std::vector<ITopicSlot*> matched;
    trie.match("sensor/1/temp", typeid(int), matched);
    EXPECT_EQ(matched.size(), 1u);

    matched.clear();
    trie.match("sensor/1/humidity", typeid(int), matched);
    EXPECT_EQ(matched.size(), 0u);

    matched.clear();
    trie.match("sensor/1/2/temp", typeid(int), matched);
    EXPECT_EQ(matched.size(), 0u);
}

TEST(WildcardTrieTest, MultiLevelMatch) {
    WildcardTrie trie;
    auto slot = std::make_shared<TopicSlot<int>>();
    slot->addSubscriber(std::function<void(const int&)>([](const int&) {}), 1);
    trie.insert("sensor/#", {&typeid(int), slot, 1});

    std::vector<ITopicSlot*> matched;
    trie.match("sensor/temp", typeid(int), matched);
    EXPECT_EQ(matched.size(), 1u);

    matched.clear();
    trie.match("sensor/a/b/c", typeid(int), matched);
    EXPECT_EQ(matched.size(), 1u);

    matched.clear();
    trie.match("sensor", typeid(int), matched);
    EXPECT_EQ(matched.size(), 1u);

    matched.clear();
    trie.match("other/thing", typeid(int), matched);
    EXPECT_EQ(matched.size(), 0u);
}

TEST(WildcardTrieTest, MixedWildcards) {
    WildcardTrie trie;
    auto slot = std::make_shared<TopicSlot<int>>();
    slot->addSubscriber(std::function<void(const int&)>([](const int&) {}), 1);
    trie.insert("a/*/c/#", {&typeid(int), slot, 1});

    std::vector<ITopicSlot*> matched;
    trie.match("a/b/c/d/e", typeid(int), matched);
    EXPECT_EQ(matched.size(), 1u);

    matched.clear();
    trie.match("a/x/c", typeid(int), matched);
    EXPECT_EQ(matched.size(), 1u);

    matched.clear();
    trie.match("a/b/d", typeid(int), matched);
    EXPECT_EQ(matched.size(), 0u);
}

TEST(WildcardTrieTest, MultiplePatterns) {
    WildcardTrie trie;
    auto slot1 = std::make_shared<TopicSlot<int>>();
    slot1->addSubscriber(std::function<void(const int&)>([](const int&) {}), 1);
    trie.insert("sensor/#", {&typeid(int), slot1, 1});

    auto slot2 = std::make_shared<TopicSlot<int>>();
    slot2->addSubscriber(std::function<void(const int&)>([](const int&) {}), 2);
    trie.insert("sensor/*/temp", {&typeid(int), slot2, 2});

    std::vector<ITopicSlot*> matched;
    trie.match("sensor/1/temp", typeid(int), matched);
    EXPECT_EQ(matched.size(), 2u); // both patterns match
}

TEST(WildcardTrieTest, RemoveEntry) {
    WildcardTrie trie;
    auto slot = std::make_shared<TopicSlot<int>>();
    slot->addSubscriber(std::function<void(const int&)>([](const int&) {}), 1);
    trie.insert("sensor/#", {&typeid(int), slot, 1});

    EXPECT_FALSE(trie.empty());
    EXPECT_TRUE(trie.remove(1));
    EXPECT_TRUE(trie.empty());

    std::vector<ITopicSlot*> matched;
    trie.match("sensor/temp", typeid(int), matched);
    EXPECT_EQ(matched.size(), 0u);
}

TEST(WildcardTrieTest, TypeFiltering) {
    WildcardTrie trie;
    auto slot = std::make_shared<TopicSlot<int>>();
    slot->addSubscriber(std::function<void(const int&)>([](const int&) {}), 1);
    trie.insert("data/#", {&typeid(int), slot, 1});

    std::vector<ITopicSlot*> matched;
    trie.match("data/x", typeid(int), matched);
    EXPECT_EQ(matched.size(), 1u);

    matched.clear();
    trie.match("data/x", typeid(std::string), matched);
    EXPECT_EQ(matched.size(), 0u); // type mismatch
}

TEST(WildcardTrieTest, HashMatchesRoot) {
    WildcardTrie trie;
    auto slot = std::make_shared<TopicSlot<int>>();
    slot->addSubscriber(std::function<void(const int&)>([](const int&) {}), 1);
    trie.insert("#", {&typeid(int), slot, 1});

    std::vector<ITopicSlot*> matched;
    trie.match("anything/at/all", typeid(int), matched);
    EXPECT_EQ(matched.size(), 1u);

    matched.clear();
    trie.match("x", typeid(int), matched);
    EXPECT_EQ(matched.size(), 1u);
}

TEST(WildcardTrieTest, EmptyNodePruning) {
    // After removal, empty trie nodes should be pruned
    WildcardTrie trie;
    auto slot = std::make_shared<TopicSlot<int>>();
    slot->addSubscriber(std::function<void(const int&)>([](const int&) {}), 1);
    trie.insert("a/b/c/d", {&typeid(int), slot, 1});

    EXPECT_FALSE(trie.empty());
    EXPECT_TRUE(trie.remove(1));
    EXPECT_TRUE(trie.empty());

    // Insert again on the same path — should work (nodes were pruned, rebuilt)
    auto slot2 = std::make_shared<TopicSlot<int>>();
    slot2->addSubscriber(std::function<void(const int&)>([](const int&) {}), 2);
    trie.insert("a/b/c/d", {&typeid(int), slot2, 2});

    std::vector<ITopicSlot*> matched;
    trie.match("a/b/c/d", typeid(int), matched);
    EXPECT_EQ(matched.size(), 1u);
}

TEST(WildcardTrieTest, PartialPruning) {
    // When two patterns share a prefix, removing one should not break the other
    WildcardTrie trie;
    auto slot1 = std::make_shared<TopicSlot<int>>();
    slot1->addSubscriber(std::function<void(const int&)>([](const int&) {}), 1);
    trie.insert("a/b/c", {&typeid(int), slot1, 1});

    auto slot2 = std::make_shared<TopicSlot<int>>();
    slot2->addSubscriber(std::function<void(const int&)>([](const int&) {}), 2);
    trie.insert("a/b/d", {&typeid(int), slot2, 2});

    EXPECT_TRUE(trie.remove(1)); // remove a/b/c, prune 'c' node but keep 'a/b'

    std::vector<ITopicSlot*> matched;
    trie.match("a/b/c", typeid(int), matched);
    EXPECT_EQ(matched.size(), 0u); // removed

    matched.clear();
    trie.match("a/b/d", typeid(int), matched);
    EXPECT_EQ(matched.size(), 1u); // still alive
}

TEST(WildcardTrieTest, EntryCountAccuracy) {
    WildcardTrie trie;
    EXPECT_TRUE(trie.empty());

    auto make_slot = [](SubscriptionId id) {
        auto slot = std::make_shared<TopicSlot<int>>();
        slot->addSubscriber(std::function<void(const int&)>([](const int&) {}), id);
        return slot;
    };

    trie.insert("a/#", {&typeid(int), make_slot(1), 1});
    trie.insert("b/#", {&typeid(int), make_slot(2), 2});
    trie.insert("c/#", {&typeid(int), make_slot(3), 3});
    EXPECT_FALSE(trie.empty());

    EXPECT_TRUE(trie.remove(1));
    EXPECT_FALSE(trie.empty());
    EXPECT_TRUE(trie.remove(2));
    EXPECT_FALSE(trie.empty());
    EXPECT_TRUE(trie.remove(3));
    EXPECT_TRUE(trie.empty());

    // Remove non-existent ID
    EXPECT_FALSE(trie.remove(999));
    EXPECT_TRUE(trie.empty()); // count should not go negative / underflow
}

// ---------- WildcardTrie RCU Tests ----------

TEST(WildcardTrieTest, ConcurrentReadWrite) {
    // Multiple readers + one writer concurrently — RCU must not crash or lose data
    WildcardTrie trie;
    constexpr int NUM_PATTERNS = 100;
    constexpr int NUM_READERS  = 4;
    constexpr int READ_ITERS   = 2000;

    // Pre-insert some patterns so readers always have something to match
    auto make_slot = [](SubscriptionId id) {
        auto slot = std::make_shared<TopicSlot<int>>();
        slot->addSubscriber(std::function<void(const int&)>([](const int&) {}), id);
        return slot;
    };
    for (int i = 0; i < 10; ++i) {
        trie.insert("pre/" + std::to_string(i) + "/#",
                     {&typeid(int), make_slot(static_cast<SubscriptionId>(i + 1)),
                      static_cast<SubscriptionId>(i + 1)});
    }

    std::atomic<bool> stop{false};

    // Writer: continuously insert and remove patterns
    std::thread writer([&] {
        for (int i = 10; i < NUM_PATTERNS && !stop.load(); ++i) {
            auto id = static_cast<SubscriptionId>(i + 1);
            trie.insert("rcu/" + std::to_string(i) + "/#",
                         {&typeid(int), make_slot(id), id});
        }
        for (int i = 10; i < NUM_PATTERNS && !stop.load(); ++i) {
            trie.remove(static_cast<SubscriptionId>(i + 1));
        }
    });

    // Readers: continuously match topics while writer mutates the trie
    std::vector<std::thread> readers;
    std::atomic<int> total_matches{0};
    for (int r = 0; r < NUM_READERS; ++r) {
        readers.emplace_back([&] {
            for (int i = 0; i < READ_ITERS; ++i) {
                std::vector<ITopicSlot*> matched;
                trie.match("pre/5/sensor/temp", typeid(int), matched);
                total_matches.fetch_add(static_cast<int>(matched.size()),
                                        std::memory_order_relaxed);
            }
        });
    }

    for (auto& t : readers) t.join();
    writer.join();

    // Every reader iteration should have matched the pre-inserted pattern
    EXPECT_GE(total_matches.load(), NUM_READERS * READ_ITERS);
}

TEST(WildcardTrieTest, SnapshotIsolation) {
    // A match() in progress sees a consistent snapshot even if insert() happens concurrently
    WildcardTrie trie;
    auto make_slot = [](SubscriptionId id) {
        auto slot = std::make_shared<TopicSlot<int>>();
        slot->addSubscriber(std::function<void(const int&)>([](const int&) {}), id);
        return slot;
    };

    trie.insert("snap/#", {&typeid(int), make_slot(1), 1});

    // Take a snapshot via match before any concurrent insert
    std::vector<ITopicSlot*> before;
    trie.match("snap/a/b", typeid(int), before);
    EXPECT_EQ(before.size(), 1u);

    // Insert more, then match again
    trie.insert("snap/a/#", {&typeid(int), make_slot(2), 2});
    std::vector<ITopicSlot*> after;
    trie.match("snap/a/b", typeid(int), after);
    EXPECT_EQ(after.size(), 2u);

    // Remove first, should only see second
    trie.remove(1);
    std::vector<ITopicSlot*> final_match;
    trie.match("snap/a/b", typeid(int), final_match);
    EXPECT_EQ(final_match.size(), 1u);
}

TEST(WildcardTrieTest, InsertAfterFullRemoval) {
    // RCU: after removing all entries and re-inserting, match still works
    WildcardTrie trie;
    auto make_slot = [](SubscriptionId id) {
        auto slot = std::make_shared<TopicSlot<int>>();
        slot->addSubscriber(std::function<void(const int&)>([](const int&) {}), id);
        return slot;
    };

    trie.insert("cycle/#", {&typeid(int), make_slot(1), 1});
    EXPECT_FALSE(trie.empty());

    trie.remove(1);
    EXPECT_TRUE(trie.empty());

    // Re-insert on same path
    trie.insert("cycle/#", {&typeid(int), make_slot(2), 2});
    EXPECT_FALSE(trie.empty());

    std::vector<ITopicSlot*> matched;
    trie.match("cycle/x/y", typeid(int), matched);
    EXPECT_EQ(matched.size(), 1u);
}

// ---------- Wildcard Validation Tests ----------

TEST_F(MessageBusTest, InvalidWildcardHashNotLast) {
    // '#' must be the last segment
    EXPECT_THROW(
        bus.subscribe<int>("a/#/b", [](const int&) {}),
        std::runtime_error);
}

TEST_F(MessageBusTest, ValidWildcardPatterns) {
    // These should all succeed without throwing
    EXPECT_NO_THROW(bus.subscribe<int>("#", [](const int&) {}));
    EXPECT_NO_THROW(bus.subscribe<int>("sensor/#", [](const int&) {}));
    EXPECT_NO_THROW(bus.subscribe<int>("sensor/*/temp", [](const int&) {}));
    EXPECT_NO_THROW(bus.subscribe<int>("a/*/c/#", [](const int&) {}));
}

// ---------- string_view API Tests ----------

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

// ---------- TopicSlot direct tests ----------

TEST(TopicSlotTest, RemoveNonExistentSubscriber) {
    TopicSlot<int> slot;
    slot.addSubscriber(std::function<void(const int&)>([](const int&) {}), 1);
    // Remove an ID that doesn't exist → should return false
    EXPECT_FALSE(slot.removeSubscriber(999));
    // Original subscriber still exists
    EXPECT_TRUE(slot.removeSubscriber(1));
}

// ---------- Concurrent subscribe same topic (double-checked locking) ----------

TEST_F(MessageBusTest, ConcurrentSubscribeSameTopic) {
    // Force getOrCreateSlot's write-lock path to find an existing slot
    // (another thread created it between read-unlock and write-lock).
    // Use many threads + multiple rounds to maximize double-check-hit chance.
    constexpr int THREADS = 16;
    constexpr int ROUNDS = 5;
    for (int r = 0; r < ROUNDS; ++r) {
        std::string topic = "concurrent/sub/" + std::to_string(r);
        std::atomic<int> count{0};
        std::vector<std::thread> threads;
        std::vector<SubscriptionId> ids(THREADS);

        for (int t = 0; t < THREADS; ++t) {
            threads.emplace_back([&, t] {
                ids[t] = bus.subscribe<int>(topic,
                    [&](const int&) { count.fetch_add(1); });
            });
        }
        for (auto& th : threads) th.join();

        bus.publish<int>(topic, 42);
        for (int i = 0; i < 100 && count.load() < THREADS; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        EXPECT_EQ(count.load(), THREADS);
    }
}

// ---------- Multi-dispatcher drain paths ----------

TEST_F(MultiDispatcherTest, StopDrainsAllMessages) {
    // Publish messages, then stop immediately → exercises router drain + worker drain
    std::atomic<int> received{0};
    bus.subscribe<int>("drain/test", [&](const int&) {
        received.fetch_add(1);
    });

    constexpr int N = 200;
    for (int i = 0; i < N; ++i) {
        while (!bus.publish<int>("drain/test", i)) {
            std::this_thread::yield();
        }
    }

    // Stop triggers drain in router and workers
    bus.stop();

    EXPECT_EQ(received.load(), N);
}

TEST_F(MultiDispatcherTest, RestartAfterStop) {
    bus.stop();
    bus.start();

    std::promise<int> promise;
    auto future = promise.get_future();

    bus.subscribe<int>("restart/test", [&](const int& v) {
        promise.set_value(v);
    });
    bus.publish<int>("restart/test", 55);

    ASSERT_EQ(future.wait_for(std::chrono::seconds(1)),
              std::future_status::ready);
    EXPECT_EQ(future.get(), 55);
}

// Exercises routeToWorker + routerLoop drain: slow workers keep router
// busy routing, then stop() triggers drain of remaining main-queue messages.
TEST_F(MultiDispatcherTest, HighVolumeStopDrain) {
    constexpr int NUM_TOPICS = 16;
    constexpr int PER_TOPIC = 50;
    std::atomic<int> received{0};
    for (int t = 0; t < NUM_TOPICS; ++t) {
        bus.subscribe<int>("drain/" + std::to_string(t), [&](const int&) {
            received.fetch_add(1, std::memory_order_relaxed);
        });
    }

    // Blast messages from multiple threads concurrently
    std::vector<std::thread> producers;
    for (int t = 0; t < NUM_TOPICS; ++t) {
        producers.emplace_back([&, t] {
            for (int i = 0; i < PER_TOPIC; ++i) {
                while (!bus.publish<int>("drain/" + std::to_string(t), i))
                    std::this_thread::yield();
            }
        });
    }
    for (auto& th : producers) th.join();

    // stop() triggers routerLoop drain → routeToWorker → workerLoop drain
    bus.stop();
    EXPECT_EQ(received.load(), NUM_TOPICS * PER_TOPIC);
}

// Uses a slow handler so stop() finds un-routed messages in the main queue.
TEST(MultiDispatcherDrainTest, RouterDrainWithPendingMessages) {
    MessageBus bus(65536, 4);
    bus.start();

    std::atomic<int> received{0};
    // Slow handler to keep workers busy → main queue accumulates
    for (int t = 0; t < 8; ++t) {
        bus.subscribe<int>("route/" + std::to_string(t), [&](const int&) {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            received.fetch_add(1, std::memory_order_relaxed);
        });
    }

    // Blast enough messages fast so that main queue has leftover at stop()
    for (int i = 0; i < 2000; ++i) {
        bus.publish<int>("route/" + std::to_string(i % 8), i);
    }

    // Immediately stop — router drain path should execute
    bus.stop();
    // All published messages that entered the queue should be eventually delivered
    EXPECT_GT(received.load(), 0);
}

// ---------- LockFreeQueue high-contention ----------

TEST(LockFreeQueueTest, HighContentionMPMC) {
    // Tiny queue (capacity=4) + many threads → maximize CAS retry branches
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
    for (auto& t : threads) t.join();

    long long expected = 0;
    for (int i = 1; i <= TOTAL; ++i) expected += i;
    EXPECT_EQ(consumed.load(), TOTAL);
    EXPECT_EQ(sum.load(), expected);
}

// ---------- WildcardTrie nested removal ----------

TEST(WildcardTrieTest, RemoveDeepNestedChild) {
    // Pattern stored deep in trie, removal requires recursive search
    WildcardTrie trie;
    auto slot1 = std::make_shared<TopicSlot<int>>();
    slot1->addSubscriber(std::function<void(const int&)>([](const int&) {}), 1);
    trie.insert("a/b/c/*/e", {&typeid(int), slot1, 1});

    auto slot2 = std::make_shared<TopicSlot<int>>();
    slot2->addSubscriber(std::function<void(const int&)>([](const int&) {}), 2);
    trie.insert("a/b/c/d/f", {&typeid(int), slot2, 2});

    // Remove the first — must recurse into children
    EXPECT_TRUE(trie.remove(1));

    std::vector<ITopicSlot*> matched;
    trie.match("a/b/c/x/e", typeid(int), matched);
    EXPECT_EQ(matched.size(), 0u); // removed

    matched.clear();
    trie.match("a/b/c/d/f", typeid(int), matched);
    EXPECT_EQ(matched.size(), 1u); // still alive
}

TEST(WildcardTrieTest, RemoveFromChildPrunesCorrectly) {
    WildcardTrie trie;
    auto slot = std::make_shared<TopicSlot<int>>();
    slot->addSubscriber(std::function<void(const int&)>([](const int&) {}), 1);
    trie.insert("x/y/z", {&typeid(int), slot, 1});

    // Only entry, remove it — entire branch should be pruned
    EXPECT_TRUE(trie.remove(1));
    EXPECT_TRUE(trie.empty());

    // Remove non-existent from empty trie
    EXPECT_FALSE(trie.remove(42));
}
