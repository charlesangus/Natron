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

#include <limits>
#include <mutex>
#include <set>
#include <unordered_map>

#ifdef emit
#define NATRON_SCENEDRAWFILTER_RESTORE_EMIT
#undef emit
#endif
#include <pxr/imaging/hd/sceneIndexPluginRegistry.h>
#include <pxr/imaging/hd/tokens.h>
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

    return hideable.count(primType) != 0;
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
        return true;
    case SceneDrawFilter::eRuleExpand:
        return rule->first != path;
    }

    return false;
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
        (*rules)[SdfPath(path)] = eRuleHide;
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
SceneDrawFilter::isHidden(const SdfPath& path) const
{
    if (hiddenBy(_rules, path)) {
        return true;
    }

    return _revealing && hiddenBy(_revealFrom, path) && (_revealed.count(path) == 0);
}

// False lets a walk stop here: a row that goes on hiding a million prims must not visit them.
bool
SceneDrawFilter::mayChangeBelow(const SdfPath& path,
                                bool toHidden) const
{
    if (hasRuleBelow(_rules, path) || hasRuleBelow(_revealFrom, path)) {
        return true;
    }

    return (hiddenBelow(_rules, path) == toHidden) && (hiddenBelow(_revealFrom, path) != toHidden);
}

HdSceneIndexPrim
SceneDrawFilter::GetPrim(const SdfPath& primPath) const
{
    HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(primPath);
    if (isHidden(primPath) && isHideable(prim.primType)) {
        return HdSceneIndexPrim();
    }

    return prim;
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

void
SceneDrawFilter::applyDelta(const SceneDrawDelta& delta,
                            std::size_t revealBatch)
{
    Rules target = _rules;
    applyToRules(delta, &target);

    std::set<SdfPath> hideRoots;
    std::set<SdfPath> showRoots;
    addPaths(delta.boxed, &hideRoots);
    addPaths(delta.hidden, &hideRoots);
    addPaths(delta.expanded, &hideRoots);
    addPaths(delta.expanded, &showRoots);
    addPaths(delta.drawn, &showRoots);
    for (const std::string& cleared : delta.cleared) {
        const SdfPath path(cleared);
        (hiddenBelow(target, path) ? hideRoots : showRoots).insert(path);
    }

    HdSceneIndexObserver::AddedPrimEntries hides;
    if (_revealing) {
        showRoots.insert(_revealRoots.begin(), _revealRoots.end());
        _rules = std::move(_revealFrom);
        _revealFrom.clear();
        _revealing = false;
        for (const SdfPath& path : _revealed) {
            hides.push_back(HdSceneIndexObserver::AddedPrimEntry(path, TfToken()));
        }
        _revealed.clear();
        _revealStack.clear();
        _revealRoots.clear();
        if (!hides.empty()) {
            _hiddenSent += hides.size();
            _SendPrimsAdded(hides);
            hides.clear();
        }
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
        if (hiddenBy(_rules, path) && !hiddenBy(_revealFrom, path) && isHideable(input->GetPrim(path).primType)) {
            hides.push_back(HdSceneIndexObserver::AddedPrimEntry(path, TfToken()));
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
    if (!hides.empty()) {
        _hiddenSent += hides.size();
        _SendPrimsAdded(hides);
    }
    if ((revealBatch == 0) && _revealing) {
        stepReveal((std::numeric_limits<std::size_t>::max)());
    }
}

std::size_t
SceneDrawFilter::stepReveal(std::size_t maxPrims)
{
    if (!_revealing) {
        return 0;
    }
    const HdSceneIndexBaseRefPtr& input = _GetInputSceneIndex();
    HdSceneIndexObserver::AddedPrimEntries entries;
    // Children are read only as each parent comes up, so a small batch does not pay for walking the
    // whole subtree first.
    while (!_revealStack.empty() && (std::size_t(entries.size()) < maxPrims)) {
        const SdfPath path = _revealStack.back();
        _revealStack.pop_back();
        if (!hiddenBy(_rules, path) && hiddenBy(_revealFrom, path)) {
            const TfToken primType = input->GetPrim(path).primType;
            if (isHideable(primType) && _revealed.insert(path).second) {
                entries.push_back(HdSceneIndexObserver::AddedPrimEntry(path, primType));
            }
        }
        if (mayChangeBelow(path, false)) {
            const SdfPathVector children = input->GetChildPrimPaths(path);
            _revealStack.insert(_revealStack.end(), children.rbegin(), children.rend());
        }
    }
    finishRevealIfDone();
    const std::size_t sent = entries.size();
    if (sent != 0) {
        _shownSent += sent;
        _SendPrimsAdded(entries);
    }

    return sent;
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
    if (_rules.empty() && !_revealing) {
        _SendPrimsAdded(entries);

        return;
    }
    HdSceneIndexObserver::AddedPrimEntries rewritten;
    for (std::size_t i = 0; i < std::size_t(entries.size()); ++i) {
        const HdSceneIndexObserver::AddedPrimEntry& entry = entries[i];
        if (!isHideable(entry.primType)) {
            continue;
        }
        if (_revealing && !hiddenBy(_rules, entry.primPath) && hiddenBy(_revealFrom, entry.primPath)) {
            // The reveal's walk may be past this prim's parent, and nothing would announce it then.
            _revealed.insert(entry.primPath);

            continue;
        }
        if (isHidden(entry.primPath)) {
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
        if (!isHidden(entry.primPath) || !isHideable(input->GetPrim(entry.primPath).primType)) {
            kept.push_back(entry);
        }
    }
    if (!kept.empty()) {
        _SendPrimsDirtied(kept);
    }
}

NATRON_NAMESPACE_EXIT
