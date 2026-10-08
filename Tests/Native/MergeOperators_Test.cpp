/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Merge/MergeOperators.h"

NATRON_NAMESPACE_USING

namespace {
using namespace MergeOperators;

const float kValues[] = { -0.5f, 0.f, 0.25f, 0.5f, 1.f, 2.f };
const float kAlphas[] = { 0.f, 0.25f, 0.5f, 1.f };

// Independent double-precision statement of each separable operator, from the SVG compositing
// formulas. A and B are premultiplied; a and b are their alphas.
double
reference(Operation op,
          double A,
          double B,
          double a,
          double b)
{
    switch (op) {
    case eATop:
        // Ab + B(1 - a)
        return A * b + B * (1 - a);
    case eAverage:
        // (A + B) / 2
        return (A + B) / 2;
    case eColorBurn:
        // A <= 0: A; otherwise 1 - min(1, (1 - B) / A)
        return A <= 0 ? A : 1 - std::min(1., (1 - B) / A);
    case eColorDodge:
        // A >= 1: A; otherwise min(1, B / (1 - A))
        return A >= 1 ? A : std::min(1., B / (1 - A));
    case eConjointOver:
        // a > b: A; b <= 0: A + B; otherwise A + B(1 - a/b)
        return a > b ? A : (b <= 0 ? A + B : A + B * (1 - a / b));
    case eCopy:
        // A
        return A;
    case eDifference:
        // |A - B|
        return std::fabs(A - B);
    case eDisjointOver:
        // a >= 1: A; a + b < 1: A + B; b <= 0: A + B(1 - a); otherwise A + B(1 - a)/b
        if (a >= 1) {
            return A;
        } else if (a + b < 1) {
            return A + B;
        } else if (b <= 0) {
            return A + B * (1 - a);
        }

        return A + B * (1 - a) / b;
    case eDivide:
        // B <= 0: 0; otherwise A / B
        return B <= 0 ? 0 : A / B;
    case eExclusion:
        // A + B - 2AB
        return A + B - 2 * A * B;
    case eFreeze:
        // B <= 0: 0; otherwise max(0, 1 - sqrt(max(0, 1 - A)) / B)
        return B <= 0 ? 0 : std::max(0., 1 - std::sqrt(std::max(0., 1 - A)) / B);
    case eFrom:
        // B - A
        return B - A;
    case eGeometric:
        // A + B == 0: 0; otherwise 2AB / (A + B)
        return A + B == 0 ? 0 : 2 * A * B / (A + B);
    case eGrainExtract:
        // B - A + 0.5
        return B - A + 0.5;
    case eGrainMerge:
        // B + A - 0.5
        return B + A - 0.5;
    case eHardLight: {
        // 2A < 1: multiply(2A, B); otherwise screen(2A - 1, B)
        double x = 2 * A;
        if (x < 1) {
            return (x < 0 && B < 0) ? x : x * B;
        }
        double s = x - 1;

        return (s <= 1 || B <= 1) ? s + B - s * B : std::max(s, B);
    }
    case eHypot:
        // sqrt(A^2 + B^2)
        return std::sqrt(A * A + B * B);
    case eIn:
        // Ab
        return A * b;
    case eMask:
        // Ba
        return B * a;
    case eMatte:
        // Aa + B(1 - a)
        return A * a + B * (1 - a);
    case eMax:
        // max(A, B)
        return std::max(A, B);
    case eMin:
        // min(A, B)
        return std::min(A, B);
    case eMinus:
        // A - B
        return A - B;
    case eMultiply:
        // A < 0 and B < 0: A; otherwise AB
        return (A < 0 && B < 0) ? A : A * B;
    case eOut:
        // A(1 - b)
        return A * (1 - b);
    case eOver:
        // A + B(1 - a)
        return A + B * (1 - a);
    case eOverlay:
        // hardLight with A and B swapped
        return reference(eHardLight, B, A, a, b);
    case ePinLight:
        // A >= 0.5: max(B, 2(A - 0.5)); otherwise min(B, 2A)
        return A >= 0.5 ? std::max(B, 2 * (A - 0.5)) : std::min(B, 2 * A);
    case ePlus:
        // A + B
        return A + B;
    case eReflect:
        // B >= 1: 1; otherwise min(1, A^2 / (1 - B))
        return B >= 1 ? 1 : std::min(1., A * A / (1 - B));
    case eScreen:
        // A <= 1 or B <= 1: A + B - AB; otherwise max(A, B)
        return (A <= 1 || B <= 1) ? A + B - A * B : std::max(A, B);
    case eSoftLight:
        // W3C compositing-1 soft-light
        if (A <= 0.5) {
            return B - (1 - 2 * A) * B * (1 - B);
        } else {
            double d = B <= 0.25 ? ((16 * B - 12) * B + 4) * B : std::sqrt(B);

            return B + (2 * A - 1) * (d - B);
        }
    case eStencil:
        // B(1 - a)
        return B * (1 - a);
    case eUnder:
        // A(1 - b) + B
        return A * (1 - b) + B;
    case eXOR:
        // A(1 - b) + B(1 - a)
        return A * (1 - b) + B * (1 - a);
    case eColor:
    case eHue:
    case eLuminosity:
    case eSaturation:
    case eOperationCount:
        break;
    }

    return 0;
}

::testing::AssertionResult
closeTo(double got,
        double want)
{
    if (std::isnan(want)) {
        return std::isnan(got) ? ::testing::AssertionSuccess() : ::testing::AssertionFailure() << got << " is not NaN";
    }
    double tol = 1e-5 * std::max(1., std::fabs(want));
    if (std::fabs(got - want) <= tol) {
        return ::testing::AssertionSuccess();
    }

    return ::testing::AssertionFailure() << "got " << got << ", want " << want;
}

double
lum(const float* c)
{
    return c[0] * 0.3 + c[1] * 0.59 + c[2] * 0.11;
}

double
sat(const float* c)
{
    return std::max(std::max(c[0], c[1]), c[2]) - std::min(std::min(c[0], c[1]), c[2]);
}
} // namespace

