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

#include "GeoCopyToPoints.h"

#include <fnmatch.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <QString>

#include "Engine/ChoiceOption.h"
#include "Engine/Geo/GeoOpUtils.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Geometry/GeoInterpolate.h"

NATRON_NAMESPACE_ENTER

namespace {
typedef std::array<double, 3> Vec3;
typedef std::array<double, 9> Mat3;

Vec3
normalized(const Vec3& v)
{
    const double l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

    return l > 0. ? Vec3 { v[0] / l, v[1] / l, v[2] / l } : v;
}

Vec3
cross(const Vec3& a,
      const Vec3& b)
{
    return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
}

Mat3
identity3()
{
    return { 1., 0., 0., 0., 1., 0., 0., 0., 1. };
}

Mat3
multiply3(const Mat3& a,
          const Mat3& b)
{
    Mat3 out {};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
        }
    }

    return out;
}

Vec3
apply3(const Mat3& m,
       const Vec3& v)
{
    return { m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
             m[6] * v[0] + m[7] * v[1] + m[8] * v[2] };
}

// Quaternions are stored x, y, z, w.
Mat3
quaternionMatrix(const std::array<double, 4>& q)
{
    double x = q[0], y = q[1], z = q[2], w = q[3];
    const double l = std::sqrt(x * x + y * y + z * z + w * w);
    if (!(l > 0.)) {
        return identity3();
    }
    x /= l;
    y /= l;
    z /= l;
    w /= l;

    return { 1. - 2. * (y * y + z * z), 2. * (x * y - z * w), 2. * (x * z + y * w),
             2. * (x * y + z * w), 1. - 2. * (x * x + z * z), 2. * (y * z - x * w),
             2. * (x * z - y * w), 2. * (y * z + x * w), 1. - 2. * (x * x + y * y) };
}

// Columns are the copy's X, Y and Z axes: Z along the normal, Y as close to up as it can be.
Mat3
lookAlong(const Vec3& normal,
          const Vec3& up)
{
    const Vec3 z = normalized(normal);
    if ((z[0] == 0.) && (z[1] == 0.) && (z[2] == 0.)) {
        return identity3();
    }
    Vec3 x = cross(up, z);
    if ((x[0] * x[0] + x[1] * x[1] + x[2] * x[2]) < 1e-20) {
        x = cross(std::abs(z[0]) < 0.9 ? Vec3 { 1., 0., 0. } : Vec3 { 0., 1., 0. }, z);
    }
    x = normalized(x);
    const Vec3 y = cross(z, x);

    return { x[0], y[0], z[0], x[1], y[1], z[1], x[2], y[2], z[2] };
}

// The smallest rotation taking +Z to the normal.
Mat3
rotateZTo(const Vec3& normal)
{
    const Vec3 b = normalized(normal);
    if ((b[0] == 0.) && (b[1] == 0.) && (b[2] == 0.)) {
        return identity3();
    }
    const double c = b[2];
    if (c < -1. + 1e-12) {
        return { 1., 0., 0., 0., -1., 0., 0., 0., -1. };
    }
    const Vec3 v = { -b[1], b[0], 0. };
    const double k = 1. / (1. + c);

    return { 1. - k * v[1] * v[1], k * v[0] * v[1], v[1],
             k * v[0] * v[1], 1. - k * v[0] * v[0], -v[0],
             -v[1], v[0], 1. - k * (v[0] * v[0] + v[1] * v[1]) };
}

std::optional<Mat3>
inverse3(const Mat3& m)
{
    const double det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (std::abs(det) < 1e-300) {
        return std::nullopt;
    }
    const double inv = 1. / det;

    return Mat3 { (m[4] * m[8] - m[5] * m[7]) * inv, (m[2] * m[7] - m[1] * m[8]) * inv, (m[1] * m[5] - m[2] * m[4]) * inv,
                  (m[5] * m[6] - m[3] * m[8]) * inv, (m[0] * m[8] - m[2] * m[6]) * inv, (m[2] * m[3] - m[0] * m[5]) * inv,
                  (m[3] * m[7] - m[4] * m[6]) * inv, (m[1] * m[6] - m[0] * m[7]) * inv, (m[0] * m[4] - m[1] * m[3]) * inv };
}

Mat3
linearPart(const GeoMatrix4& m)
{
    return { m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10] };
}

GeoMatrix4
fromLinear(const Mat3& l,
           const Vec3& translation)
{
    return { l[0], l[1], l[2], translation[0], l[3], l[4], l[5], translation[1], l[6], l[7], l[8], translation[2], 0., 0., 0., 1. };
}

