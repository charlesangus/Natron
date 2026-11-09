#pragma once

#include "GpuBuffer.h"
#include "GpuBufferPool.h"
#include "GpuKernelRegistry.h"
#include "GpuShaderCompiler.h"
#include "GpuTexture.h"
#include "GpuTransfer.h"

#include <array>
#include <cstddef>
#include <deque>
#include <functional>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace gpu {

class GpuPipelineCache;

// One piece of work on the compute queue, recorded and submitted by the thread that created it, in one
// command buffer unless chargeWork() split it. Node code goes through this class and never sees a Vulkan handle.
//
// The command buffers come from a pool owned by the registry and private to the creating
// thread, so a context must be used and destroyed on that thread.
class GpuRecordContext {
public:
    // Without a pool the context cannot hand out scratch buffers; without a pipeline cache it cannot
    // hand out runtime shaders; without a texture cache cachedTexture() uploads its own copy.
    static GpuStatus create(GpuKernelRegistry& registry, std::unique_ptr<GpuRecordContext>& out,
                            GpuBufferPool* pool = nullptr, GpuPipelineCache* pipelines = nullptr,
                            GpuTextureCache* textures = nullptr);
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

    // Dispatches a kernel with explicit bindings, as GpuKernel::recordBindings; the way to dispatch
    // a reflected kernel.
    GpuStatus dispatchBindings(GpuKernelId kernel, std::span<const GpuBinding> bindings,
                               std::span<const std::byte> push, std::array<uint32_t, 3> threads);

    // The kernel of a runtime shader from the pipeline cache, as GpuPipelineCache::get.
    GpuStatus runtimeKernel(std::string_view source, ShaderStage stage, std::string_view entry,
                            std::shared_ptr<GpuKernel>& out);

    // As dispatchBindings() for a kernel the registry does not own, such as a pipeline cache's. The
    // context holds the kernel until it is destroyed.
    GpuStatus dispatchBindings(const std::shared_ptr<GpuKernel>& kernel, std::span<const GpuBinding> bindings,
                               std::span<const std::byte> push, std::array<uint32_t, 3> threads);

    // A uniform buffer holding `data`, held until the context is destroyed.
    GpuStatus uniform(std::span<const std::byte> data, const GpuUniformBuffer*& out);

    // A sampled texture of `texels`, as GpuTexture::create, held until the context is destroyed. The upload
    // completes before the call returns.
    GpuStatus texture(const GpuTextureDesc& desc, std::span<const float> texels, const GpuTexture*& out);

    // The texture the runtime's texture cache keeps under `key`, as GpuTextureCache::acquire, held until
    // the context is destroyed. Without a texture cache it is a texture() of its own.
    GpuStatus cachedTexture(const std::string& key, const GpuTextureDesc& desc, std::span<const float> texels,
                            const GpuTexture*& out);

    // Orders earlier dispatches' storage writes before later dispatches' storage reads and writes.
    void computeBarrier();

    // A device buffer of at least `size` bytes from the pool, held until the context is destroyed so
    // it outlives the recorded work that uses it. Its contents are undefined.
    GpuStatus scratch(VkDeviceSize size, GpuBuffer*& out);

    // A scratch request was refused for lack of device memory, which node code reports only as a failure to record.
    bool ranOutOfMemory() const { return outOfMemory_; }

    // The largest buffer a dispatch binds: the device's maxStorageBufferRange, or the cap of
    // setMaxStorageBufferBytesForTests() when smaller. A dispatch binding a larger buffer fails.
    uint64_t maxStorageBufferBytes() const;

    // The largest scratch() request whose buffer, as the pool rounds and reuses it, a dispatch still binds; 0 when
    // none is.
    uint64_t maxScratchBytes() const;

    // maxScratchBytes() of a context on a device whose maxStorageBufferRange is deviceRange.
    static uint64_t maxScratchBytesFor(uint64_t deviceRange);

    // Caps maxStorageBufferBytes() in every context, so that a test sees what a device with that limit does; 0
    // removes the cap.
    static void setMaxStorageBufferBytesForTests(uint64_t bytes);

    // Records a write of `data` into dst at `offset`, visible to later dispatches. Both offset and size
    // must be multiples of 4, and the size at most 65536 bytes.
    GpuStatus updateBuffer(const GpuBuffer& dst, VkDeviceSize offset, std::span<const std::byte> data);

    // The compute-side halves of the ownership transfers of uploads and downloads made with
    // ComputeHandoff::Record; the range must match the transfer's exactly.
    void acquireFromTransfer(const GpuTransfer& transfer, const GpuBuffer& buffer, VkDeviceSize offset,
                             VkDeviceSize bytes);
    void releaseToTransfer(const GpuTransfer& transfer, const GpuBuffer& buffer, VkDeviceSize offset,
                           VkDeviceSize bytes);

    // The device the context records for.
    GpuDevice& device() const;

    // Makes submit() and submitAndWait() also wait for `semaphore`, a binary semaphore another API signals,
    // before any command of the context runs. The caller keeps it alive, through hold(), until the context is
    // destroyed.
    void waitExternalSemaphore(VkSemaphore semaphore);

