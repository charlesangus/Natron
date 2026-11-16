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

#include "Engine/Geo/GeoSpreadsheet.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <stdexcept>
#include <string_view>
#include <type_traits>

#include "Engine/Geo/GeoPacked.h"

NATRON_NAMESPACE_ENTER

namespace {

std::atomic<std::uint64_t> s_pagesRead { 0 };

constexpr std::size_t kCompactArrayElements = 3;

template <typename T>
void
appendNumber(std::string& s,
             T v)
{
    char buf[64];
    const std::to_chars_result r = std::to_chars(buf, buf + sizeof(buf), v);
    s.append(buf, r.ptr);
}

void
appendComponent(std::string& s,
                const GeoAttribute& a,
                std::size_t offset,
                int component)
{
    switch (a.storage()) {
    case eGeoStorageInt32:
        appendNumber(s, a.get<std::int32_t>(offset, component));
        break;
    case eGeoStorageInt64:
        appendNumber(s, a.get<std::int64_t>(offset, component));
        break;
    case eGeoStorageFloat32:
        appendNumber(s, a.get<float>(offset, component));
        break;
    case eGeoStorageFloat64:
        appendNumber(s, a.get<double>(offset, component));
        break;
    case eGeoStorageString:
        s += a.getString(offset, component);
        break;
    }
}

template <typename T>
void
appendFlat(std::string& s,
           const std::vector<T>& values,
           int tupleSize,
           std::size_t maxTuples)
{
    const std::size_t tuples = values.size() / std::size_t(tupleSize);
    const std::size_t shown = std::min(tuples, maxTuples);
    s += '[';
    for (std::size_t t = 0; t < shown; ++t) {
        if (t) {
            s += ", ";
        }
        if (tupleSize > 1) {
            s += '(';
        }
        for (int c = 0; c < tupleSize; ++c) {
            if (c) {
                s += ", ";
            }
            const T& v = values[t * std::size_t(tupleSize) + std::size_t(c)];
            if constexpr (std::is_same_v<T, std::string>) {
                s += v;
            } else {
                appendNumber(s, v);
            }
        }
        if (tupleSize > 1) {
            s += ')';
        }
    }
    if (shown < tuples) {
        s += ", \xE2\x80\xA6";
    }
    s += ']';
}

std::string
arrayText(const GeoAttribute& a,
          std::size_t offset,
          std::size_t maxTuples)
{
    std::string s = "(";
    appendNumber(s, std::int64_t(a.arrayLength(offset)));
    s += ") ";
    const int tupleSize = a.tupleSize();
    switch (a.storage()) {
    case eGeoStorageInt32: {
        std::vector<std::int32_t> v;
        a.getArray(offset, v);
        appendFlat(s, v, tupleSize, maxTuples);
        break;
    }
    case eGeoStorageInt64: {
        std::vector<std::int64_t> v;
        a.getArray(offset, v);
        appendFlat(s, v, tupleSize, maxTuples);
        break;
    }
    case eGeoStorageFloat32: {
        std::vector<float> v;
        a.getArray(offset, v);
        appendFlat(s, v, tupleSize, maxTuples);
        break;
    }
    case eGeoStorageFloat64: {
        std::vector<double> v;
        a.getArray(offset, v);
        appendFlat(s, v, tupleSize, maxTuples);
        break;
    }
    case eGeoStorageString: {
        std::vector<std::string> v;
        a.getStringArray(offset, v);
        appendFlat(s, v, tupleSize, maxTuples);
        break;
    }
    }

    return s;
}

double
numericComponent(const GeoAttribute& a,
                 std::size_t offset,
                 int component)
{
    switch (a.storage()) {
    case eGeoStorageInt32:
        return double(a.get<std::int32_t>(offset, component));
    case eGeoStorageInt64:
        return double(a.get<std::int64_t>(offset, component));
    case eGeoStorageFloat32:
        return double(a.get<float>(offset, component));
    case eGeoStorageFloat64:
        return a.get<double>(offset, component);
    case eGeoStorageString:
        break;
    }

    return 0.;
}

std::string
componentName(const GeoAttribute& a,
              int component)
{
    if (a.tupleSize() == 1) {
        return a.name();
    }
    static constexpr char kVector[] = { 'x', 'y', 'z', 'w' };
    static constexpr char kColour[] = { 'r', 'g', 'b', 'a' };
    std::string suffix;
    switch (a.typeInfo()) {
    case eGeoTypeInfoVector:
    case eGeoTypeInfoNormal:
    case eGeoTypeInfoPoint:
    case eGeoTypeInfoQuaternion:
        if (a.tupleSize() <= 4) {
            suffix = std::string(1, kVector[component]);
        }
        break;
    case eGeoTypeInfoColour:
        if (a.tupleSize() <= 4) {
            suffix = std::string(1, kColour[component]);
        }
        break;
    default:
        break;
    }
    if (suffix.empty()) {
        suffix = std::to_string(component);
    }

    return a.name() + "[" + suffix + "]";
}

void
appendVec3(std::string& s,
           const std::array<double, 3>& v)
{
    s += '(';
    for (std::size_t i = 0; i < 3; ++i) {
        if (i) {
            s += ", ";
        }
        appendNumber(s, v[i]);
    }
    s += ')';
}

const char*
kindName(GeoPrimitiveTypeEnum type)
{
    switch (type) {
    case eGeoPrimitivePolygon:
        return "polygon";
    case eGeoPrimitivePolyline:
        return "polyline";
    case eGeoPrimitivePackedGeometry:
        return "packedGeometry";
    case eGeoPrimitivePackedUSD:
        return "packedUSD";
    }

    return "";
}

std::string
transformText(const GeoMatrix4& m,
              bool full)
{
    std::string s;
    if (!full) {
        s = "T";
        appendVec3(s, { m[3], m[7], m[11] });

        return s;
    }
    for (std::size_t r = 0; r < 4; ++r) {
        if (r) {
            s += '\n';
        }
        for (std::size_t c = 0; c < 4; ++c) {
            if (c) {
                s += ' ';
            }
            appendNumber(s, m[r * 4 + c]);
        }
    }

    return s;
}

std::string
boundsText(const GeoBounds& b)
{
    std::string s;
    appendVec3(s, b.min);
    s += " - ";
    appendVec3(s, b.max);

    return s;
}

void
countPages(std::span<const GeoOffset> offsets)
{
    std::uint64_t pages = 0;
    GeoOffset last = -1;
    for (const GeoOffset o : offsets) {
        const GeoOffset page = o >> kGeoPageBits;
        if (page != last) {
            ++pages;
            last = page;
        }
    }
    s_pagesRead.fetch_add(pages, std::memory_order_relaxed);
}

} // namespace

