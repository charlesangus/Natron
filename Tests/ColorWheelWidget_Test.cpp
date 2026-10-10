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
#include <cstdlib>

#include <QColor>
#include <QDir>
#include <QImage>
#include <QPixmap>
#include <QPointF>
#include <QPolygonF>
#include <QString>

#include <gtest/gtest.h>

#include "Engine/ColorWheelMath.h"

#include "Gui/ColorWheelWidget.h"

NATRON_NAMESPACE_USING

namespace {
const double kAP1Weights[3] = { 0.2722287168, 0.6740817658, 0.0536895174 };
const double kAP1ToXYZ[9] = {
    0.6624541811, 0.1340042065, 0.1561876870,
    0.2722287168, 0.6740817658, 0.0536895174,
    -0.0055746495, 0.0040607335, 1.0103391003
};
const int kWidgetSize = 160;
const double kPi = 3.14159265358979323846;

LumaBasis
acescg()
{
    return LumaBasis(kAP1Weights, kAP1ToXYZ);
}

KnobColorValue
makeValue(double r,
          double g,
          double b)
{
    KnobColorValue v;

    v.rgb[0] = r;
    v.rgb[1] = g;
    v.rgb[2] = b;
    v.a = 1.0;
    v.hasAlpha = true;

    return v;
}

PlanePoint
planePoint(double x,
           double y)
{
    PlanePoint p;

    p.x = x;
    p.y = y;
    p.valid = true;

    return p;
}

void
identity(float* /*rgb*/,
         int /*count*/)
{
}

void
setUpWheel(ColorWheelWidget& wheel,
           ColorKnobRoleEnum role,
           const KnobColorValue& value,
           double rim)
{
    wheel.setDisplayConverter(&identity);
    wheel.setBasis(acescg());
    wheel.setRole(role);
    wheel.setValue(value);
    wheel.setRim(rim);
    wheel.resize(kWidgetSize, kWidgetSize);
    wheel.show();
}

double
screenAngleDegrees(const QPointF& from,
                   const QPointF& to)
{
    double degrees = std::atan2(-(to.y() - from.y()), to.x() - from.x()) * 180.0 / kPi;

    return degrees < 0.0 ? degrees + 360.0 : degrees;
}

double
angleDifference(double a,
                double b)
{
    double d = std::fmod(std::fabs(a - b), 360.0);

    return std::min(d, 360.0 - d);
}

double
distance(const QPointF& a,
         const QPointF& b)
{
    return std::hypot(a.x() - b.x(), a.y() - b.y());
}

void
expectAchromatic(QRgb pixel)
{
    EXPECT_LT(std::abs(qRed(pixel) - qGreen(pixel)), 2);
    EXPECT_LT(std::abs(qGreen(pixel) - qBlue(pixel)), 2);
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

void
saveGrab(ColorWheelWidget& wheel,
         const char* name)
{
    QString path = outputDirectory() + QString::fromUtf8("/ColorWheelWidget_") + QString::fromUtf8(name) + QString::fromUtf8(".png");

    EXPECT_TRUE(wheel.grab().save(path)) << path.toStdString();
}
} // namespace

TEST(ColorWheelWidget, NeutralCentrePixelIsAchromatic)
{
    ColorWheelWidget wheel;

    setUpWheel(wheel, eColorKnobRoleGain, makeValue(1.0, 1.0, 1.0), TrackballRates().wheelRim);

    QImage disc = wheel.getDiscImage();

    ASSERT_FALSE(disc.isNull());
    QRgb centre = disc.pixel(disc.width() / 2, disc.height() / 2);

    EXPECT_EQ(255, qAlpha(centre));
    expectAchromatic(centre);

    QImage grabbed = wheel.grab().toImage();

    ASSERT_FALSE(grabbed.isNull());
    expectAchromatic(grabbed.pixel(grabbed.width() / 2, grabbed.height() / 2));
}

TEST(ColorWheelWidget, OffsetNeutralCentrePixelIsAchromatic)
{
    ColorWheelWidget wheel;

    setUpWheel(wheel, eColorKnobRoleOffset, makeValue(0.0, 0.0, 0.0), TrackballRates().wheelRim);

    QImage disc = wheel.getDiscImage();

    ASSERT_FALSE(disc.isNull());
    expectAchromatic(disc.pixel(disc.width() / 2, disc.height() / 2));
}

TEST(ColorWheelWidget, OffDiscWhiteMarkerCaretPointsAtWhite)
{
    ColorWheelWidget wheel;
    KnobColorValue value = makeValue(2.0, 1.0, 1.0);

    setUpWheel(wheel, eColorKnobRoleGain, value, 0.01);

    DiscClamp marker = wheel.getWhiteMarker();

    EXPECT_TRUE(marker.offDisc);

    QPolygonF caret = wheel.getWhiteMarkerCaret();

    ASSERT_EQ(3, caret.size());

    PlanePoint white = whitePoint(toPlane(eColorKnobRoleGain, acescg(), value));
    double expected = planeHue(white);
    QPointF centre = wheel.getDiscCentre();

    EXPECT_LT(angleDifference(screenAngleDegrees(centre, caret[0]), expected), 2.0);
    EXPECT_LT(angleDifference(screenAngleDegrees(centre, wheel.getWhiteMarkerPosition()), expected), 2.0);
    EXPECT_LE(distance(centre, caret[0]), wheel.getDiscRadius() + 0.5);

    saveGrab(wheel, "OffDiscWhite");
}

TEST(ColorWheelWidget, OnDiscWhiteMarkerHasNoCaret)
{
    ColorWheelWidget wheel;

    setUpWheel(wheel, eColorKnobRoleGain, makeValue(1.0, 1.0, 1.0), 0.05);

    EXPECT_FALSE(wheel.getWhiteMarker().offDisc);
    EXPECT_TRUE(wheel.getWhiteMarkerCaret().isEmpty());
    EXPECT_LT(distance(wheel.getWhiteMarkerPosition(), wheel.getDiscCentre()), 0.5);
}

TEST(ColorWheelWidget, ParkedPuckReportsOffDisc)
{
    ColorWheelWidget wheel;
    double rim = 0.05;

    setUpWheel(wheel, eColorKnobRoleGain, makeValue(1.0, 1.0, 1.0), rim);

    EXPECT_FALSE(wheel.getPuck().offDisc);
    EXPECT_LT(distance(wheel.getPuckPosition(), wheel.getDiscCentre()), 0.5);

    wheel.setDragOffset(planePoint(0.5 * rim, 0.0));
    EXPECT_FALSE(wheel.getPuck().offDisc);

    wheel.setDragOffset(planePoint(-3.0 * rim, 2.0 * rim));

    DiscClamp puck = wheel.getPuck();

    EXPECT_TRUE(puck.offDisc);
    EXPECT_NEAR(wheel.getDiscRadius(), distance(wheel.getPuckPosition(), wheel.getDiscCentre()), 0.5);
    EXPECT_LT(angleDifference(screenAngleDegrees(wheel.getDiscCentre(), wheel.getPuckPosition()), planeHue(planePoint(-3.0, 2.0))), 1.0);

    saveGrab(wheel, "ParkedPuck");

    wheel.setValue(makeValue(1.1, 1.0, 0.9));
    EXPECT_FALSE(wheel.hasDragOffset());
    EXPECT_FALSE(wheel.getPuck().offDisc);
}

TEST(ColorWheelWidget, GamutTriangleVerticesMatchMaths)
{
    const KnobColorValue values[2] = { makeValue(1.0, 1.0, 1.0), makeValue(1.2, 1.0, 0.8) };

    for (int k = 0; k < 2; ++k) {
        ColorWheelWidget wheel;
        double rim = 0.5;

        setUpWheel(wheel, eColorKnobRoleGain, values[k], rim);

        GamutTriangle triangle = gamutTriangle(acescg());

        ASSERT_TRUE(triangle.valid);

        PlanePoint anchor = toPlane(eColorKnobRoleGain, acescg(), values[k]);
        QPolygonF drawn = wheel.getGamutTrianglePolygon();

        ASSERT_EQ(3, drawn.size());

        QPointF centre = wheel.getDiscCentre();
        double scale = wheel.getDiscRadius() / rim;

        for (int i = 0; i < 3; ++i) {
            QPointF expected(centre.x() + (triangle.vertex[i].x - anchor.x) * scale,
                             centre.y() - (triangle.vertex[i].y - anchor.y) * scale);

            EXPECT_LT(distance(drawn[i], expected), 1.0) << "value " << k << " vertex " << i;
        }
    }
}

TEST(ColorWheelWidget, WritesGrabPngs)
{
    {
        ColorWheelWidget wheel;

        setUpWheel(wheel, eColorKnobRoleGain, makeValue(1.0, 1.0, 1.0), TrackballRates().wheelRim);
        saveGrab(wheel, "GainNeutral");
    }
    {
        ColorWheelWidget wheel;

        setUpWheel(wheel, eColorKnobRoleGain, makeValue(1.0, 1.0, 1.0), 0.5);
        saveGrab(wheel, "GainWideRim");
    }
    {
        ColorWheelWidget wheel;

        setUpWheel(wheel, eColorKnobRoleGain, makeValue(1.3, 1.0, 0.7), 0.2);
        saveGrab(wheel, "GainWarmAnchor");
    }
    {
        ColorWheelWidget wheel;

        setUpWheel(wheel, eColorKnobRoleOffset, makeValue(0.02, 0.0, -0.01), 0.1);
        saveGrab(wheel, "OffsetAxes");
    }
}
