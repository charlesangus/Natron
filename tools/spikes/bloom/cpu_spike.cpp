#include "spikes.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>

#include "BlurKernels.h"

namespace bloomspike {

using Natron::BlurKernels::LineFilter;
using Natron::BlurKernels::LineScratch;

namespace {

    uint32_t hash32(uint32_t x)
    {
        x ^= x >> 16;
        x *= 0x7feb352du;
        x ^= x >> 15;
        x *= 0x846ca68bu;
        x ^= x >> 16;
        return x;
    }

    LineScratch& threadScratch()
    {
        static thread_local LineScratch scratch;
        return scratch;
    }

    struct Stage {
        int units = 0;
        std::function<void(int)> fn;
    };

    // out[x] = sum over the line of w[|t - x|] in[t], in double: a Dirichlet FIR whose radius
    // reaches past both ends, where every tap beyond the line reads zero.
    void truncatedFir(const float* in, float* out, int n, const std::vector<float>& w)
    {
        for (int x = 0; x < n; ++x) {
            double acc = 0.;
            for (int t = 0; t < n; ++t) {
                acc += double(w[std::size_t(std::abs(t - x))]) * in[t];
            }
            out[x] = float(acc);
        }
    }

    // Rows then columns of `img`. Without engineFir, a FIR whose radius covers a whole line runs
    // as truncatedFir, which equals LineFilter's zero-padded result at O(n^2) per line instead of
    // O(n * radius).
    std::vector<Stage> filterStages(Image& img, RefKind kind, float sigma, bool engineFir)
    {
        const int width = img.bounds.width();
        const int height = img.bounds.height();
        const int nc = img.nc;
        const std::size_t stride = img.stride();
        auto filter = std::make_shared<LineFilter>(LineFilter::forFilter(
            kind == RefKind::fir ? Natron::BlurKernels::eFilterFIRGaussian : Natron::BlurKernels::eFilterQuasiGaussian,
            sigma, 0, false));
        std::vector<Stage> stages;
        if (filter->isIdentity()) {
            return stages;
        }
        const int radius = kind == RefKind::fir ? LineFilter::firGaussianRadius(sigma) : 0;
        auto weights = std::make_shared<std::vector<float>>(LineFilter::firGaussianWeights(sigma));
        float* px = img.px.data();

        if (kind == RefKind::fir && !engineFir && radius >= width) {
            stages.push_back({ height, [=](int y) {
                                  std::vector<float> in(width), out(width);
                                  float* row = px + std::size_t(y) * stride;
                                  for (int c = 0; c < nc; ++c) {
                                      for (int x = 0; x < width; ++x) {
                                          in[x] = row[std::size_t(x) * nc + c];
                                      }
                                      truncatedFir(in.data(), out.data(), width, *weights);
                                      for (int x = 0; x < width; ++x) {
                                          row[std::size_t(x) * nc + c] = out[x];
                                      }
                                  }
                              } });
        } else {
            stages.push_back({ height, [=](int y) {
                                  float* row = px + std::size_t(y) * stride;
                                  for (int c = 0; c < nc; ++c) {
                                      filter->apply(row + c, width, nc, threadScratch());
                                  }
                              } });
        }

        if (kind == RefKind::fir && !engineFir && radius >= height) {
            stages.push_back({ int(stride), [=](int line) {
                                  std::vector<float> in(height), out(height);
                                  for (int y = 0; y < height; ++y) {
                                      in[y] = px[std::size_t(y) * stride + line];
                                  }
                                  truncatedFir(in.data(), out.data(), height, *weights);
                                  for (int y = 0; y < height; ++y) {
                                      px[std::size_t(y) * stride + line] = out[y];
                                  }
                              } });
        } else {
            const int block = LineFilter::kColumnBlock;
            const int blocks = int((stride + block - 1) / block);
            stages.push_back({ blocks, [=](int b) {
                                  const std::size_t first = std::size_t(b) * block;
                                  const int count = int(std::min<std::size_t>(block, stride - first));
                                  filter->applyColumns(px + first, height, std::ptrdiff_t(stride), count, threadScratch());
                              } });
        }
        return stages;
    }

