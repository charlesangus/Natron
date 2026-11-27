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

#include "GeoVDBConvert.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

#include "Engine/Geo/GeoOpenVDBTools.h"

#include "Engine/Geo/GeoOpUtils.h"
#include "Engine/Geo/GeoPointTree.h"

NATRON_NAMESPACE_ENTER

namespace {

bool
checkVoxelSize(double voxelSize,
               std::string* error)
{
    if (std::isfinite(voxelSize) && (voxelSize > 0.)) {
        return true;
    }
    if (error) {
        *error = "The voxel size must be a positive number.";
    }

    return false;
}

bool
fail(std::string* error,
     const std::string& message)
{
    if (error) {
        *error = message;
    }

    return false;
}

openvdb::math::Transform::Ptr
linearTransform(double voxelSize)
{
    return openvdb::math::Transform::createLinearTransform(voxelSize);
}

openvdb::Vec3d
toVec(const std::array<double, 3>& p)
{
    return openvdb::Vec3d(p[0], p[1], p[2]);
}

double
attributeComponent(const GeoAttribute& a,
                   std::size_t offset,
                   int component)
{
    switch (a.storage()) {
    case eGeoStorageFloat32:
        return double(a.get<float>(offset, component));
    case eGeoStorageFloat64:
        return a.get<double>(offset, component);
    case eGeoStorageInt32:
        return double(a.get<std::int32_t>(offset, component));
    case eGeoStorageInt64:
        return double(a.get<std::int64_t>(offset, component));
    case eGeoStorageString:
        break;
    }

    return 0.;
}

std::int64_t
attributeInteger(const GeoAttribute& a,
                 std::size_t offset)
{
    if (a.storage() == eGeoStorageInt64) {
        return a.get<std::int64_t>(offset);
    }
    if (a.storage() == eGeoStorageInt32) {
        return a.get<std::int32_t>(offset);
    }

    return std::int64_t(std::llround(attributeComponent(a, offset, 0)));
}

openvdb::VecType
vectorTypeFor(GeoTypeInfoEnum typeInfo)
{
    switch (typeInfo) {
    case eGeoTypeInfoNormal:
        return openvdb::VEC_COVARIANT_NORMALIZE;
    case eGeoTypeInfoPoint:
        return openvdb::VEC_CONTRAVARIANT_ABSOLUTE;
    case eGeoTypeInfoVector:
        return openvdb::VEC_CONTRAVARIANT_RELATIVE;
    default:
        return openvdb::VEC_INVARIANT;
    }
}

struct AttributeSource {
    const GeoAttribute* attribute = nullptr;
    GeoAttribClassEnum cls = eGeoAttribClassPoint;
};

enum AttributeGridKindEnum {
    eAttributeGridNone = 0,
    eAttributeGridFloat,
    eAttributeGridVector,
    eAttributeGridInt32,
    eAttributeGridInt64,
};

AttributeGridKindEnum
attributeGridKind(const GeoAttribute& a)
{
    if (a.isArray()) {
        return eAttributeGridNone;
    }
    const bool isFloat = (a.storage() == eGeoStorageFloat32) || (a.storage() == eGeoStorageFloat64);
    if (isFloat && (a.tupleSize() == 1)) {
        return eAttributeGridFloat;
    }
    if (isFloat && (a.tupleSize() == 3)) {
        return eAttributeGridVector;
    }
    if ((a.storage() == eGeoStorageInt32) && (a.tupleSize() == 1)) {
        return eAttributeGridInt32;
    }
    if ((a.storage() == eGeoStorageInt64) && (a.tupleSize() == 1)) {
        return eAttributeGridInt64;
    }

    return eAttributeGridNone;
}

/**
 * The attributes of @p classes matching @p pattern, P aside, in name order; the first class
 * listed wins a name several of them hold. Attributes that no grid can hold are named in notes.
 **/
std::vector<std::pair<std::string, AttributeSource>>
matchAttributes(const GeoDetail& detail,
                std::initializer_list<GeoAttribClassEnum> classes,
                const std::string& pattern,
                std::vector<std::string>* notes)
{
    std::map<std::string, AttributeSource> found;
    if (pattern.find_first_not_of(" \t") == std::string::npos) {
        return {};
    }
    for (GeoAttribClassEnum cls : classes) {
        const std::size_t n = detail.attributeCount(cls);
        for (std::size_t i = 0; i < n; ++i) {
            const GeoAttribute& a = detail.attribute(cls, i);
            if ((a.name() == GeoDetail::kPositionName) || found.count(a.name()) || !geoNameMatchesPattern(pattern, a.name())) {
                continue;
            }
            if (attributeGridKind(a) == eAttributeGridNone) {
                notes->push_back("Attribute " + a.name() + " was skipped: only numbers and three-component float "
                                                           "tuples become grids.");
                continue;
            }
            found.emplace(a.name(), AttributeSource { &a, cls });
        }
    }

    return std::vector<std::pair<std::string, AttributeSource>>(found.begin(), found.end());
}

/**
 * A grid with @p index's active topology, each voxel holding value(n, world position), n being
 * the index the voxel holds.
 **/
template <typename GridT, typename IndexGridT, typename ValueFn>
typename GridT::Ptr
gridFromIndex(const IndexGridT& index,
              const ValueFn& value)
{
    using TreeT = typename GridT::TreeType;
    using ValueT = typename GridT::ValueType;
    typename TreeT::Ptr tree(new TreeT(index.tree(), openvdb::zeroVal<ValueT>(), openvdb::TopologyCopy()));
    const openvdb::math::Transform& xform = index.transform();
    openvdb::tree::LeafManager<TreeT> leaves(*tree);
    leaves.foreach ([&index, &xform, &value](typename TreeT::LeafNodeType& leaf, std::size_t /*leafIndex*/) {
        typename IndexGridT::ConstAccessor acc = index.getConstAccessor();
        for (auto it = leaf.beginValueOn(); it; ++it) {
            const openvdb::Coord ijk = it.getCoord();
            it.setValue(value(acc.getValue(ijk), xform.indexToWorld(ijk)));
        }
    });
    typename GridT::Ptr grid = GridT::create(tree);
    grid->setTransform(xform.copy());

    return grid;
}

/**
 * The attribute grid of @p source over @p index's topology: sample(n, position, components)
 * fills the components of the attribute at the n-th element under that voxel.
 **/
template <typename IndexGridT, typename SampleFn, typename IntegerFn>
openvdb::GridBase::Ptr
attributeGrid(const IndexGridT& index,
              const GeoAttribute& attribute,
              const SampleFn& sample,
              const IntegerFn& integer)
{
    using IndexT = typename IndexGridT::ValueType;
    switch (attributeGridKind(attribute)) {
    case eAttributeGridFloat:
        return gridFromIndex<openvdb::FloatGrid>(index, [&sample](IndexT n, const openvdb::Vec3d& p) {
            double c[3] = { 0., 0., 0. };
            sample(n, p, c);

            return float(c[0]);
        });
    case eAttributeGridVector: {
        openvdb::Vec3SGrid::Ptr grid = gridFromIndex<openvdb::Vec3SGrid>(index, [&sample](IndexT n, const openvdb::Vec3d& p) {
            double c[3] = { 0., 0., 0. };
            sample(n, p, c);

            return openvdb::Vec3s(float(c[0]), float(c[1]), float(c[2]));
        });
        grid->setVectorType(vectorTypeFor(attribute.typeInfo()));

        return grid;
    }
    case eAttributeGridInt32:
        return gridFromIndex<openvdb::Int32Grid>(index, [&integer](IndexT n, const openvdb::Vec3d& p) {
            return std::int32_t(integer(n, p));
        });
    case eAttributeGridInt64:
        return gridFromIndex<openvdb::Int64Grid>(index, [&integer](IndexT n, const openvdb::Vec3d& p) {
            return std::int64_t(integer(n, p));
        });
    case eAttributeGridNone:
        break;
    }

    return openvdb::GridBase::Ptr();
}

struct Triangle {
    GeoOffset primitive;
    std::array<GeoOffset, 3> vertices;
    std::array<GeoOffset, 3> points;
};

struct TriangleMesh {
    // Index-space positions by point offset.
    std::vector<openvdb::Vec3d> points;
    std::vector<Triangle> triangles;

    std::size_t polygonCount() const
    {
        return triangles.size();
    }

    std::size_t pointCount() const
    {
        return points.size();
    }

    std::size_t vertexCount(std::size_t /*n*/) const
    {
        return 3;
    }

