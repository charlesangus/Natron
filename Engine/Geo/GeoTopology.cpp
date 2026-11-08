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

#include "GeoTopology.h"

#include <algorithm>
#include <stdexcept>

#include "Engine/ContentHasher.h"

NATRON_NAMESPACE_ENTER

namespace {

GeoAttribute
makeReferenceColumn(const char* name)
{
    GeoAttribute column(GeoAttributeDesc { name, eGeoStorageInt64, 1, eGeoTypeInfoNone, false });
    const std::int64_t invalid = GeoIndexMap::kInvalidOffset;
    column.setDefault<std::int64_t>(std::span<const std::int64_t>(&invalid, 1));

    return column;
}

GeoAttribute
makeInt32Column(const char* name)
{
    return GeoAttribute(GeoAttributeDesc { name, eGeoStorageInt32, 1, eGeoTypeInfoNone, false });
}

void
remapReferences(GeoAttribute& column,
                std::span<const GeoOffset> newFromOld)
{
    const auto lookup = [&](std::int64_t v) -> std::int64_t {
        return v >= 0 && std::size_t(v) < newFromOld.size() ? newFromOld[std::size_t(v)] : GeoIndexMap::kInvalidOffset;
    };
    column.makePageTableUnique();
    geoParallelFor(column.pageCount(), [&](std::size_t pi) {
        const GeoPage& page = *column.values().page(pi);
        const std::size_t n = column.pageElementCount(pi);
        const std::int64_t* values = reinterpret_cast<const std::int64_t*>(page.data());
        bool changes = false;
        if (page.isConstant()) {
            changes = lookup(values[0]) != values[0];
        } else {
            for (std::size_t k = 0; k < n && !changes; ++k) {
                changes = lookup(values[k]) != values[k];
            }
        }
        if (!changes) {
            return;
        }
        std::int64_t* out = column.pageForWriting<std::int64_t>(pi);
        for (std::size_t k = 0; k < n; ++k) {
            out[k] = lookup(out[k]);
        }
    });
}

} // namespace

bool
geoPrimitiveTypeIsValid(std::int32_t type)
{
    return type >= eGeoPrimitivePolygon && type <= eGeoPrimitivePackedUSD;
}

bool
geoPrimitiveIsPacked(GeoPrimitiveTypeEnum type)
{
    return type == eGeoPrimitivePackedGeometry || type == eGeoPrimitivePackedUSD;
}

GeoOffset
geoPrimitiveMinVertexCount(GeoPrimitiveTypeEnum type)
{
    switch (type) {
    case eGeoPrimitivePolygon:
        return 3;
    case eGeoPrimitivePolyline:
        return 2;
    case eGeoPrimitivePackedGeometry:
    case eGeoPrimitivePackedUSD:
        return 1;
    }

    return 1;
}

// ---------------------------------------------------------------------------------------------
// GeoTopology

GeoTopology::GeoTopology()
    : _vertexPoint(makeReferenceColumn("vertexPoint"))
    , _vertexPrimitive(makeReferenceColumn("vertexPrimitive"))
    , _primitiveVertexStart(makeReferenceColumn("primitiveVertexStart"))
    , _primitiveVertexCount(makeInt32Column("primitiveVertexCount"))
    , _primitiveType(makeInt32Column("primitiveType"))
{
}

void
GeoTopology::resizeVertices(std::size_t offsetSize)
{
    _vertexPoint.resize(offsetSize);
    _vertexPrimitive.resize(offsetSize);
}

void
GeoTopology::resizePrimitives(std::size_t offsetSize)
{
    _primitiveVertexStart.resize(offsetSize);
    _primitiveVertexCount.resize(offsetSize);
    _primitiveType.resize(offsetSize);
}

void
GeoTopology::setVertexPoint(GeoOffset vertex,
                            GeoOffset point)
{
    _vertexPoint.set<std::int64_t>(std::size_t(vertex), point);
}

void
GeoTopology::setVertexPrimitive(GeoOffset vertex,
                                GeoOffset primitive)
{
    _vertexPrimitive.set<std::int64_t>(std::size_t(vertex), primitive);
}

void
GeoTopology::setVertexPoints(GeoOffset firstVertex,
                             std::span<const GeoOffset> points)
{
    const std::size_t begin = std::size_t(firstVertex);
    _vertexPoint.scatterDense<std::int64_t>(begin, begin + points.size(), points);
}

void
GeoTopology::setVertexPrimitives(GeoOffset firstVertex,
                                 std::span<const GeoOffset> primitives)
{
    const std::size_t begin = std::size_t(firstVertex);
    _vertexPrimitive.scatterDense<std::int64_t>(begin, begin + primitives.size(), primitives);
}

void
GeoTopology::setPrimitive(GeoOffset primitive,
                          GeoPrimitiveTypeEnum type,
                          GeoVertexRange vertices)
{
    const std::size_t p = std::size_t(primitive);
    _primitiveVertexStart.set<std::int64_t>(p, vertices.start);
    _primitiveVertexCount.set<std::int32_t>(p, std::int32_t(vertices.count));
    _primitiveType.set<std::int32_t>(p, std::int32_t(type));
}

