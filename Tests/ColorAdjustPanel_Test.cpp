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
#include <vector>

#include <QApplication>
#include <QColor>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPoint>
#include <QPointF>
#include <QString>
#include <QTabWidget>

#include <gtest/gtest.h>

#include "Engine/ColorWheelMath.h"

#include "Gui/ColorAdjustPanel.h"
#include "Gui/ColorEditSlider.h"
#include "Gui/ColorWheelWidget.h"
#include "Gui/SpinBox.h"

NATRON_NAMESPACE_USING

namespace {
const double kAP1Weights[3] = { 0.2722287168, 0.6740817658, 0.0536895174 };
const double kAP1ToXYZ[9] = {
    0.6624541811, 0.1340042065, 0.1561876870,
    0.2722287168, 0.6740817658, 0.0536895174,
    -0.0055746495, 0.0040607335, 1.0103391003
};

const ColorSliderKindEnum kLineKinds[5] = {
    eColorSliderKindHue,
    eColorSliderKindSat,
    eColorSliderKindValue,
    eColorSliderKindTemp,
    eColorSliderKindTint
};

LumaBasis
acescg()
{
    return LumaBasis(kAP1Weights, kAP1ToXYZ);
}

KnobColorValue
makeValue(double r,
          double g,
          double b,
          double a = 1.0,
          bool hasAlpha = true)
{
    KnobColorValue v;

    v.rgb[0] = r;
    v.rgb[1] = g;
    v.rgb[2] = b;
    v.a = a;
    v.hasAlpha = hasAlpha;

    return v;
}

void
identity(float* /*rgb*/,
         int /*count*/)
{
}

void
setUpPanel(ColorAdjustPanel& panel,
           const KnobColorValue& value,
           int nDims = 4,
           ColorKnobRoleEnum role = eColorKnobRoleGain)
{
    panel.setDisplayConverter(&identity);
    panel.setKnobValue(value, nDims, role, acescg());
    panel.resize(720, 280);
    panel.show();
    QApplication::processEvents();
}

struct PanelRecorder {
    int starts = 0;
    int moves = 0;
    int ends = 0;
    KnobColorValue startValue = makeValue(0, 0, 0);
    KnobColorValue lastMove = makeValue(0, 0, 0);
    KnobColorValue endValue = makeValue(0, 0, 0);
    QString endLabel;

