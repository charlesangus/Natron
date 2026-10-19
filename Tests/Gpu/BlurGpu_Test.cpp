#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <vector>

#include "BlurFixture.h"
#include "GpuBlur.h"
#include "GpuHarness.h"
#include "GpuRuntime.h"
#include "GpuTestDevice.h"
#include "Tolerance.h"
#include "blur_spirv.h"

using namespace gpu;

namespace {

struct BlurCase {
    int w, h, ch;
    double sigma;
    std::vector<float> src;
};

class BlurGpuTest : public gputest::GpuSuite {
protected:
    static std::vector<BlurCase> makeCases()
    {
        std::vector<BlurCase> cases;
        uint32_t seed = 1;
        for (double sigma : blurfix::kSigmas) {
            for (const auto& s : blurfix::kSizes)
                for (int ch : blurfix::kChannelCounts)
                    cases.push_back({ s.w, s.h, ch, sigma, blurfix::makeImage(s.w, s.h, ch, seed++) });
        }
        return cases;
    }

    // Horizontal then vertical pass per case, with a scratch buffer between them.
    static std::vector<std::vector<float>> run(const std::vector<BlurCase>& cases)
    {
        std::vector<std::vector<float>> dst;
        for (const BlurCase& c : cases)
            dst.emplace_back(c.src.size(), -1.0f);
        auto kernel = makeKernel(blur_spirv, blur_spirv_words, "main", 3, sizeof(blurfix::BlurParams),
                                 blur_group_size);
        auto transfer = makeTransfer();
        if (!kernel || !transfer)
            return dst;

        std::vector<Buffer> weights;
        for (const BlurCase& c : cases) {
            const std::vector<float> wf = blurfix::halfWeights(c.sigma);
            weights.push_back(makeStorage(wf.size() * sizeof(float), wf.data()));
        }

        std::vector<TransferFrame> frames;
        std::vector<Buffer> scratch;
        for (size_t i = 0; i < cases.size(); ++i) {
            frames.push_back({ cases[i].src.data(), dst[i].data(), cases[i].src.size() * sizeof(float) });
            scratch.push_back(makeStorage(frames.back().bytes));
        }

        process(*transfer, *kernel, frames, 128,
                [&](VkCommandBuffer cmd, const ComputeBinding& b, size_t k) -> GpuStatus {
                    const BlurCase& c = cases[k];
                    GpuBlurPass pass { uint32_t(c.w), uint32_t(c.h), uint32_t(c.ch),
                                       uint32_t(blurref::radiusForSigma(c.sigma)), false };
                    const VkBuffer w = weights[k].buffer;
                    const VkBuffer tmp = scratch[k].buffer;
                    if (GpuStatus s = recordBlurPass(*kernel, cmd, b.input, tmp, w, pass); !s) {
                        return s;
                    }
                    computeBarrier(cmd);
                    pass.vertical = true;
                    return recordBlurPass(*kernel, cmd, tmp, b.output, w, pass);
                });
        return dst;
    }
};

} // namespace

GPU_TEST_F(BlurGpuTest, MatchesReference)
{
    const std::vector<BlurCase> cases = makeCases();
    const std::vector<std::vector<float>> dst = run(cases);

    std::vector<tol::ErrorStats> perSigma(std::size(blurfix::kSigmas));
    for (size_t k = 0; k < cases.size(); ++k) {
        const BlurCase& c = cases[k];
        const size_t sigmaIndex = std::find(std::begin(blurfix::kSigmas), std::end(blurfix::kSigmas), c.sigma)
            - std::begin(blurfix::kSigmas);
        const std::vector<double> ref = blurfix::reference(c.src, c.w, c.h, c.ch, c.sigma);
        tol::ErrorStats caseStats;
        for (size_t i = 0; i < ref.size(); ++i) {
            perSigma[sigmaIndex].add(dst[k][i], ref[i], k, i);
            caseStats.add(dst[k][i], ref[i], k, i);
        }
        EXPECT_EQ(caseStats.classMismatches, 0u);
        EXPECT_LE(caseStats.maxAbs, blurfix::kTolerance)
            << "sigma=" << c.sigma << " " << c.w << "x" << c.h << " ch=" << c.ch;
    }

    std::printf("blur gpu (%s) vs reference, abs bound %.1e\n", dev_->info().name.c_str(), blurfix::kTolerance);
    for (size_t s = 0; s < perSigma.size(); ++s) {
        char label[32];
        std::snprintf(label, sizeof label, "  sigma=%g", blurfix::kSigmas[s]);
        perSigma[s].print(label);
        const BlurCase& w = cases[perSigma[s].worst.group];
        const size_t pix = perSigma[s].worst.index / w.ch;
        std::printf("  worst case: %dx%d ch=%d pixel=(%zu,%zu) channel=%zu\n", w.w, w.h, w.ch, pix % w.w, pix / w.w,
                    perSigma[s].worst.index % w.ch);
    }
}

