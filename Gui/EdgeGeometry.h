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

#ifndef Gui_EdgeGeometry_h
#define Gui_EdgeGeometry_h

#include "Global/Macros.h"

#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QLineF>
#include <QPointF>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

NATRON_NAMESPACE_ENTER

// True when two closed segments share a point: a proper crossing, an end touching the other
// segment, or a collinear overlap. A zero-length segment is a point.
bool segmentsIntersect(const QLineF& a,
                       const QLineF& b);

// True when any segment of the polyline, taken point to point, meets the segment as
// segmentsIntersect() does. A polyline of fewer than two points meets nothing.
bool polylineCrossesSegment(const std::vector<QPointF>& polyline,
                            const QLineF& segment);

NATRON_NAMESPACE_EXIT

#endif // Gui_EdgeGeometry_h
