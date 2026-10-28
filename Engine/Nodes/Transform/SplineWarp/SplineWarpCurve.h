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

#ifndef Engine_Nodes_Transform_SplineWarp_SplineWarpCurve_h
#define Engine_Nodes_Transform_SplineWarp_SplineWarpCurve_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <vector>

#include "Global/GlobalDefines.h"

#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief A Bezier's shape at one time, in canonical coordinates, read once so it can be evaluated
 * by parameter and by arc length without locking the curve again.
 *
 * A parameter u lies in [0, getSegmentCount()]: its integer part is the segment, the index of the
 * control point the segment starts at, and its fraction the Bezier t on that segment. A closed
 * curve has one segment per control point, the last one ending at the first point; an open or
 * unfinished one has one fewer. A single-point curve has no segment, and every parameter is 0.
 **/
class SplineWarpCurveShape {
public:
    SplineWarpCurveShape();

    /**
     * @param feather Reads the feather points instead of the control points.
     * @param applyTransform Applies the curve's own transform, as the Roto renderer does.
     **/
    static SplineWarpCurveShape fromBezier(const Bezier& curve,
                                           double time,
                                           bool feather = false,
                                           bool applyTransform = true,
                                           bool useGuiCurves = false);

    /**
     * @brief Whether the curve is closed: not an open Bezier, and finished.
     **/
    static bool isClosedCurve(const Bezier& curve);

    /**
     * @brief The segment count of curve as it is now, without reading its points' positions.
     **/
    static int segmentCountOf(const Bezier& curve);

    bool isEmpty() const
    {
        return _pointCount == 0;
    }

    bool isClosed() const
    {
        return _closed;
    }

    int getPointCount() const
    {
        return _pointCount;
    }

    int getSegmentCount() const
    {
        return (int)_segments.size();
    }

    double clampParam(double u) const;

    Point pointAt(double u) const;

    /**
     * @brief The control polygon of a segment: its start point, the start's right tangent, the
     * end's left tangent and its end point.
     **/
    void getSegment(int segment, Point* p0, Point* p1, Point* p2, Point* p3) const;

    double getSegmentLength(int segment) const;

    double getLength() const;

    /**
     * @brief The arc length from parameter 0 to u, and its inverse. Both are piecewise linear over
     * a fixed subdivision of each segment.
     **/
    double arcLengthAtParam(double u) const;
    double paramAtArcLength(double s) const;

    /**
     * @brief The parameter of the curve point nearest (x, y), and that point's distance to it.
     **/
    double nearestParam(double x, double y, double* distance) const;

private:
    void buildArcLengthTable();

    struct Segment {
        Point p0, p1, p2, p3;
    };

    std::vector<Segment> _segments;
    // A single-point curve's point; the start of segment 0 otherwise.
    Point _first;
    int _pointCount;
    bool _closed;
    // _cumulative[seg * kSubdivisions + k]: arc length from parameter 0 to seg + k / kSubdivisions.
    std::vector<double> _cumulative;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_SplineWarp_SplineWarpCurve_h
