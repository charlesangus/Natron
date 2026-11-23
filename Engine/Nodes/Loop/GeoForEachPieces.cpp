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

#include "GeoForEachPieces.h"

#include <algorithm>
#include <list>
#include <mutex>
#include <utility>

#include "Engine/Geo/GeoConnectivity.h"
#include "Engine/Geo/GeoOpUtils.h"
#include "Engine/Geo/GeoPacked.h"
#include "Engine/Geo/GeoTopology.h"

NATRON_NAMESPACE_ENTER

namespace {

const std::size_t kKeptPartitions = 16;

struct KeptPartition {
    U64 contentHash = 0;
    GeoPieceSettings settings;
    GeoPiecePartitionConstPtr partition;
};

std::mutex&
keptPartitionsMutex()
{
    static std::mutex m;

    return m;
}

// Most recently used first.
std::list<KeptPartition>&
keptPartitions()
{
    static std::list<KeptPartition> kept;

    return kept;
}

GeoAttribClassEnum
elementClassOf(GeoPieceElementsEnum elements)
{
    return (elements == eGeoPieceElementsPoints) ? eGeoAttribClassPoint : eGeoAttribClassPrimitive;
}

const char*
elementClassName(GeoPieceElementsEnum elements)
{
    return (elements == eGeoPieceElementsPoints) ? "point" : "primitive";
}

std::vector<std::size_t>
sequence(std::size_t count)
{
    std::vector<std::size_t> out(count);
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = i;
    }

    return out;
}

bool
holds(const std::vector<std::size_t>& sorted,
      GeoOffset offset)
{
    return std::binary_search(sorted.begin(), sorted.end(), std::size_t(offset));
}

std::size_t
positionIn(const std::vector<std::size_t>& sorted,
           GeoOffset offset)
{
    return std::size_t(std::lower_bound(sorted.begin(), sorted.end(), std::size_t(offset)) - sorted.begin());
}

} // anonymous namespace

const std::string&
GeoPiecePartition::stringValue(std::size_t piece) const
{
    static const std::string none;

    return _stringValued ? _stringValues[piece] : none;
}

std::ptrdiff_t
GeoPiecePartition::find(std::int64_t value) const
{
    if (_stringValued) {
        return -1;
    }
    if (_indexValued) {
        return ((value >= 0) && (std::uint64_t(value) < size())) ? std::ptrdiff_t(value) : -1;
    }
    const auto it = _byInt.find(value);

    return (it == _byInt.end()) ? -1 : std::ptrdiff_t(it->second);
}

std::ptrdiff_t
GeoPiecePartition::find(const std::string& value) const
{
    if (!_stringValued) {
        return -1;
    }
    const auto it = _byString.find(value);

    return (it == _byString.end()) ? -1 : std::ptrdiff_t(it->second);
}

