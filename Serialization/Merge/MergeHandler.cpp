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
#include <set>
#include <tuple>

#include "Serialization/Merge/MergeRules.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

using MergeDetail::dimensionState;
using MergeDetail::knobNamed;
using MergeDetail::knobsByName;
using MergeDetail::Pick;
using MergeDetail::pick;

namespace {

    typedef std::tuple<std::optional<std::list<CurveSerialization>>, std::optional<std::map<int, std::string>>, std::optional<std::string>> KnobExtras;

    KnobExtras
    extrasOf(const KnobSerialization* knob)
    {
        if (!knob) {
            return KnobExtras();
        }

        return KnobExtras(knob->parametricCurves, knob->stringAnimation, knob->choiceOptionID);
    }

    bool
    sameUserDefinition(const KnobSerialization& a,
                       const KnobSerialization& b)
    {
        return (a.label == b.label) && (a.tooltip == b.tooltip) && (a.triggerNewLine == b.triggerNewLine) && (a.evaluatesOnChange == b.evaluatesOnChange)
            && (a.animationEnabled == b.animationEnabled) && (a.isPersistent == b.isPersistent) && (a.extraData == b.extraData)
            && (a.useHostOverlayHandle == b.useHostOverlayHandle);
    }

    std::size_t
    dimensionCount(const KnobSerialization* a,
                   const KnobSerialization* b,
                   const KnobSerialization* c)
    {
        std::size_t count = 0;
        for (const KnobSerialization* knob : { a, b, c }) {
            if (knob) {
                count = std::max(count, std::max(knob->dimensions.size(), std::size_t(std::max(knob->dimension, 0))));
            }
        }

        return count;
    }

    // True when the knob saved one dimension standing for its knobDimensions. The count comes from
    // every side because a side read from a project file does not record a plug-in knob's dimension.
    bool
    isFolded(const KnobSerialization* knob,
             std::size_t knobDimensions)
    {
        return knob && (knobDimensions > 1) && (knob->dimensions.size() == 1);
    }

    // One saved dimension stands for all of a knob's dimensions (the folded form saving and the
    // project file give a knob whose dimensions are all alike), so it is compared and merged as
    // count copies of itself.
    const KnobSerialization*
    unfolded(const KnobSerialization* knob,
             std::size_t count,
             std::optional<KnobSerialization>* storage)
    {
        if (!knob || (knob->dimensions.size() != 1) || (count < 2)) {
            return knob;
        }
        storage->emplace(*knob);
        (*storage)->dimensions.resize(count, knob->dimensions.front());

        return &**storage;
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

    MergeConflictValue
    valueOf(const KnobSerialization* knob)
    {
        MergeConflictValue value;
        if (knob) {
            value.knob = *knob;
        }

        return value;
    }

    MergeConflict
    valueConflict(const std::string& name,
                  int dimension,
                  const KnobSerialization* base,
                  const KnobSerialization* mine,
                  const KnobSerialization* theirs)
    {
        MergeConflict conflict;
        conflict.kind = MergeConflictKind::eValue;
        conflict.knob = name;
        conflict.dimension = dimension;
        conflict.base = valueOf(base);
        conflict.mine = valueOf(mine);
        conflict.theirs = valueOf(theirs);

        return conflict;
    }

    // Whether a side animates the knob: a curve on a dimension, parametric curves or a string
    // animation table.
    bool
    isAnimated(const KnobSerialization* knob)
    {
        if (!knob) {
            return false;
        }
        if (knob->parametricCurves || knob->stringAnimation) {
            return true;
        }

        return std::any_of(knob->dimensions.begin(), knob->dimensions.end(), [](const KnobDimensionSerialization& dimension) {
            return dimension.curve.has_value();
        });
    }

    typedef std::pair<std::vector<std::optional<KnobDimensionSerialization>>, KnobExtras> WholeKnobState;

    WholeKnobState
    wholeState(const KnobSerialization* knob,
               std::size_t count)
    {
        WholeKnobState state;
        state.first.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            state.first.push_back(dimensionState(knob, i));
        }
        state.second = extrasOf(knob);

        return state;
    }

} // namespace

namespace MergeDetail {

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

