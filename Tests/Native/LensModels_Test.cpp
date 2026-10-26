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
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Transform/LensModels.h"

NATRON_NAMESPACE_USING

namespace {
using namespace LensModels;

const Frame kHD = { 0., 0., 1920., 1080., 1. };

Direction
opposite(Direction d)
{
    return d == eDirectionDistort ? eDirectionUndistort : eDirectionDistort;
}

std::string
where(Direction d, double x, double y)
{
    std::ostringstream ss;
    ss << (d == eDirectionDistort ? "distort" : "undistort") << " at (" << x << ", " << y << ")";
    return ss.str();
}

// Both directions over an (n + 1)^2 grid of the frame: each maps back within tol pixels, and the
// two Jacobians are inverses of each other.
void
expectRoundTrip(const Model& m, const Frame& f, int n, double tol)
{
    for (int j = 0; j <= n; ++j) {
        for (int i = 0; i <= n; ++i) {
            const double x = f.x1 + (f.x2 - f.x1) * i / n;
            const double y = f.y1 + (f.y2 - f.y1) * j / n;
            for (Direction d : { eDirectionDistort, eDirectionUndistort }) {
                SCOPED_TRACE(where(d, x, y));
                Sample a, b;
                ASSERT_TRUE(m.map(d, x, y, &a));
                ASSERT_TRUE(m.map(opposite(d), a.x, a.y, &b));
                EXPECT_NEAR(b.x, x, tol);
                EXPECT_NEAR(b.y, y, tol);
                EXPECT_NEAR(b.J[0] * a.J[0] + b.J[1] * a.J[2], 1., 1e-9);
                EXPECT_NEAR(b.J[0] * a.J[1] + b.J[1] * a.J[3], 0., 1e-9);
                EXPECT_NEAR(b.J[2] * a.J[0] + b.J[3] * a.J[2], 0., 1e-9);
                EXPECT_NEAR(b.J[2] * a.J[1] + b.J[3] * a.J[3], 1., 1e-9);
            }
        }
    }
}

void
expectJacobianMatchesFiniteDifference(const Model& m, const Frame& f, int n)
{
    for (int j = 0; j <= n; ++j) {
        for (int i = 0; i <= n; ++i) {
            // Stay a step inside the frame, and off the exact centre where PanoTools' linear
            // radial term has a kink.
            const double x = f.x1 + 1. + (f.x2 - f.x1 - 2.) * (i + 0.37) / (n + 1);
            const double y = f.y1 + 1. + (f.y2 - f.y1 - 2.) * (j + 0.61) / (n + 1);
            for (Direction d : { eDirectionDistort, eDirectionUndistort }) {
                SCOPED_TRACE(where(d, x, y));
                double J[4], F[4];
                ASSERT_TRUE(m.jacobian(x, y, d, J));
                ASSERT_TRUE(jacobianFiniteDifference(m, d, x, y, 1e-2, F));
                for (int k = 0; k < 4; ++k) {
                    EXPECT_NEAR(J[k], F[k], 1e-5) << "entry " << k;
                }
            }
        }
    }
}

const NukeParams kNuke = { 0.05, 0.01, 0.02, -0.01, 1.2, 0.01, 0.005 };

// openfx-misc DistortionModelNuke::undistort, at render scale 1.
void
ofxNukeUndistort(const Frame& fr, const NukeParams& p, double xd, double yd, double* xu, double* yu)
{
    const double fx = (fr.x2 - fr.x1) / 2.;
    const double fy = (fr.y2 - fr.y1) / 2.;
    const double f = (std::max)(fx, fy);
    const double xc = (fr.x1 + fr.x2) / 2.;
    const double yc = (fr.y1 + fr.y2) / 2.;
    const double x = fr.par * (xd - xc) / f - p.centerX;
    const double y = (yd - yc) / f - p.centerY;
    const double x2 = x * x, y2 = y * y;
    const double r2 = x2 + y2;
    const double k2r2pk1 = p.k2 * r2 + p.k1;
    const double kry = 1 + (k2r2pk1 * r2 + p.asymmetricY * x2);
    const double krx = 1 + (k2r2pk1 * r2 + p.asymmetricX * y2) / p.anamorphicSqueeze;
    double sx = x / krx + p.centerX;
    double sy = y / kry + p.centerY;
    *xu = sx / fr.par * f + xc;
    *yu = sy * f + yc;
}

const PanoToolsParams kPanoTools = { 0.01, -0.02, 0.03, 5., -3., 2., 1. };

// openfx-misc DistortionModelPanoTools::distort, at render scale 1.
void
ofxPanoToolsDistort(const Frame& fr, const PanoToolsParams& p, double xu, double yu, double* xd, double* yd)
{
    const double fx = fr.x2 - fr.x1;
    const double fy = fr.y2 - fr.y1;
    const double f = (std::min)(fx, fy) / 2.;
    const double xc = (fr.x1 + fr.x2) / 2.;
    const double yc = (fr.y1 + fr.y2) / 2.;
    const double g = p.g / fy;
    const double t = p.t / fx;
    const double x = fr.par * (xu - xc) / f;
    const double y = (yu - yc) / f;
    const double r2 = x * x + y * y;
    const double d = 1 - (p.a + p.b + p.c);
    const double r = std::sqrt(r2);
    const double scale = (p.a * r2 + p.c) * r + p.b * r2 + d;
    double sx = x * scale / fr.par * f;
    double sy = y * scale * f;
    sx += p.d;
    sy -= p.e;
    const double sx0 = sx, sy0 = sy;
    *xd = sx + xc - g * sy0;
    *yd = sy + yc - t * sx0;
}

const PFBarrelParams kPFBarrel = { 0.05, 0.01, 0.52, 0.47, 1.1 };

// openfx-misc DistortionModelPFBarrel::undistort, at render scale 1 and for a format at the origin.
void
ofxPFBarrelUndistort(const Frame& fr, const PFBarrelParams& p, double xd, double yd, double* xu, double* yu)
{
    const double fw = fr.x2 - fr.x1;
    const double fh = fr.y2 - fr.y1;
    const double normx = std::sqrt(2.0 / (fw * fw + fh * fh));
    xd -= 0.5;
    yd -= 0.5;
    const double centx = p.centerX * fw * normx;
    double x = xd * normx;
    const double centy = p.centerY * fh * normx / p.squeeze;
    double y = yd * normx / p.squeeze;
    const double px = x - centx;
    const double py = y - centy;
    const double r2 = px * px + py * py;
    const double dr_r = r2 * (p.c3 + r2 * p.c5);
    x += px * dr_r;
    x /= normx;
    y += py * dr_r;
    y *= p.squeeze / normx;
    *xu = x + 0.5;
    *yu = y + 0.5;
}
} // anon namespace