    explicit PanelRecorder(ColorAdjustPanel& panel)
    {
        QObject::connect(&panel, &ColorAdjustPanel::valuesEdited, [this](KnobColorValue v, ColorAdjustPanel::GesturePhaseEnum phase, QString label) {
            switch (phase) {
            case ColorAdjustPanel::eGesturePhaseStart:
                // A second Start before the End would split one gesture into two undo steps.
                EXPECT_EQ(starts, ends);
                ++starts;
                startValue = v;
                break;
            case ColorAdjustPanel::eGesturePhaseMove:
                EXPECT_EQ(starts, ends + 1);
                ++moves;
                lastMove = v;
                break;
            case ColorAdjustPanel::eGesturePhaseEnd:
                ++ends;
                endValue = v;
                endLabel = label;
                break;
            }
        });
    }
};

void
sendMouse(QWidget* w,
          QEvent::Type type,
          const QPointF& pos,
          Qt::MouseButton button,
          Qt::MouseButtons buttons,
          Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QMouseEvent e(type, pos, w->mapToGlobal(pos), button, buttons, mods);

    QApplication::sendEvent(w, &e);
}

void
sendKey(QWidget* w,
        Qt::Key key)
{
    QKeyEvent e(QEvent::KeyPress, key, Qt::NoModifier);

    QApplication::sendEvent(w, &e);
}

void
typeInto(SpinBox* box,
         const char* text)
{
    box->QLineEdit::setText(QString::fromUtf8(text));
    sendKey(box, Qt::Key_Return);
}

// Moves by dx in steps and releases, with mods held throughout.
void
customDrag(ColorEditSlider* s,
           double x0,
           double dx,
           Qt::KeyboardModifiers mods = Qt::NoModifier,
           int steps = 4)
{
    double y = s->height() / 2.;

    sendMouse(s, QEvent::MouseButtonPress, QPointF(x0, y), Qt::LeftButton, Qt::LeftButton, mods);
    for (int i = 1; i <= steps; ++i) {
        sendMouse(s, QEvent::MouseMove, QPointF(x0 + dx * i / steps, y), Qt::NoButton, Qt::LeftButton, mods);
    }
    sendMouse(s, QEvent::MouseButtonRelease, QPointF(x0 + dx, y), Qt::LeftButton, Qt::NoButton, mods);
}

void
doubleClick(ColorEditSlider* s)
{
    QPointF p(s->width() / 2., s->height() / 2.);

    sendMouse(s, QEvent::MouseButtonDblClick, p, Qt::LeftButton, Qt::LeftButton);
    sendMouse(s, QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton);
}

double
customMarkerX(const ColorEditSlider* s)
{
    QRectF r = s->trackRect();
    SliderRange range = s->range();

    return r.left() + (s->value() - range.rangeMin) / (range.rangeMax - range.rangeMin) * r.width();
}

// A press near whichever end of the track is farther from the marker, so the drag is relative.
double
offMarkerX(const ColorEditSlider* s)
{
    QRectF r = s->trackRect();

    return (customMarkerX(s) > r.center().x()) ? r.left() + 5. : r.right() - 5.;
}

// A drag of this many pixels from offMarkerX(), pointed back across the track.
double
inwardDrag(const ColorEditSlider* s,
           double pixels)
{
    return (offMarkerX(s) < s->trackRect().center().x()) ? pixels : -pixels;
}

double
customUnitsPerPixel(const ColorEditSlider* s)
{
    SliderRange range = s->range();

    return (range.rangeMax - range.rangeMin) / s->trackRect().width();
}

double
relativeError(double actual,
              double expected)
{
    return std::abs(actual - expected) / std::max(std::abs(expected), 1e-300);
}

double
angleDifference(double a,
                double b)
{
    double d = std::fmod(std::fabs(a - b), 360.0);

    return std::min(d, 360.0 - d);
}

} // namespace

TEST(ColorAdjustPanel, HasNoTabsAndOneLinePerAxis)
{
    ColorAdjustPanel panel;

    setUpPanel(panel, makeValue(1.0, 1.0, 1.0));

    EXPECT_TRUE(panel.findChildren<QTabWidget*>().isEmpty());
    ASSERT_TRUE(panel.getEditArea());
    EXPECT_TRUE(panel.getWheel()->isVisibleTo(panel.getEditArea()));
    for (ColorSliderKindEnum kind : kLineKinds) {
        ASSERT_TRUE(panel.getCustomSlider(kind));
        ASSERT_TRUE(panel.getSpinBox(kind));
        EXPECT_TRUE(panel.getCustomSlider(kind)->isVisibleTo(&panel));
        EXPECT_TRUE(panel.getSpinBox(kind)->isVisibleTo(&panel));
    }
}

TEST(ColorAdjustPanel, OneDimensionalKnobShowsOnlyValue)
{
    ColorAdjustPanel panel;

    setUpPanel(panel, makeValue(0.5, 0.5, 0.5, 0.0, false), 1);

    EXPECT_FALSE(panel.getWheel()->isVisibleTo(&panel));
    for (ColorSliderKindEnum kind : kLineKinds) {
        bool expected = kind == eColorSliderKindValue;

        EXPECT_EQ(expected, panel.getCustomSlider(kind)->isVisibleTo(&panel));
        EXPECT_EQ(expected, panel.getSpinBox(kind)->isVisibleTo(&panel));
    }

    PanelRecorder rec(panel);

    typeInto(panel.getSpinBox(eColorSliderKindValue), "1");
    ASSERT_EQ(1, rec.ends);
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(2.0, rec.endValue.rgb[i], 1e-12);
    }
}

TEST(ColorAdjustPanel, ThreeDimensionalKnobShowsEveryLine)
{
    ColorAdjustPanel panel;

    setUpPanel(panel, makeValue(1.0, 0.9, 0.8, 0.0, false), 3);

    EXPECT_TRUE(panel.getWheel()->isVisibleTo(&panel));
    for (ColorSliderKindEnum kind : kLineKinds) {
        EXPECT_TRUE(panel.getCustomSlider(kind)->isVisibleTo(&panel));
    }
}

