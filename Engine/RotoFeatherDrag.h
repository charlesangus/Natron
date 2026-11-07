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

#ifndef Engine_RotoFeatherDrag_h
#define Engine_RotoFeatherDrag_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/GlobalDefines.h"
#include "Global/Macros.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The direction a feather point moves in when a feather bar is dragged, all in canonical
 * coordinates with the Bezier's transform already applied.
 *
 * When the feather point is away from its control point, this is the unit vector from the control
 * point to the feather point; leftDerivative, rightDerivative and clockWise are not used.
 *
 * When the feather point sits on its control point, this is the unit normal to the curve built
 * from the left and right derivatives at that point, oriented outward the way the feather bar is
 * drawn (clockWise is the feather polygon's orientation). If the derivatives are equal, the left
 * derivative alone gives the normal, and if that is degenerate too the direction is (0, 0).
 *
 * Orienting both cases away from the control point makes a positive distance lengthen every
 * feather, so several points dragged together all grow or all shrink.
 **/
Point featherDragDirection(const Point& controlPoint,
                           const Point& featherPoint,
                           const Point& leftDerivative,
                           const Point& rightDerivative,
                           bool clockWise);

/**
 * @brief True when the feather point sits on its control point, the case where
 * featherDragDirection needs the derivatives.
 **/
bool featherSitsOnControlPoint(const Point& controlPoint, const Point& featherPoint);

/**
 * @brief The signed distance of the drag (dx, dy) along the unit direction dir: its projection.
 **/
double featherDragDistance(const Point& dir, double dx, double dy);

/**
 * @brief featherPoint moved by the signed distance s along the unit direction dir.
 **/
Point applyFeatherDistance(const Point& featherPoint, const Point& dir, double s);

/**
 * @brief The curvature vector of the circle that leaves p0 heading towards p1 and passes through
 * p3: it points to the side the circle turns towards and its length is one over its radius. For a
 * segment from p0 to p3 with first handle p1 this is exact when the segment is a circular arc,
 * whatever its handle lengths. (0, 0) when p1 sits on p0 or p3 does.
 **/
Point featherCurvature(const Point& p0, const Point& p1, const Point& p3);

/**
 * @brief The factor that scales a feather point's tangent on one side when the point moves by
 * delta, given the curvature vector of the feather curve on that side: 1 - curvature . delta.
 * This is how an offset curve's tangents stretch, so moving every feather of a circle out by the
 * same distance keeps it a concentric circle. Never negative.
 **/
double featherTangentScale(const Point& curvature, const Point& delta);

/**
 * @brief Limits a tangent scale so the grown tangent reaches at most half way to the neighbouring
 * feather point on its side. Two tangents facing each other across a segment then never pass one
 * another, which is what tears a feather when closely packed points grow. A scale that does not
 * grow the tangent, or a tangent already longer than that reach, is left as it is.
 **/
double clampFeatherTangentScale(double scale, double tangentLength, double neighbourDistance);

NATRON_NAMESPACE_EXIT

#endif // Engine_RotoFeatherDrag_h
