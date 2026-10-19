#pragma once

#include "GpuKernel.h"

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
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

// Lazily-created compute pipelines for one device. Safe to call from any thread.
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

    // Adds a kernel that is not part of the generated table, for example a test kernel.
    // The SPIR-V it points to must outlive the registry.
    GpuKernelId registerKernel(const GpuKernelDesc& desc);

    // Adds a kernel compiled at runtime, e.g. by compileGlsl, whose layout get() reflects from
    // the module. The registry keeps its own copy of the SPIR-V and the entry name. A module
    // that cannot be reflected is reported by get().
    GpuKernelId registerRuntimeKernel(std::span<const uint32_t> spirv, std::string_view entry);

    // Null for an id that is neither generated nor registered.
    const GpuKernelDesc* desc(GpuKernelId id) const;

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

    // Pools are externally synchronized, so each thread records into its own.
    GpuStatus commandPoolForThisThread(VkCommandPool& out);
    // Dispatches made through the descriptor-pool fallback draw from a pool shared by every
    // thread, so it is only reset once no recording context is alive.
    void recordingBegan();
    void recordingEnded();
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
    mutable std::mutex mutex_;
    std::vector<Slot> slots_;
    std::deque<GpuKernelDesc> registered_;
    std::deque<RuntimeModule> runtimeModules_;
    std::vector<std::weak_ptr<GpuKernel>> external_;
    std::map<std::thread::id, VkCommandPool> pools_;
    uint32_t activeContexts_ = 0;
};

} // namespace gpu
