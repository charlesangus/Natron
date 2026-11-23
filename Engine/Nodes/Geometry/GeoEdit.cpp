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

#include "GeoEdit.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <exception>
#include <string_view>
#include <system_error>

#include <QString>

#include "Engine/Geo/GeoGroupOps.h"
#include "Engine/Geo/GeoOpUtils.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Geometry/GeoNormals.h"
#include "Engine/Nodes/Geometry/GeoViewerLayer.h"

NATRON_NAMESPACE_ENTER

namespace {

const char kNormalName[] = "N";
const char kFormatVersion[] = "1";

void
appendNumber(std::string* out,
             double value)
{
    char buffer[64];
    const std::to_chars_result r = std::to_chars(buffer, buffer + sizeof(buffer), value);
    out->append(buffer, r.ptr);
}

void
appendNumber(std::string* out,
             GeoIndex value)
{
    char buffer[32];
    const std::to_chars_result r = std::to_chars(buffer, buffer + sizeof(buffer), value);
    out->append(buffer, r.ptr);
}

std::vector<std::string_view>
splitOn(std::string_view s,
        char separator)
{
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= s.size()) {
        const std::size_t end = s.find(separator, start);
        const std::size_t stop = (end == std::string_view::npos) ? s.size() : end;
        parts.push_back(s.substr(start, stop - start));
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }

    return parts;
}

// The space-separated words of a record, empty words dropped.
std::vector<std::string_view>
words(std::string_view record)
{
    std::vector<std::string_view> out;
    for (std::string_view w : splitOn(record, ' ')) {
        if (!w.empty()) {
            out.push_back(w);
        }
    }

    return out;
}

template <typename T>
bool
parseNumber(std::string_view s,
            T* value)
{
    const char* end = s.data() + s.size();
    const std::from_chars_result r = std::from_chars(s.data(), end, *value);

    return (r.ec == std::errc()) && (r.ptr == end);
}

bool
isUsableNormal(const GeoAttribute* attribute)
{
    return attribute && !attribute->isArray() && (attribute->tupleSize() >= 3)
        && ((attribute->storage() == eGeoStorageFloat32) || (attribute->storage() == eGeoStorageFloat64));
}

GeoMatrix4
translation(const std::array<double, 3>& delta)
{
    GeoMatrix4 m = geoMatrixIdentity();
    m[3] = delta[0];
    m[7] = delta[1];
    m[11] = delta[2];

    return m;
}

std::vector<GeoIndex>
sortedUnique(std::vector<GeoIndex> v)
{
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());

    return v;
}
} // anonymous namespace

bool
GeoEditData::matches(const GeoDetail& detail) const
{
    return empty() || ((detail.pointCount() == pointCount) && (detail.primitiveCount() == primitiveCount));
}

std::string
GeoEditData::encode() const
{
    if (empty()) {
        return std::string();
    }
    std::string out = kFormatVersion;
    out += ';';
    appendNumber(&out, pointCount);
    out += ' ';
    appendNumber(&out, primitiveCount);
    for (const auto& [point, offset] : pointOffsets) {
        out += ";p ";
        appendNumber(&out, point);
        for (double v : offset) {
            out += ' ';
            appendNumber(&out, v);
        }
    }
    for (const auto& [primitive, m] : primitiveTransforms) {
        out += ";x ";
        appendNumber(&out, primitive);
        for (double v : m) {
            out += ' ';
            appendNumber(&out, v);
        }
    }

    return out;
}

bool
GeoEditData::decode(const std::string& text,
                    GeoEditData* data,
                    std::string* error)
{
    *data = GeoEditData();
    if (std::all_of(text.begin(), text.end(), [](char c) { return c == ' '; })) {
        return true;
    }
    auto fail = [&](const std::string& why) {
        if (error) {
            *error = why;
        }
        *data = GeoEditData();

        return false;
    };
    const std::vector<std::string_view> records = splitOn(text, ';');
    if ((records.size() < 2) || (words(records[0]).size() != 1) || (words(records[0])[0] != kFormatVersion)) {
        return fail("The edits are in an unknown format.");
    }
    const std::vector<std::string_view> counts = words(records[1]);
    if ((counts.size() != 2) || !parseNumber(counts[0], &data->pointCount) || !parseNumber(counts[1], &data->primitiveCount)
        || (data->pointCount < 0) || (data->primitiveCount < 0)) {
        return fail("The edits have malformed point and primitive counts.");
    }
    for (std::size_t r = 2; r < records.size(); ++r) {
        const std::vector<std::string_view> w = words(records[r]);
        GeoIndex number = -1;
        if ((w.size() < 2) || !parseNumber(w[1], &number) || (number < 0)) {
            return fail("An edit names no element.");
        }
        if ((w[0] == "p") && (w.size() == 5)) {
            std::array<double, 3> offset;
            for (int c = 0; c < 3; ++c) {
                if (!parseNumber(w[2 + c], &offset[c])) {
                    return fail("A point edit has a malformed offset.");
                }
            }
            data->pointOffsets[number] = offset;
        } else if ((w[0] == "x") && (w.size() == 18)) {
            GeoMatrix4 m;
            for (int c = 0; c < 16; ++c) {
                if (!parseNumber(w[2 + c], &m[c])) {
                    return fail("A primitive edit has a malformed transform.");
                }
            }
            data->primitiveTransforms[number] = m;
        } else {
            return fail("An edit is neither a point offset nor a primitive transform.");
        }
    }

    return true;
} // GeoEditData::decode

