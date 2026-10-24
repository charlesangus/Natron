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
#include <memory>

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointF>

#include <gtest/gtest.h>

#include "Engine/ColorWheelMath.h"

#include "Gui/ScaleSliderQWidget.h"

NATRON_NAMESPACE_USING

namespace {

const int kWidgetWidth = 300;
const int kWidgetHeight = 20;

// Builds a widget and gives it a real geometry, then re-asserts its range through the public
// setter so the padded zoom range centerOn() builds is in place before any synthetic event is
// sent -- without that, the range would only get built lazily on the widget's first paint,
// which never happens in this headless suite.
ScaleSliderQWidget*
makeSlider(double minV,
           double maxV,
           double initial,
           ScaleSliderQWidget::DataTypeEnum dataType)
{
    ScaleSliderQWidget* s = new ScaleSliderQWidget(minV, maxV, initial, false, dataType, nullptr);

    s->resize(kWidgetWidth, kWidgetHeight);
    s->show();
    QApplication::processEvents();
    s->setMinimumAndMaximum(minV, maxV);

    return s;
}

void
sendPress(QWidget* w,
          double x,
          Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    const QPointF pos(x, kWidgetHeight / 2.);
    QMouseEvent e(QEvent::MouseButtonPress, pos, w->mapToGlobal(pos), Qt::LeftButton, Qt::LeftButton, mods);

    QApplication::sendEvent(w, &e);
}

void
sendMove(QWidget* w,
         double x,
         Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    const QPointF pos(x, kWidgetHeight / 2.);
    QMouseEvent e(QEvent::MouseMove, pos, w->mapToGlobal(pos), Qt::NoButton, Qt::LeftButton, mods);

    QApplication::sendEvent(w, &e);
}

void
sendRelease(QWidget* w,
            double x,
            Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    const QPointF pos(x, kWidgetHeight / 2.);
    QMouseEvent e(QEvent::MouseButtonRelease, pos, w->mapToGlobal(pos), Qt::LeftButton, Qt::NoButton, mods);

    QApplication::sendEvent(w, &e);
}

ScaleSliderQWidget*
makeVerticalSlider(double minV,
                   double maxV,
                   double initial)
{
    ScaleSliderQWidget* s = new ScaleSliderQWidget(minV, maxV, initial, false, ScaleSliderQWidget::eDataTypeDouble, nullptr);

    s->setOrientation(Qt::Vertical);
    s->resize(kWidgetHeight, kWidgetWidth);
    s->show();
    QApplication::processEvents();
    s->setMinimumAndMaximum(minV, maxV);

    return s;
}

void
sendVerticalMouse(QWidget* w,
                  QEvent::Type type,
                  double y,
                  Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    const QPointF pos(kWidgetHeight / 2., y);
    const bool isMove = type == QEvent::MouseMove;
    const bool isRelease = type == QEvent::MouseButtonRelease;
    QMouseEvent e(type, pos, w->mapToGlobal(pos), isMove ? Qt::NoButton : Qt::LeftButton,
                  isRelease ? Qt::NoButton : Qt::LeftButton, mods);

    QApplication::sendEvent(w, &e);
}

} // namespace

TEST(ScaleSliderQWidget, PlainPressJumpsProportionallyToTheClickedPosition)
{
    std::unique_ptr<ScaleSliderQWidget> s(makeSlider(0., 1., 0., ScaleSliderQWidget::eDataTypeDouble));

    // two calibration presses, kept well away from the ends so neither clamps to
    // [minimum, maximum] and distorts the linear relationship being checked
    const double x1 = kWidgetWidth * 0.1;
    const double x2 = kWidgetWidth * 0.9;

    sendPress(s.get(), x1);
    double v1 = s->getPosition();
    sendRelease(s.get(), x1);

    sendPress(s.get(), x2);
    double v2 = s->getPosition();
    sendRelease(s.get(), x2);

    const double xQuarter = x1 + (x2 - x1) * 0.25;
    sendPress(s.get(), xQuarter);
    double quarter = s->getPosition();
    sendRelease(s.get(), xQuarter);

    EXPECT_NEAR(v1 + (v2 - v1) * 0.25, quarter, 1e-9);
}

TEST(ScaleSliderQWidget, PlainDragMovesByThePixelDeltaOverTheSliderSRange)
{
    std::unique_ptr<ScaleSliderQWidget> s(makeSlider(0., 1., 0., ScaleSliderQWidget::eDataTypeDouble));

    sendPress(s.get(), 50.);
    double start = s->getPosition();
    sendMove(s.get(), 80.); // +30 px
    double end = s->getPosition();
    sendRelease(s.get(), 80.);

    EXPECT_NEAR(30. / kWidgetWidth, end - start, 1. / kWidgetWidth);
}

