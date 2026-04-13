#pragma once

#include <memory>
#include <string>
#include <typeinfo>
#include <utility>

namespace msgbus {

struct IMessage {
    virtual ~IMessage() = default;
    virtual const std::string& topic() const = 0;
    virtual const std::type_info& type() const = 0;
};

template <typename T>
struct TypedMessage : IMessage {
    std::string topic_;
    T data_;

    TypedMessage(std::string topic, T data)
        : topic_(std::move(topic)), data_(std::move(data)) {}

    const std::string& topic() const override { return topic_; }
    const std::type_info& type() const override { return typeid(T); }
};

using MessagePtr = std::shared_ptr<IMessage>;

} // namespace msgbus
