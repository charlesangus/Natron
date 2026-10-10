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

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <random>

#include "Engine/ColorWheelMath.h"

NATRON_NAMESPACE_USING

namespace {
const double kAP1Weights[3] = { 0.2722287168, 0.6740817658, 0.0536895174 };
const double kRec709Weights[3] = { 0.2126729, 0.7151522, 0.0721750 };

// The sRGB/Rec.709 (D65) RGB-to-XYZ matrix, rows X, Y, Z over r, g, b.
const double kRec709ToXYZ[9] = {
    0.4124564, 0.3575761, 0.1804375,
    0.2126729, 0.7151522, 0.0721750,
    0.0193339, 0.1191920, 0.9503041
};

const double kAP1ToXYZ[9] = {
    0.6624541811, 0.1340042065, 0.1561876870,
    0.2722287168, 0.6740817658, 0.0536895174,
    -0.0055746495, 0.0040607335, 1.0103391003
};

KnobColorValue
makeValue(double r, double g, double b, double a, bool hasAlpha = true)
{
    KnobColorValue v;

    v.rgb[0] = r;
    v.rgb[1] = g;
    v.rgb[2] = b;
    v.a = a;
    v.hasAlpha = hasAlpha;

    return v;
}

LumaBasis
acescg()
{
    return LumaBasis(kAP1Weights, kAP1ToXYZ);
}

double
lumaTolerance(ColorKnobRoleEnum role,
              double Y)
{
    // An offset's luma can sit at or near 0, where a relative bound means nothing; its chroma
    // is computed about the 0.18 pivot, so that is the scale its rounding is relative to.
    double scale = isRatioRole(role) ? std::abs(Y) : std::max(std::abs(Y), 0.18);

    return 1e-12 * scale;
}

void
expectLumaAndAlphaKept(ColorKnobRoleEnum role,
                       const LumaBasis& basis,
                       const KnobColorValue& before,
                       const ChromaEditResult& after)
{
    double Y0 = luma(basis, before.rgb);
    double Y1 = luma(basis, after.value.rgb);

    EXPECT_NEAR(Y0, Y1, lumaTolerance(role, Y0));
    EXPECT_EQ(before.a, after.value.a);
}
} // namespace

TEST(ColorWheelMath, SplitJoinRoundTripAP1)
{
    LumaBasis basis(kAP1Weights);
    const double samples[][3] = {
        { 1.0, 1.0, 1.0 },
        { 1.5, 1.0, 1.0 },
        { 2.0, 0.5, -0.3 },
        { 8.0, 8.0, 8.0 },
        { -0.3, -0.3, -0.3 },
        { 0.0, 0.0, 0.0 }
    };

    for (const auto& rgb : samples) {
        LumaChroma lc = split(basis, rgb);
        double rebuilt[3];
        join(basis, lc.Y, lc.a, lc.b, rebuilt);
        for (int i = 0; i < 3; ++i) {
            EXPECT_NEAR(rgb[i], rebuilt[i], 1e-12);
        }
    }
}

TEST(ColorWheelMath, SplitJoinRoundTripRec709)
{
    LumaBasis basis(kRec709Weights);
    const double samples[][3] = {
        { 1.0, 1.0, 1.0 },
        { 0.2, 1.3, 0.9 },
        { 4.0, -1.0, 2.0 }
    };

    for (const auto& rgb : samples) {
        LumaChroma lc = split(basis, rgb);
        double rebuilt[3];
        join(basis, lc.Y, lc.a, lc.b, rebuilt);
        for (int i = 0; i < 3; ++i) {
            EXPECT_NEAR(rgb[i], rebuilt[i], 1e-12);
        }
    }
}

TEST(ColorWheelMath, GainOneStopDoublesValueAndAlpha)
{
    LumaBasis basis(kRec709Weights);
    KnobColorValue v = makeValue(1.0, 1.0, 1.0, 1.0);
    KnobColorValue out = scaleValue(eColorKnobRoleGain, basis, v, 2.0);

    EXPECT_NEAR(2.0, out.rgb[0], 1e-12);
    EXPECT_NEAR(2.0, out.rgb[1], 1e-12);
    EXPECT_NEAR(2.0, out.rgb[2], 1e-12);
    EXPECT_NEAR(2.0, out.a, 1e-12);
}

