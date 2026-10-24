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
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <QComboBox>
#include <QCursor>
#include <QGuiApplication>
#include <QWidget>

#include <gtest/gtest.h>

#include "Engine/ColorWheelMath.h"

#include "Gui/ColorPanelLayoutDev.h"
#include "Gui/ColorTrackballGlobalMode.h"
#include "Gui/ColorTrackballHud.h"

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
    ColorGlobalModeKeyAction s = classifyColorGlobalModeKey(Qt::Key_S, Qt::NoModifier, false);
    ColorGlobalModeKeyAction t = classifyColorGlobalModeKey(Qt::Key_T, Qt::NoModifier, true);
    ColorGlobalModeKeyAction v = classifyColorGlobalModeKey(Qt::Key_V, Qt::ShiftModifier, false);

    EXPECT_EQ(eColorGlobalModeKeySetMode, c.action);
    EXPECT_EQ(eColorGlobalModeC, c.mode);
    EXPECT_EQ(eColorGlobalModeKeySetMode, s.action);
    EXPECT_EQ(eColorGlobalModeS, s.mode);
    EXPECT_EQ(eColorGlobalModeKeySetMode, t.action);
    EXPECT_EQ(eColorGlobalModeT, t.mode);
    EXPECT_EQ(eColorGlobalModeKeySetMode, v.action);
    EXPECT_EQ(eColorGlobalModeV, v.mode);
}

TEST(ColorTrackballGlobalModeKeys, DroppedAndForeignKeysAreSwallowed)
{
    EXPECT_EQ(eColorGlobalModeKeySwallow, classifyColorGlobalModeKey(Qt::Key_H, Qt::NoModifier, false).action);
    EXPECT_EQ(eColorGlobalModeKeySwallow, classifyColorGlobalModeKey(Qt::Key_S, Qt::ControlModifier, false).action);
    EXPECT_EQ(eColorGlobalModeKeySwallow, classifyColorGlobalModeKey(Qt::Key_C, Qt::ControlModifier, false).action);
    EXPECT_EQ(eColorGlobalModeKeySwallow, classifyColorGlobalModeKey(Qt::Key_G, Qt::ControlModifier, false).action);
    EXPECT_EQ(eColorGlobalModeKeySwallow, classifyColorGlobalModeKey(Qt::Key_Down, Qt::NoModifier, false).action);
    EXPECT_EQ(eColorGlobalModeKeySwallow, classifyColorGlobalModeKey(Qt::Key_Delete, Qt::NoModifier, false).action);
}

