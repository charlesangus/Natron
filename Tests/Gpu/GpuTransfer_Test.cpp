#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <sys/mman.h>

#include "GpuBuffer.h"
#include "GpuDevice.h"
#include "GpuKernel.h"
#include "GpuRuntime.h"
#include "GpuTestDevice.h"
#include "GpuTransfer.h"
#include "fill_spirv.h"

using namespace gpu;

namespace {

constexpr size_t kWidth = 3840;
constexpr size_t kHeight = 2160;
constexpr size_t kUhdBytes = kWidth * kHeight * 4 * sizeof(float);
constexpr uint32_t kPipelinedFrames = 6;
constexpr size_t kAlign = 4096;

struct FreeDeleter {
    void operator()(void* p) const { std::free(p); }
};
using HostBuf = std::unique_ptr<uint8_t, FreeDeleter>;

HostBuf
alignedBuf(size_t bytes)
{
    const size_t rounded = (bytes + kAlign - 1) / kAlign * kAlign;
    return HostBuf(static_cast<uint8_t*>(std::aligned_alloc(kAlign, rounded)));
}

// Distinct per frame and per word so a misplaced strip or a stale frame cannot compare equal.
void
fillFrame(uint8_t* p, size_t bytes, uint32_t seed)
{
    const size_t words = bytes / 4;
    const unsigned nt = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    std::vector<std::thread> ts;
    for (unsigned t = 0; t < nt; ++t) {
        ts.emplace_back([=] {
            float* f = reinterpret_cast<float*>(p);
            const size_t b = words * t / nt;
            const size_t e = words * (t + 1) / nt;
            for (size_t i = b; i < e; ++i) {
                uint32_t x = static_cast<uint32_t>(i) * 2654435761u ^ (seed * 0x9E3779B9u);
                x ^= x >> 15;
                f[i] = static_cast<float>(x & 0xFFFFFF) * (1.0f / 16777216.0f);
            }
        });
    }
    for (std::thread& t : ts)
        t.join();
    for (size_t i = words * 4; i < bytes; ++i)
        p[i] = static_cast<uint8_t>(i + seed);
}

const char*
pathName(TransferPath p)
{
    return p == TransferPath::HostImport ? "host-import" : "staging";
}

class GpuTransferTest : public gputest::SkippableTest {
protected:
    static void SetUpTestCase()
    {
        check_ = gputest::ValidationCheck();
        GpuStatus s = gputest::createDevice(dev_);
        if (!s) {
            setupError_ = s.message;
            return;
        }
        std::printf("device: %s transferSharesComputeQueue=%d extMemHost=%d validation=%d\n", dev_->info().name.c_str(),
                    dev_->info().transferSharesComputeQueue, dev_->info().externalMemoryHost,
                    dev_->info().validationEnabled);
        for (uint32_t k = 0; k < kPipelinedFrames; ++k) {
            srcs_.push_back(alignedBuf(kUhdBytes));
            dsts_.push_back(alignedBuf(kUhdBytes));
            fillFrame(srcs_.back().get(), kUhdBytes, k + 1);
        }
    }

    static void TearDownTestCase()
    {
        srcs_.clear();
        dsts_.clear();
        dev_.reset();
        check_.expectClean("device teardown");
    }

    void SetUp() override
    {
        check_.expectClean("device setup");
        GPUTEST_DEVICE_OR_SKIP_TEST(dev_ != nullptr, setupError_);
    }

    void TearDown() override
    {
        if (dev_) {
            dev_->waitIdle();
        }
        check_.expectClean("the test");
    }

    std::unique_ptr<GpuTransfer> make(GpuTransferOptions o)
    {
        std::unique_ptr<GpuTransfer> t;
        GpuStatus s = GpuTransfer::create(*dev_, o, t);
        EXPECT_TRUE(s.ok()) << s.message;
        return t;
    }

    static std::unique_ptr<GpuDevice> dev_;
    static std::string setupError_;
    static std::vector<HostBuf> srcs_;
    static std::vector<HostBuf> dsts_;
    static gputest::ValidationCheck check_;
};

std::unique_ptr<GpuDevice> GpuTransferTest::dev_;
std::string GpuTransferTest::setupError_;
std::vector<HostBuf> GpuTransferTest::srcs_;
std::vector<HostBuf> GpuTransferTest::dsts_;
gputest::ValidationCheck GpuTransferTest::check_;

void
expectSame(const void* a, const void* b, size_t bytes, uint32_t frame)
{
    if (std::memcmp(a, b, bytes) == 0) {
        return;
    }
    const uint8_t* x = static_cast<const uint8_t*>(a);
    const uint8_t* y = static_cast<const uint8_t*>(b);
    size_t i = 0;
    while (x[i] == y[i])
        ++i;
    ADD_FAILURE() << "frame " << frame << " differs first at byte " << i << " of " << bytes;
}

} // namespace

GPU_TEST_F(GpuTransferTest, StagingRoundTripUhd)
{
    GpuTransferOptions o;
    o.allowHostImport = false;
    auto t = make(o);
    ASSERT_TRUE(t != nullptr);
    std::memset(dsts_[0].get(), 0, kUhdBytes);
    TransferFrame f { srcs_[0].get(), dsts_[0].get(), kUhdBytes };
    TransferTimeline tl;
    GpuStatus s = t->process({ &f, 1 }, nullptr, &tl);
    ASSERT_TRUE(s.ok()) << s.message;
    ASSERT_EQ(tl.frames.size(), 1u);
    EXPECT_EQ(tl.frames[0].uploadPath, TransferPath::Staging);
    EXPECT_EQ(tl.frames[0].downloadPath, TransferPath::Staging);
    EXPECT_GT(tl.frames[0].strips, 1u);
    expectSame(srcs_[0].get(), dsts_[0].get(), kUhdBytes, 0);
}

GPU_TEST_F(GpuTransferTest, HostImportRoundTripUhd)
{
    GpuTransferOptions o;
    auto t = make(o);
    ASSERT_TRUE(t != nullptr);
    if (!t->hostImportSupported()) {
        GPUTEST_SKIP() << "VK_EXT_external_memory_host unavailable";
    }
    ASSERT_EQ(kAlign % t->hostImportAlignment(), 0u);
    std::memset(dsts_[1].get(), 0, kUhdBytes);
    TransferFrame f { srcs_[1].get(), dsts_[1].get(), kUhdBytes };
    TransferTimeline tl;
    GpuStatus s = t->process({ &f, 1 }, nullptr, &tl);
    ASSERT_TRUE(s.ok()) << s.message;
    EXPECT_EQ(tl.frames[0].uploadPath, TransferPath::HostImport);
    EXPECT_EQ(tl.frames[0].downloadPath, TransferPath::HostImport);
    expectSame(srcs_[1].get(), dsts_[1].get(), kUhdBytes, 1);
}

