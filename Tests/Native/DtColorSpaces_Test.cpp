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
#include <array>
#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Color/DtColorSpaces.h"

NATRON_NAMESPACE_USING

namespace {
using namespace DtColorSpaces;

constexpr float kRoundTrip = 1e-5f;

// Linear Rec.709 to CIE XYZ D65.
constexpr Mat3 kRec709ToXyz = { { { 0.4124564f, 0.3575761f, 0.1804375f },
                                  { 0.2126729f, 0.7151522f, 0.0721750f },
                                  { 0.0193339f, 0.1191920f, 0.9503041f } } };

constexpr float kD65Xyz[3] = { 0.95047f, 1.f, 1.08883f };

std::vector<std::array<float, 3>>
sampleXyz()
{
    std::vector<std::array<float, 3>> out;
    const float rgbs[][3] = {
        { 1.f, 1.f, 1.f },
        { 0.18f, 0.18f, 0.18f },
        { 1.f, 0.f, 0.f },
        { 0.f, 1.f, 0.f },
        { 0.f, 0.f, 1.f },
        { 0.8f, 0.4f, 0.1f },
        { 0.05f, 0.3f, 0.7f },
        { 2.5f, 1.2f, 0.6f },
        { 0.01f, 0.02f, 0.015f },
        { 0.6f, 0.2f, 0.9f },
    };
    for (const auto& rgb : rgbs) {
        std::array<float, 3> xyz;
        apply(kRec709ToXyz, rgb, xyz.data());
        out.push_back(xyz);
    }

    return out;
}

void
expectNearRel(const float* a,
              const float* b,
              int n,
              float rel,
              const char* what)
{
    float scale = 1e-3f;
    for (int i = 0; i < n; ++i) {
        scale = std::max(scale, std::fabs(b[i]));
    }
    for (int i = 0; i < n; ++i) {
        EXPECT_NEAR(a[i], b[i], rel * scale) << what << " component " << i;
    }
}
} // anonymous namespace

TEST(DtColorSpaces, XyzLmsRoundTrip)
{
    for (const auto& xyz : sampleXyz()) {
        float lms[3];
        float back[3];
        xyzToLms(xyz.data(), lms);
        lmsToXyz(lms, back);
        expectNearRel(back, xyz.data(), 3, kRoundTrip, "XYZ<->LMS");
    }
}

TEST(DtColorSpaces, GradingRgbLmsRoundTrip)
{
    for (const auto& xyz : sampleXyz()) {
        float lms[3];
        float rgb[3];
        float back[3];
        xyzToLms(xyz.data(), lms);
        lmsToGradingRgb(lms, rgb);
        gradingRgbToLms(rgb, back);
        expectNearRel(back, lms, 3, kRoundTrip, "gradingRGB<->LMS");
    }
}

TEST(DtColorSpaces, YrgYchRoundTrip)
{
    for (const auto& xyz : sampleXyz()) {
        float lms[3];
        float yrg[3];
        float ych[4];
        float yrgBack[3];
        float lmsBack[3];
        xyzToLms(xyz.data(), lms);
        lmsToYrg(lms, yrg);
        yrgToYch(yrg, ych);
        ychToYrg(ych, yrgBack);
        expectNearRel(yrgBack, yrg, 3, kRoundTrip, "Yrg<->Ych");
        yrgToLms(yrgBack, lmsBack);
        expectNearRel(lmsBack, lms, 3, kRoundTrip, "LMS<->Yrg");
    }
}

// darktable's Yrg_to_Ych subtracts the Yrg chromaticity of the white; D65 itself lands there.
TEST(DtColorSpaces, D65WhiteInYrg)
{
    float lms[3];
    float yrg[3];
    xyzToLms(kD65Xyz, lms);
    lmsToYrg(lms, yrg);
    EXPECT_NEAR(yrg[1], kYrgWhiteR, 1e-4f);
    EXPECT_NEAR(yrg[2], kYrgWhiteG, 1e-4f);

    float ych[4];
    yrgToYch(yrg, ych);
    EXPECT_LT(ych[1], 1e-4f);

    // The grading-RGB norm colour_balance_rgb's commit_params builds is the white at Y = 1.
    float norm[3];
    const float ychNorm[4] = { 1.f, 0.f, 1.f, 0.f };
    ychToGradingRgb(ychNorm, norm);
    float lmsNorm[3];
    float yrgNorm[3];
    gradingRgbToLms(norm, lmsNorm);
    lmsToYrg(lmsNorm, yrgNorm);
    EXPECT_NEAR(yrgNorm[0], 1.f, kRoundTrip);
    EXPECT_NEAR(yrgNorm[1], kYrgWhiteR, kRoundTrip);
    EXPECT_NEAR(yrgNorm[2], kYrgWhiteG, kRoundTrip);
}

