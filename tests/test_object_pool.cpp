#include "test_support.h"

TEST(ObjectPoolTest, AcquireFromEmpty) {
    ObjectPool<TypedMessage<int>> pool(4);
    EXPECT_EQ(pool.acquire(), nullptr);
}

TEST(ObjectPoolTest, ReleaseAndAcquire) {
    ObjectPool<TypedMessage<int>> pool(4);
    auto* obj = new TypedMessage<int>(1, 42);
    pool.release(obj);
    auto* recycled = pool.acquire();
    EXPECT_EQ(recycled, obj);
    EXPECT_EQ(pool.acquire(), nullptr);
    delete recycled;
}

TEST(ObjectPoolTest, FullPoolDeletesObject) {
    ObjectPool<TypedMessage<int>> pool(2);
    auto* a = new TypedMessage<int>(1, 1);
    auto* b = new TypedMessage<int>(2, 2);
    auto* c = new TypedMessage<int>(3, 3);

    pool.release(a);
    pool.release(b);
    pool.release(c);

    auto* got1 = pool.acquire();
    auto* got2 = pool.acquire();
    auto* got3 = pool.acquire();
    EXPECT_NE(got1, nullptr);
    EXPECT_NE(got2, nullptr);
    EXPECT_EQ(got3, nullptr);
    delete got1;
    delete got2;
}

TEST(ObjectPoolTest, RecycleViaMessagePtr) {
    auto& pool = TypedMessagePool<int>::instance();
    {
        auto* raw = pool.acquire();
        if (!raw)
            raw = new TypedMessage<int>(1, 0);
        raw->reset(2, 77);
        raw->recycler_ = &TypedMessagePool<int>::recycle;
        MessagePtr ptr = MessagePtr::adopt(raw);
    }
    auto* recycled = pool.acquire();
    EXPECT_NE(recycled, nullptr);
    if (recycled) {
        recycled->reset(3, 100);
        EXPECT_EQ(recycled->data_, 100);
        pool.release(recycled);
    }
}