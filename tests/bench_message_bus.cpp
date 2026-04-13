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

    msgbus::MessageBus bus(static_cast<size_t>(total_messages * 2));
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

    msgbus::MessageBus bus(static_cast<size_t>(total * 2));
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

    std::vector<double> latencies;
    latencies.reserve(total_messages);
    std::atomic<int> received{0};

    bus.subscribe<TimestampedMsg>("bench/lat",
        [&](const TimestampedMsg& msg) {
            auto now = Clock::now();
            double us = std::chrono::duration<double, std::micro>(
                            now - msg.publish_time).count();
            latencies.push_back(us);
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
    latencies.clear();
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

    msgbus::MessageBus bus(static_cast<size_t>(total * 2));
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

    msgbus::MessageBus bus(static_cast<size_t>(total_messages * 2));
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
    std::atomic<bool> done{false};
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

    std::printf("\nAll benchmarks completed.\n");
    return 0;
}
