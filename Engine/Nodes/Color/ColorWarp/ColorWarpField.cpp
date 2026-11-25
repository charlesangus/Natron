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

#include "ColorWarpField.h"

#include <algorithm>
#include <cmath>

NATRON_NAMESPACE_ENTER

namespace {
const double kIdentityMatrix[9] = { 1., 0., 0., 0., 1., 0., 0., 0., 1. };
const double kZero3[3] = { 0., 0., 0. };

// t = 2 r^2 / falloff^2 >= 32 is r >= 4 falloffs, where a pin's weight exp(-t) / (2t) is below
// 2e-16 of the identity weight, so it is dropped.
const double kGaussianCutoffExponent = 2. * kColorWarpGaussianCutoff * kColorWarpGaussianCutoff;

// A Gaussian weight whose exponent is below this is treated as an exact hit: 1 / (2t) would
// otherwise overflow while the blend has already converged to that pin's displacement.
const double kGaussianExactHit = 1e-30;

double
dist2(const double* a, const double* b)
{
    const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];

    return dx * dx + dy * dy + dz * dz;
}

// stretch >= -1, so the result is never negative short of rounding, which the clamp removes.
double
lightnessDist2(const double* a, const double* b, const double* axis, double stretch)
{
    const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    const double along = dx * axis[0] + dy * axis[1] + dz * axis[2];

    return std::max(0., dx * dx + dy * dy + dz * dz + stretch * along * along);
}

bool
isNonZero(const double* d)
{
    return (d[0] != 0.) || (d[1] != 0.) || (d[2] != 0.);
}

double
rbfPhi(ColorWarpKernelEnum kernel, double r)
{
    // -r rather than r: r is conditionally negative definite, so with the sign flipped the
    // diagonal regularisation adds to a positive definite form instead of cancelling it.
    return (kernel == eColorWarpKernelRbfCubic) ? r * r * r : -r;
}

// Cyclic Jacobi on a symmetric 3x3: a is destroyed, its eigenvalues are left on the diagonal,
// and the columns of v are the eigenvectors.
void
jacobiEigen3(double a[3][3], double v[3][3])
{
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            v[i][j] = (i == j) ? 1. : 0.;
        }
    }
    for (int sweep = 0; sweep < 32; ++sweep) {
        const double off = std::fabs(a[0][1]) + std::fabs(a[0][2]) + std::fabs(a[1][2]);
        if (off < 1e-300) {
            return;
        }
        for (int p = 0; p < 2; ++p) {
            for (int q = p + 1; q < 3; ++q) {
                if (std::fabs(a[p][q]) < 1e-300) {
                    continue;
                }
                const double theta = (a[q][q] - a[p][p]) / (2. * a[p][q]);
                const double t = ((theta >= 0.) ? 1. : -1.) / (std::fabs(theta) + std::sqrt(theta * theta + 1.));
                const double c = 1. / std::sqrt(t * t + 1.);
                const double s = t * c;
                for (int k = 0; k < 3; ++k) {
                    const double akp = a[k][p], akq = a[k][q];
                    a[k][p] = c * akp - s * akq;
                    a[k][q] = s * akp + c * akq;
                }
                for (int k = 0; k < 3; ++k) {
                    const double apk = a[p][k], aqk = a[q][k];
                    a[p][k] = c * apk - s * aqk;
                    a[q][k] = s * apk + c * aqk;
                }
                for (int k = 0; k < 3; ++k) {
                    const double vkp = v[k][p], vkq = v[k][q];
                    v[k][p] = c * vkp - s * vkq;
                    v[k][q] = s * vkp + c * vkq;
                }
            }
        }
    }
}

// Gaussian elimination with partial pivoting on an n x n system with three right-hand sides,
// row-major, both destroyed. An unknown whose pivot vanishes is set to zero rather than failing,
// so a degenerate pin layout still yields a usable field.
void
solveDense(std::vector<double>& a, std::vector<double>& b, int n, std::vector<double>& x)
{
    double maxAbs = 0.;

    for (double v : a) {
        maxAbs = std::max(maxAbs, std::fabs(v));
    }
    const double tiny = 1e-14 * std::max(maxAbs, 1e-300);
    std::vector<bool> dead(n, false);
    for (int col = 0; col < n; ++col) {
        int pivot = col;
        double best = std::fabs(a[col * n + col]);
        for (int row = col + 1; row < n; ++row) {
            const double v = std::fabs(a[row * n + col]);
            if (v > best) {
                best = v;
                pivot = row;
            }
        }
        if (best <= tiny) {
            dead[col] = true;
            continue;
        }
        if (pivot != col) {
            for (int k = 0; k < n; ++k) {
                std::swap(a[col * n + k], a[pivot * n + k]);
            }
            for (int k = 0; k < 3; ++k) {
                std::swap(b[col * 3 + k], b[pivot * 3 + k]);
            }
        }
        const double inv = 1. / a[col * n + col];
        for (int row = col + 1; row < n; ++row) {
            const double f = a[row * n + col] * inv;
            if (f == 0.) {
                continue;
            }
            for (int k = col; k < n; ++k) {
                a[row * n + k] -= f * a[col * n + k];
            }
            for (int k = 0; k < 3; ++k) {
                b[row * 3 + k] -= f * b[col * 3 + k];
            }
        }
    }
    x.assign(static_cast<std::size_t>(n) * 3, 0.);
    for (int row = n - 1; row >= 0; --row) {
        if (dead[row]) {
            continue;
        }
        for (int k = 0; k < 3; ++k) {
            double acc = b[row * 3 + k];
            for (int c = row + 1; c < n; ++c) {
                acc -= a[row * n + c] * x[c * 3 + k];
            }
            x[row * 3 + k] = acc / a[row * n + row];
        }
    }
}
} // anon namespace