TEST(ScaleSliderQWidget, CtrlHeldFromThePressAnchorsInsteadOfJumpingAndGearsTheDrag)
{
    std::unique_ptr<ScaleSliderQWidget> s(makeSlider(0., 1., 0.5, ScaleSliderQWidget::eDataTypeDouble));

    // a press far from the current value's own pixel position: a jump would land elsewhere
    sendPress(s.get(), 10., Qt::ControlModifier);
    EXPECT_NEAR(0.5, s->getPosition(), 1e-9);

    sendMove(s.get(), 110., Qt::ControlModifier); // +100 px
    double delta = s->getPosition() - 0.5;
    sendRelease(s.get(), 110., Qt::ControlModifier);

    EXPECT_NEAR(100. / kWidgetWidth * gear(true, false), delta, 1e-9);
}

TEST(ScaleSliderQWidget, ShiftHeldGearsTheDragFaster)
{
    std::unique_ptr<ScaleSliderQWidget> s(makeSlider(0., 1., 0.5, ScaleSliderQWidget::eDataTypeDouble));

    sendPress(s.get(), 290., Qt::ShiftModifier);
    EXPECT_NEAR(0.5, s->getPosition(), 1e-9);

    sendMove(s.get(), 300., Qt::ShiftModifier); // +10 px
    double delta = s->getPosition() - 0.5;
    sendRelease(s.get(), 300., Qt::ShiftModifier);

    EXPECT_NEAR(10. / kWidgetWidth * gear(false, true), delta, 1e-9);
}

TEST(ScaleSliderQWidget, CtrlShiftHeldGearsTheDragSlowest)
{
    std::unique_ptr<ScaleSliderQWidget> s(makeSlider(0., 1., 0.5, ScaleSliderQWidget::eDataTypeDouble));

    sendPress(s.get(), 10., Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_NEAR(0.5, s->getPosition(), 1e-9);

    sendMove(s.get(), 110., Qt::ControlModifier | Qt::ShiftModifier); // +100 px
    double delta = s->getPosition() - 0.5;
    sendRelease(s.get(), 110., Qt::ControlModifier | Qt::ShiftModifier);

    EXPECT_NEAR(100. / kWidgetWidth * gear(true, true), delta, 1e-9);
}

TEST(ScaleSliderQWidget, HoldingCtrlPartwayThroughAPlainDragLeavesTheValueContinuous)
{
    std::unique_ptr<ScaleSliderQWidget> s(makeSlider(0., 1., 0., ScaleSliderQWidget::eDataTypeDouble));

    sendPress(s.get(), 50.);
    sendMove(s.get(), 100.); // +50 px, no modifier
    double atSwitch = s->getPosition();

    // same pixel, modifier now held: zero further motion, so the value must not jump
    sendMove(s.get(), 100., Qt::ControlModifier);
    EXPECT_NEAR(atSwitch, s->getPosition(), 1e-9);

    sendMove(s.get(), 200., Qt::ControlModifier); // +100 px geared
    double delta = s->getPosition() - atSwitch;
    sendRelease(s.get(), 200., Qt::ControlModifier);

    EXPECT_NEAR(100. / kWidgetWidth * gear(true, false), delta, 1e-9);
}

TEST(ScaleSliderQWidget, ClampingAtAnEndRespondsAtOnceWhenTheDragReverses)
{
    std::unique_ptr<ScaleSliderQWidget> s(makeSlider(0., 1., 0., ScaleSliderQWidget::eDataTypeDouble));

    sendPress(s.get(), 0.);
    sendMove(s.get(), kWidgetWidth * 4); // far past the right edge: clamps at the maximum
    ASSERT_NEAR(1., s->getPosition(), 1e-9);

    sendMove(s.get(), kWidgetWidth * 4 - 3); // -3 px
    sendRelease(s.get(), kWidgetWidth * 4 - 3);

    EXPECT_LT(s->getPosition(), 1.);
    EXPECT_NEAR(1. - 3. / kWidgetWidth, s->getPosition(), 1e-9);
}

TEST(ScaleSliderQWidget, AnIntSliderRoundsOnlyTheEmittedValueNotTheRunningAccumulator)
{
    std::unique_ptr<ScaleSliderQWidget> s(makeSlider(0., 100., 0., ScaleSliderQWidget::eDataTypeInt));

    sendPress(s.get(), 150., Qt::ControlModifier);
    ASSERT_NEAR(0., s->getPosition(), 1e-9);

    // six +5 px moves at the Ctrl gear, each worth 100/300 * 0.1 * 5 = 1/6 on its own: rounding
    // every move individually would floor each step to 0 and never move the slider at all.
    for (int i = 0; i < 6; ++i) {
        sendMove(s.get(), 150. + (i + 1) * 5., Qt::ControlModifier);
    }
    double got = s->getPosition();
    sendRelease(s.get(), 180., Qt::ControlModifier);

    EXPECT_NEAR(1., got, 1e-9);
}

TEST(ScaleSliderQWidget, AnIntSliderOverZeroToHundredAtCtrlPlus50PxGivesOneOrTwo)
{
    std::unique_ptr<ScaleSliderQWidget> s(makeSlider(0., 100., 0., ScaleSliderQWidget::eDataTypeInt));

    sendPress(s.get(), 150., Qt::ControlModifier);
    sendMove(s.get(), 200., Qt::ControlModifier); // +50 px
    double got = s->getPosition();
    sendRelease(s.get(), 200., Qt::ControlModifier);

    double raw = 50. / kWidgetWidth * 100. * gear(true, false);

    EXPECT_NEAR(std::floor(raw + 0.5), got, 1e-9);
    EXPECT_GE(got, 1.);
    EXPECT_LE(got, 2.);
}

TEST(ScaleSliderQWidget, AltPressEmitsOnlyTheResetSignal)
{
    std::unique_ptr<ScaleSliderQWidget> s(makeSlider(0., 1., 0.5, ScaleSliderQWidget::eDataTypeDouble));

    int resets = 0;
    int positionChanges = 0;

    QObject::connect(s.get(), &ScaleSliderQWidget::resetToDefaultRequested, [&]() { ++resets; });
    QObject::connect(s.get(), &ScaleSliderQWidget::positionChanged, [&](double) { ++positionChanges; });

    sendPress(s.get(), 10., Qt::AltModifier);
    sendRelease(s.get(), 10., Qt::AltModifier);

    EXPECT_EQ(1, resets);
    EXPECT_EQ(0, positionChanges);
    EXPECT_NEAR(0.5, s->getPosition(), 1e-9);
}

TEST(ScaleSliderQWidget, ReleaseAfterADragStillEmitsEditingFinishedWithHasMovedTrue)
{
    std::unique_ptr<ScaleSliderQWidget> s(makeSlider(0., 1., 0., ScaleSliderQWidget::eDataTypeDouble));

    int finishedCount = 0;
    bool lastHasMoved = false;

    QObject::connect(s.get(), &ScaleSliderQWidget::editingFinished, [&](bool hasMoved) {
        ++finishedCount;
        lastHasMoved = hasMoved;
    });

    sendPress(s.get(), 50.);
    sendMove(s.get(), 80.);
    sendRelease(s.get(), 80.);

    EXPECT_EQ(1, finishedCount);
    EXPECT_TRUE(lastHasMoved);
}

TEST(ScaleSliderQWidget, KeyboardModifiersNoLongerRescaleTheWidgetSTicks)
{
    std::unique_ptr<ScaleSliderQWidget> s(makeSlider(0., 1., 0., ScaleSliderQWidget::eDataTypeDouble));

    double before = s->increment();

    QKeyEvent press(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier);
    QApplication::sendEvent(s.get(), &press);
    EXPECT_NEAR(before, s->increment(), 1e-12);

    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Control, Qt::NoModifier);
    QApplication::sendEvent(s.get(), &release);
    EXPECT_NEAR(before, s->increment(), 1e-12);
}

