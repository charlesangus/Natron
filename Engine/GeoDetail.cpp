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

#include "GeoDetail.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <numeric>
#include <stdexcept>

#include "Engine/ContentHasher.h"
#include "Engine/Geo/GeoPacked.h"

NATRON_NAMESPACE_ENTER

struct GeoDetail::AttributeSlot {
    explicit AttributeSlot(GeoAttribute a)
        : attribute(std::move(a))
    {
    }

    GeoAttribute attribute;
    mutable std::atomic<U64> hash { 0 };
    // Cleared while a builder holds this slot for writing; only frozen slots cache their hash.
    bool frozen = true;

    bool hasCachedHash() const
    {
        return frozen && hash.load(std::memory_order_acquire) != 0;
    }

    U64 contentHash() const
    {
        if (!frozen) {
            return attribute.contentHash();
        }
        U64 h = hash.load(std::memory_order_acquire);
        if (h == 0) {
            h = attribute.contentHash();
            hash.store(h, std::memory_order_release);
        }

        return h;
    }
};

struct GeoDetail::Cache {
    std::atomic<U64> hash { 0 };
    std::mutex mutex;
    std::shared_ptr<const GeoReverseTopology> reverse;
};

namespace {

constexpr std::size_t kMinPagesForParallelHash = 64;

void
hashIndexMap(ContentHasher& hasher,
             const GeoIndexMap& map)
{
    hasher.appendI64(map.offsetSize());
    hasher.appendI64(map.size());
    if (map.isTrivial()) {
        return;
    }
    const GeoOffset pages = map.pageCount();
    for (GeoOffset p = 0; p < pages; ++p) {
        const GeoIndexMap::LivePage* live = map.livePage(p);
        if (live) {
            hasher.appendI64(p);
            hasher.appendBytes(live->words.data(), sizeof(live->words));
        }
    }
}

std::size_t
indexMapSizeInBytes(const GeoIndexMap& map)
{
    if (map.isTrivial()) {
        return 0;
    }

    return std::size_t(map.pageCount()) * (sizeof(GeoIndexMap::LivePage) + sizeof(std::shared_ptr<GeoIndexMap::LivePage>));
}

std::vector<std::size_t>
sourceOffsetsOf(const GeoIndexRemap& remap)
{
    std::vector<std::size_t> offsets(std::size_t(remap.newOffsetSize()));
    for (const GeoIndexRemap::Run& run : remap.runs()) {
        std::iota(offsets.begin() + std::ptrdiff_t(run.newOffset),
                  offsets.begin() + std::ptrdiff_t(run.newOffset + run.count),
                  std::size_t(run.oldOffset));
    }

    return offsets;
}

template <typename From, typename To>
void
convertTuples(const GeoAttribute& from,
              GeoAttribute& to)
{
    to.makePageTableUnique();
    geoParallelFor(from.pageCount(), [&](std::size_t pi) {
        const GeoPage& page = *from.values().page(pi);
        const std::size_t n = from.pageElementCount(pi) * 3;
        const From* src = reinterpret_cast<const From*>(page.data());
        To* dst = to.pageForWriting<To>(pi);
        for (std::size_t k = 0; k < n; ++k) {
            dst[k] = static_cast<To>(page.isConstant() ? src[k % 3] : src[k]);
        }
    });
    to.compactConstantPages();
}

} // namespace

// ---------------------------------------------------------------------------------------------
// GeoDetail

GeoAttributeDesc
GeoDetail::positionDesc(bool precision64)
{
    return GeoAttributeDesc { kPositionName, precision64 ? eGeoStorageFloat64 : eGeoStorageFloat32, 3, eGeoTypeInfoPoint, false };
}

GeoDetail::GeoDetail()
    : _cache(std::make_shared<Cache>())
{
    _attributes[eGeoAttribClassPoint].push_back(std::make_shared<AttributeSlot>(GeoAttribute(positionDesc(false))));
}

GeoDetail::~GeoDetail() = default;

void
GeoDetail::checkClass(GeoAttribClassEnum cls)
{
    if (cls < eGeoAttribClassPoint || cls > eGeoAttribClassDetail) {
        throw std::invalid_argument("GeoDetail: unknown attribute class");
    }
}

void
GeoDetail::checkElementClass(GeoAttribClassEnum cls)
{
    if (cls < eGeoAttribClassPoint || cls > eGeoAttribClassPrimitive) {
        throw std::invalid_argument("GeoDetail: the class has no index map or groups");
    }
}

const GeoIndexMap&
GeoDetail::indexMap(GeoAttribClassEnum cls) const
{
    checkElementClass(cls);

    return _maps[cls];
}