double
colorWarpFade(double distance,
              double radius,
              double invWidth)
{
    if (!(distance > radius)) {
        return 1.;
    }
    if (invWidth <= 0.) {
        return 0.;
    }
    const double t = (distance - radius) * invWidth;
    if (!(t < 1.)) {
        return 0.;
    }

    return 1. - t * t * (3. - 2. * t);
}

const char*
colorWarpKernelName(ColorWarpKernelEnum kernel)
{
    switch (kernel) {
    case eColorWarpKernelGaussian:
        return "gaussian";
    case eColorWarpKernelRbfLinear:
        return "rbflinear";
    case eColorWarpKernelRbfCubic:
        return "rbfcubic";
    }

    return "unknown";
}

ColorWarpField::ColorWarpField()
    : _space(eColorWarpSpaceLinearRgb)
    , _kernel(eColorWarpKernelGaussian)
    , _basis(kIdentityMatrix)
    , _pinCount(0)
    , _lightnessAxis()
    , _lightnessStretch(0.)
    , _affine()
    , _origin()
    , _invScale(1.)
    , _fadeRadius(0.)
    , _fadeInvWidth(0.)
{
}

void
ColorWarpField::prepare(const std::vector<ColorWarpPin>& pins,
                        ColorWarpSpaceEnum space,
                        const ColorWarpSpaceBasis& basis,
                        ColorWarpKernelEnum kernel,
                        double regularisation,
                        double lightnessFalloff)
{
    _space = space;
    _kernel = kernel;
    _basis = basis;
    _pinCount = 0;
    _centres.clear();
    _coefficients.clear();
    _invTwoSigma2.clear();
    std::fill(_affine, _affine + 12, 0.);
    std::fill(_origin, _origin + 3, 0.);
    _invScale = 1.;
    _fadeRadius = 0.;
    _fadeInvWidth = 0.;
    colorWarpLightnessAxis(space, basis, _lightnessAxis);
    const double k = (std::isfinite(lightnessFalloff) && (lightnessFalloff > 0.)) ? lightnessFalloff : 1.;
    _lightnessStretch = (kernel == eColorWarpKernelGaussian) ? (1. / (k * k) - 1.) : 0.;

    const double falloffScale = colorWarpFalloffScale(space);
    std::vector<double> sources, displacements, falloffs;
    for (const ColorWarpPin& pin : pins) {
        if (!pin.enabled) {
            continue;
        }
        double s[3], t[3];
        toWarpSpace(space, basis, pin.source, s);
        toWarpSpace(space, basis, pin.target, t);
        double d[3];
        bool finite = std::isfinite(pin.strength) && std::isfinite(pin.falloff);
        for (int c = 0; c < 3; ++c) {
            d[c] = pin.strength * (t[c] - s[c]);
            finite = finite && std::isfinite(s[c]) && std::isfinite(d[c]);
        }
        if (!finite) {
            continue;
        }
        sources.insert(sources.end(), s, s + 3);
        displacements.insert(displacements.end(), d, d + 3);
        falloffs.push_back(pin.falloff * falloffScale);
    }

    const int n = static_cast<int>(falloffs.size());
    std::vector<bool> used(n, false);
    std::vector<double> centres, merged, mergedFalloffs;
    bool anyMoving = false;
    for (int i = 0; i < n; ++i) {
        if (used[i]) {
            continue;
        }
        const double* si = &sources[i * 3];
        const double tol = 1e-20 * (1. + si[0] * si[0] + si[1] * si[1] + si[2] * si[2]);
        double movingSum[3] = { 0., 0., 0. };
        double movingFalloff = 0., holdFalloff = 0.;
        int moving = 0, movingFalloffs = 0, holdFalloffs = 0;
        for (int j = i; j < n; ++j) {
            if (used[j] || (dist2(si, &sources[j * 3]) > tol)) {
                continue;
            }
            used[j] = true;
            const double* dj = &displacements[j * 3];
            if (isNonZero(dj)) {
                for (int c = 0; c < 3; ++c) {
                    movingSum[c] += dj[c];
                }
                ++moving;
                if (falloffs[j] > 0.) {
                    movingFalloff += falloffs[j];
                    ++movingFalloffs;
                }
            } else if (falloffs[j] > 0.) {
                holdFalloff += falloffs[j];
                ++holdFalloffs;
            }
        }
        centres.insert(centres.end(), si, si + 3);
        if (moving > 0) {
            for (int c = 0; c < 3; ++c) {
                merged.push_back(movingSum[c] / moving);
            }
            mergedFalloffs.push_back((movingFalloffs > 0) ? movingFalloff / movingFalloffs : 0.);
            anyMoving = true;
        } else {
            merged.insert(merged.end(), 3, 0.);
            mergedFalloffs.push_back((holdFalloffs > 0) ? holdFalloff / holdFalloffs : 0.);
        }
    }
    if (!anyMoving) {
        return;
    }
    if (kernel == eColorWarpKernelGaussian) {
        prepareGaussian(centres, merged, mergedFalloffs);
    } else {
        prepareRbf(centres, merged, regularisation);
    }
}

