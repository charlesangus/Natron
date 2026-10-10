#pragma once

#include "GpuDevice.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace gpu {

struct GpuTransferOptions
{
    // Staging slots per direction; each holds one strip.
    uint32_t slotCount = 3;
    VkDeviceSize stripBytes = VkDeviceSize(16) << 20;
    // Device-side input/output buffer pairs; 2 lets upload k+1 and download k-1 run beside compute k.
    uint32_t framesInFlight = 2;
    // Size of the worker pool shared by both directions for host<->staging memcpy.
    uint32_t copyThreads = 1;
    bool allowHostImport = true;
    // Keep host-pointer imports alive across process() calls, keyed by (pointer, size), so pinning
    // is paid once per buffer. The caller must keep such memory valid until releaseHostImports().
    bool cacheHostImports = false;
    bool recordTimestamps = true;
};

enum class TransferPath
{
    Staging,
    HostImport,
};

struct TransferFrame
{
    const void* src = nullptr;
    void* dst = nullptr;
    VkDeviceSize bytes = 0;
    // Download only [dstOffset, dstOffset + dstBytes) of the output buffer into dst. Zero dstBytes
    // downloads `bytes` from the start; a strip with halo rows uses this to return just its interior.
    VkDeviceSize dstOffset = 0;
    VkDeviceSize dstBytes = 0;
};

struct ComputeBinding
{
    VkBuffer input = VK_NULL_HANDLE;
    VkBuffer output = VK_NULL_HANDLE;
    VkDeviceSize bytes = 0;
    uint32_t frameIndex = 0;
};

// Records the per-frame work into a compute-queue command buffer. Input and
// output are owned by the compute family for the duration of the callback.
using ComputeRecordFn = std::function<void(VkCommandBuffer, const ComputeBinding&)>;

struct TimeSpan
{
    double beginMs = 0.0;
    double endMs = 0.0;
    // Sum of the pieces inside [beginMs, endMs] that were actually working.
    double busyMs = 0.0;
};

struct FrameTimeline
{
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

struct TransferTimeline
{
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

class GpuTransfer
{
public:
    static GpuStatus create(GpuDevice& device, const GpuTransferOptions& options,
                            std::unique_ptr<GpuTransfer>& out);
    ~GpuTransfer();
    GpuTransfer(const GpuTransfer&) = delete;
    GpuTransfer& operator=(const GpuTransfer&) = delete;

    const GpuTransferOptions& options() const { return options_; }

    // True when host pointers can be imported; src/dst and the size must be multiples of the alignment.
    bool hostImportSupported() const { return importAlignment_ != 0; }
    VkDeviceSize hostImportAlignment() const { return importAlignment_; }

    // Streams every frame host -> device -> compute -> device -> host, pipelined
    // across frames. A null `record` copies input to output. Blocks until all
    // frames are back in their dst buffers.
    GpuStatus process(std::span<const TransferFrame> frames, const ComputeRecordFn& record,
                      TransferTimeline* timeline = nullptr);

    // Waits for the device to go idle, then drops every cached import.
    void releaseHostImports();

private:
    struct Impl;
    explicit GpuTransfer(GpuDevice& device, const GpuTransferOptions& options);

    GpuDevice& device_;
    GpuTransferOptions options_;
    VkDeviceSize importAlignment_ = 0;
    std::unique_ptr<Impl> impl_;
};

} // namespace gpu
