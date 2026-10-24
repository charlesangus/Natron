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
#include <vector>

#include <QCursor>
#include <QGuiApplication>
#include <QRect>
#include <QScreen>

#include <gtest/gtest.h>

#include "Engine/ColorWheelMath.h"
#include "Engine/Settings.h"

#include "Gui/ColorModeDiscWidget.h"
#include "Gui/ColorTrackballGlobalMode.h"
#include "Gui/ColorTrackballHud.h"
#include "Gui/ColorTrackballReveal.h"
#include "Gui/ColorWheelWidget.h"

NATRON_NAMESPACE_USING

namespace {
typedef Settings::ColorTrackballFeedbackEnum F;

const F kCrosshairReveal = Settings::eColorTrackballFeedbackCrosshairReveal;
const F kWidgetCursor = Settings::eColorTrackballFeedbackWidgetCursor;

const double kAP1Weights[3] = { 0.2722287168, 0.6740817658, 0.0536895174 };
const double kAP1ToXYZ[9] = {
    0.6624541811, 0.1340042065, 0.1561876870,
    0.2722287168, 0.6740817658, 0.0536895174,
    -0.0055746495, 0.0040607335, 1.0103391003
};
const double kInvSqrt2 = 0.70710678118654752440;

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

void
identity(float* /*rgb*/,
         int /*count*/)
{
}

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

Qt::CursorShape
topCursor()
{
    return QGuiApplication::overrideCursor() ? QGuiApplication::overrideCursor()->shape() : Qt::ArrowCursor;
}

// What the controller asks of its cursors for feedback at one moment.
void
syncCursors(ColorGlobalModeCursors& cursors,
            F feedback,
            bool engaged,
            bool strokeActive)
{
    ColorGlobalModeFeedbackState state = colorGlobalModeFeedbackState(feedback, engaged, strokeActive);

    cursors.sync(state.crosshair, state.blankCursor);
}

QPoint
windowCentre(const QWidget& w)
{
    return w.pos() + QPoint(w.width() / 2, w.height() / 2);
}
} // namespace

TEST(ColorTrackballReveal, WindowIsAClickThroughToolWindowOfTheRevealSize)
{
    ColorTrackballReveal reveal;
    const Qt::WindowFlags flags = reveal.windowFlags();

    EXPECT_TRUE(reveal.isWindow());
    EXPECT_EQ((int)Qt::Tool, (int)(flags & Qt::WindowType_Mask));
    EXPECT_TRUE(flags.testFlag(Qt::FramelessWindowHint));
    EXPECT_TRUE(flags.testFlag(Qt::WindowStaysOnTopHint));
    EXPECT_TRUE(flags.testFlag(Qt::WindowTransparentForInput));
    EXPECT_TRUE(flags.testFlag(Qt::WindowDoesNotAcceptFocus));
    EXPECT_TRUE(reveal.testAttribute(Qt::WA_ShowWithoutActivating));
    EXPECT_TRUE(reveal.testAttribute(Qt::WA_TransparentForMouseEvents));
    EXPECT_TRUE(reveal.testAttribute(Qt::WA_TranslucentBackground));
    EXPECT_EQ(ColorTrackballReveal::diameter(), reveal.width());
    EXPECT_EQ(ColorTrackballReveal::diameter(), reveal.height());
}

TEST(ColorTrackballReveal, PositionIsCentredAndClampedToTheScreen)
{
    const QRect screen(0, 0, 1920, 1080);
    const QSize size(240, 240);

    EXPECT_EQ(QPoint(380, 280), colorTrackballRevealPosition(QPoint(500, 400), size, screen));
    EXPECT_EQ(QPoint(0, 0), colorTrackballRevealPosition(QPoint(3, 4), size, screen));
    EXPECT_EQ(QPoint(1680, 840), colorTrackballRevealPosition(QPoint(1915, 1075), size, screen));

    // A second screen to the right of the first.
    const QRect right(1920, 0, 1280, 1024);

    EXPECT_EQ(QPoint(1920, 904 - 120), colorTrackballRevealPosition(QPoint(1925, 904), size, right));
    EXPECT_EQ(QPoint(380, 280), colorTrackballRevealPosition(QPoint(500, 400), size, QRect()));
}