void
ColorWarpField::prepareGaussian(const std::vector<double>& centres,
                                const std::vector<double>& displacements,
                                const std::vector<double>& falloffs)
{
    _pinCount = static_cast<int>(falloffs.size());
    _centres = centres;
    _coefficients = displacements;
    _invTwoSigma2.resize(_pinCount);
    for (int i = 0; i < _pinCount; ++i) {
        const double f = falloffs[i];
        _invTwoSigma2[i] = (f > 0.) ? 2. / (f * f) : 0.;
    }
}

void
ColorWarpField::prepareRbf(const std::vector<double>& centres,
                           const std::vector<double>& displacements,
                           double regularisation)
{
    const int n = static_cast<int>(centres.size() / 3);

    for (int i = 0; i < n; ++i) {
        for (int c = 0; c < 3; ++c) {
            _origin[c] += centres[i * 3 + c] / n;
        }
    }
    double scale2 = 0.;
    for (int i = 0; i < n; ++i) {
        scale2 = std::max(scale2, dist2(&centres[i * 3], _origin));
    }
    _invScale = (scale2 > 0.) ? 1. / std::sqrt(scale2) : 1.;
    _centres.resize(static_cast<std::size_t>(n) * 3);
    for (int i = 0; i < n; ++i) {
        for (int c = 0; c < 3; ++c) {
            _centres[i * 3 + c] = (centres[i * 3 + c] - _origin[c]) * _invScale;
        }
    }

    // The fade starts at the sphere around the centroid that holds every pin, and is wide
    // enough not to fold colours moved by up to the largest pin displacement.
    double maxDisplacement = 0.;
    for (int i = 0; i < n; ++i) {
        _fadeRadius = std::max(_fadeRadius, std::sqrt(dist2(&_centres[i * 3], kZero3)));
        maxDisplacement = std::max(maxDisplacement, std::sqrt(dist2(&displacements[i * 3], kZero3)));
    }
    const double fadeWidth = std::max(_fadeRadius, kColorWarpFadeDisplacementWidth * maxDisplacement * _invScale);
    _fadeInvWidth = (fadeWidth > 0.) ? 1. / fadeWidth : 0.;

    // The affine part only spans the directions the pins themselves span: along any other
    // direction its slope is unconstrained, and zero is the least surprising choice.
    double cov[3][3] = {};
    for (int i = 0; i < n; ++i) {
        const double* ci = &_centres[i * 3];
        for (int p = 0; p < 3; ++p) {
            for (int q = 0; q < 3; ++q) {
                cov[p][q] += ci[p] * ci[q] / n;
            }
        }
    }
    double axes[3][3];
    jacobiEigen3(cov, axes);
    // Relative, not absolute: a direction the pins barely span (grey pins that leave a line only
    // through the log toe's kink) would otherwise get a slope fitted to that sliver, which a
    // colour off it multiplies into an arbitrarily large displacement.
    const double evMax = std::max(cov[0][0], std::max(cov[1][1], cov[2][2]));
    std::vector<int> kept;
    for (int k = 0; k < 3; ++k) {
        if ((evMax > 0.) && (cov[k][k] > 1e-6 * evMax)) {
            kept.push_back(k);
        }
    }
    const int m = static_cast<int>(kept.size());
    const int size = n + 1 + m;
    const double mu = std::max(0., regularisation);
    std::vector<double> a(static_cast<std::size_t>(size) * size, 0.);
    std::vector<double> b(static_cast<std::size_t>(size) * 3, 0.);
    for (int i = 0; i < n; ++i) {
        const double* ci = &_centres[i * 3];
        for (int j = 0; j < n; ++j) {
            a[i * size + j] = rbfPhi(_kernel, std::sqrt(dist2(ci, &_centres[j * 3])));
        }
        a[i * size + i] += mu;
        a[i * size + n] = 1.;
        a[n * size + i] = 1.;
        for (int k = 0; k < m; ++k) {
            const int axis = kept[k];
            const double proj = ci[0] * axes[0][axis] + ci[1] * axes[1][axis] + ci[2] * axes[2][axis];
            a[i * size + n + 1 + k] = proj;
            a[(n + 1 + k) * size + i] = proj;
        }
        for (int c = 0; c < 3; ++c) {
            b[i * 3 + c] = displacements[i * 3 + c];
        }
    }
    std::vector<double> x;
    solveDense(a, b, size, x);

    _pinCount = n;
    _coefficients.assign(x.begin(), x.begin() + static_cast<std::ptrdiff_t>(n) * 3);
    for (int c = 0; c < 3; ++c) {
        _affine[c] = x[n * 3 + c];
    }
    for (int k = 0; k < m; ++k) {
        const int axis = kept[k];
        for (int c = 0; c < 3; ++c) {
            const double slope = x[(n + 1 + k) * 3 + c];
            for (int q = 0; q < 3; ++q) {
                _affine[3 + c * 3 + q] += slope * axes[q][axis];
            }
        }
    }
}

