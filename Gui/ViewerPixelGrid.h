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

#ifndef NATRON_GUI_VIEWERPIXELGRID_H
#define NATRON_GUI_VIEWERPIXELGRID_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cmath>

NATRON_NAMESPACE_ENTER

/**
 * @brief Moves a viewer's orthographic projection by less than half a device pixel so that
 * canonical coordinate 0 falls on a device pixel edge, keeping its size.
 *
 * Without it a viewer with an odd width or height centres the image on half pixels. At a whole
 * zoom every sample then lands exactly between two texels, and which texel, or a 50/50 blend of
 * both, comes down to float rounding in the texture coordinates and in the minify/magnify choice:
 * the image is crisp or half a pixel soft depending on how its texture happened to be allocated.
 **/
inline void
snapProjectionToPixelGrid(double* left,
                          double* right,
                          double* bottom,
                          double* top,
                          int viewportWidth,
                          int viewportHeight)
{
    if ((viewportWidth > 0) && (*right != *left)) {
        const double pixelsPerUnit = viewportWidth / (*right - *left);
        const double snapped = std::floor(*left * pixelsPerUnit + 0.5) / pixelsPerUnit;
        *right += snapped - *left;
        *left = snapped;
    }
    if ((viewportHeight > 0) && (*top != *bottom)) {
        const double pixelsPerUnit = viewportHeight / (*top - *bottom);
        const double snapped = std::floor(*bottom * pixelsPerUnit + 0.5) / pixelsPerUnit;
        *top += snapped - *bottom;
        *bottom = snapped;
    }
}

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_VIEWERPIXELGRID_H
