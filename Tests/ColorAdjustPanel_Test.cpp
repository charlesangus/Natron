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
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPixmap>
#include <QPoint>
#include <QPointF>
#include <QPointer>
#include <QString>
#include <QTabWidget>

#include <gtest/gtest.h>

#include "Engine/ColorWheelMath.h"

#include "Gui/ColorAdjustPanel.h"
#include "Gui/ColorEditSlider.h"
#include "Gui/ColorPanelLayoutDev.h"
#include "Gui/ColorWheelWidget.h"
#include "Gui/ScaleSliderQWidget.h"
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

// The slider only builds its pixel-to-value mapping on its first paint, which never happens in
// this headless suite, so the range is re-asserted once the sliders have their real widths.
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
    for (ColorSliderKindEnum kind : kLineKinds) {
        ScaleSliderQWidget* s = panel.getSlider(kind);

        if (s) {
            s->setMinimumAndMaximum(s->minimum(), s->maximum());
        }
    }
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

// Moves right by dx in steps and releases, with mods held throughout. With Ctrl or Shift held
// the press anchors at the slider's current value instead of jumping to x0.
void
sliderDrag(ScaleSliderQWidget* s,
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

double
unitsPerPixel(const ScaleSliderQWidget* s)
{
    return (s->maximum() - s->minimum()) / s->width();
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

// Ticks are painted over the track gradient, so each end is read over a few pixels and the
// sample least like a tick is kept.
QColor
darkestNear(const QImage& image,
            int x,
            int y)
{
    QColor best = image.pixelColor(x, y);

    for (int dx = -3; dx <= 3; ++dx) {
        QColor c = image.pixelColor(x + dx, y);

        if (c.lightness() < best.lightness()) {
            best = c;
        }
    }

    return best;
}

// Sets or clears NATRON_COLOR_PANEL_DEV for one test and leaves the switch at its default,
// with the variable unset, for the cases that follow in this process.
struct DevSwitchScope {
    explicit DevSwitchScope(bool enabled)
    {
        if (enabled) {
            qputenv("NATRON_COLOR_PANEL_DEV", "1");
        } else {
            qunsetenv("NATRON_COLOR_PANEL_DEV");
        }
        ColorPanelLayoutDev::reloadForTests();
    }

    ~DevSwitchScope()
    {
        ColorPanelLayoutDev::instance()->setLayout(ColorPanelLayout());
        qunsetenv("NATRON_COLOR_PANEL_DEV");
        ColorPanelLayoutDev::reloadForTests();
    }
};

struct LayoutCounter {
    int changed = 0;
    int applied = 0;

    explicit LayoutCounter(ColorAdjustPanel& panel)
    {
        QObject::connect(ColorPanelLayoutDev::instance(), &ColorPanelLayoutDev::layoutChanged, &panel, [this]() {
            ++changed;
        });
        QObject::connect(&panel, &ColorAdjustPanel::layoutApplied, [this]() {
            ++applied;
        });
    }
};

QColor
brightestNear(const QImage& image,
              int x,
              int y)
{
    QColor best = image.pixelColor(x, y);

    for (int dx = -3; dx <= 3; ++dx) {
        QColor c = image.pixelColor(x + dx, y);

        if (c.lightness() > best.lightness()) {
            best = c;
        }
    }

    return best;
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
        ASSERT_TRUE(panel.getSlider(kind));
        ASSERT_TRUE(panel.getSpinBox(kind));
        EXPECT_TRUE(panel.getSlider(kind)->isVisibleTo(&panel));
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

        EXPECT_EQ(expected, panel.getSlider(kind)->isVisibleTo(&panel));
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
        EXPECT_TRUE(panel.getSlider(kind)->isVisibleTo(&panel));
    }
}

TEST(ColorAdjustPanel, PressAtTheMiddleOfTheHueTrackSetsHalfTurnAndKeepsLuma)
{
    ColorAdjustPanel panel;
    KnobColorValue start = makeValue(1.5, 1.0, 1.0, 0.9);
    LumaBasis basis = acescg();

    setUpPanel(panel, start);

    PanelRecorder rec(panel);
    ScaleSliderQWidget* hue = panel.getSlider(eColorSliderKindHue);

    ASSERT_GT(hue->width(), 150);

    double x = hue->width() / 2.;
    double y = hue->height() / 2.;

    sendMouse(hue, QEvent::MouseButtonPress, QPointF(x, y), Qt::LeftButton, Qt::LeftButton);
    sendMouse(hue, QEvent::MouseButtonRelease, QPointF(x, y), Qt::LeftButton, Qt::NoButton);

    ASSERT_EQ(1, rec.starts);
    ASSERT_EQ(1, rec.ends);
    EXPECT_LT(angleDifference(180.0, planeHue(toPlane(eColorKnobRoleGain, basis, rec.endValue))), 2.0);
    EXPECT_LT(relativeError(luma(basis, rec.endValue.rgb), luma(basis, start.rgb)), 1e-9);
    EXPECT_EQ(0.9, rec.endValue.a);
}

TEST(ColorAdjustPanel, ValueDragIsOneGestureAndAlphaTracks)
{
    ColorAdjustPanel panel;

    setUpPanel(panel, makeValue(1.2, 1.0, 0.8, 1.0));

    PanelRecorder rec(panel);
    ScaleSliderQWidget* value = panel.getSlider(eColorSliderKindValue);

    sliderDrag(value, value->width() / 2., 60.);

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

TEST(ColorAdjustPanel, CtrlDragOnValueMovesAtATenthOfThePlainRate)
{
    ColorAdjustPanel panel;
    LumaBasis basis = acescg();

    setUpPanel(panel, makeValue(1.0, 1.0, 1.0));

    PanelRecorder rec(panel);
    ScaleSliderQWidget* value = panel.getSlider(eColorSliderKindValue);

    // Far from the marker: a Ctrl press anchors at the current value rather than jumping here.
    sliderDrag(value, 10., 100., Qt::ControlModifier);

    ASSERT_EQ(1, rec.ends);
    EXPECT_NEAR(100. * unitsPerPixel(value) * gear(true, false), std::log2(luma(basis, rec.endValue.rgb)), 1e-9);

    PanelRecorder plain(panel);
    double before = std::log2(luma(basis, panel.getValue().rgb));
    double x0 = value->width() / 2.;
    double y = value->height() / 2.;

    sendMouse(value, QEvent::MouseButtonPress, QPointF(x0, y), Qt::LeftButton, Qt::LeftButton);
    double atPress = std::log2(luma(basis, panel.getValue().rgb));
    sendMouse(value, QEvent::MouseMove, QPointF(x0 + 40., y), Qt::NoButton, Qt::LeftButton);
    double afterMove = std::log2(luma(basis, panel.getValue().rgb));
    sendMouse(value, QEvent::MouseButtonRelease, QPointF(x0 + 40., y), Qt::LeftButton, Qt::NoButton);

    EXPECT_NE(before, atPress);
    EXPECT_NEAR(40. * unitsPerPixel(value), afterMove - atPress, 1e-9);
    EXPECT_EQ(1, plain.starts);
    EXPECT_EQ(1, plain.ends);
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
    EXPECT_NEAR(0.0, panel.getSlider(eColorSliderKindValue)->getPosition(), 1e-9);

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
    ScaleSliderQWidget* hue = panel.getSlider(eColorSliderKindHue);
    double y = hue->height() / 2.;

    sendMouse(hue, QEvent::MouseButtonPress, QPointF(20., y), Qt::LeftButton, Qt::LeftButton);
    sendMouse(hue, QEvent::MouseMove, QPointF(80., y), Qt::NoButton, Qt::LeftButton);

    ASSERT_GE(rec.moves, 1);
    EXPECT_TRUE(wheel->hasDragOffset());
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(start.rgb[i], wheel->getValue().rgb[i]) << "the wheel keeps its anchor mid-drag";
    }

    sendMouse(hue, QEvent::MouseButtonRelease, QPointF(80., y), Qt::LeftButton, Qt::NoButton);

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
    ScaleSliderQWidget* temp = panel.getSlider(eColorSliderKindTemp);
    TempTintResult before = toTempTint(eColorKnobRoleGain, basis, start);

    ASSERT_TRUE(before.valid);
    sliderDrag(temp, 10., 60., Qt::ControlModifier);

    ASSERT_EQ(1, rec.ends);

    TempTintResult after = toTempTint(eColorKnobRoleGain, basis, rec.endValue);

    ASSERT_TRUE(after.valid);
    ASSERT_LT(std::abs(before.temp), temp->maximum());
    EXPECT_NEAR(before.temp + 60. * unitsPerPixel(temp) * gear(true, false), after.temp, 1e-4);
    EXPECT_NEAR(before.tint, after.tint, 1e-4);
    EXPECT_LT(relativeError(luma(basis, rec.endValue.rgb), luma(basis, start.rgb)), 1e-9);
    EXPECT_EQ(0.7, rec.endValue.a);
}

TEST(ColorAdjustPanel, AltClickOnTempResetsTemperature)
{
    ColorAdjustPanel panel;
    KnobColorValue start = makeValue(1.1, 1.0, 0.9, 0.7);
    LumaBasis basis = acescg();

    setUpPanel(panel, start);

    TempTintResult before = toTempTint(eColorKnobRoleGain, basis, start);

    ASSERT_TRUE(before.valid);
    ASSERT_GT(std::abs(before.temp), 1.0);

    PanelRecorder rec(panel);
    ScaleSliderQWidget* temp = panel.getSlider(eColorSliderKindTemp);
    QPointF p(temp->width() * 0.8, temp->height() / 2.);

    sendMouse(temp, QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton, Qt::AltModifier);
    sendMouse(temp, QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton, Qt::AltModifier);

    ASSERT_EQ(1, rec.starts);
    ASSERT_EQ(1, rec.ends);

    TempTintResult after = toTempTint(eColorKnobRoleGain, basis, rec.endValue);

    ASSERT_TRUE(after.valid);
    EXPECT_NEAR(0.0, after.temp, 1e-4);
    EXPECT_NEAR(before.tint, after.tint, 1e-4);
    EXPECT_EQ(0.7, rec.endValue.a);
}

TEST(ColorAdjustPanel, AltClickOnValueResetsGain)
{
    ColorAdjustPanel panel;

    setUpPanel(panel, makeValue(2.0, 2.0, 2.0, 2.0));

    PanelRecorder rec(panel);
    ScaleSliderQWidget* value = panel.getSlider(eColorSliderKindValue);
    QPointF p(10., value->height() / 2.);

    sendMouse(value, QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton, Qt::AltModifier);
    sendMouse(value, QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton, Qt::AltModifier);

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

    EXPECT_EQ(-0.1, panel.getSlider(eColorSliderKindTemp)->minimum());
    EXPECT_EQ(0.1, panel.getSlider(eColorSliderKindTemp)->maximum());
    EXPECT_EQ(-0.1, panel.getSlider(eColorSliderKindTint)->minimum());
    EXPECT_EQ(0.1, panel.getSlider(eColorSliderKindTint)->maximum());

    panel.setRole(eColorKnobRoleGain);
    EXPECT_EQ(-100.0, panel.getSlider(eColorSliderKindTemp)->minimum());
    EXPECT_EQ(100.0, panel.getSlider(eColorSliderKindTemp)->maximum());
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

    ScaleSliderQWidget* sat = panel.getSlider(eColorSliderKindSat);

    sliderDrag(sat, sat->width() - 10., 100., Qt::ControlModifier);

    PlanePoint grown = toPlane(eColorKnobRoleGain, basis, panel.getValue());

    ASSERT_TRUE(grown.valid);
    EXPECT_NEAR(100. * unitsPerPixel(sat) * gear(true, false), planeSaturation(grown), 1e-7);
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

    ScaleSliderQWidget* hue = panel.getSlider(eColorSliderKindHue);
    QPointF p(10., hue->height() / 2.);

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

    EXPECT_NEAR(1.0, panel.getSlider(eColorSliderKindValue)->getPosition(), 1e-9);
    EXPECT_NEAR(1.0, panel.getSpinBox(eColorSliderKindValue)->value(), 1e-9);
}

TEST(ColorAdjustPanel, SliderGradientsFollowTheCurrentColour)
{
    ColorAdjustPanel panel;

    setUpPanel(panel, makeValue(1.0, 1.0, 1.0));

    ScaleSliderQWidget* value = panel.getSlider(eColorSliderKindValue);
    QImage valueTrack = value->grab().toImage();
    int left = (int)(valueTrack.width() * 0.09);
    int right = (int)(valueTrack.width() * 0.91);
    int bandRow = -1;

    for (int y = 0; y < valueTrack.height(); ++y) {
        if (darkestNear(valueTrack, left, y).lightness() + 40 < brightestNear(valueTrack, right, y).lightness()) {
            bandRow = y;
            break;
        }
    }
    ASSERT_GE(bandRow, 0);

    panel.setKnobValue(makeValue(1.05, 1.0, 0.95));

    QImage satTrack = panel.getSlider(eColorSliderKindSat)->grab().toImage();
    int greyestSpread = 255;
    int mostTinted = -255;

    for (int dx = -3; dx <= 3; ++dx) {
        QColor grey = satTrack.pixelColor(left + dx, bandRow);
        QColor tinted = satTrack.pixelColor(right + dx, bandRow);

        greyestSpread = std::min(greyestSpread, std::abs(grey.red() - grey.blue()));
        mostTinted = std::max(mostTinted, tinted.red() - tinted.blue());
    }
    EXPECT_LE(greyestSpread, 4);
    EXPECT_GT(mostTinted, 3);
}

TEST(ColorAdjustPanel, NoDevBarWithoutTheVariable)
{
    DevSwitchScope dev(false);
    ColorAdjustPanel panel;

    setUpPanel(panel, makeValue(1.0, 1.0, 1.0));

    EXPECT_FALSE(ColorPanelLayoutDev::isDevEnabled());
    EXPECT_EQ(nullptr, panel.getDevBar());
    EXPECT_EQ(nullptr, panel.findChild<QWidget*>(QString::fromUtf8("ColorPanelDevBar")));
    EXPECT_EQ(nullptr, panel.findChild<QComboBox*>());
    EXPECT_TRUE(panel.getLayout() == ColorPanelLayout());
}

TEST(ColorAdjustPanel, DevComboChangeEmitsOnceAndRebuildsEveryPanel)
{
    DevSwitchScope dev(true);
    ColorAdjustPanel panel;
    ColorAdjustPanel other;
    KnobColorValue start = makeValue(1.2, 1.0, 0.8);

    setUpPanel(panel, start);
    setUpPanel(other, start);
    panel.setRightInset(30);
    panel.getEditArea()->setEnabled(false);

    QWidget* bar = panel.getDevBar();

    ASSERT_TRUE(bar);
    ASSERT_TRUE(other.getDevBar());

    QComboBox* style = bar->findChild<QComboBox*>(QString::fromUtf8("ColorPanelDevStyle"));
    QComboBox* otherStyle = other.getDevBar()->findChild<QComboBox*>(QString::fromUtf8("ColorPanelDevStyle"));

    ASSERT_TRUE(style);
    ASSERT_TRUE(otherStyle);

    LayoutCounter counter(panel);
    LayoutCounter otherCounter(other);
    QPointer<QWidget> oldArea = panel.getEditArea();
    QPointer<ColorWheelWidget> oldWheel = panel.getWheel();
    double valueBefore = panel.getSpinBox(eColorSliderKindValue)->value();

    style->setCurrentIndex(style->findData((int)ColorPanelLayout::SliderStyle::Custom));

    EXPECT_EQ(1, counter.changed);
    EXPECT_EQ(1, counter.applied);
    EXPECT_EQ(1, otherCounter.applied);
    EXPECT_TRUE(panel.getLayout().style == ColorPanelLayout::SliderStyle::Custom);
    EXPECT_TRUE(other.getLayout().style == ColorPanelLayout::SliderStyle::Custom);
    EXPECT_NE(oldArea.data(), panel.getEditArea());
    EXPECT_NE(oldWheel.data(), panel.getWheel());
    EXPECT_EQ(otherStyle->findData((int)ColorPanelLayout::SliderStyle::Custom), otherStyle->currentIndex());

    // What the owner set on the old edit area carries over to the new one.
    EXPECT_FALSE(panel.getEditArea()->isEnabled());
    EXPECT_EQ(30, panel.getRightInset());
    EXPECT_EQ(valueBefore, panel.getSpinBox(eColorSliderKindValue)->value());
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(start.rgb[i], panel.getWheel()->getValue().rgb[i]);
    }

    // The old widgets no longer reach the panel.
    PanelRecorder rec(panel);

    if (oldWheel) {
        QPointF c = oldWheel->getDiscCentre();

        sendMouse(oldWheel, QEvent::MouseButtonPress, c, Qt::LeftButton, Qt::LeftButton);
        sendMouse(oldWheel, QEvent::MouseMove, c + QPointF(10., 0.), Qt::NoButton, Qt::LeftButton);
        sendMouse(oldWheel, QEvent::MouseButtonRelease, c + QPointF(10., 0.), Qt::LeftButton, Qt::NoButton);
    }
    EXPECT_EQ(0, rec.starts);

    // Setting the same value again changes nothing.
    style->setCurrentIndex(style->findData((int)ColorPanelLayout::SliderStyle::Custom));
    ColorPanelLayoutDev::instance()->setSliderStyle(ColorPanelLayout::SliderStyle::Custom);
    EXPECT_EQ(1, counter.changed);
    EXPECT_EQ(1, counter.applied);
}

