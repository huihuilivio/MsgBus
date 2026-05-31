#include "test_support.h"

TEST(WildcardTrieTest, SingleLevelMatch) {
    WildcardTrie trie;
    auto slot = std::make_shared<TopicSlot<int>>();
    slot->addSubscriber(std::function<void(const int&)>([](const int&) {}), 1);
    trie.insert("sensor/*/temp", {&typeid(int), slot, 1});

    std::vector<ITopicSlot*> matched;
    (void)trie.match("sensor/1/temp", typeid(int), matched);
    EXPECT_EQ(matched.size(), 1u);

    matched.clear();
    (void)trie.match("sensor/1/humidity", typeid(int), matched);
    EXPECT_EQ(matched.size(), 0u);

    matched.clear();
    (void)trie.match("sensor/1/2/temp", typeid(int), matched);
    EXPECT_EQ(matched.size(), 0u);
}

TEST(WildcardTrieTest, MultiLevelMatch) {
    WildcardTrie trie;
    auto slot = std::make_shared<TopicSlot<int>>();
    slot->addSubscriber(std::function<void(const int&)>([](const int&) {}), 1);
    trie.insert("sensor/#", {&typeid(int), slot, 1});

    std::vector<ITopicSlot*> matched;
    (void)trie.match("sensor/temp", typeid(int), matched);
    EXPECT_EQ(matched.size(), 1u);

    matched.clear();
    (void)trie.match("sensor/a/b/c", typeid(int), matched);
    EXPECT_EQ(matched.size(), 1u);

    matched.clear();
    (void)trie.match("sensor", typeid(int), matched);
    EXPECT_EQ(matched.size(), 1u);

    matched.clear();
    (void)trie.match("other/thing", typeid(int), matched);
    EXPECT_EQ(matched.size(), 0u);
}

TEST(WildcardTrieTest, MixedWildcards) {
    WildcardTrie trie;
    auto slot = std::make_shared<TopicSlot<int>>();
    slot->addSubscriber(std::function<void(const int&)>([](const int&) {}), 1);
    trie.insert("a/*/c/#", {&typeid(int), slot, 1});

    std::vector<ITopicSlot*> matched;
    (void)trie.match("a/b/c/d/e", typeid(int), matched);
    EXPECT_EQ(matched.size(), 1u);

    matched.clear();
    (void)trie.match("a/x/c", typeid(int), matched);
    EXPECT_EQ(matched.size(), 1u);

    matched.clear();
    (void)trie.match("a/b/d", typeid(int), matched);
    EXPECT_EQ(matched.size(), 0u);
}

TEST(WildcardTrieTest, MultiplePatterns) {
    WildcardTrie trie;
    auto slot1 = std::make_shared<TopicSlot<int>>();
    slot1->addSubscriber(std::function<void(const int&)>([](const int&) {}), 1);
    trie.insert("sensor/#", {&typeid(int), slot1, 1});

    auto slot2 = std::make_shared<TopicSlot<int>>();
    slot2->addSubscriber(std::function<void(const int&)>([](const int&) {}), 2);
    trie.insert("sensor/*/temp", {&typeid(int), slot2, 2});

    std::vector<ITopicSlot*> matched;
    (void)trie.match("sensor/1/temp", typeid(int), matched);
    EXPECT_EQ(matched.size(), 2u);
}

TEST(WildcardTrieTest, RemoveEntry) {
    WildcardTrie trie;
    auto slot = std::make_shared<TopicSlot<int>>();
    slot->addSubscriber(std::function<void(const int&)>([](const int&) {}), 1);
    trie.insert("sensor/#", {&typeid(int), slot, 1});

    EXPECT_FALSE(trie.empty());
    EXPECT_TRUE(trie.remove(1));
    EXPECT_TRUE(trie.empty());

    std::vector<ITopicSlot*> matched;
    (void)trie.match("sensor/temp", typeid(int), matched);
    EXPECT_EQ(matched.size(), 0u);
}

TEST(WildcardTrieTest, TypeFiltering) {
    WildcardTrie trie;
    auto slot = std::make_shared<TopicSlot<int>>();
    slot->addSubscriber(std::function<void(const int&)>([](const int&) {}), 1);
    trie.insert("data/#", {&typeid(int), slot, 1});

    std::vector<ITopicSlot*> matched;
    (void)trie.match("data/x", typeid(int), matched);
    EXPECT_EQ(matched.size(), 1u);

    matched.clear();
    (void)trie.match("data/x", typeid(std::string), matched);
    EXPECT_EQ(matched.size(), 0u);
}

