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

#include "Gui/SceneDrawFilter.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <set>
#include <unordered_map>

#ifdef emit
#define NATRON_SCENEDRAWFILTER_RESTORE_EMIT
#undef emit
#endif
#include <pxr/imaging/hd/overlayContainerDataSource.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/sceneIndexPluginRegistry.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/visibilitySchema.h>
#ifdef NATRON_SCENEDRAWFILTER_RESTORE_EMIT
#undef NATRON_SCENEDRAWFILTER_RESTORE_EMIT
#define emit
#endif

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
std::mutex filtersMutex;
std::unordered_map<std::string, SceneDrawFilter::WeakPtr> filters;

// UNDOCUMENTED: the viewer cannot compute its engine's renderInstanceId. It is the string
// "UsdImagingGLEngine_%s_%p" in libusd_usdImagingGL.so, whose %p appears to be the engine, known
// only once the constructor has returned, and HdUtils::RenderInstanceTracker's register-first
// protocol (pxr/imaging/hd/utils.h) needs it earlier. So the id is learned here, which relies on
// UsdImagingGLEngine's constructor building its render index synchronously on the calling thread.
// hd/utils.h only says the callback "currently" runs during render index construction.
thread_local std::string appendedInstanceId;

HdSceneIndexBaseRefPtr
appendFilter(const std::string& renderInstanceId,
             const HdSceneIndexBaseRefPtr& input,
             const HdContainerDataSourceHandle& /*inputArgs*/)
{
    TfRefPtr<SceneDrawFilter> filter = SceneDrawFilter::New(input);
    {
        std::lock_guard<std::mutex> lock(filtersMutex);
        filters[renderInstanceId] = SceneDrawFilter::WeakPtr(filter);
    }
    appendedInstanceId = renderInstanceId;

    return filter;
}

// Named one by one because this filter runs ahead of the delegate's filters, which is where implicit
// surfaces turn into meshes. A geomSubset is left out: its mesh decides whether it draws.
bool
isHideable(const TfToken& primType)
{
    static const std::unordered_set<TfToken, TfToken::HashFunctor> hideable = {
        HdPrimTypeTokens->capsule,
        HdPrimTypeTokens->cone,
        HdPrimTypeTokens->cube,
        HdPrimTypeTokens->cylinder,
        HdPrimTypeTokens->mesh,
        HdPrimTypeTokens->tetMesh,
        HdPrimTypeTokens->nurbsPatch,
        HdPrimTypeTokens->basisCurves,
        HdPrimTypeTokens->nurbsCurves,
        HdPrimTypeTokens->particleField,
        HdPrimTypeTokens->plane,
        HdPrimTypeTokens->points,
        HdPrimTypeTokens->sphere,
        HdPrimTypeTokens->volume,
        HdPrimTypeTokens->model,
        HdPrimTypeTokens->instancer
    };

    return (hideable.count(primType) != 0) || HdPrimTypeIsLight(primType);
}

bool
hiddenBy(const SceneDrawFilter::Rules& rules,
         const SdfPath& path)
{
    if (rules.empty()) {
        return false;
    }
    const SceneDrawFilter::Rules::const_iterator rule = SdfPathFindLongestPrefix(rules, path);
    if (rule == rules.end()) {
        return false;
    }
    switch (rule->second) {
    case SceneDrawFilter::eRuleShow:
        return false;
    case SceneDrawFilter::eRuleHide:
    case SceneDrawFilter::eRuleHidden:
        return true;
    case SceneDrawFilter::eRuleExpand:
        return rule->first != path;
    }

    return false;
}

// Whatever this hides leaves Hydra. A light is hidden by a Hidden row alone: boxed geometry is no
// reason to darken the scene.
bool
underHiddenRow(const SceneDrawFilter::Rules& rules,
               const SdfPath& path)
{
    if (rules.empty()) {
        return false;
    }
    const SceneDrawFilter::Rules::const_iterator rule = SdfPathFindLongestPrefix(rules, path);

    return (rule != rules.end()) && (rule->second == SceneDrawFilter::eRuleHidden);
}

bool
hiddenFor(const SceneDrawFilter::Rules& rules,
          const SdfPath& path,
          const TfToken& primType)
{
    return HdPrimTypeIsLight(primType) ? underHiddenRow(rules, path) : hiddenBy(rules, path);
}

