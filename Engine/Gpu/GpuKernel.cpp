#include "GpuKernel.h"

#include "GpuBuffer.h"
#include "GpuTexture.h"

#include <algorithm>
#include <string>
#include <utility>

#include <spirv_reflect.h>

namespace gpu {

namespace {

    GpuStatus fail(VkResult r, std::string msg)
    {
        return GpuStatus { r, std::move(msg) };
    }

    GpuStatus reflectError(const std::string& msg)
    {
        return fail(VK_ERROR_INITIALIZATION_FAILED, "GpuKernel: cannot use the shader module: " + msg);
    }

    struct ReflectedModule {
        SpvReflectShaderModule* module;
        ~ReflectedModule() { spvReflectDestroyShaderModule(module); }
    };

    // Bytes up to the end of the last member, without the block's tail padding.
    uint32_t blockEnd(const SpvReflectBlockVariable& block)
    {
        if (block.member_count == 0) {
            return block.offset + block.size;
        }
        uint32_t end = 0;
        for (uint32_t i = 0; i < block.member_count; ++i) {
            end = std::max(end, block.members[i].offset + block.members[i].size);
        }
        return end;
    }

    const char* descriptorTypeName(SpvReflectDescriptorType type)
    {
        switch (type) {
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            return "storage image";
        case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            return "uniform texel buffer";
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            return "storage texel buffer";
        case SPV_REFLECT_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            return "input attachment";
        case SPV_REFLECT_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
            return "acceleration structure";
        default:
            return "unknown";
        }
    }

    // SPIRV-Reflect reports the LocalSize literal, but a WorkgroupSize built-in overrides it, and when that
    // built-in is a specialization constant (GLSL local_size_x_id) the literal is not the size the pipeline gets.
    bool workgroupSizeIsSpecConstant(const uint32_t* words, size_t count)
    {
        constexpr uint32_t kOpSpecConstantComposite = 51;
        constexpr uint32_t kOpDecorate = 71;
        constexpr uint32_t kDecorationBuiltIn = 11;
        constexpr uint32_t kBuiltInWorkgroupSize = 25;
        std::vector<uint32_t> decorated;
        std::vector<uint32_t> specComposites;
        for (size_t i = 5; i < count;) {
            const uint32_t wordCount = words[i] >> 16;
            const uint32_t opcode = words[i] & 0xffffu;
            if (wordCount == 0 || i + wordCount > count) {
                break;
            }
            if (opcode == kOpDecorate && wordCount >= 4 && words[i + 2] == kDecorationBuiltIn
                && words[i + 3] == kBuiltInWorkgroupSize) {
                decorated.push_back(words[i + 1]);
            } else if (opcode == kOpSpecConstantComposite && wordCount >= 3) {
                specComposites.push_back(words[i + 2]);
            }
            i += wordCount;
        }
        for (uint32_t id : decorated) {
            if (std::find(specComposites.begin(), specComposites.end(), id) != specComposites.end()) {
                return true;
            }
        }
        return false;
    }

