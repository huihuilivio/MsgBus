#pragma once

#include "msgbus/lock_free_queue.h"
#include "msgbus/message.h"
#include "msgbus/object_pool.h"
#include "msgbus/subscriber.h"
#include "msgbus/topic_slot.h"

#include <atomic>
#include <chrono>
#include <coroutine>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <unordered_map>
#include <utility>

namespace msgbus {

/// Per-type static object pool and recycler.
template <typename T>
struct TypedMessagePool {
    static ObjectPool<TypedMessage<T>>& instance() {
        static ObjectPool<TypedMessage<T>> pool(8192);
        return pool;
    }

    static void recycle(IMessage* msg) {
        instance().release(static_cast<TypedMessage<T>*>(msg));
    }
};

class MessageBus {
public:
    explicit MessageBus(size_t queue_capacity = 65536)
        : queue_(queue_capacity) {}

    ~MessageBus() { stop(); }

    MessageBus(const MessageBus&) = delete;
    MessageBus& operator=(const MessageBus&) = delete;

    void start() {
        if (running_.exchange(true)) return;
        dispatcher_ = std::thread(&MessageBus::dispatchLoop, this);
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (dispatcher_.joinable()) dispatcher_.join();
    }

    /// Publish a message to a topic. Returns false if queue is full.
    /// Uses per-type object pool to avoid heap allocation on the hot path.
    template <typename T>
    bool publish(const std::string& topic, T msg) {
        auto& pool = TypedMessagePool<T>::instance();
        TypedMessage<T>* raw = pool.acquire();
        if (raw) {
            raw->reset(topic, std::move(msg));
        } else {
            raw = new TypedMessage<T>(topic, std::move(msg));
        }
        raw->recycler_ = &TypedMessagePool<T>::recycle;
        return queue_.try_enqueue(MessagePtr::adopt(raw));
    }

    /// Subscribe to a topic with a handler. Returns a subscription ID.
    template <typename T, typename Handler>
    SubscriptionId subscribe(const std::string& topic, Handler&& handler) {
        auto id = next_id_.fetch_add(1, std::memory_order_relaxed) + 1;
        auto* slot = getOrCreateSlot<T>(topic);
        slot->addSubscriber(
            std::function<void(const T&)>(std::forward<Handler>(handler)), id);

        std::lock_guard<std::mutex> lock(sub_map_mutex_);
        sub_to_topic_[id] = topic;
        return id;
    }

    /// Unsubscribe by subscription ID.
    void unsubscribe(SubscriptionId id) {
        std::string topic;
        {
            std::lock_guard<std::mutex> lock(sub_map_mutex_);
            auto it = sub_to_topic_.find(id);
            if (it == sub_to_topic_.end()) return;
            topic = it->second;
            sub_to_topic_.erase(it);
        }

        ITopicSlot* slot = nullptr;
        {
            std::shared_lock<std::shared_mutex> lock(slots_mutex_);
            auto it = slots_.find(topic);
            if (it != slots_.end()) {
                slot = it->second.get();
            }
        }
        if (slot) {
            slot->removeSubscriber(id);
        }
    }

    /// Coroutine awaitable: co_await bus.async_wait<T>(topic)
    template <typename T>
    class AsyncWaitAwaitable {
    public:
        AsyncWaitAwaitable(MessageBus& bus, std::string topic)
            : state_(std::make_shared<SharedState>(bus, std::move(topic))) {}

        bool await_ready() const noexcept { return false; }

        void await_suspend(std::coroutine_handle<> handle) {
            auto s = state_;
            s->sub_id = s->bus.template subscribe<T>(s->topic,
                [s, handle](const T& msg) {
                    s->result = msg;
                    handle.resume();
                });
        }

        T await_resume() {
            state_->bus.unsubscribe(state_->sub_id);
            return std::move(*state_->result);
        }

    private:
        struct SharedState {
            MessageBus& bus;
            std::string topic;
            SubscriptionId sub_id{0};
            std::optional<T> result;
            SharedState(MessageBus& b, std::string t)
                : bus(b), topic(std::move(t)) {}
        };
        std::shared_ptr<SharedState> state_;
    };

    template <typename T>
    AsyncWaitAwaitable<T> async_wait(const std::string& topic) {
        return AsyncWaitAwaitable<T>(*this, topic);
    }

private:
    template <typename T>
    TopicSlot<T>* getOrCreateSlot(const std::string& topic) {
        // Fast path: read-only check
        {
            std::shared_lock<std::shared_mutex> lock(slots_mutex_);
            auto it = slots_.find(topic);
            if (it != slots_.end()) {
                if (*topic_types_[topic] != typeid(T)) {
                    throw std::runtime_error(
                        "Type mismatch for topic: " + topic);
                }
                return static_cast<TopicSlot<T>*>(it->second.get());
            }
        }

        // Slow path: create new slot
        std::unique_lock<std::shared_mutex> lock(slots_mutex_);
        auto it = slots_.find(topic);
        if (it != slots_.end()) {
            if (*topic_types_[topic] != typeid(T)) {
                throw std::runtime_error("Type mismatch for topic: " + topic);
            }
            return static_cast<TopicSlot<T>*>(it->second.get());
        }

        auto slot = std::make_unique<TopicSlot<T>>();
        auto* ptr = slot.get();
        slots_[topic] = std::move(slot);
        topic_types_[topic] = &typeid(T);
        return ptr;
    }

    void dispatchMessage(const MessagePtr& msg) {
        ITopicSlot* slot = nullptr;
        {
            std::shared_lock<std::shared_mutex> lock(slots_mutex_);
            auto it = slots_.find(msg->topic());
            if (it != slots_.end()) {
                // Type-safety check
                auto type_it = topic_types_.find(msg->topic());
                if (type_it != topic_types_.end() &&
                    *type_it->second == msg->type()) {
                    slot = it->second.get();
                }
            }
        }
        if (slot) {
            slot->dispatch(msg);
        }
    }

    void dispatchLoop() {
        unsigned idle = 0;
        while (running_.load(std::memory_order_relaxed)) {
            MessagePtr msg;
            if (queue_.try_dequeue(msg)) {
                idle = 0;
                dispatchMessage(msg);
            } else {
                if (idle < 64) {
                    ++idle;
                } else if (idle < 256) {
                    ++idle;
                    std::this_thread::yield();
                } else {
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                }
            }
        }
        // Drain remaining messages on shutdown
        MessagePtr msg;
        while (queue_.try_dequeue(msg)) {
            dispatchMessage(msg);
        }
    }

    LockFreeQueue<MessagePtr> queue_;

    std::shared_mutex slots_mutex_;
    std::unordered_map<std::string, std::unique_ptr<ITopicSlot>> slots_;
    std::unordered_map<std::string, const std::type_info*> topic_types_;

    std::mutex sub_map_mutex_;
    std::unordered_map<SubscriptionId, std::string> sub_to_topic_;

    std::atomic<SubscriptionId> next_id_{0};
    std::atomic<bool> running_{false};
    std::thread dispatcher_;
};

} // namespace msgbus