GeoMatrix4
translation(const Vec3& t)
{
    return fromLinear(identity3(), t);
}

const GeoAttribute*
numericPointAttribute(const GeoDetail& detail,
                      const char* name,
                      int minimumTuple)
{
    const GeoAttribute* a = detail.findAttribute(eGeoAttribClassPoint, name);

    return (a && geoIsNumeric(*a) && (a->tupleSize() >= minimumTuple)) ? a : nullptr;
}

// The instancing attributes of the target, looked up once.
struct TargetOrientation {
    const GeoAttribute* transform = nullptr;
    const GeoAttribute* orient = nullptr;
    const GeoAttribute* normal = nullptr;
    const GeoAttribute* velocity = nullptr;
    const GeoAttribute* up = nullptr;
    const GeoAttribute* pscale = nullptr;
    const GeoAttribute* scale = nullptr;
    const GeoAttribute* rot = nullptr;
    const GeoAttribute* trans = nullptr;
    const GeoAttribute* pivot = nullptr;

    explicit TargetOrientation(const GeoDetail& target)
    {
        transform = numericPointAttribute(target, "transform", 9);
        if (transform && (transform->tupleSize() != 9) && (transform->tupleSize() != 16)) {
            transform = nullptr;
        }
        orient = numericPointAttribute(target, "orient", 4);
        normal = numericPointAttribute(target, "N", 3);
        velocity = numericPointAttribute(target, "v", 3);
        up = numericPointAttribute(target, "up", 3);
        pscale = numericPointAttribute(target, "pscale", 1);
        scale = numericPointAttribute(target, "scale", 3);
        rot = numericPointAttribute(target, "rot", 4);
        trans = numericPointAttribute(target, "trans", 3);
        pivot = numericPointAttribute(target, "pivot", 3);
    }

    bool needsImplicitNormal() const
    {
        return !transform && !orient && !normal && !velocity;
    }

    GeoMatrix4 matrix(const GeoDetail& target,
                      GeoOffset point,
                      bool useOrientations,
                      const Vec3* implicitNormal) const
    {
        double buffer[16];
        auto read = [&buffer, point](const GeoAttribute* a) {
            geoReadNumeric(*a, (std::size_t)point, std::span<double>(buffer, (std::size_t)a->tupleSize()));
        };
        auto readVec3 = [&](const GeoAttribute* a) {
            read(a);

            return Vec3 { buffer[0], buffer[1], buffer[2] };
        };
        Vec3 position = target.pointPosition(point);
        if (useOrientations && trans) {
            const Vec3 t = readVec3(trans);
            position = { position[0] + t[0], position[1] + t[1], position[2] + t[2] };
        }
        if (!useOrientations) {
            return translation(position);
        }
        Mat3 linear;
        if (transform) {
            read(transform);
            // Stored for row vectors: transpose for column vectors.
            const int n = transform->tupleSize() == 16 ? 4 : 3;
            linear = { buffer[0], buffer[n], buffer[2 * n], buffer[1], buffer[n + 1], buffer[2 * n + 1], buffer[2], buffer[n + 2],
                       buffer[2 * n + 2] };
            if (n == 4) {
                position = { position[0] + buffer[12], position[1] + buffer[13], position[2] + buffer[14] };
            }
        } else {
            Mat3 align = identity3();
            if (orient) {
                read(orient);
                align = quaternionMatrix({ buffer[0], buffer[1], buffer[2], buffer[3] });
            } else {
                std::optional<Vec3> direction;
                if (normal) {
                    direction = readVec3(normal);
                } else if (velocity) {
                    direction = readVec3(velocity);
                } else if (implicitNormal) {
                    direction = *implicitNormal;
                }
                if (direction) {
                    align = up ? lookAlong(*direction, readVec3(up)) : rotateZTo(*direction);
                }
            }
            if (rot) {
                read(rot);
                align = multiply3(align, quaternionMatrix({ buffer[0], buffer[1], buffer[2], buffer[3] }));
            }
            Vec3 s = { 1., 1., 1. };
            if (scale) {
                s = readVec3(scale);
            }
            if (pscale) {
                read(pscale);
                s = { s[0] * buffer[0], s[1] * buffer[0], s[2] * buffer[0] };
            }
            linear = multiply3(align, Mat3 { s[0], 0., 0., 0., s[1], 0., 0., 0., s[2] });
        }
        GeoMatrix4 m = fromLinear(linear, position);
        if (pivot) {
            const Vec3 p = readVec3(pivot);
            m = geoMatrixMultiply(m, translation({ -p[0], -p[1], -p[2] }));
        }

        return m;
    } // matrix
};

