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
#include <vector>

#include <QApplication>
#include <QDir>
#include <QImage>
#include <QMouseEvent>
#include <QPointF>
#include <QString>
#include <QWheelEvent>

#include <gtest/gtest.h>

#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Image/ColorMath.h"

#include "Gui/KnobGuiTrapezoid.h"

NATRON_NAMESPACE_USING

namespace {
// The strip is this wide, so its drawn domain spans 200 pixels between the 4-pixel margins.
const int kWidth = 208;
const int kHeight = 30;
const double kSpan = 200.;
const int kDims = KnobDouble::eRangeWidgetDimensionCount;

struct Recorder {
    std::vector<TrapezoidStrip::GesturePhaseEnum> phases;
    std::vector<std::vector<double>> values;

    TrapezoidStrip::EditCallback callback()
    {
        return [this](const double* v, TrapezoidStrip::GesturePhaseEnum phase) {
            phases.push_back(phase);
            values.push_back(std::vector<double>(v, v + kDims));
        };
    }
};

void
identity(float* /*rgb*/,
         int /*count*/)
{
}

// The centre, then the outer low, inner low, inner high and outer high offsets.
void
setUp(TrapezoidStrip& strip,
      double centre,
      double outerLow,
      double innerLow,
      double innerHigh,
      double outerHigh,
      ColorMath::KeyModelEnum model = ColorMath::eKeyModelHSV)
{
    const double values[kDims] = { centre, outerLow, innerLow, innerHigh, outerHigh };
    double toXYZ[9];
    ColorMath::workingToXYZ(0, toXYZ);
    const float source[3] = { 0.6f, 0.8f, 0.5f };

    strip.resize(kWidth, kHeight);
    strip.setDisplayConverter(&identity);
    strip.setContext(model, ColorMath::okLabBasis(toXYZ), source, 1.f);
    strip.setValues(values);
}

void
sendMouse(TrapezoidStrip& strip,
          QEvent::Type type,
          double x,
          Qt::MouseButton button,
          Qt::MouseButtons buttons,
          Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    const QPointF pos(x, kHeight * 0.5);
    QMouseEvent e(type, pos, strip.mapToGlobal(pos), button, buttons, modifiers);

    QApplication::sendEvent(&strip, &e);
}

// Presses button at x, moves by each of dxs in turn and releases.
void
drag(TrapezoidStrip& strip,
     double x,
     const std::vector<double>& dxs,
     Qt::KeyboardModifiers modifiers = Qt::NoModifier,
     Qt::MouseButton button = Qt::LeftButton)
{
    sendMouse(strip, QEvent::MouseButtonPress, x, button, button, modifiers);
    double at = x;
    for (double dx : dxs) {
        at += dx;
        sendMouse(strip, QEvent::MouseMove, at, Qt::NoButton, button, modifiers);
    }
    sendMouse(strip, QEvent::MouseButtonRelease, at, button, Qt::NoButton, modifiers);
}

void
expectValues(const TrapezoidStrip& strip,
             double centre,
             double outerLow,
             double innerLow,
             double innerHigh,
             double outerHigh,
             double tolerance = 1e-9)
{
    const double want[kDims] = { centre, outerLow, innerLow, innerHigh, outerHigh };
    double v[kDims];

    strip.getValues(v);
    for (int i = 0; i < kDims; ++i) {
        EXPECT_NEAR(want[i], v[i], tolerance) << i;
    }
}

void
expectDomain(const TrapezoidStrip& strip,
             double low,
             double high,
             double tolerance = 1e-9)
{
    double l, h;

    strip.getDomain(&l, &h);
    EXPECT_NEAR(low, l, tolerance);
    EXPECT_NEAR(high, h, tolerance);
}

QString
outputDirectory()
{
#ifdef NATRON_TESTS_OUTPUT_DIR
    QString dir = QString::fromUtf8(NATRON_TESTS_OUTPUT_DIR);
#else
    QString dir = QDir::tempPath() + QString::fromUtf8("/NatronGuiTests");
#endif

    QDir().mkpath(dir);

    return dir;
}
} // namespace

