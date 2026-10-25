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

#include "TransformInterp.h"

#include <algorithm>
#include <cmath>

NATRON_NAMESPACE_ENTER

namespace TransformInterp {
namespace {
    using TransformMath::Mat3;

    // The finite-difference step in m: small against a segment, large against rounding.
    const double kStep = 1e-4;

    bool
    isFinite(const Mat3& mat)
    {
        for (int k = 0; k < 9; ++k) {
            if (!std::isfinite(mat.m[k])) {
                return false;
            }
        }

        return true;
    }

    // a * ca + b * cb + c * cc, element by element.
    Mat3
    combine(const Mat3& a,
            double ca,
            const Mat3& b,
            double cb,
            const Mat3& c,
            double cc)
    {
        Mat3 out;
        for (int k = 0; k < 9; ++k) {
            out.m[k] = a.m[k] * ca + b.m[k] * cb + c.m[k] * cc;
        }

        return out;
    }
} // anonymous namespace

Mat3
transformAt(const TransformMath::TransformParams& p,
            double m,
            bool invert)
{
    return TransformMath::inverseTransformCanonical(p, m, invert);
}

int
segmentsForTransform(const TransformMath::TransformParams& p)
{
    const double turn = std::fabs(p.rotate * p.amount);
    if (!std::isfinite(turn)) {
        return kMaxSegments;
    }
    const double wanted = std::ceil(turn / 4.);

    return (int)std::min((double)kMaxSegments, std::max((double)kDefaultSegments, wanted));
}

bool
derivative(const MatrixFn& fn,
           double m,
           Mat3* dH)
{
    Mat3 a, b, c;

    if ((m - kStep >= 0.) && (m + kStep <= 1.)) {
        if (!fn(m - kStep, &a) || !fn(m + kStep, &b)) {
            return false;
        }
        *dH = combine(a, -0.5 / kStep, b, 0.5 / kStep, b, 0.);

        return true;
    }
    // (-3 f(m) + 4 f(m + s) - f(m + 2s)) / 2s, with s negative at the top end.
    const double s = (m + kStep <= 1.) ? kStep : -kStep;
    if (!fn(m, &a) || !fn(m + s, &b) || !fn(m + 2. * s, &c)) {
        return false;
    }
    *dH = combine(a, -1.5 / s, b, 2. / s, c, -0.5 / s);

    return true;
}

bool
MatrixCurve::build(const MatrixFn& fn,
                   int segments)
{
    _h.clear();
    _d.clear();
    segments = std::max(1, std::min(kMaxSegments, segments));

    std::vector<Mat3> h((std::size_t)segments + 1);
    std::vector<Mat3> d((std::size_t)segments + 1);
    for (int k = 0; k <= segments; ++k) {
        // k / segments, so that 0, 1/2 and 1 are knots exactly.
        const double m = (k == segments) ? 1. : (double)k / (double)segments;
        if (!fn(m, &h[k]) || !isFinite(h[k]) || !derivative(fn, m, &d[k]) || !isFinite(d[k])) {
            return false;
        }
    }
    _h.swap(h);
    _d.swap(d);

    return true;
}

MatrixCurve
MatrixCurve::constant(const Mat3& matrix)
{
    MatrixCurve curve;

    curve._h.assign(2, matrix);
    curve._d.assign(2, Mat3());

    return curve;
}

void
MatrixCurve::evaluate(double m,
                      Mat3* H,
                      Mat3* dH) const
{
    const int segments = (int)_h.size() - 1;

    if (!(m > 0.)) {
        m = 0.;
    } else if (m > 1.) {
        m = 1.;
    }
    const double u = m * segments;
    int k = (int)std::floor(u);
    if (k >= segments) {
        k = segments - 1;
    }
    const double t = u - k;
    if (t == 0.) {
        *H = _h[k];
        *dH = _d[k];

        return;
    }
    if (t == 1.) {
        *H = _h[k + 1];
        *dH = _d[k + 1];

        return;
    }
    const double width = 1. / segments;
    const double t2 = t * t;
    const double t3 = t2 * t;
    const double h00 = 2. * t3 - 3. * t2 + 1.;
    const double h10 = t3 - 2. * t2 + t;
    const double h01 = -2. * t3 + 3. * t2;
    const double h11 = t3 - t2;
    // The basis derivatives in t; d/dm = d/dt * segments.
    const double g00 = 6. * t2 - 6. * t;
    const double g10 = 3. * t2 - 4. * t + 1.;
    const double g01 = -6. * t2 + 6. * t;
    const double g11 = 3. * t2 - 2. * t;
    const Mat3& h0 = _h[k];
    const Mat3& h1 = _h[k + 1];
    const Mat3& d0 = _d[k];
    const Mat3& d1 = _d[k + 1];
    for (int i = 0; i < 9; ++i) {
        H->m[i] = h00 * h0.m[i] + h10 * width * d0.m[i] + h01 * h1.m[i] + h11 * width * d1.m[i];
        dH->m[i] = (g00 * h0.m[i] + g01 * h1.m[i]) * segments + g10 * d0.m[i] + g11 * d1.m[i];
    }
} // MatrixCurve::evaluate

bool
MatrixCurve::isConstant() const
{
    for (std::size_t k = 1; k < _h.size(); ++k) {
        if (_h[k] != _h[0]) {
            return false;
        }
    }

    return true;
}

bool
MatrixCurve::operator==(const MatrixCurve& other) const
{
    if (_h.size() != other._h.size()) {
        return false;
    }
    for (std::size_t k = 0; k < _h.size(); ++k) {
        if ((_h[k] != other._h[k]) || (_d[k] != other._d[k])) {
            return false;
        }
    }

    return true;
}

void
MatrixCurve::pack(std::vector<float>* out) const
{
    out->reserve(out->size() + _h.size() * kFloatsPerKnot);
    for (std::size_t k = 0; k < _h.size(); ++k) {
        for (int i = 0; i < 9; ++i) {
            out->push_back((float)_h[k].m[i]);
        }
        for (int i = 0; i < 9; ++i) {
            out->push_back((float)(_h[k].m[i] - (double)(float)_h[k].m[i]));
        }
        for (int i = 0; i < 9; ++i) {
            out->push_back((float)_d[k].m[i]);
        }
    }
}

bool
sourceSample(const Mat3& H,
             const Mat3& dH,
             double px,
             double py,
             double dmdx,
             double dmdy,
             SourceSample* sample)
{
    const double ax = H.m[0] * px + H.m[1] * py + H.m[2];
    const double ay = H.m[3] * px + H.m[4] * py + H.m[5];
    const double z = H.m[6] * px + H.m[7] * py + H.m[8];

    if (!(z > 0.)) {
        return false;
    }
    const double x = ax / z;
    const double y = ay / z;
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return false;
    }
    sample->x = x;
    sample->y = y;

