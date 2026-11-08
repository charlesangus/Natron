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

#ifndef Engine_Nodes_Geometry_GeoInterpolate_h
#define Engine_Nodes_Geometry_GeoInterpolate_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "Engine/GeoDetail.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Attribute helpers shared by the geometry operators that build a new detail from an
 * input one: copying elements between attributes of the same description, and blending
 * elements with weights.
 **/

/** @brief True for a non-array attribute with integer or floating storage. */
bool geoIsNumeric(const GeoAttribute& attribute);

/** @brief Reads tupleSize() components of a numeric attribute as doubles. */
void geoReadNumeric(const GeoAttribute& attribute,
                    std::size_t offset,
                    std::span<double> out);

/** @brief Writes tupleSize() components; integer storage rounds to nearest. */
void geoWriteNumeric(GeoAttribute& attribute,
                     std::size_t offset,
                     std::span<const double> in);

/**
 * @brief Copies one element of @p source into @p destination, which must have the same
 * description. Works for every storage, arrays included.
 **/
void geoCopyElement(GeoAttribute& destination,
                    std::size_t destinationOffset,
                    const GeoAttribute& source,
                    std::size_t sourceOffset);

/**
 * @brief Writes the weighted sum of @p sourceOffsets' elements. Weights should sum to one.
 * Normals are renormalized and quaternions too. Strings and arrays cannot be blended: they take
 * the element with the largest weight.
 **/
void geoBlendElement(GeoAttribute& destination,
                     std::size_t destinationOffset,
                     const GeoAttribute& source,
                     std::span<const GeoOffset> sourceOffsets,
                     std::span<const double> weights);

/**
 * @brief Adds to @p builder an attribute described like each of @p source's attributes in @p cls
 * (P excepted) and returns the pairs, source first, in the source's order.
 **/
std::vector<std::pair<const GeoAttribute*, GeoAttribute*>>
geoMirrorAttributes(const GeoDetail& source,
                    GeoDetailBuilder& builder,
                    GeoAttribClassEnum cls);

/** @brief Copies every detail attribute of @p source onto @p builder's detail. */
void geoCopyDetailAttributes(const GeoDetail& source,
                             GeoDetailBuilder& builder);

/** @brief A point's uv (the attribute @p name): its own point value, else the average of its vertices'. */
class GeoPointUVReader {
public:
    explicit GeoPointUVReader(const GeoDetail& detail,
                              const std::string& name = "uv");

    bool valid() const
    {
        return _pointUV || _vertexUV;
    }

    std::array<double, 2> uv(GeoOffset point) const;

private:
    const GeoDetail& _detail;
    const GeoAttribute* _pointUV = nullptr;
    const GeoAttribute* _vertexUV = nullptr;
    std::shared_ptr<const GeoReverseTopology> _reverse;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Geometry_GeoInterpolate_h
