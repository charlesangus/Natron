#pragma once

#include "GpuKernel.h"
#include "GpuRaster.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace gpu {

// Ids of the kernels compiled into NatronGpu, in the order of Engine/Gpu/CMakeLists.txt.
// Descriptors added with GpuKernelRegistry::registerKernel continue after Count.
enum class GpuKernelId : uint32_t {
#define GPU_KERNELS_ENUM
#include "GpuKernels.inc"
#undef GPU_KERNELS_ENUM
    Count
};

// Ids of the rasters compiled into NatronGpu, from slang_add_raster in the same file order. Descriptors
// added with GpuKernelRegistry::registerRaster continue after Count.
enum class GpuRasterId : uint32_t {
#define GPU_RASTERS_ENUM
#include "GpuKernels.inc"
#undef GPU_RASTERS_ENUM
    Count
};

// Lazily-created compute pipelines for one device, and the raster descriptions GpuPipelineCache builds
// graphics pipelines from. Safe to call from any thread.
//
// Pointers returned by get() stay valid until invalidate() or destruction; both
// require that no recorded work still references them.
class GpuKernelRegistry {
public:
    explicit GpuKernelRegistry(GpuDevice& device);
    ~GpuKernelRegistry();
    GpuKernelRegistry(const GpuKernelRegistry&) = delete;
    GpuKernelRegistry& operator=(const GpuKernelRegistry&) = delete;

    GpuDevice& device() const { return device_; }

    // Unique among the registries of the process, never reused, unlike the registry's address.
    uint64_t serial() const { return serial_; }

    // Adds a kernel that is not part of the generated table, for example a test kernel.
    // The SPIR-V it points to must outlive the registry.
    GpuKernelId registerKernel(const GpuKernelDesc& desc);

    // Adds a kernel compiled at runtime, e.g. by compileGlsl, whose layout get() reflects from
    // the module. The registry keeps its own copy of the SPIR-V and the entry name. A module
    // that cannot be reflected is reported by get().
    GpuKernelId registerRuntimeKernel(std::span<const uint32_t> spirv, std::string_view entry);

    // Null for an id that is neither generated nor registered.
    const GpuKernelDesc* desc(GpuKernelId id) const;

    // Adds a raster that is not part of the generated table. The SPIR-V it points to must outlive the registry.
    GpuRasterId registerRaster(const GpuRasterDesc& desc);

    // Null for an id that is neither generated nor registered.
    const GpuRasterDesc* rasterDesc(GpuRasterId id) const;

    // Creates the pipeline on first use; concurrent callers share one creation.
    // A failed creation is not cached, so the next call retries.
    GpuStatus get(GpuKernelId id, GpuKernel*& out);

    // Drops every pipeline and command pool, for device loss or re-creation. Kernels
    // are recreated on the next get().
    void invalidate();

    // How many times the pipeline for `id` was built since construction.
    uint32_t creationCount(GpuKernelId id) const;

private:
    friend class GpuRecordContext;

    // Pools are externally synchronized, so each thread records into its own, one per queue family.
    GpuStatus commandPoolForThisThread(VkCommandPool& out, QueueKind queue = QueueKind::Compute);
    // Dispatches made through the descriptor-pool fallback draw from a pool shared by every
    // thread, so it is only reset once no recording context is alive.
    void recordingBegan();
    void recordingEnded();
    // Command buffers of `pool` whose work was left retiring on the device: freed by the thread that owns the pool,
    // the next time it asks for it, once `done` is set.
    void retireCommandBuffers(VkCommandPool pool, std::vector<VkCommandBuffer> buffers, std::shared_ptr<std::atomic<bool>> done);
    // A kernel the registry does not own, dispatched by a context; its descriptor-pool fallback is
    // recycled with the registry's own once no context is alive.
    void noteExternalKernel(const std::shared_ptr<GpuKernel>& kernel);

    struct RuntimeModule {
        std::vector<uint32_t> spirv;
        std::string entry;
    };

    struct Slot {
        std::unique_ptr<GpuKernel> kernel;
        uint32_t creations = 0;
    };

    Slot* slotLocked(GpuKernelId id);
    const GpuKernelDesc* descLocked(GpuKernelId id) const;
    void destroyPoolsLocked();

    GpuDevice& device_;
    const uint64_t serial_;
    mutable std::mutex mutex_;
    std::vector<Slot> slots_;
    std::deque<GpuKernelDesc> registered_;
    std::deque<GpuRasterDesc> registeredRasters_;
    std::deque<RuntimeModule> runtimeModules_;
    std::vector<std::weak_ptr<GpuKernel>> external_;
    std::map<std::pair<std::thread::id, uint32_t>, VkCommandPool> pools_;
    struct RetiredCommandBuffers {
        VkCommandPool pool = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> buffers;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::vector<RetiredCommandBuffers> retiredCommandBuffers_;
    uint32_t activeContexts_ = 0;
    // What GpuRecordContext learned of this device's speed; 0 until it finished a split context's first batch.
    std::atomic<uint64_t> learnedBatchWork_ { 0 };
};

} // namespace gpu