TEST(ColorAdjustPanel, DevToggleMidGestureRebuildsOnlyAfterTheGestureEnds)
{
    DevSwitchScope dev(true);
    ColorAdjustPanel panel;

    setUpPanel(panel, makeValue(1.0, 1.0, 1.0));

    LayoutCounter counter(panel);
    PanelRecorder rec(panel);
    ColorWheelWidget* wheel = panel.getWheel();
    QWidget* area = panel.getEditArea();
    QPointF c = wheel->getDiscCentre();

    sendMouse(wheel, QEvent::MouseButtonPress, c, Qt::LeftButton, Qt::LeftButton);
    sendMouse(wheel, QEvent::MouseMove, c + QPointF(8., -4.), Qt::NoButton, Qt::LeftButton);
    ASSERT_TRUE(panel.isEditing());

    QCheckBox* right = panel.getDevBar()->findChild<QCheckBox*>(QString::fromUtf8("ColorPanelDevExtendRight"));

    ASSERT_TRUE(right);
    right->setChecked(true);

    EXPECT_EQ(1, counter.changed);
    EXPECT_EQ(0, counter.applied);
    EXPECT_EQ(area, panel.getEditArea());
    EXPECT_FALSE(panel.getLayout().extendRight);

    sendMouse(wheel, QEvent::MouseMove, c + QPointF(16., -8.), Qt::NoButton, Qt::LeftButton);
    sendMouse(wheel, QEvent::MouseButtonRelease, c + QPointF(16., -8.), Qt::LeftButton, Qt::NoButton);

    EXPECT_EQ(1, rec.starts);
    EXPECT_GE(rec.moves, 1);
    EXPECT_EQ(1, rec.ends);
    EXPECT_FALSE(panel.isEditing());
    EXPECT_EQ(1, counter.applied);
    EXPECT_NE(area, panel.getEditArea());
    EXPECT_TRUE(panel.getLayout().extendRight);
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(panel.getValue().rgb[i], panel.getWheel()->getValue().rgb[i]);
    }
}