// sigma 170 gives radius 510, the widest the shared tile holds. Every group loads 128 + 2R samples
// whatever the image size, so a small image fills the whole tile.
GPU_TEST_F(BlurGpuTest, LargestSupportedRadiusMatchesReference)
{
    constexpr double kSigma = 170.0;
    ASSERT_LE(uint32_t(blurref::radiusForSigma(kSigma)), kGpuBlurMaxRadius);
    std::vector<BlurCase> cases;
    cases.push_back({ 333, 290, 4, kSigma, blurfix::makeImage(333, 290, 4, 1) });
    const std::vector<std::vector<float>> dst = run(cases);
    for (size_t k = 0; k < cases.size(); ++k) {
        const BlurCase& c = cases[k];
        const std::vector<double> ref = blurfix::reference(c.src, c.w, c.h, c.ch, c.sigma);
        tol::ErrorStats stats;
        for (size_t i = 0; i < ref.size(); ++i)
            stats.add(dst[k][i], ref[i], k, i);
        EXPECT_EQ(stats.classMismatches, 0u);
        EXPECT_LE(stats.maxAbs, blurfix::kTolerance);
    }
}

// Radii just past the direct path's 32 up to 300, on sizes that leave partial blur tiles on both axes,
// and lines shorter than the radius.
GPU_TEST_F(BlurGpuTest, WideRadiiMatchReference)
{
    constexpr double kWideSigmas[] = { 11.0, 11.2, 16.67, 21.5, 50.0, 100.0 };
    constexpr blurfix::Size kWideSizes[] = { { 33, 401 }, { 129, 131 }, { 300, 7 }, { 5, 97 }, { 1, 1 } };
    std::vector<BlurCase> cases;
    uint32_t seed = 500;
    for (double sigma : kWideSigmas) {
        ASSERT_GT(blurref::radiusForSigma(sigma), 32);
        for (const blurfix::Size& s : kWideSizes)
            for (int ch : blurfix::kChannelCounts)
                cases.push_back({ s.w, s.h, ch, sigma, blurfix::makeImage(s.w, s.h, ch, seed++) });
    }
    const std::vector<std::vector<float>> dst = run(cases);
    for (size_t k = 0; k < cases.size(); ++k) {
        const BlurCase& c = cases[k];
        const std::vector<double> ref = blurfix::reference(c.src, c.w, c.h, c.ch, c.sigma);
        tol::ErrorStats stats;
        for (size_t i = 0; i < ref.size(); ++i)
            stats.add(dst[k][i], ref[i], k, i);
        EXPECT_EQ(stats.classMismatches, 0u);
        EXPECT_LE(stats.maxAbs, blurfix::kTolerance) << "sigma=" << c.sigma << " " << c.w << "x" << c.h
                                                     << " ch=" << c.ch;
    }
}