    struct StageClock {
        double ms = 0.;
        bool extrapolated = false;
    };

    // Runs one unit per thread first; when the whole stage would exceed the budget, times only as
    // many leading units as fit and scales up, the units of a stage costing the same.
    void timeStage(Pool& pool, const Stage& stage, double budgetMs, StageClock& clock)
    {
        const int n = stage.units;
        if (n <= 0) {
            return;
        }
        const int p = std::min<int>(n, int(pool.size()));
        double t0 = nowMs();
        pool.run(p, stage.fn);
        const double tp = nowMs() - t0;
        if (p == n) {
            clock.ms += tp;
            return;
        }
        if (tp * n / p <= budgetMs) {
            t0 = nowMs();
            pool.run(n, stage.fn);
            clock.ms += nowMs() - t0;
            return;
        }
        int m = int(budgetMs / std::max(tp, 1e-3) * p);
        m = std::clamp(m / p * p, p, n);
        t0 = nowMs();
        pool.run(m, stage.fn);
        clock.ms += (nowMs() - t0) * n / m;
        clock.extrapolated = clock.extrapolated || m < n;
    }

    StageClock timeReference(const Image& src, RefKind kind, const BloomParams& p, Pool& pool, double budgetMs)
    {
        StageClock clock;
        Image tmp(src.bounds, src.nc);
        Image acc(src.bounds, src.nc);
        const int height = src.bounds.height();
        const std::size_t stride = src.stride();
        const Stage copy { height, [&](int row) {
                              std::memcpy(tmp.px.data() + row * stride, src.px.data() + row * stride, stride * sizeof(float));
                          } };
        const Stage add { height, [&](int row) {
                             float* a = acc.px.data() + row * stride;
                             const float* t = tmp.px.data() + row * stride;
                             for (std::size_t i = 0; i < stride; ++i) {
                                 a[i] += t[i];
                             }
                         } };
        const float count = float(p.count);
        const Stage divide { height, [&](int row) {
                                float* a = acc.px.data() + row * stride;
                                for (std::size_t i = 0; i < stride; ++i) {
                                    a[i] = a[i] / count;
                                }
                            } };
        for (int i = 0; i < p.count; ++i) {
            timeStage(pool, copy, budgetMs, clock);
            for (const Stage& s : filterStages(tmp, kind, blurSigma(p, i), true)) {
                timeStage(pool, s, budgetMs, clock);
            }
            timeStage(pool, add, budgetMs, clock);
        }
        timeStage(pool, divide, budgetMs, clock);
        return clock;
    }

    double median(std::vector<double> v)
    {
        std::sort(v.begin(), v.end());
        return v.empty() ? 0. : v[v.size() / 2];
    }

