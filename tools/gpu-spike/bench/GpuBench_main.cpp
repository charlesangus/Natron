// GPU benchmark driver: Grade, Blur and Grade->Blur at UHD, 8K, 16k and 24k, RGBA float.
//
// The 16k and 24k frames go through as horizontal strips with blur halos, each strip one
// GpuTransfer frame. One CSV row per (size, workload, transfer path, overlap) holds medians over
// the iterations; the kernel columns come from a separate resident-data run timed with timestamps.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "GpuBlur.h"
#include "GpuDevice.h"
#include "GpuKernel.h"
#include "GpuTransfer.h"

#include "../tests/BlurFixture.h"
#include "../tests/GradeFixture.h"
#include "blur_spirv.h"
#include "grade_spirv.h"

using namespace gpu;

namespace {

constexpr uint32_t kChannels = 4;
constexpr size_t kAlign = 4096;
// Strip interiors are multiples of 4 rows so every strip pointer and size stays 4096-aligned for
// host import at all benchmarked widths (row bytes are multiples of 1024).
constexpr uint32_t kRowQuantum = 4;
uint32_t gStripRows = 2048;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

struct SizeSpec {
    std::string name;
    uint32_t w, h;
    bool tiled;
    // Frames per timed stream; tiled sizes stream the strips of one image instead.
    uint32_t images;
    bool single = false;
};

enum class Wl {
    Copy,
    Grade,
    Blur3,
    Blur25,
    Blur100,
    GradeBlur25,
};

struct WlInfo {
    const char* name;
    bool grade;
    double sigma;
    bool copyOnly;
};

constexpr WlInfo kWl[] = {
    { "Copy", false, 0.0, true },
    { "Grade", true, 0.0, false },
    { "Blur3", false, 3.0, false },
    { "Blur25", false, 25.0, false },
    { "Blur100", false, 100.0, false },
    { "GradeBlur25", true, 25.0, false },
};

const WlInfo&
info(Wl w)
{
    return kWl[static_cast<int>(w)];
}

uint32_t
roundUp(uint32_t v, uint32_t q)
{
    return (v + q - 1) / q * q;
}

uint32_t
haloRows(Wl w)
{
    const WlInfo& i = info(w);
    return i.sigma > 0.0 ? roundUp(uint32_t(blurref::radiusForSigma(i.sigma)), kRowQuantum) : 0;
}

struct Unit {
    uint32_t srcRow, srcRows, dstRow, dstRows;
};

std::vector<Unit>
makeUnits(const SizeSpec& s, Wl w)
{
    if (!s.tiled) {
        return { { 0, s.h, 0, s.h } };
    }
    const uint32_t halo = haloRows(w);
    std::vector<Unit> units;
    for (uint32_t y0 = 0; y0 < s.h; y0 += gStripRows) {
        const uint32_t y1 = std::min(s.h, y0 + gStripRows);
        const uint32_t s0 = y0 > halo ? y0 - halo : 0;
        const uint32_t s1 = std::min(s.h, y1 + halo);
        units.push_back({ s0, s1 - s0, y0, y1 - y0 });
    }
    return units;
}

struct FreeDeleter {
    void operator()(void* p) const { std::free(p); }
};
using HostBuf = std::unique_ptr<uint8_t, FreeDeleter>;

HostBuf
alignedBuf(size_t bytes)
{
    return HostBuf(static_cast<uint8_t*>(std::aligned_alloc(kAlign, (bytes + kAlign - 1) / kAlign * kAlign)));
}

template <class F>
void
parallelFor(size_t n, F&& fn)
{
    const unsigned nt = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
    std::vector<std::thread> ts;
    for (unsigned t = 0; t < nt; ++t) {
        ts.emplace_back([=, &fn] { fn(n * t / nt, n * (t + 1) / nt); });
    }
    for (std::thread& t : ts) {
        t.join();
    }
}

void
fillImage(uint8_t* p, size_t bytes, uint32_t seed)
{
    float* f = reinterpret_cast<float*>(p);
    parallelFor(bytes / 4, [&](size_t b, size_t e) {
        for (size_t i = b; i < e; ++i) {
            uint32_t x = static_cast<uint32_t>(i) * 2654435761u ^ (seed * 0x9E3779B9u);
            x ^= x >> 15;
            f[i] = static_cast<float>(x & 0xFFFFFF) * (1.0f / 16777216.0f);
        }
    });
}

void
touch(uint8_t* p, size_t bytes)
{
    parallelFor(bytes, [&](size_t b, size_t e) { std::memset(p + b, 0, e - b); });
}

double
median(std::vector<double> v)
{
    if (v.empty()) {
        return kNaN;
    }
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

double
loadAvg()
{
    double l = 0.0;
    if (FILE* f = std::fopen("/proc/loadavg", "r")) {
        if (std::fscanf(f, "%lf", &l) != 1) {
            l = kNaN;
        }
        std::fclose(f);
    }
    return l;
}

struct DevBuf {
    GpuDevice* dev = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;

    DevBuf() = default;
    DevBuf(DevBuf&& o) noexcept { *this = std::move(o); }
    DevBuf& operator=(DevBuf&& o) noexcept
    {
        reset();
        dev = o.dev;
        buffer = std::exchange(o.buffer, VK_NULL_HANDLE);
        alloc = std::exchange(o.alloc, nullptr);
        return *this;
    }
    ~DevBuf() { reset(); }
    void reset()
    {
        if (buffer) {
            vmaDestroyBuffer(dev->allocator(), buffer, alloc);
        }
        buffer = VK_NULL_HANDLE;
        alloc = nullptr;
    }
};

// Device-local storage, or host-visible and filled once when `data` is given.
bool
makeStorage(GpuDevice& dev, VkDeviceSize bytes, DevBuf& out, std::string& err, const void* data = nullptr)
{
    VkBufferCreateInfo bi { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = std::max<VkDeviceSize>(bytes, 4);
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ai {};
    ai.usage = data ? VMA_MEMORY_USAGE_AUTO : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (data) {
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }
    out.dev = &dev;
    VmaAllocationInfo ainfo {};
    const VkResult r = vmaCreateBuffer(dev.allocator(), &bi, &ai, &out.buffer, &out.alloc, &ainfo);
    if (r != VK_SUCCESS) {
        err = "vmaCreateBuffer(" + std::to_string(bytes) + " bytes) VkResult " + std::to_string(r);
        out.buffer = VK_NULL_HANDLE;
        return false;
    }
    if (data && ainfo.pMappedData) {
        std::memcpy(ainfo.pMappedData, data, bytes);
        vmaFlushAllocation(dev.allocator(), out.alloc, 0, VK_WHOLE_SIZE);
    }
    return true;
}

void
computeBarrier(VkCommandBuffer cmd)
{
    VkMemoryBarrier2 mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo dep { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);
}

struct Kernels {
    std::unique_ptr<GpuKernel> grade;
    std::unique_ptr<GpuKernel> blur;
};

bool
makeKernels(GpuDevice& dev, Kernels& k)
{
    GpuKernelDesc g;
    g.spirv = grade_spirv;
    g.spirvWords = grade_spirv_words;
    g.entry = "main";
    g.storageBufferCount = 2;
    g.pushConstantBytes = sizeof(gradefix::GradeParams);
    g.groupSize = { grade_group_size[0], grade_group_size[1], grade_group_size[2] };
    GpuKernelDesc b;
    b.spirv = blur_spirv;
    b.spirvWords = blur_spirv_words;
    b.entry = "main";
    b.storageBufferCount = 3;
    b.pushConstantBytes = sizeof(blurfix::BlurParams);
    b.groupSize = { blur_group_size[0], blur_group_size[1], blur_group_size[2] };
    for (auto [desc, out] : { std::pair { &g, &k.grade }, std::pair { &b, &k.blur } }) {
        GpuStatus s = GpuKernel::create(dev, *desc, *out);
        if (!s) {
            std::fprintf(stderr, "kernel create failed: %s\n", s.message.c_str());
            return false;
        }
    }
    return true;
}

struct PassTimers {
    GpuTimer* all = nullptr;
    GpuTimer* grade = nullptr;
    GpuTimer* h = nullptr;
    GpuTimer* v = nullptr;
};

gradefix::GradeParams
gradeParams(uint32_t w, uint32_t h)
{
    gradefix::GradeParams p {};
    for (int i = 0; i < 4; ++i) {
        p.a[i] = 1.2f;
        p.b[i] = 0.02f;
        p.gamma[i] = 0.9f;
        p.invGamma[i] = 1.0f / 0.9f;
    }
    p.width = w;
    p.height = h;
    p.nComps = kChannels;
    p.channelMask = 0xF;
    p.flags = gradefix::kClampBlack;
    return p;
}

struct Work {
    Kernels* k;
    VkBuffer weights;
};

// Records one image (or strip) of the workload from `in` to `out`. `tmp` is the blur scratch buffer.
bool
recordWork(VkCommandBuffer cmd, const Work& wk, Wl wl, uint32_t w, uint32_t h, VkBuffer in, VkBuffer out,
           VkBuffer tmp, const PassTimers& t)
{
    const WlInfo& wi = info(wl);
    bool ok = true;
    auto check = [&](const GpuStatus& s) {
        if (!s) {
            std::fprintf(stderr, "record failed: %s\n", s.message.c_str());
            ok = false;
        }
    };
    auto begin = [&](GpuTimer* tm) {
        if (tm) {
            tm->begin(cmd);
        }
    };
    auto end = [&](GpuTimer* tm) {
        if (tm) {
            tm->end(cmd);
        }
    };
    begin(t.all);
    const uint32_t radius = uint32_t(blurref::radiusForSigma(wi.sigma));
    auto blurPass = [&](VkBuffer src, VkBuffer dst, bool vertical, GpuTimer* tm) {
        begin(tm);
        check(recordBlurPass(*wk.k->blur, cmd, src, dst, wk.weights, GpuBlurPass { w, h, kChannels, radius, vertical, true }));
        end(tm);
    };
    if (wi.grade) {
        const gradefix::GradeParams p = gradeParams(w, h);
        begin(t.grade);
        const VkBuffer bufs[] = { in, out };
        check(wk.k->grade->record(cmd, bufs, std::as_bytes(std::span(&p, 1)), { w, h, 1 }));
        end(t.grade);
    }
    if (wi.grade && wi.sigma > 0.0) {
        // Grade left its result in `out`; the horizontal pass uses the free input buffer as scratch.
        computeBarrier(cmd);
        blurPass(out, in, false, t.h);
        computeBarrier(cmd);
        blurPass(in, out, true, t.v);
    } else if (wi.sigma > 0.0 && tmp) {
        blurPass(in, tmp, false, t.h);
        computeBarrier(cmd);
        blurPass(tmp, out, true, t.v);
    } else if (wi.sigma > 0.0) {
        // No scratch buffer fits next to a single frame: ping-pong through `out`, then copy back.
        blurPass(in, out, false, t.h);
        computeBarrier(cmd);
        blurPass(out, in, true, t.v);
        computeBarrier(cmd);
        const VkBufferCopy r { 0, 0, VkDeviceSize(w) * h * kChannels * sizeof(float) };
        vkCmdCopyBuffer(cmd, in, out, 1, &r);
    }
    end(t.all);
    return ok;
}

struct Weights {
    DevBuf buf;
    bool ok = false;
};

bool
makeWeights(GpuDevice& dev, double sigma, DevBuf& out, std::string& err)
{
    const std::vector<double> wd = blurref::makeWeights(sigma);
    const std::vector<float> wf(wd.begin(), wd.end());
    return makeStorage(dev, wf.size() * sizeof(float), out, err, wf.data());
}

struct KernelTimes {
    double total = kNaN, grade = kNaN, h = kNaN, v = kNaN;
};

// Times the workload over every unit of one image with the data resident on the GPU.
bool
measureKernel(GpuDevice& dev, Kernels& k, const SizeSpec& s, Wl wl, const std::vector<Unit>& units,
              bool useScratch, int iters, KernelTimes& out, std::string& err)
{
    uint32_t maxRows = 0;
    for (const Unit& u : units) {
        maxRows = std::max(maxRows, u.srcRows);
    }
    const VkDeviceSize bytes = VkDeviceSize(maxRows) * s.w * kChannels * sizeof(float);
    DevBuf in, outB, tmp, wbuf;
    if (!makeStorage(dev, bytes, in, err) || !makeStorage(dev, bytes, outB, err)) {
        return false;
    }
    const bool blur = info(wl).sigma > 0.0;
    if (blur && useScratch && !makeStorage(dev, bytes, tmp, err)) {
        return false;
    }
    if (blur && !makeWeights(dev, info(wl).sigma, wbuf, err)) {
        return false;
    }
    Work wk { &k, wbuf.buffer };

    std::vector<std::unique_ptr<GpuTimer>> timers;
    auto timer = [&]() {
        std::unique_ptr<GpuTimer> t;
        GpuTimer::create(dev, t);
        timers.push_back(std::move(t));
        return timers.back().get();
    };

    VkCommandPoolCreateInfo pci { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = dev.queueFamily(QueueKind::Compute);
    VkCommandPool pool;
    if (GpuStatus st = dev.check(vkCreateCommandPool(dev.device(), &pci, nullptr, &pool), "vkCreateCommandPool"); !st) {
        err = st.message;
        return false;
    }
    struct PoolGuard {
        VkDevice d;
        VkCommandPool p;
        ~PoolGuard() { vkDestroyCommandPool(d, p, nullptr); }
    } poolGuard { dev.device(), pool };
    VkCommandBufferAllocateInfo cai { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(dev.device(), &cai, &cmd);
    VkFenceCreateInfo fci { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    vkCreateFence(dev.device(), &fci, nullptr, &fence);

    std::vector<double> totals, grades, hs, vs;
    for (int it = -1; it < iters; ++it) {
        timers.clear();
        std::vector<PassTimers> pt;
        VkCommandBufferBeginInfo bi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkResetCommandBuffer(cmd, 0) != VK_SUCCESS || vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) {
            err = "command buffer begin failed";
            return false;
        }
        bool ok = true;
        for (const Unit& u : units) {
            PassTimers t { timer(), timer(), timer(), timer() };
            pt.push_back(t);
            ok &= recordWork(cmd, wk, wl, s.w, u.srcRows, in.buffer, outB.buffer, tmp.buffer, t);
            computeBarrier(cmd);
        }
        vkEndCommandBuffer(cmd);
        if (!ok) {
            err = "record failed";
            return false;
        }
        VkCommandBufferSubmitInfo csi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
        csi.commandBuffer = cmd;
        VkSubmitInfo2 si { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
        si.commandBufferInfoCount = 1;
        si.pCommandBufferInfos = &csi;
        vkResetFences(dev.device(), 1, &fence);
        if (GpuStatus st = dev.submit(QueueKind::Compute, { &si, 1 }, fence); !st) {
            err = st.message;
            return false;
        }
        vkWaitForFences(dev.device(), 1, &fence, VK_TRUE, UINT64_MAX);
        k.grade->releaseDescriptors();
        k.blur->releaseDescriptors();
        if (it < 0) {
            continue;
        }
        double tot = 0, g = 0, h = 0, v = 0;
        auto get = [&](GpuTimer* t, double& acc) {
            double ns = 0;
            if (t->supported() && t->elapsedNs(ns).ok()) {
                acc += ns * 1e-6;
            }
        };
        for (const PassTimers& t : pt) {
            get(t.all, tot);
            if (info(wl).grade) {
                get(t.grade, g);
            }
            if (blur) {
                get(t.h, h);
                get(t.v, v);
            }
        }
        totals.push_back(tot);
        grades.push_back(g);
        hs.push_back(h);
        vs.push_back(v);
    }
    vkDestroyFence(dev.device(), fence, nullptr);
    out.total = median(totals);
    out.grade = info(wl).grade ? median(grades) : kNaN;
    out.h = blur ? median(hs) : kNaN;
    out.v = blur ? median(vs) : kNaN;
    return true;
}

struct StageTimes {
    double wall = 0, hostUp = 0, gpuUp = 0, gpuComp = 0, gpuDown = 0, hostDown = 0;
    uint32_t stripsPerImage = 0;
    bool viaImport = false;
    bool mixedPaths = false;
};

struct Images {
    std::vector<HostBuf> src, dst;
    size_t bytes = 0;
};

struct Pipeline {
    GpuDevice* dev;
    Kernels* k;
    const SizeSpec* size;
    GpuTransfer* transfer;
    std::vector<DevBuf>* scratch;
    VkBuffer weights;
    bool useScratch;
};

bool
runOnce(const Pipeline& pl, Images& im, Wl wl, const std::vector<Unit>& units, bool overlap, StageTimes& out,
        std::string& err)
{
    const SizeSpec& s = *pl.size;
    const size_t rowBytes = size_t(s.w) * kChannels * sizeof(float);
    std::vector<TransferFrame> frames;
    std::vector<uint32_t> frameUnit;
    for (size_t img = 0; img < im.src.size(); ++img) {
        for (size_t u = 0; u < units.size(); ++u) {
            const Unit& un = units[u];
            TransferFrame f;
            f.src = im.src[img].get() + size_t(un.srcRow) * rowBytes;
            f.dst = im.dst[img].get() + size_t(un.dstRow) * rowBytes;
            f.bytes = size_t(un.srcRows) * rowBytes;
            if (un.dstRows != un.srcRows) {
                f.dstOffset = size_t(un.dstRow - un.srcRow) * rowBytes;
                f.dstBytes = size_t(un.dstRows) * rowBytes;
            }
            frames.push_back(f);
            frameUnit.push_back(uint32_t(u));
        }
    }

    size_t base = 0;
    const bool copy = info(wl).copyOnly;
    const Work wk { pl.k, pl.weights };
    ComputeRecordFn record;
    if (copy) {
        // RADV corrupts a single vkCmdCopyBuffer larger than about 2.5 GiB, which the default
        // null-record copy would issue for a whole 24k frame.
        record = [](VkCommandBuffer cmd, const ComputeBinding& b) {
            constexpr VkDeviceSize kChunk = VkDeviceSize(1) << 29;
            for (VkDeviceSize off = 0; off < b.bytes; off += kChunk) {
                const VkBufferCopy r { off, off, std::min(kChunk, b.bytes - off) };
                vkCmdCopyBuffer(cmd, b.input, b.output, 1, &r);
            }
            return GpuStatus {};
        };
    } else {
        record = [&](VkCommandBuffer cmd, const ComputeBinding& b) {
            const Unit& un = units[frameUnit[base + b.frameIndex]];
            VkBuffer tmp = pl.useScratch ? (*pl.scratch)[b.frameIndex % pl.scratch->size()].buffer : VK_NULL_HANDLE;
            if (!recordWork(cmd, wk, wl, s.w, un.srcRows, b.input, b.output, tmp, PassTimers {})) {
                return GpuStatus { VK_ERROR_UNKNOWN, "recording the workload failed" };
            }
            return GpuStatus {};
        };
    }

    uint32_t imported = 0;
    uint32_t seen = 0;
    auto accumulate = [&](const TransferTimeline& tl) {
        for (const FrameTimeline& f : tl.frames) {
            imported += (f.uploadPath == TransferPath::HostImport) + (f.downloadPath == TransferPath::HostImport);
            seen += 2;
        }
        out.wall += tl.wallMs;
        out.hostUp += tl.hostUploadBusyMs();
        out.gpuUp += tl.gpuUploadBusyMs();
        out.gpuComp += tl.gpuComputeBusyMs();
        out.gpuDown += tl.gpuDownloadBusyMs();
        out.hostDown += tl.hostDownloadBusyMs();
    };
    out = {};
    if (overlap) {
        TransferTimeline tl;
        GpuStatus st = pl.transfer->process(frames, record, &tl);
        if (!st) {
            err = st.message;
            return false;
        }
        accumulate(tl);
    } else {
        for (size_t k = 0; k < frames.size(); ++k) {
            base = k;
            TransferTimeline tl;
            GpuStatus st = pl.transfer->process({ &frames[k], 1 }, record, &tl);
            if (!st) {
                err = st.message;
                return false;
            }
            accumulate(tl);
        }
    }
    if (pl.k) {
        pl.k->grade->releaseDescriptors();
        pl.k->blur->releaseDescriptors();
    }
    const double n = double(im.src.size());
    for (double* v : { &out.wall, &out.hostUp, &out.gpuUp, &out.gpuComp, &out.gpuDown, &out.hostDown }) {
        *v /= n;
    }
    out.stripsPerImage = uint32_t(units.size());
    out.viaImport = imported == seen;
    out.mixedPaths = imported != 0 && imported != seen;
    return true;
}

struct Row {
    std::string size, mode, workload, path, overlap, note, context;
    uint32_t images = 0, units = 0;
    double movedMb = kNaN;
    double kernel = kNaN, grade = kNaN, h = kNaN, v = kNaN;
    double hostUp = kNaN, gpuUp = kNaN, gpuComp = kNaN, gpuDown = kNaN, hostDown = kNaN, e2e = kNaN;
    double load1 = kNaN;
};

std::string
num(double v)
{
    if (std::isnan(v)) {
        return "NA";
    }
    char b[32];
    std::snprintf(b, sizeof b, "%.3f", v);
    return b;
}

const char* kCsvHeader = "size,mode,workload,path,overlap,images,units_per_image,moved_mb_per_image,kernel_ms,grade_ms,h_ms,v_ms,"
                         "host_up_ms,gpu_up_ms,gpu_comp_ms,gpu_down_ms,host_down_ms,e2e_ms,load1,context,note";

void
writeRow(std::ofstream& csv, const Row& r)
{
    std::string note = r.note;
    std::replace(note.begin(), note.end(), ',', ';');
    std::string context = r.context;
    std::replace(context.begin(), context.end(), ',', ';');
    csv << r.size << ',' << r.mode << ',' << r.workload << ',' << r.path << ',' << r.overlap << ',' << r.images << ','
        << r.units << ',' << num(r.movedMb) << ',' << num(r.kernel) << ',' << num(r.grade) << ',' << num(r.h) << ','
        << num(r.v) << ',' << num(r.hostUp) << ',' << num(r.gpuUp) << ',' << num(r.gpuComp) << ',' << num(r.gpuDown)
        << ',' << num(r.hostDown) << ',' << num(r.e2e) << ',' << num(r.load1) << ',' << context << ',' << note << '\n';
    csv.flush();
}

struct Config {
    std::vector<std::string> sizes { "uhd", "8k", "16k", "24k" };
    int iters = 3;
    std::string csv = "gpu-bench.csv";
    std::string note;
};

std::vector<SizeSpec>
sizeSpecs(const std::vector<std::string>& names)
{
    std::vector<SizeSpec> out;
    for (const std::string& n : names) {
        if (n == "uhd") {
            out.push_back({ "UHD", 3840, 2160, false, 8 });
        } else if (n == "8k") {
            out.push_back({ "8K", 7680, 4320, false, 4 });
        } else if (n == "16k") {
            out.push_back({ "16k", 16000, 16000, true, 1 });
        } else if (n == "24k") {
            out.push_back({ "24k", 24000, 24000, true, 1 });
        } else if (n == "16k-single") {
            out.push_back({ "16k", 16000, 16000, false, 1, true });
        } else if (n == "24k-single") {
            out.push_back({ "24k", 24000, 24000, false, 1, true });
        }
    }
    return out;
}

// Tiled and untiled runs of the same image must agree: strips with halos reproduce the whole-frame blur.
bool
verifyTiling(GpuDevice& dev, Kernels& k)
{
    SizeSpec whole { "verify", 1024, 1536, false, 1 };
    SizeSpec tiled = whole;
    tiled.tiled = true;
    const size_t bytes = size_t(whole.w) * whole.h * kChannels * sizeof(float);
    Images im;
    im.src.push_back(alignedBuf(bytes));
    fillImage(im.src[0].get(), bytes, 7);
    double worst = 0.0;
    std::vector<std::vector<float>> results;
    for (const SizeSpec* s : { &whole, &tiled }) {
        for (Wl wl : { Wl::Blur100, Wl::GradeBlur25 }) {
            GpuTransferOptions o;
            o.allowHostImport = false;
            o.copyThreads = 4;
            std::unique_ptr<GpuTransfer> t;
            if (!GpuTransfer::create(dev, o, t)) {
                return false;
            }
            std::vector<DevBuf> scratch(2);
            std::string err;
            const std::vector<Unit> units = [&] {
                std::vector<Unit> u = makeUnits(*s, wl);
                if (s->tiled) {
                    // Small strips so the 1536-row image splits several times.
                    const uint32_t halo = haloRows(wl);
                    u.clear();
                    for (uint32_t y0 = 0; y0 < s->h; y0 += 512) {
                        const uint32_t y1 = std::min(s->h, y0 + 512);
                        const uint32_t s0 = y0 > halo ? y0 - halo : 0;
                        const uint32_t s1 = std::min(s->h, y1 + halo);
                        u.push_back({ s0, s1 - s0, y0, y1 - y0 });
                    }
                }
                return u;
            }();
            uint32_t maxRows = 0;
            for (const Unit& u : units) {
                maxRows = std::max(maxRows, u.srcRows);
            }
            for (DevBuf& b : scratch) {
                if (!makeStorage(dev, VkDeviceSize(maxRows) * s->w * kChannels * 4, b, err)) {
                    return false;
                }
            }
            DevBuf wbuf;
            if (!makeWeights(dev, info(wl).sigma, wbuf, err)) {
                return false;
            }
            Images run;
            run.src.push_back(alignedBuf(bytes));
            std::memcpy(run.src[0].get(), im.src[0].get(), bytes);
            run.dst.push_back(alignedBuf(bytes));
            touch(run.dst[0].get(), bytes);
            Pipeline pl { &dev, &k, s, t.get(), &scratch, wbuf.buffer, true };
            StageTimes st;
            if (!runOnce(pl, run, wl, units, true, st, err)) {
                std::fprintf(stderr, "verify run failed: %s\n", err.c_str());
                return false;
            }
            const float* f = reinterpret_cast<const float*>(run.dst[0].get());
            results.emplace_back(f, f + bytes / 4);
        }
    }
    for (size_t i = 0; i < 2; ++i) {
        for (size_t j = 0; j < results[i].size(); ++j) {
            worst = std::max(worst, double(std::fabs(results[i][j] - results[i + 2][j])));
        }
    }
    std::printf("verify: tiled strips vs whole-frame blur, max abs difference %.3g (Blur100, GradeBlur25)\n", worst);
    return worst <= 1e-6;
}

bool
runSize(GpuDevice& dev, Kernels& k, const SizeSpec& s, const Config& cfg, std::ofstream& csv)
{
    const size_t rowBytes = size_t(s.w) * kChannels * sizeof(float);
    const size_t imageBytes = rowBytes * s.h;
    const std::string mode = s.single ? "single" : (s.tiled ? "tiled" : "whole");
    std::printf("\n== %s %ux%u (%s, %.2f GB/image, %u image(s))\n", s.name.c_str(), s.w, s.h, mode.c_str(),
                imageBytes / 1e9, s.images);

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(dev.physicalDevice(), &props);
    const uint64_t maxRange = props.limits.maxStorageBufferRange;
    std::vector<VmaBudget> budget;
    dev.queryBudget(budget);
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(dev.physicalDevice(), &mem);
    uint64_t freeVram = 0;
    for (uint32_t i = 0; i < mem.memoryHeapCount && i < budget.size(); ++i) {
        if ((mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && budget[i].budget > budget[i].usage) {
            freeVram = std::max<uint64_t>(freeVram, budget[i].budget - budget[i].usage);
        }
    }
    std::printf("maxStorageBufferRange %.2f GB, device-local VRAM headroom %.2f GB\n", maxRange / 1e9, freeVram / 1e9);

    Images im;
    im.bytes = imageBytes;
    for (uint32_t i = 0; i < s.images; ++i) {
        im.src.push_back(alignedBuf(imageBytes));
        im.dst.push_back(alignedBuf(imageBytes));
        if (!im.src.back() || !im.dst.back()) {
            std::fprintf(stderr, "host allocation of %zu bytes failed\n", imageBytes);
            return false;
        }
        fillImage(im.src.back().get(), imageBytes, i + 1);
        touch(im.dst.back().get(), imageBytes);
    }

    struct PathCfg {
        const char* name;
        bool import;
    };
    const PathCfg paths[] = { { "staging", false }, { "host-import", true } };
    const int iters = s.name == "UHD" ? std::max(cfg.iters, 5) : cfg.iters;
    const uint32_t depth = s.single ? 1 : 2;

    for (Wl wl : { Wl::Copy, Wl::Grade, Wl::Blur3, Wl::Blur25, Wl::Blur100, Wl::GradeBlur25 }) {
        const WlInfo& wi = info(wl);
        const std::vector<Unit> units = makeUnits(s, wl);
        uint64_t movedBytes = 0;
        for (const Unit& u : units) {
            movedBytes += uint64_t(u.srcRows + u.dstRows) * rowBytes;
        }
        const bool blur = wi.sigma > 0.0;
        // A single frame has room for in/out only; blur then ping-pongs and copies back.
        const bool useScratch = blur && !wi.grade && !s.single;
        std::string skip;
        if (s.single && !wi.copyOnly) {
            if (imageBytes > maxRange) {
                skip = "frame exceeds maxStorageBufferRange";
            }
        }
        if (s.single && uint64_t(2 * imageBytes) + (512ull << 20) > freeVram) {
            skip = "frame does not fit in VRAM";
        }

        Row base;
        base.size = s.name;
        base.mode = mode;
        base.workload = wi.name;
        base.images = s.images;
        base.units = uint32_t(units.size());
        base.movedMb = movedBytes / 1e6;
        base.context = cfg.note;

        KernelTimes kt;
        std::string err;
        if (!wi.copyOnly && skip.empty()) {
            if (!measureKernel(dev, k, s, wl, units, useScratch, 5, kt, err)) {
                skip = "kernel run failed: " + err;
            }
        }
        std::printf("%-12s kernel %s ms (grade %s, H %s, V %s)%s\n", wi.name, num(kt.total).c_str(),
                    num(kt.grade).c_str(), num(kt.h).c_str(), num(kt.v).c_str(), skip.empty() ? "" : (" [" + skip + "]").c_str());

        DevBuf wbuf;
        if (blur && skip.empty() && !makeWeights(dev, wi.sigma, wbuf, err)) {
            skip = err;
        }
        for (const PathCfg& pc : paths) {
            if (!skip.empty() && !wi.copyOnly) {
                for (const char* ov : { "on", "off" }) {
                    Row r = base;
                    r.path = pc.name;
                    r.overlap = ov;
                    r.note += (r.note.empty() ? "" : "; ") + skip;
                    r.load1 = loadAvg();
                    writeRow(csv, r);
                }
                continue;
            }
            GpuTransferOptions o;
            o.allowHostImport = pc.import;
            o.cacheHostImports = pc.import;
            o.copyThreads = pc.import ? 1 : 8;
            o.framesInFlight = depth;
            o.recordTimestamps = true;
            std::unique_ptr<GpuTransfer> t;
            if (GpuStatus st = GpuTransfer::create(dev, o, t); !st || (pc.import && !t->hostImportSupported())) {
                for (const char* ov : { "on", "off" }) {
                    Row r = base;
                    r.path = pc.name;
                    r.overlap = ov;
                    r.note += (r.note.empty() ? "" : "; ") + std::string(st ? "no host import" : st.message);
                    writeRow(csv, r);
                }
                continue;
            }
            std::vector<DevBuf> scratch;
            if (useScratch) {
                uint32_t maxRows = 0;
                for (const Unit& u : units) {
                    maxRows = std::max(maxRows, u.srcRows);
                }
                scratch.resize(depth);
                for (DevBuf& b : scratch) {
                    if (!makeStorage(dev, VkDeviceSize(maxRows) * rowBytes, b, err)) {
                        skip = err;
                    }
                }
            }
            Pipeline pl { &dev, &k, &s, t.get(), &scratch, wbuf.buffer, useScratch };
            for (bool overlap : { true, false }) {
                Row r = base;
                r.path = pc.name;
                r.overlap = overlap ? "on" : "off";
                r.kernel = kt.total;
                r.grade = kt.grade;
                r.h = kt.h;
                r.v = kt.v;
                std::vector<double> wall, hu, gu, gc, gd, hd;
                bool ok = skip.empty();
                // The first pass pays host-import pinning and page faults, outside the measurement.
                StageTimes st;
                if (ok) {
                    ok = runOnce(pl, im, wl, units, overlap, st, err);
                }
                for (int it = 0; ok && it < iters; ++it) {
                    if (wi.copyOnly && it == iters - 1) {
                        // The check below must see only what the last pass wrote.
                        for (HostBuf& d : im.dst) {
                            touch(d.get(), imageBytes);
                        }
                    }
                    ok = runOnce(pl, im, wl, units, overlap, st, err);
                    wall.push_back(st.wall);
                    hu.push_back(st.hostUp);
                    gu.push_back(st.gpuUp);
                    gc.push_back(st.gpuComp);
                    gd.push_back(st.gpuDown);
                    hd.push_back(st.hostDown);
                }
                if (ok && st.viaImport != pc.import) {
                    r.note += (r.note.empty() ? "" : "; ") + std::string(st.mixedPaths ? "mixed transfer paths" : "ran on the other transfer path");
                }
                if (ok && s.single && wi.copyOnly) {
                    std::vector<VmaBudget> used;
                    dev.queryBudget(used);
                    VkPhysicalDeviceMemoryProperties mp;
                    vkGetPhysicalDeviceMemoryProperties(dev.physicalDevice(), &mp);
                    for (uint32_t h = 0; h < mp.memoryHeapCount && h < used.size(); ++h) {
                        char buf[96];
                        std::snprintf(buf, sizeof buf, "%s heap in use %.1f GB", (mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? "device-local" : "host", used[h].usage / 1e9);
                        r.note += (r.note.empty() ? "" : "; ") + std::string(buf);
                    }
                }
                if (ok && wi.copyOnly) {
                    for (size_t i = 0; i < im.src.size(); ++i) {
                        const uint8_t* a = im.src[i].get();
                        const uint8_t* b = im.dst[i].get();
                        constexpr size_t kBlock = size_t(1) << 26;
                        size_t first = SIZE_MAX, bad = 0;
                        for (size_t off = 0; off < imageBytes; off += kBlock) {
                            const size_t n = std::min(kBlock, imageBytes - off);
                            if (std::memcmp(a + off, b + off, n) != 0) {
                                first = std::min(first, off);
                                ++bad;
                            }
                        }
                        if (bad) {
                            const std::string msg = "ROUND TRIP DATA MISMATCH from byte " + std::to_string(first) + " in " + std::to_string(bad) + " of " + std::to_string((imageBytes + kBlock - 1) / kBlock) + " 64MB blocks";
                            r.note += (r.note.empty() ? "" : "; ") + msg;
                            std::fprintf(stderr, "%s %s: %s\n", s.name.c_str(), pc.name, msg.c_str());
                        }
                    }
                }
                if (!ok) {
                    r.note += (r.note.empty() ? "" : "; ") + (skip.empty() ? err : skip);
                } else {
                    r.e2e = median(wall);
                    r.hostUp = median(hu);
                    r.gpuUp = median(gu);
                    r.gpuComp = median(gc);
                    r.gpuDown = median(gd);
                    r.hostDown = median(hd);
                    if (s.single && !overlap) {
                        r.note += (r.note.empty() ? "" : "; ") + std::string("one frame: overlap on and off are the same run");
                    }
                }
                r.load1 = loadAvg();
                if (s.single && overlap && ok) {
                    // A single frame cannot overlap with anything; both rows come from the same measurement.
                    Row dup = r;
                    dup.overlap = "off";
                    dup.note = r.note + (r.note.empty() ? "" : "; ") + "one frame: overlap on and off are the same run";
                    writeRow(csv, r);
                    writeRow(csv, dup);
                    std::printf("  %-12s %-11s overlap on/off e2e %9.1f ms\n", wi.name, pc.name, r.e2e);
                    break;
                }
                writeRow(csv, r);
                std::printf("  %-12s %-11s overlap %-3s e2e %9.1f ms | host-up %7.1f gpu-up %7.1f comp %7.1f gpu-down %7.1f host-down %7.1f\n",
                            wi.name, pc.name, r.overlap.c_str(), r.e2e, r.hostUp, r.gpuUp, r.gpuComp, r.gpuDown,
                            r.hostDown);
            }
            t->releaseHostImports();
            t.reset();
        }
    }
    return true;
}

} // namespace

int
main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--csv") {
            cfg.csv = next();
        } else if (a == "--iters") {
            cfg.iters = std::max(1, std::atoi(next().c_str()));
        } else if (a == "--strip-rows") {
            gStripRows = roundUp(uint32_t(std::atoi(next().c_str())), kRowQuantum);
        } else if (a == "--note") {
            cfg.note = next();
        } else if (a == "--sizes") {
            cfg.sizes.clear();
            const std::string v = next();
            for (size_t p = 0; p <= v.size();) {
                const size_t q = v.find(',', p);
                cfg.sizes.push_back(v.substr(p, q == std::string::npos ? std::string::npos : q - p));
                if (q == std::string::npos) {
                    break;
                }
                p = q + 1;
            }
        } else {
            std::fprintf(stderr, "usage: GpuBench [--sizes uhd,8k,16k,24k,24k-single] [--iters N] [--csv path] [--note text]\n");
            return 2;
        }
    }

    GpuDeviceOptions dopt;
    dopt.enableValidation = false;
    std::unique_ptr<GpuDevice> dev;
    if (GpuStatus st = GpuDevice::create(dopt, dev); !st) {
        std::fprintf(stderr, "device: %s\n", st.message.c_str());
        return 1;
    }
    if (dev->info().type == VK_PHYSICAL_DEVICE_TYPE_CPU) {
        std::fprintf(stderr, "GpuBench needs a real GPU, found a software device (%s)\n", dev->info().name.c_str());
        return 1;
    }
    std::printf("device: %s | transferUsesComputeQueue=%d hostImport=%d | iters=%d | hw threads %u\n",
                dev->info().name.c_str(), dev->info().transferUsesComputeQueue, dev->info().externalMemoryHost,
                cfg.iters, std::thread::hardware_concurrency());

    Kernels k;
    if (!makeKernels(*dev, k)) {
        return 1;
    }
    if (!verifyTiling(*dev, k)) {
        std::fprintf(stderr, "tiled blur does not match the whole-frame blur\n");
        return 3;
    }

    std::ofstream csv(cfg.csv);
    csv << kCsvHeader << '\n';
    for (const SizeSpec& s : sizeSpecs(cfg.sizes)) {
        if (!runSize(*dev, k, s, cfg, csv)) {
            return 1;
        }
    }
    return 0;
}
