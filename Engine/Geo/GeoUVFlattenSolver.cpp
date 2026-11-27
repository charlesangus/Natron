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

#include "GeoUVFlattenSolver.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <utility>

#include <Eigen/Dense>
#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>
#include <Eigen/SparseLU>

#include "Engine/Geo/GeoPage.h"

NATRON_NAMESPACE_ENTER

namespace {
const double kPi = 3.14159265358979323846;

typedef std::array<double, 3> Vec3;
typedef Eigen::SparseMatrix<double> SpMat;
typedef Eigen::Triplet<double> Triplet;

Vec3
sub(const Vec3& a,
    const Vec3& b)
{
    return { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
}

double
dot(const Vec3& a,
    const Vec3& b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Vec3
cross(const Vec3& a,
      const Vec3& b)
{
    return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
}

double
length(const Vec3& a)
{
    return std::sqrt(dot(a, a));
}

double
cornerAngle(const Vec3& at,
            const Vec3& to0,
            const Vec3& to1)
{
    const Vec3 a = sub(to0, at);
    const Vec3 b = sub(to1, at);

    return std::atan2(length(cross(a, b)), dot(a, b));
}

double
uvCornerAngle(const GeoUV& at,
              const GeoUV& to0,
              const GeoUV& to1)
{
    const double ax = to0[0] - at[0], ay = to0[1] - at[1];
    const double bx = to1[0] - at[0], by = to1[1] - at[1];

    return std::atan2(std::fabs(ax * by - ay * bx), ax * bx + ay * by);
}

bool
triangleDegenerate(const Vec3& a,
                   const Vec3& b,
                   const Vec3& c)
{
    const double area = 0.5 * length(cross(sub(b, a), sub(c, a)));
    const double perimeter = length(sub(b, a)) + length(sub(c, b)) + length(sub(a, c));

    return !(area > 1e-10 * perimeter * perimeter);
}

struct UnionFind {
    std::vector<int> parent;

    explicit UnionFind(std::size_t n)
        : parent(n)
    {
        std::iota(parent.begin(), parent.end(), 0);
    }

    int find(int i)
    {
        while (parent[i] != i) {
            parent[i] = parent[parent[i]];
            i = parent[i];
        }

        return i;
    }

    void unite(int a,
               int b)
    {
        a = find(a);
        b = find(b);
        if (a != b) {
            parent[std::max(a, b)] = std::min(a, b);
        }
    }
};

// A ring's triangles as corner indices into it: quads split along their shorter diagonal so a
// bent quad folds where it bends least, other polygons as a fan.
std::vector<std::array<int, 3>>
triangulateRing(const std::vector<int>& ring,
                const std::vector<Vec3>& positions)
{
    std::vector<std::array<int, 3>> tris;
    const int n = (int)ring.size();
    if (n < 3) {
        return tris;
    }
    if (n == 4) {
        const double d02 = length(sub(positions[ring[0]], positions[ring[2]]));
        const double d13 = length(sub(positions[ring[1]], positions[ring[3]]));
        if (d02 <= d13) {
            tris.push_back({ 0, 1, 2 });
            tris.push_back({ 0, 2, 3 });
        } else {
            tris.push_back({ 1, 2, 3 });
            tris.push_back({ 1, 3, 0 });
        }

        return tris;
    }
    for (int i = 1; i + 1 < n; ++i) {
        tris.push_back({ 0, i, i + 1 });
    }

    return tris;
}

std::vector<int>
cleanRing(const std::vector<int>& face)
{
    std::vector<int> ring;
    for (int v : face) {
        if (ring.empty() || (ring.back() != v)) {
            ring.push_back(v);
        }
    }
    while ((ring.size() > 1) && (ring.front() == ring.back())) {
        ring.pop_back();
    }

    return ring;
}

// The chart's non-degenerate triangles over the vertices they use, renumbered densely.
struct ChartSystem {
    int n = 0;
    std::vector<int> activeOf;
    std::vector<int> chartOf;
    std::vector<std::array<int, 3>> tris;
    std::vector<Vec3> pos;
    std::vector<char> boundary;
    int degenerateFaces = 0;
    double area = 0.;
};

void
buildSystem(const GeoUVFlattenChart& chart,
            ChartSystem* sys)
{
    std::vector<std::array<int, 3>> chartTris;
    for (const std::vector<int>& face : chart.faces) {
        const std::vector<int> ring = cleanRing(face);
        bool any = false;
        for (const std::array<int, 3>& t : triangulateRing(ring, chart.positions)) {
            const int a = ring[t[0]], b = ring[t[1]], c = ring[t[2]];
            if ((a == b) || (b == c) || (a == c) || triangleDegenerate(chart.positions[a], chart.positions[b], chart.positions[c])) {
                continue;
            }
            chartTris.push_back({ a, b, c });
            any = true;
        }
        if (!any) {
            ++sys->degenerateFaces;
        }
    }
    sys->activeOf.assign(chart.positions.size(), -1);
    for (const std::array<int, 3>& t : chartTris) {
        std::array<int, 3> local;
        for (int k = 0; k < 3; ++k) {
            int& active = sys->activeOf[t[k]];
            if (active < 0) {
                active = sys->n++;
                sys->chartOf.push_back(t[k]);
                sys->pos.push_back(chart.positions[t[k]]);
            }
            local[k] = active;
        }
        sys->tris.push_back(local);
        sys->area += 0.5 * length(cross(sub(sys->pos[local[1]], sys->pos[local[0]]), sub(sys->pos[local[2]], sys->pos[local[0]])));
    }
    std::map<std::pair<int, int>, int> directed;
    for (const std::array<int, 3>& t : sys->tris) {
        for (int k = 0; k < 3; ++k) {
            ++directed[std::make_pair(t[k], t[(k + 1) % 3])];
        }
    }
    sys->boundary.assign(sys->n, 0);
    for (const auto& e : directed) {
        if (directed.find(std::make_pair(e.first.second, e.first.first)) == directed.end()) {
            sys->boundary[e.first.first] = 1;
            sys->boundary[e.first.second] = 1;
        }
    }
}

// A planar projection onto the best-fitting plane, turned so the triangles keep their winding.
std::vector<double>
projectToPlane(const ChartSystem& sys)
{
    std::vector<double> x(2 * sys.n, 0.);
    if (sys.n == 0) {
        return x;
    }
    Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
    for (const Vec3& p : sys.pos) {
        centroid += Eigen::Vector3d(p[0], p[1], p[2]);
    }
    centroid /= (double)sys.n;
    Eigen::Matrix3d cov = Eigen::Matrix3d::Zero();
    for (const Vec3& p : sys.pos) {
        const Eigen::Vector3d d = Eigen::Vector3d(p[0], p[1], p[2]) - centroid;
        cov += d * d.transpose();
    }
    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> eig(cov);
    Eigen::Vector3d e1 = eig.eigenvectors().col(2);
    Eigen::Vector3d e2 = eig.eigenvectors().col(1);
    Eigen::Vector3d normal = Eigen::Vector3d::Zero();
    for (const std::array<int, 3>& t : sys.tris) {
        const Vec3 c = cross(sub(sys.pos[t[1]], sys.pos[t[0]]), sub(sys.pos[t[2]], sys.pos[t[0]]));
        normal += Eigen::Vector3d(c[0], c[1], c[2]);
    }
    if (e1.cross(e2).dot(normal) < 0.) {
        e2 = -e2;
    }
    for (int i = 0; i < sys.n; ++i) {
        const Eigen::Vector3d d = Eigen::Vector3d(sys.pos[i][0], sys.pos[i][1], sys.pos[i][2]) - centroid;
        x[2 * i] = d.dot(e1);
        x[2 * i + 1] = d.dot(e2);
    }

    return x;
}

double
uvArea(const ChartSystem& sys,
       const std::vector<double>& x)
{
    double area = 0.;
    for (const std::array<int, 3>& t : sys.tris) {
        const double ax = x[2 * t[1]] - x[2 * t[0]], ay = x[2 * t[1] + 1] - x[2 * t[0] + 1];
        const double bx = x[2 * t[2]] - x[2 * t[0]], by = x[2 * t[2] + 1] - x[2 * t[0] + 1];
        area += 0.5 * (ax * by - ay * bx);
    }

    return area;
}

// The spectral conformal energy E_D - A as a quadratic form over (u0, v0, u1, v1, ...).
SpMat
conformalEnergy(const ChartSystem& sys)
{
    std::vector<Triplet> trips;
    for (const std::array<int, 3>& t : sys.tris) {
        for (int k = 0; k < 3; ++k) {
            const int c = t[k], i = t[(k + 1) % 3], j = t[(k + 2) % 3];
            const Vec3 a = sub(sys.pos[i], sys.pos[c]);
            const Vec3 b = sub(sys.pos[j], sys.pos[c]);
            const double w = 0.25 * dot(a, b) / std::max(length(cross(a, b)), 1e-300);
            for (int d = 0; d < 2; ++d) {
                trips.emplace_back(2 * i + d, 2 * i + d, w);
                trips.emplace_back(2 * j + d, 2 * j + d, w);
                trips.emplace_back(2 * i + d, 2 * j + d, -w);
                trips.emplace_back(2 * j + d, 2 * i + d, -w);
            }
            const int from = t[k], to = t[(k + 1) % 3];
            trips.emplace_back(2 * from, 2 * to + 1, -0.25);
            trips.emplace_back(2 * to + 1, 2 * from, -0.25);
            trips.emplace_back(2 * to, 2 * from + 1, 0.25);
            trips.emplace_back(2 * from + 1, 2 * to, 0.25);
        }
    }
    SpMat q(2 * sys.n, 2 * sys.n);
    q.setFromTriplets(trips.begin(), trips.end());

    return q;
}

// The least-squares conformal energy of a layout keeping the solved triangle angles.
SpMat
angleEnergy(const ChartSystem& sys,
            const std::vector<double>& alpha)
{
    std::vector<Triplet> trips;
    int row = 0;
    for (std::size_t f = 0; f < sys.tris.size(); ++f) {
        int v[3] = { sys.tris[f][0], sys.tris[f][1], sys.tris[f][2] };
        double a[3] = { alpha[3 * f], alpha[3 * f + 1], alpha[3 * f + 2] };
        double s[3] = { std::sin(a[0]), std::sin(a[1]), std::sin(a[2]) };
        // The third corner takes the largest sine, so the ratio below stays bounded.
        int largest = 0;
        for (int k = 1; k < 3; ++k) {
            if (s[k] > s[largest]) {
                largest = k;
            }
        }
        const int shift = (largest + 1) % 3;
        int vv[3];
        double aa[3], ss[3];
        for (int k = 0; k < 3; ++k) {
            vv[k] = v[(k + shift) % 3];
            aa[k] = a[(k + shift) % 3];
            ss[k] = s[(k + shift) % 3];
        }
        const double ratio = (ss[2] == 0.) ? 1. : ss[1] / ss[2];
        const double cosine = std::cos(aa[0]) * ratio;
        const double sine = ss[0] * ratio;
        trips.emplace_back(row, 2 * vv[0], cosine - 1.);
        trips.emplace_back(row, 2 * vv[0] + 1, -sine);
        trips.emplace_back(row, 2 * vv[1], -cosine);
        trips.emplace_back(row, 2 * vv[1] + 1, sine);
        trips.emplace_back(row, 2 * vv[2], 1.);
        ++row;
        trips.emplace_back(row, 2 * vv[0], sine);
        trips.emplace_back(row, 2 * vv[0] + 1, cosine - 1.);
        trips.emplace_back(row, 2 * vv[1], -sine);
        trips.emplace_back(row, 2 * vv[1] + 1, -cosine);
        trips.emplace_back(row, 2 * vv[2] + 1, 1.);
        ++row;
    }
    SpMat a(row, 2 * sys.n);
    a.setFromTriplets(trips.begin(), trips.end());
    SpMat q = SpMat(a.transpose()) * a;

    return q;
}

// Variables merged by alignment and rectify constraints share one unknown; pinned ones are
// known values.
struct Reduction {
    std::vector<int> unknownOf;
    std::vector<double> value;
    std::vector<char> unknownIsU;
    int count = 0;
};

Reduction
makeReduction(UnionFind& merged,
              const std::vector<std::pair<int, double>>& fixes)
{
    const int nvars = (int)merged.parent.size();
    Reduction r;
    std::vector<int> rootUnknown(nvars, -2);
    std::vector<double> rootValue(nvars, 0.);
    for (const std::pair<int, double>& fix : fixes) {
        const int root = merged.find(fix.first);
        if (rootUnknown[root] == -2) {
            rootUnknown[root] = -1;
            rootValue[root] = fix.second;
        }
    }
    r.unknownOf.assign(nvars, -1);
    r.value.assign(nvars, 0.);
    for (int i = 0; i < nvars; ++i) {
        const int root = merged.find(i);
        if (rootUnknown[root] == -2) {
            rootUnknown[root] = r.count++;
            r.unknownIsU.push_back((i % 2) == 0);
        }
        r.unknownOf[i] = rootUnknown[root];
        r.value[i] = rootValue[root];
    }

    return r;
}

void
reduceEnergy(const SpMat& q,
             const Reduction& r,
             SpMat* reduced,
             Eigen::VectorXd* rhs)
{
    std::vector<Triplet> trips;
    *rhs = Eigen::VectorXd::Zero(r.count);
    for (int k = 0; k < q.outerSize(); ++k) {
        for (SpMat::InnerIterator it(q, k); it; ++it) {
            const int a = r.unknownOf[it.row()];
            if (a < 0) {
                continue;
            }
            const int b = r.unknownOf[it.col()];
            if (b >= 0) {
                trips.emplace_back(a, b, it.value());
            } else {
                (*rhs)[a] -= it.value() * r.value[it.col()];
            }
        }
    }
    reduced->resize(r.count, r.count);
    reduced->setFromTriplets(trips.begin(), trips.end());
}

std::vector<double>
expand(const Reduction& r,
       const Eigen::VectorXd& y)
{
    std::vector<double> x(r.unknownOf.size());
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = (r.unknownOf[i] >= 0) ? y[r.unknownOf[i]] : r.value[i];
    }

    return x;
}

bool
allFinite(const Eigen::VectorXd& y)
{
    for (Eigen::Index i = 0; i < y.size(); ++i) {
        if (!std::isfinite(y[i])) {
            return false;
        }
    }

    return true;
}

// A linear equation sum(coefficient * variable) = value over the full variables.
struct LinearRow {
    std::vector<std::pair<int, double>> terms;
    double value = 0.;
};

// Minimizes the energy over the unknowns, the rows holding exactly through Lagrange multipliers.
bool
solveConstrained(const SpMat& q,
                 const Reduction& r,
                 const std::vector<LinearRow>& rows,
                 const std::function<bool()>& stopped,
                 std::vector<double>* x)
{
    if (stopped()) {
        return false;
    }
    if (r.count == 0) {
        *x = expand(r, Eigen::VectorXd());

        return true;
    }
    SpMat reduced;
    Eigen::VectorXd rhs;
    reduceEnergy(q, r, &reduced, &rhs);
    std::vector<std::vector<std::pair<int, double>>> reducedRows;
    std::vector<double> rowValues;
    for (const LinearRow& row : rows) {
        std::map<int, double> terms;
        double value = row.value;
        for (const std::pair<int, double>& term : row.terms) {
            const int u = r.unknownOf[term.first];
            if (u >= 0) {
                terms[u] += term.second;
            } else {
                value -= term.second * r.value[term.first];
            }
        }
        std::vector<std::pair<int, double>> kept;
        for (const auto& term : terms) {
            if (std::fabs(term.second) > 1e-12) {
                kept.push_back(term);
            }
        }
        if (!kept.empty()) {
            reducedRows.push_back(kept);
            rowValues.push_back(value);
        }
    }
    Eigen::VectorXd y;
    if (reducedRows.empty()) {
        Eigen::SimplicialLDLT<SpMat> ldlt;
        ldlt.compute(reduced);
        if (ldlt.info() != Eigen::Success) {
            return false;
        }
        y = ldlt.solve(rhs);
    } else {
        const int n = r.count;
        const int m = (int)reducedRows.size();
        std::vector<Triplet> trips;
        for (int k = 0; k < reduced.outerSize(); ++k) {
            for (SpMat::InnerIterator it(reduced, k); it; ++it) {
                trips.emplace_back(it.row(), it.col(), it.value());
            }
        }
        Eigen::VectorXd kktRhs = Eigen::VectorXd::Zero(n + m);
        kktRhs.head(n) = rhs;
        for (int i = 0; i < m; ++i) {
            for (const std::pair<int, double>& term : reducedRows[i]) {
                trips.emplace_back(n + i, term.first, term.second);
                trips.emplace_back(term.first, n + i, term.second);
            }
            kktRhs[n + i] = rowValues[i];
        }
        SpMat kkt(n + m, n + m);
        kkt.setFromTriplets(trips.begin(), trips.end());
        kkt.makeCompressed();
        Eigen::SparseLU<SpMat, Eigen::COLAMDOrdering<int>> lu;
        lu.analyzePattern(kkt);
        lu.factorize(kkt);
        if (lu.info() != Eigen::Success) {
            return false;
        }
        const Eigen::VectorXd solution = lu.solve(kktRhs);
        if (lu.info() != Eigen::Success) {
            return false;
        }
        y = solution.head(n);
    }
    if (!allFinite(y)) {
        return false;
    }
    *x = expand(r, y);

    return true;
}

// The smallest non-trivial generalized eigenvector of E_C x = lambda B x, B weighting the
// boundary, by inverse iteration with the translations projected out.
bool
solveSpectral(const ChartSystem& sys,
              const SpMat& q,
              const Reduction& r,
              const std::function<bool()>& stopped,
              std::vector<double>* x)
{
    if (stopped()) {
        return false;
    }
    SpMat reduced;
    Eigen::VectorXd unused;
    reduceEnergy(q, r, &reduced, &unused);
    const int n = r.count;
    bool anyBoundary = false;
    for (char b : sys.boundary) {
        anyBoundary = anyBoundary || b;
    }
    Eigen::VectorXd mass = Eigen::VectorXd::Zero(n);
    for (int i = 0; i < 2 * sys.n; ++i) {
        if (!anyBoundary || sys.boundary[i / 2]) {
            mass[r.unknownOf[i]] += 1.;
        }
    }
    double diagonal = 0.;
    for (int i = 0; i < n; ++i) {
        diagonal += std::fabs(reduced.coeff(i, i));
    }
    const double epsilon = 1e-9 * std::max(diagonal / std::max(n, 1), 1e-300);
    SpMat shifted = reduced;
    for (int i = 0; i < n; ++i) {
        shifted.coeffRef(i, i) += epsilon;
    }
    Eigen::SimplicialLDLT<SpMat> ldlt;
    ldlt.compute(shifted);
    if ((ldlt.info() != Eigen::Success) || stopped()) {
        return false;
    }
    const std::vector<double> projected = projectToPlane(sys);
    Eigen::VectorXd y = Eigen::VectorXd::Zero(n);
    Eigen::VectorXd members = Eigen::VectorXd::Zero(n);
    for (int i = 0; i < 2 * sys.n; ++i) {
        y[r.unknownOf[i]] += projected[i];
        members[r.unknownOf[i]] += 1.;
    }
    for (int i = 0; i < n; ++i) {
        y[i] /= std::max(members[i], 1.);
    }
    const auto normalize = [&](Eigen::VectorXd* v) {
        for (int axis = 0; axis < 2; ++axis) {
            double sum = 0., weight = 0.;
            for (int i = 0; i < n; ++i) {
                if (r.unknownIsU[i] == (axis == 0)) {
                    sum += mass[i] * (*v)[i];
                    weight += mass[i];
                }
            }
            if (weight > 0.) {
                for (int i = 0; i < n; ++i) {
                    if (r.unknownIsU[i] == (axis == 0)) {
                        (*v)[i] -= sum / weight;
                    }
                }
            }
        }
        const double norm = std::sqrt(v->dot(mass.cwiseProduct(*v)));
        if (!(norm > 0.) || !std::isfinite(norm)) {
            return false;
        }
        *v /= norm;

        return true;
    };
    if (!normalize(&y)) {
        return false;
    }
    for (int iteration = 0; iteration < 200; ++iteration) {
        if (stopped()) {
            return false;
        }
        Eigen::VectorXd next = ldlt.solve(mass.cwiseProduct(y));
        if (!allFinite(next) || !normalize(&next)) {
            return false;
        }
        const double change = (next - y).norm();
        y = next;
        if (change < 1e-12 * std::sqrt((double)n)) {
            break;
        }
    }
    *x = expand(r, y);

    return true;
}

// ABF++, after the parametrizer of Blender (GPL-2.0-or-later): Newton steps on the angles of
// every triangle corner, with the interior vertices' planarity and wheel consistency
// eliminated through a Schur complement over two unknowns per interior vertex.
struct AbfSystem {
    int ninterior = 0;
    std::vector<int> interiorOf;
    std::vector<std::vector<int>> wedges;
    std::vector<double> alpha, beta, sine, cosine, weight, bAlpha, bTriangle, bInterior;
    std::vector<double> lambdaTriangle, lambdaPlanar, lambdaLength, bstar, dstar;
    std::vector<std::array<double, 3>> j2dt;
};

void
abfComputeSines(AbfSystem* s)
{
    for (std::size_t i = 0; i < s->alpha.size(); ++i) {
        s->sine[i] = std::sin(s->alpha[i]);
        s->cosine[i] = std::cos(s->alpha[i]);
    }
}

double
abfSinProduct(const AbfSystem& s,
              int vertex,
              int aid)
{
    double sin1 = 1., sin2 = 1.;
    for (int corner : s.wedges[vertex]) {
        const int f = corner / 3, k = corner % 3;
        const int e1 = 3 * f + (k + 1) % 3;
        const int e2 = 3 * f + (k + 2) % 3;
        if (aid == e1) {
            sin1 *= s.cosine[e1];
            sin2 = 0.;
        } else {
            sin1 *= s.sine[e1];
        }
        if (aid == e2) {
            sin1 = 0.;
            sin2 *= s.cosine[e2];
        } else {
            sin2 *= s.sine[e2];
        }
    }

    return sin1 - sin2;
}

double
abfGradAlpha(const AbfSystem& s,
             const ChartSystem& sys,
             int f,
             int k)
{
    const int id = 3 * f + k;
    const int v = sys.tris[f][k], v1 = sys.tris[f][(k + 1) % 3], v2 = sys.tris[f][(k + 2) % 3];
    double deriv = (s.alpha[id] - s.beta[id]) * s.weight[id];
    deriv += s.lambdaTriangle[f];
    if (s.interiorOf[v] >= 0) {
        deriv += s.lambdaPlanar[s.interiorOf[v]];
    }
    if (s.interiorOf[v1] >= 0) {
        deriv += s.lambdaLength[s.interiorOf[v1]] * abfSinProduct(s, v1, id);
    }
    if (s.interiorOf[v2] >= 0) {
        deriv += s.lambdaLength[s.interiorOf[v2]] * abfSinProduct(s, v2, id);
    }

    return deriv;
}

double
abfComputeGradient(AbfSystem* s,
                   const ChartSystem& sys)
{
    double norm = 0.;
    for (int f = 0; f < (int)sys.tris.size(); ++f) {
        double gtriangle = -kPi;
        for (int k = 0; k < 3; ++k) {
            const double g = abfGradAlpha(*s, sys, f, k);
            s->bAlpha[3 * f + k] = -g;
            norm += g * g;
            gtriangle += s->alpha[3 * f + k];
        }
        s->bTriangle[f] = -gtriangle;
        norm += gtriangle * gtriangle;
    }
    for (int v = 0; v < sys.n; ++v) {
        const int iv = s->interiorOf[v];
        if (iv < 0) {
            continue;
        }
        double gplanar = -2. * kPi;
        for (int corner : s->wedges[v]) {
            gplanar += s->alpha[corner];
        }
        s->bInterior[iv] = -gplanar;
        norm += gplanar * gplanar;
        const double glength = abfSinProduct(*s, v, -1);
        s->bInterior[s->ninterior + iv] = -glength;
        norm += glength * glength;
    }

    return norm;
}

void
abfAdjustAlpha(AbfSystem* s,
               int id,
               double dlambda1,
               double pre)
{
    const double dalpha = s->bAlpha[id] - dlambda1;
    const double alpha = s->alpha[id] + dalpha / s->weight[id] - pre;
    s->alpha[id] = std::min(std::max(alpha, 0.), kPi);
}

bool
abfMatrixInvert(AbfSystem* s,
                const ChartSystem& sys)
{
    const int ninterior = s->ninterior;
    const int nvar = 2 * ninterior;
    std::vector<Triplet> trips;
    Eigen::VectorXd rhs(nvar);
    for (int i = 0; i < nvar; ++i) {
        rhs[i] = s->bInterior[i];
    }
    for (int f = 0; f < (int)sys.tris.size(); ++f) {
        const int e[3] = { 3 * f, 3 * f + 1, 3 * f + 2 };
        const int v[3] = { sys.tris[f][0], sys.tris[f][1], sys.tris[f][2] };
        const double wi[3] = { 1. / s->weight[e[0]], 1. / s->weight[e[1]], 1. / s->weight[e[2]] };
        double b = s->bAlpha[e[0]] * wi[0] + s->bAlpha[e[1]] * wi[1] + s->bAlpha[e[2]] * wi[2];
        b -= s->bTriangle[f];
        const double si = 1. / (wi[0] + wi[1] + wi[2]);
        double beta[3];
        for (int k = 0; k < 3; ++k) {
            beta[k] = b * si - s->bAlpha[e[k]];
        }
        s->bstar[f] = b;
        s->dstar[f] = si;
        double w[3][3];
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                w[i][j] = (i == j) ? si - s->weight[e[i]] : si;
            }
        }
        int vid[6] = { -1, -1, -1, -1, -1, -1 };
        double j2[3][3] = { { 0., 0., 0. }, { 0., 0., 0. }, { 0., 0., 0. } };
        double row[3][6] = { { 0., 0., 0., 0., 0., 0. }, { 0., 0., 0., 0., 0., 0. }, { 0., 0., 0., 0., 0., 0. } };
        for (int c = 0; c < 3; ++c) {
            const int iv = s->interiorOf[v[c]];
            if (iv < 0) {
                continue;
            }
            vid[c] = iv;
            vid[3 + c] = ninterior + iv;
            const int o1 = (c + 1) % 3, o2 = (c + 2) % 3;
            j2[c][c] = wi[c];
            j2[o1][c] = abfSinProduct(*s, v[c], e[o1]) * wi[o1];
            j2[o2][c] = abfSinProduct(*s, v[c], e[o2]) * wi[o2];
            for (int k = 0; k < 3; ++k) {
                s->j2dt[e[k]][c] = j2[k][c];
            }
            rhs[iv] += j2[c][c] * beta[c];
            rhs[ninterior + iv] += j2[o1][c] * beta[o1] + j2[o2][c] * beta[o2];
            for (int rr = 0; rr < 3; ++rr) {
                row[rr][c] = j2[c][c] * w[rr][c];
                row[rr][3 + c] = j2[o1][c] * w[rr][o1] + j2[o2][c] * w[rr][o2];
            }
        }
        for (int i = 0; i < 3; ++i) {
            const int r = vid[i];
            if (r == -1) {
                continue;
            }
            for (int j = 0; j < 6; ++j) {
                const int c = vid[j];
                if (c == -1) {
                    continue;
                }
                for (int k = 0; k < 3; ++k) {
                    trips.emplace_back((k == i) ? r : r + ninterior, c, j2[k][i] * row[k][j]);
                }
            }
        }
    }
    SpMat m(nvar, nvar);
    m.setFromTriplets(trips.begin(), trips.end());
    m.makeCompressed();
    Eigen::SparseLU<SpMat, Eigen::COLAMDOrdering<int>> lu;
    lu.analyzePattern(m);
    lu.factorize(m);
    if (lu.info() != Eigen::Success) {
        return false;
    }
    const Eigen::VectorXd x = lu.solve(rhs);
    if ((lu.info() != Eigen::Success) || !allFinite(x)) {
        return false;
    }
    for (int f = 0; f < (int)sys.tris.size(); ++f) {
        const int e[3] = { 3 * f, 3 * f + 1, 3 * f + 2 };
        double pre[3] = { 0., 0., 0. };
        for (int c = 0; c < 3; ++c) {
            const int iv = s->interiorOf[sys.tris[f][c]];
            if (iv < 0) {
                continue;
            }
            const double planar = x[iv];
            const double wheel = x[ninterior + iv];
            for (int k = 0; k < 3; ++k) {
                pre[k] += s->j2dt[e[k]][c] * ((k == c) ? planar : wheel);
            }
        }
        double dlambda1 = pre[0] + pre[1] + pre[2];
        dlambda1 = s->dstar[f] * (s->bstar[f] - dlambda1);
        s->lambdaTriangle[f] += dlambda1;
        for (int k = 0; k < 3; ++k) {
            abfAdjustAlpha(s, e[k], dlambda1, pre[k]);
        }
    }
    for (int i = 0; i < ninterior; ++i) {
        s->lambdaPlanar[i] += x[i];
        s->lambdaLength[i] += x[ninterior + i];
    }

