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

#include "SplineWarpMesh.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <unordered_map>

#include <CDT.h>

NATRON_NAMESPACE_ENTER

namespace {
// As the thin-plate solver decides that destinations lie on a line.
const double kCollinearTolerance = 1e-10;
const double kBoxPadding = 0.25;
// Grid points this close to a triangle, in grid steps, are rasterized with it.
const double kCoverEpsilon = 1e-9;

Point
makePoint(double x,
          double y)
{
    Point p;

    p.x = x;
    p.y = y;

    return p;
}

// Merges pairs whose destinations are within kSplineWarpMergeDistance, averaging their sources;
// remap receives each pair's merged index, -1 for a pair with a non-finite coordinate.
std::vector<SplineWarpSamplePair>
mergeCoincident(const std::vector<SplineWarpSamplePair>& pairs,
                std::vector<int>* remap)
{
    struct Cell {
        std::int64_t x, y;
        bool operator==(const Cell& o) const
        {
            return x == o.x && y == o.y;
        }
    };
    struct CellHash {
        std::size_t operator()(const Cell& c) const
        {
            return std::hash<std::int64_t>()(c.x * 73856093LL ^ c.y * 19349663LL);
        }
    };

    const double cellSize = kSplineWarpMergeDistance;
    const double maxCoord = 1e12;
    std::unordered_map<Cell, std::vector<int>, CellHash> grid;
    std::vector<SplineWarpSamplePair> merged;
    std::vector<int> counts;

    remap->assign(pairs.size(), -1);
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        const SplineWarpSamplePair& p = pairs[i];
        if (!std::isfinite(p.destination.x) || !std::isfinite(p.destination.y) || !std::isfinite(p.source.x) || !std::isfinite(p.source.y) || (std::fabs(p.destination.x) > maxCoord) || (std::fabs(p.destination.y) > maxCoord)) {
            continue;
        }
        const Cell c = { (std::int64_t)std::floor(p.destination.x / cellSize), (std::int64_t)std::floor(p.destination.y / cellSize) };
        int found = -1;
        for (int dy = -1; dy <= 1 && found < 0; ++dy) {
            for (int dx = -1; dx <= 1 && found < 0; ++dx) {
                const Cell n = { c.x + dx, c.y + dy };
                std::unordered_map<Cell, std::vector<int>, CellHash>::const_iterator it = grid.find(n);
                if (it == grid.end()) {
                    continue;
                }
                for (std::size_t k = 0; k < it->second.size(); ++k) {
                    const SplineWarpSamplePair& q = merged[it->second[k]];
                    if (std::hypot(q.destination.x - p.destination.x, q.destination.y - p.destination.y) < kSplineWarpMergeDistance) {
                        found = it->second[k];
                        break;
                    }
                }
            }
        }
        if (found >= 0) {
            merged[found].source.x += p.source.x;
            merged[found].source.y += p.source.y;
            ++counts[found];
            (*remap)[i] = found;
        } else {
            (*remap)[i] = (int)merged.size();
            grid[c].push_back((int)merged.size());
            merged.push_back(p);
            counts.push_back(1);
        }
    }
    for (std::size_t i = 0; i < merged.size(); ++i) {
        merged[i].source.x /= counts[i];
        merged[i].source.y /= counts[i];
    }

    return merged;
} // mergeCoincident

// Whether the destinations span a plane, as the thin-plate solver requires.
bool
spansPlane(const std::vector<SplineWarpSamplePair>& pairs)
{
    const std::size_t n = pairs.size();

    if (n < 3) {
        return false;
    }
    double cx = 0., cy = 0.;
    for (std::size_t i = 0; i < n; ++i) {
        cx += pairs[i].destination.x;
        cy += pairs[i].destination.y;
    }
    cx /= n;
    cy /= n;
    double a = 0., b = 0., d = 0.;
    for (std::size_t i = 0; i < n; ++i) {
        const double x = pairs[i].destination.x - cx;
        const double y = pairs[i].destination.y - cy;
        a += x * x;
        b += x * y;
        d += y * y;
    }
    const double half = 0.5 * (a + d);
    const double root = std::sqrt(std::max(0., 0.25 * (a - d) * (a - d) + b * b));
    const double large = half + root;
    const double small = half - root;

    return (large > 0.) && (small > kCollinearTolerance * large);
}

