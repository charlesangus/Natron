#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "BlurFixture.h"
#include "GpuHarness.h"
#include "Tolerance.h"
#include "blur_spirv.h"

using namespace gpu;

namespace {

struct BlurCase
{
    int w, h, ch;
    double sigma;
    bool neumann;
    size_t sigmaIndex;
    std::vector<float> src;
};

class BlurGpuTest : public gputest::GpuSuite
{
protected:
    static std::vector<BlurCase> makeCases()
    {
        std::vector<BlurCase> cases;
        uint32_t seed = 1;
        size_t sigmaIndex = 0;
        for (double sigma : blurfix::kSigmas) {
            for (const auto& s : blurfix::kSizes)
                for (int ch : blurfix::kChannelCounts)
                    for (bool neumann : {false, true}) {
                        cases.push_back({s.w, s.h, ch, sigma, neumann, sigmaIndex,
                                         blurfix::makeImage(s.w, s.h, ch, seed++)});
                    }
            ++sigmaIndex;
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
        for (double sigma : blurfix::kSigmas) {
            const std::vector<double> wd = blurref::makeWeights(sigma);
            const std::vector<float> wf(wd.begin(), wd.end());
            weights.push_back(makeStorage(wf.size() * sizeof(float), wf.data()));
        }

        std::vector<TransferFrame> frames;
        std::vector<Buffer> scratch;
        for (size_t i = 0; i < cases.size(); ++i) {
            frames.push_back({cases[i].src.data(), dst[i].data(), cases[i].src.size() * sizeof(float)});
            scratch.push_back(makeStorage(frames.back().bytes));
        }

        process(*transfer, *kernel, frames, 128,
                [&](VkCommandBuffer cmd, const ComputeBinding& b, size_t k) {
                    const BlurCase& c = cases[k];
                    blurfix::BlurParams p{uint32_t(c.w), uint32_t(c.h), uint32_t(c.ch),
                                          uint32_t(blurref::radiusForSigma(c.sigma)), 0, c.neumann ? 1u : 0u};
                    const VkBuffer w = weights[c.sigmaIndex].buffer;
                    const VkBuffer tmp = scratch[k].buffer;

                    const VkBuffer horizontal[] = {b.input, tmp, w};
                    GpuStatus s = kernel->record(cmd, horizontal, std::as_bytes(std::span(&p, 1)),
                                                 {uint32_t(c.w), uint32_t(c.h), 1});
                    EXPECT_TRUE(s.ok()) << s.message;
                    computeBarrier(cmd);

                    p.vertical = 1;
                    const VkBuffer vertical[] = {tmp, b.output, w};
                    s = kernel->record(cmd, vertical, std::as_bytes(std::span(&p, 1)),
                                       {uint32_t(c.h), uint32_t(c.w), 1});
                    EXPECT_TRUE(s.ok()) << s.message;
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
        const std::vector<double> ref = blurfix::reference(c.src, c.w, c.h, c.ch, c.sigma, c.neumann);
        tol::ErrorStats caseStats;
        for (size_t i = 0; i < ref.size(); ++i) {
            perSigma[c.sigmaIndex].add(dst[k][i], ref[i], k, i);
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
