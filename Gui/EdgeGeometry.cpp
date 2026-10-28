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

#include "EdgeGeometry.h"

#include <algorithm>
#include <cstddef>

NATRON_NAMESPACE_ENTER

namespace {

// The sign of the cross product (b - a) x (c - a): which side of a->b the point c is on.
int
orientation(const QPointF& a,
            const QPointF& b,
            const QPointF& c)
{
    const double cross = (b.x() - a.x()) * (c.y() - a.y()) - (b.y() - a.y()) * (c.x() - a.x());
    if (cross > 0.) {
        return 1;
    }
    if (cross < 0.) {
        return -1;
    }

    return 0;
}

// For c collinear with a and b: whether c lies within the segment's bounding box.
bool
onSegment(const QPointF& a,
          const QPointF& b,
          const QPointF& c)
{
    return (c.x() >= std::min(a.x(), b.x())) && (c.x() <= std::max(a.x(), b.x())) && (c.y() >= std::min(a.y(), b.y())) && (c.y() <= std::max(a.y(), b.y()));
}

} // anonymous namespace

bool
segmentsIntersect(const QLineF& a,
                  const QLineF& b)
{
    const QPointF p1 = a.p1();
    const QPointF p2 = a.p2();
    const QPointF q1 = b.p1();
    const QPointF q2 = b.p2();
    const int o1 = orientation(p1, p2, q1);
    const int o2 = orientation(p1, p2, q2);
    const int o3 = orientation(q1, q2, p1);
    const int o4 = orientation(q1, q2, p2);

    if ((o1 != o2) && (o3 != o4)) {
        return true;
    }

    return ((o1 == 0) && onSegment(p1, p2, q1)) || ((o2 == 0) && onSegment(p1, p2, q2)) || ((o3 == 0) && onSegment(q1, q2, p1)) || ((o4 == 0) && onSegment(q1, q2, p2));
}

bool
polylineCrossesSegment(const std::vector<QPointF>& polyline,
                       const QLineF& segment)
{
    for (std::size_t i = 1; i < polyline.size(); ++i) {
        if (segmentsIntersect(QLineF(polyline[i - 1], polyline[i]), segment)) {
            return true;
        }
    }

    return false;
}

NATRON_NAMESPACE_EXIT
