#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>

// Merge operator goldens, shared by Tests/Native/MergeOps_Test.cpp (the CPU operators) and
// Tests/Gpu/MergeOpsOracle_Test.cpp (the C++-compiled Slang module). Each expected value is the
// formula in Engine/Gpu/Kernels/mergeops.slang's header, Nuke's where the table lists no deviation,
// evaluated in double precision. The non-separable rows were computed from the unpremultiplied PDF
// form (1-a)B + (1-b)A + ab·Blend(A/a, B/b), so they also check the premultiplied rewrite; rows
// with a zero alpha hold its limit.
namespace mergegolden {

struct SeparableGolden {
    const char* op;
    float A, a, B, b;
    double expected;
};

// Channels in channel-bit order; a and b are the alphas of every channel.
struct NonSeparableGolden {
    const char* op;
    float A[4];
    float a;
    float B[4];
    float b;
    double expected[4];
};

inline const SeparableGolden kSeparableGoldens[] = {
    { "atop", 0.25, 0.5, 0.5, 0.75, 0.4375 },
    { "atop", 1.5, 1.0, 3.0, 1.0, 1.5 },
    { "atop", -0.5, 0.5, -0.25, 0.75, -0.5 },
    { "atop", 0.25, 0.0, 0.5, 0.0, 0.5 },
    { "average", 0.25, 0.5, 0.5, 0.75, 0.375 },
    { "average", 1.5, 1.0, 3.0, 1.0, 2.25 },
    { "average", -0.5, 0.5, -0.25, 0.75, -0.375 },
    { "average", 0.25, 0.0, 0.5, 0.0, 0.375 },
    { "color-burn", 0.25, 0.5, 0.5, 0.75, -1 },
    { "color-burn", 1.5, 1.0, 3.0, 1.0, 2.33333333 },
    { "color-burn", -0.5, 0.5, -0.25, 0.75, -0.5 },
    { "color-burn", 0.25, 0.0, 0.5, 0.0, -1 },
    { "color-burn", 0.0, 1.0, 0.5, 1.0, 0 },
    { "color-burn", 0.5, 1.0, 1.5, 1.0, 2 },
    { "color-dodge", 0.25, 0.5, 0.5, 0.75, 0.666666667 },
    { "color-dodge", 1.5, 1.0, 3.0, 1.0, 1.5 },
    { "color-dodge", -0.5, 0.5, -0.25, 0.75, -0.166666667 },
    { "color-dodge", 0.25, 0.0, 0.5, 0.0, 0.666666667 },
    { "color-dodge", 1.0, 1.0, 0.5, 1.0, 1 },
    { "color-dodge", 2.0, 1.0, 0.5, 1.0, 2 },
    { "color-dodge", 0.5, 1.0, 2.0, 1.0, 4 },
    { "conjoint-over", 0.25, 0.5, 0.5, 0.75, 0.416666667 },
    { "conjoint-over", 1.5, 1.0, 3.0, 1.0, 1.5 },
    { "conjoint-over", -0.5, 0.5, -0.25, 0.75, -0.583333333 },
    { "conjoint-over", 0.25, 0.0, 0.5, 0.0, 0.25 },
    { "conjoint-over", 0.25, 0.25, 0.5, 0.75, 0.583333333 },
    { "conjoint-over", 0.5, 0.75, 0.5, 0.75, 0.5 },
    { "conjoint-over", 0.25, -0.5, 0.5, -0.25, 0.75 },
    { "copy", 0.25, 0.5, 0.5, 0.75, 0.25 },
    { "copy", 1.5, 1.0, 3.0, 1.0, 1.5 },
    { "copy", -0.5, 0.5, -0.25, 0.75, -0.5 },
    { "copy", 0.25, 0.0, 0.5, 0.0, 0.25 },
    { "difference", 0.25, 0.5, 0.5, 0.75, 0.25 },
    { "difference", 1.5, 1.0, 3.0, 1.0, 1.5 },
    { "difference", -0.5, 0.5, -0.25, 0.75, 0.25 },
    { "difference", 0.25, 0.0, 0.5, 0.0, 0.25 },
    { "disjoint-over", 0.25, 0.5, 0.5, 0.75, 0.583333333 },
    { "disjoint-over", 1.5, 1.0, 3.0, 1.0, 1.5 },
    { "disjoint-over", -0.5, 0.5, -0.25, 0.75, -0.666666667 },
    { "disjoint-over", 0.25, 0.0, 0.5, 0.0, 0.75 },
    { "disjoint-over", 0.25, 0.25, 0.5, 0.5, 0.75 },
    { "disjoint-over", 0.5, 1.5, 0.5, 0.75, 0.5 },
    { "disjoint-over", 0.25, 0.75, 0.5, 0.75, 0.416666667 },
    { "divide", 0.25, 0.5, 0.5, 0.75, 0.5 },
    { "divide", 1.5, 1.0, 3.0, 1.0, 0.5 },
    { "divide", -0.5, 0.5, -0.25, 0.75, 2 },
    { "divide", 0.25, 0.0, 0.5, 0.0, 0.5 },
    { "divide", 0.5, 1.0, 0.0, 1.0, 0 },
    { "divide", -0.5, 1.0, -0.25, 1.0, 2 },
    { "divide", 3.0, 1.0, 2.0, 1.0, 1.5 },
    { "exclusion", 0.25, 0.5, 0.5, 0.75, 0.5 },
    { "exclusion", 1.5, 1.0, 3.0, 1.0, -4.5 },
    { "exclusion", -0.5, 0.5, -0.25, 0.75, -1 },
    { "exclusion", 0.25, 0.0, 0.5, 0.0, 0.5 },
    { "freeze", 0.25, 0.5, 0.5, 0.75, -0.732050808 },
    { "freeze", 1.5, 1.0, 3.0, 1.0, 1.23570226 },
    { "freeze", -0.5, 0.5, -0.25, 0.75, 0 },
    { "freeze", 0.25, 0.0, 0.5, 0.0, -0.732050808 },
    { "freeze", 0.5, 1.0, 0.0, 1.0, 0 },
    { "freeze", 2.0, 1.0, 0.5, 1.0, 3 },
    { "freeze", 0.75, 1.0, -1.0, 1.0, 0 },
    { "from", 0.25, 0.5, 0.5, 0.75, 0.25 },
    { "from", 1.5, 1.0, 3.0, 1.0, 1.5 },
    { "from", -0.5, 0.5, -0.25, 0.75, 0.25 },
    { "from", 0.25, 0.0, 0.5, 0.0, 0.25 },
    { "geometric", 0.25, 0.5, 0.5, 0.75, 0.333333333 },
    { "geometric", 1.5, 1.0, 3.0, 1.0, 2 },
    { "geometric", -0.5, 0.5, -0.25, 0.75, -0.333333333 },
    { "geometric", 0.25, 0.0, 0.5, 0.0, 0.333333333 },
    { "geometric", 0.5, 1.0, -0.5, 1.0, 0 },
    { "grain-extract", 0.25, 0.5, 0.5, 0.75, 0.75 },
    { "grain-extract", 1.5, 1.0, 3.0, 1.0, 2 },
    { "grain-extract", -0.5, 0.5, -0.25, 0.75, 0.75 },
    { "grain-extract", 0.25, 0.0, 0.5, 0.0, 0.75 },
    { "grain-merge", 0.25, 0.5, 0.5, 0.75, 0.25 },
    { "grain-merge", 1.5, 1.0, 3.0, 1.0, 4 },
    { "grain-merge", -0.5, 0.5, -0.25, 0.75, -1.25 },
    { "grain-merge", 0.25, 0.0, 0.5, 0.0, 0.25 },
    { "hard-light", 0.25, 0.5, 0.5, 0.75, 0.25 },
    { "hard-light", 1.5, 1.0, 3.0, 1.0, 3 },
    { "hard-light", -0.5, 0.5, -0.25, 0.75, 0.25 },
    { "hard-light", 0.25, 0.0, 0.5, 0.0, 0.25 },
    { "hard-light", 0.5, 1.0, 0.25, 1.0, 0.25 },
    { "hard-light", 2.0, 1.0, 0.5, 1.0, 3 },
    { "hypot", 0.25, 0.5, 0.5, 0.75, 0.559016994 },
    { "hypot", 1.5, 1.0, 3.0, 1.0, 3.35410197 },
    { "hypot", -0.5, 0.5, -0.25, 0.75, 0.559016994 },
    { "hypot", 0.25, 0.0, 0.5, 0.0, 0.559016994 },
    { "in", 0.25, 0.5, 0.5, 0.75, 0.1875 },
    { "in", 1.5, 1.0, 3.0, 1.0, 1.5 },
    { "in", -0.5, 0.5, -0.25, 0.75, -0.375 },
    { "in", 0.25, 0.0, 0.5, 0.0, 0 },
    { "mask", 0.25, 0.5, 0.5, 0.75, 0.25 },
    { "mask", 1.5, 1.0, 3.0, 1.0, 3 },
    { "mask", -0.5, 0.5, -0.25, 0.75, -0.125 },
    { "mask", 0.25, 0.0, 0.5, 0.0, 0 },
    { "matte", 0.25, 0.5, 0.5, 0.75, 0.375 },
    { "matte", 1.5, 1.0, 3.0, 1.0, 1.5 },
    { "matte", -0.5, 0.5, -0.25, 0.75, -0.375 },
    { "matte", 0.25, 0.0, 0.5, 0.0, 0.5 },
    { "max", 0.25, 0.5, 0.5, 0.75, 0.5 },
    { "max", 1.5, 1.0, 3.0, 1.0, 3 },
    { "max", -0.5, 0.5, -0.25, 0.75, -0.25 },
    { "max", 0.25, 0.0, 0.5, 0.0, 0.5 },
    { "min", 0.25, 0.5, 0.5, 0.75, 0.25 },
    { "min", 1.5, 1.0, 3.0, 1.0, 1.5 },
    { "min", -0.5, 0.5, -0.25, 0.75, -0.5 },
    { "min", 0.25, 0.0, 0.5, 0.0, 0.25 },
    { "minus", 0.25, 0.5, 0.5, 0.75, -0.25 },
    { "minus", 1.5, 1.0, 3.0, 1.0, -1.5 },
    { "minus", -0.5, 0.5, -0.25, 0.75, -0.25 },
    { "minus", 0.25, 0.0, 0.5, 0.0, -0.25 },
    { "multiply", 0.25, 0.5, 0.5, 0.75, 0.125 },
    { "multiply", 1.5, 1.0, 3.0, 1.0, 4.5 },
    { "multiply", -0.5, 0.5, -0.25, 0.75, 0.125 },
    { "multiply", 0.25, 0.0, 0.5, 0.0, 0.125 },
    { "multiply", -2.0, 1.0, -3.0, 1.0, 6 },
    { "out", 0.25, 0.5, 0.5, 0.75, 0.0625 },
    { "out", 1.5, 1.0, 3.0, 1.0, 0 },
    { "out", -0.5, 0.5, -0.25, 0.75, -0.125 },
    { "out", 0.25, 0.0, 0.5, 0.0, 0.25 },
    { "over", 0.25, 0.5, 0.5, 0.75, 0.5 },
    { "over", 1.5, 1.0, 3.0, 1.0, 1.5 },
    { "over", -0.5, 0.5, -0.25, 0.75, -0.625 },
    { "over", 0.25, 0.0, 0.5, 0.0, 0.75 },
    { "overlay", 0.25, 0.5, 0.5, 0.75, 0.25 },
    { "overlay", 1.5, 1.0, 3.0, 1.0, 5 },
    { "overlay", -0.5, 0.5, -0.25, 0.75, 0.25 },
    { "overlay", 0.25, 0.0, 0.5, 0.0, 0.25 },
    { "overlay", 0.25, 1.0, 0.5, 1.0, 0.25 },
    { "overlay", 0.5, 1.0, 2.0, 1.0, 3 },
    { "pin-light", 0.25, 0.5, 0.5, 0.75, 0.5 },
    { "pin-light", 1.5, 1.0, 3.0, 1.0, 3 },
    { "pin-light", -0.5, 0.5, -0.25, 0.75, -1 },
    { "pin-light", 0.25, 0.0, 0.5, 0.0, 0.5 },
    { "pin-light", 0.25, 1.0, 2.0, 1.0, 0.5 },
    { "pin-light", 0.75, 1.0, -1.0, 1.0, 0.5 },
    { "pin-light", 0.5, 1.0, 0.25, 1.0, 0.25 },
    { "plus", 0.25, 0.5, 0.5, 0.75, 0.75 },
    { "plus", 1.5, 1.0, 3.0, 1.0, 4.5 },
    { "plus", -0.5, 0.5, -0.25, 0.75, -0.75 },
    { "plus", 0.25, 0.0, 0.5, 0.0, 0.75 },
    { "reflect", 0.25, 0.5, 0.5, 0.75, 0.125 },
    { "reflect", 1.5, 1.0, 3.0, 1.0, 3 },
    { "reflect", -0.5, 0.5, -0.25, 0.75, 0.2 },
    { "reflect", 0.25, 0.0, 0.5, 0.0, 0.125 },
    { "reflect", 0.5, 1.0, 1.0, 1.0, 1 },
    { "reflect", 2.0, 1.0, 0.5, 1.0, 8 },
    { "screen", 0.25, 0.5, 0.5, 0.75, 0.625 },
    { "screen", 1.5, 1.0, 3.0, 1.0, 3 },
    { "screen", -0.5, 0.5, -0.25, 0.75, -0.875 },
    { "screen", 0.25, 0.0, 0.5, 0.0, 0.625 },
    { "screen", 1.0, 1.0, 3.0, 1.0, 3 },
    { "screen", 0.5, 1.0, 2.0, 1.0, 2 },
    { "screen", 1.0, 1.0, -1.0, 1.0, 1 },
    { "screen", 2.0, 1.0, -1.0, 1.0, 2 },
    { "soft-light", 0.25, 0.5, 0.5, 0.75, 0.46875 },
    { "soft-light", 1.5, 1.0, 3.0, 1.0, 9 },
    { "soft-light", -0.5, 0.5, -0.25, 0.75, 0.3046875 },
    { "soft-light", 0.25, 0.0, 0.5, 0.0, 0.46875 },
    { "soft-light", 0.5, 1.0, 0.5, 1.0, 0.6875 },
    { "soft-light", 4.0, 1.0, 0.5, 1.0, 4 },
    { "stencil", 0.25, 0.5, 0.5, 0.75, 0.25 },
    { "stencil", 1.5, 1.0, 3.0, 1.0, 0 },
    { "stencil", -0.5, 0.5, -0.25, 0.75, -0.125 },
    { "stencil", 0.25, 0.0, 0.5, 0.0, 0.5 },
    { "under", 0.25, 0.5, 0.5, 0.75, 0.5625 },
    { "under", 1.5, 1.0, 3.0, 1.0, 3 },
    { "under", -0.5, 0.5, -0.25, 0.75, -0.375 },
    { "under", 0.25, 0.0, 0.5, 0.0, 0.75 },
    { "xor", 0.25, 0.5, 0.5, 0.75, 0.3125 },
    { "xor", 1.5, 1.0, 3.0, 1.0, 0 },
    { "xor", -0.5, 0.5, -0.25, 0.75, -0.25 },
    { "xor", 0.25, 0.0, 0.5, 0.0, 0.75 },
};

inline const NonSeparableGolden kNonSeparableGoldens[] = {
    { "hue", { 0.4, 0.2, 0.1, 0.5 }, 0.5, { 0.1, 0.3, 0.6, 0.75 }, 0.75, { 0.412333333, 0.295666667, 0.337333333, 0.875 } },
    { "hue", { 2.0, 0.5, 0.25, 1.0 }, 1.0, { 0.25, 1.0, 3.0, 1.0 }, 1.0, { 2.68821429, 0.331071429, -0.0617857143, 1 } },
    { "hue", { -0.25, 0.5, 0.1, 1.0 }, 1.0, { 0.5, 0.25, 0.75, 0.5 }, 0.5, { -0.0656666667, 0.809333333, 0.342666667, 1 } },
    { "hue", { 0.4, 0.2, 0.1, 0.0 }, 0.0, { 0.1, 0.3, 0.6, 0.75 }, 0.75, { 0.2, 0.35, 0.625, 0.75 } },
    { "hue", { 0.4, 0.2, 0.1, 0.5 }, 0.5, { 0.2, 0.2, 0.2, 0.0 }, 0.0, { 0.6, 0.4, 0.3, 0.5 } },
    { "saturation", { 0.4, 0.2, 0.1, 0.5 }, 0.5, { 0.1, 0.3, 0.6, 0.75 }, 0.75, { 0.20865, 0.34865, 0.60865, 0.875 } },
    { "saturation", { 2.0, 0.5, 0.25, 1.0 }, 1.0, { 0.25, 1.0, 3.0, 1.0 }, 1.0, { 0.520909091, 0.998181818, 2.27090909, 1 } },
    { "saturation", { -0.25, 0.5, 0.1, 1.0 }, 1.0, { 0.5, 0.25, 0.75, 0.5 }, 0.5, { 0.345, 0.5325, 0.7075, 1 } },
    { "saturation", { 0.4, 0.2, 0.1, 0.0 }, 0.0, { 0.1, 0.3, 0.6, 0.75 }, 0.75, { 0.12215, 0.36215, 0.77215, 0.75 } },
    { "saturation", { 0.4, 0.2, 0.1, 0.5 }, 0.5, { 0.2, 0.2, 0.2, 0.0 }, 0.0, { 0.6, 0.4, 0.3, 0.5 } },
    { "color", { 0.4, 0.2, 0.1, 0.5 }, 0.5, { 0.1, 0.3, 0.6, 0.75 }, 0.75, { 0.39975, 0.29975, 0.34975, 0.875 } },
    { "color", { 2.0, 0.5, 0.25, 1.0 }, 1.0, { 0.25, 1.0, 3.0, 1.0 }, 1.0, { 2.0725, 0.5725, 0.3225, 1 } },
    { "color", { -0.25, 0.5, 0.1, 1.0 }, 1.0, { 0.5, 0.25, 0.75, 0.5 }, 0.5, { 0.0145, 0.7645, 0.3645, 1 } },
    { "color", { 0.4, 0.2, 0.1, 0.0 }, 0.0, { 0.1, 0.3, 0.6, 0.75 }, 0.75, { 0.31325, 0.31325, 0.51325, 0.75 } },
    { "color", { 0.4, 0.2, 0.1, 0.5 }, 0.5, { 0.2, 0.2, 0.2, 0.0 }, 0.0, { 0.6, 0.4, 0.3, 0.5 } },
    { "luminosity", { 0.4, 0.2, 0.1, 0.5 }, 0.5, { 0.1, 0.3, 0.6, 0.75 }, 0.75, { 0.25025, 0.40025, 0.67525, 0.875 } },
    { "luminosity", { 2.0, 0.5, 0.25, 1.0 }, 1.0, { 0.25, 1.0, 3.0, 1.0 }, 1.0, { 0.1775, 0.9275, 2.9275, 1 } },
    { "luminosity", { -0.25, 0.5, 0.1, 1.0 }, 1.0, { 0.5, 0.25, 0.75, 0.5 }, 0.5, { 0.1105, 0.2355, 0.5355, 1 } },
    { "luminosity", { 0.4, 0.2, 0.1, 0.0 }, 0.0, { 0.1, 0.3, 0.6, 0.75 }, 0.75, { 0.38675, 0.53675, 0.81175, 0.75 } },
    { "luminosity", { 0.4, 0.2, 0.1, 0.5 }, 0.5, { 0.2, 0.2, 0.2, 0.0 }, 0.0, { 0.5, 0.3, 0.2, 0.5 } },
};

// A float result against a double golden: a few float roundings of the operands and the operation.
inline bool
goldenAccepts(double expected,
              float got)
{
    return std::abs(double(got) - expected) <= 2e-6 * std::max(1.0, std::abs(expected));
}

// The parity class of an operator (Engine/Nodes/README.md "Parity tolerances"): exact for sums,
// products and selects, arithmetic once a division enters, transcendental with a square root or the
// HSL blend. Returns the relative bound.
inline double
operatorTolerance(const char* op)
{
    static const char* const kArithmetic[] = { "color-burn", "color-dodge", "conjoint-over", "disjoint-over",
                                               "divide", "geometric", "reflect" };
    static const char* const kTranscendental[] = { "freeze", "hypot", "hue", "saturation", "color", "luminosity" };
    for (const char* n : kArithmetic) {
        if (std::strcmp(n, op) == 0) {
            return 1e-6;
        }
    }
    for (const char* n : kTranscendental) {
        if (std::strcmp(n, op) == 0) {
            return 1e-5;
        }
    }

    return 0.;
}

// ParityTolerance's rule: a pair of NaNs matches, as do equal infinities.
inline bool
parityAccepts(double relative,
              float reference,
              float got)
{
    if (std::isnan(reference) || std::isnan(got)) {
        return std::isnan(reference) && std::isnan(got);
    }
    if (reference == got) {
        return true;
    }

    return std::abs(double(got) - double(reference)) <= relative * std::max(1.0, std::abs(double(reference)));
}

} // namespace mergegolden