    return true;
}

bool
abfSolve(const ChartSystem& sys,
         const std::function<bool()>& stopped,
         std::vector<double>* angles)
{
    const int nfaces = (int)sys.tris.size();
    const int nangles = 3 * nfaces;
    AbfSystem s;
    s.interiorOf.assign(sys.n, -1);
    s.wedges.assign(sys.n, std::vector<int>());
    for (int f = 0; f < nfaces; ++f) {
        for (int k = 0; k < 3; ++k) {
            s.wedges[sys.tris[f][k]].push_back(3 * f + k);
        }
    }
    for (int v = 0; v < sys.n; ++v) {
        if (!sys.boundary[v]) {
            s.interiorOf[v] = s.ninterior++;
        }
    }
    s.alpha.resize(nangles);
    s.beta.resize(nangles);
    s.sine.resize(nangles);
    s.cosine.resize(nangles);
    s.weight.resize(nangles);
    s.bAlpha.resize(nangles);
    s.j2dt.assign(nangles, { 0., 0., 0. });
    s.bTriangle.resize(nfaces);
    s.bstar.resize(nfaces);
    s.dstar.resize(nfaces);
    s.lambdaTriangle.assign(nfaces, 0.);
    s.bInterior.resize(2 * s.ninterior);
    s.lambdaPlanar.assign(s.ninterior, 0.);
    s.lambdaLength.assign(s.ninterior, 1.);
    // Corners no sharper than this keep the weights 2 / a^2 finite.
    const double minAngle = 1e-3;
    for (int f = 0; f < nfaces; ++f) {
        for (int k = 0; k < 3; ++k) {
            const std::array<int, 3>& t = sys.tris[f];
            const double a = std::max(cornerAngle(sys.pos[t[k]], sys.pos[t[(k + 1) % 3]], sys.pos[t[(k + 2) % 3]]), minAngle);
            s.alpha[3 * f + k] = s.beta[3 * f + k] = a;
            s.weight[3 * f + k] = 2. / (a * a);
        }
    }
    for (int v = 0; v < sys.n; ++v) {
        if (s.interiorOf[v] < 0) {
            continue;
        }
        double sum = 0.;
        for (int corner : s.wedges[v]) {
            sum += s.beta[corner];
        }
        const double scale = (sum == 0.) ? 0. : 2. * kPi / sum;
        for (int corner : s.wedges[v]) {
            s.beta[corner] = s.alpha[corner] = s.beta[corner] * scale;
        }
    }
    if (s.ninterior > 0) {
        const double limit = (nfaces > 100) ? 1. : 0.001;
        const int maxIterations = 20;
        abfComputeSines(&s);
        int i = 0;
        for (; i < maxIterations; ++i) {
            if (stopped()) {
                return false;
            }
            if (abfComputeGradient(&s, sys) < limit) {
                break;
            }
            if (!abfMatrixInvert(&s, sys)) {
                return false;
            }
            abfComputeSines(&s);
        }
        if (i == maxIterations) {
            return false;
        }
    }
    *angles = s.alpha;

    return true;
}

