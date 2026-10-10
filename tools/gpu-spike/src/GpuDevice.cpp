#include "GpuDevice.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace gpu {

namespace {

GpuStatus fail(VkResult r, std::string msg)
{
    return GpuStatus{r, std::move(msg)};
}

std::string lower(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool hasName(const std::vector<VkExtensionProperties>& exts, const char* name)
{
    return std::any_of(exts.begin(), exts.end(), [&](const VkExtensionProperties& e) {
        return std::strcmp(e.extensionName, name) == 0;
    });
}

int typeRank(VkPhysicalDeviceType t)
{
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 0;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 1;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return 3;
    default: return 4;
    }
}

struct Features
{
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};

    Features()
    {
        f2.pNext = &f11;
        f11.pNext = &f12;
        f12.pNext = &f13;
    }
    Features(const Features&) = delete;
    Features& operator=(const Features&) = delete;
};

bool meetsRequirements(VkPhysicalDevice pd, const VkPhysicalDeviceProperties& props)
{
    if (props.apiVersion < VK_API_VERSION_1_3) {
        return false;
    }
    Features f;
    vkGetPhysicalDeviceFeatures2(pd, &f.f2);
    return f.f12.timelineSemaphore && f.f13.synchronization2 && f.f11.storageBuffer16BitAccess;
}

GpuStatus enumeratePhysical(VkInstance inst, std::vector<VkPhysicalDevice>& out)
{
    uint32_t n = 0;
    VkResult r = vkEnumeratePhysicalDevices(inst, &n, nullptr);
    if (r != VK_SUCCESS) {
        return fail(r, "vkEnumeratePhysicalDevices failed");
    }
    out.resize(n);
    r = vkEnumeratePhysicalDevices(inst, &n, out.data());
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
        return fail(r, "vkEnumeratePhysicalDevices failed");
    }
    out.resize(n);
    return {};
}

bool layerPresent(const char* name)
{
    uint32_t n = 0;
    if (vkEnumerateInstanceLayerProperties(&n, nullptr) != VK_SUCCESS) {
        return false;
    }
    std::vector<VkLayerProperties> layers(n);
    if (vkEnumerateInstanceLayerProperties(&n, layers.data()) < 0) {
        return false;
    }
    return std::any_of(layers.begin(), layers.end(), [&](const VkLayerProperties& l) {
        return std::strcmp(l.layerName, name) == 0;
    });
}

GpuStatus createInstance(bool wantValidation, VkInstance& inst, bool& validationOn)
{
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "natron-gpu";
    app.apiVersion = VK_API_VERSION_1_3;

    static const char* kValidation = "VK_LAYER_KHRONOS_validation";
    validationOn = wantValidation && layerPresent(kValidation);

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    if (validationOn) {
        ci.enabledLayerCount = 1;
        ci.ppEnabledLayerNames = &kValidation;
    }
    VkResult r = vkCreateInstance(&ci, nullptr, &inst);
    if (r != VK_SUCCESS) {
        return fail(r, "vkCreateInstance failed");
    }
    return {};
}

} // namespace

GpuStatus GpuDevice::listDevices(std::vector<GpuPhysicalDeviceDesc>& out)
{
    out.clear();
    VkInstance inst = VK_NULL_HANDLE;
    bool v = false;
    if (GpuStatus s = createInstance(false, inst, v); !s) {
        return s;
    }
    std::vector<VkPhysicalDevice> pds;
    GpuStatus s = enumeratePhysical(inst, pds);
    if (s) {
        for (uint32_t i = 0; i < pds.size(); ++i) {
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(pds[i], &p);
            out.push_back({i, p.deviceName, p.deviceType, meetsRequirements(pds[i], p)});
        }
    }
    vkDestroyInstance(inst, nullptr);
    return s;
}