    std::map<std::string, const KnobSerialization*>
    knobsByName(const std::list<KnobSerialization>& knobs)
    {
        std::map<std::string, const KnobSerialization*> result;
        for (const KnobSerialization& knob : knobs) {
            result.emplace(knob.name, &knob);
        }

        return result;
    }

    const KnobSerialization*
    knobNamed(const std::map<std::string, const KnobSerialization*>& knobs,
              const std::string& name)
    {
        std::map<std::string, const KnobSerialization*>::const_iterator found = knobs.find(name);

        return (found == knobs.end()) ? nullptr : found->second;
    }

} // namespace MergeDetail

KnobMergeHandler::~KnobMergeHandler() = default;

NodeMergeHandler::~NodeMergeHandler() = default;

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
    std::optional<KnobSerialization> unfoldedA;
    std::optional<KnobSerialization> unfoldedB;
    a = unfolded(a, count, &unfoldedA);
    b = unfolded(b, count, &unfoldedB);
    for (std::size_t i = 0; i < count; ++i) {
        if (dimensionState(a, i) != dimensionState(b, i)) {
            return false;
        }
    }

    return extrasOf(a) == extrasOf(b);
}

bool
sameKnobLists(const std::list<KnobSerialization>& a,
              const std::list<KnobSerialization>& b)
{
    const std::map<std::string, const KnobSerialization*> left = knobsByName(a);
    const std::map<std::string, const KnobSerialization*> right = knobsByName(b);
    for (const std::pair<const std::string, const KnobSerialization*>& entry : left) {
        if (!sameKnobState(entry.second, knobNamed(right, entry.first))) {
            return false;
        }
    }
    for (const std::pair<const std::string, const KnobSerialization*>& entry : right) {
        if (!left.count(entry.first) && !sameKnobState(nullptr, entry.second)) {
            return false;
        }
    }

    return true;
}

bool
sameNodeContent(const NodeSerialization& a,
                const NodeSerialization& b)
{
    if ((a.hasRotoContext != b.hasRotoContext) || (a.hasTrackerContext != b.hasTrackerContext)) {
        return false;
    }
    if (a.hasRotoContext && !MergeDetail::sameRotoContexts(a.rotoContext, b.rotoContext)) {
        return false;
    }

    return !a.hasTrackerContext || MergeDetail::sameTrackerContexts(a.trackerContext, b.trackerContext);
}

