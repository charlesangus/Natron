#pragma once

#include "GpuDevice.h"

#include <array>
#include <cstddef>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace gpu {

// Compute pipeline over an embedded SPIR-V blob.
//
// Kernel convention: every resource is a storage buffer in descriptor set 0,
// bindings 0..N-1 in declaration order; parameters are one push-constant block
// declared `[[vk::push_constant]] ConstantBuffer<P> params;` (at most
// kMaxPushConstantBytes). Uniform-buffer bindings are not supported.
struct GpuKernelDesc {
    const uint32_t* spirv = nullptr;
    size_t spirvWords = 0;
    const char* entry = "main";
    uint32_t storageBufferCount = 0;
    uint32_t pushConstantBytes = 0;
    // numthreads of the entry point, from the generated <name>_group_size.
    std::array<uint32_t, 3> groupSize { 1, 1, 1 };
};

class GpuKernel {
public:
    static constexpr uint32_t kMaxPushConstantBytes = 128;

    static GpuStatus create(GpuDevice& device, const GpuKernelDesc& desc,
                            std::unique_ptr<GpuKernel>& out);
    ~GpuKernel();
    GpuKernel(const GpuKernel&) = delete;
    GpuKernel& operator=(const GpuKernel&) = delete;

    // Records bind + push constants + dispatch covering threads.x*y*z invocations.
    // `buffers` must hold storageBufferCount entries; `push` must be
    // pushConstantBytes long. When the device lacks push descriptors the
    // descriptor sets come from a pool that releaseDescriptors() recycles once
    // no recorded work is pending.
    GpuStatus record(VkCommandBuffer cmd, std::span<const VkBuffer> buffers,
                     std::span<const std::byte> push, std::array<uint32_t, 3> threads);

    // As record(), for kernels whose groups do not map one-to-one onto a thread grid.
    GpuStatus recordGroups(VkCommandBuffer cmd, std::span<const VkBuffer> buffers,
                           std::span<const std::byte> push, std::array<uint32_t, 3> groups);

    void releaseDescriptors();

    std::array<uint32_t, 3> groupCount(std::array<uint32_t, 3> threads) const;
    const GpuKernelDesc& desc() const { return desc_; }

private:
    GpuKernel(GpuDevice& device, const GpuKernelDesc& desc)
        : device_(device)
        , desc_(desc)
    {
    }

    GpuDevice& device_;
    GpuKernelDesc desc_;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    PFN_vkCmdPushDescriptorSetKHR pushDescriptorFn_ = nullptr;
    std::array<uint32_t, 3> maxGroups_ {};

    std::mutex poolMutex_;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
};

// Two timestamps bracketing recorded work. supported() is false when the
// compute queue family reports zero valid timestamp bits.
class GpuTimer {
public:
    static GpuStatus create(GpuDevice& device, std::unique_ptr<GpuTimer>& out);
    ~GpuTimer();
    GpuTimer(const GpuTimer&) = delete;
    GpuTimer& operator=(const GpuTimer&) = delete;

    bool supported() const { return validBits_ != 0; }

    // Resets the pool and writes the start stamp; no-ops when unsupported.
    void begin(VkCommandBuffer cmd);
    void end(VkCommandBuffer cmd);

    // Elapsed nanoseconds between begin and end; call after the work completed.
    GpuStatus elapsedNs(double& ns);

private:
    GpuTimer(GpuDevice& device)
        : device_(device)
    {
    }

    GpuDevice& device_;
    VkQueryPool pool_ = VK_NULL_HANDLE;
    uint32_t validBits_ = 0;
    double periodNs_ = 1.0;
};

} // namespace gpu
