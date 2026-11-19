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
#include <string>

#include <gtest/gtest.h>

#include "Engine/Optics/DatParser.h"
#include "Engine/Optics/Prescription.h"

using namespace Natron::Optics;

namespace {

// pbrt-v3's scenes/lenses/dgauss.dat.
const char* const kPbrtDoubleGauss = "# D-GAUSS F/2 22deg HFOV\n"
                                     "# US patent 2,673,491 Tronnier\n"
                                     "# Moden Lens Design, p.312\n"
                                     "# Scaled to 50 mm from 100 mm\n"
                                     "# radius\tthick\tior\taperture\n"
                                     "29.475\t3.76\t1.67\t25.2\n"
                                     "84.83\t0.12\t1\t25.2\n"
                                     "19.275\t4.025\t1.67\t23\n"
                                     "40.77\t3.275\t1.699\t23\n"
                                     "12.75\t5.705\t1\t18\n"
                                     "0\t4.5\t0\t17.1\n"
                                     "-14.495\t1.18\t1.603\t17\n"
                                     "40.77\t6.065\t1.658\t20\n"
                                     "-20.385\t0.19\t1\t20\n"
                                     "437.065\t3.22\t1.717\t20\n"
                                     "-39.73\t0\t1\t20\n";

void
expectSameSurfaces(const Prescription& a,
                   const Prescription& b)
{
    ASSERT_EQ(a.surfaces.size(), b.surfaces.size());
    for (size_t i = 0; i < a.surfaces.size(); ++i) {
        const Surface& s = a.surfaces[i];
        const Surface& t = b.surfaces[i];
        EXPECT_EQ(s.type, t.type) << "surface " << i;
        EXPECT_DOUBLE_EQ(s.curvature, t.curvature) << "surface " << i;
        EXPECT_DOUBLE_EQ(s.spacing, t.spacing) << "surface " << i;
        EXPECT_DOUBLE_EQ(s.semiDiameter, t.semiDiameter) << "surface " << i;
        EXPECT_DOUBLE_EQ(s.nd, t.nd) << "surface " << i;
        EXPECT_DOUBLE_EQ(s.vd, t.vd) << "surface " << i;
    }
}

} // namespace

TEST(OpticsPrescription, EveryBuiltinHasExactlyOneStop)
{
    const std::vector<std::string> ids = builtinLensIds();
    ASSERT_EQ(ids.size(), 9u);
    for (const std::string& id : ids) {
        Prescription p;
        ASSERT_TRUE(builtinLens(id, &p)) << id;
        EXPECT_EQ(p.id, id);
        EXPECT_FALSE(p.name.empty()) << id;
        EXPECT_FALSE(p.source.empty()) << id;
        EXPECT_GE(p.surfaces.size(), 4u) << id;
        EXPECT_EQ(p.stopCount(), 1) << id;
        EXPECT_GE(p.stopIndex(), 1) << id;
        EXPECT_FALSE(p.hasAspheres()) << id;
        for (const Surface& s : p.surfaces) {
            EXPECT_GT(s.semiDiameter, 0.) << id;
            EXPECT_GE(s.nd, 1.) << id;
        }
    }
    Prescription none;
    EXPECT_FALSE(builtinLens("no-such-lens", &none));
}

TEST(OpticsPrescription, BuiltinsRoundTripThroughDat)
{
    for (const std::string& id : builtinLensIds()) {
        Prescription p;
        ASSERT_TRUE(builtinLens(id, &p));
        Prescription q;
        LensFileError error;
        ASSERT_TRUE(parseDat(formatDat(p), &q, &error)) << id << ": " << error.text();
        EXPECT_EQ(q.name, p.name);
        expectSameSurfaces(p, q);
    }
}

TEST(OpticsPrescription, PbrtDoubleGaussRoundTrips)
{
    Prescription parsed;
    LensFileError error;
    ASSERT_TRUE(parseDat(kPbrtDoubleGauss, &parsed, &error)) << error.text();
    EXPECT_EQ(parsed.name, "D-GAUSS F/2 22deg HFOV");
    ASSERT_EQ(parsed.surfaces.size(), 11u);
    EXPECT_EQ(parsed.stopIndex(), 5);
    EXPECT_DOUBLE_EQ(parsed.surfaces[0].radius(), 29.475);
    EXPECT_DOUBLE_EQ(parsed.surfaces[0].semiDiameter, 12.6);
    EXPECT_DOUBLE_EQ(parsed.surfaces[3].nd, 1.699);
    EXPECT_DOUBLE_EQ(parsed.surfaces[5].nd, 1.);
    EXPECT_EQ(parsed.surfaces[5].curvature, 0.);
    EXPECT_FALSE(parsed.imageDistance.has_value());

    Prescription builtin;
    ASSERT_TRUE(builtinLens("dgauss50", &builtin));
    expectSameSurfaces(parsed, builtin);

    Prescription again;
    ASSERT_TRUE(parseDat(formatDat(parsed), &again, &error)) << error.text();
    expectSameSurfaces(parsed, again);
}

