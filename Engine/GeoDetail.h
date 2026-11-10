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

#ifndef Engine_GeoDetail_h
#define Engine_GeoDetail_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Global/GlobalDefines.h"

#include "Engine/Geo/GeoAttribute.h"
#include "Engine/Geo/GeoGroup.h"
#include "Engine/Geo/GeoIndexMap.h"
#include "Engine/Geo/GeoTopology.h"

NATRON_NAMESPACE_ENTER

enum GeoAttribClassEnum {
    eGeoAttribClassPoint = 0,
    eGeoAttribClassVertex,
    eGeoAttribClassPrimitive,
    eGeoAttribClassDetail,
};

inline constexpr int kGeoAttribClassCount = 4;

/** @brief What deleting a point does to the primitives using it. */
enum GeoPointDeleteModeEnum {
    // Points still used by a live vertex are not deleted.
    eGeoPointDeleteKeepReferenced = 0,
    // The vertices using a deleted point are removed from their primitives, and a primitive
    // left with fewer than geoPrimitiveMinVertexCount() vertices is deleted.
    eGeoPointDeleteDegenerate,
};

class GeoDetail;
class GeoPointTree;
typedef std::shared_ptr<GeoDetail> GeoDetailPtr;
typedef std::shared_ptr<const GeoDetail> GeoDetailConstPtr;

/**
 * @brief A geometry detail in Houdini's model: one pool of points, vertices that each reference
 * a point, and primitives that each own a contiguous run of vertices. Each element class
 * (point, vertex, primitive) has an index map from dense element numbers to storage offsets
 * with tombstoned holes, and every class, plus the detail itself, carries named attributes over
 * paged copy-on-write columns. Point, vertex and primitive groups are paged bitsets over
 * offsets; edge groups are sets of point-offset pairs.
 *
 * The point attribute P always exists: float32 x3 with point type info, or float64 when
 * pointPrecision64() is set.
 *
 * Copying is cheap (one pointer per attribute and group, no page is copied) and copies share
 * every page. A detail is never modified in place: GeoDetailBuilder takes a copy and detaches
 * only what it writes. Const members are thread-safe; contentHash() and reverseTopology() are
 * computed once and cached, shared with copies.
 *
 * getSizeInBytes() counts every page this detail references as if it owned it, whatever else
 * shares it, so it depends only on this detail's content: a cache entry's cost is recorded at
 * insert and re-queried at destroy, and the two must agree while use counts change in between.
 **/
class GeoDetail {
public:
    static constexpr const char* kPositionName = "P";

    static GeoAttributeDesc positionDesc(bool precision64);

    GeoDetail();
    GeoDetail(const GeoDetail&) = default;
    GeoDetail(GeoDetail&&) noexcept = default;
    GeoDetail& operator=(const GeoDetail&) = default;
    GeoDetail& operator=(GeoDetail&&) noexcept = default;
    ~GeoDetail();

    GeoIndex pointCount() const
    {
        return _maps[eGeoAttribClassPoint].size();
    }

    GeoIndex vertexCount() const
    {
        return _maps[eGeoAttribClassVertex].size();
    }

    GeoIndex primitiveCount() const
    {
        return _maps[eGeoAttribClassPrimitive].size();
    }

    /** @brief Throws std::invalid_argument for the detail class, which has exactly one element. */
    const GeoIndexMap& indexMap(GeoAttribClassEnum cls) const;

    const GeoIndexMap& pointMap() const
    {
        return _maps[eGeoAttribClassPoint];
    }

    const GeoIndexMap& vertexMap() const
    {
        return _maps[eGeoAttribClassVertex];
    }

    const GeoIndexMap& primitiveMap() const
    {
        return _maps[eGeoAttribClassPrimitive];
    }

    /** @brief Size of the class's attribute columns, tombstones included; 1 for the detail class. */
    std::size_t elementOffsetSize(GeoAttribClassEnum cls) const;

    const GeoTopology& topology() const
    {
        return _topology;
    }

    GeoOffset vertexPoint(GeoOffset vertex) const
    {
        return _topology.vertexPoint(vertex);
    }

    GeoOffset vertexPrimitive(GeoOffset vertex) const
    {
        return _topology.vertexPrimitive(vertex);
    }