    void getIndexSpacePoint(std::size_t n,
                            std::size_t v,
                            openvdb::Vec3d& pos) const
    {
        pos = points[std::size_t(triangles[n].points[v])];
    }
};

std::vector<Triangle>
triangulate(const GeoDetail& detail,
            bool* closed)
{
    std::vector<Triangle> triangles;
    std::vector<std::pair<GeoOffset, GeoOffset>> edges;
    detail.primitiveMap().forEachLive([&](GeoIndex /*index*/, GeoOffset prim) {
        if (detail.primitiveType(prim) != eGeoPrimitivePolygon) {
            return;
        }
        const GeoVertexRange range = detail.primitiveVertices(prim);
        if (range.count < 3) {
            return;
        }
        for (GeoOffset i = 0; i < range.count; ++i) {
            const GeoOffset a = detail.vertexPoint(range.start + i);
            const GeoOffset b = detail.vertexPoint(range.start + (i + 1) % range.count);
            if (a != b) {
                edges.emplace_back(std::min(a, b), std::max(a, b));
            }
        }
        const GeoOffset v0 = range.start;
        for (GeoOffset i = 1; i + 1 < range.count; ++i) {
            const GeoOffset v1 = range.start + i;
            const GeoOffset v2 = range.start + i + 1;
            triangles.push_back(Triangle { prim,
                                           { v0, v1, v2 },
                                           { detail.vertexPoint(v0), detail.vertexPoint(v1), detail.vertexPoint(v2) } });
        }
    });
    std::sort(edges.begin(), edges.end());
    *closed = true;
    for (std::size_t i = 0; i < edges.size();) {
        std::size_t j = i + 1;
        while ((j < edges.size()) && (edges[j] == edges[i])) {
            ++j;
        }
        if (j - i == 1) {
            *closed = false;

            break;
        }
        i = j;
    }

    return triangles;
}

void
addGrid(GeoVDBConvertResult* result,
        const std::string& name,
        const openvdb::GridBase::Ptr& grid)
{
    grid->setName(name);
    result->notes.push_back(geoVDBMemoryNote(name, *grid));
    result->grids.push_back(GeoVDBConvertGrid { name, grid });
}

typedef openvdb::FloatGrid::ValueConverter<openvdb::Index32>::Type ParticleIndexGrid;

struct ParticleList {
    using PosType = openvdb::Vec3R;

    std::vector<openvdb::Vec3R> positions;
    std::vector<openvdb::Real> radii;
    std::vector<openvdb::Vec3R> velocities;

    std::size_t size() const
    {
        return positions.size();
    }

    void getPos(std::size_t n,
                openvdb::Vec3R& xyz) const
    {
        xyz = positions[n];
    }

    void getPosRad(std::size_t n,
                   openvdb::Vec3R& xyz,
                   openvdb::Real& radius) const
    {
        xyz = positions[n];
        radius = radii[n];
    }

    void getPosRadVel(std::size_t n,
                      openvdb::Vec3R& xyz,
                      openvdb::Real& radius,
                      openvdb::Vec3R& velocity) const
    {
        xyz = positions[n];
        radius = radii[n];
        velocity = velocities.empty() ? openvdb::Vec3R(0.) : velocities[n];
    }

    void getAtt(std::size_t n,
                openvdb::Index32& att) const
    {
        att = openvdb::Index32(n);
    }
};

openvdb::math::Transform::Ptr
gridTransform(const std::shared_ptr<const openvdb::math::Transform>& reference,
              double voxelSize)
{
    return reference ? reference->copy() : linearTransform(voxelSize);
}

template <typename ToT, typename FromT>
ToT
convertValue(const FromT& v)
{
    constexpr bool fromVec = openvdb::VecTraits<FromT>::IsVec;
    constexpr bool toVec = openvdb::VecTraits<ToT>::IsVec;
    using ToElement = typename openvdb::VecTraits<ToT>::ElementType;
    if constexpr (std::is_same_v<ToT, bool>) {
        return !(v == openvdb::zeroVal<FromT>());
    } else if constexpr (toVec && fromVec) {
        return ToT(ToElement(v[0]), ToElement(v[1]), ToElement(v[2]));
    } else if constexpr (toVec) {
        return ToT(ToElement(v));
    } else if constexpr (fromVec) {
        return openvdb::zeroVal<ToT>();
    } else if constexpr (std::is_integral_v<ToT> && std::is_floating_point_v<FromT>) {
        return ToT(std::llround(v));
    } else {
        return ToT(v);
    }
}

/** A grid of another value type with @p from's topology, metadata and transform. */
template <typename ToGridT, typename FromGridT>
typename ToGridT::Ptr
convertGrid(const FromGridT& from)
{
    using ToT = typename ToGridT::ValueType;
    using ToTreeT = typename ToGridT::TreeType;
    typename ToTreeT::Ptr tree(new ToTreeT(from.tree(), convertValue<ToT>(from.background()), openvdb::TopologyCopy()));
    typename FromGridT::ConstAccessor acc = from.getConstAccessor();
    for (auto it = tree->beginValueAll(); it; ++it) {
        it.setValue(convertValue<ToT>(acc.getValue(it.getCoord())));
    }
    typename ToGridT::Ptr out = ToGridT::create(tree);
    out->insertMeta(from);
    out->setTransform(from.transform().copy());
    if (!std::is_floating_point_v<ToT>) {
        out->setGridClass(openvdb::GRID_UNKNOWN);
    }

    return out;
}

using ConvertibleGridTypes = openvdb::TypeList<openvdb::FloatGrid,
                                               openvdb::DoubleGrid,
                                               openvdb::Int32Grid,
                                               openvdb::Int64Grid,
                                               openvdb::BoolGrid,
                                               openvdb::Vec3SGrid,
                                               openvdb::Vec3DGrid,
                                               openvdb::Vec3IGrid>;

/** Where @p grid is inside: below @p offset for an SDF, above it for another scalar, active otherwise. */
openvdb::BoolGrid::Ptr
interiorMask(const openvdb::GridBase& grid,
             double offset)
{
    openvdb::BoolGrid::Ptr mask;
    const auto op = [&](const auto& g) {
        using GridT = std::decay_t<decltype(g)>;
        using ValueT = typename GridT::ValueType;
        if constexpr (std::is_floating_point_v<ValueT>) {
            if (g.getGridClass() == openvdb::GRID_LEVEL_SET) {
                mask = openvdb::tools::sdfInteriorMask(g, ValueT(offset));

                return;
            }
            mask = openvdb::BoolGrid::create(false);
            mask->setTransform(g.transform().copy());
            openvdb::BoolGrid::Accessor acc = mask->getAccessor();
            for (auto it = g.cbeginValueOn(); it; ++it) {
                if (!(double(*it) > offset)) {
                    continue;
                }
                if (it.isVoxelValue()) {
                    acc.setValueOn(it.getCoord(), true);
                } else {
                    mask->tree().fill(it.getBoundingBox(), true, true);
                }
            }
        } else {
            mask = openvdb::BoolGrid::create(false);
            mask->setTree(typename openvdb::BoolGrid::TreeType::Ptr(new openvdb::BoolTree(g.tree(), false, true, openvdb::TopologyCopy())));
            mask->setTransform(g.transform().copy());
        }
    };
    grid.apply<ConvertibleGridTypes>(op);

    return mask;
}

/** Reference polygons in a grid's index space, with their world-space triangle normals. */
struct ReferenceMesh {
    TriangleMesh mesh;
    std::vector<std::array<double, 3>> world;
    std::vector<openvdb::Vec3d> normals;
    // The closest triangle of every voxel within three of the surface.
    openvdb::Int32Grid::Ptr index;
    openvdb::FloatGrid::Ptr sdf;

    bool build(const GeoDetail& detail,
               const openvdb::math::Transform& xform)
    {
        bool closed = true;
        mesh.triangles = triangulate(detail, &closed);
        if (mesh.triangles.empty()) {
            return false;
        }
        world = geoPointPositions(detail);
        mesh.points.resize(world.size());
        for (std::size_t i = 0; i < world.size(); ++i) {
            mesh.points[i] = xform.worldToIndex(toVec(world[i]));
        }
        normals.reserve(mesh.triangles.size());
        for (const Triangle& t : mesh.triangles) {
            const openvdb::Vec3d a = toVec(world[std::size_t(t.points[0])]);
            openvdb::Vec3d n = (toVec(world[std::size_t(t.points[1])]) - a).cross(toVec(world[std::size_t(t.points[2])]) - a);
            const double length = n.length();
            normals.push_back(length > 0. ? n / length : openvdb::Vec3d(0.));
        }
        index = openvdb::Int32Grid::create(-1);
        sdf = openvdb::tools::meshToVolume<openvdb::FloatGrid>(mesh, xform, 3.f, 3.f, 0, index.get());
        sdf->setTransform(xform.copy());
        index->setTransform(xform.copy());

        return true;
    }

    /** The closest triangle to world position @p p, or -1 away from the surface. */
    std::int32_t triangleAt(const openvdb::Int32Grid::ConstAccessor& acc,
                            const openvdb::Vec3d& p) const
    {
        const openvdb::Coord ijk = openvdb::Coord::round(index->transform().worldToIndex(p));
        std::int32_t n = -1;
        if (!acc.probeValue(ijk, n) || (n < 0) || (std::size_t(n) >= mesh.triangles.size())) {
            return -1;
        }

        return n;
    }

    /** The barycentric weights of the point of triangle @p n closest to world position @p p. */
    openvdb::Vec3d weights(std::int32_t n,
                           const openvdb::Vec3d& p) const
    {
        const Triangle& t = mesh.triangles[std::size_t(n)];
        openvdb::Vec3d uvw;
        openvdb::math::closestPointOnTriangleToPoint(toVec(world[std::size_t(t.points[0])]), toVec(world[std::size_t(t.points[1])]),
                                                     toVec(world[std::size_t(t.points[2])]), p, uvw);

        return uvw;
    }

