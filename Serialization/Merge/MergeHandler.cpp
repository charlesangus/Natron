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

#include "Serialization/Merge/MergeHandler.h"

#include <algorithm>
#include <tuple>

NATRON_NAMESPACE_ENTER
namespace Serialization {

namespace {

    typedef std::tuple<std::optional<std::list<CurveSerialization>>, std::optional<std::map<int, std::string>>, std::optional<std::string>> KnobExtras;

    std::optional<KnobDimensionSerialization>
    dimensionState(const KnobSerialization* knob,
                   std::size_t index)
    {
        if (!knob || (index >= knob->dimensions.size()) || isDefaultDimension(knob->dimensions[index])) {
            return std::nullopt;
        }
        KnobDimensionSerialization state = knob->dimensions[index];
        state.defaultValue.reset();

        return state;
    }

    KnobExtras
    extrasOf(const KnobSerialization* knob)
    {
        if (!knob) {
            return KnobExtras();
        }

        return KnobExtras(knob->parametricCurves, knob->stringAnimation, knob->choiceOptionID);
    }

    std::size_t
    dimensionCount(const KnobSerialization* a,
                   const KnobSerialization* b,
                   const KnobSerialization* c)
    {
        std::size_t count = 0;
        for (const KnobSerialization* knob : { a, b, c }) {
            if (knob) {
                count = std::max(count, knob->dimensions.size());
            }
        }

        return count;
    }

    enum class Pick {
        eMine,
        eTheirs,
        eConflict,
    };

    template <typename T>
    Pick
    pick(const T& base,
         const T& mine,
         const T& theirs)
    {
        if (mine == theirs) {
            return Pick::eMine;
        }
        if (mine == base) {
            return Pick::eTheirs;
        }
        if (theirs == base) {
            return Pick::eMine;
        }

        return Pick::eConflict;
    }

    KnobDimensionSerialization
    defaultDimension(const KnobSerialization& frame,
                     std::size_t index,
                     const KnobSerialization* side)
    {
        if (side && (index < side->dimensions.size()) && isDefaultDimension(side->dimensions[index])) {
            return side->dimensions[index];
        }
        KnobDimensionSerialization dimension;
        if (index < frame.dimensions.size()) {
            dimension.defaultValue = frame.dimensions[index].defaultValue;
            dimension.value = dimension.defaultValue;
        }

        return dimension;
    }

} // namespace

KnobMergeHandler::~KnobMergeHandler() = default;

bool
isDefaultDimension(const KnobDimensionSerialization& dimension)
{
    if (!dimension.enabled || dimension.curve || dimension.master || dimension.linkGroup || !dimension.expression.empty() || dimension.driver) {
        return false;
    }

    return !dimension.value || (dimension.defaultValue && (*dimension.value == *dimension.defaultValue));
}

bool
isDefaultKnob(const KnobSerialization& knob)
{
    if (knob.isUserKnob || knob.parametricCurves || knob.stringAnimation || knob.choiceOptionID) {
        return false;
    }

    return std::all_of(knob.dimensions.begin(), knob.dimensions.end(), [](const KnobDimensionSerialization& dimension) {
        return isDefaultDimension(dimension);
    });
}

bool
sameKnobState(const KnobSerialization* a,
              const KnobSerialization* b)
{
    if ((a && a->isUserKnob) != (b && b->isUserKnob)) {
        return false;
    }
    const std::size_t count = dimensionCount(a, b, nullptr);
    for (std::size_t i = 0; i < count; ++i) {
        if (dimensionState(a, i) != dimensionState(b, i)) {
            return false;
        }
    }

    return extrasOf(a) == extrasOf(b);
}

void
ScalarKnobMergeHandler::merge(const KnobSerialization* base,
                              const KnobSerialization* mine,
                              const KnobSerialization* theirs,
                              std::optional<KnobSerialization>* merged,
                              std::vector<MergeConflict>* conflicts) const
{
    merged->reset();
    const KnobSerialization* frame = mine ? mine : theirs;
    if (!frame) {
        return;
    }
    const std::string& name = frame->name;

    KnobSerialization result = *frame;
    const std::size_t count = dimensionCount(base, mine, theirs);
    result.dimensions.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const Pick choice = pick(dimensionState(base, i), dimensionState(mine, i), dimensionState(theirs, i));
        if (choice == Pick::eConflict) {
            MergeConflict conflict;
            conflict.kind = MergeConflictKind::eValue;
            conflict.knob = name;
            conflict.dimension = int(i);
            conflicts->push_back(conflict);
        }
        const KnobSerialization* side = (choice == Pick::eTheirs) ? theirs : mine;
        if (dimensionState(side, i)) {
            result.dimensions[i] = side->dimensions[i];
        } else {
            result.dimensions[i] = defaultDimension(*frame, i, side);
        }
    }

    const Pick extras = pick(extrasOf(base), extrasOf(mine), extrasOf(theirs));
    if (extras == Pick::eConflict) {
        MergeConflict conflict;
        conflict.kind = MergeConflictKind::eValue;
        conflict.knob = name;
        conflicts->push_back(conflict);
    }
    std::tie(result.parametricCurves, result.stringAnimation, result.choiceOptionID) = extrasOf((extras == Pick::eTheirs) ? theirs : mine);

    if (!isDefaultKnob(result)) {
        *merged = std::move(result);
    }
}

MergeHandlerRegistry::MergeHandlerRegistry()
    : _byPlugin()
    , _byKind()
    , _scalar(std::make_shared<ScalarKnobMergeHandler>())
{
}

void
MergeHandlerRegistry::setPluginHandler(const std::string& pluginID,
                                       std::shared_ptr<const KnobMergeHandler> handler)
{
    _byPlugin[pluginID] = std::move(handler);
}

void
MergeHandlerRegistry::setKindHandler(const std::string& typeName,
                                     std::shared_ptr<const KnobMergeHandler> handler)
{
    _byKind[typeName] = std::move(handler);
}

const KnobMergeHandler&
MergeHandlerRegistry::handlerFor(const std::string& pluginID,
                                 const std::string& typeName) const
{
    std::map<std::string, std::shared_ptr<const KnobMergeHandler>>::const_iterator found = _byPlugin.find(pluginID);
    if (found != _byPlugin.end()) {
        return *found->second;
    }
    found = _byKind.find(typeName);
    if (found != _byKind.end()) {
        return *found->second;
    }

    return *_scalar;
}

const MergeHandlerRegistry&
MergeHandlerRegistry::defaults()
{
    static const MergeHandlerRegistry registry;

    return registry;
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT
