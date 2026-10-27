#include "spikes.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>

#include "BlurKernels.h"
#include "GpuBlur.h"
#include "GpuMorphology.h"
#include "GpuRuntime.h"

#include "bloom_down_spirv.h"
#include "bloom_up_spirv.h"

namespace bloomspike {

namespace {

    using Natron::BlurKernels::LineFilter;

    // Mirrors DownParams in kernels/down.slang.
    struct DownParams {
        int32_t srcX1, srcY1;
        uint32_t srcStride;
        int32_t validX1, validY1, validX2, validY2;
        int32_t dstX1, dstY1;
        uint32_t dstWidth, dstHeight, dstStride, channels, binomial;
    };
    static_assert(sizeof(DownParams) == 56);

    // Mirrors UpParams in kernels/up.slang.
    struct UpParams {
        int32_t srcX1, srcY1;
        uint32_t srcStride;
        int32_t validX1, validY1, validX2, validY2;
        int32_t dstX1, dstY1;
        uint32_t dstWidth, dstHeight, dstStride, channels, factor, bicubic, first;
        float divisor;
    };
    static_assert(sizeof(UpParams) == 68);

    gpu::GpuBlurRect toGpu(const Rect& r)
    {
        return { r.x1, r.y1, r.x2, r.y2 };
    }

    std::size_t floatsOf(const Rect& r, int nc)
    {
        return std::size_t(r.width()) * r.height() * nc;
    }

    using Recorder = std::function<bool(gpu::GpuRecordContext&, std::string&)>;

    class Gpu {
    public:
        ~Gpu()
        {
            if (query_ != VK_NULL_HANDLE) {
                vkDestroyQueryPool(runtime_->device().device(), query_, nullptr);
            }
        }

