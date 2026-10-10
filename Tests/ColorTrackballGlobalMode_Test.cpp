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

#include <cmath>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/ColorWheelMath.h"

#include "Gui/ColorTrackballGlobalMode.h"

NATRON_NAMESPACE_USING

namespace {
const double kAP1Weights[3] = { 0.2722287168, 0.6740817658, 0.0536895174 };
const double kAP1ToXYZ[9] = {
    0.6624541811, 0.1340042065, 0.1561876870,
    0.2722287168, 0.6740817658, 0.0536895174,
    -0.0055746495, 0.0040607335, 1.0103391003
};

LumaBasis
acescg()
{
    return LumaBasis(kAP1Weights, kAP1ToXYZ);
}

KnobColorValue
rgba(double r,
     double g,
     double b,
     double a)
{
    KnobColorValue v;

    v.rgb[0] = r;
    v.rgb[1] = g;
    v.rgb[2] = b;
    v.a = a;
    v.hasAlpha = true;

    return v;
}

ColorKnobInfo
knobInfo(bool enabled,
         bool visible)
{
    ColorKnobInfo info;

    info.scriptName = "colour";
    info.enabled = enabled;
    info.visible = visible;

    return info;
}

void
expectSameValue(const KnobColorValue& expected,
                const KnobColorValue& actual,
                double tolerance)
{
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(expected.rgb[i], actual.rgb[i], tolerance) << "channel " << i;
    }
    EXPECT_NEAR(expected.a, actual.a, tolerance) << "alpha";
}
} // namespace

TEST(ColorTrackballGlobalModeKeys, ModeKeysPickTheMode)
{
    ColorGlobalModeKeyAction c = classifyColorGlobalModeKey(Qt::Key_C, Qt::NoModifier, false);
    ColorGlobalModeKeyAction t = classifyColorGlobalModeKey(Qt::Key_T, Qt::NoModifier, true);
    ColorGlobalModeKeyAction v = classifyColorGlobalModeKey(Qt::Key_V, Qt::ShiftModifier, false);

    EXPECT_EQ(eColorGlobalModeKeySetMode, c.action);
    EXPECT_EQ(eColorGlobalModeC, c.mode);
    EXPECT_EQ(eColorGlobalModeKeySetMode, t.action);
    EXPECT_EQ(eColorGlobalModeT, t.mode);
    EXPECT_EQ(eColorGlobalModeKeySetMode, v.action);
    EXPECT_EQ(eColorGlobalModeV, v.mode);
}

TEST(ColorTrackballGlobalModeKeys, DroppedAndForeignKeysAreSwallowed)
{
    EXPECT_EQ(eColorGlobalModeKeySwallow, classifyColorGlobalModeKey(Qt::Key_H, Qt::NoModifier, false).action);
    EXPECT_EQ(eColorGlobalModeKeySwallow, classifyColorGlobalModeKey(Qt::Key_S, Qt::NoModifier, false).action);
    EXPECT_EQ(eColorGlobalModeKeySwallow, classifyColorGlobalModeKey(Qt::Key_C, Qt::ControlModifier, false).action);
    EXPECT_EQ(eColorGlobalModeKeySwallow, classifyColorGlobalModeKey(Qt::Key_G, Qt::ControlModifier, false).action);
    EXPECT_EQ(eColorGlobalModeKeySwallow, classifyColorGlobalModeKey(Qt::Key_Down, Qt::NoModifier, false).action);
    EXPECT_EQ(eColorGlobalModeKeySwallow, classifyColorGlobalModeKey(Qt::Key_Delete, Qt::NoModifier, false).action);
}

TEST(ColorTrackballGlobalModeKeys, EscapeAndEnterAccept)
{
    EXPECT_EQ(eColorGlobalModeKeyAccept, classifyColorGlobalModeKey(Qt::Key_Escape, Qt::NoModifier, false).action);
    EXPECT_EQ(eColorGlobalModeKeyAccept, classifyColorGlobalModeKey(Qt::Key_Return, Qt::NoModifier, true).action);
    EXPECT_EQ(eColorGlobalModeKeyAccept, classifyColorGlobalModeKey(Qt::Key_Enter, Qt::KeypadModifier, false).action);
}

TEST(ColorTrackballGlobalModeKeys, CtrlUpDownSwitchKnob)
{
    EXPECT_EQ(eColorGlobalModeKeyPreviousKnob, classifyColorGlobalModeKey(Qt::Key_Up, Qt::ControlModifier, false).action);
    EXPECT_EQ(eColorGlobalModeKeyNextKnob, classifyColorGlobalModeKey(Qt::Key_Down, Qt::ControlModifier | Qt::KeypadModifier, true).action);
}

