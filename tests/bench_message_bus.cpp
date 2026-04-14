#include "msgbus/message_bus.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::high_resolution_clock;

// ============================================================
// Helper: measure latency of each message publish->handler
// ============================================================
struct LatencyStats {
    double min_us;
    double max_us;
    double avg_us;
    double p50_us;
    double p90_us;
    double p99_us;
    double p999_us;
};

static LatencyStats computeStats(std::vector<double>& samples) {
    std::sort(samples.begin(), samples.end());
    size_t n = samples.size();
    double sum = std::accumulate(samples.begin(), samples.end(), 0.0);
    auto pct = [&](double p) -> double {
        size_t idx = static_cast<size_t>(p * static_cast<double>(n));
        if (idx >= n) idx = n - 1;
        return samples[idx];
    };
    return {
        samples.front(), samples.back(), sum / static_cast<double>(n),
        pct(0.50), pct(0.90), pct(0.99), pct(0.999)
    };
}

static void printStats(const char* label, const LatencyStats& s) {
    std::printf("  %-22s  min=%.2f  avg=%.2f  p50=%.2f  p90=%.2f  "
                "p99=%.2f  p99.9=%.2f  max=%.2f  (us)\n",
                label, s.min_us, s.avg_us, s.p50_us, s.p90_us,
                s.p99_us, s.p999_us, s.max_us);
}

// ============================================================
// Bench 1: Throughput — single publisher, single subscriber
// ============================================================
static void benchThroughput(int total_messages) {
    std::printf("\n=== Throughput: 1 pub / 1 sub, %d msgs ===\n", total_messages);

    msgbus::MessageBus bus(65536);
    std::atomic<int> received{0};

    bus.subscribe<int>("bench/tp", [&](const int&) {
        received.fetch_add(1, std::memory_order_relaxed);
    });

    bus.start();

    auto t0 = Clock::now();
    for (int i = 0; i < total_messages; ++i) {
        while (!bus.publish<int>("bench/tp", i)) {
            std::this_thread::yield();
        }
    }

    // Wait for all to be delivered
    while (received.load(std::memory_order_relaxed) < total_messages) {
        std::this_thread::yield();
    }
    auto t1 = Clock::now();

    bus.stop();

    double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double qps = static_cast<double>(total_messages) / (elapsed_ms / 1000.0);
    std::printf("  Elapsed: %.2f ms  |  QPS: %.0f\n", elapsed_ms, qps);
}

// ============================================================
// Bench 2: Multi-producer throughput
// ============================================================
static void benchMultiProducer(int num_threads, int per_thread) {
    int total = num_threads * per_thread;
    std::printf("\n=== Multi-producer: %d threads x %d msgs = %d total ===\n",
                num_threads, per_thread, total);

    msgbus::MessageBus bus(65536);
    std::atomic<int> received{0};

    bus.subscribe<int>("bench/mp", [&](const int&) {
        received.fetch_add(1, std::memory_order_relaxed);
    });
    bus.start();

    auto t0 = Clock::now();

    std::vector<std::thread> producers;
    producers.reserve(num_threads);
    for (int t = 0; t < num_threads; ++t) {
        producers.emplace_back([&, t] {
            for (int i = 0; i < per_thread; ++i) {
                while (!bus.publish<int>("bench/mp", t * per_thread + i)) {
                    std::this_thread::yield();
                }
            }
        });
    }
    for (auto& th : producers) th.join();

    while (received.load(std::memory_order_relaxed) < total) {
        std::this_thread::yield();
    }
    auto t1 = Clock::now();
    bus.stop();

    double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double qps = static_cast<double>(total) / (elapsed_ms / 1000.0);
    std::printf("  Elapsed: %.2f ms  |  QPS: %.0f\n", elapsed_ms, qps);
}

// ============================================================
// Bench 3: Latency — publish-to-handler per-message latency
// ============================================================
struct TimestampedMsg {
    Clock::time_point publish_time;
};