TEST(ColorWheelMath, OffsetAddOnBlackGivesFlatLift)
{
    LumaBasis basis(kRec709Weights);
    KnobColorValue v = makeValue(0.0, 0.0, 0.0, 0.0);
    KnobColorValue out = addValue(eColorKnobRoleOffset, basis, v, 0.1);

    EXPECT_NEAR(0.1, out.rgb[0], 1e-12);
    EXPECT_NEAR(0.1, out.rgb[1], 1e-12);
    EXPECT_NEAR(0.1, out.rgb[2], 1e-12);
    EXPECT_NEAR(0.1, out.a, 1e-12);
}

TEST(ColorWheelMath, ValueEditKeepsChromaOverYForGain)
{
    LumaBasis basis(kAP1Weights);
    KnobColorValue v = makeValue(1.5, 1.0, 1.0, 1.0);
    LumaChroma before = split(basis, v.rgb);
    KnobColorValue out = scaleValue(eColorKnobRoleGain, basis, v, 3.0);
    LumaChroma after = split(basis, out.rgb);

    EXPECT_NEAR(before.a / before.Y, after.a / after.Y, 1e-12);
    EXPECT_NEAR(before.b / before.Y, after.b / after.Y, 1e-12);
}

TEST(ColorWheelMath, ValueEditKeepsChromaAbsoluteForOffset)
{
    LumaBasis basis(kAP1Weights);
    KnobColorValue v = makeValue(0.2, -0.1, 0.05, 0.0);
    LumaChroma before = split(basis, v.rgb);
    KnobColorValue out = addValue(eColorKnobRoleOffset, basis, v, 0.3);
    LumaChroma after = split(basis, out.rgb);

    EXPECT_NEAR(before.a, after.a, 1e-12);
    EXPECT_NEAR(before.b, after.b, 1e-12);
}

TEST(ColorWheelMath, ThreeDimKnobHasNoAlpha)
{
    LumaBasis basis(kRec709Weights);
    KnobColorValue v = makeValue(1.0, 1.0, 1.0, 123.0, false);
    KnobColorValue out = scaleValue(eColorKnobRoleGain, basis, v, 2.0);

    EXPECT_FALSE(out.hasAlpha);
    EXPECT_EQ(123.0, out.a);
}

TEST(ColorWheelMath, YAtOrBelowZeroFreezesARatioRole)
{
    LumaBasis basis(kRec709Weights);
    KnobColorValue v = makeValue(0.0, 0.0, 0.0, 1.0);
    double Y0 = luma(basis, v.rgb);

    EXPECT_TRUE(isFrozen(eColorKnobRoleGain, Y0));
    EXPECT_FALSE(isFrozen(eColorKnobRoleOffset, Y0));
}

TEST(ColorWheelMath, ScaleValueOnFrozenGainIsANoOp)
{
    LumaBasis basis(kRec709Weights);
    KnobColorValue v = makeValue(0.0, 0.0, 0.0, 1.0);
    KnobColorValue out = scaleValue(eColorKnobRoleGain, basis, v, 8.0);

    EXPECT_NEAR(0.0, out.rgb[0], 1e-12);
    EXPECT_NEAR(0.0, out.rgb[1], 1e-12);
    EXPECT_NEAR(0.0, out.rgb[2], 1e-12);
    EXPECT_NEAR(1.0, out.a, 1e-12);
}

TEST(ColorWheelMath, AdditiveValueEditEscapesAFrozenGain)
{
    LumaBasis basis(kRec709Weights);
    KnobColorValue v = makeValue(0.0, 0.0, 0.0, 1.0);
    KnobColorValue out = addValue(eColorKnobRoleGain, basis, v, 0.5);
    double Y1 = luma(basis, out.rgb);

    EXPECT_NEAR(0.5, out.rgb[0], 1e-12);
    EXPECT_NEAR(0.5, out.rgb[1], 1e-12);
    EXPECT_NEAR(0.5, out.rgb[2], 1e-12);
    EXPECT_NEAR(1.5, out.a, 1e-12) << "while frozen, alpha adds the Y delta rather than scaling by it";
    EXPECT_FALSE(isFrozen(eColorKnobRoleGain, Y1));
}

