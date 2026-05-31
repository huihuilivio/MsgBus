#pragma once

#include "msgbus/lock_free_queue.h"
#include "msgbus/message.h"
#include "msgbus/message_bus.h"
#include "msgbus/object_pool.h"
#include "msgbus/topic_matcher.h"
#include "msgbus/topic_registry.h"
#include "msgbus/topic_slot.h"
#include "msgbus/wildcard_trie.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace msgbus;

class MessageBusTest : public ::testing::Test {
protected:
    MessageBus bus;

    void SetUp() override { bus.start(); }
    void TearDown() override { bus.stop(); }
};

class MultiDispatcherTest : public ::testing::Test {
protected:
    MessageBus bus{65536, 4};

    void SetUp() override { bus.start(); }
    void TearDown() override { bus.stop(); }
};

struct Task {
    struct promise_type {
        Task get_return_object() { return {}; }
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };
};

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
    ~DestroyableTask() {
        if (handle) {
            handle.destroy();
        }
    }
};