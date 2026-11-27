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

#include "GeoInterpolate.h"

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <type_traits>

NATRON_NAMESPACE_ENTER

namespace {
constexpr int kMaxTuple = 64;

template <typename T>
void
readAs(const GeoAttribute& attribute,
       std::size_t offset,
       std::span<double> out)
{
    T values[kMaxTuple];
    const int n = attribute.tupleSize();
    attribute.getTuple<T>(offset, std::span<T>(values, (std::size_t)n));
    for (int c = 0; c < n; ++c) {
        out[(std::size_t)c] = (double)values[c];
    }
}

template <typename T>
void
writeAs(GeoAttribute& attribute,
        std::size_t offset,
        std::span<const double> in)
{
    T values[kMaxTuple];
    const int n = attribute.tupleSize();
    for (int c = 0; c < n; ++c) {
        if constexpr (std::is_integral_v<T>) {
            values[c] = (T)std::llround(in[(std::size_t)c]);
        } else {
            values[c] = (T)in[(std::size_t)c];
        }
    }
    attribute.setTuple<T>(offset, std::span<const T>(values, (std::size_t)n));
}

template <typename T>
void
copyArray(GeoAttribute& destination,
          std::size_t destinationOffset,
          const GeoAttribute& source,
          std::size_t sourceOffset)
{
    std::vector<T> values;
    source.getArray<T>(sourceOffset, values);
    destination.setArray<T>(destinationOffset, std::span<const T>(values.data(), values.size()));
}
} // anonymous namespace

bool
geoIsNumeric(const GeoAttribute& attribute)
{
    return !attribute.isArray() && (attribute.storage() != eGeoStorageString) && (attribute.tupleSize() <= kMaxTuple);
}

void
geoReadNumeric(const GeoAttribute& attribute,
               std::size_t offset,
               std::span<double> out)
{
    switch (attribute.storage()) {
    case eGeoStorageInt32:
        readAs<std::int32_t>(attribute, offset, out);
        break;
    case eGeoStorageInt64:
        readAs<std::int64_t>(attribute, offset, out);
        break;
    case eGeoStorageFloat32:
        readAs<float>(attribute, offset, out);
        break;
    case eGeoStorageFloat64:
        readAs<double>(attribute, offset, out);
        break;
    case eGeoStorageString:
        throw std::invalid_argument("geoReadNumeric: string attribute " + attribute.name());
    }
}

void
geoWriteNumeric(GeoAttribute& attribute,
                std::size_t offset,
                std::span<const double> in)
{
    switch (attribute.storage()) {
    case eGeoStorageInt32:
        writeAs<std::int32_t>(attribute, offset, in);
        break;
    case eGeoStorageInt64:
        writeAs<std::int64_t>(attribute, offset, in);
        break;
    case eGeoStorageFloat32:
        writeAs<float>(attribute, offset, in);
        break;
    case eGeoStorageFloat64:
        writeAs<double>(attribute, offset, in);
        break;
    case eGeoStorageString:
        throw std::invalid_argument("geoWriteNumeric: string attribute " + attribute.name());
    }
}

void
geoCopyElement(GeoAttribute& destination,
               std::size_t destinationOffset,
               const GeoAttribute& source,
               std::size_t sourceOffset)
{
    if (source.isArray()) {
        switch (source.storage()) {
        case eGeoStorageInt32:
            copyArray<std::int32_t>(destination, destinationOffset, source, sourceOffset);
            break;
        case eGeoStorageInt64:
            copyArray<std::int64_t>(destination, destinationOffset, source, sourceOffset);
            break;
        case eGeoStorageFloat32:
            copyArray<float>(destination, destinationOffset, source, sourceOffset);
            break;
        case eGeoStorageFloat64:
            copyArray<double>(destination, destinationOffset, source, sourceOffset);
            break;
        case eGeoStorageString: {
            std::vector<std::string> values;
            source.getStringArray(sourceOffset, values);
            destination.setStringArray(destinationOffset, std::span<const std::string>(values.data(), values.size()));
            break;
        }
        }

        return;
    }
    if (source.storage() == eGeoStorageString) {
        // String indices are local to each attribute's table, so the text is what is copied.
        for (int c = 0; c < source.tupleSize(); ++c) {
            destination.setString(destinationOffset, source.getString(sourceOffset, c), c);
        }

        return;
    }
    double values[kMaxTuple];
    const std::span<double> tuple(values, (std::size_t)source.tupleSize());
    geoReadNumeric(source, sourceOffset, tuple);
    geoWriteNumeric(destination, destinationOffset, tuple);
}

