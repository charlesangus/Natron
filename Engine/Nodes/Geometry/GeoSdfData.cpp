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

#include "GeoSdfData.h"

#include <algorithm>
#include <utility>

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {

bool
bracket(const std::set<double>& times,
        double time,
        double* tLower,
        double* tUpper)
{
    if (times.empty()) {
        return false;
    }
    if (time <= *times.begin()) {
        *tLower = *tUpper = *times.begin();

        return true;
    }
    if (time >= *times.rbegin()) {
        *tLower = *tUpper = *times.rbegin();

        return true;
    }
    auto upper = times.lower_bound(time);
    if (*upper == time) {
        *tLower = *tUpper = time;

        return true;
    }
    *tUpper = *upper;
    --upper;
    *tLower = *upper;

    return true;
}

} // namespace

GeoSdfData::GeoSdfData(std::shared_ptr<const GeoUsdConvert> convert,
                       GeoDetailConstPtr source)
    : _convert(std::move(convert))
    , _source(std::move(source))
{
}

GeoSdfData::~GeoSdfData()
{
}

bool
GeoSdfData::hasEdits() const
{
    return !_edits.empty();
}

void
GeoSdfData::rebase(std::shared_ptr<const GeoUsdConvert> convert,
                   GeoDetailConstPtr source)
{
    _convert = std::move(convert);
    _source = std::move(source);
    _edits.clear();
}

bool
GeoSdfData::StreamsData() const
{
    // False makes SdfLayer apply a reload as field-level edits with fine-grained notices
    // instead of replacing the whole layer content.
    return false;
}

const GeoSdfData::Edit*
GeoSdfData::findEdit(const SdfPath& path) const
{
    if (_edits.empty()) {
        return nullptr;
    }
    const auto it = _edits.find(path);

    return it == _edits.end() ? nullptr : &it->second;
}

bool
GeoSdfData::ancestorHidesBase(const SdfPath& path) const
{
    if (_edits.empty()) {
        return false;
    }
    for (SdfPath p = path.GetParentPath(); !p.IsEmpty(); p = p.GetParentPath()) {
        const Edit* e = findEdit(p);
        if (e && (e->erased || e->replaces)) {
            return true;
        }
        if (p == SdfPath::AbsoluteRootPath()) {
            break;
        }
    }

    return false;
}

GeoSdfData::Edit&
GeoSdfData::editFor(const SdfPath& path)
{
    return _edits[path];
}

void
GeoSdfData::CreateSpec(const SdfPath& path,
                       SdfSpecType specType)
{
    Edit& e = editFor(path);
    e = Edit();
    e.replaces = true;
    e.type = specType;
}

bool
GeoSdfData::HasSpec(const SdfPath& path) const
{
    return GetSpecType(path) != SdfSpecTypeUnknown;
}

void
GeoSdfData::EraseSpec(const SdfPath& path)
{
    Edit& e = editFor(path);
    e = Edit();
    e.erased = true;
}

void
GeoSdfData::MoveSpec(const SdfPath& oldPath,
                     const SdfPath& newPath)
{
    const SdfSpecType type = GetSpecType(oldPath);
    if (type == SdfSpecTypeUnknown) {
        return;
    }
    std::vector<std::pair<TfToken, VtValue>> fields;
    for (const TfToken& name : List(oldPath)) {
        fields.emplace_back(name, Get(oldPath, name));
    }
    CreateSpec(newPath, type);
    Edit& moved = editFor(newPath);
    for (auto& f : fields) {
        moved.fields[f.first] = std::move(f.second);
    }
    EraseSpec(oldPath);
}

SdfSpecType
GeoSdfData::GetSpecType(const SdfPath& path) const
{
    if (const Edit* e = findEdit(path)) {
        if (e->erased) {
            return SdfSpecTypeUnknown;
        }
        if (e->replaces) {
            return e->type;
        }
    }
    if (ancestorHidesBase(path)) {
        return SdfSpecTypeUnknown;
    }

    return _convert->specType(path);
}

bool
GeoSdfData::getField(const SdfPath& path,
                     const TfToken& fieldName,
                     VtValue* value) const
{
    if (const Edit* e = findEdit(path)) {
        if (e->erased) {
            return false;
        }
        const auto it = e->fields.find(fieldName);
        if (it != e->fields.end()) {
            if (value) {
                *value = it->second;
            }

            return true;
        }
        if (e->replaces || e->erasedFields.count(fieldName)) {
            return false;
        }
    }
    if (ancestorHidesBase(path)) {
        return false;
    }

    return _convert->field(path, fieldName, value);
}

bool
GeoSdfData::Has(const SdfPath& path,
                const TfToken& fieldName,
                SdfAbstractDataValue* value) const
{
    VtValue v;
    if (!getField(path, fieldName, value ? &v : nullptr)) {
        return false;
    }

    return value ? value->StoreValue(v) : true;
}

bool
GeoSdfData::Has(const SdfPath& path,
                const TfToken& fieldName,
                VtValue* value) const
{
    return getField(path, fieldName, value);
}

VtValue
GeoSdfData::Get(const SdfPath& path,
                const TfToken& fieldName) const
{
    VtValue v;
    getField(path, fieldName, &v);

    return v;
}