namespace {

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

double
customMarkerX(const ColorEditSlider* s)
{
    QRectF r = s->trackRect();
    SliderRange range = s->range();

    return r.left() + (s->value() - range.rangeMin) / (range.rangeMax - range.rangeMin) * r.width();
}

double
customUnitsPerPixel(const ColorEditSlider* s)
{
    SliderRange range = s->range();

    return (range.rangeMax - range.rangeMin) / s->trackRect().width();
}

} // namespace

TEST(ColorAdjustPanel, CustomStylePairsColorEditSlidersWithNatronSpinBoxes)
{
    DevSwitchScope dev(true);

    ColorPanelLayoutDev::instance()->setSliderStyle(ColorPanelLayout::SliderStyle::Custom);

    ColorAdjustPanel panel;

    setUpPanel(panel, makeValue(1.2, 1.0, 0.8));
    EXPECT_TRUE(panel.getLayout().style == ColorPanelLayout::SliderStyle::Custom);
    for (ColorSliderKindEnum kind : kLineKinds) {
        ColorEditSlider* s = panel.getCustomSlider(kind);

        ASSERT_TRUE(s);
        EXPECT_EQ(nullptr, panel.getSlider(kind));
        EXPECT_TRUE(panel.getSpinBox(kind));
        EXPECT_EQ(kind, s->kind());
        EXPECT_EQ(eColorKnobRoleGain, s->role());
        EXPECT_FALSE(s->gradientStops().isEmpty());
        EXPECT_TRUE(s->isVisible());
    }
}

