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
 * @brief Moves a projection edge by at most half a device pixel so that coordinate 0 falls on a
 * device pixel edge. @p extent is the size of the projection spanning @p devicePixels.
 *
 * Without it a viewer with an odd width or height centres the image on half pixels. At a whole
 * zoom every sample then lands exactly between two texels, and which texel, or a 50/50 blend of
 * both, comes down to float rounding in the texture coordinates and in the minify/magnify choice:
 * the image is crisp or half a pixel soft depending on how its texture happened to be allocated.
 **/
inline double
snapEdgeToPixelGrid(double edge,
                    double devicePixels,
                    double extent)
{
    if ((devicePixels <= 0) || (extent <= 0)) {
        return edge;
    }
    const double pixelsPerUnit = devicePixels / extent;

    return std::floor(edge * pixelsPerUnit + 0.5) / pixelsPerUnit;
}

/**
 * @brief The centre of the device pixel containing @p widgetCoordinate, in widget coordinates.
 * @p devicePixelsPerWidgetPixel is the device pixel ratio.
 *
 * A pointer position names a pixel, but read as a coordinate it is that pixel's edge, shared with
 * its neighbour, so flooring it after unprojection picks either texel depending on rounding.
 **/
inline double
devicePixelCentre(double widgetCoordinate,
                  double devicePixelsPerWidgetPixel)
{
    if (devicePixelsPerWidgetPixel <= 0) {
        devicePixelsPerWidgetPixel = 1.;
    }

    return (std::floor(widgetCoordinate * devicePixelsPerWidgetPixel) + 0.5) / devicePixelsPerWidgetPixel;
}

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_VIEWERPIXELGRID_H
