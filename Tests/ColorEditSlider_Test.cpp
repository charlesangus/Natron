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
#include <memory>

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QString>

#include <gtest/gtest.h>

#include "Engine/ColorWheelMath.h"

#include "Gui/ColorEditSlider.h"

NATRON_NAMESPACE_USING

namespace {

const int kWidgetWidth = 220;
const int kWidgetHeight = 20;

ColorEditSlider*
makeSlider(ColorSliderKindEnum kind,
           ColorKnobRoleEnum role)
{
    ColorEditSlider* s = new ColorEditSlider(kind);

    s->setRole(role);
    s->resize(kWidgetWidth, kWidgetHeight);

    return s;
}

// The track's extent is read back from the widget, so these hold whatever its padding is.
double
markerX(const ColorEditSlider* s,
        const SliderRange& range,
        double value)
{
    QRectF r = s->trackRect();
    double t = (value - range.rangeMin) / (range.rangeMax - range.rangeMin);

    return r.left() + std::min(1., std::max(0., t)) * r.width();
}

double
unitsPerPixel(const ColorEditSlider* s,
              const SliderRange& range)
{
    return (range.rangeMax - range.rangeMin) / s->trackRect().width();
}

void
sendPress(QWidget* w,
          double x,
          Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QMouseEvent e(QEvent::MouseButtonPress, QPointF(x, kWidgetHeight / 2.), w->mapToGlobal(QPointF(x, kWidgetHeight / 2.)), Qt::LeftButton, Qt::LeftButton, mods);

    QApplication::sendEvent(w, &e);
}

void
sendMove(QWidget* w,
         double x,
         Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QMouseEvent e(QEvent::MouseMove, QPointF(x, kWidgetHeight / 2.), w->mapToGlobal(QPointF(x, kWidgetHeight / 2.)), Qt::NoButton, Qt::LeftButton, mods);

    QApplication::sendEvent(w, &e);
}

void
sendRelease(QWidget* w,
            double x,
            Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QMouseEvent e(QEvent::MouseButtonRelease, QPointF(x, kWidgetHeight / 2.), w->mapToGlobal(QPointF(x, kWidgetHeight / 2.)), Qt::LeftButton, Qt::NoButton, mods);

    QApplication::sendEvent(w, &e);
}

void
sendDoubleClick(QWidget* w,
                double x)
{
    QMouseEvent e(QEvent::MouseButtonDblClick, QPointF(x, kWidgetHeight / 2.), w->mapToGlobal(QPointF(x, kWidgetHeight / 2.)), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);

    QApplication::sendEvent(w, &e);
}

void
sendKey(QWidget* w,
        Qt::Key key)
{
    QKeyEvent e(QEvent::KeyPress, key, Qt::NoModifier);

    QApplication::sendEvent(w, &e);
}

// Counts every gesture signal so a test can assert "nothing happened" in one line.
struct GestureSpy {
    int started = 0;
    int moved = 0;
    int finished = 0;
    int resets = 0;
    double lastValue = 0.;
    QString lastLabel;
    bool lastMoved = false;

    explicit GestureSpy(ColorEditSlider* s)
    {
        QObject::connect(s, &ColorEditSlider::gestureStarted, [this]() {
            ++started;
        });
        QObject::connect(s, &ColorEditSlider::gestureMoved, [this](double value, QString label) {
            ++moved;
            lastValue = value;
            lastLabel = label;
        });
        QObject::connect(s, &ColorEditSlider::gestureFinished, [this](bool movedFlag) {
            ++finished;
            lastMoved = movedFlag;
        });
        QObject::connect(s, &ColorEditSlider::resetRequested, [this]() {
            ++resets;
        });
    }

    int total() const
    {
        return started + moved + finished + resets;
    }
};

} // namespace

TEST(ColorEditSlider, MarkerDragMovesByExactlyThatManyPixelsOfRangeRegardlessOfModifiers)
{
    TrackballRates rates;
    const Qt::KeyboardModifiers mods[] = { Qt::NoModifier, Qt::ControlModifier, Qt::ShiftModifier, Qt::ControlModifier | Qt::ShiftModifier };

    for (Qt::KeyboardModifiers mod : mods) {
        std::unique_ptr<ColorEditSlider> s(makeSlider(eColorSliderKindValue, eColorKnobRoleGain));
        SliderRange range = sliderMapping(eColorSliderKindValue, eColorKnobRoleGain, rates);

        s->setValue(0.);
        GestureSpy spy(s.get());
        double mx = markerX(s.get(), range, 0.);

        sendPress(s.get(), mx);
        sendMove(s.get(), mx + 40., mod);
        sendRelease(s.get(), mx + 40., mod);

        EXPECT_NEAR(40. * unitsPerPixel(s.get(), range) * gear(mod.testFlag(Qt::ControlModifier), mod.testFlag(Qt::ShiftModifier)), s->value(), 1e-9);
        EXPECT_EQ(1, spy.started);
        EXPECT_EQ(1, spy.finished);
        EXPECT_EQ(QString::fromUtf8("value slider (relative)"), spy.lastLabel);
    }
}

