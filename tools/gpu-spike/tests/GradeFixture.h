#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "Tolerance.h"
#include "../ref/GradeRef.h"

// Inputs, bounds and per-element checks shared by the CPU-twin and GPU Grade
// tests, so both are held to the same reference and the same tolerances.
namespace gradefix {

using tol::classify;

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

constexpr uint32_t kReverse = 1;
constexpr uint32_t kClampBlack = 2;
constexpr uint32_t kClampWhite = 4;

constexpr int64_t kUlpBound = 4;
constexpr uint32_t kHeight = 3;

struct KnobSet
{
    float a[4], b[4], gamma[4];
};

inline KnobSet makeKnobs(std::mt19937& rng)
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

inline std::vector<float> makeSweep()
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

struct GradeCase
{
    GradeParams p;
    bool reverse;
    std::vector<float> src;
};

inline std::vector<GradeCase> makeCases()
{
    std::mt19937 rng(20240607);
    const std::vector<float> sweep = makeSweep();
    const uint32_t n = static_cast<uint32_t>(sweep.size());
    std::vector<GradeCase> cases;

    for (int trial = 0; trial < 150; ++trial) {
        const KnobSet knobs = makeKnobs(rng);
        for (uint32_t nComps : {1u, 3u, 4u}) {
            for (int reverse = 0; reverse < 2; ++reverse) {
                GradeCase c{};
                GradeParams& p = c.p;
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
                c.reverse = reverse != 0;

                c.src.resize(size_t(n) * kHeight * nComps);
                for (size_t i = 0; i < c.src.size(); ++i)
                    c.src[i] = sweep[(i / nComps + (i % nComps) * 37 + (i / (n * nComps)) * 101) % n];
                cases.push_back(std::move(c));
            }
        }
    }
    return cases;
}

// Extra error a Vulkan driver may add on top of float arithmetic, per the
// SPIR-V precision table: Pow is exp2(y * log2(x)), log2 is within 2^-21
// absolute on [0.5, 2] and 3 ULP elsewhere, exp2 within 3 + 2|x| ULP, and Fma
// is only required to match a multiply followed by an add.
inline double specPowError(double x, double e, double y)
{
    using tol::ulpOf;
    if (!(x > 0) || !std::isfinite(y) || !std::isfinite(x) || y == 0)
        return 0;
    const double lx = std::log2(x);
    const double errLog = (x >= 0.5 && x <= 2.0) ? std::ldexp(1.0, -21) : 3 * ulpOf(lx);
    const double t = e * lx;
    const double errT = std::fabs(e) * errLog + 0.5 * ulpOf(t);
    return std::fabs(y) * M_LN2 * errT + (3 + 2 * std::fabs(t)) * ulpOf(y);
}

// The kernel's pow exponent is a float, so its rounding moves the result by
// about |ln(pow result)| * 2^-24 relative; the reverse path additionally
// cancels pow(v, gamma) against b. Both are properties of float arithmetic,
// not of the kernel, so the fallback bound grows with them. `spec` adds what
// a conforming GPU may do beyond that (see specPowError).
inline double conditionedTolerance(double v, const graderef::Channel& ch, bool reverse, double result,
                                   bool spec = false)
{
    using tol::ulpOf;
    constexpr double kBase = 4;
    if (!reverse) {
        const double x = ch.a * v + ch.b;
        const double unfusedProduct = spec ? ulpOf(ch.a * v) : 0;
        if (ch.gamma > 0 && ch.gamma != 1. && x > 0) {
            const double inv = 1. / ch.gamma;
            const double y = std::pow(x, inv);
            double t = (kBase + 2 * std::fabs(std::log(y))) * ulpOf(y);
            if (spec)
                t += specPowError(x, inv, y) + std::fabs(y * inv / x) * unfusedProduct;
            return t;
        }
        return kBase * ulpOf(result) + unfusedProduct;
    }
    double p = v;
    double powTerms = kBase;
    double powSpec = 0;
    if (ch.gamma != 1. && v > 0) {
        p = std::pow(v, ch.gamma);
        powTerms += 2 * std::fabs(std::log(p));
        if (spec)
            powSpec = specPowError(v, ch.gamma, p);
    }
    const double beforeDivide = p - ch.b;
    double tol = powTerms * ulpOf(p) + powSpec + 2 * ulpOf(ch.b) + 2 * ulpOf(beforeDivide);
    if (ch.a != 0)
        tol = tol / std::fabs(ch.a) + 2 * ulpOf(result);
    return tol;
}

struct Bounds
{
    int64_t ulp = kUlpBound;
    // Allow what the Vulkan precision rules permit a driver beyond the CPU
    // bounds: spec-level pow/fma error, and unspecified results for subnormal inputs.
    bool gpu = false;
};

inline bool matches(float got, double want, double conditioned, const Bounds& bounds)
{
    if (classify(got) != classify(static_cast<float>(want)))
        return false;
    if (classify(got) != tol::Kind::Finite)
        return true;
    const double abs = std::fabs(static_cast<double>(got) - want);
    return tol::ulpDistance(got, static_cast<float>(want)) <= bounds.ulp || abs <= conditioned;
}

struct GradeStats
{
    tol::ErrorStats err;
    // Beyond the plain bound but inside the per-element conditioned bound.
    uint64_t conditioned = 0;
    uint64_t passthrough = 0;
    uint64_t subnormalInputs = 0;
    uint64_t subnormalMatched = 0;
    uint64_t subnormalBetween = 0;
    uint64_t violations = 0;
    // The first few violations, for the caller to report.
    std::vector<std::string> failures;
    // Worst ratio of observed abs error to the conditioned bound, over elements past the plain bound.
    double maxConditionedUse = 0;
};

// Checks every element of one case's output against the reference, and that
// channels outside the mask came through untouched.
inline void checkCase(GradeStats& stats, size_t caseIndex, const GradeCase& c, const float* dst,
                      const Bounds& bounds = {})
{
    auto violation = [&](auto&&... parts) {
        if (++stats.violations > 10)
            return;
        std::ostringstream os;
        os << "case " << caseIndex << " element ";
        (os << ... << parts);
        stats.failures.push_back(os.str());
    };
    const GradeParams& p = c.p;
    const bool clampBlack = (p.flags & kClampBlack) != 0;
    const bool clampWhite = (p.flags & kClampWhite) != 0;
    for (size_t i = 0; i < c.src.size(); ++i) {
        const uint32_t ch = static_cast<uint32_t>(i % p.nComps);
        const uint32_t bit = p.nComps == 1 ? 3 : ch;
        if (!(p.channelMask & (1u << bit))) {
            ++stats.passthrough;
            if (std::memcmp(&dst[i], &c.src[i], sizeof(float)) != 0)
                violation(i, " changed outside the channel mask");
            continue;
        }
        const graderef::Channel chan{p.a[bit], p.b[bit], p.gamma[bit]};
        const double in = c.src[i];
        const double want = graderef::apply(c.src[i], chan, c.reverse, clampBlack, clampWhite);
        if (bounds.gpu && in != 0 && std::fabs(in) < std::numeric_limits<float>::min()) {
            // A driver may flush a subnormal to zero in some operations and not in others, so pow
            // of one can land anywhere between the two readings. Grade is monotonic in its input,
            // so the result must still lie between the values for zero and for the input.
            ++stats.subnormalInputs;
            const double zero = graderef::apply(0.0f, chan, c.reverse, clampBlack, clampWhite);
            const double tolWant = conditionedTolerance(in, chan, c.reverse, want, true);
            const double tolZero = conditionedTolerance(0, chan, c.reverse, zero, true);
            if (matches(dst[i], want, tolWant, bounds) || matches(dst[i], zero, tolZero, bounds)) {
                ++stats.subnormalMatched;
                continue;
            }
            const double got = dst[i];
            const double lo = std::min(want, zero) - std::max(tolWant, tolZero);
            const double hi = std::max(want, zero) + std::max(tolWant, tolZero);
            if (std::isfinite(got) && got >= lo && got <= hi)
                ++stats.subnormalBetween;
            else
                violation(i, " subnormal input ", c.src[i], " got=", dst[i], " outside [", want, ", ", zero, "]");
            continue;
        }
        const double conditioned = conditionedTolerance(in, chan, c.reverse, want, bounds.gpu);
        const tol::Sample s = stats.err.add(dst[i], want, caseIndex, i);
        if (s.classMismatch) {
            violation(i, " classification mismatch got=", dst[i], " want=", want, " in=", c.src[i]);
            continue;
        }
        if (!s.finite || s.ulp <= bounds.ulp)
            continue;
        stats.maxConditionedUse = std::max(stats.maxConditionedUse, s.abs / conditioned);
        if (s.abs <= conditioned) {
            ++stats.conditioned;
        } else {
            violation(i, " got=", dst[i], " want=", want, " in=", c.src[i], " ulp=", s.ulp, " abs=", s.abs,
                      " conditioned=", conditioned);
        }
    }
}

inline void printStats(const char* label, const GradeStats& s)
{
    s.err.print(label);
    std::printf("  passthrough=%llu beyond 4 ulp but within conditioned bound: %llu"
                " (worst use of that bound: %.3g)\n"
                "  subnormal inputs (not in the figures above)=%llu: matched a reading=%llu,"
                " between the readings=%llu\n",
                (unsigned long long)s.passthrough, (unsigned long long)s.conditioned, s.maxConditionedUse,
                (unsigned long long)s.subnormalInputs, (unsigned long long)s.subnormalMatched,
                (unsigned long long)s.subnormalBetween);
}

} // namespace gradefix