// Radii 1 to 32 on the narrow vertical path, on heights that leave partial 64-row tiles and widths that
// leave partial 8-column blocks, and lines shorter than the radius.
GPU_TEST_F(BlurGpuTest, NarrowRadiiMatchReference)
{
    constexpr double kNarrowSigmas[] = { 0.3, 1.0, 2.6, 3.0, 10.5, 10.6 };
    constexpr blurfix::Size kNarrowSizes[] = { { 33, 401 }, { 129, 131 }, { 300, 7 }, { 5, 64 }, { 8, 65 }, { 1, 1 } };
    std::vector<BlurCase> cases;
    uint32_t seed = 900;
    for (double sigma : kNarrowSigmas) {
        ASSERT_LE(blurref::radiusForSigma(sigma), 32);
        for (const blurfix::Size& s : kNarrowSizes)
            for (int ch : blurfix::kChannelCounts)
                cases.push_back({ s.w, s.h, ch, sigma, blurfix::makeImage(s.w, s.h, ch, seed++) });
    }
    const std::vector<std::vector<float>> dst = run(cases);
    for (size_t k = 0; k < cases.size(); ++k) {
        const BlurCase& c = cases[k];
        const std::vector<double> ref = blurfix::reference(c.src, c.w, c.h, c.ch, c.sigma);
        tol::ErrorStats stats;
        for (size_t i = 0; i < ref.size(); ++i)
            stats.add(dst[k][i], ref[i], k, i);
        EXPECT_EQ(stats.classMismatches, 0u);
        EXPECT_LE(stats.maxAbs, blurfix::kTolerance) << "sigma=" << c.sigma << " " << c.w << "x" << c.h
                                                     << " ch=" << c.ch;
    }
}

GPU_TEST_F(BlurGpuTest, RefusesRadiusAndChannelsBeyondTheTile)
{
    auto kernel = makeKernel(blur_spirv, blur_spirv_words, "main", 3, sizeof(blurfix::BlurParams), blur_group_size);
    ASSERT_TRUE(kernel != nullptr);
    const uint32_t over = uint32_t(blurref::radiusForSigma(171.0));
    ASSERT_GT(over, kGpuBlurMaxRadius);
    for (bool vertical : { false, true }) {
        const GpuStatus s = recordBlurPass(*kernel, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                           GpuBlurPass { 64, 64, 4, over, vertical });
        EXPECT_EQ(s.result, VK_ERROR_FEATURE_NOT_PRESENT) << s.message;
        EXPECT_NE(s.message.find("radius"), std::string::npos) << s.message;
    }
    const GpuStatus s = recordBlurPass(*kernel, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                       GpuBlurPass { 64, 64, 5, 3, false });
    EXPECT_EQ(s.result, VK_ERROR_FEATURE_NOT_PRESENT) << s.message;
}

TEST(BlurGroupCount, VerticalDirectPathCoversColumnBlocks)
{
    EXPECT_EQ(blurGroupCount({ 1000, 300, 4, 32, true }), (std::array<uint32_t, 3> { 5, 125, 1 }));
    EXPECT_EQ(blurGroupCount({ 1000, 300, 4, 33, true }), (std::array<uint32_t, 3> { 1000, 3, 1 }));
    EXPECT_EQ(blurGroupCount({ 1000, 300, 4, 3, false }), (std::array<uint32_t, 3> { 8, 300, 1 }));
}

