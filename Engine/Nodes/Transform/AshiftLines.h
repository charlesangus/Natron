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

#ifndef Engine_Nodes_Transform_AshiftLines_h
#define Engine_Nodes_Transform_AshiftLines_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <vector>

#include "Engine/Nodes/Transform/AshiftFit.h"

NATRON_NAMESPACE_ENTER

namespace Ashift {

/**
 * @brief One guide line the user drew, in darktable's square-pixel input coordinates (y down),
 * tagged vertical or horizontal.
 **/
struct DrawnLine {
    double x1 = 0.;
    double y1 = 0.;
    double x2 = 0.;
    double y2 = 0.;
    bool vertical = true;
};

/**
 * @brief A selected line through (x1, y1) and (x2, y2) with its homogeneous form, weight 1.
 **/
Line makeLine(double x1, double y1, double x2, double y2, int type);

/**
 * @brief The line set of drawn guide lines in a width x height image: each segment of non-zero
 * length becomes a selected line of its tagged direction, every line weighing 1.
 **/
LineSet drawnLines(const std::vector<DrawnLine>& segments, int width, int height);

/**
 * @brief The four lines of a quadrilateral, darktable's rectangle mode: points are the top left,
 * bottom left, top right and bottom right corners in pixel coordinates, y down.
 **/
LineSet quadLines(const double points[8], int width, int height);

} // namespace Ashift

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_AshiftLines_h
