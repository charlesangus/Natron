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

/*
 * Ports of the 3DE4 models of openfx-misc's DistortionModel.cpp, (C) 2013-2018 INRIA and
 * 2018-2021 The Natron Developers, GPL v2 or later.
 */

#include "LensModels3DE.h"

#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846264338327950288
#endif

NATRON_NAMESPACE_ENTER

namespace LensModels {
namespace {
    // Polynomial terms of the anamorphic core, one block per output axis, in the order
    // x^2, y^2, x^4, x^2 y^2, y^4, x^6, x^4 y^2, x^2 y^4, y^6.
    const int kAnamorphicTerms = 9;
    const int kAnamorphicY = kAnamorphicTerms;

    enum FisheyeCoeff {
        kFisheyeFocal = 0,
        kFisheyeC2,
        kFisheyeC4,
        kFisheyeC6,
        kFisheyeC8
    };

    enum StandardCoeff {
        kStandardC2 = 0,
        kStandardU1,
        kStandardV1,
        kStandardC4,
        kStandardU3,
        kStandardV3
    };

    // q = 1 + a . (X, Y, X^2, XY, Y^2, X^3, X^2 Y, X Y^2, Y^3) with X = x^2, Y = y^2, and dq/dX, dq/dY.
    void
    anamorphicFactor(const double* a, double X, double Y, double* q, double* qX, double* qY)
    {
        *q = 1. + a[0] * X + a[1] * Y
            + a[2] * X * X + a[3] * X * Y + a[4] * Y * Y
            + a[5] * X * X * X + a[6] * X * X * Y + a[7] * X * Y * Y + a[8] * Y * Y * Y;
        *qX = a[0] + 2. * a[2] * X + a[3] * Y + 3. * a[5] * X * X + 2. * a[6] * X * Y + a[7] * Y * Y;
        *qY = a[1] + a[3] * X + 2. * a[4] * Y + a[6] * X * X + 2. * a[7] * X * Y + 3. * a[8] * Y * Y;
    }

    // Pixels to 3DE4's diagonally normalized space: pixels to unit coordinates over the format,
    // out of the field of view, to filmback centimetres about the lens centre, then over the
    // filmback half-diagonal.
    Affine
    pixelToDn(const Frame& frame, const Camera3DE& camera)
    {
        const double w = frame.x2 - frame.x1;
        const double h = frame.y2 - frame.y1;
        const double xd = camera.xb_fov_unit - camera.xa_fov_unit;
        const double yd = camera.yb_fov_unit - camera.ya_fov_unit;
        const double r_fb_cm = std::sqrt(camera.w_fb_cm * camera.w_fb_cm + camera.h_fb_cm * camera.h_fb_cm) / 2.;
        Affine a;
        a.m00 = camera.w_fb_cm / (r_fb_cm * w * xd);
        a.m01 = 0.;
        a.m10 = 0.;
        a.m11 = camera.h_fb_cm / (r_fb_cm * h * yd);
        a.tx = ((-frame.x1 / w - camera.xa_fov_unit) / xd - 0.5) * camera.w_fb_cm / r_fb_cm - camera.x_lco_cm / r_fb_cm;
        a.ty = ((-frame.y1 / h - camera.ya_fov_unit) / yd - 0.5) * camera.h_fb_cm / r_fb_cm - camera.y_lco_cm / r_fb_cm;
        return a;
    }

    Model
    model3DE(ModelType type, const Frame& frame, const Camera3DE& camera)
    {
        Model m {};
        m.type = type;
        m.core = eDirectionUndistort;
        m.toCore = pixelToDn(frame, camera);
        Affine dnToPixel = {};
        affineInvert(m.toCore, &dnToPixel);
        m.fromCore = dnToPixel;
        return m;
    }

