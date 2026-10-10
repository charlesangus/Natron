#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "GpuDevice.h"
#include "GpuTransfer.h"

using namespace gpu;

namespace {

constexpr size_t kWidth = 3840;
constexpr size_t kHeight = 2160;
constexpr size_t kUhdBytes = kWidth * kHeight * 4 * sizeof(float);
constexpr uint32_t kPipelinedFrames = 16;
constexpr size_t kAlign = 4096;

struct FreeDeleter
{
    void operator()(void* p) const { std::free(p); }
};
using HostBuf = std::unique_ptr<uint8_t, FreeDeleter>;

HostBuf alignedBuf(size_t bytes)
{
    const size_t rounded = (bytes + kAlign - 1) / kAlign * kAlign;
    return HostBuf(static_cast<uint8_t*>(std::aligned_alloc(kAlign, rounded)));
}

// Distinct per frame and per word so a misplaced strip or a stale frame cannot compare equal.
void fillFrame(uint8_t* p, size_t bytes, uint32_t seed)
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
    for (std::thread& t : ts) t.join();
    for (size_t i = words * 4; i < bytes; ++i) p[i] = static_cast<uint8_t>(i + seed);
}

const char* pathName(TransferPath p)
{
    return p == TransferPath::HostImport ? "host-import" : "staging";
}

class GpuTransferTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        GpuDeviceOptions o;
        o.enableValidation = std::getenv("GPU_SPIKE_NO_VALIDATION") == nullptr;
        GpuStatus s = GpuDevice::create(o, dev_);
        if (!s) {
            setupError_ = s.message;
            return;
        }
        std::printf("device: %s transferUsesComputeQueue=%d extMemHost=%d validation=%d\n", dev_->info().name.c_str(),
                    dev_->info().transferUsesComputeQueue, dev_->info().externalMemoryHost,
                    dev_->info().validationEnabled);
        for (uint32_t k = 0; k < kPipelinedFrames; ++k) {
            srcs_.push_back(alignedBuf(kUhdBytes));
            dsts_.push_back(alignedBuf(kUhdBytes));
            fillFrame(srcs_.back().get(), kUhdBytes, k + 1);
        }
    }

    static void TearDownTestSuite()
    {
        srcs_.clear();
        dsts_.clear();
        dev_.reset();
    }

    void SetUp() override { ASSERT_TRUE(dev_) << setupError_; }

    static bool isCpuDevice() { return dev_->info().type == VK_PHYSICAL_DEVICE_TYPE_CPU; }

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
};

std::unique_ptr<GpuDevice> GpuTransferTest::dev_;
std::string GpuTransferTest::setupError_;
std::vector<HostBuf> GpuTransferTest::srcs_;
std::vector<HostBuf> GpuTransferTest::dsts_;

void expectSame(const void* a, const void* b, size_t bytes, uint32_t frame)
{
    if (std::memcmp(a, b, bytes) == 0) {
        return;
    }
    const uint8_t* x = static_cast<const uint8_t*>(a);
    const uint8_t* y = static_cast<const uint8_t*>(b);
    size_t i = 0;
    while (x[i] == y[i]) ++i;
    ADD_FAILURE() << "frame " << frame << " differs first at byte " << i << " of " << bytes;
}

} // namespace

TEST_F(GpuTransferTest, StagingRoundTripUhd)
{
    GpuTransferOptions o;
    o.allowHostImport = false;
    auto t = make(o);
    ASSERT_TRUE(t);
    std::memset(dsts_[0].get(), 0, kUhdBytes);
    TransferFrame f{srcs_[0].get(), dsts_[0].get(), kUhdBytes};
    TransferTimeline tl;
    GpuStatus s = t->process({&f, 1}, nullptr, &tl);
    ASSERT_TRUE(s.ok()) << s.message;
    ASSERT_EQ(tl.frames.size(), 1u);
    EXPECT_EQ(tl.frames[0].uploadPath, TransferPath::Staging);
    EXPECT_EQ(tl.frames[0].downloadPath, TransferPath::Staging);
    EXPECT_GT(tl.frames[0].strips, 1u);
    expectSame(srcs_[0].get(), dsts_[0].get(), kUhdBytes, 0);
}

TEST_F(GpuTransferTest, HostImportRoundTripUhd)
{
    GpuTransferOptions o;
    auto t = make(o);
    ASSERT_TRUE(t);
    if (!t->hostImportSupported()) {
        GTEST_SKIP() << "VK_EXT_external_memory_host unavailable";
    }
    ASSERT_EQ(kAlign % t->hostImportAlignment(), 0u);
    std::memset(dsts_[1].get(), 0, kUhdBytes);
    TransferFrame f{srcs_[1].get(), dsts_[1].get(), kUhdBytes};
    TransferTimeline tl;
    GpuStatus s = t->process({&f, 1}, nullptr, &tl);
    ASSERT_TRUE(s.ok()) << s.message;
    EXPECT_EQ(tl.frames[0].uploadPath, TransferPath::HostImport);
    EXPECT_EQ(tl.frames[0].downloadPath, TransferPath::HostImport);
    expectSame(srcs_[1].get(), dsts_[1].get(), kUhdBytes, 1);
}

