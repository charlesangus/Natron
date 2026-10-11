#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "GpuBuffer.h"
#include "GpuDevice.h"
#include "GpuTestDevice.h"

using namespace gpu;

namespace {

class GpuBufferTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        GpuStatus s = gputest::createDevice(dev);
        GPUTEST_DEVICE_OR_SKIP(s.ok(), s.message);
    }

    void TearDown() override
    {
        dev.reset();
        check.expectClean("the test");
    }

    gputest::ValidationCheck check;
    std::unique_ptr<GpuDevice> dev;
};

struct AlignedBlock {
    AlignedBlock(size_t alignment, size_t bytes)
        : ptr(std::aligned_alloc(alignment, bytes))
    {
    }
    ~AlignedBlock() { std::free(ptr); }
    void* ptr;
};

} // namespace

TEST_F(GpuBufferTest, DeviceInfoLimitsAreReported)
{
    const GpuDeviceInfo& i = dev->info();
    EXPECT_GT(i.maxStorageBufferRange, 0u);
    EXPECT_GT(i.maxMemoryAllocationSize, 0u);
    EXPECT_GT(i.deviceLocalHeapSize, 0u);
    EXPECT_EQ(i.copyChunkSize, 512ull << 20);
    if (i.externalMemoryHost) {
        EXPECT_GT(i.minImportedHostPointerAlignment, 0u);
    } else {
        EXPECT_EQ(i.minImportedHostPointerAlignment, 0u);
    }
    if (i.type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
        EXPECT_TRUE(i.unifiedMemory);
    }
}

TEST(GpuBufferOptionsTest, OverridesReadBackThroughDeviceInfo)
{
    std::unique_ptr<GpuDevice> baseline;
    GpuStatus s = gputest::createDevice(baseline);
    GPUTEST_DEVICE_OR_SKIP(s.ok(), s.message);
    const uint64_t limit = baseline->info().maxStorageBufferRange;
    baseline.reset();

    gpu::GpuDeviceOptions o = gputest::deviceOptions();
    o.maxStorageBufferRange = 65536;
    o.copyChunkSize = 4096;
    std::unique_ptr<GpuDevice> dev;
    s = gputest::createDevice(dev, o);
    ASSERT_TRUE(s.ok()) << s.message;
    EXPECT_EQ(dev->info().maxStorageBufferRange, std::min<uint64_t>(limit, 65536));
    EXPECT_EQ(dev->info().copyChunkSize, 4096u);

    o.maxStorageBufferRange = limit + 1;
    s = gputest::createDevice(dev, o);
    ASSERT_TRUE(s.ok()) << s.message;
    EXPECT_EQ(dev->info().maxStorageBufferRange, limit);
}

TEST_F(GpuBufferTest, DeviceLocalCreateMoveDestroy)
{
    GpuBuffer b;
    GpuStatus s = GpuBuffer::createDeviceLocal(*dev, 1 << 20, kGpuBufferDefaultUsage, b);
    ASSERT_TRUE(s.ok()) << s.message;
    EXPECT_NE(b.buffer(), static_cast<VkBuffer>(VK_NULL_HANDLE));
    EXPECT_EQ(b.kind(), GpuBufferKind::DeviceLocal);
    EXPECT_EQ(b.size(), 1u << 20);

    const VkBuffer handle = b.buffer();
    GpuBuffer moved(std::move(b));
    EXPECT_EQ(b.buffer(), static_cast<VkBuffer>(VK_NULL_HANDLE));
    EXPECT_EQ(moved.buffer(), handle);
    moved.reset();
    EXPECT_EQ(moved.buffer(), static_cast<VkBuffer>(VK_NULL_HANDLE));
}

TEST_F(GpuBufferTest, ZeroSizeIsRejected)
{
    GpuBuffer b;
    EXPECT_FALSE(GpuBuffer::createDeviceLocal(*dev, 0, kGpuBufferDefaultUsage, b).ok());
    EXPECT_EQ(b.buffer(), static_cast<VkBuffer>(VK_NULL_HANDLE));
}

TEST_F(GpuBufferTest, StagingIsMapped)
{
    GpuBuffer b;
    GpuStatus s = GpuBuffer::createStaging(*dev, 4096, kGpuBufferDefaultUsage, b);
    ASSERT_TRUE(s.ok()) << s.message;
    EXPECT_EQ(b.kind(), GpuBufferKind::Staging);
    ASSERT_NE(b.mapped(), nullptr);
    std::memset(b.mapped(), 0xab, 4096);
    EXPECT_EQ(static_cast<uint8_t*>(b.mapped())[4095], 0xab);
}