TEST(ColorWheelMath, HdrGainSurvivesUnclamped)
{
    LumaBasis basis(kAP1Weights);
    KnobColorValue v = makeValue(1.0, 1.0, 1.0, 1.0);
    KnobColorValue out = scaleValue(eColorKnobRoleGain, basis, v, 8.0);

    EXPECT_NEAR(8.0, out.rgb[0], 1e-12);
    EXPECT_NEAR(8.0, out.rgb[1], 1e-12);
    EXPECT_NEAR(8.0, out.rgb[2], 1e-12);
    EXPECT_NEAR(8.0, out.a, 1e-12);
}

TEST(ColorWheelMath, NegativeOffsetSurvivesUnclamped)
{
    LumaBasis basis(kAP1Weights);
    KnobColorValue v = makeValue(0.0, 0.0, 0.0, 0.0);
    KnobColorValue out = addValue(eColorKnobRoleOffset, basis, v, -0.3);

    EXPECT_NEAR(-0.3, out.rgb[0], 1e-12);
    EXPECT_NEAR(-0.3, out.rgb[1], 1e-12);
    EXPECT_NEAR(-0.3, out.rgb[2], 1e-12);
    EXPECT_NEAR(-0.3, out.a, 1e-12);
}

TEST(ColorWheelMath, ResetValueTargetsOneOrZeroAndColourIsANoOp)
{
    EXPECT_TRUE(resetValue(eColorKnobRoleGain).has_value());
    EXPECT_NEAR(1.0, *resetValue(eColorKnobRoleGain), 1e-12);
    EXPECT_TRUE(resetValue(eColorKnobRolePower).has_value());
    EXPECT_NEAR(1.0, *resetValue(eColorKnobRolePower), 1e-12);
    EXPECT_TRUE(resetValue(eColorKnobRoleOffset).has_value());
    EXPECT_NEAR(0.0, *resetValue(eColorKnobRoleOffset), 1e-12);
    EXPECT_FALSE(resetValue(eColorKnobRoleColour).has_value());
    EXPECT_FALSE(resetValue(eColorKnobRoleAuto).has_value());
}

TEST(ColorWheelMath, IsRatioRoleIsFalseOnlyForOffset)
{
    EXPECT_TRUE(isRatioRole(eColorKnobRoleGain));
    EXPECT_TRUE(isRatioRole(eColorKnobRoleColour));
    EXPECT_TRUE(isRatioRole(eColorKnobRolePower));
    EXPECT_TRUE(isRatioRole(eColorKnobRoleAuto));
    EXPECT_FALSE(isRatioRole(eColorKnobRoleOffset));
}

TEST(ColorWheelMath, LumaBasisRenormalisesWeightsToSumOne)
{
    const double unnormalised[3] = { 2.0, 2.0, 2.0 };
    LumaBasis basis(unnormalised);

    EXPECT_NEAR(1.0 / 3.0, basis.w[0], 1e-15);
    EXPECT_NEAR(1.0 / 3.0, basis.w[1], 1e-15);
    EXPECT_NEAR(1.0 / 3.0, basis.w[2], 1e-15);
}

TEST(ColorWheelMath, MatrixLessLumaBasisHasNoMatrix)
{
    LumaBasis basis(kAP1Weights);

    EXPECT_FALSE(basis.hasMatrix);
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            EXPECT_EQ(0.0, basis.Mprime[i][j]);
        }
    }
}

TEST(ColorWheelMath, MatrixLumaBasisScalesYRowToTheWeights)
{
    LumaBasis basis(kRec709Weights, kRec709ToXYZ);

    EXPECT_TRUE(basis.hasMatrix);
    EXPECT_NEAR(basis.w[0], basis.Mprime[1][0], 1e-9);
    EXPECT_NEAR(basis.w[1], basis.Mprime[1][1], 1e-9);
    EXPECT_NEAR(basis.w[2], basis.Mprime[1][2], 1e-9);
}

