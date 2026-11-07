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

#include "VectorFollowModels.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <Eigen/Dense>

NATRON_NAMESPACE_ENTER

namespace {

constexpr int kMaxIterations = 60;
constexpr double kHuberK = 1.345;
constexpr double kConvergence = 1e-9;
// Below this the samples agree to numerical noise, and the Huber threshold must not reach zero.
constexpr double kMinScale = 1e-9;
constexpr double kGridSteps = 16.;
constexpr int kRejectionPasses = 4;
constexpr double kRejectionSigmas = 3.;
constexpr double kMinRejection = 1e-6;

struct Normalisation {
    double cx = 0.;
    double cy = 0.;
    double scale = 1.;

    FollowMatrix forward() const
    {
        FollowMatrix t;
        t.m[0] = scale;
        t.m[2] = -scale * cx;
        t.m[4] = scale;
        t.m[5] = -scale * cy;

        return t;
    }

    FollowMatrix backward() const
    {
        return FollowMatrix::translation(cx, cy) * diagonal(1. / scale);
    }

    static FollowMatrix diagonal(double s)
    {
        FollowMatrix t;
        t.m[0] = s;
        t.m[4] = s;

        return t;
    }
};

Normalisation
normalisationOf(const std::vector<FollowPoint>& points,
                const std::vector<double>& weights)
{
    Normalisation n;
    double total = 0.;
    for (std::size_t i = 0; i < points.size(); ++i) {
        n.cx += weights[i] * points[i].x;
        n.cy += weights[i] * points[i].y;
        total += weights[i];
    }
    if (total <= 0.) {
        return n;
    }
    n.cx /= total;
    n.cy /= total;
    double distance = 0.;
    for (std::size_t i = 0; i < points.size(); ++i) {
        distance += weights[i] * std::hypot(points[i].x - n.cx, points[i].y - n.cy);
    }
    distance /= total;
    n.scale = (distance > 0.) ? (std::sqrt(2.) / distance) : 1.;

    return n;
}

bool
solveTranslate(const std::vector<FollowPoint>& src,
               const std::vector<FollowPoint>& dst,
               const std::vector<double>& weights,
               FollowMatrix* out)
{
    double dx = 0., dy = 0., total = 0.;
    for (std::size_t i = 0; i < src.size(); ++i) {
        dx += weights[i] * (dst[i].x - src[i].x);
        dy += weights[i] * (dst[i].y - src[i].y);
        total += weights[i];
    }
    if (total <= 0.) {
        return false;
    }
    *out = FollowMatrix::translation(dx / total, dy / total);

    return true;
}

bool
solveAffine(const std::vector<FollowPoint>& src,
            const std::vector<FollowPoint>& dst,
            const std::vector<double>& weights,
            FollowMatrix* out)
{
    const Normalisation ns = normalisationOf(src, weights);
    const Normalisation nd = normalisationOf(dst, weights);
    const FollowMatrix toSrc = ns.forward();
    const FollowMatrix toDst = nd.forward();

    Eigen::Matrix3d normal = Eigen::Matrix3d::Zero();
    Eigen::Vector3d rhsX = Eigen::Vector3d::Zero();
    Eigen::Vector3d rhsY = Eigen::Vector3d::Zero();
    for (std::size_t i = 0; i < src.size(); ++i) {
        if (weights[i] <= 0.) {
            continue;
        }
        const FollowPoint s = toSrc.apply(src[i]);
        const FollowPoint d = toDst.apply(dst[i]);
        const Eigen::Vector3d row(s.x, s.y, 1.);
        normal.noalias() += weights[i] * row * row.transpose();
        rhsX.noalias() += weights[i] * d.x * row;
        rhsY.noalias() += weights[i] * d.y * row;
    }
    Eigen::ColPivHouseholderQR<Eigen::Matrix3d> qr(normal);
    qr.setThreshold(1e-8);
    if (qr.rank() < 3) {
        return false;
    }
    const Eigen::Vector3d x = qr.solve(rhsX);
    const Eigen::Vector3d y = qr.solve(rhsY);
    FollowMatrix a;
    a.m[0] = x(0);
    a.m[1] = x(1);
    a.m[2] = x(2);
    a.m[3] = y(0);
    a.m[4] = y(1);
    a.m[5] = y(2);
    *out = nd.backward() * a * toSrc;

    return true;
}

bool
solvePerspective(const std::vector<FollowPoint>& src,
                 const std::vector<FollowPoint>& dst,
                 const std::vector<double>& weights,
                 FollowMatrix* out)
{
    const Normalisation ns = normalisationOf(src, weights);
    const Normalisation nd = normalisationOf(dst, weights);
    const FollowMatrix toSrc = ns.forward();
    const FollowMatrix toDst = nd.forward();

    typedef Eigen::Matrix<double, 9, 9> Matrix9d;
    typedef Eigen::Matrix<double, 9, 1> Vector9d;
    Matrix9d normal = Matrix9d::Zero();
    for (std::size_t i = 0; i < src.size(); ++i) {
        if (weights[i] <= 0.) {
            continue;
        }
        const FollowPoint s = toSrc.apply(src[i]);
        const FollowPoint d = toDst.apply(dst[i]);
        Vector9d rowX, rowY;
        rowX << s.x, s.y, 1., 0., 0., 0., -d.x * s.x, -d.x * s.y, -d.x;
        rowY << 0., 0., 0., s.x, s.y, 1., -d.y * s.x, -d.y * s.y, -d.y;
        normal.noalias() += weights[i] * (rowX * rowX.transpose() + rowY * rowY.transpose());
    }
    Eigen::SelfAdjointEigenSolver<Matrix9d> eigen(normal);
    if (eigen.info() != Eigen::Success) {
        return false;
    }
    // A second near-zero eigenvalue means the samples admit more than one homography.
    const double largest = eigen.eigenvalues()(8);
    if ((largest <= 0.) || (eigen.eigenvalues()(1) < 1e-10 * largest)) {
        return false;
    }
    const Vector9d h = eigen.eigenvectors().col(0);
    FollowMatrix hn;
    for (int i = 0; i < 9; ++i) {
        hn.m[i] = h(i);
    }
    FollowMatrix full = nd.backward() * hn * toSrc;
    if (std::fabs(full.m[8]) < std::numeric_limits<double>::epsilon()) {
        return false;
    }
    const double norm = full.m[8];
    for (int i = 0; i < 9; ++i) {
        full.m[i] /= norm;
    }
    *out = full;

    return true;
}

bool
solveModel(VectorFollowModel model,
           const std::vector<FollowPoint>& src,
           const std::vector<FollowPoint>& dst,
           const std::vector<double>& weights,
           FollowMatrix* out)
{
    switch (model) {
    case VectorFollowModel::eTranslate:

        return solveTranslate(src, dst, weights, out);
    case VectorFollowModel::eAffine:
    case VectorFollowModel::ePerPoint:

        return solveAffine(src, dst, weights, out);
    case VectorFollowModel::ePerspective:

        return solvePerspective(src, dst, weights, out);
    }

    return false;
}

std::size_t
countUsable(const std::vector<double>& weights)
{
    return (std::size_t)std::count_if(weights.begin(), weights.end(), [](double w) { return w > 0.; });
}

std::size_t
minimumSamples(VectorFollowModel model)
{
    switch (model) {
    case VectorFollowModel::eTranslate:

        return 1;
    case VectorFollowModel::eAffine:
    case VectorFollowModel::ePerPoint:

        return 3;
    case VectorFollowModel::ePerspective:

        return 4;
    }

    return 1;
}

VectorFollowModel
simplerModel(VectorFollowModel model)
{
    return (model == VectorFollowModel::ePerspective) ? VectorFollowModel::eAffine : VectorFollowModel::eTranslate;
}

double
residual(const FollowMatrix& matrix,
         const FollowPoint& src,
         const FollowPoint& dst)
{
    const FollowPoint moved = matrix.apply(src);

    return std::hypot(moved.x - dst.x, moved.y - dst.y);
}

double
matrixDistance(const FollowMatrix& a,
               const FollowMatrix& b)
{
    double largest = 0.;
    for (int i = 0; i < 9; ++i) {
        largest = std::max(largest, std::fabs(a.m[i] - b.m[i]));
    }

    return largest;
}

/*
 * Huber never gives an outlier a weight of zero, so a far one still leans on the fit by a fraction
 * of a pixel. Once IRLS has told the two populations apart, dropping the far samples removes that.
 */
void
rejectFarSamples(VectorFollowModel model,
                 const std::vector<FollowPoint>& src,
                 const std::vector<FollowPoint>& dst,
                 const std::vector<double>& weights,
                 FollowMatrix* matrix)
{
    std::vector<double> kept(weights);
    std::vector<double> residuals;
    for (int pass = 0; pass < kRejectionPasses; ++pass) {
        residuals.clear();
        for (std::size_t i = 0; i < src.size(); ++i) {
            if (weights[i] > 0.) {
                residuals.push_back(residual(*matrix, src[i], dst[i]));
            }
        }
        std::nth_element(residuals.begin(), residuals.begin() + residuals.size() / 2, residuals.end());
        const double threshold = std::max(kRejectionSigmas * 1.4826 * residuals[residuals.size() / 2], kMinRejection);
        for (std::size_t i = 0; i < src.size(); ++i) {
            kept[i] = (residual(*matrix, src[i], dst[i]) <= threshold) ? weights[i] : 0.;
        }
        const std::size_t count = countUsable(kept);
        if ((count < minimumSamples(model)) || (count * 2 < residuals.size())) {
            return;
        }
        FollowMatrix next;
        if (!solveModel(model, src, dst, kept, &next)) {
            return;
        }
        *matrix = next;
    }
}

VectorFollowFit
fitWithFallback(VectorFollowModel model,
                const std::vector<FollowPoint>& src,
                const std::vector<FollowPoint>& dst,
                const std::vector<double>& weights,
                bool robust)
{
    VectorFollowFit fit;
    const std::size_t usable = countUsable(weights);
    if ((src.size() != dst.size()) || (src.size() != weights.size()) || (usable == 0)) {
        return fit;
    }

    VectorFollowModel current = (model == VectorFollowModel::ePerPoint) ? VectorFollowModel::eAffine : model;
    while (usable < minimumSamples(current)) {
        current = simplerModel(current);
    }

    FollowMatrix matrix;
    while (!solveModel(current, src, dst, weights, &matrix)) {
        if (current == VectorFollowModel::eTranslate) {
            return fit;
        }
        current = simplerModel(current);
    }

    if (robust) {
        // A homography is free enough to bend towards outliers, and IRLS then settles there. Weighing
        // the first pass by an affine fit starts it from residuals the outliers cannot hide in.
        if (current == VectorFollowModel::ePerspective) {
            const VectorFollowFit affine = fitWithFallback(VectorFollowModel::eAffine, src, dst, weights, true);
            if (affine.valid) {
                matrix = affine.matrix;
            }
        }
        std::vector<double> huber(weights);
        std::vector<double> residuals;
        residuals.reserve(usable);
        for (int iteration = 0; iteration < kMaxIterations; ++iteration) {
            residuals.clear();
            for (std::size_t i = 0; i < src.size(); ++i) {
                if (weights[i] > 0.) {
                    residuals.push_back(residual(matrix, src[i], dst[i]));
                }
            }
            std::nth_element(residuals.begin(), residuals.begin() + residuals.size() / 2, residuals.end());
            const double scale = std::max(1.4826 * residuals[residuals.size() / 2], kMinScale);
            const double threshold = kHuberK * scale;
            for (std::size_t i = 0; i < src.size(); ++i) {
                if (weights[i] <= 0.) {
                    continue;
                }
                const double r = residual(matrix, src[i], dst[i]);
                huber[i] = weights[i] * ((r <= threshold) ? 1. : (threshold / r));
            }
            FollowMatrix next;
            if (!solveModel(current, src, dst, huber, &next)) {
                break;
            }
            const double moved = matrixDistance(matrix, next);
            matrix = next;
            if (moved < kConvergence) {
                break;
            }
        }
        rejectFarSamples(current, src, dst, weights, &matrix);
    }

    fit.matrix = matrix;
    fit.valid = true;
    fit.model = current;

    return fit;
}

bool
insidePolygon(const std::vector<FollowPoint>& polygon,
              double x,
              double y)
{
    bool inside = false;
    for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const FollowPoint& a = polygon[i];
        const FollowPoint& b = polygon[j];
        if (((a.y > y) != (b.y > y)) && (x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x)) {
            inside = !inside;
        }
    }

