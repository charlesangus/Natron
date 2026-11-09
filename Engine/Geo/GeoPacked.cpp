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

#include "GeoPacked.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

NATRON_NAMESPACE_ENTER

GeoMatrix4
geoMatrixIdentity()
{
    GeoMatrix4 m {};
    m[0] = m[5] = m[10] = m[15] = 1.;

    return m;
}

GeoMatrix4
geoMatrixMultiply(const GeoMatrix4& a,
                  const GeoMatrix4& b)
{
    GeoMatrix4 out {};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            double sum = 0.;
            for (int k = 0; k < 4; ++k) {
                sum += a[r * 4 + k] * b[k * 4 + c];
            }
            out[r * 4 + c] = sum;
        }
    }

    return out;
}

std::optional<GeoMatrix4>
geoMatrixInverse(const GeoMatrix4& m)
{
    double a[4][8];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            a[r][c] = m[r * 4 + c];
            a[r][4 + c] = r == c ? 1. : 0.;
        }
    }
    for (int col = 0; col < 4; ++col) {
        int pivot = col;
        for (int r = col + 1; r < 4; ++r) {
            if (std::abs(a[r][col]) > std::abs(a[pivot][col])) {
                pivot = r;
            }
        }
        if (a[pivot][col] == 0.) {
            return std::nullopt;
        }
        if (pivot != col) {
            for (int c = 0; c < 8; ++c) {
                std::swap(a[pivot][c], a[col][c]);
            }
        }
        const double inv = 1. / a[col][col];
        for (int c = 0; c < 8; ++c) {
            a[col][c] *= inv;
        }
        for (int r = 0; r < 4; ++r) {
            if (r == col) {
                continue;
            }
            const double f = a[r][col];
            if (f != 0.) {
                for (int c = 0; c < 8; ++c) {
                    a[r][c] -= f * a[col][c];
                }
            }
        }
    }
    GeoMatrix4 out;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out[r * 4 + c] = a[r][4 + c];
        }
    }

    return out;
}

std::array<double, 3>
geoTransformPoint(const GeoMatrix4& m,
                  const std::array<double, 3>& p)
{
    std::array<double, 3> out;
    for (int r = 0; r < 3; ++r) {
        out[r] = m[r * 4] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3];
    }

    return out;
}

GeoBounds
geoTransformBounds(const GeoMatrix4& m,
                   const GeoBounds& b)
{
    constexpr double inf = std::numeric_limits<double>::infinity();
    GeoBounds out;
    out.min = { inf, inf, inf };
    out.max = { -inf, -inf, -inf };
    for (int corner = 0; corner < 8; ++corner) {
        const std::array<double, 3> p = {
            (corner & 1) ? b.max[0] : b.min[0],
            (corner & 2) ? b.max[1] : b.min[1],
            (corner & 4) ? b.max[2] : b.min[2],
        };
        const std::array<double, 3> t = geoTransformPoint(m, p);
        for (int i = 0; i < 3; ++i) {
            out.min[i] = std::min(out.min[i], t[i]);
            out.max[i] = std::max(out.max[i], t[i]);
        }
    }

    return out;
}

GeoBounds
geoUnionBounds(const GeoBounds& a,
               const GeoBounds& b)
{
    GeoBounds out;
    for (int i = 0; i < 3; ++i) {
        out.min[i] = std::min(a.min[i], b.min[i]);
        out.max[i] = std::max(a.max[i], b.max[i]);
    }

    return out;
}

