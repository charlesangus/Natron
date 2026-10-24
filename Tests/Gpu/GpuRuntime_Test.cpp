#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <unistd.h>

#include "GpuRuntime.h"
#include "GpuTestDevice.h"
#include "fill_spirv.h"

using namespace gpu;

namespace {

constexpr uint32_t kW = 64;
constexpr uint32_t kH = 32;
constexpr VkDeviceSize kBytes = VkDeviceSize(kW) * kH * sizeof(uint32_t);

struct FillParams {
    uint32_t width;
    uint32_t height;
    uint32_t asFloat;
};

class GpuRuntimeTest : public ::testing::Test {
protected:
    GpuStatus make(GpuFaultPlan fault = {})
    {
        GpuRuntimeOptions o;
        o.device = gputest::deviceOptions();
        o.device.fault = fault;
        // The live heap budget drifts between reads, so budget comparisons need a fixed pool budget.
        o.pool.overrideBudgetBytes = 256ull << 20;
        o.pipelines.directory = pipelineDir();
        GpuStatus s = GpuRuntime::create(o, rt);
        if (!s) {
            return s;
        }
        GpuKernelDesc d;
        d.spirv = fill_spirv;
        d.spirvWords = fill_spirv_words;
        d.entry = fill_entry;
        d.storageBufferCount = fill_storage_buffers;
        d.pushConstantBytes = fill_push_bytes;
        d.groupSize = { fill_group_size[0], fill_group_size[1], fill_group_size[2] };
        fillId = rt->registerKernel(d);
        return s;
    }

    void TearDown() override
    {
        rt.reset();
        std::error_code ec;
        std::filesystem::remove_all(pipelineDir(), ec);
        check.expectClean("the test");
    }

    static std::string pipelineDir()
    {
        return (std::filesystem::temp_directory_path() / ("natron-gpu-runtime-test-" + std::to_string(::getpid()))).string();
    }

    // Upload zeros, fill on the compute queue, download, and return the number of wrong words.
    size_t runFill(GpuStatus* failure = nullptr)
    {
        GpuBufferHandle h;
        GpuStatus s = rt->acquire(kBytes, h);
        if (!s) {
            if (failure) {
                *failure = s;
            }
            return ~size_t(0);
        }
        std::vector<uint32_t> host(kW * kH, 0);
        TimelineValue up;
        s = rt->upload({ host.data(), kBytes }, h.buffer(), 0, up);
        if (s) {
            s = rt->wait(up);
        }
        std::unique_ptr<GpuRecordContext> ctx;
        if (s) {
            s = rt->newRecordContext(ctx);
        }
        if (s) {
            const FillParams p { kW, kH, 0 };
            s = ctx->dispatch(fillId, { &h.buffer() }, p, { kW, kH, 1 });
        }
        if (s) {
            ctx->computeBarrier();
            s = ctx->submitAndWait();
        }
        TimelineValue down;
        if (s) {
            s = rt->download(h.buffer(), 0, { host.data(), kBytes }, down);
        }
        if (s) {
            s = rt->wait(down);
        }
        if (!s) {
            if (failure) {
                *failure = s;
            }
            return ~size_t(0);
        }
        size_t bad = 0;
        for (size_t i = 0; i < host.size(); ++i) {
            bad += host[i] != i;
        }
        return bad;
    }

    std::unique_ptr<GpuRuntime> rt;
    GpuKernelId fillId {};
    gputest::ValidationCheck check;
};

} // namespace

TEST(GpuFaultPlanTest, Parse)
{
    GpuFaultPlan p;
    EXPECT_TRUE(GpuFaultPlan::parse("lost@3", p));
    EXPECT_EQ(p.kind, GpuFaultPlan::Kind::DeviceLost);
    EXPECT_EQ(p.atOperation, 3u);
    EXPECT_TRUE(GpuFaultPlan::parse("oom@12", p));
    EXPECT_EQ(p.kind, GpuFaultPlan::Kind::OutOfMemory);
    for (const char* bad : { "", "lost", "lost@", "lost@0", "lost@x", "boom@1", "@1", "oom@-1" }) {
        EXPECT_FALSE(GpuFaultPlan::parse(bad, p)) << bad;
    }
}

