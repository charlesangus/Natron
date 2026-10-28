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

#include "SplineWarpCurve.h"

#include <algorithm>
#include <cmath>
#include <list>

#include "Engine/Bezier.h"
#include "Engine/BezierCP.h"
#include "Engine/Transform.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

namespace {
const int kSubdivisions = 32;

Point
makePoint(double x,
          double y)
{
    Point p;

    p.x = x;
    p.y = y;

    return p;
}

Point
applyMatrix(const Transform::Matrix3x3& m,
            const Point& p)
{
    double x = p.x, y = p.y, z = 1.;

    Transform::matApply(m, &x, &y, &z);
    if ((z != 0.) && (z != 1.)) {
        x /= z;
        y /= z;
    }

    return makePoint(x, y);
}
} // anonymous namespace

SplineWarpCurveShape::SplineWarpCurveShape()
    : _segments()
    , _first(makePoint(0., 0.))
    , _pointCount(0)
    , _closed(false)
    , _cumulative(1, 0.)
{
}

bool
SplineWarpCurveShape::isClosedCurve(const Bezier& curve)
{
    return !curve.isOpenBezier() && curve.isCurveFinished();
}

int
SplineWarpCurveShape::segmentCountOf(const Bezier& curve)
{
    const int n = curve.getControlPointsCount();

    if (n <= 1) {
        return 0;
    }

    return isClosedCurve(curve) ? n : n - 1;
}

SplineWarpCurveShape
SplineWarpCurveShape::fromBezier(const Bezier& curve,
                                 double time,
                                 bool feather,
                                 bool applyTransform,
                                 bool useGuiCurves)
{
    SplineWarpCurveShape shape;
    const std::list<BezierCPPtr> cps = feather ? curve.getFeatherPoints_mt_safe() : curve.getControlPoints_mt_safe();
    const ViewIdx view(0);

    Transform::Matrix3x3 matrix;
    if (applyTransform) {
        curve.getTransformAtTime(time, &matrix);
    }

    std::vector<Point> pos, left, right;
    pos.reserve(cps.size());
    left.reserve(cps.size());
    right.reserve(cps.size());
    for (std::list<BezierCPPtr>::const_iterator it = cps.begin(); it != cps.end(); ++it) {
        Point p, l, r;
        (*it)->getPositionAtTime(useGuiCurves, time, view, &p.x, &p.y);
        (*it)->getLeftBezierPointAtTime(useGuiCurves, time, view, &l.x, &l.y);
        (*it)->getRightBezierPointAtTime(useGuiCurves, time, view, &r.x, &r.y);
        if (applyTransform) {
            p = applyMatrix(matrix, p);
            l = applyMatrix(matrix, l);
            r = applyMatrix(matrix, r);
        }
        pos.push_back(p);
        left.push_back(l);
        right.push_back(r);
    }

    const int n = (int)pos.size();
    shape._pointCount = n;
    shape._closed = isClosedCurve(curve);
    if (n > 0) {
        shape._first = pos[0];
    }
    if (n > 1) {
        const int segmentCount = shape._closed ? n : n - 1;
        shape._segments.resize(segmentCount);
        for (int i = 0; i < segmentCount; ++i) {
            const int j = (i + 1) % n;
            Segment& s = shape._segments[i];
            s.p0 = pos[i];
            s.p1 = right[i];
            s.p2 = left[j];
            s.p3 = pos[j];
        }
    }
    shape.buildArcLengthTable();

    return shape;
}

void
SplineWarpCurveShape::buildArcLengthTable()
{
    _cumulative.assign(1, 0.);
    _cumulative.reserve(_segments.size() * kSubdivisions + 1);
    double total = 0.;
    for (std::size_t i = 0; i < _segments.size(); ++i) {
        const Segment& s = _segments[i];
        Point prev = s.p0;
        for (int k = 1; k <= kSubdivisions; ++k) {
            Point cur;
            Bezier::bezierPoint(s.p0, s.p1, s.p2, s.p3, (double)k / kSubdivisions, &cur);
            total += std::hypot(cur.x - prev.x, cur.y - prev.y);
            _cumulative.push_back(total);
            prev = cur;
        }
    }
}

double
SplineWarpCurveShape::clampParam(double u) const
{
    const double maxParam = (double)_segments.size();

    if (!(u > 0.)) {
        return 0.;
    }

    return std::min(u, maxParam);
}

void
SplineWarpCurveShape::getSegment(int segment,
                                 Point* p0,
                                 Point* p1,
                                 Point* p2,
                                 Point* p3) const
{
    if (_segments.empty()) {
        *p0 = *p1 = *p2 = *p3 = _first;

        return;
    }
    const Segment& s = _segments[std::max(0, std::min(segment, (int)_segments.size() - 1))];
    *p0 = s.p0;
    *p1 = s.p1;
    *p2 = s.p2;
    *p3 = s.p3;
}

Point
SplineWarpCurveShape::pointAt(double u) const
{
    if (_segments.empty()) {
        return _first;
    }
    u = clampParam(u);
    const int segment = std::min((int)std::floor(u), (int)_segments.size() - 1);
    const Segment& s = _segments[segment];
    Point p;
    Bezier::bezierPoint(s.p0, s.p1, s.p2, s.p3, u - segment, &p);

    return p;
}

double
SplineWarpCurveShape::getSegmentLength(int segment) const
{
    if ((segment < 0) || (segment >= (int)_segments.size())) {
        return 0.;
    }

    return _cumulative[(segment + 1) * kSubdivisions] - _cumulative[segment * kSubdivisions];
}

double
SplineWarpCurveShape::getLength() const
{
    return _cumulative.back();
}

double
SplineWarpCurveShape::arcLengthAtParam(double u) const
{
    if (_segments.empty()) {
        return 0.;
    }
    const double x = clampParam(u) * kSubdivisions;
    const int last = (int)_cumulative.size() - 1;
    const int i = std::min((int)std::floor(x), last - 1);
    const double f = x - i;

    return _cumulative[i] + f * (_cumulative[i + 1] - _cumulative[i]);
}

double
SplineWarpCurveShape::paramAtArcLength(double s) const
{
    if (_segments.empty()) {
        return 0.;
    }
    const double total = getLength();
    if (!(s > 0.)) {
        return 0.;
    }
    if (s >= total) {
        return (double)_segments.size();
    }
    // First entry strictly above s; the one before it is at or below s.
    std::vector<double>::const_iterator it = std::upper_bound(_cumulative.begin(), _cumulative.end(), s);
    const int hi = (int)(it - _cumulative.begin());
    const int lo = hi - 1;
    const double span = _cumulative[hi] - _cumulative[lo];
    const double f = (span > 0.) ? (s - _cumulative[lo]) / span : 0.;

    return (lo + f) / kSubdivisions;
}

NATRON_NAMESPACE_EXIT
