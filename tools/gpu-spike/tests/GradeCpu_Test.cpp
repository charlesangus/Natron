#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

#include "grade_spirv.h"

#include "slang-cpp-prelude.h"

#include "../ref/GradeRef.h"

extern "C" void grade_main(ComputeVaryingInput*, void* entryPointParams, void* globalParams);

namespace {

// Mirrors GradeParams in grade.slang, which has no padding between members.
struct GradeParams
{
    float a[4];
    float b[4];
    float gamma[4];
    float invGamma[4];
    uint32_t width;
    uint32_t height;
    uint32_t nComps;
    uint32_t channelMask;
    uint32_t flags;
};

struct GradeGlobals
{
    StructuredBuffer<float> srcBuf;
    RWStructuredBuffer<float> dstBuf;
    GradeParams* params;
};

constexpr uint32_t kReverse = 1;
constexpr uint32_t kClampBlack = 2;
constexpr uint32_t kClampWhite = 4;

struct KnobSet
{
    float a[4], b[4], gamma[4];
};

KnobSet makeKnobs(std::mt19937& rng)
{
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    auto pick = [&](double lo, double hi) { return lo + (hi - lo) * unit(rng); };
    // Mirrors the host-side precompute in the GradeKernel constructor.
    KnobSet k{};
    for (int i = 0; i < 4; ++i) {
        const double blackPoint = pick(-1, 1);
        double whitePoint = pick(0, 4);
        if (rng() % 16 == 0)
            whitePoint = blackPoint;
        const double black = pick(-1, 1);
        const double white = pick(0, 4);
        const double multiply = pick(0, 4);
        const double offset = pick(-1, 1);
        double gamma = pick(0.2, 5);
        switch (rng() % 12) {
        case 0: gamma = 1.0; break;
        case 1: gamma = 0.0; break;
        case 2: gamma = -pick(0, 2); break;
        default: break;
        }
        const double d = whitePoint - blackPoint;
        const double A = (d != 0) ? multiply * (white - black) / d : 0;
        k.a[i] = static_cast<float>(A);
        k.b[i] = static_cast<float>(offset + black - A * blackPoint);
        k.gamma[i] = static_cast<float>(gamma);
    }
    return k;
}

std::vector<float> makeSweep()
{
    std::vector<float> v;
    for (int i = 0; i <= 9 * 512; ++i)
        v.push_back(-1.0f + static_cast<float>(i) / 512.0f);
    const float inf = std::numeric_limits<float>::infinity();
    for (float s : {std::numeric_limits<float>::quiet_NaN(), inf, -inf, 0.0f, -0.0f, 1.0f,
                    std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::min(),
                    1e-20f, -1e-20f})
        v.push_back(s);
    return v;
}

enum class Kind { Finite, Nan, PosInf, NegInf };

Kind classify(double v)
{
    if (std::isnan(v)) return Kind::Nan;
    if (std::isinf(v)) return v > 0 ? Kind::PosInf : Kind::NegInf;
    return Kind::Finite;
}

int64_t orderedBits(float f)
{
    int32_t i;
    std::memcpy(&i, &f, sizeof i);
    return i < 0 ? static_cast<int64_t>(INT32_MIN) - i : i;
}

int64_t ulpDistance(float x, float y)
{
    return std::llabs(orderedBits(x) - orderedBits(y));
}

constexpr int64_t kUlpBound = 4;
constexpr double kAbsBound = 1e-6;

double ulpOf(double m)
{
    const float f = static_cast<float>(std::fabs(m));
    if (!std::isfinite(f))
        return 0;
    return std::nextafter(f, std::numeric_limits<float>::infinity()) - f;
}

// The kernel's pow exponent is a float, so its rounding moves the result by
// about |ln(pow result)| * 2^-24 relative; the reverse path additionally
// cancels pow(v, gamma) against b. Both are properties of float arithmetic,
// not of the kernel, so the fallback bound grows with them.
double conditionedTolerance(double v, const graderef::Channel& ch, bool reverse, double result)
{
    constexpr double kBase = 4;
    if (!reverse) {
        const double x = ch.a * v + ch.b;
        if (ch.gamma > 0 && ch.gamma != 1. && x > 0) {
            const double y = std::pow(x, 1. / ch.gamma);
            return (kBase + 2 * std::fabs(std::log(y))) * ulpOf(y);
        }
        return kBase * ulpOf(result);
    }
    double p = v;
    double powTerms = kBase;
    if (ch.gamma != 1. && v > 0) {
        p = std::pow(v, ch.gamma);
        powTerms += 2 * std::fabs(std::log(p));
    }
    const double beforeDivide = p - ch.b;
    double tol = powTerms * ulpOf(p) + 2 * ulpOf(ch.b) + 2 * ulpOf(beforeDivide);
    if (ch.a != 0)
        tol = tol / std::fabs(ch.a) + 2 * ulpOf(result);
    return tol;
}

struct Stats
{
    uint64_t conditioned = 0;
    uint64_t compared = 0;
    uint64_t nonFinite = 0;
    uint64_t passthrough = 0;
    double maxAbs = 0;
    double maxRel = 0;
    int64_t maxUlp = 0;
    std::array<uint64_t, 6> hist{}; // 0, 1, 2, 3-4, 5-16, >16
    uint64_t violations = 0;
};

void record(Stats& s, float got, double refDouble, double conditionedTol)
{
    const float want = static_cast<float>(refDouble);
    const Kind kg = classify(got), kw = classify(want);
    if (kg != kw) {
        ++s.violations;
        ADD_FAILURE() << "classification mismatch got=" << got << " want=" << want;
        return;
    }
    if (kg != Kind::Finite) {
        ++s.nonFinite;
        return;
    }
    ++s.compared;
    const double abs = std::fabs(static_cast<double>(got) - refDouble);
    const int64_t ulp = ulpDistance(got, want);
    s.maxAbs = std::max(s.maxAbs, abs);
    if (std::fabs(refDouble) > 1e-3)
        s.maxRel = std::max(s.maxRel, abs / std::fabs(refDouble));
    s.maxUlp = std::max(s.maxUlp, ulp);
    ++s.hist[ulp == 0 ? 0 : ulp == 1 ? 1 : ulp == 2 ? 2 : ulp <= 4 ? 3 : ulp <= 16 ? 4 : 5];
    if (ulp > kUlpBound && abs > kAbsBound && abs <= conditionedTol)
        ++s.conditioned;
    if (ulp > kUlpBound && abs > kAbsBound && abs > conditionedTol) {
        if (++s.violations <= 10)
            ADD_FAILURE() << "got=" << got << " want=" << refDouble << " ulp=" << ulp << " abs=" << abs;
    }
}

} // namespace