std::size_t
GeoDetail::elementOffsetSize(GeoAttribClassEnum cls) const
{
    checkClass(cls);
    if (cls == eGeoAttribClassDetail) {
        return 1;
    }

    return std::size_t(_maps[cls].offsetSize());
}

const GeoAttribute&
GeoDetail::position() const
{
    return _attributes[eGeoAttribClassPoint].front()->attribute;
}

std::array<double, 3>
GeoDetail::pointPosition(GeoOffset point) const
{
    const GeoAttribute& p = position();
    if (_pointPrecision64) {
        std::array<double, 3> v;
        p.getTuple<double>(std::size_t(point), v);
        return v;
    }
    std::array<float, 3> v;
    p.getTuple<float>(std::size_t(point), v);

    return { v[0], v[1], v[2] };
}

std::size_t
GeoDetail::attributeCount(GeoAttribClassEnum cls) const
{
    checkClass(cls);

    return _attributes[cls].size();
}

const GeoAttribute&
GeoDetail::attribute(GeoAttribClassEnum cls,
                     std::size_t i) const
{
    checkClass(cls);

    return _attributes[cls].at(i)->attribute;
}

std::shared_ptr<GeoDetail::AttributeSlot>*
GeoDetail::findSlot(GeoAttribClassEnum cls,
                    std::string_view name)
{
    checkClass(cls);
    for (std::shared_ptr<AttributeSlot>& slot : _attributes[cls]) {
        if (slot->attribute.name() == name) {
            return &slot;
        }
    }

    return nullptr;
}

const GeoDetail::AttributeSlot*
GeoDetail::findSlot(GeoAttribClassEnum cls,
                    std::string_view name) const
{
    checkClass(cls);
    for (const std::shared_ptr<AttributeSlot>& slot : _attributes[cls]) {
        if (slot->attribute.name() == name) {
            return slot.get();
        }
    }

    return nullptr;
}

const GeoAttribute*
GeoDetail::findAttribute(GeoAttribClassEnum cls,
                         std::string_view name) const
{
    const AttributeSlot* slot = findSlot(cls, name);

    return slot ? &slot->attribute : nullptr;
}

const GeoElementGroup*
GeoDetail::findGroup(GeoAttribClassEnum cls,
                     std::string_view name) const
{
    checkClass(cls);
    if (cls == eGeoAttribClassDetail) {
        return nullptr;
    }
    GroupMap::const_iterator it = _groups[cls].find(name);

    return it == _groups[cls].end() ? nullptr : &it->second;
}

std::vector<std::string>
GeoDetail::groupNames(GeoAttribClassEnum cls) const
{
    checkClass(cls);
    std::vector<std::string> names;
    if (cls == eGeoAttribClassDetail) {
        return names;
    }
    for (const GroupMap::value_type& entry : _groups[cls]) {
        names.push_back(entry.first);
    }

    return names;
}

const GeoEdgeGroup*
GeoDetail::findEdgeGroup(std::string_view name) const
{
    EdgeGroupMap::const_iterator it = _edgeGroups.find(name);

    return it == _edgeGroups.end() ? nullptr : &it->second;
}

std::vector<std::string>
GeoDetail::edgeGroupNames() const
{
    std::vector<std::string> names;
    for (const EdgeGroupMap::value_type& entry : _edgeGroups) {
        names.push_back(entry.first);
    }

    return names;
}

std::shared_ptr<const GeoReverseTopology>
GeoDetail::reverseTopology() const
{
    std::lock_guard<std::mutex> lock(_cache->mutex);
    if (!_cache->reverse) {
        _cache->reverse = std::make_shared<const GeoReverseTopology>(_topology, vertexMap(), pointMap().offsetSize());
    }

    return _cache->reverse;
}

void
GeoDetail::warmPageHashes() const
{
    std::vector<const GeoPagedColumn*> columns;
    for (const AttributeList& list : _attributes) {
        for (const std::shared_ptr<AttributeSlot>& slot : list) {
            if (slot->hasCachedHash()) {
                continue;
            }
            columns.push_back(&slot->attribute.values());
            if (slot->attribute.isArray()) {
                columns.push_back(&slot->attribute.arrayPool());
            }
        }
    }
    for (const GeoAttribute* column : _topology.columns()) {
        columns.push_back(&column->values());
    }

    std::vector<const GeoPage*> pages;
    for (const GeoPagedColumn* column : columns) {
        for (std::size_t i = 0; i < column->pageCount(); ++i) {
            pages.push_back(column->page(i).get());
        }
    }
    if (pages.size() < kMinPagesForParallelHash) {
        return;
    }
    geoParallelFor(pages.size(), [&](std::size_t i) { pages[i]->contentHash(); }, kMinPagesForParallelHash);
}

