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
#include <cstddef>
#include <vector>

#include <QCursor>
#include <QGuiApplication>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QRegion>
#include <QScreen>
#include <QStringList>

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
discCentre(const ColorTrackballReveal& reveal)
{
    return reveal.pos() + reveal.getDiscCentre();
}

// The distance from p to the nearest point of r.
double
distanceToRect(const QPointF& p,
               const QRectF& r)
{
    const double x = std::max(r.left(), std::min(p.x(), r.right()));
    const double y = std::max(r.top(), std::min(p.y(), r.bottom()));

    return std::hypot(p.x() - x, p.y() - y);
}

ColorTrackballHudState
hudState(ColorGlobalModeEnum mode)
{
    ColorTrackballHudState state;

    state.knobLabel = QString::fromUtf8("Gain");
    state.mode = mode;

    return state;
}

const ColorTrackballReveal::PillSideEnum kSides[4] = {
    ColorTrackballReveal::ePillRight, ColorTrackballReveal::ePillLeft, ColorTrackballReveal::ePillTop, ColorTrackballReveal::ePillBottom
};
} // namespace

TEST(ColorTrackballReveal, WindowIsAClickThroughToolWindowHoldingTheDiscAndTheBlock)
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

    for (ColorGlobalModeEnum mode : { eColorGlobalModeC, eColorGlobalModeS, eColorGlobalModeT, eColorGlobalModeV }) {
        reveal.setMode(mode);
        reveal.setHudState(hudState(mode));

        const QRect all(QPoint(0, 0), reveal.size());

        EXPECT_EQ(QSize(ColorTrackballReveal::diameter(), ColorTrackballReveal::diameter()), reveal.getDiscRect().size()) << (int)mode;
        EXPECT_TRUE(all.contains(reveal.getDiscRect())) << (int)mode;
        EXPECT_TRUE(all.contains(reveal.getHudBlock()->geometry())) << (int)mode;
        EXPECT_GT(reveal.height(), ColorTrackballReveal::diameter()) << (int)mode;
        for (ColorTrackballReveal::PillSideEnum side : kSides) {
            if (!reveal.getPill(side)->isHidden()) {
                EXPECT_TRUE(all.contains(reveal.getPill(side)->geometry())) << (int)mode << " " << (int)side;
            }
        }
    }
}

TEST(ColorTrackballReveal, IsMaskedToItsDiscPillsAndBlock)
{
    ColorTrackballReveal reveal;

    reveal.setMode(eColorGlobalModeS);
    reveal.setHudState(hudState(eColorGlobalModeS));

    const QRegion mask = reveal.mask();
    const QRect disc = reveal.getDiscRect();
    const QRect block = reveal.getHudBlock()->geometry();

    EXPECT_FALSE(mask.isEmpty());
    EXPECT_FALSE(mask.contains(QPoint(0, 0)));
    EXPECT_FALSE(mask.contains(disc.topLeft() + QPoint(2, 2)));
    EXPECT_FALSE(mask.contains(QPoint(disc.right() - 2, disc.top() + 2)));
    EXPECT_TRUE(mask.contains(reveal.getDiscCentre()));
    EXPECT_TRUE(mask.contains(block.center()));
    for (ColorTrackballReveal::PillSideEnum side : kSides) {
        EXPECT_TRUE(mask.contains(reveal.getPill(side)->geometry().center())) << (int)side;
    }

    // In C the pills are gone, and so is their part of the mask.
    reveal.setMode(eColorGlobalModeC);
    EXPECT_FALSE(reveal.mask().contains(reveal.getPill(ColorTrackballReveal::ePillRight)->geometry().center()));
    EXPECT_TRUE(reveal.mask().contains(reveal.getHudBlock()->geometry().center()));
}