namespace {

const GeoAttribute&
requireAttribute(const GeoDetail& detail,
                 const char* name)
{
    const GeoAttribute* a = detail.findAttribute(eGeoAttribClassPrimitive, name);
    if (!a) {
        throw std::invalid_argument(std::string("geometry has no ") + name + " attribute");
    }

    return *a;
}

void
requirePacked(const GeoDetail& detail,
              GeoOffset primitive,
              bool geometryOnly,
              bool usdOnly)
{
    if (primitive < 0 || primitive >= detail.primitiveMap().offsetSize() || !detail.primitiveMap().isLive(primitive)) {
        throw std::invalid_argument("not a live primitive");
    }
    const GeoPrimitiveTypeEnum type = detail.primitiveType(primitive);
    if (!geoPrimitiveIsPacked(type) || (geometryOnly && type != eGeoPrimitivePackedGeometry) || (usdOnly && type != eGeoPrimitivePackedUSD)) {
        throw std::invalid_argument("primitive has the wrong type");
    }
}

GeoMatrix4
readMatrix(const GeoDetail& detail,
           GeoOffset primitive,
           const char* name)
{
    GeoMatrix4 m;
    requireAttribute(detail, name).getTuple<double>(std::size_t(primitive), std::span<double>(m));

    return m;
}

GeoBounds
readBounds(const GeoAttribute& a,
           GeoOffset primitive)
{
    std::array<double, 6> v;
    a.getTuple<double>(std::size_t(primitive), std::span<double>(v));

    return GeoBounds { { v[0], v[1], v[2] }, { v[3], v[4], v[5] } };
}

GeoAttribute&
writableAttribute(GeoDetailBuilder& builder,
                  const char* name)
{
    GeoAttribute* a = builder.attributeForWriting(eGeoAttribClassPrimitive, name);
    if (!a) {
        throw std::invalid_argument(std::string("geometry has no ") + name + " attribute");
    }

    return *a;
}

void
ensureAttribute(GeoDetailBuilder& builder,
                const char* name,
                GeoStorageEnum storage,
                int tupleSize,
                GeoTypeInfoEnum typeInfo,
                const GeoMatrix4* identityDefault)
{
    GeoAttributeDesc desc;
    desc.name = name;
    desc.storage = storage;
    desc.tupleSize = tupleSize;
    desc.typeInfo = typeInfo;
    const GeoAttribute* existing = builder.detail().findAttribute(eGeoAttribClassPrimitive, name);
    if (existing && existing->desc() == desc) {
        return;
    }
    GeoAttribute& a = builder.addAttribute(eGeoAttribClassPrimitive, desc);
    if (identityDefault) {
        const std::span<const double> identity(*identityDefault);
        if (a.size() == 0) {
            a.setDefault<double>(identity);
        } else {
            a.fill<double>(0, a.size(), identity);
        }
    }
}

void
ensurePackedAttributes(GeoDetailBuilder& builder,
                       GeoPrimitiveTypeEnum type)
{
    const GeoMatrix4 identity = geoMatrixIdentity();
    ensureAttribute(builder, kGeoPackedXformAttr, eGeoStorageFloat64, 16, eGeoTypeInfoMatrix, &identity);
    ensureAttribute(builder, kGeoPackedBoundsAttr, eGeoStorageFloat64, 6, eGeoTypeInfoNone, nullptr);
    if (type == eGeoPrimitivePackedGeometry) {
        ensureAttribute(builder, kGeoPackedDetailAttr, eGeoStorageInt32, 1, eGeoTypeInfoNone, nullptr);
    } else {
        ensureAttribute(builder, kGeoPackedUSDPathAttr, eGeoStorageString, 1, eGeoTypeInfoNone, nullptr);
        ensureAttribute(builder, kGeoPackedUSDParentXformAttr, eGeoStorageFloat64, 16, eGeoTypeInfoMatrix, &identity);
    }
}

void
writeBounds(GeoAttribute& a,
            GeoOffset primitive,
            const GeoBounds& b)
{
    const std::array<double, 6> v = { b.min[0], b.min[1], b.min[2], b.max[0], b.max[1], b.max[2] };
    a.setTuple<double>(std::size_t(primitive), std::span<const double>(v));
}

} // namespace

GeoBounds
geoDetailBounds(const GeoDetail& detail)
{
    constexpr double inf = std::numeric_limits<double>::infinity();
    GeoBounds out;
    out.min = { inf, inf, inf };
    out.max = { -inf, -inf, -inf };
    bool any = false;

    detail.pointMap().forEachLive([&](GeoIndex, GeoOffset point) {
        const std::array<double, 3> p = detail.pointPosition(point);
        for (int i = 0; i < 3; ++i) {
            out.min[i] = std::min(out.min[i], p[i]);
            out.max[i] = std::max(out.max[i], p[i]);
        }
        any = true;
    });
    detail.primitiveMap().forEachLive([&](GeoIndex, GeoOffset prim) {
        if (geoPrimitiveIsPacked(detail.primitiveType(prim))) {
            const GeoBounds nested = geoPackedWorldBounds(detail, prim);
            out = any ? geoUnionBounds(out, nested) : nested;
            any = true;
        }
    });

    return any ? out : GeoBounds();
}

GeoOffset
geoAppendPackedGeometries(GeoDetailBuilder& builder,
                          std::span<const GeoOffset> pivotPoints,
                          const GeoDetailConstPtr& contained,
                          std::span<const GeoMatrix4> transforms)
{
    if (!contained) {
        throw std::invalid_argument("geoAppendPackedGeometries: null detail");
    }
    if (transforms.size() != pivotPoints.size()) {
        throw std::invalid_argument("geoAppendPackedGeometries: one transform per pivot point");
    }

    ensurePackedAttributes(builder, eGeoPrimitivePackedGeometry);
    const std::int32_t index = builder.addSharedDetail(contained);
    const GeoBounds bounds = geoDetailBounds(*contained);

    const std::vector<std::int32_t> counts(pivotPoints.size(), 1);
    const GeoOffset first = builder.appendPrimitives(eGeoPrimitivePackedGeometry, counts, pivotPoints);
    const std::size_t begin = std::size_t(first);
    const std::size_t end = begin + pivotPoints.size();

    const std::int32_t ref = index + 1;
    writableAttribute(builder, kGeoPackedDetailAttr).fill<std::int32_t>(begin, end, std::span<const std::int32_t>(&ref, 1));
    const std::array<double, 6> v = { bounds.min[0], bounds.min[1], bounds.min[2], bounds.max[0], bounds.max[1], bounds.max[2] };
    writableAttribute(builder, kGeoPackedBoundsAttr).fill<double>(begin, end, std::span<const double>(v));
    GeoAttribute& xforms = writableAttribute(builder, kGeoPackedXformAttr);
    for (std::size_t i = 0; i < transforms.size(); ++i) {
        xforms.setTuple<double>(begin + i, std::span<const double>(transforms[i]));
    }

    return first;
}