std::pair<int, int>
edgeKey(int a,
        int b)
{
    return (a < b) ? std::make_pair(a, b) : std::make_pair(b, a);
}

struct EdgeKeyHash {
    std::size_t operator()(const std::pair<int, int>& e) const
    {
        return std::hash<std::int64_t>()(((std::int64_t)e.first << 32) ^ (std::int64_t)(std::uint32_t)e.second);
    }
};
} // anonymous namespace

SplineWarpMesh::SplineWarpMesh()
    : _useFallback(false)
    , _fallback()
    , _box()
    , _destinations()
    , _sources()
    , _triangles()
    , _constrainedEdges()
{
}

RectD
SplineWarpMesh::paddedBox(const std::vector<SplineWarpSamplePair>& pairs,
                          const RectD& bounds)
{
    RectD box;
    bool any = false;

    if (!bounds.isNull() && !bounds.isInfinite() && std::isfinite(bounds.x1) && std::isfinite(bounds.y1) && std::isfinite(bounds.x2) && std::isfinite(bounds.y2)) {
        box = bounds;
        any = true;
    }
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        const Point points[2] = { pairs[i].destination, pairs[i].source };
        for (int k = 0; k < 2; ++k) {
            if (!std::isfinite(points[k].x) || !std::isfinite(points[k].y)) {
                continue;
            }
            if (any) {
                box.merge(points[k].x, points[k].y, points[k].x, points[k].y);
            } else {
                box.set(points[k].x, points[k].y, points[k].x, points[k].y);
                any = true;
            }
        }
    }
    if (!any) {
        return RectD();
    }
    const double pad = std::max(1., kBoxPadding * std::max(box.width(), box.height()));

    return RectD(box.x1 - pad, box.y1 - pad, box.x2 + pad, box.y2 + pad);
}