TEST(ColorAdjustPanel, CustomMarkerDragIsOneUndoStepAndTheSpinBoxFollows)
{
    DevSwitchScope dev(true);

    ColorPanelLayoutDev::instance()->setSliderStyle(ColorPanelLayout::SliderStyle::Custom);

    ColorAdjustPanel panel;
    LumaBasis basis = acescg();

    setUpPanel(panel, makeValue(1.0, 1.0, 1.0));

    PanelRecorder rec(panel);
    ColorEditSlider* value = panel.getCustomSlider(eColorSliderKindValue);

    ASSERT_TRUE(value);

    double before = value->value();
    double expected = before + 40. * customUnitsPerPixel(value);

    customDrag(value, customMarkerX(value), 40., Qt::ControlModifier);

    EXPECT_EQ(1, rec.starts);
    EXPECT_GE(rec.moves, 1);
    EXPECT_EQ(1, rec.ends);
    EXPECT_FALSE(panel.isEditing());
    EXPECT_EQ(QString::fromUtf8("value slider (absolute)"), rec.endLabel);
    EXPECT_NEAR(expected, std::log2(luma(basis, rec.endValue.rgb)), 1e-9);
    EXPECT_NEAR(expected, panel.getSpinBox(eColorSliderKindValue)->value(), 1e-3);
    EXPECT_NEAR(expected, value->value(), 1e-9);
}

