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

#ifndef Engine_Nodes_Transform_SplineWarp_SplineWarpCookie_h
#define Engine_Nodes_Transform_SplineWarp_SplineWarpCookie_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <memory>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

class SplineWarpContext;

/**
 * @brief The union of the cookie cutters, one float per pixel over bounds, 0 outside.
 **/
struct SplineWarpCookieMask {
    RectI bounds;
    std::vector<float> pixels;

    // The value at pixel (x, y), 0 outside bounds.
    float at(int x, int y) const;

    const float* data() const
    {
        return pixels.empty() ? (const float*)0 : &pixels[0];
    }
};

typedef std::shared_ptr<const SplineWarpCookieMask> SplineWarpCookieMaskConstPtr;

/**
 * @brief The curves the cookie mask is made of at time: every active cookie cutter at its source
 * position, which for a linked curve is the source curve of its link. Each curve once.
 **/
std::list<BezierPtr> getSplineWarpCookieCurves(const SplineWarpContext& context, double time);

/**
 * @brief The pixels the curves cover at time, feathers included, scaleX and scaleY pixels per
 * canonical unit; empty without curves.
 **/
RectI splineWarpCookieBounds(const std::list<BezierPtr>& curves, double time, double scaleX, double scaleY);

/**
 * @brief Rasterises the union of the curves at time over window, with each curve's feather and
 * fall-off, as Roto renders a shape.
 **/
SplineWarpCookieMask renderSplineWarpCookieMask(const std::list<BezierPtr>& curves, double time, const RectI& window, double scaleX, double scaleY);

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_SplineWarp_SplineWarpCookie_h
