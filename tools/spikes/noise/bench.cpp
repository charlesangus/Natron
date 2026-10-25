// Noise library spike harness: Cycles' 4D Perlin and 4D Voronoi F1 against OpenSimplex2S 4D and
// our own 4D Voronoi F1, each rendered on the CPU and on the GPU from the same pixel-to-noise
// mapping at several canonical origins.
//
//   NoiseSpikeBench [--size N|WxH] [--algos a,b,...] [--iters N] [--threads N] [--scale S]
//                   [--origins 0,1e5,...] [--out dir] [--no-gpu] [--dump N]
//
// --algos picks rows by name (cycles_perlin, cycles_voronoi_f1, os2s, own_voronoi_f1); --dump N
// prints the first N pixels of row 0 as CPU and GPU bit patterns.
//
// Per (algorithm, origin) it prints and appends to <out>/results.csv: CPU ms/Mpx on one thread
// (timed on a band of rows) and on all threads, GPU kernel ms/Mpx (median of timestamped
// dispatches), the max and mean CPU/GPU absolute difference, the share of bit-equal pixels, the
// value range, and the share of horizontally adjacent pixels with equal values, which rises when
// float32 runs out of fraction bits at large origins. EXRs of the CPU and GPU images and their
// difference go to <out>, Y channel, top row first.
//
// GPU_SPIKE_ALLOW_SOFTWARE=1 accepts a lavapipe device; without it a software device is refused,
// as in tools/gpu-spike.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "GpuBuffer.h"
#include "GpuDevice.h"
#include "GpuKernel.h"

#include "cycles/noise4d.h"
#include "cycles/voronoi4d.h"
#include "opensimplex/os2s4d.h"
#include "opensimplex/voronoi4d.h"

#include "cycles_perlin_spirv.h"
#include "cycles_voronoi_spirv.h"
#include "os2s_spirv.h"
#include "os2s_voronoi_spirv.h"

using namespace gpu;

namespace {

// The push-constant block of both .slang files.
struct SpikeParams {
    uint32_t width;
    uint32_t height;
    float originX;
    float originY;
    float scale;
    float z;
    float w;
    int32_t seed;
    float randomness;
};
static_assert(sizeof(SpikeParams) == 36);

using CpuFn = float (*)(const SpikeParams&, float x, float y);

struct Algo {
    const char* name;
    CpuFn cpu;
    const uint32_t* spirv;
    size_t spirvWords;
    const uint32_t* groupSize;
    uint32_t storageBuffers;
    uint32_t pushBytes;
};

#define SPIKE_KERNEL(t) t##_spirv, t##_spirv_words, t##_group_size, t##_storage_buffers, t##_push_bytes

const Algo kAlgos[] = {
    { "cycles_perlin",
      [](const SpikeParams& p, float x, float y) { return noisespike::cycles::snoise_4d(x, y, p.z, p.w); },
      SPIKE_KERNEL(cycles_perlin) },
    { "cycles_voronoi_f1",
      [](const SpikeParams& p, float x, float y) {
          return noisespike::cycles::voronoi_f1_4d(x, y, p.z, p.w, p.randomness);
      },
      SPIKE_KERNEL(cycles_voronoi) },
    { "os2s",
      [](const SpikeParams& p, float x, float y) {
          return noisespike::opensimplex::noise4_improve_xyz_improve_xy(p.seed, x, y, p.z, p.w);
      },
      SPIKE_KERNEL(os2s) },
    { "own_voronoi_f1",
      [](const SpikeParams& p, float x, float y) {
          return noisespike::opensimplex::voronoi_f1_4d(p.seed, x, y, p.z, p.w, p.randomness);
      },
      SPIKE_KERNEL(os2s_voronoi) },
};

struct Config {
    uint32_t width = 2048;
    uint32_t height = 2048;
    std::vector<std::string> algos;
    uint32_t dump = 0;
    int iters = 5;
    unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    float scale = 1.0f / 64.0f;
    std::vector<float> origins { 0.0f, 1e4f, 1e5f, 1e6f, 1e7f };
    std::string out = "build/spikes/noise";
    bool gpu = true;
};

double
nowMs()
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Same expression, same order, as noisePosition() in the .slang files.
void
renderRows(const Algo& a, const SpikeParams& p, uint32_t y0, uint32_t y1, float* out)
{
    for (uint32_t y = y0; y < y1; ++y) {
        float ny = (p.originY + ((float)y + 0.5f)) * p.scale;
        for (uint32_t x = 0; x < p.width; ++x) {
            float nx = (p.originX + ((float)x + 0.5f)) * p.scale;
            out[(size_t)y * p.width + x] = a.cpu(p, nx, ny);
        }
    }
}

double
renderCpu(const Algo& a, const SpikeParams& p, unsigned threads, std::vector<float>& out)
{
    out.assign((size_t)p.width * p.height, 0.0f);
    const double t0 = nowMs();
    std::vector<std::thread> ts;
    for (unsigned t = 0; t < threads; ++t) {
        ts.emplace_back([&, t] {
            for (uint32_t y = t * 8; y < p.height; y += threads * 8) {
                renderRows(a, p, y, std::min(p.height, y + 8), out.data());
            }
        });
    }
    for (std::thread& t : ts) {
        t.join();
    }
    return nowMs() - t0;
}

double
renderCpuBand(const Algo& a, const SpikeParams& p, uint32_t rows)
{
    std::vector<float> band((size_t)p.width * rows);
    const double t0 = nowMs();
    renderRows(a, p, 0, rows, band.data());
    return nowMs() - t0;
}

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

