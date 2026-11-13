#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "GpuAccel.h"
#include "GpuBuffer.h"
#include "GpuBufferPool.h"
#include "GpuRecord.h"
#include "GpuTestDevice.h"

using namespace gpu;

namespace {

constexpr uint32_t kQuadVertices = 4;
constexpr uint32_t kQuadTriangles = 2;

// Owns the quad's buffers; host-visible so a test can move a point between builds.
struct Quad {
    GpuBuffer vertices;
    GpuBuffer indices;

    GpuStatus create(GpuDevice& dev, VkBufferUsageFlags usage)
    {
        if (GpuStatus s = GpuBuffer::createStaging(dev, kQuadVertices * 3 * sizeof(float), usage, vertices); !s) {
            return s;
        }
        if (GpuStatus s = GpuBuffer::createStaging(dev, kQuadTriangles * 3 * sizeof(uint32_t), usage, indices); !s) {
            return s;
        }
        const float points[kQuadVertices * 3] = { 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0 };
        const uint32_t tris[kQuadTriangles * 3] = { 0, 1, 2, 0, 2, 3 };
        std::memcpy(vertices.mapped(), points, sizeof(points));
        std::memcpy(indices.mapped(), tris, sizeof(tris));
        return {};
    }

    void moveVertex(uint32_t index, float x, float y, float z)
    {
        float* p = static_cast<float*>(vertices.mapped()) + index * 3;
        p[0] = x;
        p[1] = y;
        p[2] = z;
    }

    GpuTriangles triangles() const
    {
        GpuTriangles t;
        t.vertices = &vertices;
        t.vertexCount = kQuadVertices;
        t.indices = &indices;
        t.triangleCount = kQuadTriangles;
        return t;
    }
};

std::array<GpuAccelInstance, 2>
twoInstances(const GpuBlas& blas)
{
    std::array<GpuAccelInstance, 2> instances;
    instances[0].blas = &blas;
    instances[0].customIndex = 1;
    instances[1].blas = &blas;
    instances[1].customIndex = 0xFFFFFF;
    instances[1].mask = 0x0F;
    instances[1].transform[3] = 2.0f;
    return instances;
}

class GpuAccelTest : public gputest::SkippableTest {
protected:
    void SetUp() override
    {
        GpuStatus s = gputest::createDevice(dev);
        GPUTEST_DEVICE_OR_SKIP_TEST(s.ok(), s.message);
        if (!dev->supportsRayQuery()) {
            GPUTEST_SKIP_TEST() << dev->info().name
                                << " lacks VK_KHR_acceleration_structure, VK_KHR_ray_query, "
                                   "VK_KHR_deferred_host_operations or bufferDeviceAddress, so it has no "
                                   "acceleration structures to test";
        }
        registry = std::make_unique<GpuKernelRegistry>(*dev);
        pool = std::make_unique<GpuBufferPool>(*dev);
        s = quad.create(*dev, kGpuAccelInputUsage);
        ASSERT_TRUE(s.ok()) << s.message;
    }

    void TearDown() override
    {
        if (dev) {
            dev->waitIdle();
        }
        tlas = GpuTlas();
        blas = GpuBlas();
        quad = Quad();
        pool.reset();
        registry.reset();
        dev.reset();
        check.expectClean("the test");
    }

    std::unique_ptr<GpuRecordContext> context(GpuBufferPool& scratchPool)
    {
        std::unique_ptr<GpuRecordContext> ctx;
        GpuStatus s = GpuRecordContext::create(*registry, ctx, &scratchPool);
        EXPECT_TRUE(s.ok()) << s.message;
        return ctx;
    }

    gputest::ValidationCheck check;
    std::unique_ptr<GpuDevice> dev;
    std::unique_ptr<GpuKernelRegistry> registry;
    std::unique_ptr<GpuBufferPool> pool;
    Quad quad;
    GpuBlas blas;
    GpuTlas tlas;
};

} // namespace

GPU_TEST_F(GpuAccelTest, BuildsAQuadBlasAndATwoInstanceTlas)
{
    auto ctx = context(*pool);
    ASSERT_TRUE(ctx != nullptr);
    GpuStatus s = GpuBlas::build(*ctx, *pool, quad.triangles(), {}, blas);
    ASSERT_TRUE(s.ok()) << s.message;
    EXPECT_TRUE(static_cast<bool>(blas));
    EXPECT_NE(blas.deviceAddress(), 0u);
    EXPECT_GT(blas.size(), 0u);

    const auto instances = twoInstances(blas);
    s = GpuTlas::build(*ctx, *pool, instances, tlas);
    ASSERT_TRUE(s.ok()) << s.message;
    EXPECT_TRUE(static_cast<bool>(tlas));
    EXPECT_EQ(tlas.instanceCount(), 2u);
    EXPECT_NE(tlas.deviceAddress(), 0u);

    s = ctx->submitAndWait();
    EXPECT_TRUE(s.ok()) << s.message;
    EXPECT_FALSE(ctx->ranOutOfMemory());
}

