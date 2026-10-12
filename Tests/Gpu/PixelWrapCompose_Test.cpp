#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include "GpuWrap.h"
#include "pixelwrap_chain_spirv.h"
#include "pixelwrap_grade_spirv.h"
#include "pixelwrap_invert_spirv.h"

namespace {

// Mirrors of the Slang C++ prelude's ComputeVaryingInput and StructuredBuffer, so the prelude's
// global type names stay out of a file that also includes the Vulkan headers.
struct TwinVarying {
    uint32_t startGroup[3];
    uint32_t endGroup[3];
};

struct TwinBuffer {
    void* data;
    size_t count;
};

struct TwinGlobals {
    TwinBuffer src;
    TwinBuffer dst;
    TwinBuffer mask;
    TwinBuffer aux;
    gpu::GpuWrapParams* params;
};

using TwinEntry = void (*)(TwinVarying*, void*, void*);

} // namespace

extern "C" void pixelwrap_grade(TwinVarying*, void*, void*);
extern "C" void pixelwrap_invert(TwinVarying*, void*, void*);
extern "C" void pixelwrap_chain(TwinVarying*, void*, void*);

namespace {

constexpr uint32_t kW = 67;
constexpr uint32_t kH = 19;
constexpr uint32_t kGradeWords = 18;

struct Fixture {
    std::vector<uint32_t> gradeParams;
    std::vector<float> src;

    explicit Fixture(uint32_t seed)
    {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> u(0.0f, 1.0f);
        float a[4], b[4], gamma[4], invGamma[4];
        for (int c = 0; c < 4; ++c) {
            a[c] = 0.5f + 1.5f * u(rng);
            b[c] = u(rng) - 0.5f;
            gamma[c] = (c == 2) ? 1.0f : 0.4f + 2.0f * u(rng);
            invGamma[c] = 1.0f / gamma[c];
        }
        gradeParams.resize(kGradeWords);
        std::memcpy(&gradeParams[0], a, sizeof a);
        std::memcpy(&gradeParams[4], b, sizeof b);
        std::memcpy(&gradeParams[8], gamma, sizeof gamma);
        std::memcpy(&gradeParams[12], invGamma, sizeof invGamma);
        gradeParams[16] = 0xF;
        gradeParams[17] = 2 | 4;
        src.resize(size_t(kW) * kH * 4);
        for (float& v : src)
            v = 1.5f * u(rng) - 0.25f;
        for (size_t i = 0; i < src.size(); i += 4)
            src[i + 3] = (i % 17 == 0) ? 0.0f : u(rng);
    }
};

std::vector<float>
run(TwinEntry k, std::vector<float> src, std::vector<uint32_t> aux)
{
    std::vector<float> dst(src.size(), -123.0f);
    std::vector<float> mask(1, 1.0f);
    gpu::GpuWrapParams p {};
    p.width = kW;
    p.height = kH;
    p.srcStride = kW * 4;
    p.srcX2 = kW;
    p.srcY2 = kH;
    p.dstStride = kW * 4;
    p.nComps = 4;
    p.channelMask = 0xF;
    p.divisorChannel = -1;
    p.skipChannel = -1;
    p.mix = 1.0f;
    TwinGlobals g { { src.data(), src.size() },
                    { dst.data(), dst.size() },
                    { mask.data(), mask.size() },
                    { aux.data(), aux.size() },
                    &p };
    TwinVarying vi { { 0, 0, 0 }, { (kW + 7) / 8, (kH + 7) / 8, 1 } };
    k(&vi, nullptr, &g);
    return dst;
}

} // namespace

TEST(PixelWrapCompose, SpirvHasMagic)
{
    for (uint32_t w : { pixelwrap_grade_spirv[0], pixelwrap_invert_spirv[0], pixelwrap_chain_spirv[0] })
        EXPECT_EQ(w, 0x07230203u);
}

TEST(PixelWrapCompose, ChainEqualsInvertAfterGrade)
{
    for (uint32_t seed : { 1u, 2u, 3u }) {
        Fixture f(seed);
        const std::vector<float> graded = run(pixelwrap_grade, f.src, f.gradeParams);
        const std::vector<float> want = run(pixelwrap_invert, graded, f.gradeParams);
        const std::vector<float> got = run(pixelwrap_chain, f.src, f.gradeParams);
        ASSERT_EQ(got.size(), want.size());
        EXPECT_EQ(0, std::memcmp(got.data(), want.data(), got.size() * sizeof(float))) << "seed " << seed;
    }
}
