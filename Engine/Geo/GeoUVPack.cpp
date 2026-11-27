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

#include "GeoUVPack.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <random>

#include "Engine/Geo/GeoPage.h"

NATRON_NAMESPACE_ENTER

namespace {

const double kPi = 3.14159265358979323846;

typedef std::vector<std::vector<GeoUV>> Polygons;

struct Grid {
    int w = 0;
    int h = 0;
    std::vector<char> cells;

    Grid() = default;
    Grid(int w_, int h_)
        : w(w_)
        , h(h_)
        , cells(static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_), 0)
    {
    }

    char at(int x, int y) const { return cells[static_cast<std::size_t>(y) * w + x]; }

    // Cells beyond the grid are dropped, so callers can draw shapes that overhang it.
    void set(int x, int y)
    {
        if (x >= 0 && y >= 0 && x < w && y < h) {
            cells[static_cast<std::size_t>(y) * w + x] = 1;
        }
    }
};

// Marks every cell the segment passes through, in cell coordinates, so the marked cells cover it.
void
markSegment(Grid* grid, const GeoUV& a, const GeoUV& b)
{
    int cx = static_cast<int>(std::floor(a[0]));
    int cy = static_cast<int>(std::floor(a[1]));
    const int ex = static_cast<int>(std::floor(b[0]));
    const int ey = static_cast<int>(std::floor(b[1]));
    const double dx = b[0] - a[0];
    const double dy = b[1] - a[1];
    const int sx = dx > 0. ? 1 : (dx < 0. ? -1 : 0);
    const int sy = dy > 0. ? 1 : (dy < 0. ? -1 : 0);
    const double inf = std::numeric_limits<double>::infinity();
    double tMaxX = sx > 0 ? (cx + 1 - a[0]) / dx : (sx < 0 ? (a[0] - cx) / -dx : inf);
    double tMaxY = sy > 0 ? (cy + 1 - a[1]) / dy : (sy < 0 ? (a[1] - cy) / -dy : inf);
    const double tDeltaX = sx != 0 ? 1. / std::abs(dx) : inf;
    const double tDeltaY = sy != 0 ? 1. / std::abs(dy) : inf;

    grid->set(cx, cy);
    for (int steps = std::abs(ex - cx) + std::abs(ey - cy) + 2; (cx != ex || cy != ey) && steps > 0; --steps) {
        if (tMaxX < tMaxY) {
            cx += sx;
            tMaxX += tDeltaX;
        } else if (tMaxY < tMaxX) {
            cy += sy;
            tMaxY += tDeltaY;
        } else {
            // Through a corner: the two side cells touch the segment there too.
            grid->set(cx + sx, cy);
            grid->set(cx, cy + sy);
            cx += sx;
            cy += sy;
            tMaxX += tDeltaX;
            tMaxY += tDeltaY;
        }
        grid->set(cx, cy);
    }
}

// Marks the cells whose centres a polygon covers, by even-odd crossings along each row.
void
fillPolygon(Grid* grid, const std::vector<GeoUV>& poly)
{
    double minY = std::numeric_limits<double>::infinity();
    double maxY = -minY;
    for (const GeoUV& c : poly) {
        minY = std::min(minY, c[1]);
        maxY = std::max(maxY, c[1]);
    }
    const int row0 = std::max(0, static_cast<int>(std::ceil(minY - 0.5)));
    const int row1 = std::min(grid->h - 1, static_cast<int>(std::floor(maxY - 0.5)));
    std::vector<double> crossings;
    for (int row = row0; row <= row1; ++row) {
        const double y = row + 0.5;
        crossings.clear();
        for (std::size_t k = 0; k < poly.size(); ++k) {
            const GeoUV& a = poly[k];
            const GeoUV& b = poly[(k + 1) % poly.size()];
            if ((a[1] <= y) != (b[1] <= y)) {
                crossings.push_back(a[0] + (y - a[1]) * (b[0] - a[0]) / (b[1] - a[1]));
            }
        }
        std::sort(crossings.begin(), crossings.end());
        for (std::size_t k = 0; k + 1 < crossings.size(); k += 2) {
            const int col0 = std::max(0, static_cast<int>(std::ceil(crossings[k] - 0.5)));
            const int col1 = std::min(grid->w - 1, static_cast<int>(std::floor(crossings[k + 1] - 0.5)));
            for (int col = col0; col <= col1; ++col) {
                grid->set(col, row);
            }
        }
    }
}

void
rasterize(Grid* grid, const Polygons& cellPolys, bool edges, bool fill)
{
    for (const std::vector<GeoUV>& poly : cellPolys) {
        if (poly.empty()) {
            continue;
        }
        if (edges) {
            for (std::size_t k = 0; k < poly.size(); ++k) {
                markSegment(grid, poly[k], poly[(k + 1) % poly.size()]);
            }
        }
        if (fill && poly.size() >= 3) {
            fillPolygon(grid, poly);
        }
    }
}

