#pragma once

#include <cstdint>
#include <functional>

namespace msgbus {

using SubscriptionId = uint64_t;

template <typename T>
struct Subscriber {
    SubscriptionId id;
    std::function<void(const T&)> handler;
};

} // namespace msgbus