    GeoPrimitiveTypeEnum primitiveType(GeoOffset primitive) const
    {
        return _topology.primitiveType(primitive);
    }

    GeoVertexRange primitiveVertices(GeoOffset primitive) const
    {
        return _topology.primitiveVertices(primitive);
    }

    bool pointPrecision64() const
    {
        return _pointPrecision64;
    }

    const GeoAttribute& position() const;

    std::array<double, 3> pointPosition(GeoOffset point) const;

    /** @brief Attributes of a class in creation order; P is always the first point attribute. */
    std::size_t attributeCount(GeoAttribClassEnum cls) const;
    const GeoAttribute& attribute(GeoAttribClassEnum cls, std::size_t i) const;
    const GeoAttribute* findAttribute(GeoAttribClassEnum cls, std::string_view name) const;

    /** @brief Point, vertex or primitive groups; nullptr when absent or for the detail class. */
    const GeoElementGroup* findGroup(GeoAttribClassEnum cls, std::string_view name) const;
    std::vector<std::string> groupNames(GeoAttribClassEnum cls) const;

    const GeoEdgeGroup* findEdgeGroup(std::string_view name) const;
    std::vector<std::string> edgeGroupNames() const;

    /**
     * @brief The immutable details that packed geometry primitives embed, one entry per distinct
     * pointer. Primitives refer to an entry by index (see GeoPacked.h); contentHash() and
     * getSizeInBytes() cover each entry once.
     **/
    std::span<const GeoDetailConstPtr> sharedDetails() const
    {
        return _sharedDetails;
    }

    /** @brief Built on first use; shared by copies until a builder changes the topology. */
    std::shared_ptr<const GeoReverseTopology> reverseTopology() const;

    /**
     * @brief The k-d tree over the live point positions, built on first use and held by this
     * detail's cache, so it is freed with the detail. Shared by copies and handed on to details
     * built from this one, which rebuild it only once P or the point numbering differ.
     **/
    std::shared_ptr<const GeoPointTree> pointTree() const;

    /**
     * @brief Combined from per-page hashes of every attribute, topology column and group, the
     * index maps' tombstones and P's precision. Equal content hashes equal whatever the
     * attributes' creation order. Cached per detail and per attribute. Never 0.
     **/
    U64 contentHash() const;

    std::size_t getSizeInBytes() const;

private:
    friend class GeoDetailBuilder;

    struct AttributeSlot;
    struct Cache;

    typedef std::vector<std::shared_ptr<AttributeSlot>> AttributeList;
    typedef std::map<std::string, GeoElementGroup, std::less<>> GroupMap;
    typedef std::map<std::string, GeoEdgeGroup, std::less<>> EdgeGroupMap;

    static void checkClass(GeoAttribClassEnum cls);
    static void checkElementClass(GeoAttribClassEnum cls);

    std::shared_ptr<AttributeSlot>* findSlot(GeoAttribClassEnum cls, std::string_view name);
    const AttributeSlot* findSlot(GeoAttribClassEnum cls, std::string_view name) const;
    void warmPageHashes() const;

    std::array<GeoIndexMap, 3> _maps;
    GeoTopology _topology;
    std::array<AttributeList, kGeoAttribClassCount> _attributes;
    std::array<GroupMap, 3> _groups;
    EdgeGroupMap _edgeGroups;
    std::vector<GeoDetailConstPtr> _sharedDetails;
    bool _pointPrecision64 = false;
    // Set while a builder owns this detail: nothing it computes is cached, since attributes
    // handed out for writing may still change.
    bool _building = false;
    std::shared_ptr<Cache> _cache;
};

/**
 * @brief The only way to change a detail. Starts from an empty detail or a copy of one, which
 * shares every page; each call detaches only the pages, attributes and groups it writes.
 *
 * References returned by addAttribute(), attributeForWriting(), addGroup() and friends stay
 * valid until the attribute or group is removed or replaced, the builder is finished, or the
 * attribute's class is defragmented (attributes only). Writing through them must not resize an
 * attribute or rename it; finish() checks both. Do not copy detail() while holding them: the
 * copy would share what they write. Copy finish()'s result instead.
 *
 * Element arguments are storage offsets. Not thread-safe; its bulk operations run in parallel
 * internally.
 **/
class GeoDetailBuilder {
public:
    GeoDetailBuilder();
    explicit GeoDetailBuilder(GeoDetail source);

