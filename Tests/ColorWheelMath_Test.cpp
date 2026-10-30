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
dot3row(const double row[3],
        const double rgb[3])
{
    return row[0] * rgb[0] + row[1] * rgb[1] + row[2] * rgb[2];
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
    // Power's value edits run on 0.18^(1/gamma), so the target that lands on gamma 1 (at zero
    // chroma) is 0.18 itself, not 1.
    EXPECT_NEAR(0.18, *resetValue(eColorKnobRolePower), 1e-12);
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

TEST(ColorWheelMath, NeutralTempTintReadsZero)
{
    LumaBasis basis = acescg();
    KnobColorValue white = makeValue(1.0, 1.0, 1.0, 1.0);
    TempTintResult r = toTempTint(eColorKnobRoleGain, basis, white);

    ASSERT_TRUE(r.valid);
    EXPECT_NEAR(0.0, r.temp, 1e-6);
    EXPECT_NEAR(0.0, r.tint, 1e-6);

    KnobColorValue blackOffset = makeValue(0.0, 0.0, 0.0, 0.0, false);
    TempTintResult offsetNeutral = toTempTint(eColorKnobRoleOffset, basis, blackOffset);

    ASSERT_TRUE(offsetNeutral.valid);
    EXPECT_NEAR(0.0, offsetNeutral.temp, 1e-12);
    EXPECT_NEAR(0.0, offsetNeutral.tint, 1e-12);
}

TEST(ColorWheelMath, KimLocusMatchesKnownTemperatures)
{
    LumaBasis basis = acescg();
    KnobColorValue white = makeValue(1.0, 1.0, 1.0, 1.0);

    // temp/tint for each CCT, computed the same way this module does (ACEScg's own white sits
    // at mired 166.77 / Duv 0.003193, about 5996 K); x, y are the Kim cubic's own output at
    // that CCT, checked bit-for-bit to confirm the formula's coefficients were transcribed
    // correctly.
    struct Case {
        double temp;
        double tint;
        double x;
        double y;
    };
    const Case cases[] = {
        { 183.3696233683991, 3.1928851027017644, 0.4470706750966438, 0.40750879834778053 }, // 2856 K
        { 33.10963930281605, 3.1928851027017644, 0.3449074537491241, 0.35151935324713224 }, // 5003 K
        { -13.01889513863469, 3.1928851027017644, 0.313432036002229, 0.323601871509382 } // 6504 K
    };
    // The true blackbody locus (CIE 1931 CMF integration via Ohno 2013) at the same three CCTs:
    // the Kim cubic's own fit error from it grows toward the low-CCT end of its domain, so this
    // check is looser than the formula's typical accuracy away from 1667 K.
    const double trueXY[][2] = {
        { 0.44753661, 0.40743031 },
        { 0.34501844, 0.35154254 },
        { 0.31346459, 0.32356923 }
    };

    for (int i = 0; i < 3; ++i) {
        ChromaEditResult set = fromTempTint(eColorKnobRoleGain, basis, white, cases[i].temp, cases[i].tint);
        ASSERT_TRUE(set.valid);
        EXPECT_NEAR(1.0, luma(basis, set.value.rgb), 1e-12);

        double X = dot3row(basis.Mprime[0], set.value.rgb);
        double Y = dot3row(basis.Mprime[1], set.value.rgb);
        double Z = dot3row(basis.Mprime[2], set.value.rgb);
        double s = X + Y + Z;
        double x = X / s;
        double y = Y / s;

        EXPECT_NEAR(cases[i].x, x, 1e-6);
        EXPECT_NEAR(cases[i].y, y, 1e-6);
        EXPECT_NEAR(trueXY[i][0], x, 1e-3);
        EXPECT_NEAR(trueXY[i][1], y, 1e-3);

        TempTintResult back = toTempTint(eColorKnobRoleGain, basis, set.value);
        ASSERT_TRUE(back.valid);
        EXPECT_NEAR(cases[i].temp, back.temp, 1e-6);
        EXPECT_NEAR(cases[i].tint, back.tint, 1e-6);
    }
}

TEST(ColorWheelMath, TempTintEditsKeepLumaExactAcrossRandomSamples)
{
    LumaBasis basis = acescg();
    std::mt19937 rng(443);
    std::uniform_real_distribution<double> gain(0.2, 4.0);
    std::uniform_real_distribution<double> offset(-0.1, 0.1);
    std::uniform_real_distribution<double> tempStep(-80.0, 80.0);
    std::uniform_real_distribution<double> tintStep(-30.0, 30.0);
    int edits = 0;
    int valid = 0;

    for (int n = 0; n < 500; ++n) {
        KnobColorValue g = makeValue(gain(rng), gain(rng), gain(rng), gain(rng));
        KnobColorValue o = makeValue(offset(rng), offset(rng), offset(rng), offset(rng));
        double temp = tempStep(rng);
        double tint = tintStep(rng);
        const std::pair<ColorKnobRoleEnum, KnobColorValue> cases[] = {
            { eColorKnobRoleGain, g },
            { eColorKnobRoleOffset, o }
        };

        for (const auto& c : cases) {
            ++edits;
            ChromaEditResult r = fromTempTint(c.first, basis, c.second, temp * (c.first == eColorKnobRoleOffset ? 0.001 : 1.0),
                                              tint * (c.first == eColorKnobRoleOffset ? 0.001 : 1.0));
            if (r.valid) {
                ++valid;
                expectLumaAndAlphaKept(c.first, basis, c.second, r);
            }
        }
    }
    EXPECT_GT(valid, edits * 8 / 10);
}

TEST(ColorWheelMath, TempTintRoundTripsThroughItsInverse)
{
    LumaBasis basis = acescg();
    KnobColorValue v = makeValue(1.3, 0.8, 1.1, 1.0);

    TempTintResult read = toTempTint(eColorKnobRoleGain, basis, v);
    ASSERT_TRUE(read.valid);

    ChromaEditResult rebuilt = fromTempTint(eColorKnobRoleGain, basis, v, read.temp, read.tint);
    ASSERT_TRUE(rebuilt.valid);
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(v.rgb[i], rebuilt.value.rgb[i], 1e-9);
    }

    ChromaEditResult set = fromTempTint(eColorKnobRoleGain, basis, v, 76.0, 37.8);
    ASSERT_TRUE(set.valid);
    TempTintResult setThenRead = toTempTint(eColorKnobRoleGain, basis, set.value);
    ASSERT_TRUE(setThenRead.valid);
    EXPECT_NEAR(76.0, setThenRead.temp, 1e-6);
    EXPECT_NEAR(37.8, setThenRead.tint, 1e-6);

    KnobColorValue o = makeValue(0.02, -0.01, 0.005, 0.0, false);
    TempTintResult offsetRead = toTempTint(eColorKnobRoleOffset, basis, o);
    ASSERT_TRUE(offsetRead.valid);
    ChromaEditResult offsetRebuilt = fromTempTint(eColorKnobRoleOffset, basis, o, offsetRead.temp, offsetRead.tint);
    ASSERT_TRUE(offsetRebuilt.valid);
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(o.rgb[i], offsetRebuilt.value.rgb[i], 1e-9);
    }
}