TEST(ColorWheelMath, NeutralMapsToTheOrigin)
{
    LumaBasis basis = acescg();
    const ColorKnobRoleEnum roles[] = { eColorKnobRoleGain, eColorKnobRoleColour, eColorKnobRolePower };

    for (ColorKnobRoleEnum role : roles) {
        PlanePoint q = toPlane(role, basis, makeValue(2.0, 2.0, 2.0, 1.0));
        EXPECT_TRUE(q.valid);
        EXPECT_FALSE(q.linear);
        EXPECT_NEAR(0.0, q.x, 1e-12);
        EXPECT_NEAR(0.0, q.y, 1e-12);
    }

    const double offsets[] = { 0.0, 0.05, -0.1 };

    for (double o : offsets) {
        PlanePoint q = toPlane(eColorKnobRoleOffset, basis, makeValue(o, o, o, 0.0));
        EXPECT_TRUE(q.valid);
        EXPECT_FALSE(q.linear);
        EXPECT_NEAR(0.0, q.x, 1e-12);
        EXPECT_NEAR(0.0, q.y, 1e-12);
    }
}

TEST(ColorWheelMath, ToPlaneAndFromPlaneRoundTrip)
{
    LumaBasis basis = acescg();
    std::mt19937 rng(44);
    std::uniform_real_distribution<double> gain(0.2, 4.0);
    std::uniform_real_distribution<double> offset(-0.1, 0.1);
    std::uniform_real_distribution<double> step(-0.02, 0.02);

    for (int n = 0; n < 200; ++n) {
        KnobColorValue g = makeValue(gain(rng), gain(rng), gain(rng), 1.0);
        KnobColorValue o = makeValue(offset(rng), offset(rng), offset(rng), 0.0);
        const std::pair<ColorKnobRoleEnum, KnobColorValue> cases[] = {
            { eColorKnobRoleGain, g },
            { eColorKnobRoleOffset, o }
        };

        for (const auto& c : cases) {
            ColorKnobRoleEnum role = c.first;
            const KnobColorValue& v = c.second;
            PlanePoint q = toPlane(role, basis, v);
            ASSERT_TRUE(q.valid);

            ChromaEditResult back = fromPlane(role, basis, v, q, luma(basis, v.rgb));
            ASSERT_TRUE(back.valid);
            for (int i = 0; i < 3; ++i) {
                EXPECT_NEAR(v.rgb[i], back.value.rgb[i], 1e-10);
            }

            PlanePoint moved = q;
            moved.x += step(rng);
            moved.y += step(rng);
            ChromaEditResult rebuilt = fromPlane(role, basis, v, moved, luma(basis, v.rgb));
            ASSERT_TRUE(rebuilt.valid);
            PlanePoint again = toPlane(role, basis, rebuilt.value);
            EXPECT_NEAR(moved.x, again.x, 1e-10);
            EXPECT_NEAR(moved.y, again.y, 1e-10);
        }
    }
}

TEST(ColorWheelMath, ChromaEditsKeepLumaExactOnACEScg)
{
    LumaBasis basis = acescg();
    std::mt19937 rng(1944);
    std::uniform_real_distribution<double> gain(0.2, 4.0);
    std::uniform_real_distribution<double> offset(-0.1, 0.1);
    std::uniform_real_distribution<double> step(-0.05, 0.05);
    std::uniform_real_distribution<double> angle(0.0, 360.0);
    std::uniform_real_distribution<double> sat(0.0, 0.1);
    int edits = 0;
    int valid = 0;

    for (int n = 0; n < 1000; ++n) {
        KnobColorValue g = makeValue(gain(rng), gain(rng), gain(rng), gain(rng));
        KnobColorValue o = makeValue(offset(rng), offset(rng), offset(rng), offset(rng));
        const std::pair<ColorKnobRoleEnum, KnobColorValue> cases[] = {
            { eColorKnobRoleGain, g },
            { eColorKnobRoleOffset, o }
        };

        for (const auto& c : cases) {
            ColorKnobRoleEnum role = c.first;
            const KnobColorValue& v = c.second;
            double hue = angle(rng);
            const ChromaEditResult results[] = {
                translate(role, basis, v, step(rng), step(rng)),
                rotateHueAboutWhite(role, basis, v, angle(rng)),
                setHue(role, basis, v, angle(rng)),
                setSaturation(role, basis, v, sat(rng), hue),
                moveSaturation(role, basis, v, step(rng), hue)
            };

            for (const ChromaEditResult& r : results) {
                ++edits;
                if (r.valid) {
                    ++valid;
                }
                expectLumaAndAlphaKept(role, basis, v, r);
            }
        }
    }
    EXPECT_GT(valid, edits * 9 / 10);
}