GeoPiecePartitionConstPtr
GeoForEachPieces::partition(const GeoDetail& detail,
                            const GeoPieceSettings& settings,
                            std::string* error)
{
    const U64 contentHash = detail.contentHash();
    {
        std::lock_guard<std::mutex> lock(keptPartitionsMutex());
        std::list<KeptPartition>& kept = keptPartitions();
        for (std::list<KeptPartition>::iterator it = kept.begin(); it != kept.end(); ++it) {
            if ((it->contentHash == contentHash) && (it->settings == settings)) {
                kept.splice(kept.begin(), kept, it);

                return kept.front().partition;
            }
        }
    }

    const GeoAttribClassEnum cls = elementClassOf(settings.elements);
    const std::vector<std::size_t> live = geoLiveOffsets(detail.indexMap(cls));
    std::shared_ptr<GeoPiecePartition> out = std::make_shared<GeoPiecePartition>();
    out->_elementClass = settings.elements;
    std::vector<std::size_t> pieceOf(live.size());
    std::size_t count = 0;

    switch (settings.method) {
    case eGeoPieceMethodAttribute: {
        if (settings.attribute.empty()) {
            *error = "Piece Attribute is empty: name the attribute whose values split the pieces.";

            return GeoPiecePartitionConstPtr();
        }
        const GeoAttribute* attribute = detail.findAttribute(cls, settings.attribute);
        if (!attribute) {
            *error = std::string("There is no ") + elementClassName(settings.elements) + " attribute \"" + settings.attribute + "\" to split the pieces by.";

            return GeoPiecePartitionConstPtr();
        }
        const GeoStorageEnum storage = attribute->storage();
        const bool integer = (storage == eGeoStorageInt32) || (storage == eGeoStorageInt64);
        if (attribute->isArray() || (attribute->tupleSize() != 1) || (!integer && (storage != eGeoStorageString))) {
            *error = std::string("The ") + elementClassName(settings.elements) + " attribute \"" + settings.attribute + "\" cannot split pieces: it must be an integer or a string with one component.";

            return GeoPiecePartitionConstPtr();
        }
        if (storage == eGeoStorageString) {
            out->_stringValued = true;
            for (std::size_t i = 0; i < live.size(); ++i) {
                std::string value(attribute->getString(live[i]));
                const auto inserted = out->_byString.emplace(value, out->_stringValues.size());
                if (inserted.second) {
                    out->_stringValues.push_back(std::move(value));
                }
                pieceOf[i] = inserted.first->second;
            }
            count = out->_stringValues.size();
        } else {
            for (std::size_t i = 0; i < live.size(); ++i) {
                const std::int64_t value = (storage == eGeoStorageInt32) ? std::int64_t(attribute->get<std::int32_t>(live[i])) : attribute->get<std::int64_t>(live[i]);
                const auto inserted = out->_byInt.emplace(value, out->_intValues.size());
                if (inserted.second) {
                    out->_intValues.push_back(value);
                }
                pieceOf[i] = inserted.first->second;
            }
            count = out->_intValues.size();
        }
        break;
    }
    case eGeoPieceMethodConnectivity: {
        const GeoConnectivity connectivity = (settings.elements == eGeoPieceElementsPoints) ? geoPointConnectivity(detail) : geoPrimitiveConnectivity(detail);
        out->_indexValued = true;
        for (std::size_t i = 0; i < live.size(); ++i) {
            pieceOf[i] = std::size_t(connectivity.labels[live[i]]);
        }
        count = std::size_t(connectivity.count);
        break;
    }
    case eGeoPieceMethodEachElement:
        out->_indexValued = true;
        for (std::size_t i = 0; i < live.size(); ++i) {
            pieceOf[i] = i;
        }
        count = live.size();
        break;
    }

    out->_starts.assign(count + 1, 0);
    for (std::size_t piece : pieceOf) {
        ++out->_starts[piece + 1];
    }
    for (std::size_t piece = 0; piece < count; ++piece) {
        out->_starts[piece + 1] += out->_starts[piece];
    }
    out->_elements.resize(live.size());
    std::vector<std::size_t> next(out->_starts.begin(), out->_starts.end() - 1);
    for (std::size_t i = 0; i < live.size(); ++i) {
        out->_elements[next[pieceOf[i]]++] = GeoOffset(live[i]);
    }

    {
        std::lock_guard<std::mutex> lock(keptPartitionsMutex());
        std::list<KeptPartition>& kept = keptPartitions();
        KeptPartition entry;
        entry.contentHash = contentHash;
        entry.settings = settings;
        entry.partition = out;
        kept.push_front(std::move(entry));
        if (kept.size() > kKeptPartitions) {
            kept.pop_back();
        }
    }

    return out;
} // GeoForEachPieces::partition

