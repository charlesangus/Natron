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

#ifndef Engine_Geo_GeoOpUtils_h
#define Engine_Geo_GeoOpUtils_h

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
#include <string>
#include <string_view>
#include <vector>

#include "Global/GlobalDefines.h"

#include "Engine/Geo/GeoPacked.h"
#include "Engine/GeoDetail.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief One flag per storage offset of an element class, 1 for a selected element. Dead
 * offsets are never selected.
 **/
typedef std::vector<char> GeoElementMask;

/** @brief The live offsets of a map, in element-number order. */
std::vector<std::size_t> geoLiveOffsets(const GeoIndexMap& map);

/** @brief P of every point offset, dead offsets included, at double precision. */
std::vector<std::array<double, 3>> geoPointPositions(const GeoDetail& detail);

/** @brief The offsets selected in @p mask, ascending. */
std::vector<GeoOffset> geoMaskedOffsets(const GeoElementMask& mask);

/**
 * @brief The whitespace-separated terms of a group pattern, an "@attr op value" comparison
 * counting as one term even with spaces around its operator or inside a quoted value.
 **/
std::vector<std::string_view> geoSplitPattern(std::string_view pattern);

/** @brief Glob match with '*' (any run) and '?' (any one character). */
bool geoGlobMatch(std::string_view pattern, std::string_view s);

/**
 * @brief Whether @p name matches a space-separated list of globs, read left to right: a glob
 * adds the names it matches and a glob prefixed with '^' removes them again.
 **/
bool geoNameMatchesPattern(std::string_view patternList, std::string_view name);

enum GeoCompareOpEnum {
    eGeoCompareEqual = 0,
    eGeoCompareNotEqual,
    eGeoCompareLess,
    eGeoCompareLessEqual,
    eGeoCompareGreater,
    eGeoCompareGreaterEqual,
};

/**
 * @brief "attribute op value": an attribute component compared with a value. A numeric
 * attribute compares as a number; a string attribute compares equal or not equal against a
 * glob, and orders lexicographically for the other operators.
 **/
struct GeoAttribCompare {
    std::string attribute;
    int component = 0;
    GeoCompareOpEnum op = eGeoCompareEqual;
    std::string value;
};

/**
 * @brief Splits an attribute reference into its name and component: "P.y" and "P[1]" are
 * component 1 of P; ".x/.y/.z/.w", ".r/.g/.b/.a" and ".u/.v" are accepted. False for an empty
 * name or a malformed suffix.
 **/
bool geoParseAttribComponent(std::string_view text, std::string* name, int* component);

/**
 * @brief Parses "@name op value", with or without the '@'; op is one of = == != < <= > >=. A
 * value in double or single quotes is taken without them.
 **/
bool geoParseAttribCompare(std::string_view text, GeoAttribCompare* compare, std::string* error);

/** @brief False when the component is out of the attribute's tuple or the attribute is an array. */
bool geoEvalAttribCompare(const GeoAttribute& attribute, std::size_t offset, const GeoAttribCompare& compare);

/** @brief The element class a node's group pattern selects from. */
enum GeoGroupTypeEnum {
    eGeoGroupTypePoints = 0,
    eGeoGroupTypePrimitives,
};

/**
 * @brief Selects elements of a point, vertex or primitive class with a group pattern. Tokens
 * are separated by spaces (see geoSplitPattern()) and applied left to right:
 *   - "N", "A-B", "A-B:step" and "A-B:every,of": element numbers ("0-10:2" every second one,
 *     "0-9:2,3" two of every three);
 *   - a group name of the class, or a glob over the class's group names;
 *   - "*": every element;
 *   - "@attr op value": an attribute comparison over the class's attributes (for vertices, a
 *     point attribute when the vertices have none of that name);
 *   - "!token": every element the token does not select;
 *   - "^token": removes what the token selects from the selection so far.
 * An empty or blank pattern selects every element. Unknown group names select nothing.
 * Returns false with @p error set for a malformed token.
 **/