U64
GeoDetail::contentHash() const
{
    if (!_building) {
        const U64 cached = _cache->hash.load(std::memory_order_acquire);
        if (cached != 0) {
            return cached;
        }
    }

    warmPageHashes();

    ContentHasher hasher;
    hasher.appendBool(_pointPrecision64);
    for (const GeoIndexMap& map : _maps) {
        hashIndexMap(hasher, map);
    }
    hasher.appendU64(_topology.contentHash());

    for (int cls = 0; cls < kGeoAttribClassCount; ++cls) {
        std::vector<const AttributeSlot*> sorted;
        sorted.reserve(_attributes[cls].size());
        for (const std::shared_ptr<AttributeSlot>& slot : _attributes[cls]) {
            sorted.push_back(slot.get());
        }
        std::sort(sorted.begin(), sorted.end(), [](const AttributeSlot* a, const AttributeSlot* b) {
            return a->attribute.name() < b->attribute.name();
        });
        hasher.appendU64(sorted.size());
        for (const AttributeSlot* slot : sorted) {
            hasher.appendU64(slot->contentHash());
        }
    }

    for (const GroupMap& groups : _groups) {
        hasher.appendU64(groups.size());
        for (const GroupMap::value_type& entry : groups) {
            hasher.appendString(entry.first);
            hasher.appendU64(entry.second.contentHash());
        }
    }
    hasher.appendU64(_edgeGroups.size());
    for (const EdgeGroupMap::value_type& entry : _edgeGroups) {
        hasher.appendString(entry.first);
        hasher.appendU64(entry.second.contentHash());
    }
    hasher.appendU64(_sharedDetails.size());
    for (const GeoDetailConstPtr& shared : _sharedDetails) {
        hasher.appendU64(shared->contentHash());
    }

    U64 h = hasher.finalize();
    if (h == 0) {
        h = 1;
    }
    if (!_building) {
        _cache->hash.store(h, std::memory_order_release);
    }

    return h;
}

std::size_t
GeoDetail::getSizeInBytes() const
{
    std::size_t bytes = sizeof(GeoDetail);
    for (const GeoIndexMap& map : _maps) {
        bytes += indexMapSizeInBytes(map);
    }
    bytes += _topology.getSizeInBytes();
    for (const AttributeList& list : _attributes) {
        for (const std::shared_ptr<AttributeSlot>& slot : list) {
            bytes += sizeof(AttributeSlot) + slot->attribute.getSizeInBytes();
        }
    }
    for (const GroupMap& groups : _groups) {
        for (const GroupMap::value_type& entry : groups) {
            bytes += entry.first.size() + entry.second.getSizeInBytes();
        }
    }
    for (const EdgeGroupMap::value_type& entry : _edgeGroups) {
        bytes += entry.first.size() + entry.second.getSizeInBytes();
    }
    for (const GeoDetailConstPtr& shared : _sharedDetails) {
        bytes += sizeof(GeoDetailConstPtr) + shared->getSizeInBytes();
    }

    return bytes;
}

// ---------------------------------------------------------------------------------------------
// GeoDetailBuilder

GeoDetailBuilder::GeoDetailBuilder()
{
    startBuilding();
}

GeoDetailBuilder::GeoDetailBuilder(GeoDetail source)
    : _detail(std::move(source))
{
    startBuilding();
}

void
GeoDetailBuilder::startBuilding()
{
    // A fresh cache keeps the source's adjacency, which stays valid until the topology changes,
    // without letting this builder's edits reach the source's cache.
    std::shared_ptr<GeoDetail::Cache> cache = std::make_shared<GeoDetail::Cache>();
    if (_detail._cache) {
        std::lock_guard<std::mutex> lock(_detail._cache->mutex);
        cache->reverse = _detail._cache->reverse;
    }
    _detail._cache = std::move(cache);
    _detail._building = true;
}

std::int32_t
GeoDetailBuilder::addSharedDetail(GeoDetailConstPtr shared)
{
    if (!shared) {
        throw std::invalid_argument("GeoDetailBuilder::addSharedDetail: null detail");
    }
    std::vector<GeoDetailConstPtr>& table = _detail._sharedDetails;
    for (std::size_t i = 0; i < table.size(); ++i) {
        if (table[i] == shared) {
            return std::int32_t(i);
        }
    }
    table.push_back(std::move(shared));

    return std::int32_t(table.size() - 1);
}