TEST(ColorTrackballReveal, APressCentresTheWindowAndACornerKeepsItInside)
{
    QScreen* screen = QGuiApplication::primaryScreen();

    ASSERT_TRUE(screen != 0);

    const QRect g = screen->geometry();
    ColorTrackballReveal reveal;
    const int half = reveal.width() / 2;

    ASSERT_GE(g.width(), 500 + half);
    ASSERT_GE(g.height(), 400 + half);

    const QPoint press = g.topLeft() + QPoint(500, 400);

    reveal.centreOn(press);
    EXPECT_EQ(press, windowCentre(reveal));

    // Following the pointer is the same placement, repeated on every move.
    const QPoint moved = press + QPoint(-37, 22);

    reveal.centreOn(moved);
    EXPECT_EQ(moved, windowCentre(reveal));

    reveal.centreOn(g.topLeft());
    EXPECT_EQ(g.topLeft(), reveal.pos());
    reveal.centreOn(QPoint(g.left() + g.width() - 1, g.top() + g.height() - 1));
    EXPECT_EQ(g.left() + g.width(), reveal.geometry().right() + 1);
    EXPECT_EQ(g.top() + g.height(), reveal.geometry().bottom() + 1);
}

TEST(ColorTrackballReveal, SetModeSwapsTheWidgetInPlace)
{
    ColorTrackballReveal reveal;

    reveal.setMode(eColorGlobalModeC);
    EXPECT_TRUE(reveal.getModeWidget() == reveal.getWheel());
    EXPECT_FALSE(reveal.getWheel()->isHidden());
    EXPECT_TRUE(reveal.getDisc()->isHidden());

    for (ColorGlobalModeEnum mode : { eColorGlobalModeS, eColorGlobalModeT, eColorGlobalModeV }) {
        reveal.setMode(mode);
        EXPECT_TRUE(reveal.getModeWidget() == reveal.getDisc());
        EXPECT_EQ(mode, reveal.getDisc()->getMode());
        EXPECT_FALSE(reveal.getDisc()->isHidden());
        EXPECT_TRUE(reveal.getWheel()->isHidden());
        EXPECT_EQ(QRect(0, 0, reveal.width(), reveal.height()), reveal.getDisc()->geometry());
    }

    reveal.setMode(eColorGlobalModeC);
    EXPECT_TRUE(reveal.getModeWidget() == reveal.getWheel());
    EXPECT_EQ(QRect(0, 0, reveal.width(), reveal.height()), reveal.getWheel()->geometry());
}

TEST(ColorTrackballReveal, PuckStartsInTheCentreAndMovesWithTheStroke)
{
    const TrackballRates rates;
    const LumaBasis basis = acescg();
    const KnobColorValue start = rgba(1.0, 1.0, 1.0, 1.0);

    for (ColorGlobalModeEnum mode : { eColorGlobalModeS, eColorGlobalModeT, eColorGlobalModeV }) {
        QPointF p = colorTrackballRevealDiscPuck(mode, eColorKnobRoleGain, 0.0, 0.0, rates);

        EXPECT_DOUBLE_EQ(0.0, p.x()) << "mode " << mode;
        EXPECT_DOUBLE_EQ(0.0, p.y()) << "mode " << mode;
    }

    // S: 90 px right is 45 degrees of hue, a quarter of the hue line's half; 250 px up is half
    // the saturation line's half.
    QPointF s = colorTrackballRevealDiscPuck(eColorGlobalModeS, eColorKnobRoleGain, 90.0, -250.0, rates);

    EXPECT_NEAR(0.25, s.x(), 1e-9);
    EXPECT_NEAR(0.5, s.y(), 1e-9);

    // T: 100 px right warms by 50 mired, towards the disc's warm right; 100 px up is 25 magenta.
    QPointF t = colorTrackballRevealDiscPuck(eColorGlobalModeT, eColorKnobRoleGain, 100.0, -100.0, rates);

    EXPECT_NEAR(0.5, t.x(), 1e-9);
    EXPECT_NEAR(0.5, t.y(), 1e-9);

    // V: 30 px right and 30 up is 0.6 stops of the value line's 3, along the diagonal.
    QPointF v = colorTrackballRevealDiscPuck(eColorGlobalModeV, eColorKnobRoleGain, 30.0, -30.0, rates);

    EXPECT_NEAR(0.2 * kInvSqrt2, v.x(), 1e-9);
    EXPECT_NEAR(0.2 * kInvSqrt2, v.y(), 1e-9);

    // Through the reveal, with a real stroke's travel.
    ColorTrackballReveal reveal;
    ColorGlobalModeStroke stroke;

    reveal.setKnob(eColorKnobRoleGain, basis, &identity, rates.wheelRim);
    reveal.setMode(eColorGlobalModeV);
    stroke.begin(start, 0.0);
    reveal.setStroke(start, start, stroke.getTotalDx(), stroke.getTotalDy(), rates);
    EXPECT_DOUBLE_EQ(0.0, reveal.getDisc()->getPuck().x());
    EXPECT_DOUBLE_EQ(0.0, reveal.getDisc()->getPuck().y());

    stroke.addMove(30.0, -30.0, 1.0);

    ChromaEditResult r = stroke.evaluate(eColorGlobalModeV, eColorKnobRoleGain, basis, rates);

    ASSERT_TRUE(r.valid);
    reveal.setStroke(start, r.value, stroke.getTotalDx(), stroke.getTotalDy(), rates);
    EXPECT_NEAR(0.2 * kInvSqrt2, reveal.getDisc()->getPuck().x(), 1e-9);
    EXPECT_NEAR(0.2 * kInvSqrt2, reveal.getDisc()->getPuck().y(), 1e-9);

    // Far past the rim it parks on it.
    stroke.addMove(3000.0, -3000.0, 1.0);
    reveal.setStroke(start, r.value, stroke.getTotalDx(), stroke.getTotalDy(), rates);
    EXPECT_NEAR(1.0, std::hypot(reveal.getDisc()->getPuck().x(), reveal.getDisc()->getPuck().y()), 1e-9);
}