GPU_TEST_F(GpuTransferTest, MisalignedPointerFallsBackToStaging)
{
    auto t = make({});
    ASSERT_TRUE(t != nullptr);
    const size_t bytes = kUhdBytes - 4096;
    std::memset(dsts_[2].get(), 0, kUhdBytes);
    TransferFrame f { srcs_[2].get() + 16, dsts_[2].get(), bytes };
    TransferTimeline tl;
    GpuStatus s = t->process({ &f, 1 }, nullptr, &tl);
    ASSERT_TRUE(s.ok()) << s.message;
    EXPECT_EQ(tl.frames[0].uploadPath, TransferPath::Staging);
    if (t->hostImportSupported()) {
        EXPECT_EQ(tl.frames[0].downloadPath, TransferPath::HostImport);
    }
    expectSame(srcs_[2].get() + 16, dsts_[2].get(), bytes, 2);
}

GPU_TEST_F(GpuTransferTest, OddSizesAndUserComputeCallback)
{
    GpuTransferOptions o;
    o.stripBytes = 1 << 20;
    o.allowHostImport = false;
    auto t = make(o);
    ASSERT_TRUE(t != nullptr);
    const std::vector<size_t> sizes = { 4, (5u << 20) + 12, 1u << 20, 3 * 1000 * 1000 + 8 };
    std::vector<TransferFrame> frames;
    for (size_t i = 0; i < sizes.size(); ++i) {
        std::memset(dsts_[i].get(), 0, sizes[i]);
        frames.push_back({ srcs_[i].get(), dsts_[i].get(), sizes[i] });
    }
    uint32_t calls = 0;
    auto copy = [&](VkCommandBuffer cb, const ComputeBinding& b) {
        EXPECT_EQ(b.bytes, sizes[b.frameIndex]);
        VkBufferCopy r { 0, 0, b.bytes };
        vkCmdCopyBuffer(cb, b.input, b.output, 1, &r);
        ++calls;
        return GpuStatus {};
    };
    GpuStatus s = t->process(frames, copy, nullptr);
    ASSERT_TRUE(s.ok()) << s.message;
    EXPECT_EQ(calls, sizes.size());
    for (size_t i = 0; i < sizes.size(); ++i) {
        expectSame(srcs_[i].get(), dsts_[i].get(), sizes[i], static_cast<uint32_t>(i));
    }
}

GPU_TEST_F(GpuTransferTest, FailedComputeRecordAbortsAndTransferStaysUsable)
{
    GpuTransferOptions o;
    o.stripBytes = 1 << 20;
    o.allowHostImport = false;
    auto t = make(o);
    ASSERT_TRUE(t != nullptr);
    constexpr size_t kBytes = (3u << 20) + 64;
    std::vector<TransferFrame> frames;
    for (uint32_t k = 0; k < 4; ++k) {
        std::memset(dsts_[k].get(), 0, kBytes);
        frames.push_back({ srcs_[k].get(), dsts_[k].get(), kBytes });
    }
    uint32_t calls = 0;
    GpuStatus s = t->process(frames, [&](VkCommandBuffer cb, const ComputeBinding& b) -> GpuStatus {
        ++calls;
        if (b.frameIndex == 2) {
            return { VK_ERROR_OUT_OF_POOL_MEMORY, "synthetic record failure" };
        }
        VkBufferCopy r { 0, 0, b.bytes };
        vkCmdCopyBuffer(cb, b.input, b.output, 1, &r);
        return {};
    });
    EXPECT_EQ(s.result, VK_ERROR_OUT_OF_POOL_MEMORY);
    EXPECT_EQ(s.message, "synthetic record failure");
    EXPECT_EQ(calls, 3u);

    ASSERT_TRUE(t->process(frames, nullptr).ok());
    for (uint32_t k = 0; k < frames.size(); ++k) {
        expectSame(srcs_[k].get(), dsts_[k].get(), kBytes, k);
    }
}

GPU_TEST_F(GpuTransferTest, ThrowingComputeRecordPropagatesAndTransferStaysUsable)
{
    GpuTransferOptions o;
    o.stripBytes = 1 << 20;
    o.allowHostImport = false;
    auto t = make(o);
    ASSERT_TRUE(t != nullptr);
    constexpr size_t kBytes = (3u << 20) + 64;
    std::vector<TransferFrame> frames;
    for (uint32_t k = 0; k < 4; ++k) {
        std::memset(dsts_[k].get(), 0, kBytes);
        frames.push_back({ srcs_[k].get(), dsts_[k].get(), kBytes });
    }
    EXPECT_THROW(t->process(frames,
                            [&](VkCommandBuffer cb, const ComputeBinding& b) -> GpuStatus {
                                if (b.frameIndex == 1) {
                                    throw std::runtime_error("synthetic");
                                }
                                VkBufferCopy r { 0, 0, b.bytes };
                                vkCmdCopyBuffer(cb, b.input, b.output, 1, &r);
                                return {};
                            }),
                 std::runtime_error);

    ASSERT_TRUE(t->process(frames, nullptr).ok());
    for (uint32_t k = 0; k < frames.size(); ++k) {
        expectSame(srcs_[k].get(), dsts_[k].get(), kBytes, k);
    }
}

namespace {

struct RunResult {
    std::string label;
    TransferTimeline tl;
};

void
printTimeline(const RunResult& r)
{
    const TransferTimeline& tl = r.tl;
    const double n = static_cast<double>(tl.frames.size());
    std::printf("\n[%s] %zu frames, up=%s down=%s, strips/frame=%u\n", r.label.c_str(), tl.frames.size(),
                pathName(tl.frames[0].uploadPath), pathName(tl.frames[0].downloadPath), tl.frames[0].strips);
    std::printf("  per-frame mean ms: host-up %.2f | gpu-up %.2f | gpu-compute %.2f | gpu-down %.2f | host-down %.2f\n",
                tl.hostUploadBusyMs() / n, tl.gpuUploadBusyMs() / n, tl.gpuComputeBusyMs() / n,
                tl.gpuDownloadBusyMs() / n, tl.hostDownloadBusyMs() / n);
    std::printf("  sum of stages %.1f ms | pipelined wall %.1f ms (%.2f ms/frame) | gpu span %.1f ms | overlap x%.2f\n",
                tl.sumOfStagesMs(), tl.wallMs, tl.wallMs / n, tl.gpuSpanMs, tl.sumOfStagesMs() / tl.wallMs);
    std::printf("  frame | host-up [b..e]   | gpu-up [b..e]    | compute [b..e]   | gpu-down [b..e]  | host-down [b..e]\n");
    for (size_t k = 0; k < tl.frames.size(); ++k) {
        const FrameTimeline& f = tl.frames[k];
        std::printf("  %5zu | %6.1f..%6.1f | %6.1f..%6.1f | %6.1f..%6.1f | %6.1f..%6.1f | %6.1f..%6.1f\n", k,
                    f.hostUpload.beginMs, f.hostUpload.endMs, f.gpuUpload.beginMs, f.gpuUpload.endMs,
                    f.gpuCompute.beginMs, f.gpuCompute.endMs, f.gpuDownload.beginMs, f.gpuDownload.endMs,
                    f.hostDownload.beginMs, f.hostDownload.endMs);
    }
}

} // namespace

