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

#include "SplineWarpSampling.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <list>
#include <unordered_map>

#include "Engine/Bezier.h"
#include "Engine/Hash64.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpContext.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpCurve.h"

NATRON_NAMESPACE_ENTER

namespace {
// Polyline points per Bezier segment for boundary outlines.
const int kBoundaryPointsPerSegment = 16;

Point
makePoint(double x,
          double y)
{
    Point p;

    p.x = x;
    p.y = y;

    return p;
}

// A walk along a curve by arc length: from startArc, forward or backward, wrapping around a
// closed curve and clamped to an open one.
struct Walk {
    const SplineWarpCurveShape* shape;
    double length;
    double startArc;
    bool reversed;
    bool closed;

    Walk()
        : shape(0)
        , length(0.)
        , startArc(0.)
        , reversed(false)
        , closed(false)
    {
    }

    double wrap(double s) const
    {
        if (closed) {
            s = std::fmod(s, length);
            if (s < 0.) {
                s += length;
            }

            return s;
        }

        return std::max(0., std::min(s, length));
    }

    Point pointAtWalk(double w) const
    {
        if (!(length > 0.)) {
            return shape->pointAt(0.);
        }
        const double s = wrap(reversed ? startArc - w : startArc + w);

        return shape->pointAt(shape->paramAtArcLength(s));
    }

    // The walk position of parameter u, in [0, length).
    double walkOfParam(double u) const
    {
        if (!(length > 0.)) {
            return 0.;
        }
        const double s = shape->arcLengthAtParam(u);
        double w = reversed ? startArc - s : s - startArc;
        if (closed) {
            w = std::fmod(w, length);
            if (w < 0.) {
                w += length;
            }
        }

        return std::max(0., std::min(w, length));
    }
};

struct Anchor {
    double source;
    double destination;
};

struct SpanPlan {
    Anchor from;
    Anchor to;
    int count;
};

struct LinkPlan {
    const SplineWarpLinkSamplingInput* input;
    Walk source;
    Walk destination;
    std::vector<SpanPlan> spans;
    // Pairs the two walks' ends too: not when two closed curves pair lap with lap.
    bool includeEnd;
};

struct BoundaryPlan {
    SplineWarpBoundaryShape shape;
    std::vector<double> cumulative;
    int count;
};

int
countFor(double length,
         double spacing)
{
    if (!(length > 0.)) {
        return 1;
    }
    const double n = std::ceil(length / spacing);

    return (int)std::max(1., std::min(n, 1e7));
}

double
signedArea(const std::vector<Point>& polygon)
{
    double a = 0.;
    const std::size_t n = polygon.size();

    for (std::size_t i = 0; i < n; ++i) {
        const Point& p = polygon[i];
        const Point& q = polygon[(i + 1) % n];
        a += p.x * q.y - q.x * p.y;
    }

    return a * 0.5;
}

// Moves each vertex of the closed polygon outward by distance along its normal, as the Roto
// renderer expands a feather by its feather distance.
std::vector<Point>
expandOutward(const std::vector<Point>& polygon,
              double distance)
{
    const std::size_t n = polygon.size();

    if ((n < 3) || !(distance > 0.)) {
        return polygon;
    }
    const bool ccw = signedArea(polygon) > 0.;
    std::vector<Point> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        const Point& prev = polygon[(i + n - 1) % n];
        const Point& next = polygon[(i + 1) % n];
        const double dx = next.x - prev.x;
        const double dy = next.y - prev.y;
        const double norm = std::hypot(dx, dy);
        out[i] = polygon[i];
        if (norm > 0.) {
            const double nx = ccw ? dy / norm : -dy / norm;
            const double ny = ccw ? -dx / norm : dx / norm;
            out[i].x += nx * distance;
            out[i].y += ny * distance;
        }
    }

    return out;
}

std::vector<double>
closedCumulative(const std::vector<Point>& polygon)
{
    const std::size_t n = polygon.size();
    std::vector<double> c(n + 1, 0.);

    for (std::size_t i = 0; i < n; ++i) {
        const Point& p = polygon[i];
        const Point& q = polygon[(i + 1) % n];
        c[i + 1] = c[i] + std::hypot(q.x - p.x, q.y - p.y);
    }

    return c;
}