TEST_F(GpuRuntimeTest, HealthyRunFills)
{
    GpuStatus s = make();
    GPUTEST_DEVICE_OR_SKIP(s.ok(), s.message);
    EXPECT_EQ(rt->state(), GpuRuntimeState::Ready);
    EXPECT_EQ(runFill(), 0u);
    EXPECT_EQ(rt->state(), GpuRuntimeState::Ready);
}

TEST_F(GpuRuntimeTest, InjectedLossLatchesAndFailsFast)
{
    // Operation 1 is the upload's submission.
    GpuStatus s = make({ GpuFaultPlan::Kind::DeviceLost, 1 });
    GPUTEST_DEVICE_OR_SKIP(s.ok(), s.message);

    GpuStatus failure;
    EXPECT_EQ(runFill(&failure), ~size_t(0));
    EXPECT_EQ(failure.result, VK_ERROR_DEVICE_LOST);
    EXPECT_EQ(rt->state(), GpuRuntimeState::Lost);

    GpuBufferHandle h;
    EXPECT_EQ(rt->acquire(kBytes, h).result, VK_ERROR_DEVICE_LOST);
    std::unique_ptr<GpuRecordContext> ctx;
    EXPECT_EQ(rt->newRecordContext(ctx).result, VK_ERROR_DEVICE_LOST);
    EXPECT_EQ(rt->waitIdle().result, VK_ERROR_DEVICE_LOST);
    EXPECT_EQ(rt->wait({}).result, VK_ERROR_DEVICE_LOST);
    EXPECT_EQ(rt->state(), GpuRuntimeState::Lost);
}

TEST_F(GpuRuntimeTest, RecreateReturnsToReadyAndDispatches)
{
    GpuStatus s = make({ GpuFaultPlan::Kind::DeviceLost, 1 });
    GPUTEST_DEVICE_OR_SKIP(s.ok(), s.message);
    EXPECT_NE(runFill(), 0u);
    ASSERT_EQ(rt->state(), GpuRuntimeState::Lost);

    s = rt->recreate();
    ASSERT_TRUE(s.ok()) << s.message;
    EXPECT_EQ(rt->state(), GpuRuntimeState::Ready);
    EXPECT_EQ(rt->leakedAllocationsAtLastTeardown(), 0u);
    // The fault is one-shot and the registered fill kernel survives under its old id.
    EXPECT_EQ(runFill(), 0u);
    EXPECT_EQ(rt->state(), GpuRuntimeState::Ready);
    EXPECT_GE(rt->registry().creationCount(fillId), 1u);
}

TEST_F(GpuRuntimeTest, TimedOutSubmissionIsALossThatReturnsWithoutWaitingForItsWork)
{
    GpuStatus s = make();
    GPUTEST_DEVICE_OR_SKIP(s.ok(), s.message);

    constexpr uint32_t side = 1024;
    constexpr VkDeviceSize bytes = VkDeviceSize(side) * side * sizeof(uint32_t);
    GpuBuffer callerOwned;
    ASSERT_TRUE(GpuBuffer::createDeviceLocal(rt->device(), bytes, kGpuBufferDefaultUsage, callerOwned).ok());
    std::unique_ptr<GpuRecordContext> ctx;
    ASSERT_TRUE(rt->newRecordContext(ctx).ok());
    GpuBuffer* scratch = nullptr;
    ASSERT_TRUE(ctx->scratch(bytes, scratch).ok());
    const FillParams p { side, side, 0 };
    // Enough work that a zero timeout finds it still running.
    for (int i = 0; i < 512; ++i) {
        GpuBuffer* target = (i % 2) ? scratch : &callerOwned;
        ASSERT_TRUE(ctx->dispatch(fillId, { target }, p, { side, side, 1 }).ok());
        ctx->computeBarrier();
    }
    const auto begin = std::chrono::steady_clock::now();
    s = ctx->submitAndWait(0);
    ASSERT_EQ(s.result, VK_ERROR_DEVICE_LOST) << s.message;
    EXPECT_EQ(rt->state(), GpuRuntimeState::Lost);
    EXPECT_TRUE(rt->device().hasAbandonedWork());
    // The work may still be running: the context and the caller's buffer are released without freeing what it uses.
    ctx.reset();
    callerOwned.reset();

    // A render meets these first and falls back to the CPU.
    GpuBufferHandle h;
    EXPECT_EQ(rt->acquire(kBytes, h).result, VK_ERROR_DEVICE_LOST);
    EXPECT_EQ(rt->newRecordContext(ctx).result, VK_ERROR_DEVICE_LOST);
    GpuStatus failure;
    EXPECT_EQ(runFill(&failure), ~size_t(0));
    EXPECT_EQ(failure.result, VK_ERROR_DEVICE_LOST);

    EXPECT_LT(std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count(), 20.);
    // Recreating would leak another device per retry if the work hangs every time.
    EXPECT_FALSE(rt->recreate().ok());
    EXPECT_EQ(rt->state(), GpuRuntimeState::Lost);
    EXPECT_TRUE(rt->device().hasAbandonedWork());
    EXPECT_FALSE(rt->recreate().ok());
}