GpuStatus GpuDevice::create(const GpuDeviceOptions& options, std::unique_ptr<GpuDevice>& out)
{
    out.reset();
    std::unique_ptr<GpuDevice> dev(new GpuDevice);

    if (GpuStatus s = createInstance(options.enableValidation, dev->instance_,
                                     dev->info_.validationEnabled); !s) {
        return s;
    }

    std::vector<VkPhysicalDevice> pds;
    if (GpuStatus s = enumeratePhysical(dev->instance_, pds); !s) {
        return s;
    }
    if (pds.empty()) {
        return fail(VK_ERROR_INITIALIZATION_FAILED, "no Vulkan physical devices");
    }

    std::string selector = options.deviceSelector;
    if (selector.empty()) {
        if (const char* env = std::getenv("NATRON_GPU_DEVICE")) {
            selector = env;
        }
    }

    std::vector<VkPhysicalDeviceProperties> props(pds.size());
    for (size_t i = 0; i < pds.size(); ++i) {
        vkGetPhysicalDeviceProperties(pds[i], &props[i]);
    }

    int pick = -1;
    if (!selector.empty()) {
        const bool numeric = std::all_of(selector.begin(), selector.end(),
                                         [](unsigned char c) { return std::isdigit(c); });
        if (numeric) {
            const unsigned long idx = std::strtoul(selector.c_str(), nullptr, 10);
            if (idx < pds.size()) {
                pick = static_cast<int>(idx);
            }
        } else {
            const std::string needle = lower(selector);
            for (size_t i = 0; i < pds.size(); ++i) {
                if (lower(props[i].deviceName).find(needle) != std::string::npos) {
                    pick = static_cast<int>(i);
                    break;
                }
            }
        }
        if (pick < 0) {
            return fail(VK_ERROR_INITIALIZATION_FAILED,
                        "NATRON_GPU_DEVICE '" + selector + "' matches no device");
        }
        if (!meetsRequirements(pds[pick], props[pick])) {
            return fail(VK_ERROR_FEATURE_NOT_PRESENT,
                        std::string("device '") + props[pick].deviceName
                            + "' lacks Vulkan 1.3, timelineSemaphore, synchronization2 or storageBuffer16BitAccess");
        }
    } else {
        int bestRank = 99;
        for (size_t i = 0; i < pds.size(); ++i) {
            if (!meetsRequirements(pds[i], props[i])) {
                continue;
            }
            const int rank = typeRank(props[i].deviceType);
            if (rank < bestRank) {
                bestRank = rank;
                pick = static_cast<int>(i);
            }
        }
        if (pick < 0) {
            return fail(VK_ERROR_FEATURE_NOT_PRESENT, "no device meets the required features");
        }
    }

    dev->physical_ = pds[pick];
    const VkPhysicalDeviceProperties& pp = props[pick];
    dev->info_.name = pp.deviceName;
    dev->info_.type = pp.deviceType;
    dev->info_.apiVersion = pp.apiVersion;
    dev->info_.physicalDeviceIndex = static_cast<uint32_t>(pick);

    uint32_t nf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev->physical_, &nf, nullptr);
    std::vector<VkQueueFamilyProperties> fams(nf);
    vkGetPhysicalDeviceQueueFamilyProperties(dev->physical_, &nf, fams.data());

    // Prefer a compute family without graphics so compute work does not share a hardware queue with it.
    int compute = -1;
    for (uint32_t i = 0; i < nf; ++i) {
        if (!(fams[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            continue;
        }
        if (compute < 0 || !(fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            compute = static_cast<int>(i);
            if (!(fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                break;
            }
        }
    }
    if (compute < 0) {
        return fail(VK_ERROR_FEATURE_NOT_PRESENT, "no compute queue family");
    }
    int transfer = -1;
    for (uint32_t i = 0; i < nf; ++i) {
        if ((fams[i].queueFlags & VK_QUEUE_TRANSFER_BIT)
            && !(fams[i].queueFlags & (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT))) {
            transfer = static_cast<int>(i);
            break;
        }
    }
    dev->info_.computeFamily = static_cast<uint32_t>(compute);
    dev->info_.transferUsesComputeQueue = transfer < 0;
    dev->info_.transferFamily = transfer < 0 ? dev->info_.computeFamily : static_cast<uint32_t>(transfer);

    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(dev->physical_, nullptr, &ne, nullptr);
    std::vector<VkExtensionProperties> exts(ne);
    vkEnumerateDeviceExtensionProperties(dev->physical_, nullptr, &ne, exts.data());

    std::vector<const char*> enabled;
    auto optional = [&](const char* name, bool& flag) {
        flag = hasName(exts, name);
        if (flag) {
            enabled.push_back(name);
        }
    };
    optional(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, dev->info_.externalMemoryFd);
    optional(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME, dev->info_.externalSemaphoreFd);
    optional(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME, dev->info_.externalMemoryHost);
    optional(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME, dev->info_.memoryBudget);
    optional(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, dev->info_.pushDescriptor);

    Features avail;
    vkGetPhysicalDeviceFeatures2(dev->physical_, &avail.f2);
    dev->info_.shaderFloat16 = avail.f12.shaderFloat16 == VK_TRUE;

    Features want;
    want.f11.storageBuffer16BitAccess = VK_TRUE;
    want.f12.timelineSemaphore = VK_TRUE;
    want.f12.shaderFloat16 = avail.f12.shaderFloat16;
    want.f13.synchronization2 = VK_TRUE;

    const float prio = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> qcis;
    auto addQueue = [&](uint32_t family) {
        VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        q.queueFamilyIndex = family;
        q.queueCount = 1;
        q.pQueuePriorities = &prio;
        qcis.push_back(q);
    };
    addQueue(dev->info_.computeFamily);
    if (!dev->info_.transferUsesComputeQueue) {
        addQueue(dev->info_.transferFamily);
    }

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &want.f2;
    dci.queueCreateInfoCount = static_cast<uint32_t>(qcis.size());
    dci.pQueueCreateInfos = qcis.data();
    dci.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
    dci.ppEnabledExtensionNames = enabled.data();
    VkResult r = vkCreateDevice(dev->physical_, &dci, nullptr, &dev->device_);
    if (r != VK_SUCCESS) {
        return fail(r, "vkCreateDevice failed");
    }

    vkGetDeviceQueue(dev->device_, dev->info_.computeFamily, 0, &dev->computeQueue_.handle);
    if (!dev->info_.transferUsesComputeQueue) {
        vkGetDeviceQueue(dev->device_, dev->info_.transferFamily, 0, &dev->transferQueue_.handle);
    }

    VmaAllocatorCreateInfo aci{};
    aci.instance = dev->instance_;
    aci.physicalDevice = dev->physical_;
    aci.device = dev->device_;
    aci.vulkanApiVersion = VK_API_VERSION_1_3;
    if (dev->info_.memoryBudget) {
        aci.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
    }
    r = vmaCreateAllocator(&aci, &dev->allocator_);
    if (r != VK_SUCCESS) {
        return fail(r, "vmaCreateAllocator failed");
    }

    const VkPhysicalDeviceMemoryProperties* mp = nullptr;
    vmaGetMemoryProperties(dev->allocator_, &mp);
    dev->memoryHeapCount_ = mp->memoryHeapCount;

    out = std::move(dev);
    return {};
}

GpuDevice::~GpuDevice()
{
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        for (auto& [key, pool] : pools_) {
            vkDestroyCommandPool(device_, pool, nullptr);
        }
    }
    if (allocator_) {
        vmaDestroyAllocator(allocator_);
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
    }
}

uint32_t GpuDevice::queueFamily(QueueKind kind) const
{
    return kind == QueueKind::Compute ? info_.computeFamily : info_.transferFamily;
}

GpuDevice::Queue& GpuDevice::queueFor(QueueKind kind)
{
    if (kind == QueueKind::Transfer && !info_.transferUsesComputeQueue) {
        return transferQueue_;
    }
    return computeQueue_;
}

GpuStatus GpuDevice::lostStatus() const
{
    return fail(VK_ERROR_DEVICE_LOST, "device is lost");
}

GpuStatus GpuDevice::check(VkResult result, const char* what)
{
    if (result == VK_ERROR_DEVICE_LOST) {
        lost_.store(true, std::memory_order_release);
    }
    if (result < 0) {
        return fail(result, std::string(what) + " failed");
    }
    return {};
}

GpuStatus GpuDevice::commandPool(QueueKind kind, VkCommandPool& out)
{
    if (isLost()) {
        return lostStatus();
    }
    const uint32_t family = queueFamily(kind);
    const auto key = std::make_pair(std::this_thread::get_id(), family);
    std::lock_guard<std::mutex> lock(poolMutex_);
    if (auto it = pools_.find(key); it != pools_.end()) {
        out = it->second;
        return {};
    }
    VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ci.queueFamilyIndex = family;
    VkCommandPool pool = VK_NULL_HANDLE;
    if (GpuStatus s = check(vkCreateCommandPool(device_, &ci, nullptr, &pool), "vkCreateCommandPool"); !s) {
        return s;
    }
    pools_.emplace(key, pool);
    out = pool;
    return {};
}

GpuStatus GpuDevice::submit(QueueKind kind, std::span<const VkSubmitInfo2> submits, VkFence fence)
{
    if (isLost()) {
        return lostStatus();
    }
    Queue& q = queueFor(kind);
    std::lock_guard<std::mutex> lock(q.mutex);
    return check(vkQueueSubmit2(q.handle, static_cast<uint32_t>(submits.size()), submits.data(), fence),
                 "vkQueueSubmit2");
}

GpuStatus GpuDevice::createTimelineSemaphore(uint64_t initialValue, VkSemaphore& out)
{
    if (isLost()) {
        return lostStatus();
    }
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type.initialValue = initialValue;
    VkSemaphoreCreateInfo ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    ci.pNext = &type;
    return check(vkCreateSemaphore(device_, &ci, nullptr, &out), "vkCreateSemaphore");
}

GpuStatus GpuDevice::waitSemaphore(VkSemaphore semaphore, uint64_t value, uint64_t timeoutNs)
{
    if (isLost()) {
        return lostStatus();
    }
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &semaphore;
    wi.pValues = &value;
    const VkResult r = vkWaitSemaphores(device_, &wi, timeoutNs);
    if (r == VK_TIMEOUT) {
        return fail(VK_TIMEOUT, "vkWaitSemaphores timed out");
    }
    return check(r, "vkWaitSemaphores");
}

GpuStatus GpuDevice::waitIdle()
{
    if (isLost()) {
        return lostStatus();
    }
    return check(vkDeviceWaitIdle(device_), "vkDeviceWaitIdle");
}

GpuStatus GpuDevice::queryBudget(std::vector<VmaBudget>& out)
{
    if (isLost()) {
        return lostStatus();
    }
    out.assign(memoryHeapCount_, VmaBudget{});
    vmaGetHeapBudgets(allocator_, out.data());
    return {};
}

} // namespace gpu