    bool makeKernel(const Algo& a, std::unique_ptr<GpuKernel>& out, std::string& err)
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

    // A device-local storage buffer holding `data`, filled through a staging copy.
    bool upload(std::span<const uint32_t> data, GpuBuffer& out, std::string& err)
    {
        const VkDeviceSize bytes = data.size_bytes();
        HostBuffer staging;
        if (!makeHostBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging, err)) {
            return false;
        }
        std::memcpy(staging.mapped, data.data(), bytes);
        vmaFlushAllocation(dev_->allocator(), staging.alloc, 0, VK_WHOLE_SIZE);
        if (GpuStatus s = GpuBuffer::createDeviceLocal(*dev_, bytes, kGpuBufferDefaultUsage, out); !s) {
            err = "table buffer: " + s.message;
            return false;
        }
        VkCommandBufferBeginInfo bi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkResetCommandBuffer(cmd_, 0) != VK_SUCCESS || vkBeginCommandBuffer(cmd_, &bi) != VK_SUCCESS) {
            err = "command buffer begin failed";
            return false;
        }
        VkBufferCopy r { 0, 0, bytes };
        vkCmdCopyBuffer(cmd_, staging.buffer, out.buffer(), 1, &r);
        VkMemoryBarrier2 mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        VkDependencyInfo dep { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd_, &dep);
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
        return true;
    }

    // Median kernel time over `iters` timed dispatches after one warm-up; the last dispatch is
    // read back into `out`. `tables` is bound after the output when the kernel takes a second
    // storage buffer.
    bool render(GpuKernel& k, const SpikeParams& p, const GpuBuffer& tables, int iters, std::vector<float>& out,
                double& kernelMs, std::string& err)
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
        const VkBuffer allBufs[] = { dst.buffer(), tables.buffer() };
        const std::span<const VkBuffer> bufs(allBufs, k.desc().storageBufferCount);
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
            if (GpuStatus s = k.record(cmd_, bufs, std::as_bytes(std::span(&p, 1)), { p.width, p.height, 1 }); !s) {
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
flatShare(const std::vector<float>& px, uint32_t w, uint32_t h)
{
    size_t flat = 0;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 1; x < w; ++x) {
            flat += px[(size_t)y * w + x] == px[(size_t)y * w + x - 1];
        }
    }
    return (double)flat / ((double)(w - 1) * h);
}

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
        } else if (a == "--dump") {
            cfg.dump = (uint32_t)std::stoul(next());
        } else if (a == "--iters") {
            cfg.iters = std::stoi(next());
        } else if (a == "--threads") {
            cfg.threads = std::max(1u, (unsigned)std::stoul(next()));
        } else if (a == "--scale") {
            cfg.scale = std::stof(next());
        } else if (a == "--out") {
            cfg.out = next();
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
    return cfg.width > 0 && cfg.height > 0 && cfg.iters > 0;
}

} // namespace

