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

#ifndef Engine_Ai_MaskTrace_h
#define Engine_Ai_MaskTrace_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <vector>

NATRON_NAMESPACE_ENTER

/**
 * @brief Turns a probability mask into closed cubic Bezier outlines.
 *
 * Coordinates are mask pixels: x to the right, y down, pixel (i, j) covering [i, i + 1) x [j, j + 1),
 * so a pixel's centre is (i + 0.5, j + 0.5).
 **/
namespace MaskTrace {

struct Point {
    double x = 0.;
    double y = 0.;
};

struct Polygon {
    std::vector<Point> points;
};

// A control point and its two tangent handles, absolute.
struct CurvePoint {
    Point position;
    Point left;
    Point right;
    // The handles turn here (a corner): a Bezier must keep them broken, or it realigns them.
    bool cusp = false;
};

struct Contour {
    // Closed: the last point joins the first.
    std::vector<CurvePoint> points;
    // A hole cuts its enclosing outline.
    bool isHole = false;
    // How many contours enclose this one: 0 for a top-level outline, 1 for its holes, 2 for an
    // island inside a hole, and so on. Holes have odd depths.
    int depth = 0;
    // Area enclosed, in square pixels.
    double area = 0.;
};

struct Options {
    // Iso level of the outline.
    double threshold = 0.5;
    // Douglas-Peucker tolerance and Bezier fitting error, in pixels.
    double simplify = 1.5;
    // Contours enclosing less are dropped, in square pixels.
    double minArea = 64.;
    bool keepHoles = true;
    // Smooth curves through the outline; off keeps straight segments between its corners.
    bool smooth = true;
    // Turns sharper than this, in degrees, stay corners when smoothing.
    double cornerAngle = 60.;
};

/**
 * @brief The iso-contours of mask at threshold, as closed polygons interpolated between pixel
 * centres (marching squares). Outside the mask counts as below threshold, so every contour closes.
 * Their direction is arbitrary; trace() orients them.
 **/
std::vector<Polygon> isoContours(const std::vector<float>& mask, int width, int height, double threshold);

// The shoelace signed area of a closed polygon.
double signedArea(const Polygon& polygon);

// Whether p lies inside the closed polygon (even-odd rule).
bool contains(const Polygon& polygon, const Point& p);

// Douglas-Peucker simplification of a closed polygon.
Polygon simplify(const Polygon& polygon, double tolerance);

/**
 * @brief Fits closed cubic curves through polygon within maxError pixels, breaking the tangent at
 * turns sharper than cornerAngle (Schneider's algorithm, per corner-to-corner run).
 **/
std::vector<CurvePoint> fitCurves(const Polygon& polygon, double maxError, double cornerAngle);

// Straight segments through the polygon's points: the handles sit on the points.
std::vector<CurvePoint> straightCurves(const Polygon& polygon);

/**
 * @brief The outlines of mask, ordered by depth so that a shape is always listed after the shapes
 * enclosing it. Outlines have a positive shoelace area in the y-down frame, holes a negative one.
 **/
std::vector<Contour> trace(const std::vector<float>& mask, int width, int height, const Options& options);

} // namespace MaskTrace

NATRON_NAMESPACE_EXIT

#endif // Engine_Ai_MaskTrace_h
