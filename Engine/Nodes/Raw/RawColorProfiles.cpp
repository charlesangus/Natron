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
 * Ported from darktable, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7):
 * src/common/colorspaces.c (the primaries, _PQ_fct(), _HLG_fct(), _colorspaces_create_transfer()
 * and the profile constructors), src/iop/colorin.c (_lerp_lut(), lerp_lut() and the unbounded
 * coefficients of commit_params()), src/common/iop_profile.c and iop_profile.h
 * (_init_unbounded_coeffs(), extrapolate_lut()), src/develop/imageop_math.h
 * (dt_iop_estimate_exp(), dt_iop_eval_exp()) and src/common/colorspaces_inline_conversions.h
 * (dt_Lab_to_XYZ()). Copyright (C) 2009-2026 darktable developers, distributed under the GNU GPL
 * version 3 or later.
 */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "RawColorProfiles.h"

#include <algorithm>
#include <cmath>
#include <vector>

NATRON_NAMESPACE_ENTER

namespace RawColorProfiles {
namespace {
    const int kLutSamples = 0x10000;
    const int kTransferSamples = 4096;

    struct Chromaticity {
        double x, y;
    };

    struct Primaries {
        Chromaticity r, g, b;
    };

    const Chromaticity kD65 = { 0.31271, 0.32902 };
    const Chromaticity kD50 = { 0.34567, 0.35850 };

    const Primaries kSrgbPrimaries = { { 0.6400, 0.3300 }, { 0.3000, 0.6000 }, { 0.1500, 0.0600 } };
    const Primaries kRec2020Primaries = { { 0.7080, 0.2920 }, { 0.1700, 0.7970 }, { 0.1310, 0.0460 } };
    const Primaries kAdobePrimaries = { { 0.6400, 0.3300 }, { 0.2100, 0.7100 }, { 0.1500, 0.0600 } };
    const Primaries kP3Primaries = { { 0.680, 0.320 }, { 0.265, 0.690 }, { 0.150, 0.060 } };
    const Primaries kProPhotoPrimaries = { { 0.734699, 0.265301 }, { 0.159597, 0.840403 }, { 0.036598, 0.000105 } };

    struct ParametricCurve {
        double g, a, b, c, d;
    };

    const ParametricCurve kSrgbCurve = { 2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92, 0.04045 };
    const ParametricCurve kRec709Curve = { 1 / 0.45, 1.0 / 1.099, 0.099 / 1.099, 1.0 / 4.5, 0.081 };
    const double kAdobeGamma = 2.19921875;

    RawColorMath::Mat3
    matrixOf(const Primaries& p,
             const Chromaticity& white)
    {
        const RawColorMath::Mat3 columns = { p.r.x, p.g.x, p.b.x,
                                             p.r.y, p.g.y, p.b.y,
                                             1. - p.r.x - p.r.y, 1. - p.g.x - p.g.y, 1. - p.b.x - p.b.y };

        return *RawColorMath::profileCameraToXyz(columns, { white.x, white.y, 1. - white.x - white.y });
    }

    RawColorMath::Mat3
    d50ToD65()
    {
        return *RawColorMath::bradford(RawColorMath::lcmsD50White(), RawColorMath::d65White());
    }

    double
    parametric(const ParametricCurve& p,
               double x)
    {
        if (x >= p.d) {
            const double e = p.a * x + p.b;

            return (e > 0.) ? std::pow(e, p.g) : 0.;
        }

        return p.c * x;
    }

    // Little CMS's parametric type -4, which cmsReverseToneCurveEx() returns for type 4.
    double
    parametricInverse(const ParametricCurve& p,
                      double y)
    {
        const double e = p.a * p.d + p.b;
        const double disc = (e < 0.) ? 0. : std::pow(e, p.g);

        if (y >= disc) {
            return (std::pow(y, 1. / p.g) - p.b) / p.a;
        }

        return y / p.c;
    }

    double
    pqFct(double x)
    {
        const double M1 = 2610.0 / 16384.0;
        const double M2 = (2523.0 / 4096.0) * 128.0;
        const double C1 = 3424.0 / 4096.0;
        const double C2 = (2413.0 / 4096.0) * 32.0;
        const double C3 = (2392.0 / 4096.0) * 32.0;

        if (x == 0.0) {
            return 0.0;
        }
        const double sign = x;
        x = std::fabs(x);
        const double xpo = std::pow(x, 1.0 / M2);
        const double num = std::max(xpo - C1, 0.0);
        const double den = C2 - C3 * xpo;
        const double res = std::pow(num / den, 1.0 / M1);

        return std::copysign(res, sign);
    }

    double
    hlgFct(double x)
    {
        const double Beta = 0.04;
        const double RA = 5.591816309728916;
        const double B = 0.28466892;
        const double C = 0.5599107295;
        double e = std::max(x * (1.0 - Beta) + Beta, 0.0);

        if (e == 0.0) {
            return 0.0;
        }
        const double sign = e;
        e = std::fabs(e);
        double res;
        if (e <= 0.5) {
            res = e * e / 3.0;
        } else {
            res = (std::exp((e - C) * RA) + B) / 12.0;
        }

        return std::copysign(res, sign);
    }