void
GeoDetailBuilder::pruneSharedDetails()
{
    std::vector<GeoDetailConstPtr>& table = _detail._sharedDetails;
    if (table.empty()) {
        return;
    }
    const GeoAttribute* refs = _detail.findAttribute(eGeoAttribClassPrimitive, kGeoPackedDetailAttr);
    if (!refs) {
        table.clear();
        return;
    }

    std::vector<std::int32_t> newIndex(table.size(), -1);
    std::vector<GeoOffset> live;
    _detail.primitiveMap().forEachLive([&](GeoIndex, GeoOffset offset) {
        if (_detail.primitiveType(offset) == eGeoPrimitivePackedGeometry) {
            const std::int32_t ref = refs->get<std::int32_t>(std::size_t(offset)) - 1;
            if (ref >= 0 && std::size_t(ref) < table.size()) {
                newIndex[std::size_t(ref)] = 0;
                live.push_back(offset);
            }
        }
    });

    std::vector<GeoDetailConstPtr> kept;
    for (std::size_t i = 0; i < table.size(); ++i) {
        if (newIndex[i] == 0) {
            newIndex[i] = std::int32_t(kept.size());
            kept.push_back(table[i]);
        }
    }
    if (kept.size() == table.size()) {
        return;
    }

    GeoAttribute* writable = attributeForWriting(eGeoAttribClassPrimitive, kGeoPackedDetailAttr);
    for (const GeoOffset offset : live) {
        const std::int32_t ref = writable->get<std::int32_t>(std::size_t(offset)) - 1;
        writable->set<std::int32_t>(std::size_t(offset), newIndex[std::size_t(ref)] + 1);
    }
    table = std::move(kept);
}

GeoDetail
GeoDetailBuilder::finish()
{
    validate();
    pruneSharedDetails();
    for (GeoDetail::AttributeList& list : _detail._attributes) {
        for (std::shared_ptr<GeoDetail::AttributeSlot>& slot : list) {
            if (!slot->frozen) {
                slot->attribute.endWriting();
                slot->hash.store(0, std::memory_order_relaxed);
                slot->frozen = true;
            }
        }
    }
    _detail._topology.endWriting();
    _detail._building = false;
    GeoDetail out = std::move(_detail);
    _detail = GeoDetail();
    startBuilding();

    return out;
}

void
GeoDetailBuilder::validate() const
{
    for (int cls = 0; cls < kGeoAttribClassCount; ++cls) {
        const std::size_t size = _detail.elementOffsetSize(GeoAttribClassEnum(cls));
        const GeoDetail::AttributeList& list = _detail._attributes[cls];
        for (std::size_t i = 0; i < list.size(); ++i) {
            const GeoAttribute& a = list[i]->attribute;
            if (a.size() != size) {
                throw std::logic_error("GeoDetailBuilder: attribute " + a.name() + " was resized");
            }
            if (a.name().empty()) {
                throw std::logic_error("GeoDetailBuilder: an attribute lost its name");
            }
            for (std::size_t j = 0; j < i; ++j) {
                if (list[j]->attribute.name() == a.name()) {
                    throw std::logic_error("GeoDetailBuilder: two attributes are named " + a.name());
                }
            }
        }
    }
    const GeoAttribute& p = _detail.position();
    if (!(p.desc() == GeoDetail::positionDesc(_detail._pointPrecision64))) {
        throw std::logic_error("GeoDetailBuilder: P lost its required description");
    }
}

GeoAttribute&
GeoDetailBuilder::writable(std::shared_ptr<GeoDetail::AttributeSlot>& slot)
{
    if (!geoIsSoleOwner(slot)) {
        slot = std::make_shared<GeoDetail::AttributeSlot>(slot->attribute);
    }
    if (slot->frozen) {
        slot->frozen = false;
        slot->hash.store(0, std::memory_order_relaxed);
    }

    return slot->attribute;
}

void
GeoDetailBuilder::resizeClass(GeoAttribClassEnum cls)
{
    const std::size_t size = _detail.elementOffsetSize(cls);
    for (std::shared_ptr<GeoDetail::AttributeSlot>& slot : _detail._attributes[cls]) {
        if (slot->attribute.size() != size) {
            writable(slot).resize(size);
        }
    }
    if (cls == eGeoAttribClassVertex) {
        _detail._topology.resizeVertices(size);
    } else if (cls == eGeoAttribClassPrimitive) {
        _detail._topology.resizePrimitives(size);
    }
}

void
GeoDetailBuilder::topologyChanged()
{
    // A copy of detail() may share the cache while it keeps the old topology.
    if (geoIsSoleOwner(_detail._cache)) {
        _detail._cache->reverse.reset();
    } else {
        _detail._cache = std::make_shared<GeoDetail::Cache>();
    }
}