std::pair<int, int>
edgeKey(int a,
        int b)
{
    return std::make_pair(std::min(a, b), std::max(a, b));
}

// Classes the edges of each quad region holding a rectify edge as horizontal (0) or vertical
// (1), spreading from the rectify edges across the edges the region's quads share.
std::map<std::pair<int, int>, int>
classifyRectify(const GeoUVFlattenChart& chart,
                const ChartSystem& sys,
                const std::vector<std::array<int, 2>>& rectify)
{
    std::map<std::pair<int, int>, int> classes;
    std::vector<std::array<int, 4>> quads;
    for (const std::vector<int>& face : chart.faces) {
        const std::vector<int> ring = cleanRing(face);
        if (ring.size() != 4) {
            continue;
        }
        std::array<int, 4> q;
        bool active = true;
        for (int k = 0; k < 4; ++k) {
            q[k] = sys.activeOf[ring[k]];
            active = active && (q[k] >= 0);
        }
        if (active) {
            quads.push_back(q);
        }
    }
    std::map<std::pair<int, int>, std::vector<int>> quadsOfEdge;
    for (int i = 0; i < (int)quads.size(); ++i) {
        for (int k = 0; k < 4; ++k) {
            quadsOfEdge[edgeKey(quads[i][k], quads[i][(k + 1) % 4])].push_back(i);
        }
    }
    std::vector<char> inRegion(quads.size(), 0);
    for (const std::array<int, 2>& e : rectify) {
        const auto found = quadsOfEdge.find(edgeKey(e[0], e[1]));
        if (found != quadsOfEdge.end()) {
            for (int q : found->second) {
                inRegion[q] = 1;
            }
        }
    }
    const auto sideOf = [&](int q, const std::pair<int, int>& key) {
        for (int k = 0; k < 4; ++k) {
            if (edgeKey(quads[q][k], quads[q][(k + 1) % 4]) == key) {
                return k;
            }
        }

        return -1;
    };
    const auto assign = [&](int q, int side, int cls) {
        for (int m = 0; m < 4; ++m) {
            const int k = (side + m) % 4;
            classes.emplace(edgeKey(quads[q][k], quads[q][(k + 1) % 4]), ((m % 2) == 0) ? cls : 1 - cls);
        }
    };
    std::vector<char> visited(quads.size(), 0);
    for (const std::array<int, 2>& e : rectify) {
        const std::pair<int, int> key = edgeKey(e[0], e[1]);
        const auto found = quadsOfEdge.find(key);
        if (found == quadsOfEdge.end()) {
            continue;
        }
        for (int seed : found->second) {
            if (visited[seed]) {
                continue;
            }
            visited[seed] = 1;
            const auto known = classes.find(key);
            assign(seed, sideOf(seed, key), (known != classes.end()) ? known->second : 0);
            std::vector<int> queue(1, seed);
            while (!queue.empty()) {
                const int q = queue.back();
                queue.pop_back();
                for (int k = 0; k < 4; ++k) {
                    const std::pair<int, int> side = edgeKey(quads[q][k], quads[q][(k + 1) % 4]);
                    const int cls = classes[side];
                    for (int next : quadsOfEdge[side]) {
                        if (!inRegion[next] || visited[next]) {
                            continue;
                        }
                        visited[next] = 1;
                        assign(next, sideOf(next, side), cls);
                        queue.push_back(next);
                    }
                }
            }
        }
    }

    return classes;
}