TEST(WildcardTrieTest, HashMatchesRoot) {
    WildcardTrie trie;
    auto slot = std::make_shared<TopicSlot<int>>();
    slot->addSubscriber(std::function<void(const int&)>([](const int&) {}), 1);
    trie.insert("#", {&typeid(int), slot, 1});

    std::vector<ITopicSlot*> matched;
    (void)trie.match("anything/at/all", typeid(int), matched);
    EXPECT_EQ(matched.size(), 1u);

    matched.clear();
    (void)trie.match("x", typeid(int), matched);
    EXPECT_EQ(matched.size(), 1u);
}

TEST(WildcardTrieTest, EmptyNodePruning) {
    WildcardTrie trie;
    auto slot = std::make_shared<TopicSlot<int>>();
    slot->addSubscriber(std::function<void(const int&)>([](const int&) {}), 1);
    trie.insert("a/b/c/d", {&typeid(int), slot, 1});

    EXPECT_FALSE(trie.empty());
    EXPECT_TRUE(trie.remove(1));
    EXPECT_TRUE(trie.empty());

    auto slot2 = std::make_shared<TopicSlot<int>>();
    slot2->addSubscriber(std::function<void(const int&)>([](const int&) {}), 2);
    trie.insert("a/b/c/d", {&typeid(int), slot2, 2});

    std::vector<ITopicSlot*> matched;
    (void)trie.match("a/b/c/d", typeid(int), matched);
    EXPECT_EQ(matched.size(), 1u);
}

TEST(WildcardTrieTest, PartialPruning) {
    WildcardTrie trie;
    auto slot1 = std::make_shared<TopicSlot<int>>();
    slot1->addSubscriber(std::function<void(const int&)>([](const int&) {}), 1);
    trie.insert("a/b/c", {&typeid(int), slot1, 1});

    auto slot2 = std::make_shared<TopicSlot<int>>();
    slot2->addSubscriber(std::function<void(const int&)>([](const int&) {}), 2);
    trie.insert("a/b/d", {&typeid(int), slot2, 2});

    EXPECT_TRUE(trie.remove(1));

    std::vector<ITopicSlot*> matched;
    (void)trie.match("a/b/c", typeid(int), matched);
    EXPECT_EQ(matched.size(), 0u);

    matched.clear();
    (void)trie.match("a/b/d", typeid(int), matched);
    EXPECT_EQ(matched.size(), 1u);
}

TEST(WildcardTrieTest, EntryCountAccuracy) {
    WildcardTrie trie;
    EXPECT_TRUE(trie.empty());

    auto make_slot = [](SubscriptionId id) {
        auto slot = std::make_shared<TopicSlot<int>>();
        slot->addSubscriber(std::function<void(const int&)>([](const int&) {}), id);
        return slot;
    };

    trie.insert("a/#", {&typeid(int), make_slot(1), 1});
    trie.insert("b/#", {&typeid(int), make_slot(2), 2});
    trie.insert("c/#", {&typeid(int), make_slot(3), 3});
    EXPECT_FALSE(trie.empty());

    EXPECT_TRUE(trie.remove(1));
    EXPECT_FALSE(trie.empty());
    EXPECT_TRUE(trie.remove(2));
    EXPECT_FALSE(trie.empty());
    EXPECT_TRUE(trie.remove(3));
    EXPECT_TRUE(trie.empty());

    EXPECT_FALSE(trie.remove(999));
    EXPECT_TRUE(trie.empty());
}

