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

#include "SplineWarpAlign.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "Engine/Bezier.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpCurve.h"

NATRON_NAMESPACE_ENTER

namespace {
// Points paired per candidate alignment.
const int kSamples = 128;
// Start offsets tried around a closed destination; a multiple of kSamples, so the coarse search
// reads the table without interpolating.
const int kOffsets = 512;
const int kRefineSteps = 32;

// A curve sampled at kOffsets points evenly spaced in arc length, plus the end point when open,
// relative to their centroid.
class ArcTable {
public:
    ArcTable(const SplineWarpCurveShape& shape)
        : _closed(shape.isClosed())
        , _points()
    {
        const double length = shape.getLength();
        const int count = _closed ? kOffsets : kOffsets + 1;

        _points.reserve(count);
        double cx = 0., cy = 0.;
        for (int i = 0; i < count; ++i) {
            const Point p = shape.pointAt(shape.paramAtArcLength(length * i / kOffsets));
            _points.push_back(p);
            cx += p.x;
            cy += p.y;
        }
        // The closing end point would weigh the start twice.
        const int centroidCount = _closed ? count : count - 1;
        if (!_closed) {
            cx -= _points.back().x;
            cy -= _points.back().y;
        }
        cx /= centroidCount;
        cy /= centroidCount;
        for (std::size_t i = 0; i < _points.size(); ++i) {
            _points[i].x -= cx;
            _points[i].y -= cy;
        }
    }

    // Position at arc-length fraction f of the curve; f wraps around a closed curve and clamps
    // to [0, 1] on an open one.
    Point at(double f) const
    {
        double x = f * kOffsets;

        if (_closed) {
            x = std::fmod(x, (double)kOffsets);
            if (x < 0.) {
                x += kOffsets;
            }
        } else {
            x = std::max(0., std::min(x, (double)kOffsets));
        }
        int i = (int)std::floor(x);
        const int last = (int)_points.size() - 1;
        if (i > last) {
            i = last;
        }
        const double t = x - i;
        const int j = _closed ? (i + 1) % kOffsets : std::min(i + 1, last);
        Point p;
        p.x = _points[i].x + t * (_points[j].x - _points[i].x);
        p.y = _points[i].y + t * (_points[j].y - _points[i].y);

        return p;
    }

    const Point& atIndex(int i) const
    {
        return _points[i];
    }

private:
    bool _closed;
    std::vector<Point> _points;
};

struct Pairing {
    std::vector<double> fractions;
    std::vector<Point> source;
};

double
cost(const Pairing& pairing,
     const ArcTable& destination,
     double startFraction,
     bool reversed)
{
    double sum = 0.;

    for (std::size_t i = 0; i < pairing.fractions.size(); ++i) {
        const double f = reversed ? startFraction - pairing.fractions[i] : startFraction + pairing.fractions[i];
        const Point d = destination.at(f);
        const double dx = d.x - pairing.source[i].x;
        const double dy = d.y - pairing.source[i].y;
        sum += dx * dx + dy * dy;
    }

    return sum;
}

// The coarse search on a closed destination: offset index m, read straight from the table.
double
costAtIndex(const Pairing& pairing,
            const ArcTable& destination,
            int m,
            bool reversed)
{
    const int step = kOffsets / kSamples;
    double sum = 0.;

    for (int i = 0; i < kSamples; ++i) {
        int k = reversed ? m - i * step : m + i * step;
        k %= kOffsets;
        if (k < 0) {
            k += kOffsets;
        }
        const Point& d = destination.atIndex(k);
        const double dx = d.x - pairing.source[i].x;
        const double dy = d.y - pairing.source[i].y;
        sum += dx * dx + dy * dy;
    }

    return sum;
}
} // anonymous namespace

SplineWarpAlignment
computeSplineWarpAlignment(const SplineWarpCurveShape& source,
                           const SplineWarpCurveShape& destination)
{
    SplineWarpAlignment result;

    if ((source.getSegmentCount() == 0) || (destination.getSegmentCount() == 0) || !(source.getLength() > 0.) || !(destination.getLength() > 0.)) {
        return result;
    }

    const ArcTable sourceTable(source);
    const ArcTable destinationTable(destination);

    // Two closed curves pair one lap with one lap; otherwise both ends of each walk are paired.
    const bool bothClosed = source.isClosed() && destination.isClosed();
    Pairing pairing;
    pairing.fractions.reserve(kSamples);
    pairing.source.reserve(kSamples);
    for (int i = 0; i < kSamples; ++i) {
        const double f = bothClosed ? (double)i / kSamples : (double)i / (kSamples - 1);
        pairing.fractions.push_back(f);
        pairing.source.push_back(sourceTable.at(f));
    }

    if (!destination.isClosed()) {
        // An open destination is walked from its first point, or from its last when reversed.
        const double forward = cost(pairing, destinationTable, 0., false);
        const double backward = cost(pairing, destinationTable, 1., true);
        result.reversed = backward < forward;
        result.startOffset = 0.;

        return result;
    }

    double best = std::numeric_limits<double>::infinity();
    double bestFraction = 0.;
    bool bestReversed = false;
    for (int direction = 0; direction < 2; ++direction) {
        const bool reversed = (direction == 1);
        for (int m = 0; m < kOffsets; ++m) {
            const double c = bothClosed ? costAtIndex(pairing, destinationTable, m, reversed)
                                        : cost(pairing, destinationTable, (double)m / kOffsets, reversed);
            if (c < best) {
                best = c;
                bestFraction = (double)m / kOffsets;
                bestReversed = reversed;
            }
        }
    }

    // Two rounds of a finer scan around the best grid offset.
    double halfWidth = 1. / kOffsets;
    for (int round = 0; round < 2; ++round) {
        const double center = bestFraction;
        for (int k = -kRefineSteps; k <= kRefineSteps; ++k) {
            const double f = center + halfWidth * k / kRefineSteps;
            const double c = cost(pairing, destinationTable, f, bestReversed);
            if (c < best) {
                best = c;
                bestFraction = f;
            }
        }
        halfWidth /= kRefineSteps;
    }

    bestFraction = std::fmod(bestFraction, 1.);
    if (bestFraction < 0.) {
        bestFraction += 1.;
    }
    double offset = destination.paramAtArcLength(bestFraction * destination.getLength());
    if (offset >= destination.getSegmentCount()) {
        offset = 0.;
    }
    result.startOffset = offset;
    result.reversed = bestReversed;

    return result;
}

SplineWarpAlignment
computeSplineWarpAlignment(const Bezier& source,
                           const Bezier& destination,
                           double time)
{
    return computeSplineWarpAlignment(SplineWarpCurveShape::fromBezier(source, time),
                                      SplineWarpCurveShape::fromBezier(destination, time));
}

NATRON_NAMESPACE_EXIT