TEST(ColorTrackballReveal, DirectionLabelsPerMode)
{
    const QString minus(QChar(0x2212));
    ColorTrackballRevealLabels s = colorTrackballRevealDirectionLabels(eColorGlobalModeS);
    ColorTrackballRevealLabels t = colorTrackballRevealDirectionLabels(eColorGlobalModeT);
    ColorTrackballRevealLabels v = colorTrackballRevealDirectionLabels(eColorGlobalModeV);
    ColorTrackballRevealLabels c = colorTrackballRevealDirectionLabels(eColorGlobalModeC);

    EXPECT_EQ(QString::fromUtf8("+Hue"), s.right);
    EXPECT_EQ(minus + QString::fromUtf8("Hue"), s.left);
    EXPECT_EQ(QString::fromUtf8("+Sat"), s.top);
    EXPECT_EQ(minus + QString::fromUtf8("Sat"), s.bottom);
    EXPECT_EQ(QString::fromUtf8("Warmer"), t.right);
    EXPECT_EQ(QString::fromUtf8("Cooler"), t.left);
    EXPECT_EQ(QString::fromUtf8("Less green"), t.top);
    EXPECT_EQ(QString::fromUtf8("Greener"), t.bottom);
    EXPECT_EQ(QString::fromUtf8("+Val"), v.right);
    EXPECT_EQ(QString::fromUtf8("+Val"), v.top);
    EXPECT_EQ(minus + QString::fromUtf8("Val"), v.left);
    EXPECT_EQ(minus + QString::fromUtf8("Val"), v.bottom);
    EXPECT_TRUE(c.right.isEmpty() && c.left.isEmpty() && c.top.isEmpty() && c.bottom.isEmpty());
}

TEST(ColorTrackballReveal, PillsSitOnTheirSideOutsideTheRingAndTheBlockUnderneath)
{
    ColorTrackballReveal reveal;

    for (ColorGlobalModeEnum mode : { eColorGlobalModeS, eColorGlobalModeT, eColorGlobalModeV }) {
        reveal.setMode(mode);
        reveal.setHudState(hudState(mode));

        const ColorTrackballRevealLabels labels = colorTrackballRevealDirectionLabels(mode);
        const QPointF c = reveal.getDiscCentre();
        const double Ro = colorWheelRingOuterRadius(reveal.getDiscRect().size());
        const QRect right = reveal.getPill(ColorTrackballReveal::ePillRight)->geometry();
        const QRect left = reveal.getPill(ColorTrackballReveal::ePillLeft)->geometry();
        const QRect top = reveal.getPill(ColorTrackballReveal::ePillTop)->geometry();
        const QRect bottom = reveal.getPill(ColorTrackballReveal::ePillBottom)->geometry();

        EXPECT_EQ(labels.right, reveal.getPill(ColorTrackballReveal::ePillRight)->text()) << (int)mode;
        EXPECT_EQ(labels.left, reveal.getPill(ColorTrackballReveal::ePillLeft)->text()) << (int)mode;
        EXPECT_EQ(labels.top, reveal.getPill(ColorTrackballReveal::ePillTop)->text()) << (int)mode;
        EXPECT_EQ(labels.bottom, reveal.getPill(ColorTrackballReveal::ePillBottom)->text()) << (int)mode;

        // Each is on its own side, and none reaches the ring.
        EXPECT_GT(right.left(), c.x() + Ro) << (int)mode;
        EXPECT_LT(left.right(), c.x() - Ro) << (int)mode;
        EXPECT_LT(top.bottom(), c.y() - Ro) << (int)mode;
        EXPECT_GT(bottom.top(), c.y() + Ro) << (int)mode;
        EXPECT_NEAR(c.y(), right.center().y(), 1.0) << (int)mode;
        EXPECT_NEAR(c.y(), left.center().y(), 1.0) << (int)mode;
        EXPECT_NEAR(c.x(), top.center().x(), 1.0) << (int)mode;
        EXPECT_NEAR(c.x(), bottom.center().x(), 1.0) << (int)mode;
        for (ColorTrackballReveal::PillSideEnum side : kSides) {
            const QRect r = reveal.getPill(side)->geometry();

            EXPECT_FALSE(reveal.getPill(side)->isHidden()) << (int)mode << " " << (int)side;
            EXPECT_GE(distanceToRect(c, QRectF(r)), Ro + 4.0) << (int)mode << " " << (int)side;
        }

        const QRect block = reveal.getHudBlock()->geometry();

        EXPECT_GT(block.top(), bottom.bottom()) << (int)mode;
        EXPECT_NEAR(c.x(), block.center().x(), 1.0) << (int)mode;
        for (ColorTrackballReveal::PillSideEnum side : kSides) {
            EXPECT_FALSE(block.intersects(reveal.getPill(side)->geometry())) << (int)mode << " " << (int)side;
        }
    }

    // C has no pills; the block sits right under the ring.
    reveal.setMode(eColorGlobalModeC);
    for (ColorTrackballReveal::PillSideEnum side : kSides) {
        EXPECT_TRUE(reveal.getPill(side)->isHidden()) << (int)side;
    }
    EXPECT_GT(reveal.getHudBlock()->geometry().top(), reveal.getDiscCentre().y() + colorWheelRingOuterRadius(reveal.getDiscRect().size()));
}

