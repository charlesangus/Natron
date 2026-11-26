#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <thread>
#include <vector>

#include "GpuBuffer.h"
#include "GpuBufferPool.h"
#include "GpuDevice.h"
#include "GpuKernelRegistry.h"
#include "GpuRecord.h"
#include "GpuTestDevice.h"

using namespace gpu;

namespace {

constexpr VkDeviceSize kBytes = 1024 * 1024;
constexpr std::chrono::milliseconds kStopAfter(50);
// A stop is seen within one 20 ms poll of the wait; a second poll and scheduling make up the rest.
constexpr std::chrono::milliseconds kStopBound = kStopAfter + 2 * std::chrono::milliseconds(20) + std::chrono::milliseconds(60);
constexpr uint64_t kLastTimelineValue = 2;

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
        GpuRecordContext::setBatchWorkForTests(0);
        if (dev) {
            // A failed check must not leave the device waiting on a value nobody signals.
            signalTimeline(kLastTimelineValue);
            dev->finishRetiring();
            if (dev->hasAbandonedWork()) {
                // The device is leaked with its work, which may still wait on the semaphore.
                timeline = VK_NULL_HANDLE;
            } else if (dev->isLost()) {
                dev->drainQueues();
            } else {
                EXPECT_TRUE(dev->waitIdle().ok());
            }
            if (timeline != VK_NULL_HANDLE) {
                vkDestroySemaphore(dev->device(), timeline, nullptr);
            }
        }
        pool.reset();
        registry.reset();
        dev.reset();
        check.expectClean("the test");
    }

    void signalTimeline(uint64_t value)
    {
        if (timeline == VK_NULL_HANDLE || signalled >= value) {
            return;
        }
        VkSemaphoreSignalInfo si { VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO };
        si.semaphore = timeline;
        si.value = value;
        EXPECT_EQ(vkSignalSemaphore(dev->device(), &si), VK_SUCCESS);
        signalled = value;
    }

    // Records a write of `buffer` that waits for the timeline to reach `waitValue`; `split` ends a command buffer
    // between two such writes, so that the first is run and waited on by itself.
    GpuStatus record(std::unique_ptr<GpuRecordContext>& ctx,
                     const GpuBuffer& buffer,
                     const std::shared_ptr<void>& holding,
                     uint64_t waitValue,
                     bool split)
    {
        GpuStatus s = GpuRecordContext::create(*registry, ctx, pool.get());
        if (!s) {
            return s;
        }
        ctx->waitTimeline(TimelineValue { timeline, waitValue });
        ctx->hold(holding);
        const uint32_t value = 0x3F800000u;
        s = ctx->updateBuffer(buffer, 0, std::as_bytes(std::span(&value, 1)));
        if (!s || !split) {
            return s;
        }
        s = ctx->chargeWork(1);
        if (!s) {
            return s;
        }
        return ctx->updateBuffer(buffer, 4, std::as_bytes(std::span(&value, 1)));
    }

    // Submits the context and stops its wait kStopAfter into it.
    static GpuStatus submitAndStop(GpuRecordContext& ctx, std::chrono::steady_clock::duration* waited)
    {
        const auto start = std::chrono::steady_clock::now();
        const std::function<bool()> stop = [start]() { return (std::chrono::steady_clock::now() - start) >= kStopAfter; };
        GpuStatus s = ctx.submitAndWait(30'000'000'000ull, stop);
        *waited = std::chrono::steady_clock::now() - start;
        return s;
    }

    GpuStatus recordSubmitAndStop(const GpuBuffer& buffer,
                                  const std::shared_ptr<void>& holding,
                                  std::chrono::steady_clock::duration* waited,
                                  uint64_t waitValue = 1,
                                  bool split = false)
    {
        std::unique_ptr<GpuRecordContext> ctx;
        GpuStatus s = record(ctx, buffer, holding, waitValue, split);
        if (!s) {
            return s;
        }
        return submitAndStop(*ctx, waited);
    }

    // Reaps until `watch` expires, which a fence the device signals soon brings within the bound.
    bool reapUntilExpired(const std::weak_ptr<void>& watch)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!watch.expired() && std::chrono::steady_clock::now() < deadline) {
            dev->reapRetired();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return watch.expired();
    }

    // Asking for this thread's command pool frees the retired command buffers whose work is done.
    void sweepCommandBuffers()
    {
        std::unique_ptr<GpuRecordContext> ctx;
        EXPECT_TRUE(GpuRecordContext::create(*registry, ctx, pool.get()).ok());
    }

    gputest::ValidationCheck check;
    std::unique_ptr<GpuDevice> dev;
    std::unique_ptr<GpuKernelRegistry> registry;
    std::unique_ptr<GpuBufferPool> pool;
    VkSemaphore timeline = VK_NULL_HANDLE;
    uint64_t signalled = 0;
};

} // namespace