TEST(LensModels, NukeHandValue)
{
    const Frame fr = { 0., 0., 2000., 1000., 1. };
    const NukeParams p = { 0.1, 0., 0., 0., 1., 0., 0. };
    const Model m = makeNuke(fr, p);
    double xu, yu;
    ASSERT_TRUE(m.undistort(2000., 500., &xu, &yu));
    EXPECT_NEAR(xu, 1000. + 1000. / 1.1, 1e-9);
    EXPECT_NEAR(yu, 500., 1e-9);
}

TEST(LensModels, NukeMatchesOpenFXMisc)
{
    const Frame frames[] = { kHD, { 0., 0., 1828., 1556., 2. } };
    for (const Frame& fr : frames) {
        const Model m = makeNuke(fr, kNuke);
        for (double x : { 0., 333.3, 914., 1500., 1828. }) {
            for (double y : { 0., 211.7, 540., 1080. }) {
                double xu, yu, rx, ry;
                ASSERT_TRUE(m.undistort(x, y, &xu, &yu));
                ofxNukeUndistort(fr, kNuke, x, y, &rx, &ry);
                EXPECT_NEAR(xu, rx, 1e-9);
                EXPECT_NEAR(yu, ry, 1e-9);
            }
        }
    }
}

TEST(LensModels, NukeRoundTrip)
{
    expectRoundTrip(makeNuke(kHD, kNuke), kHD, 16, 1e-6);
    // x spans [-2, 2] in Nuke's normalized space here, out of kNuke's reach.
    const Frame anamorphic = { 0., 0., 1828., 1556., 2. };
    const NukeParams mild = { 0.02, 0.002, 0.02, -0.01, 1.2, 0.01, 0.005 };
    expectRoundTrip(makeNuke(anamorphic, mild), anamorphic, 16, 1e-6);
}