// Area-weighted normals of the polygons around each point, by point offset; empty when the
// target has no polygons.
std::vector<Vec3>
implicitPointNormals(const GeoDetail& target)
{
    std::vector<Vec3> normals;
    if (target.primitiveCount() == 0) {
        return normals;
    }
    normals.assign((std::size_t)target.pointMap().offsetSize(), Vec3 { 0., 0., 0. });
    target.primitiveMap().forEachLive([&](GeoIndex, GeoOffset primitive) {
        if (target.primitiveType(primitive) != eGeoPrimitivePolygon) {
            return;
        }
        const GeoVertexRange range = target.primitiveVertices(primitive);
        if (range.count < 3) {
            return;
        }
        Vec3 n = { 0., 0., 0. };
        for (GeoOffset k = 0; k < range.count; ++k) {
            const Vec3 a = target.pointPosition(target.vertexPoint(range.start + k));
            const Vec3 b = target.pointPosition(target.vertexPoint(range.start + (k + 1) % range.count));
            n[0] += (a[1] - b[1]) * (a[2] + b[2]);
            n[1] += (a[2] - b[2]) * (a[0] + b[0]);
            n[2] += (a[0] - b[0]) * (a[1] + b[1]);
        }
        for (GeoOffset k = 0; k < range.count; ++k) {
            Vec3& sum = normals[(std::size_t)target.vertexPoint(range.start + k)];
            sum = { sum[0] + n[0], sum[1] + n[1], sum[2] + n[2] };
        }
    });
    for (Vec3& n : normals) {
        n = normalized(n);
    }

    return normals;
}

bool
attributeSelected(const std::string& name,
                  const std::vector<std::string>& tokens)
{
    bool selected = false;
    for (const std::string& token : tokens) {
        if (!token.empty() && (token[0] == '^')) {
            if (fnmatch(token.c_str() + 1, name.c_str(), 0) == 0) {
                selected = false;
            }
        } else if (fnmatch(token.c_str(), name.c_str(), 0) == 0) {
            selected = true;
        }
    }

    return selected;
}

std::vector<std::string>
patternTokens(const std::string& pattern)
{
    std::string spaced = pattern;
    std::replace(spaced.begin(), spaced.end(), ',', ' ');
    std::istringstream stream(spaced);
    std::vector<std::string> tokens;
    std::string token;
    while (stream >> token) {
        tokens.push_back(token);
    }

    return tokens;
}

bool
isPackedIntrinsic(const std::string& name)
{
    return (name == kGeoPackedXformAttr) || (name == kGeoPackedBoundsAttr) || (name == kGeoPackedDetailAttr) || (name == kGeoPackedUSDPathAttr) || (name == kGeoPackedUSDParentXformAttr);
}

enum TransformKindEnum {
    eTransformNone = 0,
    eTransformPoint,
    eTransformVector,
    eTransformNormal,
};

TransformKindEnum
transformKind(const GeoAttribute& a)
{
    if (!geoIsNumeric(a) || (a.tupleSize() != 3) || (a.storage() == eGeoStorageInt32) || (a.storage() == eGeoStorageInt64)) {
        return eTransformNone;
    }
    switch (a.typeInfo()) {
    case eGeoTypeInfoPoint:
        return eTransformPoint;
    case eGeoTypeInfoVector:
        return eTransformVector;
    case eGeoTypeInfoNormal:
        return eTransformNormal;
    default:
        return eTransformNone;
    }
}

struct CopyTransform {
    GeoMatrix4 m;
    Mat3 linear;
    Mat3 normal;

    explicit CopyTransform(const GeoMatrix4& matrix)
        : m(matrix)
        , linear(linearPart(matrix))
        , normal(identity3())
    {
        if (const std::optional<Mat3> inv = inverse3(linear)) {
            const Mat3& i = *inv;
            normal = { i[0], i[3], i[6], i[1], i[4], i[7], i[2], i[5], i[8] };
        }
    }
};

struct MirroredAttribute {
    const GeoAttribute* source;
    GeoAttribute* destination;
    TransformKindEnum kind;
};

