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

#include <QColor>
#include <QImage>
#include <QPixmap>
#include <QPointF>

#include <gtest/gtest.h>

#include "Engine/ColorWheelMath.h"

#include "Gui/ColorModeDiscWidget.h"
#include "Gui/ColorWheelWidget.h"
#include "Gui/DisplayLegendColour.h"

NATRON_NAMESPACE_USING

namespace {
const int kDiscSize = 240;

const double kRec709ToXYZ[9] = {
    0.4124564, 0.3575761, 0.1804375,
    0.2126729, 0.7151522, 0.0721750,
    0.0193339, 0.1191920, 0.9503041
};

void
identity(float* /*rgb*/,
         int /*count*/)
{
}

LumaBasis
rec709()
{
    return LumaBasis(kRec709LumaWeights, kRec709ToXYZ);
}

// An sRGB display of a linear Rec.709 working space.
void
srgbDisplay(float* rgb,
            int count)
{
    for (int i = 0; i < 3 * count; ++i) {
        rgb[i] = (float)srgbEncode(rgb[i]);
    }
}

QImage
renderMode(ColorModeDiscWidget& disc,
           ColorGlobalModeEnum mode,
           const ColorModeDiscWidget::DisplayConverter& converter = &identity)
{
    disc.setDisplayConverter(converter);
    disc.setMode(mode);
    disc.resize(kDiscSize, kDiscSize);
    disc.show();

    return disc.grab().toImage();
}

QColor
sampleAtPixel(const QImage& image,
              double x,
              double y)
{
    double dpr = image.devicePixelRatio() > 0.0 ? image.devicePixelRatio() : 1.0;

    return QColor::fromRgb(image.pixel((int)std::lround(x * dpr), (int)std::lround(y * dpr)));
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

    return sampleAtPixel(image, c.x() + u * R, c.y() - v * R);
}

// Midway across the ring, degrees counter-clockwise from the right.
QColor
rimAt(const ColorModeDiscWidget& disc,
      const QImage& image,
      double degrees)
{
    QPointF c = disc.getDiscCentre();
    double mid = 0.5 * (disc.getRingInnerRadius() + disc.getRingOuterRadius());
    double angle = degrees * M_PI / 180.0;

    return sampleAtPixel(image, c.x() + mid * std::cos(angle), c.y() - mid * std::sin(angle));
}

double
whiteness(const QColor& c)
{
    int hi = std::max(c.red(), std::max(c.green(), c.blue()));
    int lo = std::min(c.red(), std::min(c.green(), c.blue()));

    return hi > 0 ? 1.0 - (double)(hi - lo) / hi : 1.0;
}

double
signedDegrees(double d)
{
    d = std::fmod(d, 360.0);
    if (d > 180.0) {
        d -= 360.0;
    } else if (d <= -180.0) {
        d += 360.0;
    }

    return d;
}

OkLab
okLabOf(const QColor& c)
{
    const double rgb[3] = { srgbDecode(c.redF()), srgbDecode(c.greenF()), srgbDecode(c.blueF()) };

    return linearSrgbToOkLab(rgb);
}

double
okHueOf(const QColor& c)
{
    const OkLab lab = okLabOf(c);

    return std::atan2(lab.b, lab.a) * 180.0 / M_PI;
}

double
okChromaOf(const QColor& c)
{
    const OkLab lab = okLabOf(c);

    return std::hypot(lab.a, lab.b);
}

// The OKLab hue, in degrees, an sRGB display shows for a plane hue: the hue the S disc draws
// for a drag that turns the knob to it.
double
legendHueOf(const LumaBasis& basis,
            double planeDegrees)
{
    const ColorWheelHueTable table = buildColorWheelHueTable(basis);
    const int k = colorWheelHueIndex(planeDegrees);
    std::vector<float> rgb(table.rgb.begin() + 3 * k, table.rgb.begin() + 3 * k + 3);

    return displayLegendHues(rgb, &srgbDisplay)[0] * 180.0 / M_PI;
}

KnobColorValue
valueWithHue(const LumaBasis& basis,
             double degrees)
{
    double theta = degrees * M_PI / 180.0;
    PlanePoint q;

    q.x = 0.05 * std::cos(theta);
    q.y = 0.05 * std::sin(theta);
    q.valid = true;

    ChromaEditResult r = fromPlane(eColorKnobRoleGain, basis, neutralGainValue(), q, 1.0);

    EXPECT_TRUE(r.valid);

    return r.value;
}

// The smallest arc of HSV hue holding every chromatic sample: about 315 degrees for eight
// samples of a plain hue ring.
double
hueSpan(const std::vector<QColor>& samples)
{
    std::vector<double> hues;

    for (const QColor& c : samples) {
        if ((c.hsvHue() >= 0) && (c.hsvSaturation() > 20)) {
            hues.push_back(c.hsvHueF() * 360.0);
        }
    }
    if (hues.size() < 2) {
        return 0.0;
    }
    std::sort(hues.begin(), hues.end());

    double gap = hues.front() + 360.0 - hues.back();

    for (std::size_t k = 1; k < hues.size(); ++k) {
        gap = std::max(gap, hues[k] - hues[k - 1]);
    }

    return 360.0 - gap;
}

std::vector<QColor>
eightRimSamples(const ColorModeDiscWidget& disc,
                const QImage& image)
{
    std::vector<QColor> samples;

    for (int k = 0; k < 8; ++k) {
        samples.push_back(rimAt(disc, image, k * 45.0 + 10.0));
    }

    return samples;
}
} // anonymous namespace