TEST(ColorWheelMath, TempTintRoundTripIsLooseOnlyRightAtAKimFormulaBreakpoint)
{
    // The Kim cubic is only C0 at its T = 4000 K / T = 2222 K piecewise joins (mired 250 and
    // 450.045): a target landing within about a mired of one of them gives the finite-difference
    // Newton step a one-sided kink to cross, so it stalls short of its usual tight convergence.
    // That is a property of the locus formula itself, so it is bounded here instead of asserted
    // away.
    LumaBasis basis = acescg();
    KnobColorValue v = makeValue(2.869346, 2.944037, 0.695136, 1.0);
    TempTintResult read = toTempTint(eColorKnobRoleGain, basis, v);

    ASSERT_TRUE(read.valid);

    ChromaEditResult rebuilt = fromTempTint(eColorKnobRoleGain, basis, v, read.temp, read.tint);
    ASSERT_TRUE(rebuilt.valid);
    EXPECT_NEAR(luma(basis, v.rgb), luma(basis, rebuilt.value.rgb), 1e-12 * luma(basis, v.rgb));
    for (int i = 0; i < 3; ++i) {
        EXPECT_FALSE(std::isnan(rebuilt.value.rgb[i]));
        EXPECT_NEAR(v.rgb[i], rebuilt.value.rgb[i], 5e-6);
    }
}

TEST(ColorWheelMath, OffsetTempTintAxesAreZeroLumaUnitDirections)
{
    LumaBasis basis = acescg();
    TempTintAxes axes = linearTempTintAxes(basis);

    ASSERT_TRUE(axes.valid);

    double rgbTemp[3];
    double rgbTint[3];
    join(basis, 0.0, axes.temp[0], axes.temp[1], rgbTemp);
    join(basis, 0.0, axes.tint[0], axes.tint[1], rgbTint);
    EXPECT_NEAR(0.0, luma(basis, rgbTemp), 1e-15);
    EXPECT_NEAR(0.0, luma(basis, rgbTint), 1e-15);
    EXPECT_NEAR(1.0, std::hypot(axes.temp[0], axes.temp[1]), 1e-9);
    EXPECT_NEAR(1.0, std::hypot(axes.tint[0], axes.tint[1]), 1e-9);

    LumaBasis noMatrix(kRec709Weights);
    EXPECT_FALSE(linearTempTintAxes(noMatrix).valid);
}

