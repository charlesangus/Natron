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

#include "OverlayPointMap.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "Engine/EffectInstance.h"
#include "Engine/Transform.h"

NATRON_NAMESPACE_ENTER

namespace OverlayPointMap {
namespace {
    const int kMaxSubdivisionDepth = 8;
    const int kMaxLineSearchHalvings = 10;

    bool
    isFinite(const Point& p)
    {
        return std::isfinite(p.x) && std::isfinite(p.y);
    }

    double
    distance(const Point& a,
             const Point& b)
    {
        return std::hypot(a.x - b.x, a.y - b.y);
    }

    Point
    midpoint(const Point& a,
             const Point& b)
    {
        Point m;

        m.x = 0.5 * (a.x + b.x);
        m.y = 0.5 * (a.y + b.y);

        return m;
    }

    Result
    residual(const PointFn& fn,
             const Point& p,
             const Point& target,
             Point* r)
    {
        Point mapped = p;
        const Result result = fn(p, &mapped);

        if (!isUsable(result)) {
            return result;
        }
        if (!isFinite(mapped)) {
            return eUnmappable;
        }
        r->x = mapped.x - target.x;
        r->y = mapped.y - target.y;

        return eMapped;
    }

    Result
    newton(const PointFn& fn,
           const Point& target,
           const Point& seed,
           const InvertOptions& options,
           Point* out)
    {
        Point p = seed;
        Point r;
        Result result = residual(fn, p, target, &r);

        if (!isUsable(result)) {
            return result;
        }
        double norm = std::hypot(r.x, r.y);
        for (int iteration = 0; iteration <= options.maxIterations; ++iteration) {
            if (norm <= options.tolerance) {
                *out = p;

                return eMapped;
            }
            if (iteration == options.maxIterations) {
                break;
            }
            // The step scales with the point so the difference stays well above rounding at large
            // coordinates and well inside the local linear region at small ones.
            const double h = 1e-4 * std::max(1., std::max(std::fabs(p.x), std::fabs(p.y)));
            Point px0 = p, px1 = p, py0 = p, py1 = p;
            px0.x -= h;
            px1.x += h;
            py0.y -= h;
            py1.y += h;
            Point rx0, rx1, ry0, ry1;
            if ((residual(fn, px0, target, &rx0) != eMapped) || (residual(fn, px1, target, &rx1) != eMapped) || (residual(fn, py0, target, &ry0) != eMapped) || (residual(fn, py1, target, &ry1) != eMapped)) {
                return eUnmappable;
            }
            const double j00 = (rx1.x - rx0.x) / (2. * h);
            const double j10 = (rx1.y - rx0.y) / (2. * h);
            const double j01 = (ry1.x - ry0.x) / (2. * h);
            const double j11 = (ry1.y - ry0.y) / (2. * h);
            const double det = j00 * j11 - j01 * j10;
            if (!std::isfinite(det) || (std::fabs(det) < 1e-14)) {
                return eUnmappable;
            }
            const double dx = -(j11 * r.x - j01 * r.y) / det;
            const double dy = -(-j10 * r.x + j00 * r.y) / det;
            double lambda = 1.;
            bool improved = false;
            for (int halving = 0; halving < kMaxLineSearchHalvings; ++halving, lambda *= 0.5) {
                Point trial = p;
                trial.x += lambda * dx;
                trial.y += lambda * dy;
                Point rt;
                if (residual(fn, trial, target, &rt) != eMapped) {
                    continue;
                }
                const double trialNorm = std::hypot(rt.x, rt.y);
                if (trialNorm < norm) {
                    p = trial;
                    r = rt;
                    norm = trialNorm;
                    improved = true;
                    break;
                }
            }
            if (!improved) {
                break;
            }
        }

        return eUnmappable;
    } // newton

    struct Sample {
        Point source;
        Point mapped;
        Result result = eUnmappable;
    };

    class PolylineBuilder {
    public:
        PolylineBuilder(const SeededPointFn& fn,
                        double tolerance,
                        std::vector<std::vector<Point>>* runs)
            : _fn(fn)
            , _tolerance(tolerance)
            , _runs(runs)
        {
        }

