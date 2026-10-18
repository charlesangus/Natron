#include "GpuRuntime.h"

#include <algorithm>
#include <cstdlib>

namespace gpu {

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
    return {};
}

void
GpuRuntime::teardown()
{
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

GpuStatus
GpuRuntime::recreate()
{
    std::lock_guard<std::mutex> lock(mutex_);
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