// Whether a descendant of `path` with no rule of its own is hidden, which needs no child's path.
bool
hiddenBelow(const SceneDrawFilter::Rules& rules,
            const SdfPath& path)
{
    if (rules.empty()) {
        return false;
    }
    const SceneDrawFilter::Rules::const_iterator rule = SdfPathFindLongestPrefix(rules, path);

    return (rule != rules.end()) && (rule->second != SceneDrawFilter::eRuleShow);
}

bool
hasRuleBelow(const SceneDrawFilter::Rules& rules,
             const SdfPath& path)
{
    const SceneDrawFilter::Rules::const_iterator next = rules.upper_bound(path);

    return (next != rules.end()) && next->first.HasPrefix(path);
}

void
addPaths(const std::vector<std::string>& paths,
         std::set<SdfPath>* roots)
{
    for (const std::string& path : paths) {
        roots->insert(SdfPath(path));
    }
}

std::vector<SdfPath>
topmost(const std::set<SdfPath>& roots)
{
    std::vector<SdfPath> kept;
    for (const SdfPath& root : roots) {
        if (kept.empty() || !root.HasPrefix(kept.back())) {
            kept.push_back(root);
        }
    }

    return kept;
}

// Laid over the input's data, so it wins over any visibility the stage authors. The renderer reads it
// through HdSceneIndexAdapterSceneDelegate::GetVisible.
const HdContainerDataSourceHandle&
invisibleData()
{
    static const HdContainerDataSourceHandle data = HdRetainedContainerDataSource::New(
        HdVisibilitySchema::GetSchemaToken(),
        HdVisibilitySchema::Builder().SetVisibility(HdRetainedTypedSampledDataSource<bool>::New(false)).Build());

    return data;
}

const HdDataSourceLocatorSet&
visibilityLocators()
{
    static const HdDataSourceLocatorSet locators { HdVisibilitySchema::GetDefaultLocator() };

    return locators;
}
} // anon namespace

void
SceneDrawFilter::registerForAllRenderers()
{
    static std::once_flag once;
    std::call_once(once, []() {
        // UNDOCUMENTED: the earliest insertion is assumed to put this ahead of the render
        // delegate's own filters; the registry header promises only relative order. Hiding works
        // wherever it sits, so a later place would cost time, not correctness.
        HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
            HdSceneIndexPluginRegistryTokens->allRenderers.GetString(),
            appendFilter,
            HdContainerDataSourceHandle(),
            0,
            HdSceneIndexPluginRegistry::InsertionOrderAtStart);
    });
}

std::string
SceneDrawFilter::takeAppendedInstanceId()
{
    std::string id;
    id.swap(appendedInstanceId);

    return id;
}

SceneDrawFilter::WeakPtr
SceneDrawFilter::find(const std::string& renderInstanceId)
{
    std::lock_guard<std::mutex> lock(filtersMutex);
    const auto it = filters.find(renderInstanceId);

    return (it != filters.end()) ? it->second : WeakPtr();
}

TfRefPtr<SceneDrawFilter>
SceneDrawFilter::New(const HdSceneIndexBaseRefPtr& input)
{
    return TfCreateRefPtr(new SceneDrawFilter(input));
}

void
SceneDrawFilter::applyToRules(const SceneDrawDelta& delta,
                              Rules* rules)
{
    for (const std::string& path : delta.cleared) {
        rules->erase(SdfPath(path));
    }
    for (const std::string& path : delta.drawn) {
        (*rules)[SdfPath(path)] = eRuleShow;
    }
    for (const std::string& path : delta.expanded) {
        (*rules)[SdfPath(path)] = eRuleExpand;
    }
    for (const std::string& path : delta.boxed) {
        (*rules)[SdfPath(path)] = eRuleHide;
    }
    for (const std::string& path : delta.hidden) {
        (*rules)[SdfPath(path)] = eRuleHidden;
    }
}

SceneDrawFilter::SceneDrawFilter(const HdSceneIndexBaseRefPtr& input)
    : HdSingleInputFilteringSceneIndexBase(input)
{
}

SceneDrawFilter::~SceneDrawFilter()
{
    std::lock_guard<std::mutex> lock(filtersMutex);
    for (auto it = filters.begin(); it != filters.end(); ++it) {
        // TfWeakPtr against a raw or ref pointer goes through a rewritten operator== that never
        // returns under C++20.
        if (get_pointer(it->second) == this) {
            filters.erase(it);
            break;
        }
    }
}