namespace {

// The blur recordSeparableBlur promises: a source resolved against srcValid, filtered over the lines
// of domain, black outside the source and beyond the domain's edges, zero outside it. In float with half
// weights it also follows the CPU FIR's order of operations.
template <typename T>
std::vector<double>
regionReference(const std::vector<float>& src, const GpuBlurImage& srcImage, const GpuBlurRect& valid,
                const GpuBlurRect& domain, const GpuBlurRect& out, int nc, const std::vector<T>& hw)
{
    const int r = int(hw.size()) - 1;
    const auto sourceAt = [&](int x, int y, int c) -> T {
        if (valid.empty()) {
            return 0.0;
        }
        if (x < valid.x1 || x >= valid.x2 || y < valid.y1 || y >= valid.y2) {
            return 0.0;
        }
        return src[srcImage.offsetFloats + size_t(y - srcImage.bounds.y1) * srcImage.strideFloats
                   + size_t(x - srcImage.bounds.x1) * nc + c];
    };
    const int midW = out.x2 - out.x1;
    const int midH = domain.y2 - domain.y1;
    std::vector<T> mid(size_t(midW) * midH * nc, T(0));
    for (int y = domain.y1; y < domain.y2; ++y) {
        for (int x = out.x1; x < out.x2; ++x) {
            if (x < domain.x1 || x >= domain.x2) {
                continue;
            }
            for (int c = 0; c < nc; ++c) {
                const auto tap = [&](int p) -> T {
                    return (p >= domain.x1 && p < domain.x2) ? sourceAt(p, y, c) : T(0);
                };
                T acc = hw[0] * tap(x);
                for (int k = 1; k <= r; ++k) {
                    acc += hw[k] * (tap(x - k) + tap(x + k));
                }
                mid[(size_t(y - domain.y1) * midW + (x - out.x1)) * nc + c] = acc;
            }
        }
    }
    const int outW = out.x2 - out.x1;
    std::vector<double> res(size_t(outW) * (out.y2 - out.y1) * nc, 0.0);
    for (int y = out.y1; y < out.y2; ++y) {
        if (y < domain.y1 || y >= domain.y2) {
            continue;
        }
        for (int x = out.x1; x < out.x2; ++x) {
            for (int c = 0; c < nc; ++c) {
                const auto tap = [&](int p) -> T {
                    return (p >= domain.y1 && p < domain.y2) ? mid[(size_t(p - domain.y1) * midW + (x - out.x1)) * nc + c]
                                                             : T(0);
                };
                T acc = hw[0] * tap(y);
                for (int k = 1; k <= r; ++k) {
                    acc += hw[k] * (tap(y - k) + tap(y + k));
                }
                res[(size_t(y - out.y1) * outW + (x - out.x1)) * nc + c] = acc;
            }
        }
    }
    return res;
}

struct RegionCase {
    GpuBlurRect srcBounds;
    GpuBlurRect valid;
    GpuBlurRect domain;
    GpuBlurRect dstBounds;
    GpuBlurRect output;
};

} // namespace