static void benchLatency(int total_messages) {
    std::printf("\n=== Latency: 1 pub / 1 sub, %d msgs ===\n", total_messages);

    msgbus::MessageBus bus(static_cast<size_t>(total_messages * 2));

    std::mutex lat_mu;
    std::vector<double> latencies;
    latencies.reserve(total_messages);
    std::atomic<int> received{0};

    bus.subscribe<TimestampedMsg>("bench/lat",
        [&](const TimestampedMsg& msg) {
            auto now = Clock::now();
            double us = std::chrono::duration<double, std::micro>(
                            now - msg.publish_time).count();
            {
                std::lock_guard<std::mutex> lk(lat_mu);
                latencies.push_back(us);
            }
            received.fetch_add(1, std::memory_order_relaxed);
        });

    bus.start();

    // Warm up
    for (int i = 0; i < 1000; ++i) {
        bus.publish<TimestampedMsg>("bench/lat", {Clock::now()});
    }
    while (received.load(std::memory_order_relaxed) < 1000) {
        std::this_thread::yield();
    }

    // Reset
    {
        std::lock_guard<std::mutex> lk(lat_mu);
        latencies.clear();
    }
    received.store(0, std::memory_order_relaxed);

    for (int i = 0; i < total_messages; ++i) {
        while (!bus.publish<TimestampedMsg>("bench/lat", {Clock::now()})) {
            std::this_thread::yield();
        }
    }
    while (received.load(std::memory_order_relaxed) < total_messages) {
        std::this_thread::yield();
    }
    bus.stop();

    auto stats = computeStats(latencies);
    printStats("Publish->Handler", stats);
}

// ============================================================
// Bench 4: Multi-topic throughput
// ============================================================
static void benchMultiTopic(int num_topics, int msgs_per_topic) {
    int total = num_topics * msgs_per_topic;
    std::printf("\n=== Multi-topic: %d topics x %d msgs = %d total ===\n",
                num_topics, msgs_per_topic, total);

    msgbus::MessageBus bus(65536);
    std::atomic<int> received{0};

    for (int t = 0; t < num_topics; ++t) {
        bus.subscribe<int>("bench/topic/" + std::to_string(t),
            [&](const int&) {
                received.fetch_add(1, std::memory_order_relaxed);
            });
    }
    bus.start();

    auto t0 = Clock::now();
    for (int t = 0; t < num_topics; ++t) {
        std::string topic = "bench/topic/" + std::to_string(t);
        for (int i = 0; i < msgs_per_topic; ++i) {
            while (!bus.publish<int>(topic, i)) {
                std::this_thread::yield();
            }
        }
    }
    while (received.load(std::memory_order_relaxed) < total) {
        std::this_thread::yield();
    }
    auto t1 = Clock::now();
    bus.stop();

    double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double qps = static_cast<double>(total) / (elapsed_ms / 1000.0);
    std::printf("  Elapsed: %.2f ms  |  QPS: %.0f\n", elapsed_ms, qps);
}

// ============================================================
// Bench 5: Fan-out — 1 publisher, N subscribers per topic
// ============================================================
static void benchFanOut(int num_subscribers, int total_messages) {
    std::printf("\n=== Fan-out: 1 pub / %d subs, %d msgs ===\n",
                num_subscribers, total_messages);

    msgbus::MessageBus bus(65536);
    std::atomic<int> received{0};
    int expected = total_messages * num_subscribers;

    for (int s = 0; s < num_subscribers; ++s) {
        bus.subscribe<int>("bench/fan", [&](const int&) {
            received.fetch_add(1, std::memory_order_relaxed);
        });
    }
    bus.start();

    auto t0 = Clock::now();
    for (int i = 0; i < total_messages; ++i) {
        while (!bus.publish<int>("bench/fan", i)) {
            std::this_thread::yield();
        }
    }
    while (received.load(std::memory_order_relaxed) < expected) {
        std::this_thread::yield();
    }
    auto t1 = Clock::now();
    bus.stop();

    double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double deliveries_per_sec = static_cast<double>(expected) / (elapsed_ms / 1000.0);
    std::printf("  Elapsed: %.2f ms  |  Deliveries/s: %.0f  (total deliveries: %d)\n",
                elapsed_ms, deliveries_per_sec, expected);
}