SplineWarpMesh
SplineWarpMesh::build(const std::vector<SplineWarpSamplePair>& pairs,
                      const std::vector<SplineWarpSamplePolyline>& polylines,
                      const RectD& box)
{
    SplineWarpMesh mesh;
    std::vector<int> remap;
    const std::vector<SplineWarpSamplePair> merged = mergeCoincident(pairs, &remap);

    if (merged.empty()) {
        return mesh;
    }
    if (!spansPlane(merged)) {
        mesh._useFallback = true;
        mesh._fallback = SplineWarpTps::solve(merged, 0.);

        return mesh;
    }

    // The corners lie strictly outside every destination.
    RectD corners = box;
    double minX = merged[0].destination.x, maxX = minX;
    double minY = merged[0].destination.y, maxY = minY;
    for (std::size_t i = 1; i < merged.size(); ++i) {
        minX = std::min(minX, merged[i].destination.x);
        maxX = std::max(maxX, merged[i].destination.x);
        minY = std::min(minY, merged[i].destination.y);
        maxY = std::max(maxY, merged[i].destination.y);
    }
    const double margin = std::max(1., kBoxPadding * std::max(maxX - minX, maxY - minY));
    if (corners.isNull() || corners.isInfinite() || !(corners.x1 < minX) || !std::isfinite(corners.x1)) {
        corners.x1 = minX - margin;
    }
    if (corners.isNull() || corners.isInfinite() || !(corners.y1 < minY) || !std::isfinite(corners.y1)) {
        corners.y1 = minY - margin;
    }
    if (corners.isNull() || corners.isInfinite() || !(corners.x2 > maxX) || !std::isfinite(corners.x2)) {
        corners.x2 = maxX + margin;
    }
    if (corners.isNull() || corners.isInfinite() || !(corners.y2 > maxY) || !std::isfinite(corners.y2)) {
        corners.y2 = maxY + margin;
    }
    mesh._box = corners;

    const int n = (int)merged.size();
    std::vector<CDT::V2d<double>> vertices;
    vertices.reserve(n + 4);
    for (int i = 0; i < n; ++i) {
        vertices.push_back(CDT::V2d<double>(merged[i].destination.x, merged[i].destination.y));
    }
    const double cornerX[4] = { corners.x1, corners.x2, corners.x2, corners.x1 };
    const double cornerY[4] = { corners.y1, corners.y1, corners.y2, corners.y2 };
    for (int k = 0; k < 4; ++k) {
        vertices.push_back(CDT::V2d<double>(cornerX[k], cornerY[k]));
    }
    const int base = (int)vertices.size();

    // Constraint edges in insertion order, without repeats.
    std::vector<CDT::Edge> edges;
    std::unordered_map<std::pair<int, int>, int, EdgeKeyHash> edgeOrder;
    const auto addEdge = [&](int a, int b) {
        if ((a < 0) || (b < 0) || (a == b)) {
            return;
        }
        const std::pair<int, int> key = edgeKey(a, b);
        if (edgeOrder.find(key) != edgeOrder.end()) {
            return;
        }
        edgeOrder[key] = (int)edges.size();
        edges.push_back(CDT::Edge((CDT::VertInd)a, (CDT::VertInd)b));
    };
    for (std::size_t p = 0; p < polylines.size(); ++p) {
        const std::vector<int>& indices = polylines[p].pairs;
        std::vector<int> chain;
        for (std::size_t i = 0; i < indices.size(); ++i) {
            const int pair = indices[i];
            const int v = ((pair >= 0) && (pair < (int)remap.size())) ? remap[pair] : -1;
            if ((v >= 0) && (chain.empty() || (chain.back() != v))) {
                chain.push_back(v);
            }
        }
        for (std::size_t i = 1; i < chain.size(); ++i) {
            addEdge(chain[i - 1], chain[i]);
        }
        if (polylines[p].closed && (chain.size() > 2)) {
            addEdge(chain.back(), chain.front());
        }
    }

    typedef CDT::Triangulation<double> Triangulation;
    std::unique_ptr<Triangulation> triangulation(new Triangulation(CDT::VertexInsertionOrder::Auto, CDT::IntersectingConstraintEdges::TryResolve, 0.));
    try {
        triangulation->insertVertices(vertices);
        triangulation->insertEdges(edges);
        triangulation->eraseSuperTriangle();
    } catch (const std::exception&) {
        // Rounding broke the resolution of a crossing: triangulate without constraints.
        edges.clear();
        edgeOrder.clear();
        triangulation.reset(new Triangulation(CDT::VertexInsertionOrder::Auto, CDT::IntersectingConstraintEdges::TryResolve, 0.));
        try {
            triangulation->insertVertices(vertices);
            triangulation->eraseSuperTriangle();
        } catch (const std::exception&) {
            mesh._useFallback = true;
            mesh._fallback = SplineWarpTps::solve(merged, 0.);

            return mesh;
        }
    }
    const Triangulation& cdt = *triangulation;

    const int count = (int)cdt.vertices.size();
    mesh._destinations.resize(count);
    mesh._sources.resize(count);
    for (int i = 0; i < count; ++i) {
        mesh._destinations[i] = makePoint(cdt.vertices[i].x, cdt.vertices[i].y);
    }
    for (int i = 0; i < n; ++i) {
        mesh._sources[i] = merged[i].source;
    }
    for (int i = n; i < count; ++i) {
        mesh._sources[i] = mesh._destinations[i];
    }

    // A vertex added at a crossing lies on the pieces of the original edges it splits: it takes
    // its source along the one inserted first.
    std::vector<int> firstOriginal(count, -1);
    for (CDT::EdgeUSet::const_iterator it = cdt.fixedEdges.begin(); it != cdt.fixedEdges.end(); ++it) {
        const int a = (int)it->v1();
        const int b = (int)it->v2();
        mesh._constrainedEdges.push_back(edgeKey(a, b));
        if ((a < base) && (b < base)) {
            continue;
        }
        const std::unordered_map<CDT::Edge, CDT::EdgeVec>::const_iterator originals = cdt.pieceToOriginals.find(*it);
        if (originals == cdt.pieceToOriginals.end()) {
            continue;
        }
        for (std::size_t k = 0; k < originals->second.size(); ++k) {
            const CDT::Edge& o = originals->second[k];
            const std::unordered_map<std::pair<int, int>, int, EdgeKeyHash>::const_iterator order = edgeOrder.find(edgeKey((int)o.v1(), (int)o.v2()));
            if (order == edgeOrder.end()) {
                continue;
            }
            const int ends[2] = { a, b };
            for (int e = 0; e < 2; ++e) {
                const int v = ends[e];
                if ((v >= base) && ((firstOriginal[v] < 0) || (order->second < firstOriginal[v]))) {
                    firstOriginal[v] = order->second;
                }
            }
        }
    }
    std::sort(mesh._constrainedEdges.begin(), mesh._constrainedEdges.end());
    for (int v = base; v < count; ++v) {
        if (firstOriginal[v] < 0) {
            continue;
        }
        const CDT::Edge& e = edges[firstOriginal[v]];
        const int a = (int)e.v1();
        const int b = (int)e.v2();
        const Point& pa = mesh._destinations[a];
        const Point& pb = mesh._destinations[b];
        const double ex = pb.x - pa.x;
        const double ey = pb.y - pa.y;
        const double len2 = ex * ex + ey * ey;
        double t = (len2 > 0.) ? ((mesh._destinations[v].x - pa.x) * ex + (mesh._destinations[v].y - pa.y) * ey) / len2 : 0.;
        t = std::max(0., std::min(1., t));
        mesh._sources[v] = makePoint(mesh._sources[a].x + t * (mesh._sources[b].x - mesh._sources[a].x),
                                     mesh._sources[a].y + t * (mesh._sources[b].y - mesh._sources[a].y));
    }

    mesh._triangles.reserve(cdt.triangles.size());
    for (std::size_t t = 0; t < cdt.triangles.size(); ++t) {
        Triangle triangle;
        for (int k = 0; k < 3; ++k) {
            triangle.v[k] = (int)cdt.triangles[t].vertices[k];
        }
        mesh._triangles.push_back(triangle);
    }

    return mesh;
} // SplineWarpMesh::build