TEST(ColorWheelMath, OutOfDomainTempTintNeverProducesNaN)
{
    LumaBasis basis = acescg();
    KnobColorValue white = makeValue(1.0, 1.0, 1.0, 1.0);

    KnobColorValue negative = makeValue(-1.0, 0.5, 0.3, 1.0);
    TempTintResult fromNegative = toTempTint(eColorKnobRoleGain, basis, negative);
    EXPECT_FALSE(fromNegative.valid);
    EXPECT_FALSE(std::isnan(fromNegative.temp));
    EXPECT_FALSE(std::isnan(fromNegative.tint));

    ChromaEditResult offLocus = fromTempTint(eColorKnobRoleGain, basis, white, 100000.0, 0.0);
    EXPECT_FALSE(offLocus.valid);
    for (int i = 0; i < 3; ++i) {
        EXPECT_FALSE(std::isnan(offLocus.value.rgb[i]));
    }

    ChromaEditResult hugeTint = fromTempTint(eColorKnobRoleGain, basis, white, 0.0, 1.0e6);
    EXPECT_FALSE(hugeTint.valid);
    for (int i = 0; i < 3; ++i) {
        EXPECT_FALSE(std::isnan(hugeTint.value.rgb[i]));
    }

    KnobColorValue black = makeValue(0.0, 0.0, 0.0, 1.0);
    EXPECT_FALSE(toTempTint(eColorKnobRoleGain, basis, black).valid);
    EXPECT_FALSE(fromTempTint(eColorKnobRoleGain, basis, black, 10.0, 0.0).valid);

    LumaBasis noMatrix(kRec709Weights);
    EXPECT_FALSE(toTempTint(eColorKnobRoleGain, noMatrix, white).valid);
    EXPECT_FALSE(fromTempTint(eColorKnobRoleGain, noMatrix, white, 10.0, 0.0).valid);
}

TEST(ColorWheelMath, DrawLocusIsFiniteAndReCentresOnTheAnchor)
{
    LumaBasis basis = acescg();
    PlanePoint origin;
    origin.x = 0.0;
    origin.y = 0.0;
    origin.valid = true;

    std::vector<PlanePoint> locus = drawLocus(basis, origin);
    ASSERT_GT(static_cast<int>(locus.size()), 50);
    for (const PlanePoint& p : locus) {
        EXPECT_TRUE(p.valid);
        EXPECT_FALSE(std::isnan(p.x));
        EXPECT_FALSE(std::isnan(p.y));
    }

    PlanePoint anchor;
    anchor.x = 0.01;
    anchor.y = -0.02;
    anchor.valid = true;

    std::vector<PlanePoint> shifted = drawLocus(basis, anchor);
    ASSERT_EQ(locus.size(), shifted.size());
    EXPECT_NEAR(locus[10].x - shifted[10].x, anchor.x, 1e-9);
    EXPECT_NEAR(locus[10].y - shifted[10].y, anchor.y, 1e-9);

    LumaBasis noMatrix(kRec709Weights);
    EXPECT_TRUE(drawLocus(noMatrix, origin).empty());
}

namespace {
// The luma Power's gain model actually runs on: the effect each gamma channel has on 18% grey,
// not the gamma values themselves.
double
powerEffectLuma(const LumaBasis& basis,
                const double gammaRgb[3])
{
    double effect[3];

    for (int i = 0; i < 3; ++i) {
        effect[i] = powerToEffect(gammaRgb[i]).effect;
    }

    return luma(basis, effect);
}
} // namespace

TEST(ColorWheelMath, PowerEffectRoundTripsThroughGammaOne)
{
    PowerEffectResult toEffect = powerToEffect(1.0);

    ASSERT_TRUE(toEffect.valid);
    EXPECT_NEAR(0.18, toEffect.effect, 1e-12);

    PowerGammaResult backToGamma = effectToGamma(toEffect.effect);

    ASSERT_TRUE(backToGamma.valid);
    EXPECT_NEAR(1.0, backToGamma.gamma, 1e-12);

    PowerGammaResult direct = effectToGamma(0.18);

    ASSERT_TRUE(direct.valid);
    EXPECT_NEAR(1.0, direct.gamma, 1e-12);

    PowerEffectResult higherGamma = powerToEffect(2.4);

    ASSERT_TRUE(higherGamma.valid);
    PowerGammaResult roundTripped = effectToGamma(higherGamma.effect);

    ASSERT_TRUE(roundTripped.valid);
    EXPECT_NEAR(2.4, roundTripped.gamma, 1e-9);
}

TEST(ColorWheelMath, PowerEffectIsInvalidOutsideItsDomain)
{
    EXPECT_FALSE(powerToEffect(0.0).valid);
    EXPECT_FALSE(powerToEffect(-1.0).valid);
    EXPECT_FALSE(effectToGamma(0.0).valid);
    EXPECT_FALSE(effectToGamma(-0.1).valid);
    EXPECT_FALSE(effectToGamma(1.0).valid);
    EXPECT_FALSE(effectToGamma(1.5).valid);
}

TEST(ColorWheelMath, PowerHueSaturationAndTempTintEditsKeepMidGreyLuminanceExact)
{
    LumaBasis basis = acescg();
    KnobColorValue gammaValue = makeValue(1.0, 1.0, 1.2, 1.0);
    double Y0 = powerEffectLuma(basis, gammaValue.rgb);

    ChromaEditResult rotated = rotateHueAboutWhite(eColorKnobRolePower, basis, gammaValue, 37.0);

    ASSERT_TRUE(rotated.valid);
    EXPECT_NEAR(Y0, powerEffectLuma(basis, rotated.value.rgb), 1e-12 * Y0);
    EXPECT_NEAR(gammaValue.a, rotated.value.a, 1e-9) << "hue never touches alpha";

    ChromaEditResult saturated = moveSaturation(eColorKnobRolePower, basis, gammaValue, 0.02, 0.0);

    ASSERT_TRUE(saturated.valid);
    EXPECT_NEAR(Y0, powerEffectLuma(basis, saturated.value.rgb), 1e-12 * Y0);
    EXPECT_NEAR(gammaValue.a, saturated.value.a, 1e-9) << "saturation never touches alpha";

    ChromaEditResult warmed = fromTempTint(eColorKnobRolePower, basis, gammaValue, 20.0, -5.0);

    ASSERT_TRUE(warmed.valid);
    EXPECT_NEAR(Y0, powerEffectLuma(basis, warmed.value.rgb), 1e-12 * Y0);
    EXPECT_NEAR(gammaValue.a, warmed.value.a, 1e-9) << "temp/tint never touches alpha";
}