    void qualitySpike(const Config& cfg, Pool& pool)
    {
        const int width = cfg.qualityWidth;
        const int height = cfg.qualityHeight;
        const Rect rod { 0, 0, width, height };
        const int maxCount = *std::max_element(cfg.counts.begin(), cfg.counts.end());
        std::printf("\n== Spike 1a: quality, %dx%d, 1 channel, black outside, sigma_min %.2f, ratio %.2f ==\n", width,
                    height, cfg.sigmaMin, cfg.ratio);
        std::printf("Errors relative to the reference's peak |ref| (max, rms); px = max |d| / (|ref| + 1e-3 peak).\n");
        std::printf("Row 'iir(D)' is the quasi-Gaussian sum against the FIR sum. levels: decimation k of each blur.\n");
        std::printf("%-6s %5s %3s %-16s %-24s | %9s %9s %9s | %9s %9s\n", "input", "size", "cnt", "method", "levels",
                    "fir_max", "fir_rms", "fir_px", "iir_max", "iir_rms");
        FILE* csv = openCsv(cfg, "quality.csv",
                            "input,size,count,method,levels,decimated,fir_max,fir_rms,fir_px,iir_max,iir_rms,iir_px");

        struct Worst {
            double firRms = 0., firMax = 0., iirRms = 0.;
            int cases = 0;
        };
        std::map<std::string, Worst> worst;

        for (const char* inputName : { "point", "edge", "plate" }) {
            const Image src = makeInput(inputName, width, height, 1);
            for (double size : cfg.sizes) {
                const BloomParams p { size, cfg.ratio, maxCount, cfg.sigmaMin };
                const std::vector<Image> firs = referenceBlurs(src, RefKind::fir, p, maxCount, pool);
                const std::vector<Image> iirs = referenceBlurs(src, RefKind::iir, p, maxCount, pool);
                std::vector<std::vector<Image>> pyramids;
                std::vector<PyramidPlan> plans;
                for (const Variant& v : cfg.variants) {
                    plans.push_back(makePlan(p, v));
                    pyramids.emplace_back();
                    renderPyramid(plans.back(), src, rod, rod, pool, &pyramids.back());
                }
                for (int count : cfg.counts) {
                    const Image refF = combine(firs, count);
                    const Image refI = combine(iirs, count);
                    const ErrorStats d = compareImages(refF, refI);
                    std::printf("%-6s %5g %3d %-16s %-24s | %9.2e %9.2e %9.2e | %9s %9s\n", inputName, size, count, "iir(D)",
                                "-", d.maxRel, d.rmsRel, d.pxRelMax, "-", "-");
                    std::fprintf(csv, "%s,%g,%d,iir,,0,%.6e,%.6e,%.6e,,,\n", inputName, size, count, d.maxRel, d.rmsRel,
                                 d.pxRelMax);
                    Worst& wi = worst["iir(D)"];
                    wi.firRms = std::max(wi.firRms, d.rmsRel);
                    wi.firMax = std::max(wi.firMax, d.maxRel);
                    ++wi.cases;
                    for (std::size_t vi = 0; vi < cfg.variants.size(); ++vi) {
                        const Image got = combine(pyramids[vi], count);
                        const ErrorStats ef = compareImages(refF, got);
                        const ErrorStats ei = compareImages(refI, got);
                        const std::string levels = levelList(plans[vi], count);
                        bool decimated = false;
                        for (int i = 0; i < count; ++i) {
                            decimated = decimated || plans[vi].blurs[i].level > 0;
                        }
                        const std::string name = variantName(cfg.variants[vi]);
                        std::printf("%-6s %5g %3d %-16s %-24s | %9.2e %9.2e %9.2e | %9.2e %9.2e\n", inputName, size, count,
                                    name.c_str(), levels.c_str(), ef.maxRel, ef.rmsRel, ef.pxRelMax, ei.maxRel, ei.rmsRel);
                        std::fprintf(csv, "%s,%g,%d,%s,%s,%d,%.6e,%.6e,%.6e,%.6e,%.6e,%.6e\n", inputName, size, count,
                                     name.c_str(), levels.c_str(), int(decimated), ef.maxRel, ef.rmsRel, ef.pxRelMax,
                                     ei.maxRel, ei.rmsRel, ei.pxRelMax);
                        if (decimated) {
                            Worst& w = worst[name];
                            w.firRms = std::max(w.firRms, ef.rmsRel);
                            w.firMax = std::max(w.firMax, ef.maxRel);
                            w.iirRms = std::max(w.iirRms, ei.rmsRel);
                            ++w.cases;
                        }
                    }
                }
                std::fflush(stdout);
            }
        }
        std::fclose(csv);
        std::printf("\nWorst case over the decimated cases (iir(D): over all cases, against FIR):\n");
        std::printf("%-16s %6s %9s %9s %9s\n", "method", "cases", "fir_rms", "fir_max", "iir_rms");
        for (const auto& [name, w] : worst) {
            std::printf("%-16s %6d %9.2e %9.2e %9.2e\n", name.c_str(), w.cases, w.firRms, w.firMax, w.iirRms);
        }
    }