std::uint64_t
geoSpreadsheetPagesRead()
{
    return s_pagesRead.load(std::memory_order_relaxed);
}

void
geoSpreadsheetResetPagesRead()
{
    s_pagesRead.store(0, std::memory_order_relaxed);
}

GeoSpreadsheetSource::~GeoSpreadsheetSource() = default;

GeoDetailSpreadsheetSource::GeoDetailSpreadsheetSource(GeoDetailConstPtr detail)
    : _detail(std::move(detail))
{
    if (!_detail) {
        throw std::invalid_argument("GeoDetailSpreadsheetSource needs a detail");
    }
    for (int c = 0; c < kGeoAttribClassCount; ++c) {
        const GeoAttribClassEnum cls = GeoAttribClassEnum(c);
        std::vector<GeoSpreadsheetColumn>& cols = _columns[c];
        std::vector<Column>& binds = _bindings[c];
        const auto add = [&](std::string name, bool full, Column b) {
            cols.push_back(GeoSpreadsheetColumn { std::move(name), full, std::string() });
            binds.push_back(b);
        };
        if (cls == eGeoAttribClassVertex) {
            add("Point", false, Column { Column::eVertexPoint, 0, 0 });
        }
        const bool packed = cls == eGeoAttribClassPrimitive && _detail->findAttribute(cls, kGeoPackedXformAttr);
        if (packed) {
            add("intrinsic:kind", false, Column { Column::ePrimitiveKind, 0, 0 });
            add("intrinsic:transform", true, Column { Column::ePrimitiveTransform, 0, 0 });
            add("intrinsic:bounds", false, Column { Column::ePrimitiveBounds, 0, 0 });
            if (_detail->findAttribute(cls, kGeoPackedUSDPathAttr)) {
                add("intrinsic:sourcePath", false, Column { Column::ePrimitiveSourcePath, 0, 0 });
            }
        }
        const std::size_t count = _detail->attributeCount(cls);
        for (std::size_t i = 0; i < count; ++i) {
            const GeoAttribute& a = _detail->attribute(cls, i);
            if (packed && std::string_view(a.name()).starts_with("packed")) {
                continue;
            }
            if (a.isArray()) {
                add(a.name(), true, Column { Column::eAttribute, i, 0 });
                continue;
            }
            for (int k = 0; k < a.tupleSize(); ++k) {
                add(componentName(a, k), false, Column { Column::eAttribute, i, k });
            }
        }
    }
}