bool
SceneDrawFilter::isHidden(const SdfPath& path,
                          const TfToken& primType) const
{
    if (hiddenFor(_rules, path, primType)) {
        return true;
    }

    return _revealing && hiddenFor(_revealFrom, path, primType) && (_revealed.count(path) == 0);
}

bool
SceneDrawFilter::hasDrawnLights(const LightPredicate& counts) const
{
    for (const SdfPath& light : _lights) {
        if ((!counts || counts(light)) && !isHidden(light, HdPrimTypeTokens->rectLight)) {
            return true;
        }
    }

    return false;
}

bool
SceneDrawFilter::hidesLights() const
{
    const auto hidden = [](const Rules::value_type& rule) {
        return rule.second == eRuleHidden;
    };

    return std::any_of(_rules.begin(), _rules.end(), hidden) || (_revealing && std::any_of(_revealFrom.begin(), _revealFrom.end(), hidden));
}

void
SceneDrawFilter::trackLights(const HdSceneIndexObserver::AddedPrimEntries& entries)
{
    for (const HdSceneIndexObserver::AddedPrimEntry& entry : entries) {
        if (HdPrimTypeIsLight(entry.primType)) {
            _lights.insert(entry.primPath);
        } else {
            _lights.erase(entry.primPath);
        }
    }
}

// False lets a walk stop here: a row that goes on hiding a million prims must not visit them.
bool
SceneDrawFilter::mayChangeBelow(const SdfPath& path,
                                bool toHidden) const
{
    if (hasRuleBelow(_rules, path) || hasRuleBelow(_revealFrom, path)) {
        return true;
    }

    if ((hiddenBelow(_rules, path) == toHidden) && (hiddenBelow(_revealFrom, path) != toHidden)) {
        return true;
    }

    return (underHiddenRow(_rules, path) == toHidden) && (underHiddenRow(_revealFrom, path) != toHidden);
}

HdSceneIndexPrim
SceneDrawFilter::GetPrim(const SdfPath& primPath) const
{
    HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(primPath);
    if (!isHideable(prim.primType) || !isHidden(primPath, prim.primType)) {
        return prim;
    }
    if (_invisible.count(primPath) != 0) {
        prim.dataSource = HdOverlayContainerDataSource::OverlayedContainerDataSources(invisibleData(), prim.dataSource);

        return prim;
    }

    return HdSceneIndexPrim();
}

SdfPathVector
SceneDrawFilter::GetChildPrimPaths(const SdfPath& primPath) const
{
    return _GetInputSceneIndex()->GetChildPrimPaths(primPath);
}

void
SceneDrawFilter::seedRules(const Rules& rules)
{
    _rules = rules;
}

SceneDrawFilter::OutputEnum
SceneDrawFilter::heldOutput(const SdfPath& path) const
{
    return (_invisible.count(path) != 0) ? eOutputInvisible : eOutputAbsent;
}

void
SceneDrawFilter::transition(const SdfPath& path,
                            const TfToken& primType,
                            OutputEnum before,
                            OutputEnum after)
{
    if (after == eOutputInvisible) {
        _invisible.insert(path);
    } else {
        _invisible.erase(path);
    }
    const auto at = _changeAt.emplace(path, _changes.size());
    if (at.second) {
        _changes.push_back(Change { path, primType, before, after });

        return;
    }
    Change& change = _changes[at.first->second];
    change.after = after;
    if (!primType.IsEmpty()) {
        change.primType = primType;
    }
}