TEST(ColorTrackballGlobalModeKeys, ShortcutOverrideClaimsBareSWhileEngaged)
{
    QList<QKeySequence> passThrough;

    passThrough.push_back(QKeySequence(Qt::CTRL | Qt::Key_E));

    // Bare S is Project Settings' own shortcut; the mode must claim it instead of letting it
    // through, the same as it claims every other key that is not named in passThrough.
    EXPECT_TRUE(colorGlobalModeShortcutOverrideAccepted(Qt::Key_S, Qt::NoModifier, passThrough));
    EXPECT_TRUE(colorGlobalModeShortcutOverrideAccepted(Qt::Key_C, Qt::NoModifier, passThrough));
    EXPECT_FALSE(colorGlobalModeShortcutOverrideAccepted(Qt::Key_E, Qt::ControlModifier, passThrough));
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

    const ColorGlobalModeEnum modes[] = { eColorGlobalModeC, eColorGlobalModeS, eColorGlobalModeT, eColorGlobalModeV };

    for (std::size_t m = 0; m < sizeof(modes) / sizeof(modes[0]); ++m) {
        ChromaEditResult r = stroke.evaluate(modes[m], eColorKnobRoleGain, basis, rates);

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
    const ColorGlobalModeEnum modes[] = { eColorGlobalModeC, eColorGlobalModeS, eColorGlobalModeT };

    for (std::size_t m = 0; m < sizeof(modes) / sizeof(modes[0]); ++m) {
        ColorGlobalModeStroke stroke;

        stroke.begin(start, 0.0);
        stroke.addMove(40.0, -30.0, 1.0);

        ChromaEditResult r = stroke.evaluate(modes[m], eColorKnobRoleGain, basis, rates);

        ASSERT_TRUE(r.valid) << "mode " << m;
        EXPECT_NEAR(Y, luma(basis, r.value.rgb), 1e-12 * Y) << "mode " << m;
        EXPECT_DOUBLE_EQ(1.0, r.value.a) << "mode " << m;
    }
}

TEST(ColorTrackballGlobalModeStroke, SModeNeutralStartGrowsSaturationAlongTheDraggedHue)
{
    TrackballRates rates;
    LumaBasis basis = acescg();
    ColorGlobalModeStroke stroke;

    stroke.begin(rgba(1, 1, 1, 1), 10.0);
    // 60 px of x at 0.5 degree per px turns the remembered hue by 30 degrees; 1000 px up grows
    // saturation by 1000 times the saturation rate.
    stroke.addMove(60.0, -1000.0, 1.0);

    ChromaEditResult r = stroke.evaluate(eColorGlobalModeS, eColorKnobRoleGain, basis, rates);

    ASSERT_TRUE(r.valid);

    PlanePoint p = toPlane(eColorKnobRoleGain, basis, r.value);

    ASSERT_TRUE(p.valid);
    EXPECT_NEAR(10.0 + 60.0 * rates.hueDegreesPerPixel, planeHue(p), 1e-6);
    EXPECT_NEAR(1000.0 * rates.satPlanePerPixel, planeSaturation(p), 1e-9);
    EXPECT_NEAR(planeHue(p), stroke.rememberedHueAfter(eColorGlobalModeS, eColorKnobRoleGain, basis, r.value, rates), 1e-9);
}

TEST(ColorTrackballGlobalModeStroke, CModeDragTranslatesThePlaneLikeTheWheelsOwnTrackball)
{
    TrackballRates rates;
    LumaBasis basis = acescg();
    ColorGlobalModeStroke stroke;

    stroke.begin(rgba(1, 1, 1, 1), 0.0);
    stroke.addMove(64.0, 0.0, 1.0);

    ChromaEditResult r = stroke.evaluate(eColorGlobalModeC, eColorKnobRoleGain, basis, rates);

    ASSERT_TRUE(r.valid);

    PlanePoint p = toPlane(eColorKnobRoleGain, basis, r.value);

    ASSERT_TRUE(p.valid);
    EXPECT_NEAR(rates.wheelRim, p.x, 1e-9);
    EXPECT_NEAR(0.0, p.y, 1e-9);
    EXPECT_NEAR(1.0, luma(basis, r.value.rgb), 1e-12);
}

TEST(ColorTrackballGlobalModeStroke, RememberedHueAdvancesOnAnSStrokeThatStaysNeutral)
{
    TrackballRates rates;
    LumaBasis basis = acescg();
    KnobColorValue neutral = rgba(1, 1, 1, 1);
    ColorGlobalModeStroke stroke;

    stroke.begin(neutral, 350.0);
    stroke.addMove(40.0, 0.0, 1.0);

    // Only S reads its stroke's hue travel back into the remembered hue: C's translate has no
    // hue axis of its own to lose, and V never touches hue either.
    EXPECT_NEAR(10.0, stroke.rememberedHueAfter(eColorGlobalModeS, eColorKnobRoleGain, basis, neutral, rates), 1e-9);
    EXPECT_NEAR(350.0, stroke.rememberedHueAfter(eColorGlobalModeC, eColorKnobRoleGain, basis, neutral, rates), 1e-9);
    EXPECT_NEAR(350.0, stroke.rememberedHueAfter(eColorGlobalModeV, eColorKnobRoleGain, basis, neutral, rates), 1e-9);
}

TEST(ColorTrackballGlobalModeNames, ModeKeyNames)
{
    EXPECT_EQ(std::string("C"), colorGlobalModeKeyName(eColorGlobalModeC).toStdString());
    EXPECT_EQ(std::string("S"), colorGlobalModeKeyName(eColorGlobalModeS).toStdString());
    EXPECT_EQ(std::string("T"), colorGlobalModeKeyName(eColorGlobalModeT).toStdString());
    EXPECT_EQ(std::string("V"), colorGlobalModeKeyName(eColorGlobalModeV).toStdString());
    EXPECT_TRUE(ColorTrackballGlobalMode::instanceFor(nullptr) == nullptr);
}

TEST(ColorTrackballGlobalModeStroke, LongPowerValueStrokeParksAtTheGammaCap)
{
    TrackballRates rates;
    LumaBasis basis = acescg();
    ColorGlobalModeStroke stroke;
    double previous = 1.0;

    stroke.begin(rgba(1, 1, 1, 1), 0.0);
    for (int i = 0; i < 100; ++i) {
        stroke.addMove(10.0, 0.0, 1.0);

        ChromaEditResult r = stroke.evaluate(eColorGlobalModeV, eColorKnobRolePower, basis, rates);

        ASSERT_TRUE(r.valid);
        EXPECT_GE(r.value.rgb[0], previous - 1e-12) << "move " << i;
        EXPECT_LE(r.value.rgb[0], kPowerGammaMax + 1e-12) << "move " << i;
        previous = r.value.rgb[0];
    }
    EXPECT_NEAR(kPowerGammaMax, previous, 1e-9);
}

namespace {
int
overrideCursorDepth()
{
    // Pops everything to count it, then puts the same shapes back in the same order.
    std::vector<Qt::CursorShape> shapes;

    while (QGuiApplication::overrideCursor()) {
        shapes.push_back(QGuiApplication::overrideCursor()->shape());
        QGuiApplication::restoreOverrideCursor();
    }
    for (std::size_t i = shapes.size(); i > 0; --i) {
        QGuiApplication::setOverrideCursor(QCursor(shapes[i - 1]));
    }

    return (int)shapes.size();
}

const Settings::ColorTrackballFeedbackEnum kCrosshairReveal = Settings::eColorTrackballFeedbackCrosshairReveal;
const Settings::ColorTrackballFeedbackEnum kWidgetCursor = Settings::eColorTrackballFeedbackWidgetCursor;

Qt::CursorShape
topCursor()
{
    return QGuiApplication::overrideCursor() ? QGuiApplication::overrideCursor()->shape() : Qt::ArrowCursor;
}

// What the controller asks of its cursors with the switch's feedback at one moment.
void
syncCursors(ColorGlobalModeCursors& cursors,
            const ColorGlobalModeFeedbackSwitch& sw,
            bool engaged,
            bool strokeActive)
{
    ColorGlobalModeFeedbackState state = colorGlobalModeFeedbackState(sw.getFeedback(), engaged, strokeActive);

    cursors.sync(state.crosshair, state.blankCursor);
}

} // namespace

TEST(ColorTrackballGlobalModeFeedback, OnlyCrosshairRevealShowsTheCrosshair)
{
    EXPECT_TRUE(colorGlobalModeFeedbackShowsCrosshair(kCrosshairReveal));
    EXPECT_FALSE(colorGlobalModeFeedbackShowsCrosshair(kWidgetCursor));
}

TEST(ColorTrackballGlobalModeFeedback, DefaultIsThePreferencesDefault)
{
    ColorGlobalModeFeedbackSwitch sw;

    EXPECT_EQ(kCrosshairReveal, sw.getFeedback());
}

TEST(ColorTrackballGlobalModeFeedback, AChangeOutsideAStrokeAppliesAtOnce)
{
    ColorGlobalModeFeedbackSwitch sw;

    EXPECT_TRUE(sw.offer(kWidgetCursor, false));
    EXPECT_EQ(kWidgetCursor, sw.getFeedback());
    EXPECT_FALSE(sw.offer(kWidgetCursor, false));
    EXPECT_FALSE(sw.strokeEnded());
    EXPECT_TRUE(sw.offer(kCrosshairReveal, false));
    EXPECT_EQ(kCrosshairReveal, sw.getFeedback());
}

TEST(ColorTrackballGlobalModeFeedback, AChangeMidStrokeWaitsForTheStrokeToEnd)
{
    ColorGlobalModeFeedbackSwitch sw;

    EXPECT_FALSE(sw.offer(kWidgetCursor, true));
    EXPECT_EQ(kCrosshairReveal, sw.getFeedback());
    EXPECT_TRUE(sw.strokeEnded());
    EXPECT_EQ(kWidgetCursor, sw.getFeedback());
    EXPECT_FALSE(sw.strokeEnded());

    // A value set and then reverted within one stroke leaves nothing to apply at its end.
    EXPECT_FALSE(sw.offer(kCrosshairReveal, true));
    EXPECT_FALSE(sw.offer(kWidgetCursor, true));
    EXPECT_FALSE(sw.strokeEnded());
    EXPECT_EQ(kWidgetCursor, sw.getFeedback());
}

TEST(ColorTrackballGlobalModeFeedback, AFlipMidStrokeShowsAfterReleaseAndExitLeavesNoCursor)
{
    const int baseline = overrideCursorDepth();

    {
        ColorGlobalModeFeedbackSwitch sw;
        ColorGlobalModeCursors cursors;

        // Engage with Crosshair + reveal, press, then flip the preference mid-stroke.
        syncCursors(cursors, sw, true, false);
        EXPECT_EQ(Qt::CrossCursor, topCursor());
        syncCursors(cursors, sw, true, true);
        EXPECT_FALSE(sw.offer(kWidgetCursor, true));
        syncCursors(cursors, sw, true, true);
        EXPECT_EQ(baseline + 2, overrideCursorDepth());
        EXPECT_EQ(Qt::BlankCursor, topCursor());
        EXPECT_TRUE(colorGlobalModeFeedbackState(sw.getFeedback(), true, true).crosshair);

        // Release: the widget cursor takes over, with no crosshair under it.
        EXPECT_TRUE(sw.strokeEnded());
        syncCursors(cursors, sw, true, false);
        EXPECT_EQ(baseline + 1, overrideCursorDepth());
        EXPECT_EQ(Qt::BlankCursor, topCursor());
        EXPECT_TRUE(colorGlobalModeFeedbackState(sw.getFeedback(), true, false).graphicFollowsPointer);

        // And back the same way.
        syncCursors(cursors, sw, true, true);
        EXPECT_FALSE(sw.offer(kCrosshairReveal, true));
        EXPECT_FALSE(colorGlobalModeFeedbackState(sw.getFeedback(), true, true).crosshair);
        EXPECT_TRUE(sw.strokeEnded());
        syncCursors(cursors, sw, true, false);
        EXPECT_EQ(baseline + 1, overrideCursorDepth());
        EXPECT_EQ(Qt::CrossCursor, topCursor());

        // Exit.
        syncCursors(cursors, sw, false, false);
        EXPECT_EQ(baseline, overrideCursorDepth());

        // Exit mid-stroke with each value.
        for (Settings::ColorTrackballFeedbackEnum f : { kCrosshairReveal, kWidgetCursor }) {
            sw.offer(f, false);
            syncCursors(cursors, sw, true, false);
            syncCursors(cursors, sw, true, true);
            syncCursors(cursors, sw, false, false);
            EXPECT_EQ(baseline, overrideCursorDepth());
        }
    }
    EXPECT_EQ(baseline, overrideCursorDepth());
}

TEST(ColorTrackballGlobalModeFeedback, CrosshairIsPushedOnceAndRestoredOnce)
{
    const int baseline = overrideCursorDepth();

    {
        ColorGlobalModeOverrideCursor crosshair(Qt::CrossCursor);

        EXPECT_FALSE(crosshair.isHeld());
        crosshair.setHeld(true);
        ASSERT_TRUE(QGuiApplication::overrideCursor() != nullptr);
        EXPECT_EQ(Qt::CrossCursor, QGuiApplication::overrideCursor()->shape());
        EXPECT_EQ(baseline + 1, overrideCursorDepth());

        // Engage while engaged, then exit followed by a leave for a modal widget.
        crosshair.setHeld(true);
        EXPECT_EQ(baseline + 1, overrideCursorDepth());
        crosshair.setHeld(false);
        crosshair.setHeld(false);
        EXPECT_EQ(baseline, overrideCursorDepth());

        // A second engage/exit cycle.
        crosshair.setHeld(true);
        EXPECT_EQ(Qt::CrossCursor, QGuiApplication::overrideCursor()->shape());
        crosshair.setHeld(false);
        EXPECT_EQ(baseline, overrideCursorDepth());

        // Destroyed while held, as when the Gui goes away mid-session.
        crosshair.setHeld(true);
    }
    EXPECT_EQ(baseline, overrideCursorDepth());
}

TEST(ColorTrackballGlobalModeFeedback, TheDevBarHasNoFeedbackControls)
{
    qputenv("NATRON_COLOR_PANEL_DEV", "1");
    ColorPanelLayoutDev::reloadForTests();

    std::unique_ptr<QWidget> bar(ColorPanelLayoutDev::createDevBar(nullptr));

    ASSERT_TRUE(bar != nullptr);
    EXPECT_TRUE(bar->findChild<QComboBox*>(QString::fromUtf8("ColorPanelDevFeedback")) == nullptr);
    EXPECT_TRUE(bar->findChild<QComboBox*>(QString::fromUtf8("ColorPanelDevStyle")) != nullptr);
    EXPECT_TRUE(bar->findChild<QComboBox*>(QString::fromUtf8("ColorPanelDevPlacement")) != nullptr);
    EXPECT_EQ(2, bar->findChildren<QComboBox*>().size());

    bar.reset();
    qunsetenv("NATRON_COLOR_PANEL_DEV");
    ColorPanelLayoutDev::reloadForTests();
}
