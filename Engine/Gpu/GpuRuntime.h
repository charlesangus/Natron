#pragma once

#include "GpuBuffer.h"
#include "GpuBufferPool.h"
#include "GpuDevice.h"
#include "GpuKernelRegistry.h"
#include "GpuPipelineCache.h"
#include "GpuRecord.h"
#include "GpuTransfer.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gpu {

enum class GpuRuntimeState {
    Ready,
    // An allocation ran out of device memory; the pool budget was reduced and work continues.
    Degraded,
    // VK_ERROR_DEVICE_LOST was seen. Sticky until recreate().
    Lost,
};

struct GpuImportPreparer;

// Host ranges the runtime's helper thread faults in and imports, in order, ahead of the transfers that will use them.
// The ranges must stay allocated until wait() or cancel() returns, and each must stay untouched by the device until a
// waitFor() past it, wait() or cancel() returns: the helper thread writes the pages it faults in.
class GpuImportJob {
public:
    // Blocks until every range was prepared or skipped.
    void wait();

    // Blocks until the first `count` ranges were prepared or skipped, while the helper thread may still be preparing
    // the ones after them.
    void waitFor(std::size_t count);

    // Skips the ranges not started yet, then waits for the one in progress.
    void cancel();

    bool finished() const;

private:
    friend class GpuRuntime;
    friend struct GpuImportPreparer;

    void finish();
    void advance();

    std::vector<HostRange> ranges_;
    std::atomic<bool> cancelled_ { false };
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t prepared_ = 0;
    bool finished_ = false;
};

struct GpuRuntimeOptions {
    GpuDeviceOptions device;
    GpuTransferOptions transfer;
    GpuBufferPoolOptions pool;
    GpuPipelineCacheOptions pipelines;
};

// Owns the device, the transfer streams, the kernel registry, the buffer pool and the pipeline cache of runtime
// shaders, built in that order and destroyed in reverse.
//
// Fault injection: GpuDeviceOptions::fault, or when that is unset the environment variable
// NATRON_GPU_FAULT=lost@N or oom@N read by create(). N counts runtime operations from 1: every queue
// submission (GpuDevice::submit, which covers transfers and record contexts) and every GpuRuntime::acquire.
// The fault fires once, on operation N, and is not re-armed by recreate().
//
// Thread safety: state() is lock-free and the transitions are serialized. recreate() invalidates every
// pointer, reference and handle obtained from the runtime, so the caller must have no other thread using it
// and no live pool handle or record context.
class GpuRuntime {
public:
    static GpuStatus create(const GpuRuntimeOptions& options, std::unique_ptr<GpuRuntime>& out);
    ~GpuRuntime();
    GpuRuntime(const GpuRuntime&) = delete;
    GpuRuntime& operator=(const GpuRuntime&) = delete;

    GpuRuntimeState state() const { return state_.load(std::memory_order_acquire); }
    bool isLost() const { return state() == GpuRuntimeState::Lost; }

    // Differs for every device built by any runtime of the process, so that what was learned about one device is
    // never applied to another.
    uint64_t generation() const { return generation_.load(std::memory_order_acquire); }

    GpuDevice& device() const { return *device_; }
    GpuTransfer& transfer() const { return *transfer_; }
    GpuKernelRegistry& registry() const { return *registry_; }
    GpuBufferPool& pool() const { return *pool_; }
    // Rebuilt by recreate(), so a kernel taken from it must be dropped first, like any other handle.
    GpuPipelineCache& pipelines() const { return *pipelines_; }

    // Like GpuKernelRegistry::registerKernel, but the descriptor is registered again after recreate(),
    // under the same id. The SPIR-V it points to must outlive the runtime.
    GpuKernelId registerKernel(const GpuKernelDesc& desc);

    // Like GpuKernelRegistry::registerRuntimeKernel, and registered again after recreate() under the same id.
    // The runtime keeps its own copy of the SPIR-V and the entry name.
    GpuKernelId registerRuntimeKernel(std::span<const uint32_t> spirv, std::string_view entry);

    // Entry points that fail fast with VK_ERROR_DEVICE_LOST while Lost and otherwise report their
    // outcome to the state machine, except for an acquire() refused over budget contention, which
    // leaves the state and the budget alone.
    GpuStatus acquire(VkDeviceSize size, GpuBufferHandle& out);
    GpuStatus newRecordContext(std::unique_ptr<GpuRecordContext>& out);
    GpuStatus upload(HostRange src, GpuBuffer& dst, VkDeviceSize offset, TimelineValue& done,
                     const StreamOptions& options = {});
    GpuStatus download(GpuBuffer& src, VkDeviceSize offset, HostRange dst, TimelineValue& done,
                       const StreamOptions& options = {});
    GpuStatus wait(TimelineValue value);
    GpuStatus waitIdle();

    // Faults in `ranges` and imports them into the transfer's cache on a helper thread, so that the transfers
    // into them find their imports made. Null when there is nothing to do: the transfer caches no imports, the
    // device cannot import, the runtime is lost or `ranges` is empty. Jobs run one after another; teardown and
    // recreate() skip the ranges of those not finished.
    std::shared_ptr<GpuImportJob> prepareImports(std::vector<HostRange> ranges);

    // Feeds the outcome of a call made directly on a component into the state machine: a lost result latches
    // Lost, an out-of-memory result calls noteOutOfMemory(requestBytes). Returns `status` unchanged.
    GpuStatus note(const GpuStatus& status, uint64_t requestBytes = 0);

    // Moves Ready to Degraded and lowers the pool budget by `requestBytes`, never below one byte. Does
    // nothing while Lost.
    void noteOutOfMemory(uint64_t requestBytes);

    // Waits for the device where possible, destroys everything, creates the device again with the
    // original options, invalidates the registry and returns to Ready. Fails, staying Lost, when the
    // device cannot be created.
    GpuStatus recreate();

    // VMA allocations still alive when the previous device was torn down, after every component that owns
    // memory was destroyed. Anything but zero is a leak.
    uint64_t leakedAllocationsAtLastTeardown() const { return leaked_; }

private:
    GpuRuntime() = default;

    GpuStatus build();
    void teardown();
    GpuStatus lostStatus() const;

    uint64_t leaked_ = 0;
    std::atomic<uint64_t> generation_ { 0 };
    GpuRuntimeOptions options_;
    std::vector<GpuKernelDesc> registered_;
    struct RuntimeModule {
        std::vector<uint32_t> spirv;
        std::string entry;
    };
    std::deque<RuntimeModule> runtimeModules_;
    std::mutex mutex_;
    std::atomic<GpuRuntimeState> state_ { GpuRuntimeState::Ready };

    std::unique_ptr<GpuDevice> device_;
    std::unique_ptr<GpuTransfer> transfer_;
    std::unique_ptr<GpuKernelRegistry> registry_;
    std::unique_ptr<GpuBufferPool> pool_;
    std::unique_ptr<GpuPipelineCache> pipelines_;
    std::unique_ptr<GpuImportPreparer> preparer_;
};

} // namespace gpu