// Prints the stage timeline of each configuration. Overlap depends on machine load, so the
// figures are reported rather than asserted; the test checks the data and transfer paths.
GPU_TEST_F(GpuTransferTest, PipelinedFramesTimeline)
{
    const uint32_t many = std::max(2u, std::min(8u, std::thread::hardware_concurrency()));
    struct Config {
        std::string label;
        bool import;
        uint32_t threads;
        bool serial;
        bool cached = false;
    };
    const std::vector<Config> configs = {
        { "staging, 1 copy thread, serial", false, 1, true },
        { "staging, 1 copy thread", false, 1, false },
        { "staging, " + std::to_string(many) + " copy threads, serial", false, many, true },
        { "staging, " + std::to_string(many) + " copy threads", false, many, false },
        { "host-import, serial", true, 1, true },
        { "host-import", true, 1, false },
        { "host-import cached, serial", true, 1, true, true },
        { "host-import cached", true, 1, false, true },
    };

    std::vector<TransferFrame> frames;
    for (uint32_t k = 0; k < kPipelinedFrames; ++k) {
        frames.push_back({ srcs_[k].get(), dsts_[k].get(), kUhdBytes });
    }

    std::vector<RunResult> results;
    for (const Config& c : configs) {
        GpuTransferOptions o;
        o.allowHostImport = c.import;
        o.copyThreads = c.threads;
        o.cacheHostImports = c.cached;
        o.recordTimestamps = true;
        auto t = make(o);
        ASSERT_TRUE(t != nullptr);
        if (c.import && !t->hostImportSupported()) {
            std::printf("\n[%s] skipped: no VK_EXT_external_memory_host\n", c.label.c_str());
            continue;
        }
        for (uint32_t k = 0; k < kPipelinedFrames; ++k) {
            std::memset(dsts_[k].get(), 0, kUhdBytes);
        }
        // Warm-up pays first-touch page faults, and with cached imports the pinning, outside the measurement.
        ASSERT_TRUE(t->process({ frames.data(), c.cached ? frames.size() : 1 }, nullptr, nullptr).ok());

        RunResult r { c.label, {} };
        if (c.serial) {
            for (uint32_t k = 0; k < kPipelinedFrames; ++k) {
                TransferTimeline one;
                GpuStatus s = t->process({ &frames[k], 1 }, nullptr, &one);
                ASSERT_TRUE(s.ok()) << s.message;
                r.tl.wallMs += one.wallMs;
                r.tl.gpuSpanMs += one.gpuSpanMs;
                r.tl.gpuTimestamps = one.gpuTimestamps;
                r.tl.frames.push_back(one.frames[0]);
            }
        } else {
            GpuStatus s = t->process(frames, nullptr, &r.tl);
            ASSERT_TRUE(s.ok()) << s.message;
        }
        for (uint32_t k = 0; k < kPipelinedFrames; ++k) {
            expectSame(srcs_[k].get(), dsts_[k].get(), kUhdBytes, k);
        }
        const TransferPath want = c.import ? TransferPath::HostImport : TransferPath::Staging;
        for (const FrameTimeline& f : r.tl.frames) {
            EXPECT_EQ(f.uploadPath, want);
            EXPECT_EQ(f.downloadPath, want);
        }
        printTimeline(r);
        results.push_back(std::move(r));
    }

    std::printf("\nsummary (ms, %u UHD RGBA float frames, %.1f MB each)\n", kPipelinedFrames, kUhdBytes / 1e6);
    std::printf("  %-34s %9s %9s %9s %9s %9s %10s %10s\n", "config", "host-up", "gpu-up", "compute", "gpu-down",
                "host-down", "sum", "wall");
    for (const RunResult& r : results) {
        const double n = static_cast<double>(r.tl.frames.size());
        std::printf("  %-34s %9.2f %9.2f %9.2f %9.2f %9.2f %10.1f %10.1f\n", r.label.c_str(), r.tl.hostUploadBusyMs() / n,
                    r.tl.gpuUploadBusyMs() / n, r.tl.gpuComputeBusyMs() / n, r.tl.gpuDownloadBusyMs() / n,
                    r.tl.hostDownloadBusyMs() / n, r.tl.sumOfStagesMs(), r.tl.wallMs);
    }
}

namespace {

constexpr VkDeviceSize kChunk = VkDeviceSize(1) << 20;
constexpr VkDeviceSize kNineMiB = VkDeviceSize(9) << 20;

struct FillParams {
    uint32_t width;
    uint32_t height;
    uint32_t asFloat;
};

} // namespace