Point
pointAtClosedArc(const std::vector<Point>& polygon,
                 const std::vector<double>& cumulative,
                 double s)
{
    const std::size_t n = polygon.size();
    std::vector<double>::const_iterator it = std::upper_bound(cumulative.begin(), cumulative.end(), s);
    std::size_t hi = (std::size_t)(it - cumulative.begin());

    if (hi == 0) {
        return polygon[0];
    }
    if (hi > n) {
        hi = n;
    }
    const std::size_t lo = hi - 1;
    const double span = cumulative[hi] - cumulative[lo];
    const double f = (span > 0.) ? (s - cumulative[lo]) / span : 0.;
    const Point& p = polygon[lo];
    const Point& q = polygon[hi % n];

    return makePoint(p.x + f * (q.x - p.x), p.y + f * (q.y - p.y));
}

LinkPlan
planLink(const SplineWarpLinkSamplingInput& link,
         double spacing)
{
    LinkPlan plan;

    plan.input = &link;
    plan.source.shape = link.source;
    plan.source.length = link.source->getLength();
    plan.source.closed = link.source->isClosed() && (plan.source.length > 0.);
    plan.destination.shape = link.destination;
    plan.destination.length = link.destination->getLength();
    plan.destination.closed = link.destination->isClosed() && (plan.destination.length > 0.);
    plan.destination.reversed = link.reversed;
    if (plan.destination.closed) {
        plan.destination.startArc = link.destination->arcLengthAtParam(link.startOffset);
    } else {
        plan.destination.startArc = link.reversed ? plan.destination.length : 0.;
    }

    const double ls = plan.source.length;
    const double ld = plan.destination.length;
    plan.includeEnd = !(plan.source.closed && plan.destination.closed);

    if (!(ls > 0.) && !(ld > 0.)) {
        // Point to point.
        SpanPlan span;
        span.from.source = span.from.destination = 0.;
        span.to = span.from;
        span.count = 1;
        plan.spans.push_back(span);
        plan.includeEnd = false;

        return plan;
    }

    std::vector<Anchor> anchors;
    const double eps = 1e-9 * std::max(1., std::max(ls, ld));
    for (std::size_t i = 0; i < link.correspondences.size(); ++i) {
        Anchor a;
        a.source = plan.source.walkOfParam(link.correspondences[i].first);
        a.destination = plan.destination.walkOfParam(link.correspondences[i].second);
        // A correspondence on the walk's start or end is the end anchor itself.
        if ((a.source <= eps) || (a.source >= ls - eps)) {
            continue;
        }
        anchors.push_back(a);
    }
    std::sort(anchors.begin(), anchors.end(), [](const Anchor& a, const Anchor& b) {
        return a.source < b.source;
    });

    Anchor start;
    start.source = 0.;
    start.destination = 0.;
    Anchor end;
    end.source = ls;
    end.destination = ld;
    anchors.insert(anchors.begin(), start);
    anchors.push_back(end);

    for (std::size_t i = 0; i + 1 < anchors.size(); ++i) {
        SpanPlan span;
        span.from = anchors[i];
        span.to = anchors[i + 1];
        if ((span.to.source - span.from.source <= eps) && (std::fabs(span.to.destination - span.from.destination) <= eps)) {
            continue;
        }
        span.count = countFor(std::max(span.to.source - span.from.source, std::fabs(span.to.destination - span.from.destination)), spacing);
        plan.spans.push_back(span);
    }

    return plan;
} // planLink

void
emitLinkPairs(const LinkPlan& plan,
              double amount,
              bool invert,
              std::vector<SplineWarpSamplePair>* pairs)
{
    std::vector<std::pair<Point, Point>> raw;

    for (std::size_t i = 0; i < plan.spans.size(); ++i) {
        const SpanPlan& span = plan.spans[i];
        for (int k = 0; k < span.count; ++k) {
            const double f = (double)k / span.count;
            const double ws = span.from.source + f * (span.to.source - span.from.source);
            const double wd = span.from.destination + f * (span.to.destination - span.from.destination);
            raw.push_back(std::make_pair(plan.destination.pointAtWalk(wd), plan.source.pointAtWalk(ws)));
        }
    }
    if (plan.includeEnd && !plan.spans.empty()) {
        raw.push_back(std::make_pair(plan.destination.pointAtWalk(plan.destination.length), plan.source.pointAtWalk(plan.source.length)));
    }

    for (std::size_t i = 0; i < raw.size(); ++i) {
        Point d = raw[i].first;
        Point s = raw[i].second;
        if (invert) {
            std::swap(d, s);
        }
        SplineWarpSamplePair pair;
        pair.source = s;
        pair.destination = makePoint(s.x + amount * (d.x - s.x), s.y + amount * (d.y - s.y));
        pairs->push_back(pair);
    }
}