TEST(ColorTrackballReveal, TheBlockShowsTheKnobNameAndTheActiveMode)
{
    ColorTrackballReveal reveal;
    ColorTrackballHudState state = hudState(eColorGlobalModeT);

    state.knobLabel = QString::fromUtf8("Lift");
    reveal.setMode(eColorGlobalModeT);
    reveal.setHudState(state);

    const QStringList lines = reveal.getHudBlock()->lines();

    ASSERT_FALSE(lines.isEmpty());
    EXPECT_EQ(QString::fromUtf8("Lift"), lines[0]);
    EXPECT_EQ(eColorGlobalModeT, reveal.getHudBlock()->litKey());
    // The knob alone: no node name, no position in the knob list.
    EXPECT_FALSE(lines[0].contains(QString::fromUtf8(" - ")));
    EXPECT_FALSE(lines[0].contains(QString::fromUtf8("/")));
}

TEST(ColorTrackballReveal, PositionPutsTheDiscOnTheCentreAndClampsTheWindow)
{
    const QRect screen(0, 0, 1920, 1080);
    const QSize size(240, 240);
    const QPoint half(120, 120);

    EXPECT_EQ(QPoint(380, 280), colorTrackballRevealPosition(QPoint(500, 400), half, size, screen));
    EXPECT_EQ(QPoint(0, 0), colorTrackballRevealPosition(QPoint(3, 4), half, size, screen));
    EXPECT_EQ(QPoint(1680, 840), colorTrackballRevealPosition(QPoint(1915, 1075), half, size, screen));

    // A second screen to the right of the first.
    const QRect right(1920, 0, 1280, 1024);

    EXPECT_EQ(QPoint(1920, 904 - 120), colorTrackballRevealPosition(QPoint(1925, 904), half, size, right));
    EXPECT_EQ(QPoint(380, 280), colorTrackballRevealPosition(QPoint(500, 400), half, size, QRect()));

    // A window taller than its disc, with the disc at its top: the disc goes on the centre.
    EXPECT_EQ(QPoint(400, 280), colorTrackballRevealPosition(QPoint(500, 400), QPoint(100, 120), QSize(320, 400), screen));
    EXPECT_EQ(QPoint(400, 1080 - 400), colorTrackballRevealPosition(QPoint(500, 1000), QPoint(100, 120), QSize(320, 400), screen));
}

TEST(ColorTrackballReveal, APressCentresTheDiscAndACornerKeepsTheWindowInside)
{
    QScreen* screen = QGuiApplication::primaryScreen();

    ASSERT_TRUE(screen != 0);

    const QRect g = screen->geometry();
    ColorTrackballReveal reveal;

    reveal.setMode(eColorGlobalModeS);
    reveal.setHudState(hudState(eColorGlobalModeS));
    ASSERT_GE(g.width(), reveal.width() + 80);
    ASSERT_GE(g.height(), reveal.height() + 80);

    const QPoint press = g.topLeft() + reveal.getDiscCentre() + QPoint(40, 40);

    reveal.centreOn(press);
    EXPECT_EQ(press, discCentre(reveal));

    // Following the pointer is the same placement, repeated on every move.
    const QPoint moved = press + QPoint(-37, 22);

    reveal.centreOn(moved);
    EXPECT_EQ(moved, discCentre(reveal));

    reveal.centreOn(g.topLeft());
    EXPECT_EQ(g.topLeft(), reveal.pos());
    reveal.centreOn(QPoint(g.left() + g.width() - 1, g.top() + g.height() - 1));
    EXPECT_EQ(g.left() + g.width(), reveal.geometry().right() + 1);
    EXPECT_EQ(g.top() + g.height(), reveal.geometry().bottom() + 1);
}

