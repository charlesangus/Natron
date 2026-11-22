// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "Tests/Flow/FlowTestPlate.h"
#include "Tests/Gpu/GpuFlowTestSupport.h"

#include "Engine/Flow/DisFlow.h"
#include "Engine/Flow/TvL1Flow.h"
#include "Engine/Gpu/GpuBufferPool.h"

NATRON_NAMESPACE_USING
#ifdef NATRON_HAVE_GPU
using namespace FlowTest;
using namespace GpuFlowTest;

namespace {
const int kWidth = 256;
const int kHeight = 192;
const std::uint64_t kNeverSplit = UINT64_MAX;
const std::uint64_t kSplitEveryCharge = 1;

// One pass of this small a frame is far under the bound on any device, so missing it means the stop was not honoured.
const int kBatchesBeforeAbort = 3;
const std::chrono::milliseconds kAbortBound(2000);

class BatchOverride {
public:
    explicit BatchOverride(std::uint64_t work,
                           const std::function<void()>& hook = std::function<void()>())
    {
        gpu::GpuRecordContext::setBatchWorkForTests(work);
        gpu::GpuRecordContext::setBatchFinishedHookForTests(hook);
    }

    ~BatchOverride()
    {
        gpu::GpuRecordContext::setBatchWorkForTests(0);
        gpu::GpuRecordContext::setBatchFinishedHookForTests(std::function<void()>());
    }

    BatchOverride(const BatchOverride&) = delete;
    BatchOverride& operator=(const BatchOverride&) = delete;
};

// A context of `batches` command buffers, each writing its index plus one to its own float of `buffer`.
void
recordWrites(gpu::GpuRecordContext& ctx,
             const gpu::GpuBuffer& buffer,
             int batches)
{
    for (int i = 0; i < batches; ++i) {
        const float value = (float)(i + 1);
        const gpu::GpuStatus written = ctx.updateBuffer(buffer, (VkDeviceSize)i * sizeof(float), std::as_bytes(std::span(&value, 1)));
        ASSERT_TRUE(bool(written)) << written.message;
        ctx.computeBarrier();
        if ((i + 1) < batches) {
            const gpu::GpuStatus charged = ctx.chargeWork(1);
            ASSERT_TRUE(bool(charged)) << charged.message;
        }
    }
}

void
expectSameField(const FlowField& a,
                const FlowField& b)
{
    ASSERT_EQ(a.width, b.width);
    ASSERT_EQ(a.height, b.height);
    int mismatches = 0;
    for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
            if ((a.u.at(x, y) != b.u.at(x, y)) || (a.v.at(x, y) != b.v.at(x, y)) || (a.confidence.at(x, y) != b.confidence.at(x, y))) {
                ++mismatches;
            }
        }
    }
    EXPECT_EQ(0, mismatches);
}

template <typename Params>
void
expectSplitMatchesUnsplit(const Params& params)
{
    const NoiseTexture texture(2024);
    const Frame a = translatedFrame(texture, kWidth, kHeight, 0., 0.);
    const Frame b = translatedFrame(texture, kWidth, kHeight, 3.5, -2.25);
    Harness harness;
    if (!harness.create()) {
        return;
    }
    FlowField unsplit;
    {
        const BatchOverride never(kNeverSplit);
        ASSERT_TRUE(solveOnDevice(harness, a.pixels, b.pixels, a.width, a.height, params, &unsplit));
    }
    // Every pass alone, and passes cut across a level at a size that also divides the DIS search by rows.
    for (std::uint64_t work : { kSplitEveryCharge, (std::uint64_t)1 << 16 }) {
        std::atomic<int> batches { 0 };
        const BatchOverride split(work, [&batches]() { ++batches; });
        FlowField field;
        ASSERT_TRUE(solveOnDevice(harness, a.pixels, b.pixels, a.width, a.height, params, &field));
        EXPECT_GT(batches.load(), 1);
        expectSameField(unsplit, field);
    }
}

