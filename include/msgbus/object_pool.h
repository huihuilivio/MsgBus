#pragma once

#include "msgbus/lock_free_queue.h"

#include <cstddef>

namespace msgbus {

/// Lock-free object pool backed by a bounded freelist queue.
/// Objects are not pre-allocated; they are cached after first use.
template <typename T>
class ObjectPool {
public:
    explicit ObjectPool(size_t capacity) : freelist_(capacity) {}

    ~ObjectPool() {
        T* p = nullptr;
        while (freelist_.try_dequeue(p)) {
            delete p;
        }
    }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    /// Try to get a recycled object. Returns nullptr if pool is empty.
    T* acquire() {
        T* p = nullptr;
        freelist_.try_dequeue(p);
        return p;
    }

    /// Return an object to the pool. Deletes it if pool is full.
    void release(T* p) {
        if (!freelist_.try_enqueue(p)) {
            delete p;
        }
    }

private:
    LockFreeQueue<T*> freelist_;
};

} // namespace msgbus
