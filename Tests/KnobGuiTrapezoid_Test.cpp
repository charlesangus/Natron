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
#include <QMouseEvent>
#include <QPointF>
#include <QString>

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

struct Recorder {
    std::vector<TrapezoidStrip::GesturePhaseEnum> phases;
    std::vector<std::vector<double>> values;

    TrapezoidStrip::EditCallback callback()
    {
        return [this](const double* v, TrapezoidStrip::GesturePhaseEnum phase) {
            phases.push_back(phase);
            values.push_back(std::vector<double>(v, v + 4));
        };
    }
};

void
identity(float* /*rgb*/,
         int /*count*/)
{
}

void
setUp(TrapezoidStrip& strip,
      double a,
      double b,
      double c,
      double d,
      ColorMath::KeyModelEnum model = ColorMath::eKeyModelHSV)
{
    const double values[4] = { a, b, c, d };
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
          Qt::MouseButtons buttons,
          Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    const QPointF pos(x, kHeight * 0.5);
    QMouseEvent e(type, pos, strip.mapToGlobal(pos), Qt::LeftButton, buttons, modifiers);

    QApplication::sendEvent(&strip, &e);
}

// Presses at x, moves by each of dxs in turn and releases.
void
drag(TrapezoidStrip& strip,
     double x,
     const std::vector<double>& dxs,
     Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    sendMouse(strip, QEvent::MouseButtonPress, x, Qt::LeftButton, modifiers);
    double at = x;
    for (double dx : dxs) {
        at += dx;
        sendMouse(strip, QEvent::MouseMove, at, Qt::LeftButton, modifiers);
    }
    sendMouse(strip, QEvent::MouseButtonRelease, at, Qt::NoButton, modifiers);
}