TEST(DtColorSpaces, GamutCheckYrgKeepsInsideAndClipsOutside)
{
    float inside[4];
    makeYch(0.5f, 0.05f, 1.f, inside);
    const float before = inside[1];
    gamutCheckYrg(inside);
    EXPECT_EQ(inside[1], before);

    float outside[4];
    makeYch(0.5f, 10.f, 3.5f, outside);
    gamutCheckYrg(outside);
    EXPECT_LT(outside[1], 10.f);
    float yrg[3];
    ychToYrg(outside, yrg);
    EXPECT_GE(yrg[1], -1e-6f);
    EXPECT_GE(yrg[2], -1e-6f);
    EXPECT_LE(yrg[1] + yrg[2], 1.f + 1e-6f);
}

// The p = 134 exponent of JzAzBz's perceptual quantiser amplifies single-precision rounding to
// a few 1e-5, so the round trip is held to 1e-4.
TEST(DtColorSpaces, JzAzBzRoundTrip)
{
    for (const auto& xyz : sampleXyz()) {
        float jab[3];
        float back[3];
        xyzToJzAzBz(xyz.data(), jab);
        jzAzBzToXyz(jab, back);
        expectNearRel(back, xyz.data(), 3, 1e-4f, "XYZ<->JzAzBz");
    }
}

// Values from the JzAzBz formulas in double precision: the D65 white is achromatic.
TEST(DtColorSpaces, JzAzBzOfD65)
{
    float jab[3];
    xyzToJzAzBz(kD65Xyz, jab);
    EXPECT_NEAR(jab[0], 1.75800492e-02f, 2e-6f);
    EXPECT_NEAR(jab[1], 0.f, 1e-4f);
    EXPECT_NEAR(jab[2], 0.f, 1e-4f);

    const float grey[3] = { 0.18f * kD65Xyz[0], 0.18f * kD65Xyz[1], 0.18f * kD65Xyz[2] };
    xyzToJzAzBz(grey, jab);
    EXPECT_NEAR(jab[0], 5.86344233e-03f, 1e-6f);
}

TEST(DtColorSpaces, XyYRoundTrip)
{
    for (const auto& xyz : sampleXyz()) {
        float xyY[3];
        float back[3];
        d65XyzToXyY(xyz.data(), xyY);
        xyYToXyz(xyY, back);
        expectNearRel(back, xyz.data(), 3, kRoundTrip, "XYZ<->xyY");
    }

    const float black[3] = { 0.f, 0.f, 0.f };
    float xyY[3];
    d65XyzToXyY(black, xyY);
    EXPECT_EQ(xyY[0], kD65x);
    EXPECT_EQ(xyY[1], kD65y);
    EXPECT_EQ(xyY[2], 0.f);
}

TEST(DtColorSpaces, UcsLightness)
{
    // Y = 1 in double precision.
    EXPECT_NEAR(yToUcsLStar(1.f), 0.98805049f, 1e-6f);
    EXPECT_NEAR(yToUcsLStar(0.18f), 0.48573255f, 1e-6f);
    for (float y : { 0.001f, 0.18f, 1.f, 4.f, 50.f }) {
        EXPECT_NEAR(ucsLStarToY(yToUcsLStar(y)), y, kRoundTrip * std::max(1.f, y));
    }
}