void
GeoTopology::setPrimitives(GeoOffset firstPrimitive,
                           GeoPrimitiveTypeEnum type,
                           std::span<const GeoOffset> starts,
                           std::span<const std::int32_t> counts)
{
    if (starts.size() != counts.size()) {
        throw std::invalid_argument("GeoTopology::setPrimitives: starts and counts differ in length");
    }
    const std::size_t begin = std::size_t(firstPrimitive);
    const std::size_t end = begin + starts.size();
    _primitiveVertexStart.scatterDense<std::int64_t>(begin, end, starts);
    _primitiveVertexCount.scatterDense<std::int32_t>(begin, end, counts);
    const std::int32_t typeValue = type;
    _primitiveType.fill<std::int32_t>(begin, end, std::span<const std::int32_t>(&typeValue, 1));
}

void
GeoTopology::copyVertex(GeoOffset dst,
                        GeoOffset src)
{
    _vertexPoint.copyElement(std::size_t(dst), std::size_t(src));
    _vertexPrimitive.copyElement(std::size_t(dst), std::size_t(src));
}

void
GeoTopology::gatherVertices(std::span<const std::size_t> sourceOffsets)
{
    _vertexPoint = _vertexPoint.gathered(sourceOffsets);
    _vertexPrimitive = _vertexPrimitive.gathered(sourceOffsets);
}

void
GeoTopology::gatherPrimitives(std::span<const std::size_t> sourceOffsets)
{
    _primitiveVertexStart = _primitiveVertexStart.gathered(sourceOffsets);
    _primitiveVertexCount = _primitiveVertexCount.gathered(sourceOffsets);
    _primitiveType = _primitiveType.gathered(sourceOffsets);
}

void
GeoTopology::remapVertexPoints(std::span<const GeoOffset> newFromOld)
{
    remapReferences(_vertexPoint, newFromOld);
}

void
GeoTopology::remapVertexPrimitives(std::span<const GeoOffset> newFromOld)
{
    remapReferences(_vertexPrimitive, newFromOld);
}

void
GeoTopology::remapPrimitiveVertexStarts(std::span<const GeoOffset> newFromOld)
{
    remapReferences(_primitiveVertexStart, newFromOld);
}

U64
GeoTopology::contentHash() const
{
    ContentHasher hasher;
    for (const GeoAttribute* column : columns()) {
        hasher.appendU64(column->contentHash());
    }
    const U64 h = hasher.finalize();

    return h ? h : 1;
}

std::size_t
GeoTopology::getSizeInBytes() const
{
    std::size_t bytes = 0;
    for (const GeoAttribute* column : columns()) {
        bytes += column->getSizeInBytes();
    }

    return bytes;
}

// ---------------------------------------------------------------------------------------------
// GeoReverseTopology

GeoReverseTopology::GeoReverseTopology(const GeoTopology& topology,
                                       const GeoIndexMap& vertices,
                                       GeoOffset pointOffsetSize)
    : _starts(std::size_t(std::max<GeoOffset>(pointOffsetSize, 0)) + 1, 0)
{
    const GeoAttribute& column = topology.vertexPointColumn();
    const auto pointOf = [&](GeoOffset vertex) -> GeoOffset {
        const GeoOffset p = column.get<std::int64_t>(std::size_t(vertex));
        return p >= 0 && p < pointOffsetSize ? p : GeoIndexMap::kInvalidOffset;
    };

    vertices.forEachLive([&](GeoIndex, GeoOffset vertex) {
        const GeoOffset p = pointOf(vertex);
        if (p >= 0) {
            ++_starts[std::size_t(p) + 1];
        }
    });
    for (std::size_t i = 1; i < _starts.size(); ++i) {
        _starts[i] += _starts[i - 1];
    }
    _vertices.resize(std::size_t(_starts.back()));
    std::vector<GeoOffset> cursor(_starts.begin(), _starts.end() - 1);
    vertices.forEachLive([&](GeoIndex, GeoOffset vertex) {
        const GeoOffset p = pointOf(vertex);
        if (p >= 0) {
            _vertices[std::size_t(cursor[std::size_t(p)]++)] = vertex;
        }
    });
}

std::span<const GeoOffset>
GeoReverseTopology::pointVertices(GeoOffset point) const
{
    if (point < 0 || point >= pointOffsetSize()) {
        return {};
    }
    const std::size_t begin = std::size_t(_starts[std::size_t(point)]);
    const std::size_t end = std::size_t(_starts[std::size_t(point) + 1]);

    return std::span<const GeoOffset>(_vertices.data() + begin, end - begin);
}

void
GeoReverseTopology::pointPrimitives(GeoOffset point,
                                    const GeoTopology& topology,
                                    std::vector<GeoOffset>& out) const
{
    out.clear();
    for (GeoOffset vertex : pointVertices(point)) {
        out.push_back(topology.vertexPrimitive(vertex));
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

std::size_t
GeoReverseTopology::getSizeInBytes() const
{
    return (_starts.size() + _vertices.size()) * sizeof(GeoOffset);
}

NATRON_NAMESPACE_EXIT