GPU_TEST_F(GpuTransferTest, ChunkedRoundTripBothPaths)
{
    GpuDeviceOptions opts = gputest::deviceOptions();
    opts.copyChunkSize = kChunk;
    std::unique_ptr<GpuDevice> dev;
    GpuStatus s = gputest::createDevice(dev, opts);
    ASSERT_TRUE(s.ok()) << s.message;
    ASSERT_EQ(dev->info().copyChunkSize, kChunk);

    for (bool import : { false, true }) {
        GpuTransferOptions o;
        o.allowHostImport = import;
        std::unique_ptr<GpuTransfer> t;
        s = GpuTransfer::create(*dev, o, t);
        ASSERT_TRUE(s.ok()) << s.message;
        if (import && !t->hostImportSupported()) {
            std::printf("host-import path skipped: no VK_EXT_external_memory_host\n");
            continue;
        }
        const TransferPath want = import ? TransferPath::HostImport : TransferPath::Staging;
        GpuBuffer buf;
        s = GpuBuffer::createDeviceLocal(*dev, kNineMiB, kGpuBufferDefaultUsage, buf);
        ASSERT_TRUE(s.ok()) << s.message;
        uint8_t* src = srcs_[0].get();
        uint8_t* dst = dsts_[0].get();
        std::memset(dst, 0, kNineMiB);

        TransferPath upPath = TransferPath::Staging;
        StreamOptions up;
        up.path = &upPath;
        TimelineValue uploaded;
        s = t->upload({ src, kNineMiB }, buf, 0, uploaded, up);
        ASSERT_TRUE(s.ok()) << s.message;
        EXPECT_EQ(upPath, want);

        TransferPath downPath = TransferPath::Staging;
        StreamOptions down;
        down.path = &downPath;
        down.waits = { &uploaded, 1 };
        TimelineValue downloaded;
        s = t->download(buf, 0, { dst, kNineMiB }, downloaded, down);
        ASSERT_TRUE(s.ok()) << s.message;
        EXPECT_EQ(downPath, want);
        s = t->wait(downloaded);
        ASSERT_TRUE(s.ok()) << s.message;
        expectSame(src, dst, kNineMiB, 0);

        // Strip interiors: an aligned and a misaligned destination for the same row range.
        const VkDeviceSize offset = (VkDeviceSize(3) << 20) + 4096;
        const VkDeviceSize bytes = (VkDeviceSize(2) << 20) + 8192;
        uint8_t* aligned = dsts_[1].get();
        uint8_t* misaligned = dsts_[2].get() + 16;
        std::memset(aligned, 0, bytes);
        std::memset(misaligned, 0, bytes);
        StreamOptions part;
        part.handoff = ComputeHandoff::None;
        part.waits = { &downloaded, 1 };
        TransferPath alignedPath = TransferPath::Staging;
        TransferPath misalignedPath = TransferPath::HostImport;
        TimelineValue a;
        TimelineValue b;
        part.path = &alignedPath;
        ASSERT_TRUE(t->download(buf, offset, { aligned, bytes }, a, part).ok());
        part.path = &misalignedPath;
        ASSERT_TRUE(t->download(buf, offset, { misaligned, bytes }, b, part).ok());
        ASSERT_TRUE(t->wait(b).ok());
        ASSERT_TRUE(t->wait(a).ok());
        EXPECT_EQ(alignedPath, want);
        EXPECT_EQ(misalignedPath, TransferPath::Staging);
        expectSame(src + offset, aligned, bytes, 1);
        expectSame(src + offset, misaligned, bytes, 2);
    }
}

// Uploads into one buffer from one thread while another thread downloads a second buffer, so that on a device with
// queues of their own the two directions run side by side. Each direction signals its own timeline, in increasing
// order, and every byte lands where it should.
GPU_TEST_F(GpuTransferTest, ConcurrentUploadsAndDownloadsOnSeparateQueues)
{
    const GpuDeviceInfo& info = dev_->info();
    if (info.transferSharesComputeQueue) {
        GPUTEST_SKIP() << "the device has one queue, so uploads and downloads never run concurrently";
    }
    std::printf("queues: compute %u/%u upload %u/%u download %u/%u\n", info.computeFamily, info.computeQueueIndex,
                info.transferFamily, info.uploadQueueIndex, info.transferFamily, info.downloadQueueIndex);

    GpuTransferOptions o;
    o.copyThreads = 2;
    auto t = make(o);
    ASSERT_TRUE(t != nullptr);

    constexpr VkDeviceSize kPiece = VkDeviceSize(16) << 20;
    constexpr uint32_t kRounds = 6;
    GpuBuffer upBuf;
    GpuBuffer downBuf;
    ASSERT_TRUE(GpuBuffer::createDeviceLocal(*dev_, kPiece * kRounds, kGpuBufferDefaultUsage, upBuf).ok());
    ASSERT_TRUE(GpuBuffer::createDeviceLocal(*dev_, kPiece, kGpuBufferDefaultUsage, downBuf).ok());

    StreamOptions none;
    none.handoff = ComputeHandoff::None;
    TimelineValue seeded;
    ASSERT_TRUE(t->upload({ srcs_[5].get(), kPiece }, downBuf, 0, seeded, none).ok());
    ASSERT_TRUE(t->wait(seeded).ok());

    std::vector<TimelineValue> ups(kRounds);
    std::vector<TimelineValue> downs(kRounds);
    std::atomic<bool> go { false };
    std::thread uploader([&] {
        while (!go.load()) {
            std::this_thread::yield();
        }
        for (uint32_t k = 0; k < kRounds; ++k) {
            const GpuStatus s = t->upload({ srcs_[k % 5].get() + k * kPiece, kPiece }, upBuf, k * kPiece, ups[k], none);
            EXPECT_TRUE(s.ok()) << s.message;
        }
    });
    std::thread downloader([&] {
        StreamOptions after = none;
        after.waits = { &seeded, 1 };
        while (!go.load()) {
            std::this_thread::yield();
        }
        for (uint32_t k = 0; k < kRounds; ++k) {
            std::memset(dsts_[k].get(), 0, kPiece);
            const GpuStatus s = t->download(downBuf, 0, { dsts_[k].get(), kPiece }, downs[k], after);
            EXPECT_TRUE(s.ok()) << s.message;
        }
    });
    go.store(true);
    uploader.join();
    downloader.join();

    ASSERT_TRUE(ups[0] && downs[0]);
    EXPECT_NE(ups[0].semaphore, downs[0].semaphore) << "uploads and downloads share a timeline";
    for (uint32_t k = 1; k < kRounds; ++k) {
        EXPECT_EQ(ups[k].semaphore, ups[0].semaphore);
        EXPECT_EQ(downs[k].semaphore, downs[0].semaphore);
        EXPECT_GT(ups[k].value, ups[k - 1].value);
        EXPECT_GT(downs[k].value, downs[k - 1].value);
    }
    ASSERT_TRUE(t->wait(ups.back()).ok());
    ASSERT_TRUE(t->wait(downs.back()).ok());
    for (const TimelineValue& v : { ups.back(), downs.back() }) {
        uint64_t reached = 0;
        ASSERT_EQ(vkGetSemaphoreCounterValue(dev_->device(), v.semaphore, &reached), VK_SUCCESS);
        EXPECT_GE(reached, v.value);
    }
    for (uint32_t k = 0; k < kRounds; ++k) {
        ASSERT_TRUE(t->wait(downs[k]).ok());
        expectSame(srcs_[5].get(), dsts_[k].get(), kPiece, k);
    }

    StreamOptions back = none;
    back.waits = ups;
    for (uint32_t k = 0; k < kRounds; ++k) {
        std::memset(dsts_[k].get(), 0, kPiece);
        TimelineValue done;
        ASSERT_TRUE(t->download(upBuf, k * kPiece, { dsts_[k].get(), kPiece }, done, back).ok());
        ASSERT_TRUE(t->wait(done).ok());
        expectSame(srcs_[k % 5].get() + k * kPiece, dsts_[k].get(), kPiece, k);
    }
}