    /**
     * Voxels where the closest triangles of two face neighbours turn sharper than @p tolerance,
     * grown by two voxels: which side of an edge claims a voxel near it is arbitrary, so the
     * voxels holding the edge itself may otherwise be missed.
     **/
    openvdb::BoolTree::Ptr edgeMask(double tolerance) const
    {
        openvdb::BoolTree::Ptr mask(new openvdb::BoolTree(false));
        openvdb::BoolTree::Accessor out(*mask);
        openvdb::Int32Grid::ConstAccessor acc = index->getConstAccessor();
        static const openvdb::Coord kNeighbours[3] = { openvdb::Coord(1, 0, 0), openvdb::Coord(0, 1, 0), openvdb::Coord(0, 0, 1) };
        for (auto it = index->cbeginValueOn(); it; ++it) {
            const std::int32_t n = *it;
            if ((n < 0) || (std::size_t(n) >= normals.size())) {
                continue;
            }
            const openvdb::Coord ijk = it.getCoord();
            for (const openvdb::Coord& d : kNeighbours) {
                std::int32_t m = -1;
                if (!acc.probeValue(ijk + d, m) || (m == n) || (m < 0) || (std::size_t(m) >= normals.size())) {
                    continue;
                }
                if (normals[std::size_t(n)].dot(normals[std::size_t(m)]) < tolerance) {
                    out.setValueOn(ijk, true);
                    out.setValueOn(ijk + d, true);
                }
            }
        }
        openvdb::tools::dilateActiveValues(*mask, 2, openvdb::tools::NN_FACE, openvdb::tools::IGNORE_TILES);

        return mask;
    }
};

/** The attributes of class @p cls of @p reference matching @p pattern, P aside. */
std::vector<const GeoAttribute*>
transferredAttributes(const GeoDetail& reference,
                      GeoAttribClassEnum cls,
                      const std::string& pattern)
{
    std::vector<const GeoAttribute*> found;
    const std::size_t n = reference.attributeCount(cls);
    for (std::size_t i = 0; i < n; ++i) {
        const GeoAttribute& a = reference.attribute(cls, i);
        if ((a.name() != GeoDetail::kPositionName) && geoNameMatchesPattern(pattern, a.name())) {
            found.push_back(&a);
        }
    }

    return found;
}

/** Writes @p value to component @p c of @p dst's element @p offset, a float attribute. */
void
setFloatComponent(GeoAttribute& dst,
                  std::size_t offset,
                  int c,
                  double value)
{
    if (dst.storage() == eGeoStorageFloat64) {
        dst.set<double>(offset, value, c);
    } else {
        dst.set<float>(offset, float(value), c);
    }
}

bool
isFloatStorage(const GeoAttribute& a)
{
    return !a.isArray() && ((a.storage() == eGeoStorageFloat32) || (a.storage() == eGeoStorageFloat64));
}

/**
 * Moves the points flagged in @p sharpen whose voxel is on in @p edges to the point that best fits
 * the planes of the reference triangles crossing that voxel's edges, kept inside the voxel.
 **/
void
sharpenFeaturePoints(const ReferenceMesh& reference,
                     const openvdb::math::Transform& xform,
                     const openvdb::BoolTree& edges,
                     const std::vector<bool>& sharpen,
                     std::vector<openvdb::Vec3d>& points)
{
    std::vector<openvdb::Vec3s> indexPoints(reference.mesh.points.size());
    for (std::size_t i = 0; i < indexPoints.size(); ++i) {
        indexPoints[i] = openvdb::Vec3s(reference.mesh.points[i]);
    }
    std::vector<openvdb::Vec4I> polygons;
    std::vector<openvdb::Vec3d> planeNormals;
    polygons.reserve(reference.mesh.triangles.size());
    planeNormals.reserve(reference.mesh.triangles.size());
    for (const Triangle& t : reference.mesh.triangles) {
        polygons.emplace_back(openvdb::Index32(t.points[0]), openvdb::Index32(t.points[1]), openvdb::Index32(t.points[2]),
                              openvdb::util::INVALID_IDX);
        const openvdb::Vec3d a = reference.mesh.points[std::size_t(t.points[0])];
        openvdb::Vec3d n = (reference.mesh.points[std::size_t(t.points[1])] - a).cross(reference.mesh.points[std::size_t(t.points[2])] - a);
        const double length = n.length();
        planeNormals.push_back(length > 0. ? n / length : openvdb::Vec3d(0.));
    }
    openvdb::tools::MeshToVoxelEdgeData edgeData;
    edgeData.convert(indexPoints, polygons);
    openvdb::tools::MeshToVoxelEdgeData::Accessor acc = edgeData.getAccessor();
    const openvdb::BoolTree::ConstAccessor mask(edges);
    std::vector<openvdb::Vec3d> crossings;
    std::vector<openvdb::Index32> primitives;
    std::vector<openvdb::Vec3d> normals;
    for (std::size_t i = 0; i < points.size() && i < sharpen.size(); ++i) {
        if (!sharpen[i]) {
            continue;
        }
        const openvdb::Vec3d p = xform.worldToIndex(points[i]);
        const openvdb::Coord ijk = openvdb::Coord::floor(p);
        if (!mask.isValueOn(ijk)) {
            continue;
        }
        crossings.clear();
        primitives.clear();
        edgeData.getEdgeData(acc, ijk, crossings, primitives);
        if (crossings.size() < 2) {
            continue;
        }
        openvdb::Vec3d centre(0.);
        for (const openvdb::Vec3d& q : crossings) {
            centre += q;
        }
        centre /= double(crossings.size());
        normals.clear();
        for (std::size_t k = 0; k < crossings.size(); ++k) {
            crossings[k] -= centre;
            normals.push_back(std::size_t(primitives[k]) < planeNormals.size() ? planeNormals[std::size_t(primitives[k])] : openvdb::Vec3d(0.));
        }
        // Solved about the crossings' centre, the fit stays well conditioned far from the origin.
        openvdb::Vec3d feature = centre + openvdb::tools::findFeaturePoint(crossings, normals);
        const openvdb::Vec3d low = ijk.asVec3d();
        for (int c = 0; c < 3; ++c) {
            feature[c] = std::clamp(feature[c], low[c], low[c] + 1.);
        }
        points[i] = xform.indexToWorld(feature);
    }
} // sharpenFeaturePoints

template <typename GridT>
bool
meshGrid(const GridT& grid,
         const GeoSdfToPolygonsParams& params,
         const GeoDetail* referenceDetail,
         GeoDetailBuilder& out,
         std::string* error)
{
    const openvdb::math::Transform& xform = grid.transform();
    openvdb::tools::VolumeToMesh mesher(params.isovalue, std::clamp(params.adaptivity, 0., 1.));

    ReferenceMesh reference;
    const bool withReference = referenceDetail && reference.build(*referenceDetail, xform);
    openvdb::BoolTree::Ptr edges;
    if (withReference) {
        if constexpr (std::is_same_v<GridT, openvdb::FloatGrid>) {
            mesher.setRefGrid(reference.sdf, std::clamp(params.internalAdaptivity, 0., 1.));
        } else {
            mesher.setRefGrid(convertGrid<GridT>(*reference.sdf), std::clamp(params.internalAdaptivity, 0., 1.));
        }
        edges = reference.edgeMask(params.edgeTolerance);
        mesher.setAdaptivityMask(edges);
    }
    if (params.surfaceMask) {
        mesher.setSurfaceMask(interiorMask(*params.surfaceMask, params.maskOffset), params.invertSurfaceMask);
    }
    if (params.adaptivityField) {
        if (params.adaptivityField->isType<openvdb::FloatGrid>()) {
            mesher.setSpatialAdaptivity(params.adaptivityField);
        } else if (const openvdb::DoubleGrid* field = dynamic_cast<const openvdb::DoubleGrid*>(params.adaptivityField.get())) {
            mesher.setSpatialAdaptivity(convertGrid<openvdb::FloatGrid>(*field));
        } else {
            return fail(error, "The adaptivity field " + params.adaptivityField->getName() + " holds " + params.adaptivityField->valueType() + " values; it needs float or double ones.");
        }
    }
    mesher(grid);

    const std::size_t pointCount = mesher.pointListSize();
    if (pointCount == 0) {
        return true;
    }
    // volumeToMesh winds its polygons clockwise seen from outside, and it already reverses its
    // own inside test for grids that are not SDFs, so every grid class needs the opposite order.
    std::vector<std::int32_t> counts;
    std::vector<std::size_t> localPoints;
    std::vector<char> polygonFlags;
    std::vector<bool> onSurface(pointCount, false);
    const auto addPolygon = [&](const auto& polygon, int size, char flags) {
        counts.push_back(size);
        polygonFlags.push_back(flags);
        for (int k = 0; k < size; ++k) {
            const std::size_t point = std::size_t(polygon[(size - k) % size]);
            localPoints.push_back(point);
            if ((flags & openvdb::tools::POLYFLAG_EXTERIOR) && (point < pointCount)) {
                onSurface[point] = true;
            }
        }
    };
    for (std::size_t p = 0; p < mesher.polygonPoolListSize(); ++p) {
        const openvdb::tools::PolygonPool& pool = mesher.polygonPoolList()[p];
        for (std::size_t n = 0; n < pool.numQuads(); ++n) {
            addPolygon(pool.quad(n), 4, char(pool.quadFlags(n)));
        }
        for (std::size_t n = 0; n < pool.numTriangles(); ++n) {
            addPolygon(pool.triangle(n), 3, char(pool.triangleFlags(n)));
        }
    }
    if (counts.empty()) {
        return true;
    }

    std::vector<openvdb::Vec3d> points(pointCount);
    for (std::size_t i = 0; i < pointCount; ++i) {
        points[i] = openvdb::Vec3d(mesher.pointList()[i]);
    }
    if (withReference && params.sharpenFeatures) {
        sharpenFeaturePoints(reference, xform, *edges, onSurface, points);
    }
    const GeoOffset first = out.appendPoints(GeoIndex(pointCount));
    for (std::size_t i = 0; i < pointCount; ++i) {
        out.setPointPosition(first + GeoOffset(i), points[i].x(), points[i].y(), points[i].z());
    }
    std::vector<GeoOffset> vertexPoints(localPoints.size());
    for (std::size_t i = 0; i < localPoints.size(); ++i) {
        vertexPoints[i] = first + GeoOffset(localPoints[i]);
    }
    const GeoOffset firstPrimitive = out.appendPrimitives(eGeoPrimitivePolygon, counts, vertexPoints);
    const std::size_t primitiveCount = counts.size();
    const auto isSurface = [&](std::size_t i) {
        return (polygonFlags[i] & openvdb::tools::POLYFLAG_EXTERIOR) != 0;
    };

    if (withReference) {
        const auto addGroup = [&](GeoAttribClassEnum cls, const std::string& name, std::size_t count, GeoOffset firstElement,
                                  const std::function<bool(std::size_t)>& member) {
            if (name.empty()) {
                return;
            }
            GeoElementGroup& group = out.addGroup(cls, name);
            for (std::size_t i = 0; i < count; ++i) {
                if (member(i)) {
                    group.add(firstElement + GeoOffset(i));
                }
            }
        };
        addGroup(eGeoAttribClassPrimitive, params.surfaceGroup, primitiveCount, firstPrimitive, isSurface);
        addGroup(eGeoAttribClassPrimitive, params.interiorGroup, primitiveCount, firstPrimitive, [&](std::size_t i) {
            return !isSurface(i);
        });
        addGroup(eGeoAttribClassPrimitive, params.seamLineGroup, primitiveCount, firstPrimitive, [&](std::size_t i) {
            return (polygonFlags[i] & openvdb::tools::POLYFLAG_FRACTURE_SEAM) != 0;
        });
        const std::vector<std::uint8_t>& pointFlags = mesher.pointFlags();
        addGroup(eGeoAttribClassPoint, params.seamPoints, pointCount, first, [&](std::size_t i) {
            return (i < pointFlags.size()) && (pointFlags[i] != 0);
        });
    }

    const GeoDetail& detail = out.detail();
    if (params.computeVertexNormals) {
        std::vector<openvdb::Vec3d> faceNormals(primitiveCount);
        std::vector<std::vector<std::size_t>> around(pointCount);
        for (std::size_t i = 0; i < primitiveCount; ++i) {
            const GeoVertexRange range = detail.primitiveVertices(firstPrimitive + GeoOffset(i));
            openvdb::Vec3d n(0.);
            for (GeoOffset k = 0; k < range.count; ++k) {
                const std::size_t a = std::size_t(detail.vertexPoint(range.start + k) - first);
                const std::size_t b = std::size_t(detail.vertexPoint(range.start + (k + 1) % range.count) - first);
                n += points[a].cross(points[b]);
                around[a].push_back(i);
            }
            const double length = n.length();
            faceNormals[i] = length > 0. ? n / length : openvdb::Vec3d(0.);
        }
        GeoAttributeDesc desc;
        desc.name = "N";
        desc.tupleSize = 3;
        desc.typeInfo = eGeoTypeInfoNormal;
        GeoAttribute& normals = geoConformAttribute(out, eGeoAttribClassVertex, desc);
        for (std::size_t i = 0; i < primitiveCount; ++i) {
            const GeoVertexRange range = detail.primitiveVertices(firstPrimitive + GeoOffset(i));
            for (GeoOffset k = 0; k < range.count; ++k) {
                const std::size_t point = std::size_t(detail.vertexPoint(range.start + k) - first);
                openvdb::Vec3d sum(0.);
                for (const std::size_t other : around[point]) {
                    const bool sameSide = !withReference || (isSurface(other) == isSurface(i));
                    if (sameSide && (faceNormals[other].dot(faceNormals[i]) > 0.5)) {
                        sum += faceNormals[other];
                    }
                }
                const double length = sum.length();
                const openvdb::Vec3d n = length > 0. ? sum / length : faceNormals[i];
                for (int c = 0; c < 3; ++c) {
                    setFloatComponent(normals, std::size_t(range.start + k), c, n[c]);
                }
            }
        }
    }

    if (!referenceDetail || !params.transferSurfaceAttributes) {
        return true;
    }
    const std::vector<const GeoAttribute*> pointAttributes = transferredAttributes(*referenceDetail, eGeoAttribClassPoint, params.referenceAttributes);
    if (!pointAttributes.empty() && (referenceDetail->pointCount() > 0)) {
        const std::shared_ptr<const GeoPointTree> tree = referenceDetail->pointTree();
        std::vector<std::size_t> srcOffsets(pointCount);
        std::vector<std::size_t> dstOffsets(pointCount);
        for (std::size_t i = 0; i < pointCount; ++i) {
            const GeoIndex nearest = tree->nearest({ points[i].x(), points[i].y(), points[i].z() });
            srcOffsets[i] = std::size_t(referenceDetail->pointMap().offsetFromIndex(nearest));
            dstOffsets[i] = std::size_t(first) + i;
        }
        for (const GeoAttribute* src : pointAttributes) {
            GeoAttribute& dst = geoConformAttribute(out, eGeoAttribClassPoint, src->desc());
            geoCopyAttributeElements(*src, srcOffsets, dst, dstOffsets);
        }
    }

    const std::vector<const GeoAttribute*> primitiveAttributes = transferredAttributes(*referenceDetail, eGeoAttribClassPrimitive, params.referenceAttributes);
    const std::vector<const GeoAttribute*> vertexAttributes = transferredAttributes(*referenceDetail, eGeoAttribClassVertex, params.referenceAttributes);
    if (!withReference || (primitiveAttributes.empty() && vertexAttributes.empty())) {
        return true;
    }
    const openvdb::Int32Grid::ConstAccessor acc = reference.index->getConstAccessor();
    std::vector<std::size_t> srcPrimitives;
    std::vector<std::size_t> dstPrimitives;
    struct VertexSample {
        std::size_t vertex;
        std::int32_t triangle;
        openvdb::Vec3d weights;
    };
    std::vector<VertexSample> vertexSamples;
    for (std::size_t i = 0; i < primitiveCount; ++i) {
        if (!isSurface(i)) {
            continue;
        }
        const GeoOffset prim = firstPrimitive + GeoOffset(i);
        const GeoVertexRange range = detail.primitiveVertices(prim);
        openvdb::Vec3d centre(0.);
        for (GeoOffset k = 0; k < range.count; ++k) {
            const openvdb::Vec3d& p = points[std::size_t(detail.vertexPoint(range.start + k) - first)];
            centre += p;
            const std::int32_t n = reference.triangleAt(acc, p);
            if (n >= 0) {
                vertexSamples.push_back(VertexSample { std::size_t(range.start + k), n, reference.weights(n, p) });
            }
        }
        const std::int32_t n = reference.triangleAt(acc, centre / double(std::max<GeoOffset>(range.count, 1)));
        if (n >= 0) {
            srcPrimitives.push_back(std::size_t(reference.mesh.triangles[std::size_t(n)].primitive));
            dstPrimitives.push_back(std::size_t(prim));
        }
    }
    for (const GeoAttribute* src : primitiveAttributes) {
        GeoAttribute& dst = geoConformAttribute(out, eGeoAttribClassPrimitive, src->desc());
        geoCopyAttributeElements(*src, srcPrimitives, dst, dstPrimitives);
    }
    for (const GeoAttribute* src : vertexAttributes) {
        GeoAttribute& dst = geoConformAttribute(out, eGeoAttribClassVertex, src->desc());
        if (isFloatStorage(*src)) {
            for (const VertexSample& s : vertexSamples) {
                const Triangle& t = reference.mesh.triangles[std::size_t(s.triangle)];
                for (int c = 0; c < src->tupleSize(); ++c) {
                    double value = 0.;
                    for (int corner = 0; corner < 3; ++corner) {
                        value += s.weights[corner] * attributeComponent(*src, std::size_t(t.vertices[corner]), c);
                    }
                    setFloatComponent(dst, s.vertex, c, value);
                }
            }
            continue;
        }
        std::vector<std::size_t> srcVertices;
        std::vector<std::size_t> dstVertices;
        for (const VertexSample& s : vertexSamples) {
            const Triangle& t = reference.mesh.triangles[std::size_t(s.triangle)];
            const int corner = (s.weights[0] >= s.weights[1]) ? (s.weights[0] >= s.weights[2] ? 0 : 2) : (s.weights[1] >= s.weights[2] ? 1 : 2);
            srcVertices.push_back(std::size_t(t.vertices[corner]));
            dstVertices.push_back(s.vertex);
        }
        geoCopyAttributeElements(*src, srcVertices, dst, dstVertices);
    }

    return true;
} // meshGrid

template <typename ValueT>
ValueT
toleranceValue(double tolerance)
{
    if constexpr (openvdb::VecTraits<ValueT>::IsVec) {
        return ValueT(typename openvdb::VecTraits<ValueT>::ElementType(tolerance));
    } else if constexpr (std::is_integral_v<ValueT>) {
        return ValueT(std::llround(tolerance));
    } else {
        return ValueT(tolerance);
    }
}

/** Deactivates the voxels within @p tolerance of the background, then collapses constant regions. */
template <typename GridT>
void
pruneGrid(GridT& grid,
          double tolerance)
{
    using ValueT = typename GridT::ValueType;
    if constexpr (!std::is_same_v<ValueT, bool>) {
        const ValueT t = toleranceValue<ValueT>(std::max(tolerance, 0.));
        openvdb::tools::deactivate(grid.tree(), grid.background(), t);
        openvdb::tools::prune(grid.tree(), t);
    }
}

/** Runs fn with the sampler @p interpolation names, or the nearest voxel for integer grids. */
template <typename GridT, typename Fn>
void
withSampler(GeoVDBInterpolationEnum interpolation,
            const Fn& fn)
{
    if constexpr (std::is_integral_v<typename GridT::ValueType>) {
        fn(openvdb::tools::PointSampler());
    } else {
        switch (interpolation) {
        case eGeoVDBInterpolationPoint:
            fn(openvdb::tools::PointSampler());
            break;
        case eGeoVDBInterpolationLinear:
            fn(openvdb::tools::BoxSampler());
            break;
        case eGeoVDBInterpolationQuadratic:
            fn(openvdb::tools::QuadraticSampler());
            break;
        }
    }
}

template <typename GridT>
GeoVDBGridBaseConstPtr
resampled(const GridT& grid,
          const openvdb::math::Transform& target,
          const GeoVDBResampleParams& params)
{
    using ValueT = typename GridT::ValueType;
    ValueT background = grid.background();
    bool levelSet = false;
    if constexpr (std::is_floating_point_v<ValueT>) {
        levelSet = grid.getGridClass() == openvdb::GRID_LEVEL_SET;
        // The level set rebuild takes its band from the output background, which is in world units.
        const double from = grid.voxelSize()[0];
        if (levelSet && params.rebuildSdf && (from > 0.)) {
            background = ValueT(double(background) * target.voxelSize()[0] / from);
        }
    }
    typename GridT::Ptr out = GridT::create(background);
    out->insertMeta(grid);
    out->setTransform(target.copy());
    openvdb::util::NullInterrupter interrupter;
    withSampler<GridT>(params.interpolation, [&](auto sampler) {
        using SamplerT = decltype(sampler);
        if constexpr (std::is_floating_point_v<ValueT>) {
            if (levelSet && params.rebuildSdf) {
                openvdb::tools::resampleToMatch<SamplerT>(grid, *out, interrupter);

                return;
            }
        }
        openvdb::tools::doResampleToMatch<SamplerT>(grid, *out, interrupter);
    });
    pruneGrid(*out, params.pruneTolerance);

    return out;
}

template <typename GridT>
GeoVDBGridBaseConstPtr
transformed(const GridT& grid,
            const GeoVDBIndexTransform& xform,
            const GeoVDBResampleParams& params)
{
    using ValueT = typename GridT::ValueType;
    const double kRadians = openvdb::math::pi<double>() / 180.;
    const openvdb::tools::GridTransformer transformer(openvdb::Vec3R(xform.pivot[0], xform.pivot[1], xform.pivot[2]),
                                                      openvdb::Vec3R(xform.scale[0], xform.scale[1], xform.scale[2]),
                                                      openvdb::Vec3R(xform.rotate[0], xform.rotate[1], xform.rotate[2]) * kRadians,
                                                      openvdb::Vec3R(xform.translate[0], xform.translate[1], xform.translate[2]),
                                                      xform.order, xform.rotateOrder);
    typename GridT::Ptr out = GridT::create(grid.background());
    out->insertMeta(grid);
    out->setTransform(grid.transform().copy());
    withSampler<GridT>(params.interpolation, [&](auto sampler) {
        transformer.transformGrid<decltype(sampler), GridT>(grid, *out);
    });
    if constexpr (std::is_floating_point_v<ValueT>) {
        if (params.rebuildSdf && (grid.getGridClass() == openvdb::GRID_LEVEL_SET)) {
            const double voxel = grid.voxelSize()[0];
            const float halfWidth = float(voxel > 0. ? double(grid.background()) / voxel : 3.);
            typename GridT::Ptr rebuilt = openvdb::tools::levelSetRebuild(*out, 0.f, halfWidth, halfWidth);
            rebuilt->insertMeta(grid);
            out = rebuilt;
        }
    }
    if constexpr (openvdb::VecTraits<ValueT>::IsVec) {
        if (xform.transformVectors) {
            // The transformer's matrix maps index space to index space; the vectors are in world
            // units, so they need it seen from world space.
            openvdb::Mat4d matrix = transformer.getTransform();
            const openvdb::math::Transform& space = grid.transform();
            if (space.isLinear()) {
                const openvdb::Mat4d toWorld = space.baseMap()->getAffineMap()->getMat4();
                matrix = toWorld.inverse() * matrix * toWorld;
            } else {
                matrix.setTranslation(openvdb::Vec3d(0.));
            }
            openvdb::tools::transformVectors(*out, matrix);
        }
    }
    pruneGrid(*out, params.pruneTolerance);

    return out;
}

/** Runs fn on @p grid as the resampled type it is; false for other types. */
template <typename Fn>
bool
applyResampled(const openvdb::GridBase& grid,
               const Fn& fn)
{
    using ResampledTypes = openvdb::TypeList<openvdb::FloatGrid,
                                             openvdb::DoubleGrid,
                                             openvdb::Int32Grid,
                                             openvdb::Int64Grid,
                                             openvdb::Vec3SGrid,
                                             openvdb::Vec3DGrid>;

    return grid.apply<ResampledTypes>(fn);
}

} // anonymous namespace