    // _colorspaces_create_transfer(): the samples Little CMS interpolates linearly.
    std::vector<float>
    transferTable(double (*fct)(double))
    {
        std::vector<float> values(kTransferSamples);

        for (int i = 0; i < kTransferSamples; ++i) {
            const double x = (float)i / (kTransferSamples - 1);
            values[i] = (float)std::min(fct(x), 1.0);
        }

        return values;
    }

    const std::vector<float>&
    transfer(Trc trc)
    {
        static const std::vector<float> pq = transferTable(pqFct);
        static const std::vector<float> hlg = transferTable(hlgFct);

        return (trc == Trc::Pq) ? pq : hlg;
    }

    double
    tabulated(const std::vector<float>& table,
              double x)
    {
        if (x <= 0.) {
            return table.front();
        }
        if (x >= 1.) {
            return table.back();
        }
        const double v = x * (table.size() - 1);
        const int cell0 = (int)std::floor(v);
        const int cell1 = (int)std::ceil(v);
        const double rest = v - cell0;

        return table[cell0] + (table[cell1] - table[cell0]) * rest;
    }

    double
    tabulatedInverse(const std::vector<float>& table,
                     double y)
    {
        if (y <= table.front()) {
            return 0.;
        }
        if (y >= table.back()) {
            return 1.;
        }
        const std::vector<float>::const_iterator upper = std::upper_bound(table.begin(), table.end(), (float)y);
        const int i = (int)(upper - table.begin()) - 1;
        const double y0 = table[i];
        const double y1 = table[i + 1];
        const double rest = (y1 > y0) ? (y - y0) / (y1 - y0) : 0.;

        return (i + rest) / (table.size() - 1);
    }

    double
    curveInverse(Trc trc,
                 double y)
    {
        switch (trc) {
        case Trc::Linear:
            return y;
        case Trc::Srgb:
            return parametricInverse(kSrgbCurve, y);
        case Trc::AdobeRgb:
            return (y > 0.) ? std::pow(y, 1. / kAdobeGamma) : 0.;
        case Trc::Rec709:
            return parametricInverse(kRec709Curve, y);
        case Trc::Pq:
        case Trc::Hlg:
            return tabulatedInverse(transfer(trc), y);
        }

        return y;
    }

    struct Lut {
        std::vector<float> table;
        float coeffs[3];
    };

    // lerp_lut() and extrapolate_lut(), which agree: the index clamped to the table.
    float
    lerpClamped(const std::vector<float>& lut,
                float v)
    {
        const float ft = std::min(std::max(v * (kLutSamples - 1), 0.f), (float)(kLutSamples - 1));
        const int t = (ft < kLutSamples - 2) ? (int)ft : kLutSamples - 2;
        const float f = ft - t;

        return lut[t] * (1.0f - f) + lut[t + 1] * f;
    }

    Lut
    buildLut(Trc trc,
             bool reverse)
    {
        Lut lut;

        lut.table.resize(kLutSamples);
        for (int k = 0; k < kLutSamples; ++k) {
            const float x = k / (kLutSamples - 1.0f);
            lut.table[k] = (float)(reverse ? curveInverse(trc, x) : curve(trc, x));
        }
        const float x[4] = { 0.7f, 0.8f, 0.9f, 1.0f };
        float y[4];
        for (int i = 0; i < 4; ++i) {
            y[i] = lerpClamped(lut.table, x[i]);
        }
        // dt_iop_estimate_exp(): y = y0 * (x / x0)^g through the last point, g averaged.
        const float x0 = x[3];
        const float y0 = y[3];
        float g = 0.0f;
        int cnt = 0;
        for (int k = 0; k < 3; ++k) {
            const float yy = y[k] / y0;
            const float xx = x[k] / x0;
            if ((yy > 0.0f) && (xx > 0.0f)) {
                g += std::log(y[k] / y0) / std::log(x[k] / x0);
                ++cnt;
            }
        }
        g = cnt ? g * (1.0f / cnt) : 1.0f;
        lut.coeffs[0] = 1.0f / x0;
        lut.coeffs[1] = y0;
        lut.coeffs[2] = g;

        return lut;
    }

    const Lut&
    lutOf(Trc trc,
          bool reverse)
    {
        static const Lut srgb = buildLut(Trc::Srgb, false);
        static const Lut adobe = buildLut(Trc::AdobeRgb, false);
        static const Lut rec709 = buildLut(Trc::Rec709, false);
        static const Lut pq = buildLut(Trc::Pq, false);
        static const Lut hlg = buildLut(Trc::Hlg, false);
        static const Lut srgbReverse = buildLut(Trc::Srgb, true);
        static const Lut adobeReverse = buildLut(Trc::AdobeRgb, true);
        static const Lut rec709Reverse = buildLut(Trc::Rec709, true);
        static const Lut pqReverse = buildLut(Trc::Pq, true);
        static const Lut hlgReverse = buildLut(Trc::Hlg, true);

        switch (trc) {
        case Trc::AdobeRgb:
            return reverse ? adobeReverse : adobe;
        case Trc::Rec709:
            return reverse ? rec709Reverse : rec709;
        case Trc::Pq:
            return reverse ? pqReverse : pq;
        case Trc::Hlg:
            return reverse ? hlgReverse : hlg;
        case Trc::Linear:
        case Trc::Srgb:
            break;
        }

        return reverse ? srgbReverse : srgb;
    }

