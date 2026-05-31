#include "test_support.h"

TEST(TopicMatcherTest, ExactMatch) {
    EXPECT_TRUE(topicMatches("a/b/c", "a/b/c"));
    EXPECT_FALSE(topicMatches("a/b/c", "a/b/d"));
    EXPECT_FALSE(topicMatches("a/b", "a/b/c"));
}

TEST(TopicMatcherTest, SingleLevelWildcard) {
    EXPECT_TRUE(topicMatches("sensor/*/temp", "sensor/1/temp"));
    EXPECT_TRUE(topicMatches("sensor/*/temp", "sensor/abc/temp"));
    EXPECT_FALSE(topicMatches("sensor/*/temp", "sensor/1/2/temp"));
    EXPECT_FALSE(topicMatches("sensor/*/temp", "sensor/1/humidity"));
}

TEST(TopicMatcherTest, MultiLevelWildcard) {
    EXPECT_TRUE(topicMatches("sensor/#", "sensor/1/temp"));
    EXPECT_TRUE(topicMatches("sensor/#", "sensor"));
    EXPECT_TRUE(topicMatches("sensor/#", "sensor/a/b/c/d"));
    EXPECT_TRUE(topicMatches("#", "anything/at/all"));
    EXPECT_FALSE(topicMatches("sensor/#", "other/1"));
}

TEST(TopicMatcherTest, MixedWildcards) {
    EXPECT_TRUE(topicMatches("a/*/c/#", "a/b/c/d/e"));
    EXPECT_TRUE(topicMatches("a/*/c/#", "a/x/c"));
    EXPECT_FALSE(topicMatches("a/*/c/#", "a/b/d"));
}

TEST(TopicMatcherTest, IsWildcard) {
    EXPECT_TRUE(isWildcard("sensor/*"));
    EXPECT_TRUE(isWildcard("sensor/#"));
    EXPECT_TRUE(isWildcard("*/temp"));
    EXPECT_FALSE(isWildcard("sensor/temp"));
    EXPECT_FALSE(isWildcard("plain"));
}