#include "GpuRuntime.h"

#include <algorithm>
#include <cstdlib>
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
    preparer_ = std::make_unique<GpuImportPreparer>(*transfer_);
    return {};
}

void
GpuRuntime::teardown()
{
    preparer_.reset();
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
    return note(GpuRecordContext::create(*registry_, out, pool_.get(), pipelines_.get()));
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

GpuStatus
GpuRuntime::recreate()
{
    std::lock_guard<std::mutex> lock(mutex_);
    // Stopped first, so that it imports nothing after the imports are released.
    preparer_.reset();
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