TEST(ColorTrackballGlobalModeKeys, UndoAndRedoOnlyBetweenStrokes)
{
    EXPECT_EQ(eColorGlobalModeKeyUndo, classifyColorGlobalModeKey(Qt::Key_Z, Qt::ControlModifier, false).action);
    EXPECT_EQ(eColorGlobalModeKeyRedo, classifyColorGlobalModeKey(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier, false).action);
    EXPECT_EQ(eColorGlobalModeKeyRedo, classifyColorGlobalModeKey(Qt::Key_Y, Qt::ControlModifier, false).action);
    EXPECT_EQ(eColorGlobalModeKeySwallow, classifyColorGlobalModeKey(Qt::Key_Z, Qt::ControlModifier, true).action);
    EXPECT_EQ(eColorGlobalModeKeySwallow, classifyColorGlobalModeKey(Qt::Key_Z, Qt::NoModifier, false).action);
}

TEST(ColorTrackballGlobalModeKeys, ModifierKeysReportTheirOwnState)
{
    EXPECT_EQ(Qt::KeyboardModifiers(Qt::ControlModifier), colorGlobalModeModifiersAfterKey(Qt::Key_Control, true, Qt::NoModifier));
    EXPECT_EQ(Qt::KeyboardModifiers(Qt::ShiftModifier), colorGlobalModeModifiersAfterKey(Qt::Key_Control, false, Qt::ControlModifier | Qt::ShiftModifier));
    EXPECT_EQ(Qt::KeyboardModifiers(Qt::ShiftModifier), colorGlobalModeModifiersAfterKey(Qt::Key_C, true, Qt::ShiftModifier));
}

TEST(ColorTrackballGlobalModeKnobs, StepsSkipIllegalKnobsAndStopAtTheEnds)
{
    std::vector<ColorKnobInfo> knobs;

    knobs.push_back(knobInfo(true, true));
    knobs.push_back(knobInfo(false, true));
    knobs.push_back(knobInfo(true, false));
    knobs.push_back(knobInfo(true, true));

    EXPECT_EQ(3, stepColorKnobIndex(knobs, 0, 1));
    EXPECT_EQ(0, stepColorKnobIndex(knobs, 3, -1));
    EXPECT_EQ(-1, stepColorKnobIndex(knobs, 3, 1));
    EXPECT_EQ(-1, stepColorKnobIndex(knobs, 0, -1));
}

TEST(ColorTrackballGlobalModeKnobs, WheelStepsOncePerNotch)
{
    int accumulator = 0;

    EXPECT_EQ(1, takeColorGlobalModeWheelNotches(&accumulator, 120));
    EXPECT_EQ(0, takeColorGlobalModeWheelNotches(&accumulator, 60));
    EXPECT_EQ(1, takeColorGlobalModeWheelNotches(&accumulator, 60));
    EXPECT_EQ(0, takeColorGlobalModeWheelNotches(&accumulator, 100));
    EXPECT_EQ(-1, takeColorGlobalModeWheelNotches(&accumulator, -120));
    EXPECT_EQ(-2, takeColorGlobalModeWheelNotches(&accumulator, -240));
}

TEST(ColorTrackballGlobalModeStroke, ValueOnHundredPixelsDoublesGainAndAlpha)
{
    TrackballRates rates;
    LumaBasis basis = acescg();
    ColorGlobalModeStroke right;
    ColorGlobalModeStroke up;

    right.begin(rgba(1, 1, 1, 1), 0.0);
    right.addMove(100.0, 0.0, 1.0);
    up.begin(rgba(1, 1, 1, 1), 0.0);
    up.addMove(0.0, -100.0, 1.0);

    ChromaEditResult r = right.evaluate(eColorGlobalModeV, eColorKnobRoleGain, basis, rates);
    ChromaEditResult u = up.evaluate(eColorGlobalModeV, eColorKnobRoleGain, basis, rates);

    ASSERT_TRUE(r.valid);
    ASSERT_TRUE(u.valid);
    expectSameValue(rgba(2, 2, 2, 2), r.value, 1e-12);
    expectSameValue(rgba(2, 2, 2, 2), u.value, 1e-12);
}

TEST(ColorTrackballGlobalModeStroke, ManySmallMovesLandWhereOneBigMoveDoes)
{
    TrackballRates rates;
    LumaBasis basis = acescg();
    ColorGlobalModeStroke small;
    ColorGlobalModeStroke big;

    small.begin(rgba(1.2, 1.0, 0.8, 1.0), 0.0);
    big.begin(rgba(1.2, 1.0, 0.8, 1.0), 0.0);
    for (int i = 0; i < 100; ++i) {
        small.addMove(1.0, -0.5, 1.0);
    }
    big.addMove(100.0, -50.0, 1.0);

    ChromaEditResult s = small.evaluate(eColorGlobalModeC, eColorKnobRoleGain, basis, rates);
    ChromaEditResult b = big.evaluate(eColorGlobalModeC, eColorKnobRoleGain, basis, rates);

    ASSERT_TRUE(s.valid);
    ASSERT_TRUE(b.valid);
    expectSameValue(b.value, s.value, 1e-12);
}