TEST(LensModels, NukeJacobian)
{
    expectJacobianMatchesFiniteDifference(makeNuke(kHD, kNuke), kHD, 8);
}

TEST(LensModels, PanoToolsHandValue)
{
    const Frame fr = { 0., 0., 2000., 1000., 1. };
    const PanoToolsParams p = { 0., 0., 0.1, 0., 0., 0., 0. };
    const Model m = makePanoTools(fr, p);
    double xd, yd;
    ASSERT_TRUE(m.distort(1500., 500., &xd, &yd));
    EXPECT_NEAR(xd, 1500., 1e-9);
    EXPECT_NEAR(yd, 500., 1e-9);
    ASSERT_TRUE(m.distort(1250., 500., &xd, &yd));
    EXPECT_NEAR(xd, 1237.5, 1e-9);
    EXPECT_NEAR(yd, 500., 1e-9);
}

TEST(LensModels, PanoToolsMatchesOpenFXMisc)
{
    const Frame frames[] = { kHD, { 0., 0., 1828., 1556., 2. } };
    for (const Frame& fr : frames) {
        const Model m = makePanoTools(fr, kPanoTools);
        for (double x : { 0., 333.3, 914., 1500., 1828. }) {
            for (double y : { 0., 211.7, 540., 1080. }) {
                double xd, yd, rx, ry;
                ASSERT_TRUE(m.distort(x, y, &xd, &yd));
                ofxPanoToolsDistort(fr, kPanoTools, x, y, &rx, &ry);
                EXPECT_NEAR(xd, rx, 1e-9);
                EXPECT_NEAR(yd, ry, 1e-9);
            }
        }
    }
}

TEST(LensModels, PanoToolsRoundTrip)
{
    expectRoundTrip(makePanoTools(kHD, kPanoTools), kHD, 16, 1e-6);
}

TEST(LensModels, PanoToolsJacobian)
{
    expectJacobianMatchesFiniteDifference(makePanoTools(kHD, kPanoTools), kHD, 8);
}

TEST(LensModels, PFBarrelMatchesOpenFXMisc)
{
    const Model m = makePFBarrel(kHD, kPFBarrel);
    for (double x : { 0., 333.3, 960., 1500., 1920. }) {
        for (double y : { 0., 211.7, 540., 1080. }) {
            double xu, yu, rx, ry;
            ASSERT_TRUE(m.undistort(x, y, &xu, &yu));
            ofxPFBarrelUndistort(kHD, kPFBarrel, x, y, &rx, &ry);
            EXPECT_NEAR(xu, rx, 1e-9);
            EXPECT_NEAR(yu, ry, 1e-9);
        }
    }
}

TEST(LensModels, PFBarrelRoundTrip)
{
    expectRoundTrip(makePFBarrel(kHD, kPFBarrel), kHD, 16, 1e-6);
}

TEST(LensModels, PFBarrelJacobian)
{
    expectJacobianMatchesFiniteDifference(makePFBarrel(kHD, kPFBarrel), kHD, 8);
}

