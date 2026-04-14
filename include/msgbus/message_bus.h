#pragma once

#include "msgbus/config.h"
#include "msgbus/lock_free_queue.h"
#include "msgbus/message.h"
#include "msgbus/object_pool.h"
#include "msgbus/subscriber.h"
#include "msgbus/topic_matcher.h"
#include "msgbus/topic_registry.h"
#include "msgbus/topic_slot.h"
#include "msgbus/wildcard_trie.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

namespace msgbus {

// ============ FullPolicy & TypedMessagePool ============

/// Queue-full policy for publish().
enum class FullPolicy {
    ReturnFalse,   ///< Return false immediately (default, zero overhead).
    DropOldest,    ///< Drop the oldest message; always succeeds.
    DropNewest,    ///< Drop the new message silently; always returns true.
    Block,         ///< Block until space is available (or bus stops).
    BlockTimeout,  ///< Block up to a timeout, then return false.
};

/// Per-type static object pool and recycler.
template <typename T>
struct TypedMessagePool {
    static ObjectPool<TypedMessage<T>>& instance() {
        static ObjectPool<TypedMessage<T>> pool(kDefaultPoolCapacity);
        return pool;
    }

    static void recycle(IMessage* msg) {
        instance().release(static_cast<TypedMessage<T>*>(msg));
    }
};

class MessageBus {
public:
    // ============ Construction & Lifecycle ============

