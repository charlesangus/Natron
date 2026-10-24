#pragma once

#include "GpuBuffer.h"
#include "GpuDevice.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace gpu {

struct GpuTransferOptions {
    // Staging slots per direction; each holds one strip.
    uint32_t slotCount = 3;
    VkDeviceSize stripBytes = VkDeviceSize(16) << 20;
    // Device-side input/output buffer pairs; 2 lets upload k+1 and download k-1 run beside compute k.
    uint32_t framesInFlight = 2;
    // Size of the worker pool shared by both directions for host<->staging memcpy.
    uint32_t copyThreads = 1;
    bool allowHostImport = true;
    // Keep host-pointer imports alive across calls so pinning is paid once per allocation. The
    // caller must call forgetHostAllocation() before freeing such memory, or releaseHostImports().
    bool cacheHostImports = false;
    // Fills the device-side TimeSpans of TransferTimeline. Costs a blocking query-pool reset per process().
    bool recordTimestamps = false;
    // Called on the importing thread, with no lock held, between creating an import meant for the cache and
    // caching it, so that tests can interleave forgetHostAllocation() with it.
    std::function<void(const void* host, VkDeviceSize bytes)> onImportCreated;
    // Makes every host-pointer import fail as a driver rejection would, so that tests can reach the
    // ImportFailed staged reason.
    bool failHostImportsForTests = false;
};

enum class TransferPath {
    Staging,
    HostImport,
};

// Caller-owned host memory. upload() only reads it.
struct HostRange {
    void* data = nullptr;
    VkDeviceSize bytes = 0;
};

// A point on a timeline semaphore; reaching it means the operation that returned it is done on the
// device. A default value is already reached.
struct TimelineValue {
    VkSemaphore semaphore = VK_NULL_HANDLE;
    uint64_t value = 0;

    explicit operator bool() const { return semaphore != VK_NULL_HANDLE && value != 0; }

    // A wait entry for VkSubmitInfo2::pWaitSemaphoreInfos of a submission on any queue.
    VkSemaphoreSubmitInfo waitInfo(VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) const
    {
        VkSemaphoreSubmitInfo w { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
        w.semaphore = semaphore;
        w.value = value;
        w.stageMask = stages;
        return w;
    }
};

// Who performs the compute-queue half of the queue-family ownership transfer of the device range.
// All three behave the same when transfer and compute share a queue family.
enum class ComputeHandoff {
    // GpuTransfer submits it on the compute queue; the returned value already covers it.
    Submit,
    // The caller records recordComputeAcquire() / recordComputeRelease() into its own compute
    // command buffer, which waits on the upload value or is waited on by the download.
    Record,
    // The range never visits the compute queue, e.g. an upload read straight back by a download.
    None,
};

struct TimeSpan {
    double beginMs = 0.0;
    double endMs = 0.0;
    // Sum of the pieces inside [beginMs, endMs] that were actually working.
    double busyMs = 0.0;
};

struct StreamOptions {
    // Device work the transfer must wait for, e.g. compute that reads the range being overwritten
    // or writes the range being downloaded.
    std::span<const TimelineValue> waits;
    ComputeHandoff handoff = ComputeHandoff::Submit;
    // Receives the path taken.
    TransferPath* path = nullptr;
    // Writes device timestamps at queries timestampQuery and timestampQuery + 1 around the
    // transfer, when the transfer family supports them.
    VkQueryPool timestampPool = VK_NULL_HANDLE;
    uint32_t timestampQuery = 0;
    // Host memcpy time, relative to hostOrigin. A download fills it in by the time wait() returns.
    TimeSpan* hostSpan = nullptr;
    std::chrono::steady_clock::time_point hostOrigin {};
};

struct HostImportStats {
    // Host imports created so far, cached or not, and the bytes they imported.
    uint64_t created = 0;
    uint64_t bytes = 0;
    // Of those, the ones created by prepareImport(), upload() and download().
    uint64_t prepared = 0;
    uint64_t atUpload = 0;
    uint64_t atDownload = 0;
    // Imports destroyed right after creation: another thread cached the range first, or it was forgotten meanwhile.
    uint64_t dropped = 0;
    // Imports in the cache now.
    uint64_t cached = 0;
};

// Why a transfer went through the staging ring instead of importing its host memory.
enum class StagedReason {
    // Host import is unsupported or turned off.
    Disabled,
    // The range is not aligned for import and no cached import covers it.
    Unaligned,
    // The driver or the buffer rejected the import.
    ImportFailed,
    // The range was forgotten while its import was being created.
    Forgotten,
};

// Uploads and downloads that took the staging path, by reason.
struct StagedReasonCounts {
    uint64_t disabled = 0;
    uint64_t unaligned = 0;
    uint64_t importFailed = 0;
    uint64_t forgotten = 0;

    uint64_t total() const { return disabled + unaligned + importFailed + forgotten; }
};

struct TransferFrame {
    const void* src = nullptr;
    void* dst = nullptr;
    VkDeviceSize bytes = 0;
    // Download only [dstOffset, dstOffset + dstBytes) of the output buffer into dst. Zero dstBytes
    // downloads `bytes` from the start; a strip with halo rows uses this to return just its interior.
    VkDeviceSize dstOffset = 0;
    VkDeviceSize dstBytes = 0;
};

struct ComputeBinding {
    VkBuffer input = VK_NULL_HANDLE;
    VkBuffer output = VK_NULL_HANDLE;
    VkDeviceSize bytes = 0;
    uint32_t frameIndex = 0;
};

// Records the per-frame work into a compute-queue command buffer. Input and
// output are owned by the compute family for the duration of the callback.
// A failed status stops process() before that command buffer is submitted, and
// process() returns it.
using ComputeRecordFn = std::function<GpuStatus(VkCommandBuffer, const ComputeBinding&)>;

struct FrameTimeline {
    TransferPath uploadPath = TransferPath::Staging;
    TransferPath downloadPath = TransferPath::Staging;
    uint32_t strips = 0;

