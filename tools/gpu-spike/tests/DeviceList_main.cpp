#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

bool hasExt(const std::vector<VkExtensionProperties>& exts, const char* name)
{
    return std::any_of(exts.begin(), exts.end(), [&](const VkExtensionProperties& e) {
        return std::strcmp(e.extensionName, name) == 0;
    });
}

const char* yn(bool b) { return b ? "yes" : "no"; }

std::string queueFlags(VkQueueFlags f)
{
    std::string s;
    auto add = [&](VkQueueFlagBits bit, const char* n) {
        if (f & bit) {
            s += s.empty() ? "" : "|";
            s += n;
        }
    };
    add(VK_QUEUE_GRAPHICS_BIT, "graphics");
    add(VK_QUEUE_COMPUTE_BIT, "compute");
    add(VK_QUEUE_TRANSFER_BIT, "transfer");
    add(VK_QUEUE_SPARSE_BINDING_BIT, "sparse");
    add(VK_QUEUE_VIDEO_DECODE_BIT_KHR, "video-decode");
    add(VK_QUEUE_VIDEO_ENCODE_BIT_KHR, "video-encode");
    add(VK_QUEUE_OPTICAL_FLOW_BIT_NV, "optical-flow");
    return s;
}

} // namespace

int main()
{
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = vkCreateInstance(&ici, nullptr, &inst);
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "vkCreateInstance failed: %d\n", static_cast<int>(r));
        return 1;
    }

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(inst, &n, devs.data());
    std::printf("%u physical device(s)\n", n);

    for (uint32_t i = 0; i < n; ++i) {
        VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drv};
        vkGetPhysicalDeviceProperties2(devs[i], &props);
        const uint32_t v = props.properties.apiVersion;

        std::printf("[%u] %s\n", i, props.properties.deviceName);
        std::printf("    driver: id=%d name=\"%s\" info=\"%s\"\n", static_cast<int>(drv.driverID),
                    drv.driverName, drv.driverInfo);
        std::printf("    api: %u.%u.%u\n", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v),
                    VK_API_VERSION_PATCH(v));

        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qf(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, qf.data());
        for (uint32_t q = 0; q < qn; ++q) {
            const bool compute = (qf[q].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
            const bool transferOnly =
                (qf[q].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == 0 &&
                (qf[q].queueFlags & VK_QUEUE_TRANSFER_BIT) != 0;
            std::printf("    queue family %u: count=%u flags=%s%s%s\n", q, qf[q].queueCount,
                        queueFlags(qf[q].queueFlags).c_str(), compute ? " [compute]" : "",
                        transferOnly ? " [transfer-only]" : "");
        }

        uint32_t en = 0;
        vkEnumerateDeviceExtensionProperties(devs[i], nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> exts(en);
        vkEnumerateDeviceExtensionProperties(devs[i], nullptr, &en, exts.data());

        std::printf("    VK_KHR_external_memory_fd:     %s\n", yn(hasExt(exts, "VK_KHR_external_memory_fd")));
        std::printf("    VK_KHR_external_semaphore_fd:  %s\n", yn(hasExt(exts, "VK_KHR_external_semaphore_fd")));
        std::printf("    VK_EXT_external_memory_host:   %s\n", yn(hasExt(exts, "VK_EXT_external_memory_host")));
        std::printf("    VK_EXT_memory_budget:          %s\n", yn(hasExt(exts, "VK_EXT_memory_budget")));
        std::printf("    VK_KHR_push_descriptor:        %s\n", yn(hasExt(exts, "VK_KHR_push_descriptor")));

        VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        f11.pNext = &f12;
        f12.pNext = &f13;
        VkPhysicalDeviceFeatures2 feats{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
        const bool core12 = v >= VK_API_VERSION_1_2;
        const bool core13 = v >= VK_API_VERSION_1_3;
        if (core12) {
            // Chained 1.2/1.3 structs are invalid on a device that does not report that version.
            if (!core13)
                f12.pNext = nullptr;
            vkGetPhysicalDeviceFeatures2(devs[i], &feats);
        }
        std::printf("    timelineSemaphore:             %s\n", yn(core12 && f12.timelineSemaphore));
        std::printf("    synchronization2:              %s\n", yn(core13 && f13.synchronization2));
        std::printf("    shaderFloat16:                 %s\n", yn(core12 && f12.shaderFloat16));
        std::printf("    storageBuffer16BitAccess:      %s\n", yn(core12 && f11.storageBuffer16BitAccess));
    }

    vkDestroyInstance(inst, nullptr);
    return 0;
}