// Merges pairs whose destinations are within kSplineWarpMergeDistance, averaging their sources.
// Returns the number of merged pairs taken from the first firstCount; remap receives each pair's
// merged index, -1 for a dropped pair.
int
mergePairs(std::vector<SplineWarpSamplePair>* pairs,
           int firstCount,
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
    merged.reserve(pairs->size());
    counts.reserve(pairs->size());
    int mergedFirst = 0;
    remap->assign(pairs->size(), -1);

    for (std::size_t i = 0; i < pairs->size(); ++i) {
        const SplineWarpSamplePair& p = (*pairs)[i];
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
            // merged[found].source holds the running sum until the end.
            merged[found].source.x += p.source.x;
            merged[found].source.y += p.source.y;
            ++counts[found];
            (*remap)[i] = found;
        } else {
            (*remap)[i] = (int)merged.size();
            grid[c].push_back((int)merged.size());
            merged.push_back(p);
            counts.push_back(1);
            if ((int)i < firstCount) {
                ++mergedFirst;
            }
        }
    }
    for (std::size_t i = 0; i < merged.size(); ++i) {
        merged[i].source.x /= counts[i];
        merged[i].source.y /= counts[i];
    }
    pairs->swap(merged);

    return mergedFirst;
} // mergePairs

// The merged indices of the raw pairs first to last - 1, without repeats between neighbours.
SplineWarpSamplePolyline
mergedPolyline(const std::vector<int>& remap,
               std::size_t first,
               std::size_t last,
               bool closed)
{
    SplineWarpSamplePolyline polyline;

    for (std::size_t i = first; i < last; ++i) {
        const int index = remap[i];
        if ((index >= 0) && (polyline.pairs.empty() || (polyline.pairs.back() != index))) {
            polyline.pairs.push_back(index);
        }
    }
    if (closed && (polyline.pairs.size() > 1) && (polyline.pairs.back() == polyline.pairs.front())) {
        polyline.pairs.pop_back();
    }
    polyline.closed = closed && (polyline.pairs.size() > 2);

    return polyline;
}
} // anonymous namespace

bool
splineWarpPointInPolygon(const std::vector<Point>& polygon,
                         double x,
                         double y)
{
    bool inside = false;
    const std::size_t n = polygon.size();

    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const Point& a = polygon[i];
        const Point& b = polygon[j];
        if ((a.y > y) != (b.y > y)) {
            const double xi = a.x + (y - a.y) * (b.x - a.x) / (b.y - a.y);
            if (x < xi) {
                inside = !inside;
            }
        }
    }

    return inside;
}

double
splineWarpDistanceToPolyline(const std::vector<Point>& polyline,
                             double x,
                             double y)
{
    const std::size_t n = polyline.size();

    if (n == 0) {
        return std::numeric_limits<double>::infinity();
    }
    if (n == 1) {
        return std::hypot(x - polyline[0].x, y - polyline[0].y);
    }
    double best2 = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < n; ++i) {
        const Point& a = polyline[i];
        const Point& b = polyline[(i + 1) % n];
        const double ex = b.x - a.x;
        const double ey = b.y - a.y;
        const double len2 = ex * ex + ey * ey;
        double t = (len2 > 0.) ? ((x - a.x) * ex + (y - a.y) * ey) / len2 : 0.;
        t = std::max(0., std::min(1., t));
        const double dx = a.x + t * ex - x;
        const double dy = a.y + t * ey - y;
        best2 = std::min(best2, dx * dx + dy * dy);
    }

    return std::sqrt(best2);
}

void
splineWarpPolygonRowCrossings(const std::vector<Point>& polygon,
                              double y,
                              std::vector<double>* crossings)
{
    const std::size_t n = polygon.size();

    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const Point& a = polygon[i];
        const Point& b = polygon[j];
        if ((a.y > y) != (b.y > y)) {
            crossings->push_back(a.x + (y - a.y) * (b.x - a.x) / (b.y - a.y));
        }
    }
}

std::vector<Point>
splineWarpPolyline(const SplineWarpCurveShape& shape,
                   int perSegment)
{
    std::vector<Point> out;
    const int segments = shape.getSegmentCount();

    if (shape.isEmpty()) {
        return out;
    }
    if (segments == 0) {
        out.push_back(shape.pointAt(0.));

        return out;
    }
    perSegment = std::max(1, perSegment);
    const int count = segments * perSegment;
    out.reserve(count + 1);
    for (int i = 0; i < count; ++i) {
        out.push_back(shape.pointAt((double)i / perSegment));
    }
    if (!shape.isClosed()) {
        out.push_back(shape.pointAt((double)segments));
    }

    return out;
}