GPU_TEST_F(GpuAccelTest, RejectsACustomIndexOver24Bits)
{
    auto ctx = context(*pool);
    ASSERT_TRUE(ctx != nullptr);
    GpuStatus s = GpuBlas::build(*ctx, *pool, quad.triangles(), {}, blas);
    ASSERT_TRUE(s.ok()) << s.message;
    auto instances = twoInstances(blas);
    instances[1].customIndex = 0x1000000;
    s = GpuTlas::build(*ctx, *pool, instances, tlas);
    EXPECT_EQ(s.result, VK_ERROR_INITIALIZATION_FAILED) << s.message;
    EXPECT_FALSE(static_cast<bool>(tlas));
    s = ctx->submitAndWait();
    EXPECT_TRUE(s.ok()) << s.message;
}

GPU_TEST_F(GpuAccelTest, RefitsAfterAVertexMoves)
{
    auto build = context(*pool);
    ASSERT_TRUE(build != nullptr);
    GpuStatus s = GpuBlas::build(*build, *pool, quad.triangles(), {}, blas);
    ASSERT_TRUE(s.ok()) << s.message;
    const auto instances = twoInstances(blas);
    s = GpuTlas::build(*build, *pool, instances, tlas);
    ASSERT_TRUE(s.ok()) << s.message;
    s = build->submitAndWait();
    ASSERT_TRUE(s.ok()) << s.message;

    quad.moveVertex(2, 1.5f, 1.25f, 0.5f);
    auto refit = context(*pool);
    ASSERT_TRUE(refit != nullptr);
    const VkDeviceAddress before = blas.deviceAddress();
    s = blas.refit(*refit, quad.triangles());
    ASSERT_TRUE(s.ok()) << s.message;
    EXPECT_EQ(blas.deviceAddress(), before);
    s = tlas.rebuild(*refit, *pool, instances);
    ASSERT_TRUE(s.ok()) << s.message;
    s = refit->submitAndWait();
    EXPECT_TRUE(s.ok()) << s.message;

    GpuTriangles fewer = quad.triangles();
    fewer.triangleCount = 1;
    auto mismatched = context(*pool);
    ASSERT_TRUE(mismatched != nullptr);
    s = blas.refit(*mismatched, fewer);
    EXPECT_EQ(s.result, VK_ERROR_INITIALIZATION_FAILED) << s.message;
    EXPECT_TRUE(mismatched->submitAndWait().ok());
}

GPU_TEST_F(GpuAccelTest, RefitNeedsAllowUpdate)
{
    GpuBlasOptions options;
    options.allowUpdate = false;
    auto build = context(*pool);
    ASSERT_TRUE(build != nullptr);
    GpuStatus s = GpuBlas::build(*build, *pool, quad.triangles(), options, blas);
    ASSERT_TRUE(s.ok()) << s.message;
    ASSERT_TRUE(build->submitAndWait().ok());

    auto refit = context(*pool);
    ASSERT_TRUE(refit != nullptr);
    s = blas.refit(*refit, quad.triangles());
    EXPECT_EQ(s.result, VK_ERROR_INITIALIZATION_FAILED) << s.message;
    EXPECT_TRUE(refit->submitAndWait().ok());
}

GPU_TEST_F(GpuAccelTest, CompactionShrinksOrKeepsTheSize)
{
    auto build = context(*pool);
    ASSERT_TRUE(build != nullptr);
    GpuStatus s = GpuBlas::build(*build, *pool, quad.triangles(), {}, blas);
    ASSERT_TRUE(s.ok()) << s.message;
    s = build->submitAndWait();
    ASSERT_TRUE(s.ok()) << s.message;

    const VkDeviceSize original = blas.size();
    VkDeviceSize compactedBytes = 0;
    s = blas.compactedSize(compactedBytes);
    ASSERT_TRUE(s.ok()) << s.message;
    std::printf("quad BLAS on %s: %llu bytes, %llu compacted\n", dev->info().name.c_str(),
                static_cast<unsigned long long>(original), static_cast<unsigned long long>(compactedBytes));
    EXPECT_GT(compactedBytes, 0u);
    EXPECT_LE(compactedBytes, original);

    auto compact = context(*pool);
    ASSERT_TRUE(compact != nullptr);
    s = blas.compact(*compact, *pool);
    ASSERT_TRUE(s.ok()) << s.message;
    EXPECT_EQ(blas.size(), compactedBytes);
    EXPECT_LE(blas.size(), original);
    const auto instances = twoInstances(blas);
    s = GpuTlas::build(*compact, *pool, instances, tlas);
    ASSERT_TRUE(s.ok()) << s.message;
    s = compact->submitAndWait();
    EXPECT_TRUE(s.ok()) << s.message;

    VkDeviceSize again = 0;
    EXPECT_FALSE(blas.compactedSize(again).ok());
}