// Grows the set cells by p in x and y. With expand the result is 2p larger, cell (p, p) over
// the source's (0, 0); with outsideSet the cells beyond the source count as set.
Grid
grow(const Grid& g, int p, bool expand, bool outsideSet)
{
    const int e = expand ? p : 0;
    Grid across(g.w + 2 * e, g.h);
    std::vector<int> prefix(static_cast<std::size_t>(std::max(g.w, g.h)) + 1);
    for (int y = 0; y < g.h; ++y) {
        prefix[0] = 0;
        for (int x = 0; x < g.w; ++x) {
            prefix[x + 1] = prefix[x] + g.at(x, y);
        }
        for (int x = 0; x < across.w; ++x) {
            const int lo = x - e - p;
            const int hi = x - e + p;
            if (outsideSet && (lo < 0 || hi >= g.w)) {
                across.set(x, y);
            } else if (prefix[std::min(hi, g.w - 1) + 1] - prefix[std::max(lo, 0)] > 0) {
                across.set(x, y);
            }
        }
    }
    Grid out(across.w, g.h + 2 * e);
    for (int x = 0; x < across.w; ++x) {
        prefix[0] = 0;
        for (int y = 0; y < g.h; ++y) {
            prefix[y + 1] = prefix[y] + across.at(x, y);
        }
        for (int y = 0; y < out.h; ++y) {
            const int lo = y - e - p;
            const int hi = y - e + p;
            if (outsideSet && (lo < 0 || hi >= g.h)) {
                out.set(x, y);
            } else if ((std::min(hi, g.h - 1) >= std::max(lo, 0)) && (prefix[std::min(hi, g.h - 1) + 1] - prefix[std::max(lo, 0)] > 0)) {
                out.set(x, y);
            }
        }
    }

    return out;
}

// Fills each row and then each column between its first and last set cell, closing holes and
// hollows for packing without cavities.
void
fillSpans(Grid* g)
{
    for (int y = 0; y < g->h; ++y) {
        int first = -1;
        int last = -1;
        for (int x = 0; x < g->w; ++x) {
            if (g->at(x, y)) {
                first = first < 0 ? x : first;
                last = x;
            }
        }
        for (int x = std::max(first, 0); x <= last; ++x) {
            g->set(x, y);
        }
    }
    for (int x = 0; x < g->w; ++x) {
        int first = -1;
        int last = -1;
        for (int y = 0; y < g->h; ++y) {
            if (g->at(x, y)) {
                first = first < 0 ? y : first;
                last = y;
            }
        }
        for (int y = std::max(first, 0); y <= last; ++y) {
            g->set(x, y);
        }
    }
}

struct Run {
    int row;
    int a;
    int b;
};

std::vector<Run>
runsOf(const Grid& g, int dx, int dy)
{
    std::vector<Run> runs;
    for (int y = 0; y < g.h; ++y) {
        int x = 0;
        while (x < g.w) {
            if (!g.at(x, y)) {
                ++x;
                continue;
            }
            const int a = x;
            while (x < g.w && g.at(x, y)) {
                ++x;
            }
            runs.push_back(Run { y + dy, a + dx, x + dx });
        }
    }

    return runs;
}

// An island at one scale and rotation, rasterised for placing.
struct Shape {
    bool valid = false;
    std::array<double, 4> m = { 1., 0., 0., 1. };
    // Where cell (0, 0) of the core starts, in the transformed uv.
    GeoUV origin = { 0., 0. };
    int w = 0;
    int h = 0;
    std::vector<Run> core;
    // The core grown by the padding, relative to the core's cell (0, 0).
    std::vector<Run> padded;
};

/**
 * A target as a grid of cells, each storing how many free cells run from it to the right along
 * its row (0 when taken). A placement test then checks one number per run of the island, and a
 * failed test says how far right the next possible place is.
 **/
struct Bin {
    int id = -1;
    long long rank = 0;
    double x0 = 0.;
    double y0 = 0.;
    int w = 0;
    int h = 0;
    std::vector<std::int32_t> free;

    std::int32_t& at(int x, int y) { return free[static_cast<std::size_t>(y) * w + x]; }

    void rebuildRow(int y)
    {
        std::int32_t run = 0;
        for (int x = w - 1; x >= 0; --x) {
            std::int32_t& cell = at(x, y);
            run = cell == 0 ? 0 : run + 1;
            cell = run;
        }
    }

    void setTaken(const Grid& taken)
    {
        free.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 1);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                if (taken.at(x, y)) {
                    at(x, y) = 0;
                }
            }
            rebuildRow(y);
        }
    }

    // True when the island fits with its core's cell (0, 0) at (x, y); otherwise *next is the
    // least x at which it could fit along this row.
    bool fits(const Shape& shape, int x, int y, int* next)
    {
        for (const Run& run : shape.padded) {
            const int row = y + run.row;
            if (row < 0 || row >= h) {
                continue;
            }
            const int a = std::max(0, x + run.a);
            const int b = std::min(w, x + run.b);
            if (a >= b) {
                continue;
            }
            const std::int32_t freeCells = at(a, row);
            if (freeCells < b - a) {
                *next = a + freeCells - run.a + 1;

                return false;
            }
        }

        return true;
    }

    void take(const Shape& shape, int x, int y)
    {
        int lastRow = std::numeric_limits<int>::min();
        for (const Run& run : shape.core) {
            const int row = y + run.row;
            if (row != lastRow && lastRow != std::numeric_limits<int>::min()) {
                rebuildRow(lastRow);
            }
            lastRow = row;
            for (int c = x + run.a; c < x + run.b; ++c) {
                at(c, row) = 0;
            }
        }
        if (lastRow != std::numeric_limits<int>::min()) {
            rebuildRow(lastRow);
        }
    }
};