void
copyAttributes(const std::vector<MirroredAttribute>& attributes,
               std::size_t destination,
               std::size_t source,
               const CopyTransform& xform)
{
    for (const MirroredAttribute& a : attributes) {
        geoCopyElement(*a.destination, destination, *a.source, source);
        if (a.kind == eTransformNone) {
            continue;
        }
        double v[3];
        geoReadNumeric(*a.source, source, std::span<double>(v, 3));
        Vec3 out;
        switch (a.kind) {
        case eTransformPoint:
            out = geoTransformPoint(xform.m, { v[0], v[1], v[2] });
            break;
        case eTransformVector:
            out = apply3(xform.linear, { v[0], v[1], v[2] });
            break;
        case eTransformNormal: {
            const double length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            out = normalized(apply3(xform.normal, { v[0], v[1], v[2] }));
            out = { out[0] * length, out[1] * length, out[2] * length };
            break;
        }
        case eTransformNone:
            break;
        }
        geoWriteNumeric(*a.destination, destination, std::span<const double>(out.data(), 3));
    }
}

std::vector<MirroredAttribute>
mirror(const GeoDetail& source,
       GeoDetailBuilder& output,
       GeoAttribClassEnum cls,
       bool skipPackedIntrinsics)
{
    std::vector<MirroredAttribute> out;
    for (const auto& [from, to] : geoMirrorAttributes(source, output, cls)) {
        if (skipPackedIntrinsics && isPackedIntrinsic(from->name())) {
            continue;
        }
        out.push_back(MirroredAttribute { from, to, transformKind(*from) });
    }

    return out;
}

struct MirroredGroup {
    const GeoElementGroup* source;
    GeoElementGroup* destination;
};

std::vector<MirroredGroup>
mirrorGroups(const GeoDetail& source,
             GeoDetailBuilder& output,
             GeoAttribClassEnum cls)
{
    std::vector<MirroredGroup> out;
    for (const std::string& name : source.groupNames(cls)) {
        out.push_back(MirroredGroup { source.findGroup(cls, name), &output.addGroup(cls, name) });
    }

    return out;
}

void
copyGroups(const std::vector<MirroredGroup>& groups,
           GeoOffset destination,
           GeoOffset source)
{
    for (const MirroredGroup& g : groups) {
        if (g.source->contains(source)) {
            g.destination->add(destination);
        }
    }
}

GeoDetailConstPtr
restrictToSelection(const GeoDetailConstPtr& source,
                    const GeoElementMask& selected)
{
    std::vector<GeoOffset> dropped;
    source->primitiveMap().forEachLive([&](GeoIndex, GeoOffset primitive) {
        if (!selected[std::size_t(primitive)]) {
            dropped.push_back(primitive);
        }
    });
    if (dropped.empty()) {
        return source;
    }
    GeoDetailBuilder builder(*source);
    builder.deletePrimitives(dropped, true);

    return std::make_shared<const GeoDetail>(builder.finish());
}

// One copy's new elements, as offset ranges: the output starts empty and only grows.
struct CopyElements {
    GeoOffset pointBegin, pointEnd;
    GeoOffset vertexBegin, vertexEnd;
    GeoOffset primitiveBegin, primitiveEnd;
};