GPU_TEST_F(GpuRecordCancelTest, StoppedWaitReturnsAndRetiresUntilTheWorkFinishes)
{
    GpuBuffer buffer;
    ASSERT_TRUE(GpuBuffer::createDeviceLocal(*dev, kBytes, kGpuBufferDefaultUsage, buffer).ok());
    auto holding = std::make_shared<int>(1);
    const std::weak_ptr<int> watch = holding;

    std::chrono::steady_clock::duration waited {};
    const GpuStatus s = recordSubmitAndStop(buffer, holding, &waited);
    holding.reset();
    EXPECT_EQ(s.result, VK_INCOMPLETE) << s.message;
    EXPECT_LT(waited, kStopBound);
    EXPECT_FALSE(dev->hasAbandonedWork());
    EXPECT_TRUE(dev->hasRetiringWork());
    EXPECT_FALSE(watch.expired());
    EXPECT_GT(registry->retiredCommandBufferCountForTests(), 0u);

    buffer = GpuBuffer();
    dev->reapRetired();
    EXPECT_FALSE(watch.expired());

    signalTimeline(1);
    dev->finishRetiring();
    EXPECT_TRUE(watch.expired());
    EXPECT_FALSE(dev->hasRetiringWork());
    EXPECT_FALSE(dev->hasAbandonedWork());

    sweepCommandBuffers();
    EXPECT_EQ(registry->retiredCommandBufferCountForTests(), 0u);
}

GPU_TEST_F(GpuRecordCancelTest, ReleasedPoolBufferIsNotReusedBeforeTheWorkFinishes)
{
    GpuBufferHandle bound;
    ASSERT_TRUE(pool->acquire(kBytes, bound).ok());
    const VkBuffer original = bound.vkBuffer();

    std::chrono::steady_clock::duration waited {};
    const GpuStatus s = recordSubmitAndStop(bound.buffer(), nullptr, &waited);
    ASSERT_EQ(s.result, VK_INCOMPLETE) << s.message;
    bound = GpuBufferHandle();
    EXPECT_GT(pool->stats().quarantinedBytes, 0u);

    {
        GpuBufferHandle next;
        ASSERT_TRUE(pool->acquire(kBytes, next).ok());
        EXPECT_NE(next.vkBuffer(), original);
    }

    signalTimeline(1);
    dev->finishRetiring();
    std::vector<GpuBufferHandle> after(2);
    ASSERT_TRUE(pool->acquire(kBytes, after[0]).ok());
    ASSERT_TRUE(pool->acquire(kBytes, after[1]).ok());
    EXPECT_TRUE(after[0].vkBuffer() == original || after[1].vkBuffer() == original);
    EXPECT_EQ(pool->stats().quarantinedBytes, 0u);
}