// Hue shows one turn centred on the centre, saturation 0 to 1 (0.4 for OkLCh chroma) and luma
// 0 to the larger of 1 and the outer high point.
TEST(KnobGuiTrapezoid, DefaultDomains)
{
    TrapezoidStrip hue(KnobDouble::eRangeWidgetRoleHue);
    setUp(hue, 0.3, 0.1, 0.05, 0.05, 0.1);
    expectDomain(hue, -0.2, 0.8);

    TrapezoidStrip sat(KnobDouble::eRangeWidgetRoleSaturation);
    setUp(sat, 0.5, 0.4, 0.2, 0.2, 0.9);
    expectDomain(sat, 0., 1.);
    EXPECT_NEAR(4., sat.valueToX(0.), 1e-9);
    EXPECT_NEAR(4. + kSpan, sat.valueToX(1.), 1e-9);
    EXPECT_NEAR(0.25, sat.xToValue(4. + kSpan * 0.25), 1e-9);
    setUp(sat, 0.05, 0.04, 0.02, 0.02, 0.1, ColorMath::eKeyModelOkLCh);
    expectDomain(sat, 0., 0.4);

    TrapezoidStrip luma(KnobDouble::eRangeWidgetRoleLuma);
    setUp(luma, 0.2, 0.1, 0.05, 0.05, 0.3);
    expectDomain(luma, 0., 1.);
    setUp(luma, 0.5, 0.5, 0.45, 0.5, 1.5);
    expectDomain(luma, 0., 2.);
}

// A hue range across red is drawn unsplit around its centre.
TEST(KnobGuiTrapezoid, HuePositionsAroundTheCentre)
{
    TrapezoidStrip strip(KnobDouble::eRangeWidgetRoleHue);

    setUp(strip, 0., 0.1, 0.05, 0.05, 0.1);
    expectDomain(strip, -0.5, 0.5);
    double p[4];
    double centre = -1.;
    strip.getPositions(p, &centre);
    EXPECT_NEAR(0., centre, 1e-9);
    EXPECT_NEAR(-0.1, p[0], 1e-9);
    EXPECT_NEAR(-0.05, p[1], 1e-9);
    EXPECT_NEAR(0.05, p[2], 1e-9);
    EXPECT_NEAR(0.1, p[3], 1e-9);
    EXPECT_NEAR(4. + kSpan * 0.5, strip.valueToX(centre), 1e-9);

    // An offset past a turn is drawn as a turn, as it keys.
    setUp(strip, 0.5, 3., 0.05, 0.05, 0.1);
    strip.getPositions(p);
    EXPECT_NEAR(-0.5, p[0], 1e-9);
}

TEST(KnobGuiTrapezoid, PressFindsHandlesAndThePlateau)
{
    TrapezoidStrip strip(KnobDouble::eRangeWidgetRoleLuma);

    // 0.1, 0.3, 0.6, 0.8.
    setUp(strip, 0.45, 0.35, 0.15, 0.15, 0.35);
    EXPECT_EQ(TrapezoidStrip::ePartOuterLow, strip.partAt(strip.valueToX(0.1) + 2.));
    EXPECT_EQ(TrapezoidStrip::ePartInnerLow, strip.partAt(strip.valueToX(0.3)));
    EXPECT_EQ(TrapezoidStrip::ePartInnerHigh, strip.partAt(strip.valueToX(0.6) - 3.));
    EXPECT_EQ(TrapezoidStrip::ePartOuterHigh, strip.partAt(strip.valueToX(0.8)));
    EXPECT_EQ(TrapezoidStrip::ePartPlateau, strip.partAt(strip.valueToX(0.45)));
    EXPECT_EQ(TrapezoidStrip::ePartNone, strip.partAt(strip.valueToX(0.95)));
}

// A handle drag writes its own offset only, reports start, moves and end, and stops at its
// neighbour or, for an inner handle, at the centre.
TEST(KnobGuiTrapezoid, HandleDragIsClampedBetweenNeighboursAndTheCentre)
{
    TrapezoidStrip strip(KnobDouble::eRangeWidgetRoleLuma);
    Recorder recorder;

    setUp(strip, 0.45, 0.35, 0.15, 0.15, 0.35);
    strip.setEditCallback(recorder.callback());
    drag(strip, strip.valueToX(0.3), { 10., 10. });
    expectValues(strip, 0.45, 0.35, 0.05, 0.15, 0.35);
    ASSERT_EQ(4u, recorder.phases.size());
    EXPECT_EQ(TrapezoidStrip::eGesturePhaseStart, recorder.phases[0]);
    EXPECT_EQ(TrapezoidStrip::eGesturePhaseMove, recorder.phases[1]);
    EXPECT_EQ(TrapezoidStrip::eGesturePhaseMove, recorder.phases[2]);
    EXPECT_EQ(TrapezoidStrip::eGesturePhaseEnd, recorder.phases[3]);
    EXPECT_NEAR(0.05, recorder.values[3][KnobDouble::eRangeWidgetDimensionInnerLow], 1e-9);
    EXPECT_FALSE(strip.isDragging());

    // Far past the centre, the inner low point stops there.
    drag(strip, strip.valueToX(0.4), { 150. });
    expectValues(strip, 0.45, 0.35, 0., 0.15, 0.35);
    // The inner high point stops at the centre coming down.
    drag(strip, strip.valueToX(0.6) + 2., { -150. });
    expectValues(strip, 0.45, 0.35, 0., 0., 0.35);
    // The outer high point stops at the inner high one.
    drag(strip, strip.valueToX(0.8), { -150. });
    expectValues(strip, 0.45, 0.35, 0., 0., 0.);

    // A click with no move edits nothing.
    recorder.phases.clear();
    drag(strip, strip.valueToX(0.1), {});
    EXPECT_TRUE(recorder.phases.empty());
}