// A format whose origin is not (0,0) gives the same mapping, shifted.
TEST(LensModels, OffsetOriginShiftsTheMapping)
{
    const double ox = 100., oy = -50.;
    const Frame shifted = { kHD.x1 + ox, kHD.y1 + oy, kHD.x2 + ox, kHD.y2 + oy, 1. };
    const Model pairs[][2] = {
        { makeNuke(kHD, kNuke), makeNuke(shifted, kNuke) },
        { makePanoTools(kHD, kPanoTools), makePanoTools(shifted, kPanoTools) },
        { makePFBarrel(kHD, kPFBarrel), makePFBarrel(shifted, kPFBarrel) },
    };
    for (const auto& pair : pairs) {
        for (Direction d : { eDirectionDistort, eDirectionUndistort }) {
            Sample a, b;
            ASSERT_TRUE(pair[0].map(d, 300., 700., &a));
            ASSERT_TRUE(pair[1].map(d, 300. + ox, 700. + oy, &b));
            EXPECT_NEAR(b.x, a.x + ox, 1e-9);
            EXPECT_NEAR(b.y, a.y + oy, 1e-9);
            for (int k = 0; k < 4; ++k) {
                EXPECT_NEAR(b.J[k], a.J[k], 1e-9);
            }
        }
    }
}

TEST(LensModels, DegenerateParametersFailCleanly)
{
    const Frame empty = { 0., 0., 0., 0., 1. };
    const Model m = makeNuke(empty, kNuke);
    EXPECT_FALSE(m.valid);
    Sample s;
    EXPECT_FALSE(m.map(eDirectionUndistort, 10., 10., &s));
    EXPECT_FALSE(m.map(eDirectionDistort, 10., 10., &s));

    PFBarrelParams p = kPFBarrel;
    p.squeeze = 0.;
    EXPECT_FALSE(makePFBarrel(kHD, p).valid);
}

// With k1 = 0.9 the Nuke undistortion x / (1 + 0.9 r^2) never exceeds radius 0.527, so no
// distorted point exists for the bottom row: Newton reports failure and hands back no NaN.
TEST(LensModels, NonConvergenceIsReportedNotNaN)
{
    const NukeParams strong = { 0.9, 0., 0., 0., 1., 0., 0. };
    const Model m = makeNuke(kHD, strong);
    int failures = 0;
    for (int i = 0; i <= 32; ++i) {
        const double x = 1920. * i / 32;
        Sample s;
        if (m.map(eDirectionDistort, x, 0., &s)) {
            EXPECT_TRUE(std::isfinite(s.x) && std::isfinite(s.y));
            for (int k = 0; k < 4; ++k) {
                EXPECT_TRUE(std::isfinite(s.J[k]));
            }
        } else {
            ++failures;
        }
    }
    EXPECT_GT(failures, 0);
}

TEST(LensModels, AffineComposeAndInvert)
{
    const Affine a = { 2., 0.5, -0.25, 1.5, 10., -4. };
    Affine inv;
    ASSERT_TRUE(affineInvert(a, &inv));
    const Affine id = affineCompose(inv, a);
    EXPECT_NEAR(id.m00, 1., 1e-12);
    EXPECT_NEAR(id.m01, 0., 1e-12);
    EXPECT_NEAR(id.m10, 0., 1e-12);
    EXPECT_NEAR(id.m11, 1., 1e-12);
    EXPECT_NEAR(id.tx, 0., 1e-12);
    EXPECT_NEAR(id.ty, 0., 1e-12);
    const Affine singular = { 1., 2., 2., 4., 0., 0. };
    EXPECT_FALSE(affineInvert(singular, &inv));
}