TEST(ColorWheelMath, PowerValueEditWrapsTheGainEditOnTheEffectVectorAlphaIncluded)
{
    LumaBasis basis = acescg();
    KnobColorValue gammaValue = makeValue(1.0, 1.0, 1.0, 1.0);

    KnobColorValue out = scaleValue(eColorKnobRolePower, basis, gammaValue, 2.0);

    // Doubling the effect's luma from 0.18 (gamma 1 on every channel, including alpha) lands
    // every channel at effect 0.36, since the Gain edit this wraps keeps a ratio role's puck in
    // place: the effect vector was achromatic, so it stays achromatic.
    double expectedGamma = effectToGamma(0.36).gamma;

    EXPECT_NEAR(expectedGamma, out.rgb[0], 1e-9);
    EXPECT_NEAR(expectedGamma, out.rgb[1], 1e-9);
    EXPECT_NEAR(expectedGamma, out.rgb[2], 1e-9);
    EXPECT_NEAR(expectedGamma, out.a, 1e-9);

    KnobColorValue noAlpha = makeValue(1.0, 1.0, 1.0, 0.0, false);
    KnobColorValue outNoAlpha = scaleValue(eColorKnobRolePower, basis, noAlpha, 2.0);

    EXPECT_FALSE(outNoAlpha.hasAlpha);
    EXPECT_EQ(0.0, outNoAlpha.a) << "a 3-dim knob has no alpha to track";
}

TEST(ColorWheelMath, PowerValueEditCapsTheGammaInsteadOfRefusing)
{
    LumaBasis basis = acescg();
    KnobColorValue gammaOne = makeValue(1.0, 1.0, 1.0, 1.0);

    // Effect 1 (Y = 1) is gamma infinity; it parks at the cap on every channel, alpha included.
    KnobColorValue atOne = setValue(eColorKnobRolePower, basis, gammaOne, 1.0);

    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(kPowerGammaMax, atOne.rgb[i], 1e-9);
    }
    EXPECT_NEAR(kPowerGammaMax, atOne.a, 1e-9);

    // Far past effect 1 the edit still parks at the cap instead of being refused.
    KnobColorValue pastOne = scaleValue(eColorKnobRolePower, basis, gammaOne, 16.0);

    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(kPowerGammaMax, pastOne.rgb[i], 1e-9);
    }

    // Down to Y = 0 parks on the bottom of the range rather than going non-positive.
    KnobColorValue atZero = setValue(eColorKnobRolePower, basis, gammaOne, 0.0);

    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(kPowerGammaMin, atZero.rgb[i], 1e-9);
    }
    EXPECT_NEAR(kPowerGammaMin, atZero.a, 1e-9);
}

TEST(ColorWheelMath, PowerValueDragNeverSnapsBackToItsStart)
{
    LumaBasis basis = acescg();
    KnobColorValue start = makeValue(1.0, 1.0, 1.0, 1.0);
    double previous = 1.0;

    // A long V drag in the stroke style: every step re-applies the total travel to the start.
    for (int step = 1; step <= 80; ++step) {
        ColorEdit edit;

        edit.axis = eColorAxisValue;
        edit.d1 = 0.05 * step;
        ChromaEditResult r = applyEdit(eColorKnobRolePower, basis, start, edit, 0.0);

        ASSERT_TRUE(r.valid);
        EXPECT_GE(r.value.rgb[0], previous - 1e-12) << "step " << step;
        EXPECT_LE(r.value.rgb[0], kPowerGammaMax + 1e-12) << "step " << step;
        previous = r.value.rgb[0];
    }
    EXPECT_NEAR(kPowerGammaMax, previous, 1e-9);
}