bool geoSelectElements(const GeoDetail& detail,
                       GeoAttribClassEnum cls,
                       std::string_view pattern,
                       GeoElementMask* mask,
                       std::string* error);

/** @brief Calls f(edge, primitive) for each edge of each live polygon and polyline, edges repeating where primitives share them. */
template <typename F>
void
geoForEachEdge(const GeoDetail& detail, F&& f)
{
    detail.primitiveMap().forEachLive([&](GeoIndex /*index*/, GeoOffset prim) {
        const GeoPrimitiveTypeEnum type = detail.primitiveType(prim);
        if (type != eGeoPrimitivePolygon && type != eGeoPrimitivePolyline) {
            return;
        }
        const GeoVertexRange range = detail.primitiveVertices(prim);
        if (range.count < 2) {
            return;
        }
        const GeoOffset segments = (type == eGeoPrimitivePolygon && range.count > 2) ? range.count : range.count - 1;
        for (GeoOffset i = 0; i < segments; ++i) {
            const GeoOffset a = detail.vertexPoint(range.start + i);
            const GeoOffset b = detail.vertexPoint(range.start + (i + 1) % range.count);
            if (a != b) {
                f(GeoEdge(a, b), prim);
            }
        }
    });
}

/**
 * @brief The description two attributes of one name share after a merge: integer and float
 * storage promote to float, 32 to 64 bits, and the larger tuple wins. The type info is the
 * first's unless it has none. nullopt when they cannot share one (string against numeric,
 * array against non-array).
 **/
std::optional<GeoAttributeDesc> geoPromotedDesc(const GeoAttributeDesc& a, const GeoAttributeDesc& b);

/**
 * @brief Writes src's elements srcOffsets[i] into dst's elements dstOffsets[i], converting
 * storage and padding with zeros or truncating tuples where the descriptions differ. Pages of
 * dst whose content does not change stay shared. Nothing is written when one is a string
 * attribute and the other is not, or one is an array and the other is not.
 **/
void geoCopyAttributeElements(const GeoAttribute& src,
                              std::span<const std::size_t> srcOffsets,
                              GeoAttribute& dst,
                              std::span<const std::size_t> dstOffsets);

/**
 * @brief The attribute with exactly @p desc, created when absent. An existing one of another
 * description is replaced and its values carried over through geoCopyAttributeElements().
 **/
GeoAttribute& geoConformAttribute(GeoDetailBuilder& builder, GeoAttribClassEnum cls, const GeoAttributeDesc& desc);

/** @brief Fills dst's elements [begin, end) with src's default, converted; nothing for strings and arrays. */
void geoFillWithDefaultOf(const GeoAttribute& src, GeoAttribute& dst, std::size_t begin, std::size_t end);

/**
 * @brief Gives dst src's default, converted: as dst's own default while dst is empty, otherwise
 * by filling every element, since a non-empty attribute's default cannot change.
 **/
void geoAdoptDefault(const GeoAttribute& src, GeoAttribute& dst);

enum GeoRotateOrderEnum {
    eGeoRotateOrderXYZ = 0,
    eGeoRotateOrderXZY,
    eGeoRotateOrderYXZ,
    eGeoRotateOrderYZX,
    eGeoRotateOrderZXY,
    eGeoRotateOrderZYX,
};

/**
 * @brief translate * pivot * rotate * scale * pivot^-1: scaled, then turned (degrees, the first
 * axis of the order first) about the pivot, then moved.
 **/
GeoMatrix4 geoComposeTransform(const std::array<double, 3>& translate,
                               const std::array<double, 3>& rotateDegrees,
                               const std::array<double, 3>& scale,
                               const std::array<double, 3>& pivot,
                               GeoRotateOrderEnum order);

NATRON_NAMESPACE_EXIT

#endif // Engine_Geo_GeoOpUtils_h
