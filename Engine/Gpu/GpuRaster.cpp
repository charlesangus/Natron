#include "GpuRaster.h"

#include <cfenv>
#include <string>
#include <utility>
#include <vector>

namespace gpu {

namespace {

    GpuStatus fail(VkResult r, std::string msg)
    {
        return GpuStatus { r, std::move(msg) };
    }

    struct ShaderModules {
        VkDevice device = VK_NULL_HANDLE;
        VkShaderModule vertex = VK_NULL_HANDLE;
        VkShaderModule fragment = VK_NULL_HANDLE;
        ~ShaderModules()
        {
            if (vertex) {
                vkDestroyShaderModule(device, vertex, nullptr);
            }
            if (fragment) {
                vkDestroyShaderModule(device, fragment, nullptr);
            }
        }
    };

} // namespace

GpuStatus
GpuRasterPipeline::create(GpuDevice& device, const GpuRasterDesc& desc, VkFormat colourFormat, GpuRasterBlend blend,
                          std::unique_ptr<GpuRasterPipeline>& out, VkPipelineCache pipelineCache)
{
    if (device.isLost()) {
        return fail(VK_ERROR_DEVICE_LOST, "GpuRasterPipeline::create: device lost");
    }
    if (!device.info().rasterFloatBlend) {
        return fail(VK_ERROR_FEATURE_NOT_PRESENT, "GpuRasterPipeline::create: the device cannot blend float colour attachments");
    }
    if (colourFormat != VK_FORMAT_R32_SFLOAT && colourFormat != VK_FORMAT_R32G32B32A32_SFLOAT) {
        return fail(VK_ERROR_FORMAT_NOT_SUPPORTED, "GpuRasterPipeline::create: the attachment must be R32_SFLOAT or R32G32B32A32_SFLOAT");
    }
    if (!desc.vertexSpirv || desc.vertexSpirvWords == 0 || !desc.fragmentSpirv || desc.fragmentSpirvWords == 0
        || !desc.entry || desc.pushConstantBytes > kMaxPushConstantBytes || desc.pushConstantBytes % 4 != 0) {
        return fail(VK_ERROR_INITIALIZATION_FAILED, "GpuRasterPipeline::create: invalid raster description");
    }
    std::unique_ptr<GpuRasterPipeline> p(new GpuRasterPipeline(device, desc, colourFormat, blend));
    // As for compute kernels: pipelines are built on render threads, where debug builds trap the
    // floating-point exceptions Mesa's compilers raise of their own doing.
    std::fenv_t env;
    std::feholdexcept(&env);
    const GpuStatus created = p->createPipeline(pipelineCache);
    std::fesetenv(&env);
    if (!created) {
        return created;
    }
    out = std::move(p);
    return {};
}

GpuStatus
GpuRasterPipeline::createPipeline(VkPipelineCache pipelineCache)
{
    const VkDevice dev = device_.device();
    constexpr VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    std::vector<VkDescriptorSetLayoutBinding> layoutBindings(desc_.storageBufferCount);
    for (uint32_t i = 0; i < desc_.storageBufferCount; ++i) {
        layoutBindings[i] = { i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages, nullptr };
    }
    VkDescriptorSetLayoutCreateInfo sli { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    sli.flags = device_.info().pushDescriptor ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0;
    sli.bindingCount = static_cast<uint32_t>(layoutBindings.size());
    sli.pBindings = layoutBindings.data();
    GpuStatus s = device_.check(vkCreateDescriptorSetLayout(dev, &sli, nullptr, &setLayout_),
                                "vkCreateDescriptorSetLayout");
    if (!s) {
        return s;
    }

    VkPushConstantRange range { stages, 0, desc_.pushConstantBytes };
    VkPipelineLayoutCreateInfo pli { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &setLayout_;
    pli.pushConstantRangeCount = desc_.pushConstantBytes ? 1 : 0;
    pli.pPushConstantRanges = &range;
    s = device_.check(vkCreatePipelineLayout(dev, &pli, nullptr, &pipelineLayout_), "vkCreatePipelineLayout");
    if (!s) {
        return s;
    }

    ShaderModules modules;
    modules.device = dev;
    VkShaderModuleCreateInfo smi { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smi.codeSize = desc_.vertexSpirvWords * sizeof(uint32_t);
    smi.pCode = desc_.vertexSpirv;
    s = device_.check(vkCreateShaderModule(dev, &smi, nullptr, &modules.vertex), "vkCreateShaderModule");
    if (!s) {
        return s;
    }
    smi.codeSize = desc_.fragmentSpirvWords * sizeof(uint32_t);
    smi.pCode = desc_.fragmentSpirv;
    s = device_.check(vkCreateShaderModule(dev, &smi, nullptr, &modules.fragment), "vkCreateShaderModule");
    if (!s) {
        return s;
    }

    VkPipelineShaderStageCreateInfo stageInfos[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, modules.vertex,
          desc_.entry, nullptr },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
          modules.fragment, desc_.entry, nullptr },
    };

    VkPipelineVertexInputStateCreateInfo vertexInput { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };

    VkPipelineInputAssemblyStateCreateInfo assembly { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState attachment {};
    attachment.colorWriteMask = (format_ == VK_FORMAT_R32_SFLOAT)
        ? VkColorComponentFlags(VK_COLOR_COMPONENT_R_BIT)
        : VkColorComponentFlags(VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT
                                | VK_COLOR_COMPONENT_A_BIT);
    if (blend_ == GpuRasterBlend::Additive) {
        attachment.blendEnable = VK_TRUE;
        attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.colorBlendOp = VK_BLEND_OP_ADD;
        attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo colourBlend { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    colourBlend.attachmentCount = 1;
    colourBlend.pAttachments = &attachment;

    const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    VkPipelineRenderingCreateInfo rendering { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &format_;

    VkGraphicsPipelineCreateInfo gpi { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gpi.pNext = &rendering;
    gpi.stageCount = 2;
    gpi.pStages = stageInfos;
    gpi.pVertexInputState = &vertexInput;
    gpi.pInputAssemblyState = &assembly;
    gpi.pViewportState = &viewport;
    gpi.pRasterizationState = &raster;
    gpi.pMultisampleState = &multisample;
    gpi.pColorBlendState = &colourBlend;
    gpi.pDynamicState = &dynamic;
    gpi.layout = pipelineLayout_;
    return device_.check(vkCreateGraphicsPipelines(dev, pipelineCache, 1, &gpi, nullptr, &pipeline_),
                         "vkCreateGraphicsPipelines");
}

GpuRasterPipeline::~GpuRasterPipeline()
{
    // Work that never finished may still use the pipeline.
    if (device_.hasAbandonedWork()) {
        return;
    }
    const VkDevice dev = device_.device();
    if (pipeline_) {
        vkDestroyPipeline(dev, pipeline_, nullptr);
    }
    if (pipelineLayout_) {
        vkDestroyPipelineLayout(dev, pipelineLayout_, nullptr);
    }
    if (setLayout_) {
        vkDestroyDescriptorSetLayout(dev, setLayout_, nullptr);
    }
}

} // namespace gpu
