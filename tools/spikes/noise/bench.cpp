// Noise library spike harness. Renders FastNoise2's 4D Perlin, SuperSimplex and Cellular F1 (each
// alone and under FBm) through:
//
//   a_base, a_base_fma, a_v3, a_v3_fma  the plain C++ port (fn2port/), x86-64 or x86-64-v3, unfused
//                                       or with explicit fma; the SoA row loop
//   b_strict, b_relaxed                 FastNoise2 itself (fn2lib/), GenPositionArray4D per row
//   fnl                                 FastNoiseLite's scalar 3D equivalent (z only), the floor
//   gpu, gpu_fma                        fn2port4d.slang through the Vulkan fixture, unfused / fused
//
// Pixel (x, y) maps to noise position (origin + x + 0.5, origin + y + 0.5, z, w) times 1/scale.
//
//   NoiseSpikeBench [--size N|WxH] [--algos a,b] [--impls a,b] [--iters N] [--threads N]
//                   [--scale S] [--octaves N] [--origins 0,1e5] [--band N] [--out dir]
//                   [--fn2-simd max|scalar|sse2|sse41|avx2|avx512] [--exr-all] [--no-gpu]
//
// Per (algorithm, origin, implementation) it prints and appends to <out>/results.csv: CPU ms/Mpx
// on one thread (a band of rows) and on all threads, GPU kernel ms/Mpx (median of timestamped
// dispatches), and the max (CSV: also mean) absolute difference against a_base, gpu and gpu_fma.
// For the a_* variants it also checks that the row loop equals per-pixel fbm() bit for bit on every
// eighth row ("self" column: mismatching pixels). fnl is 3D noise, so its differences are not
// comparable and are left empty. EXRs (Y channel, top row first) of a_base, b_strict, fnl, gpu and
// the a_base-vs-gpu and a_base-vs-b_strict differences go to <out> for the first origin;
// --exr-all writes every implementation at every origin.
//
// GPU_SPIKE_ALLOW_SOFTWARE=1 accepts a lavapipe device; without it a software device is refused,
// as in tools/gpu-spike. GPU timings mean something only on the RX 7900 XTX host.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "GpuBuffer.h"
#include "GpuDevice.h"
#include "GpuKernel.h"

#include "FastNoiseLite.h"
#include "fn2lib/fn2shim.h"
#include "fn2port/fn2port_rows.h"

#include "fn2port_gpu_fma_spirv.h"
#include "fn2port_gpu_spirv.h"

using namespace gpu;

namespace {

// The push-constant block of fn2port4d.slang.
struct GpuParams {
    uint32_t width;
    uint32_t height;
    float originX;
    float originY;
    float frequency;
    float z;
    float w;
    uint32_t seed;
    int32_t octaves;
    float lacunarity;
    float gain;
    float weightedStrength;
    float gridJitter;
    int32_t type;
};
static_assert(sizeof(GpuParams) == 56);

struct KernelBlob {
    const char* name;
    const uint32_t* spirv;
    size_t spirvWords;
    const uint32_t* groupSize;
    uint32_t storageBuffers;
    uint32_t pushBytes;
};

#define SPIKE_KERNEL(t) #t, t##_spirv, t##_spirv_words, t##_group_size, t##_storage_buffers, t##_push_bytes

const KernelBlob kKernels[] = { { SPIKE_KERNEL(fn2port_gpu) }, { SPIKE_KERNEL(fn2port_gpu_fma) } };

struct HostBuffer {
    GpuDevice* dev = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    void* mapped = nullptr;

    ~HostBuffer()
    {
        if (buffer) {
            vmaDestroyBuffer(dev->allocator(), buffer, alloc);
        }
    }
};

class Gpu {
public:
    ~Gpu()
    {
        if (dev_) {
            dev_->waitIdle();
            if (fence_) {
                vkDestroyFence(dev_->device(), fence_, nullptr);
            }
            if (pool_) {
                vkDestroyCommandPool(dev_->device(), pool_, nullptr);
            }
        }
    }

