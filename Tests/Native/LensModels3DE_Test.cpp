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
#include <functional>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "Engine/Nodes/Transform/LensModels3DE.h"

NATRON_NAMESPACE_USING

namespace {
using namespace LensModels;

const Frame kHD = { 0., 0., 1920., 1080., 1. };

// 35 mm lens on a 16:9 filmback, lens centre slightly off.
const Camera3DE kCamera = { 0., 0., 1., 1., 3.5, 100., 3.6, 2.025, 0.01, -0.02, 1. };

const Classic3DEParams kClassic = { 0.05, 1.1, 0.01, -0.02, 0.01 };
const Anamorphic6Params kAnamorphic6 = {
    0.04, 0.03, 0.01, -0.005,
    0.01, 0.008, 0.002, -0.001, 0.001, 0.0005,
    0.002, 0.001, 0.0005, -0.0003, 0.0002, 0.0001, 0.0001, -0.0001
};
const Fisheye8Params kFisheye = { -0.05, 0.01, 0., 0.001 };
const Camera3DE kFisheyeCamera = { 0., 0., 1., 1., 2.5, 100., 3.6, 2.025, 0.01, -0.02, 1. };
const Standard3DEParams kStandard = { 0.05, 0.01, -0.01, 0.01, 0.002, -0.003, 30., 0.05 };
const Anamorphic4Params kAnamorphic4 = {
    0.04, 0.03, 0.01, -0.005,
    0.01, 0.008, 0.002, -0.001, 0.001, 0.0005,
    10., 1.05, 0.98
};
const Camera3DE kAnamorphicCamera = { 0., 0., 1., 1., 5., 100., 2.2, 1.866, 0.01, -0.02, 2. };

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

void
expectRoundTrip(const Model& m, const Frame& f, int n, double tol)
{
    ASSERT_TRUE(m.valid);
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

// openfx-misc DistortionModel3DEBase: pixels to dn and back, for a format at the origin.
struct OfxBase3DE {
    Frame fr;
    Camera3DE cam;

    double rfb() const
    {
        return std::sqrt(cam.w_fb_cm * cam.w_fb_cm + cam.h_fb_cm * cam.h_fb_cm) / 2.0;
    }

    void pixToDn(double x, double y, double* u, double* v) const
    {
        const double xs = x / (fr.x2 - fr.x1);
        const double ys = y / (fr.y2 - fr.y1);
        const double ux = (xs - cam.xa_fov_unit) / (cam.xb_fov_unit - cam.xa_fov_unit);
        const double uy = (ys - cam.ya_fov_unit) / (cam.yb_fov_unit - cam.ya_fov_unit);
        *u = ((ux - 1.0 / 2.0) * cam.w_fb_cm - cam.x_lco_cm) / rfb();
        *v = ((uy - 1.0 / 2.0) * cam.h_fb_cm - cam.y_lco_cm) / rfb();
    }

    void dnToPix(double u, double v, double* x, double* y) const
    {
        const double ux = (u * rfb() + cam.w_fb_cm / 2 + cam.x_lco_cm) / cam.w_fb_cm;
        const double uy = (v * rfb() + cam.h_fb_cm / 2 + cam.y_lco_cm) / cam.h_fb_cm;
        *x = (ux * (cam.xb_fov_unit - cam.xa_fov_unit) + cam.xa_fov_unit) * (fr.x2 - fr.x1);
        *y = (uy * (cam.yb_fov_unit - cam.ya_fov_unit) + cam.ya_fov_unit) * (fr.y2 - fr.y1);
    }

    void undistort(const std::function<void(double, double, double*, double*)>& dn,
                   double x, double y, double* xu, double* yu) const
    {
        double u, v, uu, vu;
        pixToDn(x, y, &u, &v);
        dn(u, v, &uu, &vu);
        dnToPix(uu, vu, xu, yu);
    }
};

// openfx-misc DistortionModel3DEClassic::undistort_dn.
void
ofxClassic(const Classic3DEParams& p, double xd, double yd, double* xu, double* yu)
{
    const double cxx = p.ld / p.sq, cxy = (p.ld + p.cx) / p.sq, cyx = p.ld + p.cy, cyy = p.ld;
    const double cxxx = p.qu / p.sq, cxxy = 2.0 * p.qu / p.sq, cxyy = p.qu / p.sq;
    const double cyxx = p.qu, cyyx = 2.0 * p.qu, cyyy = p.qu;
    const double p0_2 = xd * xd, p1_2 = yd * yd;
    const double p0_4 = p0_2 * p0_2, p1_4 = p1_2 * p1_2, p01_2 = p0_2 * p1_2;
    *xu = xd * (1 + cxx * p0_2 + cxy * p1_2 + cxxx * p0_4 + cxxy * p01_2 + cxyy * p1_4);
    *yu = yd * (1 + cyx * p0_2 + cyy * p1_2 + cyxx * p0_4 + cyyx * p01_2 + cyyy * p1_4);
}

// openfx-misc DistortionModel3DEAnamorphic6::undistort_dn, written out from its cos(2m phi) terms.
void
ofxAnamorphic6(const Anamorphic6Params& p, double x, double y, double* xu, double* yu)
{
    const double r2 = x * x + y * y;
    const double c2 = (x * x - y * y) / r2; // cos 2 phi
    const double s2 = 2. * x * y / r2; // sin 2 phi
    const double c4 = c2 * c2 - s2 * s2;
    const double s4 = 2. * c2 * s2;
    const double c6 = c4 * c2 - s4 * s2;
    const double r4 = r2 * r2, r6 = r4 * r2;
    *xu = x * (1. + r2 * (p.cx02 + p.cx22 * c2) + r4 * (p.cx04 + p.cx24 * c2 + p.cx44 * c4) + r6 * (p.cx06 + p.cx26 * c2 + p.cx46 * c4 + p.cx66 * c6));
    *yu = y * (1. + r2 * (p.cy02 + p.cy22 * c2) + r4 * (p.cy04 + p.cy24 * c2 + p.cy44 * c4) + r6 * (p.cy06 + p.cy26 * c2 + p.cy46 * c4 + p.cy66 * c6));
}

// openfx-misc DistortionModel3DEFishEye8::undistort_dn, inside its unclipped region.
void
ofxFisheye(const Camera3DE& cam, const Fisheye8Params& p, double x, double y, double* xu, double* yu)
{
    const double rfb = std::sqrt(cam.w_fb_cm * cam.w_fb_cm + cam.h_fb_cm * cam.h_fb_cm) / 2.0;
    const double f_dn = cam.fl_cm / rfb;
    const double r_esa = std::sqrt(x * x + y * y);
    const double phi = 2 * std::asin(r_esa / (2 * f_dn));
    const double r_plain = f_dn * std::tan(phi);
    const double xp = x * r_plain / r_esa, yp = y * r_plain / r_esa;
    const double r2 = xp * xp + yp * yp, r4 = r2 * r2, r6 = r4 * r2, r8 = r4 * r4;
    const double q = 1. + p.c2 * r2 + p.c4 * r4 + p.c6 * r6 + p.c8 * r8;
    *xu = xp * q;
    *yu = yp * q;
}

// openfx-misc DistortionModel3DEStandard::undistort_dn.
void
ofxStandard(const Standard3DEParams& p, double x, double y, double* xu, double* yu)
{
    const double q = std::sqrt(1.0 + p.b);
    const double c = std::cos(p.phi * M_PI / 180.0), s = std::sin(p.phi * M_PI / 180.0);
    const double mxx = c * c * q + s * s / q, mxy = (q - 1.0 / q) * c * s, myy = c * c / q + s * s * q;
    const double x2 = x * x, y2 = y * y, xy = x * y, r2 = x2 + y2, r4 = r2 * r2;
    const double x_dn = x * (1.0 + p.c2 * r2 + p.c4 * r4) + (r2 + 2.0 * x2) * (p.u1 + p.u3 * r2) + 2.0 * xy * (p.v1 + p.v3 * r2);
    const double y_dn = y * (1.0 + p.c2 * r2 + p.c4 * r4) + (r2 + 2.0 * y2) * (p.v1 + p.v3 * r2) + 2.0 * xy * (p.u1 + p.u3 * r2);
    *xu = mxx * x_dn + mxy * y_dn;
    *yu = mxy * x_dn + myy * y_dn;
}

// openfx-misc DistortionModel3DEAnamorphic4::undistort_dn.
void
ofxAnamorphic4(const Camera3DE& cam, const Anamorphic4Params& p, double xd, double yd, double* xu, double* yu)
{
    const double cosphi = std::cos(p.phi * M_PI / 180.0), sinphi = std::sin(p.phi * M_PI / 180.0);
    Anamorphic6Params a6 = {};
    a6.cx02 = p.cx02;
    a6.cy02 = p.cy02;
    a6.cx22 = p.cx22;
    a6.cy22 = p.cy22;
    a6.cx04 = p.cx04;
    a6.cy04 = p.cy04;
    a6.cx24 = p.cx24;
    a6.cy24 = p.cy24;
    a6.cx44 = p.cx44;
    a6.cy44 = p.cy44;
    xd /= cam.pa;
    const double x = cosphi * xd + sinphi * yd;
    const double y = -sinphi * xd + cosphi * yd;
    double xq, yq;
    ofxAnamorphic6(a6, x, y, &xq, &yq);
    xq *= cam.pa;
    yq *= p.sqy;
    xq *= p.sqx;
    *xu = cosphi * xq - sinphi * yq;
    *yu = sinphi * xq + cosphi * yq;
}

void
expectMatchesReference(const Model& m, const Frame& fr,
                       const std::function<void(double, double, double*, double*)>& reference)
{
    for (double x : { 0., 333.3, 960.7, 1500., 1920. }) {
        for (double y : { 0., 211.7, 540.3, 1080. }) {
            SCOPED_TRACE(where(eDirectionUndistort, x, y));
            double xu, yu, rx, ry;
            ASSERT_TRUE(m.undistort(x, y, &xu, &yu));
            reference(x, y, &rx, &ry);
            EXPECT_NEAR(xu, rx, 1e-9 * fr.x2);
            EXPECT_NEAR(yu, ry, 1e-9 * fr.x2);
        }
    }
}
} // anon namespace

TEST(LensModels3DE, IdentityParametersAreIdentity)
{
    const Classic3DEParams classic = { 0., 1., 0., 0., 0. };
    const Model m = make3DEClassic(kHD, kCamera, classic);
    for (Direction d : { eDirectionDistort, eDirectionUndistort }) {
        Sample s;
        ASSERT_TRUE(m.map(d, 123.4, 567.8, &s));
        EXPECT_NEAR(s.x, 123.4, 1e-9);
        EXPECT_NEAR(s.y, 567.8, 1e-9);
        EXPECT_NEAR(s.J[0], 1., 1e-12);
        EXPECT_NEAR(s.J[1], 0., 1e-12);
        EXPECT_NEAR(s.J[2], 0., 1e-12);
        EXPECT_NEAR(s.J[3], 1., 1e-12);
    }
}

TEST(LensModels3DE, ClassicMatchesOpenFXMisc)
{
    const OfxBase3DE base = { kHD, kCamera };
    expectMatchesReference(make3DEClassic(kHD, kCamera, kClassic), kHD,
                           [&](double x, double y, double* xu, double* yu) {
                               base.undistort([](double u, double v, double* uu, double* vu) { ofxClassic(kClassic, u, v, uu, vu); }, x, y, xu, yu);
                           });
}

TEST(LensModels3DE, ClassicRoundTripAndJacobian)
{
    const Model m = make3DEClassic(kHD, kCamera, kClassic);
    expectRoundTrip(m, kHD, 16, 1e-6);
    expectJacobianMatchesFiniteDifference(m, kHD, 8);
}

TEST(LensModels3DE, Anamorphic6MatchesOpenFXMisc)
{
    const OfxBase3DE base = { kHD, kCamera };
    expectMatchesReference(make3DEAnamorphic6(kHD, kCamera, kAnamorphic6), kHD,
                           [&](double x, double y, double* xu, double* yu) {
                               base.undistort([](double u, double v, double* uu, double* vu) { ofxAnamorphic6(kAnamorphic6, u, v, uu, vu); }, x, y, xu, yu);
                           });
}

TEST(LensModels3DE, Anamorphic6RoundTripAndJacobian)
{
    const Model m = make3DEAnamorphic6(kHD, kCamera, kAnamorphic6);
    expectRoundTrip(m, kHD, 16, 1e-6);
    expectJacobianMatchesFiniteDifference(m, kHD, 8);
}

TEST(LensModels3DE, FisheyeMatchesOpenFXMisc)
{
    const OfxBase3DE base = { kHD, kFisheyeCamera };
    expectMatchesReference(make3DEFisheye8(kHD, kFisheyeCamera, kFisheye), kHD,
                           [&](double x, double y, double* xu, double* yu) {
                               base.undistort([](double u, double v, double* uu, double* vu) { ofxFisheye(kFisheyeCamera, kFisheye, u, v, uu, vu); }, x, y, xu, yu);
                           });
}

TEST(LensModels3DE, FisheyeRoundTripAndJacobian)
{
    const Model m = make3DEFisheye8(kHD, kFisheyeCamera, kFisheye);
    expectRoundTrip(m, kHD, 16, 1e-6);
    expectJacobianMatchesFiniteDifference(m, kHD, 8);
}

// A 10 mm fisheye on this filmback sees past 90 degrees off axis in the corners: those points are
// outside the model, and both directions report it instead of returning NaN.
TEST(LensModels3DE, FisheyeBeyondDomainFails)
{
    Camera3DE wide = kFisheyeCamera;
    wide.fl_cm = 1.0;
    const Model m = make3DEFisheye8(kHD, wide, kFisheye);
    ASSERT_TRUE(m.valid);
    Sample s;
    EXPECT_TRUE(m.map(eDirectionUndistort, 960., 540., &s));
    EXPECT_FALSE(m.map(eDirectionUndistort, 0., 0., &s));
    EXPECT_FALSE(m.map(eDirectionUndistort, 1920., 1080., &s));
    for (int i = 0; i <= 32; ++i) {
        const double x = -4000. + 250. * i;
        for (Direction d : { eDirectionDistort, eDirectionUndistort }) {
            if (m.map(d, x, -3000., &s)) {
                EXPECT_TRUE(std::isfinite(s.x) && std::isfinite(s.y));
                for (int k = 0; k < 4; ++k) {
                    EXPECT_TRUE(std::isfinite(s.J[k]));
                }
            }
        }
    }
}

TEST(LensModels3DE, StandardMatchesOpenFXMisc)
{
    const OfxBase3DE base = { kHD, kCamera };
    expectMatchesReference(make3DEStandard(kHD, kCamera, kStandard), kHD,
                           [&](double x, double y, double* xu, double* yu) {
                               base.undistort([](double u, double v, double* uu, double* vu) { ofxStandard(kStandard, u, v, uu, vu); }, x, y, xu, yu);
                           });
}

TEST(LensModels3DE, StandardRoundTripAndJacobian)
{
    const Model m = make3DEStandard(kHD, kCamera, kStandard);
    expectRoundTrip(m, kHD, 16, 1e-6);
    expectJacobianMatchesFiniteDifference(m, kHD, 8);
}

TEST(LensModels3DE, Anamorphic4MatchesOpenFXMisc)
{
    const OfxBase3DE base = { kHD, kAnamorphicCamera };
    expectMatchesReference(make3DEAnamorphic4(kHD, kAnamorphicCamera, kAnamorphic4), kHD,
                           [&](double x, double y, double* xu, double* yu) {
                               base.undistort([](double u, double v, double* uu, double* vu) { ofxAnamorphic4(kAnamorphicCamera, kAnamorphic4, u, v, uu, vu); }, x, y, xu, yu);
                           });
}

TEST(LensModels3DE, Anamorphic4RoundTripAndJacobian)
{
    const Model m = make3DEAnamorphic4(kHD, kAnamorphicCamera, kAnamorphic4);
    expectRoundTrip(m, kHD, 16, 1e-6);
    expectJacobianMatchesFiniteDifference(m, kHD, 8);
}

TEST(LensModels3DE, OffsetOriginShiftsTheMapping)
{
    const double ox = 100., oy = -50.;
    const Frame shifted = { kHD.x1 + ox, kHD.y1 + oy, kHD.x2 + ox, kHD.y2 + oy, 1. };
    const Model pairs[][2] = {
        { make3DEClassic(kHD, kCamera, kClassic), make3DEClassic(shifted, kCamera, kClassic) },
        { make3DEAnamorphic6(kHD, kCamera, kAnamorphic6), make3DEAnamorphic6(shifted, kCamera, kAnamorphic6) },
        { make3DEFisheye8(kHD, kFisheyeCamera, kFisheye), make3DEFisheye8(shifted, kFisheyeCamera, kFisheye) },
        { make3DEStandard(kHD, kCamera, kStandard), make3DEStandard(shifted, kCamera, kStandard) },
        { make3DEAnamorphic4(kHD, kAnamorphicCamera, kAnamorphic4), make3DEAnamorphic4(shifted, kAnamorphicCamera, kAnamorphic4) },
    };
    for (const auto& pair : pairs) {
        for (Direction d : { eDirectionDistort, eDirectionUndistort }) {
            Sample a, b;
            ASSERT_TRUE(pair[0].map(d, 300., 700., &a));
            ASSERT_TRUE(pair[1].map(d, 300. + ox, 700. + oy, &b));
            EXPECT_NEAR(b.x, a.x + ox, 1e-8);
            EXPECT_NEAR(b.y, a.y + oy, 1e-8);
        }
    }
}

TEST(LensModels3DE, DegenerateCameraIsInvalid)
{
    Camera3DE cam = kCamera;
    cam.xb_fov_unit = cam.xa_fov_unit;
    EXPECT_FALSE(make3DEClassic(kHD, cam, kClassic).valid);
    Standard3DEParams p = kStandard;
    p.b = -2.;
    EXPECT_FALSE(make3DEStandard(kHD, kCamera, p).valid);
}