TEST(ColorEditSlider, PressOnMarkerDragsByTheSameDeltaAsDraggingFromElsewhere)
{
    TrackballRates rates;
    std::unique_ptr<ColorEditSlider> onMarker(makeSlider(eColorSliderKindValue, eColorKnobRoleGain));
    std::unique_ptr<ColorEditSlider> elsewhere(makeSlider(eColorSliderKindValue, eColorKnobRoleGain));
    SliderRange range = sliderMapping(eColorSliderKindValue, eColorKnobRoleGain, rates);

    onMarker->setValue(0.);
    elsewhere->setValue(0.);

    double mx = markerX(onMarker.get(), range, 0.);
    double other = onMarker->trackRect().left() + 5.;

    sendPress(onMarker.get(), mx);
    sendMove(onMarker.get(), mx + 40.);
    sendRelease(onMarker.get(), mx + 40.);
    sendPress(elsewhere.get(), other);
    sendMove(elsewhere.get(), other + 40.);
    sendRelease(elsewhere.get(), other + 40.);

    EXPECT_NEAR(40. * unitsPerPixel(onMarker.get(), range), onMarker->value(), 1e-9);
    EXPECT_NEAR(elsewhere->value(), onMarker->value(), 1e-12);
}

TEST(ColorEditSlider, ClickOnMarkerWithoutMovingLeavesTheValueUnchanged)
{
    TrackballRates rates;
    std::unique_ptr<ColorEditSlider> s(makeSlider(eColorSliderKindValue, eColorKnobRoleGain));
    SliderRange range = sliderMapping(eColorSliderKindValue, eColorKnobRoleGain, rates);

    s->setValue(0.5);
    GestureSpy spy(s.get());
    double mx = markerX(s.get(), range, 0.5);

    sendPress(s.get(), mx);
    sendRelease(s.get(), mx);

    EXPECT_EQ(0.5, s->value());
    EXPECT_EQ(0, spy.total());
}

TEST(ColorEditSlider, RelativeDragWithCtrlMovesOneTenthAsFar)
{
    TrackballRates rates;
    std::unique_ptr<ColorEditSlider> s(makeSlider(eColorSliderKindValue, eColorKnobRoleGain));
    SliderRange range = sliderMapping(eColorSliderKindValue, eColorKnobRoleGain, rates);

    s->setValue(0.);
    GestureSpy spy(s.get());

    // Pressed well off the marker (which sits mid-track at value 0), so this is a relative drag.
    double pressX = s->trackRect().left() + 5.;

    sendPress(s.get(), pressX);
    sendMove(s.get(), pressX + 80., Qt::ControlModifier);
    sendRelease(s.get(), pressX + 80., Qt::ControlModifier);

    // 80 px at Ctrl's gear is 8 px of range.
    EXPECT_NEAR(8. * unitsPerPixel(s.get(), range), s->value(), 1e-9);
    EXPECT_NEAR(80. * unitsPerPixel(s.get(), range) * gear(true, false), s->value(), 1e-9);
    EXPECT_EQ(QString::fromUtf8("value slider (relative)"), spy.lastLabel);
}

TEST(ColorEditSlider, GearIsReadFromEachMove)
{
    TrackballRates rates;
    std::unique_ptr<ColorEditSlider> s(makeSlider(eColorSliderKindValue, eColorKnobRoleGain));
    SliderRange range = sliderMapping(eColorSliderKindValue, eColorKnobRoleGain, rates);
    double ppx = unitsPerPixel(s.get(), range);

    s->setValue(0.);
    double pressX = s->trackRect().left() + 5.;

    sendPress(s.get(), pressX);
    sendMove(s.get(), pressX + 20.);
    sendMove(s.get(), pressX + 40., Qt::ControlModifier);
    sendMove(s.get(), pressX + 50., Qt::ShiftModifier);
    sendRelease(s.get(), pressX + 50., Qt::ShiftModifier);

    double expected = ppx * (20. * gear(false, false) + 20. * gear(true, false) + 10. * gear(false, true));

    EXPECT_NEAR(expected, s->value(), 1e-9);
}

