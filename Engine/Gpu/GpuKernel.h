#pragma once

#include "GpuDevice.h"

#include <array>
#include <cstddef>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gpu {

// Compute pipeline over an embedded SPIR-V blob.
//
// Offline kernel convention: every resource is a storage buffer in descriptor
// set 0, bindings 0..N-1 in declaration order; parameters are one push-constant
// block declared `[[vk::push_constant]] ConstantBuffer<P> params;` (at most
// kMaxPushConstantBytes).
//
// A reflected kernel (`reflect` set, typically compiled at runtime) instead takes
// its layout from the module: storage buffers, uniform buffers, combined image
// samplers, sampled images and samplers in set 0, at most one push-constant block,
// and the entry point's local size. storageBufferCount, pushConstantBytes and
// groupSize are then ignored on input.
struct GpuKernelDesc {
    const uint32_t* spirv = nullptr;
    size_t spirvWords = 0;
    const char* entry = "main";
    uint32_t storageBufferCount = 0;
    uint32_t pushConstantBytes = 0;
    // numthreads of the entry point, from the generated <name>_group_size.
    std::array<uint32_t, 3> groupSize { 1, 1, 1 };
    bool reflect = false;
};

class GpuBuffer;
class GpuTexture;

// One descriptor of a kernel's set 0.
struct GpuKernelBinding {
    uint32_t binding = 0;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    // The variable name, or the block type name of an unnamed block; empty for offline kernels.
    std::string name;
    // Bytes the uniform block declares; 0 for other types.
    uint32_t blockBytes = 0;
    // The image dimensionality for COMBINED_IMAGE_SAMPLER and SAMPLED_IMAGE bindings.
    VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_MAX_ENUM;
};

// Host-visible uniform buffer filled once at creation. Move-only; it must outlive the
// recorded work that reads it and be destroyed before its GpuDevice.
class GpuUniformBuffer {
public:
    GpuUniformBuffer() = default;
    ~GpuUniformBuffer() { reset(); }
    GpuUniformBuffer(GpuUniformBuffer&& other) noexcept { moveFrom(other); }
    GpuUniformBuffer& operator=(GpuUniformBuffer&& other) noexcept
    {
        if (this != &other) {
            reset();
            moveFrom(other);
        }
        return *this;
    }
    GpuUniformBuffer(const GpuUniformBuffer&) = delete;
    GpuUniformBuffer& operator=(const GpuUniformBuffer&) = delete;

    // `data` must be non-empty and at most the device's maxUniformBufferRange.
    static GpuStatus create(GpuDevice& device, std::span<const std::byte> data, GpuUniformBuffer& out);

    VkBuffer buffer() const { return buffer_; }
    VkDeviceSize size() const { return size_; }
    explicit operator bool() const { return buffer_ != VK_NULL_HANDLE; }

    void reset();

private:
    void moveFrom(GpuUniformBuffer& other) noexcept;

    GpuDevice* device_ = nullptr;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = nullptr;
    VkDeviceSize size_ = 0;
};

// A resource for one binding of set 0, for GpuKernel::recordBindings. The handles are
// borrowed: the resource must outlive the recorded work.
struct GpuBinding {
    enum class Kind {
        StorageBuffer,
        UniformBuffer,
        Texture,
    };

    uint32_t binding = 0;
    Kind kind = Kind::StorageBuffer;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize bufferSize = 0;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_MAX_ENUM;

    static GpuBinding storage(uint32_t binding, const GpuBuffer& buffer);
    static GpuBinding uniform(uint32_t binding, const GpuUniformBuffer& buffer);
    // Fills a combined image sampler, a sampled image or a sampler binding, whichever the kernel declares.
    static GpuBinding texture(uint32_t binding, const GpuTexture& texture);
};

class GpuKernel {
public:
    static constexpr uint32_t kMaxPushConstantBytes = 128;

    // Builds the layout and the pipeline. For a reflected description it fails, without
    // creating anything, on a module it cannot describe: an unknown entry point, a
    // descriptor outside set 0, a descriptor array, a descriptor type other than those
    // listed on GpuKernelDesc, a non-1D/2D/3D or arrayed or multisampled image, more
    // than one push-constant block or one above kMaxPushConstantBytes, or a local size
    // given by specialization constants.
    static GpuStatus create(GpuDevice& device, const GpuKernelDesc& desc,
                            std::unique_ptr<GpuKernel>& out);

    // A reflected kernel over `spirv`, e.g. the output of compileGlsl. The words are only
    // read during the call.
    static GpuStatus createFromSpirv(GpuDevice& device, std::span<const uint32_t> spirv,
                                     const char* entry, std::unique_ptr<GpuKernel>& out);
    ~GpuKernel();
    GpuKernel(const GpuKernel&) = delete;
    GpuKernel& operator=(const GpuKernel&) = delete;

    // Records bind + push constants + dispatch covering threads.x*y*z invocations.
    // `buffers` must hold storageBufferCount entries; `push` must be
    // pushConstantBytes long. When the device lacks push descriptors the
    // descriptor sets come from a pool that releaseDescriptors() recycles once
    // no recorded work is pending. Reflected kernels are bound with recordBindings().
    GpuStatus record(VkCommandBuffer cmd, std::span<const VkBuffer> buffers,
                     std::span<const std::byte> push, std::array<uint32_t, 3> threads);

    // As record(), for kernels whose groups do not map one-to-one onto a thread grid.
    GpuStatus recordGroups(VkCommandBuffer cmd, std::span<const VkBuffer> buffers,
                           std::span<const std::byte> push, std::array<uint32_t, 3> groups);

    // As recordGroups(), binding each entry of bindings() exactly once by binding number.
    // Each GpuBinding must match the declared descriptor type, a uniform buffer must
    // hold at least blockBytes, and a texture's view type must match an image binding's.
    GpuStatus recordBindings(VkCommandBuffer cmd, std::span<const GpuBinding> bindings,
                             std::span<const std::byte> push, std::array<uint32_t, 3> groups);

    void releaseDescriptors();

    std::array<uint32_t, 3> groupCount(std::array<uint32_t, 3> threads) const;
    // For a reflected kernel, pushConstantBytes and groupSize are the reflected values.
    const GpuKernelDesc& desc() const { return desc_; }
    std::span<const GpuKernelBinding> bindings() const { return bindings_; }
    // Null when no binding has that name.
    const GpuKernelBinding* findBinding(std::string_view name) const;

private:
    GpuKernel(GpuDevice& device, const GpuKernelDesc& desc)
        : device_(device)
        , desc_(desc)
    {
    }

    GpuStatus reflectLayout();
    // Creates the set layout, pipeline layout, pipeline and descriptor pool from bindings_ and desc_.
    GpuStatus createPipeline();
    GpuStatus bindAndDispatch(VkCommandBuffer cmd, std::span<VkWriteDescriptorSet> writes,
                              std::span<const std::byte> push, std::array<uint32_t, 3> groups);

    GpuDevice& device_;
    GpuKernelDesc desc_;
    std::vector<GpuKernelBinding> bindings_;
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
