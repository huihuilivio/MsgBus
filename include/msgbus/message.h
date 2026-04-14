#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>
#include <typeinfo>
#include <utility>

namespace msgbus {

/// Compact topic identifier — replaces std::string on the hot path.
using TopicId = uint32_t;
inline constexpr TopicId kInvalidTopicId = 0;

/// Base message with intrusive reference count (replaces shared_ptr overhead).
struct IMessage {
    std::atomic<int> ref_count_{0};
    void (*recycler_)(IMessage*) = nullptr;

    virtual ~IMessage() = default;
    virtual TopicId topic_id() const = 0;
    virtual const std::type_info& type() const = 0;

    /// Cached topic string (set during publish, valid for message lifetime).
    std::string_view topic_sv() const noexcept { return topic_sv_; }
    void set_topic_sv(std::string_view sv) noexcept { topic_sv_ = sv; }

    void add_ref() noexcept {
        ref_count_.fetch_add(1, std::memory_order_relaxed);
    }

    /// Returns true when ref count drops to zero.
    bool release_ref() noexcept {
        return ref_count_.fetch_sub(1, std::memory_order_acq_rel) == 1;
    }

private:
    std::string_view topic_sv_;
};

template <typename T>
struct TypedMessage : IMessage {
    TopicId topic_id_;
    T data_;

    TypedMessage(TopicId topic_id, T data)
        : topic_id_(topic_id), data_(std::move(data)) {}

    /// Reset a pooled object for reuse (integer assign instead of string copy).
    void reset(TopicId topic_id, T data) {
        ref_count_.store(0, std::memory_order_relaxed);
        recycler_ = nullptr;
        topic_id_ = topic_id;
        data_ = std::move(data);
    }

    TopicId topic_id() const override { return topic_id_; }
    const std::type_info& type() const override { return typeid(T); }
};

/// Intrusive reference-counted pointer for IMessage.
/// Eliminates shared_ptr's separate control block allocation.
class MessagePtr {
public:
    MessagePtr() noexcept = default;

    /// Adopt a raw pointer and add one reference.
    static MessagePtr adopt(IMessage* p) noexcept {
        MessagePtr mp;
        mp.ptr_ = p;
        if (p) p->add_ref();
        return mp;
    }

    ~MessagePtr() { reset(); }

    MessagePtr(const MessagePtr& o) noexcept : ptr_(o.ptr_) {
        if (ptr_) ptr_->add_ref();
    }

    MessagePtr& operator=(const MessagePtr& o) noexcept {
        if (ptr_ != o.ptr_) {
            reset();
            ptr_ = o.ptr_;
            if (ptr_) ptr_->add_ref();
        }
        return *this;
    }

    MessagePtr(MessagePtr&& o) noexcept : ptr_(o.ptr_) {
        o.ptr_ = nullptr;
    }

    MessagePtr& operator=(MessagePtr&& o) noexcept {
        if (this != &o) {
            reset();
            ptr_ = o.ptr_;
            o.ptr_ = nullptr;
        }
        return *this;
    }

    IMessage* get() const noexcept { return ptr_; }
    IMessage* operator->() const noexcept { return ptr_; }
    IMessage& operator*() const noexcept { return *ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

    void reset() noexcept {
        if (ptr_) {
            if (ptr_->release_ref()) {
                if (ptr_->recycler_) {
                    ptr_->recycler_(ptr_);
                } else {
                    delete ptr_;
                }
            }
            ptr_ = nullptr;
        }
    }

private:
    IMessage* ptr_ = nullptr;
};

} // namespace msgbus