    // Counts floats of a over r that differ bitwise from b, and the largest absolute difference.
    std::size_t bitMismatches(const Image& a, const Image& b, const Rect& r, double& maxAbs)
    {
        std::size_t n = 0;
        for (int y = r.y1; y < r.y2; ++y) {
            const float* pa = a.at(r.x1, y);
            const float* pb = b.at(r.x1, y);
            for (int i = 0; i < r.width() * a.nc; ++i) {
                if (std::memcmp(&pa[i], &pb[i], sizeof(float)) != 0) {
                    ++n;
                    maxAbs = std::max(maxAbs, double(std::fabs(pa[i] - pb[i])));
                }
            }
        }
        return n;
    }

    bool seamSpike(const Config& cfg, Pool& pool)
    {
        const int width = cfg.seamWidth;
        const int height = cfg.seamHeight;
        const Rect rod { 0, 0, width, height };
        const Image src = makeInput("plate", width, height, 1);
        std::printf("\n== Spike 1b: seams, plate %dx%d, windows %d wide and offset by %d, full height ==\n", width, height,
                    cfg.seamWindow, cfg.seamOffset);
        std::printf("Each window reads only its source cone clipped to the image. 'overlap': A against B; 'full': A and B\n");
        std::printf("against the full-frame render. Both must be 0 (bit-identical).\n");
        std::printf("%-16s %5s %3s %6s %6s %8s %8s %8s %9s %s\n", "variant", "size", "cnt", "winA", "halo", "inputA",
                    "overlap", "full", "max_abs", "result");
        FILE* csv = openCsv(cfg, "seam.csv", "variant,size,count,window_a,halo,input_a_width,overlap_mismatch,full_mismatch,max_abs");
        bool allExact = true;
        for (const Variant& v : cfg.variants) {
            for (double size : cfg.seamSizes) {
                for (int count : cfg.seamCounts) {
                    const PyramidPlan plan = makePlan({ size, cfg.ratio, count, cfg.sigmaMin }, v);
                    const Image full = renderPyramid(plan, src, rod, rod, pool);
                    for (int base : { 0, (width - cfg.seamWindow) / 2 }) {
                        const Rect a { base, 0, base + cfg.seamWindow, height };
                        const Rect b { base + cfg.seamOffset, 0, base + cfg.seamOffset + cfg.seamWindow, height };
                        const Rect coneA = sourceCone(plan, a);
                        const Rect inA = intersect(coneA, rod);
                        const Rect inB = intersect(sourceCone(plan, b), rod);
                        const Image imgA = renderPyramid(plan, src, inA, a, pool);
                        const Image imgB = renderPyramid(plan, src, inB, b, pool);
                        double maxAbs = 0.;
                        const std::size_t overlap = bitMismatches(imgA, imgB, intersect(a, b), maxAbs);
                        const std::size_t vsFull = bitMismatches(imgA, full, a, maxAbs) + bitMismatches(imgB, full, b, maxAbs);
                        const int halo = std::max(a.x1 - coneA.x1, coneA.x2 - a.x2);
                        const bool exact = overlap == 0 && vsFull == 0;
                        allExact = allExact && exact;
                        const std::string name = variantName(v);
                        std::printf("%-16s %5g %3d %6d %6d %8d %8zu %8zu %9.2e %s\n", name.c_str(), size, count, base, halo,
                                    inA.width(), overlap, vsFull, maxAbs, exact ? "exact" : "MISMATCH");
                        std::fprintf(csv, "%s,%g,%d,%d,%d,%d,%zu,%zu,%.6e\n", name.c_str(), size, count, base, halo,
                                     inA.width(), overlap, vsFull, maxAbs);
                    }
                }
            }
        }
        std::fclose(csv);
        std::printf("Seam test: %s\n", allExact ? "all exact" : "MISMATCHES FOUND");
        return allExact;
    }

