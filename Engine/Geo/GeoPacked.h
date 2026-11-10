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

#ifndef Engine_Geo_GeoPacked_h
#define Engine_Geo_GeoPacked_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

#include "Engine/GeoDetail.h"

NATRON_NAMESPACE_ENTER

/** @brief Row-major 4x4, applied to column vectors: translation sits in elements 3, 7 and 11. */
typedef std::array<double, 16> GeoMatrix4;

struct GeoBounds {
    std::array<double, 3> min { 0., 0., 0. };
    std::array<double, 3> max { 0., 0., 0. };

    bool operator==(const GeoBounds&) const = default;
};

GeoMatrix4 geoMatrixIdentity();
GeoMatrix4 geoMatrixMultiply(const GeoMatrix4& a, const GeoMatrix4& b);

/** @brief nullopt for a singular matrix. */
std::optional<GeoMatrix4> geoMatrixInverse(const GeoMatrix4& m);
std::array<double, 3> geoTransformPoint(const GeoMatrix4& m, const std::array<double, 3>& p);

/** @brief The box of the eight transformed corners. */
GeoBounds geoTransformBounds(const GeoMatrix4& m, const GeoBounds& b);
GeoBounds geoUnionBounds(const GeoBounds& a, const GeoBounds& b);

/**
 * A packed primitive is one vertex on one point, the pivot, plus per-primitive intrinsics kept as
 * primitive attributes. The transform (kGeoPackedXformAttr) places the contents in the owning
 * detail's space, which for USD-sourced geometry is world space; the pivot's position does not
 * enter it. Bounds are in the primitive's own space, before the transform.
 *
 * Packed Geometry embeds an immutable GeoDetail held in the owning detail's sharedDetails(), so
 * a million instances of one detail cost a million table indices and one detail. Packed USD
 * records the source prim path and the parent's world transform at the time it was read, the
 * fallback for turning the transform back into a local one when the source stage is not at hand.
 *
 * Packed primitives must be created through the functions below, which keep the attributes
 * consistent. Setters and appenders take storage offsets, like GeoDetailBuilder, and throw
 * std::invalid_argument for a dead primitive or one of the wrong type; getters do the same.
 **/
inline constexpr const char* kGeoPackedXformAttr = "packedXform";
inline constexpr const char* kGeoPackedBoundsAttr = "packedBounds";
// Index into sharedDetails() plus one; 0 means none.
inline constexpr const char* kGeoPackedDetailAttr = "packedDetail";
inline constexpr const char* kGeoPackedUSDPathAttr = "packedUSDPath";
inline constexpr const char* kGeoPackedUSDParentXformAttr = "packedUSDParentXform";

/**
 * @brief Appends one packed geometry primitive per pivot point, all embedding @p contained,
 * with local bounds taken from the contained detail's points and nested packed primitives.
 * @p transforms holds one matrix per pivot. Returns the first new primitive offset; the new
 * offsets are consecutive. Throws std::invalid_argument for a null detail or mismatched spans.
 **/
GeoOffset geoAppendPackedGeometries(GeoDetailBuilder& builder,
                                    std::span<const GeoOffset> pivotPoints,
                                    const GeoDetailConstPtr& contained,
                                    std::span<const GeoMatrix4> transforms);

GeoOffset geoAppendPackedGeometry(GeoDetailBuilder& builder,
                                  GeoOffset pivotPoint,
                                  const GeoDetailConstPtr& contained,
                                  const GeoMatrix4& transform = geoMatrixIdentity());

GeoOffset geoAppendPackedUSD(GeoDetailBuilder& builder,
                             GeoOffset pivotPoint,
                             std::string_view sourcePath,
                             const GeoMatrix4& parentWorldTransform,
                             const GeoMatrix4& worldTransform,
                             const GeoBounds& localBounds);

/** @brief Bounds of the live points and of the nested packed primitives; zero when empty. */
GeoBounds geoDetailBounds(const GeoDetail& detail);

GeoMatrix4 geoPackedTransform(const GeoDetail& detail, GeoOffset primitive);
void geoSetPackedTransform(GeoDetailBuilder& builder, GeoOffset primitive, const GeoMatrix4& transform);

GeoBounds geoPackedLocalBounds(const GeoDetail& detail, GeoOffset primitive);
void geoSetPackedLocalBounds(GeoDetailBuilder& builder, GeoOffset primitive, const GeoBounds& bounds);

/** @brief The local bounds carried by the transform. */
GeoBounds geoPackedWorldBounds(const GeoDetail& detail, GeoOffset primitive);

/** @brief The embedded detail of a packed geometry primitive. */
GeoDetailConstPtr geoPackedDetail(const GeoDetail& detail, GeoOffset primitive);

std::string_view geoPackedUSDPath(const GeoDetail& detail, GeoOffset primitive);
GeoMatrix4 geoPackedUSDParentTransform(const GeoDetail& detail, GeoOffset primitive);
void geoSetPackedUSDParentTransform(GeoDetailBuilder& builder, GeoOffset primitive, const GeoMatrix4& parentWorldTransform);

/** @brief parent^-1 * world, or nullopt when the parent transform is singular. */
std::optional<GeoMatrix4> geoPackedUSDLocalFromWorld(const GeoDetail& detail,
                                                     GeoOffset primitive,
                                                     const GeoMatrix4& worldTransform);

NATRON_NAMESPACE_EXIT

#endif // Engine_Geo_GeoPacked_h
