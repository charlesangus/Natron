#include "GpuTransfer.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <thread>

namespace gpu {

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint64_t kWaitTimeoutNs = 60ull * 1000 * 1000 * 1000;

GpuStatus fail(VkResult r, std::string msg)
{
    return GpuStatus{r, std::move(msg)};
}

double msSince(Clock::time_point t0, Clock::time_point t)
{
    return std::chrono::duration<double, std::milli>(t - t0).count();
}

class CopyPool
{
public:
    explicit CopyPool(uint32_t threads)
    {
        threads = std::max(1u, threads);
        for (uint32_t i = 0; i < threads; ++i) {
            workers_.emplace_back([this] { run(); });
        }
    }

    ~CopyPool()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (std::thread& t : workers_) {
            t.join();
        }
    }

    // Split across every worker even for a single caller; with one worker all copies from both directions serialize.
    // Returns the memcpy time itself, excluding any wait behind the other direction's chunks.
    double copy(void* dst, const void* src, size_t bytes)
    {
        const size_t parts = workers_.size();
        const size_t chunk = ((bytes + parts - 1) / parts + 63) & ~size_t(63);
        Batch batch;
        size_t chunks = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (size_t off = 0; off < bytes; off += chunk) {
                tasks_.push_back({static_cast<char*>(dst) + off, static_cast<const char*>(src) + off,
                                  std::min(chunk, bytes - off), &batch});
                ++batch.remaining;
                ++chunks;
            }
        }
        cv_.notify_all();
        std::unique_lock<std::mutex> lock(batch.mutex);
        batch.cv.wait(lock, [&] { return batch.remaining == 0; });
        return batch.workMs / static_cast<double>(std::max<size_t>(1, std::min(chunks, parts)));
    }

private:
    struct Batch
    {
        std::mutex mutex;
        std::condition_variable cv;
        size_t remaining = 0;
        double workMs = 0.0;
    };
    struct Task
    {
        char* dst;
        const char* src;
        size_t bytes;
        Batch* batch;
    };

    void run()
    {
        for (;;) {
            Task t;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [&] { return stop_ || !tasks_.empty(); });
                if (tasks_.empty()) {
                    return;
                }
                t = tasks_.front();
                tasks_.pop_front();
            }
            const Clock::time_point b = Clock::now();
            std::memcpy(t.dst, t.src, t.bytes);
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - b).count();
            std::lock_guard<std::mutex> lock(t.batch->mutex);
            t.batch->workMs += ms;
            if (--t.batch->remaining == 0) {
                t.batch->cv.notify_all();
            }
        }
    }

    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Task> tasks_;
    bool stop_ = false;
};

struct VmaBuffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
};

struct ImportedBuffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

struct Barrier
{
    VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};

    Barrier(VkBuffer buffer, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
            VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
            uint32_t srcFamily = VK_QUEUE_FAMILY_IGNORED, uint32_t dstFamily = VK_QUEUE_FAMILY_IGNORED)
    {
        b.srcStageMask = srcStage;
        b.srcAccessMask = srcAccess;
        b.dstStageMask = dstStage;
        b.dstAccessMask = dstAccess;
        b.srcQueueFamilyIndex = srcFamily;
        b.dstQueueFamilyIndex = dstFamily;
        b.buffer = buffer;
        b.offset = 0;
        b.size = VK_WHOLE_SIZE;
    }

    void record(VkCommandBuffer cb) const
    {
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.bufferMemoryBarrierCount = 1;
        dep.pBufferMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cb, &dep);
    }
};

struct SemWait
{
    VkSemaphore semaphore;
    uint64_t value;
};

} // namespace

double TransferTimeline::hostUploadBusyMs() const
{
    double s = 0;
    for (const FrameTimeline& f : frames) s += f.hostUpload.busyMs;
    return s;
}

double TransferTimeline::hostDownloadBusyMs() const
{
    double s = 0;
    for (const FrameTimeline& f : frames) s += f.hostDownload.busyMs;
    return s;
}

double TransferTimeline::gpuUploadBusyMs() const
{
    double s = 0;
    for (const FrameTimeline& f : frames) s += f.gpuUpload.busyMs;
    return s;
}

double TransferTimeline::gpuComputeBusyMs() const
{
    double s = 0;
    for (const FrameTimeline& f : frames) s += f.gpuCompute.busyMs;
    return s;
}

double TransferTimeline::gpuDownloadBusyMs() const
{
    double s = 0;
    for (const FrameTimeline& f : frames) s += f.gpuDownload.busyMs;
    return s;
}

