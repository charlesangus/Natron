#pragma once

#include "GpuBuffer.h"
#include "GpuBufferPool.h"
#include "GpuDevice.h"
#include "GpuKernelRegistry.h"
#include "GpuRecord.h"
#include "GpuTransfer.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace gpu {

enum class GpuRuntimeState {
    Ready,
    // An allocation ran out of device memory; the pool budget was reduced and work continues.
    Degraded,
    // VK_ERROR_DEVICE_LOST was seen. Sticky until recreate().
    Lost,
};

struct GpuRuntimeOptions {
    GpuDeviceOptions device;
    GpuTransferOptions transfer;
    GpuBufferPoolOptions pool;
};

// Owns the device, the transfer streams, the kernel registry and the buffer pool, built in that order and
// destroyed in reverse.
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

    GpuDevice& device() const { return *device_; }
    GpuTransfer& transfer() const { return *transfer_; }
    GpuKernelRegistry& registry() const { return *registry_; }
    GpuBufferPool& pool() const { return *pool_; }

    // Like GpuKernelRegistry::registerKernel, but the descriptor is registered again after recreate(),
    // under the same id. The SPIR-V it points to must outlive the runtime.
    GpuKernelId registerKernel(const GpuKernelDesc& desc);

    // Entry points that fail fast with VK_ERROR_DEVICE_LOST while Lost and otherwise report their
    // outcome to the state machine.
    GpuStatus acquire(VkDeviceSize size, GpuBufferHandle& out);
    GpuStatus newRecordContext(std::unique_ptr<GpuRecordContext>& out);
    GpuStatus upload(HostRange src, GpuBuffer& dst, VkDeviceSize offset, TimelineValue& done,
                     const StreamOptions& options = {});
    GpuStatus download(GpuBuffer& src, VkDeviceSize offset, HostRange dst, TimelineValue& done,
                       const StreamOptions& options = {});
    GpuStatus wait(TimelineValue value);
    GpuStatus waitIdle();

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
    GpuRuntimeOptions options_;
    std::vector<GpuKernelDesc> registered_;
    std::mutex mutex_;
    std::atomic<GpuRuntimeState> state_ { GpuRuntimeState::Ready };

    std::unique_ptr<GpuDevice> device_;
    std::unique_ptr<GpuTransfer> transfer_;
    std::unique_ptr<GpuKernelRegistry> registry_;
    std::unique_ptr<GpuBufferPool> pool_;
};

} // namespace gpu