TEST(ColorAdjustPanel, HueDragTurnsTheHueAndKeepsLumaAndAlpha)
{
    ColorAdjustPanel panel;
    KnobColorValue start = makeValue(1.5, 1.0, 1.0, 0.9);
    LumaBasis basis = acescg();

    setUpPanel(panel, start);

    PanelRecorder rec(panel);
    ColorEditSlider* hue = panel.getCustomSlider(eColorSliderKindHue);
    double hueBefore = planeHue(toPlane(eColorKnobRoleGain, basis, start));
    double dx = inwardDrag(hue, 40.);

    ASSERT_GT(hue->width(), 150);
    customDrag(hue, offMarkerX(hue), dx);

    ASSERT_EQ(1, rec.starts);
    ASSERT_EQ(1, rec.ends);
    EXPECT_LT(angleDifference(hueBefore + dx * customUnitsPerPixel(hue), planeHue(toPlane(eColorKnobRoleGain, basis, rec.endValue))), 1e-6);
    EXPECT_LT(relativeError(luma(basis, rec.endValue.rgb), luma(basis, start.rgb)), 1e-9);
    EXPECT_EQ(0.9, rec.endValue.a);
}

TEST(ColorAdjustPanel, ValueDragIsOneGestureAndAlphaTracks)
{
    ColorAdjustPanel panel;

    setUpPanel(panel, makeValue(1.2, 1.0, 0.8, 1.0));

    PanelRecorder rec(panel);
    ColorEditSlider* value = panel.getCustomSlider(eColorSliderKindValue);

    customDrag(value, value->trackRect().left() + 5., 60.);

    EXPECT_EQ(1, rec.starts);
    EXPECT_GE(rec.moves, 2);
    EXPECT_EQ(1, rec.ends);
    EXPECT_FALSE(panel.isEditing());
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(rec.lastMove.rgb[i], rec.endValue.rgb[i]);
    }

    double factor = rec.endValue.rgb[1] / 1.0;

    EXPECT_GT(factor, 1.0);
    EXPECT_NEAR(1.2 * factor, rec.endValue.rgb[0], 1e-9);
    EXPECT_NEAR(0.8 * factor, rec.endValue.rgb[2], 1e-9);
    EXPECT_NEAR(factor, rec.endValue.a, 1e-9);
}

TEST(ColorAdjustPanel, TypedTwoStopsOnAGainMultipliesLumaByFour)
{
    ColorAdjustPanel panel;
    LumaBasis basis = acescg();
    KnobColorValue start = makeValue(1.2, 1.0, 0.8, 1.0);
    double y0 = luma(basis, start.rgb);

    setUpPanel(panel, makeValue(1.2 / y0, 1.0 / y0, 0.8 / y0, 1.0));

    PanelRecorder rec(panel);

    typeInto(panel.getSpinBox(eColorSliderKindValue), "2");

    ASSERT_EQ(1, rec.starts);
    ASSERT_EQ(1, rec.ends);
    EXPECT_LT(relativeError(luma(basis, rec.endValue.rgb), 4.0), 1e-9);
    EXPECT_NEAR(4.0, rec.endValue.a, 1e-9);
    EXPECT_NEAR(2.0, panel.getSpinBox(eColorSliderKindValue)->value(), 1e-9);
}

TEST(ColorAdjustPanel, PowerValueLineCountsStopsFromMidGrey)
{
    ColorAdjustPanel panel;

    setUpPanel(panel, makeValue(1.0, 1.0, 1.0, 1.0), 4, eColorKnobRolePower);

    EXPECT_NEAR(0.0, panel.getSpinBox(eColorSliderKindValue)->value(), 1e-9) << "gamma 1 is 0 stops";
    EXPECT_NEAR(0.0, panel.getCustomSlider(eColorSliderKindValue)->value(), 1e-9);

    PanelRecorder rec(panel);

    typeInto(panel.getSpinBox(eColorSliderKindValue), "1");

    ASSERT_EQ(1, rec.ends);
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(effectToGamma(0.36).gamma, rec.endValue.rgb[i], 1e-9);
    }
    EXPECT_NEAR(1.0, panel.getSpinBox(eColorSliderKindValue)->value(), 1e-9);
}