void
expandCopy(const GeoDetail& source,
           const std::vector<GeoOffset>& sourcePoints,
           const std::vector<GeoOffset>& localPoint,
           const CopyTransform& xform,
           const std::vector<MirroredAttribute> (&attributes)[3],
           const std::vector<MirroredGroup> (&groups)[3],
           GeoDetailBuilder& output)
{
    const GeoOffset firstPoint = output.appendPoints((GeoIndex)sourcePoints.size());
    for (std::size_t i = 0; i < sourcePoints.size(); ++i) {
        const GeoOffset point = firstPoint + (GeoOffset)i;
        const Vec3 p = geoTransformPoint(xform.m, source.pointPosition(sourcePoints[i]));
        output.setPointPosition(point, p[0], p[1], p[2]);
        copyAttributes(attributes[eGeoAttribClassPoint], (std::size_t)point, (std::size_t)sourcePoints[i], xform);
        copyGroups(groups[eGeoAttribClassPoint], point, sourcePoints[i]);
    }

    std::vector<GeoOffset> batch;
    std::vector<std::int32_t> counts;
    std::vector<GeoOffset> corners;
    GeoPrimitiveTypeEnum batchType = eGeoPrimitivePolygon;
    auto finishPrimitive = [&](GeoOffset sourcePrimitive,
                               GeoOffset primitive) {
        copyAttributes(attributes[eGeoAttribClassPrimitive], (std::size_t)primitive, (std::size_t)sourcePrimitive, xform);
        copyGroups(groups[eGeoAttribClassPrimitive], primitive, sourcePrimitive);
        const GeoVertexRange from = source.primitiveVertices(sourcePrimitive);
        const GeoVertexRange to = output.detail().primitiveVertices(primitive);
        for (GeoOffset k = 0; k < std::min(from.count, to.count); ++k) {
            copyAttributes(attributes[eGeoAttribClassVertex], (std::size_t)(to.start + k), (std::size_t)(from.start + k), xform);
            copyGroups(groups[eGeoAttribClassVertex], to.start + k, from.start + k);
        }
    };
    auto flush = [&]() {
        if (batch.empty()) {
            return;
        }
        const GeoOffset first = output.appendPrimitives(batchType, counts, corners);
        for (std::size_t k = 0; k < batch.size(); ++k) {
            finishPrimitive(batch[k], first + (GeoOffset)k);
        }
        batch.clear();
        counts.clear();
        corners.clear();
    };
    source.primitiveMap().forEachLive([&](GeoIndex, GeoOffset primitive) {
        const GeoPrimitiveTypeEnum type = source.primitiveType(primitive);
        const GeoVertexRange range = source.primitiveVertices(primitive);
        if (geoPrimitiveIsPacked(type)) {
            flush();
            const GeoOffset pivot = firstPoint + localPoint[(std::size_t)source.vertexPoint(range.start)];
            const GeoMatrix4 local = geoPackedTransform(source, primitive);
            GeoOffset packed;
            if (type == eGeoPrimitivePackedGeometry) {
                packed = geoAppendPackedGeometry(output, pivot, geoPackedDetail(source, primitive), geoMatrixMultiply(xform.m, local));
            } else {
                const GeoMatrix4 parent = geoPackedUSDParentTransform(source, primitive);
                const GeoMatrix4 world = geoMatrixMultiply(xform.m, geoMatrixMultiply(parent, local));
                const std::optional<GeoMatrix4> moved = geoPackedUSDLocalFromWorld(source, primitive, world);
                packed = geoAppendPackedUSD(output, pivot, geoPackedUSDPath(source, primitive), parent,
                                            moved ? *moved : geoMatrixMultiply(xform.m, local), geoPackedLocalBounds(source, primitive));
            }
            finishPrimitive(primitive, packed);

            return;
        }
        if (!batch.empty() && (type != batchType)) {
            flush();
        }
        batchType = type;
        batch.push_back(primitive);
        counts.push_back((std::int32_t)range.count);
        for (GeoOffset k = 0; k < range.count; ++k) {
            corners.push_back(firstPoint + localPoint[(std::size_t)source.vertexPoint(range.start + k)]);
        }
    });
    flush();
} // expandCopy
} // anonymous namespace

GeoMatrix4
geoCopyPointTransform(const GeoDetail& target,
                      GeoOffset point,
                      bool useOrientations,
                      const std::array<double, 3>* implicitNormal)
{
    return TargetOrientation(target).matrix(target, point, useOrientations, implicitNormal);
}

