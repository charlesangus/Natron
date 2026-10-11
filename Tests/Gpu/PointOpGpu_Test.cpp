#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "GpuHarness.h"
#include "GradeFixture.h"
#include "pointop_chain_spirv.h"
#include "pointop_grade_spirv.h"
#include "pointop_masked_spirv.h"

using namespace gpu;

namespace {

// Mirrors PointParams in pointops.slang, which has no padding between members.
struct PointParams {
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

constexpr gradefix::Bounds kGpuBounds { gradefix::kUlpBound, true };

struct PointCase {
    PointParams p {};
    std::vector<float> src;
    std::vector<float> mask;
};

PointParams
fromGrade(const gradefix::GradeParams& g)
{
    PointParams p {};
    for (int c = 0; c < 4; ++c) {
        p.a[c] = g.a[c];
        p.b[c] = g.b[c];
        p.gamma[c] = g.gamma[c];
        p.invGamma[c] = g.invGamma[c];
    }
    p.width = g.width;
    p.height = g.height;
    p.channelMask = g.channelMask;
    p.flags = g.flags;
    return p;
}

// The grade sweep cases that are RGBA, the only layout the point-op kernels take.
std::vector<gradefix::GradeCase>
rgbaGradeCases()
{
    std::vector<gradefix::GradeCase> out;
    for (gradefix::GradeCase& c : gradefix::makeCases())
        if (c.p.nComps == 4)
            out.push_back(std::move(c));
    return out;
}

bool
subnormal(float v)
{
    return v != 0.0f && std::fabs(v) < std::numeric_limits<float>::min();
}

class PointOpGpuTest : public gputest::GpuSuite {
protected:
    static std::vector<std::vector<float>> run(const uint32_t* spirv, size_t words, const uint32_t (&groupSize)[3],
                                               const std::vector<PointCase>& cases)
    {
        std::vector<std::vector<float>> dst;
        for (const PointCase& c : cases)
            dst.emplace_back(c.src.size(), -123.0f);
        auto kernel = makeKernel(spirv, words, "main", 3, sizeof(PointParams), groupSize);
        auto transfer = makeTransfer();
        if (!kernel || !transfer)
            return dst;

        const float one = 1.0f;
        Buffer unusedMask = makeStorage(sizeof one, &one);
        std::vector<Buffer> masks;
        std::vector<TransferFrame> frames;
        for (size_t i = 0; i < cases.size(); ++i) {
            masks.push_back(cases[i].mask.empty() ? Buffer {}
                                                  : makeStorage(cases[i].mask.size() * sizeof(float),
                                                                cases[i].mask.data()));
            frames.push_back({ cases[i].src.data(), dst[i].data(), cases[i].src.size() * sizeof(float) });
        }

        process(*transfer, *kernel, frames, 256, [&](VkCommandBuffer cmd, const ComputeBinding& b, size_t k) {
            const PointParams& p = cases[k].p;
            const VkBuffer bufs[] = { b.input, b.output, masks[k].buffer ? masks[k].buffer : unusedMask.buffer };
            return kernel->record(cmd, bufs, std::as_bytes(std::span(&p, 1)), { p.width, p.height, 1 });
        });
        return dst;
    }
};

} // namespace

TEST_F(PointOpGpuTest, GradeMatchesReference)
{
    const std::vector<gradefix::GradeCase> grade = rgbaGradeCases();
    std::vector<PointCase> cases;
    for (const gradefix::GradeCase& g : grade)
        cases.push_back({ fromGrade(g.p), g.src, {} });
    const std::vector<std::vector<float>> dst = run(pointop_grade_spirv, pointop_grade_spirv_words,
                                                    pointop_grade_group_size, cases);

    gradefix::GradeStats stats;
    for (size_t k = 0; k < grade.size(); ++k)
        gradefix::checkCase(stats, k, grade[k], dst[k].data(), kGpuBounds);
    gradefix::printStats("pointop_grade gpu vs reference", stats);
    for (const std::string& f : stats.failures)
        ADD_FAILURE() << f;
    EXPECT_GT(stats.err.compared, 1000000u);
    EXPECT_GT(stats.err.nonFinite, 0u);
    EXPECT_EQ(stats.violations, 0u);
}

// Grade then invert: RGB is 1 - grade(v), alpha is grade(alpha).
TEST_F(PointOpGpuTest, ChainMatchesReference)
{
    const std::vector<gradefix::GradeCase> grade = rgbaGradeCases();
    std::vector<PointCase> cases;
    for (const gradefix::GradeCase& g : grade)
        cases.push_back({ fromGrade(g.p), g.src, {} });
    const std::vector<std::vector<float>> dst = run(pointop_chain_spirv, pointop_chain_spirv_words,
                                                    pointop_chain_group_size, cases);

    tol::ErrorStats stats;
    uint64_t violations = 0;
    for (size_t k = 0; k < grade.size(); ++k) {
        const gradefix::GradeCase& g = grade[k];
        const bool clampBlack = (g.p.flags & gradefix::kClampBlack) != 0;
        const bool clampWhite = (g.p.flags & gradefix::kClampWhite) != 0;
        for (size_t i = 0; i < g.src.size(); ++i) {
            const uint32_t c = static_cast<uint32_t>(i % 4);
            if (subnormal(g.src[i]))
                continue;
            const graderef::Channel chan { g.p.a[c], g.p.b[c], g.p.gamma[c] };
            const bool graded = (g.p.channelMask & (1u << c)) != 0;
            const double v = graded ? graderef::apply(g.src[i], chan, g.reverse, clampBlack, clampWhite) : g.src[i];
            const double want = c < 3 ? 1.0 - v : v;
            const double gradeTol = graded ? gradefix::conditionedTolerance(g.src[i], chan, g.reverse, v, true) : 0;
            const double bound = gradeTol + gradefix::kUlpBound * (tol::ulpOf(want) + tol::ulpOf(v));
            const tol::Sample s = stats.add(dst[k][i], want, k, i);
            const bool ok = !s.classMismatch && (!s.finite || s.ulp <= gradefix::kUlpBound || s.abs <= bound);
            if (!ok && ++violations <= 10)
                ADD_FAILURE() << "case " << k << " element " << i << " got=" << dst[k][i] << " want=" << want
                              << " in=" << g.src[i] << " bound=" << bound;
        }
    }
    stats.print("pointop_chain gpu vs reference");
    EXPECT_GT(stats.compared, 1000000u);
    EXPECT_EQ(violations, 0u);
}

TEST_F(PointOpGpuTest, MaskedMatchesReference)
{
    constexpr uint32_t kW = 67;
    constexpr uint32_t kH = 19;
    std::vector<PointCase> cases;
    for (uint32_t premult : { 0u, 1u }) {
        std::mt19937 rng(10 + premult);
        std::uniform_real_distribution<float> u(0.0f, 1.0f);
        PointCase c;
        for (int ch = 0; ch < 4; ++ch) {
            c.p.a[ch] = 0.5f + 1.5f * u(rng);
            c.p.b[ch] = u(rng) - 0.5f;
            c.p.gamma[ch] = (ch == 2) ? 1.0f : 0.4f + 2.0f * u(rng);
            c.p.invGamma[ch] = 1.0f / c.p.gamma[ch];
        }
        c.p.width = kW;
        c.p.height = kH;
        c.p.channelMask = 0xF;
        c.p.flags = gradefix::kClampBlack | gradefix::kClampWhite;
        c.p.mixAmount = 0.75f;
        c.p.premult = premult;
        c.src.resize(size_t(kW) * kH * 4);
        for (float& v : c.src)
            v = 1.5f * u(rng) - 0.25f;
        for (size_t i = 0; i < c.src.size(); i += 4)
            c.src[i + 3] = (i % 17 == 0) ? 0.0f : u(rng);
        c.mask.resize(size_t(kW) * kH);
        for (float& m : c.mask)
            m = u(rng);
        cases.push_back(std::move(c));
    }
    const std::vector<std::vector<float>> dst = run(pointop_masked_spirv, pointop_masked_spirv_words,
                                                    pointop_masked_group_size, cases);

    for (const PointCase& c : cases) {
        const std::vector<float>& got = dst[&c - cases.data()];
        tol::ErrorStats stats;
        for (size_t px = 0; px < c.mask.size(); ++px) {
            double v[4], in[4], out[4];
            for (int ch = 0; ch < 4; ++ch)
                v[ch] = c.src[px * 4 + ch];
            const double alpha = v[3];
            for (int ch = 0; ch < 4; ++ch)
                in[ch] = (c.p.premult && ch < 3 && alpha != 0.0) ? v[ch] / alpha : v[ch];
            for (int ch = 0; ch < 4; ++ch) {
                out[ch] = graderef::apply(in[ch], { c.p.a[ch], c.p.b[ch], c.p.gamma[ch] }, false, true, true);
                if (c.p.premult && ch < 3)
                    out[ch] *= alpha;
            }
            const double t = double(c.mask[px]) * c.p.mixAmount;
            for (int ch = 0; ch < 4; ++ch) {
                const double want = v[ch] + (out[ch] - v[ch]) * t;
                stats.add(got[px * 4 + ch], want, px, size_t(ch));
                EXPECT_NEAR(got[px * 4 + ch], want, 1e-5) << "premult " << c.p.premult << " px " << px << " c " << ch;
            }
        }
        char label[64];
        std::snprintf(label, sizeof label, "pointop_masked gpu premult=%u", c.p.premult);
        stats.print(label);
    }
}