TEST(DtColorSpaces, UcsJCHRoundTrip)
{
    const float lWhite = yToUcsLStar(1.f);
    for (const auto& xyz : sampleXyz()) {
        float xyY[3];
        float jch[3];
        float back[3];
        d65XyzToXyY(xyz.data(), xyY);
        xyYToUcsJCH(xyY, lWhite, jch);
        ucsJCHToXyY(jch, lWhite, back);
        EXPECT_NEAR(back[0], xyY[0], kRoundTrip);
        EXPECT_NEAR(back[1], xyY[1], kRoundTrip);
        EXPECT_NEAR(back[2], xyY[2], kRoundTrip * std::max(1.f, xyY[2]));

        float hsb[3];
        float hcb[3];
        float jchBack[3];
        ucsJCHToHSB(jch, hsb);
        ucsHSBToJCH(hsb, jchBack);
        expectNearRel(jchBack, jch, 3, kRoundTrip, "JCH<->HSB");
        ucsJCHToHCB(jch, hcb);
        ucsHCBToJCH(hcb, jchBack);
        expectNearRel(jchBack, jch, 3, kRoundTrip, "JCH<->HCB");
    }
}

TEST(DtColorSpaces, UcsD65IsAchromatic)
{
    const float d65[3] = { kD65x, kD65y, 1.f };
    float uv[2];
    xyYToUcsUV(d65, uv);
    EXPECT_NEAR(uv[0], 0.f, 1e-5f);
    EXPECT_NEAR(uv[1], 0.f, 1e-5f);
}

TEST(DtColorSpaces, HelperFunctions)
{
    EXPECT_NEAR(deltaH(3.f, -3.f), 6.f - kTwoPi, 1e-6f);
    EXPECT_NEAR(deltaH(-3.f, 3.f), kTwoPi - 6.f, 1e-6f);
    EXPECT_NEAR(deltaH(0.5f, 0.25f), 0.25f, 1e-7f);

    EXPECT_EQ(softClip(0.5f, 0.8f, 1.f), 0.5f);
    EXPECT_NEAR(softClip(1.f, 0.8f, 1.f), 0.8f + 0.2f * (1.f - std::exp(-1.f)), 1e-6f);
    // Far above the soft threshold the exponential underflows the float mantissa, so it lands on the hard threshold.
    EXPECT_GT(softClip(5.f, 0.8f, 1.f), 0.8f);
    EXPECT_LE(softClip(5.f, 0.8f, 1.f), 1.f);

    float lut[kGamutLutSize];
    for (int i = 0; i < kGamutLutSize; ++i) {
        lut[i] = (float)i;
    }
    // Entry i sits at hue -pi + i·2pi/512, and the ring wraps from the last entry to the first.
    EXPECT_NEAR(lookupGamut(lut, -kPi + 10.5f * kTwoPi / kGamutLutSize), 10.5f, 1e-3f);
    EXPECT_NEAR(lookupGamut(lut, -kPi + 511.5f * kTwoPi / kGamutLutSize), 255.5f, 1e-2f);
}

TEST(DtColorSpaces, GamutLuts)
{
    float jzLut[kGamutLutSize];
    float ucsLut[kGamutLutSize];
    buildJzAzBzGamutLut(kRec709ToXyz, jzLut);
    buildUcsGamutLut(kRec709ToXyz, ucsLut);
    for (int i = 0; i < kGamutLutSize; ++i) {
        EXPECT_GT(jzLut[i], 0.f) << i;
        EXPECT_GE(ucsLut[i], 0.f) << i;
    }

    // Each primary lies on the gamut boundary. The JzAzBz LUT holds the largest saturation over
    // all intensities of a hue, box-filtered, so it may exceed a primary's; the UCS LUT holds the
    // mean colourfulness of the boundary samples falling in each hue entry.
    for (int p = 0; p < 3; ++p) {
        const float xyz[3] = { kRec709ToXyz.m[0][p], kRec709ToXyz.m[1][p], kRec709ToXyz.m[2][p] };
        float jab[3];
        xyzToJzAzBz(xyz, jab);
        const float sat = fastHypot(jab[1], jab[2]) / jab[0];
        const float jzBound = lookupGamut(jzLut, std::atan2(jab[2], jab[1]));
        EXPECT_GT(jzBound, 0.9f * sat) << "primary " << p;
        EXPECT_LT(jzBound, 1.3f * sat) << "primary " << p;

        float xyY[3];
        float uv[2];
        d65XyzToXyY(xyz, xyY);
        xyYToUcsUV(xyY, uv);
        const float m2 = uv[0] * uv[0] + uv[1] * uv[1];
        EXPECT_NEAR(lookupGamut(ucsLut, std::atan2(uv[1], uv[0])), m2, 0.1f * m2) << "primary " << p;
    }
}