bool
copyGeometryToPoints(const GeoDetailConstPtr& source,
                     const GeoDetail& target,
                     const GeoCopyToPointsParams& params,
                     GeoDetailBuilder& output,
                     std::string* errorMessage)
{
    GeoElementMask sourceSelected;
    GeoElementMask targetSelected;
    if (!geoSelectElements(*source, eGeoAttribClassPrimitive, params.sourceGroup, &sourceSelected, errorMessage) || !geoSelectElements(target, eGeoAttribClassPoint, params.targetGroup, &targetSelected, errorMessage)) {
        return false;
    }
    const GeoDetailConstPtr contained = restrictToSelection(source, sourceSelected);
    const GeoDetail& src = *contained;

    std::vector<GeoOffset> targetPoints;
    target.pointMap().forEachLive([&](GeoIndex,
                                      GeoOffset point) {
        if (targetSelected[std::size_t(point)]) {
            targetPoints.push_back(point);
        }
    });

    const TargetOrientation orientation(target);
    std::vector<Vec3> implicitNormals;
    if (params.useTargetOrientations && params.useImplicitNormal && orientation.needsImplicitNormal()) {
        implicitNormals = implicitPointNormals(target);
    }
    std::vector<GeoMatrix4> transforms;
    transforms.reserve(targetPoints.size());
    for (GeoOffset point : targetPoints) {
        const Vec3* implicitNormal = implicitNormals.empty() ? nullptr : &implicitNormals[(std::size_t)point];
        transforms.push_back(orientation.matrix(target, point, params.useTargetOrientations, implicitNormal));
    }

    output.setPointPrecision64(src.pointPrecision64());
    auto offsetSizes = [&output]() {
        const GeoDetail& d = output.detail();

        return std::array<GeoOffset, 3> { d.pointMap().offsetSize(), d.vertexMap().offsetSize(),
                                          d.primitiveMap().offsetSize() };
    };
    std::vector<CopyElements> elements(targetPoints.size());
    if (params.packAndInstance) {
        GeoMatrix4 recentre = geoMatrixIdentity();
        if (params.pivot == eGeoCopyPivotCentroid) {
            const GeoBounds bounds = geoDetailBounds(src);
            recentre = translation({ -0.5 * (bounds.min[0] + bounds.max[0]), -0.5 * (bounds.min[1] + bounds.max[1]),
                                     -0.5 * (bounds.min[2] + bounds.max[2]) });
        }
        const GeoOffset firstPoint = output.appendPoints((GeoIndex)targetPoints.size());
        std::vector<GeoOffset> pivots(targetPoints.size());
        std::vector<GeoMatrix4> packedTransforms(targetPoints.size());
        for (std::size_t i = 0; i < targetPoints.size(); ++i) {
            pivots[i] = firstPoint + (GeoOffset)i;
            const Vec3 p = target.pointPosition(targetPoints[i]);
            output.setPointPosition(pivots[i], p[0], p[1], p[2]);
            packedTransforms[i] = geoMatrixMultiply(transforms[i], recentre);
        }
        if (!targetPoints.empty()) {
            const GeoOffset firstPrimitive = geoAppendPackedGeometries(output, pivots, contained, packedTransforms);
            for (std::size_t i = 0; i < targetPoints.size(); ++i) {
                const GeoOffset primitive = firstPrimitive + (GeoOffset)i;
                const GeoOffset vertex = output.detail().primitiveVertices(primitive).start;
                elements[i] = CopyElements { pivots[i], pivots[i] + 1, vertex, vertex + 1, primitive, primitive + 1 };
            }
        }
    } else {
        geoCopyDetailAttributes(src, output);
        const std::vector<MirroredAttribute> attributes[3] = {
            mirror(src, output, eGeoAttribClassPoint, false),
            mirror(src, output, eGeoAttribClassVertex, false),
            mirror(src, output, eGeoAttribClassPrimitive, true),
        };
        const std::vector<MirroredGroup> groups[3] = {
            mirrorGroups(src, output, eGeoAttribClassPoint),
            mirrorGroups(src, output, eGeoAttribClassVertex),
            mirrorGroups(src, output, eGeoAttribClassPrimitive),
        };
        std::vector<GeoOffset> sourcePoints;
        std::vector<GeoOffset> localPoint((std::size_t)src.pointMap().offsetSize(), 0);
        src.pointMap().forEachLive([&](GeoIndex index,
                                       GeoOffset point) {
            localPoint[(std::size_t)point] = (GeoOffset)index;
            sourcePoints.push_back(point);
        });
        for (std::size_t i = 0; i < targetPoints.size(); ++i) {
            const std::array<GeoOffset, 3> before = offsetSizes();
            expandCopy(src, sourcePoints, localPoint, CopyTransform(transforms[i]), attributes, groups, output);
            const std::array<GeoOffset, 3> after = offsetSizes();
            elements[i] = CopyElements { before[0], after[0], before[1], after[1], before[2], after[2] };
        }
    }

    const GeoAttribClassEnum applyClass = params.applyTo == eGeoCopyApplyToVertices ? eGeoAttribClassVertex
        : params.applyTo == eGeoCopyApplyToPrimitives                               ? eGeoAttribClassPrimitive
                                                                                    : eGeoAttribClassPoint;
    const std::vector<std::string> tokens = patternTokens(params.targetAttributes);
    std::vector<const GeoAttribute*> selected;
    for (std::size_t i = 0; i < target.attributeCount(eGeoAttribClassPoint); ++i) {
        const GeoAttribute& a = target.attribute(eGeoAttribClassPoint, i);
        if ((a.name() == GeoDetail::kPositionName) || isPackedIntrinsic(a.name()) || !attributeSelected(a.name(), tokens)) {
            continue;
        }
        selected.push_back(&a);
    }
    for (const GeoAttribute* a : selected) {
        GeoAttribute& destination = output.addAttribute(applyClass, a->desc());
        for (std::size_t i = 0; i < targetPoints.size(); ++i) {
            const CopyElements& e = elements[i];
            const GeoOffset begin = applyClass == eGeoAttribClassPoint ? e.pointBegin
                : applyClass == eGeoAttribClassVertex                  ? e.vertexBegin
                                                                       : e.primitiveBegin;
            const GeoOffset end = applyClass == eGeoAttribClassPoint ? e.pointEnd
                : applyClass == eGeoAttribClassVertex                ? e.vertexEnd
                                                                     : e.primitiveEnd;
            for (GeoOffset offset = begin; offset < end; ++offset) {
                geoCopyElement(destination, (std::size_t)offset, *a, (std::size_t)targetPoints[i]);
            }
        }
    }

    return true;
} // copyGeometryToPoints