// The fill kernel overwrites a prefix of the uploaded buffer, so the downloaded prefix shows the
// upload landed before compute and the download ran after it; the tail shows the upload itself.
GPU_TEST_F(GpuTransferTest, UploadComputeDownloadWithoutHostWait)
{
    constexpr uint32_t w = 512;
    constexpr uint32_t h = 256;
    constexpr VkDeviceSize filled = VkDeviceSize(w) * h * sizeof(uint32_t);
    constexpr VkDeviceSize total = 3 * filled;

    GpuKernelDesc d;
    d.spirv = fill_spirv;
    d.spirvWords = fill_spirv_words;
    d.storageBufferCount = 1;
    d.pushConstantBytes = sizeof(FillParams);
    d.groupSize = { fill_group_size[0], fill_group_size[1], fill_group_size[2] };
    std::unique_ptr<GpuKernel> kernel;
    GpuStatus s = GpuKernel::create(*dev_, d, kernel);
    ASSERT_TRUE(s.ok()) << s.message;

    VkDevice vk = dev_->device();
    VkCommandPoolCreateInfo pci { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = dev_->info().computeFamily;
    VkCommandPool pool = VK_NULL_HANDLE;
    ASSERT_EQ(vkCreateCommandPool(vk, &pci, nullptr, &pool), VK_SUCCESS);
    VkCommandBufferAllocateInfo ai { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    ASSERT_EQ(vkAllocateCommandBuffers(vk, &ai, &cb), VK_SUCCESS);
    VkSemaphore sem = VK_NULL_HANDLE;
    ASSERT_TRUE(dev_->createTimelineSemaphore(0, sem).ok());
    uint64_t semValue = 0;

    const uint8_t* src = srcs_[3].get();
    uint8_t* dst = dsts_[3].get();
    for (bool import : { false, true }) {
        for (ComputeHandoff handoff : { ComputeHandoff::Submit, ComputeHandoff::Record }) {
            const char* label = handoff == ComputeHandoff::Submit ? "submit" : "record";
            SCOPED_TRACE(std::string(import ? "host-import " : "staging ") + label);
            GpuTransferOptions o;
            o.allowHostImport = import;
            auto t = make(o);
            ASSERT_TRUE(t != nullptr);
            GpuBuffer buf;
            ASSERT_TRUE(GpuBuffer::createDeviceLocal(*dev_, total, kGpuBufferDefaultUsage, buf).ok());
            std::memset(dst, 0, total);

            StreamOptions up;
            up.handoff = handoff;
            TimelineValue uploaded;
            s = t->upload({ const_cast<uint8_t*>(src), total }, buf, 0, uploaded, up);
            ASSERT_TRUE(s.ok()) << s.message;

            ASSERT_EQ(vkResetCommandBuffer(cb, 0), VK_SUCCESS);
            VkCommandBufferBeginInfo bi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            ASSERT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
            if (handoff == ComputeHandoff::Record) {
                t->recordComputeAcquire(cb, buf, 0, total);
            }
            const FillParams p { w, h, 0 };
            const VkBuffer bufs[] = { buf.buffer() };
            ASSERT_TRUE(kernel->record(cb, bufs, std::as_bytes(std::span(&p, 1)), { w, h, 1 }).ok());
            if (handoff == ComputeHandoff::Record) {
                t->recordComputeRelease(cb, buf, 0, total);
            }
            ASSERT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);

            const VkSemaphoreSubmitInfo wait = uploaded.waitInfo();
            const TimelineValue computed { sem, ++semValue };
            VkSemaphoreSubmitInfo signal = computed.waitInfo();
            VkCommandBufferSubmitInfo cbi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
            cbi.commandBuffer = cb;
            VkSubmitInfo2 si { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
            si.waitSemaphoreInfoCount = 1;
            si.pWaitSemaphoreInfos = &wait;
            si.commandBufferInfoCount = 1;
            si.pCommandBufferInfos = &cbi;
            si.signalSemaphoreInfoCount = 1;
            si.pSignalSemaphoreInfos = &signal;
            s = dev_->submit(QueueKind::Compute, std::span<const VkSubmitInfo2>(&si, 1), VK_NULL_HANDLE);
            ASSERT_TRUE(s.ok()) << s.message;

            StreamOptions down;
            down.handoff = handoff;
            down.waits = { &computed, 1 };
            TimelineValue downloaded;
            s = t->download(buf, 0, { dst, total }, downloaded, down);
            ASSERT_TRUE(s.ok()) << s.message;
            s = t->wait(downloaded);
            ASSERT_TRUE(s.ok()) << s.message;
            kernel->releaseDescriptors();

            const uint32_t* words = reinterpret_cast<const uint32_t*>(dst);
            size_t wrong = 0;
            for (uint32_t i = 0; i < w * h; ++i) {
                wrong += words[i] != i;
            }
            EXPECT_EQ(wrong, 0u);
            expectSame(src + filled, dst + filled, total - filled, 3);
        }
    }

    vkDestroySemaphore(vk, sem, nullptr);
    vkDestroyCommandPool(vk, pool, nullptr);
}

GPU_TEST_F(GpuTransferTest, ForgottenHostAllocationIsReimported)
{
    GpuTransferOptions o;
    o.cacheHostImports = true;
    auto t = make(o);
    ASSERT_TRUE(t != nullptr);
    if (!t->hostImportSupported()) {
        GPUTEST_SKIP() << "VK_EXT_external_memory_host unavailable";
    }
    constexpr size_t kBytes = size_t(2) << 20;
    ASSERT_EQ(kBytes % t->hostImportAlignment(), 0u);
    void* p = mmap(nullptr, kBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(p, MAP_FAILED);
    if (reinterpret_cast<uintptr_t>(p) % t->hostImportAlignment() != 0) {
        munmap(p, kBytes);
        GPUTEST_SKIP() << "import alignment exceeds the page size";
    }
    GpuBuffer buf;
    ASSERT_TRUE(GpuBuffer::createDeviceLocal(*dev_, kBytes, kGpuBufferDefaultUsage, buf).ok());
    uint8_t* dst = dsts_[4].get();

    auto roundTrip = [&](uint32_t seed) {
        fillFrame(static_cast<uint8_t*>(p), kBytes, seed);
        std::memset(dst, 0, kBytes);
        TransferPath path = TransferPath::Staging;
        StreamOptions up;
        up.handoff = ComputeHandoff::None;
        up.path = &path;
        up.cacheImport = true;
        TimelineValue uploaded;
        GpuStatus s = t->upload({ p, kBytes }, buf, 0, uploaded, up);
        EXPECT_TRUE(s.ok()) << s.message;
        EXPECT_EQ(path, TransferPath::HostImport);
        StreamOptions down;
        down.handoff = ComputeHandoff::None;
        down.waits = { &uploaded, 1 };
        down.cacheImport = true;
        TimelineValue downloaded;
        s = t->download(buf, 0, { dst, kBytes }, downloaded, down);
        EXPECT_TRUE(s.ok()) << s.message;
        EXPECT_TRUE(t->wait(downloaded).ok());
        expectSame(p, dst, kBytes, seed);
    };

    roundTrip(11);
    const uint64_t imports = t->hostImportCount();
    roundTrip(12);
    EXPECT_EQ(t->hostImportCount(), imports) << "a cached range was imported again";

    t->forgetHostAllocation(p, kBytes);
    // MAP_FIXED over the old mapping puts fresh pages at the same address, as a free and reallocation can.
    void* q = mmap(p, kBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    ASSERT_EQ(q, p);
    roundTrip(13);
    EXPECT_EQ(t->hostImportCount(), imports + 1) << "the reallocated range was not imported again";

    t->forgetHostAllocation(p, kBytes);
    munmap(p, kBytes);
}

namespace {

// Anonymous pages at an address aligned for import, or null when the import alignment exceeds the page size.
void*
mapImportable(size_t bytes, VkDeviceSize alignment)
{
    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        return nullptr;
    }
    if (reinterpret_cast<uintptr_t>(p) % alignment != 0 || bytes % alignment != 0) {
        munmap(p, bytes);
        return nullptr;
    }
    return p;
}

} // namespace

GPU_TEST_F(GpuTransferTest, ConcurrentImportsOfOneRangeCacheOne)
{
    GpuTransferOptions o;
    o.cacheHostImports = true;
    auto t = make(o);
    ASSERT_TRUE(t != nullptr);
    if (!t->hostImportSupported()) {
        GPUTEST_SKIP() << "VK_EXT_external_memory_host unavailable";
    }
    constexpr size_t kBytes = size_t(8) << 20;
    void* p = mapImportable(kBytes, t->hostImportAlignment());
    if (!p) {
        GPUTEST_SKIP() << "import alignment exceeds the page size";
    }

    constexpr int kThreads = 8;
    const HostImportStats before = t->hostImportStats();
    std::atomic<int> ready { 0 };
    std::atomic<int> cached { 0 };
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&] {
            ++ready;
            while (ready.load() < kThreads) {
                std::this_thread::yield();
            }
            if (t->prepareImport({ p, kBytes })) {
                ++cached;
            }
        });
    }
    for (std::thread& th : threads) {
        th.join();
    }
    const HostImportStats after = t->hostImportStats();
    EXPECT_EQ(cached.load(), kThreads);
    EXPECT_EQ(after.cached - before.cached, 1u);
    EXPECT_GE(after.created - before.created, 1u);
    EXPECT_EQ((after.created - before.created) - (after.dropped - before.dropped), 1u)
        << "every import but the cached one must have been dropped";
    EXPECT_EQ(after.prepared - before.prepared, after.created - before.created);

    GpuBuffer buf;
    ASSERT_TRUE(GpuBuffer::createDeviceLocal(*dev_, kBytes, kGpuBufferDefaultUsage, buf).ok());
    StreamOptions up;
    up.handoff = ComputeHandoff::None;
    TimelineValue uploaded;
    ASSERT_TRUE(t->upload({ srcs_[0].get(), kBytes }, buf, 0, uploaded, up).ok());
    TransferPath path = TransferPath::Staging;
    StreamOptions down;
    down.handoff = ComputeHandoff::None;
    down.waits = { &uploaded, 1 };
    down.path = &path;
    TimelineValue downloaded;
    const HostImportStats beforeDownload = t->hostImportStats();
    ASSERT_TRUE(t->download(buf, 0, { p, kBytes }, downloaded, down).ok());
    ASSERT_TRUE(t->wait(downloaded).ok());
    EXPECT_EQ(path, TransferPath::HostImport);
    EXPECT_EQ(t->hostImportStats().atDownload, beforeDownload.atDownload) << "the prepared range was imported again";
    expectSame(srcs_[0].get(), p, kBytes, 1);

    t->forgetHostAllocation(p, kBytes);
    munmap(p, kBytes);
}