TEST(ColorWheelMath, PowerAbsoluteValueCountsStopsFromMidGrey)
{
    LumaBasis basis = acescg();
    KnobColorValue start = makeValue(1.7, 1.7, 1.7, 1.7);
    ColorEdit edit;

    edit.axis = eColorAxisValue;
    edit.absolute = true;
    EXPECT_NEAR(0.18, valueStopPivot(eColorKnobRolePower), 1e-12);
    EXPECT_NEAR(1.0, valueStopPivot(eColorKnobRoleGain), 1e-12);

    // 0 stops is gamma 1, the Power role's neutral.
    edit.d1 = 0.0;
    ChromaEditResult zero = applyEdit(eColorKnobRolePower, basis, start, edit, 0.0);

    ASSERT_TRUE(zero.valid);
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(1.0, zero.value.rgb[i], 1e-9);
    }
    EXPECT_NEAR(1.0, zero.value.a, 1e-9);

    // +1 and -1 stop land on effect 0.36 and 0.09, so positive stops do brighten.
    edit.d1 = 1.0;
    ChromaEditResult up = applyEdit(eColorKnobRolePower, basis, start, edit, 0.0);

    EXPECT_NEAR(effectToGamma(0.36).gamma, up.value.rgb[0], 1e-9);
    EXPECT_GT(up.value.rgb[0], 1.0);

    edit.d1 = -1.0;
    ChromaEditResult down = applyEdit(eColorKnobRolePower, basis, start, edit, 0.0);

    EXPECT_NEAR(effectToGamma(0.09).gamma, down.value.rgb[0], 1e-9);
    EXPECT_LT(down.value.rgb[0], 1.0);

    SliderRange range = sliderMapping(eColorSliderKindValue, eColorKnobRolePower, TrackballRates());

    edit.d1 = range.rangeMax;
    ChromaEditResult top = applyEdit(eColorKnobRolePower, basis, start, edit, 0.0);

    EXPECT_LT(top.value.rgb[0], kPowerGammaMax) << "the track's top end is still below the cap";
}

TEST(ColorWheelMath, GainValueEditNeverDrivesAlphaNegative)
{
    LumaBasis basis = acescg();
    KnobColorValue v = makeValue(1.0, 1.0, 1.0, 1.0);

    // -21 stops lands Y below the freeze threshold: alpha still scales, it does not shift.
    KnobColorValue deep = scaleValue(eColorKnobRoleGain, basis, v, std::pow(2.0, -21.0));

    EXPECT_GE(deep.a, 0.0);
    EXPECT_NEAR(std::pow(2.0, -21.0), deep.a, 1e-12);

    KnobColorValue toZero = setValue(eColorKnobRoleGain, basis, makeValue(2.0, 2.0, 2.0, 1.0), 0.0);

    EXPECT_GE(toZero.a, 0.0);
    EXPECT_NEAR(0.0, toZero.a, 1e-12);
}

TEST(ColorWheelMath, InferColorRoleMatchesGradesSevenNativeKnobs)
{
    // Grade's own declarations (Engine/Nodes/Color/Grade.cpp), by default value, display
    // minimum and script name, as inferColorRole would see them if Grade relied on inference.
    EXPECT_EQ(eColorKnobRoleOffset, inferColorRole({ 0.0, 0.0, 0.0, 0.0 }, -1.0, "blackPoint"));
    EXPECT_EQ(eColorKnobRoleGain, inferColorRole({ 1.0, 1.0, 1.0, 1.0 }, 0.0, "whitePoint"));
    EXPECT_EQ(eColorKnobRoleOffset, inferColorRole({ 0.0, 0.0, 0.0, 0.0 }, -1.0, "black"));
    EXPECT_EQ(eColorKnobRoleGain, inferColorRole({ 1.0, 1.0, 1.0, 1.0 }, 0.0, "white"));
    EXPECT_EQ(eColorKnobRoleGain, inferColorRole({ 1.0, 1.0, 1.0, 1.0 }, 0.0, "multiply"));
    EXPECT_EQ(eColorKnobRoleOffset, inferColorRole({ 0.0, 0.0, 0.0, 0.0 }, -1.0, "offset"));
    EXPECT_EQ(eColorKnobRolePower, inferColorRole({ 1.0, 1.0, 1.0, 1.0 }, 0.2, "gamma"));
}

TEST(ColorWheelMath, InferColorRoleConstantColourAndOfxStyleGamma)
{
    // Constant's colour knob (Engine/Nodes/Generator/Constant.cpp): all defaults 0, but its
    // display minimum is 0, not negative, so it falls through the Offset rule to Colour.
    EXPECT_EQ(eColorKnobRoleColour, inferColorRole({ 0.0, 0.0, 0.0, 0.0 }, 0.0, "color"));

    // An OFX-style single-dimension gamma parameter: default 1, name contains "gamma".
    EXPECT_EQ(eColorKnobRolePower, inferColorRole({ 1.0 }, 0.01, "gamma"));
    EXPECT_EQ(eColorKnobRolePower, inferColorRole({ 1.0, 1.0, 1.0 }, 0.01, "Gamma"));

    // A plain gain-like name does not get swept into Power.
    EXPECT_EQ(eColorKnobRoleGain, inferColorRole({ 1.0, 1.0, 1.0 }, 0.0, "exposure"));

    // An empty defaults list matches no rule.
    EXPECT_EQ(eColorKnobRoleColour, inferColorRole({}, 0.0, "anything"));
}

TEST(ColorWheelMath, GearTableMatchesTheFourModifierCombinations)
{
    EXPECT_EQ(1.0, gear(false, false));
    EXPECT_EQ(0.1, gear(true, false));
    EXPECT_EQ(4.0, gear(false, true));
    EXPECT_EQ(0.01, gear(true, true));
}