GeoDetailSpreadsheetSource::~GeoDetailSpreadsheetSource() = default;

std::int64_t
GeoDetailSpreadsheetSource::rowCount(GeoAttribClassEnum cls) const
{
    return cls == eGeoAttribClassDetail ? 1 : _detail->indexMap(cls).size();
}

const std::vector<GeoSpreadsheetColumn>&
GeoDetailSpreadsheetSource::columns(GeoAttribClassEnum cls) const
{
    return _columns[cls];
}

namespace {

// Storage offsets of dense rows [first, first + count), found through the liveness pages from the first row on,
// so the cost follows the block and not the detail.
void
rowOffsets(const GeoDetail& detail,
           GeoAttribClassEnum cls,
           std::int64_t first,
           std::int64_t count,
           std::vector<GeoOffset>& out)
{
    out.clear();
    if (count <= 0) {
        return;
    }
    if (cls == eGeoAttribClassDetail) {
        out.push_back(0);

        return;
    }
    const GeoIndexMap& map = detail.indexMap(cls);
    out.reserve(std::size_t(count));
    if (map.isTrivial()) {
        for (std::int64_t i = 0; i < count; ++i) {
            out.push_back(first + i);
        }

        return;
    }
    const GeoOffset pages = map.pageCount();
    for (GeoOffset page = map.offsetFromIndex(first) >> GeoIndexMap::kPageShift; page < pages && std::int64_t(out.size()) < count; ++page) {
        map.forEachLiveRunInPage(page, [&](GeoIndex index, GeoOffset offset, GeoOffset runCount) {
            const std::int64_t skip = std::max<std::int64_t>(first - index, 0);
            for (std::int64_t i = skip; i < runCount && std::int64_t(out.size()) < count; ++i) {
                out.push_back(offset + i);
            }
        });
    }
}

} // namespace

void
GeoDetailSpreadsheetSource::readCells(GeoAttribClassEnum cls,
                                      std::int64_t firstRow,
                                      std::int64_t rowCountWanted,
                                      std::span<const int> columnList,
                                      std::vector<std::string>& out) const
{
    const std::int64_t total = rowCount(cls);
    firstRow = std::clamp<std::int64_t>(firstRow, 0, total);
    rowCountWanted = std::clamp<std::int64_t>(rowCountWanted, 0, total - firstRow);
    std::vector<GeoOffset> offsets;
    rowOffsets(*_detail, cls, firstRow, rowCountWanted, offsets);
    const std::size_t rows = offsets.size();
    const std::size_t cols = columnList.size();
    out.assign(rows * cols, std::string());
    for (std::size_t c = 0; c < cols; ++c) {
        const Column& col = _bindings[cls][std::size_t(columnList[c])];
        countPages(offsets);
        const GeoAttribute* a = col.kind == Column::eAttribute ? &_detail->attribute(cls, col.attribute) : nullptr;
        for (std::size_t r = 0; r < rows; ++r) {
            std::string& cell = out[r * cols + c];
            const GeoOffset o = offsets[r];
            switch (col.kind) {
            case Column::eAttribute:
                if (a->isArray()) {
                    cell = arrayText(*a, std::size_t(o), kCompactArrayElements);
                } else {
                    appendComponent(cell, *a, std::size_t(o), col.component);
                }
                break;
            case Column::eVertexPoint:
                appendNumber(cell, _detail->pointMap().indexFromOffset(_detail->vertexPoint(o)));
                break;
            default:
                cell = intrinsicText(o, col.kind, false);
                break;
            }
        }
    }
}

