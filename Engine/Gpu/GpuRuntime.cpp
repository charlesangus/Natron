#include "GpuRuntime.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <thread>

#include <unistd.h>

namespace gpu {

void
GpuImportJob::finish()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        finished_ = true;
    }
    cv_.notify_all();
}

void
GpuImportJob::advance()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++prepared_;
    }
    cv_.notify_all();
}

void
GpuImportJob::wait()
{
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [&] { return finished_; });
}

void
GpuImportJob::waitFor(std::size_t count)
{
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [&] { return finished_ || prepared_ >= count; });
}

void
GpuImportJob::cancel()
{
    cancelled_.store(true);
    wait();
}

bool
GpuImportJob::finished() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return finished_;
}

struct GpuImportPreparer {
    explicit GpuImportPreparer(GpuTransfer& t)
        : transfer(t)
        , pageSize(std::max<long>(1, sysconf(_SC_PAGESIZE)))
        , thread([this] { run(); })
    {
    }

    ~GpuImportPreparer()
    {
        std::deque<std::shared_ptr<GpuImportJob>> skipped;
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop = true;
            skipped.swap(queue);
            if (current) {
                current->cancelled_.store(true);
            }
        }
        cv.notify_all();
        thread.join();
        for (const std::shared_ptr<GpuImportJob>& job : skipped) {
            job->finish();
        }
    }

    void push(const std::shared_ptr<GpuImportJob>& job)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            queue.push_back(job);
        }
        cv.notify_one();
    }

    // The pages are written, not just read: a read of an untouched anonymous page maps the shared zero page,
    // which pinning would have to replace anyway.
    void touch(const HostRange& range) const
    {
        volatile unsigned char* p = static_cast<volatile unsigned char*>(range.data);
        for (VkDeviceSize off = 0; off < range.bytes; off += pageSize) {
            p[off] = p[off];
        }
    }

    void run()
    {
        for (;;) {
            std::shared_ptr<GpuImportJob> job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [&] { return stop || !queue.empty(); });
                if (stop) {
                    return;
                }
                job = queue.front();
                queue.pop_front();
                current = job;
            }
            for (const HostRange& range : job->ranges_) {
                if (job->cancelled_.load()) {
                    break;
                }
                if (range.data && range.bytes) {
                    touch(range);
                    transfer.prepareImport(range);
                }
                job->advance();
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                current.reset();
            }
            job->finish();
        }
    }

    GpuTransfer& transfer;
    const VkDeviceSize pageSize;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::shared_ptr<GpuImportJob>> queue;
    std::shared_ptr<GpuImportJob> current;
    bool stop = false;
    std::thread thread;
};

// The busy load GpuBench's busy-gap mode measured as keeping copies at their back-to-back rate: two submissions of
// 16 MiB device-local copies on the compute queue, so that the queue is never empty while the host waits for one.
class GpuKeepAlive {
public:
    explicit GpuKeepAlive(GpuDevice& device)
        : device_(device)
    {
    }

    ~GpuKeepAlive() { stop(); }

    GpuKeepAlive(const GpuKeepAlive&) = delete;
    GpuKeepAlive& operator=(const GpuKeepAlive&) = delete;