TEST(ColorModeDiscWidget, SaturationRimRunsSixtyDegreesEitherSideOfTheHue)
{
    const LumaBasis basis = rec709();
    ColorModeDiscWidget disc;

    disc.setBasis(basis);
    disc.setCentreValue(eColorKnobRoleGain, valueWithHue(basis, 40.0));
    EXPECT_EQ(40, disc.getCentreHue());

    QImage image = renderMode(disc, eColorGlobalModeS, &srgbDisplay);

    ASSERT_FALSE(image.isNull());
    // A drag right turns the hue up, as globalModeEdit's S does for a positive dx.
    EXPECT_NEAR(0.0, signedDegrees(okHueOf(rimAt(disc, image, 180.0)) - legendHueOf(basis, -20.0)), 3.0);
    EXPECT_NEAR(0.0, signedDegrees(okHueOf(rimAt(disc, image, 0.0)) - legendHueOf(basis, 100.0)), 3.0);
    // The -Sat edge is neutral grey.
    EXPECT_GT(whiteness(rimAt(disc, image, 270.0)), 0.97);
    EXPECT_GT(whiteness(rimAt(disc, image, 270.0)), whiteness(rimAt(disc, image, 90.0)) + 0.3);
    EXPECT_GT(whiteness(sampleAt(disc, image, 0.0, -0.7)), whiteness(sampleAt(disc, image, 0.0, 0.7)) + 0.3);
}

TEST(ColorModeDiscWidget, SaturationRimFollowsTheLiveHue)
{
    const LumaBasis basis = rec709();
    ColorModeDiscWidget disc;

    disc.setBasis(basis);
    disc.setCentreValue(eColorKnobRoleGain, valueWithHue(basis, 40.0));
    renderMode(disc, eColorGlobalModeS, &srgbDisplay);

    disc.setCentreValue(eColorKnobRoleGain, valueWithHue(basis, 100.0));
    EXPECT_EQ(100, disc.getCentreHue());

    QImage after = disc.grab().toImage();

    ASSERT_FALSE(after.isNull());
    EXPECT_NEAR(0.0, signedDegrees(okHueOf(rimAt(disc, after, 180.0)) - legendHueOf(basis, 40.0)), 3.0);
    EXPECT_NEAR(0.0, signedDegrees(okHueOf(rimAt(disc, after, 0.0)) - legendHueOf(basis, 160.0)), 3.0);
}

TEST(ColorModeDiscWidget, NeutralValueKeepsTheLastDefinedHueElseZero)
{
    const LumaBasis basis = rec709();
    ColorModeDiscWidget disc;

    disc.setBasis(basis);
    disc.setCentreValue(eColorKnobRoleGain, valueWithHue(basis, 140.0));
    disc.setCentreValue(eColorKnobRoleGain, neutralGainValue());
    EXPECT_EQ(140, disc.getCentreHue());

    disc.forgetHue();
    disc.setCentreValue(eColorKnobRoleGain, neutralGainValue());
    EXPECT_EQ(0, disc.getCentreHue());
}