    return inside;
}

} // anonymous namespace

VectorFollowFit
fitFollowMotion(VectorFollowModel model,
                const std::vector<FollowPoint>& src,
                const std::vector<FollowPoint>& dst,
                const std::vector<double>& weights)
{
    return fitWithFallback(model, src, dst, weights, true);
}

VectorFollowFit
solveFollowMotion(VectorFollowModel model,
                  const std::vector<FollowPoint>& src,
                  const std::vector<FollowPoint>& dst)
{
    return fitWithFallback(model, src, dst, std::vector<double>(src.size(), 1.), false);
}

std::vector<FollowPoint>
followGridSamplesInside(const std::vector<FollowPoint>& polygon)
{
    std::vector<FollowPoint> samples;
    if (polygon.size() < 3) {
        return samples;
    }
    double x1 = polygon[0].x, x2 = polygon[0].x, y1 = polygon[0].y, y2 = polygon[0].y;
    for (const FollowPoint& p : polygon) {
        x1 = std::min(x1, p.x);
        x2 = std::max(x2, p.x);
        y1 = std::min(y1, p.y);
        y2 = std::max(y2, p.y);
    }
    const double spacing = std::max(1., std::max(x2 - x1, y2 - y1) / kGridSteps);
    for (double y = y1 + spacing / 2.; y < y2; y += spacing) {
        for (double x = x1 + spacing / 2.; x < x2; x += spacing) {
            if (insidePolygon(polygon, x, y)) {
                samples.push_back(FollowPoint(x, y));
            }
        }
    }

    return samples;
}

NATRON_NAMESPACE_EXIT