TEST(ColorWheelMath, ChromaEditsKeepLumaExactOnHdrAndNegativeValues)
{
    LumaBasis basis = acescg();
    const std::pair<ColorKnobRoleEnum, KnobColorValue> cases[] = {
        { eColorKnobRoleGain, makeValue(8.0, 6.0, 10.0, 8.0) },
        { eColorKnobRoleGain, makeValue(40.0, 38.0, 41.0, 1.0) },
        { eColorKnobRoleColour, makeValue(1.2, -0.05, 0.4, 1.0) },
        { eColorKnobRoleOffset, makeValue(-0.3, -0.3, -0.3, -0.3) },
        { eColorKnobRoleOffset, makeValue(-0.3, -0.25, -0.35, 0.0) },
        { eColorKnobRoleOffset, makeValue(2.0, 1.5, 2.5, 0.0) }
    };

    for (const auto& c : cases) {
        ColorKnobRoleEnum role = c.first;
        const KnobColorValue& v = c.second;
        const ChromaEditResult results[] = {
            translate(role, basis, v, 0.01, -0.02),
            rotateHueAboutWhite(role, basis, v, 77.0),
            setHue(role, basis, v, 200.0),
            setSaturation(role, basis, v, 0.03, 45.0),
            moveSaturation(role, basis, v, 0.01, 45.0)
        };

        for (const ChromaEditResult& r : results) {
            EXPECT_TRUE(r.valid);
            expectLumaAndAlphaKept(role, basis, v, r);
        }
    }
}

TEST(ColorWheelMath, AP1GamutTriangleHasChromaticityDiagramSides)
{
    LumaBasis basis = acescg();
    GamutTriangle triangle = gamutTriangle(basis);

    ASSERT_TRUE(triangle.valid);

    auto side = [&](int i, int j) {
        return std::hypot(triangle.vertex[i].x - triangle.vertex[j].x, triangle.vertex[i].y - triangle.vertex[j].y);
    };

    EXPECT_NEAR(0.767, side(0, 1), 1e-3);
    EXPECT_NEAR(0.787, side(1, 2), 1e-3);
    EXPECT_NEAR(0.636, side(2, 0), 1e-3);

    EXPECT_NEAR(0.71300 - 0.32168, triangle.vertex[0].x, 1e-4);
    EXPECT_NEAR(0.29300 - 0.33767, triangle.vertex[0].y, 1e-4);
}

TEST(ColorWheelMath, AP1GamutTriangleEdgesHaveOneChannelAtZero)
{
    LumaBasis basis = acescg();
    GamutTriangle triangle = gamutTriangle(basis);
    KnobColorValue unit = makeValue(1.0, 1.0, 1.0, 1.0);

    ASSERT_TRUE(triangle.valid);
    for (int i = 0; i < 3; ++i) {
        int j = (i + 1) % 3;
        int k = (i + 2) % 3;
        for (int n = 1; n < 10; ++n) {
            double t = n / 10.0;
            PlanePoint p;
            p.x = (1.0 - t) * triangle.vertex[i].x + t * triangle.vertex[j].x;
            p.y = (1.0 - t) * triangle.vertex[i].y + t * triangle.vertex[j].y;
            p.valid = true;

            ChromaEditResult r = fromPlane(eColorKnobRoleGain, basis, unit, p, 1.0);
            ASSERT_TRUE(r.valid);
            EXPECT_NEAR(0.0, r.value.rgb[k], 1e-9);
            EXPECT_GT(r.value.rgb[i], 0.0);
            EXPECT_GT(r.value.rgb[j], 0.0);
        }
    }
}

