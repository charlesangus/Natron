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

const char* const kStudioConfig = "studio-config-v4.0.0_aces-v2.0_ocio-v2.5";

std::vector<double>
everyNm()
{
    std::vector<double> nm;
    for (int i = int(kCmfMinNm); i <= int(kCmfMaxNm); ++i) {
        nm.push_back(i);
    }
    return nm;
}

Rgb
render(const std::string& illuminant, const Matrix3& xyzToWorking, SpectralWhite white,
       const std::vector<double>& nm = everyNm())
{
    std::vector<Rgb> colours;
    EXPECT_TRUE(spectralColours(illuminant, nm, xyzToWorking, white, &colours)) << illuminant;
    Rgb sum = { 0., 0., 0. };
    for (const Rgb& c : colours) {
        for (int k = 0; k < 3; ++k) {
            sum[k] += c[k];
        }
    }
    return sum;
}

double
summedLuminance(const std::string& illuminant)
{
    double y = 0.;
    for (double nm : everyNm()) {
        y += spectralXyz(illuminant, nm)[1];
    }
    return y;
}

void
expectNeutral(const Rgb& rgb, double tolerance, const std::string& what)
{
    EXPECT_NEAR(rgb[0] / rgb[1], 1., tolerance) << what;
    EXPECT_NEAR(rgb[2] / rgb[1], 1., tolerance) << what;
}

OCIO_NAMESPACE::ConstConfigRcPtr
studioConfigWithSceneLinear(const char* space)
{
    OCIO_NAMESPACE::ConfigRcPtr config = OCIO_NAMESPACE::Config::CreateFromBuiltinConfig(kStudioConfig)->createEditableCopy();
    config->setRole(OCIO_NAMESPACE::ROLE_SCENE_LINEAR, space);
    return config;
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
    EXPECT_EQ(illuminantXyz("FL12"), (Xyz { 0., 0., 0. }));
}

TEST(OpticsSpectral, TablesMatchHandReadValues)
{
    const Xyz at360 = colourMatching(360.);
    EXPECT_NEAR(at360[0], 1.299e-04, 1e-12);
    EXPECT_NEAR(at360[1], 3.917e-06, 1e-12);
    EXPECT_NEAR(at360[2], 6.061e-04, 1e-12);
    const Xyz between = colourMatching(360.5);
    EXPECT_NEAR(between[0], 0.5 * (1.299e-04 + 1.45847e-04), 1e-12);
    const Xyz at555 = colourMatching(555.);
    EXPECT_NEAR(at555[0], 0.5120501, 1e-12);
    EXPECT_EQ(at555[1], 1.);
    const Xyz below = colourMatching(300.);
    EXPECT_EQ(below[2], at360[2]);
    EXPECT_NEAR(illuminantPower("B", 320.), 0.02, 1e-12);
    EXPECT_NEAR(illuminantPower("B", 322.5), 0.5 * (0.02 + 0.26), 1e-12);
    EXPECT_NEAR(illuminantPower("E", 500.), 100., 1e-12);
}

TEST(OpticsSpectral, EqualEnergyIntegratesToEqualXyz)
{
    const Xyz e = illuminantXyz("E");
    EXPECT_EQ(e[1], 1.);
    EXPECT_NEAR(e[0], 1., 1e-3);
    EXPECT_NEAR(e[2], 1., 1e-3);
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
    Chromaticity white = kWhiteAces;
    EXPECT_FALSE(workingSpaceWhite(nullptr, &white));
    EXPECT_EQ(white, kWhiteD65);
}

TEST(OpticsSpectral, D65RendersWhiteInRec709)
{
    expectNeutral(render("D65", kXyzD65ToRec709, SpectralWhite::Physical), 1e-4, "fallback");
    const OCIO_NAMESPACE::ConstConfigRcPtr config = studioConfigWithSceneLinear("Linear Rec.709 (sRGB)");
    Chromaticity white;
    EXPECT_TRUE(workingSpaceWhite(config, &white));
    EXPECT_EQ(white, kWhiteD65);
    XyzRoute route = XyzRoute::Rec709Fallback;
    const Matrix3 m = xyzToWorkingSpace(config, &route);
    EXPECT_NE(route, XyzRoute::Rec709Fallback);
    expectNeutral(render("D65", m, SpectralWhite::Physical), 1e-3, "studio config");
}

// E has equal X, Y and Z by the observer's definition; the 1 nm table's three areas agree to 4e-4,
// which moves the ratios by under 5e-4.
TEST(OpticsSpectral, EqualEnergyKeepsItsTintInRec709)
{
    const Rgb expected = applyMatrix(kXyzD65ToRec709, { 1., 1., 1. });
    const Rgb e = render("E", kXyzD65ToRec709, SpectralWhite::Physical);
    EXPECT_NEAR(e[0] / e[1], expected[0] / expected[1], 1e-3);
    EXPECT_NEAR(e[2] / e[1], expected[2] / expected[1], 1e-3);
    EXPECT_GT(e[0] / e[1], 1.2);
}

