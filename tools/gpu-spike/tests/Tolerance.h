#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

// Compares a float result with a double-precision reference and accumulates
// max abs, max relative, a ULP histogram and the location of the worst pixel.
namespace tol {

enum class Kind { Finite, Nan, PosInf, NegInf };

inline Kind classify(double v)
{
    if (std::isnan(v)) return Kind::Nan;
    if (std::isinf(v)) return v > 0 ? Kind::PosInf : Kind::NegInf;
    return Kind::Finite;
}

inline int64_t orderedBits(float f)
{
    int32_t i;
    std::memcpy(&i, &f, sizeof i);
    return i < 0 ? static_cast<int64_t>(INT32_MIN) - i : i;
}

inline int64_t ulpDistance(float x, float y)
{
    return std::llabs(orderedBits(x) - orderedBits(y));
}

// Spacing of floats at the magnitude of m.
inline double ulpOf(double m)
{
    const float f = static_cast<float>(std::fabs(m));
    if (!std::isfinite(f))
        return 0;
    return std::nextafter(f, std::numeric_limits<float>::infinity()) - f;
}

struct Worst
{
    size_t group = 0;
    size_t index = 0;
    float got = 0;
    double want = 0;
    double abs = 0;
    int64_t ulp = 0;
};

struct Sample
{
    bool classMismatch = false;
    bool finite = false;
    double abs = 0;
    int64_t ulp = 0;
};

struct ErrorStats
{
    uint64_t compared = 0;
    uint64_t nonFinite = 0;
    uint64_t classMismatches = 0;
    double maxAbs = 0;
    // Only over references with |ref| > 1e-3, where a relative figure means something.
    double maxRel = 0;
    int64_t maxUlp = 0;
    std::array<uint64_t, 6> hist{}; // 0, 1, 2, 3-4, 5-16, >16
    Worst worst;

    // `group` and `index` only label the worst pixel: group is a case or frame number.
    Sample add(float got, double ref, size_t group, size_t index)
    {
        const float want = static_cast<float>(ref);
        Sample s;
        if (classify(got) != classify(want)) {
            ++classMismatches;
            s.classMismatch = true;
            return s;
        }
        if (classify(got) != Kind::Finite) {
            ++nonFinite;
            return s;
        }
        s.finite = true;
        ++compared;
        s.abs = std::fabs(static_cast<double>(got) - ref);
        s.ulp = ulpDistance(got, want);
        if (s.abs >= maxAbs)
            worst = {group, index, got, ref, s.abs, s.ulp};
        maxAbs = std::max(maxAbs, s.abs);
        if (std::fabs(ref) > 1e-3)
            maxRel = std::max(maxRel, s.abs / std::fabs(ref));
        maxUlp = std::max(maxUlp, s.ulp);
        ++hist[s.ulp == 0 ? 0 : s.ulp == 1 ? 1 : s.ulp == 2 ? 2 : s.ulp <= 4 ? 3 : s.ulp <= 16 ? 4 : 5];
        return s;
    }

    void print(const char* label) const
    {
        std::printf("%s: compared=%llu nonFinite=%llu\n"
                    "  maxAbs=%.3g maxRel(|ref|>1e-3)=%.3g maxUlp=%lld\n"
                    "  ulp histogram: 0:%llu 1:%llu 2:%llu 3-4:%llu 5-16:%llu >16:%llu\n"
                    "  worst: group=%zu index=%zu got=%.9g want=%.9g abs=%.3g ulp=%lld\n",
                    label, (unsigned long long)compared, (unsigned long long)nonFinite, maxAbs, maxRel,
                    (long long)maxUlp, (unsigned long long)hist[0], (unsigned long long)hist[1],
                    (unsigned long long)hist[2], (unsigned long long)hist[3], (unsigned long long)hist[4],
                    (unsigned long long)hist[5], worst.group, worst.index, (double)worst.got, worst.want,
                    worst.abs, (long long)worst.ulp);
    }
};

} // namespace tol