    // The Jacobian of the projective point at a fixed matrix, as Resampler's samplePixel has it.
    const double z2 = z * z;
    double Jxx = (H.m[0] * z - ax * H.m[6]) / z2;
    double Jxy = (H.m[1] * z - ax * H.m[7]) / z2;
    double Jyx = (H.m[3] * z - ay * H.m[6]) / z2;
    double Jyy = (H.m[4] * z - ay * H.m[7]) / z2;

    // Plus the matrix moving with the Map: d(x, y)/dm times the gradient of m.
    if ((dmdx != 0.) || (dmdy != 0.)) {
        const double dax = dH.m[0] * px + dH.m[1] * py + dH.m[2];
        const double day = dH.m[3] * px + dH.m[4] * py + dH.m[5];
        const double dz = dH.m[6] * px + dH.m[7] * py + dH.m[8];
        const double dxdm = (dax * z - ax * dz) / z2;
        const double dydm = (day * z - ay * dz) / z2;
        Jxx += dxdm * dmdx;
        Jxy += dxdm * dmdy;
        Jyx += dydm * dmdx;
        Jyy += dydm * dmdy;
    }
    sample->jacobian[0] = Jxx;
    sample->jacobian[1] = Jxy;
    sample->jacobian[2] = Jyx;
    sample->jacobian[3] = Jyy;

    return true;
} // sourceSample
} // namespace TransformInterp

NATRON_NAMESPACE_EXIT
