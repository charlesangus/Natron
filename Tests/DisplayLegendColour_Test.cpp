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
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/ColorWheelMath.h"

#include "Gui/DisplayLegendColour.h"

NATRON_NAMESPACE_USING

namespace {
const double kPi = 3.14159265358979323846;

void
srgbEncodeInPlace(float* rgb,
                  int count)
{
    for (int i = 0; i < 3 * count; ++i) {
        rgb[i] = (float)srgbEncode(rgb[i]);
    }
}

double
signedRadians(double d)
{
    d = std::fmod(d, 2.0 * kPi);
    if (d > kPi) {
        d -= 2.0 * kPi;
    } else if (d <= -kPi) {
        d += 2.0 * kPi;
    }

    return d;
}
} // anonymous namespace

TEST(DisplayLegendColour, OkLabRoundTripsLinearSrgb)
{
    const double colours[4][3] = { { 0.18, 0.18, 0.18 }, { 1.0, 0.0, 0.0 }, { 0.05, 0.6, 0.9 }, { 0.7, 0.3, 0.02 } };

    for (const auto& c : colours) {
        double back[3];

        okLabToLinearSrgb(linearSrgbToOkLab(c), back);
        for (int i = 0; i < 3; ++i) {
            EXPECT_NEAR(c[i], back[i], 1e-6);
        }
    }

    const double white[3] = { 1.0, 1.0, 1.0 };
    const OkLab w = linearSrgbToOkLab(white);

    EXPECT_NEAR(1.0, w.L, 1e-4);
    EXPECT_NEAR(0.0, w.a, 1e-4);
    EXPECT_NEAR(0.0, w.b, 1e-4);
}

TEST(DisplayLegendColour, SrgbTransferRoundTrips)
{
    for (double x : { 0.0, 0.001, 0.0031308, 0.18, 0.5, 1.0 }) {
        EXPECT_NEAR(x, srgbDecode(srgbEncode(x)), 1e-9);
    }
    EXPECT_DOUBLE_EQ(1.0, srgbEncode(2.0));
    EXPECT_DOUBLE_EQ(0.0, srgbEncode(-1.0));
}

TEST(DisplayLegendColour, MidGreyIsTheValueDiscsCentre)
{
    const double grey[3] = { kColorPivotGrey, kColorPivotGrey, kColorPivotGrey };

    EXPECT_NEAR(0.565, linearSrgbToOkLab(grey).L, 0.002);
    EXPECT_NEAR(0.4614, srgbEncode(kColorPivotGrey), 0.0005);
}

TEST(DisplayLegendColour, GamutMapKeepsAColourInsideAndReducesChromaOnlyForOneOutside)
{
    double rgb[3];

    // Well inside: kept as asked.
    okLchToGamutLinearSrgb(0.7, 0.05, 1.0, rgb);
    {
        const OkLab lab = linearSrgbToOkLab(rgb);

        EXPECT_NEAR(0.7, lab.L, 1e-4);
        EXPECT_NEAR(0.05, std::hypot(lab.a, lab.b), 1e-4);
        EXPECT_NEAR(1.0, std::atan2(lab.b, lab.a), 1e-3);
    }

    // Far outside: in [0, 1], same lightness and hue, less chroma.
    const double hue = 4.0;

    okLchToGamutLinearSrgb(0.8, 0.4, hue, rgb);
    for (int i = 0; i < 3; ++i) {
        EXPECT_GE(rgb[i], 0.0);
        EXPECT_LE(rgb[i], 1.0);
    }

    const OkLab lab = linearSrgbToOkLab(rgb);
    const double C = std::hypot(lab.a, lab.b);

    EXPECT_NEAR(0.8, lab.L, 1e-3);
    EXPECT_LT(C, 0.4);
    EXPECT_GT(C, 0.02);
    EXPECT_NEAR(0.0, signedRadians(std::atan2(lab.b, lab.a) - hue), 0.01);
}

TEST(DisplayLegendColour, EveryLegendHueKeepsMostOfItsChroma)
{
    for (int d = 0; d < 360; d += 5) {
        const double h = d * kPi / 180.0;
        double rgb[3];

        okLchToGamutLinearSrgb(displayLegendHueLightness(h), kDisplayLegendChroma, h, rgb);

        const OkLab lab = linearSrgbToOkLab(rgb);

        EXPECT_NEAR(displayLegendHueLightness(h), lab.L, 1e-3) << d;
        EXPECT_GT(std::hypot(lab.a, lab.b), 0.12) << d;
    }
}

TEST(DisplayLegendColour, HuesAreReadThroughTheConverterAndDecoded)
{
    std::vector<float> rgb = { 0.18f, 0.18f, 0.18f, 0.6f, 0.1f, 0.05f, 0.05f, 0.2f, 0.7f };
    const std::vector<double> plain = displayLegendHues(rgb, DisplayLegendConverter());
    const std::vector<double> encoded = displayLegendHues(rgb, &srgbEncodeInPlace);

    ASSERT_EQ(3u, plain.size());
    ASSERT_EQ(3u, encoded.size());
    for (int k = 1; k < 3; ++k) {
        const double c[3] = { rgb[3 * k], rgb[3 * k + 1], rgb[3 * k + 2] };
        const OkLab lab = linearSrgbToOkLab(c);
        const double expected = std::atan2(lab.b, lab.a);

        EXPECT_NEAR(0.0, signedRadians(plain[k] - expected), 1e-6) << k;
        // An sRGB display decodes what the converter encoded, so the hue is the linear one.
        EXPECT_NEAR(0.0, signedRadians(encoded[k] - expected), 1e-3) << k;
    }

    // A converter that swaps red and blue turns the hue the display reads.
    const std::vector<double> swapped = displayLegendHues(rgb, [](float* p, int count) {
        for (int k = 0; k < count; ++k) {
            std::swap(p[3 * k], p[3 * k + 2]);
            for (int i = 0; i < 3; ++i) {
                p[3 * k + i] = (float)srgbEncode(p[3 * k + i]);
            }
        }
    });

    EXPECT_GT(std::abs(signedRadians(swapped[1] - encoded[1])), 1.0);
}