TEST(MergeOperators, ThirtyNineOperatorsInOfxOrderWithOfxIds)
{
    static const char* const kIds[eOperationCount] = {
        "atop", "average", "color", "color-burn", "color-dodge", "conjoint-over", "copy", "difference",
        "disjoint-over", "divide", "exclusion", "freeze", "from", "geometric", "grain-extract",
        "grain-merge", "hard-light", "hue", "hypot", "in", "luminosity", "mask", "matte", "max", "min",
        "minus", "multiply", "out", "over", "overlay", "pinlight", "plus", "reflect", "saturation",
        "screen", "soft-light", "stencil", "under", "xor"
    };

    ASSERT_EQ(39, (int)eOperationCount);
    std::set<std::string> seen;
    for (int i = 0; i < eOperationCount; ++i) {
        EXPECT_STREQ(kIds[i], operationId((Operation)i));
        EXPECT_NE(std::string(), std::string(operationHint((Operation)i)));
        EXPECT_TRUE(seen.insert(operationId((Operation)i)).second);
        Operation back = eATop;
        ASSERT_TRUE(operationFromId(kIds[i], &back));
        EXPECT_EQ(i, (int)back);
    }
    Operation untouched = eXOR;
    EXPECT_FALSE(operationFromId("interpolated", &untouched));
    EXPECT_EQ((int)eXOR, (int)untouched);
    EXPECT_EQ(eATop, 0);
    EXPECT_EQ(eXOR, 38);
}