    bool init(std::string& err)
    {
        GpuDeviceOptions o;
        o.enableValidation = false;
        if (GpuStatus s = GpuDevice::create(o, dev_); !s) {
            err = "device: " + s.message;
            return false;
        }
        const char* allowSoftware = std::getenv("GPU_SPIKE_ALLOW_SOFTWARE");
        if (dev_->info().type == VK_PHYSICAL_DEVICE_TYPE_CPU && !(allowSoftware && allowSoftware[0] == '1')) {
            err = "found only a software device (" + dev_->info().name + "); GPU_SPIKE_ALLOW_SOFTWARE=1 accepts it";
            return false;
        }
        VkCommandPoolCreateInfo pci { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = dev_->queueFamily(QueueKind::Compute);
        if (GpuStatus s = dev_->check(vkCreateCommandPool(dev_->device(), &pci, nullptr, &pool_), "vkCreateCommandPool"); !s) {
            err = s.message;
            return false;
        }
        VkCommandBufferAllocateInfo cai { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cai.commandPool = pool_;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VkFenceCreateInfo fci { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (vkAllocateCommandBuffers(dev_->device(), &cai, &cmd_) != VK_SUCCESS
            || vkCreateFence(dev_->device(), &fci, nullptr, &fence_) != VK_SUCCESS) {
            err = "command buffer or fence creation failed";
            return false;
        }
        if (GpuStatus s = GpuTimer::create(*dev_, timer_); !s) {
            err = "timer: " + s.message;
            return false;
        }
        return true;
    }

    const GpuDeviceInfo& info() const { return dev_->info(); }

    bool makeKernel(const KernelBlob& a, std::unique_ptr<GpuKernel>& out, std::string& err)
    {
        GpuKernelDesc d;
        d.spirv = a.spirv;
        d.spirvWords = a.spirvWords;
        d.entry = "main";
        d.storageBufferCount = a.storageBuffers;
        d.pushConstantBytes = a.pushBytes;
        d.groupSize = { a.groupSize[0], a.groupSize[1], a.groupSize[2] };
        if (GpuStatus s = GpuKernel::create(*dev_, d, out); !s) {
            err = std::string(a.name) + " kernel: " + s.message;
            return false;
        }
        return true;
    }

    // Median kernel time over `iters` timed dispatches after one warm-up; the last dispatch is
    // read back into `out`.
    bool render(GpuKernel& k, const GpuParams& p, int iters, std::vector<float>& out, double& kernelMs, std::string& err)
    {
        const VkDeviceSize bytes = VkDeviceSize(p.width) * p.height * sizeof(float);
        GpuBuffer dst;
        if (GpuStatus s = GpuBuffer::createDeviceLocal(*dev_, bytes, kGpuBufferDefaultUsage, dst); !s) {
            err = "output buffer: " + s.message;
            return false;
        }
        HostBuffer rb;
        if (!makeHostBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rb, err)) {
            return false;
        }
        const VkBuffer bufs[] = { dst.buffer() };
        std::vector<double> times;
        for (int it = -1; it < iters; ++it) {
            const bool last = it == iters - 1;
            VkCommandBufferBeginInfo bi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if (vkResetCommandBuffer(cmd_, 0) != VK_SUCCESS || vkBeginCommandBuffer(cmd_, &bi) != VK_SUCCESS) {
                err = "command buffer begin failed";
                return false;
            }
            timer_->begin(cmd_);
            if (GpuStatus s = k.record(cmd_, std::span<const VkBuffer>(bufs), std::as_bytes(std::span(&p, 1)), { p.width, p.height, 1 }); !s) {
                err = "record: " + s.message;
                return false;
            }
            timer_->end(cmd_);
            if (last) {
                VkMemoryBarrier2 mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
                mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                mb.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                VkDependencyInfo dep { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                dep.memoryBarrierCount = 1;
                dep.pMemoryBarriers = &mb;
                vkCmdPipelineBarrier2(cmd_, &dep);
                VkBufferCopy r { 0, 0, bytes };
                vkCmdCopyBuffer(cmd_, dst.buffer(), rb.buffer, 1, &r);
                mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
                mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
                vkCmdPipelineBarrier2(cmd_, &dep);
            }
            vkEndCommandBuffer(cmd_);
            VkCommandBufferSubmitInfo csi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
            csi.commandBuffer = cmd_;
            VkSubmitInfo2 si { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
            si.commandBufferInfoCount = 1;
            si.pCommandBufferInfos = &csi;
            vkResetFences(dev_->device(), 1, &fence_);
            if (GpuStatus s = dev_->submit(QueueKind::Compute, { &si, 1 }, fence_); !s) {
                err = "submit: " + s.message;
                return false;
            }
            vkWaitForFences(dev_->device(), 1, &fence_, VK_TRUE, UINT64_MAX);
            k.releaseDescriptors();
            double ns = 0.0;
            if (it >= 0 && timer_->supported() && timer_->elapsedNs(ns).ok()) {
                times.push_back(ns * 1e-6);
            }
        }
        vmaInvalidateAllocation(dev_->allocator(), rb.alloc, 0, VK_WHOLE_SIZE);
        out.resize((size_t)p.width * p.height);
        std::memcpy(out.data(), rb.mapped, bytes);
        if (times.empty()) {
            kernelMs = NAN;
        } else {
            std::sort(times.begin(), times.end());
            kernelMs = times[times.size() / 2];
        }
        return true;
    }

private:
    bool makeHostBuffer(VkDeviceSize bytes, VkBufferUsageFlags usage, HostBuffer& out, std::string& err)
    {
        VkBufferCreateInfo bi { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bi.size = bytes;
        bi.usage = usage;
        VmaAllocationCreateInfo ai {};
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info {};
        out.dev = dev_.get();
        if (vmaCreateBuffer(dev_->allocator(), &bi, &ai, &out.buffer, &out.alloc, &info) != VK_SUCCESS) {
            out.buffer = VK_NULL_HANDLE;
            err = "host buffer allocation failed";
            return false;
        }
        out.mapped = info.pMappedData;
        return true;
    }

    std::unique_ptr<GpuDevice> dev_;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    std::unique_ptr<GpuTimer> timer_;
};

template <typename T>
void
put(std::string& s, const T& v)
{
    s.append(reinterpret_cast<const char*>(&v), sizeof(T));
}

void
putAttr(std::string& s, const char* name, const char* type, const std::string& data)
{
    s.append(name, std::strlen(name) + 1);
    s.append(type, std::strlen(type) + 1);
    put(s, (int32_t)data.size());
    s += data;
}

// Single-part scanline OpenEXR, one FLOAT channel "Y", no compression. Row 0 of `px` is the
// bottom of the canonical image, so rows are written in reverse.
bool
writeExr(const std::string& path, const std::vector<float>& px, uint32_t w, uint32_t h)
{
    std::string hdr;
    put(hdr, (int32_t)20000630);
    put(hdr, (int32_t)2);

    std::string ch("Y", 2);
    put(ch, (int32_t)2);
    ch.append(4, '\0');
    put(ch, (int32_t)1);
    put(ch, (int32_t)1);
    ch.push_back('\0');
    putAttr(hdr, "channels", "chlist", ch);
    putAttr(hdr, "compression", "compression", std::string(1, '\0'));
    std::string box;
    for (int32_t v : { 0, 0, (int32_t)w - 1, (int32_t)h - 1 }) {
        put(box, v);
    }
    putAttr(hdr, "dataWindow", "box2i", box);
    putAttr(hdr, "displayWindow", "box2i", box);
    putAttr(hdr, "lineOrder", "lineOrder", std::string(1, '\0'));
    std::string f;
    put(f, 1.0f);
    putAttr(hdr, "pixelAspectRatio", "float", f);
    std::string v2;
    put(v2, 0.0f);
    put(v2, 0.0f);
    putAttr(hdr, "screenWindowCenter", "v2f", v2);
    putAttr(hdr, "screenWindowWidth", "float", f);
    hdr.push_back('\0');

    const uint64_t chunk = 8 + (uint64_t)w * sizeof(float);
    const uint64_t first = hdr.size() + 8ull * h;
    for (uint32_t y = 0; y < h; ++y) {
        put(hdr, first + y * chunk);
    }
    std::ofstream o(path, std::ios::binary);
    o.write(hdr.data(), (std::streamsize)hdr.size());
    for (uint32_t y = 0; y < h; ++y) {
        const int32_t line[2] = { (int32_t)y, (int32_t)(w * sizeof(float)) };
        o.write(reinterpret_cast<const char*>(line), sizeof(line));
        o.write(reinterpret_cast<const char*>(&px[(size_t)(h - 1 - y) * w]), (std::streamsize)(w * sizeof(float)));
    }
    return (bool)o;
}

struct Stats {
    double maxDiff = 0.0;
    double meanDiff = 0.0;
    double bitEqual = 0.0;
};

Stats
compare(const std::vector<float>& a, const std::vector<float>& b, std::vector<float>& diff)
{
    Stats s;
    diff.resize(a.size());
    size_t equal = 0;
    double sum = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        const double d = std::fabs((double)a[i] - (double)b[i]);
        diff[i] = (float)d;
        s.maxDiff = std::max(s.maxDiff, d);
        sum += d;
        equal += std::memcmp(&a[i], &b[i], sizeof(float)) == 0;
    }
    s.meanDiff = sum / (double)a.size();
    s.bitEqual = (double)equal / (double)a.size();
    return s;
}

double
nowMs()
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Algo {
    const char* name;
    int type; // 0 Perlin, 1 SuperSimplex, 2 Cellular F1
    bool fbm;
};

const Algo kAlgos[] = {
    { "perlin", 0, false },
    { "supersimplex", 1, false },
    { "cellular_f1", 2, false },
    { "perlin_fbm", 0, true },
    { "supersimplex_fbm", 1, true },
    { "cellular_f1_fbm", 2, true },
};

const char* const kImpls[] = { "a_base", "a_base_fma", "a_v3", "a_v3_fma", "b_strict", "b_relaxed", "fnl", "gpu", "gpu_fma" };

struct Config {
    uint32_t width = 2048;
    uint32_t height = 2048;
    std::vector<std::string> algos;
    std::vector<std::string> impls;
    int iters = 5;
    unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    float scale = 64.0f;
    int octaves = 4;
    uint32_t band = 128;
    std::vector<float> origins { 0.0f, 1e5f };
    std::string out = "build/spikes/noise";
    std::string fn2Simd = "max";
    bool exrAll = false;
    bool gpu = true;
};

// Noise parameters shared by every implementation.
struct Noise {
    int type;
    float frequency;
    int octaves;
    float lacunarity = 2.0f;
    float gain = 0.5f;
    float weightedStrength = 0.0f;
    float gridJitter = 1.0f;
    uint32_t seed = 1337;
    float origin;
    float z = 23.5f;
    float w = 41.25f;
};

// Renders row y (width pixels) into out; must be callable from several threads at once.
using RowFn = std::function<void(int y, float* out)>;

void
renderRows(const RowFn& f, uint32_t w, uint32_t y0, uint32_t y1, float* out)
{
    for (uint32_t y = y0; y < y1; ++y) {
        f((int)y, out + (size_t)(y - y0) * w);
    }
}

double
renderImage(const RowFn& f, uint32_t w, uint32_t h, unsigned threads, std::vector<float>& out)
{
    out.assign((size_t)w * h, 0.0f);
    const double t0 = nowMs();
    std::vector<std::thread> ts;
    for (unsigned t = 0; t < threads; ++t) {
        ts.emplace_back([&, t] {
            for (uint32_t y = t * 8; y < h; y += threads * 8) {
                const uint32_t y1 = std::min(h, y + 8);
                renderRows(f, w, y, y1, out.data() + (size_t)y * w);
            }
        });
    }
    for (std::thread& t : ts) {
        t.join();
    }
    return nowMs() - t0;
}

// One-thread ms/Mpx over the first `rows` rows, after one warm-up row.
double
timeBand(const RowFn& f, uint32_t w, uint32_t rows)
{
    std::vector<float> band((size_t)w * rows);
    f(0, band.data());
    const double t0 = nowMs();
    renderRows(f, w, 0, rows, band.data());
    return (nowMs() - t0) / ((double)w * rows * 1e-6);
}

Fn2PortParams
portParams(const Noise& n, uint32_t width)
{
    return Fn2PortParams { n.type, n.frequency, n.octaves, n.lacunarity, n.gain, n.weightedStrength, n.gridJitter,
                           n.seed, n.origin, n.origin, n.z, n.w, (int)width };
}

struct Fn2Lib {
    void* (*create)(int, float, int, float, float, const char*);
    void (*destroy)(void*);
    void (*row)(void*, float*, int, const float*, const float*, const float*, const float*, int);
    const char* (*simd)(void*);
};

const Fn2Lib kFn2Strict { fn2spike_strict_create, fn2spike_strict_destroy, fn2spike_strict_row, fn2spike_strict_simd };
const Fn2Lib kFn2Relaxed { fn2spike_relaxed_create, fn2spike_relaxed_destroy, fn2spike_relaxed_row,
                           fn2spike_relaxed_simd };

struct Fn2Handle {
    const Fn2Lib* lib = nullptr;
    void* h = nullptr;
    ~Fn2Handle()
    {
        if (h) {
            lib->destroy(h);
        }
    }
};

std::string
originTag(float o)
{
    char b[32];
    std::snprintf(b, sizeof(b), "%g", o);
    return b;
}

std::vector<std::string>
splitList(const std::string& v)
{
    std::vector<std::string> out;
    for (size_t p = 0; p <= v.size();) {
        const size_t q = v.find(',', p);
        out.push_back(v.substr(p, q == std::string::npos ? std::string::npos : q - p));
        if (q == std::string::npos) {
            break;
        }
        p = q + 1;
    }
    return out;
}

bool
selected(const std::vector<std::string>& list, const char* name)
{
    return list.empty() || std::find(list.begin(), list.end(), name) != list.end();
}

bool
parseArgs(int argc, char** argv, Config& cfg)
{
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--size") {
            const std::string v = next();
            const size_t x = v.find('x');
            cfg.width = (uint32_t)std::stoul(v.substr(0, x));
            cfg.height = x == std::string::npos ? cfg.width : (uint32_t)std::stoul(v.substr(x + 1));
        } else if (a == "--algos") {
            cfg.algos = splitList(next());
        } else if (a == "--impls") {
            cfg.impls = splitList(next());
        } else if (a == "--iters") {
            cfg.iters = std::stoi(next());
        } else if (a == "--threads") {
            cfg.threads = std::max(1u, (unsigned)std::stoul(next()));
        } else if (a == "--scale") {
            cfg.scale = std::stof(next());
        } else if (a == "--octaves") {
            cfg.octaves = std::stoi(next());
        } else if (a == "--band") {
            cfg.band = (uint32_t)std::stoul(next());
        } else if (a == "--out") {
            cfg.out = next();
        } else if (a == "--fn2-simd") {
            cfg.fn2Simd = next();
        } else if (a == "--exr-all") {
            cfg.exrAll = true;
        } else if (a == "--no-gpu") {
            cfg.gpu = false;
        } else if (a == "--origins") {
            cfg.origins.clear();
            for (const std::string& o : splitList(next())) {
                cfg.origins.push_back(std::stof(o));
            }
        } else {
            return false;
        }
    }
    for (const std::string& name : cfg.algos) {
        if (std::none_of(std::begin(kAlgos), std::end(kAlgos), [&](const Algo& a) { return name == a.name; })) {
            std::fprintf(stderr, "unknown algorithm: %s\n", name.c_str());
            return false;
        }
    }
    for (const std::string& name : cfg.impls) {
        if (std::none_of(std::begin(kImpls), std::end(kImpls), [&](const char* n) { return name == n; })) {
            std::fprintf(stderr, "unknown implementation: %s\n", name.c_str());
            return false;
        }
    }
    return cfg.width > 0 && cfg.height > 0 && cfg.iters > 0 && cfg.scale > 0.0f && cfg.octaves > 1 && cfg.band > 0
        && !cfg.origins.empty();
}

} // namespace

int
main(int argc, char** argv)
{
    Config cfg;
    if (!parseArgs(argc, argv, cfg)) {
        std::fprintf(stderr, "usage: NoiseSpikeBench [--size N|WxH] [--algos a,b] [--impls a,b] [--iters N] "
                             "[--threads N] [--scale S] [--octaves N] [--origins 0,1e5] [--band N] [--out dir] "
                             "[--fn2-simd max|scalar|sse2|sse41|avx2|avx512] [--exr-all] [--no-gpu]\n");
        return 2;
    }
    std::filesystem::create_directories(cfg.out);
    cfg.band = std::min(cfg.band, cfg.height);
    const bool haveV3 = __builtin_cpu_supports("x86-64-v3");

    const bool wantGpu = cfg.gpu && (selected(cfg.impls, "gpu") || selected(cfg.impls, "gpu_fma"));
    Gpu gpu;
    std::unique_ptr<GpuKernel> kernels[std::size(kKernels)];
    if (wantGpu) {
        std::string err;
        if (!gpu.init(err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        std::printf("device: %s\n", gpu.info().name.c_str());
        for (size_t i = 0; i < std::size(kKernels); ++i) {
            if (!gpu.makeKernel(kKernels[i], kernels[i], err)) {
                std::fprintf(stderr, "%s\n", err.c_str());
                return 1;
            }
        }
    }

    const Fn2PortVariant* const variants[] = { &fn2port_base, &fn2port_base_fma, &fn2port_v3, &fn2port_v3_fma };
    const char* const variantImpl[] = { "a_base", "a_base_fma", "a_v3", "a_v3_fma" };

    const bool newCsv = !std::filesystem::exists(cfg.out + "/results.csv");
    std::ofstream csv(cfg.out + "/results.csv", std::ios::app);
    if (newCsv) {
        csv << "algo,impl,simd,size,origin,scale,octaves,threads,cpu1_ms_per_mpx,cpuN_ms_per_mpx,gpu_ms_per_mpx,"
               "max_vs_a_base,mean_vs_a_base,max_vs_gpu,mean_vs_gpu,max_vs_gpu_fma,mean_vs_gpu_fma,self_mismatch,min,"
               "max\n";
    }
    std::printf("%-17s %-10s %-7s %8s %9s %9s %9s %10s %10s %10s %6s\n", "algo", "impl", "simd", "origin", "cpu1ms/Mpx",
                "cpuNms/Mpx", "gpums/Mpx", "vs_a_base", "vs_gpu", "vs_gpu_fma", "self");

    const double mpx = (double)cfg.width * cfg.height * 1e-6;
    for (const Algo& a : kAlgos) {
        if (!selected(cfg.algos, a.name)) {
            continue;
        }
        for (size_t oi = 0; oi < cfg.origins.size(); ++oi) {
            Noise n;
            n.type = a.type;
            n.frequency = 1.0f / cfg.scale;
            n.octaves = a.fbm ? cfg.octaves : 1;
            n.origin = cfg.origins[oi];
            const bool writeKeyExr = oi == 0;
            const std::string stem = cfg.out + "/" + a.name + "_o" + originTag(n.origin);

            // References first: the GPU images and a_base.
            std::map<std::string, std::vector<float>> refs;

            struct Result {
                std::string impl, simd;
                double cpu1 = NAN, cpuN = NAN, gpu = NAN;
                std::vector<float> img;
                long self = -1;
            };
            std::vector<Result> results;

            auto runGpu = [&](size_t ki, const char* impl) -> bool {
                if (!wantGpu || !selected(cfg.impls, impl)) {
                    return true;
                }
                GpuParams p { cfg.width, cfg.height, n.origin, n.origin, n.frequency, n.z, n.w,
                              n.seed, n.octaves, n.lacunarity, n.gain, n.weightedStrength, n.gridJitter, n.type };
                Result r;
                r.impl = impl;
                r.simd = "gpu";
                double ms = NAN;
                std::string err;
                if (!gpu.render(*kernels[ki], p, cfg.iters, r.img, ms, err)) {
                    std::fprintf(stderr, "%s %s: %s\n", a.name, impl, err.c_str());
                    return false;
                }
                r.gpu = ms / mpx;
                results.push_back(std::move(r));
                return true;
            };
            if (!runGpu(0, "gpu") || !runGpu(1, "gpu_fma")) {
                return 1;
            }

            auto runCpu = [&](const char* impl, const std::string& simd, const RowFn& f) {
                Result r;
                r.impl = impl;
                r.simd = simd;
                r.cpu1 = timeBand(f, cfg.width, cfg.band);
                r.cpuN = renderImage(f, cfg.width, cfg.height, cfg.threads, r.img) / mpx;
                results.push_back(std::move(r));
            };

            // a_base must run even when not selected, as the reference.
            for (size_t vi = 0; vi < std::size(variants); ++vi) {
                const Fn2PortVariant& v = *variants[vi];
                const bool isBase = vi == 0;
                if (!isBase && !selected(cfg.impls, variantImpl[vi])) {
                    continue;
                }
                if (v.needsV3 && !haveV3) {
                    std::printf("%-17s %-10s skipped: CPU lacks x86-64-v3\n", a.name, variantImpl[vi]);
                    continue;
                }
                const Fn2PortParams pp = portParams(n, cfg.width);
                runCpu(variantImpl[vi], v.needsV3 ? "v3" : "x86-64", [&v, &pp](int y, float* out) { v.row(pp, y, out); });
                // Row loop against per-pixel fbm(), every eighth row.
                Result& r = results.back();
                std::vector<float> scalar(cfg.width);
                long mismatch = 0;
                for (uint32_t y = 0; y < cfg.height; y += 8) {
                    v.rowScalar(pp, (int)y, scalar.data());
                    for (uint32_t x = 0; x < cfg.width; ++x) {
                        mismatch += std::memcmp(&scalar[x], &r.img[(size_t)y * cfg.width + x], sizeof(float)) != 0;
                    }
                }
                r.self = mismatch;
            }

            for (const auto& [impl, lib] : { std::pair { "b_strict", &kFn2Strict }, std::pair { "b_relaxed", &kFn2Relaxed } }) {
                if (!selected(cfg.impls, impl)) {
                    continue;
                }
                Fn2Handle h;
                h.lib = lib;
                h.h = lib->create(n.type, cfg.scale, n.octaves, n.lacunarity, n.gain, cfg.fn2Simd.c_str());
                if (!h.h) {
                    std::printf("%-17s %-10s skipped: FastNoise2 has no %s level here\n", a.name, impl, cfg.fn2Simd.c_str());
                    continue;
                }
                const uint32_t w = cfg.width;
                const float py0 = n.origin;
                void* handle = h.h;
                const int seed = (int)n.seed;
                const Noise nn = n;
                runCpu(impl, lib->simd(handle), [lib, handle, w, py0, seed, nn](int y, float* out) {
                    thread_local std::vector<float> xs, ys, zs, ws;
                    xs.resize(w);
                    ys.assign(w, py0 + ((float)y + 0.5f));
                    zs.assign(w, nn.z);
                    ws.assign(w, nn.w);
                    for (uint32_t x = 0; x < w; ++x) {
                        xs[x] = nn.origin + ((float)x + 0.5f);
                    }
                    lib->row(handle, out, (int)w, xs.data(), ys.data(), zs.data(), ws.data(), seed);
                });
            }

            if (selected(cfg.impls, "fnl")) {
                FastNoiseLite fnl(n.seed);
                fnl.SetFrequency(n.frequency);
                fnl.SetNoiseType(n.type == 0       ? FastNoiseLite::NoiseType_Perlin
                                     : n.type == 1 ? FastNoiseLite::NoiseType_OpenSimplex2S
                                                   : FastNoiseLite::NoiseType_Cellular);
                fnl.SetCellularDistanceFunction(FastNoiseLite::CellularDistanceFunction_Euclidean);
                fnl.SetCellularReturnType(FastNoiseLite::CellularReturnType_Distance);
                fnl.SetCellularJitter(n.gridJitter);
                if (a.fbm) {
                    fnl.SetFractalType(FastNoiseLite::FractalType_FBm);
                    fnl.SetFractalOctaves(n.octaves);
                    fnl.SetFractalLacunarity(n.lacunarity);
                    fnl.SetFractalGain(n.gain);
                    fnl.SetFractalWeightedStrength(n.weightedStrength);
                }
                const uint32_t w = cfg.width;
                const Noise nn = n;
                runCpu("fnl", "scalar3d", [&fnl, w, nn](int y, float* out) {
                    const float py = nn.origin + ((float)y + 0.5f);
                    for (uint32_t x = 0; x < w; ++x) {
                        out[x] = fnl.GetNoise(nn.origin + ((float)x + 0.5f), py, nn.z);
                    }
                });
            }

            for (const Result& r : results) {
                if (r.impl == "a_base" || r.impl == "gpu" || r.impl == "gpu_fma") {
                    refs[r.impl] = r.img;
                }
            }
            std::vector<float> diff;
            for (const Result& r : results) {
                Stats vs[3] = { { NAN, NAN, NAN }, { NAN, NAN, NAN }, { NAN, NAN, NAN } };
                const char* refNames[3] = { "a_base", "gpu", "gpu_fma" };
                if (r.impl != "fnl") {
                    for (int k = 0; k < 3; ++k) {
                        if (auto it = refs.find(refNames[k]); it != refs.end()) {
                            vs[k] = compare(r.img, it->second, diff);
                            const bool keyDiff = writeKeyExr && k == 0 && (r.impl == "gpu" || r.impl == "b_strict");
                            if (cfg.exrAll || keyDiff) {
                                writeExr(stem + "_" + r.impl + "_vs_" + refNames[k] + "_diff.exr", diff, cfg.width,
                                         cfg.height);
                            }
                        }
                    }
                }
                const bool keyImg = r.impl == "a_base" || r.impl == "b_strict" || r.impl == "fnl" || r.impl == "gpu";
                if (cfg.exrAll || (writeKeyExr && keyImg)) {
                    writeExr(stem + "_" + r.impl + ".exr", r.img, cfg.width, cfg.height);
                }
                const auto [mn, mx] = std::minmax_element(r.img.begin(), r.img.end());
                char self[16] = "";
                if (r.self >= 0) {
                    std::snprintf(self, sizeof(self), "%ld", r.self);
                }
                std::printf("%-17s %-10s %-7s %8s %9.2f %9.2f %9.3f %10.3g %10.3g %10.3g %6s\n", a.name, r.impl.c_str(),
                            r.simd.c_str(), originTag(n.origin).c_str(), r.cpu1, r.cpuN, r.gpu, vs[0].maxDiff,
                            vs[1].maxDiff, vs[2].maxDiff, self);
                std::fflush(stdout);
                csv << a.name << ',' << r.impl << ',' << r.simd << ',' << cfg.width << 'x' << cfg.height << ','
                    << originTag(n.origin) << ',' << cfg.scale << ',' << n.octaves << ',' << cfg.threads << ',' << r.cpu1
                    << ',' << r.cpuN << ',' << r.gpu << ',' << vs[0].maxDiff << ',' << vs[0].meanDiff << ','
                    << vs[1].maxDiff << ',' << vs[1].meanDiff << ',' << vs[2].maxDiff << ',' << vs[2].meanDiff << ','
                    << self << ',' << *mn << ',' << *mx << '\n';
            }
        }
    }
    return 0;
}
