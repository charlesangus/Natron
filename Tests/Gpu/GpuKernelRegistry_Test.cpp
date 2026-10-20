#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "GpuBuffer.h"
#include "GpuRecord.h"
#include "GpuTestDevice.h"
#include "GpuWrap.h"
#include "fill_spirv.h"

using namespace gpu;

namespace {

constexpr uint32_t kW = 129;
constexpr uint32_t kH = 67;
constexpr VkDeviceSize kBytes = VkDeviceSize(kW) * kH * sizeof(uint32_t);

struct FillParams {
    uint32_t width;
    uint32_t height;
    uint32_t asFloat;
};

class GpuKernelRegistryTest : public gputest::SkippableTest {
protected:
    void SetUp() override
    {
        GpuStatus s = gputest::createDevice(dev);
        GPUTEST_DEVICE_OR_SKIP_TEST(s.ok(), s.message);

        GpuKernelDesc d;
        d.spirv = fill_spirv;
        d.spirvWords = fill_spirv_words;
        d.entry = fill_entry;
        d.storageBufferCount = fill_storage_buffers;
        d.pushConstantBytes = fill_push_bytes;
        d.groupSize = { fill_group_size[0], fill_group_size[1], fill_group_size[2] };
        registry = std::make_unique<GpuKernelRegistry>(*dev);
        fillId = registry->registerKernel(d);
    }

    void TearDown() override
    {
        if (dev) {
            dev->waitIdle();
        }
        registry.reset();
        dev.reset();
        check.expectClean("the test");
    }

    // Runs the fill kernel through its own context and returns the number of wrong words.
    size_t runFill(uint32_t asFloat)
    {
        GpuBuffer buf;
        GpuStatus s = GpuBuffer::createStaging(*dev, kBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, buf);
        EXPECT_TRUE(s.ok()) << s.message;
        if (!s.ok()) {
            return ~size_t(0);
        }
        std::memset(buf.mapped(), 0xAB, kBytes);

        std::unique_ptr<GpuRecordContext> ctx;
        s = GpuRecordContext::create(*registry, ctx);
        EXPECT_TRUE(s.ok()) << s.message;
        if (!s.ok()) {
            return ~size_t(0);
        }
        const FillParams p { kW, kH, asFloat };
        s = ctx->dispatch(fillId, { &buf }, p, { kW, kH, 1 });
        EXPECT_TRUE(s.ok()) << s.message;
        ctx->computeBarrier();
        s = ctx->submitAndWait();
        EXPECT_TRUE(s.ok()) << s.message;
        if (!s.ok()) {
            return ~size_t(0);
        }

        const auto* out = static_cast<const uint32_t*>(buf.mapped());
        size_t bad = 0;
        for (uint32_t i = 0; i < kW * kH; ++i) {
            const uint32_t want = asFloat ? [&] {
                const float f = float(i);
                uint32_t bits;
                std::memcpy(&bits, &f, sizeof bits);
                return bits;
            }()
                                          : i;
            bad += out[i] != want;
        }
        return bad;
    }

    gputest::ValidationCheck check;
    std::unique_ptr<GpuDevice> dev;
    std::unique_ptr<GpuKernelRegistry> registry;
    GpuKernelId fillId {};
};

} // namespace