TEST(MergeOperators, Predicates)
{
    const std::set<int> notMaskable = { eATop, eConjointOver, eCopy, eDisjointOver, eIn, eMask, eMatte, eOut,
                                        eOver, eScreen, eStencil, eUnder, eXOR, eHue, eSaturation, eColor,
                                        eLuminosity };
    const std::set<int> identityForB = { eATop, eExclusion, eMatte, eOver, ePlus, eScreen, eStencil, eUnder, eXOR };
    const std::set<int> nonSeparable = { eHue, eSaturation, eColor, eLuminosity };

    for (int i = 0; i < eOperationCount; ++i) {
        Operation op = (Operation)i;
        EXPECT_EQ(notMaskable.count(i) == 0, isMaskable(op)) << operationId(op);
        EXPECT_EQ(identityForB.count(i) != 0, isIdentityForBOnly(op)) << operationId(op);
        EXPECT_EQ(nonSeparable.count(i) == 0, isSeparable(op)) << operationId(op);
    }
}

TEST(MergeOperators, IdentityForBOnlyOperatorsReturnBForTransparentBlackA)
{
    for (int i = 0; i < eOperationCount; ++i) {
        Operation op = (Operation)i;
        if (!isIdentityForBOnly(op)) {
            continue;
        }
        for (float bv : kValues) {
            for (float bAlpha : kAlphas) {
                const float A[4] = { 0.f, 0.f, 0.f, 0.f };
                const float B[4] = { bv, bv * 0.5f, 0.25f, bAlpha };
                float out[4];
                mergePixel(op, false, A, B, 4, out);
                for (int c = 0; c < 4; ++c) {
                    EXPECT_TRUE(closeTo(out[c], B[c])) << operationId(op) << " c=" << c;
                }
            }
        }
    }
}

TEST(MergeOperators, EverySeparableOperatorMatchesTheIndependentTable)
{
    int checked = 0;
    for (int i = 0; i < eOperationCount; ++i) {
        Operation op = (Operation)i;
        if (!isSeparable(op)) {
            continue;
        }
        for (float Av : kValues) {
            for (float Bv : kValues) {
                for (float a : kAlphas) {
                    for (float b : kAlphas) {
                        float out = 0.f;
                        mergePixel(op, false, &Av, a, &Bv, b, 1, &out);
                        EXPECT_TRUE(closeTo(out, reference(op, Av, Bv, a, b)))
                            << operationId(op) << " A=" << Av << " B=" << Bv << " a=" << a << " b=" << b;
                        ++checked;
                    }
                }
            }
        }
    }
    EXPECT_EQ(35 * 6 * 6 * 4 * 4, checked);
}

TEST(MergeOperators, RgbaAppliesTheOperatorToAlphaWithoutMasking)
{
    const float A[4] = { 0.2f, 0.4f, 0.6f, 0.5f };
    const float B[4] = { 0.1f, 0.3f, 0.5f, 0.75f };
    for (int i = 0; i < eOperationCount; ++i) {
        Operation op = (Operation)i;
        if (!isSeparable(op)) {
            continue;
        }
        float out[4];
        mergePixel(op, false, A, B, 4, out);
        const bool matte = (op == eMatte);
        for (int c = 0; c < 4; ++c) {
            double want = reference(op, A[c], B[c], A[3], B[3]);
            if (matte && c == 3) {
                want = A[3] + B[3] - A[3] * B[3];
            }
            EXPECT_TRUE(closeTo(out[c], want)) << operationId(op) << " c=" << c;
        }
    }
}

TEST(MergeOperators, AlphaMaskingWritesUnionAlphaForMaskableOperators)
{
    const float A[4] = { 0.2f, 0.4f, 0.6f, 0.5f };
    const float B[4] = { 0.1f, 0.3f, 0.5f, 0.75f };
    const double unionAlpha = 0.5 + 0.75 - 0.5 * 0.75;
    for (int i = 0; i < eOperationCount; ++i) {
        Operation op = (Operation)i;
        if (!isSeparable(op)) {
            continue;
        }
        float out[4];
        mergePixel(op, true, A, B, 4, out);
        const bool masked = isMaskable(op) || op == eMatte;
        for (int c = 0; c < 3; ++c) {
            EXPECT_TRUE(closeTo(out[c], reference(op, A[c], B[c], A[3], B[3]))) << operationId(op);
        }
        if (masked) {
            EXPECT_TRUE(closeTo(out[3], unionAlpha)) << operationId(op);
        } else {
            EXPECT_TRUE(closeTo(out[3], reference(op, A[3], B[3], A[3], B[3]))) << operationId(op);
        }
    }
}

