#include "GpuKernel.h"

#include <string>

namespace gpu {

namespace {

    GpuStatus fail(VkResult r, std::string msg)
    {
        return GpuStatus { r, std::move(msg) };
    }

    constexpr uint32_t kDescriptorPoolSets = 1024;

} // namespace

GpuStatus
GpuKernel::create(GpuDevice& device, const GpuKernelDesc& desc,
                  std::unique_ptr<GpuKernel>& out)
{
    if (device.isLost()) {
        return fail(VK_ERROR_DEVICE_LOST, "GpuKernel::create: device lost");
    }
    if (!desc.spirv || desc.spirvWords == 0 || desc.pushConstantBytes > kMaxPushConstantBytes
        || desc.pushConstantBytes % 4 != 0 || desc.groupSize[0] == 0 || desc.groupSize[1] == 0
        || desc.groupSize[2] == 0) {
        return fail(VK_ERROR_INITIALIZATION_FAILED, "GpuKernel::create: invalid kernel description");
    }

    std::unique_ptr<GpuKernel> k(new GpuKernel(device, desc));
    const VkDevice dev = device.device();
    const bool push = device.info().pushDescriptor;

    std::vector<VkDescriptorSetLayoutBinding> bindings(desc.storageBufferCount);
    for (uint32_t i = 0; i < desc.storageBufferCount; ++i) {
        bindings[i] = { i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    }
    VkDescriptorSetLayoutCreateInfo sli { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    sli.flags = push ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0;
    sli.bindingCount = desc.storageBufferCount;
    sli.pBindings = bindings.data();
    GpuStatus s = device.check(vkCreateDescriptorSetLayout(dev, &sli, nullptr, &k->setLayout_),
                               "vkCreateDescriptorSetLayout");
    if (!s) {
        return s;
    }

    VkPushConstantRange range { VK_SHADER_STAGE_COMPUTE_BIT, 0, desc.pushConstantBytes };
    VkPipelineLayoutCreateInfo pli { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &k->setLayout_;
    pli.pushConstantRangeCount = desc.pushConstantBytes ? 1 : 0;
    pli.pPushConstantRanges = &range;
    s = device.check(vkCreatePipelineLayout(dev, &pli, nullptr, &k->pipelineLayout_),
                     "vkCreatePipelineLayout");
    if (!s) {
        return s;
    }

    VkShaderModuleCreateInfo smi { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smi.codeSize = desc.spirvWords * sizeof(uint32_t);
    smi.pCode = desc.spirv;
    VkShaderModule module = VK_NULL_HANDLE;
    s = device.check(vkCreateShaderModule(dev, &smi, nullptr, &module), "vkCreateShaderModule");
    if (!s) {
        return s;
    }

    VkComputePipelineCreateInfo cpi { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                  VK_SHADER_STAGE_COMPUTE_BIT, module, desc.entry, nullptr };
    cpi.layout = k->pipelineLayout_;
    s = device.check(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &k->pipeline_),
                     "vkCreateComputePipelines");
    vkDestroyShaderModule(dev, module, nullptr);
    if (!s) {
        return s;
    }

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device.physicalDevice(), &props);
    for (size_t i = 0; i < 3; ++i) {
        k->maxGroups_[i] = props.limits.maxComputeWorkGroupCount[i];
    }

    if (push) {
        k->pushDescriptorFn_ = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(
            vkGetDeviceProcAddr(dev, "vkCmdPushDescriptorSetKHR"));
        if (!k->pushDescriptorFn_) {
            return fail(VK_ERROR_EXTENSION_NOT_PRESENT, "vkCmdPushDescriptorSetKHR unavailable");
        }
    } else if (desc.storageBufferCount > 0) {
        VkDescriptorPoolSize size { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                    kDescriptorPoolSets * desc.storageBufferCount };
        VkDescriptorPoolCreateInfo dpi { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpi.maxSets = kDescriptorPoolSets;
        dpi.poolSizeCount = 1;
        dpi.pPoolSizes = &size;
        s = device.check(vkCreateDescriptorPool(dev, &dpi, nullptr, &k->pool_),
                         "vkCreateDescriptorPool");
        if (!s) {
            return s;
        }
    }

    out = std::move(k);
    return {};
}

GpuKernel::~GpuKernel()
{
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
    if (buffers.size() != desc_.storageBufferCount || push.size() != desc_.pushConstantBytes) {
        return fail(VK_ERROR_INITIALIZATION_FAILED, "GpuKernel::record: binding or push size mismatch");
    }
    for (size_t i = 0; i < 3; ++i) {
        if (groups[i] > maxGroups_[i]) {
            return fail(VK_ERROR_FEATURE_NOT_PRESENT,
                        "GpuKernel::record: " + std::to_string(groups[i]) + " groups along axis "
                            + std::to_string(i) + " exceed the device limit of " + std::to_string(maxGroups_[i]));
        }
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

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    if (pushDescriptorFn_) {
        pushDescriptorFn_(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0,
                          static_cast<uint32_t>(writes.size()), writes.data());
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
    if (pool_) {
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