GPU_TEST_F(GpuTransferTest, ForgetDuringPrepareDropsTheImport)
{
    std::mutex mutex;
    std::condition_variable cv;
    bool created = false;
    bool release = false;
    GpuTransferOptions o;
    o.cacheHostImports = true;
    o.onImportCreated = [&](const void*, VkDeviceSize) {
        std::unique_lock<std::mutex> lock(mutex);
        created = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release; });
    };
    auto t = make(o);
    ASSERT_TRUE(t != nullptr);
    if (!t->hostImportSupported()) {
        GPUTEST_SKIP() << "VK_EXT_external_memory_host unavailable";
    }
    constexpr size_t kBytes = size_t(2) << 20;
    void* p = mapImportable(kBytes, t->hostImportAlignment());
    if (!p) {
        GPUTEST_SKIP() << "import alignment exceeds the page size";
    }

    const HostImportStats before = t->hostImportStats();
    bool prepared = true;
    std::thread preparer([&] { prepared = t->prepareImport({ p, kBytes }); });
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return created; });
    }
    // forget() blocks until the import it marks is destroyed, so the import is held open from another thread, for
    // far longer than forget() takes to mark it.
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
        cv.notify_all();
    });
    t->forgetHostAllocation(p, kBytes);
    const HostImportStats forgotten = t->hostImportStats();
    preparer.join();
    releaser.join();

    EXPECT_FALSE(prepared) << "an import of a forgotten range was cached";
    EXPECT_EQ(forgotten.cached, before.cached);
    EXPECT_EQ(forgotten.dropped - before.dropped, 1u) << "forget() returned before the import was destroyed";
    EXPECT_EQ(t->hostImportStats().cached, before.cached);

    EXPECT_TRUE(t->prepareImport({ p, kBytes })) << "the range could not be imported again";
    EXPECT_EQ(t->hostImportStats().cached, before.cached + 1);

    t->forgetHostAllocation(p, kBytes);
    EXPECT_EQ(t->hostImportStats().cached, before.cached);
    munmap(p, kBytes);
}