TEST(ColorEditSlider, PressWithNoMoveEmitsNothing)
{
    std::unique_ptr<ColorEditSlider> s(makeSlider(eColorSliderKindHue, eColorKnobRoleGain));
    GestureSpy spy(s.get());
    double x = s->trackRect().left() + 10.;

    sendPress(s.get(), x);
    sendRelease(s.get(), x);

    EXPECT_EQ(0, spy.total());
}

TEST(ColorEditSlider, PressingPastEitherEndDragsRelativelyAndDoesNotSnap)
{
    TrackballRates rates;
    std::unique_ptr<ColorEditSlider> s(makeSlider(eColorSliderKindValue, eColorKnobRoleGain));
    SliderRange range = sliderMapping(eColorSliderKindValue, eColorKnobRoleGain, rates);

    // Past the high end: the marker itself paints clamped to the rim, but the chevron is not
    // the marker, so a press there drags from the unclamped value instead of jumping back onto the rim.
    double pastEnd = range.rangeMax + 1.;
    s->setValue(pastEnd);
    GestureSpy spy(s.get());
    double chevronX = markerX(s.get(), range, pastEnd);

    sendPress(s.get(), chevronX);
    sendMove(s.get(), chevronX + 6.);

    ASSERT_EQ(1, spy.moved);
    EXPECT_GT(s->value(), range.rangeMax);

    sendRelease(s.get(), chevronX + 6.);
}

TEST(ColorEditSlider, DoubleClickEmitsResetAndNothingElse)
{
    std::unique_ptr<ColorEditSlider> s(makeSlider(eColorSliderKindSat, eColorKnobRoleGain));
    GestureSpy spy(s.get());

    sendDoubleClick(s.get(), s->trackRect().left() + 20.);

    EXPECT_EQ(1, spy.resets);
    EXPECT_EQ(0, spy.started);
    EXPECT_EQ(0, spy.moved);
    EXPECT_EQ(0, spy.finished);
}

TEST(ColorEditSlider, EscapeDuringADragEndsItInPlaceInsteadOfCancelling)
{
    std::unique_ptr<ColorEditSlider> s(makeSlider(eColorSliderKindValue, eColorKnobRoleGain));

    s->setValue(0.);
    GestureSpy spy(s.get());
    double pressX = s->trackRect().left() + 5.; // off the marker: a relative drag

    sendPress(s.get(), pressX);
    sendMove(s.get(), pressX + 30.);
    double valueBeforeEscape = s->value();

    sendKey(s.get(), Qt::Key_Escape);
    EXPECT_EQ(1, spy.finished);
    EXPECT_TRUE(spy.lastMoved);
    EXPECT_NEAR(valueBeforeEscape, s->value(), 1e-9);

    // The drag is already over: a release that follows (as the real mouse eventually sends
    // one) must not finish a second gesture.
    sendRelease(s.get(), pressX + 30.);
    EXPECT_EQ(1, spy.finished);
}

TEST(ColorEditSlider, ProgrammaticSetValueNeverEmits)
{
    std::unique_ptr<ColorEditSlider> s(makeSlider(eColorSliderKindTint, eColorKnobRoleGain));
    GestureSpy spy(s.get());

    s->setValue(1.5);
    s->setValue(2.75);
    s->setValue(2.75);

    EXPECT_NEAR(2.75, s->value(), 1e-9);
    EXPECT_EQ(0, spy.total());
}

TEST(ColorEditSlider, RangeFollowsKindAndRole)
{
    TrackballRates rates;
    std::unique_ptr<ColorEditSlider> s(new ColorEditSlider(eColorSliderKindValue));

    for (ColorKnobRoleEnum role : { eColorKnobRoleGain, eColorKnobRoleOffset, eColorKnobRolePower }) {
        s->setRole(role);
        SliderRange expected = sliderMapping(eColorSliderKindValue, role, rates);

        EXPECT_EQ(expected.rangeMin, s->range().rangeMin);
        EXPECT_EQ(expected.rangeMax, s->range().rangeMax);
    }
    EXPECT_EQ(eColorSliderKindValue, s->kind());
}