void
GeoSdfData::Set(const SdfPath& path,
                const TfToken& fieldName,
                const VtValue& value)
{
    if (value.IsEmpty()) {
        Erase(path, fieldName);

        return;
    }
    Edit& e = editFor(path);
    e.fields[fieldName] = value;
    e.erasedFields.erase(fieldName);
}

void
GeoSdfData::Set(const SdfPath& path,
                const TfToken& fieldName,
                const SdfAbstractDataConstValue& value)
{
    VtValue v;
    if (value.GetValue(&v)) {
        Set(path, fieldName, v);
    }
}

void
GeoSdfData::Erase(const SdfPath& path,
                  const TfToken& fieldName)
{
    Edit& e = editFor(path);
    e.fields.erase(fieldName);
    if (!e.replaces) {
        e.erasedFields.insert(fieldName);
    }
}

std::vector<TfToken>
GeoSdfData::List(const SdfPath& path) const
{
    std::vector<TfToken> names;
    const Edit* e = findEdit(path);
    if (e && e->erased) {
        return names;
    }
    if (!(e && e->replaces) && !ancestorHidesBase(path)) {
        names = _convert->fieldNames(path);
    }
    if (!e) {
        return names;
    }
    names.erase(std::remove_if(names.begin(), names.end(),
                               [&](const TfToken& n) { return e->erasedFields.count(n) > 0 || e->fields.count(n) > 0; }),
                names.end());
    for (const auto& f : e->fields) {
        names.push_back(f.first);
    }

    return names;
}

SdfTimeSampleMap
GeoSdfData::timeSamples(const SdfPath& path) const
{
    VtValue v;
    if (getField(path, SdfFieldKeys->TimeSamples, &v) && v.IsHolding<SdfTimeSampleMap>()) {
        return v.UncheckedGet<SdfTimeSampleMap>();
    }

    return SdfTimeSampleMap();
}

std::set<double>
GeoSdfData::ListAllTimeSamples() const
{
    // The converter authors defaults only, so samples exist only where an edit put them.
    std::set<double> times;
    for (const auto& [path, edit] : _edits) {
        if (edit.fields.count(SdfFieldKeys->TimeSamples) && HasSpec(path)) {
            for (const auto& sample : timeSamples(path)) {
                times.insert(sample.first);
            }
        }
    }

    return times;
}

std::set<double>
GeoSdfData::ListTimeSamplesForPath(const SdfPath& path) const
{
    std::set<double> times;
    for (const auto& sample : timeSamples(path)) {
        times.insert(sample.first);
    }

    return times;
}

bool
GeoSdfData::GetBracketingTimeSamples(double time,
                                     double* tLower,
                                     double* tUpper) const
{
    return bracket(ListAllTimeSamples(), time, tLower, tUpper);
}

size_t
GeoSdfData::GetNumTimeSamplesForPath(const SdfPath& path) const
{
    return timeSamples(path).size();
}

bool
GeoSdfData::GetBracketingTimeSamplesForPath(const SdfPath& path,
                                            double time,
                                            double* tLower,
                                            double* tUpper) const
{
    return bracket(ListTimeSamplesForPath(path), time, tLower, tUpper);
}

bool
GeoSdfData::QueryTimeSample(const SdfPath& path,
                            double time,
                            VtValue* optionalValue) const
{
    const SdfTimeSampleMap samples = timeSamples(path);
    const auto it = samples.find(time);
    if (it == samples.end()) {
        return false;
    }
    if (optionalValue) {
        *optionalValue = it->second;
    }

    return true;
}

bool
GeoSdfData::QueryTimeSample(const SdfPath& path,
                            double time,
                            SdfAbstractDataValue* optionalValue) const
{
    VtValue v;
    if (!QueryTimeSample(path, time, &v)) {
        return false;
    }

    return optionalValue ? optionalValue->StoreValue(v) : true;
}

void
GeoSdfData::SetTimeSample(const SdfPath& path,
                          double time,
                          const VtValue& value)
{
    if (value.IsEmpty()) {
        EraseTimeSample(path, time);

        return;
    }
    SdfTimeSampleMap samples = timeSamples(path);
    samples[time] = value;
    Set(path, SdfFieldKeys->TimeSamples, VtValue(samples));
}

void
GeoSdfData::EraseTimeSample(const SdfPath& path,
                            double time)
{
    SdfTimeSampleMap samples = timeSamples(path);
    if (!samples.erase(time)) {
        return;
    }
    if (samples.empty()) {
        Erase(path, SdfFieldKeys->TimeSamples);
    } else {
        Set(path, SdfFieldKeys->TimeSamples, VtValue(samples));
    }
}

void
GeoSdfData::_VisitSpecs(SdfAbstractDataSpecVisitor* visitor) const
{
    bool stopped = false;
    _convert->visitSpecs([&](const SdfPath& path) {
        if (!HasSpec(path)) {
            return true;
        }
        if (!visitor->VisitSpec(*this, path)) {
            stopped = true;

            return false;
        }

        return true;
    });
    if (!stopped) {
        for (const auto& [path, edit] : _edits) {
            if (edit.replaces && !edit.erased && !_convert->hasSpec(path) && HasSpec(path)) {
                if (!visitor->VisitSpec(*this, path)) {
                    break;
                }
            }
        }
    }
    visitor->Done(*this);
}

NATRON_NAMESPACE_EXIT