GeoDetail
GeoForEachPieces::extract(const GeoDetail& detail,
                          const GeoPiecePartition& partition,
                          std::size_t piece)
{
    const std::span<const GeoOffset> members = partition.elements(piece);
    std::vector<std::size_t> points;
    std::vector<std::size_t> primitives;

    if (partition.elementClass() == eGeoPieceElementsPrimitives) {
        primitives.assign(members.begin(), members.end());
        for (std::size_t prim : primitives) {
            const GeoVertexRange range = detail.primitiveVertices(GeoOffset(prim));
            for (GeoOffset v = range.start; v < range.start + range.count; ++v) {
                points.push_back(std::size_t(detail.vertexPoint(v)));
            }
        }
        std::sort(points.begin(), points.end());
        points.erase(std::unique(points.begin(), points.end()), points.end());
    } else {
        points.assign(members.begin(), members.end());
        const std::shared_ptr<const GeoReverseTopology> reverse = detail.reverseTopology();
        std::vector<GeoOffset> usingPoint;
        std::vector<GeoOffset> candidates;
        for (std::size_t p : points) {
            usingPoint.clear();
            reverse->pointPrimitives(GeoOffset(p), detail.topology(), usingPoint);
            candidates.insert(candidates.end(), usingPoint.begin(), usingPoint.end());
        }
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
        for (GeoOffset prim : candidates) {
            const GeoVertexRange range = detail.primitiveVertices(prim);
            bool whole = true;
            for (GeoOffset v = range.start; whole && (v < range.start + range.count); ++v) {
                whole = holds(points, detail.vertexPoint(v));
            }
            if (whole) {
                primitives.push_back(std::size_t(prim));
            }
        }
    }

    GeoDetailBuilder builder;
    builder.setPointPrecision64(detail.pointPrecision64());
    const GeoAttribClassEnum classes[] = { eGeoAttribClassPoint, eGeoAttribClassVertex, eGeoAttribClassPrimitive, eGeoAttribClassDetail };
    for (GeoAttribClassEnum cls : classes) {
        for (std::size_t i = 0; i < detail.attributeCount(cls); ++i) {
            const GeoAttribute& source = detail.attribute(cls, i);
            if ((cls == eGeoAttribClassPoint) && (source.name() == GeoDetail::kPositionName)) {
                continue;
            }
            GeoAttribute& added = builder.addAttribute(cls, source.desc());
            if (cls == eGeoAttribClassDetail) {
                const std::size_t zero = 0;
                geoCopyAttributeElements(source, std::span<const std::size_t>(&zero, 1), added, std::span<const std::size_t>(&zero, 1));
            } else {
                geoAdoptDefault(source, added);
            }
        }
    }

    if (!points.empty()) {
        builder.appendPoints(GeoIndex(points.size()));
    }
    // Primitives go in runs of one type, keeping their order.
    std::vector<std::size_t> vertices;
    std::vector<std::int32_t> counts;
    std::vector<GeoOffset> corners;
    std::size_t i = 0;
    while (i < primitives.size()) {
        const GeoPrimitiveTypeEnum type = detail.primitiveType(GeoOffset(primitives[i]));
        counts.clear();
        corners.clear();
        for (; (i < primitives.size()) && (detail.primitiveType(GeoOffset(primitives[i])) == type); ++i) {
            const GeoVertexRange range = detail.primitiveVertices(GeoOffset(primitives[i]));
            counts.push_back(std::int32_t(range.count));
            for (GeoOffset v = range.start; v < range.start + range.count; ++v) {
                vertices.push_back(std::size_t(v));
                corners.push_back(GeoOffset(positionIn(points, detail.vertexPoint(v))));
            }
        }
        builder.appendPrimitives(type, counts, corners);
    }

    const std::pair<GeoAttribClassEnum, const std::vector<std::size_t>*> kept[] = {
        { eGeoAttribClassPoint, &points },
        { eGeoAttribClassVertex, &vertices },
        { eGeoAttribClassPrimitive, &primitives },
    };
    for (const auto& [cls, sources] : kept) {
        if (sources->empty()) {
            continue;
        }
        const std::vector<std::size_t> targets = sequence(sources->size());
        for (std::size_t a = 0; a < detail.attributeCount(cls); ++a) {
            const GeoAttribute& source = detail.attribute(cls, a);
            GeoAttribute* target = builder.attributeForWriting(cls, source.name());
            if (target) {
                geoCopyAttributeElements(source, *sources, *target, targets);
            }
        }
        for (const std::string& name : detail.groupNames(cls)) {
            const GeoElementGroup* group = detail.findGroup(cls, name);
            if (!group) {
                continue;
            }
            std::vector<GeoOffset> inPiece;
            for (std::size_t k = 0; k < sources->size(); ++k) {
                if (group->contains(GeoOffset((*sources)[k]))) {
                    inPiece.push_back(GeoOffset(k));
                }
            }
            builder.addGroup(cls, name).add(inPiece);
        }
    }
    for (const std::string& name : detail.edgeGroupNames()) {
        const GeoEdgeGroup* group = detail.findEdgeGroup(name);
        if (!group) {
            continue;
        }
        std::vector<GeoEdge> edges;
        for (const GeoEdge& e : group->edges()) {
            if (holds(points, e.p0) && holds(points, e.p1)) {
                edges.emplace_back(GeoOffset(positionIn(points, e.p0)), GeoOffset(positionIn(points, e.p1)));
            }
        }
        builder.addEdgeGroup(name).add(edges);
    }

    const GeoAttribute* packedSource = detail.findAttribute(eGeoAttribClassPrimitive, kGeoPackedDetailAttr);
    if (packedSource && (packedSource->storage() == eGeoStorageInt32) && !packedSource->isArray() && !primitives.empty()) {
        std::vector<std::pair<std::size_t, std::int32_t>> remapped;
        for (std::size_t k = 0; k < primitives.size(); ++k) {
            const std::int32_t index = packedSource->get<std::int32_t>(primitives[k]);
            if ((index <= 0) || (std::size_t(index) > detail.sharedDetails().size())) {
                continue;
            }
            remapped.emplace_back(k, builder.addSharedDetail(detail.sharedDetails()[std::size_t(index - 1)]) + 1);
        }
        GeoAttribute* packedTarget = builder.attributeForWriting(eGeoAttribClassPrimitive, kGeoPackedDetailAttr);
        if (packedTarget) {
            for (const auto& [prim, index] : remapped) {
                packedTarget->set<std::int32_t>(prim, index);
            }
        }
    }

    return builder.finish();
} // GeoForEachPieces::extract

NATRON_NAMESPACE_EXIT
