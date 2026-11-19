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

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "Engine/Nodes/Color/ColorWarp/ColorWarpSpace.h"

NATRON_NAMESPACE_USING

namespace {
const double kRec709ToXYZ[9] = {
    0.4124564, 0.3575761, 0.1804375,
    0.2126729, 0.7151522, 0.0721750,
    0.0193339, 0.1191920, 0.9503041
};

const double kAP1ToXYZ[9] = {
    0.6624541811, 0.1340042065, 0.1561876870,
    0.2722287168, 0.6740817658, 0.0536895174,
    -0.0055746495, 0.0040607335, 1.0103391003
};

std::vector<std::vector<double>>
samples()
{
    return {
        { 0.18, 0.18, 0.18 },
        { 0.5, 0.25, 0.1 },
        { 0.9, 0.05, 0.6 },
        { 1., 0., 0. },
        { 0., 0., 0. },
        { -0.05, -0.05, -0.05 },
        { -0.05, 0.3, 0.7 },
        { 0.002, 0.004, 0.001 },
        { 64., 64., 64. },
        { 64., 12., 0.5 },
        { 16., -0.05, 3. },
    };
}

void
expectNear3(const double* a, const double* b, double tol)
{
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(a[c], b[c], tol * std::max(1., std::fabs(b[c]))) << "channel " << c;
    }
}
} // anon namespace

TEST(ColorWarpSpace, LogEncodingRoundTripsAndIsContinuousAtTheToe)
{
    for (double x : { -0.05, -1e-6, 0., 1e-4, kColorWarpLogToe, 0.01, 0.18, 1., 64., 1e4 }) {
        EXPECT_NEAR(colorWarpLogDecode(colorWarpLogEncode(x)), x, 1e-12 * std::max(1., std::fabs(x))) << x;
    }
    const double below = colorWarpLogEncode(kColorWarpLogToe * (1. - 1e-9));
    const double above = colorWarpLogEncode(kColorWarpLogToe * (1. + 1e-9));
    EXPECT_NEAR(below, std::log2(kColorWarpLogToe), 1e-8);
    EXPECT_NEAR(above, std::log2(kColorWarpLogToe), 1e-8);
    EXPECT_LT(colorWarpLogEncode(-0.05), colorWarpLogEncode(0.));
    EXPECT_DOUBLE_EQ(colorWarpLogEncode(0.18), std::log2(0.18));
}

TEST(ColorWarpSpace, EverySpaceRoundTripsInRangeNegativeAndHdr)
{
    for (const double* matrix : { kRec709ToXYZ, kAP1ToXYZ }) {
        const ColorWarpSpaceBasis basis(matrix);
        ASSERT_TRUE(basis.valid);
        for (int s = 0; s < kColorWarpSpaceCount; ++s) {
            const ColorWarpSpaceEnum space = static_cast<ColorWarpSpaceEnum>(s);
            for (const std::vector<double>& rgb : samples()) {
                double warp[3], back[3];
                toWarpSpace(space, basis, rgb.data(), warp);
                fromWarpSpace(space, basis, warp, back);
                SCOPED_TRACE(colorWarpSpaceName(space));
                expectNear3(back, rgb.data(), 1e-9);
            }
        }
    }
}

TEST(ColorWarpSpace, OklabLogKeepsNeutralsAchromaticAndChromaExposureInvariant)
{
    const ColorWarpSpaceBasis basis(kRec709ToXYZ);
    double grey[3], brightGrey[3];
    const double g1[3] = { 0.18, 0.18, 0.18 };
    const double g2[3] = { 11.52, 11.52, 11.52 };
    toWarpSpace(eColorWarpSpaceOklabLog, basis, g1, grey);
    toWarpSpace(eColorWarpSpaceOklabLog, basis, g2, brightGrey);
    EXPECT_NEAR(grey[1], 0., 1e-3);
    EXPECT_NEAR(grey[2], 0., 1e-3);
    EXPECT_NEAR(brightGrey[0] - grey[0], 6., 1e-6);

    const double c1[3] = { 0.4, 0.1, 0.05 };
    const double c2[3] = { 6.4, 1.6, 0.8 };
    double w1[3], w2[3];
    toWarpSpace(eColorWarpSpaceOklabLog, basis, c1, w1);
    toWarpSpace(eColorWarpSpaceOklabLog, basis, c2, w2);
    EXPECT_NEAR(w2[0] - w1[0], 4., 1e-6);
    EXPECT_NEAR(w2[1], w1[1], 1e-6);
    EXPECT_NEAR(w2[2], w1[2], 1e-6);
}