double
polygonsArea(const Polygons& polys)
{
    double area = 0.;
    for (const std::vector<GeoUV>& poly : polys) {
        double twice = 0.;
        for (std::size_t k = 0; k < poly.size(); ++k) {
            const GeoUV& a = poly[k];
            const GeoUV& b = poly[(k + 1) % poly.size()];
            twice += a[0] * b[1] - b[0] * a[1];
        }
        area += 0.5 * std::abs(twice);
    }

    return area;
}

bool
polygonsBounds(const Polygons& polys,
               GeoUV* lo,
               GeoUV* hi)
{
    const double inf = std::numeric_limits<double>::infinity();
    *lo = { inf, inf };
    *hi = { -inf, -inf };
    bool any = false;
    for (const std::vector<GeoUV>& poly : polys) {
        for (const GeoUV& c : poly) {
            for (int k = 0; k < 2; ++k) {
                (*lo)[k] = std::min((*lo)[k], c[k]);
                (*hi)[k] = std::max((*hi)[k], c[k]);
            }
            any = true;
        }
    }

    return any;
}

// Quarter turns are exact, so square islands stay on the grid without ulp drift.
double
snapUnit(double v)
{
    if (std::abs(v) < 1e-12) {
        return 0.;
    }
    if (std::abs(std::abs(v) - 1.) < 1e-12) {
        return v > 0. ? 1. : -1.;
    }

    return v;
}

std::array<double, 4>
multiply(const std::array<double, 4>& a,
         const std::array<double, 4>& b)
{
    return { a[0] * b[0] + a[1] * b[2], a[0] * b[1] + a[1] * b[3],
             a[2] * b[0] + a[3] * b[2], a[2] * b[1] + a[3] * b[3] };
}

GeoUV
transformUV(const std::array<double, 4>& m,
            const GeoUV& v)
{
    return { m[0] * v[0] + m[1] * v[1], m[2] * v[0] + m[3] * v[1] };
}

// The rigid map (with mirrored, a reflection first) taking b's corners onto a's, if every corner
// lands within tolerance; the fit is the least-squares rotation of the centred corners.
bool
matchIslands(const GeoUVPackIsland& a,
             const GeoUVPackIsland& b,
             bool mirrored,
             double tolerance,
             std::array<double, 4>* m,
             GeoUV* t)
{
    if (a.polygons.size() != b.polygons.size()) {
        return false;
    }
    std::vector<GeoUV> ca;
    std::vector<GeoUV> cb;
    for (std::size_t p = 0; p < a.polygons.size(); ++p) {
        if (a.polygons[p].size() != b.polygons[p].size()) {
            return false;
        }
        ca.insert(ca.end(), a.polygons[p].begin(), a.polygons[p].end());
        cb.insert(cb.end(), b.polygons[p].begin(), b.polygons[p].end());
    }
    if (ca.empty()) {
        return false;
    }
    GeoUV meanA = { 0., 0. };
    GeoUV meanB = { 0., 0. };
    for (std::size_t k = 0; k < ca.size(); ++k) {
        for (int c = 0; c < 2; ++c) {
            meanA[c] += ca[k][c] / ca.size();
            meanB[c] += cb[k][c] / cb.size();
        }
    }
    const std::array<double, 4> flip = mirrored ? std::array<double, 4> { -1., 0., 0., 1. } : std::array<double, 4> { 1., 0., 0., 1. };
    double dot = 0.;
    double cross = 0.;
    for (std::size_t k = 0; k < ca.size(); ++k) {
        const GeoUV pa = { ca[k][0] - meanA[0], ca[k][1] - meanA[1] };
        const GeoUV pb = transformUV(flip, GeoUV { cb[k][0] - meanB[0], cb[k][1] - meanB[1] });
        dot += pb[0] * pa[0] + pb[1] * pa[1];
        cross += pb[0] * pa[1] - pb[1] * pa[0];
    }
    const double angle = std::atan2(cross, dot);
    const double c = snapUnit(std::cos(angle));
    const double s = snapUnit(std::sin(angle));
    const std::array<double, 4> fit = multiply(std::array<double, 4> { c, -s, s, c }, flip);

    GeoUV lo, hi;
    polygonsBounds(a.polygons, &lo, &hi);
    const double limit = tolerance * std::hypot(hi[0] - lo[0], hi[1] - lo[1]);
    for (std::size_t k = 0; k < ca.size(); ++k) {
        const GeoUV q = transformUV(fit, GeoUV { cb[k][0] - meanB[0], cb[k][1] - meanB[1] });
        if (std::hypot(q[0] + meanA[0] - ca[k][0], q[1] + meanA[1] - ca[k][1]) > limit) {
            return false;
        }
    }
    *m = fit;
    const GeoUV moved = transformUV(fit, meanB);
    *t = { meanA[0] - moved[0], meanA[1] - moved[1] };

    return true;
}