GeoOffset
geoAppendVDBGrids(GeoDetailBuilder& builder,
                  const GeoVDBConvertResult& result)
{
    GeoOffset first = GeoIndexMap::kInvalidOffset;
    for (const GeoVDBConvertGrid& g : result.grids) {
        const GeoOffset prim = geoAppendVDB(builder, g.grid, g.name);
        if (first == GeoIndexMap::kInvalidOffset) {
            first = prim;
        }
    }

    return first;
}

double
geoVoxelSizeForCount(const GeoDetail& detail,
                     int count,
                     GeoVDBAxisEnum axis)
{
    if ((count < 1) || (detail.pointCount() == 0)) {
        return 0.;
    }
    std::array<double, 3> lo = { std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
                                 std::numeric_limits<double>::max() };
    std::array<double, 3> hi = { -lo[0], -lo[1], -lo[2] };
    detail.pointMap().forEachLive([&](GeoIndex /*index*/, GeoOffset point) {
        const std::array<double, 3> p = detail.pointPosition(point);
        for (int d = 0; d < 3; ++d) {
            lo[d] = std::min(lo[d], p[d]);
            hi[d] = std::max(hi[d], p[d]);
        }
    });
    double extent = 0.;
    if (axis == eGeoVDBAxisLongest) {
        for (int d = 0; d < 3; ++d) {
            extent = std::max(extent, hi[d] - lo[d]);
        }
    } else {
        extent = hi[int(axis)] - lo[int(axis)];
    }

    return extent > 0. ? extent / double(count) : 0.;
}