std::vector<int>
loopVertices(const std::vector<std::array<int, 2>>& edges)
{
    std::vector<int> vertices;
    for (const std::array<int, 2>& e : edges) {
        for (int v : e) {
            if ((v >= 0) && (std::find(vertices.begin(), vertices.end(), v) == vertices.end())) {
                vertices.push_back(v);
            }
        }
    }

    return vertices;
}

bool
vertexFixed(const Reduction& r,
            int v)
{
    return (r.unknownOf[2 * v] < 0) && (r.unknownOf[2 * v + 1] < 0);
}

// Rows holding each loop's vertices on one line: through two of its fixed vertices when it
// has them, otherwise along the principal direction of the loop in the current layout.
std::vector<LinearRow>
straightenRows(const std::vector<std::vector<int>>& loops,
               const Reduction& r,
               const std::vector<double>& x)
{
    std::vector<LinearRow> rows;
    for (const std::vector<int>& loop : loops) {
        if (loop.size() < 3) {
            continue;
        }
        std::vector<int> fixed;
        for (int v : loop) {
            if (vertexFixed(r, v)) {
                fixed.push_back(v);
            }
        }
        double dx = 0., dy = 0.;
        if (fixed.size() >= 2) {
            dx = x[2 * fixed[1]] - x[2 * fixed[0]];
            dy = x[2 * fixed[1] + 1] - x[2 * fixed[0] + 1];
        }
        if (!(dx * dx + dy * dy > 0.)) {
            double mx = 0., my = 0.;
            for (int v : loop) {
                mx += x[2 * v];
                my += x[2 * v + 1];
            }
            mx /= (double)loop.size();
            my /= (double)loop.size();
            double sxx = 0., sxy = 0., syy = 0.;
            for (int v : loop) {
                const double ex = x[2 * v] - mx, ey = x[2 * v + 1] - my;
                sxx += ex * ex;
                sxy += ex * ey;
                syy += ey * ey;
            }
            const double angle = 0.5 * std::atan2(2. * sxy, sxx - syy);
            dx = std::cos(angle);
            dy = std::sin(angle);
        }
        const double len = std::sqrt(dx * dx + dy * dy);
        const double nx = -dy / len, ny = dx / len;
        const int anchor = fixed.empty() ? loop[0] : fixed[0];
        for (int v : loop) {
            if (v == anchor) {
                continue;
            }
            LinearRow row;
            row.terms = { { 2 * v, nx }, { 2 * v + 1, ny }, { 2 * anchor, -nx }, { 2 * anchor + 1, -ny } };
            rows.push_back(row);
        }
    }

    return rows;
}