GeoOffset
GeoDetailBuilder::appendPoints(GeoIndex count)
{
    GeoIndexMap& points = _detail._maps[eGeoAttribClassPoint];
    if (count <= 0) {
        return points.offsetSize();
    }
    const GeoOffset first = points.append(count);
    resizeClass(eGeoAttribClassPoint);

    return first;
}

GeoOffset
GeoDetailBuilder::appendPrimitive(GeoPrimitiveTypeEnum type,
                                  std::span<const GeoOffset> points)
{
    const std::int32_t count = std::int32_t(points.size());

    return appendPrimitives(type, std::span<const std::int32_t>(&count, 1), points);
}

GeoOffset
GeoDetailBuilder::appendPrimitives(GeoPrimitiveTypeEnum type,
                                   std::span<const std::int32_t> vertexCounts,
                                   std::span<const GeoOffset> points)
{
    if (!geoPrimitiveTypeIsValid(type)) {
        throw std::invalid_argument("GeoDetailBuilder::appendPrimitives: unknown primitive type");
    }
    std::size_t total = 0;
    for (std::int32_t c : vertexCounts) {
        if (c < 0 || (geoPrimitiveIsPacked(type) && c != 1)) {
            throw std::invalid_argument("GeoDetailBuilder::appendPrimitives: invalid vertex count");
        }
        total += std::size_t(c);
    }
    if (total != points.size()) {
        throw std::invalid_argument("GeoDetailBuilder::appendPrimitives: vertex counts do not add up to the point list");
    }
    const GeoIndexMap& pointMap = _detail._maps[eGeoAttribClassPoint];
    for (GeoOffset p : points) {
        if (!pointMap.isLive(p)) {
            throw std::invalid_argument("GeoDetailBuilder::appendPrimitives: a vertex uses a dead or missing point");
        }
    }
    GeoIndexMap& primitiveMap = _detail._maps[eGeoAttribClassPrimitive];
    if (vertexCounts.empty()) {
        return primitiveMap.offsetSize();
    }

    const GeoOffset firstPrimitive = primitiveMap.append(GeoOffset(vertexCounts.size()));
    const GeoOffset firstVertex = _detail._maps[eGeoAttribClassVertex].append(GeoOffset(total));
    resizeClass(eGeoAttribClassVertex);
    resizeClass(eGeoAttribClassPrimitive);

    std::vector<GeoOffset> vertexPrimitives(total);
    std::vector<GeoOffset> starts(vertexCounts.size());
    GeoOffset cursor = firstVertex;
    for (std::size_t i = 0; i < vertexCounts.size(); ++i) {
        const GeoOffset n = vertexCounts[i];
        starts[i] = n ? cursor : GeoIndexMap::kInvalidOffset;
        std::fill_n(vertexPrimitives.begin() + std::ptrdiff_t(cursor - firstVertex), n, firstPrimitive + GeoOffset(i));
        cursor += n;
    }
    GeoTopology& topology = _detail._topology;
    topology.setVertexPoints(firstVertex, points);
    topology.setVertexPrimitives(firstVertex, vertexPrimitives);
    topology.setPrimitives(firstPrimitive, type, starts, vertexCounts);
    topologyChanged();

    return firstPrimitive;
}

GeoIndex
GeoDetailBuilder::deletePrimitives(std::span<const GeoOffset> primitives,
                                   bool deleteUnusedPoints)
{
    std::shared_ptr<const GeoReverseTopology> reverse;
    if (deleteUnusedPoints) {
        reverse = _detail.reverseTopology();
    }
    GeoIndexMap& primitiveMap = _detail._maps[eGeoAttribClassPrimitive];
    GeoIndexMap& vertexMap = _detail._maps[eGeoAttribClassVertex];
    const GeoTopology& topology = _detail._topology;

    GeoIndex deleted = 0;
    std::vector<GeoOffset> candidates;
    for (GeoOffset primitive : primitives) {
        if (!primitiveMap.tombstone(primitive)) {
            continue;
        }
        ++deleted;
        const GeoVertexRange range = topology.primitiveVertices(primitive);
        if (range.count <= 0) {
            continue;
        }
        if (deleteUnusedPoints) {
            for (GeoOffset v = range.start; v < range.start + range.count; ++v) {
                candidates.push_back(topology.vertexPoint(v));
            }
        }
        vertexMap.tombstoneRange(range.start, range.start + range.count);
    }

    if (deleteUnusedPoints && !candidates.empty()) {
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
        std::vector<GeoOffset> unused;
        for (GeoOffset p : candidates) {
            const std::span<const GeoOffset> users = reverse->pointVertices(p);
            const bool used = std::any_of(users.begin(), users.end(), [&](GeoOffset v) { return vertexMap.isLive(v); });
            if (!used) {
                unused.push_back(p);
            }
        }
        _detail._maps[eGeoAttribClassPoint].tombstone(std::span<const GeoOffset>(unused));
    }
    if (deleted) {
        topologyChanged();
    }

    return deleted;
}