        bool init(std::string& err)
        {
            if (gpu::GpuStatus s = gpu::GpuRuntime::create(gpu::GpuRuntimeOptions {}, runtime_); !s) {
                err = "runtime: " + s.message;
                return false;
            }
            gpu::GpuDevice& dev = runtime_->device();
            const char* allowSoftware = std::getenv("GPU_SPIKE_ALLOW_SOFTWARE");
            if (dev.info().type == VK_PHYSICAL_DEVICE_TYPE_CPU && !(allowSoftware && allowSoftware[0] == '1')) {
                err = "found only a software device (" + dev.info().name + "); GPU_SPIKE_ALLOW_SOFTWARE=1 accepts it";
                return false;
            }
            if (bloom_down_push_bytes != sizeof(DownParams) || bloom_up_push_bytes != sizeof(UpParams)
                || bloom_down_storage_buffers != 2 || bloom_up_storage_buffers != 2) {
                err = "push-constant or binding layout of the Slang kernels differs from the C++ mirrors";
                return false;
            }
            downId_ = runtime_->registerKernel({ bloom_down_spirv, bloom_down_spirv_words, bloom_down_entry, bloom_down_storage_buffers, bloom_down_push_bytes, { bloom_down_group_size[0], bloom_down_group_size[1], bloom_down_group_size[2] } });
            upId_ = runtime_->registerKernel({ bloom_up_spirv, bloom_up_spirv_words, bloom_up_entry, bloom_up_storage_buffers, bloom_up_push_bytes, { bloom_up_group_size[0], bloom_up_group_size[1], bloom_up_group_size[2] } });

            uint32_t n = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(dev.physicalDevice(), &n, nullptr);
            std::vector<VkQueueFamilyProperties> fams(n);
            vkGetPhysicalDeviceQueueFamilyProperties(dev.physicalDevice(), &n, fams.data());
            validBits_ = fams[dev.queueFamily(gpu::QueueKind::Compute)].timestampValidBits;
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(dev.physicalDevice(), &props);
            periodNs_ = props.limits.timestampPeriod;
            if (validBits_ != 0) {
                VkQueryPoolCreateInfo qi { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
                qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
                qi.queryCount = 2;
                if (gpu::GpuStatus s = dev.check(vkCreateQueryPool(dev.device(), &qi, nullptr, &query_), "vkCreateQueryPool"); !s) {
                    err = s.message;
                    return false;
                }
            }
            return true;
        }

        const gpu::GpuDeviceInfo& info() const { return runtime_->device().info(); }
        bool timestamps() const { return query_ != VK_NULL_HANDLE; }

        bool acquire(std::size_t floats, gpu::GpuBufferHandle& out, std::string& err)
        {
            if (gpu::GpuStatus s = runtime_->acquire(std::max<std::size_t>(floats, 4) * sizeof(float), out); !s) {
                err = "acquire: " + s.message;
                return false;
            }
            return true;
        }

        bool upload(const Image& img, gpu::GpuBuffer& dst, std::string& err)
        {
            gpu::TimelineValue done;
            const gpu::HostRange range { const_cast<float*>(img.px.data()), img.px.size() * sizeof(float) };
            gpu::GpuStatus s = runtime_->upload(range, dst, 0, done);
            if (s) {
                s = runtime_->wait(done);
            }
            if (!s) {
                err = "upload: " + s.message;
            }
            return bool(s);
        }

        bool download(gpu::GpuBuffer& src, Image& img, std::string& err)
        {
            gpu::TimelineValue done;
            const gpu::HostRange range { img.px.data(), img.px.size() * sizeof(float) };
            gpu::GpuStatus s = runtime_->download(src, 0, range, done);
            if (s) {
                s = runtime_->wait(done);
            }
            if (!s) {
                err = "download: " + s.message;
            }
            return bool(s);
        }

        // Records and submits once; `ms` gets the kernel time between two timestamps around the
        // recorded work, or -1 without timestamp support.
        bool run(const Recorder& record, double& ms, std::string& err)
        {
            ms = -1.;
            std::unique_ptr<gpu::GpuRecordContext> ctx;
            if (gpu::GpuStatus s = runtime_->newRecordContext(ctx); !s) {
                err = "record context: " + s.message;
                return false;
            }
            if (query_) {
                ctx->resetQueries(query_, 0, 2);
                ctx->writeTimestamp(query_, 0);
            }
            if (!record(*ctx, err)) {
                return false;
            }
            if (query_) {
                ctx->writeTimestamp(query_, 1);
            }
            if (gpu::GpuStatus s = ctx->submitAndWait(); !s) {
                err = "submit: " + s.message;
                return false;
            }
            if (query_) {
                uint64_t v[2] = { 0, 0 };
                gpu::GpuDevice& dev = runtime_->device();
                if (gpu::GpuStatus s = dev.check(vkGetQueryPoolResults(dev.device(), query_, 0, 2, sizeof(v), v, sizeof(uint64_t),
                                                                       VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
                                                 "vkGetQueryPoolResults");
                    !s) {
                    err = s.message;
                    return false;
                }
                const uint64_t mask = validBits_ >= 64 ? ~0ull : ((1ull << validBits_) - 1);
                ms = double((v[1] - v[0]) & mask) * periodNs_ * 1e-6;
            }
            return true;
        }

        bool recordDown(gpu::GpuRecordContext& ctx, const gpu::GpuBuffer* src, const Rect& srcBounds, const Rect& valid,
                        const gpu::GpuBuffer* dst, const Rect& dstRect, int nc, Down down, std::string& err)
        {
            if (dstRect.empty()) {
                return true;
            }
            DownParams p {};
            p.srcX1 = srcBounds.x1;
            p.srcY1 = srcBounds.y1;
            p.srcStride = uint32_t(srcBounds.width() * nc);
            p.validX1 = valid.x1;
            p.validY1 = valid.y1;
            p.validX2 = valid.x2;
            p.validY2 = valid.y2;
            p.dstX1 = dstRect.x1;
            p.dstY1 = dstRect.y1;
            p.dstWidth = uint32_t(dstRect.width());
            p.dstHeight = uint32_t(dstRect.height());
            p.dstStride = uint32_t(dstRect.width() * nc);
            p.channels = uint32_t(nc);
            p.binomial = down == Down::binomial ? 1u : 0u;
            if (gpu::GpuStatus s = ctx.dispatch(downId_, { src, dst }, p, { p.dstWidth, p.dstHeight, 1u }); !s) {
                err = "down: " + s.message;
                return false;
            }
            return true;
        }

        // dst covers dstRect exactly.
        bool recordUp(gpu::GpuRecordContext& ctx, const gpu::GpuBuffer* src, const Rect& srcBounds, const Rect& valid,
                      const gpu::GpuBuffer* dst, const Rect& dstRect, int nc, int factor, Up up, bool first, float divisor,
                      std::string& err)
        {
            UpParams p {};
            p.srcX1 = srcBounds.x1;
            p.srcY1 = srcBounds.y1;
            p.srcStride = uint32_t(std::max(0, srcBounds.width()) * nc);
            p.validX1 = valid.x1;
            p.validY1 = valid.y1;
            p.validX2 = valid.x2;
            p.validY2 = valid.y2;
            p.dstX1 = dstRect.x1;
            p.dstY1 = dstRect.y1;
            p.dstWidth = uint32_t(dstRect.width());
            p.dstHeight = uint32_t(dstRect.height());
            p.dstStride = uint32_t(dstRect.width() * nc);
            p.channels = uint32_t(nc);
            p.factor = uint32_t(factor);
            p.bicubic = up == Up::bicubic ? 1u : 0u;
            p.first = first ? 1u : 0u;
            p.divisor = divisor;
            if (gpu::GpuStatus s = ctx.dispatch(upId_, { src, dst }, p, { p.dstWidth, p.dstHeight, 1u }); !s) {
                err = "up: " + s.message;
                return false;
            }
            return true;
        }

        // The pyramid over the whole of `rod`, from `input` (rod's pixels) into `out` (rod's pixels).
        bool recordPyramid(gpu::GpuRecordContext& ctx, const PyramidPlan& plan, const gpu::GpuBuffer* input, const Rect& rod,
                           const gpu::GpuBuffer* out, int nc, std::string& err)
        {
            const PyramidRegions g = planRegions(plan, rod, rod);
            std::vector<const gpu::GpuBuffer*> levels(plan.levels, nullptr);
            std::vector<Rect> bounds(plan.levels);
            levels[0] = input;
            bounds[0] = rod;
            for (int j = 1; j < plan.levels; ++j) {
                gpu::GpuBuffer* b = nullptr;
                if (gpu::GpuStatus s = ctx.scratch(std::max<std::size_t>(floatsOf(g.level[j], nc), 4) * sizeof(float), b); !s) {
                    err = "level scratch: " + s.message;
                    return false;
                }
                levels[j] = b;
                bounds[j] = g.level[j];
                if (!recordDown(ctx, levels[j - 1], bounds[j - 1], g.level[j - 1], b, g.level[j], nc, plan.variant.down, err)) {
                    return false;
                }
                ctx.computeBarrier();
            }
            std::vector<gpu::GpuBuffer*> blurred(plan.blurs.size(), nullptr);
            for (std::size_t i = 0; i < plan.blurs.size(); ++i) {
                const LevelBlur& b = plan.blurs[i];
                const Rect& o = g.out[i];
                if (gpu::GpuStatus s = ctx.scratch(std::max<std::size_t>(floatsOf(o, nc), 4) * sizeof(float), blurred[i]); !s) {
                    err = "blur scratch: " + s.message;
                    return false;
                }
                if (o.empty()) {
                    continue;
                }
                gpu::GpuSeparableBlur blur;
                blur.src.buffer = levels[b.level];
                blur.src.bounds = toGpu(bounds[b.level]);
                blur.src.strideFloats = uint32_t(bounds[b.level].width() * nc);
                blur.srcValid = toGpu(g.data[i]);
                blur.dst.buffer = blurred[i];
                blur.dst.bounds = toGpu(o);
                blur.dst.strideFloats = uint32_t(o.width() * nc);
                blur.output = toGpu(o);
                blur.domain = toGpu(grow(o, b.radius()));
                blur.channels = uint32_t(nc);
                blur.weightsX = b.weights;
                blur.weightsY = b.weights;
                if (gpu::GpuStatus s = gpu::recordSeparableBlur(ctx, blur); !s) {
                    err = "recordSeparableBlur: " + s.message;
                    return false;
                }
            }
            ctx.computeBarrier();
            const std::size_t n = plan.blurs.size();
            for (std::size_t i = 0; i < n; ++i) {
                const int factor = 1 << plan.blurs[i].level;
                if (!recordUp(ctx, blurred[i], g.out[i], g.out[i], out, rod, nc, factor, plan.variant.up, i == 0,
                              i + 1 == n ? float(n) : 0.f, err)) {
                    return false;
                }
                ctx.computeBarrier();
            }
            return true;
        }

        // Candidate D: `count` full-resolution quasi-Gaussian blurs through the engine's line IIR,
        // each added to `out` as soon as it is done, so one full-frame temporary is reused.
        bool recordD(gpu::GpuRecordContext& ctx, const BloomParams& p, const gpu::GpuBuffer* input, const Rect& rod,
                     const gpu::GpuBuffer* tmp, const gpu::GpuBuffer* out, int nc, std::string& err)
        {
            gpu::GpuBuffer* window = nullptr;
            if (gpu::GpuStatus s = gpu::recordFilterWindow(ctx, toGpu(rod), toGpu(rod), 0, 0, window); !s) {
                err = "recordFilterWindow: " + s.message;
                return false;
            }
            const uint32_t stride = uint32_t(rod.width() * nc);
            for (int i = 0; i < p.count; ++i) {
                const gpu::GpuLineFilter f = Natron::BlurKernels::toGpuLineFilter(
                    LineFilter::forFilter(Natron::BlurKernels::eFilterQuasiGaussian, blurSigma(p, i), 0, false));
                gpu::GpuLineBlur blur;
                blur.src = { input, 0, toGpu(rod), stride };
                blur.srcValid = toGpu(rod);
                blur.dst = { tmp, 0, toGpu(rod), stride };
                blur.output = toGpu(rod);
                blur.domain = toGpu(rod);
                blur.window = window;
                blur.channels = uint32_t(nc);
                blur.filterX = f;
                blur.filterY = f;
                if (gpu::GpuStatus s = gpu::recordLineBlur(ctx, blur); !s) {
                    err = "recordLineBlur: " + s.message;
                    return false;
                }
                ctx.computeBarrier();
                if (!recordUp(ctx, tmp, rod, rod, out, rod, nc, 1, Up::bilinear, i == 0, i + 1 == p.count ? float(p.count) : 0.f,
                              err)) {
                    return false;
                }
                ctx.computeBarrier();
            }
            return true;
        }

    private:
        std::unique_ptr<gpu::GpuRuntime> runtime_;
        gpu::GpuKernelId downId_ {};
        gpu::GpuKernelId upId_ {};
        VkQueryPool query_ = VK_NULL_HANDLE;
        uint32_t validBits_ = 0;
        double periodNs_ = 1.;
    };

    struct Buffers {
        gpu::GpuBufferHandle input, out, tmp;
    };

    bool prepare(Gpu& gpu, const Image& src, Buffers& b, std::string& err)
    {
        const std::size_t floats = src.px.size();
        return gpu.acquire(floats, b.input, err) && gpu.acquire(floats, b.out, err) && gpu.acquire(floats, b.tmp, err)
            && gpu.upload(src, b.input.buffer(), err);
    }

    bool checkSpike(const Config& cfg, Gpu& gpu, Pool& pool)
    {
        const int width = 480, height = 270, nc = 4;
        const Rect rod { 0, 0, width, height };
        const Image src = makeInput("plate", width, height, nc);
        std::printf("\n== Spike 2a: GPU against CPU, plate %dx%d, %d channels, count %d (pass: max rel <= 1e-4) ==\n", width,
                    height, nc, cfg.timingCount);
        std::printf("%5s %-16s %-24s %9s %9s %s\n", "size", "method", "against", "max_rel", "rms_rel", "result");
        Buffers b;
        std::string err;
        if (!prepare(gpu, src, b, err)) {
            std::printf("GPU check: %s\n", err.c_str());
            return false;
        }
        bool allPass = true;
        Image got(rod, nc);
        const auto report = [&](double size, const std::string& method, const char* against, const Image& ref) {
            const ErrorStats e = compareImages(ref, got);
            const bool pass = e.maxRel <= 1e-4;
            allPass = allPass && pass;
            std::printf("%5g %-16s %-24s %9.2e %9.2e %s\n", size, method.c_str(), against, e.maxRel, e.rmsRel, pass ? "pass" : "FAIL");
        };
        for (double size : cfg.timingSizes) {
            const BloomParams p { size, cfg.ratio, cfg.timingCount, cfg.sigmaMin };
            double ms = 0.;
            const Image refD = combine(referenceBlurs(src, RefKind::iir, p, p.count, pool), p.count);
            const bool ranD = gpu.run([&](gpu::GpuRecordContext& ctx, std::string& e) {
                return gpu.recordD(ctx, p, &b.input.buffer(), rod, &b.tmp.buffer(), &b.out.buffer(), nc, e);
            },
                                      ms, err);
            if (!ranD || !gpu.download(b.out.buffer(), got, err)) {
                std::printf("%5g %-16s %s\n", size, "D", err.c_str());
                allPass = false;
            } else {
                report(size, "D (line IIR)", "CPU ref-iir sum", got);
            }
            for (const Variant& v : cfg.variants) {
                const PyramidPlan plan = makePlan(p, v);
                const Image ref = renderPyramid(plan, src, rod, rod, pool);
                const bool ran = gpu.run([&](gpu::GpuRecordContext& ctx, std::string& e) {
                    return gpu.recordPyramid(ctx, plan, &b.input.buffer(), rod, &b.out.buffer(), nc, e);
                },
                                         ms, err);
                if (!ran || !gpu.download(b.out.buffer(), got, err)) {
                    std::printf("%5g %-16s %s\n", size, variantName(v).c_str(), err.c_str());
                    allPass = false;
                    continue;
                }
                report(size, variantName(v), "CPU pyramid", ref);
            }
        }
        std::printf("GPU check: %s\n", allPass ? "all pass" : "FAILURES");
        return allPass;
    }

    void timingSpike(const Config& cfg, Gpu& gpu)
    {
        std::printf("\n== Spike 2b: GPU kernel ms per frame (timestamps, median of %d after a warm-up), 4 channels, count %d ==\n",
                    cfg.iters, cfg.timingCount);
        if (!gpu.timestamps()) {
            std::printf("The compute queue has no timestamps; skipped.\n");
            return;
        }
        std::printf("%-5s %5s %-16s %-24s %10s\n", "res", "size", "method", "levels", "gpu ms");
        FILE* csv = openCsv(cfg, "gpu_timing.csv", "device,res,size,count,method,ms");
        const int nc = 4;
        for (const std::string& res : cfg.resolutions) {
            int width = 0, height = 0;
            if (!resolutionSize(res, width, height)) {
                continue;
            }
            const Rect rod { 0, 0, width, height };
            const Image src = makeInput("plate", width, height, nc);
            Buffers b;
            std::string err;
            if (!prepare(gpu, src, b, err)) {
                std::printf("%-5s %s\n", res.c_str(), err.c_str());
                continue;
            }
            for (double size : cfg.timingSizes) {
                const BloomParams p { size, cfg.ratio, cfg.timingCount, cfg.sigmaMin };
                const auto measure = [&](const std::string& method, const std::string& levels, const Recorder& record) {
                    std::vector<double> times;
                    double ms = 0.;
                    for (int it = 0; it <= cfg.iters; ++it) {
                        if (!gpu.run(record, ms, err)) {
                            std::printf("%-5s %5g %-16s %s\n", res.c_str(), size, method.c_str(), err.c_str());
                            return;
                        }
                        if (it > 0) {
                            times.push_back(ms);
                        }
                    }
                    std::sort(times.begin(), times.end());
                    const double med = times[times.size() / 2];
                    std::printf("%-5s %5g %-16s %-24s %10.2f\n", res.c_str(), size, method.c_str(), levels.c_str(), med);
                    std::fprintf(csv, "\"%s\",%s,%g,%d,%s,%.4f\n", gpu.info().name.c_str(), res.c_str(), size, p.count,
                                 method.c_str(), med);
                    std::fflush(stdout);
                };
                measure("D (line IIR)", "-", [&](gpu::GpuRecordContext& ctx, std::string& e) {
                    return gpu.recordD(ctx, p, &b.input.buffer(), rod, &b.tmp.buffer(), &b.out.buffer(), nc, e);
                });
                for (const Variant& v : cfg.variants) {
                    const PyramidPlan plan = makePlan(p, v);
                    measure(variantName(v), levelList(plan, p.count), [&](gpu::GpuRecordContext& ctx, std::string& e) {
                        return gpu.recordPyramid(ctx, plan, &b.input.buffer(), rod, &b.out.buffer(), nc, e);
                    });
                }
            }
        }
        std::fclose(csv);
    }

} // namespace

bool
runGpuSpike(const Config& cfg)
{
    Gpu gpu;
    std::string err;
    if (!gpu.init(err)) {
        std::printf("\nGPU spike skipped: %s\n", err.c_str());
        return false;
    }
    std::printf("\nGPU: %s%s\n", gpu.info().name.c_str(),
                gpu.info().type == VK_PHYSICAL_DEVICE_TYPE_CPU ? " (software: correctness only, timings meaningless)" : "");
    Pool pool(cfg.threads);
    bool ok = true;
    if (cfg.gpuCheck) {
        ok = checkSpike(cfg, gpu, pool);
    }
    if (cfg.gpuTiming) {
        timingSpike(cfg, gpu);
    }
    return ok;
}

} // namespace bloomspike