    GeoDetailBuilder(const GeoDetailBuilder&) = delete;
    GeoDetailBuilder& operator=(const GeoDetailBuilder&) = delete;

    const GeoDetail& detail() const
    {
        return _detail;
    }

    /** @brief The finished detail; the builder starts over from an empty detail. */
    GeoDetail finish();

    /** @brief New points take every point attribute's default. Returns the first new offset. */
    GeoOffset appendPoints(GeoIndex count);

    /** @brief Returns the new primitive's offset. Throws std::invalid_argument on a dead point. */
    GeoOffset appendPrimitive(GeoPrimitiveTypeEnum type, std::span<const GeoOffset> points);

    /**
     * @brief Appends vertexCounts.size() primitives of one type; primitive i uses the next
     * vertexCounts[i] entries of @p points. Packed primitives take exactly one vertex. Returns
     * the first new primitive offset; the new primitives' offsets are consecutive.
     **/
    GeoOffset appendPrimitives(GeoPrimitiveTypeEnum type,
                               std::span<const std::int32_t> vertexCounts,
                               std::span<const GeoOffset> points);

    /** @brief Rewires a live vertex to another point. Throws std::invalid_argument on a dead vertex or point. */
    void setVertexPoint(GeoOffset vertex, GeoOffset point);

    /** @brief Returns how many points were deleted; dead and repeated offsets are ignored. */
    GeoIndex deletePoints(std::span<const GeoOffset> points,
                          GeoPointDeleteModeEnum mode = eGeoPointDeleteDegenerate);

    /**
     * @brief Deletes the primitives and their vertices. With @p deleteUnusedPoints, points that
     * no live vertex uses afterwards are deleted too. Returns how many primitives were deleted.
     **/
    GeoIndex deletePrimitives(std::span<const GeoOffset> primitives, bool deleteUnusedPoints = false);

    /**
     * @brief Closes every tombstoned hole: element offsets become dense again, attributes,
     * groups and references move with them, and pages that only shift by whole pages stay
     * shared.
     **/
    void defragment();

    /**
     * @brief Index of @p shared in sharedDetails(), appended when its pointer is not there yet.
     * finish() drops entries that no live packed geometry primitive refers to. Throws
     * std::invalid_argument for a null pointer.
     **/
    std::int32_t addSharedDetail(GeoDetailConstPtr shared);

    /** @brief Converts P between float32 and float64 storage. */
    void setPointPrecision64(bool precision64);

    void setPointPosition(GeoOffset point, double x, double y, double z);

    GeoAttribute& positionForWriting();

    /**
     * @brief The attribute, created with every element at its default when absent. An existing
     * attribute with a different description is replaced. Throws std::invalid_argument for an
     * empty name or for P with a description other than positionDesc(pointPrecision64()).
     **/
    GeoAttribute& addAttribute(GeoAttribClassEnum cls, GeoAttributeDesc desc);

    /** @brief nullptr when absent. */
    GeoAttribute* attributeForWriting(GeoAttribClassEnum cls, std::string_view name);

    /** @brief Throws std::invalid_argument for P. */
    bool removeAttribute(GeoAttribClassEnum cls, std::string_view name);

    /** @brief False when @p from is absent or @p to exists. Throws std::invalid_argument for P. */
    bool renameAttribute(GeoAttribClassEnum cls, std::string_view from, std::string to);

    /** @brief The group, created empty when absent. Throws std::invalid_argument for the detail class. */
    GeoElementGroup& addGroup(GeoAttribClassEnum cls, std::string name);
    GeoElementGroup* groupForWriting(GeoAttribClassEnum cls, std::string_view name);
    bool removeGroup(GeoAttribClassEnum cls, std::string_view name);

    GeoEdgeGroup& addEdgeGroup(std::string name);
    GeoEdgeGroup* edgeGroupForWriting(std::string_view name);
    bool removeEdgeGroup(std::string_view name);

private:
    GeoAttribute& writable(std::shared_ptr<GeoDetail::AttributeSlot>& slot);
    void resizeClass(GeoAttribClassEnum cls);
    void topologyChanged();
    void validate() const;
    void startBuilding();
    void pruneSharedDetails();

    GeoDetail _detail;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_GeoDetail_h