struct Attempt {
    std::vector<GeoUVPackPlacement> placements;
    std::vector<int> unplaced;
    int packed = 0;
    // The highest cell taken, counting the targets in order, so lower means tighter.
    long long height = 0;
    // The targets' bounds, beside which unplaced islands are lined up.
    GeoUV lo = { 0., 0. };
    GeoUV hi = { 0., 0. };
    bool aborted = false;
};

class Packer {
public:
    Packer(const std::vector<GeoUVPackIsland>& islands,
           const GeoUVPackParams& params)
        : _islands(islands)
        , _params(params)
    {
    }

    bool setUp(std::string* errorMessage);

    bool poll() const
    {
        if (_stopped.load(std::memory_order_relaxed)) {
            return true;
        }
        if (_params.aborted && _params.aborted()) {
            _stopped.store(true, std::memory_order_relaxed);

            return true;
        }

        return false;
    }

    const std::vector<int>& toPack() const { return _toPack; }

    double islandArea(int i) const { return _areas[i]; }

    double targetArea() const { return _targetArea; }

    double px() const { return _px; }

    // The best of the iterations at this scale and padding.
    Attempt pack(double scale, int padding) const;

    void stack(std::vector<GeoUVPackPlacement>* placements) const;

private:
    Shape makeShape(int island, double scale, int rotation, int padding) const;
    Bin makeBin(int id, long long rank, const GeoUV& lo, const GeoUV& hi, const Polygons* area, int padding) const;
    Attempt packOnce(std::size_t iteration, const std::vector<std::vector<Shape>>& shapes, const std::vector<Bin>& baseBins, int padding) const;
    bool tryIn(Bin* bin, const std::vector<Shape>& shapes, int island, Attempt* attempt) const;

    const std::vector<GeoUVPackIsland>& _islands;
    const GeoUVPackParams& _params;
    mutable std::atomic<bool> _stopped { false };
    double _px = 1.;
    int _maxW = 1;
    int _maxH = 1;
    double _targetArea = 0.;
    std::vector<double> _areas;
    std::vector<int> _toPack;
    std::vector<int> _obstacles;
    // By island: the island it is stacked onto and the map onto that island's uv.
    std::vector<int> _leader;
    std::vector<std::array<double, 4>> _stackM;
    std::vector<GeoUV> _stackT;
};

