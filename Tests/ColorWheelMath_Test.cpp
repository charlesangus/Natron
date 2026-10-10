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