    // Host clock, relative to the start of the run.
    TimeSpan hostUpload;
    TimeSpan hostDownload;

    // Device timestamps, relative to the earliest timestamp of the run. Zero when unavailable.
    TimeSpan gpuUpload;
    TimeSpan gpuCompute;
    TimeSpan gpuDownload;
};

struct TransferTimeline {
    std::vector<FrameTimeline> frames;
    bool gpuTimestamps = false;
    double wallMs = 0.0;
    double gpuSpanMs = 0.0;

    double hostUploadBusyMs() const;
    double hostDownloadBusyMs() const;
    double gpuUploadBusyMs() const;
    double gpuComputeBusyMs() const;
    double gpuDownloadBusyMs() const;
    // What a fully serial run would take: every host and device stage end to end.
    double sumOfStagesMs() const;
};

// Host<->device copies, uploads on the upload queue and downloads on the download queue. Every copy is split at GpuDeviceInfo::copyChunkSize.
// A host range that cached imports cover, at any alignment, or that is aligned to
// minImportedHostPointerAlignment and can be imported, is copied without a host memcpy; anything else goes
// through a pinned staging ring. Thread-safe.
class GpuTransfer {
public:
    static GpuStatus create(GpuDevice& device, const GpuTransferOptions& options,
                            std::unique_ptr<GpuTransfer>& out);
    ~GpuTransfer();
    GpuTransfer(const GpuTransfer&) = delete;
    GpuTransfer& operator=(const GpuTransfer&) = delete;

    const GpuTransferOptions& options() const { return options_; }

    // True when host pointers can be imported; the pointer and the size must be multiples of the alignment.
    bool hostImportSupported() const { return importAlignment_ != 0; }
    VkDeviceSize hostImportAlignment() const { return importAlignment_; }

    // True when both the transfer and the compute families write timestamps, so that StreamOptions::timestampPool
    // is honoured; process() records its own only with GpuTransferOptions::recordTimestamps.
    bool timestampsSupported() const;
    double timestampPeriodNs() const;

    // Copies src into [offset, offset + src.bytes) of dst. Returns once the staging path has read
    // src; the import path reads it until `done` is reached, so src must stay unchanged until then.
    // With ComputeHandoff::Submit, compute work that waits on `done` may use the range directly.
    GpuStatus upload(HostRange src, GpuBuffer& dst, VkDeviceSize offset, TimelineValue& done,
                     const StreamOptions& options = {});

    // Copies [offset, offset + dst.bytes) of src into dst. dst holds the data once wait(done)
    // returns. The range is then owned by the transfer family: compute may overwrite it, but must
    // upload it again before reading it.
    GpuStatus download(GpuBuffer& src, VkDeviceSize offset, HostRange dst, TimelineValue& done,
                       const StreamOptions& options = {});

    // Blocks until `value` is reached and, for a download, its data has landed in host memory.
    GpuStatus wait(TimelineValue value);

    // The compute-side halves of the ownership transfer for ComputeHandoff::Record. The range must
    // match the upload's or download's exactly. No-ops when the families are the same.
    void recordComputeAcquire(VkCommandBuffer computeCmd, const GpuBuffer& buffer, VkDeviceSize offset,
                              VkDeviceSize bytes) const;
    void recordComputeRelease(VkCommandBuffer computeCmd, const GpuBuffer& buffer, VkDeviceSize offset,
                              VkDeviceSize bytes) const;

    // Streams every frame host -> device -> compute -> device -> host, pipelined
    // across frames. A null `record` copies input to output. Blocks until all
    // frames are back in their dst buffers. An exception thrown by `record` is
    // rethrown once the work already submitted has drained.
    GpuStatus process(std::span<const TransferFrame> frames, const ComputeRecordFn& record,
                      TransferTimeline* timeline = nullptr);

    // Imports [range.data, range.data + range.bytes) into the cache, so that a later upload() or download() of
    // it or of any part of it, aligned or not, creates no import and needs no staging. Returns whether the range is cached now: false when imports are
    // not cached, the range is not aligned for import, the import failed or the range was forgotten meanwhile.
    // The memory must stay allocated until it returns.
    bool prepareImport(HostRange range);

    // Drops every cached import overlapping [ptr, ptr + max(bytes, 1)), waiting for transfers that
    // still use them, and for imports of the range still being created, which are then never cached.
    // Call before freeing or remapping cached host memory. Cheap when nothing is cached.
    void forgetHostAllocation(const void* ptr, size_t bytes = 0);

    // Waits for the device to go idle, then drops every cached import.
    void releaseHostImports();

    // Host imports created so far, cached or not.
    uint64_t hostImportCount() const;
    // Bytes of host memory imported so far, cached or not.
    uint64_t hostImportBytes() const;
    HostImportStats hostImportStats() const;
    // Uploads and downloads that went through staging so far, by reason. prepareImport() does not count.
    StagedReasonCounts stagedReasons() const;

private:
    struct Impl;
    explicit GpuTransfer(GpuDevice& device, const GpuTransferOptions& options);

    GpuDevice& device_;
    GpuTransferOptions options_;
    VkDeviceSize importAlignment_ = 0;
    std::unique_ptr<Impl> impl_;
};

} // namespace gpu