GeoCopyToPoints::GeoCopyToPoints(NodePtr node)
    : NativeEffectBase(node)
{
}

GeoCopyToPoints::~GeoCopyToPoints()
{
}

const NodeSpec*
GeoCopyToPoints::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
GeoCopyToPoints::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls").toStdString());

    auto createString = [this, &page](const QString& label,
                                      const char* name,
                                      const QString& toolTip,
                                      const std::string& defaultValue) {
        KnobStringPtr knob = createKnob<KnobString>(label.toStdString());
        knob->setName(name);
        knob->setHintToolTip(toolTip.toStdString());
        knob->setDefaultValue(defaultValue);
        knob->setAnimationEnabled(false);
        page->addKnob(knob);

        return knob;
    };
    auto createToggle = [this, &page](const QString& label,
                                      const char* name,
                                      const QString& toolTip,
                                      bool defaultValue) {
        KnobBoolPtr knob = createKnob<KnobBool>(label.toStdString());
        knob->setName(name);
        knob->setHintToolTip(toolTip.toStdString());
        knob->setDefaultValue(defaultValue);
        knob->setAnimationEnabled(false);
        page->addKnob(knob);

        return knob;
    };

    _sourceGroup = createString(tr("Source Group"), kGeoCopyToPointsParamSourceGroup,
                                tr("The primitives of the Source to copy, with the points they use: a group pattern (group names, "
                                   "globs, element numbers, @attr comparisons, ! and ^). Empty copies all of it."),
                                std::string());
    _targetGroup = createString(tr("Target Group"), kGeoCopyToPointsParamTargetGroup,
                                tr("The Target points to copy onto: a group pattern (group names, globs, element numbers, @attr comparisons, "
                                   "! and ^). Empty copies onto every point."),
                                std::string());
    _packAndInstance = createToggle(tr("Pack And Instance"), kGeoCopyToPointsParamPackAndInstance,
                                    tr("Makes each copy one packed primitive, all of them sharing the Source instead of "
                                       "repeating it. Much lighter for many copies, but the copies cannot be edited one "
                                       "by one."),
                                    false);

    KnobChoicePtr pivot = createKnob<KnobChoice>(tr("Pivot").toStdString());
    pivot->setName(kGeoCopyToPointsParamPivot);
    pivot->setHintToolTip(tr("With Pack And Instance: which point of the Source lands on each target point.").toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("origin", tr("Origin").toStdString(), tr("The Source's origin.").toStdString()));
        options.push_back(ChoiceOption("centroid", tr("Centroid").toStdString(),
                                       tr("The centre of the Source's bounding box.").toStdString()));
        pivot->populateChoices(options);
    }
    pivot->setDefaultValue((int)eGeoCopyPivotCentroid);
    pivot->setAnimationEnabled(false);
    page->addKnob(pivot);
    _pivot = pivot;

    _useTargetOrientations = createToggle(tr("Use Target Orientations"), kGeoCopyToPointsParamUseTargetOrientations,
                                          tr("Orients, scales and offsets each copy from its point's transform, orient, N, "
                                             "v, up, rot, pscale, scale, trans and pivot attributes. Off, copies are only "
                                             "moved to their points."),
                                          true);
    _useImplicitNormal = createToggle(tr("Use Implicit Normal"), kGeoCopyToPointsParamUseImplicitNormal,
                                      tr("When the Target points have no N, orient, v or transform, aims each copy's +Z "
                                         "along the normal of the Target's surface at its point."),
                                      true);
    _targetAttributes = createString(tr("Target Attributes"), kGeoCopyToPointsParamTargetAttributes,
                                     tr("The Target point attributes copied onto every copy, as names separated by spaces "
                                        "or commas. * and ? are wildcards and a name starting with ^ is left out. By "
                                        "default every attribute but those that place the copies."),
                                     kGeoCopyDefaultTargetAttributes);

    KnobChoicePtr applyTo = createKnob<KnobChoice>(tr("Apply To").toStdString());
    applyTo->setName(kGeoCopyToPointsParamApplyTo);
    applyTo->setHintToolTip(tr("Which elements of each copy receive the Target Attributes.").toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("points", tr("Points").toStdString(), tr("Every point of the copy.").toStdString()));
        options.push_back(ChoiceOption("vertices", tr("Vertices").toStdString(), tr("Every vertex of the copy.").toStdString()));
        options.push_back(ChoiceOption("primitives", tr("Primitives").toStdString(),
                                       tr("Every primitive of the copy.").toStdString()));
        applyTo->populateChoices(options);
    }
    applyTo->setDefaultValue((int)eGeoCopyApplyToPoints);
    applyTo->setAnimationEnabled(false);
    page->addKnob(applyTo);
    _applyTo = applyTo;

    updateEnabled();
} // GeoCopyToPoints::initializeKnobs

