#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <vector>

#include "BlurFixture.h"
#include "GpuBlur.h"
#include "GpuHarness.h"
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
            const std::vector<double> wd = blurref::makeWeights(c.sigma);
            const std::vector<float> wf(wd.begin(), wd.end());
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

TEST_F(BlurGpuTest, MatchesReference)
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
TEST_F(BlurGpuTest, LargestSupportedRadiusMatchesReference)
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

TEST_F(BlurGpuTest, RefusesRadiusAndChannelsBeyondTheTile)
{
    auto kernel = makeKernel(blur_spirv, blur_spirv_words, "main", 3, sizeof(blurfix::BlurParams), blur_group_size);
    ASSERT_TRUE(kernel);
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