template <typename Solve>
void
expectAbortStopsTheSolve(const Solve& solve)
{
    const NoiseTexture texture(2024);
    const Frame a = translatedFrame(texture, kWidth, kHeight, 0., 0.);
    const Frame b = translatedFrame(texture, kWidth, kHeight, 3.5, -2.25);
    Harness harness;
    if (!harness.create()) {
        return;
    }
    const gpu::GpuFlowFormat format = formatFor(kWidth, kHeight, 1, false);
    const gpu::GpuFlowImage imageA = harness.image(a.pixels, kWidth, kHeight, 1);
    const gpu::GpuFlowImage imageB = harness.image(b.pixels, kWidth, kHeight, 1);
    gpu::GpuBufferPool& pool = harness.runtime().pool();
    const std::uint64_t liveBefore = pool.stats().liveBytes;

    std::atomic<int> batches { 0 };
    std::atomic<bool> stop { false };
    std::chrono::steady_clock::time_point stoppedAt;
    const BatchOverride split(kSplitEveryCharge, [&]() {
        if (++batches == kBatchesBeforeAbort) {
            stoppedAt = std::chrono::steady_clock::now();
            stop = true;
        }
    });
    {
        std::unique_ptr<gpu::GpuRecordContext> ctx;
        gpu::GpuStatus status = harness.runtime().newRecordContext(ctx);
        ASSERT_TRUE(bool(status)) << status.message;
        gpu::GpuFlow flow;
        gpu::GpuFlowPyramid pyramidA;
        gpu::GpuFlowPyramid pyramidB;
        status = flow.buildPyramid(*ctx, imageA, format, &pyramidA);
        if (status) {
            status = flow.buildPyramid(*ctx, imageB, format, &pyramidB);
        }
        if (status) {
            status = solve(flow, *ctx, pyramidA, pyramidB);
        }
        ASSERT_TRUE(bool(status)) << status.message;
        const std::size_t recorded = ctx->batchCount();
        ASSERT_GT(recorded, (std::size_t)kBatchesBeforeAbort * 4);
        EXPECT_GT(pool.stats().liveBytes, liveBefore);

        status = ctx->submitAndWait(kSubmitTimeoutNs, [&stop]() { return stop.load(); });
        const std::chrono::steady_clock::time_point returned = std::chrono::steady_clock::now();
        EXPECT_EQ(VK_INCOMPLETE, status.result) << status.message;
        EXPECT_LT(returned - stoppedAt, kAbortBound);
        EXPECT_EQ(kBatchesBeforeAbort, batches.load()) << "work ran after the stop";
    }
    EXPECT_LE(pool.stats().liveBytes, liveBefore) << "scratch of the stopped solve is still held";
}
} // anonymous namespace

TEST(GpuSplitWork, ChargingUpToTheBudgetEndsTheCommandBuffer)
{
    Harness harness;
    if (!harness.create()) {
        return;
    }
    const BatchOverride budget(10);
    std::unique_ptr<gpu::GpuRecordContext> ctx;
    gpu::GpuStatus status = harness.runtime().newRecordContext(ctx);
    ASSERT_TRUE(bool(status)) << status.message;
    EXPECT_EQ(10u, ctx->batchWork());
    EXPECT_EQ(1u, ctx->batchCount());
    EXPECT_TRUE(bool(ctx->chargeWork(4)));
    EXPECT_EQ(1u, ctx->batchCount());
    EXPECT_TRUE(bool(ctx->chargeWork(6)));
    EXPECT_EQ(2u, ctx->batchCount());
    EXPECT_TRUE(bool(ctx->chargeWork(9)));
    EXPECT_EQ(2u, ctx->batchCount());
    EXPECT_TRUE(bool(ctx->chargeWork(1)));
    EXPECT_EQ(3u, ctx->batchCount());
    status = ctx->submitAndWait();
    EXPECT_TRUE(bool(status)) << status.message;
    EXPECT_FALSE(bool(ctx->chargeWork(1)));
}