TEST(ColorWheelMath, ApplyEditDispatchesTrackballHueSatValueTempTintAndAlpha)
{
    LumaBasis basis = acescg();
    KnobColorValue v = makeValue(1.2, 1.0, 0.9, 1.0);

    ColorEdit trackballEdit;
    trackballEdit.axis = eColorAxisTrackball;
    trackballEdit.d1 = 0.01;
    trackballEdit.d2 = -0.005;
    ChromaEditResult viaDispatch = applyEdit(eColorKnobRoleGain, basis, v, trackballEdit, 0.0);
    ChromaEditResult viaDirectCall = translate(eColorKnobRoleGain, basis, v, 0.01, -0.005);
    ASSERT_TRUE(viaDispatch.valid);
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(viaDirectCall.value.rgb[i], viaDispatch.value.rgb[i]);
    }

    ColorEdit hueAbs;
    hueAbs.axis = eColorAxisHue;
    hueAbs.d1 = 90.0;
    hueAbs.absolute = true;
    ChromaEditResult hueResult = applyEdit(eColorKnobRoleGain, basis, v, hueAbs, 0.0);
    ASSERT_TRUE(hueResult.valid);
    PlanePoint hueQ = toPlane(eColorKnobRoleGain, basis, hueResult.value);
    EXPECT_NEAR(90.0, planeHue(hueQ), 1e-6);

    ColorEdit satAbs;
    satAbs.axis = eColorAxisSat;
    satAbs.d1 = 0.02;
    satAbs.absolute = true;
    ChromaEditResult satResult = applyEdit(eColorKnobRoleGain, basis, v, satAbs, 0.0);
    ASSERT_TRUE(satResult.valid);
    EXPECT_NEAR(0.02, planeSaturation(toPlane(eColorKnobRoleGain, basis, satResult.value)), 1e-9);

    ColorEdit valueAbsRatio;
    valueAbsRatio.axis = eColorAxisValue;
    valueAbsRatio.d1 = 1.0;
    valueAbsRatio.absolute = true;
    ChromaEditResult valueResult = applyEdit(eColorKnobRoleGain, basis, v, valueAbsRatio, 0.0);
    EXPECT_NEAR(2.0, luma(basis, valueResult.value.rgb), 1e-12);

    ColorEdit valueRelOffset;
    valueRelOffset.axis = eColorAxisValue;
    valueRelOffset.d1 = 0.05;
    KnobColorValue offsetV = makeValue(0.0, 0.0, 0.0, 0.0, false);
    ChromaEditResult offsetValueResult = applyEdit(eColorKnobRoleOffset, basis, offsetV, valueRelOffset, 0.0);
    EXPECT_NEAR(0.05, luma(basis, offsetValueResult.value.rgb), 1e-12);

    ColorEdit ttAbs;
    ttAbs.axis = eColorAxisTempTint;
    ttAbs.d1 = 20.0;
    ttAbs.d2 = -5.0;
    ttAbs.absolute = true;
    ChromaEditResult ttResult = applyEdit(eColorKnobRoleGain, basis, v, ttAbs, 0.0);
    ASSERT_TRUE(ttResult.valid);
    TempTintResult ttBack = toTempTint(eColorKnobRoleGain, basis, ttResult.value);
    ASSERT_TRUE(ttBack.valid);
    EXPECT_NEAR(20.0, ttBack.temp, 1e-6);
    EXPECT_NEAR(-5.0, ttBack.tint, 1e-6);
}

TEST(ColorWheelMath, RelativeTempTintEditFallsBackToTheLinearAxesOutOfDomain)
{
    // The same out-of-domain target OutOfDomainTempTintNeverProducesNaN uses for the absolute
    // edit, which fromTempTint alone correctly refuses: here the *relative* dispatcher is
    // expected to still move, by falling back to a direct step along linearTempTintAxes.
    LumaBasis basis = acescg();
    KnobColorValue white = makeValue(1.0, 1.0, 1.0, 1.0);

    ChromaEditResult absolute = fromTempTint(eColorKnobRoleGain, basis, white, 100000.0, 0.0);
    ASSERT_FALSE(absolute.valid);

    ColorEdit relative;
    relative.axis = eColorAxisTempTint;
    relative.d1 = 100000.0;
    relative.d2 = 0.0;
    ChromaEditResult viaDispatch = applyEdit(eColorKnobRoleGain, basis, white, relative, 0.0);

    ASSERT_TRUE(viaDispatch.valid);
    for (int i = 0; i < 3; ++i) {
        EXPECT_FALSE(std::isnan(viaDispatch.value.rgb[i]));
    }
    EXPECT_NEAR(1.0, luma(basis, viaDispatch.value.rgb), 1e-9);
}

TEST(ColorWheelMath, GlobalSYDragMovesSaturationAtTenPercentOfTheWheelsOwnRate)
{
    LumaBasis basis = acescg();
    TrackballRates rates;
    KnobColorValue neutral = makeValue(1.0, 1.0, 1.0, 1.0);

    ChromaEditResult fullSpeed = globalModeEdit(eColorGlobalModeS, eColorKnobRoleGain, basis, neutral,
                                                0.0, -100.0, gear(false, false), rates, 0.0);
    ASSERT_TRUE(fullSpeed.valid);
    EXPECT_NEAR(3.5e-3, planeSaturation(toPlane(eColorKnobRoleGain, basis, fullSpeed.value)), 1e-9);

    ChromaEditResult geared = globalModeEdit(eColorGlobalModeS, eColorKnobRoleGain, basis, neutral,
                                             0.0, -100.0, gear(true, false), rates, 0.0);
    ASSERT_TRUE(geared.valid);
    EXPECT_NEAR(3.5e-4, planeSaturation(toPlane(eColorKnobRoleGain, basis, geared.value)), 1e-9);
}