void
SceneDrawFilter::sendChanges()
{
    HdSceneIndexObserver::AddedPrimEntries added;
    HdSceneIndexObserver::DirtiedPrimEntries dirtied;
    for (const Change& change : _changes) {
        OutputEnum after = change.after;
        if ((after == eOutputInvisible) && (change.before == eOutputAbsent)) {
            // Shown and boxed again before Hydra heard of it: Hydra has still never held it.
            _invisible.erase(change.path);
            after = eOutputAbsent;
        }
        if (after == change.before) {
            continue;
        }
        if (after == eOutputAbsent) {
            added.push_back(HdSceneIndexObserver::AddedPrimEntry(change.path, TfToken()));
            ++_hiddenSent;
        } else if ((after == eOutputShown) && (change.before == eOutputAbsent)) {
            const TfToken primType = change.primType.IsEmpty() ? _GetInputSceneIndex()->GetPrim(change.path).primType : change.primType;
            added.push_back(HdSceneIndexObserver::AddedPrimEntry(change.path, primType));
            ++_shownSent;
        } else {
            dirtied.push_back(HdSceneIndexObserver::DirtiedPrimEntry(change.path, visibilityLocators()));
            ++((after == eOutputShown) ? _shownSent : _hiddenSent);
        }
    }
    _changes.clear();
    _changeAt.clear();
    if (!added.empty()) {
        _SendPrimsAdded(added);
    }
    if (!dirtied.empty()) {
        _visibilitySent += dirtied.size();
        _SendPrimsDirtied(dirtied);
    }
}

void
SceneDrawFilter::applyDelta(const SceneDrawDelta& delta,
                            std::size_t revealBatch)
{
    applyDeltas(std::vector<const SceneDrawDelta*> { &delta }, revealBatch);
}

void
SceneDrawFilter::applyDeltas(const std::vector<const SceneDrawDelta*>& deltas,
                             std::size_t revealBatch)
{
    Rules target = _rules;
    std::set<SdfPath> hideRoots;
    std::set<SdfPath> showRoots;
    for (const SceneDrawDelta* delta : deltas) {
        applyToRules(*delta, &target);
        addPaths(delta->boxed, &hideRoots);
        addPaths(delta->hidden, &hideRoots);
        addPaths(delta->expanded, &hideRoots);
        addPaths(delta->cleared, &hideRoots);
        addPaths(delta->boxed, &showRoots);
        addPaths(delta->hidden, &showRoots);
        addPaths(delta->expanded, &showRoots);
        addPaths(delta->cleared, &showRoots);
        addPaths(delta->drawn, &showRoots);
    }

    if (_revealing) {
        showRoots.insert(_revealRoots.begin(), _revealRoots.end());
        _rules = std::move(_revealFrom);
        _revealFrom.clear();
        _revealing = false;
        for (const SdfPath& path : _revealed) {
            transition(path, TfToken(), eOutputShown, underHiddenRow(_rules, path) ? eOutputAbsent : eOutputInvisible);
        }
        _revealed.clear();
        _revealStack.clear();
        _revealRoots.clear();
    }

    _revealFrom = std::move(_rules);
    _rules = std::move(target);
    _revealing = true;

    const HdSceneIndexBaseRefPtr& input = _GetInputSceneIndex();
    SdfPathVector walk;
    const std::vector<SdfPath> hideWalk = topmost(hideRoots);
    walk.assign(hideWalk.rbegin(), hideWalk.rend());
    while (!walk.empty()) {
        const SdfPath path = walk.back();
        walk.pop_back();
        const TfToken primType = input->GetPrim(path).primType;
        if (isHideable(primType) && hiddenFor(_rules, path, primType)) {
            const OutputEnum before = hiddenFor(_revealFrom, path, primType) ? heldOutput(path) : eOutputShown;
            const bool leaves = (before == eOutputAbsent) || underHiddenRow(_rules, path);
            if (before != (leaves ? eOutputAbsent : eOutputInvisible)) {
                transition(path, primType, before, leaves ? eOutputAbsent : eOutputInvisible);
            }
        }
        if (mayChangeBelow(path, true)) {
            const SdfPathVector children = input->GetChildPrimPaths(path);
            walk.insert(walk.end(), children.rbegin(), children.rend());
        }
    }

    _revealRoots = topmost(showRoots);
    _revealStack.assign(_revealRoots.rbegin(), _revealRoots.rend());
    if (_revealStack.empty()) {
        finishRevealIfDone();
    }
    if ((revealBatch == 0) && _revealing) {
        revealUpTo((std::numeric_limits<std::size_t>::max)());
    }
    sendChanges();
}

std::size_t
SceneDrawFilter::stepReveal(std::size_t maxPrims)
{
    const std::size_t shown = revealUpTo(maxPrims);
    sendChanges();

    return shown;
}

