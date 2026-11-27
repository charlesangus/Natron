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

#ifndef Engine_Geo_GeoTopology_h
#define Engine_Geo_GeoTopology_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "Global/GlobalDefines.h"

#include "Engine/Geo/GeoAttribute.h"
#include "Engine/Geo/GeoIndexMap.h"

NATRON_NAMESPACE_ENTER

enum GeoPrimitiveTypeEnum : std::int32_t {
    eGeoPrimitivePolygon = 0,
    eGeoPrimitivePolyline,
    eGeoPrimitivePackedGeometry,
    eGeoPrimitivePackedUSD,
};

bool geoPrimitiveTypeIsValid(std::int32_t type);
bool geoPrimitiveIsPacked(GeoPrimitiveTypeEnum type);

/**
 * @brief Fewest vertices a primitive of this type keeps before a point delete destroys it as
 * degenerate: 3 for a polygon, 2 for a polyline, 1 for a packed primitive.
 **/
GeoOffset geoPrimitiveMinVertexCount(GeoPrimitiveTypeEnum type);

/** @brief A primitive's vertices: the contiguous vertex offsets [start, start + count). */
struct GeoVertexRange {
    GeoOffset start = GeoIndexMap::kInvalidOffset;
    GeoOffset count = 0;
};

/**
 * @brief The element references of a detail, stored as paged copy-on-write columns like any
 * attribute: vertex -> point offset, vertex -> primitive offset, and per primitive its type and
 * its vertex range. Every primitive owns a contiguous run of vertex offsets that no other
 * primitive shares, so vertex numbers follow primitive order.
 *
 * Columns are sized to the element classes' offset sizes, tombstones included. References held
 * by tombstoned elements are not maintained. The caller keeps the sizes and references
 * consistent; GeoDetailBuilder is the intended caller.
 **/
class GeoTopology {
public:
    GeoTopology();

    std::size_t vertexOffsetSize() const
    {
        return _vertexPoint.size();
    }

    std::size_t primitiveOffsetSize() const
    {
        return _primitiveType.size();
    }

    GeoOffset vertexPoint(GeoOffset vertex) const
    {
        return _vertexPoint.get<std::int64_t>(std::size_t(vertex));
    }

    GeoOffset vertexPrimitive(GeoOffset vertex) const
    {
        return _vertexPrimitive.get<std::int64_t>(std::size_t(vertex));
    }

    GeoVertexRange primitiveVertices(GeoOffset primitive) const
    {
        return GeoVertexRange { _primitiveVertexStart.get<std::int64_t>(std::size_t(primitive)),
                                GeoOffset(_primitiveVertexCount.get<std::int32_t>(std::size_t(primitive))) };
    }

    GeoPrimitiveTypeEnum primitiveType(GeoOffset primitive) const
    {
        return static_cast<GeoPrimitiveTypeEnum>(_primitiveType.get<std::int32_t>(std::size_t(primitive)));
    }

    /** @brief int64 point offsets, -1 by default. */
    const GeoAttribute& vertexPointColumn() const
    {
        return _vertexPoint;
    }

    /** @brief int64 primitive offsets, -1 by default. */
    const GeoAttribute& vertexPrimitiveColumn() const
    {
        return _vertexPrimitive;
    }

    /** @brief int64 first vertex offsets, -1 by default. */
    const GeoAttribute& primitiveVertexStartColumn() const
    {
        return _primitiveVertexStart;
    }

    /** @brief int32 vertex counts. */
    const GeoAttribute& primitiveVertexCountColumn() const
    {
        return _primitiveVertexCount;
    }

    /** @brief int32 GeoPrimitiveTypeEnum values. */
    const GeoAttribute& primitiveTypeColumn() const
    {
        return _primitiveType;
    }

    std::array<const GeoAttribute*, 5> columns() const
    {
        return { &_vertexPoint, &_vertexPrimitive, &_primitiveVertexStart, &_primitiveVertexCount, &_primitiveType };
    }

    void resizeVertices(std::size_t offsetSize);
    void resizePrimitives(std::size_t offsetSize);

    void setVertexPoint(GeoOffset vertex, GeoOffset point);
    void setVertexPrimitive(GeoOffset vertex, GeoOffset primitive);

    /** @brief Writes points.size() consecutive vertices from @p firstVertex. */
    void setVertexPoints(GeoOffset firstVertex, std::span<const GeoOffset> points);
    void setVertexPrimitives(GeoOffset firstVertex, std::span<const GeoOffset> primitives);

    void setPrimitive(GeoOffset primitive, GeoPrimitiveTypeEnum type, GeoVertexRange vertices);

    /** @brief Writes starts.size() consecutive primitives of one type from @p firstPrimitive. */
    void setPrimitives(GeoOffset firstPrimitive,
                       GeoPrimitiveTypeEnum type,
                       std::span<const GeoOffset> starts,
                       std::span<const std::int32_t> counts);

    /** @brief Copies both vertex references of @p src to @p dst. */
    void copyVertex(GeoOffset dst, GeoOffset src);

    /** @brief Vertex columns after a gather (see GeoAttribute::gathered()). */
    void gatherVertices(std::span<const std::size_t> sourceOffsets);
    void gatherPrimitives(std::span<const std::size_t> sourceOffsets);

    /**
     * @brief Rewrite stored references through a dense old -> new table (GeoIndexRemap::
     * newFromOldTable()); values outside the table become -1. Pages whose values do not change
     * stay shared.
     **/
    void remapVertexPoints(std::span<const GeoOffset> newFromOld);
    void remapVertexPrimitives(std::span<const GeoOffset> newFromOld);
    void remapPrimitiveVertexStarts(std::span<const GeoOffset> newFromOld);

    /** @brief Lets every column page cache its hash again; see GeoAttribute::endWriting(). */
    void endWriting() noexcept;

    /** @brief Never 0. */
    U64 contentHash() const;

    std::size_t getSizeInBytes() const;

private:
    GeoAttribute _vertexPoint;
    GeoAttribute _vertexPrimitive;
    GeoAttribute _primitiveVertexStart;
    GeoAttribute _primitiveVertexCount;
    GeoAttribute _primitiveType;
};

/**
 * @brief Point -> vertices adjacency for the live vertices of a detail, as compressed rows:
 * the vertices of each point in ascending offset order. Immutable once built; a detail builds
 * it lazily, caches it and drops it whenever its topology changes.
 **/
class GeoReverseTopology {
public:
    GeoReverseTopology(const GeoTopology& topology,
                       const GeoIndexMap& vertices,
                       GeoOffset pointOffsetSize);

    GeoOffset pointOffsetSize() const
    {
        return GeoOffset(_starts.size()) - 1;
    }

    /** @brief Empty for a point outside the table or used by no live vertex. */
    std::span<const GeoOffset> pointVertices(GeoOffset point) const;

    /** @brief Fills a cleared @p out with the primitives using the point, ascending, without repeats. */
    void pointPrimitives(GeoOffset point,
                         const GeoTopology& topology,
                         std::vector<GeoOffset>& out) const;

    std::size_t getSizeInBytes() const;

private:
    std::vector<GeoOffset> _starts;
    std::vector<GeoOffset> _vertices;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Geo_GeoTopology_h