TEST(ColorAdjustPanel, SliderDragMovesThePuckWithoutReanchoringTheWheel)
{
    ColorAdjustPanel panel;
    KnobColorValue start = makeValue(1.5, 1.0, 1.0, 1.0);

    setUpPanel(panel, start);

    PanelRecorder rec(panel);
    ColorWheelWidget* wheel = panel.getWheel();
    ColorEditSlider* hue = panel.getCustomSlider(eColorSliderKindHue);
    double x0 = offMarkerX(hue);
    double x1 = x0 + inwardDrag(hue, 60.);
    double y = hue->height() / 2.;

    sendMouse(hue, QEvent::MouseButtonPress, QPointF(x0, y), Qt::LeftButton, Qt::LeftButton);
    sendMouse(hue, QEvent::MouseMove, QPointF(x1, y), Qt::NoButton, Qt::LeftButton);

    ASSERT_GE(rec.moves, 1);
    EXPECT_TRUE(wheel->hasDragOffset());
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(start.rgb[i], wheel->getValue().rgb[i]) << "the wheel keeps its anchor mid-drag";
    }

    sendMouse(hue, QEvent::MouseButtonRelease, QPointF(x1, y), Qt::LeftButton, Qt::NoButton);

    ASSERT_EQ(1, rec.ends);
    EXPECT_FALSE(wheel->hasDragOffset());
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(rec.endValue.rgb[i], wheel->getValue().rgb[i]);
    }
}

TEST(ColorAdjustPanel, ReturnOnAnUnchangedFieldEditsNothing)
{
    ColorAdjustPanel panel;

    setUpPanel(panel, makeValue(1.234567, 1.0, 0.8, 1.0));

    PanelRecorder rec(panel);

    sendKey(panel.getSpinBox(eColorSliderKindHue), Qt::Key_Return);

    EXPECT_EQ(0, rec.starts);
    EXPECT_EQ(0, rec.ends);
}

TEST(ColorAdjustPanel, TypedHueKeepsLuma)
{
    ColorAdjustPanel panel;
    KnobColorValue start = makeValue(1.5, 1.0, 1.0, 0.9);
    LumaBasis basis = acescg();

    setUpPanel(panel, start);

    PanelRecorder rec(panel);

    typeInto(panel.getSpinBox(eColorSliderKindHue), "200");

    ASSERT_EQ(1, rec.starts);
    ASSERT_EQ(1, rec.ends);
    EXPECT_LT(relativeError(luma(basis, rec.endValue.rgb), luma(basis, start.rgb)), 1e-12);
    EXPECT_NEAR(200.0, planeHue(toPlane(eColorKnobRoleGain, basis, rec.endValue)), 1e-6);
    EXPECT_EQ(0.9, rec.endValue.a);
}

TEST(ColorAdjustPanel, TempDragKeepsLumaAndLeavesAlphaAlone)
{
    ColorAdjustPanel panel;
    KnobColorValue start = makeValue(1.1, 1.0, 0.9, 0.7);
    LumaBasis basis = acescg();

    setUpPanel(panel, start);

    PanelRecorder rec(panel);
    ColorEditSlider* temp = panel.getCustomSlider(eColorSliderKindTemp);
    TempTintResult before = toTempTint(eColorKnobRoleGain, basis, start);
    double dx = inwardDrag(temp, 60.);

    ASSERT_TRUE(before.valid);
    customDrag(temp, offMarkerX(temp), dx, Qt::ControlModifier);

    ASSERT_EQ(1, rec.ends);

    TempTintResult after = toTempTint(eColorKnobRoleGain, basis, rec.endValue);

    ASSERT_TRUE(after.valid);
    ASSERT_LT(std::abs(before.temp), temp->range().rangeMax);
    EXPECT_NEAR(before.temp + dx * customUnitsPerPixel(temp) * gear(true, false), after.temp, 1e-4);
    EXPECT_NEAR(before.tint, after.tint, 1e-4);
    EXPECT_LT(relativeError(luma(basis, rec.endValue.rgb), luma(basis, start.rgb)), 1e-9);
    EXPECT_EQ(0.7, rec.endValue.a);
}

