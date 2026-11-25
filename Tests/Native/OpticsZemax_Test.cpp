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
#include <utility>

#include <gtest/gtest.h>

#include "Engine/Optics/Glass.h"
#include "Engine/Optics/Prescription.h"
#include "Engine/Optics/ZemaxParser.h"

using namespace Natron::Optics;

namespace {

const char* const kTriplet = NATRON_TESTS_FIXTURES_DIR "/optics/cooke-triplet-50mm-f4.zmx";
const char* const kZoom = NATRON_TESTS_FIXTURES_DIR "/optics/aspheric-zoom-position2.zmx";
const char* const kGlassJson = NATRON_TESTS_FIXTURES_DIR "/../../Gui/Resources/Optics/glass.json";

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
    EXPECT_DOUBLE_EQ(even.coefficient(0), 0.);
    EXPECT_DOUBLE_EQ(even.coefficient(1), -1.2e-5);
    EXPECT_DOUBLE_EQ(even.coefficient(2), 3.4e-8);
    EXPECT_DOUBLE_EQ(even.coefficient(3), -5.6e-11);
    EXPECT_DOUBLE_EQ(even.nd, 1.58913);

    const Surface& extended = lens.surfaces[3];
    EXPECT_EQ(extended.type, SurfaceType::ExtendedAsphere);
    EXPECT_DOUBLE_EQ(extended.curvature, -0.03);
    // XDAT 3 is the rho^2 term, XDAT 4 rho^4 and XDAT 5 rho^6.
    EXPECT_DOUBLE_EQ(extended.coefficient(0), 0.);
    EXPECT_DOUBLE_EQ(extended.coefficient(1), 2.5e-5);
    EXPECT_DOUBLE_EQ(extended.coefficient(2), -7.0e-8);
    EXPECT_DOUBLE_EQ(extended.coefficient(3), 0.);

    EXPECT_DOUBLE_EQ(lens.surfaces[4].nd, 1.6968);
    EXPECT_DOUBLE_EQ(lens.surfaces[4].vd, 60.);