TEST(ColorWheelMath, GlobalSXDragRotatesHueAtHalfADegreePerPixel)
{
    LumaBasis basis = acescg();
    TrackballRates rates;
    KnobColorValue chromatic = makeValue(1.2, 1.0, 0.9, 1.0);
    double startHue = planeHue(toPlane(eColorKnobRoleGain, basis, chromatic));

    ChromaEditResult edited = globalModeEdit(eColorGlobalModeS, eColorKnobRoleGain, basis, chromatic,
                                             40.0, 0.0, gear(false, false), rates, startHue);

    ASSERT_TRUE(edited.valid);
    double expectedHue = std::fmod(startHue + 40.0 * rates.hueDegreesPerPixel + 360.0, 360.0);
    EXPECT_NEAR(expectedHue, planeHue(toPlane(eColorKnobRoleGain, basis, edited.value)), 1e-6);
}

TEST(ColorWheelMath, GlobalCDragTranslatesThePlaneLikeTheWheelsOwnTrackball)
{
    LumaBasis basis = acescg();
    TrackballRates rates;
    KnobColorValue neutral = makeValue(1.0, 1.0, 1.0, 1.0);

    ChromaEditResult edited = globalModeEdit(eColorGlobalModeC, eColorKnobRoleGain, basis, neutral,
                                             64.0, 0.0, gear(false, false), rates, 0.0);

    ASSERT_TRUE(edited.valid);
    PlanePoint q = toPlane(eColorKnobRoleGain, basis, edited.value);
    EXPECT_NEAR(rates.wheelRim, q.x, 1e-9);
    EXPECT_NEAR(0.0, q.y, 1e-9);
    EXPECT_NEAR(1.0, luma(basis, edited.value.rgb), 1e-12);
    EXPECT_NEAR(neutral.a, edited.value.a, 1e-12);
}

TEST(ColorWheelMath, GlobalCDragIsLinearInTheMoveDistance)
{
    LumaBasis basis = acescg();
    TrackballRates rates;
    KnobColorValue chromatic = makeValue(1.2, 1.0, 0.9, 1.0);

    ChromaEditResult oneMove = globalModeEdit(eColorGlobalModeC, eColorKnobRoleGain, basis, chromatic,
                                              40.0, -25.0, gear(false, false), rates, 0.0);
    ChromaEditResult firstHalf = globalModeEdit(eColorGlobalModeC, eColorKnobRoleGain, basis, chromatic,
                                                20.0, -12.5, gear(false, false), rates, 0.0);
    ChromaEditResult secondHalf = globalModeEdit(eColorGlobalModeC, eColorKnobRoleGain, basis, firstHalf.value,
                                                 20.0, -12.5, gear(false, false), rates, 0.0);

    ASSERT_TRUE(oneMove.valid);
    ASSERT_TRUE(secondHalf.valid);
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(oneMove.value.rgb[i], secondHalf.value.rgb[i], 1e-12);
    }
}

TEST(ColorWheelMath, GlobalVUsesDxMinusDyAtTheRolesValueRate)
{
    LumaBasis basis = acescg();
    TrackballRates rates;

    KnobColorValue gainV = makeValue(1.0, 1.0, 1.0, 1.0);
    ChromaEditResult gainResult = globalModeEdit(eColorGlobalModeV, eColorKnobRoleGain, basis, gainV,
                                                 10.0, -10.0, gear(false, false), rates, 0.0);
    double expectedStops = 20.0 * rates.valueStopPerPixelRatio;
    EXPECT_NEAR(std::pow(2.0, expectedStops), luma(basis, gainResult.value.rgb), 1e-12);

    KnobColorValue offsetV = makeValue(0.0, 0.0, 0.0, 0.0, false);
    ChromaEditResult offsetResult = globalModeEdit(eColorGlobalModeV, eColorKnobRoleOffset, basis, offsetV,
                                                   10.0, -10.0, gear(false, false), rates, 0.0);
    double expectedDelta = 20.0 * rates.valuePerPixelOffset;
    EXPECT_NEAR(expectedDelta, luma(basis, offsetResult.value.rgb), 1e-12);
}

TEST(ColorWheelMath, GlobalTModeKeepsLumaExactForRatioAndOffsetRoles)
{
    LumaBasis basis = acescg();
    TrackballRates rates;

    KnobColorValue gainV = makeValue(1.2, 1.0, 0.9, 1.0);
    ChromaEditResult gainResult = globalModeEdit(eColorGlobalModeT, eColorKnobRoleGain, basis, gainV,
                                                 15.0, -8.0, gear(false, false), rates, 0.0);
    ASSERT_TRUE(gainResult.valid);
    expectLumaAndAlphaKept(eColorKnobRoleGain, basis, gainV, gainResult);

    KnobColorValue offsetV = makeValue(0.05, -0.02, 0.03, 0.0, false);
    ChromaEditResult offsetResult = globalModeEdit(eColorGlobalModeT, eColorKnobRoleOffset, basis, offsetV,
                                                   15.0, -8.0, gear(false, false), rates, 0.0);
    ASSERT_TRUE(offsetResult.valid);
    expectLumaAndAlphaKept(eColorKnobRoleOffset, basis, offsetV, offsetResult);
}

