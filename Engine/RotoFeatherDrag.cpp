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

#include "RotoFeatherDrag.h"

#include <cmath>

NATRON_NAMESPACE_ENTER

bool
featherSitsOnControlPoint(const Point& controlPoint,
                          const Point& featherPoint)
{
    return controlPoint.x == featherPoint.x && controlPoint.y == featherPoint.y;
}

Point
featherDragDirection(const Point& controlPoint,
                     const Point& featherPoint,
                     const Point& leftDerivative,
                     const Point& rightDerivative,
                     bool clockWise)
{
    Point dir;

    if (!featherSitsOnControlPoint(controlPoint, featherPoint)) {
        const double vx = featherPoint.x - controlPoint.x;
        const double vy = featherPoint.y - controlPoint.y;
        const double norm = std::sqrt(vx * vx + vy * vy);
        dir.x = vx / norm;
        dir.y = vy / norm;

        return dir;
    }

    const double tx = rightDerivative.x - leftDerivative.x;
    const double ty = rightDerivative.y - leftDerivative.y;
    double norm = std::sqrt(tx * tx + ty * ty);
    if (norm != 0) {
        dir.x = -ty / norm;
        dir.y = tx / norm;
    } else {
        // Kept from Bezier::expandToFeatherDistance, which draws the bar this way, so the drag
        // follows the bar the user grabbed.
        const double lx = leftDerivative.x - featherPoint.x;
        const double ly = leftDerivative.y - featherPoint.y;
        norm = std::sqrt(lx * lx + ly * ly);
        if (norm != 0) {
            dir.x = -ly / norm;
            dir.y = lx / norm;
        } else {
            dir.x = dir.y = 0.;

            return dir;
        }
    }

    if (!clockWise) {
        dir.x = -dir.x;
        dir.y = -dir.y;
    }

    return dir;
}

double
featherDragDistance(const Point& dir,
                    double dx,
                    double dy)
{
    return dir.x * dx + dir.y * dy;
}

Point
applyFeatherDistance(const Point& featherPoint,
                     const Point& dir,
                     double s)
{
    Point moved;

    moved.x = featherPoint.x + dir.x * s;
    moved.y = featherPoint.y + dir.y * s;

    return moved;
}

NATRON_NAMESPACE_EXIT