TEST(ColorWheelMath, AP1PrimariesSitNearTheFamiliarHueAngles)
{
    GamutTriangle triangle = gamutTriangle(acescg());

    EXPECT_NEAR(353.49, planeHue(triangle.vertex[0]), 0.05);
    EXPECT_NEAR(107.65, planeHue(triangle.vertex[1]), 0.05);
    EXPECT_NEAR(236.59, planeHue(triangle.vertex[2]), 0.05);
}

TEST(ColorWheelMath, UPrimeVPrimeIsAOneConstantSwap)
{
    LumaBasis basis(kAP1Weights, kAP1ToXYZ, eChromaticityKindUPrimeVPrime);
    GamutTriangle triangle = gamutTriangle(basis);

    ASSERT_TRUE(triangle.valid);

    auto side = [&](int i, int j) {
        return std::hypot(triangle.vertex[i].x - triangle.vertex[j].x, triangle.vertex[i].y - triangle.vertex[j].y);
    };

    EXPECT_NEAR(0.513, side(0, 1), 1e-3);
    EXPECT_NEAR(0.482, side(1, 2), 1e-3);
    EXPECT_NEAR(0.566, side(2, 0), 1e-3);

    KnobColorValue v = makeValue(1.3, 0.9, 0.7, 1.0);
    ChromaEditResult r = rotateHueAboutWhite(eColorKnobRoleGain, basis, v, 90.0);
    ASSERT_TRUE(r.valid);
    expectLumaAndAlphaKept(eColorKnobRoleGain, basis, v, r);
}

TEST(ColorWheelMath, FullHueRotationIsTheIdentity)
{
    LumaBasis basis = acescg();
    const std::pair<ColorKnobRoleEnum, KnobColorValue> cases[] = {
        { eColorKnobRoleGain, makeValue(1.4, 0.8, 0.6, 1.0) },
        { eColorKnobRoleOffset, makeValue(0.03, -0.02, 0.01, 0.0) }
    };

    for (const auto& c : cases) {
        ColorKnobRoleEnum role = c.first;
        const KnobColorValue& v = c.second;
        ChromaEditResult once = rotateHueAboutWhite(role, basis, v, 360.0);
        ASSERT_TRUE(once.valid);
        for (int i = 0; i < 3; ++i) {
            EXPECT_NEAR(v.rgb[i], once.value.rgb[i], 1e-10);
        }

        KnobColorValue stepped = v;
        for (int n = 0; n < 36; ++n) {
            ChromaEditResult r = rotateHueAboutWhite(role, basis, stepped, 10.0);
            ASSERT_TRUE(r.valid);
            stepped = r.value;
        }
        for (int i = 0; i < 3; ++i) {
            EXPECT_NEAR(v.rgb[i], stepped.rgb[i], 1e-10);
        }
    }
}

TEST(ColorWheelMath, OffsetFallsBackToTheLinearPlaneBelowZeroPivotLuma)
{
    LumaBasis basis = acescg();
    KnobColorValue above = makeValue(-0.17, -0.17, -0.17, 0.0);
    KnobColorValue below = makeValue(-0.19 + 0.02, -0.19 - 0.01, -0.19, 0.0);
    double shift = -0.19 - luma(basis, below.rgb);
    for (int i = 0; i < 3; ++i) {
        below.rgb[i] += shift;
    }

    ASSERT_NEAR(-0.01, 0.18 + luma(basis, below.rgb), 1e-15);
    EXPECT_FALSE(toPlane(eColorKnobRoleOffset, basis, above).linear);

    PlanePoint q = toPlane(eColorKnobRoleOffset, basis, below);
    EXPECT_TRUE(q.valid);
    EXPECT_TRUE(q.linear);

    ChromaEditResult moved = translate(eColorKnobRoleOffset, basis, below, 0.004, -0.003);
    ASSERT_TRUE(moved.valid);
    expectLumaAndAlphaKept(eColorKnobRoleOffset, basis, below, moved);

    PlanePoint after = toPlane(eColorKnobRoleOffset, basis, moved.value);
    EXPECT_TRUE(after.linear);
    EXPECT_NEAR(q.x + 0.004, after.x, 1e-10);
    EXPECT_NEAR(q.y - 0.003, after.y, 1e-10);

    ChromaEditResult rotated = rotateHueAboutWhite(eColorKnobRoleOffset, basis, below, 123.0);
    ASSERT_TRUE(rotated.valid);
    expectLumaAndAlphaKept(eColorKnobRoleOffset, basis, below, rotated);
}

