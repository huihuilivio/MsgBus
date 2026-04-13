#pragma once

#include <atomic>
#include <string>
#include <typeinfo>
#include <utility>

namespace msgbus {

/// Base message with intrusive reference count (replaces shared_ptr overhead).
struct IMessage {
    std::atomic<int> ref_count_{0};
    void (*recycler_)(IMessage*) = nullptr;

    virtual ~IMessage() = default;
    virtual const std::string& topic() const = 0;
    virtual const std::type_info& type() const = 0;

    void add_ref() noexcept {
        ref_count_.fetch_add(1, std::memory_order_relaxed);
    }

    /// Returns true when ref count drops to zero.
    bool release_ref() noexcept {
        return ref_count_.fetch_sub(1, std::memory_order_acq_rel) == 1;
    }
};

template <typename T>
struct TypedMessage : IMessage {
    std::string topic_;
    T data_;

    TypedMessage(std::string topic, T data)
        : topic_(std::move(topic)), data_(std::move(data)) {}

    /// Reset a pooled object for reuse (avoids new allocation).
    void reset(const std::string& topic, T data) {
        ref_count_.store(0, std::memory_order_relaxed);
        recycler_ = nullptr;
        topic_ = topic;
        data_ = std::move(data);
    }

    const std::string& topic() const override { return topic_; }
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