Point
SplineWarpMesh::evaluate(double x,
                         double y) const
{
    if (_useFallback) {
        return _fallback.evaluate(x, y);
    }
    if (_triangles.empty() || (x < _box.x1) || (x > _box.x2) || (y < _box.y1) || (y > _box.y2)) {
        return makePoint(x, y);
    }
    for (std::size_t t = 0; t < _triangles.size(); ++t) {
        const Point& a = _destinations[_triangles[t].v[0]];
        const Point& b = _destinations[_triangles[t].v[1]];
        const Point& c = _destinations[_triangles[t].v[2]];
        const double area2 = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (area2 == 0.) {
            continue;
        }
        const double lb = ((x - a.x) * (c.y - a.y) - (y - a.y) * (c.x - a.x)) / area2;
        const double lc = ((b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x)) / area2;
        const double la = 1. - lb - lc;
        if ((la < -kCoverEpsilon) || (lb < -kCoverEpsilon) || (lc < -kCoverEpsilon)) {
            continue;
        }
        const Point& sa = _sources[_triangles[t].v[0]];
        const Point& sb = _sources[_triangles[t].v[1]];
        const Point& sc = _sources[_triangles[t].v[2]];

        return makePoint(la * sa.x + lb * sb.x + lc * sc.x, la * sa.y + lb * sb.y + lc * sc.y);
    }

    return makePoint(x, y);
}

