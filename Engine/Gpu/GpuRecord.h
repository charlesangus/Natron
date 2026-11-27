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
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

namespace gpu {

class GpuPipelineCache;
class GpuAccelRecorder;

// One piece of work on the compute queue, or on the graphics queue for a raster context, recorded and submitted
// by the thread that created it, in one command buffer unless chargeWork() split it. Node code goes through this
// class and never sees a Vulkan handle.
//
// The command buffers come from a pool owned by the registry and private to the creating
// thread, so a context must be used and destroyed on that thread.
//
// Buffers are exclusive to one queue family. A buffer that crosses between a compute context and a raster
// context in another family is released in the context that last used it with releaseToOtherQueue() and
// acquired in the next with acquireFromOtherQueue(), the second submitted to wait for the first.
class GpuRecordContext {
public:
    // Without a pool the context cannot hand out scratch buffers; without a pipeline cache it cannot
    // hand out runtime shaders; without a texture cache cachedTexture() uploads its own copy.
    static GpuStatus create(GpuKernelRegistry& registry, std::unique_ptr<GpuRecordContext>& out,
                            GpuBufferPool* pool = nullptr, GpuPipelineCache* pipelines = nullptr,
                            GpuTextureCache* textures = nullptr);

    // As create(), for a context on the graphics queue that also records draws; its dispatches and copies
    // work as a compute context's. When the graphics family is not the compute family, textures stay with the
    // compute family where they are uploaded: such a context refuses texture(), cachedTexture() and texture
    // bindings, as it refuses acquireFromTransfer(). Fails with VK_ERROR_FEATURE_NOT_PRESENT when the device
    // lacks GpuDeviceInfo::rasterFloatBlend. Without a pipeline cache the context builds its own raster pipelines.
    static GpuStatus createRaster(GpuKernelRegistry& registry, std::unique_ptr<GpuRecordContext>& out,
                                  GpuBufferPool* pool = nullptr, GpuPipelineCache* pipelines = nullptr,
                                  GpuTextureCache* textures = nullptr);

    bool isRaster() const { return queue_ == QueueKind::Graphics; }

    // Makes every later dispatch and draw, and every chunk of an indirect draw, first call `aborted`, and fail
    // with GpuStatus::aborted set once it returns true; from then on the context records nothing, and submit()
    // and submitAndWait() run none of its commands and fail with GpuStatus::aborted set. Empty removes the check.
    void setAbortCheck(std::function<bool()> aborted);

    // Starts rendering into `attachment`, a GpuTexture::createAttachment() texture that outlives the
    // context's work, cleared to `clear` (one value per channel, the first for R32_SFLOAT). Storage and
    // transfer writes recorded before it are visible to the draws. Until endRaster() the context records only
    // draws. Raster contexts only.
    GpuStatus beginRaster(const GpuTexture& attachment, const std::array<float, 4>& clear);

    // Draws vertexCount vertices of instanceCount instances of `raster` with `blend`. `buffers` holds one
    // entry per storage binding of the raster and `push` is exactly its push-constant block.
    GpuStatus draw(GpuRasterId raster, GpuRasterBlend blend, std::span<const GpuBuffer* const> buffers,
                   std::span<const std::byte> push, uint32_t vertexCount, uint32_t instanceCount = 1);

    GpuStatus draw(GpuRasterId raster, GpuRasterBlend blend, std::initializer_list<const GpuBuffer*> buffers,
                   std::span<const std::byte> push, uint32_t vertexCount, uint32_t instanceCount = 1)
    {
        return draw(raster, blend, std::span<const GpuBuffer* const>(buffers.begin(), buffers.size()), push,
                    vertexCount, instanceCount);
    }

    // Draws the drawCount VkDrawIndexedIndirectCommand of `indirectBuffer`, tightly packed from its start, with
    // the 32-bit indices of `indexBuffer`. The commands may come from a dispatch recorded before beginRaster().
    // indirectBuffer needs VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT and indexBuffer VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    // drawCount commands must fit in indirectBuffer, which is checked. Nothing bounds what the commands read:
    // robust buffer access is not enabled, so the caller keeps every command's index range inside indexBuffer
    // and every index inside the vertex data the raster pulls. Recorded as one indirect draw per
    // maxDrawIndirectCount() commands.
    GpuStatus drawIndexedIndirect(GpuRasterId raster, GpuRasterBlend blend, std::span<const GpuBuffer* const> buffers,
                                  std::span<const std::byte> push, const GpuBuffer& indexBuffer,
                                  const GpuBuffer& indirectBuffer, uint32_t drawCount);

    GpuStatus drawIndexedIndirect(GpuRasterId raster, GpuRasterBlend blend,
                                  std::initializer_list<const GpuBuffer*> buffers, std::span<const std::byte> push,
                                  const GpuBuffer& indexBuffer, const GpuBuffer& indirectBuffer, uint32_t drawCount)
    {
        return drawIndexedIndirect(raster, blend, std::span<const GpuBuffer* const>(buffers.begin(), buffers.size()),
                                   push, indexBuffer, indirectBuffer, drawCount);
    }