    void hold(double limitMs)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                return;
            }
            ++holders_;
            const Clock::time_point until
                = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(limitMs));
            deadline_ = std::max(deadline_, until);
            if (!thread_.joinable()) {
                thread_ = std::thread([this] { run(); });
            }
        }
        cv_.notify_all();
    }

    void release()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (holders_ > 0) {
            --holders_;
        }
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    typedef std::chrono::steady_clock Clock;

    static constexpr VkDeviceSize kBytes = VkDeviceSize(16) << 20;
    static constexpr int kCopyPairs = 4;

    bool wantedLocked() const { return !stopping_ && (holders_ > 0) && (Clock::now() < deadline_); }

    bool wanted()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return wantedLocked();
    }

    void run()
    {
        if (init()) {
            for (;;) {
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    cv_.wait(lock, [&] { return stopping_ || wantedLocked(); });
                    if (stopping_) {
                        break;
                    }
                }
                if (!keepBusy()) {
                    break;
                }
            }
        }
        destroy();
    }

    bool init()
    {
        if (!GpuBuffer::createDeviceLocal(device_, kBytes, kGpuBufferDefaultUsage, a_)
            || !GpuBuffer::createDeviceLocal(device_, kBytes, kGpuBufferDefaultUsage, b_)) {
            return false;
        }
        const VkDevice vk = device_.device();
        VkCommandPoolCreateInfo pci { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pci.queueFamilyIndex = device_.queueFamily(QueueKind::Compute);
        if (vkCreateCommandPool(vk, &pci, nullptr, &pool_) != VK_SUCCESS) {
            pool_ = VK_NULL_HANDLE;
            return false;
        }
        VkCommandBufferAllocateInfo cai { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cai.commandPool = pool_;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = (uint32_t)cmd_.size();
        if (vkAllocateCommandBuffers(vk, &cai, cmd_.data()) != VK_SUCCESS) {
            return false;
        }
        VkFenceCreateInfo fci { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        for (VkFence& fence : fence_) {
            if (vkCreateFence(vk, &fci, nullptr, &fence) != VK_SUCCESS) {
                fence = VK_NULL_HANDLE;
                return false;
            }
        }
        const VkBufferCopy region { 0, 0, kBytes };
        for (VkCommandBuffer cmd : cmd_) {
            VkCommandBufferBeginInfo bi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) {
                return false;
            }
            // Every copy reads what the previous one wrote and overwrites what the one before that read, and the
            // two command buffers in flight share the buffers, so each copy, the first included, waits on the last.
            VkMemoryBarrier2 mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
            mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            mb.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
            VkDependencyInfo dep { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers = &mb;
            for (int i = 0; i < kCopyPairs; ++i) {
                vkCmdPipelineBarrier2(cmd, &dep);
                vkCmdCopyBuffer(cmd, a_.buffer(), b_.buffer(), 1, &region);
                vkCmdPipelineBarrier2(cmd, &dep);
                vkCmdCopyBuffer(cmd, b_.buffer(), a_.buffer(), 1, &region);
            }
            if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
                return false;
            }
        }
        return true;
    }

    bool submit(std::size_t i)
    {
        if (vkResetFences(device_.device(), 1, &fence_[i]) != VK_SUCCESS) {
            return false;
        }
        VkCommandBufferSubmitInfo ci { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
        ci.commandBuffer = cmd_[i];
        VkSubmitInfo2 si { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
        si.commandBufferInfoCount = 1;
        si.pCommandBufferInfos = &ci;
        if (!device_.submitUncounted(QueueKind::Compute, std::span<const VkSubmitInfo2>(&si, 1), fence_[i])) {
            return false;
        }
        pending_[i] = true;
        return true;
    }

    // Returns false when the fence can no longer be waited for, the device being lost.
    bool await(std::size_t i)
    {
        if (!pending_[i]) {
            return true;
        }
        for (;;) {
            const VkResult r = vkWaitForFences(device_.device(), 1, &fence_[i], VK_TRUE, 100'000'000ull);
            if (r == VK_TIMEOUT) {
                continue;
            }
            if (r != VK_SUCCESS) {
                device_.check(r, "vkWaitForFences");
                abandoned_ = true;
                return false;
            }
            pending_[i] = false;
            return true;
        }
    }

    // Runs until no hold wants it any more, then lets the submissions in flight finish.
    bool keepBusy()
    {
        bool ok = submit(0) && submit(1);
        std::size_t i = 0;
        while (ok) {
            ok = await(i);
            if (!ok || !wanted()) {
                break;
            }
            ok = submit(i);
            i ^= 1;
        }
        return await(0) && await(1) && ok;
    }

    void destroy()
    {
        const VkDevice vk = device_.device();
        // A command buffer still pending on a lost device must not be freed.
        if (!abandoned_) {
            for (VkFence& fence : fence_) {
                if (fence) {
                    vkDestroyFence(vk, fence, nullptr);
                    fence = VK_NULL_HANDLE;
                }
            }
            if (pool_) {
                vkDestroyCommandPool(vk, pool_, nullptr);
                pool_ = VK_NULL_HANDLE;
            }
        }
        a_.reset();
        b_.reset();
    }

    GpuDevice& device_;
    std::mutex mutex_;
    std::condition_variable cv_;
    int holders_ = 0;
    Clock::time_point deadline_ {};
    bool stopping_ = false;
    std::thread thread_;

    GpuBuffer a_;
    GpuBuffer b_;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, 2> cmd_ {};
    std::array<VkFence, 2> fence_ {};
    std::array<bool, 2> pending_ {};
    bool abandoned_ = false;
};

void
GpuBusyHold::release()
{
    if (keepAlive_) {
        keepAlive_->release();
        keepAlive_.reset();
    }
}

namespace {

    bool
    keepAliveWanted(const GpuDeviceInfo& info)
    {
        if (const char* env = std::getenv("NATRON_GPU_KEEPALIVE")) {
            if (std::strcmp(env, "0") == 0) {
                return false;
            }
            if (std::strcmp(env, "1") == 0) {
                return true;
            }
        }
        return !info.unifiedMemory;
    }

} // namespace

GpuStatus
GpuRuntime::create(const GpuRuntimeOptions& options, std::unique_ptr<GpuRuntime>& out)
{
    out.reset();
    std::unique_ptr<GpuRuntime> rt(new GpuRuntime);
    rt->options_ = options;
    GpuDeviceOptions& dev = rt->options_.device;
    if (dev.fault.kind == GpuFaultPlan::Kind::Off) {
        if (const char* env = std::getenv("NATRON_GPU_FAULT")) {
            GpuFaultPlan plan;
            if (!GpuFaultPlan::parse(env, plan)) {
                return { VK_ERROR_INITIALIZATION_FAILED, "NATRON_GPU_FAULT must be lost@N or oom@N with N >= 1" };
            }
            dev.fault = plan;
        }
    }
    GpuRuntime* self = rt.get();
    auto userOnLost = dev.onDeviceLost;
    dev.onDeviceLost = [self, userOnLost] {
        self->state_.store(GpuRuntimeState::Lost, std::memory_order_release);
        if (userOnLost) {
            userOnLost();
        }
    };
    if (GpuStatus s = rt->build(); !s) {
        return s;
    }
    out = std::move(rt);
    return {};
}

GpuRuntime::~GpuRuntime()
{
    stopKeepAlive();
    if (device_ && !isLost()) {
        device_->waitIdle();
    }
    teardown();
}

GpuStatus
GpuRuntime::build()
{
    static std::atomic<uint64_t> nextGeneration { 0 };
    generation_.store(++nextGeneration, std::memory_order_release);
    if (GpuStatus s = GpuDevice::create(options_.device, device_); !s) {
        return s;
    }
    if (GpuStatus s = GpuTransfer::create(*device_, options_.transfer, transfer_); !s) {
        teardown();
        return s;
    }
    registry_ = std::make_unique<GpuKernelRegistry>(*device_);
    for (const GpuKernelDesc& d : registered_) {
        registry_->registerKernel(d);
    }
    pool_ = std::make_unique<GpuBufferPool>(*device_, options_.pool);
    pipelines_ = std::make_unique<GpuPipelineCache>(*device_, options_.pipelines);
    textures_ = std::make_unique<GpuTextureCache>(*device_);
    preparer_ = std::make_unique<GpuImportPreparer>(*transfer_);
    if (keepAliveWanted(device_->info())) {
        keepAlive_ = std::make_shared<GpuKeepAlive>(*device_);
    }
    return {};
}

void
GpuRuntime::stopKeepAlive()
{
    if (keepAlive_) {
        keepAlive_->stop();
    }
}

void
GpuRuntime::teardown()
{
    // Stopped before anything else goes, as its buffers and submissions belong to the device.
    stopKeepAlive();
    keepAlive_.reset();
    preparer_.reset();
    if (device_ && device_->hasAbandonedWork()) {
        // The abandoned work may still use the pipelines, the pool's buffers, the command pools and the device, and
        // the buffers its callers destroy later still point at the device, so all of it is parked instead.
        VmaTotalStatistics stats {};
        vmaCalculateStatistics(device_->allocator(), &stats);
        leaked_ = stats.total.statistics.allocationCount;
        GpuDevice::parkForever(std::shared_ptr<GpuTextureCache>(std::move(textures_)));
        GpuDevice::parkForever(std::shared_ptr<GpuPipelineCache>(std::move(pipelines_)));
        GpuDevice::parkForever(std::shared_ptr<GpuBufferPool>(std::move(pool_)));
        GpuDevice::parkForever(std::shared_ptr<GpuKernelRegistry>(std::move(registry_)));
        GpuDevice::parkForever(std::shared_ptr<GpuTransfer>(std::move(transfer_)));
        GpuDevice::parkForever(std::shared_ptr<GpuDevice>(std::move(device_)));
        return;
    }
    textures_.reset();
    pipelines_.reset();
    pool_.reset();
    if (registry_) {
        registry_->invalidate();
    }
    registry_.reset();
    transfer_.reset();
    if (device_ && device_->allocator()) {
        VmaTotalStatistics stats {};
        vmaCalculateStatistics(device_->allocator(), &stats);
        leaked_ = stats.total.statistics.allocationCount;
    }
    device_.reset();
}

GpuStatus
GpuRuntime::lostStatus() const
{
    return { VK_ERROR_DEVICE_LOST, "GPU runtime is lost" };
}

GpuKernelId
GpuRuntime::registerKernel(const GpuKernelDesc& desc)
{
    std::lock_guard<std::mutex> lock(mutex_);
    registered_.push_back(desc);
    return registry_->registerKernel(desc);
}

GpuKernelId
GpuRuntime::registerRuntimeKernel(std::span<const uint32_t> spirv, std::string_view entry)
{
    std::lock_guard<std::mutex> lock(mutex_);
    RuntimeModule& m = runtimeModules_.emplace_back();
    m.spirv.assign(spirv.begin(), spirv.end());
    m.entry = entry;
    GpuKernelDesc desc;
    desc.spirv = m.spirv.data();
    desc.spirvWords = m.spirv.size();
    desc.entry = m.entry.c_str();
    desc.reflect = true;
    registered_.push_back(desc);
    return registry_->registerKernel(desc);
}

GpuStatus
GpuRuntime::note(const GpuStatus& status, uint64_t requestBytes)
{
    if (status.result == VK_ERROR_DEVICE_LOST) {
        state_.store(GpuRuntimeState::Lost, std::memory_order_release);
    } else if (status.result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
        noteOutOfMemory(requestBytes);
    }
    return status;
}

void
GpuRuntime::noteOutOfMemory(uint64_t requestBytes)
{
    std::lock_guard<std::mutex> lock(mutex_);
    GpuRuntimeState expected = GpuRuntimeState::Ready;
    state_.compare_exchange_strong(expected, GpuRuntimeState::Degraded, std::memory_order_acq_rel);
    if (isLost() || !pool_) {
        return;
    }
    const uint64_t current = pool_->budget();
    pool_->setUserBudget(current > requestBytes ? current - requestBytes : 1);
}

GpuStatus
GpuRuntime::acquire(VkDeviceSize size, GpuBufferHandle& out)
{
    if (isLost()) {
        return lostStatus();
    }
    if (GpuStatus injected = device_->countOperation(); !injected) {
        return note(injected, size);
    }
    GpuStatus status = pool_->acquire(size, out);
    // Regions running at once compete for the budget; lowering it for that would shrink it a little more with every
    // busy render.
    if (status.budgetContention) {
        return status;
    }
    return note(status, size);
}

GpuStatus
GpuRuntime::newRecordContext(std::unique_ptr<GpuRecordContext>& out)
{
    if (isLost()) {
        return lostStatus();
    }
    return note(GpuRecordContext::create(*registry_, out, pool_.get(), pipelines_.get(), textures_.get()));
}

GpuStatus
GpuRuntime::upload(HostRange src, GpuBuffer& dst, VkDeviceSize offset, TimelineValue& done,
                   const StreamOptions& options)
{
    if (isLost()) {
        return lostStatus();
    }
    return note(transfer_->upload(src, dst, offset, done, options), src.bytes);
}

GpuStatus
GpuRuntime::download(GpuBuffer& src, VkDeviceSize offset, HostRange dst, TimelineValue& done,
                     const StreamOptions& options)
{
    if (isLost()) {
        return lostStatus();
    }
    return note(transfer_->download(src, offset, dst, done, options), dst.bytes);
}

GpuStatus
GpuRuntime::wait(TimelineValue value)
{
    if (isLost()) {
        return lostStatus();
    }
    return note(transfer_->wait(value));
}

GpuStatus
GpuRuntime::waitIdle()
{
    if (isLost()) {
        return lostStatus();
    }
    return note(device_->waitIdle());
}

std::shared_ptr<GpuImportJob>
GpuRuntime::prepareImports(std::vector<HostRange> ranges)
{
    if (ranges.empty() || isLost() || !preparer_ || !transfer_->options().cacheHostImports
        || !transfer_->hostImportSupported()) {
        return nullptr;
    }
    std::shared_ptr<GpuImportJob> job = std::make_shared<GpuImportJob>();
    job->ranges_ = std::move(ranges);
    preparer_->push(job);
    return job;
}

GpuBusyHold
GpuRuntime::holdBusy(double limitMs)
{
    GpuBusyHold hold;
    if (keepAlive_ && !isLost() && (limitMs > 0.)) {
        keepAlive_->hold(limitMs);
        hold.keepAlive_ = keepAlive_;
    }
    return hold;
}

GpuStatus
GpuRuntime::recreate()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (device_ && device_->hasAbandonedWork()) {
        // Each recreation would park another device that may never finish its work.
        return { VK_ERROR_DEVICE_LOST, "GPU work did not finish in time; the device is not recreated" };
    }
    // Stopped first, so that it imports nothing after the imports are released.
    preparer_.reset();
    stopKeepAlive();
    if (device_ && !isLost()) {
        if (transfer_) {
            transfer_->releaseHostImports();
        }
        device_->waitIdle();
    } else if (device_) {
        device_->drainQueues();
    }
    teardown();
    // The fault already fired once; a fresh device would count from zero and fire it again.
    options_.device.fault = {};
    if (GpuStatus s = build(); !s) {
        state_.store(GpuRuntimeState::Lost, std::memory_order_release);
        return s;
    }
    state_.store(GpuRuntimeState::Ready, std::memory_order_release);
    return {};
}

} // namespace gpu
