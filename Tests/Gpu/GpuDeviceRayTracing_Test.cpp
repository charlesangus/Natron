#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "GpuDevice.h"
#include "GpuTestDevice.h"

using namespace gpu;

namespace {

class GpuDeviceRayTracingTest : public gputest::SkippableTest {
protected:
    void TearDown() override
    {
        dev.reset();
        unsetenv("NATRON_GPU_NO_RAY_QUERY");
        check.expectClean("the test");
    }

    gputest::ValidationCheck check;
    std::unique_ptr<GpuDevice> dev;
};

struct Advertised {
    bool accelExt = false;
    bool rayQueryExt = false;
    bool deferredExt = false;
    bool accelFeature = false;
    bool rayQueryFeature = false;
    bool bufferDeviceAddress = false;

    bool all() const
    {
        return accelExt && rayQueryExt && deferredExt && accelFeature && rayQueryFeature && bufferDeviceAddress;
    }
};

Advertised
queryAdvertised(VkPhysicalDevice pd)
{
    Advertised a;
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, exts.data());
    auto has = [&](const char* name) {
        return std::any_of(exts.begin(), exts.end(),
                           [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
    };
    a.accelExt = has(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
    a.rayQueryExt = has(VK_KHR_RAY_QUERY_EXTENSION_NAME);
    a.deferredExt = has(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);

    VkPhysicalDeviceVulkan12Features f12 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceAccelerationStructureFeaturesKHR accel { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
    VkPhysicalDeviceRayQueryFeaturesKHR rq { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
    VkPhysicalDeviceFeatures2 f2 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    f2.pNext = &f12;
    if (a.accelExt && a.rayQueryExt) {
        f12.pNext = &accel;
        accel.pNext = &rq;
    }
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    a.accelFeature = accel.accelerationStructure == VK_TRUE;
    a.rayQueryFeature = rq.rayQuery == VK_TRUE;
    a.bufferDeviceAddress = f12.bufferDeviceAddress == VK_TRUE;
    return a;
}

} // namespace

GPU_TEST_F(GpuDeviceRayTracingTest, CapabilityMatchesWhatTheDeviceAdvertises)
{
    GpuStatus s = gputest::createDevice(dev);
    GPUTEST_DEVICE_OR_SKIP_TEST(s.ok(), s.message);
    const Advertised a = queryAdvertised(dev->physicalDevice());
    std::printf("ray tracing on %s: VK_KHR_acceleration_structure=%d VK_KHR_ray_query=%d "
                "VK_KHR_deferred_host_operations=%d accelerationStructure=%d rayQuery=%d bufferDeviceAddress=%d "
                "-> supportsRayQuery=%d\n",
                dev->info().name.c_str(), a.accelExt, a.rayQueryExt, a.deferredExt, a.accelFeature,
                a.rayQueryFeature, a.bufferDeviceAddress, dev->supportsRayQuery());
    EXPECT_EQ(dev->supportsRayQuery(), a.all());
    EXPECT_EQ(dev->supportsRayQuery(), dev->info().rayQuery);
}

GPU_TEST_F(GpuDeviceRayTracingTest, EnvOverrideForcesItOff)
{
    ASSERT_EQ(setenv("NATRON_GPU_NO_RAY_QUERY", "1", 1), 0);
    GpuStatus s = gputest::createDevice(dev);
    GPUTEST_DEVICE_OR_SKIP_TEST(s.ok(), s.message);
    EXPECT_FALSE(dev->supportsRayQuery());
}

GPU_TEST_F(GpuDeviceRayTracingTest, DeviceStaysUsableWhateverTheCapability)
{
    GpuStatus s = gputest::createDevice(dev);
    GPUTEST_DEVICE_OR_SKIP_TEST(s.ok(), s.message);
    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    VkBufferCreateInfo bci { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = 4096;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (dev->supportsRayQuery()) {
        bci.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    VmaAllocationCreateInfo aci {};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    ASSERT_EQ(vmaCreateBuffer(dev->allocator(), &bci, &aci, &buf, &alloc, nullptr), VK_SUCCESS);
    if (dev->supportsRayQuery()) {
        VkBufferDeviceAddressInfo bda { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        bda.buffer = buf;
        EXPECT_NE(vkGetBufferDeviceAddress(dev->device(), &bda), 0u);
    }
    vmaDestroyBuffer(dev->allocator(), buf, alloc);
}