SplineWarpSampleSet
sampleSplineWarp(const std::vector<SplineWarpLinkSamplingInput>& links,
                 const std::vector<SplineWarpBoundarySamplingInput>& boundaries,
                 const SplineWarpSamplingSettings& settings)
{
    SplineWarpSampleSet result;
    const double spacing = (settings.spacing > 0.) ? settings.spacing : 1.;

    std::vector<LinkPlan> linkPlans;
    for (std::size_t i = 0; i < links.size(); ++i) {
        const SplineWarpLinkSamplingInput& link = links[i];
        if (!link.source || !link.destination || link.source->isEmpty() || link.destination->isEmpty()) {
            continue;
        }
        linkPlans.push_back(planLink(link, spacing));
    }

    std::vector<BoundaryPlan> boundaryPlans;
    for (std::size_t i = 0; i < boundaries.size(); ++i) {
        const SplineWarpBoundarySamplingInput& b = boundaries[i];
        if (!b.curve || (b.curve->getSegmentCount() == 0)) {
            continue;
        }
        BoundaryPlan plan;
        plan.shape.curve = splineWarpPolyline(*b.curve, kBoundaryPointsPerSegment);
        const SplineWarpCurveShape& featherShape = (b.feather && (b.feather->getSegmentCount() == b.curve->getSegmentCount())) ? *b.feather : *b.curve;
        plan.shape.feather = expandOutward(splineWarpPolyline(featherShape, kBoundaryPointsPerSegment), std::fabs(b.featherDistance));
        plan.shape.fallOff = (b.fallOff > 0.) ? b.fallOff : 1.;
        plan.cumulative = closedCumulative(plan.shape.feather);
        plan.count = std::max(3, countFor(plan.cumulative.back(), spacing));
        boundaryPlans.push_back(plan);
    }

    // The budget scales every span and boundary down by the same factor.
    long long total = 0;
    for (std::size_t i = 0; i < linkPlans.size(); ++i) {
        for (std::size_t k = 0; k < linkPlans[i].spans.size(); ++k) {
            total += linkPlans[i].spans[k].count;
        }
        if (linkPlans[i].includeEnd) {
            ++total;
        }
    }
    for (std::size_t i = 0; i < boundaryPlans.size(); ++i) {
        total += boundaryPlans[i].count;
    }
    if ((settings.budget > 0) && (total > settings.budget)) {
        const double factor = (double)settings.budget / (double)total;
        for (std::size_t i = 0; i < linkPlans.size(); ++i) {
            for (std::size_t k = 0; k < linkPlans[i].spans.size(); ++k) {
                SpanPlan& span = linkPlans[i].spans[k];
                span.count = std::max(1, (int)std::floor(span.count * factor));
            }
        }
        for (std::size_t i = 0; i < boundaryPlans.size(); ++i) {
            boundaryPlans[i].count = std::max(3, (int)std::floor(boundaryPlans[i].count * factor));
        }
    }

    // Raw pair ranges of each link, then each boundary, with whether the curve is a loop.
    std::vector<std::pair<std::size_t, bool>> rangeEnds;
    for (std::size_t i = 0; i < linkPlans.size(); ++i) {
        const double amount = settings.globalAmount * linkPlans[i].input->amount;
        emitLinkPairs(linkPlans[i], amount, settings.invert, &result.pairs);
        rangeEnds.push_back(std::make_pair(result.pairs.size(), !linkPlans[i].includeEnd));
    }
    const int linkPairs = (int)result.pairs.size();
    for (std::size_t i = 0; i < boundaryPlans.size(); ++i) {
        const BoundaryPlan& plan = boundaryPlans[i];
        const double perimeter = plan.cumulative.back();
        for (int k = 0; k < plan.count; ++k) {
            SplineWarpSamplePair pair;
            pair.destination = pointAtClosedArc(plan.shape.feather, plan.cumulative, perimeter * k / plan.count);
            pair.source = pair.destination;
            result.pairs.push_back(pair);
        }
        result.boundaries.push_back(plan.shape);
        rangeEnds.push_back(std::make_pair(result.pairs.size(), true));
    }

    std::vector<int> remap;
    result.linkPairCount = mergePairs(&result.pairs, linkPairs, &remap);
    std::size_t rangeStart = 0;
    for (std::size_t i = 0; i < rangeEnds.size(); ++i) {
        result.polylines.push_back(mergedPolyline(remap, rangeStart, rangeEnds[i].first, rangeEnds[i].second));
        rangeStart = rangeEnds[i].first;
    }

    for (int i = 0; i < result.linkPairCount && !result.linkSamplesInsideBoundaries; ++i) {
        const Point& d = result.pairs[i].destination;
        for (std::size_t b = 0; b < result.boundaries.size(); ++b) {
            if (splineWarpPointInPolygon(result.boundaries[b].feather, d.x, d.y)) {
                result.linkSamplesInsideBoundaries = true;
                break;
            }
        }
    }

    Hash64 hash;
    hash.append<int>(result.linkPairCount);
    for (std::size_t i = 0; i < result.pairs.size(); ++i) {
        hash.append<double>(result.pairs[i].destination.x);
        hash.append<double>(result.pairs[i].destination.y);
        hash.append<double>(result.pairs[i].source.x);
        hash.append<double>(result.pairs[i].source.y);
    }
    for (std::size_t p = 0; p < result.polylines.size(); ++p) {
        const SplineWarpSamplePolyline& polyline = result.polylines[p];
        hash.append<int>(polyline.closed ? 1 : 0);
        hash.append<std::size_t>(polyline.pairs.size());
        for (std::size_t i = 0; i < polyline.pairs.size(); ++i) {
            hash.append<int>(polyline.pairs[i]);
        }
    }
    for (std::size_t b = 0; b < result.boundaries.size(); ++b) {
        const SplineWarpBoundaryShape& shape = result.boundaries[b];
        hash.append<double>(shape.fallOff);
        hash.append<std::size_t>(shape.curve.size());
        for (std::size_t i = 0; i < shape.curve.size(); ++i) {
            hash.append<double>(shape.curve[i].x);
            hash.append<double>(shape.curve[i].y);
        }
        hash.append<std::size_t>(shape.feather.size());
        for (std::size_t i = 0; i < shape.feather.size(); ++i) {
            hash.append<double>(shape.feather[i].x);
            hash.append<double>(shape.feather[i].y);
        }
    }
    hash.computeHash();
    result.hash = hash.value();

    return result;
} // sampleSplineWarp

