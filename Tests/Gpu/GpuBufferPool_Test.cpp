#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "GpuBufferPool.h"
#include "GpuDevice.h"
#include "GpuTestDevice.h"

using namespace gpu;

namespace {

constexpr uint64_t kMiB = 1024 * 1024;
constexpr uint64_t kBudget = 32 * kMiB;

class GpuBufferPoolTest : public gputest::SkippableTest {
protected:
    void SetUp() override
    {
        GpuStatus s = gputest::createDevice(dev);
        GPUTEST_DEVICE_OR_SKIP_TEST(s.ok(), s.message);
        GpuBufferPoolOptions options;
        options.overrideBudgetBytes = kBudget;
        pool = std::make_unique<GpuBufferPool>(*dev, options);
    }

    void TearDown() override
    {
        pool.reset();
        dev.reset();
        check.expectClean("the test");
    }

    uint64_t usage() const
    {
        const GpuBufferPoolStats s = pool->stats();
        return s.liveBytes + s.pooledBytes;
    }

    gputest::ValidationCheck check;
    std::unique_ptr<GpuDevice> dev;
    std::unique_ptr<GpuBufferPool> pool;
};

GPU_TEST_F(GpuBufferPoolTest, BudgetIsOverrideCappedByHeap)
{
    EXPECT_EQ(pool->budget(), kBudget);
}

GPU_TEST_F(GpuBufferPoolTest, SizeClassesRoundUpWithinTwentyFivePercent)
{
    for (uint64_t n : std::initializer_list<uint64_t> { 1, 65537, 1000000, 8 * kMiB, 8 * kMiB + 1, 100 * kMiB + 7 }) {
        const uint64_t c = GpuBufferPool::sizeClass(n);
        EXPECT_GE(c, n);
        EXPECT_LE(c, n + n / 4 + 64 * 1024);
    }
}

GPU_TEST_F(GpuBufferPoolTest, UsageNeverExceedsBudget)
{
    std::vector<GpuBufferHandle> held;
    for (int i = 0; i < 40; ++i) {
        GpuBufferHandle h;
        ASSERT_TRUE(pool->acquire(3 * kMiB + static_cast<uint64_t>(i) * 100000, h).ok());
        EXPECT_LE(usage(), kBudget);
        const size_t slot = held.size();
        h.markEvictable([&held, slot] { held[slot] = GpuBufferHandle(); });
        held.push_back(h);
        EXPECT_LE(usage(), kBudget);
    }
    const GpuBufferPoolStats s = pool->stats();
    EXPECT_LE(s.peakBytes, kBudget);
    EXPECT_GT(s.evictions, 0u);
}

GPU_TEST_F(GpuBufferPoolTest, EvictsLeastRecentlyUsedExactlyOnce)
{
    constexpr int n = 4;
    std::vector<GpuBufferHandle> held(n);
    std::vector<int> order;
    std::vector<int> calls(n, 0);
    for (int i = 0; i < n; ++i) {
        ASSERT_TRUE(pool->acquire(8 * kMiB, held[i]).ok());
        held[i].markEvictable([&, i] {
            ++calls[i];
            order.push_back(i);
            held[i] = GpuBufferHandle();
        });
    }
    held[0].touch();

    std::vector<GpuBufferHandle> fresh(n);
    for (int k = 0; k < n; ++k) {
        ASSERT_TRUE(pool->acquire(8 * kMiB, fresh[k]).ok());
        EXPECT_LE(usage(), kBudget);
    }
    EXPECT_EQ(order, (std::vector<int> { 1, 2, 3, 0 }));
    EXPECT_EQ(calls, (std::vector<int>(n, 1)));
    EXPECT_EQ(pool->stats().evictions, static_cast<uint64_t>(n));
}

GPU_TEST_F(GpuBufferPoolTest, EvictFreesPooledThenLeastRecentlyUsed)
{
    constexpr int n = 3;
    std::vector<GpuBufferHandle> held(n);
    std::vector<int> order;
    for (int i = 0; i < n; ++i) {
        ASSERT_TRUE(pool->acquire(8 * kMiB, held[i]).ok());
        held[i].markEvictable([&, i] {
            order.push_back(i);
            held[i] = GpuBufferHandle();
        });
    }
    {
        GpuBufferHandle pooled;
        ASSERT_TRUE(pool->acquire(4 * kMiB, pooled).ok());
    }
    ASSERT_GT(pool->stats().pooledBytes, 0u);
    held[0].touch();

    EXPECT_GE(pool->evict(12 * kMiB), 12 * kMiB);
    EXPECT_EQ(order, (std::vector<int> { 1 }));
    EXPECT_EQ(pool->stats().pooledBytes, 0u);
    EXPECT_EQ(pool->stats().liveBytes, 2 * GpuBufferPool::sizeClass(8 * kMiB));

    EXPECT_GE(pool->evict(~uint64_t(0)), 16 * kMiB);
    EXPECT_EQ(order, (std::vector<int> { 1, 2, 0 }));
    EXPECT_EQ(usage(), 0u);
}

GPU_TEST_F(GpuBufferPoolTest, LastReleaseReturnsBufferToPool)
{
    GpuBufferHandle a;
    ASSERT_TRUE(pool->acquire(4 * kMiB, a).ok());
    GpuBufferHandle b = a;
    const VkBuffer raw = a.vkBuffer();
    a = GpuBufferHandle();
    EXPECT_EQ(pool->stats().pooledBytes, 0u);
    EXPECT_EQ(pool->stats().liveBytes, 4 * kMiB);
    b = GpuBufferHandle();
    EXPECT_EQ(pool->stats().pooledBytes, 4 * kMiB);
    EXPECT_EQ(pool->stats().liveBytes, 0u);

    GpuBufferHandle c;
    ASSERT_TRUE(pool->acquire(4 * kMiB, c).ok());
    EXPECT_EQ(c.vkBuffer(), raw);
    EXPECT_EQ(pool->stats().pooledBytes, 0u);
}

GPU_TEST_F(GpuBufferPoolTest, PooledBuffersAreTrimmedBeforeEvicting)
{
    GpuBufferHandle victim;
    ASSERT_TRUE(pool->acquire(8 * kMiB, victim).ok());
    int calls = 0;
    victim.markEvictable([&] { ++calls; });
    {
        GpuBufferHandle p1, p2, p3;
        ASSERT_TRUE(pool->acquire(8 * kMiB, p1).ok());
        ASSERT_TRUE(pool->acquire(4 * kMiB, p2).ok());
        ASSERT_TRUE(pool->acquire(4 * kMiB, p3).ok());
    }
    EXPECT_EQ(pool->stats().pooledBytes, 16 * kMiB);
    GpuBufferHandle big;
    ASSERT_TRUE(pool->acquire(24 * kMiB, big).ok());
    EXPECT_EQ(calls, 0);
    EXPECT_LE(usage(), kBudget);
}

GPU_TEST_F(GpuBufferPoolTest, OversizeRequestFailsWithoutEvicting)
{
    GpuBufferHandle h;
    ASSERT_TRUE(pool->acquire(8 * kMiB, h).ok());
    int calls = 0;
    h.markEvictable([&] { ++calls; });

    GpuBufferHandle big;
    const GpuStatus s = pool->acquire(kBudget + 1, big);
    EXPECT_EQ(s.result, VK_ERROR_OUT_OF_DEVICE_MEMORY);
    EXPECT_FALSE(static_cast<bool>(big));
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(pool->stats().evictions, 0u);
    EXPECT_TRUE(static_cast<bool>(h));
}

GPU_TEST_F(GpuBufferPoolTest, OutOfMemoryWhenNothingIsEvictable)
{
    GpuBufferHandle a, b;
    ASSERT_TRUE(pool->acquire(16 * kMiB, a).ok());
    ASSERT_TRUE(pool->acquire(16 * kMiB, b).ok());
    GpuBufferHandle c;
    EXPECT_EQ(pool->acquire(4 * kMiB, c).result, VK_ERROR_OUT_OF_DEVICE_MEMORY);
}

GPU_TEST_F(GpuBufferPoolTest, CallbackMayReenterPool)
{
    GpuBufferHandle h;
    ASSERT_TRUE(pool->acquire(24 * kMiB, h).ok());
    GpuBufferHandle spare;
    h.markEvictable([&] {
        h = GpuBufferHandle();
        GpuBufferHandle small;
        EXPECT_TRUE(pool->acquire(kMiB, small).ok());
    });
    GpuBufferHandle next;
    ASSERT_TRUE(pool->acquire(16 * kMiB, next).ok());
    EXPECT_LE(usage(), kBudget);
}

GPU_TEST_F(GpuBufferPoolTest, ConcurrentAcquireStaysWithinBudget)
{
    std::vector<std::thread> threads;
    std::atomic<int> failures { 0 };
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 50; ++i) {
                GpuBufferHandle h;
                if (!pool->acquire(2 * kMiB, h).ok()) {
                    ++failures;
                    continue;
                }
                h.touch();
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    EXPECT_EQ(failures.load(), 0);
    EXPECT_LE(pool->stats().peakBytes, kBudget);
    EXPECT_EQ(pool->stats().liveBytes, 0u);
}

} // namespace
