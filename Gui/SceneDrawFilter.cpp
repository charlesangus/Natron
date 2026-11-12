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
#include <iterator>
#include <limits>
#include <mutex>
#include <set>
#include <unordered_map>

#ifdef emit
#define NATRON_SCENEDRAWFILTER_RESTORE_EMIT
#undef emit
#endif
#include <pxr/imaging/hd/instancerTopologySchema.h>
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

using RulePtr = const SceneDrawFilter::Rules::value_type*;

enum WalkFlags : unsigned {
    eWalkHide = 1,
    eWalkShow = 2,
    // Sets each prim's output to what the rules give, without notices.
    eWalkSeed = 4
};

RulePtr
governing(const SceneDrawFilter::Rules& rules,
          const SdfPath& path)
{
    if (rules.empty()) {
        return nullptr;
    }
    const SceneDrawFilter::Rules::const_iterator rule = SdfPathFindLongestPrefix(rules, path);

    return (rule != rules.end()) ? &*rule : nullptr;
}

bool
hasRuleBelow(const SceneDrawFilter::Rules& rules,
             const SdfPath& path)
{
    const SceneDrawFilter::Rules::const_iterator next = rules.upper_bound(path);

    return (next != rules.end()) && next->first.HasPrefix(path);
}

// A child's governing rule is its own or its parent's, so a walk needs one lookup a prim, and none
// where nothing below has a rule.
void
lookUpChild(const SceneDrawFilter::Rules& rules,
            const SdfPath& path,
            RulePtr* rule,
            bool* below)
{
    SceneDrawFilter::Rules::const_iterator next = rules.lower_bound(path);
    if ((next != rules.end()) && (next->first == path)) {
        *rule = &*next;
        ++next;
    }
    *below = (next != rules.end()) && next->first.HasPrefix(path);
}

