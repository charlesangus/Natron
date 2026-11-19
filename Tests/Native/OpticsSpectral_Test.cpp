/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
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

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

#include <OpenColorIO/OpenColorIO.h>

#include "Engine/Optics/Spectral.h"

using namespace Natron::Optics;

namespace {

Xyz
integrate(const std::string& illuminant)
{
    Xyz sum = { 0., 0., 0. };
    for (int nm = int(kSpectralMinNm); nm <= int(kSpectralMaxNm); ++nm) {
        const Xyz xyz = spectralXyz(illuminant, nm);
        for (int k = 0; k < 3; ++k) {
            sum[k] += xyz[k];
        }
    }
    return sum;
}

} // namespace

TEST(OpticsSpectral, FiveWavelengthsSitAtTheMidBins)
{
    const std::vector<double> nm = sampleWavelengths(5);
    ASSERT_EQ(nm.size(), 5u);
    const double expected[5] = { 424., 492., 560., 628., 696. };
    for (int i = 0; i < 5; ++i) {
        EXPECT_NEAR(nm[i], expected[i], 1e-9);
    }
    EXPECT_TRUE(sampleWavelengths(0).empty());
}

TEST(OpticsSpectral, EveryNamedIlluminantIsPresent)
{
    const std::vector<std::string> names = illuminantNames();
    EXPECT_EQ(names.size(), 20u);
    for (const char* name : { "A", "B", "C", "D50", "D55", "D60", "D65", "D75", "E", "FL1", "FL11" }) {
        EXPECT_TRUE(hasIlluminant(name)) << name;
    }
    EXPECT_FALSE(hasIlluminant("FL12"));
    EXPECT_EQ(illuminantPower("FL12", 550.), 0.);
}

TEST(OpticsSpectral, TablesMatchHandReadValues)
{
    const Xyz at390 = colourMatching(390.);
    EXPECT_NEAR(at390[0], 3.769647e-03, 1e-12);
    EXPECT_NEAR(at390[1], 4.146161e-04, 1e-12);
    EXPECT_NEAR(at390[2], 1.847260e-02, 1e-12);
    const Xyz between = colourMatching(390.5);
    EXPECT_NEAR(between[0], 0.5 * (3.769647e-03 + 4.532416e-03), 1e-12);
    const Xyz below = colourMatching(300.);
    EXPECT_EQ(below[2], at390[2]);
    EXPECT_NEAR(illuminantPower("B", 320.), 0.02, 1e-12);
    EXPECT_NEAR(illuminantPower("B", 322.5), 0.5 * (0.02 + 0.26), 1e-12);
    EXPECT_NEAR(illuminantPower("E", 500.), 100., 1e-12);
}

TEST(OpticsSpectral, EqualEnergyIntegratesToEqualXyz)
{
    const Xyz e = integrate("E");
    EXPECT_NEAR(e[0] / e[1], 1., 1e-3);
    EXPECT_NEAR(e[2] / e[1], 1., 1e-3);
}

TEST(OpticsSpectral, FallbackMatrixTakesTheD65WhiteToRec709White)
{
    const Rgb white = applyMatrix(kXyzD65ToRec709, { 0.95047, 1., 1.08883 });
    for (double c : white) {
        EXPECT_NEAR(c, 1., 1e-4);
    }
}

TEST(OpticsSpectral, NoConfigFallsBackToRec709)
{
    XyzRoute route = XyzRoute::XyzInterchangeRole;
    EXPECT_EQ(xyzToWorkingSpace(nullptr, &route), kXyzD65ToRec709);
    EXPECT_EQ(route, XyzRoute::Rec709Fallback);
    EXPECT_EQ(xyzToWorkingSpace(OCIO_NAMESPACE::Config::CreateRaw(), &route), kXyzD65ToRec709);
    EXPECT_EQ(route, XyzRoute::Rec709Fallback);
}

TEST(OpticsSpectral, StudioConfigTakesTheD65WhiteToNeutral)
{
    OCIO_NAMESPACE::ConstConfigRcPtr config = OCIO_NAMESPACE::Config::CreateFromBuiltinConfig("studio-config-v4.0.0_aces-v2.0_ocio-v2.5");
    XyzRoute route = XyzRoute::Rec709Fallback;
    const Matrix3 m = xyzToWorkingSpace(config, &route);
    EXPECT_NE(route, XyzRoute::Rec709Fallback);
    const Rgb white = applyMatrix(m, { 0.95047, 1., 1.08883 });
    EXPECT_NEAR(white[0] / white[1], 1., 1e-3);
    EXPECT_NEAR(white[2] / white[1], 1., 1e-3);
    EXPECT_NEAR(white[1], 1., 1e-3);
}

TEST(OpticsSpectral, ColoursAreTheMatrixTimesTheWeightedMatchingFunctions)
{
    const std::vector<double> nm = sampleWavelengths(3);
    std::vector<Rgb> colours;
    ASSERT_TRUE(spectralColours("D65", nm, kXyzD65ToRec709, &colours));
    ASSERT_EQ(colours.size(), 3u);
    for (size_t i = 0; i < nm.size(); ++i) {
        const Xyz cmf = colourMatching(nm[i]);
        const double power = illuminantPower("D65", nm[i]) * 0.01;
        const Rgb expected = applyMatrix(kXyzD65ToRec709, { cmf[0] * power, cmf[1] * power, cmf[2] * power });
        for (int k = 0; k < 3; ++k) {
            EXPECT_NEAR(colours[i][k], expected[k], 1e-12);
        }
    }
    std::string error;
    EXPECT_FALSE(spectralColours("Z", nm, kXyzD65ToRec709, &colours, &error));
    EXPECT_EQ(error, "unknown illuminant \"Z\"");
    EXPECT_EQ(colours.size(), 3u);
}
