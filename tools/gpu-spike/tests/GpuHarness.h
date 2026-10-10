#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "GpuKernel.h"
#include "GpuTestDevice.h"
#include "GpuTransfer.h"

// Device setup and dispatch plumbing shared by the GPU correctness tests. The
// device is picked the usual way (NATRON_GPU_DEVICE, else ranked), so the same
// binary runs on lavapipe and on RADV.
namespace gputest {

class GpuSuite : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        suiteCheck_ = ValidationCheck();
        gpu::GpuStatus s = createDevice(dev_);
        if (!s) {
            setupError_ = s.message;
            return;
        }
        VkPhysicalDeviceDriverProperties driver { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
        VkPhysicalDeviceProperties2 props { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver };
        vkGetPhysicalDeviceProperties2(dev_->physicalDevice(), &props);
        std::printf("driver: %s %s | type=%d\n", driver.driverName, driver.driverInfo, int(dev_->info().type));
    }

    static void TearDownTestSuite()
    {
        dev_.reset();
        suiteCheck_.expectClean("device teardown");
    }

    void SetUp() override
    {
        suiteCheck_.expectClean("device setup");
        ASSERT_TRUE(dev_) << setupError_;
    }

    void TearDown() override
    {
        if (dev_) {
            dev_->waitIdle();
        }
        suiteCheck_.expectClean("the test");
    }

    static std::unique_ptr<gpu::GpuKernel> makeKernel(const uint32_t* spirv, size_t words, const char* entry,
                                                      uint32_t buffers, uint32_t pushBytes,
                                                      const uint32_t (&groupSize)[3])
    {
        gpu::GpuKernelDesc d;
        d.spirv = spirv;
        d.spirvWords = words;
        d.entry = entry;
        d.storageBufferCount = buffers;
        d.pushConstantBytes = pushBytes;
        d.groupSize = { groupSize[0], groupSize[1], groupSize[2] };
        std::unique_ptr<gpu::GpuKernel> k;
        gpu::GpuStatus s = gpu::GpuKernel::create(*dev_, d, k);
        EXPECT_TRUE(s.ok()) << s.message << " (VkResult " << s.result << ")";
        return k;
    }

    static std::unique_ptr<gpu::GpuTransfer> makeTransfer()
    {
        gpu::GpuTransferOptions o;
        o.allowHostImport = false;
        o.stripBytes = 1 << 20;
        std::unique_ptr<gpu::GpuTransfer> t;
        gpu::GpuStatus s = gpu::GpuTransfer::create(*dev_, o, t);
        EXPECT_TRUE(s.ok()) << s.message;
        return t;
    }

    // Storage buffer. With `data` it lives in host-visible memory and is filled
    // once; without, it is device-local scratch for work between passes.
    struct Buffer {
        Buffer() = default;
        Buffer(Buffer&& o) noexcept { *this = std::move(o); }
        Buffer& operator=(Buffer&& o) noexcept
        {
            reset();
            dev = o.dev;
            buffer = std::exchange(o.buffer, VK_NULL_HANDLE);
            alloc = std::exchange(o.alloc, nullptr);
            return *this;
        }
        ~Buffer() { reset(); }

        void reset()
        {
            if (buffer)
                vmaDestroyBuffer(dev->allocator(), buffer, alloc);
            buffer = VK_NULL_HANDLE;
            alloc = nullptr;
        }

        gpu::GpuDevice* dev = nullptr;
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation alloc = nullptr;
    };

    static Buffer makeStorage(VkDeviceSize bytes, const void* data = nullptr)
    {
        VkBufferCreateInfo bi { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bi.size = std::max<VkDeviceSize>(bytes, 4);
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VmaAllocationCreateInfo ai {};
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        if (data)
            ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        Buffer b;
        b.dev = dev_.get();
        VmaAllocationInfo info {};
        EXPECT_EQ(vmaCreateBuffer(dev_->allocator(), &bi, &ai, &b.buffer, &b.alloc, &info), VK_SUCCESS);
        if (data && info.pMappedData) {
            std::memcpy(info.pMappedData, data, bytes);
            vmaFlushAllocation(dev_->allocator(), b.alloc, 0, VK_WHOLE_SIZE);
        }
        return b;
    }

    // Compute write -> compute read, for dependent passes inside one callback.
    static void computeBarrier(VkCommandBuffer cmd)
    {
        VkMemoryBarrier2 mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        VkDependencyInfo dep { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    // Streams frames through the transfer path in batches small enough for the
    // kernel's descriptor pool; `record` sees the frame index within `frames`.
    static void process(gpu::GpuTransfer& transfer, gpu::GpuKernel& kernel,
                        std::span<const gpu::TransferFrame> frames, size_t batch,
                        const std::function<gpu::GpuStatus(VkCommandBuffer, const gpu::ComputeBinding&, size_t)>& record)
    {
        for (size_t first = 0; first < frames.size(); first += batch) {
            const size_t n = std::min(batch, frames.size() - first);
            gpu::GpuStatus s = transfer.process(
                frames.subspan(first, n),
                [&](VkCommandBuffer cmd, const gpu::ComputeBinding& b) { return record(cmd, b, first + b.frameIndex); });
            ASSERT_TRUE(s.ok()) << s.message << " (VkResult " << s.result << ")";
            kernel.releaseDescriptors();
        }
    }

    static std::unique_ptr<gpu::GpuDevice> dev_;
    static std::string setupError_;
    static ValidationCheck suiteCheck_;
};

inline std::unique_ptr<gpu::GpuDevice> GpuSuite::dev_;
inline std::string GpuSuite::setupError_;
inline ValidationCheck GpuSuite::suiteCheck_;

} // namespace gputest