GeoIndex
GeoDetailBuilder::deletePoints(std::span<const GeoOffset> points,
                               GeoPointDeleteModeEnum mode)
{
    GeoIndexMap& pointMap = _detail._maps[eGeoAttribClassPoint];
    std::vector<GeoOffset> doomed;
    doomed.reserve(points.size());
    for (GeoOffset p : points) {
        if (pointMap.isLive(p)) {
            doomed.push_back(p);
        }
    }
    std::sort(doomed.begin(), doomed.end());
    doomed.erase(std::unique(doomed.begin(), doomed.end()), doomed.end());
    if (doomed.empty()) {
        return 0;
    }

    const std::shared_ptr<const GeoReverseTopology> reverse = _detail.reverseTopology();
    if (mode == eGeoPointDeleteKeepReferenced) {
        std::erase_if(doomed, [&](GeoOffset p) { return !reverse->pointVertices(p).empty(); });
        return pointMap.tombstone(std::span<const GeoOffset>(doomed));
    }

    GeoTopology& topology = _detail._topology;
    GeoIndexMap& vertexMap = _detail._maps[eGeoAttribClassVertex];
    GeoIndexMap& primitiveMap = _detail._maps[eGeoAttribClassPrimitive];
    const auto isDoomed = [&](GeoOffset p) { return std::binary_search(doomed.begin(), doomed.end(), p); };

    std::vector<GeoOffset> affected;
    for (GeoOffset p : doomed) {
        for (GeoOffset v : reverse->pointVertices(p)) {
            affected.push_back(topology.vertexPrimitive(v));
        }
    }
    std::sort(affected.begin(), affected.end());
    affected.erase(std::unique(affected.begin(), affected.end()), affected.end());

    // Survivors that are no longer contiguous move to fresh vertex offsets, since a primitive
    // owns one contiguous vertex run.
    struct Rehome {
        GeoOffset primitive;
        std::size_t firstSurvivor;
        std::size_t survivorCount;
    };
    std::vector<Rehome> rehomes;
    std::vector<GeoOffset> rehomedSurvivors;
    std::vector<GeoOffset> survivors;
    for (GeoOffset primitive : affected) {
        if (!primitiveMap.isLive(primitive)) {
            continue;
        }
        const GeoVertexRange range = topology.primitiveVertices(primitive);
        const GeoPrimitiveTypeEnum type = topology.primitiveType(primitive);
        const GeoOffset end = range.start + range.count;
        survivors.clear();
        for (GeoOffset v = range.start; v < end; ++v) {
            if (!isDoomed(topology.vertexPoint(v))) {
                survivors.push_back(v);
            }
        }
        if (GeoOffset(survivors.size()) < geoPrimitiveMinVertexCount(type)) {
            primitiveMap.tombstone(primitive);
            vertexMap.tombstoneRange(range.start, end);
            continue;
        }
        if (survivors.back() - survivors.front() + 1 == GeoOffset(survivors.size())) {
            vertexMap.tombstoneRange(range.start, survivors.front());
            vertexMap.tombstoneRange(survivors.back() + 1, end);
            topology.setPrimitive(primitive, type, GeoVertexRange { survivors.front(), GeoOffset(survivors.size()) });
            continue;
        }
        rehomes.push_back(Rehome { primitive, rehomedSurvivors.size(), survivors.size() });
        rehomedSurvivors.insert(rehomedSurvivors.end(), survivors.begin(), survivors.end());
    }

    if (!rehomes.empty()) {
        GeoOffset dst = vertexMap.append(GeoOffset(rehomedSurvivors.size()));
        resizeClass(eGeoAttribClassVertex);
        std::vector<GeoAttribute*> vertexAttributes;
        for (std::shared_ptr<GeoDetail::AttributeSlot>& slot : _detail._attributes[eGeoAttribClassVertex]) {
            vertexAttributes.push_back(&writable(slot));
        }
        for (const Rehome& rehome : rehomes) {
            const GeoVertexRange old = topology.primitiveVertices(rehome.primitive);
            const GeoOffset newStart = dst;
            for (std::size_t k = 0; k < rehome.survivorCount; ++k, ++dst) {
                const GeoOffset src = rehomedSurvivors[rehome.firstSurvivor + k];
                for (GeoAttribute* a : vertexAttributes) {
                    a->copyElement(std::size_t(dst), std::size_t(src));
                }
                topology.copyVertex(dst, src);
                for (GeoDetail::GroupMap::value_type& entry : _detail._groups[eGeoAttribClassVertex]) {
                    if (entry.second.contains(src)) {
                        entry.second.add(dst);
                    }
                }
            }
            topology.setPrimitive(rehome.primitive, topology.primitiveType(rehome.primitive),
                                  GeoVertexRange { newStart, GeoOffset(rehome.survivorCount) });
            vertexMap.tombstoneRange(old.start, old.start + old.count);
        }
    }

    const GeoIndex deleted = pointMap.tombstone(std::span<const GeoOffset>(doomed));
    topologyChanged();

    return deleted;
}