    void timingSpike(const Config& cfg, Pool& pool)
    {
        Pool single(1);
        std::printf("\n== Spike 1c: CPU ms per frame, 4 channels, count %d (median of %d for the pyramid; '~': "
                    "extrapolated from leading lines, budget %.0f ms a stage) ==\n",
                    cfg.timingCount, cfg.iters, cfg.budgetMs);
        std::printf("%-5s %5s %-16s %-24s %10s %10s\n", "res", "size", "method", "levels", "1 thread", "all");
        FILE* csv = openCsv(cfg, "cpu_timing.csv", "res,size,count,method,threads,ms,extrapolated");
        for (const std::string& res : cfg.resolutions) {
            int width = 0, height = 0;
            if (!resolutionSize(res, width, height)) {
                continue;
            }
            const Rect rod { 0, 0, width, height };
            const Image src = makeInput("plate", width, height, 4);
            for (double size : cfg.timingSizes) {
                const BloomParams p { size, cfg.ratio, cfg.timingCount, cfg.sigmaMin };
                const auto report = [&](const std::string& method, const std::string& levels, StageClock one, StageClock all) {
                    char a[32], b[32];
                    std::snprintf(a, sizeof(a), "%s%.1f", one.extrapolated ? "~" : "", one.ms);
                    std::snprintf(b, sizeof(b), "%s%.1f", all.extrapolated ? "~" : "", all.ms);
                    std::printf("%-5s %5g %-16s %-24s %10s %10s\n", res.c_str(), size, method.c_str(), levels.c_str(), a, b);
                    std::fprintf(csv, "%s,%g,%d,%s,1,%.3f,%d\n", res.c_str(), size, cfg.timingCount, method.c_str(), one.ms,
                                 int(one.extrapolated));
                    std::fprintf(csv, "%s,%g,%d,%s,%u,%.3f,%d\n", res.c_str(), size, cfg.timingCount, method.c_str(),
                                 pool.size(), all.ms, int(all.extrapolated));
                    std::fflush(stdout);
                };
                for (RefKind kind : { RefKind::fir, RefKind::iir }) {
                    const StageClock one = timeReference(src, kind, p, single, cfg.budgetMs);
                    const StageClock all = timeReference(src, kind, p, pool, cfg.budgetMs);
                    report(kind == RefKind::fir ? "ref-fir" : "ref-iir(D)", "-", one, all);
                }
                for (const Variant& v : cfg.variants) {
                    const PyramidPlan plan = makePlan(p, v);
                    StageClock clocks[2];
                    Pool* pools[2] = { &single, &pool };
                    for (int t = 0; t < 2; ++t) {
                        renderPyramid(plan, src, rod, rod, *pools[t]);
                        std::vector<double> times;
                        for (int it = 0; it < cfg.iters; ++it) {
                            const double t0 = nowMs();
                            renderPyramid(plan, src, rod, rod, *pools[t]);
                            times.push_back(nowMs() - t0);
                        }
                        clocks[t].ms = median(times);
                    }
                    report(variantName(v), levelList(plan, plan.params.count), clocks[0], clocks[1]);
                }
            }
        }
        std::fclose(csv);
    }

} // namespace

bool
resolutionSize(const std::string& name, int& width, int& height)
{
    if (name == "hd") {
        width = 1920;
        height = 1080;
        return true;
    }
    if (name == "uhd") {
        width = 3840;
        height = 2160;
        return true;
    }
    return std::sscanf(name.c_str(), "%dx%d", &width, &height) == 2 && width > 0 && height > 0;
}

Image
makeInput(const std::string& name, int width, int height, int nc)
{
    Image img({ 0, 0, width, height }, nc);
    static const float channelScale[4] = { 1.f, 0.9f, 0.7f, 1.f };
    if (name == "point") {
        float* p = img.at(width / 2 + 3, height / 2 + 5);
        for (int c = 0; c < nc; ++c) {
            p[c] = 1.f;
        }
    } else if (name == "edge") {
        for (int y = 0; y < height; ++y) {
            for (int x = width / 2 + 13; x < width; ++x) {
                for (int c = 0; c < nc; ++c) {
                    img.at(x, y)[c] = 1.f;
                }
            }
        }
    } else {
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const double smooth = 0.5 + 0.5 * std::sin(x * 0.011 + 1.3) * std::cos(y * 0.017 - 0.4);
                for (int c = 0; c < nc; ++c) {
                    const double n = hash32(uint32_t(x) * 73856093u ^ uint32_t(y) * 19349663u ^ uint32_t(c) * 83492791u) / 4294967296.;
                    img.at(x, y)[c] = float((0.01 + 0.6 * smooth * (0.75 + 0.5 * n)) * channelScale[c]);
                }
            }
        }
        uint32_t state = 1u;
        const auto next = [&] {
            state = state * 1664525u + 1013904223u;
            return double(state >> 8) / 16777216.;
        };
        const int highlights = std::max(8, int(double(width) * height / 20000.));
        for (int h = 0; h < highlights; ++h) {
            const int cx = int(next() * width);
            const int cy = int(next() * height);
            const int radius = int(next() * 4.);
            const double value = 5. + 45. * next();
            for (int y = cy - radius; y <= cy + radius; ++y) {
                for (int x = cx - radius; x <= cx + radius; ++x) {
                    if (x < 0 || y < 0 || x >= width || y >= height || (x - cx) * (x - cx) + (y - cy) * (y - cy) > radius * radius) {
                        continue;
                    }
                    for (int c = 0; c < nc; ++c) {
                        img.at(x, y)[c] = float(value * channelScale[c]);
                    }
                }
            }
        }
    }
    return img;
}

