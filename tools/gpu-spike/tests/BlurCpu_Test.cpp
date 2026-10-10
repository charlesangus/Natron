#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "slang-cpp-prelude.h"

#include "../ref/BlurRef.h"

extern "C" void blurPass(ComputeVaryingInput*, void* entryPointParams, void* globalParams);

namespace {

struct BlurParams
{
    uint32_t width;
    uint32_t height;
    uint32_t channels;
    uint32_t radius;
    uint32_t vertical;
    uint32_t neumann;
};

struct BlurGlobals
{
    StructuredBuffer<float> srcBuf;
    RWStructuredBuffer<float> dstBuf;
    StructuredBuffer<float> weights;
    BlurParams* params;
};

constexpr double kTolerance = 2e-6;
constexpr uint32_t kGroup = 128;

void runPass(const std::vector<float>& src, std::vector<float>& dst, const std::vector<float>& w,
             BlurParams p)
{
    dst.assign(src.size(), -1.0f);
    BlurGlobals g{{const_cast<float*>(src.data()), src.size()}, {dst.data(), dst.size()}, {const_cast<float*>(w.data()), w.size()}, &p};
    const uint32_t len = p.vertical ? p.height : p.width;
    const uint32_t lines = p.vertical ? p.width : p.height;
    ComputeVaryingInput vi = {};
    vi.startGroupID = {0, 0, 0};
    vi.endGroupID = {(len + kGroup - 1) / kGroup, lines, 1};
    blurPass(&vi, nullptr, &g);
}

double maxError(int width, int height, int channels, double sigma, bool neumann, uint32_t seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);
    std::vector<float> img(size_t(width) * height * channels);
    for (float& v : img)
        v = dist(rng);

    const std::vector<double> wd = blurref::makeWeights(sigma);
    const std::vector<float> wf(wd.begin(), wd.end());
    BlurParams p{uint32_t(width), uint32_t(height), uint32_t(channels), uint32_t(wd.size() / 2), 0,
                 neumann ? 1u : 0u};
    std::vector<float> tmp, out;
    runPass(img, tmp, wf, p);
    p.vertical = 1;
    runPass(tmp, out, wf, p);

    const std::vector<double> ref =
        blurref::blur(std::vector<double>(img.begin(), img.end()), width, height, channels, sigma, neumann);
    double worst = 0.0;
    for (size_t i = 0; i < ref.size(); ++i)
        worst = std::max(worst, std::abs(double(out[i]) - ref[i]));
    return worst;
}

} // namespace

TEST(BlurCpu, MatchesReference)
{
    const int sizes[][2] = {{1, 1}, {2, 3}, {5, 3}, {37, 23}, {129, 131}, {257, 5}, {4, 300}};
    const double sigmas[] = {0.5, 3.0, 25.0, 100.0};
    const int channelCounts[] = {1, 3, 4};
    double overall = 0.0;
    uint32_t seed = 1;
    for (double sigma : sigmas)
        for (const auto& s : sizes)
            for (int ch : channelCounts)
                for (bool neumann : {false, true}) {
                    const double e = maxError(s[0], s[1], ch, sigma, neumann, seed++);
                    overall = std::max(overall, e);
                    EXPECT_LE(e, kTolerance)
                        << "sigma=" << sigma << " " << s[0] << "x" << s[1] << " ch=" << ch
                        << " neumann=" << neumann;
                }
    RecordProperty("max_abs_error", std::to_string(overall));
    std::printf("BlurCpu max abs error over all cases: %.3e\n", overall);
}

TEST(BlurCpu, WeightsNormalisedAndRadiusRule)
{
    for (double sigma : {0.5, 3.0, 25.0, 100.0}) {
        const std::vector<double> w = blurref::makeWeights(sigma);
        double sum = 0.0;
        for (double v : w)
            sum += v;
        EXPECT_NEAR(sum, 1.0, 1e-14);
        EXPECT_EQ(int(w.size()), 2 * int(std::ceil(3.0 * sigma)) + 1);
    }
    EXPECT_DOUBLE_EQ(blurref::sigmaForSize(2.4), 1.0);
}