TEST(ColorTrackballReveal, SetModeSwapsTheWidgetInPlace)
{
    ColorTrackballReveal reveal;
    const QPoint press(500, 400);

    reveal.setHudState(hudState(eColorGlobalModeC));
    reveal.centreOn(press);
    reveal.setMode(eColorGlobalModeC);
    EXPECT_TRUE(reveal.getModeWidget() == reveal.getWheel());
    EXPECT_FALSE(reveal.getWheel()->isHidden());
    EXPECT_TRUE(reveal.getDisc()->isHidden());

    const QPoint placed = discCentre(reveal);

    for (ColorGlobalModeEnum mode : { eColorGlobalModeS, eColorGlobalModeT, eColorGlobalModeV }) {
        reveal.setMode(mode);
        reveal.setHudState(hudState(mode));
        EXPECT_TRUE(reveal.getModeWidget() == reveal.getDisc());
        EXPECT_EQ(mode, reveal.getDisc()->getMode());
        EXPECT_FALSE(reveal.getDisc()->isHidden());
        EXPECT_TRUE(reveal.getWheel()->isHidden());
        EXPECT_EQ(reveal.getDiscRect(), reveal.getDisc()->geometry());
        // The window grows for the pills, but the disc stays where it was on screen.
        EXPECT_EQ(placed, discCentre(reveal)) << (int)mode;
    }

    reveal.setMode(eColorGlobalModeC);
    EXPECT_TRUE(reveal.getModeWidget() == reveal.getWheel());
    EXPECT_EQ(reveal.getDiscRect(), reveal.getWheel()->geometry());
    EXPECT_EQ(placed, discCentre(reveal));
}

TEST(ColorTrackballReveal, ReleaseReturnsThePointerToThePressAndTheRevealStaysPut)
{
    QScreen* screen = QGuiApplication::primaryScreen();

    ASSERT_TRUE(screen != 0);

    const QRect g = screen->geometry();
    const QPoint press = g.topLeft() + QPoint(400, 300);
    const QPoint dragEnd = press + QPoint(150, -90);
    ColorTrackballReveal reveal;

    reveal.setMode(eColorGlobalModeS);
    reveal.setHudState(hudState(eColorGlobalModeS));
    reveal.centreOn(press);

    const QPoint placed = reveal.pos();

    QCursor::setPos(dragEnd);
    ASSERT_EQ(dragEnd, QCursor::pos());

    ASSERT_TRUE(colorGlobalModeReturnPointer(press));
    EXPECT_EQ(press, QCursor::pos());
    // A following graphic is re-placed on the returned pointer, which is where it already is.
    reveal.centreOn(QCursor::pos());
    EXPECT_EQ(placed, reveal.pos());
    EXPECT_EQ(press, discCentre(reveal));
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

            EXPECT_FALSE(off.crosshair || off.blankCursor || off.graphicShown || off.graphicFollowsPointer || off.hudBlockFollowsPointer);
        }
    }

    ColorGlobalModeFeedbackState revealIdle = colorGlobalModeFeedbackState(kCrosshairReveal, true, false);
    ColorGlobalModeFeedbackState revealStroke = colorGlobalModeFeedbackState(kCrosshairReveal, true, true);

    EXPECT_TRUE(revealIdle.crosshair);
    EXPECT_FALSE(revealIdle.graphicShown || revealIdle.blankCursor);
    EXPECT_TRUE(revealStroke.crosshair && revealStroke.graphicShown && revealStroke.blankCursor);
    EXPECT_FALSE(revealStroke.graphicFollowsPointer);
    // With no disc up the hint block has its own window by the pointer; a stroke's disc carries it.
    EXPECT_TRUE(revealIdle.hudBlockFollowsPointer);
    EXPECT_FALSE(revealStroke.hudBlockFollowsPointer);

    ColorGlobalModeFeedbackState widgetIdle = colorGlobalModeFeedbackState(kWidgetCursor, true, false);
    ColorGlobalModeFeedbackState widgetStroke = colorGlobalModeFeedbackState(kWidgetCursor, true, true);

    EXPECT_FALSE(widgetIdle.crosshair);
    EXPECT_TRUE(widgetIdle.graphicShown && widgetIdle.blankCursor && widgetIdle.graphicFollowsPointer);
    EXPECT_TRUE(widgetStroke.graphicShown && widgetStroke.blankCursor);
    EXPECT_FALSE(widgetStroke.graphicFollowsPointer);
    EXPECT_FALSE(widgetIdle.hudBlockFollowsPointer || widgetStroke.hudBlockFollowsPointer);

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
