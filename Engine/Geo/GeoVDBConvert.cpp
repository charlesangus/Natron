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

/** @brief Barycentric weights of @p q, a point of the triangle (a, b, c). */
std::array<double, 3>
barycentric(const openvdb::Vec3d& a,
            const openvdb::Vec3d& b,
            const openvdb::Vec3d& c,
            const openvdb::Vec3d& q)
{
    const openvdb::Vec3d ab = b - a;
    const openvdb::Vec3d ac = c - a;
    const openvdb::Vec3d aq = q - a;
    const double d00 = ab.dot(ab);
    const double d01 = ab.dot(ac);
    const double d11 = ac.dot(ac);
    const double d20 = aq.dot(ab);
    const double d21 = aq.dot(ac);
    const double denom = d00 * d11 - d01 * d01;
    if (!(std::abs(denom) > 0.)) {
        return { 1., 0., 0. };
    }
    const double v = (d11 * d20 - d01 * d21) / denom;
    const double w = (d00 * d21 - d01 * d20) / denom;

    return { 1. - v - w, v, w };
}

void
finishSurfaceGrid(openvdb::FloatGrid& grid,
                  GeoVDBVolumeKindEnum kind,
                  bool levelSet,
                  const std::string& name)
{
    grid.setGridClass(levelSet ? openvdb::GRID_LEVEL_SET : openvdb::GRID_UNKNOWN);
    if (levelSet && (kind == eGeoVDBVolumeFog)) {
        openvdb::tools::sdfToFogVolume(grid);
    }
    grid.setName(name);
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

template <typename GridT>
bool
meshGrid(const GridT& grid,
         const GeoSdfToPolygonsParams& params,
         const GeoDetail* reference,
         GeoDetailBuilder& out)
{
    std::vector<openvdb::Vec3s> points;
    std::vector<openvdb::Vec3I> triangles;
    std::vector<openvdb::Vec4I> quads;
    openvdb::tools::volumeToMesh(grid, points, triangles, quads, params.isovalue,
                                 std::clamp(params.adaptivity, 0., 1.));
    if (points.empty()) {
        return true;
    }
    // volumeToMesh winds its polygons clockwise seen from outside, and it already reverses its
    // own inside test for fog volumes, so every grid class needs the opposite order.

    const GeoOffset first = out.appendPoints(GeoIndex(points.size()));
    for (std::size_t i = 0; i < points.size(); ++i) {
        out.setPointPosition(first + GeoOffset(i), points[i].x(), points[i].y(), points[i].z());
    }
    std::vector<std::int32_t> counts;
    std::vector<GeoOffset> vertexPoints;
    counts.reserve(quads.size() + triangles.size());
    vertexPoints.reserve(quads.size() * 4 + triangles.size() * 3);
    for (const openvdb::Vec4I& q : quads) {
        counts.push_back(4);
        for (int k = 0; k < 4; ++k) {
            vertexPoints.push_back(first + GeoOffset(q[(4 - k) % 4]));
        }
    }
    for (const openvdb::Vec3I& t : triangles) {
        counts.push_back(3);
        for (int k = 0; k < 3; ++k) {
            vertexPoints.push_back(first + GeoOffset(t[(3 - k) % 3]));
        }
    }
    if (!counts.empty()) {
        out.appendPrimitives(eGeoPrimitivePolygon, counts, vertexPoints);
    }

    if (!reference || (reference->pointCount() == 0)) {
        return true;
    }
    std::vector<const GeoAttribute*> transfer;
    const std::size_t n = reference->attributeCount(eGeoAttribClassPoint);
    for (std::size_t i = 0; i < n; ++i) {
        const GeoAttribute& a = reference->attribute(eGeoAttribClassPoint, i);
        if ((a.name() != GeoDetail::kPositionName) && geoNameMatchesPattern(params.referenceAttributes, a.name())) {
            transfer.push_back(&a);
        }
    }
    if (transfer.empty()) {
        return true;
    }
    const std::shared_ptr<const GeoPointTree> tree = reference->pointTree();
    std::vector<std::size_t> srcOffsets(points.size());
    std::vector<std::size_t> dstOffsets(points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
        const GeoIndex nearest = tree->nearest({ double(points[i].x()), double(points[i].y()), double(points[i].z()) });
        srcOffsets[i] = std::size_t(reference->pointMap().offsetFromIndex(nearest));
        dstOffsets[i] = std::size_t(first) + i;
    }
    for (const GeoAttribute* src : transfer) {
        GeoAttribute& dst = geoConformAttribute(out, eGeoAttribClassPoint, src->desc());
        geoCopyAttributeElements(*src, srcOffsets, dst, dstOffsets);
    }

    return true;
}

template <typename GridT>
GeoVDBGridBaseConstPtr
resampled(const GridT& grid,
          const openvdb::math::Transform& target,
          GeoVDBInterpolationEnum interpolation)
{
    typename GridT::ValueType background = grid.background();
    if constexpr (std::is_floating_point_v<typename GridT::ValueType>) {
        // The level set rebuild takes its band from the output background, which is in world units.
        if (grid.getGridClass() == openvdb::GRID_LEVEL_SET) {
            const double from = grid.voxelSize()[0];
            const double to = target.voxelSize()[0];
            if (from > 0.) {
                background = typename GridT::ValueType(double(background) * to / from);
            }
        }
    }
    typename GridT::Ptr out = GridT::create(background);
    out->insertMeta(grid);
    out->setTransform(target.copy());
    switch (interpolation) {
    case eGeoVDBInterpolationPoint:
        openvdb::tools::resampleToMatch<openvdb::tools::PointSampler>(grid, *out);
        break;
    case eGeoVDBInterpolationLinear:
        openvdb::tools::resampleToMatch<openvdb::tools::BoxSampler>(grid, *out);
        break;
    case eGeoVDBInterpolationQuadratic:
        openvdb::tools::resampleToMatch<openvdb::tools::QuadraticSampler>(grid, *out);
        break;
    }

    return out;
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

bool
geoPolygonsToSdf(const GeoDetail& source,
                 const GeoPolygonsToSdfParams& params,
                 GeoVDBConvertResult* result,
                 std::string* error)
{
    geoInitializeOpenVDB();
    if (!checkVoxelSize(params.voxelSize, error)) {
        return false;
    }
    if (!(params.exteriorBand >= 0.) || !(params.interiorBand >= 0.)) {
        return fail(error, "The band widths cannot be negative.");
    }
    const openvdb::math::Transform::Ptr xform = linearTransform(params.voxelSize);
    // OpenVDB needs a band of at least one voxel on each side to find the surface's sign.
    const float exterior = float(std::max(params.exteriorBand, 1.));
    const float interior = params.fillInterior ? std::numeric_limits<float>::max() : float(std::max(params.interiorBand, 1.));

    bool closed = true;
    TriangleMesh mesh;
    mesh.triangles = triangulate(source, &closed);
    if (mesh.triangles.empty()) {
        openvdb::FloatGrid::Ptr empty = openvdb::FloatGrid::create(float(exterior * params.voxelSize));
        empty->setTransform(xform);
        finishSurfaceGrid(*empty, params.kind, true, params.name);
        addGrid(result, params.name, empty);

        return true;
    }
    const std::vector<std::array<double, 3>> world = geoPointPositions(source);
    mesh.points.resize(world.size());
    for (std::size_t i = 0; i < world.size(); ++i) {
        mesh.points[i] = xform->worldToIndex(toVec(world[i]));
    }

    const bool levelSet = closed || (params.kind == eGeoVDBVolumeFog);
    int flags = 0;
    if (!levelSet) {
        flags |= openvdb::tools::UNSIGNED_DISTANCE_FIELD;
        result->notes.push_back("The mesh is not closed, so it has no inside: the distances are unsigned and Fill "
                                "Interior has no effect. Use Fog output, turn Fill Interior off, or close the mesh "
                                "for a signed distance field.");
    } else if (!closed) {
        result->notes.push_back("The mesh is not closed, so what counts as its inside is guessed and the fog may "
                                "leak through its holes.");
    }
    std::vector<std::string> attributeNotes;
    const std::vector<std::pair<std::string, AttributeSource>> attributes = matchAttributes(source, { eGeoAttribClassVertex, eGeoAttribClassPoint, eGeoAttribClassPrimitive },
                                                                                            params.attributes, &attributeNotes);

    openvdb::Int32Grid::Ptr index;
    if (!attributes.empty()) {
        index = openvdb::Int32Grid::create(0);
    }
    openvdb::FloatGrid::Ptr grid = openvdb::tools::meshToVolume<openvdb::FloatGrid>(
        mesh, *xform, exterior, levelSet ? interior : exterior, flags, index.get());
    grid->setTransform(xform->copy());
    finishSurfaceGrid(*grid, params.kind, levelSet, params.name);
    addGrid(result, params.name, grid);

    if (index) {
        index->setTransform(xform->copy());
        for (const auto& [name, src] : attributes) {
            const GeoAttribute& a = *src.attribute;
            const GeoAttribClassEnum cls = src.cls;
            const auto element = [&source, cls](const Triangle& t, int corner) -> std::size_t {
                if (cls == eGeoAttribClassPrimitive) {
                    return std::size_t(t.primitive);
                }

                return std::size_t(cls == eGeoAttribClassVertex ? t.vertices[corner] : t.points[corner]);
            };
            const auto weights = [&mesh, &world](std::int32_t n, const openvdb::Vec3d& p) {
                const Triangle& t = mesh.triangles[std::size_t(n)];
                const openvdb::Vec3d a0 = toVec(world[std::size_t(t.points[0])]);
                const openvdb::Vec3d a1 = toVec(world[std::size_t(t.points[1])]);
                const openvdb::Vec3d a2 = toVec(world[std::size_t(t.points[2])]);
                openvdb::Vec3d uvw;
                const openvdb::Vec3d q = openvdb::math::closestPointOnTriangleToPoint(a0, a1, a2, p, uvw);

                return barycentric(a0, a1, a2, q);
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
                const std::array<double, 3> w = weights(n, p);
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
                const std::array<double, 3> w = weights(n, p);
                const int corner = int(std::max_element(w.begin(), w.end()) - w.begin());

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
               std::string* error)
{
    geoInitializeOpenVDB();
    if (!checkVoxelSize(params.voxelSize, error)) {
        return false;
    }
    if (!(params.bandWidth > 0.)) {
        return fail(error, "The band width must be positive.");
    }
    if (!(params.minRadius > 0.)) {
        return fail(error, "The minimum radius must be positive.");
    }
    if (params.trails && !(params.trailSpacing > 0.)) {
        return fail(error, "The trail spacing must be positive.");
    }
    const openvdb::math::Transform::Ptr xform = linearTransform(params.voxelSize);
    openvdb::FloatGrid::Ptr grid = openvdb::FloatGrid::create(float(params.bandWidth * params.voxelSize));
    grid->setTransform(xform);
    grid->setGridClass(openvdb::GRID_LEVEL_SET);

    ParticleList particles;
    std::vector<GeoOffset> offsets;
    const GeoAttribute* pscale = source.findAttribute(eGeoAttribClassPoint, "pscale");
    if (pscale && ((pscale->isArray()) || (pscale->storage() == eGeoStorageString))) {
        pscale = nullptr;
    }
    const GeoAttribute* v = params.trails ? source.findAttribute(eGeoAttribClassPoint, "v") : nullptr;
    if (v && (v->isArray() || (v->storage() == eGeoStorageString) || (v->tupleSize() < 3))) {
        v = nullptr;
    }
    source.pointMap().forEachLive([&](GeoIndex /*index*/, GeoOffset point) {
        offsets.push_back(point);
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
    if (params.trails && !v && !particles.positions.empty()) {
        result->notes.push_back("The points have no three-component v attribute, so there are no trails.");
    }

    std::vector<std::string> attributeNotes;
    const std::vector<std::pair<std::string, AttributeSource>> attributes = matchAttributes(source, { eGeoAttribClassPoint }, params.attributes, &attributeNotes);

    // ParticlesToLevelSet::getMinCount() keeps only one thread's count, so count the skipped
    // spheres here with the same voxel-unit test it applies.
    std::size_t skipped = 0;
    for (const openvdb::Real radius : particles.radii) {
        if (radius / params.voxelSize < params.minRadius) {
            ++skipped;
        }
    }
    ParticleIndexGrid::Ptr index;
    if (!particles.positions.empty()) {
        const bool trails = params.trails && v;
        if (attributes.empty()) {
            openvdb::tools::ParticlesToLevelSet<openvdb::FloatGrid> raster(*grid);
            raster.setRmin(params.minRadius);
            raster.setRmax(std::numeric_limits<openvdb::Real>::max());
            if (trails) {
                raster.rasterizeTrails(particles, params.trailSpacing);
            } else {
                raster.rasterizeSpheres(particles);
            }
            raster.finalize(true);
        } else {
            openvdb::tools::ParticlesToLevelSet<openvdb::FloatGrid, openvdb::Index32> raster(*grid);
            raster.setRmin(params.minRadius);
            raster.setRmax(std::numeric_limits<openvdb::Real>::max());
            if (trails) {
                raster.rasterizeTrails(particles, params.trailSpacing);
            } else {
                raster.rasterizeSpheres(particles);
            }
            raster.finalize(true);
            index = raster.attributeGrid();
        }
    }
    if (skipped > 0) {
        std::ostringstream os;
        os << skipped << (skipped == 1 ? " point was" : " points were")
           << " skipped: their spheres are smaller than the minimum radius.";
        result->notes.push_back(os.str());
    }
    finishSurfaceGrid(*grid, params.kind, true, params.name);
    addGrid(result, params.name, grid);

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
        return meshGrid(*g, params, reference, out);
    }
    if (const openvdb::DoubleGrid* g = dynamic_cast<const openvdb::DoubleGrid*>(&grid)) {
        return meshGrid(*g, params, reference, out);
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

GeoVDBGridBaseConstPtr
geoFogToSdf(const openvdb::GridBase& grid,
            double isovalue,
            std::string* error)
{
    if (const openvdb::FloatGrid* g = dynamic_cast<const openvdb::FloatGrid*>(&grid)) {
        openvdb::FloatGrid::Ptr sdf = openvdb::tools::fogToSdf(*g, float(isovalue));
        sdf->insertMeta(*g);
        sdf->setGridClass(openvdb::GRID_LEVEL_SET);

        return sdf;
    }
    if (const openvdb::DoubleGrid* g = dynamic_cast<const openvdb::DoubleGrid*>(&grid)) {
        openvdb::DoubleGrid::Ptr sdf = openvdb::tools::fogToSdf(*g, isovalue);
        sdf->insertMeta(*g);
        sdf->setGridClass(openvdb::GRID_LEVEL_SET);

        return sdf;
    }
    fail(error, "Only float and double grids convert to an SDF; " + grid.getName() + " holds " + grid.valueType() + ".");

    return GeoVDBGridBaseConstPtr();
}

GeoVDBGridBaseConstPtr
geoResampleVDB(const openvdb::GridBase& grid,
               const openvdb::math::Transform& target,
               GeoVDBInterpolationEnum interpolation,
               std::string* error)
{
    if (const openvdb::FloatGrid* g = dynamic_cast<const openvdb::FloatGrid*>(&grid)) {
        return resampled(*g, target, interpolation);
    }
    if (const openvdb::DoubleGrid* g = dynamic_cast<const openvdb::DoubleGrid*>(&grid)) {
        return resampled(*g, target, interpolation);
    }
    if (const openvdb::Vec3SGrid* g = dynamic_cast<const openvdb::Vec3SGrid*>(&grid)) {
        return resampled(*g, target, interpolation);
    }
    if (const openvdb::Vec3DGrid* g = dynamic_cast<const openvdb::Vec3DGrid*>(&grid)) {
        return resampled(*g, target, interpolation);
    }
    fail(error, "Only float, double and vector grids can be resampled; " + grid.getName() + " holds " + grid.valueType() + ".");

    return GeoVDBGridBaseConstPtr();
}

std::shared_ptr<openvdb::math::Transform>
geoScaledVDBTransform(const openvdb::GridBase& grid,
                      double scale)
{
    openvdb::math::Transform::Ptr xform = grid.transform().copy();
    xform->preScale(scale);

    return xform;
}

std::shared_ptr<openvdb::math::Transform>
geoVoxelSizedVDBTransform(const openvdb::GridBase& grid,
                          double voxelSize)
{
    const double current = grid.transform().voxelSize()[0];

    return geoScaledVDBTransform(grid, current > 0. ? voxelSize / current : 1.);
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
