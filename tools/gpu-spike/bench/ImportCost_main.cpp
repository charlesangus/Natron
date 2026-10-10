// Host-pointer import (pin) cost on the selected device, and what it adds per frame when frames are
// allocated fresh versus recycled.
//
// Part 1: median import and free time of a page-aligned buffer per size. Part 2: a churn loop of UHD
// RGBA float frames, each round-tripped host -> device -> host through GpuTransfer:
//   a) fresh aligned_alloc + first touch + import + transfer + free, every frame
//   b) recycled buffers with cached imports (transfer only)
//   c) recycled buffers through the staging path

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "GpuDevice.h"
#include "GpuTransfer.h"

using namespace gpu;
using Clock = std::chrono::steady_clock;

namespace {

constexpr size_t kUhdBytes = size_t(3840) * 2160 * 4 * sizeof(float);
constexpr uint32_t kChurnFrames = 32;
constexpr int kReps = 7;

double
ms(Clock::time_point a, Clock::time_point b)
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}

double
median(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    return v.empty() ? 0.0 : v[v.size() / 2];
}

size_t
roundUp(size_t n, size_t a)
{
    return (n + a - 1) / a * a;
}

double
memAvailableBytes()
{
    FILE* f = std::fopen("/proc/meminfo", "r");
    if (!f)
        return 0.0;
    char line[256];
    double kb = 0.0;
    while (std::fgets(line, sizeof line, f)) {
        if (std::sscanf(line, "MemAvailable: %lf kB", &kb) == 1)
            break;
    }
    std::fclose(f);
    return kb * 1024.0;
}

struct Importer {
    VkDevice vk;
    PFN_vkGetMemoryHostPointerPropertiesEXT getProps;
    VkPhysicalDeviceMemoryProperties mem;
    mutable const char* failedStep = "";
    mutable VkResult failedResult = VK_SUCCESS;

    struct Handle {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };

