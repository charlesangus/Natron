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
    bool neumann;
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
                    for (bool neumann : { false, true }) {
                        cases.push_back({ s.w, s.h, ch, sigma, neumann, blurfix::makeImage(s.w, s.h, ch, seed++) });
                    }
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
                                       uint32_t(blurref::radiusForSigma(c.sigma)), false, c.neumann };
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
        const std::vector<double> ref = blurfix::reference(c.src, c.w, c.h, c.ch, c.sigma, c.neumann);
        tol::ErrorStats caseStats;
        for (size_t i = 0; i < ref.size(); ++i) {
            perSigma[sigmaIndex].add(dst[k][i], ref[i], k, i);
            caseStats.add(dst[k][i], ref[i], k, i);
        }
        EXPECT_EQ(caseStats.classMismatches, 0u);
        EXPECT_LE(caseStats.maxAbs, blurfix::kTolerance)
            << "sigma=" << c.sigma << " " << c.w << "x" << c.h << " ch=" << c.ch << " neumann=" << c.neumann;
    }

    std::printf("blur gpu (%s) vs reference, abs bound %.1e\n", dev_->info().name.c_str(), blurfix::kTolerance);
    for (size_t s = 0; s < perSigma.size(); ++s) {
        char label[32];
        std::snprintf(label, sizeof label, "  sigma=%g", blurfix::kSigmas[s]);
        perSigma[s].print(label);
        const BlurCase& w = cases[perSigma[s].worst.group];
        const size_t pix = perSigma[s].worst.index / w.ch;
        std::printf("  worst case: %dx%d ch=%d neumann=%d pixel=(%zu,%zu) channel=%zu\n", w.w, w.h, w.ch,
                    w.neumann, pix % w.w, pix / w.w, perSigma[s].worst.index % w.ch);
    }
}

// sigma 170 gives radius 510, the widest the shared tile holds. Every group loads 128 + 2R samples
// whatever the image size, so a small image fills the whole tile.
GPU_TEST_F(BlurGpuTest, LargestSupportedRadiusMatchesReference)
{
    constexpr double kSigma = 170.0;
    ASSERT_LE(uint32_t(blurref::radiusForSigma(kSigma)), kGpuBlurMaxRadius);
    std::vector<BlurCase> cases;
    for (bool neumann : { false, true })
        cases.push_back({ 333, 290, 4, kSigma, neumann, blurfix::makeImage(333, 290, 4, neumann ? 2 : 1) });
    const std::vector<std::vector<float>> dst = run(cases);
    for (size_t k = 0; k < cases.size(); ++k) {
        const BlurCase& c = cases[k];
        const std::vector<double> ref = blurfix::reference(c.src, c.w, c.h, c.ch, c.sigma, c.neumann);
        tol::ErrorStats stats;
        for (size_t i = 0; i < ref.size(); ++i)
            stats.add(dst[k][i], ref[i], k, i);
        EXPECT_EQ(stats.classMismatches, 0u);
        EXPECT_LE(stats.maxAbs, blurfix::kTolerance) << "neumann=" << c.neumann;
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
                                           GpuBlurPass { 64, 64, 4, over, vertical, true });
        EXPECT_EQ(s.result, VK_ERROR_FEATURE_NOT_PRESENT) << s.message;
        EXPECT_NE(s.message.find("radius"), std::string::npos) << s.message;
    }
    const GpuStatus s = recordBlurPass(*kernel, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                       GpuBlurPass { 64, 64, 5, 3, false, true });
    EXPECT_EQ(s.result, VK_ERROR_FEATURE_NOT_PRESENT) << s.message;
}

TEST(BlurGroupCount, VerticalDirectPathCoversColumnBlocks)
{
    EXPECT_EQ(blurGroupCount({ 1000, 300, 4, 32, true, true }), (std::array<uint32_t, 3> { 3, 32, 1 }));
    EXPECT_EQ(blurGroupCount({ 1000, 300, 4, 33, true, true }), (std::array<uint32_t, 3> { 3, 1000, 1 }));
    EXPECT_EQ(blurGroupCount({ 1000, 300, 4, 3, false, true }), (std::array<uint32_t, 3> { 8, 300, 1 }));
}

namespace {

// The blur recordSeparableBlur promises: a source resolved against srcValid, filtered over the lines
// of domain with the boundary condition at the domain's edges, zero outside it. In float with half
// weights it also follows the CPU FIR's order of operations.
template <typename T>
std::vector<double>
regionReference(const std::vector<float>& src, const GpuBlurImage& srcImage, const GpuBlurRect& valid,
                const GpuBlurRect& domain, const GpuBlurRect& out, int nc, const std::vector<T>& hw, bool neumann)
{
    const int r = int(hw.size()) - 1;
    const auto sourceAt = [&](int x, int y, int c) -> T {
        if (valid.empty()) {
            return 0.0;
        }
        if (x < valid.x1 || x >= valid.x2 || y < valid.y1 || y >= valid.y2) {
            if (!neumann) {
                return 0.0;
            }
            x = std::clamp(x, valid.x1, valid.x2 - 1);
            y = std::clamp(y, valid.y1, valid.y2 - 1);
        }
        return src[srcImage.offsetFloats + size_t(y - srcImage.bounds.y1) * srcImage.strideFloats
                   + size_t(x - srcImage.bounds.x1) * nc + c];
    };
    // Resolves a tap against [lo, hi); false when it reads zero.
    const auto resolve = [&](int p, int lo, int hi, int& q) {
        q = p;
        if (p >= lo && p < hi) {
            return true;
        }
        if (!neumann) {
            return false;
        }
        q = std::clamp(p, lo, hi - 1);
        return true;
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
                    int q;
                    return resolve(p, domain.x1, domain.x2, q) ? sourceAt(q, y, c) : T(0);
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
                    int q;
                    return resolve(p, domain.y1, domain.y2, q) ? mid[(size_t(q - domain.y1) * midW + (x - out.x1)) * nc + c]
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
// domain reaching past the source, through the record context's scratch buffers.
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
    };
    uint32_t seed = 100;
    for (const RegionCase& rc : cases) {
        for (double sigma : blurfix::kSigmas) {
            for (int nc : blurfix::kChannelCounts) {
                for (bool neumann : { false, true }) {
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
                    b.neumann = neumann;
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
                                                                    std::vector<double>(full.begin() + full.size() / 2, full.end()), neumann);
                    const std::vector<double> refF = regionReference(src, srcImage, rc.valid, rc.domain, rc.output, nc, weights, neumann);
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
                        << "sigma=" << sigma << " nc=" << nc << " neumann=" << neumann << " domain x1=" << rc.domain.x1;
                    EXPECT_LE(worstF, blurfix::kTolerance)
                        << "float order, sigma=" << sigma << " nc=" << nc << " neumann=" << neumann << " domain x1=" << rc.domain.x1;
                }
            }
        }
    }
    rt.reset();
    validation.expectClean("the region blurs");
}