TEST(ColorWheelMath, OffsetLinearPlaneAgreesWithProjectiveNearWhite)
{
    LumaBasis basis = acescg();
    KnobColorValue v = makeValue(1e-4, -5e-5, 2e-5, 0.0);
    PlanePoint q = toPlane(eColorKnobRoleOffset, basis, v);
    double lx = (basis.J[0][0] * v.rgb[0] + basis.J[0][1] * v.rgb[1] + basis.J[0][2] * v.rgb[2]) / 0.18;
    double ly = (basis.J[1][0] * v.rgb[0] + basis.J[1][1] * v.rgb[1] + basis.J[1][2] * v.rgb[2]) / 0.18;

    ASSERT_TRUE(q.valid);
    EXPECT_FALSE(q.linear);
    EXPECT_NEAR(lx, q.x, 1e-7);
    EXPECT_NEAR(ly, q.y, 1e-7);
}

TEST(ColorWheelMath, SaturationGrowsAlongTheRememberedHueFromNeutral)
{
    LumaBasis basis = acescg();
    KnobColorValue neutral = makeValue(1.0, 1.0, 1.0, 1.0);

    ChromaEditResult set = setSaturation(eColorKnobRoleGain, basis, neutral, 0.02, 120.0);
    ASSERT_TRUE(set.valid);
    PlanePoint q = toPlane(eColorKnobRoleGain, basis, set.value);
    EXPECT_NEAR(120.0, planeHue(q), 1e-6);
    EXPECT_NEAR(0.02, planeSaturation(q), 1e-10);

    ChromaEditResult moved = moveSaturation(eColorKnobRoleGain, basis, neutral, 0.01, 300.0);
    ASSERT_TRUE(moved.valid);
    q = toPlane(eColorKnobRoleGain, basis, moved.value);
    EXPECT_NEAR(300.0, planeHue(q), 1e-6);
    EXPECT_NEAR(0.01, planeSaturation(q), 1e-10);

    ChromaEditResult keepsOwnHue = setSaturation(eColorKnobRoleGain, basis, set.value, 0.05, 300.0);
    ASSERT_TRUE(keepsOwnHue.valid);
    EXPECT_NEAR(120.0, planeHue(toPlane(eColorKnobRoleGain, basis, keepsOwnHue.value)), 1e-6);

    ChromaEditResult pastWhite = moveSaturation(eColorKnobRoleGain, basis, set.value, -0.5, 0.0);
    ASSERT_TRUE(pastWhite.valid);
    EXPECT_NEAR(0.0, planeSaturation(toPlane(eColorKnobRoleGain, basis, pastWhite.value)), 1e-12);
}

TEST(ColorWheelMath, SetHueKeepsSaturationAndIsANoOpOnNeutral)
{
    LumaBasis basis = acescg();
    KnobColorValue v = makeValue(1.3, 0.9, 0.7, 1.0);
    double s0 = planeSaturation(toPlane(eColorKnobRoleGain, basis, v));
    ChromaEditResult r = setHue(eColorKnobRoleGain, basis, v, 200.0);

    ASSERT_TRUE(r.valid);
    PlanePoint q = toPlane(eColorKnobRoleGain, basis, r.value);
    EXPECT_NEAR(200.0, planeHue(q), 1e-6);
    EXPECT_NEAR(s0, planeSaturation(q), 1e-10);

    KnobColorValue neutral = makeValue(0.5, 0.5, 0.5, 1.0);
    ChromaEditResult same = setHue(eColorKnobRoleGain, basis, neutral, 200.0);
    ASSERT_TRUE(same.valid);
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(neutral.rgb[i], same.value.rgb[i]);
    }
}