std::string
GeoDetailSpreadsheetSource::intrinsicText(GeoOffset primitive,
                                          Column::KindEnum kind,
                                          bool full) const
{
    const GeoPrimitiveTypeEnum type = _detail->primitiveType(primitive);
    if (kind == Column::ePrimitiveKind) {
        return kindName(type);
    }
    if (!geoPrimitiveIsPacked(type)) {
        return std::string();
    }
    switch (kind) {
    case Column::ePrimitiveTransform:
        return transformText(geoPackedTransform(*_detail, primitive), full);
    case Column::ePrimitiveBounds:
        return boundsText(geoPackedLocalBounds(*_detail, primitive));
    case Column::ePrimitiveSourcePath:
        return type == eGeoPrimitivePackedUSD ? std::string(geoPackedUSDPath(*_detail, primitive)) : std::string();
    default:
        break;
    }

    return std::string();
}

GeoSpreadsheetSortKey
GeoDetailSpreadsheetSource::sortKey(GeoAttribClassEnum cls,
                                    int column,
                                    std::int64_t row) const
{
    std::vector<GeoOffset> offsets;
    rowOffsets(*_detail, cls, row, 1, offsets);
    if (offsets.empty()) {
        return 0.;
    }
    const GeoOffset o = offsets[0];
    const Column& col = _bindings[cls][std::size_t(column)];
    switch (col.kind) {
    case Column::eAttribute: {
        const GeoAttribute& a = _detail->attribute(cls, col.attribute);
        if (a.isArray()) {
            return GeoSpreadsheetArrayLength { std::int64_t(a.arrayLength(std::size_t(o))) };
        }
        if (a.storage() == eGeoStorageString) {
            return std::string(a.getString(std::size_t(o), col.component));
        }

        return numericComponent(a, std::size_t(o), col.component);
    }
    case Column::eVertexPoint:
        return double(_detail->pointMap().indexFromOffset(_detail->vertexPoint(o)));
    case Column::ePrimitiveKind:
    case Column::ePrimitiveSourcePath:
        return intrinsicText(o, col.kind, false);
    case Column::ePrimitiveTransform:
        return geoPrimitiveIsPacked(_detail->primitiveType(o)) ? geoPackedTransform(*_detail, o)[3] : 0.;
    case Column::ePrimitiveBounds:
        return geoPrimitiveIsPacked(_detail->primitiveType(o)) ? geoPackedLocalBounds(*_detail, o).min[0] : 0.;
    }

    return 0.;
}

std::string
GeoDetailSpreadsheetSource::fullValue(GeoAttribClassEnum cls,
                                      int column,
                                      std::int64_t row) const
{
    std::vector<GeoOffset> offsets;
    rowOffsets(*_detail, cls, row, 1, offsets);
    if (offsets.empty()) {
        return std::string();
    }
    const GeoOffset o = offsets[0];
    const Column& col = _bindings[cls][std::size_t(column)];
    if (col.kind == Column::eAttribute) {
        const GeoAttribute& a = _detail->attribute(cls, col.attribute);
        if (a.isArray()) {
            return arrayText(a, std::size_t(o), std::size_t(-1));
        }
        std::string s;
        appendComponent(s, a, std::size_t(o), col.component);

        return s;
    }
    if (col.kind == Column::eVertexPoint) {
        return std::to_string(_detail->pointMap().indexFromOffset(_detail->vertexPoint(o)));
    }

    return intrinsicText(o, col.kind, true);
}

NATRON_NAMESPACE_EXIT