// Sub-rects inside padded buffers at non-zero offsets, outputs reaching past the domain, and a
// domain reaching past the source, through the record context's scratch buffers. Sigma 11 and up
// take the shared-tile vertical pass, and the last case's output is taller than its domain.
TEST(BlurRegionGpu, SubRectsMatchReference)
{
    gputest::ValidationCheck validation;
    GpuRuntimeOptions o;
    o.device = gputest::deviceOptions();
    std::unique_ptr<GpuRuntime> rt;
    GpuStatus s = GpuRuntime::create(o, rt);
    GPUTEST_DEVICE_OR_SKIP(s.ok(), s.message);

    const RegionCase cases[] = {
        { { -3, -2, 37, 29 }, { 0, 1, 33, 27 }, { -10, -8, 45, 40 }, { -15, -5, 50, 36 }, { -12, -3, 47, 33 } },
        { { 0, 0, 61, 47 }, { 0, 0, 61, 47 }, { 0, 0, 61, 47 }, { 0, 0, 61, 47 }, { 7, 5, 30, 40 } },
        { { 2, 3, 9, 6 }, { 2, 3, 9, 6 }, { -40, -30, 51, 39 }, { -40, -30, 51, 39 }, { -40, -30, 51, 39 } },
        { { 0, 0, 4, 4 }, {}, { 0, 0, 13, 9 }, { 0, 0, 13, 9 }, { 0, 0, 13, 9 } },
        { { 0, 0, 40, 30 }, { 0, 0, 40, 30 }, { 0, 5, 40, 20 }, { -2, -4, 43, 35 }, { -1, -3, 41, 33 } },
    };
    uint32_t seed = 100;
    for (const RegionCase& rc : cases) {
        for (double sigma : { 0.5, 3.0, 11.0, 25.0, 100.0 }) {
            for (int nc : blurfix::kChannelCounts) {
                GpuBlurImage srcImage;
                srcImage.bounds = rc.srcBounds;
                srcImage.offsetFloats = 7;
                srcImage.strideFloats = uint32_t((rc.srcBounds.x2 - rc.srcBounds.x1 + 3) * nc);
                const size_t srcFloats = srcImage.offsetFloats
                    + size_t(rc.srcBounds.y2 - rc.srcBounds.y1) * srcImage.strideFloats;
                std::vector<float> src = blurfix::makeImage(int(srcFloats), 1, 1, seed++);

                GpuBlurImage dstImage;
                dstImage.bounds = rc.dstBounds;
                dstImage.offsetFloats = 5;
                dstImage.strideFloats = uint32_t((rc.dstBounds.x2 - rc.dstBounds.x1 + 1) * nc);
                const size_t dstFloats = dstImage.offsetFloats
                    + size_t(rc.dstBounds.y2 - rc.dstBounds.y1) * dstImage.strideFloats;
                std::vector<float> dst(dstFloats, -7.0f);

                GpuBufferHandle srcBuf, dstBuf;
                ASSERT_TRUE(rt->acquire(srcFloats * sizeof(float), srcBuf).ok());
                ASSERT_TRUE(rt->acquire(dstFloats * sizeof(float), dstBuf).ok());
                TimelineValue done;
                ASSERT_TRUE(rt->upload({ src.data(), srcFloats * sizeof(float) }, srcBuf.buffer(), 0, done).ok());
                ASSERT_TRUE(rt->wait(done).ok());
                ASSERT_TRUE(rt->upload({ dst.data(), dstFloats * sizeof(float) }, dstBuf.buffer(), 0, done).ok());
                ASSERT_TRUE(rt->wait(done).ok());

                const std::vector<float> weights = blurfix::halfWeights(sigma);
                GpuSeparableBlur b;
                srcImage.buffer = &srcBuf.buffer();
                dstImage.buffer = &dstBuf.buffer();
                b.src = srcImage;
                b.srcValid = rc.valid;
                b.dst = dstImage;
                b.output = rc.output;
                b.domain = rc.domain;
                b.channels = uint32_t(nc);
                b.weightsX = weights;
                b.weightsY = weights;

                std::unique_ptr<GpuRecordContext> ctx;
                ASSERT_TRUE(rt->newRecordContext(ctx).ok());
                s = recordSeparableBlur(*ctx, b);
                ASSERT_TRUE(s.ok()) << s.message;
                s = ctx->submitAndWait();
                ASSERT_TRUE(s.ok()) << s.message;
                ctx.reset();
                ASSERT_TRUE(rt->download(dstBuf.buffer(), 0, { dst.data(), dstFloats * sizeof(float) }, done).ok());
                ASSERT_TRUE(rt->wait(done).ok());

                const std::vector<double> full = blurref::makeWeights(sigma);
                const std::vector<double> ref = regionReference(src, srcImage, rc.valid, rc.domain, rc.output, nc,
                                                                std::vector<double>(full.begin() + full.size() / 2, full.end()));
                const std::vector<double> refF = regionReference(src, srcImage, rc.valid, rc.domain, rc.output, nc, weights);
                const int outW = rc.output.x2 - rc.output.x1;
                double worst = 0.0;
                double worstF = 0.0;
                size_t untouchedChanged = 0;
                for (int y = rc.dstBounds.y1; y < rc.dstBounds.y2; ++y) {
                    for (int x = rc.dstBounds.x1; x < rc.dstBounds.x2; ++x) {
                        const bool inOut = x >= rc.output.x1 && x < rc.output.x2 && y >= rc.output.y1
                            && y < rc.output.y2;
                        for (int c = 0; c < nc; ++c) {
                            const float got = dst[dstImage.offsetFloats + size_t(y - rc.dstBounds.y1) * dstImage.strideFloats
                                                  + size_t(x - rc.dstBounds.x1) * nc + c];
                            if (!inOut) {
                                untouchedChanged += got != -7.0f;
                                continue;
                            }
                            const double want
                                = ref[(size_t(y - rc.output.y1) * outW + (x - rc.output.x1)) * nc + c];
                            worst = std::max(worst, std::abs(double(got) - want));
                            worstF = std::max(worstF, std::abs(double(got) - refF[(size_t(y - rc.output.y1) * outW + (x - rc.output.x1)) * nc + c]));
                        }
                    }
                }
                EXPECT_EQ(untouchedChanged, 0u);
                EXPECT_LE(worst, blurfix::kTolerance)
                    << "sigma=" << sigma << " nc=" << nc << " domain x1=" << rc.domain.x1;
                EXPECT_LE(worstF, blurfix::kTolerance)
                    << "float order, sigma=" << sigma << " nc=" << nc << " domain x1=" << rc.domain.x1;
            }
        }
    }
    rt.reset();
    validation.expectClean("the region blurs");
}