void
ColorWarpField::displacementAt(const double warp[3], double d[3]) const
{
    d[0] = d[1] = d[2] = 0.;
    if (_pinCount == 0) {
        return;
    }
    if (_kernel == eColorWarpKernelGaussian) {
        double sumW = 0.;
        double acc[3] = { 0., 0., 0. };
        for (int i = 0; i < _pinCount; ++i) {
            const double* ci = &_centres[i * 3];
            const double* di = &_coefficients[i * 3];
            const double r2 = lightnessDist2(warp, ci, _lightnessAxis, _lightnessStretch);
            const double t = r2 * _invTwoSigma2[i];
            if ((r2 == 0.) || ((_invTwoSigma2[i] > 0.) && (t < kGaussianExactHit))) {
                d[0] = di[0];
                d[1] = di[1];
                d[2] = di[2];
                return;
            }
            if ((_invTwoSigma2[i] <= 0.) || (t >= kGaussianCutoffExponent)) {
                continue;
            }
            const double w = std::exp(-t) / (2. * t);
            sumW += w;
            for (int c = 0; c < 3; ++c) {
                acc[c] += w * di[c];
            }
        }
        if (sumW > 0.) {
            const double norm = 1. / (1. + sumW);
            for (int c = 0; c < 3; ++c) {
                d[c] = acc[c] * norm;
            }
        }
        return;
    }

    double x[3];
    for (int c = 0; c < 3; ++c) {
        x[c] = (warp[c] - _origin[c]) * _invScale;
    }
    const double fade = colorWarpFade(std::sqrt(dist2(x, kZero3)), _fadeRadius, _fadeInvWidth);
    if (fade <= 0.) {
        return;
    }
    for (int c = 0; c < 3; ++c) {
        d[c] = _affine[c] + _affine[3 + c * 3] * x[0] + _affine[4 + c * 3] * x[1] + _affine[5 + c * 3] * x[2];
    }
    for (int i = 0; i < _pinCount; ++i) {
        const double phi = rbfPhi(_kernel, std::sqrt(dist2(x, &_centres[i * 3])));
        for (int c = 0; c < 3; ++c) {
            d[c] += _coefficients[i * 3 + c] * phi;
        }
    }
    if (fade < 1.) {
        for (int c = 0; c < 3; ++c) {
            d[c] *= fade;
        }
    }
}

void
ColorWarpField::apply(const double rgb[3], double out[3]) const
{
    out[0] = rgb[0];
    out[1] = rgb[1];
    out[2] = rgb[2];
    if ((_pinCount == 0) || !std::isfinite(rgb[0]) || !std::isfinite(rgb[1]) || !std::isfinite(rgb[2])) {
        return;
    }
    double w[3], d[3];
    toWarpSpace(_space, _basis, rgb, w);
    displacementAt(w, d);
    if (!isNonZero(d)) {
        return;
    }
    for (int c = 0; c < 3; ++c) {
        w[c] += d[c];
    }
    fromWarpSpace(_space, _basis, w, out);
}

NATRON_NAMESPACE_EXIT
