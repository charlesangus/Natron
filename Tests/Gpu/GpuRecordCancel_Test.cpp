#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "GpuBuffer.h"
#include "GpuBufferPool.h"
#include "GpuDevice.h"
#include "GpuRecord.h"
#include "GpuTestDevice.h"

using namespace gpu;

namespace {

constexpr VkDeviceSize kBytes = 1024 * 1024;
constexpr std::chrono::milliseconds kStopAfter(50);
constexpr std::chrono::milliseconds kStopBound(500);

class GpuRecordCancelTest : public gputest::SkippableTest {
protected:
    void SetUp() override
    {
        GpuStatus s = gputest::createDevice(dev);
        GPUTEST_DEVICE_OR_SKIP_TEST(s.ok(), s.message);
        registry = std::make_unique<GpuKernelRegistry>(*dev);
        GpuBufferPoolOptions options;
        options.overrideBudgetBytes = 64 * kBytes;
        pool = std::make_unique<GpuBufferPool>(*dev, options);

        VkSemaphoreTypeCreateInfo ti { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
        ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo si { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        si.pNext = &ti;
        ASSERT_EQ(vkCreateSemaphore(dev->device(), &si, nullptr, &timeline), VK_SUCCESS);
    }

    void TearDown() override
    {
        if (dev) {
            // A failed check must not leave the device waiting on a value nobody signals.
            signalTimeline();
            dev->finishRetiring();
            EXPECT_TRUE(dev->waitIdle().ok());
            if (timeline != VK_NULL_HANDLE) {
                vkDestroySemaphore(dev->device(), timeline, nullptr);
            }
        }
        pool.reset();
        registry.reset();
        dev.reset();
        check.expectClean("the test");
    }

    void signalTimeline()
    {
        if (timeline == VK_NULL_HANDLE || signalled) {
            return;
        }
        VkSemaphoreSignalInfo si { VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO };
        si.semaphore = timeline;
        si.value = 1;
        EXPECT_EQ(vkSignalSemaphore(dev->device(), &si), VK_SUCCESS);
        signalled = true;
    }

    // Records a write of `buffer` that waits for the timeline, then stops the wait kStopAfter into it.
    GpuStatus submitAndStop(const GpuBuffer& buffer,
                            const std::shared_ptr<void>& holding,
                            std::chrono::steady_clock::duration* waited)
    {
        std::unique_ptr<GpuRecordContext> ctx;
        GpuStatus s = GpuRecordContext::create(*registry, ctx, pool.get());
        if (!s) {
            return s;
        }
        ctx->waitTimeline(TimelineValue { timeline, 1 });
        ctx->hold(holding);
        const uint32_t value = 0x3F800000u;
        s = ctx->updateBuffer(buffer, 0, std::as_bytes(std::span(&value, 1)));
        if (!s) {
            return s;
        }
        const auto start = std::chrono::steady_clock::now();
        const std::function<bool()> stop = [start]() { return (std::chrono::steady_clock::now() - start) >= kStopAfter; };
        s = ctx->submitAndWait(30'000'000'000ull, stop);
        *waited = std::chrono::steady_clock::now() - start;
        return s;
    }

    gputest::ValidationCheck check;
    std::unique_ptr<GpuDevice> dev;
    std::unique_ptr<GpuKernelRegistry> registry;
    std::unique_ptr<GpuBufferPool> pool;
    VkSemaphore timeline = VK_NULL_HANDLE;
    bool signalled = false;
};

} // namespace

GPU_TEST_F(GpuRecordCancelTest, StoppedWaitReturnsAndRetiresUntilTheWorkFinishes)
{
    GpuBuffer buffer;
    ASSERT_TRUE(GpuBuffer::createDeviceLocal(*dev, kBytes, kGpuBufferDefaultUsage, buffer).ok());
    auto holding = std::make_shared<int>(1);
    const std::weak_ptr<int> watch = holding;

    std::chrono::steady_clock::duration waited {};
    const GpuStatus s = submitAndStop(buffer, holding, &waited);
    holding.reset();
    EXPECT_EQ(s.result, VK_INCOMPLETE) << s.message;
    EXPECT_LT(waited, kStopBound);
    EXPECT_FALSE(dev->hasAbandonedWork());
    EXPECT_TRUE(dev->hasRetiringWork());
    EXPECT_FALSE(watch.expired());

    buffer = GpuBuffer();
    dev->reapRetired();
    EXPECT_FALSE(watch.expired());

    signalTimeline();
    dev->finishRetiring();
    EXPECT_TRUE(watch.expired());
    EXPECT_FALSE(dev->hasRetiringWork());
    EXPECT_FALSE(dev->hasAbandonedWork());
}

GPU_TEST_F(GpuRecordCancelTest, ReleasedPoolBufferIsNotReusedBeforeTheWorkFinishes)
{
    GpuBufferHandle bound;
    ASSERT_TRUE(pool->acquire(kBytes, bound).ok());
    const VkBuffer original = bound.vkBuffer();

    std::chrono::steady_clock::duration waited {};
    const GpuStatus s = submitAndStop(bound.buffer(), nullptr, &waited);
    ASSERT_EQ(s.result, VK_INCOMPLETE) << s.message;
    bound = GpuBufferHandle();
    EXPECT_GT(pool->stats().quarantinedBytes, 0u);

    {
        GpuBufferHandle next;
        ASSERT_TRUE(pool->acquire(kBytes, next).ok());
        EXPECT_NE(next.vkBuffer(), original);
    }

    signalTimeline();
    dev->finishRetiring();
    std::vector<GpuBufferHandle> after(2);
    ASSERT_TRUE(pool->acquire(kBytes, after[0]).ok());
    ASSERT_TRUE(pool->acquire(kBytes, after[1]).ok());
    EXPECT_TRUE(after[0].vkBuffer() == original || after[1].vkBuffer() == original);
    EXPECT_EQ(pool->stats().quarantinedBytes, 0u);
}