std::size_t
SceneDrawFilter::revealUpTo(std::size_t maxPrims)
{
    if (!_revealing) {
        return 0;
    }
    const HdSceneIndexBaseRefPtr& input = _GetInputSceneIndex();
    std::size_t shown = 0;
    // Children are read only as each parent comes up, so a small batch does not pay for walking the
    // whole subtree first.
    while (!_revealStack.empty() && (shown < maxPrims)) {
        const SdfPath path = _revealStack.back();
        _revealStack.pop_back();
        const TfToken primType = input->GetPrim(path).primType;
        if (!hiddenFor(_rules, path, primType) && hiddenFor(_revealFrom, path, primType)) {
            if (isHideable(primType) && _revealed.insert(path).second) {
                transition(path, primType, heldOutput(path), eOutputShown);
                ++shown;
            }
        }
        if (mayChangeBelow(path, false)) {
            const SdfPathVector children = input->GetChildPrimPaths(path);
            _revealStack.insert(_revealStack.end(), children.rbegin(), children.rend());
        }
    }
    finishRevealIfDone();

    return shown;
}

void
SceneDrawFilter::finishRevealIfDone()
{
    if (!_revealStack.empty()) {
        return;
    }
    _revealing = false;
    _revealFrom.clear();
    _revealed.clear();
    _revealRoots.clear();
}

void
SceneDrawFilter::_PrimsAdded(const HdSceneIndexBase& /*sender*/,
                             const HdSceneIndexObserver::AddedPrimEntries& entries)
{
    _inputAdded += entries.size();
    _lastInputAdded = Clock::now();
    trackLights(entries);
    if (_rules.empty() && !_revealing) {
        _SendPrimsAdded(entries);

        return;
    }
    HdSceneIndexObserver::AddedPrimEntries rewritten;
    for (std::size_t i = 0; i < std::size_t(entries.size()); ++i) {
        const HdSceneIndexObserver::AddedPrimEntry& entry = entries[i];
        if (!isHideable(entry.primType)) {
            _invisible.erase(entry.primPath);

            continue;
        }
        if (_revealing && !hiddenFor(_rules, entry.primPath, entry.primType) && hiddenFor(_revealFrom, entry.primPath, entry.primType)) {
            // The reveal's walk may be past this prim's parent, and nothing would announce it then.
            _revealed.insert(entry.primPath);
            _invisible.erase(entry.primPath);

            continue;
        }
        // A prim Hydra holds invisible stays held: passed on, its GetPrim() keeps it invisible.
        if (isHidden(entry.primPath, entry.primType) && (_invisible.count(entry.primPath) == 0)) {
            if (rewritten.empty()) {
                rewritten = entries;
            }
            rewritten[i].primType = TfToken();
        }
    }
    _SendPrimsAdded(rewritten.empty() ? entries : rewritten);
}

void
SceneDrawFilter::_PrimsRemoved(const HdSceneIndexBase& /*sender*/,
                               const HdSceneIndexObserver::RemovedPrimEntries& entries)
{
    _inputRemoved += entries.size();
    for (const HdSceneIndexObserver::RemovedPrimEntry& entry : entries) {
        for (std::set<SdfPath>* held : { &_lights, &_invisible }) {
            std::set<SdfPath>::iterator it = held->lower_bound(entry.primPath);
            while ((it != held->end()) && it->HasPrefix(entry.primPath)) {
                it = held->erase(it);
            }
        }
    }
    _SendPrimsRemoved(entries);
}

void
SceneDrawFilter::_PrimsDirtied(const HdSceneIndexBase& /*sender*/,
                               const HdSceneIndexObserver::DirtiedPrimEntries& entries)
{
    _inputDirtied += entries.size();
    if (_rules.empty() && !_revealing) {
        _SendPrimsDirtied(entries);

        return;
    }
    const HdSceneIndexBaseRefPtr& input = _GetInputSceneIndex();
    HdSceneIndexObserver::DirtiedPrimEntries kept;
    for (const HdSceneIndexObserver::DirtiedPrimEntry& entry : entries) {
        const TfToken primType = input->GetPrim(entry.primPath).primType;
        // Storm defers an invisible prim's sync, keeping the dirty bits for when it shows again, so
        // dropping them would show stale data.
        if (!isHideable(primType) || !isHidden(entry.primPath, primType) || (_invisible.count(entry.primPath) != 0)) {
            kept.push_back(entry);
        }
    }
    if (!kept.empty()) {
        _SendPrimsDirtied(kept);
    }
}

NATRON_NAMESPACE_EXIT