void
ScalarKnobMergeHandler::merge(const KnobSerialization* base,
                              const KnobSerialization* mine,
                              const KnobSerialization* theirs,
                              std::optional<KnobSerialization>* merged,
                              std::vector<MergeConflict>* conflicts) const
{
    merged->reset();
    const KnobSerialization* folded = mine ? mine : theirs;
    if (!folded) {
        return;
    }
    // When every side saved the knob folded, its one dimension merges as the whole knob's and
    // stays folded; otherwise a folded side is compared dimension by dimension.
    const std::size_t knobDimensions = dimensionCount(base, mine, theirs);
    const bool allFolded = isFolded(mine, knobDimensions) && isFolded(theirs, knobDimensions) && (!base || isFolded(base, knobDimensions));
    const std::size_t count = allFolded ? 1 : knobDimensions;
    std::optional<KnobSerialization> unfoldedBase;
    std::optional<KnobSerialization> unfoldedMine;
    std::optional<KnobSerialization> unfoldedTheirs;
    base = unfolded(base, count, &unfoldedBase);
    mine = unfolded(mine, count, &unfoldedMine);
    theirs = unfolded(theirs, count, &unfoldedTheirs);
    const KnobSerialization* frame = mine ? mine : theirs;
    const std::string& name = frame->name;

    KnobSerialization result = *frame;
    result.dimensions.resize(count);
    const auto takeDimension = [&](std::size_t i, const KnobSerialization* side) {
        if (dimensionState(side, i)) {
            result.dimensions[i] = side->dimensions[i];
        } else {
            result.dimensions[i] = defaultDimension(*frame, i, side);
        }
    };

    // An animated knob is one unit: a change to any key, value, interpolation, tangent or
    // extrapolation of its curves is a change to the whole knob, and keys are never reconciled.
    if (isAnimated(base) || isAnimated(mine) || isAnimated(theirs)) {
        const Pick choice = pick(wholeState(base, count), wholeState(mine, count), wholeState(theirs, count));
        const KnobSerialization* side = (choice == Pick::eTheirs) ? theirs : mine;
        for (std::size_t i = 0; i < count; ++i) {
            takeDimension(i, side);
        }
        std::tie(result.parametricCurves, result.stringAnimation, result.choiceOptionID) = extrasOf(side);
        if (choice == Pick::eConflict) {
            conflicts->push_back(valueConflict(name, -1, base, mine, theirs));
        }
    } else {
        for (std::size_t i = 0; i < count; ++i) {
            const Pick choice = pick(dimensionState(base, i), dimensionState(mine, i), dimensionState(theirs, i));
            takeDimension(i, (choice == Pick::eTheirs) ? theirs : mine);
            if (choice == Pick::eConflict) {
                KnobDimensionSerialization dimension = result.dimensions[i];
                if (mergeDimension(base, mine, theirs, i, &dimension)) {
                    result.dimensions[i] = std::move(dimension);
                } else {
                    conflicts->push_back(valueConflict(name, allFolded ? kMergeConflictAllDimensions : int(i), base, mine, theirs));
                }
            }
        }
        const KnobExtras mineExtras = extrasOf(mine);
        const KnobExtras theirsExtras = extrasOf(theirs);
        const Pick extras = pick(extrasOf(base), mineExtras, theirsExtras);
        std::tie(result.parametricCurves, result.stringAnimation, result.choiceOptionID) = (extras == Pick::eTheirs) ? theirsExtras : mineExtras;
        if (extras == Pick::eConflict) {
            conflicts->push_back(valueConflict(name, -1, base, mine, theirs));
        }
    }

    // A merge that leaves a folded knob as it was keeps its folded form.
    if ((folded->dimensions.size() == 1) && (count > 1)
        && std::all_of(result.dimensions.begin(), result.dimensions.end(), [&](const KnobDimensionSerialization& dimension) {
               return dimension == folded->dimensions.front();
           })) {
        result.dimensions.resize(1);
    }
    if (!isDefaultKnob(result)) {
        *merged = std::move(result);
    }
}

bool
ScalarKnobMergeHandler::mergeDimension(const KnobSerialization* /*base*/,
                                       const KnobSerialization* /*mine*/,
                                       const KnobSerialization* /*theirs*/,
                                       std::size_t /*index*/,
                                       KnobDimensionSerialization* /*merged*/) const
{
    return false;
}

std::list<KnobSerialization>
mergeKnobLists(const MergeHandlerRegistry& handlers,
               const std::string& pluginID,
               const std::list<KnobSerialization>* base,
               const std::list<KnobSerialization>& mine,
               const std::list<KnobSerialization>& theirs,
               std::vector<MergeConflict>* conflicts)
{
    const std::list<KnobSerialization> none;
    const std::map<std::string, const KnobSerialization*> baseKnobs = knobsByName(base ? *base : none);
    const std::map<std::string, const KnobSerialization*> mineKnobs = knobsByName(mine);
    const std::map<std::string, const KnobSerialization*> theirsKnobs = knobsByName(theirs);

    std::vector<std::string> names;
    std::set<std::string> seen;
    for (const std::list<KnobSerialization>* list : { &mine, &theirs, base ? base : &none }) {
        for (const KnobSerialization& knob : *list) {
            if (seen.insert(knob.name).second) {
                names.push_back(knob.name);
            }
        }
    }

    std::list<KnobSerialization> result;
    for (const std::string& name : names) {
        const KnobSerialization* b = knobNamed(baseKnobs, name);
        const KnobSerialization* m = knobNamed(mineKnobs, name);
        const KnobSerialization* t = knobNamed(theirsKnobs, name);
        const bool user = (b && b->isUserKnob) || (m && m->isUserKnob) || (t && t->isUserKnob);
        if (user && (!m || !t)) {
            // A user knob exists only where a side made it, so its absence is a delete.
            const KnobSerialization* kept = m ? m : t;
            if (!kept || (b && sameKnobState(b, kept) && sameUserDefinition(*b, *kept))) {
                continue;
            }
            if (b) {
                MergeConflict conflict;
                conflict.kind = MergeConflictKind::eDeleteModify;
                conflict.knob = name;
                conflict.side = m ? MergeSide::eTheirs : MergeSide::eMine;
                conflict.base = valueOf(b);
                conflict.mine = valueOf(m);
                conflict.theirs = valueOf(t);
                conflicts->push_back(conflict);
            }
            result.push_back(*kept);
            continue;
        }
        std::optional<KnobSerialization> merged;
        handlers.handlerFor(pluginID, b, m, t).merge(b, m, t, &merged, conflicts);
        if (merged) {
            result.push_back(std::move(*merged));
        }
    }

    return result;
}