void
GeoDetailBuilder::defragment()
{
    std::array<std::vector<GeoOffset>, 3> newFromOld;
    std::array<bool, 3> moved {};
    for (int cls = 0; cls < 3; ++cls) {
        GeoIndexMap& map = _detail._maps[cls];
        GeoDefragmentResult result = map.defragment();
        if (result.remap.isIdentity()) {
            map = std::move(result.map);
            continue;
        }
        moved[cls] = true;
        const std::vector<std::size_t> sources = sourceOffsetsOf(result.remap);
        for (std::shared_ptr<GeoDetail::AttributeSlot>& slot : _detail._attributes[cls]) {
            GeoAttribute& a = writable(slot);
            a = a.gathered(sources);
        }
        if (cls == eGeoAttribClassVertex) {
            _detail._topology.gatherVertices(sources);
        } else if (cls == eGeoAttribClassPrimitive) {
            _detail._topology.gatherPrimitives(sources);
        }
        for (GeoDetail::GroupMap::value_type& entry : _detail._groups[cls]) {
            entry.second = entry.second.remapped(result.remap);
        }
        newFromOld[cls] = result.remap.newFromOldTable();
        map = std::move(result.map);
    }

    GeoTopology& topology = _detail._topology;
    if (moved[eGeoAttribClassPoint]) {
        topology.remapVertexPoints(newFromOld[eGeoAttribClassPoint]);
        for (GeoDetail::EdgeGroupMap::value_type& entry : _detail._edgeGroups) {
            entry.second = entry.second.remappedPoints(newFromOld[eGeoAttribClassPoint]);
        }
    }
    if (moved[eGeoAttribClassVertex]) {
        topology.remapPrimitiveVertexStarts(newFromOld[eGeoAttribClassVertex]);
    }
    if (moved[eGeoAttribClassPrimitive]) {
        topology.remapVertexPrimitives(newFromOld[eGeoAttribClassPrimitive]);
    }
    if (moved[0] || moved[1] || moved[2]) {
        topologyChanged();
    }
}

void
GeoDetailBuilder::setPointPrecision64(bool precision64)
{
    if (precision64 == _detail._pointPrecision64) {
        return;
    }
    std::shared_ptr<GeoDetail::AttributeSlot>& slot = _detail._attributes[eGeoAttribClassPoint].front();
    const GeoAttribute& from = slot->attribute;
    GeoAttribute to(GeoDetail::positionDesc(precision64), from.size());
    if (precision64) {
        convertTuples<float, double>(from, to);
    } else {
        convertTuples<double, float>(from, to);
    }
    slot = std::make_shared<GeoDetail::AttributeSlot>(std::move(to));
    slot->frozen = false;
    _detail._pointPrecision64 = precision64;
}

void
GeoDetailBuilder::setPointPosition(GeoOffset point,
                                   double x,
                                   double y,
                                   double z)
{
    GeoAttribute& p = positionForWriting();
    if (_detail._pointPrecision64) {
        const std::array<double, 3> v { x, y, z };
        p.setTuple<double>(std::size_t(point), v);
    } else {
        const std::array<float, 3> v { float(x), float(y), float(z) };
        p.setTuple<float>(std::size_t(point), v);
    }
}

GeoAttribute&
GeoDetailBuilder::positionForWriting()
{
    return writable(_detail._attributes[eGeoAttribClassPoint].front());
}