void
geoBlendElement(GeoAttribute& destination,
                std::size_t destinationOffset,
                const GeoAttribute& source,
                std::span<const GeoOffset> sourceOffsets,
                std::span<const double> weights)
{
    if (sourceOffsets.empty()) {
        return;
    }
    if (!geoIsNumeric(source)) {
        std::size_t best = 0;
        for (std::size_t i = 1; i < weights.size(); ++i) {
            if (weights[i] > weights[best]) {
                best = i;
            }
        }
        geoCopyElement(destination, destinationOffset, source, (std::size_t)sourceOffsets[best]);

        return;
    }
    const int n = source.tupleSize();
    double sum[kMaxTuple] = {};
    double values[kMaxTuple];
    for (std::size_t i = 0; i < sourceOffsets.size(); ++i) {
        geoReadNumeric(source, (std::size_t)sourceOffsets[i], std::span<double>(values, (std::size_t)n));
        for (int c = 0; c < n; ++c) {
            sum[c] += weights[i] * values[c];
        }
    }
    const GeoTypeInfoEnum info = source.typeInfo();
    if ((info == eGeoTypeInfoNormal) || (info == eGeoTypeInfoQuaternion)) {
        double length = 0.;
        for (int c = 0; c < n; ++c) {
            length += sum[c] * sum[c];
        }
        length = std::sqrt(length);
        if (length > 0.) {
            for (int c = 0; c < n; ++c) {
                sum[c] /= length;
            }
        }
    }
    geoWriteNumeric(destination, destinationOffset, std::span<const double>(sum, (std::size_t)n));
}

std::vector<std::pair<const GeoAttribute*, GeoAttribute*>>
geoMirrorAttributes(const GeoDetail& source,
                    GeoDetailBuilder& builder,
                    GeoAttribClassEnum cls)
{
    std::vector<const GeoAttribute*> sources;
    for (std::size_t i = 0; i < source.attributeCount(cls); ++i) {
        const GeoAttribute& attribute = source.attribute(cls, i);
        if ((cls == eGeoAttribClassPoint) && (attribute.name() == GeoDetail::kPositionName)) {
            continue;
        }
        sources.push_back(&attribute);
    }
    for (const GeoAttribute* attribute : sources) {
        builder.addAttribute(cls, attribute->desc());
    }
    std::vector<std::pair<const GeoAttribute*, GeoAttribute*>> pairs;
    pairs.reserve(sources.size());
    for (const GeoAttribute* attribute : sources) {
        pairs.emplace_back(attribute, builder.attributeForWriting(cls, attribute->name()));
    }

    return pairs;
}

void
geoCopyDetailAttributes(const GeoDetail& source,
                        GeoDetailBuilder& builder)
{
    for (const auto& [from, to] : geoMirrorAttributes(source, builder, eGeoAttribClassDetail)) {
        geoCopyElement(*to, 0, *from, 0);
    }
}

GeoPointUVReader::GeoPointUVReader(const GeoDetail& detail,
                                   const std::string& name)
    : _detail(detail)
{
    const GeoAttribute* pointUV = detail.findAttribute(eGeoAttribClassPoint, name);
    if (pointUV && geoIsNumeric(*pointUV) && (pointUV->tupleSize() >= 2)) {
        _pointUV = pointUV;

        return;
    }
    const GeoAttribute* vertexUV = detail.findAttribute(eGeoAttribClassVertex, name);
    if (vertexUV && geoIsNumeric(*vertexUV) && (vertexUV->tupleSize() >= 2)) {
        _vertexUV = vertexUV;
        _reverse = detail.reverseTopology();
    }
}

std::array<double, 2>
GeoPointUVReader::uv(GeoOffset point) const
{
    double values[kMaxTuple];
    if (_pointUV) {
        geoReadNumeric(*_pointUV, (std::size_t)point, std::span<double>(values, (std::size_t)_pointUV->tupleSize()));

        return { values[0], values[1] };
    }
    if (!_vertexUV) {
        return { 0., 0. };
    }
    std::array<double, 2> sum = { 0., 0. };
    const std::span<const GeoOffset> vertices = _reverse->pointVertices(point);
    for (GeoOffset vertex : vertices) {
        geoReadNumeric(*_vertexUV, (std::size_t)vertex, std::span<double>(values, (std::size_t)_vertexUV->tupleSize()));
        sum[0] += values[0];
        sum[1] += values[1];
    }
    if (!vertices.empty()) {
        sum[0] /= (double)vertices.size();
        sum[1] /= (double)vertices.size();
    }

    return sum;
}

NATRON_NAMESPACE_EXIT
