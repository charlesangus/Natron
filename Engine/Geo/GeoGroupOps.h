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

#ifndef Engine_Geo_GeoGroupOps_h
#define Engine_Geo_GeoGroupOps_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <span>
#include <vector>

#include "Engine/Geo/GeoGroup.h"
#include "Engine/Geo/GeoOpUtils.h"
#include "Engine/GeoDetail.h"

NATRON_NAMESPACE_ENTER

/**
 * Element-set conversions over a detail. Point and primitive sets are GeoElementMask values
 * indexed by storage offset; edge sets are sorted, duplicate-free GeoEdge lists. Inputs may
 * name dead or out-of-range elements, which are ignored, and results hold live elements only.
 * Polygon and polyline primitives own edges; packed primitives are whole primitives and have
 * none.
 **/

/**
 * @brief The primitives built on the points. With @p entirelyContained a primitive needs every
 * one of its points in the set; without it, any one point is enough.
 **/
GeoElementMask geoPromotePointsToPrimitives(const GeoDetail& detail, const GeoElementMask& points, bool entirelyContained);

/** @brief Every point of the primitives. */
GeoElementMask geoPromotePrimitivesToPoints(const GeoDetail& detail, const GeoElementMask& primitives);

/** @brief The endpoints of the edges. */
GeoElementMask geoPromoteEdgesToPoints(const GeoDetail& detail, std::span<const GeoEdge> edges);

/** @brief The primitives that use any of the edges. */
GeoElementMask geoPromoteEdgesToPrimitives(const GeoDetail& detail, std::span<const GeoEdge> edges);

/**
 * @brief The edges of the detail with both ends in the set, or with @p entirelyContained false,
 * either end.
 **/
std::vector<GeoEdge> geoPromotePointsToEdges(const GeoDetail& detail, const GeoElementMask& points, bool entirelyContained);

/** @brief Every edge of the primitives. */
std::vector<GeoEdge> geoPromotePrimitivesToEdges(const GeoDetail& detail, const GeoElementMask& primitives);

/**
 * @brief Grows the set by @p steps rings of neighbours; a negative count shrinks it by that many
 * rings, removing the elements on the set's boundary. Points neighbour each other through a
 * shared edge, primitives through a shared edge, and edges through a shared point. An element
 * of the set whose neighbours are all in the set is not on its boundary, so the open border of
 * a mesh does not erode a set.
 **/
GeoElementMask geoExpandPoints(const GeoDetail& detail, const GeoElementMask& points, int steps);
GeoElementMask geoExpandPrimitives(const GeoDetail& detail, const GeoElementMask& primitives, int steps);
std::vector<GeoEdge> geoExpandEdges(const GeoDetail& detail, std::span<const GeoEdge> edges, int steps);

NATRON_NAMESPACE_EXIT

#endif // Engine_Geo_GeoGroupOps_h
