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

#ifndef Engine_Geo_GeoConnectivity_h
#define Engine_Geo_GeoConnectivity_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstdint>
#include <vector>

#include "Engine/GeoDetail.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The connected pieces of a detail: two primitives are connected when they share a
 * point, two points when a primitive uses both. A point no primitive uses is a piece of its own,
 * as is a primitive with no vertices.
 *
 * labels holds the piece of every storage offset of the class asked for, -1 for a dead one.
 * Pieces are numbered from 0 in the order of their lowest live offset, so equal topology gives
 * equal labels whatever the attributes.
 **/
struct GeoConnectivity {
    std::vector<std::int32_t> labels;
    std::int32_t count = 0;
};

/** @brief The connected pieces over the live points of @p detail. */
GeoConnectivity geoPointConnectivity(const GeoDetail& detail);

/** @brief The connected pieces over the live primitives of @p detail. */
GeoConnectivity geoPrimitiveConnectivity(const GeoDetail& detail);

NATRON_NAMESPACE_EXIT

#endif // Engine_Geo_GeoConnectivity_h