SplineWarpSampleSet
sampleSplineWarp(const SplineWarpContext& context,
                 const SplineWarpSamplingSettings& settings)
{
    const double time = settings.time;
    // The shapes outlive the inputs pointing at them.
    std::list<SplineWarpCurveShape> shapes;
    std::vector<SplineWarpLinkSamplingInput> links;
    std::vector<SplineWarpBoundarySamplingInput> boundaries;

    const std::vector<SplineWarpLinkPtr> contextLinks = context.getLinks();
    for (std::size_t i = 0; i < contextLinks.size(); ++i) {
        const SplineWarpLinkPtr& link = contextLinks[i];
        const BezierPtr source = link->getSource();
        const BezierPtr destination = link->getDestination();
        if (!source || !destination || !source->isActivated(time) || !destination->isActivated(time)) {
            continue;
        }
        SplineWarpLinkSamplingInput input;
        shapes.push_back(SplineWarpCurveShape::fromBezier(*source, time));
        input.source = &shapes.back();
        shapes.push_back(SplineWarpCurveShape::fromBezier(*destination, time));
        input.destination = &shapes.back();
        input.startOffset = link->getStartOffset();
        input.reversed = link->isReversed();
        input.amount = link->getAmount(time);
        const int count = link->getCorrespondenceCount();
        for (int k = 0; k < count; ++k) {
            double s, d;
            if (link->getCorrespondence(k, time, &s, &d)) {
                input.correspondences.push_back(std::make_pair(s, d));
            }
        }
        links.push_back(input);
    }

    const std::list<BezierPtr> contextBoundaries = context.getBoundaries();
    for (std::list<BezierPtr>::const_iterator it = contextBoundaries.begin(); it != contextBoundaries.end(); ++it) {
        const BezierPtr& curve = *it;
        if (!curve->isActivated(time)) {
            continue;
        }
        SplineWarpBoundarySamplingInput input;
        shapes.push_back(SplineWarpCurveShape::fromBezier(*curve, time));
        input.curve = &shapes.back();
        shapes.push_back(SplineWarpCurveShape::fromBezier(*curve, time, true));
        input.feather = &shapes.back();
        input.featherDistance = curve->getFeatherDistance(time);
        input.fallOff = curve->getFeatherFallOff(time);
        boundaries.push_back(input);
    }

    return sampleSplineWarp(links, boundaries, settings);
}

NATRON_NAMESPACE_EXIT