    // Ends rendering. Storage writes of the draws are visible to later dispatches and copies.
    GpuStatus endRaster();

    // Copies the attachment of the last endRaster() into the start of `dst`, texels tightly packed as
    // GpuTexture::packedBytes() counts them, visible to later dispatches, draws and copies and to the host
    // once the work is done.
    GpuStatus copyToImage(const GpuBuffer& dst);

    // The halves of a queue-family ownership transfer of a buffer range between a compute context and a raster
    // context: the context that used the range last releases it, and the one that uses it next, submitted to
    // wait for the first, acquires it with the same range. Nothing is recorded when both queues are in one family.
    GpuStatus releaseToOtherQueue(const GpuBuffer& buffer, VkDeviceSize offset, VkDeviceSize bytes);
    GpuStatus acquireFromOtherQueue(const GpuBuffer& buffer, VkDeviceSize offset, VkDeviceSize bytes);
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
    // the context is destroyed. Without a texture cache it is a texture() of its own. Both are refused by a
    // raster context outside the compute family.
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

    // The most commands one indirect draw records: the device's maxDrawIndirectCount, 1 without
    // multiDrawIndirect, or the cap of setMaxDrawIndirectCountForTests() when smaller.
    uint32_t maxDrawIndirectCount() const;

    // Caps maxDrawIndirectCount() in every context, so that a test sees what a device with that limit or
    // without multiDrawIndirect does; 0 removes the cap.
    static void setMaxDrawIndirectCountForTests(uint32_t count);

    // Records a write of `data` into dst at `offset`, visible to later dispatches. Both offset and size
    // must be multiples of 4, and the size at most 65536 bytes.
    GpuStatus updateBuffer(const GpuBuffer& dst, VkDeviceSize offset, std::span<const std::byte> data);

    // The compute-side halves of the ownership transfers of uploads and downloads made with
    // ComputeHandoff::Record; the range must match the transfer's exactly. A raster context outside the compute
    // family refuses them: its buffers come from a compute context through acquireFromOtherQueue().
    GpuStatus acquireFromTransfer(const GpuTransfer& transfer, const GpuBuffer& buffer, VkDeviceSize offset,
                                  VkDeviceSize bytes);
    GpuStatus releaseToTransfer(const GpuTransfer& transfer, const GpuBuffer& buffer, VkDeviceSize offset,
                                VkDeviceSize bytes);

    // The device the context records for.
    GpuDevice& device() const;

    // Makes submit() and submitAndWait() also wait for `semaphore`, a binary semaphore another API signals,
    // before any command of the context runs. The caller keeps it alive, through hold(), until the context is
    // destroyed.
    void waitExternalSemaphore(VkSemaphore semaphore);

    // For tests that need work left pending on the device: makes submit() and submitAndWait() also wait for
    // `value`, a timeline semaphore value that may be signalled later, before any command of the context runs. The
    // caller keeps the semaphore alive until the work is done. An aborted context drops the wait unless it also has
    // an external semaphore to consume, as it then submits the waits alone.
    void waitTimeline(TimelineValue value);

    // Keeps `object` alive until the context is destroyed, or for good when the device is lost with the
    // context's work in flight.
    void hold(std::shared_ptr<void> object);

    // Records copies of `regions` from `src`, a GpuBuffer::createExportable() buffer that another API wrote and
    // owns, into `dst`, visible to later dispatches: src's first srcBytes are acquired from the external queue
    // family first and released back to it after, so the other API may write them again once the context's
    // work is done.
    GpuStatus copyFromExternal(const GpuBuffer& src, VkDeviceSize srcBytes, const GpuBuffer& dst,
                               std::span<const VkBufferCopy> regions);

    // Records copies of `regions` from `src` into `dst`, both buffers of this device, ordered after the dispatches
    // recorded before it and visible to those recorded after and to host reads once the context's work is done.
    GpuStatus copyBuffer(const GpuBuffer& src, const GpuBuffer& dst, std::span<const VkBufferCopy> regions);

    // Records a copy of `bytes` from src at srcOffset into dst at dstOffset, as copyBuffer() with one region does.
    GpuStatus copyBuffer(const GpuBuffer& src, VkDeviceSize srcOffset, const GpuBuffer& dst, VkDeviceSize dstOffset,
                         VkDeviceSize bytes);

