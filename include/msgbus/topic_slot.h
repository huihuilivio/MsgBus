#pragma once

#include "msgbus/message.h"
#include "msgbus/subscriber.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

namespace msgbus {

struct ITopicSlot {
    virtual ~ITopicSlot() = default;
    virtual void dispatch(const MessagePtr& msg) = 0;
    virtual bool removeSubscriber(SubscriptionId id) = 0;
};

template <typename T>
class TopicSlot : public ITopicSlot {
public:
    using SubscriberList = std::vector<Subscriber<T>>;

    TopicSlot() : subscribers_(std::make_shared<SubscriberList>()) {}

    SubscriptionId addSubscriber(std::function<void(const T&)> handler,
                                 SubscriptionId id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto new_list = std::make_shared<SubscriberList>(*subscribers_);
        new_list->push_back(Subscriber<T>{id, std::move(handler)});
        subscribers_ = std::move(new_list);
        return id;
    }

    bool removeSubscriber(SubscriptionId id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto new_list = std::make_shared<SubscriberList>();
        new_list->reserve(subscribers_->size());
        bool found = false;
        for (const auto& sub : *subscribers_) {
            if (sub.id == id) {
                found = true;
            } else {
                new_list->push_back(sub);
            }
        }
        if (found) {
            subscribers_ = std::move(new_list);
        }
        return found;
    }

    void dispatch(const MessagePtr& msg) override {
        // Copy shared_ptr under lock, then iterate outside lock (COW snapshot)
        std::shared_ptr<SubscriberList> subs;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            subs = subscribers_;
        }
        auto typed = std::static_pointer_cast<TypedMessage<T>>(msg);
        for (const auto& sub : *subs) {
            try {
                sub.handler(typed->data_);
            } catch (...) {
                // Handler exception isolation
            }
        }
    }

private:
    std::shared_ptr<SubscriberList> subscribers_;
    std::mutex mutex_;
};

} // namespace msgbus
