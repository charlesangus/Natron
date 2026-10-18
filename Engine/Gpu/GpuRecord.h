#pragma once

#include "GpuBuffer.h"
#include "GpuBufferPool.h"
#include "GpuKernelRegistry.h"
#include "GpuTransfer.h"

#include <array>
#include <cstddef>
#include <deque>
#include <initializer_list>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

namespace gpu {

// One command buffer on the compute queue, recorded and submitted by the thread that created it.
// Node code goes through this class and never sees a Vulkan handle.
//
// The command buffer comes from a pool owned by the registry and private to the creating
// thread, so a context must be used and destroyed on that thread.
class GpuRecordContext {
public:
    // Without a pool the context cannot hand out scratch buffers.
    static GpuStatus create(GpuKernelRegistry& registry, std::unique_ptr<GpuRecordContext>& out,
                            GpuBufferPool* pool = nullptr);
    ~GpuRecordContext();
    GpuRecordContext(const GpuRecordContext&) = delete;
    GpuRecordContext& operator=(const GpuRecordContext&) = delete;

    // `buffers` holds one entry per storage binding of the kernel; `push` is exactly its push-constant
    // block; `threads` is the invocation grid, rounded up to whole workgroups.
    GpuStatus dispatch(GpuKernelId kernel, std::span<const GpuBuffer* const> buffers,
                       std::span<const std::byte> push, std::array<uint32_t, 3> threads);

    GpuStatus dispatch(GpuKernelId kernel, std::initializer_list<const GpuBuffer*> buffers,
                       std::span<const std::byte> push, std::array<uint32_t, 3> threads)
    {
        return dispatch(kernel, std::span<const GpuBuffer* const>(buffers.begin(), buffers.size()), push, threads);
    }

    template <typename Params>
    GpuStatus dispatch(GpuKernelId kernel, std::initializer_list<const GpuBuffer*> buffers, const Params& params,
                       std::array<uint32_t, 3> threads)
    {
        static_assert(std::is_trivially_copyable_v<Params>);
        return dispatch(kernel, buffers, std::as_bytes(std::span(&params, 1)), threads);
    }

    // Dispatches a kernel with explicit bindings, as GpuKernel::recordBindings; the only way to
    // dispatch a reflected kernel.
    GpuStatus dispatchBindings(GpuKernelId kernel, std::span<const GpuBinding> bindings,
                               std::span<const std::byte> push, std::array<uint32_t, 3> threads);

    // A uniform buffer holding `data`, held until the context is destroyed.
    GpuStatus uniform(std::span<const std::byte> data, const GpuUniformBuffer*& out);

    // Orders earlier dispatches' storage writes before later dispatches' storage reads and writes.
    void computeBarrier();

    // A device buffer of at least `size` bytes from the pool, held until the context is destroyed so
    // it outlives the recorded work that uses it. Its contents are undefined.
    GpuStatus scratch(VkDeviceSize size, GpuBuffer*& out);

    // A scratch request was refused for lack of device memory, which node code reports only as a failure to record.
    bool ranOutOfMemory() const { return outOfMemory_; }

    // Records a write of `data` into dst at `offset`, visible to later dispatches. Both offset and size
    // must be multiples of 4, and the size at most 65536 bytes.
    GpuStatus updateBuffer(const GpuBuffer& dst, VkDeviceSize offset, std::span<const std::byte> data);

    // The compute-side halves of the ownership transfers of uploads and downloads made with
    // ComputeHandoff::Record; the range must match the transfer's exactly.
    void acquireFromTransfer(const GpuTransfer& transfer, const GpuBuffer& buffer, VkDeviceSize offset,
                             VkDeviceSize bytes);
    void releaseToTransfer(const GpuTransfer& transfer, const GpuBuffer& buffer, VkDeviceSize offset,
                           VkDeviceSize bytes);

    void resetQueries(VkQueryPool pool, uint32_t first, uint32_t count);
    // Writes the time at which every command recorded before it, in this buffer and before it on the queue, is done.
    void writeTimestamp(VkQueryPool pool, uint32_t query);

    // Ends the buffer, submits it and blocks until the queue finished it. The context cannot record afterwards.
    // Work still running after timeoutNs latches a device loss and fails with VK_ERROR_DEVICE_LOST, but the call
    // returns only once that work is done or the device really is lost, so that the buffers it uses outlive it.
    GpuStatus submitAndWait(uint64_t timeoutNs = 30'000'000'000ull);

    // Ends the buffer and submits it once every wait is reached, signalling `signal` when it is done, without
    // blocking. The context holds the command buffer and its scratch buffers, so it must not be destroyed before
    // `signal` is reached.
    GpuStatus submit(std::span<const TimelineValue> waits, TimelineValue signal);

private:
    GpuRecordContext(GpuKernelRegistry& registry, GpuBufferPool* pool)
        : registry_(registry)
        , bufferPool_(pool)
    {
    }

    GpuKernelRegistry& registry_;
    GpuBufferPool* bufferPool_;
    std::vector<GpuBufferHandle> held_;
    std::deque<GpuUniformBuffer> uniforms_;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    bool open_ = false;
    bool outOfMemory_ = false;
};

} // namespace gpu