GeoAttribute&
GeoDetailBuilder::addAttribute(GeoAttribClassEnum cls,
                               GeoAttributeDesc desc)
{
    GeoDetail::checkClass(cls);
    if (desc.name.empty()) {
        throw std::invalid_argument("GeoDetailBuilder::addAttribute: empty name");
    }
    const bool isPosition = cls == eGeoAttribClassPoint && desc.name == GeoDetail::kPositionName;
    if (isPosition && !(desc == GeoDetail::positionDesc(_detail._pointPrecision64))) {
        throw std::invalid_argument("GeoDetailBuilder::addAttribute: P must keep its required description");
    }
    std::shared_ptr<GeoDetail::AttributeSlot>* existing = _detail.findSlot(cls, desc.name);
    if (existing && (*existing)->attribute.desc() == desc) {
        return writable(*existing);
    }
    std::shared_ptr<GeoDetail::AttributeSlot> slot = std::make_shared<GeoDetail::AttributeSlot>(GeoAttribute(std::move(desc), _detail.elementOffsetSize(cls)));
    slot->frozen = false;
    if (existing) {
        *existing = std::move(slot);
        return (*existing)->attribute;
    }
    _detail._attributes[cls].push_back(std::move(slot));

    return _detail._attributes[cls].back()->attribute;
}

GeoAttribute*
GeoDetailBuilder::attributeForWriting(GeoAttribClassEnum cls,
                                      std::string_view name)
{
    std::shared_ptr<GeoDetail::AttributeSlot>* slot = _detail.findSlot(cls, name);

    return slot ? &writable(*slot) : nullptr;
}

bool
GeoDetailBuilder::removeAttribute(GeoAttribClassEnum cls,
                                  std::string_view name)
{
    if (cls == eGeoAttribClassPoint && name == GeoDetail::kPositionName) {
        throw std::invalid_argument("GeoDetailBuilder::removeAttribute: P is required");
    }
    std::shared_ptr<GeoDetail::AttributeSlot>* slot = _detail.findSlot(cls, name);
    if (!slot) {
        return false;
    }
    GeoDetail::AttributeList& list = _detail._attributes[cls];
    list.erase(list.begin() + (slot - list.data()));

    return true;
}

bool
GeoDetailBuilder::renameAttribute(GeoAttribClassEnum cls,
                                  std::string_view from,
                                  std::string to)
{
    if (cls == eGeoAttribClassPoint && (from == GeoDetail::kPositionName || to == GeoDetail::kPositionName)) {
        throw std::invalid_argument("GeoDetailBuilder::renameAttribute: P cannot be renamed or replaced");
    }
    if (to.empty()) {
        throw std::invalid_argument("GeoDetailBuilder::renameAttribute: empty name");
    }
    std::shared_ptr<GeoDetail::AttributeSlot>* slot = _detail.findSlot(cls, from);
    if (!slot || _detail.findSlot(cls, to)) {
        return false;
    }
    writable(*slot).setName(std::move(to));

    return true;
}

GeoElementGroup&
GeoDetailBuilder::addGroup(GeoAttribClassEnum cls,
                           std::string name)
{
    GeoDetail::checkElementClass(cls);
    if (name.empty()) {
        throw std::invalid_argument("GeoDetailBuilder::addGroup: empty name");
    }

    return _detail._groups[cls].try_emplace(std::move(name)).first->second;
}

GeoElementGroup*
GeoDetailBuilder::groupForWriting(GeoAttribClassEnum cls,
                                  std::string_view name)
{
    GeoDetail::checkElementClass(cls);
    GeoDetail::GroupMap::iterator it = _detail._groups[cls].find(name);

    return it == _detail._groups[cls].end() ? nullptr : &it->second;
}

bool
GeoDetailBuilder::removeGroup(GeoAttribClassEnum cls,
                              std::string_view name)
{
    GeoDetail::checkElementClass(cls);
    GeoDetail::GroupMap::iterator it = _detail._groups[cls].find(name);
    if (it == _detail._groups[cls].end()) {
        return false;
    }
    _detail._groups[cls].erase(it);

    return true;
}

GeoEdgeGroup&
GeoDetailBuilder::addEdgeGroup(std::string name)
{
    if (name.empty()) {
        throw std::invalid_argument("GeoDetailBuilder::addEdgeGroup: empty name");
    }

    return _detail._edgeGroups.try_emplace(std::move(name)).first->second;
}

GeoEdgeGroup*
GeoDetailBuilder::edgeGroupForWriting(std::string_view name)
{
    GeoDetail::EdgeGroupMap::iterator it = _detail._edgeGroups.find(name);

    return it == _detail._edgeGroups.end() ? nullptr : &it->second;
}

bool
GeoDetailBuilder::removeEdgeGroup(std::string_view name)
{
    GeoDetail::EdgeGroupMap::iterator it = _detail._edgeGroups.find(name);
    if (it == _detail._edgeGroups.end()) {
        return false;
    }
    _detail._edgeGroups.erase(it);

    return true;
}

NATRON_NAMESPACE_EXIT