TEST(WildcardTrieTest, ConcurrentReadWrite) {
    WildcardTrie trie;
    constexpr int NUM_PATTERNS = 100;
    constexpr int NUM_READERS = 4;
    constexpr int READ_ITERS = 2000;

    auto make_slot = [](SubscriptionId id) {
        auto slot = std::make_shared<TopicSlot<int>>();
        slot->addSubscriber(std::function<void(const int&)>([](const int&) {}), id);
        return slot;
    };
    for (int i = 0; i < 10; ++i) {
        trie.insert("pre/" + std::to_string(i) + "/#",
                    {&typeid(int), make_slot(static_cast<SubscriptionId>(i + 1)),
                     static_cast<SubscriptionId>(i + 1)});
    }

    std::atomic<bool> stop{false};

    std::thread writer([&] {
        for (int i = 10; i < NUM_PATTERNS && !stop.load(); ++i) {
            auto id = static_cast<SubscriptionId>(i + 1);
            trie.insert("rcu/" + std::to_string(i) + "/#", {&typeid(int), make_slot(id), id});
        }
        for (int i = 10; i < NUM_PATTERNS && !stop.load(); ++i) {
            trie.remove(static_cast<SubscriptionId>(i + 1));
        }
    });

    std::vector<std::thread> readers;
    std::atomic<int> total_matches{0};
    for (int r = 0; r < NUM_READERS; ++r) {
        readers.emplace_back([&] {
            for (int i = 0; i < READ_ITERS; ++i) {
                std::vector<ITopicSlot*> matched;
                (void)trie.match("pre/5/sensor/temp", typeid(int), matched);
                total_matches.fetch_add(static_cast<int>(matched.size()),
                                        std::memory_order_relaxed);
            }
        });
    }

    for (auto& t : readers)
        t.join();
    writer.join();

    EXPECT_GE(total_matches.load(), NUM_READERS * READ_ITERS);
}

TEST(WildcardTrieTest, SnapshotIsolation) {
    WildcardTrie trie;
    auto make_slot = [](SubscriptionId id) {
        auto slot = std::make_shared<TopicSlot<int>>();
        slot->addSubscriber(std::function<void(const int&)>([](const int&) {}), id);
        return slot;
    };

    trie.insert("snap/#", {&typeid(int), make_slot(1), 1});

    std::vector<ITopicSlot*> before;
    (void)trie.match("snap/a/b", typeid(int), before);
    EXPECT_EQ(before.size(), 1u);

    trie.insert("snap/a/#", {&typeid(int), make_slot(2), 2});
    std::vector<ITopicSlot*> after;
    (void)trie.match("snap/a/b", typeid(int), after);
    EXPECT_EQ(after.size(), 2u);

    trie.remove(1);
    std::vector<ITopicSlot*> final_match;
    (void)trie.match("snap/a/b", typeid(int), final_match);
    EXPECT_EQ(final_match.size(), 1u);
}

TEST(WildcardTrieTest, InsertAfterFullRemoval) {
    WildcardTrie trie;
    auto make_slot = [](SubscriptionId id) {
        auto slot = std::make_shared<TopicSlot<int>>();
        slot->addSubscriber(std::function<void(const int&)>([](const int&) {}), id);
        return slot;
    };

    trie.insert("cycle/#", {&typeid(int), make_slot(1), 1});
    EXPECT_FALSE(trie.empty());

    trie.remove(1);
    EXPECT_TRUE(trie.empty());

    trie.insert("cycle/#", {&typeid(int), make_slot(2), 2});
    EXPECT_FALSE(trie.empty());

    std::vector<ITopicSlot*> matched;
    (void)trie.match("cycle/x/y", typeid(int), matched);
    EXPECT_EQ(matched.size(), 1u);
}

TEST(WildcardTrieTest, RemoveDeepNestedChild) {
    WildcardTrie trie;
    auto slot1 = std::make_shared<TopicSlot<int>>();
    slot1->addSubscriber(std::function<void(const int&)>([](const int&) {}), 1);
    trie.insert("a/b/c/*/e", {&typeid(int), slot1, 1});

    auto slot2 = std::make_shared<TopicSlot<int>>();
    slot2->addSubscriber(std::function<void(const int&)>([](const int&) {}), 2);
    trie.insert("a/b/c/d/f", {&typeid(int), slot2, 2});

    EXPECT_TRUE(trie.remove(1));

    std::vector<ITopicSlot*> matched;
    (void)trie.match("a/b/c/x/e", typeid(int), matched);
    EXPECT_EQ(matched.size(), 0u);

    matched.clear();
    (void)trie.match("a/b/c/d/f", typeid(int), matched);
    EXPECT_EQ(matched.size(), 1u);
}

TEST(WildcardTrieTest, RemoveFromChildPrunesCorrectly) {
    WildcardTrie trie;
    auto slot = std::make_shared<TopicSlot<int>>();
    slot->addSubscriber(std::function<void(const int&)>([](const int&) {}), 1);
    trie.insert("x/y/z", {&typeid(int), slot, 1});

    EXPECT_TRUE(trie.remove(1));
    EXPECT_TRUE(trie.empty());
    EXPECT_FALSE(trie.remove(42));
}