        Sample sample(const Point& p,
                      const Point& seed)
        {
            Sample s;

            s.source = p;
            s.mapped = seed;
            s.result = _fn(p, seed, &s.mapped);
            if (isUsable(s.result) && !isFinite(s.mapped)) {
                s.result = eUnmappable;
            }
            if (s.result == ePending) {
                _pending = true;
            }
            if (s.result == eMapped) {
                _anyMapped = true;
            }

            return s;
        }

        void emit(const Sample& s)
        {
            if (!isUsable(s.result)) {
                closeRun();

                return;
            }
            _current.push_back(s.mapped);
        }

        void closeRun()
        {
            if (_current.size() >= 2) {
                _runs->push_back(_current);
            }
            _current.clear();
        }

        // Emits the points after a, up to and including b.
        void subdivide(const Sample& a,
                       const Sample& b,
                       int depth)
        {
            if (depth >= kMaxSubdivisionDepth) {
                emit(b);

                return;
            }
            const bool aOk = isUsable(a.result);
            const bool bOk = isUsable(b.result);
            const Point seed = aOk ? a.mapped : (bOk ? b.mapped : midpoint(a.source, b.source));
            const Sample m = sample(midpoint(a.source, b.source), seed);
            const bool mOk = isUsable(m.result);
            if (aOk && bOk && mOk) {
                if (distance(m.mapped, midpoint(a.mapped, b.mapped)) <= _tolerance) {
                    emit(b);

                    return;
                }
            } else if (!aOk && !bOk && !mOk) {
                emit(b);

                return;
            }
            subdivide(a, m, depth + 1);
            subdivide(m, b, depth + 1);
        }

        bool pending() const
        {
            return _pending;
        }

        bool anyMapped() const
        {
            return _anyMapped;
        }

