#pragma once

#include "msgbus/subscriber.h"
#include "msgbus/topic_slot.h"

#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <typeinfo>
#include <unordered_map>
#include <vector>

namespace msgbus {

/// A trie that indexes wildcard subscription patterns for O(depth) lookup
/// instead of O(N) linear scan over all wildcard entries.
///
/// Structure mirrors MQTT topic levels:
///   "sensor/*/temp"  → ["sensor", "*", "temp"]
///   "sensor/#"       → ["sensor", "#"]
///
/// On dispatch, we walk the trie with the concrete topic's levels.
/// At each node we check:
///   1. The exact child matching this level
///   2. The '*' child (matches one level)
///   3. The '#' child (matches all remaining levels — terminal)
class WildcardTrie {
public:
    struct Entry {
        std::string pattern;
        const std::type_info* type;
        std::shared_ptr<ITopicSlot> slot;
        SubscriptionId sub_id;
    };

    /// Insert a wildcard pattern. Caller must hold external write lock.
    void insert(const Entry& entry) {
        auto levels = splitLevels(entry.pattern);
        Node* cur = &root_;
        for (auto& level : levels) {
            auto it = cur->children.find(std::string(level));
            if (it == cur->children.end()) {
                auto child = std::make_unique<Node>();
                auto* ptr = child.get();
                cur->children.emplace(std::string(level), std::move(child));
                cur = ptr;
            } else {
                cur = it->second.get();
            }
        }
        cur->entries.push_back(entry);
    }

    /// Remove a subscription by ID. Returns true if found.
    /// Caller must hold external write lock.
    bool remove(SubscriptionId id) {
        return removeFrom(&root_, id);
    }

    /// Find all matching entries for a concrete topic.
    /// Caller must hold external read lock.
    void match(std::string_view topic, const std::type_info& msg_type,
               std::vector<ITopicSlot*>& out) const {
        auto levels = splitLevels(topic);
        matchNode(&root_, levels, 0, msg_type, out);
    }

    /// Returns true if trie has no entries at all.
    bool empty() const {
        return isEmpty(&root_);
    }

private:
    struct Node {
        std::unordered_map<std::string, std::unique_ptr<Node>> children;
        std::vector<Entry> entries; // non-empty only at terminal nodes
    };

    Node root_;

    static std::vector<std::string_view> splitLevels(std::string_view s) {
        std::vector<std::string_view> levels;
        size_t start = 0;
        while (start < s.size()) {
            size_t pos = s.find('/', start);
            if (pos == std::string_view::npos) {
                levels.push_back(s.substr(start));
                break;
            }
            levels.push_back(s.substr(start, pos - start));
            start = pos + 1;
        }
        return levels;
    }

    void matchNode(const Node* node, const std::vector<std::string_view>& levels,
                   size_t depth, const std::type_info& msg_type,
                   std::vector<ITopicSlot*>& out) const {
        if (!node) return;

        // '#' child matches all remaining levels (including zero)
        auto hash_it = node->children.find("#");
        if (hash_it != node->children.end()) {
            for (auto& entry : hash_it->second->entries) {
                if (*entry.type == msg_type) {
                    out.push_back(entry.slot.get());
                }
            }
        }

        if (depth >= levels.size()) {
            // If pattern had trailing '/#', it was handled above.
            // Check terminal entries at this node (exact pattern end).
            for (auto& entry : node->entries) {
                if (*entry.type == msg_type) {
                    out.push_back(entry.slot.get());
                }
            }
            return;
        }

        // Exact level match
        auto exact_it = node->children.find(std::string(levels[depth]));
        if (exact_it != node->children.end()) {
            matchNode(exact_it->second.get(), levels, depth + 1, msg_type, out);
        }

        // '*' matches exactly one level
        auto star_it = node->children.find("*");
        if (star_it != node->children.end()) {
            matchNode(star_it->second.get(), levels, depth + 1, msg_type, out);
        }
    }

    bool removeFrom(Node* node, SubscriptionId id) {
        // Check entries at this node
        for (auto it = node->entries.begin(); it != node->entries.end(); ++it) {
            if (it->sub_id == id) {
                it->slot->removeSubscriber(id);
                node->entries.erase(it);
                return true;
            }
        }
        // Recurse into children
        for (auto& [key, child] : node->children) {
            if (removeFrom(child.get(), id)) return true;
        }
        return false;
    }

    bool isEmpty(const Node* node) const {
        if (!node->entries.empty()) return false;
        for (auto& [key, child] : node->children) {
            if (!isEmpty(child.get())) return false;
        }
        return true;
    }
};

} // namespace msgbus
