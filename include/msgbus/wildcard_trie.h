#pragma once

#include "msgbus/config.h"
#include "msgbus/subscriber.h"
#include "msgbus/topic_slot.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <typeinfo>
#include <unordered_map>
#include <vector>

namespace msgbus {

/// A trie that indexes wildcard subscription patterns for O(depth) lookup
/// instead of O(N) linear scan over all wildcard entries.
///
/// Uses RCU (Read-Copy-Update) internally:
///   - Read path (match/empty): atomic load of snapshot pointer (lock-free
///     where std::atomic<shared_ptr> is available; brief mutex otherwise),
///     then traverses immutable data with no locks held.
///   - Write path (insert/remove): serialized by mutex, deep-copies the
///     snapshot, modifies the copy, atomically publishes it.
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
        const std::type_info* type;
        std::shared_ptr<ITopicSlot> slot;
        SubscriptionId sub_id;
    };

    WildcardTrie() : snapshot_(std::make_shared<const Snapshot>()) {}

    /// Insert a wildcard pattern. Thread-safe (RCU write).
    void insert(std::string_view pattern, const Entry& entry) {
        std::lock_guard<std::mutex> lk(write_mutex_);
        auto snap = cloneSnapshot();
        auto levels = splitLevels(pattern);
        Node* cur = &snap->root;
        for (auto& level : levels) {
            auto it = cur->children.find(level);
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
        ++snap->entry_count;
        publishSnapshot(std::move(snap));
    }

    /// Remove a subscription by ID. Returns true if found. Thread-safe (RCU write).
    bool remove(SubscriptionId id) {
        std::lock_guard<std::mutex> lk(write_mutex_);
        auto snap = cloneSnapshot();
        bool found = removeFrom(&snap->root, id);
        if (found) {
            --snap->entry_count;
            publishSnapshot(std::move(snap));
        }
        return found;
    }

    /// Find all matching entries for a concrete topic. Thread-safe (RCU read).
    void match(std::string_view topic, const std::type_info& msg_type,
               std::vector<ITopicSlot*>& out) const {
        auto snap = loadSnapshot();
        if (snap->entry_count == 0) return;
        auto levels = splitLevels(topic);
        matchNode(&snap->root, levels, 0, msg_type, out);
    }

    /// Returns true if trie has no entries at all. Thread-safe (RCU read).
    bool empty() const {
        return loadSnapshot()->entry_count == 0;
    }

private:
    // Transparent hash/equal so find(string_view) avoids allocating a temp std::string.
    struct SVHash {
        using is_transparent = void;
        size_t operator()(std::string_view sv) const noexcept {
            return std::hash<std::string_view>{}(sv);
        }
    };
    struct SVEqual {
        using is_transparent = void;
        bool operator()(std::string_view a, std::string_view b) const noexcept {
            return a == b;
        }
    };

    struct Node {
        std::unordered_map<std::string, std::unique_ptr<Node>, SVHash, SVEqual> children;
        std::vector<Entry> entries; // non-empty only at terminal nodes
    };

    /// Immutable snapshot of the entire trie.
    struct Snapshot {
        Node root;
        size_t entry_count = 0;
    };

    // --- RCU machinery ---

    /// Load the current immutable snapshot.
    std::shared_ptr<const Snapshot> loadSnapshot() const {
#if MSGBUS_HAS_ATOMIC_SHARED_PTR
        return snapshot_.load(std::memory_order_acquire);
#else
        std::lock_guard<std::mutex> lk(rcu_mutex_);
        return snapshot_;
#endif
    }

    /// Publish a new snapshot (called under write_mutex_).
    void publishSnapshot(std::shared_ptr<const Snapshot> s) {
#if MSGBUS_HAS_ATOMIC_SHARED_PTR
        snapshot_.store(std::move(s), std::memory_order_release);
#else
        std::lock_guard<std::mutex> lk(rcu_mutex_);
        snapshot_ = std::move(s);
#endif
    }

    /// Deep-clone current snapshot for COW modification.
    std::shared_ptr<Snapshot> cloneSnapshot() const {
        auto old = loadSnapshot();
        auto snap = std::make_shared<Snapshot>();
        snap->entry_count = old->entry_count;
        cloneNode(old->root, snap->root);
        return snap;
    }

    static void cloneNode(const Node& src, Node& dst) {
        dst.entries = src.entries;
        for (const auto& [key, child] : src.children) {
            auto cloned = std::make_unique<Node>();
            cloneNode(*child, *cloned);
            dst.children.emplace(key, std::move(cloned));
        }
    }

    // --- Trie algorithms ---

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

    static void matchNode(const Node* node, const std::vector<std::string_view>& levels,
                   size_t depth, const std::type_info& msg_type,
                   std::vector<ITopicSlot*>& out) {
        if (!node) return;

        // '#' child matches all remaining levels (including zero)
        auto hash_it = node->children.find(std::string_view("#"));
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
        auto exact_it = node->children.find(levels[depth]);
        if (exact_it != node->children.end()) {
            matchNode(exact_it->second.get(), levels, depth + 1, msg_type, out);
        }

        // '*' matches exactly one level
        auto star_it = node->children.find(std::string_view("*"));
        if (star_it != node->children.end()) {
            matchNode(star_it->second.get(), levels, depth + 1, msg_type, out);
        }
    }

    static bool removeFrom(Node* node, SubscriptionId id) {
        // Check entries at this node
        for (auto it = node->entries.begin(); it != node->entries.end(); ++it) {
            if (it->sub_id == id) {
                it->slot->removeSubscriber(id);
                node->entries.erase(it);
                return true;
            }
        }
        // Recurse into children; prune empty nodes on the way back
        for (auto it = node->children.begin(); it != node->children.end(); ++it) {
            if (removeFrom(it->second.get(), id)) {
                if (it->second->entries.empty() && it->second->children.empty()) {
                    it = node->children.erase(it); // safe: returns next iterator
                }
                return true;
            }
        }
        return false;
    }

#if MSGBUS_HAS_ATOMIC_SHARED_PTR
    std::atomic<std::shared_ptr<const Snapshot>> snapshot_;  // lock-free read via atomic load
#else
    mutable std::mutex rcu_mutex_;                           // fallback: protects snapshot_ copy
    std::shared_ptr<const Snapshot> snapshot_;
#endif
    std::mutex write_mutex_;                                  // serializes COW writes
};

} // namespace msgbus