void
SplineWarpMesh::rasterize(int i0,
                          int j0,
                          int nx,
                          int ny,
                          double step,
                          double* sourceX,
                          double* sourceY) const
{
    if ((nx <= 0) || (ny <= 0) || !(step > 0.)) {
        return;
    }
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            const std::size_t idx = (std::size_t)j * nx + i;
            const Point s = _useFallback ? _fallback.evaluate((i0 + i) * step, (j0 + j) * step) : makePoint((i0 + i) * step, (j0 + j) * step);
            sourceX[idx] = s.x;
            sourceY[idx] = s.y;
        }
    }
    if (_useFallback) {
        return;
    }

    for (std::size_t t = 0; t < _triangles.size(); ++t) {
        const Point& a = _destinations[_triangles[t].v[0]];
        const Point& b = _destinations[_triangles[t].v[1]];
        const Point& c = _destinations[_triangles[t].v[2]];
        const double area2 = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (area2 == 0.) {
            continue;
        }
        const Point& sa = _sources[_triangles[t].v[0]];
        const Point& sb = _sources[_triangles[t].v[1]];
        const Point& sc = _sources[_triangles[t].v[2]];

        const double minY = std::min(a.y, std::min(b.y, c.y));
        const double maxY = std::max(a.y, std::max(b.y, c.y));
        const int jFirst = std::max(0, (int)std::ceil(minY / step - kCoverEpsilon) - j0);
        const int jLast = std::min(ny - 1, (int)std::floor(maxY / step + kCoverEpsilon) - j0);
        const Point* const corners[3] = { &a, &b, &c };
        for (int j = jFirst; j <= jLast; ++j) {
            const double y = (j0 + j) * step;
            // The row's span across the triangle.
            double xl = std::numeric_limits<double>::infinity();
            double xr = -std::numeric_limits<double>::infinity();
            for (int k = 0; k < 3; ++k) {
                const Point& p = *corners[k];
                const Point& q = *corners[(k + 1) % 3];
                const double lo = std::min(p.y, q.y);
                const double hi = std::max(p.y, q.y);
                if ((y < lo - kCoverEpsilon * step) || (y > hi + kCoverEpsilon * step)) {
                    continue;
                }
                if (hi - lo <= 0.) {
                    xl = std::min(xl, std::min(p.x, q.x));
                    xr = std::max(xr, std::max(p.x, q.x));
                } else {
                    const double f = std::max(0., std::min(1., (y - p.y) / (q.y - p.y)));
                    const double x = p.x + f * (q.x - p.x);
                    xl = std::min(xl, x);
                    xr = std::max(xr, x);
                }
            }
            if (!(xl <= xr)) {
                continue;
            }
            const int iFirst = std::max(0, (int)std::ceil(xl / step - kCoverEpsilon) - i0);
            const int iLast = std::min(nx - 1, (int)std::floor(xr / step + kCoverEpsilon) - i0);
            for (int i = iFirst; i <= iLast; ++i) {
                const double x = (i0 + i) * step;
                const double lb = ((x - a.x) * (c.y - a.y) - (y - a.y) * (c.x - a.x)) / area2;
                const double lc = ((b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x)) / area2;
                const double la = 1. - lb - lc;
                const std::size_t idx = (std::size_t)j * nx + i;
                sourceX[idx] = la * sa.x + lb * sb.x + lc * sc.x;
                sourceY[idx] = la * sa.y + lb * sb.y + lc * sc.y;
            }
        }
    }
} // SplineWarpMesh::rasterize

SplineWarpMeshConstPtr
SplineWarpMeshCache::get(U64 sampleHash,
                         const std::vector<SplineWarpSamplePair>& pairs,
                         const std::vector<SplineWarpSamplePolyline>& polylines,
                         const RectD& box)
{
    Key key;

    key.hash = sampleHash;
    key.x1 = box.x1;
    key.y1 = box.y1;
    key.x2 = box.x2;
    key.y2 = box.y2;

    return _cache.get(key, [&]() {
        return SplineWarpMesh::build(pairs, polylines, box);
    });
}

NATRON_NAMESPACE_EXIT