TEST(ColorTrackballReveal, CWheelPuckIsTheStrokesTranslate)
{
    const TrackballRates rates;
    const LumaBasis basis = acescg();
    const KnobColorValue start = rgba(1.0, 0.9, 0.8, 1.0);
    ColorTrackballReveal reveal;

    reveal.setKnob(eColorKnobRoleGain, basis, &identity, rates.wheelRim);
    reveal.setMode(eColorGlobalModeC);

    // Anchored on the start, the puck in the centre until the stroke moves.
    reveal.setStroke(start, start, 0.0, 0.0, rates);
    EXPECT_NEAR(0.0, reveal.getWheel()->getDragOffset().x, 1e-12);
    EXPECT_NEAR(0.0, reveal.getWheel()->getDragOffset().y, 1e-12);
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(start.rgb[i], reveal.getWheel()->getValue().rgb[i]);
    }

    ColorGlobalModeStroke stroke;

    stroke.begin(start, 0.0);
    stroke.addMove(20.0, -10.0, 1.0);

    ChromaEditResult r = stroke.evaluate(eColorGlobalModeC, eColorKnobRoleGain, basis, rates);

    ASSERT_TRUE(r.valid);
    reveal.setStroke(start, r.value, stroke.getTotalDx(), stroke.getTotalDy(), rates);
    EXPECT_NEAR(20.0 * rates.globalTrackballPlanePerPixel, reveal.getWheel()->getDragOffset().x, 1e-6);
    EXPECT_NEAR(10.0 * rates.globalTrackballPlanePerPixel, reveal.getWheel()->getDragOffset().y, 1e-6);
}

TEST(ColorTrackballReveal, TheStrokesValueDoesNotDependOnTheReveal)
{
    const TrackballRates rates;
    const LumaBasis basis = acescg();
    const KnobColorValue start = rgba(0.8, 0.7, 0.6, 1.0);
    ColorTrackballReveal reveal;

    reveal.setKnob(eColorKnobRoleGain, basis, &identity, rates.wheelRim);
    for (ColorGlobalModeEnum mode : { eColorGlobalModeC, eColorGlobalModeS, eColorGlobalModeT, eColorGlobalModeV }) {
        ColorGlobalModeStroke plain;
        ColorGlobalModeStroke shown;

        plain.begin(start, 0.0);
        shown.begin(start, 0.0);
        reveal.setMode(mode);
        for (int step = 0; step < 5; ++step) {
            plain.addMove(7.0, -3.0, 1.0);
            shown.addMove(7.0, -3.0, 1.0);

            ChromaEditResult a = plain.evaluate(mode, eColorKnobRoleGain, basis, rates);
            ChromaEditResult b = shown.evaluate(mode, eColorKnobRoleGain, basis, rates);

            reveal.setStroke(start, b.value, shown.getTotalDx(), shown.getTotalDy(), rates);
            for (int i = 0; i < 3; ++i) {
                EXPECT_EQ(a.value.rgb[i], b.value.rgb[i]) << "mode " << mode << " step " << step;
            }
        }
    }
}

TEST(ColorTrackballRevealFeedback, WhatEachFeedbackShows)
{
    for (F f : { kCrosshairReveal, kWidgetCursor }) {
        for (bool stroke : { false, true }) {
            ColorGlobalModeFeedbackState off = colorGlobalModeFeedbackState(f, false, stroke);

            EXPECT_FALSE(off.crosshair || off.blankCursor || off.graphicShown || off.graphicFollowsPointer);
        }
    }

    ColorGlobalModeFeedbackState revealIdle = colorGlobalModeFeedbackState(kCrosshairReveal, true, false);
    ColorGlobalModeFeedbackState revealStroke = colorGlobalModeFeedbackState(kCrosshairReveal, true, true);

    EXPECT_TRUE(revealIdle.crosshair);
    EXPECT_FALSE(revealIdle.graphicShown || revealIdle.blankCursor);
    EXPECT_TRUE(revealStroke.crosshair && revealStroke.graphicShown && revealStroke.blankCursor);
    EXPECT_FALSE(revealStroke.graphicFollowsPointer);

    ColorGlobalModeFeedbackState widgetIdle = colorGlobalModeFeedbackState(kWidgetCursor, true, false);
    ColorGlobalModeFeedbackState widgetStroke = colorGlobalModeFeedbackState(kWidgetCursor, true, true);

    EXPECT_FALSE(widgetIdle.crosshair);
    EXPECT_TRUE(widgetIdle.graphicShown && widgetIdle.blankCursor && widgetIdle.graphicFollowsPointer);
    EXPECT_TRUE(widgetStroke.graphicShown && widgetStroke.blankCursor);
    EXPECT_FALSE(widgetStroke.graphicFollowsPointer);

    EXPECT_TRUE(colorGlobalModeFeedbackGraphicIsCursor(kWidgetCursor));
    EXPECT_FALSE(colorGlobalModeFeedbackGraphicIsCursor(kCrosshairReveal));
}