TEST(KnobGuiTrapezoid, PlateauDragMovesTheCentre)
{
    TrapezoidStrip strip(KnobDouble::eRangeWidgetRoleSaturation);

    setUp(strip, 0.45, 0.35, 0.15, 0.15, 0.35);
    drag(strip, strip.valueToX(0.4), { -20. });
    expectValues(strip, 0.35, 0.35, 0.15, 0.15, 0.35);
}

TEST(KnobGuiTrapezoid, ShiftDragMovesTheOuterPartner)
{
    TrapezoidStrip strip(KnobDouble::eRangeWidgetRoleLuma);

    setUp(strip, 0.45, 0.35, 0.15, 0.15, 0.35);
    drag(strip, strip.valueToX(0.6), { 20. }, Qt::ShiftModifier);
    expectValues(strip, 0.45, 0.35, 0.15, 0.25, 0.45);
    drag(strip, strip.valueToX(0.3), { -20. }, Qt::ShiftModifier);
    expectValues(strip, 0.45, 0.45, 0.25, 0.25, 0.45);
    // The pair stops when the inner point meets the centre.
    drag(strip, strip.valueToX(0.2), { 150. }, Qt::ShiftModifier);
    expectValues(strip, 0.45, 0.2, 0., 0.25, 0.45);
}

// Coincident handles: a move to the left takes the lowest, to the right the highest.
TEST(KnobGuiTrapezoid, CoincidentHandlesFollowTheDragDirection)
{
    TrapezoidStrip strip(KnobDouble::eRangeWidgetRoleLuma);

    // 0.1, 0.4, 0.4, 0.8.
    setUp(strip, 0.4, 0.3, 0., 0., 0.4);
    drag(strip, strip.valueToX(0.4), { 20. });
    expectValues(strip, 0.4, 0.3, 0., 0.1, 0.4);

    setUp(strip, 0.4, 0.3, 0., 0., 0.4);
    drag(strip, strip.valueToX(0.4), { -20. });
    expectValues(strip, 0.4, 0.3, 0.1, 0., 0.4);

    // All four at the centre.
    setUp(strip, 0.5, 0., 0., 0., 0.);
    drag(strip, strip.valueToX(0.5), { -20. });
    expectValues(strip, 0.5, 0.1, 0., 0., 0.);
    setUp(strip, 0.5, 0., 0., 0., 0.);
    drag(strip, strip.valueToX(0.5), { 20. });
    expectValues(strip, 0.5, 0., 0., 0., 0.1);
}

// The hue centre is written wrapped, and the outer points stay within a turn of it.
TEST(KnobGuiTrapezoid, HueDragWrapsAndStaysWithinATurn)
{
    TrapezoidStrip strip(KnobDouble::eRangeWidgetRoleHue);

    setUp(strip, 0.9, 0.1, 0.05, 0.05, 0.1);
    // One turn is 200 pixels: the plateau moves 0.15 turn up, across red.
    drag(strip, strip.valueToX(0.9), { 30. });
    expectValues(strip, 0.05, 0.1, 0.05, 0.05, 0.1);

    setUp(strip, 0.35, 0.15, 0.05, 0.05, 0.15);
    // The outer points are dragged far out, and stop a turn from the centre.
    drag(strip, strip.valueToX(0.2), { -100., -100., -100. });
    drag(strip, strip.valueToX(0.5), { 100., 100., 100. });
    expectValues(strip, 0.35, 1., 0.05, 0.05, 1.);
    double p[4];
    double centre;
    strip.getPositions(p, &centre);
    EXPECT_NEAR(1., centre - p[0], 1e-9);
    EXPECT_NEAR(1., p[3] - centre, 1e-9);
}

