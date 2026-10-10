#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include "pointop_chain_spirv.h"
#include "pointop_grade_spirv.h"
#include "pointop_invert_spirv.h"
#include "pointop_masked_spirv.h"

#include "slang-cpp-prelude.h"

extern "C" void pointop_grade(ComputeVaryingInput*, void*, void*);
extern "C" void pointop_invert(ComputeVaryingInput*, void*, void*);
extern "C" void pointop_chain(ComputeVaryingInput*, void*, void*);
extern "C" void pointop_masked(ComputeVaryingInput*, void*, void*);

namespace {

struct PointParams
{
    float a[4];
    float b[4];
    float gamma[4];
    float invGamma[4];
    uint32_t width;
    uint32_t height;
    uint32_t channelMask;
    uint32_t flags;
    float mixAmount;
    uint32_t premult;
};

struct PointGlobals
{
    StructuredBuffer<float> srcBuf;
    RWStructuredBuffer<float> dstBuf;
    StructuredBuffer<float> maskBuf;
    PointParams* params;
};

constexpr uint32_t kW = 67;
constexpr uint32_t kH = 19;

using Kernel = void (*)(ComputeVaryingInput*, void*, void*);

std::vector<float> run(Kernel k, const std::vector<float>& src, const std::vector<float>& mask,
                       PointParams& p)
{
    std::vector<float> dst(src.size(), -123.0f);
    PointGlobals g{{const_cast<float*>(src.data()), src.size() / 4},
                   {dst.data(), dst.size() / 4},
                   {const_cast<float*>(mask.data()), mask.size()},
                   &p};
    ComputeVaryingInput vi = {};
    vi.startGroupID = {0, 0, 0};
    vi.endGroupID = {(kW + 7) / 8, (kH + 7) / 8, 1};
    k(&vi, nullptr, &g);
    return dst;
}

struct Fixture
{
    PointParams p{};
    std::vector<float> src;
    std::vector<float> mask;

    explicit Fixture(uint32_t seed)
    {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> u(0.0f, 1.0f);
        for (int c = 0; c < 4; ++c) {
            p.a[c] = 0.5f + 1.5f * u(rng);
            p.b[c] = u(rng) - 0.5f;
            p.gamma[c] = (c == 2) ? 1.0f : 0.4f + 2.0f * u(rng);
            p.invGamma[c] = 1.0f / p.gamma[c];
        }
        p.width = kW;
        p.height = kH;
        p.channelMask = 0xF;
        p.flags = 2 | 4;
        p.mixAmount = 0.75f;
        src.resize(size_t(kW) * kH * 4);
        for (float& v : src)
            v = 1.5f * u(rng) - 0.25f;
        for (size_t i = 0; i < src.size(); i += 4)
            src[i + 3] = (i % 17 == 0) ? 0.0f : u(rng);
        mask.resize(size_t(kW) * kH);
        for (float& m : mask)
            m = u(rng);
    }
};

int64_t orderedBits(float f)
{
    int32_t i;
    std::memcpy(&i, &f, sizeof i);
    return i < 0 ? static_cast<int64_t>(INT32_MIN) - i : i;
}

float gradeScalar(float v, const PointParams& p, int c)
{
    float x = std::fma(p.a[c], v, p.b[c]);
    if (p.gamma[c] <= 0.0f)
        x = x < 1.0f ? 0.0f : x == 1.0f ? 1.0f : INFINITY;
    else if (p.gamma[c] != 1.0f && x > 0.0f)
        x = std::pow(x, p.invGamma[c]);
    if (p.flags & 2)
        x = (0.0f < x) ? x : 0.0f;
    if (p.flags & 4)
        x = (x < 1.0f) ? x : 1.0f;
    return x;
}

} // namespace

TEST(PointOpCompose, SpirvHasMagic)
{
    for (uint32_t w : {pointop_grade_spirv[0], pointop_invert_spirv[0], pointop_chain_spirv[0],
                       pointop_masked_spirv[0]})
        EXPECT_EQ(w, 0x07230203u);
}

TEST(PointOpCompose, ChainEqualsInvertAfterGrade)
{
    for (uint32_t seed : {1u, 2u, 3u}) {
        Fixture f(seed);
        const std::vector<float> graded = run(pointop_grade, f.src, f.mask, f.p);
        const std::vector<float> want = run(pointop_invert, graded, f.mask, f.p);
        const std::vector<float> got = run(pointop_chain, f.src, f.mask, f.p);
        ASSERT_EQ(got.size(), want.size());
        int64_t maxUlp = 0;
        for (size_t i = 0; i < got.size(); ++i)
            maxUlp = std::max<int64_t>(maxUlp, std::llabs(orderedBits(got[i]) - orderedBits(want[i])));
        EXPECT_LE(maxUlp, 1) << "seed " << seed;
        EXPECT_EQ(0, std::memcmp(got.data(), want.data(), got.size() * sizeof(float)));
    }
}

TEST(PointOpCompose, MaskedMatchesHandWrittenReference)
{
    for (uint32_t premult : {0u, 1u}) {
        Fixture f(10 + premult);
        f.p.premult = premult;
        const std::vector<float> got = run(pointop_masked, f.src, f.mask, f.p);
        int64_t maxUlp = 0;
        for (size_t px = 0; px < f.mask.size(); ++px) {
            float v[4], in[4], out[4];
            std::memcpy(v, &f.src[px * 4], sizeof v);
            const float alpha = v[3];
            for (int c = 0; c < 4; ++c)
                in[c] = (premult && c < 3 && alpha != 0.0f) ? v[c] / alpha : v[c];
            for (int c = 0; c < 4; ++c) {
                out[c] = gradeScalar(in[c], f.p, c);
                if (premult && c < 3)
                    out[c] *= alpha;
            }
            const float t = f.mask[px] * f.p.mixAmount;
            for (int c = 0; c < 4; ++c) {
                const float want = v[c] + (out[c] - v[c]) * t;
                const float have = got[px * 4 + c];
                maxUlp = std::max<int64_t>(maxUlp, std::llabs(orderedBits(have) - orderedBits(want)));
                EXPECT_NEAR(have, want, 1e-5f) << "px " << px << " c " << c;
            }
        }
        std::printf("masked premult=%u max ulp vs reference: %lld\n", premult, (long long)maxUlp);
    }
}