void
ContentNodeMergeHandler::merge(const NodeSerialization* base,
                               const NodeSerialization& theirs,
                               const MergeHandlerRegistry& handlers,
                               NodeSerialization* merged,
                               std::vector<MergeConflict>* conflicts) const
{
    if (merged->hasRotoContext && theirs.hasRotoContext) {
        const RotoContextSerialization* baseRoto = (base && base->hasRotoContext) ? &base->rotoContext : nullptr;
        merged->rotoContext = mergeRotoContexts(baseRoto, merged->rotoContext, theirs.rotoContext, handlers, conflicts);
    } else if (!merged->hasRotoContext && theirs.hasRotoContext && !(base && base->hasRotoContext)) {
        merged->hasRotoContext = true;
        merged->rotoContext = theirs.rotoContext;
    }
    if (merged->hasTrackerContext && theirs.hasTrackerContext) {
        const TrackerContextSerialization* baseTracker = (base && base->hasTrackerContext) ? &base->trackerContext : nullptr;
        merged->trackerContext = mergeTrackerContexts(baseTracker, merged->trackerContext, theirs.trackerContext, handlers, conflicts);
    } else if (!merged->hasTrackerContext && theirs.hasTrackerContext && !(base && base->hasTrackerContext)) {
        merged->hasTrackerContext = true;
        merged->trackerContext = theirs.trackerContext;
    }
}

MergeHandlerRegistry::MergeHandlerRegistry()
    : _byPlugin()
    , _byKind()
    , _scalar(std::make_shared<ScalarKnobMergeHandler>())
    , _nodeByPlugin()
    , _content(std::make_shared<ContentNodeMergeHandler>())
{
    _byKind["String"] = std::make_shared<TextKnobMergeHandler>();
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

void
MergeHandlerRegistry::setNodeHandler(const std::string& pluginID,
                                     std::shared_ptr<const NodeMergeHandler> handler)
{
    _nodeByPlugin[pluginID] = std::move(handler);
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

namespace {

    bool
    holdsMultiLineString(const KnobSerialization* knob)
    {
        if (!knob) {
            return false;
        }
        for (const KnobDimensionSerialization& dimension : knob->dimensions) {
            if (!dimension.value) {
                continue;
            }
            const std::string* text = std::get_if<std::string>(&*dimension.value);
            if (text && (text->find('\n') != std::string::npos)) {
                return true;
            }
        }

        return false;
    }

} // namespace

const KnobMergeHandler&
MergeHandlerRegistry::handlerFor(const std::string& pluginID,
                                 const KnobSerialization* base,
                                 const KnobSerialization* mine,
                                 const KnobSerialization* theirs) const
{
    const KnobSerialization* any = mine ? mine : (theirs ? theirs : base);
    if (any->typeName.empty() && (holdsMultiLineString(base) || holdsMultiLineString(mine) || holdsMultiLineString(theirs))) {
        return handlerFor(pluginID, "String");
    }

    return handlerFor(pluginID, any->typeName);
}

const NodeMergeHandler&
MergeHandlerRegistry::nodeHandlerFor(const std::string& pluginID) const
{
    std::map<std::string, std::shared_ptr<const NodeMergeHandler>>::const_iterator found = _nodeByPlugin.find(pluginID);

    return (found == _nodeByPlugin.end()) ? *_content : *found->second;
}

const MergeHandlerRegistry&
MergeHandlerRegistry::defaults()
{
    static const MergeHandlerRegistry registry;

    return registry;
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT
