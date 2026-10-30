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

/*
 * The maths are a port of darktable's rotate and perspective module, DARKTABLE_REF release-5.6.2
 * (commit 20891f6c6fa7), src/iop/ashift.c, Copyright (C) 2016-2026 darktable developers,
 * distributed under the GNU GPL version 3 or later.
 */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "AshiftLines.h"

#include <cmath>

NATRON_NAMESPACE_ENTER

namespace Ashift {

Line
makeLine(double x1,
         double y1,
         double x2,
         double y2,
         int type)
{
    Line line;

    line.p1[0] = x1;
    line.p1[1] = y1;
    line.p1[2] = 1.;
    line.p2[0] = x2;
    line.p2[1] = y2;
    line.p2[2] = 1.;
    vec3prodn(line.L, line.p1, line.p2);
    vec3lnorm(line.L, line.L);
    line.length = std::sqrt((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1));
    line.width = 1.;
    line.weight = 1.;
    line.type = type;

    return line;
}

LineSet
drawnLines(const std::vector<DrawnLine>& segments,
           int width,
           int height)
{
    LineSet set;

    set.width = width;
    set.height = height;
    for (const DrawnLine& s : segments) {
        if ((s.x1 == s.x2) && (s.y1 == s.y2)) {
            continue;
        }
        set.lines.push_back(makeLine(s.x1, s.y1, s.x2, s.y2, s.vertical ? eLineVerticalSelected : eLineHorizontalSelected));
        if (s.vertical) {
            ++set.verticalCount;
            set.verticalWeight += 1.;
        } else {
            ++set.horizontalCount;
            set.horizontalWeight += 1.;
        }
    }

    return set;
}

LineSet
quadLines(const double pts[8],
          int width,
          int height)
{
    // darktable's order: the left side, the right side, the top and the bottom.
    const int sides[4][2] = { { 0, 1 }, { 2, 3 }, { 0, 2 }, { 1, 3 } };
    LineSet set;

    for (int i = 0; i < 4; ++i) {
        const double x1 = pts[2 * sides[i][0]];
        const double y1 = pts[2 * sides[i][0] + 1];
        const double x2 = pts[2 * sides[i][1]];
        const double y2 = pts[2 * sides[i][1] + 1];
        const bool horizontal = std::fabs(x1 - x2) > std::fabs(y1 - y2);
        set.lines.push_back(makeLine(x1, y1, x2, y2, horizontal ? eLineHorizontalSelected : eLineVerticalSelected));
    }
    set.width = width;
    set.height = height;
    set.verticalCount = 2;
    set.horizontalCount = 2;
    set.verticalWeight = 2.;
    set.horizontalWeight = 2.;

    return set;
}

} // namespace Ashift

NATRON_NAMESPACE_EXIT
