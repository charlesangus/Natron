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
// output at location 0, and with a depth test its depth goes to a D32_SFLOAT attachment.
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

enum class GpuRasterDepth {
    // Every fragment reaches the colour attachment, in draw order.
    None,
    // A fragment reaches the colour attachment, and writes its depth, only where it is nearer than the
    // D32_SFLOAT depth attachment's value, so the nearest surface wins whatever the draw order.
    Less,
};

// A graphics pipeline for dynamic rendering into one colour attachment: triangle lists, no culling,
// one sample, no stencil, viewport and scissor set when recording. Float attachments (R32_SFLOAT,
// R32G32B32A32_SFLOAT) take either blend; uint attachments (R32G32_UINT, R32G32B32A32_UINT) hold ids,
// which cannot blend, so only Replace. It must not be destroyed while recorded work still uses it, and
// must be destroyed before its GpuDevice.
class GpuRasterPipeline {
public:
    static constexpr uint32_t kMaxPushConstantBytes = 128;

    // Fails with VK_ERROR_FEATURE_NOT_PRESENT when the device lacks GpuDeviceInfo::rasterFloatBlend for a
    // float attachment, or GpuDeviceInfo::rasterVisibility for a uint attachment or a depth test, and with
    // VK_ERROR_FORMAT_NOT_SUPPORTED for another attachment format or a uint one with Additive.
    // `pipelineCache`, when not null, is passed to vkCreateGraphicsPipelines.
    static GpuStatus create(GpuDevice& device, const GpuRasterDesc& desc, VkFormat colourFormat, GpuRasterBlend blend,
                            GpuRasterDepth depth, std::unique_ptr<GpuRasterPipeline>& out,
                            VkPipelineCache pipelineCache = VK_NULL_HANDLE);

    // Whether a colour attachment in `format` holds uint ids rather than floats.
    static bool isUintFormat(VkFormat format)
    {
        return format == VK_FORMAT_R32G32_UINT || format == VK_FORMAT_R32G32B32A32_UINT;
    }
    static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
    ~GpuRasterPipeline();
    GpuRasterPipeline(const GpuRasterPipeline&) = delete;
    GpuRasterPipeline& operator=(const GpuRasterPipeline&) = delete;

    const GpuRasterDesc& desc() const { return desc_; }
    VkFormat colourFormat() const { return format_; }
    GpuRasterBlend blend() const { return blend_; }
    GpuRasterDepth depth() const { return depth_; }

    VkPipeline pipeline() const { return pipeline_; }
    VkPipelineLayout pipelineLayout() const { return pipelineLayout_; }
    VkDescriptorSetLayout setLayout() const { return setLayout_; }

private:
    GpuRasterPipeline(GpuDevice& device, const GpuRasterDesc& desc, VkFormat format, GpuRasterBlend blend,
                      GpuRasterDepth depth)
        : device_(device)
        , desc_(desc)
        , format_(format)
        , blend_(blend)
        , depth_(depth)
    {
    }

    GpuStatus createPipeline(VkPipelineCache pipelineCache);

    GpuDevice& device_;
    GpuRasterDesc desc_;
    VkFormat format_;
    GpuRasterBlend blend_;
    GpuRasterDepth depth_;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

} // namespace gpu