TEST(ColorAdjustPanel, DoubleClickOnTempResetsTemperature)
{
    ColorAdjustPanel panel;
    KnobColorValue start = makeValue(1.1, 1.0, 0.9, 0.7);
    LumaBasis basis = acescg();

    setUpPanel(panel, start);

    TempTintResult before = toTempTint(eColorKnobRoleGain, basis, start);

    ASSERT_TRUE(before.valid);
    ASSERT_GT(std::abs(before.temp), 1.0);

    PanelRecorder rec(panel);
    doubleClick(panel.getCustomSlider(eColorSliderKindTemp));

    ASSERT_EQ(1, rec.starts);
    ASSERT_EQ(1, rec.ends);

    TempTintResult after = toTempTint(eColorKnobRoleGain, basis, rec.endValue);

    ASSERT_TRUE(after.valid);
    EXPECT_NEAR(0.0, after.temp, 1e-4);
    EXPECT_NEAR(before.tint, after.tint, 1e-4);
    EXPECT_EQ(0.7, rec.endValue.a);
}

TEST(ColorAdjustPanel, DoubleClickOnValueResetsGain)
{
    ColorAdjustPanel panel;

    setUpPanel(panel, makeValue(2.0, 2.0, 2.0, 2.0));

    PanelRecorder rec(panel);
    doubleClick(panel.getCustomSlider(eColorSliderKindValue));

    ASSERT_EQ(1, rec.starts);
    ASSERT_EQ(1, rec.ends);
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(1.0, rec.endValue.rgb[i], 1e-12);
    }
    EXPECT_NEAR(1.0, rec.endValue.a, 1e-12);
}

TEST(ColorAdjustPanel, OffsetTempAndTintRangesAreATenth)
{
    ColorAdjustPanel panel;

    setUpPanel(panel, makeValue(0.0, 0.0, 0.0, 0.0), 4, eColorKnobRoleOffset);

    EXPECT_EQ(-0.1, panel.getCustomSlider(eColorSliderKindTemp)->range().rangeMin);
    EXPECT_EQ(0.1, panel.getCustomSlider(eColorSliderKindTemp)->range().rangeMax);
    EXPECT_EQ(-0.1, panel.getCustomSlider(eColorSliderKindTint)->range().rangeMin);
    EXPECT_EQ(0.1, panel.getCustomSlider(eColorSliderKindTint)->range().rangeMax);

    panel.setRole(eColorKnobRoleGain);
    EXPECT_EQ(-100.0, panel.getCustomSlider(eColorSliderKindTemp)->range().rangeMin);
    EXPECT_EQ(100.0, panel.getCustomSlider(eColorSliderKindTemp)->range().rangeMax);
}

TEST(ColorAdjustPanel, SatFromZeroGrowsAlongRememberedHue)
{
    ColorAdjustPanel panel;
    KnobColorValue start = makeValue(1.2, 1.0, 0.8);
    LumaBasis basis = acescg();
    double hue0 = planeHue(toPlane(eColorKnobRoleGain, basis, start));

    setUpPanel(panel, start);
    typeInto(panel.getSpinBox(eColorSliderKindSat), "0");

    PlanePoint neutral = toPlane(eColorKnobRoleGain, basis, panel.getValue());

    ASSERT_TRUE(neutral.valid);
    EXPECT_LT(planeSaturation(neutral), 1e-9);
    EXPECT_NEAR(hue0, panel.getRememberedHue(), 1e-9);

    ColorEditSlider* sat = panel.getCustomSlider(eColorSliderKindSat);

    customDrag(sat, sat->trackRect().right() - 10., 100., Qt::ControlModifier);

    PlanePoint grown = toPlane(eColorKnobRoleGain, basis, panel.getValue());

    ASSERT_TRUE(grown.valid);
    EXPECT_NEAR(100. * customUnitsPerPixel(sat) * gear(true, false), planeSaturation(grown), 1e-7);
    EXPECT_LT(angleDifference(hue0, planeHue(grown)), 0.5);
}