    private:
        const SeededPointFn& _fn;
        const double _tolerance;
        std::vector<std::vector<Point>>* _runs;
        std::vector<Point> _current;
        bool _pending = false;
        bool _anyMapped = false;
    };
} // anon namespace

Direction
opposite(Direction direction)
{
    return (direction == eDirectionForward) ? eDirectionInverse : eDirectionForward;
}

Result
applyMatrix(const Transform::Matrix3x3& m,
            const Point& in,
            Point* out)
{
    const Transform::Point3D p = Transform::matApply(m, Transform::Point3D(in.x, in.y, 1.));

    if (!std::isfinite(p.z) || (std::fabs(p.z) < 1e-12)) {
        return eUnmappable;
    }
    Point result;
    result.x = p.x / p.z;
    result.y = p.y / p.z;
    if (!isFinite(result)) {
        return eUnmappable;
    }
    *out = result;

    return eMapped;
}

Result
invertNumerically(const PointFn& fn,
                  const Point& target,
                  const Point& seed,
                  Point* out,
                  const InvertOptions& options)
{
    if (!isFinite(target)) {
        return eUnmappable;
    }
    const Point start = isFinite(seed) ? seed : target;
    const Result first = newton(fn, target, start, options, out);
    if ((first == eMapped) || (first == ePending)) {
        return first;
    }
    if (options.seedRegion.isNull() || !(options.seedStep > 0.)) {
        return eUnmappable;
    }
    double best = std::numeric_limits<double>::infinity();
    Point bestSeed = start;
    const std::vector<Point> grid = sampleGrid(options.seedRegion, options.seedStep);
    for (const Point& q : grid) {
        Point r;
        const Result result = residual(fn, q, target, &r);
        if (result == ePending) {
            return ePending;
        }
        if (result != eMapped) {
            continue;
        }
        const double norm = std::hypot(r.x, r.y);
        if (norm < best) {
            best = norm;
            bestSeed = q;
        }
    }
    if (!std::isfinite(best)) {
        return eUnmappable;
    }

    return newton(fn, target, bestSeed, options, out);
} // invertNumerically

std::vector<Point>
sampleGrid(const RectD& region,
           double step)
{
    std::vector<Point> points;

    if (region.isNull() || !(step > 0.) || !std::isfinite(region.x1) || !std::isfinite(region.x2) || !std::isfinite(region.y1) || !std::isfinite(region.y2)) {
        return points;
    }
    const int nx = std::max(1, (int)std::ceil(region.width() / step)) + 1;
    const int ny = std::max(1, (int)std::ceil(region.height() / step)) + 1;
    points.reserve((std::size_t)nx * (std::size_t)ny);
    for (int j = 0; j < ny; ++j) {
        Point p;
        p.y = std::min(region.y2, region.y1 + j * step);
        for (int i = 0; i < nx; ++i) {
            p.x = std::min(region.x2, region.x1 + i * step);
            points.push_back(p);
        }
    }

    return points;
}

Result
mapPolyline(const SeededPointFn& fn,
            const std::vector<Point>& points,
            bool closed,
            double tolerancePx,
            double viewportScale,
            std::vector<std::vector<Point>>* runs)
{
    runs->clear();
    if (points.empty()) {
        return eUnmappable;
    }
    const double scale = (viewportScale > 0.) ? viewportScale : 1.;
    const double tolerance = std::max(tolerancePx, 1e-6) / scale;
    PolylineBuilder builder(fn, tolerance, runs);
    Sample previous = builder.sample(points[0], points[0]);
    const Sample first = previous;
    builder.emit(previous);
    for (std::size_t i = 1; i < points.size(); ++i) {
        const Point seed = isUsable(previous.result) ? previous.mapped : points[i];
        const Sample next = builder.sample(points[i], seed);
        builder.subdivide(previous, next, 0);
        previous = next;
    }
    if (closed && (points.size() > 2)) {
        builder.subdivide(previous, first, 0);
    }
    builder.closeRun();
    if (builder.pending()) {
        runs->clear();

        return ePending;
    }
    if (runs->empty()) {
        return eUnmappable;
    }

    return builder.anyMapped() ? eMapped : eIdentity;
} // mapPolyline

SeedGrid::SeedGrid(const RectD& region,
                   int nx,
                   int ny)
    : _region(region)
    , _nx(std::max(1, nx))
    , _ny(std::max(1, ny))
    , _values((std::size_t)_nx * (std::size_t)_ny)
    , _distances((std::size_t)_nx * (std::size_t)_ny, std::numeric_limits<double>::infinity())
{
}

bool
SeedGrid::cellOf(const Point& at,
                 bool clamp,
                 int* i,
                 int* j) const
{
    if (!isFinite(at) || _region.isNull()) {
        return false;
    }
    const double fx = (at.x - _region.x1) / _region.width() * _nx;
    const double fy = (at.y - _region.y1) / _region.height() * _ny;
    if (!clamp && ((fx < 0.) || (fy < 0.) || (fx >= _nx) || (fy >= _ny))) {
        return false;
    }
    *i = (int)std::min((double)(_nx - 1), std::max(0., std::floor(fx)));
    *j = (int)std::min((double)(_ny - 1), std::max(0., std::floor(fy)));

    return true;
}

void
SeedGrid::add(const Point& at,
              const Point& value)
{
    int i = 0, j = 0;

    if (!isFinite(value) || !cellOf(at, false, &i, &j)) {
        return;
    }
    Point centre;
    centre.x = _region.x1 + (i + 0.5) * _region.width() / _nx;
    centre.y = _region.y1 + (j + 0.5) * _region.height() / _ny;
    const double d = distance(at, centre);
    const std::size_t index = (std::size_t)j * _nx + i;
    if (d < _distances[index]) {
        _distances[index] = d;
        _values[index] = value;
        _any = true;
    }
}

void
SeedGrid::fillHoles()
{
    if (!_any) {
        return;
    }
    std::vector<std::size_t> frontier;
    for (std::size_t index = 0; index < _distances.size(); ++index) {
        if (std::isfinite(_distances[index])) {
            frontier.push_back(index);
        }
    }
    while (!frontier.empty()) {
        std::vector<std::size_t> next;
        for (std::size_t index : frontier) {
            const int i = (int)(index % _nx);
            const int j = (int)(index / _nx);
            const int neighbours[4][2] = { { i - 1, j }, { i + 1, j }, { i, j - 1 }, { i, j + 1 } };
            for (const int* n : neighbours) {
                if ((n[0] < 0) || (n[1] < 0) || (n[0] >= _nx) || (n[1] >= _ny)) {
                    continue;
                }
                const std::size_t ni = (std::size_t)n[1] * _nx + n[0];
                if (std::isfinite(_distances[ni])) {
                    continue;
                }
                // Filled cells stay behind every sample that lands in them later.
                _distances[ni] = std::numeric_limits<double>::max();
                _values[ni] = _values[index];
                next.push_back(ni);
            }
        }
        frontier.swap(next);
    }
}

bool
SeedGrid::lookup(const Point& at,
                 Point* value) const
{
    int i = 0, j = 0;

    if (!_any || !cellOf(at, true, &i, &j)) {
        return false;
    }
    const std::size_t index = (std::size_t)j * _nx + i;
    if (!std::isfinite(_distances[index])) {
        return false;
    }
    *value = _values[index];

    return true;
}

void
OverlayMapChain::append(const EffectInstancePtr& effect,
                        int inputNb,
                        Direction direction)
{
    Leg leg;

    leg.effect = effect;
    leg.inputNb = inputNb;
    leg.direction = direction;
    _legs.push_back(leg);
}

OverlayMapChain
OverlayMapChain::inverted() const
{
    OverlayMapChain ret;

    for (std::vector<Leg>::const_reverse_iterator it = _legs.rbegin(); it != _legs.rend(); ++it) {
        Leg leg = *it;
        leg.direction = opposite(leg.direction);
        ret._legs.push_back(leg);
    }

    return ret;
}

Result
OverlayMapChain::mapPoint(double time,
                          ViewIdx view,
                          const Point& in,
                          Point* out,
                          std::vector<Point>* seeds) const
{
    Point p = in;
    bool pending = false;
    bool mapped = false;

    if (seeds && (seeds->size() != _legs.size())) {
        seeds->assign(_legs.size(), in);
    }
    for (std::size_t i = 0; i < _legs.size(); ++i) {
        const Leg& leg = _legs[i];
        EffectInstancePtr effect = leg.effect.lock();
        if (!effect) {
            return eUnmappable;
        }
        Point next = p;
        const Point* seed = seeds ? &(*seeds)[i] : 0;
        const Result result = effect->mapOverlayPoint_public(time, view, leg.inputNb, leg.direction, p, &next, seed);
        switch (result) {
        case eUnmappable:

            return eUnmappable;
        case ePending:
            pending = true;
            next = p;
            break;
        case eIdentity:
            next = p;
            break;
        case eMapped:
            mapped = true;
            break;
        }
        if (seeds) {
            (*seeds)[i] = next;
        }
        p = next;
    }
    if (pending) {
        return ePending;
    }
    *out = p;

    return mapped ? eMapped : eIdentity;
} // OverlayMapChain::mapPoint

Result
OverlayMapChain::mapPolyline(double time,
                             ViewIdx view,
                             const std::vector<Point>& points,
                             bool closed,
                             double tolerancePx,
                             double viewportScale,
                             std::vector<std::vector<Point>>* runs) const
{
    // Samples arrive in nearly curve order, so each leg's last answer is a close seed for the next
    // sample's numeric inverse.
    std::vector<Point> seeds;
    const SeededPointFn fn = [&](const Point& in, const Point& /*seed*/, Point* out) {
        std::vector<Point> trial = seeds;
        const Result result = mapPoint(time, view, in, out, &trial);
        if (isUsable(result)) {
            seeds.swap(trial);
        }

        return result;
    };

    return OverlayPointMap::mapPolyline(fn, points, closed, tolerancePx, viewportScale, runs);
}
} // namespace OverlayPointMap

NATRON_NAMESPACE_EXIT