// ============================================================
// Bench 6: Lock-free queue raw throughput
// ============================================================
static void benchQueueRaw(int total_messages) {
    std::printf("\n=== Raw Queue: 1 prod / 1 cons, %d msgs ===\n", total_messages);

    msgbus::LockFreeQueue<int> q(static_cast<size_t>(total_messages * 2));
    std::atomic<int> consumed{0};

    auto t0 = Clock::now();

    std::thread consumer([&] {
        int val;
        while (consumed.load(std::memory_order_relaxed) < total_messages) {
            if (q.try_dequeue(val)) {
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    for (int i = 0; i < total_messages; ++i) {
        while (!q.try_enqueue(i)) {
            std::this_thread::yield();
        }
    }

    consumer.join();
    auto t1 = Clock::now();

    double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double qps = static_cast<double>(total_messages) / (elapsed_ms / 1000.0);
    std::printf("  Elapsed: %.2f ms  |  Ops/s: %.0f\n", elapsed_ms, qps);
}

// ============================================================
// Bench 7: Wildcard matching with trie — N patterns, M messages
// ============================================================
static void benchWildcard(int num_patterns, int total_messages) {
    std::printf("\n=== Wildcard: %d patterns, %d msgs ===\n",
                num_patterns, total_messages);

    msgbus::MessageBus bus(65536);
    std::atomic<int> received{0};

    // Create N wildcard patterns: "bench/wild/N/#"
    for (int p = 0; p < num_patterns; ++p) {
        std::string pattern = "bench/wild/" + std::to_string(p) + "/#";
        bus.subscribe<int>(pattern, [&](const int&) {
            received.fetch_add(1, std::memory_order_relaxed);
        });
    }
    bus.start();

    // Publish to topics that match pattern 0 only
    auto t0 = Clock::now();
    for (int i = 0; i < total_messages; ++i) {
        while (!bus.publish<int>("bench/wild/0/sensor/temp", i)) {
            std::this_thread::yield();
        }
    }
    while (received.load(std::memory_order_relaxed) < total_messages) {
        std::this_thread::yield();
    }
    auto t1 = Clock::now();
    bus.stop();

    double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double qps = static_cast<double>(total_messages) / (elapsed_ms / 1000.0);
    std::printf("  Elapsed: %.2f ms  |  QPS: %.0f\n", elapsed_ms, qps);
}

// ============================================================
// Bench 8: Wildcard trie RCU — concurrent match throughput
// ============================================================
static void benchWildcardRCU(int num_patterns, int total_messages, int num_readers) {
    std::printf("\n=== Wildcard RCU: %d patterns, %d msgs, %d readers ===\n",
                num_patterns, total_messages, num_readers);

    msgbus::MessageBus bus(65536, static_cast<unsigned>(num_readers));
    std::atomic<int> received{0};

    for (int p = 0; p < num_patterns; ++p) {
        std::string pattern = "bench/rcu/" + std::to_string(p) + "/#";
        bus.subscribe<int>(pattern, [&](const int&) {
            received.fetch_add(1, std::memory_order_relaxed);
        });
    }
    bus.start();

    auto t0 = Clock::now();
    for (int i = 0; i < total_messages; ++i) {
        while (!bus.publish<int>("bench/rcu/0/sensor/temp", i)) {
            std::this_thread::yield();
        }
    }
    while (received.load(std::memory_order_relaxed) < total_messages) {
        std::this_thread::yield();
    }
    auto t1 = Clock::now();
    bus.stop();

    double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double qps = static_cast<double>(total_messages) / (elapsed_ms / 1000.0);
    std::printf("  Elapsed: %.2f ms  |  QPS: %.0f\n", elapsed_ms, qps);
}

// ============================================================
// Bench 9: FullPolicy throughput — compare all 5 strategies
// ============================================================
static const char* policyName(msgbus::FullPolicy p) {
    switch (p) {
    case msgbus::FullPolicy::ReturnFalse: return "ReturnFalse";
    case msgbus::FullPolicy::DropOldest:  return "DropOldest";
    case msgbus::FullPolicy::DropNewest:  return "DropNewest";
    case msgbus::FullPolicy::Block:       return "Block";
    case msgbus::FullPolicy::BlockTimeout:return "BlockTimeout";
    }
    return "Unknown";
}

/// Single-producer throughput under each policy.
/// Uses a small queue (1024) so the queue is frequently full, exercising the policy path.
static void benchPolicySingleProducer(msgbus::FullPolicy policy, int total_messages) {
    constexpr size_t kSmallQueue = 1024;

    msgbus::MessageBus bus(kSmallQueue, 1, policy, std::chrono::milliseconds{50});
    std::atomic<int> received{0};

    bus.subscribe<int>("bench/policy", [&](const int&) {
        received.fetch_add(1, std::memory_order_relaxed);
    });
    bus.start();

    int published = 0;
    auto t0 = Clock::now();
    for (int i = 0; i < total_messages; ++i) {
        if (bus.publish<int>("bench/policy", i)) {
            ++published;
        }
    }
    bus.stop(); // drains remaining messages in queue
    auto t1 = Clock::now();

    int delivered = received.load(std::memory_order_relaxed);
    double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double qps = static_cast<double>(published) / (elapsed_ms / 1000.0);
    std::printf("  %-14s  published=%d  delivered=%d  elapsed=%.2f ms  QPS=%.0f\n",
                policyName(policy), published, delivered, elapsed_ms, qps);
}

/// Multi-producer throughput under each policy.
static void benchPolicyMultiProducer(msgbus::FullPolicy policy,
                                     int num_threads, int per_thread) {
    constexpr size_t kSmallQueue = 1024;
    int total = num_threads * per_thread;

    msgbus::MessageBus bus(kSmallQueue, 1, policy, std::chrono::milliseconds{50});
    std::atomic<int> received{0};
    std::atomic<int> published{0};

    bus.subscribe<int>("bench/policy/mp", [&](const int&) {
        received.fetch_add(1, std::memory_order_relaxed);
    });
    bus.start();

    auto t0 = Clock::now();
    std::vector<std::thread> producers;
    producers.reserve(num_threads);
    for (int t = 0; t < num_threads; ++t) {
        producers.emplace_back([&, t] {
            for (int i = 0; i < per_thread; ++i) {
                if (bus.publish<int>("bench/policy/mp", t * per_thread + i)) {
                    published.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : producers) th.join();

    bus.stop(); // drains remaining messages
    auto t1 = Clock::now();

    int pub = published.load(std::memory_order_relaxed);
    int delivered = received.load(std::memory_order_relaxed);
    double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double qps = static_cast<double>(pub) / (elapsed_ms / 1000.0);
    std::printf("  %-14s  published=%d  delivered=%d  elapsed=%.2f ms  QPS=%.0f\n",
                policyName(policy), pub, delivered, elapsed_ms, qps);
}

/// Publish-to-handler latency under each policy.
static void benchPolicyLatency(msgbus::FullPolicy policy, int total_messages) {
    constexpr size_t kSmallQueue = 4096;

    msgbus::MessageBus bus(kSmallQueue, 1, policy, std::chrono::milliseconds{50});

    std::mutex lat_mu;
    std::vector<double> latencies;
    latencies.reserve(total_messages);
    std::atomic<int> received{0};

    bus.subscribe<TimestampedMsg>("bench/policy/lat",
        [&](const TimestampedMsg& msg) {
            auto now = Clock::now();
            double us = std::chrono::duration<double, std::micro>(
                            now - msg.publish_time).count();
            {
                std::lock_guard<std::mutex> lk(lat_mu);
                latencies.push_back(us);
            }
            received.fetch_add(1, std::memory_order_relaxed);
        });

    bus.start();

    // Warm up
    for (int i = 0; i < 500; ++i) {
        bus.publish<TimestampedMsg>("bench/policy/lat", {Clock::now()});
    }
    while (received.load(std::memory_order_relaxed) < 500) {
        std::this_thread::yield();
    }
    {
        std::lock_guard<std::mutex> lk(lat_mu);
        latencies.clear();
    }
    received.store(0, std::memory_order_relaxed);

    int published = 0;
    for (int i = 0; i < total_messages; ++i) {
        if (bus.publish<TimestampedMsg>("bench/policy/lat", {Clock::now()})) {
            ++published;
        }
    }
    bus.stop(); // drains queue

    int delivered = received.load(std::memory_order_relaxed);
    if (!latencies.empty()) {
        auto stats = computeStats(latencies);
        std::printf("  %-14s  ", policyName(policy));
        std::printf("delivered=%d  min=%.2f  avg=%.2f  p50=%.2f  p90=%.2f  "
                    "p99=%.2f  max=%.2f  (us)\n",
                    delivered, stats.min_us, stats.avg_us, stats.p50_us,
                    stats.p90_us, stats.p99_us, stats.max_us);
    }
}

static void benchFullPolicySuite() {
    constexpr int N = 500'000;

    std::printf("\n=== FullPolicy Single-Producer Throughput (%d msgs, queue=1024) ===\n", N);
    benchPolicySingleProducer(msgbus::FullPolicy::ReturnFalse, N);
    benchPolicySingleProducer(msgbus::FullPolicy::DropNewest, N);
    benchPolicySingleProducer(msgbus::FullPolicy::DropOldest, N);
    benchPolicySingleProducer(msgbus::FullPolicy::Block, N);
    benchPolicySingleProducer(msgbus::FullPolicy::BlockTimeout, N);

    std::printf("\n=== FullPolicy Multi-Producer Throughput (4 threads x %d msgs, queue=1024) ===\n", N / 4);
    benchPolicyMultiProducer(msgbus::FullPolicy::ReturnFalse, 4, N / 4);
    benchPolicyMultiProducer(msgbus::FullPolicy::DropNewest, 4, N / 4);
    benchPolicyMultiProducer(msgbus::FullPolicy::DropOldest, 4, N / 4);
    benchPolicyMultiProducer(msgbus::FullPolicy::Block, 4, N / 4);
    benchPolicyMultiProducer(msgbus::FullPolicy::BlockTimeout, 4, N / 4);

    constexpr int LAT_N = 50'000;
    std::printf("\n=== FullPolicy Latency (%d msgs, queue=4096) ===\n", LAT_N);
    benchPolicyLatency(msgbus::FullPolicy::ReturnFalse, LAT_N);
    benchPolicyLatency(msgbus::FullPolicy::DropNewest, LAT_N);
    benchPolicyLatency(msgbus::FullPolicy::DropOldest, LAT_N);
    benchPolicyLatency(msgbus::FullPolicy::Block, LAT_N);
    benchPolicyLatency(msgbus::FullPolicy::BlockTimeout, LAT_N);
}

// ============================================================
// Multi-round runner — runs a throughput benchmark N times, prints median QPS
// ============================================================
static double measureQPS(std::function<double()> fn, int rounds = 3) {
    std::vector<double> results;
    results.reserve(rounds);
    for (int r = 0; r < rounds; ++r) {
        results.push_back(fn());
    }
    std::sort(results.begin(), results.end());
    return results[rounds / 2]; // median
}

// ============================================================
// Bench 10: Multi-dispatcher exact-match throughput
// ============================================================
static void benchMultiDispatcher(int num_dispatchers, int total_messages) {
    std::printf("\n=== Multi-dispatcher: %d dispatchers, %d msgs ===\n",
                num_dispatchers, total_messages);

    msgbus::MessageBus bus(65536, static_cast<unsigned>(num_dispatchers));
    std::atomic<int> received{0};

    // Subscribe on multiple topics to spread across dispatchers
    for (int t = 0; t < 10; ++t) {
        bus.subscribe<int>("bench/md/" + std::to_string(t), [&](const int&) {
            received.fetch_add(1, std::memory_order_relaxed);
        });
    }
    bus.start();

    auto t0 = Clock::now();
    for (int i = 0; i < total_messages; ++i) {
        std::string topic = "bench/md/" + std::to_string(i % 10);
        while (!bus.publish<int>(topic, i)) {
            std::this_thread::yield();
        }
    }
    while (received.load(std::memory_order_relaxed) < total_messages) {
        std::this_thread::yield();
    }
    auto t1 = Clock::now();
    bus.stop();

    double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double qps = static_cast<double>(total_messages) / (elapsed_ms / 1000.0);
    std::printf("  Elapsed: %.2f ms  |  QPS: %.0f\n", elapsed_ms, qps);
}

// ============================================================
// Bench 11: Concurrent subscribe/unsubscribe during publish
// ============================================================
static void benchConcurrentSubUnsub(int total_messages) {
    std::printf("\n=== Concurrent Sub/Unsub: %d msgs, 2 sub-churners ===\n",
                total_messages);

    msgbus::MessageBus bus(65536);
    std::atomic<int> received{0};
    std::atomic<bool> churning{true};

    // Stable subscriber
    bus.subscribe<int>("bench/churn", [&](const int&) {
        received.fetch_add(1, std::memory_order_relaxed);
    });
    bus.start();

    // Subscription churner threads: subscribe + unsubscribe rapidly
    std::vector<std::thread> churners;
    std::atomic<int> churn_ops{0};
    for (int c = 0; c < 2; ++c) {
        churners.emplace_back([&] {
            while (churning.load(std::memory_order_relaxed)) {
                auto id = bus.subscribe<int>("bench/churn",
                    [](const int&) {});
                bus.unsubscribe(id);
                churn_ops.fetch_add(2, std::memory_order_relaxed);
            }
        });
    }

    auto t0 = Clock::now();
    for (int i = 0; i < total_messages; ++i) {
        while (!bus.publish<int>("bench/churn", i)) {
            std::this_thread::yield();
        }
    }
    while (received.load(std::memory_order_relaxed) < total_messages) {
        std::this_thread::yield();
    }
    auto t1 = Clock::now();

    churning.store(false, std::memory_order_relaxed);
    for (auto& t : churners) t.join();
    bus.stop();

    double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double qps = static_cast<double>(total_messages) / (elapsed_ms / 1000.0);
    int ops = churn_ops.load(std::memory_order_relaxed);
    std::printf("  Elapsed: %.2f ms  |  Publish QPS: %.0f  |  Sub/Unsub ops: %d\n",
                elapsed_ms, qps, ops);
}

// ============================================================
// Bench 12: Raw Queue MPMC — 4 producers / 4 consumers
// ============================================================
static void benchQueueMPMC(int total_messages, int num_producers, int num_consumers) {
    std::printf("\n=== Raw Queue MPMC: %dP/%dC, %d msgs ===\n",
                num_producers, num_consumers, total_messages);

    msgbus::LockFreeQueue<int> q(65536);
    int per_producer = total_messages / num_producers;
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};

    auto t0 = Clock::now();

    std::vector<std::thread> consumers;
    for (int c = 0; c < num_consumers; ++c) {
        consumers.emplace_back([&] {
            int val;
            while (consumed.load(std::memory_order_relaxed) < total_messages) {
                if (q.try_dequeue(val)) {
                    consumed.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    std::vector<std::thread> producers;
    for (int p = 0; p < num_producers; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < per_producer; ++i) {
                while (!q.try_enqueue(p * per_producer + i)) {
                    std::this_thread::yield();
                }
            }
        });
    }

    for (auto& t : producers) t.join();
    for (auto& t : consumers) t.join();
    auto t1 = Clock::now();

    double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double qps = static_cast<double>(total_messages) / (elapsed_ms / 1000.0);
    std::printf("  Elapsed: %.2f ms  |  Ops/s: %.0f\n", elapsed_ms, qps);
}

// ============================================================
// Bench 13: ObjectPool hit rate impact
// ============================================================
static void benchPoolHitRate(int total_messages, bool with_pool) {
    std::printf("\n=== Pool %s: %d msgs ===\n",
                with_pool ? "Enabled" : "Bypassed", total_messages);

    msgbus::MessageBus bus(65536);
    std::atomic<int> received{0};

    if (with_pool) {
        // Warm up pool: publish+drain some messages first
        bus.subscribe<int>("bench/pool", [&](const int&) {
            received.fetch_add(1, std::memory_order_relaxed);
        });
        bus.start();
        for (int i = 0; i < 10000; ++i) {
            bus.publish<int>("bench/pool", i);
        }
        while (received.load(std::memory_order_relaxed) < 10000) {
            std::this_thread::yield();
        }
        received.store(0, std::memory_order_relaxed);
    } else {
        // Use a large payload type unlikely to be cached
        bus.subscribe<std::string>("bench/pool", [&](const std::string&) {
            received.fetch_add(1, std::memory_order_relaxed);
        });
        bus.start();
    }

    auto t0 = Clock::now();
    if (with_pool) {
        for (int i = 0; i < total_messages; ++i) {
            while (!bus.publish<int>("bench/pool", i)) {
                std::this_thread::yield();
            }
        }
    } else {
        for (int i = 0; i < total_messages; ++i) {
            while (!bus.publish<std::string>("bench/pool",
                       std::string("payload_") + std::to_string(i))) {
                std::this_thread::yield();
            }
        }
    }
    while (received.load(std::memory_order_relaxed) < total_messages) {
        std::this_thread::yield();
    }
    auto t1 = Clock::now();
    bus.stop();

    double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double qps = static_cast<double>(total_messages) / (elapsed_ms / 1000.0);
    std::printf("  Elapsed: %.2f ms  |  QPS: %.0f\n", elapsed_ms, qps);
}

// ============================================================
// Bench 14: Stable throughput — 3 rounds, report median
// ============================================================
static void benchStableThroughput() {
    constexpr int N = 500'000;
    constexpr int ROUNDS = 3;

    std::printf("\n=== Stable Throughput: %d msgs, %d rounds (median) ===\n", N, ROUNDS);

    auto run1P1S = [&]() -> double {
        msgbus::MessageBus bus(65536);
        std::atomic<int> received{0};
        bus.subscribe<int>("bench/stable", [&](const int&) {
            received.fetch_add(1, std::memory_order_relaxed);
        });
        bus.start();
        auto t0 = Clock::now();
        for (int i = 0; i < N; ++i) {
            while (!bus.publish<int>("bench/stable", i))
                std::this_thread::yield();
        }
        while (received.load(std::memory_order_relaxed) < N)
            std::this_thread::yield();
        auto t1 = Clock::now();
        bus.stop();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        return static_cast<double>(N) / (ms / 1000.0);
    };

    double median1 = measureQPS(run1P1S, ROUNDS);
    std::printf("  1P/1S  median QPS: %.0f\n", median1);

    auto run4P = [&]() -> double {
        msgbus::MessageBus bus(65536);
        std::atomic<int> received{0};
        bus.subscribe<int>("bench/stable4", [&](const int&) {
            received.fetch_add(1, std::memory_order_relaxed);
        });
        bus.start();
        int per = N / 4;
        auto t0 = Clock::now();
        std::vector<std::thread> prods;
        for (int t = 0; t < 4; ++t) {
            prods.emplace_back([&] {
                for (int i = 0; i < per; ++i)
                    while (!bus.publish<int>("bench/stable4", i))
                        std::this_thread::yield();
            });
        }
        for (auto& th : prods) th.join();
        while (received.load(std::memory_order_relaxed) < N)
            std::this_thread::yield();
        auto t1 = Clock::now();
        bus.stop();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        return static_cast<double>(N) / (ms / 1000.0);
    };

    double median4 = measureQPS(run4P, ROUNDS);
    std::printf("  4P/1S  median QPS: %.0f\n", median4);
}

// ============================================================
// Main
// ============================================================
int main() {
    std::printf("MsgBus Performance Benchmark\n");
    std::printf("Hardware concurrency: %u\n",
                std::thread::hardware_concurrency());

    benchQueueRaw(1'000'000);
    benchThroughput(1'000'000);
    benchMultiProducer(4, 250'000);
    benchLatency(100'000);
    benchMultiTopic(100, 10'000);
    benchFanOut(10, 100'000);
    benchFanOut(100, 10'000);
    benchWildcard(100, 100'000);
    benchWildcard(1000, 100'000);
    benchWildcardRCU(100, 100'000, 4);
    benchWildcardRCU(1000, 100'000, 4);

    benchFullPolicySuite();

    benchMultiDispatcher(2, 500'000);
    benchMultiDispatcher(4, 500'000);
    benchConcurrentSubUnsub(500'000);
    benchQueueMPMC(1'000'000, 4, 4);
    benchPoolHitRate(500'000, true);
    benchPoolHitRate(500'000, false);
    benchStableThroughput();

    std::printf("\nAll benchmarks completed.\n");
    return 0;
}