double TransferTimeline::sumOfStagesMs() const
{
    return hostUploadBusyMs() + gpuUploadBusyMs() + gpuComputeBusyMs() + gpuDownloadBusyMs()
           + hostDownloadBusyMs();
}

struct GpuTransfer::Impl
{
    GpuDevice& dev;
    const GpuTransferOptions& opt;
    VkDevice vk;
    bool ownershipTransfers;
    uint32_t computeFamily;
    uint32_t transferFamily;
    bool tsTransfer = false;
    bool tsCompute = false;
    float timestampPeriod = 1.0f;
    PFN_vkGetMemoryHostPointerPropertiesEXT getHostPointerProps = nullptr;
    VkDeviceSize importAlignment = 0;
    uint32_t memoryTypeCount = 0;
    VkMemoryPropertyFlags memoryTypeFlags[VK_MAX_MEMORY_TYPES] = {};

    CopyPool copies;

    std::vector<VmaBuffer> upSlots;
    std::vector<uint64_t> upSlotValue;
    std::vector<VmaBuffer> downSlots;
    std::vector<VmaBuffer> devIn;
    std::vector<VmaBuffer> devOut;
    VkDeviceSize devCapacity = 0;

    VkCommandPool upPool = VK_NULL_HANDLE;
    VkCommandPool downPool = VK_NULL_HANDLE;
    VkCommandPool compPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> upCbs;
    std::vector<VkCommandBuffer> downCbs;
    std::vector<VkCommandBuffer> compCbs;
    VkCommandBuffer miscCb = VK_NULL_HANDLE;

    VkSemaphore upTL = VK_NULL_HANDLE;
    VkSemaphore compTL = VK_NULL_HANDLE;
    VkSemaphore downTL = VK_NULL_HANDLE;
    uint64_t upValue = 0;
    uint64_t compValue = 0;
    uint64_t downValue = 0;
    uint64_t miscValue = 0;
    uint32_t upNext = 0;

    VkQueryPool queries = VK_NULL_HANDLE;
    uint32_t queryCapacity = 0;

    std::mutex importMutex;
    std::map<std::pair<uintptr_t, VkDeviceSize>, ImportedBuffer> importCache;

    Impl(GpuDevice& d, const GpuTransferOptions& o)
        : dev(d), opt(o), vk(d.device()), ownershipTransfers(!d.info().transferUsesComputeQueue),
          computeFamily(d.info().computeFamily), transferFamily(d.info().transferFamily),
          copies(o.copyThreads)
    {
    }

    ~Impl()
    {
        if (vk == VK_NULL_HANDLE) {
            return;
        }
        releaseCachedImports();
        vkDeviceWaitIdle(vk);
        auto freeBuf = [&](VmaBuffer& b) {
            if (b.buffer) vmaDestroyBuffer(dev.allocator(), b.buffer, b.allocation);
            b = {};
        };
        for (auto* v : {&upSlots, &downSlots, &devIn, &devOut}) {
            for (VmaBuffer& b : *v) freeBuf(b);
        }
        for (VkCommandPool p : {upPool, downPool, compPool}) {
            if (p) vkDestroyCommandPool(vk, p, nullptr);
        }
        for (VkSemaphore s : {upTL, compTL, downTL}) {
            if (s) vkDestroySemaphore(vk, s, nullptr);
        }
        if (queries) vkDestroyQueryPool(vk, queries, nullptr);
    }