void
GeoCopyToPoints::updateEnabled()
{
    const KnobBoolPtr pack = _packAndInstance.lock();
    const KnobBoolPtr orientations = _useTargetOrientations.lock();
    if (const KnobChoicePtr pivot = _pivot.lock()) {
        pivot->setAllDimensionsEnabled(pack && pack->getValue());
    }
    if (const KnobBoolPtr implicit = _useImplicitNormal.lock()) {
        implicit->setAllDimensionsEnabled(orientations && orientations->getValue());
    }
}

void
GeoCopyToPoints::onKnobsLoaded()
{
    updateEnabled();
}

bool
GeoCopyToPoints::knobChanged(KnobI* k,
                             ValueChangedReasonEnum /*reason*/,
                             ViewSpec /*view*/,
                             double /*time*/,
                             bool /*originatedFromMainThread*/)
{
    const KnobBoolPtr pack = _packAndInstance.lock();
    const KnobBoolPtr orientations = _useTargetOrientations.lock();
    if ((pack && (k == pack.get())) || (orientations && (k == orientations.get()))) {
        updateEnabled();

        return true;
    }

    return false;
}

GeoCopyToPointsParams
GeoCopyToPoints::readParams(double time,
                            ViewSpec view) const
{
    GeoCopyToPointsParams p;
    auto text = [time, view](const KnobStringWPtr& weak,
                             const std::string& fallback) {
        const KnobStringPtr knob = weak.lock();

        return knob ? knob->getValueAtTime(time, 0, view) : fallback;
    };
    auto toggle = [time, view](const KnobBoolWPtr& weak,
                               bool fallback) {
        const KnobBoolPtr knob = weak.lock();

        return knob ? knob->getValueAtTime(time, 0, view) : fallback;
    };
    p.sourceGroup = text(_sourceGroup, p.sourceGroup);
    p.targetGroup = text(_targetGroup, p.targetGroup);
    p.packAndInstance = toggle(_packAndInstance, p.packAndInstance);
    if (const KnobChoicePtr pivot = _pivot.lock()) {
        p.pivot = pivot->getValueAtTime(time, 0, view) == (int)eGeoCopyPivotOrigin ? eGeoCopyPivotOrigin : eGeoCopyPivotCentroid;
    }
    p.useTargetOrientations = toggle(_useTargetOrientations, p.useTargetOrientations);
    p.useImplicitNormal = toggle(_useImplicitNormal, p.useImplicitNormal);
    p.targetAttributes = text(_targetAttributes, p.targetAttributes);
    if (const KnobChoicePtr applyTo = _applyTo.lock()) {
        const int value = applyTo->getValueAtTime(time, 0, view);
        if ((value >= (int)eGeoCopyApplyToPoints) && (value <= (int)eGeoCopyApplyToPrimitives)) {
            p.applyTo = (GeoCopyApplyToEnum)value;
        }
    }

    return p;
}

StatusEnum
GeoCopyToPoints::renderGeometry(const GeoRenderActionArgs& args,
                                GeoDetailConstPtr* outputDetail,
                                std::string* errorMessage)
{
    const GeoDetailConstPtr source = args.getInputDetail(0);
    const GeoDetailConstPtr target = args.getInputDetail(1);
    if (!source || !target) {
        if (errorMessage) {
            *errorMessage = tr("GeoCopyToPoints needs geometry on both its Source and Target inputs.").toStdString();
        }

        return eStatusFailed;
    }
    const GeoCopyToPointsParams params = readParams(args.time, args.view);

    return renderGeometryGenerator([&source, &target, &params](GeoDetailBuilder& detail, std::string* error) {
        return copyGeometryToPoints(source, *target, params, detail, error) ? eStatusOK : eStatusFailed;
    },
                                   outputDetail, errorMessage);
}

NATRON_NAMESPACE_EXIT