bool
hiddenBy(RulePtr rule,
         const SdfPath& path)
{
    if (!rule) {
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

// Whatever this hides leaves Hydra.
bool
underHiddenRow(RulePtr rule)
{
    return rule && (rule->second == SceneDrawFilter::eRuleHidden);
}

// A light is hidden by a Hidden row alone: boxed geometry is no reason to darken the scene.
bool
hiddenFor(RulePtr rule,
          const SdfPath& path,
          bool light)
{
    return light ? underHiddenRow(rule) : hiddenBy(rule, path);
}

// Whether geometry below a prim whose subtree has no rule of its own is hidden.
bool
geometryHiddenBelow(RulePtr rule)
{
    return rule && (rule->second != SceneDrawFilter::eRuleShow);
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

const HdDataSourceLocatorSet&
maskLocators()
{
    static const HdDataSourceLocatorSet locators { HdInstancerTopologySchema::GetDefaultLocator().Append(HdInstancerTopologySchemaTokens->mask) };

    return locators;
}

// Under a Children row too: an instance draws only what lies below it.
bool
instanceHiddenBy(RulePtr rule)
{
    return geometryHiddenBelow(rule);
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
    const auto root = _nodes.try_emplace(SdfPath::AbsoluteRootPath());
    _root = &root.first->second;
    _root->path = &root.first->first;
    if (!input) {
        return;
    }
    // The filter hears only of what the input adds from now on.
    SdfPathVector stack = input->GetChildPrimPaths(SdfPath::AbsoluteRootPath());
    while (!stack.empty()) {
        const SdfPath path = stack.back();
        stack.pop_back();
        const HdSceneIndexPrim prim = input->GetPrim(path);
        indexPrim(path, prim.primType);
        readInstancer(path, prim);
        const SdfPathVector children = input->GetChildPrimPaths(path);
        stack.insert(stack.end(), children.rbegin(), children.rend());
    }
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

SceneDrawFilter::Node&
SceneDrawFilter::nodeAt(const SdfPath& path)
{
    const auto found = _nodes.try_emplace(path);
    Node& node = found.first->second;
    if (!found.second) {
        return node;
    }
    node.path = &found.first->first;
    Node& parent = nodeAt(path.GetParentPath());
    node.parent = &parent;
    node.prev = parent.lastChild;
    if (parent.lastChild) {
        parent.lastChild->next = &node;
    } else {
        parent.firstChild = &node;
    }
    parent.lastChild = &node;

    return node;
}

SceneDrawFilter::Node*
SceneDrawFilter::indexPrim(const SdfPath& path,
                           const TfToken& primType)
{
    if (!path.IsAbsolutePath()) {
        return nullptr;
    }
    Node& node = nodeAt(path);
    node.type = primType;
    node.hideable = isHideable(primType);
    const bool light = HdPrimTypeIsLight(primType);
    if (light != node.light) {
        node.light = light;
        if (light) {
            _lights.insert(&node);
        } else {
            _lights.erase(&node);
        }
    }

    return &node;
}

void
SceneDrawFilter::forget(Node* top)
{
    for (Frame& frame : _revealStack) {
        if (frame.next == top) {
            frame.next = top->next;
        }
    }
    const auto inside = [top](const Frame& frame) {
        for (const Node* node = frame.node; node; node = node->parent) {
            if (node == top) {
                return true;
            }
        }

        return false;
    };
    _revealStack.erase(std::remove_if(_revealStack.begin(), _revealStack.end(), inside), _revealStack.end());

    Node* parent = top->parent;
    (top->prev ? top->prev->next : parent->firstChild) = top->next;
    (top->next ? top->next->prev : parent->lastChild) = top->prev;

    std::vector<Node*> doomed { top };
    for (std::size_t i = 0; i < doomed.size(); ++i) {
        for (Node* child = doomed[i]->firstChild; child; child = child->next) {
            doomed.push_back(child);
        }
    }
    for (Node* node : doomed) {
        if (node->type == HdPrimTypeTokens->instancer) {
            forgetInstancer(*node->path);
        }
        if (node->light) {
            _lights.erase(node);
        }
        if (node->output == eOutputInvisible) {
            --_invisibleCount;
        }
        _nodes.erase(_nodes.find(*node->path));
    }
}

void
SceneDrawFilter::setOutput(Node& node,
                           OutputEnum output)
{
    if (node.output == eOutputInvisible) {
        --_invisibleCount;
    }
    if (output == eOutputInvisible) {
        ++_invisibleCount;
    }
    node.output = output;
}

// Instancers are few, so reading one through the input is cheap enough; its instances may be many.
void
SceneDrawFilter::readInstancer(const SdfPath& path,
                               const HdSceneIndexPrim& prim)
{
    forgetInstancer(path);
    if (prim.primType != HdPrimTypeTokens->instancer) {
        return;
    }
    const HdPathArrayDataSourceHandle source = HdInstancerTopologySchema::GetFromParent(prim.dataSource).GetInstanceLocations();
    if (!source) {
        return;
    }
    const VtArray<SdfPath> locations = source->GetTypedValue(0.f);
    if (locations.empty()) {
        return;
    }
    Instancer& record = _instancers[path];
    record.locations.assign(locations.cbegin(), locations.cend());
    record.hidden.assign(record.locations.size(), false);
    // The notice this came with carries the mask, so none is queued.
    record.pending = true;
    for (std::size_t i = 0; i < record.locations.size(); ++i) {
        const SdfPath& location = record.locations[i];
        _locations.emplace(location, std::make_pair(path, i));
        if (!_rules.empty()) {
            setInstanceHidden(path, record, i, instanceHiddenBy(governing(_rules, location)));
        }
    }
    record.pending = false;
}

void
SceneDrawFilter::forgetInstancer(const SdfPath& path)
{
    const auto found = _instancers.find(path);
    if (found == _instancers.end()) {
        return;
    }
    for (const SdfPath& location : found->second.locations) {
        const auto range = _locations.equal_range(location);
        for (auto it = range.first; it != range.second;) {
            it = (it->second.first == path) ? _locations.erase(it) : std::next(it);
        }
    }
    _maskedCount -= found->second.hiddenCount;
    _instancers.erase(found);
}

bool
SceneDrawFilter::setInstanceHidden(const SdfPath& instancer,
                                   Instancer& record,
                                   std::size_t index,
                                   bool hidden)
{
    if (record.hidden[index] == hidden) {
        return false;
    }
    record.hidden[index] = hidden;
    if (hidden) {
        ++record.hiddenCount;
        ++_maskedCount;
    } else {
        --record.hiddenCount;
        --_maskedCount;
    }
    if (!record.pending) {
        record.pending = true;
        _maskChanges.push_back(instancer);
    }

    return true;
}

// The instances at or below the changed rows, as no other instance's governing rule has changed.
void
SceneDrawFilter::updateInstances(const std::vector<SdfPath>& roots)
{
    for (const SdfPath& root : roots) {
        for (auto it = _locations.lower_bound(root); (it != _locations.end()) && it->first.HasPrefix(root); ++it) {
            const auto record = _instancers.find(it->second.first);
            if (record != _instancers.end()) {
                setInstanceHidden(it->second.first, record->second, it->second.second, instanceHiddenBy(governing(_rules, it->first)));
            }
        }
    }
}

bool
SceneDrawFilter::hasDrawnLights(const LightPredicate& counts) const
{
    for (const Node* light : _lights) {
        if ((light->output == eOutputShown) && (!counts || counts(*light->path))) {
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
    if (std::any_of(_rules.begin(), _rules.end(), hidden)) {
        return true;
    }
    const auto held = [](const Node* light) {
        return light->output != eOutputShown;
    };

    return std::any_of(_lights.begin(), _lights.end(), held);
}

HdSceneIndexPrim
SceneDrawFilter::GetPrim(const SdfPath& primPath) const
{
    HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(primPath);
    if ((_rules.empty() && !_revealing) || !isHideable(prim.primType)) {
        return prim;
    }
    const auto node = _nodes.find(primPath);
    OutputEnum output = eOutputShown;
    if (node != _nodes.end()) {
        output = node->second.output;
    } else if (hiddenFor(governing(_rules, primPath), primPath, HdPrimTypeIsLight(prim.primType))) {
        output = eOutputAbsent;
    }
    switch (output) {
    case eOutputShown:
        return withMask(primPath, prim);
    case eOutputInvisible:
        prim.dataSource = HdOverlayContainerDataSource::OverlayedContainerDataSources(invisibleData(), prim.dataSource);

        return prim;
    case eOutputAbsent:
        break;
    }

    return HdSceneIndexPrim();
}

// Over the mask the input authors, such as a point instancer's invisibleIds.
HdSceneIndexPrim
SceneDrawFilter::withMask(const SdfPath& primPath,
                          HdSceneIndexPrim prim) const
{
    if (prim.primType != HdPrimTypeTokens->instancer) {
        return prim;
    }
    const auto found = _instancers.find(primPath);
    if ((found == _instancers.end()) || (found->second.hiddenCount == 0)) {
        return prim;
    }
    const std::vector<bool>& hidden = found->second.hidden;
    VtArray<bool> mask;
    if (const HdBoolArrayDataSourceHandle authored = HdInstancerTopologySchema::GetFromParent(prim.dataSource).GetMask()) {
        mask = authored->GetTypedValue(0.f);
    }
    if (mask.size() < hidden.size()) {
        mask.resize(hidden.size(), true);
    }
    for (std::size_t i = 0; i < hidden.size(); ++i) {
        if (hidden[i]) {
            mask[i] = false;
        }
    }
    const HdContainerDataSourceHandle overlay = HdRetainedContainerDataSource::New(
        HdInstancerTopologySchema::GetSchemaToken(),
        HdInstancerTopologySchema::Builder().SetMask(HdRetainedTypedSampledDataSource<VtArray<bool>>::New(mask)).Build());
    prim.dataSource = HdOverlayContainerDataSource::OverlayedContainerDataSources(overlay, prim.dataSource);

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
    _revealStack.clear();
    finishRevealIfDone();
    std::vector<Frame> stack { rootFrame(_root, true) };
    walk(&stack, eWalkSeed, (std::numeric_limits<std::size_t>::max)());
    updateInstances({ SdfPath::AbsoluteRootPath() });
    for (const SdfPath& path : _maskChanges) {
        _instancers[path].pending = false;
    }
    _maskChanges.clear();
}

SceneDrawFilter::Frame
SceneDrawFilter::rootFrame(Node* node,
                           bool structural) const
{
    const SdfPath& path = *node->path;
    Frame frame { node, nullptr, governing(_rules, path), nullptr, hasRuleBelow(_rules, path), false, false, structural };
    if (!structural) {
        frame.was = governing(_revealFrom, path);
        frame.belowWas = hasRuleBelow(_revealFrom, path);
    }

    return frame;
}

SceneDrawFilter::Frame
SceneDrawFilter::childFrame(const Frame& parent,
                            Node* child) const
{
    Frame frame { child, nullptr, parent.now, parent.was, false, false, false, parent.structural };
    if (parent.belowNow) {
        lookUpChild(_rules, *child->path, &frame.now, &frame.belowNow);
    }
    if (parent.belowWas) {
        lookUpChild(_revealFrom, *child->path, &frame.was, &frame.belowWas);
    }

    return frame;
}

// False lets a walk stop here: a row that goes on hiding a million prims must not visit them.
bool
SceneDrawFilter::descends(const Frame& frame,
                          unsigned mode) const
{
    if (!frame.node->firstChild) {
        return false;
    }
    if (mode & eWalkSeed) {
        return true;
    }
    if (frame.belowNow) {
        return true;
    }
    const bool geometryHidden = geometryHiddenBelow(frame.now);
    const bool lightsHidden = underHiddenRow(frame.now);
    if (!frame.structural && !frame.belowWas && (geometryHidden == geometryHiddenBelow(frame.was)) && (lightsHidden == underHiddenRow(frame.was))) {
        return false;
    }

    return ((mode & eWalkHide) && geometryHidden) || ((mode & eWalkShow) && !(geometryHidden && lightsHidden));
}

std::size_t
SceneDrawFilter::visit(const Frame& frame,
                       unsigned mode)
{
    Node& node = *frame.node;
    if (!node.hideable) {
        return 0;
    }
    const bool hidden = hiddenFor(frame.now, *node.path, node.light);
    if (mode & eWalkSeed) {
        setOutput(node, hidden ? eOutputAbsent : eOutputShown);

        return 0;
    }
    if (hidden) {
        if (mode & eWalkHide) {
            const OutputEnum after = ((node.output == eOutputAbsent) || underHiddenRow(frame.now)) ? eOutputAbsent : eOutputInvisible;
            if (after != node.output) {
                transition(node, after);
            }
        }

        return 0;
    }
    if ((mode & eWalkShow) && (node.output != eOutputShown)) {
        transition(node, eOutputShown);

        return 1;
    }

    return 0;
}

// Children are read only as each parent comes up, so a small batch does not pay for walking the
// whole subtree first.
std::size_t
SceneDrawFilter::walk(std::vector<Frame>* stack,
                      unsigned mode,
                      std::size_t maxShown)
{
    std::size_t shown = 0;
    while (!stack->empty() && (shown < maxShown)) {
        Frame& frame = stack->back();
        if (!frame.entered) {
            frame.entered = true;
            shown += visit(frame, mode);
            if (!descends(frame, mode)) {
                stack->pop_back();
                continue;
            }
            frame.next = frame.node->firstChild;
        }
        Node* child = frame.next;
        if (!child) {
            stack->pop_back();
            continue;
        }
        frame.next = child->next;
        const Frame below = childFrame(frame, child);
        stack->push_back(below);
    }

    return shown;
}

void
SceneDrawFilter::transition(Node& node,
                            OutputEnum after)
{
    if (!node.pending) {
        node.pending = true;
        node.before = node.output;
        _changes.push_back(&node);
    }
    setOutput(node, after);
}

void
SceneDrawFilter::sendChanges()
{
    std::size_t addedCount = 0;
    for (Node* node : _changes) {
        node->pending = false;
        if ((node->output == eOutputInvisible) && (node->before == eOutputAbsent)) {
            // Shown and boxed again before Hydra heard of it: Hydra has still never held it.
            setOutput(*node, eOutputAbsent);
        }
        if ((node->output != node->before) && ((node->output == eOutputAbsent) || (node->before == eOutputAbsent))) {
            ++addedCount;
        }
    }
    HdSceneIndexObserver::AddedPrimEntries added;
    added.reserve(addedCount);
    for (const Node* node : _changes) {
        if (node->output == node->before) {
            continue;
        }
        if (node->output == eOutputAbsent) {
            added.emplace_back(*node->path, TfToken());
            ++_hiddenSent;
        } else if (node->before == eOutputAbsent) {
            added.emplace_back(*node->path, node->type);
            ++_shownSent;
        }
    }
    if (!added.empty()) {
        _SendPrimsAdded(added);
    }

    // A dirtied entry carries its locator set inline, hundreds of bytes: a million-prim change built
    // as one notice spends most of its time on that memory, where a batch at a time stays in cache.
    const std::size_t dirtiedBatch = 4096;
    HdSceneIndexObserver::DirtiedPrimEntries dirtied;
    for (const Node* node : _changes) {
        if ((node->output == node->before) || (node->output == eOutputAbsent) || (node->before == eOutputAbsent)) {
            continue;
        }
        dirtied.emplace_back(*node->path, visibilityLocators());
        ++((node->output == eOutputShown) ? _shownSent : _hiddenSent);
        if (dirtied.size() == dirtiedBatch) {
            _visibilitySent += dirtied.size();
            _SendPrimsDirtied(dirtied);
            dirtied.clear();
        }
    }
    if (!dirtied.empty()) {
        _visibilitySent += dirtied.size();
        _SendPrimsDirtied(dirtied);
        dirtied.clear();
    }
    _changes.clear();

    for (const SdfPath& path : _maskChanges) {
        const auto record = _instancers.find(path);
        if (record != _instancers.end()) {
            record->second.pending = false;
        }
        const auto node = _nodes.find(path);
        if ((node == _nodes.end()) || (node->second.output != eOutputAbsent)) {
            dirtied.emplace_back(path, maskLocators());
        }
    }
    _maskChanges.clear();
    if (!dirtied.empty()) {
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
    std::set<SdfPath> roots;
    for (const SceneDrawDelta* delta : deltas) {
        applyToRules(*delta, &target);
        addPaths(delta->drawn, &roots);
        addPaths(delta->expanded, &roots);
        addPaths(delta->boxed, &roots);
        addPaths(delta->hidden, &roots);
        addPaths(delta->cleared, &roots);
    }
    _revealFrom = std::move(_rules);
    _rules = std::move(target);

    // Outside the changed rows the new rules agree with the old, so what a reveal under way has yet to
    // announce is still to be shown, and nothing else there changes.
    for (Frame& frame : _revealStack) {
        const SdfPath& path = *frame.node->path;
        frame.now = governing(_rules, path);
        frame.belowNow = hasRuleBelow(_rules, path);
        frame.was = nullptr;
        frame.belowWas = false;
        frame.structural = true;
    }

    std::vector<Frame> changed;
    const std::vector<SdfPath> tops = topmost(roots);
    for (std::vector<SdfPath>::const_reverse_iterator it = tops.rbegin(); it != tops.rend(); ++it) {
        const auto node = _nodes.find(*it);
        if (node != _nodes.end()) {
            changed.push_back(rootFrame(&node->second, false));
        }
    }

    const std::size_t all = (std::numeric_limits<std::size_t>::max)();
    if (revealBatch == 0) {
        walk(&changed, eWalkHide | eWalkShow, all);
        walk(&_revealStack, eWalkShow, all);
    } else {
        std::vector<Frame> hiding = changed;
        walk(&hiding, eWalkHide, all);
        _revealStack.insert(_revealStack.end(), changed.begin(), changed.end());
    }
    // Masks change at once, reveal or not: an instance costs Hydra nothing to show.
    updateInstances(tops);
    _revealing = true;
    finishRevealIfDone();
    sendChanges();
}

std::size_t
SceneDrawFilter::stepReveal(std::size_t maxPrims)
{
    const std::size_t shown = walk(&_revealStack, eWalkShow, maxPrims);
    finishRevealIfDone();
    sendChanges();

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
}

void
SceneDrawFilter::_PrimsAdded(const HdSceneIndexBase& /*sender*/,
                             const HdSceneIndexObserver::AddedPrimEntries& entries)
{
    _inputAdded += entries.size();
    _lastInputAdded = Clock::now();
    HdSceneIndexObserver::AddedPrimEntries rewritten;
    for (std::size_t i = 0; i < std::size_t(entries.size()); ++i) {
        const HdSceneIndexObserver::AddedPrimEntry& entry = entries[i];
        Node* node = indexPrim(entry.primPath, entry.primType);
        if (!node) {
            continue;
        }
        if (entry.primType == HdPrimTypeTokens->instancer) {
            readInstancer(entry.primPath, _GetInputSceneIndex()->GetPrim(entry.primPath));
        } else if (!_instancers.empty()) {
            forgetInstancer(entry.primPath);
        }
        OutputEnum output = eOutputShown;
        if (node->hideable && !_rules.empty()) {
            const RulePtr rule = governing(_rules, entry.primPath);
            if (hiddenFor(rule, entry.primPath, node->light)) {
                // A prim Hydra holds invisible stays held: passed on, its GetPrim() keeps it invisible.
                output = ((node->output == eOutputInvisible) && !underHiddenRow(rule)) ? eOutputInvisible : eOutputAbsent;
            }
        }
        setOutput(*node, output);
        if (output == eOutputAbsent) {
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
        const auto found = _nodes.find(entry.primPath);
        if (found == _nodes.end()) {
            continue;
        }
        Node* node = &found->second;
        if (node != _root) {
            forget(node);
            continue;
        }
        while (_root->firstChild) {
            forget(_root->firstChild);
        }
    }
    finishRevealIfDone();
    _SendPrimsRemoved(entries);
}

void
SceneDrawFilter::_PrimsDirtied(const HdSceneIndexBase& /*sender*/,
                               const HdSceneIndexObserver::DirtiedPrimEntries& entries)
{
    _inputDirtied += entries.size();
    for (const HdSceneIndexObserver::DirtiedPrimEntry& entry : entries) {
        if (entry.dirtyLocators.Intersects(HdInstancerTopologySchema::GetDefaultLocator())) {
            const auto node = _nodes.find(entry.primPath);
            if ((node != _nodes.end()) && (node->second.type == HdPrimTypeTokens->instancer)) {
                readInstancer(entry.primPath, _GetInputSceneIndex()->GetPrim(entry.primPath));
            }
        }
    }
    if (_rules.empty() && !_revealing) {
        _SendPrimsDirtied(entries);

        return;
    }
    HdSceneIndexObserver::DirtiedPrimEntries kept;
    for (const HdSceneIndexObserver::DirtiedPrimEntry& entry : entries) {
        const auto node = _nodes.find(entry.primPath);
        // Storm defers an invisible prim's sync, keeping the dirty bits for when it shows again, so
        // only a prim out of Hydra can do without them.
        if ((node == _nodes.end()) || !node->second.hideable || (node->second.output != eOutputAbsent)) {
            kept.push_back(entry);
        }
    }
    if (!kept.empty()) {
        _SendPrimsDirtied(kept);
    }
}

NATRON_NAMESPACE_EXIT