bool
Packer::setUp(std::string* errorMessage)
{
    const GeoUVPackParams& p = _params;
    if (p.resolution < 1) {
        *errorMessage = "the search resolution must be at least 1";

        return false;
    }
    if (p.iterations < 1) {
        *errorMessage = "the iterations must be at least 1";

        return false;
    }
    if (p.rotationDivisions < 1) {
        *errorMessage = "the rotation divisions must be at least 1";

        return false;
    }
    if (p.padding < 0) {
        *errorMessage = "the padding must not be negative";

        return false;
    }
    if ((p.scaleMode == eGeoUVPackScaleFixed) && !(p.fixedScale > 0.)) {
        *errorMessage = "the fixed scale must be positive";

        return false;
    }
    if ((p.scaleMode == eGeoUVPackScaleLargest) && !(p.scaleTolerance > 0.)) {
        *errorMessage = "the scale search tolerance must be positive";

        return false;
    }

    double longest = 0.;
    switch (p.targetType) {
    case eGeoUVPackTargetRectangles:
        if (p.rectangles.empty()) {
            *errorMessage = "there are no target rectangles";

            return false;
        }
        for (const GeoUVPackRect& r : p.rectangles) {
            if (!(r.x1 > r.x0) || !(r.y1 > r.y0)) {
                *errorMessage = "a target rectangle is empty";

                return false;
            }
            longest = std::max(longest, std::max(r.x1 - r.x0, r.y1 - r.y0));
            _targetArea += (r.x1 - r.x0) * (r.y1 - r.y0);
        }
        _px = longest / p.resolution;
        for (const GeoUVPackRect& r : p.rectangles) {
            _maxW = std::max(_maxW, static_cast<int>(std::floor((r.x1 - r.x0) / _px + 1e-9)));
            _maxH = std::max(_maxH, static_cast<int>(std::floor((r.y1 - r.y0) / _px + 1e-9)));
        }
        break;
    case eGeoUVPackTargetUDIM: {
        if (!(p.tileSize > 0.) || (p.columns < 1) || (p.startingUdim < 1001)) {
            *errorMessage = "the UDIM tiles need a positive size, at least one column and a starting UDIM of at least 1001";

            return false;
        }
        _px = p.tileSize / p.resolution;
        _maxW = _maxH = p.resolution;
        // The tiles packing can reach at the largest scale: the starting one and those named.
        std::vector<int> tiles(1, p.startingUdim);
        for (const GeoUVPackIsland& island : _islands) {
            const int t = island.target >= 0 ? island.target : p.defaultTarget;
            if ((t >= 1001) && (std::find(tiles.begin(), tiles.end(), t) == tiles.end())) {
                tiles.push_back(t);
            }
        }
        _targetArea = tiles.size() * p.tileSize * p.tileSize;
        break;
    }
    case eGeoUVPackTargetIslands: {
        if (p.targetIslands.empty()) {
            *errorMessage = "there are no target islands";

            return false;
        }
        for (const GeoUVPackIsland& target : p.targetIslands) {
            GeoUV lo, hi;
            if (!polygonsBounds(target.polygons, &lo, &hi)) {
                *errorMessage = "a target island has no polygons";

                return false;
            }
            longest = std::max(longest, std::max(hi[0] - lo[0], hi[1] - lo[1]));
            _targetArea += polygonsArea(target.polygons);
        }
        if (!(longest > 0.)) {
            *errorMessage = "the target islands are empty";

            return false;
        }
        _px = longest / p.resolution;
        for (const GeoUVPackIsland& target : p.targetIslands) {
            GeoUV lo, hi;
            polygonsBounds(target.polygons, &lo, &hi);
            _maxW = std::max(_maxW, static_cast<int>(std::floor((hi[0] - lo[0]) / _px)) + 1);
            _maxH = std::max(_maxH, static_cast<int>(std::floor((hi[1] - lo[1]) / _px)) + 1);
        }
        break;
    }
    }

    const int n = static_cast<int>(_islands.size());
    _areas.resize(n);
    _leader.assign(n, -1);
    _stackM.assign(n, std::array<double, 4> { 1., 0., 0., 1. });
    _stackT.assign(n, GeoUV { 0., 0. });
    for (int i = 0; i < n; ++i) {
        const GeoUVPackIsland& island = _islands[i];
        _areas[i] = polygonsArea(island.polygons) * island.scale * island.scale;
        if (!(_areas[i] > 0.)) {
            // Degenerate islands still take cells, so count their box for the scale bound.
            GeoUV lo, hi;
            if (polygonsBounds(island.polygons, &lo, &hi)) {
                _areas[i] = std::max(hi[0] - lo[0], _px) * std::max(hi[1] - lo[1], _px) * island.scale * island.scale;
            }
        }
    }

    // Stacking: each stackable island looks for an earlier match (and with stackOnObstacles, an
    // obstacle) to sit on; the first of a kind is packed and the rest follow it.
    std::vector<int> leaders;
    if (p.stackIdentical && p.stackOnObstacles) {
        for (int i = 0; i < n; ++i) {
            if (_islands[i].obstacle && _islands[i].stackable && !_islands[i].polygons.empty()) {
                leaders.push_back(i);
            }
        }
    }
    for (int i = 0; i < n; ++i) {
        const GeoUVPackIsland& island = _islands[i];
        if (island.obstacle) {
            if (p.avoidObstacles) {
                _obstacles.push_back(i);
            }
            continue;
        }
        if (island.polygons.empty()) {
            continue;
        }
        bool stacked = false;
        if (p.stackIdentical && island.stackable) {
            for (int j : leaders) {
                if (matchIslands(_islands[j], island, false, p.stackTolerance, &_stackM[i], &_stackT[i]) || (p.stackMirrored && matchIslands(_islands[j], island, true, p.stackTolerance, &_stackM[i], &_stackT[i]))) {
                    _leader[i] = j;
                    stacked = true;
                    break;
                }
            }
            if (!stacked) {
                leaders.push_back(i);
            }
        }
        if (!stacked) {
            _toPack.push_back(i);
        }
    }

    return true;
} // Packer::setUp

Shape
Packer::makeShape(int island,
                  double scale,
                  int rotation,
                  int padding) const
{
    Shape shape;
    const GeoUVPackIsland& source = _islands[island];
    const double angle = 2. * kPi * rotation / _params.rotationDivisions;
    const double c = snapUnit(std::cos(angle));
    const double s = snapUnit(std::sin(angle));
    const double k = scale * source.scale;
    shape.m = { k * c, -k * s, k * s, k * c };

    Polygons moved = source.polygons;
    for (std::vector<GeoUV>& poly : moved) {
        for (GeoUV& corner : poly) {
            corner = transformUV(shape.m, corner);
        }
    }
    GeoUV lo, hi;
    if (!polygonsBounds(moved, &lo, &hi)) {
        return shape;
    }
    const double cellsW = std::floor((hi[0] - lo[0]) / _px) + 1.;
    const double cellsH = std::floor((hi[1] - lo[1]) / _px) + 1.;
    // Larger than every target, it cannot be placed, so skip rasterising it.
    if (!(cellsW <= _maxW) || !(cellsH <= _maxH)) {
        return shape;
    }
    shape.origin = lo;
    shape.w = static_cast<int>(cellsW);
    shape.h = static_cast<int>(cellsH);
    for (std::vector<GeoUV>& poly : moved) {
        for (GeoUV& corner : poly) {
            corner = { (corner[0] - lo[0]) / _px, (corner[1] - lo[1]) / _px };
        }
    }
    Grid core(shape.w, shape.h);
    rasterize(&core, moved, true, true);
    if (!_params.packInCavities) {
        fillSpans(&core);
    }
    shape.core = runsOf(core, 0, 0);
    shape.padded = padding > 0 ? runsOf(grow(core, padding, true, false), -padding, -padding) : shape.core;
    shape.valid = true;

    return shape;
}