TEST(LensModels, ParsePFBarrelFile)
{
    const std::string text = "#PFBarrel 2011 v2\n"
                             "# exported\n"
                             "1920\n1080\n1.0\n"
                             "\n"
                             "2000\r\n1125\n"
                             "High Order\n"
                             "1.5\n"
                             "2\n"
                             "1\n0.01\n0.002\n0.5\n0.5\n"
                             "10\n0.02\n0.003\n0.51\n0.49\n";
    PFBarrelFile f;
    std::string error;
    ASSERT_TRUE(parsePFBarrelFile(text, &f, &error)) << error;
    EXPECT_EQ(f.version, 2);
    EXPECT_EQ(f.originalWidth, 1920);
    EXPECT_EQ(f.originalHeight, 1080);
    EXPECT_EQ(f.originalPixelAspect, 1.);
    EXPECT_EQ(f.undistortedWidth, 2000);
    EXPECT_EQ(f.undistortedHeight, 1125);
    EXPECT_TRUE(f.highOrder);
    EXPECT_EQ(f.squeeze, 1.5);
    ASSERT_EQ(f.keys.size(), 2u);
    EXPECT_EQ(f.keys[0].frame, 1);
    EXPECT_EQ(f.keys[0].c3, 0.01);
    EXPECT_EQ(f.keys[0].c5, 0.002);
    EXPECT_EQ(f.keys[1].frame, 10);
    EXPECT_EQ(f.keys[1].c3, 0.02);
    EXPECT_EQ(f.keys[1].c5, 0.003);
    EXPECT_EQ(f.keys[1].centerX, 0.51);
    EXPECT_EQ(f.keys[1].centerY, 0.49);
}

TEST(LensModels, ParsePFBarrelLowOrderDropsC5)
{
    const std::string text = "#PFBarrel 2011 v1\n"
                             "1920\n1080\n1920\n1080\n"
                             "Low Order\n"
                             "1\n1\n"
                             "5\n0.01\n0.7\n0.5\n0.5\n";
    PFBarrelFile f;
    ASSERT_TRUE(parsePFBarrelFile(text, &f, 0));
    EXPECT_EQ(f.version, 1);
    EXPECT_EQ(f.originalPixelAspect, 1.);
    EXPECT_FALSE(f.highOrder);
    ASSERT_EQ(f.keys.size(), 1u);
    EXPECT_EQ(f.keys[0].c5, 0.);
}

TEST(LensModels, ParsePFBarrelErrors)
{
    PFBarrelFile f;
    std::string error;
    EXPECT_FALSE(parsePFBarrelFile("#PFBarrel 2012 v9\n", &f, &error));
    EXPECT_EQ(error.compare(0, 11, "Bad header:"), 0) << error;
    EXPECT_FALSE(parsePFBarrelFile("#PFBarrel 2011 v1\n1920\n1080\n", &f, &error));
    EXPECT_FALSE(parsePFBarrelFile("#PFBarrel 2011 v1\n1920\nwide\n", &f, &error));
    EXPECT_EQ(error.compare(0, 15, "Not an integer:"), 0) << error;
    EXPECT_FALSE(parsePFBarrelFile("#PFBarrel 2011 v1\n1920\n1080\n1920\n1080\nMedium\n", &f, &error));
    EXPECT_EQ(error.compare(0, 10, "Bad model:"), 0) << error;
}

// A v2 file with one key whose fields default to a valid file; each case replaces one of them.
namespace {
struct PFBarrelFields {
    std::string originalWidth = "1920";
    std::string originalHeight = "1080";
    std::string pixelAspect = "1.0";
    std::string undistortedWidth = "1920";
    std::string undistortedHeight = "1080";
    std::string squeeze = "1.0";
    std::string keyCount = "1";
    std::string c3 = "0.01";
    std::string centerX = "0.5";
    std::string trailer;

    std::string text() const
    {
        return "#PFBarrel 2011 v2\n" + originalWidth + "\n" + originalHeight + "\n" + pixelAspect + "\n" + undistortedWidth + "\n" + undistortedHeight + "\n" + "High Order\n" + squeeze + "\n" + keyCount + "\n" + "1\n" + c3 + "\n0.002\n" + centerX + "\n0.5\n" + trailer;
    }
};
} // namespace