    // Keeps `object` alive until the context is destroyed, or for good when the device is lost with the
    // context's work in flight.
    void hold(std::shared_ptr<void> object);

    // Records copies of `regions` from `src`, a GpuBuffer::createExportable() buffer that another API wrote and
    // owns, into `dst`, visible to later dispatches: src's first srcBytes are acquired from the external queue
    // family first and released back to it after, so the other API may write them again once the context's
    // work is done.
    GpuStatus copyFromExternal(const GpuBuffer& src, VkDeviceSize srcBytes, const GpuBuffer& dst,
                               std::span<const VkBufferCopy> regions);

    void resetQueries(VkQueryPool pool, uint32_t first, uint32_t count);
    // Writes the time at which every command recorded before it, in this buffer and before it on the queue, is done.
    void writeTimestamp(VkQueryPool pool, uint32_t query);

    // Charges `work` to the open command buffer and, once it holds batchWork(), ends it and opens another, so a
    // long multi-pass node runs as bounded submits that a caller's stop can fall between. Call it only where every
    // later command may run in a later submit, which is anywhere after computeBarrier(): the barrier's scope
    // spans command buffers in submission order. One unit of work is about one sampled value.
    GpuStatus chargeWork(uint64_t work);

    // The work a command buffer holds before chargeWork() ends it: what the device was last seen to finish in about
    // a tenth of a second, or setBatchWorkForTests().
    uint64_t batchWork() const;

    // The command buffers the context's work is split into so far.
    size_t batchCount() const { return finished_.size() + 1; }

    // Overrides batchWork() in every context: 1 ends a buffer at every charge, UINT64_MAX never does; 0 removes the
    // override.
    static void setBatchWorkForTests(uint64_t work);

    // The most work any command buffer chargeWork() ended held, over every context, since setBatchWorkForTests() was
    // last called; only buffers ended under an override count.
    static uint64_t largestBatchWorkForTests();

    // Calls `hook` on the submitting thread after each command buffer but the last has finished, before `stop` is
    // asked; an empty hook removes it.
    static void setBatchFinishedHookForTests(const std::function<void()>& hook);

    // Ends the buffer, submits it and blocks until the queue finished it. The context cannot record afterwards.
    // Work still running after timeoutNs fails at once with VK_ERROR_DEVICE_LOST, through
    // GpuDevice::abandonInFlightWork(): what the context holds is parked, and the buffers the caller bound are not
    // freed when it destroys them, so the work never writes released memory. A split context applies timeoutNs to
    // each command buffer; when `stop` returns true between them, it fails with VK_INCOMPLETE and none of its work
    // is left on the device.
    GpuStatus submitAndWait(uint64_t timeoutNs = 30'000'000'000ull, const std::function<bool()>& stop = {});

    // Ends the buffer and submits it once every wait is reached, signalling `signal` when it is done. The context
    // holds the command buffer and its scratch buffers, so it must not be destroyed before `signal` is reached.
    // Only a split context blocks: until all its command buffers but the last have finished, one at a time. When
    // `stop` returns true between them it fails with VK_INCOMPLETE, `signal` is never signalled, and none of its work
    // is left on the device.
    GpuStatus submit(std::span<const TimelineValue> waits, TimelineValue signal, const std::function<bool()>& stop = {});

private:
    struct FinishedBatch {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        uint64_t work = 0;
    };

    GpuStatus beginCommandBuffer(VkCommandBuffer& out);
    GpuStatus submitFenced(VkCommandBuffer cmd, std::span<const VkSemaphoreSubmitInfo> waits, uint64_t timeoutNs);
    GpuStatus waitBeforeBatches(std::span<const TimelineValue> waits, uint64_t timeoutNs, const std::function<bool()>& stop);
    GpuStatus runBatchesBeforeLast(std::span<const VkSemaphoreSubmitInfo> waits, uint64_t timeoutNs, const std::function<bool()>& stop);
    void abandonWork(const char* what);
    void parkInFlightWork();
    std::vector<VkSemaphoreSubmitInfo> externalWaitInfos() const;
    GpuStatus checkBindable(const char* where, uint32_t binding, VkDeviceSize bytes) const;

    GpuRecordContext(GpuKernelRegistry& registry, GpuBufferPool* pool, GpuPipelineCache* pipelines,
                     GpuTextureCache* textures)
        : registry_(registry)
        , bufferPool_(pool)
        , pipelines_(pipelines)
        , textureCache_(textures)
    {
    }

    GpuKernelRegistry& registry_;
    GpuBufferPool* bufferPool_;
    GpuPipelineCache* pipelines_;
    GpuTextureCache* textureCache_;
    std::vector<GpuBufferHandle> held_;
    std::deque<GpuUniformBuffer> uniforms_;
    std::vector<std::unique_ptr<GpuTexture>> textures_;
    std::vector<std::shared_ptr<const GpuTexture>> cachedTextures_;
    std::vector<std::shared_ptr<GpuKernel>> kernels_;
    std::vector<std::shared_ptr<void>> heldObjects_;
    std::vector<VkSemaphore> externalWaits_;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    std::vector<FinishedBatch> finished_;
    uint64_t work_ = 0;
    bool open_ = false;
    bool outOfMemory_ = false;
};

} // namespace gpu