TEST(MergeOperators, MatteForcesAlphaMaskingEvenWhenOff)
{
    const float A[4] = { 0.2f, 0.4f, 0.6f, 0.5f };
    const float B[4] = { 0.1f, 0.3f, 0.5f, 0.75f };
    float off[4], on[4];
    mergePixel(eMatte, false, A, B, 4, off);
    mergePixel(eMatte, true, A, B, 4, on);
    for (int c = 0; c < 4; ++c) {
        EXPECT_EQ(on[c], off[c]);
    }
    EXPECT_TRUE(closeTo(off[3], 0.5 + 0.75 - 0.5 * 0.75));
}

TEST(MergeOperators, AlphaMaskingIsIgnoredWithoutAFourthChannel)
{
    const float A[4] = { 0.2f, 0.4f, 0.6f, 0.f };
    const float B[4] = { 0.1f, 0.3f, 0.5f, 0.f };
    for (int n = 1; n <= 3; ++n) {
        float masked[4] = { 9.f, 9.f, 9.f, 9.f };
        float plain[4] = { 9.f, 9.f, 9.f, 9.f };
        mergePixel(eMultiply, true, A, B, n, masked);
        mergePixel(eMultiply, false, A, B, n, plain);
        for (int c = 0; c < 4; ++c) {
            EXPECT_EQ(plain[c], masked[c]);
        }
        EXPECT_EQ(9.f, plain[3]);
    }
}

TEST(MergeOperators, DerivedAlphaFollowsTheComponentCount)
{
    const float A[4] = { 0.5f, 0.4f, 0.6f, 0.25f };
    const float B[4] = { 0.25f, 0.3f, 0.5f, 0.75f };
    float derived[4], explicitAlpha[4];

    mergePixel(eOver, false, A, B, 1, derived);
    mergePixel(eOver, false, A, 0.5f, B, 0.25f, 1, explicitAlpha);
    EXPECT_EQ(explicitAlpha[0], derived[0]);

    mergePixel(eOver, false, A, B, 2, derived);
    mergePixel(eOver, false, A, 1.f, B, 1.f, 2, explicitAlpha);
    EXPECT_EQ(explicitAlpha[0], derived[0]);
    EXPECT_EQ(explicitAlpha[1], derived[1]);
}

TEST(MergeOperators, OutputMayAliasB)
{
    for (int i = 0; i < eOperationCount; ++i) {
        Operation op = (Operation)i;
        for (int masking = 0; masking < 2; ++masking) {
            const float A[4] = { 0.2f, 0.4f, 0.6f, 0.5f };
            const float B[4] = { 0.1f, 0.3f, 0.5f, 0.75f };
            float inPlace[4] = { B[0], B[1], B[2], B[3] };
            float apart[4];
            mergePixel(op, masking != 0, A, inPlace, 4, inPlace);
            mergePixel(op, masking != 0, A, B, 4, apart);
            for (int c = 0; c < 4; ++c) {
                EXPECT_EQ(apart[c], inPlace[c]) << operationId(op);
            }
        }
    }
}

TEST(MergeOperators, HslModesLeaveBWhereAIsTransparent)
{
    const Operation hsl[] = { eHue, eSaturation, eColor, eLuminosity };
    const float A[4] = { 0.f, 0.f, 0.f, 0.f };
    const float B[4] = { 0.2f, 0.5f, 0.3f, 0.8f };
    for (Operation op : hsl) {
        float out[4];
        mergePixel(op, false, A, B, 4, out);
        for (int c = 0; c < 4; ++c) {
            EXPECT_TRUE(closeTo(out[c], B[c])) << operationId(op);
        }
    }
}