// Two vertices far apart to hold a pinless angle-based layout in place: on the first
// straightened loop when there is one, the extremes of the longest bounding box axis
// otherwise. A closed loop's first and last vertices are neighbours, so the pair on a loop is
// found as the vertex farthest from its first, and the vertex farthest from that.
void
autoLock(const ChartSystem& sys,
         const std::vector<std::vector<int>>& loops,
         UnionFind& merged,
         bool hasMerges,
         std::vector<std::pair<int, double>>* fixes)
{
    int p1 = -1, p2 = -1;
    if (!loops.empty() && (loops[0].size() >= 2)) {
        const std::vector<int>& loop = loops[0];
        const auto farthestFrom = [&](int from) {
            int best = from;
            double bestDistance = -1.;
            for (int v : loop) {
                const double distance = length(sub(sys.pos[v], sys.pos[from]));
                if (distance > bestDistance) {
                    best = v;
                    bestDistance = distance;
                }
            }

            return best;
        };
        p1 = farthestFrom(loop.front());
        p2 = farthestFrom(p1);
    }
    if ((p1 < 0) || (p1 == p2)) {
        Vec3 lo = sys.pos[0], hi = sys.pos[0];
        for (const Vec3& p : sys.pos) {
            for (int a = 0; a < 3; ++a) {
                lo[a] = std::min(lo[a], p[a]);
                hi[a] = std::max(hi[a], p[a]);
            }
        }
        int axis = 0;
        for (int a = 1; a < 3; ++a) {
            if (hi[a] - lo[a] > hi[axis] - lo[axis]) {
                axis = a;
            }
        }
        p1 = p2 = 0;
        for (int v = 1; v < sys.n; ++v) {
            if (sys.pos[v][axis] < sys.pos[p1][axis]) {
                p1 = v;
            }
            if (sys.pos[v][axis] > sys.pos[p2][axis]) {
                p2 = v;
            }
        }
    }
    double d = length(sub(sys.pos[p2], sys.pos[p1]));
    if (!(d > 0.)) {
        d = 1.;
    }
    fixes->push_back({ 2 * p1, 0. });
    fixes->push_back({ 2 * p1 + 1, 0. });
    const bool sameU = merged.find(2 * p2) == merged.find(2 * p1);
    const bool sameV = merged.find(2 * p2 + 1) == merged.find(2 * p1 + 1);
    if (!hasMerges) {
        fixes->push_back({ 2 * p2, d });
        fixes->push_back({ 2 * p2 + 1, 0. });
    } else if (!sameU) {
        fixes->push_back({ 2 * p2, d });
    } else if (!sameV) {
        fixes->push_back({ 2 * p2 + 1, d });
    }
}

