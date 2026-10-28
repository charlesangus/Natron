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

#include "SplineWarpTps.h"

#include <algorithm>
#include <cmath>

#include <Eigen/Dense>

NATRON_NAMESPACE_ENTER

namespace {
// Relative to the spread of the destinations, which the normalization makes 1.
const double kCollinearTolerance = 1e-10;
const double kDegenerateScale = 1e-9;

// The thin-plate kernel r^2 log r, from r^2.
inline double
kernel(double r2)
{
    return (r2 > 0.) ? 0.5 * r2 * std::log(r2) : 0.;
}

Point
makePoint(double x,
          double y)
{
    Point p;

    p.x = x;
    p.y = y;

    return p;
}
} // anonymous namespace

SplineWarpTps::SplineWarpTps()
    : _model(eModelIdentity)
    , _center(makePoint(0., 0.))
    , _invScale(1.)
    , _bendingEnergy(0.)
{
    _matrix[0][0] = 1.;
    _matrix[0][1] = 0.;
    _matrix[1][0] = 0.;
    _matrix[1][1] = 1.;
    _offset[0] = _offset[1] = 0.;
    for (int i = 0; i < 3; ++i) {
        _affineX[i] = _affineY[i] = 0.;
    }
}

void
SplineWarpTps::setAffine(const double m[2][2],
                         const double offset[2])
{
    _matrix[0][0] = m[0][0];
    _matrix[0][1] = m[0][1];
    _matrix[1][0] = m[1][0];
    _matrix[1][1] = m[1][1];
    _offset[0] = offset[0];
    _offset[1] = offset[1];
}