TEST(ColorAdjustPanel, CustomRelativeCtrlDragMovesATenthAndIsOneUndoStep)
{
    DevSwitchScope dev(true);

    ColorPanelLayoutDev::instance()->setSliderStyle(ColorPanelLayout::SliderStyle::Custom);

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

TEST(ColorAdjustPanel, SwitchingStylesLiveKeepsTheValueAndEveryLine)
{
    DevSwitchScope dev(true);
    ColorAdjustPanel panel;
    KnobColorValue start = makeValue(1.2, 1.0, 0.8);

    setUpPanel(panel, start);
    panel.setRightInset(24);

    double hueBefore = panel.getSpinBox(eColorSliderKindHue)->value();
    double valueBefore = panel.getSpinBox(eColorSliderKindValue)->value();

    ColorPanelLayoutDev::instance()->setSliderStyle(ColorPanelLayout::SliderStyle::Custom);
    ASSERT_TRUE(panel.getLayout().style == ColorPanelLayout::SliderStyle::Custom);
    EXPECT_EQ(hueBefore, panel.getSpinBox(eColorSliderKindHue)->value());
    EXPECT_EQ(valueBefore, panel.getSpinBox(eColorSliderKindValue)->value());
    EXPECT_NEAR(valueBefore, panel.getCustomSlider(eColorSliderKindValue)->value(), 1e-3);

    ColorPanelLayoutDev::instance()->setSliderStyle(ColorPanelLayout::SliderStyle::Vertical);
    ASSERT_TRUE(panel.getLayout().style == ColorPanelLayout::SliderStyle::Vertical);
    EXPECT_EQ(nullptr, panel.getCustomSlider(eColorSliderKindValue));
    EXPECT_EQ(hueBefore, panel.getSpinBox(eColorSliderKindHue)->value());
    EXPECT_EQ(valueBefore, panel.getSpinBox(eColorSliderKindValue)->value());
    EXPECT_EQ(24, panel.getRightInset());

    ColorPanelLayoutDev::instance()->setSliderStyle(ColorPanelLayout::SliderStyle::Natron);
    ASSERT_TRUE(panel.getSlider(eColorSliderKindValue));
    EXPECT_EQ(Qt::Horizontal, panel.getSlider(eColorSliderKindValue)->orientation());
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(start.rgb[i], panel.getValue().rgb[i]);
    }
}