bool
GeoEdit::apply(GeoDetailBuilder& builder,
               const GeoEditData& data,
               std::string* warning)
{
    if (data.empty()) {
        return true;
    }
    const GeoDetail& detail = builder.detail();
    if (!data.matches(detail)) {
        if (warning) {
            *warning = tr("The edits were made on %1 points and %2 primitives, but the input has %3 points and %4 primitives: "
                          "they are not applied. Connect the input they were made on, or clear the edits.")
                           .arg(data.pointCount)
                           .arg(data.primitiveCount)
                           .arg(detail.pointCount())
                           .arg(detail.primitiveCount())
                           .toStdString();
        }

        return false;
    }

    std::vector<std::pair<GeoOffset, std::array<double, 3>>> positions;
    positions.reserve(data.pointOffsets.size());
    for (const auto& [point, offset] : data.pointOffsets) {
        const GeoOffset o = detail.pointMap().offsetFromIndex(point);
        if (o < 0) {
            continue;
        }
        const std::array<double, 3> p = detail.pointPosition(o);
        positions.emplace_back(o, std::array<double, 3> { p[0] + offset[0], p[1] + offset[1], p[2] + offset[2] });
    }
    std::vector<std::pair<GeoOffset, GeoMatrix4>> packed;
    for (const auto& [primitive, m] : data.primitiveTransforms) {
        const GeoOffset o = detail.primitiveMap().offsetFromIndex(primitive);
        if ((o >= 0) && geoPrimitiveIsPacked(detail.primitiveType(o))) {
            packed.emplace_back(o, geoMatrixMultiply(m, geoPackedTransform(detail, o)));
        }
    }

    GeoAttribClassEnum normalClass = eGeoAttribClassPoint;
    bool hasNormal = false;
    for (GeoAttribClassEnum cls : { eGeoAttribClassPoint, eGeoAttribClassVertex, eGeoAttribClassPrimitive }) {
        if (isUsableNormal(detail.findAttribute(cls, kNormalName))) {
            normalClass = cls;
            hasNormal = true;
            break;
        }
    }

    for (const auto& [point, p] : positions) {
        builder.setPointPosition(point, p[0], p[1], p[2]);
    }
    for (const auto& [primitive, m] : packed) {
        geoSetPackedTransform(builder, primitive, m);
    }

    // Packed primitives contribute no normals, so only point edits can change them.
    if (hasNormal && !positions.empty()) {
        GeoNormalsParams normals;
        normals.target = (normalClass == eGeoAttribClassVertex) ? eGeoNormalsTargetVertices
            : (normalClass == eGeoAttribClassPrimitive)         ? eGeoNormalsTargetPrimitives
                                                                : eGeoNormalsTargetPoints;
        std::string ignored;
        GeoNormals::computeNormals(builder, normals, &ignored);
    }

    return true;
} // GeoEdit::apply

GeoEditTargets
GeoEdit::targets(const GeoDetail& detail,
                 const GeoComponentSelection& selection)
{
    GeoEditTargets out;
    const GeoIndex pointCount = detail.pointCount();
    switch (selection.type) {
    case eGeoComponentObjects:
        break;
    case eGeoComponentPoints:
        for (GeoIndex point : selection.elements) {
            if ((point >= 0) && (point < pointCount)) {
                out.points.push_back(point);
            }
        }
        break;
    case eGeoComponentEdges:
        for (const GeoEdge& edge : selection.edges) {
            for (GeoIndex point : { edge.p0, edge.p1 }) {
                if ((point >= 0) && (point < pointCount)) {
                    out.points.push_back(point);
                }
            }
        }
        break;
    case eGeoComponentPrimitives: {
        const GeoIndexMap& primitives = detail.primitiveMap();
        GeoElementMask unpacked(std::size_t(std::max<GeoOffset>(0, primitives.offsetSize())), 0);
        for (GeoIndex primitive : selection.elements) {
            const GeoOffset o = (primitive >= 0) ? primitives.offsetFromIndex(primitive) : GeoOffset(-1);
            if ((o < 0) || (std::size_t(o) >= unpacked.size())) {
                continue;
            }
            if (geoPrimitiveIsPacked(detail.primitiveType(o))) {
                out.packedPrimitives.push_back(primitive);
            } else {
                unpacked[std::size_t(o)] = 1;
            }
        }
        for (GeoOffset point : geoMaskedOffsets(geoPromotePrimitivesToPoints(detail, unpacked))) {
            out.points.push_back(detail.pointMap().indexFromOffset(point));
        }
        break;
    }
    }
    out.points = sortedUnique(std::move(out.points));
    out.packedPrimitives = sortedUnique(std::move(out.packedPrimitives));

    return out;
} // GeoEdit::targets