TEST(ColorAdjustPanel, OneWheelDragEmitsOneStartAndOneEndAndRecentres)
{
    ColorAdjustPanel panel;
    KnobColorValue start = makeValue(1.0, 1.0, 1.0, 1.0);
    LumaBasis basis = acescg();

    setUpPanel(panel, start);

    PanelRecorder rec(panel);
    ColorWheelWidget* wheel = panel.getWheel();

    ASSERT_TRUE(wheel);
    ASSERT_GT(wheel->getDiscRadius(), 10.0);

    QPointF c = wheel->getDiscCentre();

    sendMouse(wheel, QEvent::MouseButtonPress, c, Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= 6; ++i) {
        sendMouse(wheel, QEvent::MouseMove, c + QPointF(4.0 * i, -2.0 * i), Qt::NoButton, Qt::LeftButton);
        EXPECT_TRUE(panel.isEditing());
        EXPECT_TRUE(wheel->hasDragOffset());
    }
    sendMouse(wheel, QEvent::MouseButtonRelease, c + QPointF(24.0, -12.0), Qt::LeftButton, Qt::NoButton);

    EXPECT_EQ(1, rec.starts);
    EXPECT_EQ(6, rec.moves);
    EXPECT_EQ(1, rec.ends);
    EXPECT_FALSE(panel.isEditing());
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(start.rgb[i], rec.startValue.rgb[i]);
        EXPECT_EQ(rec.lastMove.rgb[i], rec.endValue.rgb[i]);
        EXPECT_EQ(rec.endValue.rgb[i], wheel->getValue().rgb[i]);
    }
    EXPECT_FALSE(wheel->hasDragOffset());
    EXPECT_GT(planeSaturation(toPlane(eColorKnobRoleGain, basis, rec.endValue)), 0.0);
    EXPECT_LT(relativeError(luma(basis, rec.endValue.rgb), 1.0), 1e-12);
    EXPECT_EQ(1.0, rec.endValue.a);
}

