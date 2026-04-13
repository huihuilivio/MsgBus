#pragma once

#include "msgbus/message.h"

#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace msgbus {

/// Thread-safe topic string ↔ ID registry.
/// Strings are stored once in the map; IDs are used everywhere internally.
/// Read path (resolve existing) takes only a shared lock.
class TopicRegistry {
public:
    /// Register or look up a topic string. Returns a stable TopicId.
    /// Thread-safe (multiple threads may call concurrently).
    TopicId resolve(std::string_view topic) {
        // Fast read path — most topics already registered
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            auto it = map_.find(topic);
            if (it != map_.end()) return it->second;
        }
        // Slow write path — first time seeing this topic
        std::unique_lock<std::shared_mutex> lock(mutex_);
        // Double-check under exclusive lock
        auto it = map_.find(topic);
        if (it != map_.end()) return it->second;

        TopicId id = next_id_++;
        // unordered_map guarantees reference stability: pointers/references
        // to stored keys are never invalidated by insertion or rehash.
        auto [ins, _] = map_.emplace(std::string(topic), id);
        id_to_sv_.emplace(id, std::string_view(ins->first));
        return id;
    }

    /// Look up the string for a given TopicId. Returns empty view if invalid.
    std::string_view to_string(TopicId id) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = id_to_sv_.find(id);
        if (it != id_to_sv_.end()) return it->second;
        return {};
    }

private:
    mutable std::shared_mutex mutex_;
    // Transparent hash/equal so we can find(string_view) on a string-keyed map.
    // WARNING: id_to_sv_ stores string_view pointing into map_ keys.
    // This relies on std::unordered_map's pointer/reference stability guarantee
    // (keys are never relocated by insertion or rehash). Do NOT replace map_
    // with a container that invalidates references (e.g. std::vector, flat_map).
    struct SVHash {
        using is_transparent = void;
        size_t operator()(std::string_view sv) const {
            return std::hash<std::string_view>{}(sv);
        }
    };
    struct SVEqual {
        using is_transparent = void;
        bool operator()(const std::string& a, std::string_view b) const {
            return a == b;
        }
        bool operator()(std::string_view a, const std::string& b) const {
            return a == b;
        }
        bool operator()(const std::string& a, const std::string& b) const {
            return a == b;
        }
    };
    std::unordered_map<std::string, TopicId, SVHash, SVEqual> map_;
    std::unordered_map<TopicId, std::string_view> id_to_sv_;
    TopicId next_id_ = 1; // 0 is kInvalidTopicId; only mutated under unique_lock(mutex_)
};

} // namespace msgbus
