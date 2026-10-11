#pragma once

#include "GpuBuffer.h"
#include "GpuKernelRegistry.h"

#include <array>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <span>
#include <type_traits>

namespace gpu {

// One command buffer on the compute queue, recorded and submitted by the thread that created it.
// Node code goes through this class and never sees a Vulkan handle.
//
// The command buffer comes from a pool owned by the registry and private to the creating
// thread, so a context must be used and destroyed on that thread.
class GpuRecordContext {
public:
    static GpuStatus create(GpuKernelRegistry& registry, std::unique_ptr<GpuRecordContext>& out);
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

    // Orders earlier dispatches' storage writes before later dispatches' storage reads and writes.
    void computeBarrier();

    // Ends the buffer, submits it and blocks until the queue finished it. The context cannot record afterwards.
    GpuStatus submitAndWait(uint64_t timeoutNs = 30'000'000'000ull);

private:
    GpuRecordContext(GpuKernelRegistry& registry)
        : registry_(registry)
    {
    }

    GpuKernelRegistry& registry_;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    bool open_ = false;
};

} // namespace gpu
