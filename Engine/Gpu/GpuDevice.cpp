#include "GpuDevice.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>

namespace gpu {

namespace {

    GpuStatus fail(VkResult r, std::string msg)
    {
        return GpuStatus { r, std::move(msg) };
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
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            return 0;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            return 1;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            return 2;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:
            return 3;
        default:
            return 4;
        }
    }

    const char* typeName(VkPhysicalDeviceType t)
    {
        switch (t) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            return "discrete";
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            return "integrated";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            return "virtual";
        case VK_PHYSICAL_DEVICE_TYPE_CPU:
            return "cpu";
        default:
            return "other";
        }
    }

    std::string sizeText(uint64_t bytes)
    {
        constexpr uint64_t kGiB = 1ull << 30;
        constexpr uint64_t kMiB = 1ull << 20;
        if (bytes >= kGiB) {
            return std::to_string((bytes + kGiB / 2) / kGiB) + " GiB";
        }
        return std::to_string((bytes + kMiB / 2) / kMiB) + " MiB";
    }

    // NVIDIA packs driverVersion as 10.8.8.6 bits rather than the Vulkan 7.10.12 layout.
    std::string driverVersionText(const VkPhysicalDeviceProperties& p)
    {
        const uint32_t v = p.driverVersion;
        if (p.vendorID == 0x10DE) {
            return std::to_string(v >> 22) + "." + std::to_string((v >> 14) & 0xff) + "."
                + std::to_string((v >> 6) & 0xff);
        }
        return std::to_string(VK_API_VERSION_MAJOR(v)) + "." + std::to_string(VK_API_VERSION_MINOR(v)) + "."
            + std::to_string(VK_API_VERSION_PATCH(v));
    }

    uint64_t largestDeviceLocalHeap(VkPhysicalDevice pd)
    {
        VkPhysicalDeviceMemoryProperties mem {};
        vkGetPhysicalDeviceMemoryProperties(pd, &mem);
        uint64_t localHeap = 0;
        for (uint32_t i = 0; i < mem.memoryHeapCount; ++i) {
            if (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                localHeap = std::max<uint64_t>(localHeap, mem.memoryHeaps[i].size);
            }
        }
        return localHeap;
    }

    struct Features {
        VkPhysicalDeviceVulkan11Features f11 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
        VkPhysicalDeviceVulkan12Features f12 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        VkPhysicalDeviceVulkan13Features f13 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        VkPhysicalDeviceFeatures2 f2 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };

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

    bool instanceExtensionPresent(const char* layer, const char* name)
    {
        uint32_t n = 0;
        if (vkEnumerateInstanceExtensionProperties(layer, &n, nullptr) != VK_SUCCESS) {
            return false;
        }
        std::vector<VkExtensionProperties> exts(n);
        if (vkEnumerateInstanceExtensionProperties(layer, &n, exts.data()) < 0) {
            return false;
        }
        return hasName(exts, name);
    }

    VKAPI_ATTR VkBool32 VKAPI_CALL forwardDebugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                       VkDebugUtilsMessageTypeFlagsEXT types,
                                                       const VkDebugUtilsMessengerCallbackDataEXT* data, void* user)
    {
        const auto* fn = static_cast<const GpuDebugMessageFn*>(user);
        (*fn)(severity, types, data && data->pMessage ? data->pMessage : "");
        return VK_FALSE;
    }

    VkDebugUtilsMessengerCreateInfoEXT messengerInfo(const GpuDebugMessageFn& fn)
    {
        VkDebugUtilsMessengerCreateInfoEXT ci { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
        ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
            | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        ci.pfnUserCallback = forwardDebugMessage;
        ci.pUserData = const_cast<GpuDebugMessageFn*>(&fn);
        return ci;
    }

    bool envFlag(const char* name)
    {
        const char* v = std::getenv(name);
        return v && std::strcmp(v, "1") == 0;
    }

    // RADV exposes its SDMA engines as a transfer-only queue family only under RADV_PERFTEST=transfer_queue, which it
    // reads as each instance is created. Copies there run several times faster than on the compute queue and stop
    // contending with kernels; other drivers ignore the variable.
    void requestTransferOnlyFamily()
    {
#ifndef _WIN32
        static std::once_flag once;
        std::call_once(once, [] {
            const char* current = std::getenv("RADV_PERFTEST");
            const std::string value = withPerftestToken(current, "transfer_queue");
            if (!current || value != current) {
                setenv("RADV_PERFTEST", value.c_str(), 1);
            }
        });
#endif
    }

    GpuStatus createInstance(bool wantValidation, const GpuDebugMessageFn& debug, VkInstance& inst,
                             bool& validationOn, bool& messengerOn)
    {
        requestTransferOnlyFamily();
        VkApplicationInfo app { VK_STRUCTURE_TYPE_APPLICATION_INFO };
        app.pApplicationName = "natron-gpu";
        app.apiVersion = VK_API_VERSION_1_3;

        static const char* kValidation = "VK_LAYER_KHRONOS_validation";
        validationOn = wantValidation && layerPresent(kValidation);
        messengerOn = debug && (instanceExtensionPresent(nullptr, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) || (validationOn && instanceExtensionPresent(kValidation, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)));

        VkInstanceCreateInfo ci { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        ci.pApplicationInfo = &app;
        if (validationOn) {
            ci.enabledLayerCount = 1;
            ci.ppEnabledLayerNames = &kValidation;
        }
        static const char* kDebugUtils = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        // Chained so messages from vkCreateInstance and vkDestroyInstance reach the callback too.
        VkDebugUtilsMessengerCreateInfoEXT mci {};
        if (messengerOn) {
            mci = messengerInfo(debug);
            ci.pNext = &mci;
            ci.enabledExtensionCount = 1;
            ci.ppEnabledExtensionNames = &kDebugUtils;
        }
        VkResult r = vkCreateInstance(&ci, nullptr, &inst);
        if (r != VK_SUCCESS) {
            return fail(r, "vkCreateInstance failed");
        }
        return {};
    }

} // namespace

bool
isUnifiedMemory(VkPhysicalDeviceType type)
{
    return (type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) || (type == VK_PHYSICAL_DEVICE_TYPE_CPU);
}

std::optional<GpuAutoPick>
pickAutoDevice(const std::vector<GpuDeviceCandidate>& candidates)
{
    std::vector<size_t> order;
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (candidates[i].meetsRequirements) {
            order.push_back(i);
        }
    }
    if (order.empty()) {
        return std::nullopt;
    }
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const GpuDeviceCandidate& x = candidates[a];
        const GpuDeviceCandidate& y = candidates[b];
        if (typeRank(x.type) != typeRank(y.type)) {
            return typeRank(x.type) < typeRank(y.type);
        }
        if (x.deviceLocalHeapSize != y.deviceLocalHeapSize) {
            return x.deviceLocalHeapSize > y.deviceLocalHeapSize;
        }
        return x.index < y.index;
    });

    const GpuDeviceCandidate& best = candidates[order.front()];
    GpuAutoPick pick;
    pick.candidate = order.front();
    pick.reason = std::string("auto: ") + typeName(best.type) + ", " + sizeText(best.deviceLocalHeapSize);
    for (size_t k = 1; k < order.size(); ++k) {
        const GpuDeviceCandidate& other = candidates[order[k]];
        pick.reason += (k == 1 ? ", over " : ", ") + other.name;
        if (other.type == best.type) {
            pick.reason += " (" + sizeText(other.deviceLocalHeapSize) + ")";
        } else {
            pick.reason += std::string(" (") + typeName(other.type) + ")";
        }
    }
    return pick;
}