void
applySimilarity(const GeoUVPackPlacement& placement,
                std::vector<double>* x)
{
    for (std::size_t i = 0; i + 1 < x->size(); i += 2) {
        const GeoUV uv = geoUVPackApply(placement, { (*x)[i], (*x)[i + 1] });
        (*x)[i] = uv[0];
        (*x)[i + 1] = uv[1];
    }
}
} // anonymous namespace

void
geoBuildUVFlattenCharts(const GeoDetail& detail,
                        const GeoUVIslands& islands,
                        std::vector<GeoUVFlattenChart>* charts,
                        std::vector<int>* chartVertexOfVertex)
{
    const GeoOffset vertexCount = detail.vertexMap().offsetSize();
    charts->assign(islands.islands.size(), GeoUVFlattenChart());
    chartVertexOfVertex->assign((std::size_t)vertexCount, -1);
    UnionFind corners((std::size_t)vertexCount);
    for (const std::vector<GeoOffset>& island : islands.islands) {
        for (GeoOffset prim : island) {
            const GeoVertexRange range = detail.primitiveVertices(prim);
            for (GeoOffset v = range.start; v < range.start + range.count; ++v) {
                if (((std::size_t)v >= islands.vertexEdgeTwin.size()) || islands.vertexEdgeCut[v]) {
                    continue;
                }
                const GeoOffset t = islands.vertexEdgeTwin[v];
                if (t == GeoIndexMap::kInvalidOffset) {
                    continue;
                }
                const GeoOffset vn = geoVertexEdgeEnd(detail, v);
                const GeoOffset tn = geoVertexEdgeEnd(detail, t);
                if (detail.vertexPoint(v) == detail.vertexPoint(t)) {
                    corners.unite((int)v, (int)t);
                    corners.unite((int)vn, (int)tn);
                } else {
                    corners.unite((int)v, (int)tn);
                    corners.unite((int)vn, (int)t);
                }
            }
        }
    }
    for (std::size_t i = 0; i < islands.islands.size(); ++i) {
        GeoUVFlattenChart& chart = (*charts)[i];
        std::map<int, int> vertexOfRoot;
        for (GeoOffset prim : islands.islands[i]) {
            const GeoVertexRange range = detail.primitiveVertices(prim);
            std::vector<int> face;
            for (GeoOffset v = range.start; v < range.start + range.count; ++v) {
                const int root = corners.find((int)v);
                auto found = vertexOfRoot.find(root);
                if (found == vertexOfRoot.end()) {
                    found = vertexOfRoot.emplace(root, (int)chart.positions.size()).first;
                    chart.positions.push_back(detail.pointPosition(detail.vertexPoint(v)));
                    chart.vertexOffsets.push_back(v);
                }
                (*chartVertexOfVertex)[v] = found->second;
                face.push_back(found->second);
            }
            chart.faces.push_back(face);
            chart.faceOffsets.push_back(prim);
        }
    }
}