TEST(ColorTrackballGlobalModeStroke, DraggingBackReturnsToTheStart)
{
    TrackballRates rates;
    LumaBasis basis = acescg();
    KnobColorValue start = rgba(1.1, 1.0, 0.9, 1.0);
    ColorGlobalModeStroke stroke;

    stroke.begin(start, 0.0);
    stroke.addMove(50.0, 20.0, 1.0);
    stroke.addMove(-50.0, -20.0, 1.0);

    for (int m = 0; m < 3; ++m) {
        ChromaEditResult r = stroke.evaluate((ColorGlobalModeEnum)m, eColorKnobRoleGain, basis, rates);

        ASSERT_TRUE(r.valid) << "mode " << m;
        expectSameValue(start, r.value, 1e-9);
    }
}

TEST(ColorTrackballGlobalModeStroke, GearAppliesFromTheNextMoveOnly)
{
    TrackballRates rates;
    LumaBasis basis = acescg();
    ColorGlobalModeStroke geared;
    ColorGlobalModeStroke plain;

    geared.begin(rgba(1, 1, 1, 1), 0.0);
    geared.addMove(10.0, 0.0, 1.0);
    geared.addMove(10.0, 0.0, gear(true, false));
    plain.begin(rgba(1, 1, 1, 1), 0.0);
    plain.addMove(11.0, 0.0, 1.0);

    EXPECT_NEAR(11.0, geared.getTotalDx(), 1e-12);

    ChromaEditResult g = geared.evaluate(eColorGlobalModeV, eColorKnobRoleGain, basis, rates);
    ChromaEditResult p = plain.evaluate(eColorGlobalModeV, eColorKnobRoleGain, basis, rates);

    ASSERT_TRUE(g.valid);
    expectSameValue(p.value, g.value, 1e-12);
}

TEST(ColorTrackballGlobalModeStroke, ChromaModesKeepLuma)
{
    TrackballRates rates;
    LumaBasis basis = acescg();
    KnobColorValue start = rgba(1.1, 1.0, 0.9, 1.0);
    const double Y = luma(basis, start.rgb);

    for (int m = 0; m < 2; ++m) {
        ColorGlobalModeStroke stroke;

        stroke.begin(start, 0.0);
        stroke.addMove(40.0, -30.0, 1.0);

        ChromaEditResult r = stroke.evaluate((ColorGlobalModeEnum)m, eColorKnobRoleGain, basis, rates);

        ASSERT_TRUE(r.valid) << "mode " << m;
        EXPECT_NEAR(Y, luma(basis, r.value.rgb), 1e-12 * Y) << "mode " << m;
        EXPECT_DOUBLE_EQ(1.0, r.value.a) << "mode " << m;
    }
}

TEST(ColorTrackballGlobalModeStroke, NeutralStartGrowsSaturationAlongTheDraggedHue)
{
    TrackballRates rates;
    LumaBasis basis = acescg();
    ColorGlobalModeStroke stroke;

    stroke.begin(rgba(1, 1, 1, 1), 10.0);
    // 60 px of x at 0.5 degree per px turns the remembered hue by 30 degrees; 1000 px up grows
    // saturation by 1000 times the saturation rate.
    stroke.addMove(60.0, -1000.0, 1.0);

    ChromaEditResult r = stroke.evaluate(eColorGlobalModeC, eColorKnobRoleGain, basis, rates);

    ASSERT_TRUE(r.valid);

    PlanePoint p = toPlane(eColorKnobRoleGain, basis, r.value);

    ASSERT_TRUE(p.valid);
    EXPECT_NEAR(10.0 + 60.0 * rates.hueDegreesPerPixel, planeHue(p), 1e-6);
    EXPECT_NEAR(1000.0 * rates.satPlanePerPixel, planeSaturation(p), 1e-9);
    EXPECT_NEAR(planeHue(p), stroke.rememberedHueAfter(eColorGlobalModeC, eColorKnobRoleGain, basis, r.value, rates), 1e-9);
}

TEST(ColorTrackballGlobalModeStroke, RememberedHueAdvancesOnAStrokeThatStaysNeutral)
{
    TrackballRates rates;
    LumaBasis basis = acescg();
    KnobColorValue neutral = rgba(1, 1, 1, 1);
    ColorGlobalModeStroke stroke;

    stroke.begin(neutral, 350.0);
    stroke.addMove(40.0, 0.0, 1.0);

    EXPECT_NEAR(10.0, stroke.rememberedHueAfter(eColorGlobalModeC, eColorKnobRoleGain, basis, neutral, rates), 1e-9);
    EXPECT_NEAR(350.0, stroke.rememberedHueAfter(eColorGlobalModeV, eColorKnobRoleGain, basis, neutral, rates), 1e-9);
}

TEST(ColorTrackballGlobalModeNames, ModeKeyNames)
{
    EXPECT_EQ(std::string("C"), ColorTrackballGlobalMode::modeKeyName(eColorGlobalModeC).toStdString());
    EXPECT_EQ(std::string("T"), ColorTrackballGlobalMode::modeKeyName(eColorGlobalModeT).toStdString());
    EXPECT_EQ(std::string("V"), ColorTrackballGlobalMode::modeKeyName(eColorGlobalModeV).toStdString());
    EXPECT_TRUE(ColorTrackballGlobalMode::instanceFor(nullptr) == nullptr);
}