    const Surface& odd = lens.surfaces[5];
    EXPECT_EQ(odd.type, SurfaceType::ExtendedOddAsphere);
    EXPECT_DOUBLE_EQ(odd.conic, 0.25);
    EXPECT_DOUBLE_EQ(odd.coefficient(0), 0.);
    EXPECT_DOUBLE_EQ(odd.coefficient(1), 1.5e-4);
    EXPECT_DOUBLE_EQ(odd.coefficient(2), -2.0e-6);
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

namespace {

// An object at infinity, then lensLines as surface 1, a stop and the image surface.
std::string
singleSurface(const std::string& lensLines,
              const std::string& header = std::string())
{
    const std::string rest = "  DISZ 2\n  GLAS ___BLANK 1 0 1.5 60\n  DIAM 5\n"
                             "SURF 2\n  STOP\n  DISZ 10\n  DIAM 4\n"
                             "SURF 3\n  DISZ 0\n";

    return header + "SURF 0\n  DISZ INFINITY\nSURF 1\n" + lensLines + rest;
}

} // namespace

// Extra data 1 is the term count and 2 the normalisation radius; the terms start at 3. Term i
// multiplies (r / R)^(2i), so the r^(2i) coefficient is the term over R^(2i).
TEST(OpticsZemax, ExtendedAsphereTermsUseTheNormalisationRadius)
{
    const std::string text = singleSurface("  TYPE XASPHERE\n  CURV 0.01\n  XDAT 1 2\n  XDAT 2 2\n  XDAT 3 8\n  XDAT 4 32\n  XDAT 5 99\n");
    Prescription lens;
    LensFileError error;
    ASSERT_TRUE(parseZemax(text, &lens, &error)) << error.text();
    ASSERT_EQ(lens.surfaces.size(), 2u);
    EXPECT_DOUBLE_EQ(lens.surfaces[0].coefficient(0), 2.);
    EXPECT_DOUBLE_EQ(lens.surfaces[0].coefficient(1), 2.);
    // Past the term count of 2.
    EXPECT_DOUBLE_EQ(lens.surfaces[0].coefficient(2), 0.);
    EXPECT_DOUBLE_EQ(lens.surfaces[1].curvature, 0.);
}

// Odd term i multiplies (r / R)^i, from extra data 3.
TEST(OpticsZemax, ExtendedOddAsphereTermsStartAtTheFirstPower)
{
    const std::string text = singleSurface("  TYPE XOSPHERE\n  CURV 0.01\n  XDAT 1 3\n  XDAT 2 2\n  XDAT 3 4\n  XDAT 4 8\n  XDAT 5 16\n");
    Prescription lens;
    LensFileError error;
    ASSERT_TRUE(parseZemax(text, &lens, &error)) << error.text();
    const Surface& s = lens.surfaces[0];
    EXPECT_DOUBLE_EQ(s.coefficient(0), 2.);
    EXPECT_DOUBLE_EQ(s.coefficient(1), 2.);
    EXPECT_DOUBLE_EQ(s.coefficient(2), 2.);
    EXPECT_DOUBLE_EQ(s.coefficient(3), 0.);
}

TEST(OpticsZemax, ExtendedAsphereKeepsEveryTerm)
{
    std::string lines = "  TYPE XASPHERE\n  CURV 0.01\n  XDAT 1 20\n  XDAT 2 2\n";
    for (int term = 1; term <= 20; ++term) {
        lines += "  XDAT " + std::to_string(term + 2) + " " + std::to_string(term) + "\n";
    }
    Prescription lens;
    LensFileError error;
    ASSERT_TRUE(parseZemax(singleSurface(lines), &lens, &error)) << error.text();
    const Surface& s = lens.surfaces[0];
    ASSERT_EQ(s.coefficients.size(), 20u);
    for (int term = 1; term <= 20; ++term) {
        EXPECT_DOUBLE_EQ(s.coefficient(term - 1), term / std::pow(2., 2 * term)) << term;
    }
    EXPECT_DOUBLE_EQ(s.coefficient(20), 0.);
}

TEST(OpticsZemax, EvenAsphereKeepsParametersPastEight)
{
    const std::string text = singleSurface("  TYPE EVENASPH\n  CURV 0.01\n  PARM 2 1e-5\n  PARM 12 3e-30\n");
    Prescription lens;
    LensFileError error;
    ASSERT_TRUE(parseZemax(text, &lens, &error)) << error.text();
    const Surface& s = lens.surfaces[0];
    ASSERT_EQ(s.coefficients.size(), 12u);
    EXPECT_DOUBLE_EQ(s.coefficient(1), 1e-5);
    EXPECT_DOUBLE_EQ(s.coefficient(11), 3e-30);
}

// The layout of the XASPHERE surfaces in published lens databases that have no XDAT.
TEST(OpticsZemax, ExtendedAsphereReadsEvenCoefficientsFromParm)
{
    const std::string text = singleSurface("  TYPE XASPHERE\n  CURV -0.0467739973993657 0 0 0 0\n  PARM 1 0\n  PARM 2 -1.72185E-06\n"
                                           "  PARM 3 1.36926E-07\n  PARM 4 -5.70286E-10\n  PARM 5 7.90200E-13\n  PARM 6 3.81362E-15\n"
                                           "  PARM 7 -1.51801E-17\n  PARM 8 1.63301E-20\n  CONI -0.056767\n");
    Prescription lens;
    LensFileError error;
    ASSERT_TRUE(parseZemax(text, &lens, &error)) << error.text();
    const Surface& s = lens.surfaces[0];
    EXPECT_EQ(s.type, SurfaceType::ExtendedAsphere);
    EXPECT_DOUBLE_EQ(s.conic, -0.056767);
    ASSERT_EQ(s.coefficients.size(), 8u);
    EXPECT_DOUBLE_EQ(s.coefficient(0), 0.);
    EXPECT_DOUBLE_EQ(s.coefficient(1), -1.72185E-06);
    EXPECT_DOUBLE_EQ(s.coefficient(2), 1.36926E-07);
    EXPECT_DOUBLE_EQ(s.coefficient(7), 1.63301E-20);
}

TEST(OpticsZemax, ExtendedAsphereWithParmAndXdatTermsIsAnError)
{
    const std::string text = singleSurface("  TYPE XASPHERE\n  CURV 0.01\n  PARM 2 1e-5\n  XDAT 1 2\n  XDAT 2 1\n  XDAT 3 0\n  XDAT 4 2e-5\n");
    Prescription lens;
    LensFileError error;
    EXPECT_FALSE(parseZemax(text, &lens, &error));
    EXPECT_NE(error.message.find("both PARM and XDAT"), std::string::npos) << error.text();

    const std::string zeroParm = singleSurface("  TYPE XASPHERE\n  CURV 0.01\n  PARM 1 0\n  XDAT 1 2\n  XDAT 2 1\n  XDAT 4 2e-5\n");
    ASSERT_TRUE(parseZemax(zeroParm, &lens, &error)) << error.text();
    EXPECT_DOUBLE_EQ(lens.surfaces[0].coefficient(1), 2e-5);
}

TEST(OpticsZemax, InchLensesAreConvertedToMillimetres)
{
    const std::string text = "UNIT IN X W X CM MR CPMM\n"
                             "SURF 0\n  DISZ INFINITY\n"
                             "SURF 1\n  TYPE EVENASPH\n  CURV 0.5\n  PARM 1 0.25\n  PARM 2 0.125\n  DISZ 0.2\n"
                             "  GLAS ___BLANK 1 0 1.5 60\n  DIAM 1\n"
                             "SURF 2\n  TYPE XASPHERE\n  CURV -0.25\n  XDAT 1 2\n  XDAT 2 2\n  XDAT 3 0.5\n  XDAT 4 4\n"
                             "  DISZ 0.1\n  DIAM 1\n"
                             "SURF 3\n  STOP\n  DISZ 2\n  DIAM 0.5\n"
                             "SURF 4\n  DISZ 0\n";
    Prescription lens;
    LensFileError error;
    ASSERT_TRUE(parseZemax(text, &lens, &error)) << error.text();
    ASSERT_EQ(lens.surfaces.size(), 3u);
    const double in = 25.4;
    const Surface& even = lens.surfaces[0];
    EXPECT_DOUBLE_EQ(even.curvature, 0.5 / in);
    EXPECT_DOUBLE_EQ(even.spacing, 0.2 * in);
    EXPECT_DOUBLE_EQ(even.semiDiameter, in);
    EXPECT_DOUBLE_EQ(even.coefficient(0), 0.25 / in);
    EXPECT_DOUBLE_EQ(even.coefficient(1), 0.125 / (in * in * in));
    const Surface& extended = lens.surfaces[1];
    EXPECT_DOUBLE_EQ(extended.curvature, -0.25 / in);
    EXPECT_NEAR(extended.coefficient(0), 0.5 / 4. / in, 1e-15);
    EXPECT_NEAR(extended.coefficient(1), 4. / 16. / (in * in * in), 1e-15);
    EXPECT_DOUBLE_EQ(lens.surfaces[2].semiDiameter, 0.5 * in);
    ASSERT_TRUE(lens.imageDistance.has_value());
    EXPECT_DOUBLE_EQ(*lens.imageDistance, 2. * in);

    // The sag in mm is the file's sag in inches times 25.4.
    const double rIn = 0.8, rMm = rIn * in;
    const double sagIn = 0.25 * rIn * rIn + 0.125 * std::pow(rIn, 4.);
    const double sagMm = even.coefficient(0) * rMm * rMm + even.coefficient(1) * std::pow(rMm, 4.);
    EXPECT_NEAR(sagMm, sagIn * in, 1e-12);
}

TEST(OpticsZemax, CentimetreAndMetreLenses)
{
    for (const auto& [unit, scale] : { std::pair<const char*, double> { "CM", 10. }, { "METER", 1000. }, { "MM", 1. } }) {
        Prescription lens;
        LensFileError error;
        ASSERT_TRUE(parseZemax(singleSurface("  CURV 0.1\n", std::string("UNIT ") + unit + "\n"), &lens, &error)) << unit << ": " << error.text();
        EXPECT_DOUBLE_EQ(lens.surfaces[0].curvature, 0.1 / scale) << unit;
        EXPECT_DOUBLE_EQ(lens.surfaces[0].spacing, 2. * scale) << unit;
    }
}

TEST(OpticsZemax, UnknownUnitIsAnError)
{
    Prescription lens;
    LensFileError error;
    EXPECT_FALSE(parseZemax(singleSurface("  CURV 0.1\n", "VERS 1\nUNIT FT X W X CM MR CPMM\n"), &lens, &error));
    EXPECT_EQ(error.line, 2);
    EXPECT_NE(error.message.find("FT"), std::string::npos) << error.message;
}

TEST(OpticsZemax, NonFiniteNumbersAreErrors)
{
    Prescription lens;
    LensFileError error;
    // CURV is on line 4 of each file.
    for (const char* curv : { "nan", "INFINITY", "-inf", "1e999" }) {
        EXPECT_FALSE(parseZemax(singleSurface(std::string("  CURV ") + curv + "\n"), &lens, &error)) << curv;
        EXPECT_EQ(error.line, 4) << curv;
    }
    EXPECT_FALSE(parseZemax(singleSurface("  CONI nan\n"), &lens, &error));
    EXPECT_FALSE(parseZemax(singleSurface("  TYPE EVENASPH\n  PARM 2 inf\n"), &lens, &error));
    EXPECT_FALSE(parseZemax("FNUM nan\n" + singleSurface(std::string()), &lens, &error));
    EXPECT_EQ(error.line, 1);
    // DISZ INFINITY is the object at infinity.
    EXPECT_TRUE(parseZemax(singleSurface(std::string()), &lens, &error)) << error.text();
}

TEST(OpticsZemax, IndicesMustBeWholeNumbers)
{
    Prescription lens;
    LensFileError error;
    EXPECT_FALSE(parseZemax(singleSurface("  TYPE EVENASPH\n  PARM 2.5 1\n"), &lens, &error));
    EXPECT_EQ(error.line, 5);
    EXPECT_FALSE(parseZemax(singleSurface("  TYPE XASPHERE\n  XDAT 1e300 1\n"), &lens, &error));
    EXPECT_EQ(error.line, 5);
    EXPECT_FALSE(parseZemax(singleSurface("  TYPE XASPHERE\n  XDAT -3 1\n"), &lens, &error));
    EXPECT_FALSE(parseZemax(singleSurface("  TYPE XASPHERE\n  XDAT 1 2.5\n"), &lens, &error));
    EXPECT_FALSE(parseZemax("SURF 0\n  DISZ INFINITY\nSURF 3000000000\n  DISZ 1\n", &lens, &error));
    EXPECT_EQ(error.line, 3);
}

TEST(OpticsZemax, IndentedSurfStartsANewSurface)
{
    const std::string text = "SURF 0\n  DISZ INFINITY\n"
                             "  SURF 1\n  CURV 0.1\n  DISZ 2\n  GLAS ___BLANK 1 0 1.5 60\n  DIAM 5\n"
                             "  SURF 2\n  STOP\n  DISZ 10\n  DIAM 4\n"
                             "  SURF 3\n  DISZ 0\n";
    Prescription lens;
    LensFileError error;
    ASSERT_TRUE(parseZemax(text, &lens, &error)) << error.text();
    ASSERT_EQ(lens.surfaces.size(), 2u);
    EXPECT_DOUBLE_EQ(lens.surfaces[0].curvature, 0.1);
    EXPECT_DOUBLE_EQ(lens.surfaces[0].nd, 1.5);
    EXPECT_EQ(lens.surfaces[1].type, SurfaceType::Stop);
    EXPECT_DOUBLE_EQ(*lens.imageDistance, 10.);
}

TEST(OpticsZemax, StopWithoutSemiDiameterIsAnError)
{
    const std::string text = "SURF 0\n  DISZ INFINITY\n"
                             "SURF 1\n  CURV 0.1\n  DISZ 2\n  GLAS ___BLANK 1 0 1.5 60\n  DIAM 5\n"
                             "SURF 2\n  STOP\n  DISZ 10\n"
                             "SURF 3\n  DISZ 0\n";
    Prescription lens;
    LensFileError error;
    EXPECT_FALSE(parseZemax(text, &lens, &error));
    EXPECT_EQ(error.line, 8);
    EXPECT_NE(error.message.find("stop"), std::string::npos) << error.message;
    std::string zero = text;
    zero.insert(zero.find("SURF 3"), "  DIAM 0\n");
    EXPECT_FALSE(parseZemax(zero, &lens, &error));
    EXPECT_EQ(error.line, 11);
}

TEST(OpticsZemax, NamedGlassIsLookedUpInTheCatalogue)
{
    // False once another test has loaded the shared catalogue; it is the bundled file either way.
    GlassCatalogue::setSharedPath(kGlassJson);
    std::string catalogueError;
    ASSERT_FALSE(GlassCatalogue::shared(&catalogueError).glasses().empty()) << catalogueError;
    const Glass* bk7 = GlassCatalogue::shared().find("Schott", "N-BK7");
    ASSERT_NE(bk7, nullptr);

    const std::string text = "SURF 0\n  DISZ INFINITY\n"
                             "SURF 1\n  CURV 0.1\n  DISZ 2\n  GLAS N-BK7\n  DIAM 5\n"
                             "SURF 2\n  STOP\n  DISZ 10\n  DIAM 4\n"
                             "SURF 3\n  DISZ 0\n";
    Prescription lens;
    LensFileError error;
    ASSERT_TRUE(parseZemax(text, &lens, &error)) << error.text();
    ASSERT_EQ(lens.surfaces.size(), 2u);
    EXPECT_DOUBLE_EQ(lens.surfaces[0].nd, bk7->nd);
    EXPECT_DOUBLE_EQ(lens.surfaces[0].vd, bk7->vd);

    std::string unknown = text;
    unknown.replace(unknown.find("N-BK7"), 5, "NO-SUCH-GLASS");
    EXPECT_FALSE(parseZemax(unknown, &lens, &error));
    EXPECT_EQ(error.line, 6);
    EXPECT_NE(error.message.find("NO-SUCH-GLASS"), std::string::npos) << error.message;
}