TEST(LensModels, ParsePFBarrelAcceptsTrailingBlankAndCommentLines)
{
    PFBarrelFields fields;
    PFBarrelFile f;
    std::string error;
    ASSERT_TRUE(parsePFBarrelFile(fields.text(), &f, &error)) << error;
    fields.trailer = "\n  \t\n# end\r\n\n";
    EXPECT_TRUE(parsePFBarrelFile(fields.text(), &f, &error)) << error;
}

TEST(LensModels, ParsePFBarrelRejectsNonFiniteNumbers)
{
    for (const char* bad : { "nan", "NAN", "inf", "-inf", "infinity", "1e999" }) {
        PFBarrelFields c3;
        c3.c3 = bad;
        PFBarrelFields centerX;
        centerX.centerX = bad;
        PFBarrelFields squeeze;
        squeeze.squeeze = bad;
        PFBarrelFields aspect;
        aspect.pixelAspect = bad;
        for (const PFBarrelFields& fields : { c3, centerX, squeeze, aspect }) {
            PFBarrelFile f;
            std::string error;
            EXPECT_FALSE(parsePFBarrelFile(fields.text(), &f, &error)) << bad;
            EXPECT_EQ(error.compare(0, 13, "Not a number:"), 0) << bad << ": " << error;
        }
    }
}

TEST(LensModels, ParsePFBarrelRejectsNonPositiveDimensions)
{
    std::vector<PFBarrelFields> cases;
    for (const char* bad : { "0", "-1920" }) {
        PFBarrelFields fields;
        fields.originalWidth = bad;
        cases.push_back(fields);
        fields = PFBarrelFields();
        fields.originalHeight = bad;
        cases.push_back(fields);
        fields = PFBarrelFields();
        fields.undistortedWidth = bad;
        cases.push_back(fields);
        fields = PFBarrelFields();
        fields.undistortedHeight = bad;
        cases.push_back(fields);
    }
    for (const char* bad : { "0", "-1.0" }) {
        PFBarrelFields fields;
        fields.pixelAspect = bad;
        cases.push_back(fields);
    }
    for (const PFBarrelFields& fields : cases) {
        PFBarrelFile f;
        std::string error;
        EXPECT_FALSE(parsePFBarrelFile(fields.text(), &f, &error)) << fields.text();
        EXPECT_EQ(error, "Bad dimensions") << fields.text();
    }
}

TEST(LensModels, ParsePFBarrelRejectsNonPositiveSqueeze)
{
    for (const char* bad : { "0", "0.0", "-1.5" }) {
        PFBarrelFields fields;
        fields.squeeze = bad;
        PFBarrelFile f;
        std::string error;
        EXPECT_FALSE(parsePFBarrelFile(fields.text(), &f, &error)) << bad;
        EXPECT_EQ(error, "Bad squeeze") << bad;
    }
}

TEST(LensModels, ParsePFBarrelRejectsNoKeys)
{
    PFBarrelFile f;
    std::string error;
    const std::string text = "#PFBarrel 2011 v1\n1920\n1080\n1920\n1080\nLow Order\n1\n0\n";
    EXPECT_FALSE(parsePFBarrelFile(text, &f, &error));
    EXPECT_EQ(error, "Bad key count");
    PFBarrelFields negative;
    negative.keyCount = "-1";
    EXPECT_FALSE(parsePFBarrelFile(negative.text(), &f, &error));
    EXPECT_EQ(error, "Bad key count");
}

TEST(LensModels, ParsePFBarrelRejectsTrailingData)
{
    for (const char* trailer : { "2\n", "garbage\n", "\n# note\n0.5", " 7\n" }) {
        PFBarrelFields fields;
        fields.trailer = trailer;
        PFBarrelFile f;
        std::string error;
        EXPECT_FALSE(parsePFBarrelFile(fields.text(), &f, &error)) << trailer;
        EXPECT_EQ(error.compare(0, 36, "Unexpected data after the last key: "), 0) << trailer << ": " << error;
    }
}