    // Records filling the first `size` bytes of `dst` with the 32-bit `value`, ordered and made visible as copyBuffer()
    // is. size is a multiple of 4.
    GpuStatus fillBuffer(const GpuBuffer& dst, VkDeviceSize size, uint32_t value);

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
    // each command buffer. `stop` is asked between command buffers and every 20 ms of a wait; once it returns
    // true the call fails with VK_INCOMPLETE. A command buffer already submitted is then left to finish: the device
    // keeps it, its fence and everything the context holds until it does (GpuDevice::retireWhenSignalled()), and the
    // pooled buffers, buffers and textures the caller releases meanwhile are neither reused nor freed before then.
    // Later work on the same queue is ordered after it by its own barriers, as submission order counts toward them;
    // work on another queue is not ordered after it at all, so must not touch what it writes. The device is not
    // treated as lost.
    GpuStatus submitAndWait(uint64_t timeoutNs = 30'000'000'000ull, const std::function<bool()>& stop = {});

    // Ends the buffer and submits it once every wait is reached, signalling `signal` when it is done. The context
    // holds the command buffer and its scratch buffers, so it must not be destroyed before `signal` is reached.
    // Only a split context blocks: until all its command buffers but the last have finished, one at a time. When
    // `stop` returns true meanwhile it fails with VK_INCOMPLETE and `signal` is never signalled; a command buffer
    // already submitted is left to finish as submitAndWait() leaves one. An aborted context submits the waits and
    // the signal alone, so `signal` is still reached, and returns GpuStatus::aborted.
    GpuStatus submit(std::span<const TimelineValue> waits, TimelineValue signal, const std::function<bool()>& stop = {});

private:
    // Acceleration-structure builds record Vulkan commands of their own into the context's buffer.
    friend class GpuAccelRecorder;
    struct FinishedBatch {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        uint64_t work = 0;
    };

    static GpuStatus createOn(QueueKind queue, GpuKernelRegistry& registry, std::unique_ptr<GpuRecordContext>& out,
                              GpuBufferPool* pool, GpuPipelineCache* pipelines, GpuTextureCache* textures);
    GpuStatus beginCommandBuffer(VkCommandBuffer& out);
    // A null `cmd` submits the waits alone.
    GpuStatus submitFenced(VkCommandBuffer cmd, std::span<const VkSemaphoreSubmitInfo> waits, uint64_t timeoutNs,
                           const std::function<bool()>& stop);
    // Hands the submitted work behind `fence`, and everything the context holds, to GpuDevice::retireWhenSignalled().
    void retireWork(VkFence fence);
    std::shared_ptr<void> takeHoldings(std::shared_ptr<void> finalizer);
    GpuStatus waitBeforeBatches(std::span<const TimelineValue> waits, uint64_t timeoutNs, const std::function<bool()>& stop);
    GpuStatus runBatchesBeforeLast(std::span<const VkSemaphoreSubmitInfo> waits, uint64_t timeoutNs, const std::function<bool()>& stop);
    void abandonWork(const char* what);
    void touchBindings(std::span<const GpuBinding> bindings);
    void parkInFlightWork();
    std::vector<VkSemaphoreSubmitInfo> externalWaitInfos() const;
    GpuStatus checkBindable(const char* where, uint32_t binding, VkDeviceSize bytes) const;
    bool outsideComputeFamily() const;
    GpuStatus checkTextureBindings(const char* where, std::span<const GpuBinding> bindings) const;
    // Fails when the context was submitted, aborted, or is between beginRaster() and endRaster().
    GpuStatus recordable(const char* where) const;
    // As recordable(), then runs the abort check.
    GpuStatus checkAbort(const char* where);
    GpuStatus drawable(const char* where);
    GpuStatus bindRaster(const char* where, GpuRasterId raster, GpuRasterBlend blend,
                         std::span<const GpuBuffer* const> buffers, std::span<const std::byte> push);
    GpuStatus rasterPipeline(GpuRasterId raster, GpuRasterBlend blend, std::shared_ptr<GpuRasterPipeline>& out);
    GpuStatus rasterDescriptorSet(VkDescriptorSetLayout layout, uint32_t storageBuffers, VkDescriptorSet& out);
    void ownershipBarrier(const GpuBuffer& buffer, VkDeviceSize offset, VkDeviceSize bytes, bool release);

    GpuRecordContext(QueueKind queue, GpuKernelRegistry& registry, GpuBufferPool* pool, GpuPipelineCache* pipelines,
                     GpuTextureCache* textures)
        : registry_(registry)
        , bufferPool_(pool)
        , pipelines_(pipelines)
        , textureCache_(textures)
        , queue_(queue)
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
    std::vector<TimelineValue> timelineWaits_;
    // Every buffer the commands read or write, for GpuDevice::retireWhenSignalled().
    std::vector<VkBuffer> touched_;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    // The build scratch acceleration-structure builds share; the barrier before each build orders the previous
    // build's scratch writes before it.
    GpuBuffer* accelScratch_ = nullptr;
    std::vector<FinishedBatch> finished_;
    uint64_t work_ = 0;
    bool open_ = false;
    bool outOfMemory_ = false;
    // The work was handed to the device to retire, which ends the registry's recording for it.
    bool retired_ = false;

    const QueueKind queue_;
    std::function<bool()> abortCheck_;
    bool aborted_ = false;
    const GpuTexture* rasterTarget_ = nullptr;
    const GpuTexture* rendered_ = nullptr;
    std::vector<std::shared_ptr<GpuRasterPipeline>> rasterPipelines_;
    std::map<std::tuple<uint32_t, VkFormat, GpuRasterBlend>, std::shared_ptr<GpuRasterPipeline>> ownRasters_;
    PFN_vkCmdPushDescriptorSetKHR pushDescriptorFn_ = nullptr;
    std::vector<VkDescriptorPool> descriptorPools_;
};

} // namespace gpu