    Affine
    linear(double m00, double m01, double m10, double m11)
    {
        Affine a = { m00, m01, m10, m11, 0., 0. };
        return a;
    }
} // anon namespace

bool
coreAnamorphic3DE(const double* c, double x, double y, double* gx, double* gy, double J[4])
{
    const double X = x * x;
    const double Y = y * y;
    double qx, qxX, qxY, qy, qyX, qyY;
    anamorphicFactor(c, X, Y, &qx, &qxX, &qxY);
    anamorphicFactor(c + kAnamorphicY, X, Y, &qy, &qyX, &qyY);
    *gx = x * qx;
    *gy = y * qy;
    J[0] = qx + 2. * X * qxX;
    J[1] = 2. * x * y * qxY;
    J[2] = 2. * x * y * qyX;
    J[3] = qy + 2. * Y * qyY;
    return true;
}

bool
coreFisheye3DE(const double* c, double x, double y, double* gx, double* gy, double J[4])
{
    const double f = c[kFisheyeFocal];
    if (!(f > 0.)) {
        return false;
    }
    // Equisolid-angle to rectilinear: r = 2 f sin(phi / 2) becomes R = f tan(phi), defined
    // for phi below 90 degrees only.
    double px = x;
    double py = y;
    double Je[4] = { 1., 0., 0., 1. };
    const double r2 = x * x + y * y;
    if (r2 > 0.) {
        const double r = std::sqrt(r2);
        const double arg = r / (2. * f);
        if (!(arg < 1.)) {
            return false;
        }
        const double phi = 2. * std::asin(arg);
        if (!(phi < M_PI / 2.)) {
            return false;
        }
        const double cosPhi = std::cos(phi);
        const double R = f * std::tan(phi);
        const double s = R / r;
        const double dRdr = 1. / (cosPhi * cosPhi * std::sqrt(1. - arg * arg));
        const double k = (dRdr - s) / r2;
        px = x * s;
        py = y * s;
        Je[0] = s + k * x * x;
        Je[1] = k * x * y;
        Je[2] = k * x * y;
        Je[3] = s + k * y * y;
    }
    const double R2 = px * px + py * py;
    const double q = 1. + R2 * (c[kFisheyeC2] + R2 * (c[kFisheyeC4] + R2 * (c[kFisheyeC6] + R2 * c[kFisheyeC8])));
    const double dq = c[kFisheyeC2] + R2 * (2. * c[kFisheyeC4] + R2 * (3. * c[kFisheyeC6] + 4. * R2 * c[kFisheyeC8]));
    *gx = px * q;
    *gy = py * q;
    const double Jr0 = q + 2. * dq * px * px;
    const double Jr1 = 2. * dq * px * py;
    const double Jr3 = q + 2. * dq * py * py;
    J[0] = Jr0 * Je[0] + Jr1 * Je[2];
    J[1] = Jr0 * Je[1] + Jr1 * Je[3];
    J[2] = Jr1 * Je[0] + Jr3 * Je[2];
    J[3] = Jr1 * Je[1] + Jr3 * Je[3];
    return true;
}

bool
coreStandard3DE(const double* c, double x, double y, double* gx, double* gy, double J[4])
{
    const double x2 = x * x;
    const double y2 = y * y;
    const double xy = x * y;
    const double r2 = x2 + y2;
    const double K = 1. + c[kStandardC2] * r2 + c[kStandardC4] * r2 * r2;
    const double Kr = c[kStandardC2] + 2. * c[kStandardC4] * r2;
    const double U = c[kStandardU1] + c[kStandardU3] * r2;
    const double V = c[kStandardV1] + c[kStandardV3] * r2;
    const double u3 = c[kStandardU3];
    const double v3 = c[kStandardV3];
    *gx = x * K + (r2 + 2. * x2) * U + 2. * xy * V;
    *gy = y * K + (r2 + 2. * y2) * V + 2. * xy * U;
    J[0] = K + 2. * x2 * Kr + 6. * x * U + 2. * x * u3 * (r2 + 2. * x2) + 2. * y * V + 4. * x2 * y * v3;
    J[1] = 2. * xy * Kr + 2. * y * U + 2. * y * u3 * (r2 + 2. * x2) + 2. * x * V + 4. * x * y2 * v3;
    J[2] = 2. * xy * Kr + 2. * x * V + 2. * x * v3 * (r2 + 2. * y2) + 2. * y * U + 4. * x2 * y * u3;
    J[3] = K + 2. * y2 * Kr + 6. * y * V + 2. * y * v3 * (r2 + 2. * y2) + 2. * x * U + 4. * x * y2 * u3;
    return true;
}

Model
make3DEClassic(const Frame& frame, const Camera3DE& camera, const Classic3DEParams& params)
{
    Model m = model3DE(eModel3DEClassic, frame, camera);
    m.c[0] = params.ld / params.sq;
    m.c[1] = (params.ld + params.cx) / params.sq;
    m.c[2] = params.qu / params.sq;
    m.c[3] = 2. * params.qu / params.sq;
    m.c[4] = params.qu / params.sq;
    m.c[kAnamorphicY + 0] = params.ld + params.cy;
    m.c[kAnamorphicY + 1] = params.ld;
    m.c[kAnamorphicY + 2] = params.qu;
    m.c[kAnamorphicY + 3] = 2. * params.qu;
    m.c[kAnamorphicY + 4] = params.qu;
    finalizeModel(&m);
    return m;
}

namespace {
    // The expansion of sum c(2m, 2n) r^2n cos(2m phi) into monomials (ldpk
    // generic_anamorphic_distortion::prepare()).
    void
    anamorphicCoefficients(double c02, double c22, double c04, double c24, double c44,
                           double c06, double c26, double c46, double c66, double* a)
    {
        a[0] = c02 + c22;
        a[1] = c02 - c22;
        a[2] = c04 + c24 + c44;
        a[3] = 2. * c04 - 6. * c44;
        a[4] = c04 - c24 + c44;
        a[5] = c06 + c26 + c46 + c66;
        a[6] = 3. * c06 + c26 - 5. * c46 - 15. * c66;
        a[7] = 3. * c06 - c26 - 5. * c46 + 15. * c66;
        a[8] = c06 - c26 + c46 - c66;
    }
} // anon namespace

Model
make3DEAnamorphic6(const Frame& frame, const Camera3DE& camera, const Anamorphic6Params& p)
{
    Model m = model3DE(eModel3DEAnamorphic6, frame, camera);
    anamorphicCoefficients(p.cx02, p.cx22, p.cx04, p.cx24, p.cx44, p.cx06, p.cx26, p.cx46, p.cx66, m.c);
    anamorphicCoefficients(p.cy02, p.cy22, p.cy04, p.cy24, p.cy44, p.cy06, p.cy26, p.cy46, p.cy66, m.c + kAnamorphicY);
    finalizeModel(&m);
    return m;
}

Model
make3DEFisheye8(const Frame& frame, const Camera3DE& camera, const Fisheye8Params& params)
{
    Model m = model3DE(eModel3DEFisheye8, frame, camera);
    const double r_fb_cm = std::sqrt(camera.w_fb_cm * camera.w_fb_cm + camera.h_fb_cm * camera.h_fb_cm) / 2.;
    m.c[kFisheyeFocal] = camera.fl_cm / r_fb_cm;
    m.c[kFisheyeC2] = params.c2;
    m.c[kFisheyeC4] = params.c4;
    m.c[kFisheyeC6] = params.c6;
    m.c[kFisheyeC8] = params.c8;
    finalizeModel(&m);
    return m;
}

Model
make3DEStandard(const Frame& frame, const Camera3DE& camera, const Standard3DEParams& params)
{
    Model m = model3DE(eModel3DEStandard, frame, camera);
    // ldpk cylindric_extender_2: scale by sqrt(1 + b) along phi and by its inverse across it.
    const double q = std::sqrt(1. + params.b);
    const double c = std::cos(params.phi * M_PI / 180.);
    const double s = std::sin(params.phi * M_PI / 180.);
    const double mxy = (q - 1. / q) * c * s;
    m.fromCore = affineCompose(m.fromCore, linear(c * c * q + s * s / q, mxy, mxy, c * c / q + s * s * q));
    m.c[kStandardC2] = params.c2;
    m.c[kStandardU1] = params.u1;
    m.c[kStandardV1] = params.v1;
    m.c[kStandardC4] = params.c4;
    m.c[kStandardU3] = params.u3;
    m.c[kStandardV3] = params.v3;
    finalizeModel(&m);
    return m;
}

Model
make3DEAnamorphic4(const Frame& frame, const Camera3DE& camera, const Anamorphic4Params& p)
{
    Model m = model3DE(eModel3DEAnamorphic4, frame, camera);
    const double c = std::cos(p.phi * M_PI / 180.);
    const double s = std::sin(p.phi * M_PI / 180.);
    // ldpk: rotation(squeeze_x(squeeze_y(pa(anamorphic(rotation^-1(pa^-1(p))))))).
    m.toCore = affineCompose(linear(c / camera.pa, s, -s / camera.pa, c), m.toCore);
    m.fromCore = affineCompose(m.fromCore, linear(c * camera.pa * p.sqx, -s * p.sqy, s * camera.pa * p.sqx, c * p.sqy));
    anamorphicCoefficients(p.cx02, p.cx22, p.cx04, p.cx24, p.cx44, 0., 0., 0., 0., m.c);
    anamorphicCoefficients(p.cy02, p.cy22, p.cy04, p.cy24, p.cy44, 0., 0., 0., 0., m.c + kAnamorphicY);
    finalizeModel(&m);
    return m;
}
} // namespace LensModels

NATRON_NAMESPACE_EXIT