// A target from its bounds (and for a target island, its polygons): the cells outside the
// polygons, within the boundary padding, or under an avoided obstacle start out taken.
Bin
Packer::makeBin(int id,
                long long rank,
                const GeoUV& lo,
                const GeoUV& hi,
                const Polygons* area,
                int padding) const
{
    Bin bin;
    bin.id = id;
    bin.rank = rank;
    bin.x0 = lo[0];
    bin.y0 = lo[1];
    auto toCells = [&](const Polygons& polys) {
        Polygons cells = polys;
        for (std::vector<GeoUV>& poly : cells) {
            for (GeoUV& corner : poly) {
                corner = { (corner[0] - bin.x0) / _px, (corner[1] - bin.y0) / _px };
            }
        }

        return cells;
    };
    Grid taken;
    if (area) {
        bin.w = static_cast<int>(std::floor((hi[0] - lo[0]) / _px)) + 1;
        bin.h = static_cast<int>(std::floor((hi[1] - lo[1]) / _px)) + 1;
        const Polygons cells = toCells(*area);
        Grid inside(bin.w, bin.h);
        rasterize(&inside, cells, false, true);
        Grid edge(bin.w, bin.h);
        rasterize(&edge, cells, true, false);
        taken = Grid(bin.w, bin.h);
        for (std::size_t k = 0; k < taken.cells.size(); ++k) {
            taken.cells[k] = !inside.cells[k] || edge.cells[k];
        }
    } else {
        bin.w = std::max(1, static_cast<int>(std::floor((hi[0] - lo[0]) / _px + 1e-9)));
        bin.h = std::max(1, static_cast<int>(std::floor((hi[1] - lo[1]) / _px + 1e-9)));
        taken = Grid(bin.w, bin.h);
    }
    if (_params.padBoundaries && padding > 0) {
        taken = grow(taken, padding, false, true);
    }
    for (int o : _obstacles) {
        rasterize(&taken, toCells(_islands[o].polygons), true, true);
    }
    bin.setTaken(taken);

    return bin;
}

bool
Packer::tryIn(Bin* bin,
              const std::vector<Shape>& shapes,
              int island,
              Attempt* attempt) const
{
    int best = -1;
    int bestX = 0;
    int bestY = 0;
    int bestTop = bin->h + 1;
    for (std::size_t r = 0; r < shapes.size(); ++r) {
        const Shape& shape = shapes[r];
        if (!shape.valid || (shape.w > bin->w) || (shape.h > bin->h)) {
            continue;
        }
        // Lowest row first, then leftmost: the first fit is this rotation's lowest top.
        bool found = false;
        for (int y = 0; y + shape.h < bestTop && y + shape.h <= bin->h && !found; ++y) {
            if (poll()) {
                return false;
            }
            int x = 0;
            while (x + shape.w <= bin->w) {
                int next = x + 1;
                if (bin->fits(shape, x, y, &next)) {
                    best = static_cast<int>(r);
                    bestX = x;
                    bestY = y;
                    bestTop = y + shape.h;
                    found = true;
                    break;
                }
                x = std::max(next, x + 1);
            }
        }
    }
    if (best < 0) {
        return false;
    }
    const Shape& shape = shapes[best];
    bin->take(shape, bestX, bestY);
    GeoUVPackPlacement& placement = attempt->placements[island];
    placement.m = shape.m;
    placement.t = { bin->x0 + bestX * _px - shape.origin[0], bin->y0 + bestY * _px - shape.origin[1] };
    placement.packed = true;
    placement.target = bin->id;
    attempt->height = std::max(attempt->height, bin->rank * (std::max(_maxH, bin->h) + 1) + bestTop);

    return true;
}