GPU_TEST_F(GpuKernelRegistryTest, LooksUpGeneratedAndRegisteredKernels)
{
    const GpuKernelDesc* blur = registry->desc(GpuKernelId::blur);
    ASSERT_NE(blur, nullptr);
    EXPECT_GT(blur->spirvWords, 0u);
    EXPECT_GT(blur->storageBufferCount, 0u);
    EXPECT_GT(blur->pushConstantBytes, 0u);

    const GpuKernelDesc* divide = registry->desc(GpuKernelId::wrap_divide);
    ASSERT_NE(divide, nullptr);
    EXPECT_EQ(divide->storageBufferCount, kGpuWrapBindings);
    EXPECT_EQ(divide->pushConstantBytes, sizeof(GpuWrapParams));

    const GpuKernelDesc* fill = registry->desc(fillId);
    ASSERT_NE(fill, nullptr);
    EXPECT_EQ(fill->storageBufferCount, 1u);
    EXPECT_EQ(fill->pushConstantBytes, sizeof(FillParams));
    EXPECT_EQ(fill->groupSize, (std::array<uint32_t, 3> { 16, 16, 1 }));

    EXPECT_EQ(registry->desc(static_cast<GpuKernelId>(static_cast<uint32_t>(fillId) + 1)), nullptr);

    GpuKernel* k = nullptr;
    ASSERT_TRUE(registry->get(GpuKernelId::blur, k).ok());
    ASSERT_NE(k, nullptr);
    EXPECT_EQ(&k->desc() == registry->desc(GpuKernelId::blur), false);
    EXPECT_EQ(k->desc().spirv, registry->desc(GpuKernelId::blur)->spirv);
    GpuKernel* again = nullptr;
    ASSERT_TRUE(registry->get(GpuKernelId::blur, again).ok());
    EXPECT_EQ(k, again);

    EXPECT_FALSE(registry->get(static_cast<GpuKernelId>(static_cast<uint32_t>(fillId) + 1), k).ok());
}

GPU_TEST_F(GpuKernelRegistryTest, CreatesEachPipelineOnceAcrossThreadsAndAgainAfterInvalidate)
{
    std::vector<GpuKernelId> ids;
    for (uint32_t i = 0; i < static_cast<uint32_t>(GpuKernelId::Count); ++i) {
        ids.push_back(static_cast<GpuKernelId>(i));
    }
    ids.push_back(fillId);

    const auto hammer = [&] {
        std::atomic<int> failures { 0 };
        std::atomic<bool> go { false };
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&, t] {
                while (!go.load()) {
                }
                for (size_t n = 0; n < ids.size(); ++n) {
                    GpuKernel* k = nullptr;
                    if (!registry->get(ids[(n + t) % ids.size()], k).ok() || !k) {
                        ++failures;
                    }
                }
            });
        }
        go = true;
        for (std::thread& th : threads) {
            th.join();
        }
        EXPECT_EQ(failures.load(), 0);
    };

    hammer();
    for (GpuKernelId id : ids) {
        EXPECT_EQ(registry->creationCount(id), 1u) << "kernel " << static_cast<uint32_t>(id);
    }

    registry->invalidate();
    hammer();
    for (GpuKernelId id : ids) {
        EXPECT_EQ(registry->creationCount(id), 2u) << "kernel " << static_cast<uint32_t>(id);
    }
}

GPU_TEST_F(GpuKernelRegistryTest, FillThroughRecordContextUint)
{
    EXPECT_EQ(runFill(0), 0u);
}

GPU_TEST_F(GpuKernelRegistryTest, FillThroughRecordContextFloat)
{
    EXPECT_EQ(runFill(1), 0u);
}

GPU_TEST_F(GpuKernelRegistryTest, ConcurrentContextsEachRecordOnTheirOwnThread)
{
    std::atomic<size_t> bad { 0 };
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 20; ++i) {
                bad += runFill(i & 1);
            }
        });
    }
    for (std::thread& th : threads) {
        th.join();
    }
    EXPECT_EQ(bad.load(), 0u);
}

GPU_TEST_F(GpuKernelRegistryTest, RejectsWrongBindingCountAndSubmittedContext)
{
    std::unique_ptr<GpuRecordContext> ctx;
    ASSERT_TRUE(GpuRecordContext::create(*registry, ctx).ok());
    const FillParams p { kW, kH, 0 };
    EXPECT_FALSE(ctx->dispatch(fillId, std::span<const GpuBuffer* const>(), std::as_bytes(std::span(&p, 1)), { 1, 1, 1 }).ok());
    ASSERT_TRUE(ctx->submitAndWait().ok());
    GpuBuffer buf;
    ASSERT_TRUE(GpuBuffer::createStaging(*dev, kBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, buf).ok());
    EXPECT_FALSE(ctx->dispatch(fillId, { &buf }, p, { 1, 1, 1 }).ok());
}