TEST(ColorAdjustPanel, PressWithoutMoveEmitsNothing)
{
    ColorAdjustPanel panel;

    setUpPanel(panel, makeValue(1.0, 1.0, 1.0));

    PanelRecorder rec(panel);
    ColorWheelWidget* wheel = panel.getWheel();
    QPointF c = wheel->getDiscCentre();

    sendMouse(wheel, QEvent::MouseButtonPress, c, Qt::LeftButton, Qt::LeftButton);
    sendMouse(wheel, QEvent::MouseButtonRelease, c, Qt::LeftButton, Qt::NoButton);

    ColorEditSlider* hue = panel.getCustomSlider(eColorSliderKindHue);
    QPointF p(offMarkerX(hue), hue->height() / 2.);

    sendMouse(hue, QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
    sendMouse(hue, QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton, Qt::ControlModifier);

    EXPECT_EQ(0, rec.starts);
    EXPECT_EQ(0, rec.moves);
    EXPECT_EQ(0, rec.ends);
    EXPECT_FALSE(panel.isEditing());
}

TEST(ColorAdjustPanel, KnobEchoDuringAGestureIsIgnored)
{
    ColorAdjustPanel panel;

    setUpPanel(panel, makeValue(1.0, 1.0, 1.0));

    ColorWheelWidget* wheel = panel.getWheel();
    QPointF c = wheel->getDiscCentre();

    QObject::connect(&panel, &ColorAdjustPanel::valuesEdited, [&panel](KnobColorValue /*v*/, ColorAdjustPanel::GesturePhaseEnum phase, QString /*label*/) {
        if (phase == ColorAdjustPanel::eGesturePhaseMove) {
            panel.setKnobValue(makeValue(5.0, 5.0, 5.0));
        }
    });
    sendMouse(wheel, QEvent::MouseButtonPress, c, Qt::LeftButton, Qt::LeftButton);
    sendMouse(wheel, QEvent::MouseMove, c + QPointF(10.0, 0.0), Qt::NoButton, Qt::LeftButton);
    EXPECT_TRUE(wheel->hasDragOffset());
    sendMouse(wheel, QEvent::MouseMove, c + QPointF(20.0, 0.0), Qt::NoButton, Qt::LeftButton);
    sendMouse(wheel, QEvent::MouseButtonRelease, c + QPointF(20.0, 0.0), Qt::LeftButton, Qt::NoButton);

    EXPECT_LT(relativeError(luma(acescg(), panel.getValue().rgb), 1.0), 1e-12);
}

TEST(ColorAdjustPanel, SliderLinesFollowTheValue)
{
    ColorAdjustPanel panel;

    setUpPanel(panel, makeValue(1.0, 1.0, 1.0));
    panel.setKnobValue(makeValue(2.0, 2.0, 2.0));

    EXPECT_NEAR(1.0, panel.getCustomSlider(eColorSliderKindValue)->value(), 1e-9);
    EXPECT_NEAR(1.0, panel.getSpinBox(eColorSliderKindValue)->value(), 1e-9);
}

TEST(ColorAdjustPanel, SliderGradientsFollowTheCurrentColour)
{
    ColorAdjustPanel panel;

    setUpPanel(panel, makeValue(1.0, 1.0, 1.0));

    QGradientStops valueStops = panel.getCustomSlider(eColorSliderKindValue)->gradientStops();

    ASSERT_GE(valueStops.size(), 2);
    EXPECT_LT(valueStops.first().second.lightness() + 40, valueStops.last().second.lightness());

    panel.setKnobValue(makeValue(1.05, 1.0, 0.95));

    QGradientStops satStops = panel.getCustomSlider(eColorSliderKindSat)->gradientStops();

    ASSERT_GE(satStops.size(), 2);

    QColor grey = satStops.first().second;
    QColor tinted = satStops.last().second;

    EXPECT_LE(std::abs(grey.red() - grey.blue()), 4);
    EXPECT_GT(tinted.red() - tinted.blue(), 3);
}

TEST(ColorAdjustPanel, PairsColorEditSlidersWithNatronSpinBoxes)
{
    ColorAdjustPanel panel;

    setUpPanel(panel, makeValue(1.2, 1.0, 0.8));
    for (ColorSliderKindEnum kind : kLineKinds) {
        ColorEditSlider* s = panel.getCustomSlider(kind);

        ASSERT_TRUE(s);
        EXPECT_TRUE(panel.getSpinBox(kind));
        EXPECT_EQ(kind, s->kind());
        EXPECT_EQ(eColorKnobRoleGain, s->role());
        EXPECT_FALSE(s->gradientStops().isEmpty());
        EXPECT_TRUE(s->isVisible());
    }
}

TEST(ColorAdjustPanel, MarkerPressDragIsRelativeOneUndoStepAndTheSpinBoxFollows)
{
    ColorAdjustPanel panel;
    LumaBasis basis = acescg();

    setUpPanel(panel, makeValue(1.0, 1.0, 1.0));

    PanelRecorder rec(panel);
    ColorEditSlider* value = panel.getCustomSlider(eColorSliderKindValue);

    ASSERT_TRUE(value);

    double before = value->value();
    double expected = before + 40. * customUnitsPerPixel(value) * gear(false, false);

    customDrag(value, customMarkerX(value), 40.);

    EXPECT_EQ(1, rec.starts);
    EXPECT_GE(rec.moves, 1);
    EXPECT_EQ(1, rec.ends);
    EXPECT_FALSE(panel.isEditing());
    EXPECT_EQ(QString::fromUtf8("value slider (relative)"), rec.endLabel);
    EXPECT_NEAR(expected, std::log2(luma(basis, rec.endValue.rgb)), 1e-9);
    EXPECT_NEAR(expected, panel.getSpinBox(eColorSliderKindValue)->value(), 1e-3);
    EXPECT_NEAR(expected, value->value(), 1e-9);
}

TEST(ColorAdjustPanel, RelativeCtrlDragMovesATenthAndIsOneUndoStep)
{
    ColorAdjustPanel panel;
    LumaBasis basis = acescg();

    setUpPanel(panel, makeValue(1.0, 1.0, 1.0));

    PanelRecorder rec(panel);
    ColorEditSlider* value = panel.getCustomSlider(eColorSliderKindValue);

    ASSERT_TRUE(value);

    // Far left of the marker, which sits mid-track at no gain: a relative drag.
    customDrag(value, value->trackRect().left() + 5., 80., Qt::ControlModifier);

    ASSERT_EQ(1, rec.starts);
    EXPECT_EQ(1, rec.ends);
    EXPECT_EQ(QString::fromUtf8("value slider (relative)"), rec.endLabel);

    double expected = 8. * customUnitsPerPixel(value);

    EXPECT_NEAR(expected, std::log2(luma(basis, rec.endValue.rgb)), 1e-9);
    EXPECT_NEAR(expected, panel.getSpinBox(eColorSliderKindValue)->value(), 1e-3);
}