// The wheel zooms around the cursor, a middle or Alt drag pans, and resetZoom() restores the
// default domain. Neither edits the values.
TEST(KnobGuiTrapezoid, ZoomAndPan)
{
    TrapezoidStrip strip(KnobDouble::eRangeWidgetRoleLuma);
    Recorder recorder;

    setUp(strip, 0.45, 0.35, 0.15, 0.15, 0.35);
    strip.setEditCallback(recorder.callback());
    EXPECT_FALSE(strip.isZoomed());
    const double x = strip.valueToX(0.3);
    strip.zoomAround(x, 0.5);
    EXPECT_TRUE(strip.isZoomed());
    expectDomain(strip, 0.15, 0.65);
    EXPECT_NEAR(0.3, strip.xToValue(x), 1e-9);

    {
        const QPointF pos(x, kHeight * 0.5);
        QWheelEvent e(pos, strip.mapToGlobal(pos), QPoint(), QPoint(0, 480), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(&strip, &e);
    }
    // 480 eighths of a degree up halves the domain again.
    expectDomain(strip, 0.225, 0.475);
    EXPECT_NEAR(0.3, strip.xToValue(x), 1e-9);

    strip.resetZoom();
    EXPECT_FALSE(strip.isZoomed());
    expectDomain(strip, 0., 1.);

    strip.panByPixels(20.);
    expectDomain(strip, -0.1, 0.9);
    strip.resetZoom();

    // A middle drag to the right moves the content right.
    drag(strip, strip.valueToX(0.45), { 20. }, Qt::NoModifier, Qt::MiddleButton);
    expectDomain(strip, -0.1, 0.9);
    strip.resetZoom();
    drag(strip, strip.valueToX(0.45), { -40. }, Qt::AltModifier);
    expectDomain(strip, 0.2, 1.2);
    expectValues(strip, 0.45, 0.35, 0.15, 0.15, 0.35);
    EXPECT_TRUE(recorder.phases.empty());

    // Zoomed in, a handle drag moves the value by the zoomed scale.
    strip.resetZoom();
    strip.zoomAround(strip.valueToX(0.3), 0.5);
    drag(strip, strip.valueToX(0.3), { 20. });
    expectValues(strip, 0.45, 0.35, 0.1, 0.15, 0.35);
}

// The trapezoid's base is open: just above it the fill reads as it does mid-shape.
TEST(KnobGuiTrapezoid, NoLineAlongTheBase)
{
    TrapezoidStrip strip(KnobDouble::eRangeWidgetRoleLuma);

    // 0.1, 0.3, 0.6, 0.8; the source tick is at 0.5 and the centre notch at the top.
    setUp(strip, 0.45, 0.35, 0.15, 0.15, 0.35);
    const QImage image = strip.grab().toImage();
    ASSERT_EQ(kWidth, image.width());
    ASSERT_EQ(kHeight, image.height());
    const int x = (int)std::floor(strip.valueToX(0.38));
    // The base is at y = 25.
    const QColor mid = image.pixelColor(x, 15);
    const QColor nearBase = image.pixelColor(x, 24);
    EXPECT_NEAR(mid.red(), nearBase.red(), 2);
    EXPECT_NEAR(mid.green(), nearBase.green(), 2);
    EXPECT_NEAR(mid.blue(), nearBase.blue(), 2);
}

TEST(KnobGuiTrapezoid, WritesGrabPngs)
{
    struct Case {
        const char* name;
        KnobDouble::RangeWidgetRoleEnum role;
        ColorMath::KeyModelEnum model;
        double values[kDims];
        double zoom;
    };
    const Case cases[] = {
        { "HueHSV", KnobDouble::eRangeWidgetRoleHue, ColorMath::eKeyModelHSV, { 0., 0.15, 0.05, 0.05, 0.15 }, 1. },
        { "SatHSL", KnobDouble::eRangeWidgetRoleSaturation, ColorMath::eKeyModelHSL, { 0.5, 0.4, 0.2, 0.2, 0.4 }, 1. },
        { "LumaOkLCh", KnobDouble::eRangeWidgetRoleLuma, ColorMath::eKeyModelOkLCh, { 0.6, 0.4, 0.2, 0.2, 0.8 }, 1. },
        { "HueOkLCh", KnobDouble::eRangeWidgetRoleHue, ColorMath::eKeyModelOkLCh, { 0.4, 0.1, 0.05, 0.05, 0.1 }, 1. },
        { "HueOkLChZoomedOut", KnobDouble::eRangeWidgetRoleHue, ColorMath::eKeyModelOkLCh, { 0.4, 0.1, 0.05, 0.05, 0.1 }, 3. },
    };
    for (const Case& c : cases) {
        TrapezoidStrip strip(c.role);
        setUp(strip, c.values[0], c.values[1], c.values[2], c.values[3], c.values[4], c.model);
        if (c.zoom != 1.) {
            strip.zoomAround(kWidth * 0.5, c.zoom);
        }
        // The project's display conversion is absent here, so the strip encodes sRGB itself.
        strip.setDisplayConverter(TrapezoidStrip::DisplayConverter());
        const QString path = outputDirectory() + QString::fromUtf8("/KnobGuiTrapezoid_") + QString::fromUtf8(c.name) + QString::fromUtf8(".png");
        EXPECT_TRUE(strip.grab().save(path)) << path.toStdString();
    }
}