GPU_TEST_F(GpuTransferTest, UnalignedPartOfACachedImportCreatesNoImport)
{
    GpuTransferOptions o;
    o.cacheHostImports = true;
    auto t = make(o);
    ASSERT_TRUE(t != nullptr);
    if (!t->hostImportSupported()) {
        GPUTEST_SKIP() << "VK_EXT_external_memory_host unavailable";
    }
    constexpr size_t kBytes = size_t(4) << 20;
    void* p = mapImportable(kBytes, t->hostImportAlignment());
    if (!p) {
        GPUTEST_SKIP() << "import alignment exceeds the page size";
    }
    uint8_t* host = static_cast<uint8_t*>(p);
    fillFrame(host, kBytes, 21);
    ASSERT_TRUE(t->prepareImport({ p, kBytes }));

    // A strip of rows whose width is no multiple of the alignment: neither its start nor its size is aligned.
    constexpr VkDeviceSize kOffset = (VkDeviceSize(1) << 20) + 48;
    constexpr VkDeviceSize kPart = (VkDeviceSize(2) << 20) + 1000;
    GpuBuffer buf;
    ASSERT_TRUE(GpuBuffer::createDeviceLocal(*dev_, kPart, kGpuBufferDefaultUsage, buf).ok());
    const HostImportStats before = t->hostImportStats();

    TransferPath upPath = TransferPath::Staging;
    StreamOptions up;
    up.handoff = ComputeHandoff::None;
    up.path = &upPath;
    TimelineValue uploaded;
    ASSERT_TRUE(t->upload({ host + kOffset, kPart }, buf, 0, uploaded, up).ok());
    EXPECT_EQ(upPath, TransferPath::HostImport);

    // Downloaded back into another unaligned part of the same import.
    constexpr VkDeviceSize kBack = 16;
    std::vector<uint8_t> expected(host + kOffset, host + kOffset + kPart);
    TransferPath downPath = TransferPath::Staging;
    StreamOptions down;
    down.handoff = ComputeHandoff::None;
    down.waits = { &uploaded, 1 };
    down.path = &downPath;
    TimelineValue downloaded;
    ASSERT_TRUE(t->download(buf, 0, { host + kBack, kPart }, downloaded, down).ok());
    ASSERT_TRUE(t->wait(downloaded).ok());
    EXPECT_EQ(downPath, TransferPath::HostImport);
    expectSame(expected.data(), host + kBack, kPart, 1);

    const HostImportStats after = t->hostImportStats();
    EXPECT_EQ(after.created, before.created) << "a part of a cached import was imported again";
    EXPECT_EQ(after.atUpload, before.atUpload);
    EXPECT_EQ(after.atDownload, before.atDownload);
    EXPECT_EQ(after.cached, before.cached);

    // Forgotten, the same unaligned range goes through staging again.
    t->forgetHostAllocation(p, kBytes);
    TimelineValue staged;
    ASSERT_TRUE(t->upload({ host + kOffset, kPart }, buf, 0, staged, up).ok());
    ASSERT_TRUE(t->wait(staged).ok());
    EXPECT_EQ(upPath, TransferPath::Staging);
    munmap(p, kBytes);
}

GPU_TEST_F(GpuTransferTest, StripDownloadProceedsOnceItsChunkIsPrepared)
{
    constexpr size_t kChunk = size_t(2) << 20;
    std::mutex mutex;
    std::condition_variable cv;
    const void* held = nullptr;
    bool reached = false;
    bool release = false;
    GpuRuntimeOptions o;
    o.device = gputest::deviceOptions();
    o.transfer.cacheHostImports = true;
    o.transfer.onImportCreated = [&](const void* host, VkDeviceSize) {
        std::unique_lock<std::mutex> lock(mutex);
        if (host != held) {
            return;
        }
        reached = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release; });
    };
    std::unique_ptr<GpuRuntime> rt;
    const GpuStatus created = GpuRuntime::create(o, rt);
    ASSERT_TRUE(created.ok()) << created.message;
    // Declared after the runtime, so that a failed assertion lets the helper thread go before the runtime joins it.
    struct Releaser {
        std::mutex& mutex;
        std::condition_variable& cv;
        bool& release;
        ~Releaser()
        {
            std::lock_guard<std::mutex> lock(mutex);
            release = true;
            cv.notify_all();
        }
    } releaser { mutex, cv, release };
    GpuTransfer& t = rt->transfer();
    if (!t.hostImportSupported()) {
        GPUTEST_SKIP() << "VK_EXT_external_memory_host unavailable";
    }
    void* p = mapImportable(3 * kChunk, t.hostImportAlignment());
    if (!p) {
        GPUTEST_SKIP() << "import alignment exceeds the page size";
    }
    uint8_t* host = static_cast<uint8_t*>(p);
    {
        std::lock_guard<std::mutex> lock(mutex);
        held = host + kChunk;
    }

    const HostImportStats before = t.hostImportStats();
    std::shared_ptr<GpuImportJob> job = rt->prepareImports({ { host, kChunk }, { host + kChunk, kChunk }, { host + 2 * kChunk, kChunk } });
    ASSERT_TRUE(job != nullptr);
    job->waitFor(1);
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return reached; });
    }
    EXPECT_FALSE(job->finished());

    // A strip's rows inside the first chunk, at no aligned offset or size.
    constexpr VkDeviceSize kOffset = 48;
    constexpr VkDeviceSize kPart = (VkDeviceSize(1) << 20) + 1000;
    GpuBuffer buf;
    ASSERT_TRUE(GpuBuffer::createDeviceLocal(rt->device(), kPart, kGpuBufferDefaultUsage, buf).ok());
    StreamOptions up;
    up.handoff = ComputeHandoff::None;
    TimelineValue uploaded;
    ASSERT_TRUE(rt->upload({ srcs_[0].get(), kPart }, buf, 0, uploaded, up).ok());
    TransferPath path = TransferPath::Staging;
    StreamOptions down;
    down.handoff = ComputeHandoff::None;
    down.waits = { &uploaded, 1 };
    down.path = &path;
    TimelineValue downloaded;
    ASSERT_TRUE(rt->download(buf, 0, { host + kOffset, kPart }, downloaded, down).ok());
    ASSERT_TRUE(rt->wait(downloaded).ok());
    EXPECT_EQ(path, TransferPath::HostImport) << "the strip did not copy into its prepared chunk";
    EXPECT_EQ(t.hostImportStats().atDownload, before.atDownload) << "the strip imported its rows itself";
    EXPECT_FALSE(job->finished()) << "the later chunks were prepared before the strip landed";
    expectSame(srcs_[0].get(), host + kOffset, kPart, 1);

    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
        cv.notify_all();
    }
    job->wait();
    EXPECT_EQ(t.hostImportStats().cached - before.cached, 3u) << "each chunk is its own import";

    t.forgetHostAllocation(p, 3 * kChunk);
    munmap(p, 3 * kChunk);
}