TEST(GpuSplitWork, AnUnsplitContextNeverCallsTheHookOrTheStop)
{
    Harness harness;
    if (!harness.create()) {
        return;
    }
    int finished = 0;
    const BatchOverride never(kNeverSplit, [&finished]() { ++finished; });
    std::unique_ptr<gpu::GpuRecordContext> ctx;
    gpu::GpuStatus status = harness.runtime().newRecordContext(ctx);
    ASSERT_TRUE(bool(status)) << status.message;
    EXPECT_TRUE(bool(ctx->chargeWork(1ull << 40)));
    EXPECT_EQ(1u, ctx->batchCount());
    int asked = 0;
    status = ctx->submitAndWait(30'000'000'000ull, [&asked]() { ++asked; return true; });
    EXPECT_TRUE(bool(status)) << status.message;
    EXPECT_EQ(0, finished);
    EXPECT_EQ(0, asked);
}

TEST(GpuSplitWork, EveryCommandBufferOfASubmitRunsAndTheLastSignals)
{
    Harness harness;
    if (!harness.create()) {
        return;
    }
    const int batches = 5;
    const gpu::GpuBuffer* buffer = harness.upload(std::vector<float>((std::size_t)batches, 0.f));
    ASSERT_TRUE(buffer != nullptr);
    gpu::GpuDevice& device = harness.runtime().device();
    VkSemaphore timeline = VK_NULL_HANDLE;
    ASSERT_TRUE(bool(device.createTimelineSemaphore(0, timeline)));
    int finished = 0;
    {
        const BatchOverride split(kSplitEveryCharge, [&finished]() { ++finished; });
        std::unique_ptr<gpu::GpuRecordContext> ctx;
        gpu::GpuStatus status = harness.runtime().newRecordContext(ctx);
        ASSERT_TRUE(bool(status)) << status.message;
        recordWrites(*ctx, *buffer, batches);
        EXPECT_EQ((std::size_t)batches, ctx->batchCount());
        const gpu::TimelineValue done { timeline, 1 };
        status = ctx->submit(std::span<const gpu::TimelineValue>(), done);
        EXPECT_TRUE(bool(status)) << status.message;
        status = harness.runtime().wait(done);
        EXPECT_TRUE(bool(status)) << status.message;
    }
    EXPECT_EQ(batches - 1, finished);
    const std::vector<float> got = harness.read(buffer, (std::size_t)batches);
    ASSERT_EQ((std::size_t)batches, got.size());
    for (int i = 0; i < batches; ++i) {
        EXPECT_EQ((float)(i + 1), got[(std::size_t)i]) << "command buffer " << i;
    }
    vkDestroySemaphore(device.device(), timeline, nullptr);
}

TEST(GpuSplitWork, AStoppedSubmitRunsNothingMoreAndNeverSignals)
{
    Harness harness;
    if (!harness.create()) {
        return;
    }
    const int batches = 6;
    const int ran = 2;
    const gpu::GpuBuffer* buffer = harness.upload(std::vector<float>((std::size_t)batches, 0.f));
    ASSERT_TRUE(buffer != nullptr);
    gpu::GpuDevice& device = harness.runtime().device();
    VkSemaphore timeline = VK_NULL_HANDLE;
    ASSERT_TRUE(bool(device.createTimelineSemaphore(0, timeline)));
    int finished = 0;
    {
        const BatchOverride split(kSplitEveryCharge, [&finished]() { ++finished; });
        std::unique_ptr<gpu::GpuRecordContext> ctx;
        gpu::GpuStatus status = harness.runtime().newRecordContext(ctx);
        ASSERT_TRUE(bool(status)) << status.message;
        recordWrites(*ctx, *buffer, batches);
        status = ctx->submit(std::span<const gpu::TimelineValue>(), gpu::TimelineValue { timeline, 1 }, [&]() { return finished >= ran; });
        EXPECT_EQ(VK_INCOMPLETE, status.result) << status.message;
    }
    EXPECT_EQ(ran, finished);
    uint64_t signalled = 0;
    ASSERT_EQ(VK_SUCCESS, vkGetSemaphoreCounterValue(device.device(), timeline, &signalled));
    EXPECT_EQ(0u, signalled);
    const std::vector<float> got = harness.read(buffer, (std::size_t)batches);
    ASSERT_EQ((std::size_t)batches, got.size());
    for (int i = 0; i < batches; ++i) {
        EXPECT_EQ((i < ran) ? (float)(i + 1) : 0.f, got[(std::size_t)i]) << "command buffer " << i;
    }
    vkDestroySemaphore(device.device(), timeline, nullptr);
}

TEST(GpuSplitWork, ASplitDisSolveMatchesAnUnsplitOne)
{
    expectSplitMatchesUnsplit(disParamsForQuality(eFlowQualityMedium));
}

TEST(GpuSplitWork, ASplitTvL1SolveMatchesAnUnsplitOne)
{
    expectSplitMatchesUnsplit(tvL1ParamsForQuality(eFlowQualityMedium));
}

TEST(GpuSplitWork, AbortingADisSolveStopsItAndReleasesItsScratch)
{
    const gpu::GpuFlowDisParams params = gpuParams(disParamsForQuality(eFlowQualityMedium));
    expectAbortStopsTheSolve([&params](gpu::GpuFlow& flow, gpu::GpuRecordContext& ctx, const gpu::GpuFlowPyramid& a, const gpu::GpuFlowPyramid& b) {
        gpu::GpuFlowField field;

        return flow.solveDis(ctx, a, b, nullptr, params, &field);
    });
}

TEST(GpuSplitWork, AbortingATvL1SolveStopsItAndReleasesItsScratch)
{
    const gpu::GpuFlowTvL1Params params = gpuParams(tvL1ParamsForQuality(eFlowQualityMedium));
    expectAbortStopsTheSolve([&params](gpu::GpuFlow& flow, gpu::GpuRecordContext& ctx, const gpu::GpuFlowPyramid& a, const gpu::GpuFlowPyramid& b) {
        gpu::GpuFlowField field;

        return flow.solveTvL1(ctx, a, b, nullptr, params, &field);
    });
}
#endif // NATRON_HAVE_GPU