    bool import(void* p, VkDeviceSize bytes, Handle& out) const
    {
        VkMemoryHostPointerPropertiesEXT hp { VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
        if (VkResult r = getProps(vk, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, p, &hp); r != VK_SUCCESS)
            return fail("vkGetMemoryHostPointerPropertiesEXT", r);
        VkExternalMemoryBufferCreateInfo ext { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
        ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        VkBufferCreateInfo bci { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.pNext = &ext;
        bci.size = bytes;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (VkResult r = vkCreateBuffer(vk, &bci, nullptr, &out.buffer); r != VK_SUCCESS)
            return fail("vkCreateBuffer", r);
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(vk, out.buffer, &req);
        const uint32_t bits = req.memoryTypeBits & hp.memoryTypeBits;
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                type = i;
                break;
            }
        }
        VkImportMemoryHostPointerInfoEXT imp { VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT };
        imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        imp.pHostPointer = p;
        VkMemoryAllocateInfo mai { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.pNext = &imp;
        mai.allocationSize = bytes;
        mai.memoryTypeIndex = type;
        VkResult r = (type == UINT32_MAX || req.size > bytes) ? VK_ERROR_FEATURE_NOT_PRESENT : VK_SUCCESS;
        const char* step = "memory type / requirements";
        if (r == VK_SUCCESS) {
            step = "vkAllocateMemory";
            r = vkAllocateMemory(vk, &mai, nullptr, &out.memory);
        }
        if (r == VK_SUCCESS) {
            step = "vkBindBufferMemory";
            r = vkBindBufferMemory(vk, out.buffer, out.memory, 0);
        }
        if (r != VK_SUCCESS) {
            release(out);
            return fail(step, r);
        }
        return true;
    }

    bool fail(const char* step, VkResult r) const
    {
        failedStep = step;
        failedResult = r;
        return false;
    }

    void release(Handle& h) const
    {
        if (h.buffer)
            vkDestroyBuffer(vk, h.buffer, nullptr);
        if (h.memory)
            vkFreeMemory(vk, h.memory, nullptr);
        h = {};
    }
};

void
touch(void* p, size_t bytes)
{
    std::memset(p, 0x3c, bytes);
}

void
sizeSweep(const Importer& imp, size_t align)
{
    std::printf("\n== import / free per buffer size (median of %d, page-aligned) ==\n", kReps);
    std::printf("%-8s %10s %12s %12s %12s %12s %12s\n", "label", "MB", "alloc+touch", "pin(touched)", "unpin",
                "pin(cold)", "free-host");
    struct Sz {
        const char* label;
        size_t bytes;
    };
    const Sz sizes[] = { { "1MB", size_t(1) << 20 }, { "UHD", kUhdBytes }, { "8K", size_t(7680) * 4320 * 16 },
                         { "4GB", size_t(4) << 30 } };
    for (const Sz& s : sizes) {
        const size_t bytes = roundUp(s.bytes, align);
        if (double(bytes) * 2.0 > memAvailableBytes()) {
            std::printf("%-8s skipped: not enough free RAM\n", s.label);
            continue;
        }
        std::vector<double> tAlloc, tPin, tUnpin, tCold, tFree;
        bool failed = false;
        for (int r = 0; r < kReps && !failed; ++r) {
            const auto t0 = Clock::now();
            void* p = std::aligned_alloc(align, bytes);
            if (!p) {
                failed = true;
                break;
            }
            touch(p, bytes);
            const auto t1 = Clock::now();
            Importer::Handle h;
            const bool ok = imp.import(p, bytes, h);
            const auto t2 = Clock::now();
            if (!ok) {
                std::free(p);
                failed = true;
                break;
            }
            imp.release(h);
            const auto t3 = Clock::now();
            std::free(p);
            const auto t4 = Clock::now();
            tAlloc.push_back(ms(t0, t1));
            tPin.push_back(ms(t1, t2));
            tUnpin.push_back(ms(t2, t3));
            tFree.push_back(ms(t3, t4));

            void* q = std::aligned_alloc(align, bytes);
            const auto c0 = Clock::now();
            Importer::Handle hc;
            const bool okc = imp.import(q, bytes, hc);
            const auto c1 = Clock::now();
            imp.release(hc);
            std::free(q);
            if (okc)
                tCold.push_back(ms(c0, c1));
        }
        if (failed) {
            std::printf("%-8s import failed in %s (VkResult %d)\n", s.label, imp.failedStep, int(imp.failedResult));
            continue;
        }
        std::printf("%-8s %10.1f %12.3f %12.3f %12.3f %12.3f %12.3f\n", s.label, double(bytes) / (1 << 20),
                    median(tAlloc), median(tPin), median(tUnpin), median(tCold), median(tFree));
    }
}

struct ChurnResult {
    double msPerFrame = 0.0;
    bool ok = false;
};

ChurnResult
churnFresh(GpuTransfer& xfer, size_t align)
{
    const size_t bytes = roundUp(kUhdBytes, align);
    std::vector<double> t;
    for (uint32_t i = 0; i < kChurnFrames; ++i) {
        const auto t0 = Clock::now();
        void* src = std::aligned_alloc(align, bytes);
        void* dst = std::aligned_alloc(align, bytes);
        if (!src || !dst)
            return {};
        touch(src, bytes);
        const TransferFrame f { src, dst, bytes };
        if (!xfer.process({ &f, 1 }, nullptr)) {
            return {};
        }
        std::free(src);
        std::free(dst);
        t.push_back(ms(t0, Clock::now()));
    }
    return { median(t), true };
}

ChurnResult
churnRecycled(GpuTransfer& xfer, size_t align)
{
    const size_t bytes = roundUp(kUhdBytes, align);
    constexpr int kPool = 4;
    void* src[kPool];
    void* dst[kPool];
    for (int i = 0; i < kPool; ++i) {
        src[i] = std::aligned_alloc(align, bytes);
        dst[i] = std::aligned_alloc(align, bytes);
        touch(src[i], bytes);
        touch(dst[i], bytes);
    }
    ChurnResult res;
    bool good = true;
    // One pass over the pool outside the timing so cached imports are already in place.
    for (int i = 0; i < kPool && good; ++i) {
        const TransferFrame f { src[i], dst[i], bytes };
        good = bool(xfer.process({ &f, 1 }, nullptr));
    }
    std::vector<double> t;
    for (uint32_t i = 0; i < kChurnFrames && good; ++i) {
        const TransferFrame f { src[i % kPool], dst[i % kPool], bytes };
        const auto t0 = Clock::now();
        good = bool(xfer.process({ &f, 1 }, nullptr));
        t.push_back(ms(t0, Clock::now()));
    }
    xfer.releaseHostImports();
    for (int i = 0; i < kPool; ++i) {
        std::free(src[i]);
        std::free(dst[i]);
    }
    if (good) {
        res.msPerFrame = median(t);
        res.ok = true;
    }
    return res;
}

} // namespace

int
main()
{
    std::unique_ptr<GpuDevice> dev;
    if (GpuStatus s = GpuDevice::create({}, dev); !s) {
        std::fprintf(stderr, "GpuDevice::create failed: %s\n", s.message.c_str());
        return 1;
    }
    std::printf("device: %s  externalMemoryHost=%d\n", dev->info().name.c_str(), int(dev->info().externalMemoryHost));

    GpuTransferOptions base;
    base.copyThreads = 8;
    base.cacheHostImports = true;
    std::unique_ptr<GpuTransfer> cached;
    if (GpuStatus s = GpuTransfer::create(*dev, base, cached); !s) {
        std::fprintf(stderr, "GpuTransfer::create failed: %s\n", s.message.c_str());
        return 1;
    }
    if (!cached->hostImportSupported()) {
        std::fprintf(stderr, "host import unsupported on this device\n");
        return 2;
    }
    const size_t align = size_t(cached->hostImportAlignment());
    std::printf("import alignment: %zu bytes, UHD frame %.1f MB\n", align, double(kUhdBytes) / (1 << 20));

    Importer imp { dev->device(),
                   reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(
                       vkGetDeviceProcAddr(dev->device(), "vkGetMemoryHostPointerPropertiesEXT")),
                   {}, "", VK_SUCCESS };
    vkGetPhysicalDeviceMemoryProperties(dev->physicalDevice(), &imp.mem);
    if (!imp.getProps) {
        std::fprintf(stderr, "vkGetMemoryHostPointerPropertiesEXT missing\n");
        return 2;
    }

    sizeSweep(imp, align);

    GpuTransferOptions uncachedOpt = base;
    uncachedOpt.cacheHostImports = false;
    std::unique_ptr<GpuTransfer> uncached;
    GpuTransferOptions stagingOpt = base;
    stagingOpt.allowHostImport = false;
    std::unique_ptr<GpuTransfer> staging;
    if (!GpuTransfer::create(*dev, uncachedOpt, uncached) || !GpuTransfer::create(*dev, stagingOpt, staging)) {
        std::fprintf(stderr, "GpuTransfer::create failed\n");
        return 1;
    }

    std::printf("\n== churn: %u UHD round-trips, median ms/frame (8 copy threads for staging) ==\n", kChurnFrames);
    const ChurnResult a = churnFresh(*uncached, align);
    const ChurnResult b = churnRecycled(*cached, align);
    const ChurnResult c = churnRecycled(*staging, align);
    std::printf("a) fresh alloc+touch+import+transfer+free: %s %.2f\n", a.ok ? "ok" : "FAILED", a.msPerFrame);
    std::printf("b) recycled, cached import, transfer only: %s %.2f\n", b.ok ? "ok" : "FAILED", b.msPerFrame);
    std::printf("c) recycled, staging:                      %s %.2f\n", c.ok ? "ok" : "FAILED", c.msPerFrame);
    if (a.ok && b.ok) {
        std::printf("overhead a - b: %.2f ms/frame (%.0f%% of b's transfer)\n", a.msPerFrame - b.msPerFrame,
                    100.0 * (a.msPerFrame - b.msPerFrame) / b.msPerFrame);
    }
    return (a.ok && b.ok && c.ok) ? 0 : 1;
}
