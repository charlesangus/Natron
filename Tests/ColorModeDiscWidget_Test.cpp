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

#include <QColor>
#include <QImage>
#include <QPixmap>
#include <QPointF>

#include <gtest/gtest.h>

#include "Engine/ColorWheelMath.h"

#include "Gui/ColorModeDiscWidget.h"
#include "Gui/ColorWheelWidget.h"

NATRON_NAMESPACE_USING

namespace {
const int kDiscSize = 240;

void
identity(float* /*rgb*/,
         int /*count*/)
{
}

QImage
renderMode(ColorModeDiscWidget& disc,
           ColorGlobalModeEnum mode)
{
    disc.setDisplayConverter(&identity);
    disc.setMode(mode);
    disc.resize(kDiscSize, kDiscSize);
    disc.show();

    return disc.grab().toImage();
}

// u runs right and v up, both in units of the disc radius.
QColor
sampleAt(const ColorModeDiscWidget& disc,
         const QImage& image,
         double u,
         double v)
{
    QPointF c = disc.getDiscCentre();
    double R = disc.getDiscRadius();
    double dpr = image.devicePixelRatio() > 0.0 ? image.devicePixelRatio() : 1.0;
    int x = (int)std::lround((c.x() + u * R) * dpr);
    int y = (int)std::lround((c.y() - v * R) * dpr);

    return QColor::fromRgb(image.pixel(x, y));
}

double
whiteness(const QColor& c)
{
    int hi = std::max(c.red(), std::max(c.green(), c.blue()));
    int lo = std::min(c.red(), std::min(c.green(), c.blue()));

    return hi > 0 ? 1.0 - (double)(hi - lo) / hi : 1.0;
}

double
hueDifference(const QColor& a,
              const QColor& b)
{
    double d = std::fmod(std::fabs(a.hsvHueF() - b.hsvHueF()) * 360.0, 360.0);

    return std::min(d, 360.0 - d);
}
} // anonymous namespace

TEST(ColorModeDiscWidget, SaturationDiscIsWhiterAtTheBottomAndHueRunsAcross)
{
    ColorModeDiscWidget disc;
    QImage image = renderMode(disc, eColorGlobalModeS);

    ASSERT_FALSE(image.isNull());
    EXPECT_GT(whiteness(sampleAt(disc, image, 0.0, -0.7)), whiteness(sampleAt(disc, image, 0.0, 0.7)) + 0.3);
    EXPECT_GT(hueDifference(sampleAt(disc, image, -0.6, 0.5), sampleAt(disc, image, 0.6, 0.5)), 30.0);
}

TEST(ColorModeDiscWidget, TemperatureDiscIsWarmerAtTheRightAndGreenerAtTheBottom)
{
    ColorModeDiscWidget disc;
    QImage image = renderMode(disc, eColorGlobalModeT);

    ASSERT_FALSE(image.isNull());

    QColor left = sampleAt(disc, image, -0.7, 0.0);
    QColor right = sampleAt(disc, image, 0.7, 0.0);
    QColor bottom = sampleAt(disc, image, 0.0, -0.7);
    QColor top = sampleAt(disc, image, 0.0, 0.7);

    EXPECT_GT(right.red() - right.blue(), left.red() - left.blue());
    EXPECT_GT(bottom.green() - (bottom.red() + bottom.blue()) / 2, top.green() - (top.red() + top.blue()) / 2);
}

TEST(ColorModeDiscWidget, ValueDiscIsLighterAtTheTopRight)
{
    ColorModeDiscWidget disc;
    QImage image = renderMode(disc, eColorGlobalModeV);

    ASSERT_FALSE(image.isNull());
    EXPECT_GT(sampleAt(disc, image, 0.6, 0.6).lightness(), sampleAt(disc, image, -0.6, -0.6).lightness() + 100);
}