GPU_TEST_F(GpuAccelTest, SizesOverTheBudgetReturnOutOfMemory)
{
    GpuBufferPoolOptions tiny;
    tiny.overrideBudgetBytes = 256;
    GpuBufferPool small(*dev, tiny);

    GpuAccelSizes sizes;
    GpuStatus s = GpuBlas::querySizes(*dev, quad.triangles(), {}, sizes);
    ASSERT_TRUE(s.ok()) << s.message;
    ASSERT_GT(sizes.structureBytes, small.budget());

    {
        auto ctx = context(small);
        ASSERT_TRUE(ctx != nullptr);
        s = GpuBlas::build(*ctx, small, quad.triangles(), {}, blas);
        EXPECT_EQ(s.result, VK_ERROR_OUT_OF_DEVICE_MEMORY) << s.message;
        EXPECT_FALSE(static_cast<bool>(blas));
        EXPECT_TRUE(ctx->ranOutOfMemory());
        EXPECT_TRUE(ctx->submitAndWait().ok());
    }

    auto build = context(*pool);
    ASSERT_TRUE(build != nullptr);
    s = GpuBlas::build(*build, *pool, quad.triangles(), {}, blas);
    ASSERT_TRUE(s.ok()) << s.message;
    ASSERT_TRUE(build->submitAndWait().ok());

    s = GpuTlas::querySizes(*dev, 2, sizes);
    ASSERT_TRUE(s.ok()) << s.message;
    ASSERT_GT(sizes.structureBytes, small.budget());
    auto ctx = context(small);
    ASSERT_TRUE(ctx != nullptr);
    const auto instances = twoInstances(blas);
    s = GpuTlas::build(*ctx, small, instances, tlas);
    EXPECT_EQ(s.result, VK_ERROR_OUT_OF_DEVICE_MEMORY) << s.message;
    EXPECT_FALSE(static_cast<bool>(tlas));
    EXPECT_TRUE(ctx->ranOutOfMemory());
    EXPECT_TRUE(ctx->submitAndWait().ok());
}

namespace {

class GpuAccelWithoutRayQueryTest : public gputest::SkippableTest {
protected:
    void SetUp() override
    {
        ASSERT_EQ(setenv("NATRON_GPU_NO_RAY_QUERY", "1", 1), 0);
        GpuStatus s = gputest::createDevice(dev);
        GPUTEST_DEVICE_OR_SKIP_TEST(s.ok(), s.message);
    }

    void TearDown() override
    {
        if (dev) {
            dev->waitIdle();
        }
        dev.reset();
        unsetenv("NATRON_GPU_NO_RAY_QUERY");
        check.expectClean("the test");
    }

    gputest::ValidationCheck check;
    std::unique_ptr<GpuDevice> dev;
};

} // namespace

GPU_TEST_F(GpuAccelWithoutRayQueryTest, EveryCallFailsCleanly)
{
    ASSERT_FALSE(dev->supportsRayQuery());
    {
        GpuKernelRegistry registry(*dev);
        GpuBufferPool pool(*dev);
        Quad quad;
        GpuStatus s = quad.create(*dev, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        ASSERT_TRUE(s.ok()) << s.message;

        GpuAccelSizes sizes;
        EXPECT_EQ(GpuBlas::querySizes(*dev, quad.triangles(), {}, sizes).result, VK_ERROR_FEATURE_NOT_PRESENT);
        EXPECT_EQ(GpuTlas::querySizes(*dev, 2, sizes).result, VK_ERROR_FEATURE_NOT_PRESENT);

        std::unique_ptr<GpuRecordContext> ctx;
        s = GpuRecordContext::create(registry, ctx, &pool);
        ASSERT_TRUE(s.ok()) << s.message;
        GpuBlas blas;
        EXPECT_EQ(GpuBlas::build(*ctx, pool, quad.triangles(), {}, blas).result, VK_ERROR_FEATURE_NOT_PRESENT);
        EXPECT_FALSE(static_cast<bool>(blas));
        EXPECT_EQ(blas.refit(*ctx, quad.triangles()).result, VK_ERROR_FEATURE_NOT_PRESENT);
        EXPECT_EQ(blas.compact(*ctx, pool).result, VK_ERROR_FEATURE_NOT_PRESENT);
        GpuTlas tlas;
        const std::array<GpuAccelInstance, 1> instances {};
        EXPECT_EQ(GpuTlas::build(*ctx, pool, instances, tlas).result, VK_ERROR_FEATURE_NOT_PRESENT);
        EXPECT_FALSE(static_cast<bool>(tlas));
        EXPECT_FALSE(ctx->ranOutOfMemory());
        s = ctx->submitAndWait();
        EXPECT_TRUE(s.ok()) << s.message;
    }
}