    /// @param queue_capacity   Capacity of the internal queue.
    /// @param num_dispatchers  Number of dispatcher threads (0 = auto = hardware_concurrency).
    /// @param policy           Queue-full strategy for publish().
    /// @param publish_timeout  Timeout for FullPolicy::BlockTimeout.
    explicit MessageBus(size_t queue_capacity = kDefaultQueueCapacity,
                        unsigned num_dispatchers = 1,
                        FullPolicy policy = FullPolicy::ReturnFalse,
                        std::chrono::milliseconds publish_timeout = std::chrono::milliseconds{100})
        : policy_(policy)
        , publish_timeout_(publish_timeout)
        , queue_(queue_capacity)
        , queue_capacity_(queue_capacity)
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
            // Multi-dispatcher: router thread + N worker threads.
            // Router is started FIRST so it can begin routing immediately.
            // Workers are joined AFTER router to ensure all routed messages are consumed.
            worker_queues_.reserve(num_dispatchers_);
            worker_cvs_.reserve(num_dispatchers_);
            worker_cv_mutexes_.reserve(num_dispatchers_);
            worker_sleeping_ = std::make_unique<std::atomic<bool>[]>(num_dispatchers_);
            for (unsigned i = 0; i < num_dispatchers_; ++i) {
                worker_queues_.push_back(
                    std::make_unique<LockFreeQueue<MessagePtr>>(queue_capacity_));
                worker_cvs_.push_back(std::make_unique<std::condition_variable>());
                worker_cv_mutexes_.push_back(std::make_unique<std::mutex>());
                worker_sleeping_[i].store(false, std::memory_order_relaxed);
            }
            // Start router first (dispatchers_[0])
            dispatchers_.emplace_back(&MessageBus::routerLoop, this);
            // Then start workers
            for (unsigned i = 0; i < num_dispatchers_; ++i) {
                dispatchers_.emplace_back(&MessageBus::workerLoop, this, i);
            }
        }
    }

    void stop() {
        if (!running_.exchange(false)) return;
        // Wake all sleeping threads
        cv_.notify_all();
        cv_not_full_.notify_all(); // wake blocked publishers
        for (auto& wcv : worker_cvs_) wcv->notify_all();

        if (dispatchers_.size() > 1) {
            // Multi-dispatcher: join router first (dispatchers_[0]) so it
            // finishes draining the main queue into worker queues, THEN
            // join workers so they consume all routed messages.
            if (dispatchers_[0].joinable()) dispatchers_[0].join();
            // Signal workers that router is done draining
            router_drained_.store(true, std::memory_order_release);
            for (size_t i = 1; i < dispatchers_.size(); ++i) {
                if (dispatchers_[i].joinable()) dispatchers_[i].join();
            }
        } else {
            for (auto& t : dispatchers_) {
                if (t.joinable()) t.join();
            }
        }
        dispatchers_.clear();
        worker_queues_.clear();
        worker_cvs_.clear();
        worker_cv_mutexes_.clear();
        worker_sleeping_.reset();
        router_drained_.store(false, std::memory_order_relaxed);
        dispatcher_sleeping_.store(false, std::memory_order_relaxed);
    }

    // ============ Publish & Subscribe ============

    /// Publish a message to a topic.
    /// Behavior on queue full depends on the FullPolicy set at construction.
    /// Returns false only for ReturnFalse (queue full) or BlockTimeout (timed out).
    template <typename T>
    bool publish(std::string_view topic, T msg) {
        TopicId tid = registry_.resolve(topic);
        auto& pool = TypedMessagePool<T>::instance();
        TypedMessage<T>* raw = pool.acquire();
        if (raw) {
            raw->reset(tid, std::move(msg));
        } else {
            raw = new TypedMessage<T>(tid, std::move(msg));
        }
        raw->recycler_ = &TypedMessagePool<T>::recycle;
        // Cache topic string for wildcard dispatch (avoids registry reverse lookup)
        raw->set_topic_sv(registry_.to_string(tid));
        return enqueueMessage(MessagePtr::adopt(raw));
    }

    /// Returns the current full policy.
    FullPolicy policy() const { return policy_; }

    /// Subscribe to a topic (or wildcard pattern) with a handler.
    /// Wildcards: '*' matches one level, '#' matches zero or more trailing levels.
    /// Returns a subscription ID.
    template <typename T, typename Handler>
    SubscriptionId subscribe(std::string_view topic, Handler&& handler) {
        auto id = next_id_.fetch_add(1, std::memory_order_relaxed) + 1;

        if (isWildcard(topic)) {
            // Validate: '#' must be the last segment (MQTT rule)
            auto pos = topic.find('#');
            if (pos != std::string_view::npos &&
                pos + 1 != topic.size()) {
                throw std::runtime_error(
                    "Invalid wildcard pattern: '#' must be the last segment");
            }

            // Wildcard subscription: insert into trie index
            auto slot = std::make_shared<TopicSlot<T>>();
            slot->addSubscriber(
                std::function<void(const T&)>(std::forward<Handler>(handler)), id);

            wildcard_trie_.insert(topic, {&typeid(T), slot, id});

            std::lock_guard<std::mutex> lk(sub_map_mutex_);
            sub_to_topic_[id] = {kInvalidTopicId, true};
        } else {
            TopicId tid = registry_.resolve(topic);
            auto* slot = getOrCreateSlot<T>(tid);
            slot->addSubscriber(
                std::function<void(const T&)>(std::forward<Handler>(handler)), id);

            std::lock_guard<std::mutex> lk(sub_map_mutex_);
            sub_to_topic_[id] = {tid, false};
        }
        return id;
    }

    /// Unsubscribe by subscription ID.
    /// Thread-safe: may be called from any thread, including from within a
    /// subscription handler callback.  The handler that is currently executing
    /// will run to completion; subsequent dispatches will no longer invoke it.
    void unsubscribe(SubscriptionId id) {
        SubInfo info;
        {
            std::lock_guard<std::mutex> lock(sub_map_mutex_);
            auto it = sub_to_topic_.find(id);
            if (it == sub_to_topic_.end()) return;
            info = it->second;
            sub_to_topic_.erase(it);
        }

        if (info.is_wildcard) {
            wildcard_trie_.remove(id);
        } else {
            auto snap = loadSlots();
            auto it = snap->find(info.tid);
            if (it != snap->end()) {
                it->second->removeSubscriber(id);
            }
        }
    }

    // ============ Coroutine Support ============

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
                    // Guard: ensure handler fires at most once.
                    // In multi-dispatcher + wildcard scenarios, different topics
                    // matching the same pattern may trigger this handler from
                    // different worker threads concurrently.
                    bool expected = false;
                    if (!s->fired.compare_exchange_strong(expected, true)) {
                        return; // Already fired — skip.
                    }
                    s->result = msg;
                    // Ensure result write is visible before the coroutine
                    // reads it (may resume on a different thread).
                    std::atomic_thread_fence(std::memory_order_release);
                    handle.resume();
                });
        }

        T await_resume() {
            state_->bus.unsubscribe(state_->sub_id);
            state_->sub_id = 0;
            return std::move(*state_->result);
        }

        ~AsyncWaitAwaitable() {
            if (state_ && state_->sub_id != 0) {
                state_->bus.unsubscribe(state_->sub_id);
            }
        }

    private:
        struct SharedState {
            MessageBus& bus;
            std::string topic;
            SubscriptionId sub_id{0};
            std::optional<T> result;
            std::atomic<bool> fired{false};
            SharedState(MessageBus& b, std::string t)
                : bus(b), topic(std::move(t)) {}
        };
        std::shared_ptr<SharedState> state_;
    };

    template <typename T>
    AsyncWaitAwaitable<T> async_wait(std::string_view topic) {
        return AsyncWaitAwaitable<T>(*this, std::string(topic));
    }

    /// Returns the number of dispatcher threads.
    unsigned dispatcher_count() const { return num_dispatchers_; }

    /// Access the topic registry (e.g. to resolve TopicId ↔ string).
    TopicRegistry& registry() { return registry_; }
    const TopicRegistry& registry() const { return registry_; }

    // ============ TopicHandle (Cached Publish) ============

    template <typename T>
    class TopicHandle {
    public:
        /// Publish a message through the cached handle (skips resolve + topic hash).
        bool publish(T msg) {
            auto& pool = TypedMessagePool<T>::instance();
            TypedMessage<T>* raw = pool.acquire();
            if (raw) {
                raw->reset(tid_, std::move(msg));
            } else {
                raw = new TypedMessage<T>(tid_, std::move(msg));
            }
            raw->recycler_ = &TypedMessagePool<T>::recycle;
            raw->set_topic_sv(topic_sv_);
            return bus_->enqueueMessage(MessagePtr::adopt(raw));
        }

    private:
        friend class MessageBus;
        TopicHandle(MessageBus* bus, TopicId tid, std::string_view sv)
            : bus_(bus), tid_(tid), topic_sv_(sv) {}
        MessageBus* bus_;
        TopicId tid_;
        std::string_view topic_sv_;
    };

    /// Create a cached handle for high-frequency publishing to the same topic.
    /// The returned handle skips topic resolve on each publish.
    template <typename T>
    TopicHandle<T> topic(std::string_view topic) {
        TopicId tid = registry_.resolve(topic);
        return TopicHandle<T>(this, tid, registry_.to_string(tid));
    }