void
expectValues(const TrapezoidStrip& strip,
             double a,
             double b,
             double c,
             double d,
             double tolerance = 1e-9)
{
    double v[4];

    strip.getValues(v);
    EXPECT_NEAR(a, v[0], tolerance);
    EXPECT_NEAR(b, v[1], tolerance);
    EXPECT_NEAR(c, v[2], tolerance);
    EXPECT_NEAR(d, v[3], tolerance);
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

TEST(KnobGuiTrapezoid, SaturationDomainCoversZeroToOneAndTheRange)
{
    TrapezoidStrip strip(KnobDouble::eRangeWidgetRoleSaturation);
    double low, high;

    setUp(strip, 0.1, 0.3, 0.6, 0.8);
    strip.getDomain(&low, &high);
    EXPECT_DOUBLE_EQ(0., low);
    EXPECT_DOUBLE_EQ(1., high);
    EXPECT_NEAR(4., strip.valueToX(0.), 1e-9);
    EXPECT_NEAR(4. + kSpan, strip.valueToX(1.), 1e-9);
    EXPECT_NEAR(0.25, strip.xToValue(4. + kSpan * 0.25), 1e-9);

    setUp(strip, -0.5, 0.3, 0.6, 2.);
    strip.getDomain(&low, &high);
    EXPECT_DOUBLE_EQ(-0.5, low);
    EXPECT_DOUBLE_EQ(2., high);

    // OkLCh chroma is drawn to 0.4.
    setUp(strip, 0.01, 0.05, 0.1, 0.2, ColorMath::eKeyModelOkLCh);
    strip.getDomain(&low, &high);
    EXPECT_DOUBLE_EQ(0., low);
    EXPECT_DOUBLE_EQ(0.4, high);
}

// A hue range across red is drawn unsplit, one turn centred on its plateau.
TEST(KnobGuiTrapezoid, HueDomainIsCentredOnThePlateau)
{
    TrapezoidStrip strip(KnobDouble::eRangeWidgetRoleHue);
    double low, high;

    setUp(strip, 0.9, 0.95, 0.05, 0.1);
    strip.getDomain(&low, &high);
    EXPECT_NEAR(0.5, low, 1e-9);
    EXPECT_NEAR(1.5, high, 1e-9);
    double p[4];
    strip.getPositions(p);
    EXPECT_NEAR(0.9, p[0], 1e-9);
    EXPECT_NEAR(0.95, p[1], 1e-9);
    EXPECT_NEAR(1.05, p[2], 1e-9);
    EXPECT_NEAR(1.1, p[3], 1e-9);
    EXPECT_NEAR(4. + kSpan * 0.5, (strip.valueToX(p[1]) + strip.valueToX(p[2])) * 0.5, 1e-9);
}

TEST(KnobGuiTrapezoid, PressFindsHandlesAndThePlateau)
{
    TrapezoidStrip strip(KnobDouble::eRangeWidgetRoleLuma);

    setUp(strip, 0.1, 0.3, 0.6, 0.8);
    EXPECT_EQ(TrapezoidStrip::ePartOuterLow, strip.partAt(strip.valueToX(0.1) + 2.));
    EXPECT_EQ(TrapezoidStrip::ePartInnerLow, strip.partAt(strip.valueToX(0.3)));
    EXPECT_EQ(TrapezoidStrip::ePartInnerHigh, strip.partAt(strip.valueToX(0.6) - 3.));
    EXPECT_EQ(TrapezoidStrip::ePartOuterHigh, strip.partAt(strip.valueToX(0.8)));
    EXPECT_EQ(TrapezoidStrip::ePartPlateau, strip.partAt(strip.valueToX(0.45)));
    EXPECT_EQ(TrapezoidStrip::ePartNone, strip.partAt(strip.valueToX(0.95)));
}

// A handle drag reports start, moves and end, and stops at its neighbours.
TEST(KnobGuiTrapezoid, HandleDragIsClampedBetweenNeighbours)
{
    TrapezoidStrip strip(KnobDouble::eRangeWidgetRoleLuma);
    Recorder recorder;

    setUp(strip, 0.1, 0.3, 0.6, 0.8);
    strip.setEditCallback(recorder.callback());
    drag(strip, strip.valueToX(0.3), { 10., 10. });
    expectValues(strip, 0.1, 0.4, 0.6, 0.8);
    ASSERT_EQ(4u, recorder.phases.size());
    EXPECT_EQ(TrapezoidStrip::eGesturePhaseStart, recorder.phases[0]);
    EXPECT_EQ(TrapezoidStrip::eGesturePhaseMove, recorder.phases[1]);
    EXPECT_EQ(TrapezoidStrip::eGesturePhaseMove, recorder.phases[2]);
    EXPECT_EQ(TrapezoidStrip::eGesturePhaseEnd, recorder.phases[3]);
    EXPECT_NEAR(0.4, recorder.values[3][1], 1e-9);
    EXPECT_FALSE(strip.isDragging());

    // Far past the inner high point, it stops there.
    drag(strip, strip.valueToX(0.4), { 150. });
    expectValues(strip, 0.1, 0.6, 0.6, 0.8);

    // A click with no move edits nothing.
    recorder.phases.clear();
    drag(strip, strip.valueToX(0.1), {});
    EXPECT_TRUE(recorder.phases.empty());
}

TEST(KnobGuiTrapezoid, PlateauDragMovesAllFour)
{
    TrapezoidStrip strip(KnobDouble::eRangeWidgetRoleSaturation);

    setUp(strip, 0.1, 0.3, 0.6, 0.8);
    drag(strip, strip.valueToX(0.45), { -20. });
    expectValues(strip, 0., 0.2, 0.5, 0.7);
}

TEST(KnobGuiTrapezoid, ShiftDragMovesTheOuterPartner)
{
    TrapezoidStrip strip(KnobDouble::eRangeWidgetRoleLuma);

    setUp(strip, 0.1, 0.3, 0.6, 0.8);
    drag(strip, strip.valueToX(0.6), { 20. }, Qt::ShiftModifier);
    expectValues(strip, 0.1, 0.3, 0.7, 0.9);
    drag(strip, strip.valueToX(0.3), { -20. }, Qt::ShiftModifier);
    expectValues(strip, 0., 0.2, 0.7, 0.9);
    // The pair stops when the inner point meets the other inner one.
    drag(strip, strip.valueToX(0.2), { 150. }, Qt::ShiftModifier);
    expectValues(strip, 0.5, 0.7, 0.7, 0.9);
}

// Coincident handles: a move to the left takes the lowest, to the right the highest.
TEST(KnobGuiTrapezoid, CoincidentHandlesFollowTheDragDirection)
{
    TrapezoidStrip strip(KnobDouble::eRangeWidgetRoleLuma);

    setUp(strip, 0.1, 0.4, 0.4, 0.8);
    drag(strip, strip.valueToX(0.4), { 20. });
    expectValues(strip, 0.1, 0.4, 0.5, 0.8);

    setUp(strip, 0.1, 0.4, 0.4, 0.8);
    drag(strip, strip.valueToX(0.4), { -20. });
    expectValues(strip, 0.1, 0.3, 0.4, 0.8);

    // All four at one place.
    setUp(strip, 0.5, 0.5, 0.5, 0.5);
    drag(strip, strip.valueToX(0.5), { -20. });
    expectValues(strip, 0.4, 0.5, 0.5, 0.5);
    setUp(strip, 0.5, 0.5, 0.5, 0.5);
    drag(strip, strip.valueToX(0.5), { 20. });
    expectValues(strip, 0.5, 0.5, 0.5, 0.6);
}

// Hue points are written wrapped, and the outer points stay within a turn.
TEST(KnobGuiTrapezoid, HueDragWrapsAndStaysWithinATurn)
{
    TrapezoidStrip strip(KnobDouble::eRangeWidgetRoleHue);

    setUp(strip, 0.9, 0.95, 0.05, 0.1);
    // One turn is 200 pixels: the plateau moves 0.15 turn up, across red.
    drag(strip, strip.valueToX(1.), { 30. });
    expectValues(strip, 0.05, 0.1, 0.2, 0.25, 1e-9);

    setUp(strip, 0.2, 0.3, 0.4, 0.5);
    // The outer low point is dragged far down, and stops a turn below the outer high one.
    drag(strip, strip.valueToX(0.2), { -100., -100., -100. });
    double v[4];
    strip.getValues(v);
    EXPECT_NEAR(0.5, v[0], 1e-5);
    EXPECT_GE(v[0], 0.);
    EXPECT_LT(v[0], 1.);
    double p[4];
    strip.getPositions(p);
    EXPECT_LT(p[3] - p[0], 1.);
}

TEST(KnobGuiTrapezoid, WritesGrabPngs)
{
    struct Case {
        const char* name;
        KnobDouble::RangeWidgetRoleEnum role;
        ColorMath::KeyModelEnum model;
        double values[4];
    };
    const Case cases[] = {
        { "HueHSV", KnobDouble::eRangeWidgetRoleHue, ColorMath::eKeyModelHSV, { 0.85, 0.95, 0.05, 0.15 } },
        { "SatHSL", KnobDouble::eRangeWidgetRoleSaturation, ColorMath::eKeyModelHSL, { 0.1, 0.3, 0.7, 0.9 } },
        { "LumaOkLCh", KnobDouble::eRangeWidgetRoleLuma, ColorMath::eKeyModelOkLCh, { 0.2, 0.4, 0.8, 1.4 } },
        { "HueOkLCh", KnobDouble::eRangeWidgetRoleHue, ColorMath::eKeyModelOkLCh, { 0.3, 0.35, 0.45, 0.5 } },
    };
    for (const Case& c : cases) {
        TrapezoidStrip strip(c.role);
        setUp(strip, c.values[0], c.values[1], c.values[2], c.values[3], c.model);
        // The project's display conversion is absent here, so the strip encodes sRGB itself.
        strip.setDisplayConverter(TrapezoidStrip::DisplayConverter());
        const QString path = outputDirectory() + QString::fromUtf8("/KnobGuiTrapezoid_") + QString::fromUtf8(c.name) + QString::fromUtf8(".png");
        EXPECT_TRUE(strip.grab().save(path)) << path.toStdString();
    }
}