Attempt
Packer::packOnce(std::size_t iteration,
                 const std::vector<std::vector<Shape>>& shapes,
                 const std::vector<Bin>& baseBins,
                 int padding) const
{
    const GeoUVPackParams& p = _params;
    Attempt attempt;
    attempt.placements.resize(_islands.size());
    std::vector<Bin> bins = baseBins;
    std::map<int, std::size_t> tiles;
    auto tileBounds = [&](int udim, GeoUV* lo, GeoUV* hi) {
        const int k = udim - 1001;
        *lo = { (k % p.columns) * p.tileSize, (k / p.columns) * p.tileSize };
        *hi = { (*lo)[0] + p.tileSize, (*lo)[1] + p.tileSize };
    };
    auto makeTile = [&](int udim) {
        GeoUV lo, hi;
        tileBounds(udim, &lo, &hi);

        return makeBin(udim, udim - 1001, lo, hi, nullptr, padding);
    };
    auto tile = [&](int udim) -> Bin& {
        std::map<int, std::size_t>::iterator found = tiles.find(udim);
        if (found == tiles.end()) {
            found = tiles.emplace(udim, bins.size()).first;
            bins.push_back(makeTile(udim));
        }

        return bins[found->second];
    };

    std::vector<int> order = _toPack;
    if (iteration == 0) {
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return _areas[a] > _areas[b]; });
    } else {
        std::seed_seq seq { p.seed, static_cast<std::uint32_t>(iteration) };
        std::mt19937 rng(seq);
        std::shuffle(order.begin(), order.end(), rng);
    }

    for (int i : order) {
        if (poll()) {
            attempt.aborted = true;

            return attempt;
        }
        const std::vector<Shape>& islandShapes = shapes[i];
        const int target = _islands[i].target >= 0 ? _islands[i].target : p.defaultTarget;
        bool placed = false;
        if (p.targetType == eGeoUVPackTargetUDIM) {
            if (target >= 1001) {
                placed = tryIn(&tile(target), islandShapes, i, &attempt);
            } else if (p.scaleMode == eGeoUVPackScaleLargest) {
                placed = tryIn(&tile(p.startingUdim), islandShapes, i, &attempt);
            } else {
                // At a fixed scale the tiles fill in turn from the starting one, opening the next
                // when none so far has room; an island too large for an empty tile is not packed.
                for (int udim = p.startingUdim; !placed && !poll(); ++udim) {
                    std::map<int, std::size_t>::const_iterator found = tiles.find(udim);
                    if (found != tiles.end()) {
                        placed = tryIn(&bins[found->second], islandShapes, i, &attempt);
                        continue;
                    }
                    Bin fresh = makeTile(udim);
                    placed = tryIn(&fresh, islandShapes, i, &attempt);
                    if (placed) {
                        tiles.emplace(udim, bins.size());
                        bins.push_back(std::move(fresh));
                    }
                    break;
                }
            }
        } else if ((target >= 0) && (target < static_cast<int>(bins.size()))) {
            placed = tryIn(&bins[target], islandShapes, i, &attempt);
        } else {
            for (std::size_t b = 0; b < bins.size() && !placed; ++b) {
                placed = tryIn(&bins[b], islandShapes, i, &attempt);
            }
        }
        if (poll()) {
            attempt.aborted = true;

            return attempt;
        }
        if (placed) {
            ++attempt.packed;
        } else {
            attempt.unplaced.push_back(i);
        }
    }

    if (bins.empty()) {
        tileBounds(p.startingUdim, &attempt.lo, &attempt.hi);
    } else {
        const double inf = std::numeric_limits<double>::infinity();
        attempt.lo = { inf, inf };
        attempt.hi = { -inf, -inf };
        for (const Bin& bin : bins) {
            attempt.lo = { std::min(attempt.lo[0], bin.x0), std::min(attempt.lo[1], bin.y0) };
            attempt.hi = { std::max(attempt.hi[0], bin.x0 + bin.w * _px), std::max(attempt.hi[1], bin.y0 + bin.h * _px) };
        }
    }

    return attempt;
} // Packer::packOnce

Attempt
Packer::pack(double scale,
             int padding) const
{
    const GeoUVPackParams& p = _params;
    std::vector<std::vector<Shape>> shapes(_islands.size());
    geoParallelFor(_toPack.size(), [&](std::size_t k) {
        const int i = _toPack[k];
        shapes[i].resize(p.rotationDivisions);
        for (int r = 0; r < p.rotationDivisions && !poll(); ++r) {
            shapes[i][r] = makeShape(i, scale, r, padding);
        } }, 8);
    if (poll()) {
        Attempt aborted;
        aborted.aborted = true;

        return aborted;
    }

    std::vector<Bin> baseBins;
    if (p.targetType == eGeoUVPackTargetRectangles) {
        for (std::size_t r = 0; r < p.rectangles.size(); ++r) {
            const GeoUVPackRect& rect = p.rectangles[r];
            baseBins.push_back(makeBin(static_cast<int>(r), static_cast<long long>(r), GeoUV { rect.x0, rect.y0 }, GeoUV { rect.x1, rect.y1 }, nullptr, padding));
        }
    } else if (p.targetType == eGeoUVPackTargetIslands) {
        for (std::size_t t = 0; t < p.targetIslands.size(); ++t) {
            GeoUV lo, hi;
            polygonsBounds(p.targetIslands[t].polygons, &lo, &hi);
            baseBins.push_back(makeBin(static_cast<int>(t), static_cast<long long>(t), lo, hi, &p.targetIslands[t].polygons, padding));
        }
    }

    std::vector<Attempt> attempts(p.iterations);
    geoParallelFor(attempts.size(), [&](std::size_t k) { attempts[k] = packOnce(k, shapes, baseBins, padding); }, 2);

    std::size_t best = 0;
    for (std::size_t k = 0; k < attempts.size(); ++k) {
        if (attempts[k].aborted) {
            return attempts[k];
        }
        if ((attempts[k].packed > attempts[best].packed) || ((attempts[k].packed == attempts[best].packed) && (attempts[k].height < attempts[best].height))) {
            best = k;
        }
    }

    return std::move(attempts[best]);
} // Packer::pack

void
Packer::stack(std::vector<GeoUVPackPlacement>* placements) const
{
    for (std::size_t i = 0; i < _leader.size(); ++i) {
        const int j = _leader[i];
        if (j < 0) {
            continue;
        }
        const GeoUVPackPlacement& leader = (*placements)[j];
        GeoUVPackPlacement& follower = (*placements)[i];
        follower.m = multiply(leader.m, _stackM[i]);
        const GeoUV moved = transformUV(leader.m, _stackT[i]);
        follower.t = { moved[0] + leader.t[0], moved[1] + leader.t[1] };
        // On an obstacle it stays with that island, which is where it already was.
        follower.packed = _islands[j].obstacle ? true : leader.packed;
        follower.target = leader.target;
        follower.stackedOn = j;
    }
}

} // anon namespace