private:
    // ============ RCU Slot Map ============

    using SlotMap = std::unordered_map<TopicId, std::shared_ptr<ITopicSlot>>;

    std::shared_ptr<const SlotMap> loadSlots() const {
#if MSGBUS_HAS_ATOMIC_SHARED_PTR
        return slots_.load(std::memory_order_acquire);
#else
        std::lock_guard<std::mutex> lk(slots_mutex_);
        return slots_;
#endif
    }

    void publishSlots(std::shared_ptr<const SlotMap> s) {
#if MSGBUS_HAS_ATOMIC_SHARED_PTR
        slots_.store(std::move(s), std::memory_order_release);
#else
        std::lock_guard<std::mutex> lk(slots_mutex_);
        slots_ = std::move(s);
#endif
    }

    /// Verify that the slot registered for @p tid matches type T.
    template <typename T>
    TopicSlot<T>* checkedSlot(TopicId tid, const std::shared_ptr<ITopicSlot>& sp) {
        if (sp->msg_type && *sp->msg_type != typeid(T)) {
            throw std::runtime_error(
                "Type mismatch for topic: " + std::string(registry_.to_string(tid)));
        }
        return static_cast<TopicSlot<T>*>(sp.get());
    }

    template <typename T>
    TopicSlot<T>* getOrCreateSlot(TopicId tid) {
        // Fast path: read snapshot
        {
            auto snap = loadSlots();
            auto it = snap->find(tid);
            if (it != snap->end()) {
                return checkedSlot<T>(tid, it->second);
            }
        }
        // Slow path: COW write
        std::lock_guard<std::mutex> lk(slots_write_mutex_);
        auto old = loadSlots();
        auto it = old->find(tid);
        if (it != old->end()) {
            return checkedSlot<T>(tid, it->second);
        }
        auto new_map = std::make_shared<SlotMap>(*old);
        auto slot = std::make_shared<TopicSlot<T>>();
        auto* ptr = slot.get();
        (*new_map)[tid] = std::move(slot);
        publishSlots(std::move(new_map));
        return ptr;
    }

    void dispatchMessage(const MessagePtr& msg) {
        TopicId tid = msg->topic_id();
        // 1. Exact-match dispatch (RCU snapshot, zero-lock read)
        {
            auto snap = loadSlots();
            auto it = snap->find(tid);
            if (it != snap->end() &&
                it->second->msg_type &&
                *it->second->msg_type == msg->type()) {
                it->second->dispatch(msg);
            }
        }

        // 2. Wildcard-match dispatch via trie (RCU: no external lock needed)
        //    Uses cached topic_sv from publish() — no registry reverse lookup.
        //    The guard keeps the trie snapshot alive so the raw ITopicSlot*
        //    pointers remain valid throughout the dispatch loop.
        {
            std::string_view topic_sv = msg->topic_sv();
            thread_local std::vector<ITopicSlot*> matched;
            matched.clear();
            auto wc_guard = wildcard_trie_.match(topic_sv, msg->type(), matched);
            for (auto* slot : matched) {
                slot->dispatch(msg);
            }
        }
    }

    // ============ Enqueue & Backpressure ============

    bool enqueueMessage(MessagePtr mptr) {
        bool enqueued = false;
        switch (policy_) {
        case FullPolicy::ReturnFalse:
            enqueued = queue_.try_enqueue(std::move(mptr));
            if (enqueued) wakeDispatcher();
            return enqueued;

        case FullPolicy::DropNewest:
            enqueued = queue_.try_enqueue(std::move(mptr));
            if (enqueued) wakeDispatcher();
            return true; // always report success

        case FullPolicy::DropOldest: {
            std::lock_guard<std::mutex> lk(publish_mutex_);
            if (!queue_.try_enqueue(mptr)) { // copy (keep mptr valid for retry)
                MessagePtr oldest;
                queue_.try_dequeue(oldest); // discard oldest
                // Retry — under publish_mutex_ only this producer enqueues,
                // so after dequeue the slot is available.  Yield between
                // retries to avoid busy-spinning while holding the lock.
                while (!queue_.try_enqueue(mptr)) {
                    if (!queue_.try_dequeue(oldest)) {
                        std::this_thread::yield();
                    }
                }
            }
            wakeDispatcher();
            return true;
        }

        case FullPolicy::Block:
            enqueued = queue_.try_enqueue(mptr); // copy (keeps mptr valid for retry)
            if (!enqueued) {
                std::unique_lock<std::mutex> lk(cv_not_full_mutex_);
                cv_not_full_.wait(lk, [&] {
                    if (!running_.load(std::memory_order_acquire)) return true;
                    enqueued = queue_.try_enqueue(mptr);
                    return enqueued;
                });
            }
            if (enqueued) wakeDispatcher();
            return enqueued;

        case FullPolicy::BlockTimeout: {
            enqueued = queue_.try_enqueue(mptr);
            if (!enqueued) {
                auto deadline = std::chrono::steady_clock::now() + publish_timeout_;
                std::unique_lock<std::mutex> lk(cv_not_full_mutex_);
                cv_not_full_.wait_until(lk, deadline, [&] {
                    if (!running_.load(std::memory_order_acquire)) return true;
                    enqueued = queue_.try_enqueue(mptr);
                    return enqueued;
                });
            }
            if (enqueued) wakeDispatcher();
            return enqueued;
        }
        }
        return false; // unreachable
    }

    bool mainDequeue(MessagePtr& msg) {
        return queue_.try_dequeue(msg);
    }

    void notifyNotFull() {
        if (policy_ == FullPolicy::Block || policy_ == FullPolicy::BlockTimeout) {
            cv_not_full_.notify_one();
        }
    }

    /// Wake the main dispatch/router thread only if it is actually sleeping.
    void wakeDispatcher() {
        if (dispatcher_sleeping_.load(std::memory_order_acquire)) {
            cv_.notify_one();
        }
    }

    /// Wake a specific worker thread only if it is actually sleeping.
    void wakeWorker(size_t idx) {
        if (worker_sleeping_[idx].load(std::memory_order_acquire)) {
            worker_cvs_[idx]->notify_one();
        }
    }

    // ============ Single-Dispatcher Mode ============

    void dispatchLoop() {
        unsigned idle = 0;
        while (running_.load(std::memory_order_acquire)) {
            MessagePtr msg;
            if (mainDequeue(msg)) {
                idle = 0;
                notifyNotFull();
                dispatchMessage(msg);
            } else {
                if (idle < kSpinThreshold) {
                    ++idle;
                } else if (idle < kYieldThreshold) {
                    ++idle;
                    std::this_thread::yield();
                } else {
                    std::unique_lock<std::mutex> lk(cv_mutex_);
                    dispatcher_sleeping_.store(true, std::memory_order_release);
                    cv_.wait_for(lk, std::chrono::milliseconds(1),
                        [this] { return !running_.load(std::memory_order_acquire); });
                    dispatcher_sleeping_.store(false, std::memory_order_release);
                }
            }
        }
        // Drain remaining messages
        MessagePtr msg;
        while (mainDequeue(msg)) {
            notifyNotFull();
            dispatchMessage(msg);
        }
    }

    // ============ Multi-Dispatcher Mode ============

    /// Router thread: dequeue from main queue, hash-route to worker queues.
    /// Messages with the same topic always go to the same worker (ordering guarantee).
    void routerLoop() {
        unsigned idle = 0;
        while (running_.load(std::memory_order_acquire)) {
            MessagePtr msg;
            if (mainDequeue(msg)) {
                idle = 0;
                notifyNotFull();
                routeToWorker(msg);
            } else {
                if (idle < kSpinThreshold) {
                    ++idle;
                } else if (idle < kYieldThreshold) {
                    ++idle;
                    std::this_thread::yield();
                } else {
                    std::unique_lock<std::mutex> lk(cv_mutex_);
                    dispatcher_sleeping_.store(true, std::memory_order_release);
                    cv_.wait_for(lk, std::chrono::milliseconds(1),
                        [this] { return !running_.load(std::memory_order_acquire); });
                    dispatcher_sleeping_.store(false, std::memory_order_release);
                }
            }
        }
        // Drain main queue into worker queues.
        // Workers are still alive (stop() joins router before workers).
        MessagePtr msg;
        while (mainDequeue(msg)) {
            notifyNotFull();
            routeToWorker(msg);
        }
    }

    void routeToWorker(const MessagePtr& msg) {
        size_t idx = std::hash<TopicId>{}(msg->topic_id()) % num_dispatchers_;
        while (!worker_queues_[idx]->try_enqueue(msg)) {
            std::this_thread::yield();
        }
        wakeWorker(idx);
    }

    /// Worker thread: dequeue from its own queue and dispatch.
    void workerLoop(unsigned worker_id) {
        auto& wq = *worker_queues_[worker_id];
        auto& wcv = *worker_cvs_[worker_id];
        auto& wcv_mu = *worker_cv_mutexes_[worker_id];
        unsigned idle = 0;
        while (running_.load(std::memory_order_acquire)) {
            MessagePtr msg;
            if (wq.try_dequeue(msg)) {
                idle = 0;
                dispatchMessage(msg);
            } else {
                if (idle < kSpinThreshold) {
                    ++idle;
                } else if (idle < kYieldThreshold) {
                    ++idle;
                    std::this_thread::yield();
                } else {
                    std::unique_lock<std::mutex> lk(wcv_mu);
                    worker_sleeping_[worker_id].store(true, std::memory_order_release);
                    wcv.wait_for(lk, std::chrono::milliseconds(1),
                        [this] { return !running_.load(std::memory_order_acquire); });
                    worker_sleeping_[worker_id].store(false, std::memory_order_release);
                }
            }
        }
        // Keep draining until router has finished AND queue is empty.
        // router_drained_ ensures we don't exit before router pushes last messages.
        unsigned drain_idle = 0;
        while (!router_drained_.load(std::memory_order_acquire)) {
            MessagePtr msg;
            if (wq.try_dequeue(msg)) {
                drain_idle = 0;
                dispatchMessage(msg);
            } else if (drain_idle < kSpinThreshold) {
                ++drain_idle;
            } else {
                std::this_thread::yield();
            }
        }
        // Final drain after router is done
        MessagePtr msg;
        while (wq.try_dequeue(msg)) {
            dispatchMessage(msg);
        }
    }

    // ============ Member Variables ============

    FullPolicy policy_;
    std::chrono::milliseconds publish_timeout_;

    // Main queue (MPMC, used by all policies)
    LockFreeQueue<MessagePtr> queue_;
    std::mutex publish_mutex_;              // serializes DropOldest dequeue+enqueue

    // Backpressure signaling for Block / BlockTimeout
    std::mutex cv_not_full_mutex_;
    std::condition_variable cv_not_full_;

    size_t queue_capacity_;
    unsigned num_dispatchers_;
    std::atomic<bool> router_drained_{false};
    std::atomic<bool> dispatcher_sleeping_{false};

    // Multi-dispatcher worker queues (one per worker)
    std::vector<std::unique_ptr<LockFreeQueue<MessagePtr>>> worker_queues_;
    std::vector<std::unique_ptr<std::condition_variable>> worker_cvs_;
    std::vector<std::unique_ptr<std::mutex>> worker_cv_mutexes_;
    std::unique_ptr<std::atomic<bool>[]> worker_sleeping_;
    std::vector<std::thread> dispatchers_;

    // Exact-match slots (RCU: immutable snapshot, lock-free read path)
#if MSGBUS_HAS_ATOMIC_SHARED_PTR
    std::atomic<std::shared_ptr<const SlotMap>> slots_{
        std::make_shared<const SlotMap>()};
#else
    mutable std::mutex slots_mutex_;           // fallback: protects snapshot copy
    std::shared_ptr<const SlotMap> slots_{
        std::make_shared<const SlotMap>()};
#endif
    std::mutex slots_write_mutex_;             // serializes COW writes

    // Wildcard subscriptions (trie-indexed, internally RCU-synchronized)
    WildcardTrie wildcard_trie_;

    struct SubInfo {
        TopicId tid;
        bool is_wildcard;
    };
    std::mutex sub_map_mutex_;
    std::unordered_map<SubscriptionId, SubInfo> sub_to_topic_;

    std::atomic<SubscriptionId> next_id_{0};
    std::atomic<bool> running_{false};
    TopicRegistry registry_;

    // Condition variable for dispatch/router threads (replaces sleep backoff)
    std::mutex cv_mutex_;
    std::condition_variable cv_;
};

} // namespace msgbus