GPU_TEST_F(GpuRecordCancelTest, PoolBufferTheRetiringWorkDoesNotUseIsReusedAtOnce)
{
    GpuBuffer target;
    ASSERT_TRUE(GpuBuffer::createDeviceLocal(*dev, kBytes, kGpuBufferDefaultUsage, target).ok());
    std::chrono::steady_clock::duration waited {};
    const GpuStatus s = recordSubmitAndStop(target, nullptr, &waited);
    ASSERT_EQ(s.result, VK_INCOMPLETE) << s.message;
    ASSERT_TRUE(dev->hasRetiringWork());

    VkBuffer unrelated = VK_NULL_HANDLE;
    {
        GpuBufferHandle handle;
        ASSERT_TRUE(pool->acquire(kBytes, handle).ok());
        unrelated = handle.vkBuffer();
    }
    EXPECT_EQ(pool->stats().quarantinedBytes, 0u);
    GpuBufferHandle again;
    ASSERT_TRUE(pool->acquire(kBytes, again).ok());
    EXPECT_EQ(again.vkBuffer(), unrelated);
}

GPU_TEST_F(GpuRecordCancelTest, StopWhileASplitContextWaitsForAnEarlierBatchRetiresIt)
{
    GpuBuffer buffer;
    ASSERT_TRUE(GpuBuffer::createDeviceLocal(*dev, kBytes, kGpuBufferDefaultUsage, buffer).ok());
    auto holding = std::make_shared<int>(1);
    const std::weak_ptr<int> watch = holding;

    GpuRecordContext::setBatchWorkForTests(1);
    std::unique_ptr<GpuRecordContext> ctx;
    ASSERT_TRUE(record(ctx, buffer, holding, 1, true).ok());
    GpuRecordContext::setBatchWorkForTests(0);
    holding.reset();

    std::chrono::steady_clock::duration waited {};
    const GpuStatus s = submitAndStop(*ctx, &waited);
    EXPECT_EQ(s.result, VK_INCOMPLETE) << s.message;
    EXPECT_LT(waited, kStopBound);
    EXPECT_TRUE(dev->hasRetiringWork());
    EXPECT_FALSE(dev->hasAbandonedWork());
    // The batch that was waited on and the one never submitted.
    EXPECT_EQ(registry->retiredCommandBufferCountForTests(), 2u);
    ctx.reset();
    EXPECT_FALSE(watch.expired());

    signalTimeline(1);
    dev->finishRetiring();
    EXPECT_TRUE(watch.expired());
    EXPECT_FALSE(dev->hasRetiringWork());
    sweepCommandBuffers();
    EXPECT_EQ(registry->retiredCommandBufferCountForTests(), 0u);
}

GPU_TEST_F(GpuRecordCancelTest, BufferHeldByOneRetiringContextAndBoundByAnotherWaitsForBoth)
{
    GpuBufferHandle bound;
    ASSERT_TRUE(pool->acquire(kBytes, bound).ok());
    const VkBuffer original = bound.vkBuffer();
    // The first context holds the only other reference to the buffer; the second binds it without holding it.
    auto firstHolding = std::make_shared<GpuBufferHandle>(bound);
    const std::weak_ptr<void> firstDone = firstHolding;

    std::chrono::steady_clock::duration waited {};
    GpuStatus s = recordSubmitAndStop(bound.buffer(), firstHolding, &waited, 1);
    ASSERT_EQ(s.result, VK_INCOMPLETE) << s.message;
    s = recordSubmitAndStop(bound.buffer(), nullptr, &waited, kLastTimelineValue);
    ASSERT_EQ(s.result, VK_INCOMPLETE) << s.message;
    firstHolding.reset();
    bound = GpuBufferHandle();
    EXPECT_EQ(pool->stats().quarantinedBytes, 0u);

    signalTimeline(1);
    ASSERT_TRUE(reapUntilExpired(firstDone));
    EXPECT_TRUE(dev->hasRetiringWork());
    EXPECT_GT(pool->stats().quarantinedBytes, 0u);
    {
        GpuBufferHandle next;
        ASSERT_TRUE(pool->acquire(kBytes, next).ok());
        EXPECT_NE(next.vkBuffer(), original);
    }

    signalTimeline(kLastTimelineValue);
    dev->finishRetiring();
    EXPECT_FALSE(dev->hasRetiringWork());
    std::vector<GpuBufferHandle> after(2);
    ASSERT_TRUE(pool->acquire(kBytes, after[0]).ok());
    ASSERT_TRUE(pool->acquire(kBytes, after[1]).ok());
    EXPECT_TRUE(after[0].vkBuffer() == original || after[1].vkBuffer() == original);
    EXPECT_EQ(pool->stats().quarantinedBytes, 0u);
}