GeoVDBGridBaseConstPtr
geoFindVDB(const GeoDetail& detail,
           const std::string& pattern)
{
    GeoVDBGridBaseConstPtr found;
    detail.primitiveMap().forEachLive([&](GeoIndex /*index*/, GeoOffset prim) {
        if (!found && (detail.primitiveType(prim) == eGeoPrimitiveVDB) && geoNameMatchesPattern(pattern, std::string(geoVDBName(detail, prim)))) {
            found = geoVDBGrid(detail, prim);
        }
    });

    return found;
}

namespace {

struct PolygonBands {
    float exterior = 1.f;
    float interior = 1.f;
};

bool
polygonBands(const GeoPolygonsToSdfParams& params,
             double voxelSize,
             PolygonBands* bands,
             std::string* error)
{
    const double exterior = params.useWorldSpaceForBand ? params.exteriorBandWorld / voxelSize : params.exteriorBand;
    const double interior = params.useWorldSpaceForBand ? params.interiorBandWorld / voxelSize : params.interiorBand;
    if (!(exterior >= 0.) || !(interior >= 0.)) {
        return fail(error, "The band widths cannot be negative.");
    }
    // OpenVDB needs a band of at least one voxel on each side to find the surface's sign.
    bands->exterior = float(std::max(exterior, 1.));
    bands->interior = params.fillInterior ? std::numeric_limits<float>::max() : float(std::max(interior, 1.));

    return true;
}

double
transformVoxelSize(const std::shared_ptr<const openvdb::math::Transform>& transform,
                   double voxelSize)
{
    return transform ? transform->voxelSize()[0] : voxelSize;
}

double
pointsBandVoxels(const GeoPointsToSdfParams& params,
                 double voxelSize)
{
    return params.useWorldSpaceForBand ? params.bandWidthWorld / voxelSize : params.bandWidth;
}

/** The sphere radii of @p source's live points, in their order, and their offsets. */
ParticleList
particlesOf(const GeoDetail& source,
            const GeoPointsToSdfParams& params,
            bool withVelocity,
            std::vector<GeoOffset>* offsets)
{
    ParticleList particles;
    const GeoAttribute* pscale = source.findAttribute(eGeoAttribClassPoint, "pscale");
    if (pscale && ((pscale->isArray()) || (pscale->storage() == eGeoStorageString))) {
        pscale = nullptr;
    }
    const GeoAttribute* v = withVelocity ? source.findAttribute(eGeoAttribClassPoint, "v") : nullptr;
    if (v && (v->isArray() || (v->storage() == eGeoStorageString) || (v->tupleSize() < 3))) {
        v = nullptr;
    }
    source.pointMap().forEachLive([&](GeoIndex /*index*/, GeoOffset point) {
        if (offsets) {
            offsets->push_back(point);
        }
        particles.positions.push_back(toVec(source.pointPosition(point)));
        const double scale = pscale ? attributeComponent(*pscale, std::size_t(point), 0) : 1.;
        particles.radii.push_back(std::abs(scale * params.radiusScale));
        if (v) {
            particles.velocities.push_back(openvdb::Vec3R(attributeComponent(*v, std::size_t(point), 0),
                                                          attributeComponent(*v, std::size_t(point), 1),
                                                          attributeComponent(*v, std::size_t(point), 2))
                                           * params.velocityScale);
        }
    });

    return particles;
}

double
polygonsVoxelEstimate(const std::vector<Triangle>& triangles,
                      const std::vector<std::array<double, 3>>& world,
                      const PolygonBands& bands,
                      const GeoPolygonsToSdfParams& params,
                      double voxelSize)
{
    double area = 0.;
    double volume = 0.;
    for (const Triangle& t : triangles) {
        const openvdb::Vec3d a = toVec(world[std::size_t(t.points[0])]);
        const openvdb::Vec3d b = toVec(world[std::size_t(t.points[1])]);
        const openvdb::Vec3d c = toVec(world[std::size_t(t.points[2])]);
        area += 0.5 * (b - a).cross(c - a).length();
        volume += a.dot(b.cross(c)) / 6.;
    }
    const double layers = double(bands.exterior) + (params.fillInterior ? 0. : double(bands.interior));
    double voxels = area / (voxelSize * voxelSize) * layers;
    if (params.fillInterior) {
        voxels += std::abs(volume) / (voxelSize * voxelSize * voxelSize);
    }

    return voxels;
}

double
pointsVoxelEstimate(const ParticleList& particles,
                    const GeoPointsToSdfParams& params,
                    double voxelSize)
{
    const double band = pointsBandVoxels(params, voxelSize) * voxelSize;
    const double invDx = 1. / voxelSize;
    const double pi = openvdb::math::pi<double>();
    double volume = 0.;
    for (std::size_t i = 0; i < particles.size(); ++i) {
        const double r = particles.radii[i];
        if (r * invDx < params.minRadius) {
            continue;
        }
        const double outer = r + band;
        const double inner = std::max(r - band, 0.);
        volume += 4. / 3. * pi * (outer * outer * outer - inner * inner * inner);
        if (!particles.velocities.empty()) {
            volume += pi * (outer * outer - inner * inner) * particles.velocities[i].length();
        }
    }

    return volume * invDx * invDx * invDx;
}

/** Hands the estimate to @p estimated and makes it the first note. */
void
reportEstimate(double voxels,
               const GeoVDBEstimateCallback& estimated,
               GeoVDBConvertResult* result)
{
    const std::string note = geoVDBEstimateNote(voxels);
    if (estimated) {
        estimated(note);
    }
    result->notes.insert(result->notes.begin(), note);
}

} // anonymous namespace