TEST(GradeCpu, SpirvHasMagic)
{
    ASSERT_GT(grade_spirv_words, 5u);
    EXPECT_EQ(grade_spirv[0], 0x07230203u);
}

TEST(GradeCpu, MatchesReferenceOverSweep)
{
    std::mt19937 rng(20240607);
    const std::vector<float> sweep = makeSweep();
    const uint32_t n = static_cast<uint32_t>(sweep.size());
    constexpr uint32_t kHeight = 3;
    Stats stats;

    for (int trial = 0; trial < 150; ++trial) {
        const KnobSet knobs = makeKnobs(rng);
        for (uint32_t nComps : {1u, 3u, 4u}) {
            for (int reverse = 0; reverse < 2; ++reverse) {
                GradeParams p{};
                for (int i = 0; i < 4; ++i) {
                    p.a[i] = knobs.a[i];
                    p.b[i] = knobs.b[i];
                    p.gamma[i] = knobs.gamma[i];
                    p.invGamma[i] = static_cast<float>(1.0 / static_cast<double>(knobs.gamma[i]));
                }
                p.width = n;
                p.height = kHeight;
                p.nComps = nComps;
                p.channelMask = trial % 3 == 0 ? (rng() & 0xF) : 0xF;
                p.flags = (reverse ? kReverse : 0) | ((rng() & 1) ? kClampBlack : 0) |
                          ((rng() & 1) ? kClampWhite : 0);

                std::vector<float> src(size_t(n) * kHeight * nComps);
                for (size_t i = 0; i < src.size(); ++i)
                    src[i] = sweep[(i / nComps + (i % nComps) * 37 + (i / (n * nComps)) * 101) % n];
                std::vector<float> dst(src.size(), -123.0f);

                GradeGlobals g{{src.data(), src.size()}, {dst.data(), dst.size()}, &p};
                ComputeVaryingInput vi = {};
                vi.startGroupID = {0, 0, 0};
                vi.endGroupID = {(n + 7) / 8, (kHeight + 7) / 8, 1};
                grade_main(&vi, nullptr, &g);

                for (size_t i = 0; i < src.size(); ++i) {
                    const uint32_t c = static_cast<uint32_t>(i % nComps);
                    const uint32_t bit = nComps == 1 ? 3 : c;
                    if (!(p.channelMask & (1u << bit))) {
                        ++stats.passthrough;
                        ASSERT_EQ(0, std::memcmp(&dst[i], &src[i], sizeof(float))) << i;
                        continue;
                    }
                    const graderef::Channel ch{p.a[bit], p.b[bit], p.gamma[bit]};
                    const double want = graderef::apply(src[i], ch, reverse != 0,
                                                        (p.flags & kClampBlack) != 0,
                                                        (p.flags & kClampWhite) != 0);
                    const double tol = conditionedTolerance(src[i], ch, reverse != 0, want);
                    record(stats, dst[i], want, tol);
                }
            }
        }
    }

    std::printf("grade cpu twin vs reference: compared=%llu nonFinite=%llu passthrough=%llu\n"
                "  maxAbs=%.3g maxRel(|ref|>1e-3)=%.3g maxUlp=%lld\n"
                "  ulp histogram: 0:%llu 1:%llu 2:%llu 3-4:%llu 5-16:%llu >16:%llu\n"
                "  beyond 4 ulp / 1e-6 but within conditioned bound: %llu\n",
                (unsigned long long)stats.compared, (unsigned long long)stats.nonFinite,
                (unsigned long long)stats.passthrough, stats.maxAbs, stats.maxRel,
                (long long)stats.maxUlp, (unsigned long long)stats.hist[0],
                (unsigned long long)stats.hist[1], (unsigned long long)stats.hist[2],
                (unsigned long long)stats.hist[3], (unsigned long long)stats.hist[4],
                (unsigned long long)stats.hist[5],
                (unsigned long long)stats.conditioned);

    EXPECT_GT(stats.compared, 1000000u);
    EXPECT_GT(stats.nonFinite, 0u);
    EXPECT_EQ(stats.violations, 0u);
}
