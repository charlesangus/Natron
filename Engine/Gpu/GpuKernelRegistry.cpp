#include "GpuKernelRegistry.h"

namespace gpu {

#define GPU_KERNELS_TABLE
#include "GpuKernels.inc"
#undef GPU_KERNELS_TABLE

namespace {

    constexpr size_t kGeneratedCount = sizeof(kGpuKernelTable) / sizeof(kGpuKernelTable[0]);
    static_assert(kGeneratedCount == static_cast<size_t>(GpuKernelId::Count),
                  "kernel table and id enum come from the same list");

} // namespace

GpuKernelRegistry::GpuKernelRegistry(GpuDevice& device)
    : device_(device)
    , slots_(kGeneratedCount)
{
}

GpuKernelRegistry::~GpuKernelRegistry()
{
    std::lock_guard<std::mutex> lock(mutex_);
    slots_.clear();
    destroyPoolsLocked();
}

GpuKernelId
GpuKernelRegistry::registerKernel(const GpuKernelDesc& desc)
{
    std::lock_guard<std::mutex> lock(mutex_);
    registered_.push_back(desc);
    slots_.emplace_back();
    return static_cast<GpuKernelId>(kGeneratedCount + registered_.size() - 1);
}

GpuKernelId
GpuKernelRegistry::registerRuntimeKernel(std::span<const uint32_t> spirv, std::string_view entry)
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
    slots_.emplace_back();
    return static_cast<GpuKernelId>(kGeneratedCount + registered_.size() - 1);
}

const GpuKernelDesc*
GpuKernelRegistry::descLocked(GpuKernelId id) const
{
    const size_t i = static_cast<size_t>(id);
    if (i < kGeneratedCount) {
        return &kGpuKernelTable[i];
    }
    if (i - kGeneratedCount < registered_.size()) {
        return &registered_[i - kGeneratedCount];
    }
    return nullptr;
}

const GpuKernelDesc*
GpuKernelRegistry::desc(GpuKernelId id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return descLocked(id);
}

GpuStatus
GpuKernelRegistry::get(GpuKernelId id, GpuKernel*& out)
{
    out = nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    const GpuKernelDesc* d = descLocked(id);
    if (!d) {
        return { VK_ERROR_INITIALIZATION_FAILED, "GpuKernelRegistry::get: unknown kernel id" };
    }
    Slot& slot = slots_[static_cast<size_t>(id)];
    if (!slot.kernel) {
        if (GpuStatus s = GpuKernel::create(device_, *d, slot.kernel); !s) {
            slot.kernel.reset();
            return s;
        }
        ++slot.creations;
    }
    out = slot.kernel.get();
    return {};
}

uint32_t
GpuKernelRegistry::creationCount(GpuKernelId id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t i = static_cast<size_t>(id);
    return i < slots_.size() ? slots_[i].creations : 0;
}

void
GpuKernelRegistry::destroyPoolsLocked()
{
    // A command buffer of work that never finished may still be executing out of one of these pools.
    if (device_.hasAbandonedWork()) {
        pools_.clear();
        return;
    }
    for (auto& [thread, pool] : pools_) {
        vkDestroyCommandPool(device_.device(), pool, nullptr);
    }
    pools_.clear();
}

void
GpuKernelRegistry::invalidate()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (Slot& slot : slots_) {
        slot.kernel.reset();
    }
    external_.clear();
    destroyPoolsLocked();
}

GpuStatus
GpuKernelRegistry::commandPoolForThisThread(VkCommandPool& out)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pools_.find(std::this_thread::get_id());
    if (it == pools_.end()) {
        VkCommandPoolCreateInfo ci { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        ci.queueFamilyIndex = device_.queueFamily(QueueKind::Compute);
        VkCommandPool pool = VK_NULL_HANDLE;
        if (GpuStatus s = device_.check(vkCreateCommandPool(device_.device(), &ci, nullptr, &pool),
                                        "vkCreateCommandPool");
            !s) {
            return s;
        }
        it = pools_.emplace(std::this_thread::get_id(), pool).first;
    }
    out = it->second;
    return {};
}

void
GpuKernelRegistry::recordingBegan()
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++activeContexts_;
}

void
GpuKernelRegistry::recordingEnded()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (--activeContexts_ == 0 && !device_.info().pushDescriptor) {
        for (Slot& slot : slots_) {
            if (slot.kernel) {
                slot.kernel->releaseDescriptors();
            }
        }
        for (const std::weak_ptr<GpuKernel>& weak : external_) {
            if (std::shared_ptr<GpuKernel> kernel = weak.lock()) {
                kernel->releaseDescriptors();
            }
        }
        external_.clear();
    }
}

void
GpuKernelRegistry::noteExternalKernel(const std::shared_ptr<GpuKernel>& kernel)
{
    if (!kernel || device_.info().pushDescriptor) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (const std::weak_ptr<GpuKernel>& weak : external_) {
        if (!weak.owner_before(kernel) && !kernel.owner_before(weak)) {
            return;
        }
    }
    external_.push_back(kernel);
}

} // namespace gpu