TEST_F(GpuBufferTest, HostImportedWrapsCallerMemory)
{
    if (!dev->info().externalMemoryHost) {
        GPUTEST_SKIP() << "VK_EXT_external_memory_host is not available";
    }
    const size_t align = static_cast<size_t>(dev->info().minImportedHostPointerAlignment);
    const size_t bytes = align * 4;
    AlignedBlock block(align, bytes);
    ASSERT_NE(block.ptr, nullptr);

    GpuBuffer b;
    GpuStatus s = GpuBuffer::createHostImported(*dev, block.ptr, bytes, kGpuBufferDefaultUsage, b);
    if (!s.ok()) {
        EXPECT_TRUE(s.fallbackToStaging) << s.message;
        GPUTEST_SKIP() << "host import refused by the driver: " << s.message;
    }
    EXPECT_EQ(b.kind(), GpuBufferKind::HostImported);
    EXPECT_EQ(b.size(), bytes);
    EXPECT_EQ(b.mapped(), block.ptr);
}

TEST_F(GpuBufferTest, MisalignedHostImportReturnsStatus)
{
    if (!dev->info().externalMemoryHost) {
        GPUTEST_SKIP() << "VK_EXT_external_memory_host is not available";
    }
    const size_t align = static_cast<size_t>(dev->info().minImportedHostPointerAlignment);
    AlignedBlock block(align, align * 4);
    ASSERT_NE(block.ptr, nullptr);

    GpuBuffer b;
    GpuStatus s = GpuBuffer::createHostImported(*dev, static_cast<char*>(block.ptr) + 1, align * 2,
                                                kGpuBufferDefaultUsage, b);
    EXPECT_FALSE(s.ok());
    EXPECT_TRUE(s.fallbackToStaging);
    EXPECT_EQ(b.buffer(), static_cast<VkBuffer>(VK_NULL_HANDLE));

    s = GpuBuffer::createHostImported(*dev, block.ptr, align + 1, kGpuBufferDefaultUsage, b);
    EXPECT_FALSE(s.ok());
    EXPECT_TRUE(s.fallbackToStaging);
    EXPECT_EQ(b.buffer(), static_cast<VkBuffer>(VK_NULL_HANDLE));

    s = GpuBuffer::createHostImported(*dev, block.ptr, 4ull << 30, kGpuBufferDefaultUsage, b);
    EXPECT_FALSE(s.ok());
    EXPECT_TRUE(s.fallbackToStaging);
    EXPECT_EQ(b.buffer(), static_cast<VkBuffer>(VK_NULL_HANDLE));
}

TEST_F(GpuBufferTest, HostImportAboveMaxAllocationSizeIsRejected)
{
    if (!dev->info().externalMemoryHost) {
        GPUTEST_SKIP() << "VK_EXT_external_memory_host is not available";
    }
    const uint64_t align = std::max<uint64_t>(dev->info().minImportedHostPointerAlignment, 1);
    const uint64_t maxAlloc = dev->info().maxMemoryAllocationSize;
    if (maxAlloc == 0 || maxAlloc >= (1ull << 40)) {
        GPUTEST_SKIP() << "maxMemoryAllocationSize is too large to exceed in a test";
    }
    AlignedBlock block(static_cast<size_t>(align), static_cast<size_t>(align));
    ASSERT_NE(block.ptr, nullptr);

    // Rejection happens before any Vulkan call, so the oversized range is never touched.
    const uint64_t oversized = (maxAlloc / align + 1) * align;
    GpuBuffer b;
    GpuStatus s = GpuBuffer::createHostImported(*dev, block.ptr, oversized, kGpuBufferDefaultUsage, b);
    EXPECT_FALSE(s.ok());
    EXPECT_TRUE(s.fallbackToStaging);
    EXPECT_EQ(b.buffer(), static_cast<VkBuffer>(VK_NULL_HANDLE));
}

TEST_F(GpuBufferTest, ExportableCreateAndDestroy)
{
    if (!dev->info().externalMemoryFd) {
        GPUTEST_SKIP() << "VK_KHR_external_memory_fd is not available";
    }
    GpuBuffer b;
    GpuStatus s = GpuBuffer::createExportable(*dev, 1 << 16, kGpuBufferDefaultUsage, b);
    if (!s.ok()) {
        GPUTEST_SKIP() << "OPAQUE_FD export unsupported: " << s.message;
    }
    EXPECT_EQ(b.kind(), GpuBufferKind::Exportable);
    EXPECT_NE(b.memory(), static_cast<VkDeviceMemory>(VK_NULL_HANDLE));
    EXPECT_EQ(b.memoryOffset(), 0u);
    b.reset();
    EXPECT_EQ(b.buffer(), static_cast<VkBuffer>(VK_NULL_HANDLE));
}