    GpuStatus createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage memUsage,
                           VmaAllocationCreateFlags flags, VmaBuffer& out)
    {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = size;
        bci.usage = usage;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo aci{};
        aci.usage = memUsage;
        aci.flags = flags;
        VmaAllocationInfo info{};
        if (GpuStatus s = dev.check(vmaCreateBuffer(dev.allocator(), &bci, &aci, &out.buffer, &out.allocation, &info),
                                    "vmaCreateBuffer");
            !s) {
            return s;
        }
        out.mapped = info.pMappedData;
        out.size = size;
        return {};
    }

    GpuStatus createPool(uint32_t family, uint32_t count, VkCommandPool& pool, std::vector<VkCommandBuffer>& cbs)
    {
        VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        ci.queueFamilyIndex = family;
        if (GpuStatus s = dev.check(vkCreateCommandPool(vk, &ci, nullptr, &pool), "vkCreateCommandPool"); !s) {
            return s;
        }
        cbs.resize(count);
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = count;
        return dev.check(vkAllocateCommandBuffers(vk, &ai, cbs.data()), "vkAllocateCommandBuffers");
    }

    GpuStatus init()
    {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostProps{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        if (dev.info().externalMemoryHost) {
            props.pNext = &hostProps;
        }
        vkGetPhysicalDeviceProperties2(dev.physicalDevice(), &props);
        timestampPeriod = props.properties.limits.timestampPeriod;

        if (dev.info().externalMemoryHost && opt.allowHostImport) {
            getHostPointerProps = reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(
                vkGetDeviceProcAddr(vk, "vkGetMemoryHostPointerPropertiesEXT"));
            if (getHostPointerProps) {
                importAlignment = hostProps.minImportedHostPointerAlignment;
            }
        }

        const VkPhysicalDeviceMemoryProperties* mp = nullptr;
        vmaGetMemoryProperties(dev.allocator(), &mp);
        memoryTypeCount = mp->memoryTypeCount;
        for (uint32_t i = 0; i < memoryTypeCount; ++i) {
            memoryTypeFlags[i] = mp->memoryTypes[i].propertyFlags;
        }

        uint32_t nf = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev.physicalDevice(), &nf, nullptr);
        std::vector<VkQueueFamilyProperties> fams(nf);
        vkGetPhysicalDeviceQueueFamilyProperties(dev.physicalDevice(), &nf, fams.data());
        tsCompute = opt.recordTimestamps && fams[computeFamily].timestampValidBits > 0;
        tsTransfer = opt.recordTimestamps && fams[transferFamily].timestampValidBits > 0;

        const uint32_t n = std::max(1u, opt.slotCount);
        const uint32_t depth = std::max(1u, opt.framesInFlight);

        upSlots.resize(n);
        downSlots.resize(n);
        upSlotValue.assign(n, 0);
        for (uint32_t i = 0; i < n; ++i) {
            if (GpuStatus s = createBuffer(opt.stripBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                           VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                           VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                                               | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                           upSlots[i]);
                !s) {
                return s;
            }
            if (GpuStatus s = createBuffer(opt.stripBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                           VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                           VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                                               | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                           downSlots[i]);
                !s) {
                return s;
            }
        }
        devIn.resize(depth);
        devOut.resize(depth);

        if (GpuStatus s = createPool(transferFamily, n, upPool, upCbs); !s) return s;
        if (GpuStatus s = createPool(transferFamily, n, downPool, downCbs); !s) return s;
        if (GpuStatus s = createPool(computeFamily, depth + 1, compPool, compCbs); !s) return s;
        miscCb = compCbs.back();
        compCbs.pop_back();

        for (VkSemaphore* sem : {&upTL, &compTL, &downTL}) {
            if (GpuStatus s = dev.createTimelineSemaphore(0, *sem); !s) return s;
        }
        return {};
    }

    GpuStatus ensureDeviceBuffers(VkDeviceSize bytes)
    {
        if (bytes <= devCapacity) {
            return {};
        }
        for (auto* v : {&devIn, &devOut}) {
            for (VmaBuffer& b : *v) {
                if (b.buffer) vmaDestroyBuffer(dev.allocator(), b.buffer, b.allocation);
                b = {};
                const VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                                                 | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                if (GpuStatus s = createBuffer(bytes, usage, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, 0, b); !s) {
                    devCapacity = 0;
                    return s;
                }
            }
        }
        devCapacity = bytes;
        return {};
    }

    GpuStatus ensureQueries(uint32_t count)
    {
        if (count == 0) {
            return {};
        }
        if (count > queryCapacity) {
            if (queries) vkDestroyQueryPool(vk, queries, nullptr);
            queries = VK_NULL_HANDLE;
            queryCapacity = 0;
            VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
            ci.queryCount = count;
            if (GpuStatus s = dev.check(vkCreateQueryPool(vk, &ci, nullptr, &queries), "vkCreateQueryPool"); !s) {
                return s;
            }
            queryCapacity = count;
        }
        // hostQueryReset is not enabled on the device, so the reset goes through a command buffer.
        if (GpuStatus s = beginOneTime(miscCb); !s) return s;
        vkCmdResetQueryPool(miscCb, queries, 0, count);
        if (GpuStatus s = dev.check(vkEndCommandBuffer(miscCb), "vkEndCommandBuffer"); !s) return s;
        const uint64_t v = ++compValue;
        if (GpuStatus s = submit(QueueKind::Compute, miscCb, {}, compTL, v); !s) return s;
        return dev.waitSemaphore(compTL, v, kWaitTimeoutNs);
    }

    GpuStatus beginOneTime(VkCommandBuffer cb)
    {
        if (GpuStatus s = dev.check(vkResetCommandBuffer(cb, 0), "vkResetCommandBuffer"); !s) {
            return s;
        }
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        return dev.check(vkBeginCommandBuffer(cb, &bi), "vkBeginCommandBuffer");
    }

    GpuStatus submit(QueueKind kind, VkCommandBuffer cb, std::initializer_list<SemWait> waits, VkSemaphore signal,
                     uint64_t signalValue)
    {
        VkSemaphoreSubmitInfo w[4];
        uint32_t nw = 0;
        for (const SemWait& sw : waits) {
            if (sw.value == 0) continue;
            w[nw] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
            w[nw].semaphore = sw.semaphore;
            w[nw].value = sw.value;
            w[nw].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            ++nw;
        }
        VkSemaphoreSubmitInfo s{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        s.semaphore = signal;
        s.value = signalValue;
        s.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkCommandBufferSubmitInfo c{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        c.commandBuffer = cb;
        VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        si.waitSemaphoreInfoCount = nw;
        si.pWaitSemaphoreInfos = w;
        si.commandBufferInfoCount = 1;
        si.pCommandBufferInfos = &c;
        si.signalSemaphoreInfoCount = 1;
        si.pSignalSemaphoreInfos = &s;
        return dev.submit(kind, std::span<const VkSubmitInfo2>(&si, 1), VK_NULL_HANDLE);
    }

    bool importable(const void* p, VkDeviceSize bytes) const
    {
        return importAlignment != 0 && p != nullptr && reinterpret_cast<uintptr_t>(p) % importAlignment == 0
               && bytes % importAlignment == 0;
    }

    // Failure here is not an error: the caller falls back to staging.
    bool importHost(const void* p, VkDeviceSize bytes, ImportedBuffer& out)
    {
        if (!importable(p, bytes)) {
            return false;
        }
        void* ptr = const_cast<void*>(p);
        VkMemoryHostPointerPropertiesEXT hp{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        if (getHostPointerProps(vk, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, ptr, &hp) != VK_SUCCESS) {
            return false;
        }
        VkExternalMemoryBufferCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
        ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.pNext = &ext;
        bci.size = bytes;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(vk, &bci, nullptr, &out.buffer) != VK_SUCCESS) {
            return false;
        }
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(vk, out.buffer, &req);
        const uint32_t bits = req.memoryTypeBits & hp.memoryTypeBits;
        uint32_t type = UINT32_MAX;
        // Without HOST_COHERENT the host would need mapped flush/invalidate, which imported memory cannot do here.
        for (uint32_t i = 0; i < memoryTypeCount; ++i) {
            if ((bits & (1u << i)) && (memoryTypeFlags[i] & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                type = i;
                break;
            }
        }
        if (type == UINT32_MAX || req.size > bytes) {
            vkDestroyBuffer(vk, out.buffer, nullptr);
            out = {};
            return false;
        }
        VkImportMemoryHostPointerInfoEXT imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
        imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        imp.pHostPointer = ptr;
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.pNext = &imp;
        mai.allocationSize = bytes;
        mai.memoryTypeIndex = type;
        if (vkAllocateMemory(vk, &mai, nullptr, &out.memory) != VK_SUCCESS) {
            vkDestroyBuffer(vk, out.buffer, nullptr);
            out = {};
            return false;
        }
        if (vkBindBufferMemory(vk, out.buffer, out.memory, 0) != VK_SUCCESS) {
            vkDestroyBuffer(vk, out.buffer, nullptr);
            vkFreeMemory(vk, out.memory, nullptr);
            out = {};
            return false;
        }
        return true;
    }

    void destroyImport(ImportedBuffer& b)
    {
        if (b.buffer) vkDestroyBuffer(vk, b.buffer, nullptr);
        if (b.memory) vkFreeMemory(vk, b.memory, nullptr);
        b = {};
    }

    // Uncached imports are appended to `owned` and released by the caller once the GPU is done with them.
    bool acquireImport(const void* p, VkDeviceSize bytes, std::vector<ImportedBuffer>& owned, ImportedBuffer& out)
    {
        if (!importable(p, bytes)) {
            return false;
        }
        if (!opt.cacheHostImports) {
            if (!importHost(p, bytes, out)) return false;
            owned.push_back(out);
            return true;
        }
        const auto key = std::make_pair(reinterpret_cast<uintptr_t>(p), bytes);
        std::lock_guard<std::mutex> lock(importMutex);
        if (auto it = importCache.find(key); it != importCache.end()) {
            out = it->second;
            return true;
        }
        if (!importHost(p, bytes, out)) return false;
        importCache.emplace(key, out);
        return true;
    }

    void releaseCachedImports()
    {
        std::lock_guard<std::mutex> lock(importMutex);
        if (importCache.empty()) {
            return;
        }
        vkDeviceWaitIdle(vk);
        for (auto& [key, b] : importCache) destroyImport(b);
        importCache.clear();
    }
};

GpuTransfer::GpuTransfer(GpuDevice& device, const GpuTransferOptions& options)
    : device_(device), options_(options)
{
}

GpuTransfer::~GpuTransfer() = default;

GpuStatus GpuTransfer::create(GpuDevice& device, const GpuTransferOptions& options, std::unique_ptr<GpuTransfer>& out)
{
    out.reset();
    if (options.stripBytes == 0 || options.stripBytes % 64 != 0) {
        return fail(VK_ERROR_INITIALIZATION_FAILED, "stripBytes must be a non-zero multiple of 64");
    }
    std::unique_ptr<GpuTransfer> t(new GpuTransfer(device, options));
    t->impl_ = std::make_unique<Impl>(device, t->options_);
    if (GpuStatus s = t->impl_->init(); !s) {
        return s;
    }
    t->importAlignment_ = t->impl_->importAlignment;
    out = std::move(t);
    return {};
}

GpuStatus GpuTransfer::process(std::span<const TransferFrame> frames, const ComputeRecordFn& record,
                               TransferTimeline* timeline)
{
    Impl& m = *impl_;
    if (frames.empty()) {
        return {};
    }
    if (device_.isLost()) {
        return fail(VK_ERROR_DEVICE_LOST, "device is lost");
    }

    const uint32_t F = static_cast<uint32_t>(frames.size());
    const uint32_t N = static_cast<uint32_t>(m.upSlots.size());
    const uint32_t D = static_cast<uint32_t>(m.devIn.size());
    const VkDeviceSize strip = options_.stripBytes;

    VkDeviceSize maxBytes = 0;
    for (const TransferFrame& f : frames) {
        if (!f.src || !f.dst || f.bytes == 0) {
            return fail(VK_ERROR_UNKNOWN, "frame needs src, dst and a non-zero size");
        }
        if (f.dstBytes != 0 && f.dstOffset + f.dstBytes > f.bytes) {
            return fail(VK_ERROR_UNKNOWN, "download range exceeds the frame");
        }
        maxBytes = std::max(maxBytes, f.bytes);
    }
    auto downBytes = [&](const TransferFrame& f) { return f.dstBytes != 0 ? f.dstBytes : f.bytes; };
    auto downOffset = [&](const TransferFrame& f) { return f.dstBytes != 0 ? f.dstOffset : VkDeviceSize(0); };
    auto downPieces = [&](const TransferFrame& f) {
        return static_cast<uint32_t>((downBytes(f) + strip - 1) / strip);
    };
    if (GpuStatus s = m.ensureDeviceBuffers(maxBytes); !s) {
        return s;
    }

    const uint32_t maxPieces = static_cast<uint32_t>((maxBytes + strip - 1) / strip);
    const uint32_t perFrame = 4 * maxPieces + 2;
    const bool timestamps = m.tsCompute || m.tsTransfer;
    if (timestamps) {
        if (GpuStatus s = m.ensureQueries(perFrame * F); !s) {
            return s;
        }
    }
    std::vector<uint8_t> written(timestamps ? perFrame * F : 0, 0);
    auto qUp = [&](uint32_t k, uint32_t i) { return k * perFrame + 2 * i; };
    auto qComp = [&](uint32_t k) { return k * perFrame + 2 * maxPieces; };
    auto qDown = [&](uint32_t k, uint32_t i) { return k * perFrame + 2 * maxPieces + 2 + 2 * i; };
    auto stamp = [&](VkCommandBuffer cb, bool familyOk, uint32_t q) {
        if (timestamps && familyOk) {
            vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m.queries, q);
            written[q] = 1;
        }
    };

    std::vector<FrameTimeline> tl(F);
    for (uint32_t k = 0; k < F; ++k) {
        tl[k].strips = static_cast<uint32_t>((frames[k].bytes + strip - 1) / strip);
    }

    const uint64_t compBase = m.compValue;
    std::vector<uint64_t> uploadDone(F, 0);
    std::vector<uint64_t> downloadDone(F, 0);

    std::mutex mtx;
    std::condition_variable cv;
    uint32_t computeSubmitted = 0;
    uint32_t downloadSubmitted = 0;
    bool aborted = false;
    GpuStatus firstError;

    auto raise = [&](GpuStatus s) {
        std::lock_guard<std::mutex> lock(mtx);
        if (!aborted) {
            firstError = std::move(s);
            aborted = true;
        }
        cv.notify_all();
    };

    const Clock::time_point t0 = Clock::now();
    std::vector<ImportedBuffer> upImports;
    std::vector<ImportedBuffer> downImports;

    auto uploader = [&]() -> GpuStatus {
        for (uint32_t k = 0; k < F; ++k) {
            const TransferFrame& f = frames[k];
            const uint32_t d = k % D;
            FrameTimeline& ft = tl[k];
            const uint64_t inFree = k >= D ? compBase + (k - D) + 1 : 0;

            ImportedBuffer imp;
            const Clock::time_point ti = Clock::now();
            const bool useImport = m.acquireImport(f.src, f.bytes, upImports, imp);
            if (useImport) {
                const Clock::time_point te = Clock::now();
                ft.uploadPath = TransferPath::HostImport;
                ft.hostUpload = {msSince(t0, ti), msSince(t0, te), msSince(ti, te)};
            }

            const uint32_t pieces = useImport ? 1 : ft.strips;
            for (uint32_t i = 0; i < pieces; ++i) {
                const uint32_t r = m.upNext++ % N;
                if (GpuStatus s = device_.waitSemaphore(m.upTL, m.upSlotValue[r], kWaitTimeoutNs); !s) {
                    return s;
                }
                const VkDeviceSize off = useImport ? 0 : i * strip;
                const VkDeviceSize len = useImport ? f.bytes : std::min(strip, f.bytes - off);
                VkBuffer srcBuf = useImport ? imp.buffer : m.upSlots[r].buffer;
                if (!useImport) {
                    const Clock::time_point cs = Clock::now();
                    const double work = m.copies.copy(m.upSlots[r].mapped, static_cast<const char*>(f.src) + off, len);
                    vmaFlushAllocation(device_.allocator(), m.upSlots[r].allocation, 0, len);
                    const Clock::time_point ce = Clock::now();
                    if (i == 0) ft.hostUpload.beginMs = msSince(t0, cs);
                    ft.hostUpload.endMs = msSince(t0, ce);
                    ft.hostUpload.busyMs += work;
                }

                VkCommandBuffer cb = m.upCbs[r];
                if (GpuStatus s = m.beginOneTime(cb); !s) return s;
                stamp(cb, m.tsTransfer, qUp(k, i));
                VkBufferCopy region{0, useImport ? 0 : off, len};
                vkCmdCopyBuffer(cb, srcBuf, m.devIn[d].buffer, 1, &region);
                if (i + 1 == pieces && m.ownershipTransfers) {
                    Barrier(m.devIn[d].buffer, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, m.transferFamily, m.computeFamily)
                        .record(cb);
                }
                stamp(cb, m.tsTransfer, qUp(k, i) + 1);
                if (GpuStatus s = device_.check(vkEndCommandBuffer(cb), "vkEndCommandBuffer"); !s) return s;
                const uint64_t v = ++m.upValue;
                if (GpuStatus s = m.submit(QueueKind::Transfer, cb, {{m.compTL, inFree}}, m.upTL, v); !s) {
                    return s;
                }
                m.upSlotValue[r] = v;
            }
            uploadDone[k] = m.upValue;

            uint64_t outFree = 0;
            if (k >= D) {
                std::unique_lock<std::mutex> lock(mtx);
                cv.wait(lock, [&] { return aborted || downloadSubmitted > k - D; });
                if (aborted) return {};
                outFree = downloadDone[k - D];
                lock.unlock();
                if (GpuStatus s = device_.waitSemaphore(m.compTL, compBase + (k - D) + 1, kWaitTimeoutNs); !s) {
                    return s;
                }
            }

            VkCommandBuffer cb = m.compCbs[d];
            if (GpuStatus s = m.beginOneTime(cb); !s) return s;
            stamp(cb, m.tsCompute, qComp(k));
            if (m.ownershipTransfers) {
                Barrier(m.devIn[d].buffer, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT, m.transferFamily,
                        m.computeFamily)
                    .record(cb);
            }
            if (record) {
                if (GpuStatus s = record(cb, ComputeBinding{m.devIn[d].buffer, m.devOut[d].buffer, f.bytes, k}); !s) {
                    return s;
                }
            } else {
                VkBufferCopy region{0, 0, f.bytes};
                vkCmdCopyBuffer(cb, m.devIn[d].buffer, m.devOut[d].buffer, 1, &region);
            }
            if (m.ownershipTransfers) {
                Barrier(m.devOut[d].buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, m.computeFamily, m.transferFamily)
                    .record(cb);
            }
            stamp(cb, m.tsCompute, qComp(k) + 1);
            if (GpuStatus s = device_.check(vkEndCommandBuffer(cb), "vkEndCommandBuffer"); !s) return s;
            if (GpuStatus s = m.submit(QueueKind::Compute, cb, {{m.upTL, uploadDone[k]}, {m.downTL, outFree}}, m.compTL,
                                       compBase + k + 1);
                !s) {
                return s;
            }
            {
                std::lock_guard<std::mutex> lock(mtx);
                computeSubmitted = k + 1;
            }
            cv.notify_all();
        }
        return {};
    };

    struct Piece
    {
        uint32_t frame;
        uint32_t ring;
        uint64_t value;
        VkDeviceSize offset;
        VkDeviceSize bytes;
        bool staging;
        bool last;
    };

    auto downloader = [&]() -> GpuStatus {
        std::deque<Piece> fifo;
        uint32_t ringNext = 0;

        auto drainOne = [&]() -> GpuStatus {
            Piece p = fifo.front();
            fifo.pop_front();
            if (GpuStatus s = device_.waitSemaphore(m.downTL, p.value, kWaitTimeoutNs); !s) {
                return s;
            }
            FrameTimeline& ft = tl[p.frame];
            const Clock::time_point cs = Clock::now();
            if (p.staging) {
                vmaInvalidateAllocation(device_.allocator(), m.downSlots[p.ring].allocation, 0, p.bytes);
                const double work = m.copies.copy(static_cast<char*>(frames[p.frame].dst) + p.offset,
                                                  m.downSlots[p.ring].mapped, p.bytes);
                const Clock::time_point ce = Clock::now();
                if (p.offset == 0) ft.hostDownload.beginMs = msSince(t0, cs);
                ft.hostDownload.busyMs += work;
                ft.hostDownload.endMs = msSince(t0, ce);
            } else {
                ft.hostDownload.endMs = msSince(t0, cs);
            }
            return {};
        };

        for (uint32_t k = 0; k < F; ++k) {
            {
                std::unique_lock<std::mutex> lock(mtx);
                cv.wait(lock, [&] { return aborted || computeSubmitted > k; });
                if (aborted) return {};
            }
            const TransferFrame& f = frames[k];
            const uint32_t d = k % D;
            FrameTimeline& ft = tl[k];

            ImportedBuffer imp;
            const Clock::time_point ti = Clock::now();
            const bool useImport = m.acquireImport(f.dst, downBytes(f), downImports, imp);
            if (useImport) {
                const Clock::time_point te = Clock::now();
                ft.downloadPath = TransferPath::HostImport;
                ft.hostDownload = {msSince(t0, ti), msSince(t0, te), msSince(ti, te)};
            }

            const uint32_t pieces = useImport ? 1 : downPieces(f);
            for (uint32_t i = 0; i < pieces; ++i) {
                if (fifo.size() == N) {
                    if (GpuStatus s = drainOne(); !s) return s;
                }
                const uint32_t r = ringNext++ % N;
                const VkDeviceSize off = useImport ? 0 : i * strip;
                const VkDeviceSize len = useImport ? downBytes(f) : std::min(strip, downBytes(f) - off);
                VkBuffer dstBuf = useImport ? imp.buffer : m.downSlots[r].buffer;

                VkCommandBuffer cb = m.downCbs[r];
                if (GpuStatus s = m.beginOneTime(cb); !s) return s;
                stamp(cb, m.tsTransfer, qDown(k, i));
                if (i == 0 && m.ownershipTransfers) {
                    Barrier(m.devOut[d].buffer, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COPY_BIT,
                            VK_ACCESS_2_TRANSFER_READ_BIT, m.computeFamily, m.transferFamily)
                        .record(cb);
                }
                VkBufferCopy region{downOffset(f) + off, 0, len};
                vkCmdCopyBuffer(cb, m.devOut[d].buffer, dstBuf, 1, &region);
                Barrier(dstBuf, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                        VK_ACCESS_2_HOST_READ_BIT)
                    .record(cb);
                stamp(cb, m.tsTransfer, qDown(k, i) + 1);
                if (GpuStatus s = device_.check(vkEndCommandBuffer(cb), "vkEndCommandBuffer"); !s) return s;
                const uint64_t v = ++m.downValue;
                if (GpuStatus s = m.submit(QueueKind::Transfer, cb, {{m.compTL, compBase + k + 1}}, m.downTL, v); !s) {
                    return s;
                }
                fifo.push_back({k, r, v, off, len, !useImport, i + 1 == pieces});
            }
            {
                std::lock_guard<std::mutex> lock(mtx);
                downloadDone[k] = m.downValue;
                downloadSubmitted = k + 1;
            }
            cv.notify_all();
        }
        while (!fifo.empty()) {
            if (GpuStatus s = drainOne(); !s) return s;
        }
        return {};
    };

    std::exception_ptr upException;
    std::exception_ptr downException;
    std::thread down([&] {
        try {
            if (GpuStatus s = downloader(); !s) raise(std::move(s));
        } catch (...) {
            downException = std::current_exception();
            raise(fail(VK_ERROR_UNKNOWN, "GpuTransfer download thread threw"));
        }
    });
    try {
        if (GpuStatus s = uploader(); !s) raise(std::move(s));
    } catch (...) {
        upException = std::current_exception();
        raise(fail(VK_ERROR_UNKNOWN, "GpuTransfer compute callback or upload threw"));
    }
    down.join();
    const Clock::time_point t1 = Clock::now();

    m.compValue = compBase + F;
    if (aborted) {
        vkDeviceWaitIdle(m.vk);
    } else if (GpuStatus s = device_.waitSemaphore(m.compTL, m.compValue, kWaitTimeoutNs); !s) {
        raise(std::move(s));
        vkDeviceWaitIdle(m.vk);
    }
    for (ImportedBuffer& b : upImports) m.destroyImport(b);
    for (ImportedBuffer& b : downImports) m.destroyImport(b);
    if (upException) {
        std::rethrow_exception(upException);
    }
    if (downException) {
        std::rethrow_exception(downException);
    }
    if (aborted) {
        return firstError;
    }

    if (timeline) {
        timeline->frames = std::move(tl);
        timeline->wallMs = msSince(t0, t1);
        timeline->gpuTimestamps = timestamps;
        timeline->gpuSpanMs = 0;
        if (timestamps) {
            std::vector<uint64_t> ticks(written.size(), 0);
            for (uint32_t q = 0; q < written.size(); ++q) {
                if (!written[q]) continue;
                if (GpuStatus s = device_.check(vkGetQueryPoolResults(m.vk, m.queries, q, 1, sizeof(uint64_t), &ticks[q],
                                                                      sizeof(uint64_t),
                                                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
                                                "vkGetQueryPoolResults");
                    !s) {
                    return s;
                }
            }
            uint64_t origin = std::numeric_limits<uint64_t>::max();
            uint64_t last = 0;
            for (uint32_t q = 0; q < written.size(); ++q) {
                if (written[q]) {
                    origin = std::min(origin, ticks[q]);
                    last = std::max(last, ticks[q]);
                }
            }
            const double toMs = double(m.timestampPeriod) * 1e-6;
            timeline->gpuSpanMs = origin <= last ? double(last - origin) * toMs : 0.0;
            auto span = [&](TimeSpan& out, uint32_t q0, uint32_t pieces) {
                bool any = false;
                for (uint32_t i = 0; i < pieces; ++i) {
                    const uint32_t q = q0 + 2 * i;
                    if (!written[q] || !written[q + 1]) continue;
                    const double b = double(ticks[q] - origin) * toMs;
                    const double e = double(ticks[q + 1] - origin) * toMs;
                    out.beginMs = any ? std::min(out.beginMs, b) : b;
                    out.endMs = any ? std::max(out.endMs, e) : e;
                    out.busyMs += e - b;
                    any = true;
                }
            };
            for (uint32_t k = 0; k < F; ++k) {
                FrameTimeline& ft = timeline->frames[k];
                span(ft.gpuUpload, qUp(k, 0), ft.uploadPath == TransferPath::HostImport ? 1 : ft.strips);
                span(ft.gpuCompute, qComp(k), 1);
                span(ft.gpuDownload, qDown(k, 0), ft.downloadPath == TransferPath::HostImport ? 1 : downPieces(frames[k]));
            }
        }
    }
    return {};
}

void GpuTransfer::releaseHostImports()
{
    impl_->releaseCachedImports();
}

} // namespace gpu
