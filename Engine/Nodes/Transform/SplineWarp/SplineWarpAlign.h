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

#ifndef Engine_Nodes_Transform_SplineWarp_SplineWarpAlign_h
#define Engine_Nodes_Transform_SplineWarp_SplineWarpAlign_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

class SplineWarpCurveShape;

/**
 * @brief Where pairing starts on a link's destination and which way it walks, as SplineWarpLink
 * holds it.
 **/
struct SplineWarpAlignment {
    double startOffset;
    bool reversed;

    SplineWarpAlignment()
        : startOffset(0.)
        , reversed(false)
    {
    }
};

/**
 * @brief The alignment of destination to source that minimises the sum of squared distances
 * between arc-length-paired points, each taken relative to its curve's centroid. A closed
 * destination is tried at every start on a fine arc-length grid, in both directions; an open one
 * forward and reversed only.
 **/
SplineWarpAlignment computeSplineWarpAlignment(const SplineWarpCurveShape& source,
                                               const SplineWarpCurveShape& destination);

/**
 * @brief Same, on the curves' shapes at time.
 **/
SplineWarpAlignment computeSplineWarpAlignment(const Bezier& source,
                                               const Bezier& destination,
                                               double time);

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_SplineWarp_SplineWarpAlign_h