GPU_TEST_F(GpuTransferTest, StagedUploadsCountTheirReason)
{
    constexpr VkDeviceSize kBytes = VkDeviceSize(1) << 20;
    GpuBuffer buf;
    ASSERT_TRUE(GpuBuffer::createDeviceLocal(*dev_, kBytes, kGpuBufferDefaultUsage, buf).ok());
    StreamOptions so;
    so.handoff = ComputeHandoff::None;

    GpuTransferOptions off;
    off.allowHostImport = false;
    auto disabled = make(off);
    ASSERT_TRUE(disabled != nullptr);
    TimelineValue done;
    ASSERT_TRUE(disabled->upload({ srcs_[0].get(), kBytes }, buf, 0, done, so).ok());
    ASSERT_TRUE(disabled->wait(done).ok());
    StagedReasonCounts c = disabled->stagedReasons();
    EXPECT_EQ(c.disabled, 1u);
    EXPECT_EQ(c.total(), 1u);

    auto t = make({});
    ASSERT_TRUE(t != nullptr);
    if (!t->hostImportSupported()) {
        GPUTEST_SKIP() << "VK_EXT_external_memory_host unavailable";
    }
    ASSERT_TRUE(t->upload({ srcs_[0].get() + 16, kBytes - 4096 }, buf, 0, done, so).ok());
    ASSERT_TRUE(t->wait(done).ok());
    c = t->stagedReasons();
    EXPECT_EQ(c.unaligned, 1u);
    EXPECT_EQ(c.total(), 1u);

    TransferPath path = TransferPath::Staging;
    so.path = &path;
    ASSERT_TRUE(t->upload({ srcs_[0].get(), kBytes }, buf, 0, done, so).ok());
    ASSERT_TRUE(t->wait(done).ok());
    EXPECT_EQ(path, TransferPath::HostImport);
    EXPECT_EQ(t->stagedReasons().total(), 1u) << "an imported upload was counted as staged";

    EXPECT_FALSE(t->prepareImport({ srcs_[0].get() + 16, kBytes - 4096 }));
    EXPECT_EQ(t->stagedReasons().total(), 1u) << "prepareImport() counted as a staged transfer";
}

GPU_TEST_F(GpuTransferTest, UploadWhoseImportFailsCountsImportFailed)
{
    constexpr VkDeviceSize kBytes = VkDeviceSize(1) << 20;
    GpuTransferOptions o;
    o.failHostImportsForTests = true;
    auto t = make(o);
    ASSERT_TRUE(t != nullptr);
    if (!t->hostImportSupported()) {
        GPUTEST_SKIP() << "VK_EXT_external_memory_host unavailable";
    }
    GpuBuffer buf;
    ASSERT_TRUE(GpuBuffer::createDeviceLocal(*dev_, kBytes, kGpuBufferDefaultUsage, buf).ok());
    StreamOptions so;
    so.handoff = ComputeHandoff::None;
    TransferPath path = TransferPath::HostImport;
    so.path = &path;
    TimelineValue done;
    ASSERT_TRUE(t->upload({ srcs_[0].get(), kBytes }, buf, 0, done, so).ok());
    ASSERT_TRUE(t->wait(done).ok());
    EXPECT_EQ(path, TransferPath::Staging);
    const StagedReasonCounts c = t->stagedReasons();
    EXPECT_EQ(c.importFailed, 1u);
    EXPECT_EQ(c.total(), 1u);
}

GPU_TEST_F(GpuTransferTest, UploadOfAForgottenRangeCountsForgotten)
{
    std::mutex mutex;
    std::condition_variable cv;
    bool created = false;
    bool release = false;
    GpuTransferOptions o;
    o.cacheHostImports = true;
    o.onImportCreated = [&](const void*, VkDeviceSize) {
        std::unique_lock<std::mutex> lock(mutex);
        created = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release; });
    };
    auto t = make(o);
    ASSERT_TRUE(t != nullptr);
    if (!t->hostImportSupported()) {
        GPUTEST_SKIP() << "VK_EXT_external_memory_host unavailable";
    }
    constexpr size_t kBytes = size_t(2) << 20;
    void* p = mapImportable(kBytes, t->hostImportAlignment());
    if (!p) {
        GPUTEST_SKIP() << "import alignment exceeds the page size";
    }
    GpuBuffer buf;
    ASSERT_TRUE(GpuBuffer::createDeviceLocal(*dev_, kBytes, kGpuBufferDefaultUsage, buf).ok());

    StreamOptions so;
    so.handoff = ComputeHandoff::None;
    so.cacheImport = true;
    TransferPath path = TransferPath::HostImport;
    so.path = &path;
    GpuStatus status;
    TimelineValue done;
    std::thread uploader([&] { status = t->upload({ p, kBytes }, buf, 0, done, so); });
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return created; });
    }
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
        cv.notify_all();
    });
    t->forgetHostAllocation(p, kBytes);
    uploader.join();
    releaser.join();

    ASSERT_TRUE(status.ok()) << status.message;
    ASSERT_TRUE(t->wait(done).ok());
    EXPECT_EQ(path, TransferPath::Staging);
    const StagedReasonCounts c = t->stagedReasons();
    EXPECT_EQ(c.forgotten, 1u);
    EXPECT_EQ(c.total(), 1u);
    munmap(p, kBytes);
}

GPU_TEST_F(GpuTransferTest, UploadWithoutCacheImportLeavesNoCachedImport)
{
    GpuTransferOptions o;
    o.cacheHostImports = true;
    auto t = make(o);
    ASSERT_TRUE(t != nullptr);
    if (!t->hostImportSupported()) {
        GPUTEST_SKIP() << "VK_EXT_external_memory_host unavailable";
    }
    constexpr size_t kBytes = size_t(2) << 20;
    void* p = mapImportable(kBytes, t->hostImportAlignment());
    if (!p) {
        GPUTEST_SKIP() << "import alignment exceeds the page size";
    }
    GpuBuffer buf;
    ASSERT_TRUE(GpuBuffer::createDeviceLocal(*dev_, kBytes, kGpuBufferDefaultUsage, buf).ok());
    uint8_t* dst = dsts_[4].get();

    // Memory nobody calls forgetHostAllocation() for: the address is reused below without one.
    auto roundTrip = [&](uint32_t seed) {
        fillFrame(static_cast<uint8_t*>(p), kBytes, seed);
        std::memset(dst, 0, kBytes);
        TransferPath path = TransferPath::Staging;
        StreamOptions up;
        up.handoff = ComputeHandoff::None;
        up.path = &path;
        TimelineValue uploaded;
        GpuStatus s = t->upload({ p, kBytes }, buf, 0, uploaded, up);
        EXPECT_TRUE(s.ok()) << s.message;
        EXPECT_EQ(path, TransferPath::HostImport);
        StreamOptions down;
        down.handoff = ComputeHandoff::None;
        down.waits = { &uploaded, 1 };
        TimelineValue downloaded;
        s = t->download(buf, 0, { dst, kBytes }, downloaded, down);
        EXPECT_TRUE(s.ok()) << s.message;
        EXPECT_TRUE(t->wait(downloaded).ok());
        expectSame(p, dst, kBytes, seed);
    };

    const HostImportStats before = t->hostImportStats();
    roundTrip(31);
    EXPECT_EQ(t->hostImportStats().cached, before.cached) << "an upload without cacheImport cached its import";

    void* q = mmap(p, kBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    ASSERT_EQ(q, p);
    roundTrip(32);
    EXPECT_EQ(t->hostImportStats().cached, before.cached);
    munmap(p, kBytes);
}