TEST(ScaleSliderQWidget, VerticalDragUpwardRaisesTheValueByThePixelDeltaOverTheHeight)
{
    std::unique_ptr<ScaleSliderQWidget> s(makeVerticalSlider(0., 1., 0.));

    sendVerticalMouse(s.get(), QEvent::MouseButtonPress, 200.);
    const double start = s->getPosition();
    sendVerticalMouse(s.get(), QEvent::MouseMove, 170.); // 30 px up
    const double end = s->getPosition();
    sendVerticalMouse(s.get(), QEvent::MouseButtonRelease, 170.);

    EXPECT_NEAR(0.1, end - start, 1e-9);
}

TEST(ScaleSliderQWidget, VerticalCtrlDragGearsTheUpwardDrag)
{
    std::unique_ptr<ScaleSliderQWidget> s(makeVerticalSlider(0., 1., 0.5));

    sendVerticalMouse(s.get(), QEvent::MouseButtonPress, 250., Qt::ControlModifier);
    EXPECT_DOUBLE_EQ(0.5, s->getPosition());
    sendVerticalMouse(s.get(), QEvent::MouseMove, 150., Qt::ControlModifier); // 100 px up
    const double end = s->getPosition();
    sendVerticalMouse(s.get(), QEvent::MouseButtonRelease, 150., Qt::ControlModifier);

    EXPECT_NEAR(0.5 + 0.1 / 3., end, 1e-6);
}

TEST(ScaleSliderQWidget, VerticalPressJumpsToTheFractionFromTheBottom)
{
    std::unique_ptr<ScaleSliderQWidget> s(makeVerticalSlider(0., 1., 0.));

    // The track runs from the bottom row (minimum) to the top row (maximum), the convention the
    // marker is drawn with; mouse events carry whole pixels, so row 224 is 75 of the 299 steps up.
    const int row = kWidgetWidth - 1 - 75;
    sendVerticalMouse(s.get(), QEvent::MouseButtonPress, row);
    EXPECT_NEAR(75. / (kWidgetWidth - 1), s->getPosition(), 1e-9);
    sendVerticalMouse(s.get(), QEvent::MouseButtonRelease, row);
}