TEST(OpticsPrescription, DatAbbeColumnIsOptional)
{
    Prescription p;
    LensFileError error;
    ASSERT_TRUE(parseDat("50 5 1.5168 20 64.17\n-50 2 1 20\n0 40 0 10\n", &p, &error)) << error.text();
    ASSERT_EQ(p.surfaces.size(), 3u);
    EXPECT_DOUBLE_EQ(p.surfaces[0].vd, 64.17);
    EXPECT_EQ(p.surfaces[1].vd, 0.);
}

TEST(OpticsPrescription, DatWithoutStopFailsWithLineNumber)
{
    Prescription p;
    LensFileError error;
    EXPECT_FALSE(parseDat("# no stop\n50 5 1.5 20\n-50 40 1 20\n", &p, &error));
    EXPECT_EQ(error.line, 3);
    EXPECT_NE(error.text().find("line 3"), std::string::npos) << error.text();
    EXPECT_NE(error.message.find("stop"), std::string::npos) << error.message;
}

TEST(OpticsPrescription, MalformedDatRowNamesTheLine)
{
    Prescription p;
    LensFileError error;
    EXPECT_FALSE(parseDat("50 5 1.5 20\n-50 x 1 20\n0 1 0 10\n", &p, &error));
    EXPECT_EQ(error.line, 2);
    EXPECT_FALSE(parseDat("50 5 1.5 20\n0 1 0 10\n-50 1\n", &p, &error));
    EXPECT_EQ(error.line, 3);
    EXPECT_FALSE(parseDat("50 5 1.5 20\n0 1 0 10\n0 1 0 10\n", &p, &error));
    EXPECT_EQ(error.line, 3);
}

TEST(OpticsPrescription, NonFiniteDatNumbersAreErrors)
{
    Prescription p;
    LensFileError error;
    EXPECT_FALSE(parseDat("50 5 1.5 20\n-50 nan 1 20\n0 1 0 10\n", &p, &error));
    EXPECT_EQ(error.line, 2);
    EXPECT_FALSE(parseDat("inf 5 1.5 20\n-50 2 1 20\n0 1 0 10\n", &p, &error));
    EXPECT_EQ(error.line, 1);
    EXPECT_FALSE(parseDat("50 5 1.5 20\n-50 2 1 1e999\n0 1 0 10\n", &p, &error));
    EXPECT_EQ(error.line, 2);
}

TEST(OpticsPrescription, DatStopWithoutApertureIsAnError)
{
    Prescription p;
    LensFileError error;
    EXPECT_FALSE(parseDat("50 5 1.5 20\n-50 2 1 20\n0 40 0 0\n", &p, &error));
    EXPECT_EQ(error.line, 3);
    EXPECT_NE(error.message.find("stop"), std::string::npos) << error.message;
}

TEST(OpticsPrescription, LayoutPlacesVerticesAndCentres)
{
    Prescription p;
    ASSERT_TRUE(builtinLens("dgauss50", &p));
    const std::vector<SurfaceLayout> l = p.layout();
    ASSERT_EQ(l.size(), p.surfaces.size());
    EXPECT_EQ(l[0].vertexZ, 0.);
    EXPECT_DOUBLE_EQ(l[0].centreZ, 29.475);
    EXPECT_DOUBLE_EQ(l[1].vertexZ, 3.76);
    EXPECT_DOUBLE_EQ(l[1].centreZ, 3.76 + 84.83);
    EXPECT_TRUE(std::isinf(l[5].centreZ));
    double z = 0.;
    for (size_t i = 0; i + 1 < p.surfaces.size(); ++i) {
        z += p.surfaces[i].spacing;
    }
    EXPECT_DOUBLE_EQ(l.back().vertexZ, z);
    EXPECT_DOUBLE_EQ(p.totalTrack(), z);
}
