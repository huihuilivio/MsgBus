#pragma once

#include "msgbus/config.h"
#include "msgbus/message.h"
#include "msgbus/subscriber.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace msgbus {

struct ITopicSlot {
    virtual ~ITopicSlot() = default;
    virtual void dispatch(const MessagePtr& msg) = 0;
    virtual bool removeSubscriber(SubscriptionId id) = 0;
    /// The type_info of the message type this slot handles.
    const std::type_info* msg_type = nullptr;
};

template <typename T>
class TopicSlot : public ITopicSlot {
public:
    using SubscriberList = std::vector<Subscriber<T>>;

    TopicSlot() : subscribers_(std::make_shared<SubscriberList>()) {
        this->msg_type = &typeid(T);
    }

    SubscriptionId addSubscriber(std::function<void(const T&)> handler,
                                 SubscriptionId id) {
        std::lock_guard<std::mutex> lock(write_mutex_);
        auto old = loadSubscribers();
        auto new_list = std::make_shared<SubscriberList>(*old);
        new_list->push_back(Subscriber<T>{id, std::move(handler)});
        publishSubscribers(std::move(new_list));
        return id;
    }

    bool removeSubscriber(SubscriptionId id) override {
        std::lock_guard<std::mutex> lock(write_mutex_);
        auto old = loadSubscribers();
        auto new_list = std::make_shared<SubscriberList>();
        new_list->reserve(old->size());
        bool found = false;
        for (const auto& sub : *old) {
            if (sub.id == id) {
                found = true;
            } else {
                new_list->push_back(sub);
            }
        }
        if (found) {
            publishSubscribers(std::move(new_list));
        }
        return found;
    }

    void dispatch(const MessagePtr& msg) override {
        // Lock-free read of COW snapshot (no mutex on hot path)
        auto subs = loadSubscribers();
        auto* typed = static_cast<TypedMessage<T>*>(msg.get());
        for (const auto& sub : *subs) {
            try {
                sub.handler(typed->data_);
            } catch (...) {
                // Handler exception isolation
            }
        }
    }

private:
    std::shared_ptr<SubscriberList> loadSubscribers() const {
#if MSGBUS_HAS_ATOMIC_SHARED_PTR
        return subscribers_.load(std::memory_order_acquire);
#else
        std::lock_guard<std::mutex> lk(read_mutex_);
        return subscribers_;
#endif
    }

    void publishSubscribers(std::shared_ptr<SubscriberList> s) {
#if MSGBUS_HAS_ATOMIC_SHARED_PTR
        subscribers_.store(std::move(s), std::memory_order_release);
#else
        std::lock_guard<std::mutex> lk(read_mutex_);
        subscribers_ = std::move(s);
#endif
    }

#if MSGBUS_HAS_ATOMIC_SHARED_PTR
    std::atomic<std::shared_ptr<SubscriberList>> subscribers_;
#else
    mutable std::mutex read_mutex_;   // fallback: protects snapshot copy
    std::shared_ptr<SubscriberList> subscribers_;
#endif
    std::mutex write_mutex_;          // serializes COW writes
};

} // namespace msgbus