TEST(MergeOperators, HslModesOnOpaqueInputsKeepTheDocumentedLuminanceAndSaturation)
{
    // A and B are in gamut, so the clip step is inactive and the defining identities hold.
    const float A[4] = { 0.6f, 0.3f, 0.2f, 1.f };
    const float B[4] = { 0.35f, 0.4f, 0.5f, 1.f };
    float hue[4], saturation[4], color[4], luminosity[4];
    mergePixel(eHue, false, A, B, 4, hue);
    mergePixel(eSaturation, false, A, B, 4, saturation);
    mergePixel(eColor, false, A, B, 4, color);
    mergePixel(eLuminosity, false, A, B, 4, luminosity);

    // Hue: SetLum(SetSat(A, Sat(B)), Lum(B))
    EXPECT_TRUE(closeTo(sat(hue), sat(B)));
    EXPECT_TRUE(closeTo(lum(hue), lum(B)));
    // Saturation: SetLum(SetSat(B, Sat(A)), Lum(B))
    EXPECT_TRUE(closeTo(sat(saturation), sat(A)));
    EXPECT_TRUE(closeTo(lum(saturation), lum(B)));
    // Color: SetLum(A, Lum(B))
    EXPECT_TRUE(closeTo(lum(color), lum(B)));
    EXPECT_TRUE(closeTo(color[0] - color[1], A[0] - A[1]));
    // Luminosity: SetLum(B, Lum(A))
    EXPECT_TRUE(closeTo(lum(luminosity), lum(A)));
    EXPECT_TRUE(closeTo(luminosity[0] - luminosity[1], B[0] - B[1]));

    for (const float* out : { hue, saturation, color, luminosity }) {
        EXPECT_TRUE(closeTo(out[3], 1.0));
    }
}

TEST(MergeOperators, HslModesWriteUnionAlphaAndSkipColourBelowThreeComponents)
{
    const float A[4] = { 0.3f, 0.2f, 0.1f, 0.5f };
    const float B[4] = { 0.2f, 0.3f, 0.4f, 0.75f };
    float out[4];
    mergePixel(eColor, true, A, B, 4, out);
    EXPECT_TRUE(closeTo(out[3], 0.5 + 0.75 - 0.5 * 0.75));

    float two[4] = { 9.f, 9.f, 9.f, 9.f };
    mergePixel(eHue, false, A, B, 2, two);
    EXPECT_TRUE(closeTo(two[0], 0.0));
    EXPECT_EQ(9.f, two[2]);
}

namespace {
bool
sameBits(float x,
         float y)
{
    std::uint32_t bx, by;
    std::memcpy(&bx, &x, sizeof(bx));
    std::memcpy(&by, &y, sizeof(by));

    return bx == by;
}

// A row side as the RowSide contract has it: zero wherever the side lacks the pixel.
void
makeRowSide(int width,
            int nComps,
            int seed,
            int absentEvery,
            std::vector<float>* pixels,
            std::vector<unsigned char>* present)
{
    pixels->assign((std::size_t)width * nComps, 0.f);
    present->assign(width, (unsigned char)1);
    for (int x = 0; x < width; ++x) {
        if ((x % absentEvery) == seed % absentEvery) {
            (*present)[x] = 0;
            continue;
        }
        for (int c = 0; c < nComps; ++c) {
            const bool isAlpha = (nComps == 1) || (c == 3);
            const int k = x * 5 + c * 3 + seed;
            (*pixels)[(std::size_t)x * nComps + c] = isAlpha ? kAlphas[k % 4] : kValues[k % 6];
        }
    }
}

float
rowAlpha(const float* pix,
         int nComps,
         bool present,
         float opaqueAlpha)
{
    if (nComps == 4) {
        return pix[3];
    } else if (nComps == 1) {
        return pix[0];
    }

    return present ? opaqueAlpha : 0.f;
}
} // namespace

