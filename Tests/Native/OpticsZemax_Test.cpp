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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include <cmath>
#include <fstream>
#include <iterator>
#include <string>

#include <gtest/gtest.h>

#include "Engine/Optics/Prescription.h"
#include "Engine/Optics/ZemaxParser.h"

using namespace Natron::Optics;

namespace {

const char* const kTriplet = NATRON_TESTS_FIXTURES_DIR "/optics/cooke-triplet-50mm-f4.zmx";
const char* const kZoom = NATRON_TESTS_FIXTURES_DIR "/optics/aspheric-zoom-position2.zmx";

std::string
readBytes(const char* path)
{
    std::ifstream file(path, std::ios::binary);

    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

} // namespace

TEST(OpticsZemax, SphericalPrimeMatchesHandReadValues)
{
    Prescription lens;
    LensFileError error;
    ASSERT_TRUE(loadLensFile(kTriplet, &lens, &error)) << error.text();
    EXPECT_EQ(lens.id, "cooke-triplet-50mm-f4");
    EXPECT_EQ(lens.name, "Cooke triplet 50mm f/4");
    EXPECT_NE(lens.source.find("Geiser"), std::string::npos);
    EXPECT_DOUBLE_EQ(lens.fNumber, 4.);
    ASSERT_EQ(lens.surfaces.size(), 7u);
    EXPECT_EQ(lens.stopIndex(), 2);
    EXPECT_FALSE(lens.hasAspheres());

    const Surface& front = lens.surfaces[0];
    EXPECT_EQ(front.type, SurfaceType::Standard);
    EXPECT_DOUBLE_EQ(front.curvature, 0.042170961076203);
    EXPECT_DOUBLE_EQ(front.spacing, 4.831);
    EXPECT_DOUBLE_EQ(front.semiDiameter, 9.2085);
    EXPECT_DOUBLE_EQ(front.nd, 1.691);
    EXPECT_DOUBLE_EQ(front.vd, 54.71);
    EXPECT_DOUBLE_EQ(lens.surfaces[1].nd, 1.);
    EXPECT_DOUBLE_EQ(lens.surfaces[2].semiDiameter, 4.811);
    EXPECT_DOUBLE_EQ(lens.surfaces[3].curvature, -0.040889761);
    EXPECT_DOUBLE_EQ(lens.surfaces[3].vd, 32.21);
    EXPECT_DOUBLE_EQ(lens.surfaces[6].curvature, -0.048794293);
    ASSERT_TRUE(lens.imageDistance.has_value());
    EXPECT_DOUBLE_EQ(*lens.imageDistance, 41.2372);
}

TEST(OpticsZemax, Utf16AsphericZoomMatchesHandReadValues)
{
    Prescription lens;
    LensFileError error;
    ASSERT_TRUE(loadLensFile(kZoom, &lens, &error)) << error.text();
    EXPECT_EQ(lens.name, "Aspheric zoom, position 2 of 3");
    EXPECT_DOUBLE_EQ(lens.fNumber, 3.5);
    ASSERT_EQ(lens.surfaces.size(), 6u);
    EXPECT_EQ(lens.stopIndex(), 2);
    EXPECT_TRUE(lens.hasAspheres());

    const Surface& even = lens.surfaces[0];
    EXPECT_EQ(even.type, SurfaceType::EvenAsphere);
    EXPECT_DOUBLE_EQ(even.curvature, 0.02);
    EXPECT_DOUBLE_EQ(even.conic, -0.5);
    EXPECT_DOUBLE_EQ(even.coefficients[0], 0.);
    EXPECT_DOUBLE_EQ(even.coefficients[1], -1.2e-5);
    EXPECT_DOUBLE_EQ(even.coefficients[2], 3.4e-8);
    EXPECT_DOUBLE_EQ(even.coefficients[3], -5.6e-11);
    EXPECT_DOUBLE_EQ(even.nd, 1.58913);

    const Surface& extended = lens.surfaces[3];
    EXPECT_EQ(extended.type, SurfaceType::ExtendedAsphere);
    EXPECT_DOUBLE_EQ(extended.curvature, -0.03);
    EXPECT_DOUBLE_EQ(extended.coefficients[0], 2.5e-5);
    EXPECT_DOUBLE_EQ(extended.coefficients[1], -7.0e-8);
    EXPECT_DOUBLE_EQ(extended.coefficients[2], 0.);

    EXPECT_DOUBLE_EQ(lens.surfaces[4].nd, 1.6968);
    EXPECT_DOUBLE_EQ(lens.surfaces[4].vd, 60.);

    const Surface& odd = lens.surfaces[5];
    EXPECT_EQ(odd.type, SurfaceType::ExtendedOddAsphere);
    EXPECT_DOUBLE_EQ(odd.conic, 0.25);
    EXPECT_DOUBLE_EQ(odd.coefficients[0], 0.);
    EXPECT_DOUBLE_EQ(odd.coefficients[1], 1.5e-4);
    EXPECT_DOUBLE_EQ(odd.coefficients[2], -2.0e-6);
    ASSERT_TRUE(lens.imageDistance.has_value());
    EXPECT_DOUBLE_EQ(*lens.imageDistance, 30.);
}

TEST(OpticsZemax, TruncatedFileFailsWithLineNumber)
{
    const std::string full = readBytes(kTriplet);
    ASSERT_FALSE(full.empty());
    const size_t cut = full.find("  DISZ 0.975");
    ASSERT_NE(cut, std::string::npos);
    Prescription lens;
    LensFileError error;
    EXPECT_FALSE(parseZemax(full.substr(0, cut), &lens, &error));
    // SURF 4 starts on line 38 and lost its DISZ.
    EXPECT_EQ(error.line, 38);
    EXPECT_NE(error.message.find("DISZ"), std::string::npos);
}

TEST(OpticsZemax, MalformedNumberNamesTheLine)
{
    const std::string text = "SURF 0\n  CURV 0\n  DISZ INFINITY\nSURF 1\n  CURV 0.x1\n  DISZ 1\n";
    Prescription lens;
    LensFileError error;
    EXPECT_FALSE(parseZemax(text, &lens, &error));
    EXPECT_EQ(error.line, 5);
}

TEST(OpticsZemax, UnsupportedSurfaceTypeIsAnError)
{
    const std::string text = "SURF 0\n  CURV 0\n  DISZ INFINITY\nSURF 1\n  TYPE TOROIDAL\n  CURV 0\n  DISZ 1\n";
    Prescription lens;
    LensFileError error;
    EXPECT_FALSE(parseZemax(text, &lens, &error));
    EXPECT_EQ(error.line, 5);
    EXPECT_NE(error.message.find("TOROIDAL"), std::string::npos);
}

TEST(OpticsZemax, UnknownExtensionIsAnError)
{
    Prescription lens;
    LensFileError error;
    EXPECT_FALSE(loadLensFile(NATRON_TESTS_FIXTURES_DIR "/optics/lens.txt", &lens, &error));
}

TEST(OpticsZemax, ExtendedAsphereTermsUseTheNormalisationRadius)
{
    const std::string text = "SURF 0\n  DISZ INFINITY\n"
                             "SURF 1\n  TYPE XASPHERE\n  CURV 0.01\n  XDAT 1 2\n  XDAT 2 2\n  XDAT 4 8\n  XDAT 5 32\n"
                             "  DISZ 2\n  GLAS ___BLANK 1 0 1.5 60\n"
                             "SURF 2\n  STOP\n  DISZ 10\n"
                             "SURF 3\n  DISZ 0\n";
    Prescription lens;
    LensFileError error;
    ASSERT_TRUE(parseZemax(text, &lens, &error)) << error.text();
    ASSERT_EQ(lens.surfaces.size(), 2u);
    EXPECT_DOUBLE_EQ(lens.surfaces[0].coefficients[0], 2.);
    EXPECT_DOUBLE_EQ(lens.surfaces[0].coefficients[1], 2.);
    EXPECT_DOUBLE_EQ(lens.surfaces[1].curvature, 0.);
}