GeoUV
geoUVPackApply(const GeoUVPackPlacement& placement,
               const GeoUV& uv)
{
    const GeoUV moved = transformUV(placement.m, uv);

    return { moved[0] + placement.t[0], moved[1] + placement.t[1] };
}

bool
geoPackUVIslands(const std::vector<GeoUVPackIsland>& islands,
                 const GeoUVPackParams& params,
                 GeoUVPackResult* result,
                 std::string* errorMessage)
{
    *result = GeoUVPackResult();
    result->placements.resize(islands.size());
    Packer packer(islands, params);
    if (!packer.setUp(errorMessage)) {
        return false;
    }
    const std::size_t wanted = packer.toPack().size();
    auto stopped = [&]() {
        result->aborted = true;

        return false;
    };

    double scale = params.fixedScale;
    Attempt best;
    best.placements.resize(islands.size());
    if (wanted == 0) {
        scale = params.scaleMode == eGeoUVPackScaleFixed ? params.fixedScale : 1.;
    } else if (params.scaleMode == eGeoUVPackScaleFixed) {
        best = packer.pack(scale, params.padding);
        if (best.aborted) {
            return stopped();
        }
    } else {
        // No packing covers more than the targets' area, which bounds the scale from above;
        // halve until everything fits, then bisect between the two.
        double area = 0.;
        for (int i : packer.toPack()) {
            area += packer.islandArea(i);
        }
        double hi = area > 0. ? std::sqrt(packer.targetArea() / area) : 1.;
        Attempt attempt = packer.pack(hi, params.padding);
        if (attempt.aborted) {
            return stopped();
        }
        if (attempt.packed == static_cast<int>(wanted)) {
            scale = hi;
            best = std::move(attempt);
        } else {
            double lo = hi;
            bool found = false;
            for (int halvings = 0; halvings < 60 && !found; ++halvings) {
                hi = lo;
                lo *= 0.5;
                attempt = packer.pack(lo, params.padding);
                if (attempt.aborted) {
                    return stopped();
                }
                found = attempt.packed == static_cast<int>(wanted);
                if (found || (halvings == 59)) {
                    best = std::move(attempt);
                }
            }
            scale = lo;
            while (found && (hi - lo > params.scaleTolerance * lo)) {
                const double mid = 0.5 * (lo + hi);
                attempt = packer.pack(mid, params.padding);
                if (attempt.aborted) {
                    return stopped();
                }
                if (attempt.packed == static_cast<int>(wanted)) {
                    lo = mid;
                    best = std::move(attempt);
                } else {
                    hi = mid;
                }
            }
            scale = lo;
        }
    }

    int padding = params.padding;
    if (params.spread && wanted > 0) {
        // The widest padding that still packs as many islands at this scale.
        int lo = padding;
        int hi = -1;
        for (int step = std::max(1, padding); lo + step <= params.resolution; step *= 2) {
            Attempt attempt = packer.pack(scale, lo + step);
            if (attempt.aborted) {
                return stopped();
            }
            if (attempt.packed >= best.packed) {
                lo += step;
                best = std::move(attempt);
            } else {
                hi = lo + step;
                break;
            }
        }
        while (hi > lo + 1) {
            const int mid = lo + (hi - lo) / 2;
            Attempt attempt = packer.pack(scale, mid);
            if (attempt.aborted) {
                return stopped();
            }
            if (attempt.packed >= best.packed) {
                lo = mid;
                best = std::move(attempt);
            } else {
                hi = mid;
            }
        }
        padding = lo;
    }

    // Islands that did not fit are lined up in a row to the right of the targets.
    const double gap = std::max(1, padding) * packer.px();
    double cursor = best.hi[0] + gap;
    std::sort(best.unplaced.begin(), best.unplaced.end());
    for (int i : best.unplaced) {
        GeoUVPackPlacement& placement = best.placements[i];
        const double k = scale * islands[i].scale;
        placement.m = { k, 0., 0., k };
        GeoUV lo, hi;
        polygonsBounds(islands[i].polygons, &lo, &hi);
        placement.t = { cursor - k * lo[0], best.lo[1] - k * lo[1] };
        placement.packed = false;
        placement.target = -1;
        cursor += k * (hi[0] - lo[0]) + gap;
    }
    packer.stack(&best.placements);

    result->placements = std::move(best.placements);
    result->scale = scale;
    result->padding = padding;
    for (const GeoUVPackPlacement& placement : result->placements) {
        if ((params.targetType == eGeoUVPackTargetUDIM) && placement.packed) {
            result->maxUdim = std::max(result->maxUdim, placement.target);
        }
    }
    for (std::size_t i = 0; i < islands.size(); ++i) {
        if (!islands[i].obstacle && !islands[i].polygons.empty() && !result->placements[i].packed) {
            ++result->nonpacked;
        }
    }

    return true;
} // geoPackUVIslands

NATRON_NAMESPACE_EXIT