std::string
withPerftestToken(const char* current, std::string_view token)
{
    std::string value = current ? current : "";
    // Mesa splits these lists on commas and spaces alike.
    std::string_view rest = value;
    while (!rest.empty()) {
        const size_t skip = rest.find_first_not_of(", ");
        if (skip == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(skip);
        const size_t end = rest.find_first_of(", ");
        if (rest.substr(0, end) == token) {
            return value;
        }
        rest = (end == std::string_view::npos) ? std::string_view() : rest.substr(end);
    }
    if (!value.empty()) {
        value += ',';
    }
    value += token;
    return value;
}

std::optional<GpuQueuePlan>
planQueues(std::span<const VkQueueFamilyProperties> families)
{
    // Prefer a compute family without graphics so compute work does not share a hardware queue with it.
    int compute = -1;
    for (size_t i = 0; i < families.size(); ++i) {
        if (!(families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            continue;
        }
        if (compute < 0 || !(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            compute = static_cast<int>(i);
            if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                break;
            }
        }
    }
    if (compute < 0) {
        return std::nullopt;
    }
    int transfer = -1;
    for (size_t i = 0; i < families.size(); ++i) {
        if ((families[i].queueFlags & VK_QUEUE_TRANSFER_BIT)
            && !(families[i].queueFlags & (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT))) {
            transfer = static_cast<int>(i);
            break;
        }
    }
    // Uploads and downloads get queues of their own, so that both overlap compute and each other, as far as the
    // families have queues to give: without a transfer-only family they come out of the compute family, and with
    // only two queues there they share the second.
    GpuQueuePlan plan;
    plan.computeFamily = static_cast<uint32_t>(compute);
    plan.computeQueueIndex = 0;
    plan.computeQueues = 1;
    if (transfer >= 0) {
        plan.transferFamily = static_cast<uint32_t>(transfer);
        plan.transferQueues = std::min<uint32_t>(std::max<uint32_t>(families[transfer].queueCount, 1), 2);
        plan.uploadQueueIndex = 0;
        plan.downloadQueueIndex = plan.transferQueues - 1;
    } else {
        plan.transferFamily = plan.computeFamily;
        plan.computeQueues = std::min<uint32_t>(std::max<uint32_t>(families[compute].queueCount, 1), 3);
        plan.uploadQueueIndex = plan.computeQueues > 1 ? 1 : 0;
        plan.downloadQueueIndex = plan.computeQueues - 1;
    }
    plan.transferSharesComputeQueue = transfer < 0 && plan.computeQueues == 1;
    return plan;
}

GpuStatus
GpuDevice::create(const GpuDeviceOptions& options, std::unique_ptr<GpuDevice>& out)
{
    out.reset();
    std::unique_ptr<GpuDevice> dev(new GpuDevice);
    dev->debugMessage_ = options.debugMessage;
    if (options.fault.kind != GpuFaultPlan::Kind::Off) {
        dev->faultKind_.store(options.fault.kind);
        dev->faultAt_.store(options.fault.atOperation);
    }
    dev->onDeviceLost_ = options.onDeviceLost;

    if (GpuStatus s = createInstance(options.enableValidation || envFlag("NATRON_GPU_VALIDATION"),
                                     dev->debugMessage_, dev->instance_, dev->info_.validationEnabled,
                                     dev->info_.debugMessenger);
        !s) {
        return s;
    }
    if (dev->info_.debugMessenger) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(dev->instance_, "vkCreateDebugUtilsMessengerEXT"));
        const VkDebugUtilsMessengerCreateInfoEXT mci = messengerInfo(dev->debugMessage_);
        if (!create) {
            return fail(VK_ERROR_EXTENSION_NOT_PRESENT, "vkCreateDebugUtilsMessengerEXT unavailable");
        }
        if (VkResult r = create(dev->instance_, &mci, nullptr, &dev->messenger_); r != VK_SUCCESS) {
            return fail(r, "vkCreateDebugUtilsMessengerEXT failed");
        }
    }

    std::vector<VkPhysicalDevice> pds;
    if (GpuStatus s = enumeratePhysical(dev->instance_, pds); !s) {
        return s;
    }
    if (pds.empty()) {
        return fail(VK_ERROR_INITIALIZATION_FAILED, "no Vulkan physical devices");
    }

    std::string selector = options.deviceSelector;
    std::string selectionReason;
    if (!selector.empty()) {
        selectionReason = "device selector " + selector;
    } else if (const char* env = std::getenv("NATRON_GPU_DEVICE")) {
        selector = env;
        selectionReason = std::string("NATRON_GPU_DEVICE=") + env;
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
        std::vector<GpuDeviceCandidate> candidates(pds.size());
        for (size_t i = 0; i < pds.size(); ++i) {
            candidates[i].index = static_cast<uint32_t>(i);
            candidates[i].type = props[i].deviceType;
            candidates[i].deviceLocalHeapSize = largestDeviceLocalHeap(pds[i]);
            candidates[i].name = props[i].deviceName;
            candidates[i].meetsRequirements = meetsRequirements(pds[i], props[i]);
        }
        if (const std::optional<GpuAutoPick> best = pickAutoDevice(candidates)) {
            pick = static_cast<int>(candidates[best->candidate].index);
            selectionReason = best->reason;
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
    dev->info_.driverVersion = driverVersionText(pp);
    dev->info_.selectionReason = std::move(selectionReason);

    uint32_t nf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev->physical_, &nf, nullptr);
    std::vector<VkQueueFamilyProperties> fams(nf);
    vkGetPhysicalDeviceQueueFamilyProperties(dev->physical_, &nf, fams.data());

    const std::optional<GpuQueuePlan> queues = planQueues(fams);
    if (!queues) {
        return fail(VK_ERROR_FEATURE_NOT_PRESENT, "no compute queue family");
    }
    dev->info_.computeFamily = queues->computeFamily;
    dev->info_.transferFamily = queues->transferFamily;
    dev->info_.computeQueueIndex = queues->computeQueueIndex;
    dev->info_.uploadQueueIndex = queues->uploadQueueIndex;
    dev->info_.downloadQueueIndex = queues->downloadQueueIndex;
    dev->info_.transferSharesComputeQueue = queues->transferSharesComputeQueue;

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
    if (!options.disablePushDescriptor) {
        optional(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, dev->info_.pushDescriptor);
    }

    {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostProps {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT
        };
        VkPhysicalDeviceVulkan11Properties p11 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES };
        VkPhysicalDeviceProperties2 p2 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        p2.pNext = &p11;
        if (dev->info_.externalMemoryHost) {
            p11.pNext = &hostProps;
        }
        vkGetPhysicalDeviceProperties2(dev->physical_, &p2);

        uint64_t range = p2.properties.limits.maxStorageBufferRange;
        if (options.maxStorageBufferRange != 0) {
            range = std::min<uint64_t>(range, options.maxStorageBufferRange);
        }
        dev->info_.maxStorageBufferRange = range;
        dev->info_.copyChunkSize = std::max<uint64_t>(options.copyChunkSize, 1);
        dev->info_.maxMemoryAllocationSize = p11.maxMemoryAllocationSize;
        dev->info_.minImportedHostPointerAlignment
            = dev->info_.externalMemoryHost ? hostProps.minImportedHostPointerAlignment : 0;

        dev->info_.deviceLocalHeapSize = largestDeviceLocalHeap(dev->physical_);
        dev->info_.unifiedMemory = isUnifiedMemory(dev->info_.type);
    }

    Features avail;
    vkGetPhysicalDeviceFeatures2(dev->physical_, &avail.f2);
    dev->info_.shaderFloat16 = avail.f12.shaderFloat16 == VK_TRUE;

    Features want;
    want.f11.storageBuffer16BitAccess = VK_TRUE;
    want.f12.timelineSemaphore = VK_TRUE;
    want.f12.shaderFloat16 = avail.f12.shaderFloat16;
    want.f13.synchronization2 = VK_TRUE;

    const float prios[3] = { 1.0f, 1.0f, 1.0f };
    std::vector<VkDeviceQueueCreateInfo> qcis;
    auto addQueues = [&](uint32_t family, uint32_t count) {
        VkDeviceQueueCreateInfo q { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        q.queueFamilyIndex = family;
        q.queueCount = count;
        q.pQueuePriorities = prios;
        qcis.push_back(q);
    };
    addQueues(queues->computeFamily, queues->computeQueues);
    if (queues->transferQueues > 0) {
        addQueues(queues->transferFamily, queues->transferQueues);
    }

    VkDeviceCreateInfo dci { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext = &want.f2;
    dci.queueCreateInfoCount = static_cast<uint32_t>(qcis.size());
    dci.pQueueCreateInfos = qcis.data();
    dci.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
    dci.ppEnabledExtensionNames = enabled.data();
    VkResult r = vkCreateDevice(dev->physical_, &dci, nullptr, &dev->device_);
    if (r != VK_SUCCESS) {
        return fail(r, "vkCreateDevice failed");
    }

    {
        const struct {
            QueueKind kind;
            uint32_t family;
            uint32_t index;
        } routes[] = {
            { QueueKind::Compute, dev->info_.computeFamily, dev->info_.computeQueueIndex },
            { QueueKind::Upload, dev->info_.transferFamily, dev->info_.uploadQueueIndex },
            { QueueKind::Download, dev->info_.transferFamily, dev->info_.downloadQueueIndex },
        };
        size_t used = 0;
        for (const auto& r : routes) {
            VkQueue handle = VK_NULL_HANDLE;
            vkGetDeviceQueue(dev->device_, r.family, r.index, &handle);
            Queue* q = nullptr;
            for (size_t i = 0; i < used; ++i) {
                if (dev->queues_[i].handle == handle) {
                    q = &dev->queues_[i];
                }
            }
            if (!q) {
                q = &dev->queues_[used++];
                q->handle = handle;
            }
            dev->route_[static_cast<size_t>(r.kind)] = q;
        }
    }

    VmaAllocatorCreateInfo aci {};
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
    if (hasAbandonedWork()) {
        return;
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }
    if (allocator_) {
        vmaDestroyAllocator(allocator_);
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
    }
    if (messenger_ != VK_NULL_HANDLE) {
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy) {
            destroy(instance_, messenger_, nullptr);
        }
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
    }
}

uint32_t
GpuDevice::queueFamily(QueueKind kind) const
{
    return kind == QueueKind::Compute ? info_.computeFamily : info_.transferFamily;
}

GpuDevice::Queue&
GpuDevice::queueFor(QueueKind kind)
{
    return *route_[static_cast<size_t>(kind)];
}

GpuStatus
GpuDevice::lostStatus() const
{
    return fail(VK_ERROR_DEVICE_LOST, "device is lost");
}

GpuStatus
GpuDevice::check(VkResult result, const char* what)
{
    if (result == VK_ERROR_DEVICE_LOST && !lost_.exchange(true, std::memory_order_acq_rel) && onDeviceLost_) {
        onDeviceLost_();
    }
    if (result < 0) {
        return fail(result, std::string(what) + " failed");
    }
    return {};
}

GpuStatus
GpuDevice::countOperation()
{
    const uint64_t n = operations_.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint64_t at = faultAt_.load(std::memory_order_acquire);
    if (at == 0 || n != at) {
        return {};
    }
    const GpuFaultPlan::Kind kind = faultKind_.load(std::memory_order_acquire);
    if (kind == GpuFaultPlan::Kind::Off) {
        return {};
    }
    if (kind == GpuFaultPlan::Kind::DeviceLost) {
        return check(VK_ERROR_DEVICE_LOST, "injected fault");
    }
    return check(VK_ERROR_OUT_OF_DEVICE_MEMORY, "injected fault");
}

void
GpuDevice::injectFault(GpuFaultPlan::Kind kind, uint64_t inOperations)
{
    if (kind == GpuFaultPlan::Kind::Off || inOperations == 0) {
        faultAt_.store(0, std::memory_order_release);
        faultKind_.store(GpuFaultPlan::Kind::Off, std::memory_order_release);
        return;
    }
    faultKind_.store(kind, std::memory_order_release);
    faultAt_.store(operations_.load(std::memory_order_relaxed) + inOperations, std::memory_order_release);
}

void
GpuDevice::drainQueues()
{
    if ((device_ == VK_NULL_HANDLE) || hasAbandonedWork()) {
        return;
    }
    waitIdleLocked();
}

void
GpuDevice::abandonInFlightWork(const char* what)
{
    // Set before the loss is latched, so that whoever sees the loss also sees there is nothing to drain.
    abandoned_.store(true, std::memory_order_release);
    check(VK_ERROR_DEVICE_LOST, what);
}

void
GpuDevice::parkForever(std::shared_ptr<void> object)
{
    // Heap-allocated and never freed, so that exit runs no destructor on what is parked.
    static std::mutex* const mutex = new std::mutex;
    static std::vector<std::shared_ptr<void>>* const parked = new std::vector<std::shared_ptr<void>>;
    std::lock_guard<std::mutex> lock(*mutex);
    parked->push_back(std::move(object));
}

VkResult
GpuDevice::waitIdleLocked()
{
    // Vulkan requires every queue of the device to be externally synchronized across vkDeviceWaitIdle.
    std::scoped_lock lock(queues_[0].mutex, queues_[1].mutex, queues_[2].mutex);
    return vkDeviceWaitIdle(device_);
}

bool
GpuFaultPlan::parse(const std::string& text, GpuFaultPlan& out)
{
    const size_t at = text.find('@');
    if (at == std::string::npos) {
        return false;
    }
    const std::string kind = text.substr(0, at);
    const std::string num = text.substr(at + 1);
    if (num.empty() || num.find_first_not_of("0123456789") != std::string::npos || num.size() > 18) {
        return false;
    }
    const uint64_t n = std::stoull(num);
    if (n == 0) {
        return false;
    }
    if (kind == "lost") {
        out = { Kind::DeviceLost, n };
    } else if (kind == "oom") {
        out = { Kind::OutOfMemory, n };
    } else {
        return false;
    }
    return true;
}

GpuStatus
GpuDevice::submit(QueueKind kind, std::span<const VkSubmitInfo2> submits, VkFence fence)
{
    if (isLost()) {
        return lostStatus();
    }
    if (GpuStatus injected = countOperation(); !injected) {
        return injected;
    }
    return submitUncounted(kind, submits, fence);
}

GpuStatus
GpuDevice::submitUncounted(QueueKind kind, std::span<const VkSubmitInfo2> submits, VkFence fence)
{
    if (isLost()) {
        return lostStatus();
    }
    Queue& q = queueFor(kind);
    std::lock_guard<std::mutex> lock(q.mutex);
    return check(vkQueueSubmit2(q.handle, static_cast<uint32_t>(submits.size()), submits.data(), fence),
                 "vkQueueSubmit2");
}

GpuStatus
GpuDevice::createTimelineSemaphore(uint64_t initialValue, VkSemaphore& out)
{
    if (isLost()) {
        return lostStatus();
    }
    VkSemaphoreTypeCreateInfo type { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type.initialValue = initialValue;
    VkSemaphoreCreateInfo ci { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    ci.pNext = &type;
    return check(vkCreateSemaphore(device_, &ci, nullptr, &out), "vkCreateSemaphore");
}

GpuStatus
GpuDevice::waitSemaphore(VkSemaphore semaphore, uint64_t value, uint64_t timeoutNs)
{
    if (isLost()) {
        return lostStatus();
    }
    VkSemaphoreWaitInfo wi { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
    wi.semaphoreCount = 1;
    wi.pSemaphores = &semaphore;
    wi.pValues = &value;
    const VkResult r = vkWaitSemaphores(device_, &wi, timeoutNs);
    if (r == VK_TIMEOUT) {
        return fail(VK_TIMEOUT, "vkWaitSemaphores timed out");
    }
    return check(r, "vkWaitSemaphores");
}

GpuStatus
GpuDevice::waitIdle()
{
    if (isLost()) {
        return lostStatus();
    }
    return check(waitIdleLocked(), "vkDeviceWaitIdle");
}

GpuStatus
GpuDevice::queryBudget(std::vector<VmaBudget>& out)
{
    if (isLost()) {
        return lostStatus();
    }
    out.assign(memoryHeapCount_, VmaBudget {});
    vmaGetHeapBudgets(allocator_, out.data());
    return {};
}

} // namespace gpu