bool
GeoEdit::centre(const GeoDetail& detail,
                const GeoEditTargets& targets,
                std::array<double, 3>* centre)
{
    std::array<double, 3> sum = { 0., 0., 0. };
    std::size_t count = 0;
    for (GeoIndex point : targets.points) {
        const GeoOffset o = detail.pointMap().offsetFromIndex(point);
        if (o < 0) {
            continue;
        }
        const std::array<double, 3> p = detail.pointPosition(o);
        for (int c = 0; c < 3; ++c) {
            sum[c] += p[c];
        }
        ++count;
    }
    for (GeoIndex primitive : targets.packedPrimitives) {
        const GeoOffset o = detail.primitiveMap().offsetFromIndex(primitive);
        if ((o < 0) || !geoPrimitiveIsPacked(detail.primitiveType(o))) {
            continue;
        }
        const GeoBounds b = geoPackedWorldBounds(detail, o);
        for (int c = 0; c < 3; ++c) {
            sum[c] += (b.min[c] + b.max[c]) * 0.5;
        }
        ++count;
    }
    if (count == 0) {
        return false;
    }
    for (int c = 0; c < 3; ++c) {
        (*centre)[c] = sum[c] / double(count);
    }

    return true;
}

void
GeoEdit::translate(GeoEditData* data,
                   const GeoDetail& detail,
                   const GeoEditTargets& targets,
                   const std::array<double, 3>& delta)
{
    data->pointCount = detail.pointCount();
    data->primitiveCount = detail.primitiveCount();
    for (GeoIndex point : targets.points) {
        std::array<double, 3>& offset = data->pointOffsets.try_emplace(point, std::array<double, 3> { 0., 0., 0. }).first->second;
        for (int c = 0; c < 3; ++c) {
            offset[c] += delta[c];
        }
    }
    const GeoMatrix4 t = translation(delta);
    for (GeoIndex primitive : targets.packedPrimitives) {
        GeoMatrix4& m = data->primitiveTransforms.try_emplace(primitive, geoMatrixIdentity()).first->second;
        m = geoMatrixMultiply(t, m);
    }
}

GeoEdit::GeoEdit(NodePtr node)
    : NativeEffectBase(node)
{
}

GeoEdit::~GeoEdit()
{
}

const NodeSpec*
GeoEdit::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

KnobStringPtr
GeoEdit::getEditsKnob() const
{
    return _edits.lock();
}

void
GeoEdit::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls").toStdString());

    // Written by drags in the 3D view, which keep it consistent with the input; typing into it
    // could only break that.
    KnobStringPtr edits = createKnob<KnobString>(tr("Edits").toStdString());
    edits->setName(kGeoEditParamEdits);
    edits->setHintToolTip(tr("The point offsets and packed primitive transforms dragged in the 3D view, with the input's "
                             "point and primitive counts they were made on.")
                              .toStdString());
    edits->setAnimationEnabled(false);
    edits->setSecret(true);
    page->addKnob(edits);
    _edits = edits;

    KnobButtonPtr clear = createKnob<KnobButton>(tr("Clear Edits").toStdString());
    clear->setName(kGeoEditParamClearEdits);
    clear->setHintToolTip(tr("Removes every edit, so the input passes through unchanged.").toStdString());
    page->addKnob(clear);
    _clearEdits = clear;
}

bool
GeoEdit::knobChanged(KnobI* k,
                     ValueChangedReasonEnum /*reason*/,
                     ViewSpec /*view*/,
                     double /*time*/,
                     bool /*originatedFromMainThread*/)
{
    const KnobButtonPtr button = _clearEdits.lock();
    const KnobStringPtr edits = _edits.lock();
    if (!button || !edits || (k != button.get())) {
        return false;
    }
    if (!edits->getValue().empty()) {
        setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOnCreateNewCommand);
        edits->setValueFromPlugin(std::string(), ViewSpec::all(), 0);
        setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOff);
    }

    return true;
}

StatusEnum
GeoEdit::renderGeometry(const GeoRenderActionArgs& args,
                        GeoDetailConstPtr* outputDetail,
                        std::string* errorMessage)
{
    const KnobStringPtr knob = _edits.lock();
    GeoEditData data;
    std::string decodeError;
    if (knob && !GeoEditData::decode(knob->getValueAtTime(args.time, 0, args.view), &data, &decodeError)) {
        if (errorMessage) {
            *errorMessage = decodeError;
        }
        clearPersistentMessage(false);

        return eStatusFailed;
    }
    std::string warning;
    const StatusEnum status = renderGeometryFromInput(args, 0, [&data, &warning](GeoDetailBuilder& detail, std::string* error) {
        try {
            apply(detail, data, &warning);

            return eStatusOK;
        } catch (const std::exception& e) {
            if (error) {
                *error = e.what();
            }

            return eStatusFailed;
        } }, outputDetail, errorMessage);
    if (status == eStatusOK) {
        if (warning.empty()) {
            clearPersistentMessage(false);
        } else {
            setPersistentMessage(eMessageTypeWarning, warning);
        }
    }

    return status;
}

NATRON_NAMESPACE_EXIT