GeoOffset
geoAppendPackedGeometry(GeoDetailBuilder& builder,
                        GeoOffset pivotPoint,
                        const GeoDetailConstPtr& contained,
                        const GeoMatrix4& transform)
{
    return geoAppendPackedGeometries(builder, std::span<const GeoOffset>(&pivotPoint, 1), contained, std::span<const GeoMatrix4>(&transform, 1));
}

GeoOffset
geoAppendPackedUSD(GeoDetailBuilder& builder,
                   GeoOffset pivotPoint,
                   std::string_view sourcePath,
                   const GeoMatrix4& parentWorldTransform,
                   const GeoMatrix4& transform,
                   const GeoBounds& localBounds)
{
    ensurePackedAttributes(builder, eGeoPrimitivePackedUSD);
    const std::int32_t count = 1;
    const GeoOffset first = builder.appendPrimitives(eGeoPrimitivePackedUSD, std::span<const std::int32_t>(&count, 1), std::span<const GeoOffset>(&pivotPoint, 1));

    writableAttribute(builder, kGeoPackedUSDPathAttr).setString(std::size_t(first), sourcePath);
    writableAttribute(builder, kGeoPackedUSDParentXformAttr).setTuple<double>(std::size_t(first), std::span<const double>(parentWorldTransform));
    writableAttribute(builder, kGeoPackedXformAttr).setTuple<double>(std::size_t(first), std::span<const double>(transform));
    writeBounds(writableAttribute(builder, kGeoPackedBoundsAttr), first, localBounds);

    return first;
}

GeoMatrix4
geoPackedTransform(const GeoDetail& detail,
                   GeoOffset primitive)
{
    requirePacked(detail, primitive, false, false);

    return readMatrix(detail, primitive, kGeoPackedXformAttr);
}

void
geoSetPackedTransform(GeoDetailBuilder& builder,
                      GeoOffset primitive,
                      const GeoMatrix4& transform)
{
    requirePacked(builder.detail(), primitive, false, false);
    writableAttribute(builder, kGeoPackedXformAttr).setTuple<double>(std::size_t(primitive), std::span<const double>(transform));
}

GeoBounds
geoPackedLocalBounds(const GeoDetail& detail,
                     GeoOffset primitive)
{
    requirePacked(detail, primitive, false, false);

    return readBounds(requireAttribute(detail, kGeoPackedBoundsAttr), primitive);
}

void
geoSetPackedLocalBounds(GeoDetailBuilder& builder,
                        GeoOffset primitive,
                        const GeoBounds& bounds)
{
    requirePacked(builder.detail(), primitive, false, false);
    writeBounds(writableAttribute(builder, kGeoPackedBoundsAttr), primitive, bounds);
}

GeoBounds
geoPackedWorldBounds(const GeoDetail& detail,
                     GeoOffset primitive)
{
    return geoTransformBounds(geoPackedTransform(detail, primitive), geoPackedLocalBounds(detail, primitive));
}

GeoDetailConstPtr
geoPackedDetail(const GeoDetail& detail,
                GeoOffset primitive)
{
    requirePacked(detail, primitive, true, false);
    const std::int32_t ref = requireAttribute(detail, kGeoPackedDetailAttr).get<std::int32_t>(std::size_t(primitive)) - 1;
    const std::span<const GeoDetailConstPtr> table = detail.sharedDetails();
    if (ref < 0 || std::size_t(ref) >= table.size()) {
        throw std::invalid_argument("packed geometry primitive has no embedded detail");
    }

    return table[std::size_t(ref)];
}

std::string_view
geoPackedUSDPath(const GeoDetail& detail,
                 GeoOffset primitive)
{
    requirePacked(detail, primitive, false, true);

    return requireAttribute(detail, kGeoPackedUSDPathAttr).getString(std::size_t(primitive));
}

GeoMatrix4
geoPackedUSDParentTransform(const GeoDetail& detail,
                            GeoOffset primitive)
{
    requirePacked(detail, primitive, false, true);

    return readMatrix(detail, primitive, kGeoPackedUSDParentXformAttr);
}

void
geoSetPackedUSDParentTransform(GeoDetailBuilder& builder,
                               GeoOffset primitive,
                               const GeoMatrix4& parentWorldTransform)
{
    requirePacked(builder.detail(), primitive, false, true);
    writableAttribute(builder, kGeoPackedUSDParentXformAttr).setTuple<double>(std::size_t(primitive), std::span<const double>(parentWorldTransform));
}

std::optional<GeoMatrix4>
geoPackedUSDLocalFromWorld(const GeoDetail& detail,
                           GeoOffset primitive,
                           const GeoMatrix4& worldTransform)
{
    const std::optional<GeoMatrix4> inverse = geoMatrixInverse(geoPackedUSDParentTransform(detail, primitive));
    if (!inverse) {
        return std::nullopt;
    }

    return geoMatrixMultiply(*inverse, worldTransform);
}

NATRON_NAMESPACE_EXIT
