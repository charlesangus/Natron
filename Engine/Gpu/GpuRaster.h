#pragma once

#include "GpuDevice.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace gpu {

// A vertex and a fragment stage over embedded SPIR-V, from slang_add_raster.
//
// Raster convention: every resource is a storage buffer in descriptor set 0, bindings 0..N-1, visible
// to both stages; parameters are one push-constant block shared by both stages (at most
// GpuRasterPipeline::kMaxPushConstantBytes). There are no vertex inputs: the vertex stage pulls its
// data from the storage buffers by vertex and instance index. The fragment stage writes one colour
// output at location 0.
struct GpuRasterDesc {
    const uint32_t* vertexSpirv = nullptr;
    size_t vertexSpirvWords = 0;
    const uint32_t* fragmentSpirv = nullptr;
    size_t fragmentSpirvWords = 0;
    const char* entry = "main";
    uint32_t storageBufferCount = 0;
    uint32_t pushConstantBytes = 0;
};

enum class GpuRasterBlend {
    // The fragment's colour overwrites the attachment's.
    Replace,
    // The fragment's colour is added to the attachment's, alpha included.
    Additive,
};

// A graphics pipeline for dynamic rendering into one float colour attachment (R32_SFLOAT or
// R32G32B32A32_SFLOAT): triangle lists, no culling, one sample, no depth or stencil, viewport and
// scissor set when recording. It must not be destroyed while recorded work still uses it, and must
// be destroyed before its GpuDevice.
class GpuRasterPipeline {
public:
    static constexpr uint32_t kMaxPushConstantBytes = 128;

    // Fails with VK_ERROR_FEATURE_NOT_PRESENT when the device lacks GpuDeviceInfo::rasterFloatBlend, and
    // with VK_ERROR_FORMAT_NOT_SUPPORTED for another attachment format. `pipelineCache`, when not null, is
    // passed to vkCreateGraphicsPipelines.
    static GpuStatus create(GpuDevice& device, const GpuRasterDesc& desc, VkFormat colourFormat, GpuRasterBlend blend,
                            std::unique_ptr<GpuRasterPipeline>& out, VkPipelineCache pipelineCache = VK_NULL_HANDLE);
    ~GpuRasterPipeline();
    GpuRasterPipeline(const GpuRasterPipeline&) = delete;
    GpuRasterPipeline& operator=(const GpuRasterPipeline&) = delete;

    const GpuRasterDesc& desc() const { return desc_; }
    VkFormat colourFormat() const { return format_; }
    GpuRasterBlend blend() const { return blend_; }

    VkPipeline pipeline() const { return pipeline_; }
    VkPipelineLayout pipelineLayout() const { return pipelineLayout_; }
    VkDescriptorSetLayout setLayout() const { return setLayout_; }

private:
    GpuRasterPipeline(GpuDevice& device, const GpuRasterDesc& desc, VkFormat format, GpuRasterBlend blend)
        : device_(device)
        , desc_(desc)
        , format_(format)
        , blend_(blend)
    {
    }

    GpuStatus createPipeline(VkPipelineCache pipelineCache);

    GpuDevice& device_;
    GpuRasterDesc desc_;
    VkFormat format_;
    GpuRasterBlend blend_;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

} // namespace gpu
