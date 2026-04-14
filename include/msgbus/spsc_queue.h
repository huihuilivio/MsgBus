#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <utility>

namespace msgbus {

/// Bounded SPSC (Single Producer, Single Consumer) lock-free ring queue.
///
/// Compared to LockFreeQueue (MPMC Vyukov), this has:
///   - Zero CAS on the normal path (pure load/store)
///   - enqueue_overwrite() for DropOldest semantics
///
/// Thread model:
///   - Exactly ONE thread calls try_enqueue / enqueue_overwrite (producer)
///   - Exactly ONE thread calls try_dequeue (consumer)
///   - Violating this is undefined behavior.
template <typename T>
class SPSCQueue {
public:
    explicit SPSCQueue(size_t capacity)
        : capacity_(roundUpPowerOf2(capacity))
        , mask_(capacity_ - 1)
        , buffer_(new T[capacity_])
        , write_pos_(0)
        , read_pos_(0) {}

    ~SPSCQueue() {
        T dummy;
        while (try_dequeue(dummy)) {}
    }

    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;

    /// Enqueue a value. Returns false if queue is full.
    /// Lock-free: single atomic store (no CAS).
    bool try_enqueue(T value) {
        size_t w = write_pos_.load(std::memory_order_relaxed);
        size_t r = read_pos_.load(std::memory_order_acquire);
        if (w - r >= capacity_) return false; // full
        buffer_[w & mask_] = std::move(value);
        write_pos_.store(w + 1, std::memory_order_release);
        return true;
    }

    /// Enqueue a value, dropping the oldest entry if full (DropOldest).
    /// Always succeeds. Uses CAS on read_pos_ only when the queue is full
    /// to safely advance the consumer's position.
    void enqueue_overwrite(T value) {
        size_t w = write_pos_.load(std::memory_order_relaxed);
        if (w - read_pos_.load(std::memory_order_acquire) >= capacity_) {
            // Full: advance consumer's read position (drop oldest).
            // CAS ensures correctness if consumer concurrently dequeues.
            // If CAS fails, consumer already freed a slot — either way we proceed.
            size_t r = read_pos_.load(std::memory_order_relaxed);
            read_pos_.compare_exchange_strong(r, r + 1,
                std::memory_order_release, std::memory_order_relaxed);
        }
        buffer_[w & mask_] = std::move(value);
        write_pos_.store(w + 1, std::memory_order_release);
    }

    /// Dequeue a value. Returns false if queue is empty.
    /// Lock-free: single atomic store (no CAS).
    bool try_dequeue(T& value) {
        size_t r = read_pos_.load(std::memory_order_relaxed);
        size_t w = write_pos_.load(std::memory_order_acquire);
        if (r == w) return false; // empty
        value = std::move(buffer_[r & mask_]);
        read_pos_.store(r + 1, std::memory_order_release);
        return true;
    }

    /// Current number of elements (approximate, for diagnostics only).
    size_t size_approx() const {
        size_t w = write_pos_.load(std::memory_order_relaxed);
        size_t r = read_pos_.load(std::memory_order_relaxed);
        return w - r;
    }

    size_t capacity() const { return capacity_; }

private:
    static size_t roundUpPowerOf2(size_t v) {
        v--;
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
        v |= v >> 32;
        v++;
        return v < 2 ? 2 : v;
    }

    size_t capacity_;
    size_t mask_;
    std::unique_ptr<T[]> buffer_;
    alignas(64) std::atomic<size_t> write_pos_;
    alignas(64) std::atomic<size_t> read_pos_;
};

} // namespace msgbus