TEST(MergeOperators, RowFunctionsMatchMergePixelBitForBit)
{
    const int width = 37;
    const float kSentinel = 7.f;

    for (int i = 0; i < eOperationCount; ++i) {
        const Operation op = (Operation)i;
        for (int nComps = 1; nComps <= 4; ++nComps) {
            const RowFunction mergeRow = mergeRowFunction(op, nComps);
            const RowFunction overRow = mergeOverRowFunction(op, nComps);
            ASSERT_TRUE(mergeRow != 0) << operationId(op) << " " << nComps;
            ASSERT_TRUE(overRow != 0) << operationId(op) << " " << nComps;
            for (int masking = 0; masking < 2; ++masking) {
                for (int opaque = 0; opaque < 2; ++opaque) {
                    const float aOpaque = opaque ? 1.f : 0.f;
                    const float bOpaque = opaque ? 0.f : 1.f;
                    std::vector<float> A, B;
                    std::vector<unsigned char> aPresent, bPresent;
                    makeRowSide(width, nComps, 1, 3, &A, &aPresent);
                    makeRowSide(width, nComps, 2, 4, &B, &bPresent);

                    // A over B; once with the presence flags and once with every pixel present.
                    for (int allPresent = 0; allPresent < 2; ++allPresent) {
                        std::vector<unsigned char> aHas = allPresent ? std::vector<unsigned char>(width, 1) : aPresent;
                        std::vector<unsigned char> bHas = allPresent ? std::vector<unsigned char>(width, 1) : bPresent;
                        const RowSide aSide = { &A[0], allPresent ? 0 : &aHas[0], aOpaque };
                        const RowSide bSide = { &B[0], allPresent ? 0 : &bHas[0], bOpaque };
                        std::vector<float> out(A.size(), kSentinel);
                        mergeRow(masking != 0, aSide, bSide, width, &out[0]);
                        for (int x = 0; x < width; ++x) {
                            const std::size_t p = (std::size_t)x * nComps;
                            float expected[4] = { 0.f, 0.f, 0.f, 0.f };
                            if (aHas[x] || bHas[x]) {
                                const float a = rowAlpha(&A[p], nComps, aHas[x] != 0, aOpaque);
                                const float b = rowAlpha(&B[p], nComps, bHas[x] != 0, bOpaque);
                                mergePixel(op, masking != 0, &A[p], a, &B[p], b, nComps, expected);
                            }
                            for (int c = 0; c < nComps; ++c) {
                                EXPECT_TRUE(sameBits(expected[c], out[p + c]))
                                    << operationId(op) << " nComps " << nComps << " masking " << masking << " x " << x << " c " << c
                                    << ": " << out[p + c] << " vs " << expected[c];
                            }
                        }
                    }

                    // A over a running result, in place.
                    std::vector<float> running(B);
                    std::vector<float> expected(B);
                    const RowSide aSide = { &A[0], &aPresent[0], aOpaque };
                    const RowSide runningSide = { &running[0], 0, 1.f };
                    overRow(masking != 0, aSide, runningSide, width, &running[0]);
                    for (int x = 0; x < width; ++x) {
                        if (!aPresent[x]) {
                            continue;
                        }
                        const std::size_t p = (std::size_t)x * nComps;
                        const float a = rowAlpha(&A[p], nComps, true, aOpaque);
                        const float b = rowAlpha(&expected[p], nComps, true, 1.f);
                        mergePixel(op, masking != 0, &A[p], a, &expected[p], b, nComps, &expected[p]);
                    }
                    for (std::size_t k = 0; k < running.size(); ++k) {
                        EXPECT_TRUE(sameBits(expected[k], running[k]))
                            << operationId(op) << " over, nComps " << nComps << " masking " << masking << " index " << k;
                    }
                }
            }
        }
    }
    EXPECT_TRUE(mergeRowFunction(eOver, 0) == 0);
    EXPECT_TRUE(mergeRowFunction(eOver, 5) == 0);
    EXPECT_TRUE(mergeOverRowFunction(eOperationCount, 4) == 0);
}