TEST(ColorAdjustPanel, VerticalStyleHasAColumnPerLineAsTallAsTheWheel)
{
    DevSwitchScope dev(true);

    ColorPanelLayoutDev::instance()->setSliderStyle(ColorPanelLayout::SliderStyle::Vertical);

    ColorAdjustPanel panel;

    setUpPanel(panel, makeValue(1.2, 1.0, 0.8));
    panel.resize(900, 500);
    QApplication::processEvents();
    QApplication::processEvents();

    ColorWheelWidget* wheel = panel.getWheel();
    int vertical = 0;

    for (ColorSliderKindEnum kind : kLineKinds) {
        ScaleSliderQWidget* s = panel.getSlider(kind);
        SpinBox* box = panel.getSpinBox(kind);

        ASSERT_TRUE(s);
        ASSERT_TRUE(box);
        EXPECT_EQ(nullptr, panel.getCustomSlider(kind));
        if (s->orientation() == Qt::Vertical) {
            ++vertical;
        }
        EXPECT_TRUE(s->isVisible());
        EXPECT_NEAR(wheel->height(), s->height(), 2);

        // Caption above, field below, and the column right of the wheel.
        EXPECT_GE(box->geometry().top(), s->geometry().bottom());
        EXPECT_GE(s->geometry().left(), wheel->geometry().right());
    }
    EXPECT_EQ(5, vertical);
    EXPECT_GE(wheel->width(), ColorWheelWidget::minimumDiameter());
    EXPECT_LE(wheel->width(), ColorWheelWidget::maximumDiameter());
}

