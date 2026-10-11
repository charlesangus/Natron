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

#include <memory>

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QString>

#include <gtest/gtest.h>

#include "Engine/ColorWheelMath.h"

#include "Gui/ColorEditSlider.h"
#include "Gui/LineEdit.h"

NATRON_NAMESPACE_USING

namespace {

// The widget's own layout carves a font-sized LineEdit plus a 6px gap and 4px of padding on
// each side off a resize() width; the field's width depends on the font, so the track width
// below is read back from the widget rather than assumed, and every test is built on that.
const int kWidgetWidth = 220;
const int kWidgetHeight = 20;
const double kTrackLeft = 4.;

ColorEditSlider*
makeSlider(ColorSliderKindEnum kind,
           ColorKnobRoleEnum role)
{
    ColorEditSlider* s = new ColorEditSlider(kind);

    s->setRole(role);
    s->resize(kWidgetWidth, kWidgetHeight);

    return s;
}

double
trackWidthOf(const ColorEditSlider* s)
{
    return kWidgetWidth - s->valueLineEdit()->width() - 6. - 2. * kTrackLeft;
}

double
markerX(const ColorEditSlider* s,
        const SliderRange& range,
        double value)
{
    return kTrackLeft + valueToMarker(range, value) * trackWidthOf(s);
}

void
sendPress(QWidget* w,
          double x,
          Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QMouseEvent e(QEvent::MouseButtonPress, QPointF(x, kWidgetHeight / 2.), Qt::LeftButton, Qt::LeftButton, mods);

    QApplication::sendEvent(w, &e);
}

void
sendMove(QWidget* w,
         double x,
         Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QMouseEvent e(QEvent::MouseMove, QPointF(x, kWidgetHeight / 2.), Qt::NoButton, Qt::LeftButton, mods);

    QApplication::sendEvent(w, &e);
}

void
sendRelease(QWidget* w,
            double x,
            Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QMouseEvent e(QEvent::MouseButtonRelease, QPointF(x, kWidgetHeight / 2.), Qt::LeftButton, Qt::NoButton, mods);

    QApplication::sendEvent(w, &e);
}

void
sendDoubleClick(QWidget* w,
                double x)
{
    QMouseEvent e(QEvent::MouseButtonDblClick, QPointF(x, kWidgetHeight / 2.), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);

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
    bool lastAbsolute = false;
    QString lastLabel;
    bool lastMoved = false;

    explicit GestureSpy(ColorEditSlider* s)
    {
        QObject::connect(s, &ColorEditSlider::gestureStarted, [this]() {
            ++started;
        });
        QObject::connect(s, &ColorEditSlider::gestureMoved, [this](double value, bool absolute, QString label) {
            ++moved;
            lastValue = value;
            lastAbsolute = absolute;
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
        double span = range.rangeMax - range.rangeMin;

        s->setValue(0.);
        GestureSpy spy(s.get());
        double mx = markerX(s.get(), range, 0.);

        sendPress(s.get(), mx);
        sendMove(s.get(), mx + 40., mod);
        sendRelease(s.get(), mx + 40., mod);

        double expected = 40. * span / trackWidthOf(s.get());
        EXPECT_NEAR(expected, s->value(), 1e-9);
        EXPECT_EQ(1, spy.started);
        EXPECT_EQ(1, spy.finished);
        EXPECT_TRUE(spy.lastAbsolute);
        EXPECT_EQ(QString::fromUtf8("value slider (absolute)"), spy.lastLabel);
    }
}

TEST(ColorEditSlider, RelativeDragWithCtrlMovesOneTenthAsFar)
{
    TrackballRates rates;
    std::unique_ptr<ColorEditSlider> s(makeSlider(eColorSliderKindValue, eColorKnobRoleGain));
    SliderRange range = sliderMapping(eColorSliderKindValue, eColorKnobRoleGain, rates);
    double span = range.rangeMax - range.rangeMin;

    s->setValue(0.);
    GestureSpy spy(s.get());

    // Pressed well off the marker (which sits mid-track at value 0), so this is a relative drag.
    double pressX = kTrackLeft + 5.;

    sendPress(s.get(), pressX);
    sendMove(s.get(), pressX + 80., Qt::ControlModifier);
    sendRelease(s.get(), pressX + 80., Qt::ControlModifier);

    double expected = 8. * span / trackWidthOf(s.get()); // 80px at gear 0.1 == 8px of range
    EXPECT_NEAR(expected, s->value(), 1e-9);
    EXPECT_FALSE(spy.lastAbsolute);
    EXPECT_EQ(QString::fromUtf8("value slider (relative)"), spy.lastLabel);
}

TEST(ColorEditSlider, PressWithNoMoveEmitsNothing)
{
    std::unique_ptr<ColorEditSlider> s(makeSlider(eColorSliderKindHue, eColorKnobRoleGain));
    GestureSpy spy(s.get());
    double x = kTrackLeft + 10.;

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
    // the marker, so a press there is a relative drag, not an absolute jump back onto the rim.
    double pastEnd = range.rangeMax + 1.;
    s->setValue(pastEnd);
    GestureSpy spy(s.get());
    double chevronX = markerX(s.get(), range, pastEnd); // the (clamped) pixel the chevron is drawn at

    sendPress(s.get(), chevronX);
    sendMove(s.get(), chevronX + 6.);

    ASSERT_EQ(1, spy.moved);
    EXPECT_FALSE(spy.lastAbsolute);
    // An absolute drag landing exactly on the rim would clamp to rangeMax; a relative drag
    // instead keeps climbing past it.
    EXPECT_GT(s->value(), range.rangeMax);

    sendRelease(s.get(), chevronX + 6.);
}

TEST(ColorEditSlider, DoubleClickEmitsResetAndNothingElse)
{
    std::unique_ptr<ColorEditSlider> s(makeSlider(eColorSliderKindSat, eColorKnobRoleGain));
    GestureSpy spy(s.get());

    sendDoubleClick(s.get(), kTrackLeft + 20.);

    EXPECT_EQ(1, spy.resets);
    EXPECT_EQ(0, spy.started);
    EXPECT_EQ(0, spy.moved);
    EXPECT_EQ(0, spy.finished);
}

TEST(ColorEditSlider, EscapeDuringADragEndsItInPlaceInsteadOfCancelling)
{
    std::unique_ptr<ColorEditSlider> s(makeSlider(eColorSliderKindValue, eColorKnobRoleGain));
    SliderRange range = sliderMapping(eColorSliderKindValue, eColorKnobRoleGain, TrackballRates());

    s->setValue(0.);
    GestureSpy spy(s.get());
    double pressX = kTrackLeft + 5.; // off the marker: a relative drag

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

TEST(ColorEditSlider, TypedFieldCommitsAsOneCompleteGesture)
{
    std::unique_ptr<ColorEditSlider> s(makeSlider(eColorSliderKindValue, eColorKnobRoleGain));

    s->setValue(0.);
    GestureSpy spy(s.get());
    LineEdit* edit = s->valueLineEdit();

    edit->setText(QString::fromUtf8("2.5"));
    sendKey(edit, Qt::Key_Return);

    EXPECT_EQ(1, spy.started);
    EXPECT_EQ(1, spy.moved);
    EXPECT_EQ(1, spy.finished);
    EXPECT_TRUE(spy.lastAbsolute);
    EXPECT_NEAR(2.5, spy.lastValue, 1e-9);
    EXPECT_NEAR(2.5, s->value(), 1e-9);
    EXPECT_EQ(QString::fromUtf8("typed value"), spy.lastLabel);
}

TEST(ColorEditSlider, EscapeInTheTypedFieldRevertsTextWithoutEmitting)
{
    std::unique_ptr<ColorEditSlider> s(makeSlider(eColorSliderKindValue, eColorKnobRoleGain));

    s->setValue(1.);
    GestureSpy spy(s.get());
    LineEdit* edit = s->valueLineEdit();

    edit->setText(QString::fromUtf8("garbage"));
    sendKey(edit, Qt::Key_Escape);

    EXPECT_EQ(QString::fromUtf8("1.00"), edit->text());
    EXPECT_NEAR(1., s->value(), 1e-9);
    EXPECT_EQ(0, spy.total());
}

TEST(ColorEditSlider, ProgrammaticSetValueNeverEmits)
{
    std::unique_ptr<ColorEditSlider> s(makeSlider(eColorSliderKindAlpha, eColorKnobRoleGain));
    GestureSpy spy(s.get());

    s->setValue(1.5);
    s->setValue(2.75);
    s->setValue(2.75); // no-op: same value twice

    EXPECT_NEAR(2.75, s->value(), 1e-9);
    EXPECT_EQ(0, spy.total());
}

TEST(ColorEditSlider, RangeFollowsKindAndRole)
{
    std::unique_ptr<ColorEditSlider> s(new ColorEditSlider(eColorSliderKindValue));

    s->setRole(eColorKnobRoleGain);
    SliderRange ratioRange = s->range();
    EXPECT_NEAR(-3., ratioRange.rangeMin, 1e-9);
    EXPECT_NEAR(3., ratioRange.rangeMax, 1e-9);

    s->setRole(eColorKnobRoleOffset);
    SliderRange offsetRange = s->range();
    EXPECT_NEAR(-0.25, offsetRange.rangeMin, 1e-9);
    EXPECT_NEAR(0.25, offsetRange.rangeMax, 1e-9);

    EXPECT_EQ(eColorSliderKindValue, s->kind());
}
