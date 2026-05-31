#include "test_support.h"

TEST(TopicRegistryTest, ResolveAndToString) {
    TopicRegistry reg;
    TopicId id1 = reg.resolve("sensor/temp");
    TopicId id2 = reg.resolve("sensor/humidity");
    EXPECT_NE(id1, kInvalidTopicId);
    EXPECT_NE(id2, kInvalidTopicId);
    EXPECT_NE(id1, id2);
    EXPECT_EQ(reg.to_string(id1), "sensor/temp");
    EXPECT_EQ(reg.to_string(id2), "sensor/humidity");
}

TEST(TopicRegistryTest, SameTopicSameId) {
    TopicRegistry reg;
    TopicId a = reg.resolve("x/y");
    TopicId b = reg.resolve("x/y");
    EXPECT_EQ(a, b);
}

TEST(TopicRegistryTest, InvalidIdReturnsEmpty) {
    TopicRegistry reg;
    EXPECT_TRUE(reg.to_string(999).empty());
}

TEST(TopicRegistryTest, ConcurrentResolve) {
    TopicRegistry reg;
    constexpr int THREADS = 8;
    constexpr int PER_THREAD = 100;
    std::vector<std::thread> threads;
    std::vector<TopicId> ids(THREADS * PER_THREAD);

    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < PER_THREAD; ++i) {
                std::string topic = "t/" + std::to_string(t) + "/" + std::to_string(i);
                ids[t * PER_THREAD + i] = reg.resolve(topic);
            }
        });
    }
    for (auto& th : threads)
        th.join();

    std::set<TopicId> unique_ids(ids.begin(), ids.end());
    EXPECT_EQ(unique_ids.size(), static_cast<size_t>(THREADS * PER_THREAD));
}