TEST_F(GpuRuntimeTest, RecreateFromReadyHasNoLeaks)
{
    GpuStatus s = make();
    GPUTEST_DEVICE_OR_SKIP(s.ok(), s.message);
    EXPECT_EQ(runFill(), 0u);
    ASSERT_TRUE(rt->recreate().ok());
    EXPECT_EQ(rt->leakedAllocationsAtLastTeardown(), 0u);
    EXPECT_EQ(runFill(), 0u);
    ASSERT_TRUE(rt->recreate().ok());
    EXPECT_EQ(rt->leakedAllocationsAtLastTeardown(), 0u);
}

TEST_F(GpuRuntimeTest, InjectedOomDegradesAndShrinksBudget)
{
    GpuStatus s = make({ GpuFaultPlan::Kind::OutOfMemory, 1 });
    GPUTEST_DEVICE_OR_SKIP(s.ok(), s.message);
    const uint64_t before = rt->pool().budget();
    constexpr VkDeviceSize request = 1 << 20;
    ASSERT_GT(before, 2 * request);

    GpuBufferHandle h;
    s = rt->acquire(request, h);
    EXPECT_EQ(s.result, VK_ERROR_OUT_OF_DEVICE_MEMORY);
    EXPECT_EQ(rt->state(), GpuRuntimeState::Degraded);
    EXPECT_EQ(rt->pool().budget(), before - request);

    // Degraded still serves requests that fit.
    EXPECT_EQ(runFill(), 0u);
    EXPECT_EQ(rt->state(), GpuRuntimeState::Degraded);

    ASSERT_TRUE(rt->recreate().ok());
    EXPECT_EQ(rt->state(), GpuRuntimeState::Ready);
    EXPECT_EQ(rt->pool().budget(), before);
}

TEST_F(GpuRuntimeTest, InjectedFaultCountsFromNow)
{
    GpuStatus s = make();
    GPUTEST_DEVICE_OR_SKIP(s.ok(), s.message);
    EXPECT_EQ(runFill(), 0u);

    rt->device().injectFault(GpuFaultPlan::Kind::OutOfMemory, 2);
    GpuBufferHandle a, b, c;
    EXPECT_TRUE(rt->acquire(kBytes, a).ok());
    EXPECT_EQ(rt->acquire(kBytes, b).result, VK_ERROR_OUT_OF_DEVICE_MEMORY);
    EXPECT_TRUE(rt->acquire(kBytes, c).ok());
    EXPECT_EQ(rt->state(), GpuRuntimeState::Degraded);

    rt->device().injectFault(GpuFaultPlan::Kind::DeviceLost, 1);
    rt->device().injectFault(GpuFaultPlan::Kind::Off, 0);
    EXPECT_EQ(runFill(), 0u);
    EXPECT_EQ(rt->state(), GpuRuntimeState::Degraded);
}

TEST_F(GpuRuntimeTest, RealPoolOomDegrades)
{
    GpuStatus s = make();
    GPUTEST_DEVICE_OR_SKIP(s.ok(), s.message);
    GpuBufferHandle h;
    const uint64_t before = rt->pool().budget();
    s = rt->acquire(before + (VkDeviceSize(1) << 20), h);
    EXPECT_EQ(s.result, VK_ERROR_OUT_OF_DEVICE_MEMORY);
    EXPECT_EQ(rt->state(), GpuRuntimeState::Degraded);
}

