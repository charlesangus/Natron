#include "GpuTransfer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <thread>

namespace gpu {

namespace {

    using Clock = std::chrono::steady_clock;

    constexpr uint64_t kWaitTimeoutNs = 60ull * 1000 * 1000 * 1000;
    constexpr uint64_t k4GiB = 1ull << 32;
    constexpr VkBufferUsageFlags kImportUsage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    constexpr uint32_t kQueriesPerFrame = 6;

    GpuStatus fail(VkResult r, std::string msg)
    {
        return GpuStatus { r, std::move(msg) };
    }

    double msSince(Clock::time_point t0, Clock::time_point t)
    {
        return std::chrono::duration<double, std::milli>(t - t0).count();
    }

    class CopyPool {
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
                    tasks_.push_back({ static_cast<char*>(dst) + off, static_cast<const char*>(src) + off,
                                       std::min(chunk, bytes - off), &batch });
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
        struct Batch {
            std::mutex mutex;
            std::condition_variable cv;
            size_t remaining = 0;
            double workMs = 0.0;
        };
        struct Task {
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

    struct VmaBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        void* mapped = nullptr;
        VkDeviceSize size = 0;
    };

    void bufferBarrier(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size,
                       VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                       VkAccessFlags2 dstAccess, uint32_t srcFamily, uint32_t dstFamily)
    {
        VkBufferMemoryBarrier2 b { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
        b.srcStageMask = srcStage;
        b.srcAccessMask = srcAccess;
        b.dstStageMask = dstStage;
        b.dstAccessMask = dstAccess;
        b.srcQueueFamilyIndex = srcFamily;
        b.dstQueueFamilyIndex = dstFamily;
        b.buffer = buffer;
        b.offset = offset;
        b.size = size;
        VkDependencyInfo dep { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.bufferMemoryBarrierCount = 1;
        dep.pBufferMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cb, &dep);
    }

    // Host reads of staging or imported memory need the copy's writes made visible to the host domain.
    void copyToHostBarrier(VkCommandBuffer cb)
    {
        VkMemoryBarrier2 b { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        b.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dep { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cb, &dep);
    }

    std::vector<VkSemaphoreSubmitInfo> waitInfos(std::span<const TimelineValue> values)
    {
        std::vector<VkSemaphoreSubmitInfo> out;
        for (const TimelineValue& v : values) {
            if (v) {
                out.push_back(v.waitInfo());
            }
        }
        return out;
    }

    struct CbPool {
        struct Entry {
            VkCommandBuffer cb = VK_NULL_HANDLE;
            VkSemaphore semaphore = VK_NULL_HANDLE;
            uint64_t value = 0;
        };
        VkCommandPool pool = VK_NULL_HANDLE;
        std::vector<Entry> entries;
    };

    // One contiguous stretch of a host range that is backed by one imported buffer.
    struct ImportSeg {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize bufferOffset = 0;
        VkDeviceSize hostOffset = 0;
        VkDeviceSize bytes = 0;
        uintptr_t cacheKey = 0;
    };

    struct CachedImport {
        GpuBuffer buffer;
        VkDeviceSize bytes = 0;
        // The last upload and download that copy through it, each on its own timeline.
        uint64_t lastUpload = 0;
        uint64_t lastDownload = 0;
    };

    // An import being created outside the cache lock. forget() marks the ones it overlaps, which are then
    // destroyed instead of cached, and waits until they are.
    struct PendingImport {
        uintptr_t lo = 0;
        uintptr_t hi = 0;
        bool forgotten = false;
    };

    enum class ImportOrigin {
        Prepare,
        Upload,
        Download,
    };

    struct Retired {
        GpuBuffer buffer;
        TimelineValue lastUse;
    };

    struct DrainItem {
        uint32_t slot = 0;
        uint64_t value = 0;
        char* dst = nullptr;
        VkDeviceSize bytes = 0;
        TimeSpan* span = nullptr;
        Clock::time_point origin {};
        bool first = false;
    };

} // namespace

double
TransferTimeline::hostUploadBusyMs() const
{
    double s = 0;
    for (const FrameTimeline& f : frames)
        s += f.hostUpload.busyMs;
    return s;
}

double
TransferTimeline::hostDownloadBusyMs() const
{
    double s = 0;
    for (const FrameTimeline& f : frames)
        s += f.hostDownload.busyMs;
    return s;
}

double
TransferTimeline::gpuUploadBusyMs() const
{
    double s = 0;
    for (const FrameTimeline& f : frames)
        s += f.gpuUpload.busyMs;
    return s;
}

double
TransferTimeline::gpuComputeBusyMs() const
{
    double s = 0;
    for (const FrameTimeline& f : frames)
        s += f.gpuCompute.busyMs;
    return s;
}

double
TransferTimeline::gpuDownloadBusyMs() const
{
    double s = 0;
    for (const FrameTimeline& f : frames)
        s += f.gpuDownload.busyMs;
    return s;
}

double
TransferTimeline::sumOfStagesMs() const
{
    return hostUploadBusyMs() + gpuUploadBusyMs() + gpuComputeBusyMs() + gpuDownloadBusyMs()
        + hostDownloadBusyMs();
}

struct GpuTransfer::Impl {
    GpuDevice& dev;
    const GpuTransferOptions& opt;
    VkDevice vk;
    bool ownership;
    uint32_t computeFamily;
    uint32_t transferFamily;
    bool tsTransfer = false;
    bool tsCompute = false;
    // Whether the transfer family can write the timestamps a caller asks for through StreamOptions.
    bool stampable = false;
    bool timestampsSupported = false;
    float timestampPeriod = 1.0f;
    VkDeviceSize importAlignment = 0;
    VkDeviceSize importPieceMax = 0;
    VkDeviceSize chunk;
    VkDeviceSize stagingPiece;

    CopyPool copies;

    // Uploads and downloads each signal a timeline of their own, since they may run on different queues.
    // upValue is guarded by upMutex and downValue by downMutex.
    VkSemaphore upTL = VK_NULL_HANDLE;
    uint64_t upValue = 0;
    VkSemaphore downTL = VK_NULL_HANDLE;
    uint64_t downValue = 0;

    // Compute-queue halves of ownership transfers get their own timeline: values signalled from two
    // queues on one semaphore could reach the device out of order.
    VkSemaphore ownerTL = VK_NULL_HANDLE;
    std::mutex ownerMutex;
    uint64_t ownerValue = 0;
    CbPool ownerCbs;

    std::mutex upMutex;
    CbPool upCbs;
    std::vector<VmaBuffer> upSlots;
    std::vector<uint64_t> upSlotValue;
    uint32_t upNext = 0;

    std::mutex downMutex;
    CbPool downCbs;
    std::vector<VmaBuffer> downSlots;
    uint32_t downNext = 0;

    std::mutex drainMutex;
    std::condition_variable drainCv;
    std::deque<DrainItem> drain;
    std::vector<uint8_t> downSlotBusy;
    bool drainStop = false;
    GpuStatus drainError;
    std::thread drainThread;

    std::mutex importMutex;
    std::condition_variable importCv;
    std::map<uintptr_t, CachedImport> importCache;
    std::list<PendingImport> pendingImports;
    std::atomic<size_t> cachedImports { 0 };
    std::atomic<size_t> pendingCount { 0 };
    std::atomic<uint64_t> importCount { 0 };
    std::atomic<uint64_t> importBytes { 0 };
    std::atomic<uint64_t> preparedImports { 0 };
    std::atomic<uint64_t> uploadImports { 0 };
    std::atomic<uint64_t> downloadImports { 0 };
    std::atomic<uint64_t> droppedImports { 0 };

    std::mutex retireMutex;
    std::vector<Retired> retired;

    std::mutex processMutex;
    std::vector<GpuBuffer> devIn;
    std::vector<GpuBuffer> devOut;
    VkDeviceSize devCapacity = 0;
    VkCommandPool compPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> compCbs;
    VkCommandBuffer miscCb = VK_NULL_HANDLE;
    VkSemaphore compTL = VK_NULL_HANDLE;
    uint64_t compValue = 0;
    VkQueryPool queries = VK_NULL_HANDLE;
    uint32_t queryCapacity = 0;

    Impl(GpuDevice& d, const GpuTransferOptions& o)
        : dev(d)
        , opt(o)
        , vk(d.device())
        , ownership(d.info().transferFamily != d.info().computeFamily)
        , computeFamily(d.info().computeFamily)
        , transferFamily(d.info().transferFamily)
        , chunk(std::max<VkDeviceSize>(1, d.info().copyChunkSize))
        , stagingPiece(std::min<VkDeviceSize>(o.stripBytes, chunk))
        , copies(o.copyThreads)
    {
    }

    ~Impl()
    {
        if (vk == VK_NULL_HANDLE) {
            return;
        }
        dev.drainQueues();
        if (drainThread.joinable()) {
            {
                std::lock_guard<std::mutex> lock(drainMutex);
                drainStop = true;
            }
            drainCv.notify_all();
            drainThread.join();
        }
        importCache.clear();
        retired.clear();
        devIn.clear();
        devOut.clear();
        for (auto* v : { &upSlots, &downSlots }) {
            for (VmaBuffer& b : *v) {
                if (b.buffer)
                    vmaDestroyBuffer(dev.allocator(), b.buffer, b.allocation);
                b = {};
            }
        }
        for (VkCommandPool p : { upCbs.pool, downCbs.pool, ownerCbs.pool, compPool }) {
            if (p)
                vkDestroyCommandPool(vk, p, nullptr);
        }
        for (VkSemaphore s : { upTL, downTL, ownerTL, compTL }) {
            if (s)
                vkDestroySemaphore(vk, s, nullptr);
        }
        if (queries)
            vkDestroyQueryPool(vk, queries, nullptr);
    }

    GpuStatus createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage memUsage,
                           VmaAllocationCreateFlags flags, VmaBuffer& out)
    {
        VkBufferCreateInfo bci { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = size;
        bci.usage = usage;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo aci {};
        aci.usage = memUsage;
        aci.flags = flags;
        VmaAllocationInfo info {};
        if (GpuStatus s = dev.check(vmaCreateBuffer(dev.allocator(), &bci, &aci, &out.buffer, &out.allocation, &info),
                                    "vmaCreateBuffer");
            !s) {
            return s;
        }
        out.mapped = info.pMappedData;
        out.size = size;
        return {};
    }

    GpuStatus createPool(uint32_t family, VkCommandPool& pool)
    {
        VkCommandPoolCreateInfo ci { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        ci.queueFamilyIndex = family;
        return dev.check(vkCreateCommandPool(vk, &ci, nullptr, &pool), "vkCreateCommandPool");
    }

    GpuStatus allocateCbs(VkCommandPool pool, uint32_t count, VkCommandBuffer* out)
    {
        VkCommandBufferAllocateInfo ai { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = count;
        return dev.check(vkAllocateCommandBuffers(vk, &ai, out), "vkAllocateCommandBuffers");
    }

    GpuStatus init()
    {
        VkPhysicalDeviceProperties props {};
        vkGetPhysicalDeviceProperties(dev.physicalDevice(), &props);
        timestampPeriod = props.limits.timestampPeriod;

        const GpuDeviceInfo& info = dev.info();
        if (info.externalMemoryHost && opt.allowHostImport && info.minImportedHostPointerAlignment != 0) {
            importAlignment = info.minImportedHostPointerAlignment;
            // Pieces stay under 4 GiB: RADV mishandles larger host imports.
            VkDeviceSize limit = k4GiB - importAlignment;
            if (info.maxMemoryAllocationSize != 0) {
                limit = std::min<VkDeviceSize>(limit, info.maxMemoryAllocationSize);
            }
            importPieceMax = limit / importAlignment * importAlignment;
            if (importPieceMax == 0) {
                importAlignment = 0;
            }
        }

        uint32_t nf = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev.physicalDevice(), &nf, nullptr);
        std::vector<VkQueueFamilyProperties> fams(nf);
        vkGetPhysicalDeviceQueueFamilyProperties(dev.physicalDevice(), &nf, fams.data());
        tsCompute = opt.recordTimestamps && fams[computeFamily].timestampValidBits > 0;
        tsTransfer = opt.recordTimestamps && fams[transferFamily].timestampValidBits > 0;
        stampable = fams[transferFamily].timestampValidBits > 0;
        timestampsSupported = stampable && fams[computeFamily].timestampValidBits > 0;

        const uint32_t n = std::max(1u, opt.slotCount);
        const uint32_t depth = std::max(1u, opt.framesInFlight);

        upSlots.resize(n);
        downSlots.resize(n);
        upSlotValue.assign(n, 0);
        downSlotBusy.assign(n, 0);
        for (uint32_t i = 0; i < n; ++i) {
            if (GpuStatus s = createBuffer(stagingPiece, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                           VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                           VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                                               | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                           upSlots[i]);
                !s) {
                return s;
            }
            if (GpuStatus s = createBuffer(stagingPiece, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
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

        if (GpuStatus s = createPool(transferFamily, upCbs.pool); !s)
            return s;
        if (GpuStatus s = createPool(transferFamily, downCbs.pool); !s)
            return s;
        if (GpuStatus s = createPool(computeFamily, ownerCbs.pool); !s)
            return s;
        if (GpuStatus s = createPool(computeFamily, compPool); !s)
            return s;
        compCbs.resize(depth + 1);
        if (GpuStatus s = allocateCbs(compPool, depth + 1, compCbs.data()); !s)
            return s;
        miscCb = compCbs.back();
        compCbs.pop_back();

        for (VkSemaphore* sem : { &upTL, &downTL, &ownerTL, &compTL }) {
            if (GpuStatus s = dev.createTimelineSemaphore(0, *sem); !s)
                return s;
        }
        drainThread = std::thread([this] { drainLoop(); });
        return {};
    }

    uint64_t reached(VkSemaphore semaphore)
    {
        uint64_t v = 0;
        vkGetSemaphoreCounterValue(vk, semaphore, &v);
        return v;
    }

    GpuStatus beginOneTime(VkCommandBuffer cb)
    {
        if (GpuStatus s = dev.check(vkResetCommandBuffer(cb, 0), "vkResetCommandBuffer"); !s) {
            return s;
        }
        VkCommandBufferBeginInfo bi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        return dev.check(vkBeginCommandBuffer(cb, &bi), "vkBeginCommandBuffer");
    }

    // Hands out a command buffer whose last submission has completed, already begun. The caller
    // holds the lock that guards `pool`.
    GpuStatus acquireCb(CbPool& pool, size_t& index)
    {
        index = pool.entries.size();
        for (size_t i = 0; i < pool.entries.size(); ++i) {
            const CbPool::Entry& e = pool.entries[i];
            if (e.semaphore == VK_NULL_HANDLE || reached(e.semaphore) >= e.value) {
                index = i;
                break;
            }
        }
        if (index == pool.entries.size()) {
            CbPool::Entry e;
            if (GpuStatus s = allocateCbs(pool.pool, 1, &e.cb); !s) {
                return s;
            }
            pool.entries.push_back(e);
        }
        pool.entries[index].semaphore = VK_NULL_HANDLE;
        pool.entries[index].value = 0;
        return beginOneTime(pool.entries[index].cb);
    }

    GpuStatus submit(QueueKind kind, VkCommandBuffer cb, std::span<const VkSemaphoreSubmitInfo> waits,
                     VkSemaphore signal, uint64_t signalValue)
    {
        VkSemaphoreSubmitInfo s { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
        s.semaphore = signal;
        s.value = signalValue;
        s.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkCommandBufferSubmitInfo c { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
        c.commandBuffer = cb;
        VkSubmitInfo2 si { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
        si.waitSemaphoreInfoCount = static_cast<uint32_t>(waits.size());
        si.pWaitSemaphoreInfos = waits.data();
        si.commandBufferInfoCount = 1;
        si.pCommandBufferInfos = &c;
        si.signalSemaphoreInfoCount = 1;
        si.pSignalSemaphoreInfos = &s;
        return dev.submit(kind, std::span<const VkSubmitInfo2>(&si, 1), VK_NULL_HANDLE);
    }

    // Ends `cb`, submits it on the upload or download queue and tags the pool entry with the new value on that
    // direction's timeline. The caller holds upMutex or downMutex, so the value is taken and submitted under one
    // lock and the timeline is signalled in increasing order.
    GpuStatus submitTransfer(QueueKind kind, CbPool& pool, size_t index, std::span<const VkSemaphoreSubmitInfo> waits,
                             uint64_t& value)
    {
        VkCommandBuffer cb = pool.entries[index].cb;
        if (GpuStatus s = dev.check(vkEndCommandBuffer(cb), "vkEndCommandBuffer"); !s) {
            return s;
        }
        const bool up = kind == QueueKind::Upload;
        VkSemaphore timeline = up ? upTL : downTL;
        uint64_t& last = up ? upValue : downValue;
        const uint64_t v = last + 1;
        if (GpuStatus s = submit(kind, cb, waits, timeline, v); !s) {
            return s;
        }
        last = v;
        pool.entries[index] = { cb, timeline, v };
        value = v;
        return {};
    }

    template <typename RecordFn>
    GpuStatus submitOwner(RecordFn&& record, std::span<const VkSemaphoreSubmitInfo> waits, TimelineValue& done)
    {
        std::lock_guard<std::mutex> lock(ownerMutex);
        size_t index = 0;
        if (GpuStatus s = acquireCb(ownerCbs, index); !s) {
            return s;
        }
        VkCommandBuffer cb = ownerCbs.entries[index].cb;
        record(cb);
        if (GpuStatus s = dev.check(vkEndCommandBuffer(cb), "vkEndCommandBuffer"); !s) {
            return s;
        }
        const uint64_t v = ownerValue + 1;
        if (GpuStatus s = submit(QueueKind::Compute, cb, waits, ownerTL, v); !s) {
            return s;
        }
        ownerValue = v;
        ownerCbs.entries[index] = { cb, ownerTL, v };
        done = { ownerTL, v };
        return {};
    }

    void transferRelease(VkCommandBuffer cb, VkBuffer b, VkDeviceSize offset, VkDeviceSize size) const
    {
        bufferBarrier(cb, b, offset, size, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, transferFamily, computeFamily);
    }

    void computeAcquire(VkCommandBuffer cb, VkBuffer b, VkDeviceSize offset, VkDeviceSize size) const
    {
        bufferBarrier(cb, b, offset, size, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                      transferFamily, computeFamily);
    }

    void computeRelease(VkCommandBuffer cb, VkBuffer b, VkDeviceSize offset, VkDeviceSize size) const
    {
        bufferBarrier(cb, b, offset, size, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, computeFamily, transferFamily);
    }

    void transferAcquire(VkCommandBuffer cb, VkBuffer b, VkDeviceSize offset, VkDeviceSize size) const
    {
        bufferBarrier(cb, b, offset, size, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COPY_BIT,
                      VK_ACCESS_2_TRANSFER_READ_BIT, computeFamily, transferFamily);
    }

    void stamp(VkCommandBuffer cb, const StreamOptions& o, uint32_t which) const
    {
        if (stampable && o.timestampPool != VK_NULL_HANDLE) {
            vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, o.timestampPool, o.timestampQuery + which);
        }
    }

    // Splits a device-side copy at the chunk size; a single large copy corrupts data on RADV.
    void chunkedCopy(VkCommandBuffer cb, VkBuffer src, VkDeviceSize srcOffset, VkBuffer dst, VkDeviceSize dstOffset,
                     VkDeviceSize bytes) const
    {
        for (VkDeviceSize c = 0; c < bytes; c += chunk) {
            const VkBufferCopy r { srcOffset + c, dstOffset + c, std::min(chunk, bytes - c) };
            vkCmdCopyBuffer(cb, src, dst, 1, &r);
        }
    }

    void countImport(VkDeviceSize len, ImportOrigin origin)
    {
        ++importCount;
        importBytes += len;
        switch (origin) {
        case ImportOrigin::Prepare:
            ++preparedImports;
            break;
        case ImportOrigin::Upload:
            ++uploadImports;
            break;
        case ImportOrigin::Download:
            ++downloadImports;
            break;
        }
    }

    // The caller holds importMutex.
    bool cacheOverlaps(uintptr_t lo, uintptr_t hi) const
    {
        auto next = importCache.upper_bound(lo);
        if (next != importCache.end() && next->first < hi) {
            return true;
        }
        if (next != importCache.begin()) {
            auto prev = std::prev(next);
            return prev->first + prev->second.bytes > lo;
        }
        return false;
    }

    // The caller holds importMutex.
    bool pendingOverlaps(uintptr_t lo, uintptr_t hi) const
    {
        for (const PendingImport& p : pendingImports) {
            if (p.lo < hi && lo < p.hi) {
                return true;
            }
        }
        return false;
    }

    // The caller holds importMutex. Fills segs and returns true when cached imports cover all of [base, base + bytes);
    // leaves segs empty otherwise.
    bool coverFromCache(uintptr_t base, VkDeviceSize bytes, std::vector<ImportSeg>& segs) const
    {
        for (VkDeviceSize pos = 0; pos < bytes;) {
            const uintptr_t addr = base + pos;
            auto next = importCache.upper_bound(addr);
            if (next == importCache.begin()) {
                segs.clear();
                return false;
            }
            auto prev = std::prev(next);
            const uintptr_t end = prev->first + prev->second.bytes;
            if (end <= addr) {
                segs.clear();
                return false;
            }
            const VkDeviceSize len = std::min<VkDeviceSize>(end - addr, bytes - pos);
            segs.push_back({ prev->second.buffer.buffer(), addr - prev->first, pos, len, prev->first });
            pos += len;
        }
        return true;
    }

    // Covers [p, p + bytes) with imported buffers, or returns false for the staging path. Uncached
    // imports land in `owned` and must outlive the transfer that uses them. A range that cached imports
    // already cover needs no alignment: copies out of an import may start and end anywhere in it.
    bool resolveImports(const void* p, VkDeviceSize bytes, std::vector<ImportSeg>& segs, std::vector<GpuBuffer>& owned,
                        ImportOrigin origin)
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(p);
        if (importAlignment == 0 || p == nullptr) {
            return false;
        }
        const bool aligned = base % importAlignment == 0 && bytes % importAlignment == 0;
        if (opt.cacheHostImports && (cachedImports.load() != 0 || pendingCount.load() != 0)) {
            std::unique_lock<std::mutex> lock(importMutex);
            for (;;) {
                if (coverFromCache(base, bytes, segs)) {
                    return true;
                }
                // An aligned range imports what is missing itself, and waits for overlapping imports there.
                if (aligned || !pendingOverlaps(base, base + bytes)) {
                    break;
                }
                importCv.wait(lock);
            }
        }
        if (!aligned) {
            return false;
        }
        char* host = static_cast<char*>(const_cast<void*>(p));
        if (!opt.cacheHostImports) {
            for (VkDeviceSize pos = 0; pos < bytes;) {
                const VkDeviceSize len = std::min(importPieceMax, bytes - pos);
                GpuBuffer b;
                if (!GpuBuffer::createHostImported(dev, host + pos, len, kImportUsage, b)) {
                    segs.clear();
                    owned.clear();
                    return false;
                }
                countImport(len, origin);
                segs.push_back({ b.buffer(), 0, pos, len, 0 });
                owned.push_back(std::move(b));
                pos += len;
            }
            return true;
        }

        // Pinning takes long enough that holding the lock through it would serialize every thread's imports,
        // so the lock only guards the lookups and the insertion.
        std::unique_lock<std::mutex> lock(importMutex);
        for (VkDeviceSize pos = 0; pos < bytes;) {
            const uintptr_t addr = base + pos;
            auto next = importCache.upper_bound(addr);
            if (next != importCache.begin()) {
                auto prev = std::prev(next);
                if (prev->first + prev->second.bytes > addr) {
                    const VkDeviceSize len = std::min<VkDeviceSize>(prev->first + prev->second.bytes - addr, bytes - pos);
                    segs.push_back({ prev->second.buffer.buffer(), addr - prev->first, pos, len, prev->first });
                    pos += len;
                    continue;
                }
            }
            // Another thread is importing this address: its import serves this range too once cached.
            if (pendingOverlaps(addr, addr + 1)) {
                importCv.wait(lock);
                continue;
            }
            VkDeviceSize len = std::min(importPieceMax, bytes - pos);
            if (next != importCache.end()) {
                len = std::min<VkDeviceSize>(len, next->first - addr);
            }
            for (const PendingImport& other : pendingImports) {
                if (other.lo > addr) {
                    len = std::min<VkDeviceSize>(len, other.lo - addr);
                }
            }
            auto mine = pendingImports.insert(pendingImports.end(), PendingImport { addr, addr + len, false });
            pendingCount.store(pendingImports.size());
            lock.unlock();

            CachedImport entry;
            const bool created = static_cast<bool>(GpuBuffer::createHostImported(dev, host + pos, len, kImportUsage, entry.buffer));
            if (created) {
                countImport(len, origin);
                if (opt.onImportCreated) {
                    opt.onImportCreated(host + pos, len);
                }
            }

            lock.lock();
            const bool forgotten = mine->forgotten;
            const bool keep = created && !forgotten && !cacheOverlaps(addr, addr + len);
            if (keep) {
                entry.bytes = len;
                segs.push_back({ entry.buffer.buffer(), 0, pos, std::min(len, bytes - pos), addr });
                importCache.emplace(addr, std::move(entry));
                cachedImports.store(importCache.size(), std::memory_order_relaxed);
            } else if (created) {
                ++droppedImports;
                // forget() waits for the pending entry, so the import must be gone before the entry is.
                lock.unlock();
                entry.buffer = GpuBuffer();
                lock.lock();
            }
            pendingImports.erase(mine);
            pendingCount.store(pendingImports.size());
            importCv.notify_all();
            if (!created || forgotten) {
                segs.clear();
                return false;
            }
            if (keep) {
                pos += len;
            }
        }
        return true;
    }

    bool prepare(HostRange range)
    {
        if (!opt.cacheHostImports || dev.isLost()) {
            return false;
        }
        std::vector<ImportSeg> segs;
        std::vector<GpuBuffer> owned;
        return resolveImports(range.data, range.bytes, segs, owned, ImportOrigin::Prepare);
    }

    void markUsed(const std::vector<ImportSeg>& segs, std::vector<GpuBuffer>& owned, TimelineValue use)
    {
        if (opt.cacheHostImports) {
            std::lock_guard<std::mutex> lock(importMutex);
            for (const ImportSeg& s : segs) {
                if (auto it = importCache.find(s.cacheKey); it != importCache.end()) {
                    uint64_t& last = use.semaphore == upTL ? it->second.lastUpload : it->second.lastDownload;
                    last = std::max(last, use.value);
                }
            }
        }
        if (!owned.empty()) {
            std::lock_guard<std::mutex> lock(retireMutex);
            for (GpuBuffer& b : owned) {
                retired.push_back({ std::move(b), use });
            }
            owned.clear();
        }
    }

    void collectRetired()
    {
        std::vector<Retired> done;
        {
            std::lock_guard<std::mutex> lock(retireMutex);
            if (retired.empty()) {
                return;
            }
            const uint64_t up = reached(upTL);
            const uint64_t down = reached(downTL);
            auto split = std::stable_partition(retired.begin(), retired.end(), [&](const Retired& x) {
                return x.lastUse.value > (x.lastUse.semaphore == upTL ? up : down);
            });
            std::move(split, retired.end(), std::back_inserter(done));
            retired.erase(split, retired.end());
        }
    }

    void forget(const void* ptr, size_t bytes)
    {
        if (cachedImports.load() == 0 && pendingCount.load() == 0) {
            return;
        }
        const uintptr_t lo = reinterpret_cast<uintptr_t>(ptr);
        const uintptr_t hi = lo + std::max<size_t>(bytes, 1);
        std::vector<CachedImport> dropped;
        {
            std::unique_lock<std::mutex> lock(importMutex);
            for (PendingImport& p : pendingImports) {
                if (p.lo < hi && lo < p.hi) {
                    p.forgotten = true;
                }
            }
            importCv.wait(lock, [&] { return !pendingOverlaps(lo, hi); });
            auto it = importCache.upper_bound(lo);
            if (it != importCache.begin()) {
                auto prev = std::prev(it);
                if (prev->first + prev->second.bytes > lo) {
                    it = prev;
                }
            }
            while (it != importCache.end() && it->first < hi) {
                dropped.push_back(std::move(it->second));
                it = importCache.erase(it);
            }
            cachedImports.store(importCache.size(), std::memory_order_relaxed);
        }
        for (CachedImport& c : dropped) {
            if (c.lastUpload > reached(upTL)) {
                dev.waitSemaphore(upTL, c.lastUpload, kWaitTimeoutNs);
            }
            if (c.lastDownload > reached(downTL)) {
                dev.waitSemaphore(downTL, c.lastDownload, kWaitTimeoutNs);
            }
        }
    }

    void releaseCachedImports()
    {
        std::unique_lock<std::mutex> lock(importMutex);
        for (PendingImport& p : pendingImports) {
            p.forgotten = true;
        }
        importCv.wait(lock, [&] { return pendingImports.empty(); });
        dev.drainQueues();
        importCache.clear();
        cachedImports.store(0, std::memory_order_relaxed);
        std::lock_guard<std::mutex> rl(retireMutex);
        retired.clear();
    }

    GpuStatus checkRange(const GpuBuffer& buffer, VkDeviceSize offset, const HostRange& host)
    {
        if (!host.data || host.bytes == 0 || !buffer) {
            return fail(VK_ERROR_UNKNOWN, "transfer needs a host range, a buffer and a non-zero size");
        }
        if (offset > buffer.size() || host.bytes > buffer.size() - offset) {
            return fail(VK_ERROR_UNKNOWN, "transfer range exceeds the buffer");
        }
        if (dev.isLost()) {
            return fail(VK_ERROR_DEVICE_LOST, "device is lost");
        }
        return {};
    }

    GpuStatus upload(HostRange src, GpuBuffer& dst, VkDeviceSize offset, TimelineValue& done, const StreamOptions& o)
    {
        done = {};
        if (GpuStatus s = checkRange(dst, offset, src); !s) {
            return s;
        }
        collectRetired();
        const Clock::time_point h0 = Clock::now();
        const std::vector<VkSemaphoreSubmitInfo> waits = waitInfos(o.waits);
        const bool handoff = ownership && o.handoff != ComputeHandoff::None;
        const VkDeviceSize bytes = src.bytes;
        uint64_t v = 0;
        {
            std::vector<ImportSeg> segs;
            std::vector<GpuBuffer> owned;
            const bool imported = resolveImports(src.data, bytes, segs, owned, ImportOrigin::Upload);
            std::lock_guard<std::mutex> lock(upMutex);
            if (o.path) {
                *o.path = imported ? TransferPath::HostImport : TransferPath::Staging;
            }
            if (imported) {
                if (o.hostSpan) {
                    const Clock::time_point h1 = Clock::now();
                    *o.hostSpan = { msSince(o.hostOrigin, h0), msSince(o.hostOrigin, h1), msSince(h0, h1) };
                }
                size_t ci = 0;
                if (GpuStatus s = acquireCb(upCbs, ci); !s) {
                    return s;
                }
                VkCommandBuffer cb = upCbs.entries[ci].cb;
                stamp(cb, o, 0);
                for (const ImportSeg& seg : segs) {
                    chunkedCopy(cb, seg.buffer, seg.bufferOffset, dst.buffer(), offset + seg.hostOffset, seg.bytes);
                }
                if (handoff) {
                    transferRelease(cb, dst.buffer(), offset, bytes);
                }
                stamp(cb, o, 1);
                if (GpuStatus s = submitTransfer(QueueKind::Upload, upCbs, ci, waits, v); !s) {
                    return s;
                }
                markUsed(segs, owned, { upTL, v });
            } else {
                const uint32_t n = static_cast<uint32_t>(upSlots.size());
                const VkDeviceSize pieces = (bytes + stagingPiece - 1) / stagingPiece;
                for (VkDeviceSize i = 0; i < pieces; ++i) {
                    const uint32_t r = upNext++ % n;
                    if (GpuStatus s = dev.waitSemaphore(upTL, upSlotValue[r], kWaitTimeoutNs); !s) {
                        return s;
                    }
                    const VkDeviceSize off = i * stagingPiece;
                    const VkDeviceSize len = std::min(stagingPiece, bytes - off);
                    const Clock::time_point cs = Clock::now();
                    const double work = copies.copy(upSlots[r].mapped, static_cast<const char*>(src.data) + off, len);
                    vmaFlushAllocation(dev.allocator(), upSlots[r].allocation, 0, len);
                    if (o.hostSpan) {
                        const Clock::time_point ce = Clock::now();
                        if (i == 0)
                            o.hostSpan->beginMs = msSince(o.hostOrigin, cs);
                        o.hostSpan->endMs = msSince(o.hostOrigin, ce);
                        o.hostSpan->busyMs += work;
                    }

                    size_t ci = 0;
                    if (GpuStatus s = acquireCb(upCbs, ci); !s) {
                        return s;
                    }
                    VkCommandBuffer cb = upCbs.entries[ci].cb;
                    const bool last = i + 1 == pieces;
                    if (i == 0) {
                        stamp(cb, o, 0);
                    }
                    const VkBufferCopy region { 0, offset + off, len };
                    vkCmdCopyBuffer(cb, upSlots[r].buffer, dst.buffer(), 1, &region);
                    if (last && handoff) {
                        transferRelease(cb, dst.buffer(), offset, bytes);
                    }
                    if (last) {
                        stamp(cb, o, 1);
                    }
                    const std::span<const VkSemaphoreSubmitInfo> w
                        = i == 0 ? std::span<const VkSemaphoreSubmitInfo>(waits) : std::span<const VkSemaphoreSubmitInfo>();
                    if (GpuStatus s = submitTransfer(QueueKind::Upload, upCbs, ci, w, v); !s) {
                        return s;
                    }
                    upSlotValue[r] = v;
                }
            }
        }
        if (handoff && o.handoff == ComputeHandoff::Submit) {
            const VkSemaphoreSubmitInfo w = TimelineValue { upTL, v }.waitInfo();
            return submitOwner([&](VkCommandBuffer cb) { computeAcquire(cb, dst.buffer(), offset, bytes); },
                               std::span<const VkSemaphoreSubmitInfo>(&w, 1), done);
        }
        done = { upTL, v };
        return {};
    }

    GpuStatus download(GpuBuffer& src, VkDeviceSize offset, HostRange dst, TimelineValue& done, const StreamOptions& o)
    {
        done = {};
        if (GpuStatus s = checkRange(src, offset, dst); !s) {
            return s;
        }
        collectRetired();
        const Clock::time_point h0 = Clock::now();
        std::vector<VkSemaphoreSubmitInfo> waits = waitInfos(o.waits);
        const bool handoff = ownership && o.handoff != ComputeHandoff::None;
        const VkDeviceSize bytes = dst.bytes;
        if (handoff && o.handoff == ComputeHandoff::Submit) {
            TimelineValue released;
            if (GpuStatus s = submitOwner([&](VkCommandBuffer cb) { computeRelease(cb, src.buffer(), offset, bytes); },
                                          waits, released);
                !s) {
                return s;
            }
            waits = { released.waitInfo() };
        }

        uint64_t v = 0;
        std::vector<ImportSeg> segs;
        std::vector<GpuBuffer> owned;
        const bool imported = resolveImports(dst.data, bytes, segs, owned, ImportOrigin::Download);
        std::lock_guard<std::mutex> lock(downMutex);
        if (o.path) {
            *o.path = imported ? TransferPath::HostImport : TransferPath::Staging;
        }
        if (imported) {
            if (o.hostSpan) {
                const Clock::time_point h1 = Clock::now();
                *o.hostSpan = { msSince(o.hostOrigin, h0), msSince(o.hostOrigin, h1), msSince(h0, h1) };
            }
            size_t ci = 0;
            if (GpuStatus s = acquireCb(downCbs, ci); !s) {
                return s;
            }
            VkCommandBuffer cb = downCbs.entries[ci].cb;
            stamp(cb, o, 0);
            if (handoff) {
                transferAcquire(cb, src.buffer(), offset, bytes);
            }
            for (const ImportSeg& seg : segs) {
                chunkedCopy(cb, src.buffer(), offset + seg.hostOffset, seg.buffer, seg.bufferOffset, seg.bytes);
            }
            copyToHostBarrier(cb);
            stamp(cb, o, 1);
            if (GpuStatus s = submitTransfer(QueueKind::Download, downCbs, ci, waits, v); !s) {
                return s;
            }
            markUsed(segs, owned, { downTL, v });
            done = { downTL, v };
            return {};
        }

        const uint32_t n = static_cast<uint32_t>(downSlots.size());
        const VkDeviceSize pieces = (bytes + stagingPiece - 1) / stagingPiece;
        for (VkDeviceSize i = 0; i < pieces; ++i) {
            const uint32_t r = downNext++ % n;
            {
                std::unique_lock<std::mutex> dl(drainMutex);
                drainCv.wait(dl, [&] { return !downSlotBusy[r]; });
            }
            const VkDeviceSize off = i * stagingPiece;
            const VkDeviceSize len = std::min(stagingPiece, bytes - off);
            size_t ci = 0;
            if (GpuStatus s = acquireCb(downCbs, ci); !s) {
                return s;
            }
            VkCommandBuffer cb = downCbs.entries[ci].cb;
            const bool last = i + 1 == pieces;
            if (i == 0) {
                stamp(cb, o, 0);
                if (handoff) {
                    transferAcquire(cb, src.buffer(), offset, bytes);
                }
            }
            const VkBufferCopy region { offset + off, 0, len };
            vkCmdCopyBuffer(cb, src.buffer(), downSlots[r].buffer, 1, &region);
            copyToHostBarrier(cb);
            if (last) {
                stamp(cb, o, 1);
            }
            const std::span<const VkSemaphoreSubmitInfo> w
                = i == 0 ? std::span<const VkSemaphoreSubmitInfo>(waits) : std::span<const VkSemaphoreSubmitInfo>();
            if (GpuStatus s = submitTransfer(QueueKind::Download, downCbs, ci, w, v); !s) {
                return s;
            }
            {
                std::lock_guard<std::mutex> dl(drainMutex);
                downSlotBusy[r] = 1;
                drain.push_back({ r, v, static_cast<char*>(dst.data) + off, len, o.hostSpan, o.hostOrigin, i == 0 });
            }
            drainCv.notify_all();
        }
        done = { downTL, v };
        return {};
    }

    void drainLoop()
    {
        for (;;) {
            DrainItem item;
            {
                std::unique_lock<std::mutex> lock(drainMutex);
                drainCv.wait(lock, [&] { return drainStop || !drain.empty(); });
                if (drain.empty()) {
                    return;
                }
                item = drain.front();
            }
            GpuStatus s = dev.waitSemaphore(downTL, item.value, kWaitTimeoutNs);
            if (s) {
                const VmaBuffer& slot = downSlots[item.slot];
                vmaInvalidateAllocation(dev.allocator(), slot.allocation, 0, item.bytes);
                const Clock::time_point cs = Clock::now();
                const double work = copies.copy(item.dst, slot.mapped, item.bytes);
                if (item.span) {
                    if (item.first)
                        item.span->beginMs = msSince(item.origin, cs);
                    item.span->busyMs += work;
                    item.span->endMs = msSince(item.origin, Clock::now());
                }
            }
            {
                std::lock_guard<std::mutex> lock(drainMutex);
                if (!s && drainError.ok()) {
                    drainError = std::move(s);
                }
                downSlotBusy[item.slot] = 0;
                drain.pop_front();
            }
            drainCv.notify_all();
        }
    }

    GpuStatus waitDrained(uint64_t value)
    {
        std::unique_lock<std::mutex> lock(drainMutex);
        drainCv.wait(lock, [&] { return drain.empty() || drain.front().value > value; });
        return drainError;
    }

    GpuStatus wait(TimelineValue t)
    {
        if (!t) {
            return {};
        }
        if (GpuStatus s = dev.waitSemaphore(t.semaphore, t.value, kWaitTimeoutNs); !s) {
            return s;
        }
        if (t.semaphore == downTL) {
            if (GpuStatus s = waitDrained(t.value); !s) {
                return s;
            }
        }
        collectRetired();
        return {};
    }

    GpuStatus ensureDeviceBuffers(VkDeviceSize bytes)
    {
        if (bytes <= devCapacity) {
            return {};
        }
        devCapacity = 0;
        for (auto* v : { &devIn, &devOut }) {
            for (GpuBuffer& b : *v) {
                if (GpuStatus s = GpuBuffer::createDeviceLocal(dev, bytes, kGpuBufferDefaultUsage, b); !s) {
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
            if (queries)
                vkDestroyQueryPool(vk, queries, nullptr);
            queries = VK_NULL_HANDLE;
            queryCapacity = 0;
            VkQueryPoolCreateInfo ci { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
            ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
            ci.queryCount = count;
            if (GpuStatus s = dev.check(vkCreateQueryPool(vk, &ci, nullptr, &queries), "vkCreateQueryPool"); !s) {
                return s;
            }
            queryCapacity = count;
        }
        // hostQueryReset is not enabled on the device, so the reset goes through a command buffer.
        if (GpuStatus s = beginOneTime(miscCb); !s)
            return s;
        vkCmdResetQueryPool(miscCb, queries, 0, count);
        if (GpuStatus s = dev.check(vkEndCommandBuffer(miscCb), "vkEndCommandBuffer"); !s)
            return s;
        const uint64_t v = ++compValue;
        if (GpuStatus s = submit(QueueKind::Compute, miscCb, {}, compTL, v); !s)
            return s;
        return dev.waitSemaphore(compTL, v, kWaitTimeoutNs);
    }
};

GpuTransfer::GpuTransfer(GpuDevice& device, const GpuTransferOptions& options)
    : device_(device)
    , options_(options)
{
}

GpuTransfer::~GpuTransfer() = default;

bool
GpuTransfer::timestampsSupported() const
{
    return impl_->timestampsSupported;
}

double
GpuTransfer::timestampPeriodNs() const
{
    return impl_->timestampPeriod;
}

GpuStatus
GpuTransfer::create(GpuDevice& device, const GpuTransferOptions& options, std::unique_ptr<GpuTransfer>& out)
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

GpuStatus
GpuTransfer::upload(HostRange src, GpuBuffer& dst, VkDeviceSize offset, TimelineValue& done,
                    const StreamOptions& options)
{
    return impl_->upload(src, dst, offset, done, options);
}

GpuStatus
GpuTransfer::download(GpuBuffer& src, VkDeviceSize offset, HostRange dst, TimelineValue& done,
                      const StreamOptions& options)
{
    return impl_->download(src, offset, dst, done, options);
}

GpuStatus
GpuTransfer::wait(TimelineValue value)
{
    return impl_->wait(value);
}

void
GpuTransfer::recordComputeAcquire(VkCommandBuffer computeCmd, const GpuBuffer& buffer, VkDeviceSize offset,
                                  VkDeviceSize bytes) const
{
    if (impl_->ownership) {
        impl_->computeAcquire(computeCmd, buffer.buffer(), offset, bytes);
    }
}

void
GpuTransfer::recordComputeRelease(VkCommandBuffer computeCmd, const GpuBuffer& buffer, VkDeviceSize offset,
                                  VkDeviceSize bytes) const
{
    if (impl_->ownership) {
        impl_->computeRelease(computeCmd, buffer.buffer(), offset, bytes);
    }
}

GpuStatus
GpuTransfer::process(std::span<const TransferFrame> frames, const ComputeRecordFn& record,
                     TransferTimeline* timeline)
{
    Impl& m = *impl_;
    if (frames.empty()) {
        return {};
    }
    if (device_.isLost()) {
        return fail(VK_ERROR_DEVICE_LOST, "device is lost");
    }
    std::lock_guard<std::mutex> processLock(m.processMutex);

    const uint32_t F = static_cast<uint32_t>(frames.size());
    const uint32_t D = static_cast<uint32_t>(m.devIn.size());

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
    if (GpuStatus s = m.ensureDeviceBuffers(maxBytes); !s) {
        return s;
    }

    const bool timestamps = m.tsCompute || m.tsTransfer;
    if (timestamps) {
        if (GpuStatus s = m.ensureQueries(kQueriesPerFrame * F); !s) {
            return s;
        }
    }
    auto qUp = [&](uint32_t k) { return k * kQueriesPerFrame; };
    auto qComp = [&](uint32_t k) { return k * kQueriesPerFrame + 2; };
    auto qDown = [&](uint32_t k) { return k * kQueriesPerFrame + 4; };

    std::vector<FrameTimeline> tl(F);
    for (uint32_t k = 0; k < F; ++k) {
        tl[k].strips = static_cast<uint32_t>((frames[k].bytes + m.stagingPiece - 1) / m.stagingPiece);
    }

    const uint64_t compBase = m.compValue;
    auto computeDone = [&](uint32_t k) { return TimelineValue { m.compTL, compBase + k + 1 }; };
    std::vector<TimelineValue> uploadDone(F);
    std::vector<TimelineValue> downloadDone(F);

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

    auto streamOptions = [&](const TimelineValue& wait, TransferPath* path, uint32_t query, TimeSpan* host) {
        StreamOptions so;
        so.waits = std::span<const TimelineValue>(&wait, wait ? 1 : 0);
        so.handoff = ComputeHandoff::Record;
        so.path = path;
        if (timestamps) {
            so.timestampPool = m.queries;
            so.timestampQuery = query;
        }
        so.hostSpan = host;
        so.hostOrigin = t0;
        return so;
    };

    auto uploader = [&]() -> GpuStatus {
        for (uint32_t k = 0; k < F; ++k) {
            const TransferFrame& f = frames[k];
            const uint32_t d = k % D;
            FrameTimeline& ft = tl[k];
            const TimelineValue inFree = k >= D ? computeDone(k - D) : TimelineValue {};
            const StreamOptions so = streamOptions(inFree, &ft.uploadPath, qUp(k), &ft.hostUpload);
            if (GpuStatus s = m.upload(HostRange { const_cast<void*>(f.src), f.bytes }, m.devIn[d], 0, uploadDone[k], so);
                !s) {
                return s;
            }

            TimelineValue outFree;
            if (k >= D) {
                std::unique_lock<std::mutex> lock(mtx);
                cv.wait(lock, [&] { return aborted || downloadSubmitted > k - D; });
                if (aborted)
                    return {};
                outFree = downloadDone[k - D];
                lock.unlock();
                if (GpuStatus s = device_.waitSemaphore(m.compTL, computeDone(k - D).value, kWaitTimeoutNs); !s) {
                    return s;
                }
            }

            VkCommandBuffer cb = m.compCbs[d];
            if (GpuStatus s = m.beginOneTime(cb); !s)
                return s;
            if (m.tsCompute)
                vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m.queries, qComp(k));
            recordComputeAcquire(cb, m.devIn[d], 0, f.bytes);
            if (record) {
                if (GpuStatus s = record(cb, ComputeBinding { m.devIn[d].buffer(), m.devOut[d].buffer(), f.bytes, k });
                    !s) {
                    return s;
                }
            } else {
                const VkBufferCopy region { 0, 0, f.bytes };
                vkCmdCopyBuffer(cb, m.devIn[d].buffer(), m.devOut[d].buffer(), 1, &region);
            }
            recordComputeRelease(cb, m.devOut[d], downOffset(f), downBytes(f));
            if (m.tsCompute)
                vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m.queries, qComp(k) + 1);
            if (GpuStatus s = device_.check(vkEndCommandBuffer(cb), "vkEndCommandBuffer"); !s)
                return s;
            const TimelineValue deps[] = { uploadDone[k], outFree };
            const std::vector<VkSemaphoreSubmitInfo> waits = waitInfos(deps);
            if (GpuStatus s = m.submit(QueueKind::Compute, cb, waits, m.compTL, computeDone(k).value); !s) {
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

    auto downloader = [&]() -> GpuStatus {
        for (uint32_t k = 0; k < F; ++k) {
            {
                std::unique_lock<std::mutex> lock(mtx);
                cv.wait(lock, [&] { return aborted || computeSubmitted > k; });
                if (aborted)
                    return {};
            }
            const TransferFrame& f = frames[k];
            FrameTimeline& ft = tl[k];
            const TimelineValue computed = computeDone(k);
            const StreamOptions so = streamOptions(computed, &ft.downloadPath, qDown(k), &ft.hostDownload);
            TimelineValue done;
            if (GpuStatus s = m.download(m.devOut[k % D], downOffset(f), HostRange { f.dst, downBytes(f) }, done, so);
                !s) {
                return s;
            }
            {
                std::lock_guard<std::mutex> lock(mtx);
                downloadDone[k] = done;
                downloadSubmitted = k + 1;
            }
            cv.notify_all();
        }
        return m.wait(downloadDone[F - 1]);
    };

    std::exception_ptr upException;
    std::exception_ptr downException;
    std::thread down([&] {
        try {
            if (GpuStatus s = downloader(); !s)
                raise(std::move(s));
        } catch (...) {
            downException = std::current_exception();
            raise(fail(VK_ERROR_UNKNOWN, "GpuTransfer download thread threw"));
        }
    });
    try {
        if (GpuStatus s = uploader(); !s)
            raise(std::move(s));
    } catch (...) {
        upException = std::current_exception();
        raise(fail(VK_ERROR_UNKNOWN, "GpuTransfer compute callback or upload threw"));
    }
    down.join();
    const Clock::time_point t1 = Clock::now();

    m.compValue = compBase + F;
    if (aborted) {
        m.dev.drainQueues();
        m.waitDrained(std::numeric_limits<uint64_t>::max());
    } else if (GpuStatus s = device_.waitSemaphore(m.compTL, m.compValue, kWaitTimeoutNs); !s) {
        raise(std::move(s));
        m.dev.drainQueues();
        m.waitDrained(std::numeric_limits<uint64_t>::max());
    }
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
            const uint32_t count = kQueriesPerFrame * F;
            std::vector<uint8_t> written(count, 0);
            for (uint32_t k = 0; k < F; ++k) {
                for (uint32_t i = 0; i < 2; ++i) {
                    written[qUp(k) + i] = m.tsTransfer;
                    written[qComp(k) + i] = m.tsCompute;
                    written[qDown(k) + i] = m.tsTransfer;
                }
            }
            std::vector<uint64_t> ticks(count, 0);
            for (uint32_t q = 0; q < count; ++q) {
                if (!written[q])
                    continue;
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
            for (uint32_t q = 0; q < count; ++q) {
                if (written[q]) {
                    origin = std::min(origin, ticks[q]);
                    last = std::max(last, ticks[q]);
                }
            }
            const double toMs = double(m.timestampPeriod) * 1e-6;
            timeline->gpuSpanMs = origin <= last ? double(last - origin) * toMs : 0.0;
            auto span = [&](TimeSpan& out, uint32_t q) {
                if (!written[q] || !written[q + 1])
                    return;
                out.beginMs = double(ticks[q] - origin) * toMs;
                out.endMs = double(ticks[q + 1] - origin) * toMs;
                out.busyMs = out.endMs - out.beginMs;
            };
            for (uint32_t k = 0; k < F; ++k) {
                FrameTimeline& ft = timeline->frames[k];
                span(ft.gpuUpload, qUp(k));
                span(ft.gpuCompute, qComp(k));
                span(ft.gpuDownload, qDown(k));
            }
        }
    }
    return {};
}

void
GpuTransfer::forgetHostAllocation(const void* ptr, size_t bytes)
{
    impl_->forget(ptr, bytes);
}

void
GpuTransfer::releaseHostImports()
{
    impl_->releaseCachedImports();
}

uint64_t
GpuTransfer::hostImportCount() const
{
    return impl_->importCount.load();
}

uint64_t
GpuTransfer::hostImportBytes() const
{
    return impl_->importBytes.load();
}

HostImportStats
GpuTransfer::hostImportStats() const
{
    const Impl& m = *impl_;
    HostImportStats s;
    s.created = m.importCount.load();
    s.bytes = m.importBytes.load();
    s.prepared = m.preparedImports.load();
    s.atUpload = m.uploadImports.load();
    s.atDownload = m.downloadImports.load();
    s.dropped = m.droppedImports.load();
    s.cached = m.cachedImports.load();
    return s;
}

bool
GpuTransfer::prepareImport(HostRange range)
{
    return impl_->prepare(range);
}

} // namespace gpu