    float
    evalExp(const float* coeffs,
            float x)
    {
        return coeffs[1] * std::pow(x * coeffs[0], coeffs[2]);
    }
} // namespace

Trc
trcOf(Space space)
{
    switch (space) {
    case Space::Srgb:
    case Space::DisplayP3:
    case Space::Brg:
        return Trc::Srgb;
    case Space::AdobeRgb:
        return Trc::AdobeRgb;
    case Space::Rec709:
        return Trc::Rec709;
    case Space::PqRec2020:
    case Space::PqP3:
        return Trc::Pq;
    case Space::HlgRec2020:
    case Space::HlgP3:
        return Trc::Hlg;
    case Space::LinearRec709:
    case Space::LinearRec2020:
    case Space::LinearProPhoto:
    case Space::LinearXyz:
    case Space::Lab:
    case Space::LinearInfraredBgr:
        break;
    }

    return Trc::Linear;
}

RawColorMath::Mat3
toXyz(Space space)
{
    switch (space) {
    case Space::Srgb:
    case Space::Rec709:
        return matrixOf(kSrgbPrimaries, kD65);
    case Space::AdobeRgb:
        return matrixOf(kAdobePrimaries, kD65);
    case Space::LinearRec709:
        return RawColorMath::srgbToXyz();
    case Space::LinearRec2020:
    case Space::PqRec2020:
    case Space::HlgRec2020:
        return matrixOf(kRec2020Primaries, kD65);
    case Space::PqP3:
    case Space::HlgP3:
    case Space::DisplayP3:
        return matrixOf(kP3Primaries, kD65);
    case Space::LinearProPhoto:
        return matrixOf(kProPhotoPrimaries, kD50);
    case Space::LinearXyz:
    case Space::Lab:
        return d50ToD65();
    case Space::LinearInfraredBgr:
        return matrixOf({ kSrgbPrimaries.b, kSrgbPrimaries.g, kSrgbPrimaries.r }, kD65);
    case Space::Brg:
        return matrixOf({ kSrgbPrimaries.b, kSrgbPrimaries.r, kSrgbPrimaries.g }, kD65);
    }

    return RawColorMath::srgbToXyz();
}

RawColorMath::Mat3
fromXyz(Space space)
{
    return *RawColorMath::invert(toXyz(space));
}

double
curve(Trc trc,
      double x)
{
    switch (trc) {
    case Trc::Linear:
        return x;
    case Trc::Srgb:
        return parametric(kSrgbCurve, x);
    case Trc::AdobeRgb:
        return (x > 0.) ? std::pow(x, kAdobeGamma) : 0.;
    case Trc::Rec709:
        return parametric(kRec709Curve, x);
    case Trc::Pq:
    case Trc::Hlg:
        return tabulated(transfer(trc), x);
    }

    return x;
}

float
decode(Trc trc,
       float v)
{
    if (trc == Trc::Linear) {
        return v;
    }
    const Lut& lut = lutOf(trc, false);
    if (v < 1.0f) {
        // _lerp_lut(): negatives clipped, the index left unclamped as v < 1.
        const float ft = std::max(v, 0.0f) * (kLutSamples - 1);
        const int t = (int)ft;
        const float f = ft - t;

        return lut.table[t] * (1.0f - f) + lut.table[t + 1] * f;
    }

    return evalExp(lut.coeffs, v);
}

float
encode(Trc trc,
       float v)
{
    if (trc == Trc::Linear) {
        return v;
    }
    const Lut& lut = lutOf(trc, true);

    return (v < 1.0f) ? lerpClamped(lut.table, v) : evalExp(lut.coeffs, v);
}

std::array<float, 3>
labToXyzD50(float L,
            float a,
            float b)
{
    const float d50[3] = { 0.9642f, 1.0f, 0.8249f };
    const float fy = (L + 16.0f) * (1.0f / 116.0f);
    const float f[3] = { a * (1.0f / 500.0f) + fy, fy, fy - b * (1.0f / 200.0f) };
    const float epsilon = 0.20689655172413796f;
    const float kappa = 24389.0f / 27.0f;
    std::array<float, 3> xyz;

    for (int c = 0; c < 3; ++c) {
        const float x = f[c];
        xyz[c] = d50[c] * ((x > epsilon) ? x * x * x : (116.0f * x - 16.0f) / kappa);
    }

    return xyz;
}
} // namespace RawColorProfiles

NATRON_NAMESPACE_EXIT