TEST(ColorModeDiscWidget, TemperatureRimIsWarmAtTheRightAndMagentaAtTheTop)
{
    ColorModeDiscWidget disc;

    disc.setBasis(rec709());

    QImage image = renderMode(disc, eColorGlobalModeT, &srgbDisplay);

    ASSERT_FALSE(image.isNull());

    QColor left = rimAt(disc, image, 180.0);
    QColor right = rimAt(disc, image, 0.0);
    QColor bottom = rimAt(disc, image, 270.0);
    QColor top = rimAt(disc, image, 90.0);

    EXPECT_GT(right.red() - right.blue(), left.red() - left.blue() + 60);
    EXPECT_GT(right.red(), right.blue());
    EXPECT_GT(left.blue(), left.red());
    EXPECT_GT(bottom.green() - (bottom.red() + bottom.blue()) / 2, top.green() - (top.red() + top.blue()) / 2 + 60);
    EXPECT_GT(top.red(), top.green());
    EXPECT_GT(top.blue(), top.green());

    QColor innerLeft = sampleAt(disc, image, -0.7, 0.0);
    QColor innerRight = sampleAt(disc, image, 0.7, 0.0);

    EXPECT_GT(innerRight.red() - innerRight.blue(), innerLeft.red() - innerLeft.blue());

    // Chroma stays within the legend's cap all the way round, and the centre is neutral.
    for (int k = 0; k < 16; ++k) {
        EXPECT_LT(okChromaOf(rimAt(disc, image, k * 22.5)), 0.16) << "degrees " << k * 22.5;
    }
    EXPECT_GT(whiteness(sampleAt(disc, image, 0.0, 0.0)), 0.97);
}

TEST(ColorModeDiscWidget, ValueRunsFromBlackThroughMidGreyToWhite)
{
    ColorModeDiscWidget disc;
    QImage image = renderMode(disc, eColorGlobalModeV, &srgbDisplay);

    ASSERT_FALSE(image.isNull());

    // The centre is mid grey as the display shows it.
    const int midGrey = (int)std::lround(srgbEncode(kColorPivotGrey) * 255.0);
    const QColor centre = sampleAt(disc, image, 0.0, 0.0);

    EXPECT_NEAR(midGrey, centre.red(), 3);
    EXPECT_NEAR(midGrey, centre.green(), 3);
    EXPECT_NEAR(midGrey, centre.blue(), 3);

    // The ends sit on the rim's diagonal.
    EXPECT_GE(rimAt(disc, image, 45.0).lightness(), 245);
    EXPECT_LE(rimAt(disc, image, 225.0).lightness(), 15);
    EXPECT_GT(rimAt(disc, image, 90.0).lightness(), midGrey + 60);
    EXPECT_GT(rimAt(disc, image, 0.0).lightness(), midGrey + 60);
    EXPECT_LT(rimAt(disc, image, 270.0).lightness(), midGrey - 60);
    EXPECT_LT(rimAt(disc, image, 180.0).lightness(), midGrey - 60);
    EXPECT_GT(sampleAt(disc, image, 0.6, 0.6).lightness(), sampleAt(disc, image, -0.6, -0.6).lightness() + 100);
    EXPECT_GT(whiteness(rimAt(disc, image, 45.0)), 0.97);
}

TEST(ColorModeDiscWidget, NoModeDrawsThePlainHueRing)
{
    const LumaBasis basis = rec709();

    for (ColorGlobalModeEnum mode : { eColorGlobalModeS, eColorGlobalModeT, eColorGlobalModeV }) {
        ColorModeDiscWidget disc;

        disc.setBasis(basis);
        disc.setCentreHue(200.0);

        QImage image = renderMode(disc, mode);

        ASSERT_FALSE(image.isNull());

        const double Ro = disc.getRingOuterRadius();
        const double dpr = image.devicePixelRatio() > 0.0 ? image.devicePixelRatio() : 1.0;
        QImage ring = renderColorWheelRing(basis, Ro, disc.getRingInnerRadius(), dpr, &identity);
        const double mid = 0.5 * (disc.getRingInnerRadius() + Ro);
        int differing = 0;

        for (int k = 0; k < 8; ++k) {
            double degrees = k * 45.0 + 10.0;
            QColor drawn = rimAt(disc, image, degrees);
            double angle = degrees * M_PI / 180.0;
            QColor wheel = sampleAtPixel(ring, Ro + mid * std::cos(angle), Ro - mid * std::sin(angle));
            int diff = std::abs(drawn.red() - wheel.red()) + std::abs(drawn.green() - wheel.green()) + std::abs(drawn.blue() - wheel.blue());

            differing += (diff > 60) ? 1 : 0;
        }
        EXPECT_GE(differing, 4) << "mode " << (int)mode;
        if (mode != eColorGlobalModeT) {
            // T's rim circles white, so it holds every hue; its layout is checked on its own.
            EXPECT_LE(hueSpan(eightRimSamples(disc, image)), 300.0) << "mode " << (int)mode;
        }
    }
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

    QImage image = renderMode(disc, eColorGlobalModeC);

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