TEST(ColorTrackballRevealFeedback, ARevealStrokeHidesTheCursorAndLeavesOnlyTheCrosshair)
{
    const int baseline = overrideCursorDepth();

    {
        ColorGlobalModeCursors cursors;

        syncCursors(cursors, kCrosshairReveal, true, false);
        EXPECT_EQ(baseline + 1, overrideCursorDepth());
        EXPECT_EQ(Qt::CrossCursor, topCursor());

        // Press, then release.
        syncCursors(cursors, kCrosshairReveal, true, true);
        EXPECT_EQ(baseline + 2, overrideCursorDepth());
        EXPECT_EQ(Qt::BlankCursor, topCursor());
        syncCursors(cursors, kCrosshairReveal, true, false);
        EXPECT_EQ(baseline + 1, overrideCursorDepth());
        EXPECT_EQ(Qt::CrossCursor, topCursor());

        // Press, then a lost grab: the same stroke end, reached without a release.
        syncCursors(cursors, kCrosshairReveal, true, true);
        syncCursors(cursors, kCrosshairReveal, true, false);
        EXPECT_EQ(baseline + 1, overrideCursorDepth());
        EXPECT_EQ(Qt::CrossCursor, topCursor());

        // Exit mid-stroke, as a modal dialog taking over does.
        syncCursors(cursors, kCrosshairReveal, true, true);
        syncCursors(cursors, kCrosshairReveal, false, false);
        EXPECT_EQ(baseline, overrideCursorDepth());

        // Destroyed while both are held.
        syncCursors(cursors, kCrosshairReveal, true, true);
    }
    EXPECT_EQ(baseline, overrideCursorDepth());
}

TEST(ColorTrackballRevealFeedback, WidgetCursorIsBlankFromEngageToExit)
{
    const int baseline = overrideCursorDepth();
    ColorGlobalModeCursors cursors;

    syncCursors(cursors, kWidgetCursor, true, false);
    EXPECT_EQ(baseline + 1, overrideCursorDepth());
    EXPECT_EQ(Qt::BlankCursor, topCursor());
    syncCursors(cursors, kWidgetCursor, true, true);
    EXPECT_EQ(baseline + 1, overrideCursorDepth());
    syncCursors(cursors, kWidgetCursor, true, false);
    EXPECT_EQ(baseline + 1, overrideCursorDepth());
    EXPECT_EQ(Qt::BlankCursor, topCursor());

    // Switched to Crosshair + reveal between strokes: the crosshair is all that is left.
    syncCursors(cursors, kCrosshairReveal, true, false);
    EXPECT_EQ(baseline + 1, overrideCursorDepth());
    EXPECT_EQ(Qt::CrossCursor, topCursor());

    // And back: the blank cursor goes on top of nothing but the stack it found.
    syncCursors(cursors, kWidgetCursor, true, false);
    EXPECT_EQ(baseline + 1, overrideCursorDepth());
    EXPECT_EQ(Qt::BlankCursor, topCursor());

    syncCursors(cursors, kWidgetCursor, false, false);
    EXPECT_EQ(baseline, overrideCursorDepth());
}

TEST(ColorTrackballRevealFeedback, OrderIsKeptWhenTheCrosshairChangesUnderTheBlankCursor)
{
    const int baseline = overrideCursorDepth();
    ColorGlobalModeCursors cursors;

    cursors.sync(false, true);
    cursors.sync(true, true);
    EXPECT_EQ(baseline + 2, overrideCursorDepth());
    EXPECT_EQ(Qt::BlankCursor, topCursor());
    cursors.sync(false, true);
    EXPECT_EQ(baseline + 1, overrideCursorDepth());
    EXPECT_EQ(Qt::BlankCursor, topCursor());
    cursors.sync(false, false);
    EXPECT_EQ(baseline, overrideCursorDepth());
}