TEST_F(GpuTransferTest, MisalignedPointerFallsBackToStaging)
{
    auto t = make({});
    ASSERT_TRUE(t);
    const size_t bytes = kUhdBytes - 4096;
    std::memset(dsts_[2].get(), 0, kUhdBytes);
    TransferFrame f{srcs_[2].get() + 16, dsts_[2].get(), bytes};
    TransferTimeline tl;
    GpuStatus s = t->process({&f, 1}, nullptr, &tl);
    ASSERT_TRUE(s.ok()) << s.message;
    EXPECT_EQ(tl.frames[0].uploadPath, TransferPath::Staging);
    if (t->hostImportSupported()) {
        EXPECT_EQ(tl.frames[0].downloadPath, TransferPath::HostImport);
    }
    expectSame(srcs_[2].get() + 16, dsts_[2].get(), bytes, 2);
}

TEST_F(GpuTransferTest, OddSizesAndUserComputeCallback)
{
    GpuTransferOptions o;
    o.stripBytes = 1 << 20;
    o.allowHostImport = false;
    auto t = make(o);
    ASSERT_TRUE(t);
    const std::vector<size_t> sizes = {4, (5u << 20) + 12, 1u << 20, 3 * 1000 * 1000 + 8};
    std::vector<TransferFrame> frames;
    for (size_t i = 0; i < sizes.size(); ++i) {
        std::memset(dsts_[i].get(), 0, sizes[i]);
        frames.push_back({srcs_[i].get(), dsts_[i].get(), sizes[i]});
    }
    uint32_t calls = 0;
    auto copy = [&](VkCommandBuffer cb, const ComputeBinding& b) {
        EXPECT_EQ(b.bytes, sizes[b.frameIndex]);
        VkBufferCopy r{0, 0, b.bytes};
        vkCmdCopyBuffer(cb, b.input, b.output, 1, &r);
        ++calls;
    };
    GpuStatus s = t->process(frames, copy, nullptr);
    ASSERT_TRUE(s.ok()) << s.message;
    EXPECT_EQ(calls, sizes.size());
    for (size_t i = 0; i < sizes.size(); ++i) {
        expectSame(srcs_[i].get(), dsts_[i].get(), sizes[i], static_cast<uint32_t>(i));
    }
}

namespace {

struct RunResult
{
    std::string label;
    TransferTimeline tl;
};

void printTimeline(const RunResult& r)
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

TEST_F(GpuTransferTest, Pipelined16FramesOverlap)
{
    const uint32_t many = std::max(2u, std::min(8u, std::thread::hardware_concurrency()));
    struct Config
    {
        std::string label;
        bool import;
        uint32_t threads;
        bool serial;
        bool cached = false;
    };
    const std::vector<Config> configs = {
        {"staging, 1 copy thread, serial", false, 1, true},
        {"staging, 1 copy thread", false, 1, false},
        {"staging, " + std::to_string(many) + " copy threads, serial", false, many, true},
        {"staging, " + std::to_string(many) + " copy threads", false, many, false},
        {"host-import, serial", true, 1, true},
        {"host-import", true, 1, false},
        {"host-import cached, serial", true, 1, true, true},
        {"host-import cached", true, 1, false, true},
    };

    std::vector<TransferFrame> frames;
    for (uint32_t k = 0; k < kPipelinedFrames; ++k) {
        frames.push_back({srcs_[k].get(), dsts_[k].get(), kUhdBytes});
    }

    std::vector<RunResult> results;
    for (const Config& c : configs) {
        GpuTransferOptions o;
        o.allowHostImport = c.import;
        o.copyThreads = c.threads;
        o.cacheHostImports = c.cached;
        auto t = make(o);
        ASSERT_TRUE(t);
        if (c.import && !t->hostImportSupported()) {
            std::printf("\n[%s] skipped: no VK_EXT_external_memory_host\n", c.label.c_str());
            continue;
        }
        for (uint32_t k = 0; k < kPipelinedFrames; ++k) {
            std::memset(dsts_[k].get(), 0, kUhdBytes);
        }
        // Warm-up pays first-touch page faults, and with cached imports the pinning, outside the measurement.
        ASSERT_TRUE(t->process({frames.data(), c.cached ? frames.size() : 1}, nullptr, nullptr).ok());

        RunResult r{c.label, {}};
        if (c.serial) {
            for (uint32_t k = 0; k < kPipelinedFrames; ++k) {
                TransferTimeline one;
                GpuStatus s = t->process({&frames[k], 1}, nullptr, &one);
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
        if (!c.serial && r.tl.gpuTimestamps && !isCpuDevice()) {
            EXPECT_LT(r.tl.wallMs, r.tl.sumOfStagesMs()) << c.label;
        }
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