int
main(int argc, char** argv)
{
    Config cfg;
    if (!parseArgs(argc, argv, cfg)) {
        std::fprintf(stderr, "usage: NoiseSpikeBench [--size N|WxH] [--algos a,b,...] [--iters N] [--threads N] "
                             "[--scale S] [--origins 0,1e5,...] [--out dir] [--no-gpu] [--dump N]\n");
        return 2;
    }
    std::filesystem::create_directories(cfg.out);

    Gpu gpu;
    if (cfg.gpu) {
        std::string err;
        if (!gpu.init(err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        std::printf("device: %s\n", gpu.info().name.c_str());
    }
    std::unique_ptr<GpuKernel> kernels[std::size(kAlgos)];
    GpuBuffer tables;
    if (cfg.gpu) {
        std::string err;
        if (!gpu.upload(noisespike::opensimplex::packedTables(), tables, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        for (size_t i = 0; i < std::size(kAlgos); ++i) {
            std::string err;
            if (!gpu.makeKernel(kAlgos[i], kernels[i], err)) {
                std::fprintf(stderr, "%s\n", err.c_str());
                return 1;
            }
        }
    }

    const bool newCsv = !std::filesystem::exists(cfg.out + "/results.csv");
    std::ofstream csv(cfg.out + "/results.csv", std::ios::app);
    if (newCsv) {
        csv << "algo,size,origin,scale,cpu1_ms_per_mpx,cpuN_ms_per_mpx,threads,gpu_ms_per_mpx,max_abs_diff,"
               "mean_abs_diff,bit_equal,min,max,flat_share\n";
    }
    std::printf("%-18s %10s %10s %10s %10s %11s %11s %9s %9s %9s %7s\n", "algo", "origin", "cpu1ms/Mpx",
                "cpuNms/Mpx", "gpums/Mpx", "maxdiff", "meandiff", "bitequal", "min", "max", "flat");

    const double mpx = (double)cfg.width * cfg.height * 1e-6;
    const uint32_t bandRows = std::min<uint32_t>(cfg.height, 128);
    for (size_t ai = 0; ai < std::size(kAlgos); ++ai) {
        const Algo& a = kAlgos[ai];
        if (!cfg.algos.empty() && std::find(cfg.algos.begin(), cfg.algos.end(), a.name) == cfg.algos.end()) {
            continue;
        }
        for (float origin : cfg.origins) {
            SpikeParams p { cfg.width, cfg.height, origin, origin, cfg.scale, 0.37f, 0.61f, 2000, 1.0f };
            const double cpu1 = renderCpuBand(a, p, bandRows) / ((double)cfg.width * bandRows * 1e-6);
            std::vector<float> cpu;
            const double cpuN = renderCpu(a, p, cfg.threads, cpu) / mpx;
            const auto [mn, mx] = std::minmax_element(cpu.begin(), cpu.end());
            const double flat = flatShare(cpu, cfg.width, cfg.height);
            const std::string stem = cfg.out + "/" + a.name + "_o" + originTag(origin);
            writeExr(stem + "_cpu.exr", cpu, cfg.width, cfg.height);

            double gpuMs = NAN;
            Stats st { NAN, NAN, NAN };
            if (cfg.gpu) {
                std::vector<float> g;
                std::string err;
                if (!gpu.render(*kernels[ai], p, tables, cfg.iters, g, gpuMs, err)) {
                    std::fprintf(stderr, "%s: %s\n", a.name, err.c_str());
                    return 1;
                }
                std::vector<float> diff;
                st = compare(cpu, g, diff);
                writeExr(stem + "_gpu.exr", g, cfg.width, cfg.height);
                writeExr(stem + "_diff.exr", diff, cfg.width, cfg.height);
                for (uint32_t x = 0; x < std::min(cfg.dump, cfg.width); ++x) {
                    uint32_t cb, gb;
                    std::memcpy(&cb, &cpu[x], sizeof(cb));
                    std::memcpy(&gb, &g[x], sizeof(gb));
                    std::printf("  %s o%s x=%u cpu=%.9g (%08X) gpu=%.9g (%08X)\n", a.name, originTag(origin).c_str(), x,
                                cpu[x], cb, g[x], gb);
                }
            }
            const double gpuPerMpx = gpuMs / mpx;
            std::printf("%-18s %10s %10.2f %10.2f %10.3f %11.3g %11.3g %9.6f %9.4f %9.4f %7.4f\n", a.name,
                        originTag(origin).c_str(), cpu1, cpuN, gpuPerMpx, st.maxDiff, st.meanDiff, st.bitEqual, *mn,
                        *mx, flat);
            std::fflush(stdout);
            csv << a.name << ',' << cfg.width << 'x' << cfg.height << ',' << originTag(origin) << ',' << cfg.scale << ',' << cpu1 << ','
                << cpuN << ',' << cfg.threads << ',' << gpuPerMpx << ',' << st.maxDiff << ',' << st.meanDiff << ','
                << st.bitEqual << ',' << *mn << ',' << *mx << ',' << flat << '\n';
        }
    }
    return 0;
}