TEST(ColorWheelMath, SliderMappingMatchesTheHybridTableRanges)
{
    TrackballRates rates;

    SliderRange hue = sliderMapping(eColorSliderKindHue, eColorKnobRoleGain, rates);
    EXPECT_EQ(0.0, hue.rangeMin);
    EXPECT_EQ(360.0, hue.rangeMax);

    SliderRange sat = sliderMapping(eColorSliderKindSat, eColorKnobRoleOffset, rates);
    EXPECT_EQ(0.0, sat.rangeMin);
    EXPECT_EQ(rates.wheelRim, sat.rangeMax);

    SliderRange valueGain = sliderMapping(eColorSliderKindValue, eColorKnobRoleGain, rates);
    EXPECT_EQ(-3.0, valueGain.rangeMin);
    EXPECT_EQ(3.0, valueGain.rangeMax);

    SliderRange valueOffset = sliderMapping(eColorSliderKindValue, eColorKnobRoleOffset, rates);
    EXPECT_EQ(-0.25, valueOffset.rangeMin);
    EXPECT_EQ(0.25, valueOffset.rangeMax);

    SliderRange tempGain = sliderMapping(eColorSliderKindTemp, eColorKnobRolePower, rates);
    EXPECT_EQ(-100.0, tempGain.rangeMin);
    EXPECT_EQ(100.0, tempGain.rangeMax);

    SliderRange tempOffset = sliderMapping(eColorSliderKindTemp, eColorKnobRoleOffset, rates);
    EXPECT_EQ(-0.1, tempOffset.rangeMin);
    EXPECT_EQ(0.1, tempOffset.rangeMax);

    SliderRange tintGain = sliderMapping(eColorSliderKindTint, eColorKnobRoleColour, rates);
    EXPECT_EQ(-50.0, tintGain.rangeMin);
    EXPECT_EQ(50.0, tintGain.rangeMax);

    SliderRange tintOffset = sliderMapping(eColorSliderKindTint, eColorKnobRoleOffset, rates);
    EXPECT_EQ(-0.1, tintOffset.rangeMin);
    EXPECT_EQ(0.1, tintOffset.rangeMax);

    SliderRange valuePower = sliderMapping(eColorSliderKindValue, eColorKnobRolePower, rates);
    EXPECT_EQ(-2.0, valuePower.rangeMin);
    EXPECT_EQ(2.0, valuePower.rangeMax);
}

TEST(ColorWheelMath, SliderMarkerMapsOntoTheRange)
{
    TrackballRates rates;
    SliderRange temp = sliderMapping(eColorSliderKindTemp, eColorKnobRoleGain, rates);

    EXPECT_NEAR(-100.0, markerToValue(temp, 0.0), 1e-12);
    EXPECT_NEAR(0.0, markerToValue(temp, 0.5), 1e-12);
    EXPECT_NEAR(100.0, markerToValue(temp, 1.0), 1e-12);
    EXPECT_NEAR(100.0, markerToValue(temp, 1.5), 1e-12);
    EXPECT_NEAR(-100.0, markerToValue(temp, -0.5), 1e-12);
}

TEST(ColorWheelMath, PolarEditMovesHueAndRadius)
{
    PolarValue start;
    start.hueDegrees = 0.0;
    start.radius = 0.1;

    ColorEdit up;
    up.axis = eColorAxisTrackball;
    up.d1 = -0.1;
    up.d2 = 0.1;
    PolarValue moved = applyPolarEdit(start, up, 1.0);
    EXPECT_NEAR(90.0, moved.hueDegrees, 1e-9);
    EXPECT_NEAR(0.1, moved.radius, 1e-12);

    ColorEdit far;
    far.axis = eColorAxisTrackball;
    far.d1 = 0.0;
    far.d2 = -5.0;
    PolarValue clamped = applyPolarEdit(start, far, 0.5);
    EXPECT_NEAR(0.5, clamped.radius, 1e-12);
    EXPECT_GE(clamped.hueDegrees, 0.0);
    EXPECT_LT(clamped.hueDegrees, 360.0);
    EXPECT_GT(clamped.hueDegrees, 270.0);

    ColorEdit centre;
    centre.axis = eColorAxisTrackball;
    centre.d1 = -0.1;
    PolarValue atCentre = applyPolarEdit(start, centre, 1.0);
    EXPECT_NEAR(0.0, atCentre.radius, 1e-12);
    EXPECT_EQ(0.0, atCentre.hueDegrees);

    ColorEdit hue;
    hue.axis = eColorAxisHue;
    hue.absolute = true;
    hue.d1 = -30.0;
    PolarValue turned = applyPolarEdit(start, hue, 1.0);
    EXPECT_NEAR(330.0, turned.hueDegrees, 1e-9);
    EXPECT_NEAR(0.1, turned.radius, 1e-12);

    ColorEdit reset;
    reset.axis = eColorAxisSat;
    reset.absolute = true;
    reset.d1 = 0.0;
    EXPECT_EQ(0.0, applyPolarEdit(start, reset, 1.0).radius);
}
