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

#ifndef Engine_RotoFeatherRaster_h
#define Engine_RotoFeatherRaster_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/GlobalDefines.h"
#include "Global/Macros.h"

#include <vector>

#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief One cell of a shape's feather: inner0 then inner1 are consecutive points of the shape's
 * polygon, in the polygon's order, and outer0 and outer1 the feather points they fall off to.
 *
 * Across the cell the position is bilinear in (t, v): t goes from the inner edge (0) to the outer
 * edge (1) and v from the inner0-outer0 side (0) to the inner1-outer1 side (1).
 **/
struct RotoFeatherQuad {
    Point inner0;
    Point outer0;
    Point outer1;
    Point inner1;
};

/**
 * @brief The feather's opacity at fraction t of the way from the shape (t <= 0 gives 1) to its
 * feather (t >= 1 gives 0), for the shape's fall-off.
 *
 * Roto has always placed the ramp's position along a cubic Bezier with inner control points at
 * 1 / (2 fallOff^2 + 1) and 2 / (fallOff^2 + 2) of the way across while its opacity goes linearly
 * from 1 to 0, and has always painted that ramp through itself as its own mask, so the opacity
 * is squared: a fall-off of 1 gives (1 - t)^2.
 **/
double rotoFeatherFalloff(double t, double fallOff);

/**
 * @brief Raises each pixel of alpha (roi.width() x roi.height(), row 0 at roi.y1) to the
 * feather's opacity at the pixel's centre where that is higher. Points are in pixels.
 *
 * quads lists the cells of one closed shape in the shape polygon's order, each cell sharing its
 * inner1-outer1 side with the next cell's inner0-outer0 side. A pixel covered by several cells,
 * as where a concave part folds the feather over itself, takes the highest of their opacities,
 * which stays continuous because neighbouring cells agree along the side they share.
 *
 * A cell whose outer edge lies outside the shape also covers up to one pixel inward of its inner
 * edge at full opacity, so that the pixels between a chord of the shape's polygon and the curve
 * it stands for, outside the shape's fill on a concave part, are not left out.
 **/
void rasterizeRotoFeather(const std::vector<RotoFeatherQuad>& quads,
                          double fallOff,
                          const RectI& roi,
                          float* alpha);

NATRON_NAMESPACE_EXIT

#endif // Engine_RotoFeatherRaster_h