TEST(OpticsSpectral, AcesCgTakesItsOwnWhiteToNeutralAndTintsD65)
{
    const OCIO_NAMESPACE::ConstConfigRcPtr config = OCIO_NAMESPACE::Config::CreateFromBuiltinConfig(kStudioConfig);
    Chromaticity white;
    EXPECT_TRUE(workingSpaceWhite(config, &white));
    EXPECT_EQ(white, kWhiteAces);
    XyzRoute route = XyzRoute::Rec709Fallback;
    const Matrix3 m = xyzToWorkingSpace(config, &route);
    EXPECT_NE(route, XyzRoute::Rec709Fallback);
    const Rgb acesWhite = applyMatrix(m, { kWhiteAces[0] / kWhiteAces[1], 1., (1. - kWhiteAces[0] - kWhiteAces[1]) / kWhiteAces[1] });
    for (double c : acesWhite) {
        EXPECT_NEAR(c, 1., 1e-5);
    }
    expectNeutral(render("D60", m, SpectralWhite::Physical), 1e-3, "D60");
    const Rgb d65 = render("D65", m, SpectralWhite::Physical);
    EXPECT_LT(d65[0] / d65[1], 0.99);
    EXPECT_GT(d65[2] / d65[1], 1.01);
}

TEST(OpticsSpectral, LocalWhiteRendersTheIlluminantNeutralAtItsLuminance)
{
    const OCIO_NAMESPACE::ConstConfigRcPtr config = OCIO_NAMESPACE::Config::CreateFromBuiltinConfig(kStudioConfig);
    const Matrix3 spaces[2] = { kXyzD65ToRec709, xyzToWorkingSpace(config) };
    for (const Matrix3& m : spaces) {
        for (const char* illuminant : { "E", "D65", "A" }) {
            const Rgb rgb = render(illuminant, m, SpectralWhite::WorkingSpace);
            expectNeutral(rgb, 1e-4, illuminant);
            EXPECT_NEAR(rgb[1] / summedLuminance(illuminant), 1., 1e-4) << illuminant;
        }
    }
}

TEST(OpticsSpectral, ColoursAreTheMatrixTimesTheBalancedMatchingFunctions)
{
    const std::vector<double> nm = sampleWavelengths(3);
    std::vector<Rgb> colours;
    ASSERT_TRUE(spectralColours("D65", nm, kXyzD65ToRec709, SpectralWhite::Physical, &colours));
    ASSERT_EQ(colours.size(), 3u);
    Xyz dense = { 0., 0., 0. }, sampled = { 0., 0., 0. };
    for (double n : everyNm()) {
        for (int k = 0; k < 3; ++k) {
            dense[k] += spectralXyz("D65", n)[k];
        }
    }
    for (double n : nm) {
        for (int k = 0; k < 3; ++k) {
            sampled[k] += spectralXyz("D65", n)[k];
        }
    }
    for (size_t i = 0; i < nm.size(); ++i) {
        const Xyz cmf = colourMatching(nm[i]);
        const double power = illuminantPower("D65", nm[i]) * 0.01;
        Xyz xyz;
        for (int k = 0; k < 3; ++k) {
            xyz[k] = cmf[k] * power * dense[k] / sampled[k];
        }
        const Rgb expected = applyMatrix(kXyzD65ToRec709, xyz);
        for (int k = 0; k < 3; ++k) {
            EXPECT_NEAR(colours[i][k], expected[k], 1e-12);
        }
    }
    std::string error;
    EXPECT_FALSE(spectralColours("Z", nm, kXyzD65ToRec709, SpectralWhite::Physical, &colours, &error));
    EXPECT_EQ(error, "unknown illuminant \"Z\"");
    EXPECT_EQ(colours.size(), 3u);
    const Matrix3 noRed = { 0., 0., 0., 0., 1., 0., 0., 0., 1. };
    EXPECT_FALSE(spectralColours("D65", nm, noRed, SpectralWhite::WorkingSpace, &colours, &error));
    EXPECT_EQ(error, "illuminant \"D65\" is outside the working space");
    EXPECT_FALSE(spectralColours("D65", { 750., 800. }, kXyzD65ToRec709, SpectralWhite::Physical, &colours, &error));
    EXPECT_NE(error.find("miss one of X, Y and Z"), std::string::npos) << error;
}

// The renderers sum the colours of the wavelengths they trace, so 5 or 16 samples must give the
// colour and brightness the 1 nm integral does.
TEST(OpticsSpectral, SampledWavelengthsSumToTheDenseIntegral)
{
    const OCIO_NAMESPACE::ConstConfigRcPtr config = OCIO_NAMESPACE::Config::CreateFromBuiltinConfig(kStudioConfig);
    const Matrix3 spaces[2] = { kXyzD65ToRec709, xyzToWorkingSpace(config) };
    for (const Matrix3& m : spaces) {
        for (const char* illuminant : { "E", "D65", "A" }) {
            const Rgb physical = render(illuminant, m, SpectralWhite::Physical);
            for (int count : { 5, 16 }) {
                const std::vector<double> nm = sampleWavelengths(count);
                const std::string what = std::string(illuminant) + " x" + std::to_string(count);
                const Rgb local = render(illuminant, m, SpectralWhite::WorkingSpace, nm);
                expectNeutral(local, 1e-9, what);
                EXPECT_NEAR(local[1] / summedLuminance(illuminant), 1., 1e-4) << what;
                const Rgb sampled = render(illuminant, m, SpectralWhite::Physical, nm);
                for (int k = 0; k < 3; ++k) {
                    EXPECT_NEAR(sampled[k] / physical[k], 1., 1e-9) << what << " channel " << k;
                }
            }
        }
    }
}
