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
#include <QPainterPath>
#include <QPointF>
#include <QPolygonF>
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

// The knob's values in dimension order: outer low, inner low, centre, inner high, outer high.
void
setUp(TrapezoidStrip& strip,
      double outerLow,
      double innerLow,
      double centre,
      double innerHigh,
      double outerHigh,
      ColorMath::KeyModelEnum model = ColorMath::eKeyModelHSV)
{
    const double values[kDims] = { outerLow, innerLow, centre, innerHigh, outerHigh };
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
             double outerLow,
             double innerLow,
             double centre,
             double innerHigh,
             double outerHigh,
             double tolerance = 1e-9)
{
    const double want[kDims] = { outerLow, innerLow, centre, innerHigh, outerHigh };
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

// Hue shows one turn centred on the centre; saturation and luma show 0 to 1 in every model,
// widened by a margin of 5% of the span on a side where a point lies outside.
TEST(KnobGuiTrapezoid, DefaultDomains)
{
    TrapezoidStrip hue(KnobDouble::eRangeWidgetRoleHue);
    setUp(hue, 0.05, 0.05, 0.3, 0.05, 0.05);
    expectDomain(hue, -0.2, 0.8);

    TrapezoidStrip sat(KnobDouble::eRangeWidgetRoleSaturation);
    setUp(sat, 0.2, 0.2, 0.5, 0.2, 0.2);
    expectDomain(sat, 0., 1.);
    EXPECT_NEAR(4., sat.valueToX(0.), 1e-9);
    EXPECT_NEAR(4. + kSpan, sat.valueToX(1.), 1e-9);
    EXPECT_NEAR(0.25, sat.xToValue(4. + kSpan * 0.25), 1e-9);
    setUp(sat, 0.02, 0.02, 0.05, 0.02, 0.08, ColorMath::eKeyModelOkLCh);
    expectDomain(sat, 0., 1.);
    // The outer high point at 1.4.
    setUp(sat, 0.2, 0.2, 0.5, 0.2, 0.7);
    expectDomain(sat, 0., 1.47);

    TrapezoidStrip luma(KnobDouble::eRangeWidgetRoleLuma);
    setUp(luma, 0.05, 0.05, 0.2, 0.05, 0.25);
    expectDomain(luma, 0., 1.);
    // The default luma range reaches 2.
    setUp(luma, 0.05, 0.45, 0.5, 0.5, 1.);
    expectDomain(luma, 0., 2.1);
    // The outer low point at -0.2.
    setUp(luma, 0.3, 0.1, 0.2, 0.1, 0.1);
    expectDomain(luma, -0.26, 1.);
}

// The outer points lie beyond the inner ones by their own offsets.
TEST(KnobGuiTrapezoid, OuterOffsetsFromTheInnerEdges)
{
    TrapezoidStrip strip(KnobDouble::eRangeWidgetRoleSaturation);

    setUp(strip, 0.04, 0.14, 0.2, 0.2, 0.);
    double p[4];
    double centre = -1.;
    strip.getPositions(p, &centre);
    EXPECT_NEAR(0.2, centre, 1e-9);
    EXPECT_NEAR(0.02, p[0], 1e-9);
    EXPECT_NEAR(0.06, p[1], 1e-9);
    EXPECT_NEAR(0.4, p[2], 1e-9);
    EXPECT_NEAR(0.4, p[3], 1e-9);

    // Offsets below 0 are drawn as 0.
    setUp(strip, -0.1, 0.1, 0.5, -0.2, 0.1);
    strip.getPositions(p);
    EXPECT_NEAR(0.4, p[0], 1e-9);
    EXPECT_NEAR(0.4, p[1], 1e-9);
    EXPECT_NEAR(0.5, p[2], 1e-9);
    EXPECT_NEAR(0.6, p[3], 1e-9);
}

// A hue range across red is drawn unsplit around its centre.
TEST(KnobGuiTrapezoid, HuePositionsAroundTheCentre)
{
    TrapezoidStrip strip(KnobDouble::eRangeWidgetRoleHue);

    setUp(strip, 0.05, 0.05, 0., 0.05, 0.05);
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
    setUp(strip, 3., 0.05, 0.5, 0.05, 0.05);
    strip.getPositions(p);
    EXPECT_NEAR(-0.55, p[0], 1e-9);
}

TEST(KnobGuiTrapezoid, PressFindsHandlesAndThePlateau)
{
    TrapezoidStrip strip(KnobDouble::eRangeWidgetRoleLuma);

    // 0.1, 0.3, 0.6, 0.8.
    setUp(strip, 0.2, 0.15, 0.45, 0.15, 0.2);
    EXPECT_EQ(TrapezoidStrip::ePartOuterLow, strip.partAt(strip.valueToX(0.1) + 2.));
    EXPECT_EQ(TrapezoidStrip::ePartInnerLow, strip.partAt(strip.valueToX(0.3)));
    EXPECT_EQ(TrapezoidStrip::ePartInnerHigh, strip.partAt(strip.valueToX(0.6) - 3.));
    EXPECT_EQ(TrapezoidStrip::ePartOuterHigh, strip.partAt(strip.valueToX(0.8)));
    EXPECT_EQ(TrapezoidStrip::ePartPlateau, strip.partAt(strip.valueToX(0.45)));
    EXPECT_EQ(TrapezoidStrip::ePartNone, strip.partAt(strip.valueToX(0.95)));
}

// A handle drag writes its own offset only, an inner point carrying its outer one with it,
// reports start, moves and end, and stops at the centre (inner) or at its inner point (outer).
TEST(KnobGuiTrapezoid, HandleDragIsClampedAndCarriesTheOuterPoint)
{
    TrapezoidStrip strip(KnobDouble::eRangeWidgetRoleLuma);
    Recorder recorder;

    setUp(strip, 0.2, 0.15, 0.45, 0.15, 0.2);
    strip.setEditCallback(recorder.callback());
    drag(strip, strip.valueToX(0.3), { 10., 10. });
    expectValues(strip, 0.2, 0.05, 0.45, 0.15, 0.2);
    double p[4];
    strip.getPositions(p);
    EXPECT_NEAR(0.2, p[0], 1e-9);
    EXPECT_NEAR(0.4, p[1], 1e-9);
    ASSERT_EQ(4u, recorder.phases.size());
    EXPECT_EQ(TrapezoidStrip::eGesturePhaseStart, recorder.phases[0]);
    EXPECT_EQ(TrapezoidStrip::eGesturePhaseMove, recorder.phases[1]);
    EXPECT_EQ(TrapezoidStrip::eGesturePhaseMove, recorder.phases[2]);
    EXPECT_EQ(TrapezoidStrip::eGesturePhaseEnd, recorder.phases[3]);
    EXPECT_NEAR(0.05, recorder.values[3][KnobDouble::eRangeWidgetDimensionInnerLow], 1e-9);
    EXPECT_FALSE(strip.isDragging());

    // Far past the centre, the inner low point stops there.
    drag(strip, strip.valueToX(0.4), { 150. });
    expectValues(strip, 0.2, 0., 0.45, 0.15, 0.2);
    // The inner high point stops at the centre coming down: 0.25, 0.45, 0.45, 0.65.
    drag(strip, strip.valueToX(0.6) + 2., { -150. });
    expectValues(strip, 0.2, 0., 0.45, 0., 0.2);
    // The outer high point stops at the inner high one.
    drag(strip, strip.valueToX(0.65), { -150. });
    expectValues(strip, 0.2, 0., 0.45, 0., 0.);
    // The outer low point moves out alone.
    drag(strip, strip.valueToX(0.25), { -20. });
    expectValues(strip, 0.3, 0., 0.45, 0., 0.);

    // A click with no move edits nothing.
    recorder.phases.clear();
    drag(strip, strip.valueToX(0.15), {});
    EXPECT_TRUE(recorder.phases.empty());
}

TEST(KnobGuiTrapezoid, PlateauDragMovesTheCentre)
{
    TrapezoidStrip strip(KnobDouble::eRangeWidgetRoleSaturation);

    setUp(strip, 0.2, 0.15, 0.45, 0.15, 0.2);
    drag(strip, strip.valueToX(0.4), { -20. });
    expectValues(strip, 0.2, 0.15, 0.35, 0.15, 0.2);
}

TEST(KnobGuiTrapezoid, ShiftDragLeavesTheOuterPointInPlace)
{
    TrapezoidStrip strip(KnobDouble::eRangeWidgetRoleLuma);

    // 0.1, 0.3, 0.6, 0.8.
    setUp(strip, 0.2, 0.15, 0.45, 0.15, 0.2);
    drag(strip, strip.valueToX(0.6), { 20. }, Qt::ShiftModifier);
    expectValues(strip, 0.2, 0.15, 0.45, 0.25, 0.1);
    drag(strip, strip.valueToX(0.3), { -20. }, Qt::ShiftModifier);
    expectValues(strip, 0.1, 0.25, 0.45, 0.25, 0.1);
    // The inner point stops at its outer one, and at the centre.
    drag(strip, strip.valueToX(0.2), { -150. }, Qt::ShiftModifier);
    expectValues(strip, 0., 0.35, 0.45, 0.25, 0.1);
    drag(strip, strip.valueToX(0.7), { -150. }, Qt::ShiftModifier);
    expectValues(strip, 0., 0.35, 0.45, 0., 0.35);
}

// Coincident handles: a move to the left takes the lowest, to the right the highest.
TEST(KnobGuiTrapezoid, CoincidentHandlesFollowTheDragDirection)
{
    TrapezoidStrip strip(KnobDouble::eRangeWidgetRoleLuma);

    // 0.1, 0.4, 0.4, 0.8.
    setUp(strip, 0.3, 0., 0.4, 0., 0.4);
    drag(strip, strip.valueToX(0.4), { 20. });
    expectValues(strip, 0.3, 0., 0.4, 0.1, 0.4);

    setUp(strip, 0.3, 0., 0.4, 0., 0.4);
    drag(strip, strip.valueToX(0.4), { -20. });
    expectValues(strip, 0.3, 0.1, 0.4, 0., 0.4);

    // All four at the centre.
    setUp(strip, 0., 0., 0.5, 0., 0.);
    drag(strip, strip.valueToX(0.5), { -20. });
    expectValues(strip, 0.1, 0., 0.5, 0., 0.);
    setUp(strip, 0., 0., 0.5, 0., 0.);
    drag(strip, strip.valueToX(0.5), { 20. });
    expectValues(strip, 0., 0., 0.5, 0., 0.1);
}

// The hue centre is written wrapped, and each offset stays within a turn.
TEST(KnobGuiTrapezoid, HueDragWrapsAndStaysWithinATurn)
{
    TrapezoidStrip strip(KnobDouble::eRangeWidgetRoleHue);

    setUp(strip, 0.05, 0.05, 0.9, 0.05, 0.05);
    // One turn is 200 pixels: the plateau moves 0.15 turn up, across red.
    drag(strip, strip.valueToX(0.9), { 30. });
    expectValues(strip, 0.05, 0.05, 0.05, 0.05, 0.05);

    // 0.2, 0.3, 0.4, 0.5.
    setUp(strip, 0.1, 0.05, 0.35, 0.05, 0.1);
    // The outer points are dragged far out, and stop a turn from their inner points.
    drag(strip, strip.valueToX(0.2), { -100., -100., -100. });
    drag(strip, strip.valueToX(0.5), { 100., 100., 100. });
    expectValues(strip, 1., 0.05, 0.35, 0.05, 1.);
    double p[4];
    double centre;
    strip.getPositions(p, &centre);
    EXPECT_NEAR(1.05, centre - p[0], 1e-9);
    EXPECT_NEAR(1.05, p[3] - centre, 1e-9);
}

// The wheel zooms around the cursor, a middle or Alt drag pans, and resetZoom() restores the
// default domain. Neither edits the values.
TEST(KnobGuiTrapezoid, ZoomAndPan)
{
    TrapezoidStrip strip(KnobDouble::eRangeWidgetRoleLuma);
    Recorder recorder;

    setUp(strip, 0.2, 0.15, 0.45, 0.15, 0.2);
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
    expectValues(strip, 0.2, 0.15, 0.45, 0.15, 0.2);
    EXPECT_TRUE(recorder.phases.empty());

    // Reset Zoom takes in a point outside 0 to 1.
    setUp(strip, 0.05, 0.45, 0.5, 0.5, 1.);
    strip.zoomAround(strip.valueToX(0.5), 0.25);
    strip.resetZoom();
    expectDomain(strip, 0., 2.1);

    // Zoomed in, a handle drag moves the value by the zoomed scale.
    setUp(strip, 0.2, 0.15, 0.45, 0.15, 0.2);
    strip.resetZoom();
    strip.zoomAround(strip.valueToX(0.3), 0.5);
    drag(strip, strip.valueToX(0.3), { 20. });
    expectValues(strip, 0.2, 0.1, 0.45, 0.15, 0.2);
}

// The trapezoid's base is open: just above it the fill reads as it does mid-shape.
TEST(KnobGuiTrapezoid, NoLineAlongTheBase)
{
    TrapezoidStrip strip(KnobDouble::eRangeWidgetRoleLuma);

    // 0.1, 0.3, 0.6, 0.8; the source tick is at 0.5 and the centre notch at the top.
    setUp(strip, 0.2, 0.15, 0.45, 0.15, 0.2);
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

// The height of an outline above its base at x, as a share of the trapezoid's height, from the
// path flattened into line segments; -1 where the outline does not cross x.
static double
outlineHeightAt(const QPainterPath& outline,
                double x)
{
    const QList<QPolygonF> polygons = outline.toSubpathPolygons();
    if (polygons.isEmpty() || polygons.front().size() < 2) {
        return -1.;
    }
    const QPolygonF& poly = polygons.front();
    const QRectF bounds = outline.boundingRect();
    const double base = poly.front().y();
    const double height = base - bounds.top();
    if (!(height > 0.)) {
        return -1.;
    }
    for (int i = 1; i < poly.size(); ++i) {
        const QPointF& a = poly[i - 1];
        const QPointF& b = poly[i];
        if ((x >= a.x()) && (x <= b.x()) && (b.x() > a.x())) {
            const double t = (x - a.x()) / (b.x() - a.x());

            return (base - (a.y() + t * (b.y() - a.y()))) / height;
        }
    }

    return -1.;
}

// Smooth draws the falloffs as smoothsteps: a quarter of the way along the rising edge the
// outline is at 0.15625 of the height, not 0.25.
TEST(KnobGuiTrapezoid, SmoothCurvesTheFalloffs)
{
    TrapezoidStrip strip(KnobDouble::eRangeWidgetRoleLuma);

    // 0.1, 0.3, 0.6, 0.8.
    setUp(strip, 0.2, 0.15, 0.45, 0.15, 0.2);
    strip.resize(kWidth, 120);
    EXPECT_FALSE(strip.isSmooth());
    const QPainterPath linear = strip.outlinePath();
    strip.setSmooth(true);
    EXPECT_TRUE(strip.isSmooth());
    const QPainterPath smooth = strip.outlinePath();

    // The flattening of a cubic is within a fraction of a pixel of the curve.
    const double tolerance = 0.01;
    const double rising = strip.valueToX(0.15);
    EXPECT_NEAR(0.25, outlineHeightAt(linear, rising), tolerance);
    EXPECT_NEAR(0.15625, outlineHeightAt(smooth, rising), tolerance);
    const double falling = strip.valueToX(0.75);
    EXPECT_NEAR(0.25, outlineHeightAt(linear, falling), tolerance);
    EXPECT_NEAR(0.15625, outlineHeightAt(smooth, falling), tolerance);
    // Three quarters up the rising edge the smoothstep is above the line.
    const double upper = strip.valueToX(0.25);
    EXPECT_NEAR(0.75, outlineHeightAt(linear, upper), tolerance);
    EXPECT_NEAR(0.84375, outlineHeightAt(smooth, upper), tolerance);
    // The plateau is drawn alike.
    const double plateau = strip.valueToX(0.4);
    EXPECT_NEAR(1., outlineHeightAt(linear, plateau), 1e-9);
    EXPECT_NEAR(1., outlineHeightAt(smooth, plateau), 1e-9);
}

TEST(KnobGuiTrapezoid, WritesGrabPngs)
{
    struct Case {
        const char* name;
        KnobDouble::RangeWidgetRoleEnum role;
        ColorMath::KeyModelEnum model;
        double values[kDims];
        double zoom;
        bool smooth;
    };
    const Case cases[] = {
        { "HueHSV", KnobDouble::eRangeWidgetRoleHue, ColorMath::eKeyModelHSV, { 0.1, 0.05, 0., 0.05, 0.1 }, 1., false },
        { "SatHSL", KnobDouble::eRangeWidgetRoleSaturation, ColorMath::eKeyModelHSL, { 0.2, 0.2, 0.5, 0.2, 0.2 }, 1., false },
        { "LumaOkLCh", KnobDouble::eRangeWidgetRoleLuma, ColorMath::eKeyModelOkLCh, { 0.2, 0.2, 0.6, 0.2, 0.6 }, 1., false },
        { "LumaOkLChSmooth", KnobDouble::eRangeWidgetRoleLuma, ColorMath::eKeyModelOkLCh, { 0.2, 0.2, 0.6, 0.2, 0.6 }, 1., true },
        { "HueOkLCh", KnobDouble::eRangeWidgetRoleHue, ColorMath::eKeyModelOkLCh, { 0.05, 0.05, 0.4, 0.05, 0.05 }, 1., false },
        { "HueOkLChZoomedOut", KnobDouble::eRangeWidgetRoleHue, ColorMath::eKeyModelOkLCh, { 0.05, 0.05, 0.4, 0.05, 0.05 }, 3., false },
    };
    for (const Case& c : cases) {
        TrapezoidStrip strip(c.role);
        setUp(strip, c.values[0], c.values[1], c.values[2], c.values[3], c.values[4], c.model);
        strip.setSmooth(c.smooth);
        if (c.zoom != 1.) {
            strip.zoomAround(kWidth * 0.5, c.zoom);
        }
        // The project's display conversion is absent here, so the strip encodes sRGB itself.
        strip.setDisplayConverter(TrapezoidStrip::DisplayConverter());
        const QString path = outputDirectory() + QString::fromUtf8("/KnobGuiTrapezoid_") + QString::fromUtf8(c.name) + QString::fromUtf8(".png");
        EXPECT_TRUE(strip.grab().save(path)) << path.toStdString();
    }
}