bool
geoFlattenUVChart(const GeoUVFlattenChart& chart,
                  const GeoUVFlattenChartConstraints& constraints,
                  GeoUVFlattenMethodEnum method,
                  GeoUVFlattenChartResult* result,
                  std::string* errorMessage,
                  const std::function<bool()>& aborted)
{
    *result = GeoUVFlattenChartResult();
    bool stop = false;
    const std::function<bool()> stopped = [&]() {
        stop = stop || (aborted && aborted());

        return stop;
    };
    const auto stopSolve = [&]() {
        result->aborted = true;
        result->solved = false;
        if (errorMessage) {
            errorMessage->clear();
        }

        return false;
    };
    result->uv.assign(chart.positions.size(), GeoUV { 0., 0. });
    ChartSystem sys;
    buildSystem(chart, &sys);
    result->degenerateFaces = sys.degenerateFaces;
    if (sys.n == 0) {
        result->solved = true;

        return true;
    }
    const auto active = [&](int v) {
        return ((v >= 0) && (v < (int)sys.activeOf.size())) ? sys.activeOf[v] : -1;
    };

    UnionFind merged(2 * (std::size_t)sys.n);
    bool hasMerges = false;
    for (const GeoUVFlattenAlignment& alignment : constraints.alignments) {
        int first = -1;
        for (int cv : alignment.vertices) {
            const int v = active(cv);
            if (v < 0) {
                continue;
            }
            const int var = 2 * v + ((alignment.axis == eGeoUVFlattenAxisU) ? 1 : 0);
            if (first < 0) {
                first = var;
            } else {
                merged.unite(first, var);
                hasMerges = true;
            }
        }
    }
    std::vector<std::array<int, 2>> rectify;
    for (const std::array<int, 2>& e : constraints.rectify) {
        if ((active(e[0]) >= 0) && (active(e[1]) >= 0)) {
            rectify.push_back({ active(e[0]), active(e[1]) });
        }
    }
    if (!rectify.empty()) {
        for (const auto& e : classifyRectify(chart, sys, rectify)) {
            const int offset = (e.second == 0) ? 1 : 0;
            merged.unite(2 * e.first.first + offset, 2 * e.first.second + offset);
            hasMerges = true;
        }
    }
    std::vector<std::vector<int>> loops;
    for (const std::vector<std::array<int, 2>>& straightening : constraints.straightenings) {
        std::vector<std::array<int, 2>> edges;
        for (const std::array<int, 2>& e : straightening) {
            edges.push_back({ active(e[0]), active(e[1]) });
        }
        std::vector<int> loop = loopVertices(edges);
        if (loop.size() >= 3) {
            loops.push_back(loop);
        }
    }
    // Two pins on one vertex fix one point, which leaves the similarity free.
    std::vector<GeoUVFlattenPin> pins;
    std::map<int, std::size_t> pinOfVertex;
    for (const GeoUVFlattenPin& pin : constraints.pins) {
        if (active(pin.vertex) >= 0) {
            GeoUVFlattenPin p = pin;
            p.vertex = active(pin.vertex);
            p.referenceVertex = active(pin.referenceVertex);
            const auto found = pinOfVertex.find(p.vertex);
            if (found == pinOfVertex.end()) {
                pinOfVertex.emplace(p.vertex, pins.size());
                pins.push_back(p);
            } else {
                pins[found->second] = p;
            }
        }
    }
    std::vector<std::pair<int, double>> pinFixes;
    if (pins.size() >= 2) {
        for (const GeoUVFlattenPin& pin : pins) {
            pinFixes.push_back({ 2 * pin.vertex, pin.uv[0] });
            pinFixes.push_back({ 2 * pin.vertex + 1, pin.uv[1] });
        }
    }
    std::vector<std::pair<int, double>> boundaryFixes = pinFixes;
    const bool preserveBoundary = (method == eGeoUVFlattenMethodSCP) && (constraints.preservedBoundary.size() == chart.positions.size())
        && std::any_of(sys.boundary.begin(), sys.boundary.end(), [](char b) { return b != 0; });
    if (preserveBoundary) {
        for (int v = 0; v < sys.n; ++v) {
            if (sys.boundary[v]) {
                const GeoUV& uv = constraints.preservedBoundary[sys.chartOf[v]];
                boundaryFixes.push_back({ 2 * v, uv[0] });
                boundaryFixes.push_back({ 2 * v + 1, uv[1] });
            }
        }
    }

    std::vector<double> x;
    bool ok = false;
    if (method == eGeoUVFlattenMethodABF) {
        std::vector<double> angles;
        if (abfSolve(sys, stopped, &angles)) {
            const SpMat q = angleEnergy(sys, angles);
            std::vector<std::pair<int, double>> fixes = pinFixes;
            if (fixes.empty()) {
                autoLock(sys, loops, merged, hasMerges, &fixes);
            }
            const Reduction r = makeReduction(merged, fixes);
            ok = solveConstrained(q, r, std::vector<LinearRow>(), stopped, &x);
            if (ok && !loops.empty()) {
                std::vector<double> straightened;
                if (solveConstrained(q, r, straightenRows(loops, r, x), stopped, &straightened)) {
                    x = straightened;
                }
            }
        }
        if (stopped()) {
            return stopSolve();
        }
        result->fellBack = !ok;
    }
    if (!ok) {
        const SpMat q = conformalEnergy(sys);
        const std::vector<std::pair<int, double>>& fixes = preserveBoundary ? boundaryFixes : pinFixes;
        const Reduction r = makeReduction(merged, fixes);
        ok = fixes.empty() ? solveSpectral(sys, q, r, stopped, &x) : solveConstrained(q, r, std::vector<LinearRow>(), stopped, &x);
        if (stopped()) {
            return stopSolve();
        }
    }
    result->solved = ok;
    if (!ok) {
        x = projectToPlane(sys);
        if (errorMessage) {
            *errorMessage = "the island could not be flattened";
        }
    }
    result->placed = ok && (!pinFixes.empty() || preserveBoundary || (pins.size() == 1));
    if (ok && pinFixes.empty() && !preserveBoundary) {
        double area = uvArea(sys, x);
        if (area < 0.) {
            for (int v = 0; v < sys.n; ++v) {
                x[2 * v + 1] = -x[2 * v + 1];
            }
            area = -area;
        }
        if ((area > 0.) && (sys.area > 0.)) {
            const double s = std::sqrt(sys.area / area);
            for (double& value : x) {
                value *= s;
            }
        }
        if (pins.size() == 1) {
            const GeoUVFlattenPin& pin = pins[0];
            const GeoUV at = { x[2 * pin.vertex], x[2 * pin.vertex + 1] };
            GeoUVPackPlacement placement;
            const bool referenced = (pin.referenceVertex >= 0) && (pin.referenceVertex != pin.vertex) && ((pin.referenceUV[0] != pin.uv[0]) || (pin.referenceUV[1] != pin.uv[1]));
            if (referenced) {
                const GeoUV next = { x[2 * pin.referenceVertex], x[2 * pin.referenceVertex + 1] };
                placement = geoUVFitSimilarity({ at, next }, { pin.uv, pin.referenceUV });
            } else {
                placement.t = { pin.uv[0] - at[0], pin.uv[1] - at[1] };
            }
            applySimilarity(placement, &x);
            x[2 * pin.vertex] = pin.uv[0];
            x[2 * pin.vertex + 1] = pin.uv[1];
        }
    }
    for (int v = 0; v < sys.n; ++v) {
        result->uv[sys.chartOf[v]] = { x[2 * v], x[2 * v + 1] };
    }
    std::vector<char> known(chart.positions.size(), 0);
    for (int v = 0; v < sys.n; ++v) {
        known[sys.chartOf[v]] = 1;
    }
    for (const std::vector<int>& face : chart.faces) {
        GeoUV sum = { 0., 0. };
        int count = 0;
        for (int v : face) {
            if (known[v]) {
                sum[0] += result->uv[v][0];
                sum[1] += result->uv[v][1];
                ++count;
            }
        }
        if (count == 0) {
            sum = result->uv[sys.chartOf[0]];
            count = 1;
        }
        for (int v : face) {
            if (!known[v]) {
                result->uv[v] = { sum[0] / count, sum[1] / count };
            }
        }
    }

    return ok;
} // geoFlattenUVChart

bool
geoFlattenUVCharts(const std::vector<GeoUVFlattenChart>& charts,
                   const std::vector<GeoUVFlattenChartConstraints>& constraints,
                   GeoUVFlattenMethodEnum method,
                   const std::function<bool()>& aborted,
                   std::vector<GeoUVFlattenChartResult>* results,
                   std::string* errorMessage)
{
    results->assign(charts.size(), GeoUVFlattenChartResult());
    std::atomic<bool> stop(false);
    const GeoUVFlattenChartConstraints none;
    geoParallelFor(charts.size(), [&](std::size_t i) {
        if (stop.load(std::memory_order_relaxed) || (aborted && aborted())) {
            stop.store(true, std::memory_order_relaxed);

            return;
        }
        std::string ignored;
        geoFlattenUVChart(charts[i], (i < constraints.size()) ? constraints[i] : none, method, &(*results)[i], &ignored, aborted);
        if ((*results)[i].aborted) {
            stop.store(true, std::memory_order_relaxed);
        } }, 1);
    if (stop.load()) {
        if (errorMessage) {
            errorMessage->clear();
        }

        return false;
    }

    return true;
}