TEST(ColorAdjustPanel, VerticalStyleOneDimensionalKnobShowsOnlyValue)
{
    DevSwitchScope dev(true);

    ColorPanelLayoutDev::instance()->setSliderStyle(ColorPanelLayout::SliderStyle::Vertical);

    ColorAdjustPanel panel;

    setUpPanel(panel, makeValue(1.0, 1.0, 1.0), 1);

    EXPECT_FALSE(panel.getWheel()->isVisible());
    for (ColorSliderKindEnum kind : kLineKinds) {
        bool expected = kind == eColorSliderKindValue;

        EXPECT_EQ(expected, panel.getSlider(kind)->isVisible());
        EXPECT_EQ(expected, panel.getSpinBox(kind)->isVisible());
    }
}

TEST(ColorAdjustPanel, VerticalUpwardDragOnValueRaisesItInOneUndoStep)
{
    DevSwitchScope dev(true);

    ColorPanelLayoutDev::instance()->setSliderStyle(ColorPanelLayout::SliderStyle::Vertical);

    ColorAdjustPanel panel;
    LumaBasis basis = acescg();

    setUpPanel(panel, makeValue(1.0, 1.0, 1.0));

    PanelRecorder rec(panel);
    ScaleSliderQWidget* value = panel.getSlider(eColorSliderKindValue);
    double x = value->width() / 2.;
    double y0 = value->height() / 2.;

    sendMouse(value, QEvent::MouseButtonPress, QPointF(x, y0), Qt::LeftButton, Qt::LeftButton);
    double atPress = std::log2(luma(basis, panel.getValue().rgb));
    for (int i = 1; i <= 4; ++i) {
        sendMouse(value, QEvent::MouseMove, QPointF(x, y0 - 10. * i), Qt::NoButton, Qt::LeftButton);
    }
    sendMouse(value, QEvent::MouseButtonRelease, QPointF(x, y0 - 40.), Qt::LeftButton, Qt::NoButton);

    EXPECT_EQ(1, rec.starts);
    EXPECT_EQ(1, rec.ends);
    EXPECT_FALSE(panel.isEditing());
    EXPECT_GT(std::log2(luma(basis, rec.endValue.rgb)), atPress);
}
