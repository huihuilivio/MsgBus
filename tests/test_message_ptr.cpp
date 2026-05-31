#include "test_support.h"

TEST(MessagePtrTest, DefaultNull) {
    MessagePtr ptr;
    EXPECT_FALSE(ptr);
    EXPECT_EQ(ptr.get(), nullptr);
}

TEST(MessagePtrTest, AdoptAndAccess) {
    auto* raw = new TypedMessage<int>(1, 42);
    MessagePtr ptr = MessagePtr::adopt(raw);
    EXPECT_TRUE(ptr);
    EXPECT_EQ(ptr->topic_id(), 1u);
    EXPECT_EQ(ptr->type(), typeid(int));
    EXPECT_EQ(static_cast<TypedMessage<int>*>(ptr.get())->data_, 42);
}

TEST(MessagePtrTest, CopyIncrementsRefCount) {
    auto* raw = new TypedMessage<int>(1, 1);
    MessagePtr p1 = MessagePtr::adopt(raw);
    {
        MessagePtr p2 = p1;
        EXPECT_EQ(p2.get(), p1.get());
        EXPECT_EQ(raw->ref_count_.load(), 2);
    }
    EXPECT_EQ(raw->ref_count_.load(), 1);
}

TEST(MessagePtrTest, MoveTransfersOwnership) {
    auto* raw = new TypedMessage<int>(1, 1);
    MessagePtr p1 = MessagePtr::adopt(raw);
    MessagePtr p2 = std::move(p1);
    EXPECT_FALSE(p1);
    EXPECT_TRUE(p2);
    EXPECT_EQ(p2.get(), raw);
    EXPECT_EQ(raw->ref_count_.load(), 1);
}

TEST(MessagePtrTest, CopyAssignment) {
    auto* r1 = new TypedMessage<int>(1, 1);
    auto* r2 = new TypedMessage<int>(2, 2);
    MessagePtr p1 = MessagePtr::adopt(r1);
    MessagePtr p2 = MessagePtr::adopt(r2);
    p2 = p1;
    EXPECT_EQ(p2.get(), r1);
    EXPECT_EQ(r1->ref_count_.load(), 2);
}

TEST(MessagePtrTest, MoveAssignment) {
    auto* r1 = new TypedMessage<int>(1, 1);
    auto* r2 = new TypedMessage<int>(2, 2);
    MessagePtr p1 = MessagePtr::adopt(r1);
    MessagePtr p2 = MessagePtr::adopt(r2);
    p2 = std::move(p1);
    EXPECT_FALSE(p1);
    EXPECT_EQ(p2.get(), r1);
    EXPECT_EQ(r1->ref_count_.load(), 1);
}

TEST(MessagePtrTest, SelfCopyAssignment) {
    auto* raw = new TypedMessage<int>(1, 1);
    MessagePtr p = MessagePtr::adopt(raw);
    auto& ref = p;
    p = ref;
    EXPECT_EQ(p.get(), raw);
    EXPECT_EQ(raw->ref_count_.load(), 1);
}

TEST(MessagePtrTest, SelfMoveAssignment) {
    auto* raw = new TypedMessage<int>(1, 1);
    MessagePtr p = MessagePtr::adopt(raw);
    auto& ref = p;
    p = std::move(ref);
    EXPECT_EQ(p.get(), raw);
    EXPECT_EQ(raw->ref_count_.load(), 1);
}

TEST(MessagePtrTest, RecyclerCalledOnDestroy) {
    auto* raw = new TypedMessage<int>(1, 1);
    raw->recycler_ = [](IMessage* msg) { static_cast<TypedMessage<int>*>(msg)->data_ = 12345; };
    {
        MessagePtr p = MessagePtr::adopt(raw);
        raw->ref_count_.store(1, std::memory_order_relaxed);
    }
    EXPECT_EQ(raw->data_, 12345);
    delete raw;
}

TEST(MessagePtrTest, ResetToNull) {
    auto* raw = new TypedMessage<int>(1, 1);
    MessagePtr p = MessagePtr::adopt(raw);
    EXPECT_TRUE(p);
    p.reset();
    EXPECT_FALSE(p);
    EXPECT_EQ(p.get(), nullptr);
}

TEST(MessagePtrTest, AdoptNull) {
    MessagePtr p = MessagePtr::adopt(nullptr);
    EXPECT_FALSE(p);
}

TEST(TypedMessageTest, Construction) {
    TypedMessage<std::string> msg(1, "hello");
    EXPECT_EQ(msg.topic_id(), 1u);
    EXPECT_EQ(msg.data_, "hello");
    EXPECT_EQ(msg.type(), typeid(std::string));
}

TEST(TypedMessageTest, ResetForReuse) {
    auto* msg = new TypedMessage<int>(1, 1);
    msg->ref_count_.store(5, std::memory_order_relaxed);
    msg->recycler_ = reinterpret_cast<void (*)(IMessage*)>(0xDEAD);

    msg->reset(2, 99);

    EXPECT_EQ(msg->topic_id(), 2u);
    EXPECT_EQ(msg->data_, 99);
    EXPECT_EQ(msg->ref_count_.load(), 0);
    EXPECT_EQ(msg->recycler_, nullptr);
    delete msg;
}