SplineWarpTps
SplineWarpTps::solve(const std::vector<SplineWarpSamplePair>& pairs,
                     double smoothness)
{
    SplineWarpTps tps;
    const int n = (int)pairs.size();

    if (n == 0) {
        return tps;
    }

    // Translation by the mean displacement: the one-pair model, and the fallback when every
    // destination coincides.
    double meanDx = 0., meanDy = 0., cx = 0., cy = 0.;
    for (int i = 0; i < n; ++i) {
        meanDx += pairs[i].source.x - pairs[i].destination.x;
        meanDy += pairs[i].source.y - pairs[i].destination.y;
        cx += pairs[i].destination.x;
        cy += pairs[i].destination.y;
    }
    meanDx /= n;
    meanDy /= n;
    cx /= n;
    cy /= n;
    const double identity[2][2] = { { 1., 0. }, { 0., 1. } };
    const double translation[2] = { meanDx, meanDy };
    tps.setAffine(identity, translation);
    tps._model = eModelTranslation;

    double spread2 = 0.;
    for (int i = 0; i < n; ++i) {
        const double dx = pairs[i].destination.x - cx;
        const double dy = pairs[i].destination.y - cy;
        spread2 += dx * dx + dy * dy;
    }
    const double scale = std::sqrt(spread2 / n);
    if ((n == 1) || !(scale > kDegenerateScale * std::max(1., std::max(std::fabs(cx), std::fabs(cy))))) {
        return tps;
    }

    if (n == 2) {
        // The similarity z -> a z + b, in complex numbers, taking both destinations to their sources.
        const double zx = pairs[1].destination.x - pairs[0].destination.x;
        const double zy = pairs[1].destination.y - pairs[0].destination.y;
        const double wx = pairs[1].source.x - pairs[0].source.x;
        const double wy = pairs[1].source.y - pairs[0].source.y;
        const double z2 = zx * zx + zy * zy;
        const double ar = (wx * zx + wy * zy) / z2;
        const double ai = (wy * zx - wx * zy) / z2;
        const double m[2][2] = { { ar, -ai }, { ai, ar } };
        const double offset[2] = {
            pairs[0].source.x - (ar * pairs[0].destination.x - ai * pairs[0].destination.y),
            pairs[0].source.y - (ai * pairs[0].destination.x + ar * pairs[0].destination.y)
        };
        tps.setAffine(m, offset);
        tps._model = eModelSimilarity;

        return tps;
    }

    const double invScale = 1. / scale;
    Eigen::VectorXd px(n), py(n);
    for (int i = 0; i < n; ++i) {
        px(i) = (pairs[i].destination.x - cx) * invScale;
        py(i) = (pairs[i].destination.y - cy) * invScale;
    }

    // The least-squares affine fit of minimum norm on the displacement, about the centroid: its
    // constant term is the mean displacement, and the pseudo-inverse leaves a direction the
    // destinations do not span undisplaced.
    Eigen::Matrix2d cov = Eigen::Matrix2d::Zero();
    Eigen::Matrix2d cross = Eigen::Matrix2d::Zero();
    for (int i = 0; i < n; ++i) {
        const Eigen::Vector2d p(px(i), py(i));
        const Eigen::Vector2d d(pairs[i].source.x - pairs[i].destination.x - meanDx,
                                pairs[i].source.y - pairs[i].destination.y - meanDy);
        cov += p * p.transpose();
        cross += d * p.transpose();
    }
    cov /= n;
    cross /= n;
    Eigen::SelfAdjointEigenSolver<Eigen::Matrix2d> eigen(cov);
    const Eigen::Vector2d values = eigen.eigenvalues();
    const bool collinear = !(values(0) > kCollinearTolerance * std::max(values(1), 1e-300));

    auto fitAffine = [&]() {
        Eigen::Matrix2d pinv = Eigen::Matrix2d::Zero();
        for (int k = 0; k < 2; ++k) {
            if (values(k) > kCollinearTolerance * values(1)) {
                const Eigen::Vector2d v = eigen.eigenvectors().col(k);
                pinv += (v * v.transpose()) / values(k);
            }
        }
        // displacement(p) = mean + M (p - c) / scale.
        const Eigen::Matrix2d m = cross * pinv * invScale;
        const double matrix[2][2] = { { 1. + m(0, 0), m(0, 1) }, { m(1, 0), 1. + m(1, 1) } };
        const double offset[2] = { meanDx - (m(0, 0) * cx + m(0, 1) * cy), meanDy - (m(1, 0) * cx + m(1, 1) * cy) };
        tps.setAffine(matrix, offset);
        tps._model = eModelAffine;
    };

    if (collinear) {
        fitAffine();

        return tps;
    }

    // [K + lambda I, P; P^T, 0] [W; A] = [S; 0], with P = [1 x y] and S the sources.
    const int size = n + 3;
    Eigen::MatrixXd system = Eigen::MatrixXd::Zero(size, size);
    const double lambda = std::max(0., smoothness);
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            const double dx = px(i) - px(j);
            const double dy = py(i) - py(j);
            const double k = kernel(dx * dx + dy * dy);
            system(i, j) = k;
            system(j, i) = k;
        }
        system(i, i) = lambda;
        system(i, n) = 1.;
        system(i, n + 1) = px(i);
        system(i, n + 2) = py(i);
        system(n, i) = 1.;
        system(n + 1, i) = px(i);
        system(n + 2, i) = py(i);
    }
    Eigen::MatrixXd rhs = Eigen::MatrixXd::Zero(size, 2);
    for (int i = 0; i < n; ++i) {
        rhs(i, 0) = pairs[i].source.x;
        rhs(i, 1) = pairs[i].source.y;
    }

    const Eigen::PartialPivLU<Eigen::MatrixXd> lu(system);
    const Eigen::MatrixXd solution = lu.solve(rhs);
    const double residual = (system * solution - rhs).norm();
    if (!solution.allFinite() || !(residual <= 1e-6 * std::max(1., rhs.norm()))) {
        fitAffine();

        return tps;
    }

    tps._model = eModelThinPlate;
    tps._center = makePoint(cx, cy);
    tps._invScale = invScale;
    tps._centersX.assign(px.data(), px.data() + n);
    tps._centersY.assign(py.data(), py.data() + n);
    tps._weightsX.resize(n);
    tps._weightsY.resize(n);
    for (int i = 0; i < n; ++i) {
        tps._weightsX[i] = solution(i, 0);
        tps._weightsY[i] = solution(i, 1);
    }
    for (int k = 0; k < 3; ++k) {
        tps._affineX[k] = solution(n + k, 0);
        tps._affineY[k] = solution(n + k, 1);
    }
    const Eigen::MatrixXd w = solution.topRows(n);
    Eigen::MatrixXd kernelMatrix = system.topLeftCorner(n, n);
    kernelMatrix.diagonal().setZero();
    tps._bendingEnergy = (w.transpose() * kernelMatrix * w).trace();

    return tps;
} // SplineWarpTps::solve

Point
SplineWarpTps::evaluate(double x,
                        double y) const
{
    if (_model == eModelIdentity) {
        return makePoint(x, y);
    }
    if (_model != eModelThinPlate) {
        return makePoint(_matrix[0][0] * x + _matrix[0][1] * y + _offset[0],
                         _matrix[1][0] * x + _matrix[1][1] * y + _offset[1]);
    }
    const double nx = (x - _center.x) * _invScale;
    const double ny = (y - _center.y) * _invScale;
    double sx = _affineX[0] + _affineX[1] * nx + _affineX[2] * ny;
    double sy = _affineY[0] + _affineY[1] * nx + _affineY[2] * ny;
    const std::size_t n = _centersX.size();
    for (std::size_t i = 0; i < n; ++i) {
        const double dx = nx - _centersX[i];
        const double dy = ny - _centersY[i];
        const double k = kernel(dx * dx + dy * dy);
        sx += _weightsX[i] * k;
        sy += _weightsY[i] * k;
    }

    return makePoint(sx, sy);
}

SplineWarpTpsConstPtr
SplineWarpTpsCache::get(U64 sampleHash,
                        double smoothness,
                        const std::vector<SplineWarpSamplePair>& pairs)
{
    Key key;

    key.hash = sampleHash;
    key.smoothness = smoothness;

    return _cache.get(key, [&pairs, smoothness]() {
        return SplineWarpTps::solve(pairs, smoothness);
    });
}

NATRON_NAMESPACE_EXIT