TEST(ColorWarpSpace, HsvRoundTripsSceneLinearColours)
{
    for (const std::vector<double>& rgb : samples()) {
        double hsv[3], back[3];
        colorWarpRgbToHsv(rgb.data(), hsv);
        if (std::max(rgb[0], std::max(rgb[1], rgb[2])) <= 0.) {
            EXPECT_EQ(hsv[1], 0.);
            continue;
        }
        colorWarpHsvToRgb(hsv, back);
        expectNear3(back, rgb.data(), 1e-12);
    }
}

TEST(ColorWarpSpace, HsvOffsetReproducesAHueRotationOnASaturatedColour)
{
    const ColorWarpSpaceBasis basis(kRec709ToXYZ);
    const double red[3] = { 1., 0., 0. };
    const double green[3] = { 0., 1., 0. };
    const double rotate[3] = { 120., 0., 0. };
    double shift[3];
    shiftFromHsvRgbOffset(basis, eColorWarpOffsetHsv, red, rotate, shift);

    double redXyz[3], warped[3], landed[3];
    toWarpSpace(eColorWarpSpaceXyz, basis, red, redXyz);
    for (int c = 0; c < 3; ++c) {
        warped[c] = redXyz[c] + shift[c];
    }
    fromWarpSpace(eColorWarpSpaceXyz, basis, warped, landed);
    expectNear3(landed, green, 1e-9);

    double offset[3];
    hsvRgbOffsetFromShift(basis, eColorWarpOffsetHsv, red, shift, offset);
    EXPECT_NEAR(offset[0], 120., 1e-7);
    EXPECT_NEAR(offset[1], 0., 1e-9);
    EXPECT_NEAR(offset[2], 0., 1e-9);

    const double back[3] = { -150., 0., 0. };
    shiftFromHsvRgbOffset(basis, eColorWarpOffsetHsv, red, back, shift);
    hsvRgbOffsetFromShift(basis, eColorWarpOffsetHsv, red, shift, offset);
    EXPECT_NEAR(offset[0], -150., 1e-7);
}

TEST(ColorWarpSpace, HsvAndRgbOffsetsRoundTripOnHdrSources)
{
    const ColorWarpSpaceBasis basis(kAP1ToXYZ);
    const double source[3] = { 12., 4., 1.5 };
    const double hsvOffset[3] = { 25., -0.2, 3. };
    const double rgbOffset[3] = { -2., 1., 0.25 };
    double shift[3], offset[3];

    shiftFromHsvRgbOffset(basis, eColorWarpOffsetHsv, source, hsvOffset, shift);
    hsvRgbOffsetFromShift(basis, eColorWarpOffsetHsv, source, shift, offset);
    expectNear3(offset, hsvOffset, 1e-9);

    shiftFromHsvRgbOffset(basis, eColorWarpOffsetRgb, source, rgbOffset, shift);
    hsvRgbOffsetFromShift(basis, eColorWarpOffsetRgb, source, shift, offset);
    expectNear3(offset, rgbOffset, 1e-9);
}

TEST(ColorWarpSpace, SingularMatrixFallsBackWithoutNaN)
{
    const double singular[9] = { 1., 1., 1., 1., 1., 1., 0., 0., 1. };
    const ColorWarpSpaceBasis basis(singular);
    EXPECT_FALSE(basis.valid);
    const double rgb[3] = { 0.3, 0.2, 0.1 };
    double warp[3], back[3];
    toWarpSpace(eColorWarpSpaceOklabLog, basis, rgb, warp);
    fromWarpSpace(eColorWarpSpaceOklabLog, basis, warp, back);
    expectNear3(back, rgb, 1e-9);
}