    constexpr uint32_t kDescriptorPoolSets = 1024;

} // namespace

void
GpuUniformBuffer::moveFrom(GpuUniformBuffer& o) noexcept
{
    device_ = std::exchange(o.device_, nullptr);
    buffer_ = std::exchange(o.buffer_, VK_NULL_HANDLE);
    allocation_ = std::exchange(o.allocation_, nullptr);
    size_ = std::exchange(o.size_, 0);
}

void
GpuUniformBuffer::reset()
{
    if (device_ && allocation_) {
        vmaDestroyBuffer(device_->allocator(), buffer_, allocation_);
    }
    device_ = nullptr;
    buffer_ = VK_NULL_HANDLE;
    allocation_ = nullptr;
    size_ = 0;
}

GpuStatus
GpuUniformBuffer::create(GpuDevice& device, std::span<const std::byte> data, GpuUniformBuffer& out)
{
    out.reset();
    if (device.isLost()) {
        return fail(VK_ERROR_DEVICE_LOST, "GpuUniformBuffer::create: device lost");
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device.physicalDevice(), &props);
    if (data.empty() || data.size() > props.limits.maxUniformBufferRange) {
        return fail(VK_ERROR_INITIALIZATION_FAILED, "GpuUniformBuffer::create: " + std::to_string(data.size()) + " bytes is outside 1.." + std::to_string(props.limits.maxUniformBufferRange));
    }
    VkBufferCreateInfo bci { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = data.size();
    bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo aci {};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    GpuUniformBuffer b;
    if (GpuStatus s = device.check(vmaCreateBuffer(device.allocator(), &bci, &aci, &b.buffer_, &b.allocation_, nullptr),
                                   "vmaCreateBuffer");
        !s) {
        return s;
    }
    b.device_ = &device;
    b.size_ = data.size();
    if (GpuStatus s = device.check(vmaCopyMemoryToAllocation(device.allocator(), data.data(), b.allocation_, 0, data.size()),
                                   "vmaCopyMemoryToAllocation");
        !s) {
        return s;
    }
    out = std::move(b);
    return {};
}

GpuBinding
GpuBinding::storage(uint32_t binding, const GpuBuffer& buffer)
{
    GpuBinding b;
    b.binding = binding;
    b.kind = Kind::StorageBuffer;
    b.buffer = buffer.buffer();
    b.bufferSize = buffer.size();
    return b;
}

GpuBinding
GpuBinding::uniform(uint32_t binding, const GpuUniformBuffer& buffer)
{
    GpuBinding b;
    b.binding = binding;
    b.kind = Kind::UniformBuffer;
    b.buffer = buffer.buffer();
    b.bufferSize = buffer.size();
    return b;
}

GpuBinding
GpuBinding::texture(uint32_t binding, const GpuTexture& texture)
{
    GpuBinding b;
    b.binding = binding;
    b.kind = Kind::Texture;
    b.view = texture.view();
    b.sampler = texture.sampler();
    b.viewType = texture.viewType();
    return b;
}

GpuStatus
GpuKernel::create(GpuDevice& device, const GpuKernelDesc& desc,
                  std::unique_ptr<GpuKernel>& out, VkPipelineCache pipelineCache)
{
    if (device.isLost()) {
        return fail(VK_ERROR_DEVICE_LOST, "GpuKernel::create: device lost");
    }
    std::unique_ptr<GpuKernel> k(new GpuKernel(device, desc));
    if (desc.reflect) {
        if (!desc.spirv || desc.spirvWords == 0 || !desc.entry) {
            return fail(VK_ERROR_INITIALIZATION_FAILED, "GpuKernel::create: invalid kernel description");
        }
        if (GpuStatus s = k->reflectLayout(); !s) {
            return s;
        }
    } else {
        if (!desc.spirv || desc.spirvWords == 0 || desc.pushConstantBytes > kMaxPushConstantBytes
            || desc.pushConstantBytes % 4 != 0 || desc.groupSize[0] == 0 || desc.groupSize[1] == 0
            || desc.groupSize[2] == 0) {
            return fail(VK_ERROR_INITIALIZATION_FAILED, "GpuKernel::create: invalid kernel description");
        }
        k->bindings_.resize(desc.storageBufferCount);
        for (uint32_t i = 0; i < desc.storageBufferCount; ++i) {
            k->bindings_[i].binding = i;
            k->bindings_[i].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
    }
    if (GpuStatus s = k->createPipeline(pipelineCache); !s) {
        return s;
    }
    out = std::move(k);
    return {};
}

GpuStatus
GpuKernel::createFromSpirv(GpuDevice& device, std::span<const uint32_t> spirv, const char* entry,
                           std::unique_ptr<GpuKernel>& out, VkPipelineCache pipelineCache)
{
    GpuKernelDesc desc;
    desc.spirv = spirv.data();
    desc.spirvWords = spirv.size();
    desc.entry = entry;
    desc.reflect = true;
    GpuStatus s = create(device, desc, out, pipelineCache);
    if (s && out) {
        out->desc_.spirv = nullptr;
        out->desc_.spirvWords = 0;
        out->desc_.entry = "";
    }
    return s;
}

GpuStatus
GpuKernel::reflectLayout()
{
    SpvReflectShaderModule module {};
    if (spvReflectCreateShaderModule2(SPV_REFLECT_MODULE_FLAG_NO_COPY, desc_.spirvWords * sizeof(uint32_t),
                                      desc_.spirv, &module)
        != SPV_REFLECT_RESULT_SUCCESS) {
        return reflectError("the SPIR-V module could not be parsed");
    }
    ReflectedModule guard { &module };

    const SpvReflectEntryPoint* ep = spvReflectGetEntryPoint(&module, desc_.entry);
    if (!ep) {
        return reflectError(std::string("no entry point named '") + desc_.entry + "'");
    }
    if (ep->shader_stage != SPV_REFLECT_SHADER_STAGE_COMPUTE_BIT) {
        return reflectError(std::string("entry point '") + desc_.entry + "' is not a compute shader");
    }
    const uint32_t local[3] = { ep->local_size.x, ep->local_size.y, ep->local_size.z };
    for (size_t i = 0; i < 3; ++i) {
        if (local[i] == 0 || local[i] == static_cast<uint32_t>(SPV_REFLECT_EXECUTION_MODE_SPEC_CONSTANT)) {
            return reflectError("the local size is not a literal");
        }
        desc_.groupSize[i] = local[i];
    }
    if (workgroupSizeIsSpecConstant(desc_.spirv, desc_.spirvWords)) {
        return reflectError("the local size is not a literal");
    }

    uint32_t count = 0;
    if (spvReflectEnumerateDescriptorBindings(&module, &count, nullptr) != SPV_REFLECT_RESULT_SUCCESS) {
        return reflectError("descriptor bindings could not be enumerated");
    }
    std::vector<SpvReflectDescriptorBinding*> found(count);
    if (count > 0 && spvReflectEnumerateDescriptorBindings(&module, &count, found.data()) != SPV_REFLECT_RESULT_SUCCESS) {
        return reflectError("descriptor bindings could not be enumerated");
    }
    std::vector<GpuKernelBinding> bindings;
    for (const SpvReflectDescriptorBinding* b : found) {
        GpuKernelBinding out;
        out.binding = b->binding;
        if (b->name && *b->name) {
            out.name = b->name;
        } else if (b->type_description && b->type_description->type_name) {
            out.name = b->type_description->type_name;
        }
        const std::string what = "binding " + std::to_string(b->binding) + " ('" + out.name + "')";
        if (b->set != 0) {
            return reflectError(what + " is in descriptor set " + std::to_string(b->set) + "; only set 0 is supported");
        }
        if (b->count != 1) {
            return reflectError(what + " is a descriptor array, which is not supported");
        }
        switch (b->descriptor_type) {
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            out.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            break;
        case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            out.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            out.blockBytes = blockEnd(b->block);
            break;
        case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER:
            out.type = VK_DESCRIPTOR_TYPE_SAMPLER;
            break;
        case SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            out.type = b->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            if (b->image.arrayed || b->image.ms) {
                return reflectError(what + " is an arrayed or multisampled image, which is not supported");
            }
            switch (b->image.dim) {
            case SpvDim1D:
                out.viewType = VK_IMAGE_VIEW_TYPE_1D;
                break;
            case SpvDim2D:
                out.viewType = VK_IMAGE_VIEW_TYPE_2D;
                break;
            case SpvDim3D:
                out.viewType = VK_IMAGE_VIEW_TYPE_3D;
                break;
            default:
                return reflectError(what + " is an image that is not 1D, 2D or 3D");
            }
            break;
        default:
            return reflectError(what + " is a " + descriptorTypeName(b->descriptor_type) + " ("
                                + std::to_string(static_cast<int>(b->descriptor_type)) + "), which is not supported");
        }
        for (const GpuKernelBinding& other : bindings) {
            if (other.binding == out.binding) {
                return reflectError(what + " aliases another resource at the same binding");
            }
        }
        bindings.push_back(std::move(out));
    }
    std::sort(bindings.begin(), bindings.end(),
              [](const GpuKernelBinding& a, const GpuKernelBinding& b) { return a.binding < b.binding; });

    if (device_.info().pushDescriptor) {
        VkPhysicalDevicePushDescriptorPropertiesKHR pdp { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR };
        VkPhysicalDeviceProperties2 props { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        props.pNext = &pdp;
        vkGetPhysicalDeviceProperties2(device_.physicalDevice(), &props);
        if (bindings.size() > pdp.maxPushDescriptors) {
            return reflectError(std::to_string(bindings.size()) + " bindings exceed the device's "
                                + std::to_string(pdp.maxPushDescriptors) + " push descriptors");
        }
    }

    uint32_t blockCount = 0;
    if (spvReflectEnumeratePushConstantBlocks(&module, &blockCount, nullptr) != SPV_REFLECT_RESULT_SUCCESS) {
        return reflectError("push-constant blocks could not be enumerated");
    }
    if (blockCount > 1) {
        return reflectError("more than one push-constant block");
    }
    uint32_t pushBytes = 0;
    if (blockCount == 1) {
        SpvReflectBlockVariable* block = nullptr;
        if (spvReflectEnumeratePushConstantBlocks(&module, &blockCount, &block) != SPV_REFLECT_RESULT_SUCCESS || !block) {
            return reflectError("push-constant blocks could not be enumerated");
        }
        // The range starts at offset 0, so it must reach the end of the last member even when the first one is offset.
        pushBytes = (blockEnd(*block) + 3u) & ~3u;
        if (pushBytes > kMaxPushConstantBytes) {
            return reflectError("the push-constant block is " + std::to_string(pushBytes) + " bytes, above "
                                + std::to_string(kMaxPushConstantBytes));
        }
    }
    desc_.pushConstantBytes = pushBytes;
    desc_.storageBufferCount = 0;
    bindings_ = std::move(bindings);
    return {};
}

GpuStatus
GpuKernel::createPipeline(VkPipelineCache pipelineCache)
{
    const VkDevice dev = device_.device();
    const bool push = device_.info().pushDescriptor;

    std::vector<VkDescriptorSetLayoutBinding> layoutBindings(bindings_.size());
    for (size_t i = 0; i < bindings_.size(); ++i) {
        layoutBindings[i] = { bindings_[i].binding, bindings_[i].type, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    }
    VkDescriptorSetLayoutCreateInfo sli { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    sli.flags = push ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0;
    sli.bindingCount = static_cast<uint32_t>(layoutBindings.size());
    sli.pBindings = layoutBindings.data();
    GpuStatus s = device_.check(vkCreateDescriptorSetLayout(dev, &sli, nullptr, &setLayout_),
                                "vkCreateDescriptorSetLayout");
    if (!s) {
        return s;
    }

    VkPushConstantRange range { VK_SHADER_STAGE_COMPUTE_BIT, 0, desc_.pushConstantBytes };
    VkPipelineLayoutCreateInfo pli { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &setLayout_;
    pli.pushConstantRangeCount = desc_.pushConstantBytes ? 1 : 0;
    pli.pPushConstantRanges = &range;
    s = device_.check(vkCreatePipelineLayout(dev, &pli, nullptr, &pipelineLayout_),
                      "vkCreatePipelineLayout");
    if (!s) {
        return s;
    }

    VkShaderModuleCreateInfo smi { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smi.codeSize = desc_.spirvWords * sizeof(uint32_t);
    smi.pCode = desc_.spirv;
    VkShaderModule module = VK_NULL_HANDLE;
    s = device_.check(vkCreateShaderModule(dev, &smi, nullptr, &module), "vkCreateShaderModule");
    if (!s) {
        return s;
    }

    VkComputePipelineCreateInfo cpi { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                  VK_SHADER_STAGE_COMPUTE_BIT, module, desc_.entry, nullptr };
    cpi.layout = pipelineLayout_;
    s = device_.check(vkCreateComputePipelines(dev, pipelineCache, 1, &cpi, nullptr, &pipeline_),
                      "vkCreateComputePipelines");
    vkDestroyShaderModule(dev, module, nullptr);
    if (!s) {
        return s;
    }

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device_.physicalDevice(), &props);
    for (size_t i = 0; i < 3; ++i) {
        maxGroups_[i] = props.limits.maxComputeWorkGroupCount[i];
    }

    if (push) {
        pushDescriptorFn_ = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(
            vkGetDeviceProcAddr(dev, "vkCmdPushDescriptorSetKHR"));
        if (!pushDescriptorFn_) {
            return fail(VK_ERROR_EXTENSION_NOT_PRESENT, "vkCmdPushDescriptorSetKHR unavailable");
        }
    } else if (!bindings_.empty()) {
        std::vector<VkDescriptorPoolSize> sizes;
        for (const GpuKernelBinding& b : bindings_) {
            auto it = std::find_if(sizes.begin(), sizes.end(),
                                   [&](const VkDescriptorPoolSize& p) { return p.type == b.type; });
            if (it == sizes.end()) {
                sizes.push_back({ b.type, kDescriptorPoolSets });
            } else {
                it->descriptorCount += kDescriptorPoolSets;
            }
        }
        VkDescriptorPoolCreateInfo dpi { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpi.maxSets = kDescriptorPoolSets;
        dpi.poolSizeCount = static_cast<uint32_t>(sizes.size());
        dpi.pPoolSizes = sizes.data();
        s = device_.check(vkCreateDescriptorPool(dev, &dpi, nullptr, &pool_),
                          "vkCreateDescriptorPool");
        if (!s) {
            return s;
        }
    }
    return {};
}

const GpuKernelBinding*
GpuKernel::findBinding(std::string_view name) const
{
    for (const GpuKernelBinding& b : bindings_) {
        if (b.name == name) {
            return &b;
        }
    }
    return nullptr;
}

GpuKernel::~GpuKernel()
{
    // Work that never finished may still use the pipeline and the descriptor sets allocated from the pool.
    if (device_.hasAbandonedWork()) {
        return;
    }
    const VkDevice dev = device_.device();
    if (pool_) {
        vkDestroyDescriptorPool(dev, pool_, nullptr);
    }
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

std::array<uint32_t, 3>
GpuKernel::groupCount(std::array<uint32_t, 3> threads) const
{
    std::array<uint32_t, 3> g {};
    for (size_t i = 0; i < 3; ++i) {
        g[i] = static_cast<uint32_t>((uint64_t(threads[i]) + desc_.groupSize[i] - 1) / desc_.groupSize[i]);
    }
    return g;
}

GpuStatus
GpuKernel::record(VkCommandBuffer cmd, std::span<const VkBuffer> buffers,
                  std::span<const std::byte> push, std::array<uint32_t, 3> threads)
{
    return recordGroups(cmd, buffers, push, groupCount(threads));
}

GpuStatus
GpuKernel::recordGroups(VkCommandBuffer cmd, std::span<const VkBuffer> buffers,
                        std::span<const std::byte> push, std::array<uint32_t, 3> groups)
{
    if (desc_.reflect) {
        return fail(VK_ERROR_INITIALIZATION_FAILED, "GpuKernel::record: a reflected kernel is bound with recordBindings()");
    }
    if (buffers.size() != desc_.storageBufferCount || push.size() != desc_.pushConstantBytes) {
        return fail(VK_ERROR_INITIALIZATION_FAILED, "GpuKernel::record: binding or push size mismatch");
    }

    std::vector<VkDescriptorBufferInfo> infos(buffers.size());
    std::vector<VkWriteDescriptorSet> writes(buffers.size());
    for (size_t i = 0; i < buffers.size(); ++i) {
        infos[i] = { buffers[i], 0, VK_WHOLE_SIZE };
        writes[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[i].dstBinding = static_cast<uint32_t>(i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    return bindAndDispatch(cmd, writes, push, groups);
}

GpuStatus
GpuKernel::recordBindings(VkCommandBuffer cmd, std::span<const GpuBinding> bindings,
                          std::span<const std::byte> push, std::array<uint32_t, 3> groups)
{
    if (push.size() != desc_.pushConstantBytes) {
        return fail(VK_ERROR_INITIALIZATION_FAILED, "GpuKernel::recordBindings: push-constant size mismatch, " + std::to_string(push.size()) + " bytes for a " + std::to_string(desc_.pushConstantBytes) + "-byte block");
    }
    if (bindings.size() != bindings_.size()) {
        return fail(VK_ERROR_INITIALIZATION_FAILED, "GpuKernel::recordBindings: " + std::to_string(bindings.size()) + " bindings for a kernel declaring " + std::to_string(bindings_.size()));
    }

    // Reserved up front: the writes point into these arrays.
    std::vector<VkDescriptorBufferInfo> bufferInfos;
    std::vector<VkDescriptorImageInfo> imageInfos;
    bufferInfos.reserve(bindings.size());
    imageInfos.reserve(bindings.size());
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(bindings.size());
    std::vector<bool> seen(bindings_.size(), false);
    for (const GpuBinding& b : bindings) {
        const std::string where = "GpuKernel::recordBindings: binding " + std::to_string(b.binding);
        size_t slot = bindings_.size();
        for (size_t i = 0; i < bindings_.size(); ++i) {
            if (bindings_[i].binding == b.binding) {
                slot = i;
                break;
            }
        }
        if (slot == bindings_.size()) {
            return fail(VK_ERROR_INITIALIZATION_FAILED, where + " is not declared by the kernel");
        }
        if (seen[slot]) {
            return fail(VK_ERROR_INITIALIZATION_FAILED, where + " is bound twice");
        }
        seen[slot] = true;
        const GpuKernelBinding& decl = bindings_[slot];

        VkWriteDescriptorSet w { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstBinding = decl.binding;
        w.descriptorCount = 1;
        w.descriptorType = decl.type;
        switch (decl.type) {
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: {
            const bool uniform = decl.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            if (b.kind != (uniform ? GpuBinding::Kind::UniformBuffer : GpuBinding::Kind::StorageBuffer)) {
                return fail(VK_ERROR_INITIALIZATION_FAILED,
                            where + " expects a " + (uniform ? "uniform" : "storage") + " buffer");
            }
            if (b.buffer == VK_NULL_HANDLE) {
                return fail(VK_ERROR_INITIALIZATION_FAILED, where + " has no buffer");
            }
            if (uniform && b.bufferSize < decl.blockBytes) {
                return fail(VK_ERROR_INITIALIZATION_FAILED, where + " holds " + std::to_string(b.bufferSize) + " bytes for a " + std::to_string(decl.blockBytes) + "-byte uniform block");
            }
            bufferInfos.push_back({ b.buffer, 0, VK_WHOLE_SIZE });
            w.pBufferInfo = &bufferInfos.back();
            break;
        }
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_SAMPLER: {
            if (b.kind != GpuBinding::Kind::Texture) {
                return fail(VK_ERROR_INITIALIZATION_FAILED, where + " expects a texture");
            }
            const bool needsView = decl.type != VK_DESCRIPTOR_TYPE_SAMPLER;
            const bool needsSampler = decl.type != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            if ((needsView && b.view == VK_NULL_HANDLE) || (needsSampler && b.sampler == VK_NULL_HANDLE)) {
                return fail(VK_ERROR_INITIALIZATION_FAILED, where + " has no texture");
            }
            if (needsView && b.viewType != decl.viewType) {
                return fail(VK_ERROR_INITIALIZATION_FAILED, where + " binds a texture of the wrong dimensionality");
            }
            imageInfos.push_back({ needsSampler ? b.sampler : VK_NULL_HANDLE, needsView ? b.view : VK_NULL_HANDLE,
                                   needsView ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED });
            w.pImageInfo = &imageInfos.back();
            break;
        }
        default:
            return fail(VK_ERROR_INITIALIZATION_FAILED, where + " has an unsupported descriptor type");
        }
        writes.push_back(w);
    }
    return bindAndDispatch(cmd, writes, push, groups);
}

GpuStatus
GpuKernel::bindAndDispatch(VkCommandBuffer cmd, std::span<VkWriteDescriptorSet> writes,
                           std::span<const std::byte> push, std::array<uint32_t, 3> groups)
{
    for (size_t i = 0; i < 3; ++i) {
        if (groups[i] > maxGroups_[i]) {
            return fail(VK_ERROR_FEATURE_NOT_PRESENT,
                        "GpuKernel::record: " + std::to_string(groups[i]) + " groups along axis "
                            + std::to_string(i) + " exceed the device limit of " + std::to_string(maxGroups_[i]));
        }
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    if (pushDescriptorFn_) {
        if (!writes.empty()) {
            pushDescriptorFn_(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0,
                              static_cast<uint32_t>(writes.size()), writes.data());
        }
    } else if (pool_) {
        VkDescriptorSet set = VK_NULL_HANDLE;
        {
            std::lock_guard<std::mutex> lock(poolMutex_);
            VkDescriptorSetAllocateInfo ai { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = pool_;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &setLayout_;
            const VkResult r = vkAllocateDescriptorSets(device_.device(), &ai, &set);
            if (r == VK_ERROR_OUT_OF_POOL_MEMORY || r == VK_ERROR_FRAGMENTED_POOL) {
                return fail(r, "GpuKernel::record: descriptor pool exhausted after " + std::to_string(kDescriptorPoolSets) + " dispatches; call releaseDescriptors() once recorded work completes");
            }
            if (GpuStatus s = device_.check(r, "vkAllocateDescriptorSets"); !s) {
                return s;
            }
        }
        for (VkWriteDescriptorSet& w : writes) {
            w.dstSet = set;
        }
        vkUpdateDescriptorSets(device_.device(), static_cast<uint32_t>(writes.size()), writes.data(),
                               0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1, &set, 0,
                                nullptr);
    }
    if (!push.empty()) {
        vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           static_cast<uint32_t>(push.size()), push.data());
    }
    vkCmdDispatch(cmd, groups[0], groups[1], groups[2]);
    return {};
}

void
GpuKernel::releaseDescriptors()
{
    std::lock_guard<std::mutex> lock(poolMutex_);
    // Resetting frees the sets that work which never finished may still read, so the pool is kept as it is.
    if (pool_ && !device_.hasAbandonedWork()) {
        vkResetDescriptorPool(device_.device(), pool_, 0);
    }
}

GpuStatus
GpuTimer::create(GpuDevice& device, std::unique_ptr<GpuTimer>& out)
{
    std::unique_ptr<GpuTimer> t(new GpuTimer(device));

    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device.physicalDevice(), &n, nullptr);
    std::vector<VkQueueFamilyProperties> fams(n);
    vkGetPhysicalDeviceQueueFamilyProperties(device.physicalDevice(), &n, fams.data());
    t->validBits_ = fams[device.queueFamily(QueueKind::Compute)].timestampValidBits;

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device.physicalDevice(), &props);
    t->periodNs_ = props.limits.timestampPeriod;

    if (t->validBits_ != 0) {
        VkQueryPoolCreateInfo qi { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 2;
        GpuStatus s = device.check(vkCreateQueryPool(device.device(), &qi, nullptr, &t->pool_),
                                   "vkCreateQueryPool");
        if (!s) {
            return s;
        }
    }
    out = std::move(t);
    return {};
}

GpuTimer::~GpuTimer()
{
    if (pool_) {
        vkDestroyQueryPool(device_.device(), pool_, nullptr);
    }
}

void
GpuTimer::begin(VkCommandBuffer cmd)
{
    if (!pool_) {
        return;
    }
    vkCmdResetQueryPool(cmd, pool_, 0, 2);
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool_, 0);
}

void
GpuTimer::end(VkCommandBuffer cmd)
{
    if (!pool_) {
        return;
    }
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool_, 1);
}

GpuStatus
GpuTimer::elapsedNs(double& ns)
{
    ns = 0;
    if (!pool_) {
        return {};
    }
    uint64_t v[2] = { 0, 0 };
    GpuStatus s = device_.check(vkGetQueryPoolResults(device_.device(), pool_, 0, 2, sizeof(v), v,
                                                      sizeof(uint64_t),
                                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
                                "vkGetQueryPoolResults");
    if (!s) {
        return s;
    }
    const uint64_t mask = validBits_ >= 64 ? ~0ull : ((1ull << validBits_) - 1);
    ns = static_cast<double>((v[1] - v[0]) & mask) * periodNs_;
    return {};
}

} // namespace gpu