GPU_TEST_F(GpuRecordCancelTest, CommandBuffersOfAnExitedThreadAreFreedByAnother)
{
    GpuBuffer buffer;
    ASSERT_TRUE(GpuBuffer::createDeviceLocal(*dev, kBytes, kGpuBufferDefaultUsage, buffer).ok());
    VkResult result = VK_SUCCESS;
    std::thread worker([&]() {
        std::chrono::steady_clock::duration waited {};
        result = recordSubmitAndStop(buffer, nullptr, &waited).result;
    });
    worker.join();
    ASSERT_EQ(result, VK_INCOMPLETE);
    EXPECT_GT(registry->retiredCommandBufferCountForTests(), 0u);

    signalTimeline(1);
    dev->finishRetiring();
    sweepCommandBuffers();
    EXPECT_EQ(registry->retiredCommandBufferCountForTests(), 0u);
}

GPU_TEST_F(GpuRecordCancelTest, LossWhileRetiringKeepsTheHoldingsUntilTheFenceSignals)
{
    GpuBuffer buffer;
    ASSERT_TRUE(GpuBuffer::createDeviceLocal(*dev, kBytes, kGpuBufferDefaultUsage, buffer).ok());
    auto holding = std::make_shared<int>(1);
    const std::weak_ptr<int> watch = holding;
    std::chrono::steady_clock::duration waited {};
    const GpuStatus s = recordSubmitAndStop(buffer, holding, &waited);
    holding.reset();
    ASSERT_EQ(s.result, VK_INCOMPLETE) << s.message;

    dev->injectFault(GpuFaultPlan::Kind::DeviceLost, 1);
    EXPECT_EQ(dev->submit(QueueKind::Compute, {}, VK_NULL_HANDLE).result, VK_ERROR_DEVICE_LOST);
    EXPECT_TRUE(dev->isLost());
    EXPECT_FALSE(dev->hasAbandonedWork());
    // An injected loss leaves the device running the work, so what it uses stays until its fence signals.
    dev->reapRetired();
    EXPECT_TRUE(dev->hasRetiringWork());
    EXPECT_FALSE(watch.expired());

    signalTimeline(1);
    dev->finishRetiring();
    EXPECT_TRUE(watch.expired());
    EXPECT_FALSE(dev->hasRetiringWork());
    EXPECT_FALSE(dev->hasAbandonedWork());
}

GPU_TEST_F(GpuRecordCancelTest, AbandonedDeviceParksTheRetiringHoldings)
{
    GpuBuffer buffer;
    ASSERT_TRUE(GpuBuffer::createDeviceLocal(*dev, kBytes, kGpuBufferDefaultUsage, buffer).ok());
    auto holding = std::make_shared<int>(1);
    const std::weak_ptr<int> watch = holding;
    std::chrono::steady_clock::duration waited {};
    const GpuStatus s = recordSubmitAndStop(buffer, holding, &waited);
    holding.reset();
    ASSERT_EQ(s.result, VK_INCOMPLETE) << s.message;

    dev->abandonInFlightWork("the test");
    EXPECT_TRUE(dev->isLost());
    EXPECT_FALSE(dev->hasRetiringWork());
    signalTimeline(1);
    dev->finishRetiring();
    dev->reapRetired();
    EXPECT_FALSE(watch.expired());
}