double
geoPolygonsToSdfVoxelEstimate(const GeoDetail& source,
                              const GeoPolygonsToSdfParams& params)
{
    const double voxelSize = transformVoxelSize(params.transform, params.voxelSize);
    PolygonBands bands;
    if (!(voxelSize > 0.) || !polygonBands(params, voxelSize, &bands, nullptr)) {
        return 0.;
    }
    bool closed = true;

    return polygonsVoxelEstimate(triangulate(source, &closed), geoPointPositions(source), bands, params, voxelSize);
}

double
geoPointsToSdfVoxelEstimate(const GeoDetail& source,
                            const GeoPointsToSdfParams& params)
{
    const double voxelSize = transformVoxelSize(params.transform, params.voxelSize);
    if (!(voxelSize > 0.)) {
        return 0.;
    }
    const bool trails = params.footprint == eGeoParticleFootprintVelocityTrail;

    return pointsVoxelEstimate(particlesOf(source, params, trails, nullptr), params, voxelSize);
}

std::string
geoVDBEstimateNote(double voxels)
{
    std::ostringstream os;
    os << "Estimated before building: about " << std::llround(voxels) << " active voxels, " << std::fixed << std::setprecision(1)
       << voxels * sizeof(float) / (1024. * 1024.) << " MB per grid";

    return os.str();
}

bool
geoPolygonsToSdf(const GeoDetail& source,
                 const GeoPolygonsToSdfParams& params,
                 GeoVDBConvertResult* result,
                 std::string* error,
                 const GeoVDBEstimateCallback& estimated)
{
    geoInitializeOpenVDB();
    const double voxelSize = transformVoxelSize(params.transform, params.voxelSize);
    if (!checkVoxelSize(voxelSize, error)) {
        return false;
    }
    PolygonBands bands;
    if (!polygonBands(params, voxelSize, &bands, error)) {
        return false;
    }
    const openvdb::math::Transform::Ptr xform = gridTransform(params.transform, params.voxelSize);
    const bool unsignedDistance = params.buildDistance && params.unsignedDistanceField;

    bool closed = true;
    TriangleMesh mesh;
    mesh.triangles = triangulate(source, &closed);
    const std::vector<std::array<double, 3>> world = geoPointPositions(source);
    reportEstimate(polygonsVoxelEstimate(mesh.triangles, world, bands, params, voxelSize), estimated, result);
    if (mesh.triangles.empty()) {
        if (params.buildDistance) {
            openvdb::FloatGrid::Ptr empty = openvdb::FloatGrid::create(float(bands.exterior * voxelSize));
            empty->setTransform(xform->copy());
            empty->setGridClass(unsignedDistance ? openvdb::GRID_UNKNOWN : openvdb::GRID_LEVEL_SET);
            addGrid(result, params.distanceName, empty);
        }
        if (params.buildFog) {
            openvdb::FloatGrid::Ptr empty = openvdb::FloatGrid::create(0.f);
            empty->setTransform(xform->copy());
            empty->setGridClass(openvdb::GRID_FOG_VOLUME);
            addGrid(result, params.fogName, empty);
        }

        return true;
    }
    mesh.points.resize(world.size());
    for (std::size_t i = 0; i < world.size(); ++i) {
        mesh.points[i] = xform->worldToIndex(toVec(world[i]));
    }

    std::vector<std::string> attributeNotes;
    const std::vector<std::pair<std::string, AttributeSource>> attributes = matchAttributes(source, { eGeoAttribClassVertex, eGeoAttribClassPoint, eGeoAttribClassPrimitive },
                                                                                            params.attributes, &attributeNotes);
    const bool needSigned = params.buildFog || (params.buildDistance && !params.unsignedDistanceField) || (!unsignedDistance && !attributes.empty());
    if (!closed && (params.buildFog || (params.buildDistance && !params.unsignedDistanceField))) {
        result->notes.push_back("The mesh is not closed, so its inside is guessed: the sign of the distances and the "
                                "fog may leak through its holes. Turn Unsigned Distance Field on for distances that "
                                "need no inside, or close the mesh.");
    }

    openvdb::Int32Grid::Ptr index;
    if (!attributes.empty()) {
        index = openvdb::Int32Grid::create(0);
    }
    openvdb::FloatGrid::Ptr unsignedGrid;
    if (unsignedDistance) {
        unsignedGrid = openvdb::tools::meshToVolume<openvdb::FloatGrid>(mesh, *xform, bands.exterior, bands.exterior,
                                                                        openvdb::tools::UNSIGNED_DISTANCE_FIELD, index.get());
        unsignedGrid->setTransform(xform->copy());
        unsignedGrid->setGridClass(openvdb::GRID_UNKNOWN);
        addGrid(result, params.distanceName, unsignedGrid);
    }
    if (needSigned) {
        openvdb::FloatGrid::Ptr grid = openvdb::tools::meshToVolume<openvdb::FloatGrid>(mesh, *xform, bands.exterior, bands.interior, 0,
                                                                                        unsignedDistance ? nullptr : index.get());
        grid->setTransform(xform->copy());
        grid->setGridClass(openvdb::GRID_LEVEL_SET);
        const bool signedDistance = params.buildDistance && !unsignedDistance;
        if (signedDistance) {
            addGrid(result, params.distanceName, grid);
        }
        if (params.buildFog) {
            openvdb::FloatGrid::Ptr fog = signedDistance ? grid->deepCopy() : grid;
            float cutoff = float(bands.interior * voxelSize);
            if (params.fillInterior) {
                float deepest = 0.f;
                for (auto it = fog->tree().cbeginValueAll(); it; ++it) {
                    deepest = std::min(deepest, *it);
                }
                cutoff = std::max(-deepest, float(voxelSize));
            }
            openvdb::tools::sdfToFogVolume(*fog, cutoff);
            // sdfToFogVolume scales interior values by 1/cutoff without clamping, and it may lower
            // the cutoff it is given, so values deeper than the cutoff would read above one.
            openvdb::tree::LeafManager<openvdb::FloatTree> leaves(fog->tree());
            leaves.foreach ([](openvdb::FloatTree::LeafNodeType& leaf, std::size_t /*leafIndex*/) {
                for (auto it = leaf.beginValueOn(); it; ++it) {
                    if (*it > 1.f) {
                        it.setValue(1.f);
                    }
                }
            });
            for (auto it = fog->tree().beginValueOn(); it; ++it) {
                if (!it.isVoxelValue() && (*it > 1.f)) {
                    it.setValue(1.f);
                }
            }
            addGrid(result, params.fogName, fog);
        }
    }

    if (index) {
        index->setTransform(xform->copy());
        for (const auto& [name, src] : attributes) {
            const GeoAttribute& a = *src.attribute;
            const GeoAttribClassEnum cls = src.cls;
            const auto element = [cls](const Triangle& t, int corner) -> std::size_t {
                if (cls == eGeoAttribClassPrimitive) {
                    return std::size_t(t.primitive);
                }

                return std::size_t(cls == eGeoAttribClassVertex ? t.vertices[corner] : t.points[corner]);
            };
            const auto weights = [&mesh, &world](std::int32_t n, const openvdb::Vec3d& p) {
                const Triangle& t = mesh.triangles[std::size_t(n)];
                openvdb::Vec3d uvw;
                openvdb::math::closestPointOnTriangleToPoint(toVec(world[std::size_t(t.points[0])]), toVec(world[std::size_t(t.points[1])]),
                                                             toVec(world[std::size_t(t.points[2])]), p, uvw);

                return uvw;
            };
            const auto valid = [&mesh](std::int32_t n) {
                return (n >= 0) && (std::size_t(n) < mesh.triangles.size());
            };
            const auto sample = [&](std::int32_t n, const openvdb::Vec3d& p, double* out) {
                if (!valid(n)) {
                    return;
                }
                const Triangle& t = mesh.triangles[std::size_t(n)];
                const int components = std::min(a.tupleSize(), 3);
                if (cls == eGeoAttribClassPrimitive) {
                    for (int c = 0; c < components; ++c) {
                        out[c] = attributeComponent(a, std::size_t(t.primitive), c);
                    }

                    return;
                }
                const openvdb::Vec3d w = weights(n, p);
                for (int corner = 0; corner < 3; ++corner) {
                    const std::size_t e = element(t, corner);
                    for (int c = 0; c < components; ++c) {
                        out[c] += w[corner] * attributeComponent(a, e, c);
                    }
                }
            };
            const auto integer = [&](std::int32_t n, const openvdb::Vec3d& p) -> std::int64_t {
                if (!valid(n)) {
                    return 0;
                }
                const Triangle& t = mesh.triangles[std::size_t(n)];
                if (cls == eGeoAttribClassPrimitive) {
                    return attributeInteger(a, std::size_t(t.primitive));
                }
                const openvdb::Vec3d w = weights(n, p);
                const int corner = (w[0] >= w[1]) ? (w[0] >= w[2] ? 0 : 2) : (w[1] >= w[2] ? 1 : 2);

                return attributeInteger(a, element(t, corner));
            };
            if (const openvdb::GridBase::Ptr g = attributeGrid(*index, a, sample, integer)) {
                addGrid(result, name, g);
            }
        }
    }
    result->notes.insert(result->notes.end(), attributeNotes.begin(), attributeNotes.end());

    return true;
} // geoPolygonsToSdf

