#pragma once

#include "msgbus/lock_free_queue.h"
#include "msgbus/message.h"
#include "msgbus/object_pool.h"
#include "msgbus/subscriber.h"
#include "msgbus/topic_matcher.h"
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
#include <vector>

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
    /// @param queue_capacity  Capacity of the internal lock-free queue.
    /// @param num_dispatchers Number of dispatcher threads (0 = auto = hardware_concurrency).
    explicit MessageBus(size_t queue_capacity = 65536, unsigned num_dispatchers = 1)
        : queue_(queue_capacity)
        , num_dispatchers_(num_dispatchers == 0
              ? std::max(1u, std::thread::hardware_concurrency())
              : num_dispatchers) {}

    ~MessageBus() { stop(); }

    MessageBus(const MessageBus&) = delete;
    MessageBus& operator=(const MessageBus&) = delete;

    void start() {
        if (running_.exchange(true)) return;
        if (num_dispatchers_ == 1) {
            dispatchers_.emplace_back(&MessageBus::dispatchLoop, this);
        } else {
            // Multi-dispatcher: one global dequeue thread fans out to per-worker queues
            worker_queues_.reserve(num_dispatchers_);
            for (unsigned i = 0; i < num_dispatchers_; ++i) {
                worker_queues_.push_back(
                    std::make_unique<LockFreeQueue<MessagePtr>>(queue_capacity()));
            }
            for (unsigned i = 0; i < num_dispatchers_; ++i) {
                dispatchers_.emplace_back(&MessageBus::workerLoop, this, i);
            }
            dispatchers_.emplace_back(&MessageBus::routerLoop, this);
        }
    }

    void stop() {
        if (!running_.exchange(false)) return;
        for (auto& t : dispatchers_) {
            if (t.joinable()) t.join();
        }
        dispatchers_.clear();
        worker_queues_.clear();
    }

    /// Publish a message to a topic. Returns false if queue is full.
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

    /// Subscribe to a topic (or wildcard pattern) with a handler.
    /// Wildcards: '*' matches one level, '#' matches zero or more trailing levels.
    /// Returns a subscription ID.
    template <typename T, typename Handler>
    SubscriptionId subscribe(const std::string& topic, Handler&& handler) {
        auto id = next_id_.fetch_add(1, std::memory_order_relaxed) + 1;

        if (isWildcard(topic)) {
            // Wildcard subscription: store pattern + slot
            auto slot = std::make_shared<TopicSlot<T>>();
            slot->addSubscriber(
                std::function<void(const T&)>(std::forward<Handler>(handler)), id);

            std::unique_lock<std::shared_mutex> lock(wildcards_mutex_);
            wildcard_entries_.push_back({topic, &typeid(T), slot, id});
        } else {
            auto* slot = getOrCreateSlot<T>(topic);
            slot->addSubscriber(
                std::function<void(const T&)>(std::forward<Handler>(handler)), id);
        }

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

        if (isWildcard(topic)) {
            std::unique_lock<std::shared_mutex> lock(wildcards_mutex_);
            for (auto it = wildcard_entries_.begin(); it != wildcard_entries_.end(); ++it) {
                if (it->sub_id == id) {
                    it->slot->removeSubscriber(id);
                    wildcard_entries_.erase(it);
                    break;
                }
            }
        } else {
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

    /// Returns the number of dispatcher threads.
    unsigned dispatcher_count() const { return num_dispatchers_; }

private:
    size_t queue_capacity() const {
        // Estimate: use same capacity as main queue for worker queues
        return 65536;
    }

    template <typename T>
    TopicSlot<T>* getOrCreateSlot(const std::string& topic) {
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
        // 1. Exact-match dispatch
        {
            std::shared_lock<std::shared_mutex> lock(slots_mutex_);
            auto it = slots_.find(msg->topic());
            if (it != slots_.end()) {
                auto type_it = topic_types_.find(msg->topic());
                if (type_it != topic_types_.end() &&
                    *type_it->second == msg->type()) {
                    it->second->dispatch(msg);
                }
            }
        }

        // 2. Wildcard-match dispatch
        {
            std::shared_lock<std::shared_mutex> lock(wildcards_mutex_);
            for (auto& entry : wildcard_entries_) {
                if (*entry.type == msg->type() &&
                    topicMatches(entry.pattern, msg->topic())) {
                    entry.slot->dispatch(msg);
                }
            }
        }
    }

    // --- Single-dispatcher mode ---
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
        MessagePtr msg;
        while (queue_.try_dequeue(msg)) {
            dispatchMessage(msg);
        }
    }

    // --- Multi-dispatcher mode ---

    /// Router thread: dequeue from main queue, hash-route to worker queues.
    /// Messages with the same topic always go to the same worker (ordering guarantee).
    void routerLoop() {
        std::hash<std::string> hasher;
        unsigned idle = 0;
        while (running_.load(std::memory_order_relaxed)) {
            MessagePtr msg;
            if (queue_.try_dequeue(msg)) {
                idle = 0;
                size_t idx = hasher(msg->topic()) % num_dispatchers_;
                while (!worker_queues_[idx]->try_enqueue(msg)) {
                    std::this_thread::yield();
                }
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
        // Drain main queue
        MessagePtr msg;
        while (queue_.try_dequeue(msg)) {
            size_t idx = hasher(msg->topic()) % num_dispatchers_;
            while (!worker_queues_[idx]->try_enqueue(msg)) {
                std::this_thread::yield();
            }
        }
        // Signal workers to drain by setting a flag they can check
        // Workers will exit when running_ is false and their queue is empty
    }

    /// Worker thread: dequeue from its own queue and dispatch.
    void workerLoop(unsigned worker_id) {
        auto& wq = *worker_queues_[worker_id];
        unsigned idle = 0;
        while (running_.load(std::memory_order_relaxed)) {
            MessagePtr msg;
            if (wq.try_dequeue(msg)) {
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
        // Drain worker queue
        MessagePtr msg;
        while (wq.try_dequeue(msg)) {
            dispatchMessage(msg);
        }
    }

    LockFreeQueue<MessagePtr> queue_;
    unsigned num_dispatchers_;

    // Multi-dispatcher worker queues (one per worker)
    std::vector<std::unique_ptr<LockFreeQueue<MessagePtr>>> worker_queues_;
    std::vector<std::thread> dispatchers_;

    // Exact-match slots
    std::shared_mutex slots_mutex_;
    std::unordered_map<std::string, std::unique_ptr<ITopicSlot>> slots_;
    std::unordered_map<std::string, const std::type_info*> topic_types_;

    // Wildcard subscriptions
    struct WildcardEntry {
        std::string pattern;
        const std::type_info* type;
        std::shared_ptr<ITopicSlot> slot;
        SubscriptionId sub_id;
    };
    std::shared_mutex wildcards_mutex_;
    std::vector<WildcardEntry> wildcard_entries_;

    std::mutex sub_map_mutex_;
    std::unordered_map<SubscriptionId, std::string> sub_to_topic_;

    std::atomic<SubscriptionId> next_id_{0};
    std::atomic<bool> running_{false};
};

} // namespace msgbus