double
geoUVFlattenAngleDistortion(const GeoUVFlattenChart& chart,
                            const std::vector<GeoUV>& uv)
{
    double sum = 0.;
    int count = 0;
    for (const std::vector<int>& face : chart.faces) {
        const std::vector<int> ring = cleanRing(face);
        for (const std::array<int, 3>& t : triangulateRing(ring, chart.positions)) {
            const int v[3] = { ring[t[0]], ring[t[1]], ring[t[2]] };
            if (triangleDegenerate(chart.positions[v[0]], chart.positions[v[1]], chart.positions[v[2]])) {
                continue;
            }
            for (int k = 0; k < 3; ++k) {
                const int a = v[k], b = v[(k + 1) % 3], c = v[(k + 2) % 3];
                const double surface = cornerAngle(chart.positions[a], chart.positions[b], chart.positions[c]);
                const double flat = uvCornerAngle(uv[a], uv[b], uv[c]);
                sum += std::fabs(flat - surface);
                ++count;
            }
        }
    }

    return (count > 0) ? sum / count : 0.;
}

double
geoUVMinimumAreaRotation(const std::vector<GeoUV>& points)
{
    std::vector<GeoUV> sorted = points;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    if (sorted.size() < 3) {
        if (sorted.size() == 2) {
            const double angle = -std::atan2(sorted[1][1] - sorted[0][1], sorted[1][0] - sorted[0][0]);

            return std::remainder(angle, kPi / 2.);
        }

        return 0.;
    }
    const auto turn = [](const GeoUV& o, const GeoUV& a, const GeoUV& b) {
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
    };
    std::vector<GeoUV> hull(2 * sorted.size());
    std::size_t k = 0;
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        while ((k >= 2) && (turn(hull[k - 2], hull[k - 1], sorted[i]) <= 0.)) {
            --k;
        }
        hull[k++] = sorted[i];
    }
    for (std::size_t i = sorted.size() - 1, lower = k + 1; i > 0; --i) {
        while ((k >= lower) && (turn(hull[k - 2], hull[k - 1], sorted[i - 1]) <= 0.)) {
            --k;
        }
        hull[k++] = sorted[i - 1];
    }
    hull.resize(k - 1);
    double best = 0., bestArea = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < hull.size(); ++i) {
        const GeoUV& a = hull[i];
        const GeoUV& b = hull[(i + 1) % hull.size()];
        const double rotation = std::remainder(-std::atan2(b[1] - a[1], b[0] - a[0]), kPi / 2.);
        const double c = std::cos(rotation), s = std::sin(rotation);
        double x0 = std::numeric_limits<double>::infinity(), x1 = -x0, y0 = x0, y1 = -x0;
        for (const GeoUV& p : hull) {
            const double x = c * p[0] - s * p[1], y = s * p[0] + c * p[1];
            x0 = std::min(x0, x);
            x1 = std::max(x1, x);
            y0 = std::min(y0, y);
            y1 = std::max(y1, y);
        }
        const double area = (x1 - x0) * (y1 - y0);
        if ((area < bestArea * (1. - 1e-9)) || ((area <= bestArea * (1. + 1e-9)) && (std::fabs(rotation) < std::fabs(best)))) {
            bestArea = std::min(area, bestArea);
            best = rotation;
        }
    }

    return best;
}

GeoUVPackPlacement
geoUVFitSimilarity(const std::vector<GeoUV>& from,
                   const std::vector<GeoUV>& to)
{
    GeoUVPackPlacement placement;
    const std::size_t n = std::min(from.size(), to.size());
    if (n == 0) {
        return placement;
    }
    GeoUV cf = { 0., 0. }, ct = { 0., 0. };
    for (std::size_t i = 0; i < n; ++i) {
        cf[0] += from[i][0];
        cf[1] += from[i][1];
        ct[0] += to[i][0];
        ct[1] += to[i][1];
    }
    for (int a = 0; a < 2; ++a) {
        cf[a] /= (double)n;
        ct[a] /= (double)n;
    }
    double re = 0., im = 0., norm = 0.;
    for (std::size_t i = 0; i < n; ++i) {
        const double fx = from[i][0] - cf[0], fy = from[i][1] - cf[1];
        const double tx = to[i][0] - ct[0], ty = to[i][1] - ct[1];
        re += fx * tx + fy * ty;
        im += fx * ty - fy * tx;
        norm += fx * fx + fy * fy;
    }
    if (norm > 0.) {
        re /= norm;
        im /= norm;
    } else {
        re = 1.;
        im = 0.;
    }
    placement.m = { re, -im, im, re };
    placement.t = { ct[0] - (re * cf[0] - im * cf[1]), ct[1] - (im * cf[0] + re * cf[1]) };
    placement.packed = true;

    return placement;
}

GeoUVPackPlacement
geoUVFitInTurns(const std::vector<GeoUV>& from,
                const std::vector<GeoUV>& to,
                int step)
{
    static const std::array<double, 4> kTurns[4] = { { 1., 0., 0., 1. }, { 0., -1., 1., 0. }, { -1., 0., 0., -1. }, { 0., 1., -1., 0. } };
    GeoUVPackPlacement placement;
    placement.packed = true;
    const std::size_t n = std::min(from.size(), to.size());
    if (n == 0) {
        return placement;
    }
    GeoUV cf = { 0., 0. }, ct = { 0., 0. };
    for (std::size_t i = 0; i < n; ++i) {
        for (int a = 0; a < 2; ++a) {
            cf[a] += from[i][a] / (double)n;
            ct[a] += to[i][a] / (double)n;
        }
    }
    int best = 0;
    double bestDot = -std::numeric_limits<double>::infinity();
    for (int k = 0; k < 4; k += std::max(1, step)) {
        const std::array<double, 4>& m = kTurns[k];
        double dot = 0.;
        for (std::size_t i = 0; i < n; ++i) {
            const double fx = from[i][0] - cf[0], fy = from[i][1] - cf[1];
            dot += (m[0] * fx + m[1] * fy) * (to[i][0] - ct[0]) + (m[2] * fx + m[3] * fy) * (to[i][1] - ct[1]);
        }
        if (dot > bestDot) {
            best = k;
            bestDot = dot;
        }
    }
    const std::array<double, 4>& m = kTurns[best];
    GeoUV flo = { std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity() };
    GeoUV fhi = { -flo[0], -flo[1] };
    GeoUV tlo = flo, thi = fhi;
    for (std::size_t i = 0; i < n; ++i) {
        const GeoUV turned = { m[0] * from[i][0] + m[1] * from[i][1], m[2] * from[i][0] + m[3] * from[i][1] };
        for (int a = 0; a < 2; ++a) {
            flo[a] = std::min(flo[a], turned[a]);
            fhi[a] = std::max(fhi[a], turned[a]);
            tlo[a] = std::min(tlo[a], to[i][a]);
            thi[a] = std::max(thi[a], to[i][a]);
        }
    }
    double scale = std::numeric_limits<double>::infinity();
    for (int a = 0; a < 2; ++a) {
        if (fhi[a] - flo[a] > 0.) {
            scale = std::min(scale, (thi[a] - tlo[a]) / (fhi[a] - flo[a]));
        }
    }
    if (!(scale < std::numeric_limits<double>::infinity())) {
        scale = 1.;
    }
    placement.m = { scale * m[0], scale * m[1], scale * m[2], scale * m[3] };
    for (int a = 0; a < 2; ++a) {
        placement.t[a] = 0.5 * (tlo[a] + thi[a]) - scale * 0.5 * (flo[a] + fhi[a]);
    }

    return placement;
}

NATRON_NAMESPACE_EXIT
