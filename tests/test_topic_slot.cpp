#include "test_support.h"

TEST(TopicSlotTest, RemoveNonExistentSubscriber) {
    TopicSlot<int> slot;
    slot.addSubscriber(std::function<void(const int&)>([](const int&) {}), 1);
    EXPECT_FALSE(slot.removeSubscriber(999));
    EXPECT_TRUE(slot.removeSubscriber(1));
}