TEST_F(GpuRuntimeTest, EnvironmentVariableInjectsFault)
{
    ASSERT_EQ(setenv("NATRON_GPU_FAULT", "lost@1", 1), 0);
    GpuStatus s = make();
    unsetenv("NATRON_GPU_FAULT");
    GPUTEST_DEVICE_OR_SKIP(s.ok(), s.message);
    EXPECT_NE(runFill(), 0u);
    EXPECT_EQ(rt->state(), GpuRuntimeState::Lost);
}

TEST_F(GpuRuntimeTest, MalformedEnvironmentVariableIsRejected)
{
    ASSERT_EQ(setenv("NATRON_GPU_FAULT", "bogus", 1), 0);
    GpuStatus s = make();
    unsetenv("NATRON_GPU_FAULT");
    EXPECT_FALSE(s.ok());
}

TEST_F(GpuRuntimeTest, RuntimeShaderDispatchesThroughARecordContextAcrossRecreate)
{
    GpuStatus s = make();
    GPUTEST_DEVICE_OR_SKIP(s.ok(), s.message);
    const std::string source = "#version 460\n// GpuRuntimeTest " + std::to_string(::getpid()) + "\n"
                                                                                                 "layout(local_size_x = 8, local_size_y = 8) in;\n"
                                                                                                 "layout(std430, set = 0, binding = 0) writeonly buffer Out { uint words[]; };\n"
                                                                                                 "layout(push_constant) uniform Push { uint width; uint height; } pc;\n"
                                                                                                 "void main() {\n"
                                                                                                 "    if (gl_GlobalInvocationID.x >= pc.width || gl_GlobalInvocationID.y >= pc.height) return;\n"
                                                                                                 "    uint i = gl_GlobalInvocationID.y * pc.width + gl_GlobalInvocationID.x;\n"
                                                                                                 "    words[i] = i;\n"
                                                                                                 "}\n";
    for (int round = 0; round < 2; ++round) {
        std::shared_ptr<GpuKernel> kernel;
        s = rt->pipelines().get(source, ShaderStage::Compute, "main", kernel);
        ASSERT_TRUE(s.ok()) << s.message;
        const GpuPipelineCache::Stats stats = rt->pipelines().stats();
        EXPECT_EQ(stats.memoryHits, 0u) << "round " << round;
        EXPECT_EQ(stats.compiles + stats.diskHits, 1u) << "round " << round;
        if (round == 1) {
            EXPECT_EQ(stats.diskHits, 1u) << "the rebuilt cache keeps the disk entries";
        }

        GpuBufferHandle h;
        ASSERT_TRUE(rt->acquire(kBytes, h).ok());
        std::unique_ptr<GpuRecordContext> ctx;
        ASSERT_TRUE(rt->newRecordContext(ctx).ok());
        const GpuBinding binding = GpuBinding::storage(0, h.buffer());
        const uint32_t push[2] = { kW, kH };
        s = ctx->dispatchBindings(kernel, std::span<const GpuBinding>(&binding, 1), std::as_bytes(std::span<const uint32_t>(push, 2)),
                                  { kW, kH, 1 });
        ASSERT_TRUE(s.ok()) << s.message;
        // The context holds the kernel, so the recorded work outlives the cache's and the caller's references.
        kernel.reset();
        rt->pipelines().clearMemory();
        ctx->computeBarrier();
        s = ctx->submitAndWait();
        ASSERT_TRUE(s.ok()) << s.message;
        ctx.reset();

        std::vector<uint32_t> host(kW * kH, ~0u);
        TimelineValue down;
        s = rt->download(h.buffer(), 0, { host.data(), kBytes }, down);
        if (s) {
            s = rt->wait(down);
        }
        ASSERT_TRUE(s.ok()) << s.message;
        size_t bad = 0;
        for (size_t i = 0; i < host.size(); ++i) {
            bad += host[i] != i;
        }
        EXPECT_EQ(bad, 0u) << "round " << round;
        h = GpuBufferHandle();

        if (round == 0) {
            ASSERT_TRUE(rt->recreate().ok());
            EXPECT_EQ(rt->leakedAllocationsAtLastTeardown(), 0u);
        }
    }
}