std::vector<Image>
referenceBlurs(const Image& src, RefKind kind, const BloomParams& p, int count, Pool& pool)
{
    std::vector<Image> out;
    for (int i = 0; i < count; ++i) {
        Image b = src;
        for (const Stage& s : filterStages(b, kind, blurSigma(p, i), false)) {
            pool.run(s.units, s.fn);
        }
        out.push_back(std::move(b));
    }
    return out;
}

ErrorStats
compareImages(const Image& ref, const Image& got)
{
    ErrorStats e;
    for (float v : ref.px) {
        e.peak = std::max(e.peak, double(std::fabs(v)));
    }
    const double peak = e.peak > 0. ? e.peak : 1.;
    double sumSq = 0.;
    for (std::size_t i = 0; i < ref.px.size(); ++i) {
        const double r = ref.px[i];
        const double d = std::fabs(double(got.px[i]) - r);
        e.maxAbs = std::max(e.maxAbs, d);
        sumSq += d * d;
        e.pxRelMax = std::max(e.pxRelMax, d / (std::fabs(r) + 1e-3 * peak));
    }
    e.maxRel = e.maxAbs / peak;
    e.rmsRel = ref.px.empty() ? 0. : std::sqrt(sumSq / double(ref.px.size())) / peak;
    return e;
}

std::string
levelList(const PyramidPlan& plan, int count)
{
    std::string s;
    for (int i = 0; i < count && i < int(plan.blurs.size()); ++i) {
        s += (i ? "," : "") + std::to_string(plan.blurs[i].level);
    }
    return s;
}

FILE*
openCsv(const Config& cfg, const char* name, const char* header)
{
    std::filesystem::create_directories(cfg.out);
    const std::filesystem::path path = std::filesystem::path(cfg.out) / name;
    const bool fresh = !std::filesystem::exists(path);
    FILE* f = std::fopen(path.c_str(), "a");
    if (!f) {
        f = std::fopen("/dev/null", "w");
    } else if (fresh) {
        std::fprintf(f, "%s\n", header);
    }
    return f;
}

void
runCpuSpike(const Config& cfg)
{
    Pool pool(cfg.threads);
    std::printf("CPU threads: %u\n", pool.size());
    if (cfg.quality) {
        qualitySpike(cfg, pool);
    }
    if (cfg.seam) {
        seamSpike(cfg, pool);
    }
    if (cfg.cpuTiming) {
        timingSpike(cfg, pool);
    }
}

} // namespace bloomspike