bool
geoPointsToSdf(const GeoDetail& source,
               const GeoPointsToSdfParams& params,
               GeoVDBConvertResult* result,
               std::string* error,
               const GeoVDBEstimateCallback& estimated)
{
    geoInitializeOpenVDB();
    const double voxelSize = transformVoxelSize(params.transform, params.voxelSize);
    if (!checkVoxelSize(voxelSize, error)) {
        return false;
    }
    const double band = pointsBandVoxels(params, voxelSize);
    if (!(band > 0.)) {
        return fail(error, "The band width must be positive.");
    }
    if (!(params.minRadius > 0.)) {
        return fail(error, "The minimum radius must be positive.");
    }
    const bool trailFootprint = params.footprint == eGeoParticleFootprintVelocityTrail;
    if (trailFootprint && !(params.trailSpacing > 0.)) {
        return fail(error, "The trail spacing must be positive.");
    }
    if (params.buildMask && !(params.maskWidth > 0.)) {
        return fail(error, "The mask width must be positive.");
    }
    const openvdb::math::Transform::Ptr xform = gridTransform(params.transform, params.voxelSize);
    const float background = float(band * voxelSize);

    std::vector<GeoOffset> offsets;
    const ParticleList particles = particlesOf(source, params, trailFootprint, &offsets);
    reportEstimate(pointsVoxelEstimate(particles, params, voxelSize), estimated, result);
    const bool trails = trailFootprint && !particles.velocities.empty();
    if (trailFootprint && !trails && !particles.positions.empty()) {
        result->notes.push_back("The points have no three-component v attribute, so there are no trails.");
    }

    std::vector<std::string> attributeNotes;
    const std::vector<std::pair<std::string, AttributeSource>> attributes = matchAttributes(source, { eGeoAttribClassPoint }, params.attributes, &attributeNotes);

    // ParticlesToLevelSet::getMinCount() keeps only one thread's count, so count the skipped
    // spheres here with the test it applies.
    const double invDx = 1. / voxelSize;
    std::size_t skipped = 0;
    for (const openvdb::Real radius : particles.radii) {
        if (radius * invDx < params.minRadius) {
            ++skipped;
        }
    }

    const auto emptyLevelSet = [&]() {
        openvdb::FloatGrid::Ptr grid = openvdb::FloatGrid::create(background);
        grid->setTransform(xform->copy());
        grid->setGridClass(openvdb::GRID_LEVEL_SET);

        return grid;
    };
    const auto rasterize = [&](openvdb::FloatGrid& grid, const ParticleList& list, double rmin, ParticleIndexGrid::Ptr* index) {
        if (list.positions.empty()) {
            return;
        }
        const auto run = [&](auto& raster) {
            raster.setRmin(rmin);
            raster.setRmax(std::numeric_limits<openvdb::Real>::max());
            if (trails) {
                raster.rasterizeTrails(list, params.trailSpacing);
            } else {
                raster.rasterizeSpheres(list);
            }
            raster.finalize(params.prune);
        };
        if (index) {
            openvdb::tools::ParticlesToLevelSet<openvdb::FloatGrid, openvdb::Index32> raster(grid);
            run(raster);
            *index = raster.attributeGrid();
        } else {
            openvdb::tools::ParticlesToLevelSet<openvdb::FloatGrid> raster(grid);
            run(raster);
        }
        if (!params.prune) {
            // Without pruning, the tiles deep inside large spheres are left with the outside's sign.
            openvdb::tools::signedFloodFill(grid.tree());
        }
    };

    ParticleIndexGrid::Ptr index;
    openvdb::FloatGrid::Ptr distance;
    if (params.buildDistance || params.buildFog || !attributes.empty()) {
        distance = emptyLevelSet();
        if (params.mergeInto) {
            const openvdb::FloatGrid* merge = dynamic_cast<const openvdb::FloatGrid*>(params.mergeInto.get());
            if (merge && (merge->getGridClass() == openvdb::GRID_LEVEL_SET)) {
                openvdb::util::NullInterrupter interrupter;
                openvdb::tools::resampleToMatch<openvdb::tools::BoxSampler>(*merge, *distance, interrupter);
            } else {
                result->notes.push_back("The reference VDB " + params.mergeInto->getName() + " is not a float SDF, so the spheres were not merged into it.");
            }
        }
        rasterize(*distance, particles, params.minRadius, attributes.empty() ? nullptr : &index);
    }
    if (skipped > 0) {
        std::ostringstream os;
        os << skipped << (skipped == 1 ? " point was" : " points were")
           << " skipped: their spheres are smaller than the minimum radius.";
        result->notes.push_back(os.str());
    }
    if (params.buildDistance) {
        addGrid(result, params.distanceName, distance);
    }
    if (params.buildFog) {
        openvdb::FloatGrid::Ptr fog = params.buildDistance ? distance->deepCopy() : distance;
        openvdb::tools::sdfToFogVolume(*fog);
        addGrid(result, params.fogName, fog);
    }
    if (params.buildMask) {
        // Only the spheres the distance grid draws get a shell, and the minimum radius scales with
        // them so a sphere near it keeps both walls of its shell rather than losing the inner one.
        ParticleList drawn;
        for (std::size_t i = 0; i < particles.size(); ++i) {
            if (particles.radii[i] * invDx < params.minRadius) {
                continue;
            }
            drawn.positions.push_back(particles.positions[i]);
            drawn.radii.push_back(particles.radii[i]);
            if (!particles.velocities.empty()) {
                drawn.velocities.push_back(particles.velocities[i]);
            }
        }
        const auto scaled = [&drawn](double scale) {
            ParticleList list = drawn;
            for (openvdb::Real& r : list.radii) {
                r *= scale;
            }

            return list;
        };
        const double outerScale = 1. + 0.5 * params.maskWidth;
        const double innerScale = 1. - 0.5 * params.maskWidth;
        openvdb::FloatGrid::Ptr mask = emptyLevelSet();
        rasterize(*mask, scaled(outerScale), params.minRadius * outerScale, nullptr);
        if (params.maskWidth < 2.) {
            openvdb::FloatGrid::Ptr inner = emptyLevelSet();
            rasterize(*inner, scaled(innerScale), params.minRadius * innerScale, nullptr);
            openvdb::tools::csgDifference(*mask, *inner);
        }
        openvdb::tools::sdfToFogVolume(*mask);
        addGrid(result, params.maskName, mask);
    }

    if (index) {
        index->setTransform(xform->copy());
        for (const auto& [name, src] : attributes) {
            const GeoAttribute& a = *src.attribute;
            const auto valid = [&offsets](openvdb::Index32 n) {
                return std::size_t(n) < offsets.size();
            };
            const auto sample = [&](openvdb::Index32 n, const openvdb::Vec3d& /*p*/, double* out) {
                if (!valid(n)) {
                    return;
                }
                for (int c = 0; c < std::min(a.tupleSize(), 3); ++c) {
                    out[c] = attributeComponent(a, std::size_t(offsets[n]), c);
                }
            };
            const auto integer = [&](openvdb::Index32 n, const openvdb::Vec3d& /*p*/) -> std::int64_t {
                return valid(n) ? attributeInteger(a, std::size_t(offsets[n])) : 0;
            };
            if (const openvdb::GridBase::Ptr g = attributeGrid(*index, a, sample, integer)) {
                addGrid(result, name, g);
            }
        }
    }
    result->notes.insert(result->notes.end(), attributeNotes.begin(), attributeNotes.end());

    return true;
} // geoPointsToSdf