TEST(ColorModeDiscWidget, PuckBeyondTheRimIsClampedOntoIt)
{
    ColorModeDiscWidget disc;
    QImage before = renderMode(disc, eColorGlobalModeV);

    disc.setPuck(QPointF(2.0, 2.0));

    EXPECT_NEAR(std::hypot(disc.getPuck().x(), disc.getPuck().y()), 1.0, 1e-9);

    QPointF c = disc.getDiscCentre();
    QPointF pos = disc.getPuckWidgetPos();

    EXPECT_NEAR(std::hypot(pos.x() - c.x(), pos.y() - c.y()), disc.getDiscRadius(), 1.0);
    EXPECT_GT(pos.x(), c.x());
    EXPECT_LT(pos.y(), c.y());

    QImage after = disc.grab().toImage();
    int x = (int)std::lround(pos.x());
    int y = (int)std::lround(pos.y());

    ASSERT_FALSE(after.isNull());

    bool changed = false;
    bool bright = false;

    for (int dy = -8; dy <= 8; ++dy) {
        for (int dx = -8; dx <= 8; ++dx) {
            changed = changed || (after.pixel(x + dx, y + dy) != before.pixel(x + dx, y + dy));
            bright = bright || (QColor::fromRgb(after.pixel(x + dx, y + dy)).lightness() >= 250);
        }
    }
    EXPECT_TRUE(changed);
    EXPECT_TRUE(bright);
}

TEST(ColorModeDiscWidget, PuckInsideTheRimIsKept)
{
    ColorModeDiscWidget disc;

    disc.setPuck(QPointF(0.25, -0.5));
    EXPECT_DOUBLE_EQ(disc.getPuck().x(), 0.25);
    EXPECT_DOUBLE_EQ(disc.getPuck().y(), -0.5);
}

TEST(ColorModeDiscWidget, RingIsTheWheelsRingInTheSamePlace)
{
    const LumaBasis basis(kRec709LumaWeights);
    ColorModeDiscWidget disc;
    ColorWheelWidget wheel;

    disc.setBasis(basis);

    QImage image = renderMode(disc, eColorGlobalModeS);

    wheel.resize(disc.size());
    EXPECT_DOUBLE_EQ(wheel.getRingOuterRadius(), disc.getRingOuterRadius());
    EXPECT_DOUBLE_EQ(wheel.getRingInnerRadius(), disc.getRingInnerRadius());
    EXPECT_DOUBLE_EQ(wheel.getDiscRadius(), disc.getDiscRadius());
    EXPECT_DOUBLE_EQ(colorWheelRingOuterRadius(disc.size()), disc.getRingOuterRadius());

    const double Ro = disc.getRingOuterRadius();
    const double Ri = disc.getRingInnerRadius();
    const double mid = 0.5 * (Ro + Ri);
    const double dpr = image.devicePixelRatio() > 0.0 ? image.devicePixelRatio() : 1.0;
    QImage ring = renderColorWheelRing(basis, Ro, Ri, dpr, &identity);
    QPointF c = disc.getDiscCentre();

    ASSERT_FALSE(ring.isNull());
    for (int k = 0; k < 8; ++k) {
        double angle = k * M_PI / 4.0 + 0.2;
        double dx = mid * std::cos(angle);
        double dy = -mid * std::sin(angle);
        QColor drawn = QColor::fromRgb(image.pixel((int)std::lround((c.x() + dx) * dpr), (int)std::lround((c.y() + dy) * dpr)));
        QColor shared = QColor::fromRgb(ring.pixel((int)std::lround((Ro + dx) * dpr), (int)std::lround((Ro + dy) * dpr)));

        EXPECT_NEAR(shared.red(), drawn.red(), 6) << "angle " << angle;
        EXPECT_NEAR(shared.green(), drawn.green(), 6) << "angle " << angle;
        EXPECT_NEAR(shared.blue(), drawn.blue(), 6) << "angle " << angle;
    }
}