TEST(ColorWheelMath, FrozenRatioRoleRefusesChromaEdits)
{
    LumaBasis basis = acescg();
    KnobColorValue black = makeValue(0.0, 0.0, 0.0, 1.0);

    EXPECT_FALSE(toPlane(eColorKnobRoleGain, basis, black).valid);

    ChromaEditResult r = translate(eColorKnobRoleGain, basis, black, 0.01, 0.0);
    EXPECT_FALSE(r.valid);
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(0.0, r.value.rgb[i]);
    }
    EXPECT_FALSE(setSaturation(eColorKnobRoleGain, basis, black, 0.02, 0.0).valid);
}

TEST(ColorWheelMath, MatrixlessBasisUsesALinearLumaExactPlane)
{
    LumaBasis basis(kRec709Weights);
    KnobColorValue v = makeValue(1.2, 0.9, 1.1, 1.0);
    PlanePoint q = toPlane(eColorKnobRoleGain, basis, v);

    EXPECT_FALSE(gamutTriangle(basis).valid);
    EXPECT_TRUE(q.valid);
    EXPECT_TRUE(q.linear);

    ChromaEditResult back = fromPlane(eColorKnobRoleGain, basis, v, q, luma(basis, v.rgb));
    ASSERT_TRUE(back.valid);
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(v.rgb[i], back.value.rgb[i], 1e-12);
    }

    ChromaEditResult r = rotateHueAboutWhite(eColorKnobRoleGain, basis, v, 45.0);
    ASSERT_TRUE(r.valid);
    expectLumaAndAlphaKept(eColorKnobRoleGain, basis, v, r);

    KnobColorValue neutral = makeValue(0.7, 0.7, 0.7, 1.0);
    PlanePoint n = toPlane(eColorKnobRoleGain, basis, neutral);
    EXPECT_NEAR(0.0, n.x, 1e-15);
    EXPECT_NEAR(0.0, n.y, 1e-15);
}

TEST(ColorWheelMath, Rec709WhiteIsD65)
{
    LumaBasis basis(kRec709Weights, kRec709ToXYZ);

    EXPECT_NEAR(0.3127, basis.white[0], 1e-4);
    EXPECT_NEAR(0.3290, basis.white[1], 1e-4);
}

TEST(ColorWheelMath, WhitePointIsTheNegatedAnchor)
{
    LumaBasis basis = acescg();
    PlanePoint anchor = toPlane(eColorKnobRoleGain, basis, makeValue(2.0, 1.0, 1.0, 1.0));
    PlanePoint white = whitePoint(anchor);

    EXPECT_EQ(-anchor.x, white.x);
    EXPECT_EQ(-anchor.y, white.y);
}

TEST(ColorWheelMath, ClampToDiscReportsTheDirectionToAnOffDiscPoint)
{
    PlanePoint far;
    far.x = 0.3;
    far.y = -0.4;
    far.valid = true;

    DiscClamp off = clampToDisc(far, 0.1);
    EXPECT_TRUE(off.offDisc);
    EXPECT_NEAR(0.6, off.direction.x, 1e-15);
    EXPECT_NEAR(-0.8, off.direction.y, 1e-15);
    EXPECT_NEAR(0.06, off.point.x, 1e-15);
    EXPECT_NEAR(-0.08, off.point.y, 1e-15);

    PlanePoint near;
    near.x = 0.01;
    near.y = 0.02;
    near.valid = true;

    DiscClamp on = clampToDisc(near, 0.1);
    EXPECT_FALSE(on.offDisc);
    EXPECT_EQ(0.01, on.point.x);
    EXPECT_EQ(0.02, on.point.y);

    LumaBasis basis = acescg();
    PlanePoint anchor = toPlane(eColorKnobRoleGain, basis, makeValue(2.0, 1.0, 1.0, 1.0));
    PlanePoint white = whitePoint(anchor);
    DiscClamp caret = clampToDisc(white, 0.001);
    EXPECT_TRUE(caret.offDisc);
    EXPECT_NEAR(std::fmod(planeHue(anchor) + 180.0, 360.0), planeHue(caret.direction), 1e-9);
}