bool
geoSdfToPolygons(const openvdb::GridBase& grid,
                 const GeoSdfToPolygonsParams& params,
                 const GeoDetail* reference,
                 GeoDetailBuilder& out,
                 std::string* error)
{
    if (const openvdb::FloatGrid* g = dynamic_cast<const openvdb::FloatGrid*>(&grid)) {
        return meshGrid(*g, params, reference, out, error);
    }
    if (const openvdb::DoubleGrid* g = dynamic_cast<const openvdb::DoubleGrid*>(&grid)) {
        return meshGrid(*g, params, reference, out, error);
    }

    return fail(error, "Only float and double grids can be turned into polygons; " + grid.getName() + " holds " + grid.valueType() + ".");
}

GeoVDBGridBaseConstPtr
geoSdfToFog(const openvdb::GridBase& grid,
            std::string* error)
{
    if (const openvdb::FloatGrid* g = dynamic_cast<const openvdb::FloatGrid*>(&grid)) {
        openvdb::FloatGrid::Ptr fog = g->deepCopy();
        openvdb::tools::sdfToFogVolume(*fog);

        return fog;
    }
    if (const openvdb::DoubleGrid* g = dynamic_cast<const openvdb::DoubleGrid*>(&grid)) {
        openvdb::DoubleGrid::Ptr fog = g->deepCopy();
        openvdb::tools::sdfToFogVolume(*fog);

        return fog;
    }
    fail(error, "Only float and double grids convert to fog; " + grid.getName() + " holds " + grid.valueType() + ".");

    return GeoVDBGridBaseConstPtr();
}

namespace {

template <typename GridT>
GeoVDBGridBaseConstPtr
fogToSdf(const GridT& fog,
         double isovalue)
{
    constexpr float kHalfWidth = 3.f;
    typename GridT::Ptr sdf = openvdb::tools::levelSetRebuild(fog, float(isovalue), kHalfWidth, kHalfWidth);
    const openvdb::Name name = fog.getName();
    sdf->insertMeta(fog);
    sdf->setName(name);
    sdf->setGridClass(openvdb::GRID_LEVEL_SET);

    return sdf;
}

} // anonymous namespace

GeoVDBGridBaseConstPtr
geoFogToSdf(const openvdb::GridBase& grid,
            double isovalue,
            std::string* error)
{
    if (const openvdb::FloatGrid* g = dynamic_cast<const openvdb::FloatGrid*>(&grid)) {
        return fogToSdf(*g, isovalue);
    }
    if (const openvdb::DoubleGrid* g = dynamic_cast<const openvdb::DoubleGrid*>(&grid)) {
        return fogToSdf(*g, isovalue);
    }
    fail(error, "Only float and double grids convert to an SDF; " + grid.getName() + " holds " + grid.valueType() + ".");

    return GeoVDBGridBaseConstPtr();
}

GeoVDBGridBaseConstPtr
geoConvertVDBValues(const GeoVDBGridBaseConstPtr& grid,
                    GeoVDBValueTypeEnum type,
                    GeoVDBPrecisionEnum precision,
                    std::string* error)
{
    if (!grid || ((type == eGeoVDBValueTypeSame) && (precision == eGeoVDBPrecisionSame))) {
        return grid;
    }
    GeoVDBGridBaseConstPtr converted;
    const auto op = [&](const auto& g) {
        using GridT = std::decay_t<decltype(g)>;
        using ValueT = typename GridT::ValueType;
        using ElementT = typename openvdb::VecTraits<ValueT>::ElementType;
        GeoVDBValueTypeEnum target = type;
        if (target == eGeoVDBValueTypeSame) {
            if constexpr (std::is_same_v<ValueT, bool>) {
                target = eGeoVDBValueTypeBool;
            } else if constexpr (openvdb::VecTraits<ValueT>::IsVec) {
                target = eGeoVDBValueTypeVector;
            } else if constexpr (std::is_integral_v<ValueT>) {
                target = eGeoVDBValueTypeInteger;
            } else {
                target = eGeoVDBValueTypeFloat;
            }
        }
        const bool wide = (precision == eGeoVDBPrecisionSame) ? (sizeof(ElementT) == 8) : (precision == eGeoVDBPrecision64);
        switch (target) {
        case eGeoVDBValueTypeSame:
        case eGeoVDBValueTypeFloat:
            converted = wide ? GeoVDBGridBaseConstPtr(convertGrid<openvdb::DoubleGrid>(g)) : GeoVDBGridBaseConstPtr(convertGrid<openvdb::FloatGrid>(g));
            break;
        case eGeoVDBValueTypeInteger:
            converted = wide ? GeoVDBGridBaseConstPtr(convertGrid<openvdb::Int64Grid>(g)) : GeoVDBGridBaseConstPtr(convertGrid<openvdb::Int32Grid>(g));
            break;
        case eGeoVDBValueTypeVector:
            converted = wide ? GeoVDBGridBaseConstPtr(convertGrid<openvdb::Vec3DGrid>(g)) : GeoVDBGridBaseConstPtr(convertGrid<openvdb::Vec3SGrid>(g));
            break;
        case eGeoVDBValueTypeBool:
            converted = convertGrid<openvdb::BoolGrid>(g);
            break;
        }
        if (converted && (converted->type() == g.type())) {
            converted = grid;
        }
    };
    if (!grid->apply<ConvertibleGridTypes>(op)) {
        fail(error, grid->getName() + " holds " + grid->valueType() + " values, which cannot be converted.");
    }

    return converted;
}

GeoVDBGridBaseConstPtr
geoCleanupVDB(const openvdb::GridBase& grid,
              const GeoVDBCleanupParams& params)
{
    GeoVDBGridBaseConstPtr cleaned;
    const auto op = [&](const auto& g) {
        using GridT = std::decay_t<decltype(g)>;
        using ValueT = typename GridT::ValueType;
        typename GridT::Ptr copy = g.deepCopy();
        pruneGrid(*copy, params.pruneTolerance);
        if constexpr (std::is_floating_point_v<ValueT>) {
            if (copy->getGridClass() == openvdb::GRID_LEVEL_SET) {
                if (params.signedFloodFill) {
                    openvdb::tools::signedFloodFill(copy->tree());
                    if (params.activateInsideVoxels) {
                        const openvdb::BoolGrid::Ptr inside = openvdb::tools::sdfInteriorMask(*copy);
                        copy->tree().topologyUnion(inside->tree());
                    }
                }
            }
        }
        cleaned = copy;
    };
    if (!grid.apply<ConvertibleGridTypes>(op)) {
        return grid.copyGrid();
    }

    return cleaned;
}

GeoVDBGridBaseConstPtr
geoResampleVDB(const openvdb::GridBase& grid,
               const openvdb::math::Transform& target,
               const GeoVDBResampleParams& params,
               std::string* error)
{
    GeoVDBGridBaseConstPtr out;
    if (!applyResampled(grid, [&](const auto& g) {
            out = resampled(g, target, params);
        })) {
        fail(error, "Only float, double, vector and integer grids can be resampled; " + grid.getName() + " holds " + grid.valueType() + ".");
    }

    return out;
}

GeoVDBGridBaseConstPtr
geoTransformVDB(const openvdb::GridBase& grid,
                const GeoVDBIndexTransform& xform,
                const GeoVDBResampleParams& params,
                std::string* error)
{
    GeoVDBGridBaseConstPtr out;
    try {
        if (!applyResampled(grid, [&](const auto& g) {
                out = transformed(g, xform, params);
            })) {
            fail(error, "Only float, double, vector and integer grids can be transformed; " + grid.getName() + " holds " + grid.valueType() + ".");
        }
    } catch (const openvdb::Exception& e) {
        fail(error, e.what());
        out.reset();
    }

    return out;
}

std::shared_ptr<openvdb::math::Transform>
geoScaledVDBTransform(const openvdb::GridBase& grid,
                      double scale,
                      bool axisAligned)
{
    if (axisAligned) {
        openvdb::math::Transform::Ptr xform = linearTransform(grid.transform().voxelSize()[0] * scale);
        xform->postTranslate(grid.transform().indexToWorld(openvdb::Vec3d(0.)));

        return xform;
    }
    openvdb::math::Transform::Ptr xform = grid.transform().copy();
    xform->preScale(scale);

    return xform;
}

std::shared_ptr<openvdb::math::Transform>
geoVoxelSizedVDBTransform(const openvdb::GridBase& grid,
                          double voxelSize,
                          bool axisAligned)
{
    const double current = grid.transform().voxelSize()[0];

    return geoScaledVDBTransform(grid, current > 0. ? voxelSize / current : 1., axisAligned);
}

std::string
geoVDBMemoryNote(const std::string& name,
                 const openvdb::GridBase& grid)
{
    std::ostringstream os;
    os << name << ": " << grid.activeVoxelCount() << " active voxels, " << std::fixed << std::setprecision(1)
       << double(grid.memUsage()) / (1024. * 1024.) << " MB";

    return os.str();
}

NATRON_NAMESPACE_EXIT
