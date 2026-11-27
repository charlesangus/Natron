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

#ifndef Engine_Geo_GeoUVIslands_h
#define Engine_Geo_GeoUVIslands_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <set>
#include <string>
#include <vector>

#include "Global/GlobalDefines.h"

#include "Engine/Geo/GeoGroup.h"
#include "Engine/Geo/GeoOpUtils.h"
#include "Engine/GeoDetail.h"

NATRON_NAMESPACE_ENTER

/** @brief What joins two polygons sharing an edge into one island. */
enum GeoUVIslandConnectivityEnum {
    // The uv agrees, within the tolerance, at both ends of the edge on both sides.
    eGeoUVIslandConnectivityUV = 0,
    // Sharing the edge is enough: islands are cut only by the seams.
    eGeoUVIslandConnectivityTopology,
};

struct GeoUVIslandOptions {
    GeoUVIslandConnectivityEnum connectivity = eGeoUVIslandConnectivityUV;
    // A vertex or point attribute with two or more numeric components; only read for UV
    // connectivity. A point attribute never splits an island, since both sides share it.
    std::string uvAttribute = "uv";
    // How far apart u or v may be, at either end of an edge, for the edge still to join.
    double tolerance = 1e-5;
    // Edges that always cut, whatever the connectivity.
    std::set<GeoEdge> seams;
    // An integer primitive attribute merging islands: the islands whose value is the same are
    // grouped. An island's value is that of its lowest-numbered polygon with one that is not
    // negative; an island with none stays on its own. Empty for no merging.
    std::string islandAttribute;
    // The polygons taking part, by primitive offset; empty for every polygon.
    GeoElementMask primitives;
};

/**
 * @brief The islands of a detail's polygons. A vertex edge runs from a vertex to the next one of
 * its polygon, so each polygon edge has one per polygon using it; it is named by its first
 * vertex.
 **/
struct GeoUVIslands {
    // By primitive offset: the island, or -1 for primitives in none (not a polygon, outside the
    // options' primitives, degenerate or dead).
    std::vector<int> islandOfPrimitive;
    // Each island's polygons in ascending offset; islands are ordered by their first polygon.
    std::vector<std::vector<GeoOffset>> islands;
    // By island: the group it was merged into by the island attribute. Groups are numbered by
    // their first island; without an island attribute each island is its own group.
    std::vector<int> groupOfIsland;
    std::vector<std::vector<int>> groups;
    // By vertex offset: the vertex edge across the polygon edge when exactly one other polygon
    // of the islands shares it, whether or not the edge joins them; kInvalidOffset otherwise.
    std::vector<GeoOffset> vertexEdgeTwin;
    // By vertex offset: 1 where the vertex edge is on its island's boundary (no twin, a seam, a
    // non-manifold edge or a uv break), 0 where it joins, and 0 for vertices of no island.
    std::vector<char> vertexEdgeCut;
    // By island: its boundary as loops of vertex edges, each continuing from the point the last
    // one ends at.
    std::vector<std::vector<std::vector<GeoOffset>>> boundaryLoops;
    // The polygons skipped for having no area: fewer than three distinct points, or an area
    // negligible beside the square of their perimeter.
    std::vector<GeoOffset> degenerate;
};

/** @brief The vertex a vertex edge ends at: the next of its polygon, wrapping round. */
GeoOffset geoVertexEdgeEnd(const GeoDetail& detail,
                           GeoOffset vertex);

/** @brief The polygon edge under a vertex edge. */
GeoEdge geoVertexEdge(const GeoDetail& detail,
                      GeoOffset vertex);

/**
 * @brief Splits the polygons into islands: polygons are joined across a polygon edge exactly two
 * of them share (so a non-manifold edge always cuts) when it is not a seam and the connectivity
 * allows. Fails, with *errorMessage set, when UV connectivity names a missing or non-numeric
 * attribute or the island attribute is not an integer primitive attribute.
 **/
bool geoComputeUVIslands(const GeoDetail& detail,
                         const GeoUVIslandOptions& options,
                         GeoUVIslands* islands,
                         std::string* errorMessage);

NATRON_NAMESPACE_EXIT

#endif // Engine_Geo_GeoUVIslands_h
