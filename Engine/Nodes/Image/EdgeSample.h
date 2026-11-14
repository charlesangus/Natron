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

#ifndef Engine_Nodes_Image_EdgeSample_h
#define Engine_Nodes_Image_EdgeSample_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/NodeMetadata.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief How every CPU path reads a source outside its image, by the source's EdgeMode.
 **/
namespace EdgeSample {

/**
 * @brief Where a read at v along an axis of [lo, hiExclusive) lands, in *sampled: v itself
 * inside, the nearest end of the axis outside for Stretch. False when the read finds no pixel and
 * is zero: outside for Black, anywhere on an empty axis.
 **/
inline bool
coord(int v,
      int lo,
      int hiExclusive,
      EdgeMode mode,
      int* sampled)
{
    if (hiExclusive <= lo) {
        return false;
    }
    if (v < lo) {
        if (mode == EdgeMode::Black) {
            return false;
        }
        v = lo;
    } else if (v >= hiExclusive) {
        if (mode == EdgeMode::Black) {
            return false;
        }
        v = hiExclusive - 1;
    }
    *sampled = v;

    return true;
}

/**
 * @brief coord() on both axes of bounds: the pixel a read at (x, y) lands on, false for zero.
 **/
inline bool
pixel(int x,
      int y,
      const RectI& bounds,
      EdgeMode mode,
      int* sx,
      int* sy)
{
    return coord(x, bounds.x1, bounds.x2, mode, sx) && coord(y, bounds.y1, bounds.y2, mode, sy);
}

} // namespace EdgeSample

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Image_EdgeSample_h
