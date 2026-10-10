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
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPixmap>
#include <QPoint>
#include <QPointF>
#include <QString>
#include <QTabWidget>

#include <gtest/gtest.h>

#include "Engine/ColorWheelMath.h"

#include "Gui/ColorAdjustPanel.h"
#include "Gui/ColorEditSlider.h"
#include "Gui/ColorWheelWidget.h"
#include "Gui/LineEdit.h"

NATRON_NAMESPACE_USING

namespace {
const double kAP1Weights[3] = { 0.2722287168, 0.6740817658, 0.0536895174 };
const double kAP1ToXYZ[9] = {
    0.6624541811, 0.1340042065, 0.1561876870,
    0.2722287168, 0.6740817658, 0.0536895174,
    -0.0055746495, 0.0040607335, 1.0103391003
};

// ColorEditSlider lays out a 56px field, a 6px gap and 4px of padding either side, so its
// track starts at x = 4 and is 70px narrower than the slider.
const double kTrackLeft = 4.;
const double kTrackInset = 70.;

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
                ++starts;
                startValue = v;
                break;
            case ColorAdjustPanel::eGesturePhaseMove:
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
          Qt::MouseButtons buttons)
{
    QMouseEvent e(type, pos, w->mapToGlobal(pos), button, buttons, Qt::NoModifier);

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
typeInto(LineEdit* edit,
         const char* text)
{
    edit->setText(QString::fromUtf8(text));
    sendKey(edit, Qt::Key_Return);
}

// Presses well clear of the marker so the drag is relative, moves right by dx in two steps
// and releases.
void
relativeDrag(ColorEditSlider* s,
             double dx)
{
    double y = s->height() / 2.;
    double trackWidth = s->width() - kTrackInset;
    double markerX = kTrackLeft + valueToMarker(s->range(), s->value()) * trackWidth;
    double x0 = (markerX > kTrackLeft + trackWidth / 2.) ? kTrackLeft + 4. : kTrackLeft + trackWidth / 2. + 10.;

    if (std::abs(x0 - markerX) < 20.) {
        x0 = kTrackLeft + 4.;
    }
    sendMouse(s, QEvent::MouseButtonPress, QPointF(x0, y), Qt::LeftButton, Qt::LeftButton);
    sendMouse(s, QEvent::MouseMove, QPointF(x0 + dx / 2., y), Qt::NoButton, Qt::LeftButton);
    sendMouse(s, QEvent::MouseMove, QPointF(x0 + dx, y), Qt::NoButton, Qt::LeftButton);
    sendMouse(s, QEvent::MouseButtonRelease, QPointF(x0 + dx, y), Qt::LeftButton, Qt::NoButton);
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

TEST(ColorAdjustPanel, ValueRelativeDragOfOneHundredPixelsDoublesGainAndAlpha)
{
    ColorAdjustPanel panel;

    setUpPanel(panel, makeValue(1.2, 1.0, 0.8, 1.0));

    PanelRecorder rec(panel);
    ColorEditSlider* slider = panel.getSlider(eColorSliderKindValue);

    ASSERT_TRUE(slider);
    ASSERT_GT(slider->width(), 200);
    relativeDrag(slider, 100.);

    EXPECT_EQ(1, rec.starts);
    EXPECT_EQ(1, rec.ends);
    EXPECT_NEAR(2.4, rec.endValue.rgb[0], 1e-9);
    EXPECT_NEAR(2.0, rec.endValue.rgb[1], 1e-9);
    EXPECT_NEAR(1.6, rec.endValue.rgb[2], 1e-9);
    EXPECT_NEAR(2.0, rec.endValue.a, 1e-9);
}

TEST(ColorAdjustPanel, TempDragKeepsLumaAndLeavesAlphaAlone)
{
    ColorAdjustPanel panel;
    KnobColorValue start = makeValue(1.2, 1.0, 0.8, 0.7);

    setUpPanel(panel, start);

    PanelRecorder rec(panel);
    ColorEditSlider* slider = panel.getSlider(eColorSliderKindTemp);

    ASSERT_TRUE(slider);
    relativeDrag(slider, 60.);

    ASSERT_EQ(1, rec.ends);

    LumaBasis basis = acescg();

    EXPECT_LT(relativeError(luma(basis, rec.endValue.rgb), luma(basis, start.rgb)), 1e-9);
    EXPECT_EQ(0.7, rec.endValue.a);
    EXPECT_GT(std::abs(rec.endValue.rgb[0] - start.rgb[0]) + std::abs(rec.endValue.rgb[2] - start.rgb[2]), 1e-4);
}

TEST(ColorAdjustPanel, SatFromZeroGrowsAlongRememberedHue)
{
    ColorAdjustPanel panel;
    KnobColorValue start = makeValue(1.2, 1.0, 0.8);
    LumaBasis basis = acescg();
    double hue0 = planeHue(toPlane(eColorKnobRoleGain, basis, start));

    setUpPanel(panel, start);

    ColorEditSlider* slider = panel.getSlider(eColorSliderKindSat);

    ASSERT_TRUE(slider);
    typeInto(slider->valueLineEdit(), "0");

    PlanePoint neutral = toPlane(eColorKnobRoleGain, basis, panel.getValue());

    ASSERT_TRUE(neutral.valid);
    EXPECT_LT(planeSaturation(neutral), 1e-9);
    EXPECT_NEAR(hue0, panel.getRememberedHue(), 1e-9);

    relativeDrag(slider, 100.);

    PlanePoint grown = toPlane(eColorKnobRoleGain, basis, panel.getValue());

    ASSERT_TRUE(grown.valid);
    EXPECT_NEAR(100. * TrackballRates().satPlanePerPixel, planeSaturation(grown), 1e-6);
    EXPECT_LT(angleDifference(hue0, planeHue(grown)), 0.5);
}

TEST(ColorAdjustPanel, TypedHueKeepsLuma)
{
    ColorAdjustPanel panel;
    KnobColorValue start = makeValue(1.5, 1.0, 1.0, 0.9);
    LumaBasis basis = acescg();

    setUpPanel(panel, start);

    PanelRecorder rec(panel);
    ColorEditSlider* slider = panel.getSlider(eColorSliderKindHue);

    ASSERT_TRUE(slider);
    typeInto(slider->valueLineEdit(), "200");

    ASSERT_EQ(1, rec.starts);
    ASSERT_EQ(1, rec.ends);
    EXPECT_LT(relativeError(luma(basis, rec.endValue.rgb), luma(basis, start.rgb)), 1e-12);
    EXPECT_NEAR(200.0, planeHue(toPlane(eColorKnobRoleGain, basis, rec.endValue)), 1e-6);
    EXPECT_EQ(0.9, rec.endValue.a);
}

TEST(ColorAdjustPanel, TypedRedOfFourRoundTripsUnclamped)
{
    ColorAdjustPanel panel;

    setUpPanel(panel, makeValue(1.0, 0.5, 0.25));
    panel.getTabWidget()->setCurrentWidget(panel.getValuesTab());

    PanelRecorder rec(panel);
    LineEdit* red = panel.getValueField(ColorAdjustPanel::eValueFieldR);

    ASSERT_TRUE(red);
    typeInto(red, "4.0");

    ASSERT_EQ(1, rec.ends);
    EXPECT_EQ(4.0, rec.endValue.rgb[0]);
    EXPECT_EQ(0.5, rec.endValue.rgb[1]);
    EXPECT_EQ(0.25, rec.endValue.rgb[2]);
    EXPECT_EQ(4.0, panel.getValue().rgb[0]);
    EXPECT_EQ(4.0, red->text().toDouble());
    EXPECT_EQ(4.0, panel.getValueField(ColorAdjustPanel::eValueFieldV)->text().toDouble());
}

TEST(ColorAdjustPanel, TypedHsvValueIsAValueEditSoAlphaTracks)
{
    ColorAdjustPanel panel;

    setUpPanel(panel, makeValue(1.2, 1.0, 0.8, 1.0));

    PanelRecorder rec(panel);

    typeInto(panel.getValueField(ColorAdjustPanel::eValueFieldV), "2.4");

    ASSERT_EQ(1, rec.ends);
    EXPECT_NEAR(2.4, rec.endValue.rgb[0], 1e-12);
    EXPECT_NEAR(2.0, rec.endValue.rgb[1], 1e-12);
    EXPECT_NEAR(1.6, rec.endValue.rgb[2], 1e-12);
    EXPECT_NEAR(2.0, rec.endValue.a, 1e-12);
}

TEST(ColorAdjustPanel, UnclampedHsvRoundTrips)
{
    const double samples[][3] = {
        { 4.0, -0.5, 2.0 }, { 0.2, 0.7, 12.0 }, { 1.5, 1.0, 1.0 }, { 0.3, 0.3, 0.3 }, { -0.2, -0.1, -0.4 }
    };

    for (const auto& rgb : samples) {
        double hsv[3];
        double back[3];

        ColorAdjustPanel::rgbToHsv(rgb, hsv);
        ColorAdjustPanel::hsvToRgb(hsv, back);
        for (int i = 0; i < 3; ++i) {
            EXPECT_NEAR(rgb[i], back[i], 1e-12);
        }
    }
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

    EXPECT_EQ(0, rec.starts);
    EXPECT_EQ(0, rec.moves);
    EXPECT_EQ(0, rec.ends);
    EXPECT_FALSE(panel.isEditing());
}

TEST(ColorAdjustPanel, ValueSliderDoubleClickResetsGain)
{
    ColorAdjustPanel panel;

    setUpPanel(panel, makeValue(2.0, 2.0, 2.0, 2.0));

    PanelRecorder rec(panel);
    ColorEditSlider* slider = panel.getSlider(eColorSliderKindValue);
    QPointF p(kTrackLeft + 10., slider->height() / 2.);

    sendMouse(slider, QEvent::MouseButtonDblClick, p, Qt::LeftButton, Qt::LeftButton);

    ASSERT_EQ(1, rec.starts);
    ASSERT_EQ(1, rec.ends);
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(1.0, rec.endValue.rgb[i], 1e-12);
    }
    EXPECT_NEAR(1.0, rec.endValue.a, 1e-12);
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

TEST(ColorAdjustPanel, OneDimensionalKnobShowsOnlyValue)
{
    ColorAdjustPanel panel;

    setUpPanel(panel, makeValue(0.5, 0.5, 0.5, 0.0, false), 1);

    QWidget* wheelTab = panel.getWheelTab();
    QWidget* valuesTab = panel.getValuesTab();

    EXPECT_FALSE(panel.getWheel()->isVisibleTo(wheelTab));
    EXPECT_TRUE(panel.getSlider(eColorSliderKindValue)->isVisibleTo(wheelTab));
    EXPECT_FALSE(panel.getSlider(eColorSliderKindHue)->isVisibleTo(wheelTab));
    EXPECT_FALSE(panel.getSlider(eColorSliderKindSat)->isVisibleTo(wheelTab));
    EXPECT_FALSE(panel.getSlider(eColorSliderKindTemp)->isVisibleTo(wheelTab));
    EXPECT_FALSE(panel.getSlider(eColorSliderKindTint)->isVisibleTo(wheelTab));
    EXPECT_FALSE(panel.getSlider(eColorSliderKindAlpha)->isVisibleTo(wheelTab));
    EXPECT_TRUE(panel.getValueField(ColorAdjustPanel::eValueFieldR)->isVisibleTo(valuesTab));
    EXPECT_FALSE(panel.getValueField(ColorAdjustPanel::eValueFieldG)->isVisibleTo(valuesTab));
    EXPECT_FALSE(panel.getValueField(ColorAdjustPanel::eValueFieldHex)->isVisibleTo(valuesTab));

    PanelRecorder rec(panel);

    typeInto(panel.getValueField(ColorAdjustPanel::eValueFieldR), "3");
    ASSERT_EQ(1, rec.ends);
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(3.0, rec.endValue.rgb[i]);
    }
}

TEST(ColorAdjustPanel, ThreeDimensionalKnobHidesAlpha)
{
    ColorAdjustPanel panel;

    setUpPanel(panel, makeValue(1.0, 0.9, 0.8, 0.0, false), 3);

    QWidget* wheelTab = panel.getWheelTab();
    QWidget* valuesTab = panel.getValuesTab();

    EXPECT_TRUE(panel.getWheel()->isVisibleTo(wheelTab));
    EXPECT_TRUE(panel.getSlider(eColorSliderKindHue)->isVisibleTo(wheelTab));
    EXPECT_FALSE(panel.getSlider(eColorSliderKindAlpha)->isVisibleTo(wheelTab));
    EXPECT_TRUE(panel.getValueField(ColorAdjustPanel::eValueFieldB)->isVisibleTo(valuesTab));
    EXPECT_FALSE(panel.getValueField(ColorAdjustPanel::eValueFieldA)->isVisibleTo(valuesTab));
}

TEST(ColorAdjustPanel, ValueSliderMarkerFollowsTheValue)
{
    ColorAdjustPanel panel;

    setUpPanel(panel, makeValue(1.0, 1.0, 1.0));

    PanelRecorder rec(panel);
    ColorEditSlider* slider = panel.getSlider(eColorSliderKindValue);

    relativeDrag(slider, 50.);
    ASSERT_EQ(1, rec.ends);
    EXPECT_NEAR(0.5, slider->value(), 1e-9);
}

TEST(ColorAdjustPanel, SliderGradientsFollowTheCurrentColour)
{
    ColorAdjustPanel panel;

    setUpPanel(panel, makeValue(1.0, 1.0, 1.0));

    ColorEditSlider* value = panel.getSlider(eColorSliderKindValue);
    double y = value->height() / 2.;
    double trackWidth = value->width() - kTrackInset;
    QImage track = value->grab().toImage();
    QColor dark = track.pixelColor(QPoint((int)(kTrackLeft + 6.), (int)y));
    QColor bright = track.pixelColor(QPoint((int)(kTrackLeft + trackWidth - 6.), (int)y));

    EXPECT_LT(dark.lightness() + 40, bright.lightness());

    panel.setKnobValue(makeValue(1.05, 1.0, 0.95));

    ColorEditSlider* sat = panel.getSlider(eColorSliderKindSat);
    QImage satTrack = sat->grab().toImage();
    QColor grey = satTrack.pixelColor(QPoint((int)(kTrackLeft + 3.), (int)y));
    QColor tinted = satTrack.pixelColor(QPoint((int)(kTrackLeft + sat->width() - kTrackInset - 3.), (int)y));

    EXPECT_LE(std::abs(grey.red() - grey.blue()), 3);
    EXPECT_GT(tinted.red(), tinted.blue() + 3);
}
