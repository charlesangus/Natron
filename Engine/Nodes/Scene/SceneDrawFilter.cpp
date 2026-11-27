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

#include "Engine/Nodes/Scene/SceneDrawFilter.h"

#include "Engine/Nodes/Scene/StormAddedPrimDedupe.h"

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
#include <pxr/imaging/hd/geomSubsetSchema.h>
#include <pxr/imaging/hd/instancerTopologySchema.h>
#include <pxr/imaging/hd/meshSchema.h>
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
// UsdImagingGLEngine's constructor building its render index synchronously, on its own thread.
// hd/utils.h only says the callback "currently" runs during render index construction.
thread_local SceneDrawFilter::EngineScope* activeScope = nullptr;

HdSceneIndexBaseRefPtr
appendFilter(const std::string& renderInstanceId,
             const HdSceneIndexBaseRefPtr& input,
             const HdContainerDataSourceHandle& /*inputArgs*/)
{
    if (!activeScope) {
        return input;
    }
    TfRefPtr<SceneDrawFilter> filter = SceneDrawFilter::New(input);
    {
        std::lock_guard<std::mutex> lock(filtersMutex);
        filters[renderInstanceId] = SceneDrawFilter::WeakPtr(filter);
    }
    activeScope->setInstanceId(renderInstanceId);

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
    eWalkSeed = 4,
    // Brings into Hydra, invisible, the geometry the rules show and Hydra lacks.
    eWalkStage = 8
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
topologyLocators()
{
    static const HdDataSourceLocatorSet locators { HdMeshSchema::GetTopologyLocator() };

    return locators;
}

const HdDataSourceLocatorSet&
subsetLocators()
{
    static const HdDataSourceLocatorSet locators { HdGeomSubsetSchema::GetDefaultLocator() };

    return locators;
}

// Invisible, so the scene index adapter lists its faces among the mesh topology's invisible ones,
// as the legacy delegate does for its own such child.
HdSceneIndexPrim
facesSubset(const VtIntArray& faces)
{
    HdSceneIndexPrim prim;
    prim.primType = HdPrimTypeTokens->geomSubset;
    prim.dataSource = HdOverlayContainerDataSource::OverlayedContainerDataSources(
        invisibleData(),
        HdRetainedContainerDataSource::New(
            HdGeomSubsetSchema::GetSchemaToken(),
            HdGeomSubsetSchema::Builder()
                .SetType(HdGeomSubsetSchema::BuildTypeDataSource(HdGeomSubsetSchemaTokens->typeFaceSet))
                .SetIndices(HdRetainedTypedSampledDataSource<VtIntArray>::New(faces))
                .Build()));

    return prim;
}

std::size_t
countSet(const std::vector<bool>& bits)
{
    return std::size_t(std::count(bits.begin(), bits.end(), true));
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

// How little a rule draws, for the rows of a shared prototype's instances that disagree.
int
ruleStrength(SceneDrawFilter::RuleEnum rule)
{
    switch (rule) {
    case SceneDrawFilter::eRuleShow:
        return 0;
    case SceneDrawFilter::eRuleExpand:
        return 1;
    case SceneDrawFilter::eRuleHide:
        return 2;
    case SceneDrawFilter::eRuleHidden:
        return 3;
    }

    return 0;
}

const SceneDrawFilter::RuleEnum kRulesByStrength[] = {
    SceneDrawFilter::eRuleShow, SceneDrawFilter::eRuleExpand, SceneDrawFilter::eRuleHide, SceneDrawFilter::eRuleHidden
};

// Paths whose rule differs between a and b, either way.
void
addDifferences(const SceneDrawFilter::Rules& a,
               const SceneDrawFilter::Rules& b,
               std::set<SdfPath>* roots)
{
    auto i = a.begin();
    auto j = b.begin();
    while ((i != a.end()) || (j != b.end())) {
        if ((j == b.end()) || ((i != a.end()) && (i->first < j->first))) {
            roots->insert(i->first);
            ++i;
        } else if ((i == a.end()) || (j->first < i->first)) {
            roots->insert(j->first);
            ++j;
        } else {
            if (i->second != j->second) {
                roots->insert(i->first);
            }
            ++i;
            ++j;
        }
    }
}

// Bounds the passes over nested native instances, so a cyclic input cannot loop.
const int kMaxInstanceNesting = 16;
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
    // Admitting brings a material's prims in after the material, in batches, which is what makes
    // Storm add it once per prim bound to it.
    StormAddedPrimDedupe::registerForStorm();
}

SceneDrawFilter::EngineScope::EngineScope()
    : _previous(activeScope)
{
    activeScope = this;
}

SceneDrawFilter::EngineScope::~EngineScope()
{
    activeScope = _previous;
}

std::string
SceneDrawFilter::EngineScope::getInstanceId() const
{
    return _instanceId;
}

void
SceneDrawFilter::EngineScope::setInstanceId(const std::string& renderInstanceId)
{
    _instanceId = renderInstanceId;
}

std::vector<std::string>
SceneDrawFilter::getInstanceIds()
{
    std::lock_guard<std::mutex> lock(filtersMutex);
    std::vector<std::string> ids;
    ids.reserve(filters.size());
    for (const auto& entry : filters) {
        ids.push_back(entry.first);
    }

    return ids;
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

bool
SceneDrawFilter::isUnderHiddenRow(const Rules& rules,
                                  const SdfPath& path)
{
    return underHiddenRow(governing(rules, path));
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
    const SdfPathVector tops = input->GetChildPrimPaths(SdfPath::AbsoluteRootPath());
    SdfPathVector stack(tops.rbegin(), tops.rend());
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
    // Up to the nearest ancestor already known, without recursing as deep as the scene is.
    for (Node* child = &node;;) {
        const auto above = _nodes.try_emplace(child->path->GetParentPath());
        Node& parent = above.first->second;
        if (above.second) {
            parent.path = &above.first->first;
        }
        child->parent = &parent;
        child->prev = parent.lastChild;
        if (parent.lastChild) {
            parent.lastChild->next = child;
        } else {
            parent.firstChild = child;
        }
        parent.lastChild = child;
        if (!above.second) {
            break;
        }
        child = &parent;
    }

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
            _lightPaths.insert(path);
        } else {
            _lights.erase(&node);
            _lightPaths.erase(path);
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
    if (top->queuedBelow != 0) {
        for (Node* above = parent; above; above = above->parent) {
            above->queuedBelow -= top->queuedBelow;
        }
    }
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
            _lightPaths.erase(*node->path);
        }
        if ((!_arrivalRoots.empty() || _admitting) && node->hideable && (node->output == eOutputShown)) {
            _removedHeld.insert(*node->path);
        }
        if (node->output == eOutputInvisible) {
            --_invisibleCount;
        }
        // What comes back in its place waits for the next budget, which sees it as it then is.
        if (hasBudgetLayer()) {
            _budgetHidden.erase(*node->path);
            _budgetFaces.erase(*node->path);
            const auto mask = _budgetMasks.find(*node->path);
            if (mask != _budgetMasks.end()) {
                _budgetMaskedCount -= countSet(mask->second);
                _budgetMasks.erase(mask);
            }
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
    // An instance read again keeps whether it was admitted; one the input newly names waits.
    std::unordered_map<SdfPath, bool, SdfPath::Hash> waited;
    if (_admitting) {
        const auto old = _instancers.find(path);
        if (old != _instancers.end()) {
            for (std::size_t i = 0; i < old->second.locations.size(); ++i) {
                waited.emplace(old->second.locations[i], old->second.waiting[i]);
            }
        }
    }
    // The budget's mask is by instance index, which a changed instance list makes name other
    // instances, as a load does for native instances. They draw until the next budget.
    std::vector<SdfPath> budgetMasked;
    const auto masked = _budgetMasks.find(path);
    if (masked != _budgetMasks.end()) {
        const auto old = _instancers.find(path);
        if (old != _instancers.end()) {
            budgetMasked = old->second.locations;
        }
    }
    const auto dropStaleMask = [&]() {
        if (masked == _budgetMasks.end()) {
            return;
        }
        const auto now = _instancers.find(path);
        if ((now == _instancers.end()) || (now->second.locations != budgetMasked)) {
            _budgetMaskedCount -= countSet(masked->second);
            _budgetMasks.erase(masked);
        }
    };
    forgetInstancer(path);
    if (prim.primType != HdPrimTypeTokens->instancer) {
        dropStaleMask();

        return;
    }
    const HdInstancerTopologySchema topology = HdInstancerTopologySchema::GetFromParent(prim.dataSource);
    Instancer& record = _instancers[path];
    if (const HdPathArrayDataSourceHandle prototypes = topology.GetPrototypes()) {
        const VtArray<SdfPath> paths = prototypes->GetTypedValue(0.f);
        for (const SdfPath& prototype : paths) {
            if (prototype.IsAbsolutePath() && _prototypeOwners.emplace(prototype, path).second) {
                record.prototypes.push_back(prototype);
            }
        }
    }
    const HdPathArrayDataSourceHandle source = topology.GetInstanceLocations();
    if (!source) {
        dropStaleMask();

        return;
    }
    const VtArray<SdfPath> locations = source->GetTypedValue(0.f);
    if (locations.empty()) {
        dropStaleMask();

        return;
    }
    record.locations.assign(locations.cbegin(), locations.cend());
    _instancersChanged = true;
    record.hidden.assign(record.locations.size(), false);
    record.waiting.assign(record.locations.size(), false);
    // The notice this came with carries the mask, so none is queued.
    record.pending = true;
    for (std::size_t i = 0; i < record.locations.size(); ++i) {
        const SdfPath& location = record.locations[i];
        _locations.emplace(location, std::make_pair(path, i));
        _scannedTo.clear();
        if (!_rules.empty()) {
            setInstanceHidden(path, record, i, instanceOffBy(_rules, record, i));
        }
        if (_admitting && !record.hidden[i]) {
            const auto before = waited.find(location);
            setInstanceWaiting(path, record, i, (before == waited.end()) || before->second);
        }
    }
    record.pending = false;
    dropStaleMask();
}

void
SceneDrawFilter::forgetInstancer(const SdfPath& path)
{
    const auto found = _instancers.find(path);
    if (found == _instancers.end()) {
        return;
    }
    if (!found->second.locations.empty()) {
        _instancersChanged = true;
    }
    for (const SdfPath& location : found->second.locations) {
        const auto range = _locations.equal_range(location);
        for (auto it = range.first; it != range.second;) {
            it = (it->second.first == path) ? _locations.erase(it) : std::next(it);
        }
    }
    _scannedTo.clear();
    for (const SdfPath& prototype : found->second.prototypes) {
        _prototypeOwners.erase(prototype);
    }
    _maskedCount -= found->second.hiddenCount;
    _waitingCount -= found->second.waitingCount;
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
        // The rules have it, so it waits for nothing; showing it again costs Hydra nothing.
        setInstanceWaiting(instancer, record, index, false);
    } else {
        --record.hiddenCount;
        --_maskedCount;
    }
    queueMaskChange(instancer, record);

    return true;
}

void
SceneDrawFilter::setInstanceWaiting(const SdfPath& instancer,
                                    Instancer& record,
                                    std::size_t index,
                                    bool waiting)
{
    if (record.waiting[index] == waiting) {
        return;
    }
    record.waiting[index] = waiting;
    if (waiting) {
        if (!isFilling()) {
            _admitted = 0;
        }
        ++record.waitingCount;
        ++_waitingCount;
        record.waitingFrom = (std::min)(record.waitingFrom, index);
        _scannedTo.clear();
    } else {
        --record.waitingCount;
        --_waitingCount;
    }
    queueMaskChange(instancer, record);
}

void
SceneDrawFilter::queueMaskChange(const SdfPath& instancer,
                                 Instancer& record)
{
    if (!record.pending) {
        record.pending = true;
        _maskChanges.push_back(instancer);
    }
}

// The instances at or below the changed rows, as no other instance's governing rule has changed.
void
SceneDrawFilter::updateInstances(const std::vector<SdfPath>& roots)
{
    for (const SdfPath& root : roots) {
        for (auto it = _locations.lower_bound(root); (it != _locations.end()) && it->first.HasPrefix(root); ++it) {
            const auto record = _instancers.find(it->second.first);
            if (record != _instancers.end()) {
                setInstanceHidden(it->second.first, record->second, it->second.second, instanceOffBy(_rules, record->second, it->second.second));
            }
        }
    }
}

bool
SceneDrawFilter::instanceOffBy(const Rules& rules,
                               const Instancer& record,
                               std::size_t index) const
{
    const SdfPath& location = record.locations[index];
    const RulePtr rule = governing(rules, location);
    if (rule && (rule->second == eRuleExpand) && (rule->first == location) && hasOnePrototype(record)) {
        return false;
    }

    return instanceHiddenBy(rule);
}

SceneDrawFilter::Rules
SceneDrawFilter::effectiveRules(const Rules& stageRules) const
{
    if (stageRules.empty() || _locations.empty()) {
        return stageRules;
    }
    Rules effective = stageRules;
    std::vector<SdfPath> pending;
    pending.reserve(stageRules.size());
    for (const Rules::value_type& rule : stageRules) {
        pending.push_back(rule.first);
    }
    const SdfPath& self = SdfPath::ReflexiveRelativePath();
    // Each pass moves the rows one level of nested instances further in.
    for (int depth = 0; !pending.empty() && (depth < kMaxInstanceNesting); ++depth) {
        struct Inside {
            std::set<SdfPath> relative;
            std::set<std::size_t> instances;
        };
        std::map<SdfPath, Inside> byInstancer;
        for (const SdfPath& path : pending) {
            const std::pair<SdfPath, std::size_t>* instance = nativeInstanceOf(path);
            if (!instance) {
                continue;
            }
            const auto record = _instancers.find(instance->first);
            if ((record == _instancers.end()) || !hasOnePrototype(record->second)) {
                continue;
            }
            const SdfPath& location = record->second.locations[instance->second];
            // Any other row on the instance itself turns it off in the mask.
            if (path == location) {
                const Rules::const_iterator own = effective.find(path);
                if ((own == effective.end()) || (own->second != eRuleExpand)) {
                    continue;
                }
            }
            Inside& inside = byInstancer[instance->first];
            inside.relative.insert(path.MakeRelativePath(location));
            inside.instances.insert(instance->second);
        }
        pending.clear();
        for (const auto& entry : byInstancer) {
            const Instancer& record = _instancers.find(entry.first)->second;
            const SdfPath& prototype = record.prototypes.front();
            for (const SdfPath& relative : entry.second.relative) {
                int strongest = -1;
                for (const std::size_t index : entry.second.instances) {
                    if (instanceOffBy(effective, record, index)) {
                        continue;
                    }
                    const SdfPath& location = record.locations[index];
                    const SdfPath at = (relative == self) ? location : location.AppendPath(relative);
                    const RulePtr rule = governing(effective, at);
                    // A row above the instance reaches it through the mask alone.
                    if (!rule || !rule->first.HasPrefix(location)) {
                        continue;
                    }
                    RuleEnum value = rule->second;
                    if ((value == eRuleExpand) && (rule->first != at)) {
                        value = eRuleHide;
                    }
                    strongest = (std::max)(strongest, ruleStrength(value));
                }
                if (strongest < 0) {
                    continue;
                }
                const SdfPath target = (relative == self) ? prototype : prototype.AppendPath(relative);
                effective[target] = kRulesByStrength[strongest];
                pending.push_back(target);
            }
        }
    }

    return effective;
} // SceneDrawFilter::effectiveRules

SdfPath
SceneDrawFilter::prototypePathOf(const SdfPath& path,
                                 std::pair<SdfPath, std::size_t>* shared) const
{
    *shared = std::pair<SdfPath, std::size_t>();
    SdfPath at = path;
    for (int depth = 0; depth < kMaxInstanceNesting; ++depth) {
        const std::pair<SdfPath, std::size_t>* instance = nativeInstanceOf(at);
        if (!instance) {
            break;
        }
        const auto record = _instancers.find(instance->first);
        if (record == _instancers.end()) {
            break;
        }
        const SdfPath& location = record->second.locations[instance->second];
        if (location == at) {
            break;
        }
        if (!hasOnePrototype(record->second) || (record->second.locations.size() > 1)) {
            *shared = *instance;

            return SdfPath();
        }
        at = at.ReplacePrefix(location, record->second.prototypes.front());
    }

    return at;
}

void
SceneDrawFilter::refreshRules()
{
    _instancersChanged = false;
    if (_stageRules.empty()) {
        return;
    }
    Rules effective = effectiveRules(_stageRules);
    std::set<SdfPath> roots;
    addDifferences(_rules, effective, &roots);
    if (!roots.empty()) {
        applyRules(std::move(effective), roots, std::vector<const SceneDrawDelta*>(), 0, true);
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

bool
SceneDrawFilter::hidesEveryLight(const LightPredicate& counts) const
{
    if (!hidesLights()) {
        return false;
    }
    bool any = false;
    for (const Node* light : _lights) {
        if (counts && !counts(*light->path)) {
            continue;
        }
        if (light->output == eOutputShown) {
            return false;
        }
        any = true;
    }

    return any;
}

HdSceneIndexPrim
SceneDrawFilter::GetPrim(const SdfPath& primPath) const
{
    if (!_budgetFaces.empty() && (primPath.GetNameToken() == budgetFacesChildName())) {
        const auto found = _budgetFaces.find(primPath.GetParentPath());
        if (found != _budgetFaces.end()) {
            return facesSubset(found->second);
        }
    }
    HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(primPath);
    if (!isHideable(prim.primType)) {
        return prim;
    }
    if (!isFiltering()) {
        return hasBudgetLayer() ? withBudget(primPath, withMask(primPath, prim)) : prim;
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
        return hasBudgetLayer() ? withBudget(primPath, withMask(primPath, prim)) : withMask(primPath, prim);
    case eOutputInvisible:
        prim.dataSource = HdOverlayContainerDataSource::OverlayedContainerDataSources(invisibleData(), prim.dataSource);

        return prim;
    case eOutputAbsent:
        break;
    }

    return HdSceneIndexPrim();
}

// Over the mask the input authors, such as a point instancer's invisibleIds: what the rules turn off
// and what the budget does both go.
HdSceneIndexPrim
SceneDrawFilter::withMask(const SdfPath& primPath,
                          HdSceneIndexPrim prim) const
{
    if (prim.primType != HdPrimTypeTokens->instancer) {
        return prim;
    }
    const auto found = _instancers.find(primPath);
    const std::vector<bool>* hidden = ((found != _instancers.end()) && (found->second.hiddenCount != 0)) ? &found->second.hidden : nullptr;
    const std::vector<bool>* waiting = ((found != _instancers.end()) && (found->second.waitingCount != 0)) ? &found->second.waiting : nullptr;
    const std::vector<bool>* budget = nullptr;
    if (!_budgetMasks.empty()) {
        const auto masked = _budgetMasks.find(primPath);
        if (masked != _budgetMasks.end()) {
            budget = &masked->second;
        }
    }
    if (!hidden && !waiting && !budget) {
        return prim;
    }
    VtArray<bool> mask;
    if (const HdBoolArrayDataSourceHandle authored = HdInstancerTopologySchema::GetFromParent(prim.dataSource).GetMask()) {
        mask = authored->GetTypedValue(0.f);
    }
    const std::size_t size = (std::max)({ hidden ? hidden->size() : 0, waiting ? waiting->size() : 0, budget ? budget->size() : 0 });
    if (mask.size() < size) {
        mask.resize(size, true);
    }
    for (const std::vector<bool>* off : { hidden, waiting, budget }) {
        if (!off) {
            continue;
        }
        for (std::size_t i = 0; i < off->size(); ++i) {
            if ((*off)[i]) {
                mask[i] = false;
            }
        }
    }
    const HdContainerDataSourceHandle overlay = HdRetainedContainerDataSource::New(
        HdInstancerTopologySchema::GetSchemaToken(),
        HdInstancerTopologySchema::Builder().SetMask(HdRetainedTypedSampledDataSource<VtArray<bool>>::New(mask)).Build());
    prim.dataSource = HdOverlayContainerDataSource::OverlayedContainerDataSources(overlay, prim.dataSource);

    return prim;
}

HdSceneIndexPrim
SceneDrawFilter::withBudget(const SdfPath& primPath,
                            HdSceneIndexPrim prim) const
{
    if (!_budgetHidden.empty() && HdPrimTypeIsGprim(prim.primType) && (_budgetHidden.count(primPath) != 0)) {
        prim.dataSource = HdOverlayContainerDataSource::OverlayedContainerDataSources(invisibleData(), prim.dataSource);
    }

    return prim;
}

SdfPathVector
SceneDrawFilter::GetChildPrimPaths(const SdfPath& primPath) const
{
    SdfPathVector children = _GetInputSceneIndex()->GetChildPrimPaths(primPath);
    if (!_budgetFaces.empty() && (_budgetFaces.count(primPath) != 0)) {
        children.push_back(primPath.AppendChild(budgetFacesChildName()));
    }

    return children;
}

void
SceneDrawFilter::seedRules(const Rules& rules)
{
    _stageRules = rules;
    _rules = effectiveRules(rules);
    _instancersChanged = false;
    _revealStack.clear();
    _arrivalRoots.clear();
    finishRevealIfDone();
    std::vector<Frame> stack { rootFrame(_root, true) };
    walk(&stack, eWalkSeed, (std::numeric_limits<std::size_t>::max)(), (Clock::time_point::max)());
    updateInstances({ SdfPath::AbsoluteRootPath() });
    if (_admitting) {
        for (auto& instancer : _instancers) {
            Instancer& record = instancer.second;
            for (std::size_t i = 0; i < record.locations.size(); ++i) {
                setInstanceWaiting(instancer.first, record, i, !record.hidden[i]);
            }
        }
    }
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

    if ((mode & eWalkHide) && geometryHidden) {
        return true;
    }
    if (!(mode & (eWalkShow | eWalkStage)) || lightsHidden) {
        return false;
    }

    // Under a boxed row only lights can show, so a reveal that row cut short ends within the step.
    return !geometryHidden || hasLightBelow(*frame.node->path);
}

bool
SceneDrawFilter::hasLightBelow(const SdfPath& path) const
{
    const std::set<SdfPath>::const_iterator next = _lightPaths.upper_bound(path);

    return (next != _lightPaths.end()) && next->HasPrefix(path);
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
        const bool waits = !hidden && queues(node);
        setOutput(node, (hidden || waits) ? eOutputAbsent : eOutputShown);
        if (waits) {
            enqueue(node);
        } else {
            dequeue(node);
        }

        return 0;
    }
    if (hidden) {
        dequeue(node);
        if (mode & eWalkHide) {
            // What Hydra holds until the call's notices go: a prim it has never held stays out of it.
            const OutputEnum held = node.pending ? node.before : node.output;
            const OutputEnum after = ((held == eOutputAbsent) || underHiddenRow(frame.now)) ? eOutputAbsent : eOutputInvisible;
            if (after != node.output) {
                transition(node, after);
            }
        }

        return 0;
    }
    if (mode & eWalkStage) {
        if ((node.output == eOutputAbsent) && !node.light) {
            transition(node, eOutputInvisible);
        }

        return 0;
    }
    if ((mode & eWalkShow) && (node.output != eOutputShown)) {
        const OutputEnum held = node.pending ? node.before : node.output;
        if ((held == eOutputAbsent) && queues(node)) {
            if (node.output != eOutputAbsent) {
                transition(node, eOutputAbsent);
            }
            enqueue(node);

            return 0;
        }
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
                      std::size_t maxShown,
                      Clock::time_point deadline)
{
    // Reading the clock costs about what a visit does.
    const unsigned clockEvery = 512;
    const bool timed = (deadline != (Clock::time_point::max)());
    unsigned untilClock = clockEvery;
    std::size_t shown = 0;
    _stepCut = false;
    while (!stack->empty() && (shown < maxShown)) {
        if (timed && (--untilClock == 0)) {
            untilClock = clockEvery;
            if (Clock::now() >= deadline) {
                _stepCut = true;
                break;
            }
        }
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
            ++_insertedSent;
            if (node->output == eOutputShown) {
                ++_shownSent;
            }
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
SceneDrawFilter::replaceRules(const Rules& rules)
{
    if (rules == _stageRules) {
        return;
    }
    SceneDrawDelta delta;
    for (const Rules::value_type& rule : _stageRules) {
        if (rules.find(rule.first) == rules.end()) {
            delta.cleared.push_back(rule.first.GetString());
        }
    }
    for (const Rules::value_type& rule : rules) {
        const Rules::const_iterator current = _stageRules.find(rule.first);
        if ((current != _stageRules.end()) && (current->second == rule.second)) {
            continue;
        }
        std::vector<std::string>* list = nullptr;
        switch (rule.second) {
        case eRuleShow:
            list = &delta.drawn;
            break;
        case eRuleExpand:
            list = &delta.expanded;
            break;
        case eRuleHide:
            list = &delta.boxed;
            break;
        case eRuleHidden:
            list = &delta.hidden;
            break;
        }
        list->push_back(rule.first.GetString());
    }
    applyDelta(delta);
}

void
SceneDrawFilter::applyDeltas(const std::vector<const SceneDrawDelta*>& deltas,
                             std::size_t revealBatch)
{
    Rules target = _stageRules;
    std::set<SdfPath> roots;
    for (const SceneDrawDelta* delta : deltas) {
        applyToRules(*delta, &target);
        addPaths(delta->drawn, &roots);
        addPaths(delta->expanded, &roots);
        addPaths(delta->boxed, &roots);
        addPaths(delta->hidden, &roots);
        addPaths(delta->cleared, &roots);
    }
    Rules effective = effectiveRules(target);
    addDifferences(_rules, effective, &roots);
    _stageRules = std::move(target);
    _instancersChanged = false;
    applyRules(std::move(effective), roots, deltas, revealBatch, false);
}

void
SceneDrawFilter::applyRules(Rules effective,
                            const std::set<SdfPath>& roots,
                            const std::vector<const SceneDrawDelta*>& deltas,
                            std::size_t revealBatch,
                            bool fromInput)
{
    if (_admitting) {
        // What Hydra lacks waits to be admitted instead of being revealed.
        revealBatch = 0;
        if (!fromInput) {
            _orderNext = 0;
        }
    } else if (fromInput && _revealing) {
        // A reveal under way keeps its pace over what the instancers moved.
        revealBatch = 1;
    }
    _revealFrom = std::move(_rules);
    _rules = std::move(effective);
    const std::vector<SdfPath> tops = topmost(roots);

    // A frontier at or below a changed row goes, the change's own frame there taking its place.
    // That frame and one below a frontier still under way cannot tell what Hydra has yet to be
    // shown there from what the rules changed, so they look at every prim the rules show.
    std::vector<bool> overlapping(tops.size(), false);
    const auto underChanged = [&tops, &overlapping](const Frame& frame) {
        const SdfPath& path = *frame.node->path;
        std::vector<SdfPath>::const_iterator above = std::upper_bound(tops.begin(), tops.end(), path);
        if ((above != tops.begin()) && path.HasPrefix(*std::prev(above))) {
            overlapping[std::size_t(std::prev(above) - tops.begin())] = true;

            return true;
        }
        for (; (above != tops.end()) && above->HasPrefix(path); ++above) {
            overlapping[std::size_t(above - tops.begin())] = true;
        }

        return false;
    };
    _revealStack.erase(std::remove_if(_revealStack.begin(), _revealStack.end(), underChanged), _revealStack.end());

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

    std::vector<Frame> hiding;
    std::vector<Frame> changed;
    for (std::size_t i = tops.size(); i-- > 0;) {
        const auto node = _nodes.find(tops[i]);
        if (node != _nodes.end()) {
            hiding.push_back(rootFrame(&node->second, false));
            changed.push_back(rootFrame(&node->second, overlapping[i]));
        }
    }

    const std::size_t all = (std::numeric_limits<std::size_t>::max)();
    const Clock::time_point never = (Clock::time_point::max)();
    walk(&hiding, eWalkHide, all, never);
    if (revealBatch == 0) {
        walk(&changed, eWalkShow, all, never);
        walk(&_revealStack, eWalkShow, all, never);
    } else {
        std::vector<Frame> staging = changed;
        walk(&staging, eWalkStage, all, never);
        _revealStack.insert(_revealStack.end(), changed.begin(), changed.end());
    }
    if (!fromInput) {
        updateRevealRoots(deltas, tops, revealBatch);
    }
    // Masks change at once, reveal or not: an instance costs Hydra nothing to show.
    updateInstances(tops);
    _revealing = true;
    finishRevealIfDone();
    sendChanges();
}

void
SceneDrawFilter::updateRevealRoots(const std::vector<const SceneDrawDelta*>& deltas,
                                   const std::vector<SdfPath>& changed,
                                   std::size_t revealBatch)
{
    if (revealBatch == 0) {
        _arrivalRoots.clear();
        _removedHeld.clear();

        return;
    }
    for (const SdfPath& top : changed) {
        std::set<SdfPath>::iterator it = _arrivalRoots.lower_bound(top);
        while ((it != _arrivalRoots.end()) && it->HasPrefix(top)) {
            it = _arrivalRoots.erase(it);
        }
    }
    for (const SceneDrawDelta* delta : deltas) {
        for (const SceneDrawLoadRequest& load : delta->loads) {
            const SdfPath path(load.path);
            if (path.IsAbsolutePath() && path.IsPrimPath()) {
                _arrivalRoots.insert(path);
            }
        }
    }
}

// Lights come at once, as a reveal showing geometry in the dark would be no picture of the scene.
bool
SceneDrawFilter::defersArrival(const Node& node,
                               bool known,
                               OutputEnum before) const
{
    if (!node.hideable || node.light) {
        return false;
    }
    if (known) {
        return before != eOutputShown;
    }

    return _removedHeld.empty() || (_removedHeld.count(*node.path) == 0);
}

std::size_t
SceneDrawFilter::stepReveal(std::size_t maxPrims,
                            Clock::time_point deadline)
{
    const std::size_t shown = walk(&_revealStack, eWalkShow, maxPrims, deadline);
    const bool cut = _stepCut;
    finishRevealIfDone();
    sendChanges();
    _stepCut = cut;

    return shown;
}

void
SceneDrawFilter::endRender()
{
    _removedHeld.clear();
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
    // Instancers first, so that the prims of a prototype in the same notice meet the rows that
    // govern them through its instances.
    for (const HdSceneIndexObserver::AddedPrimEntry& entry : entries) {
        if (!entry.primPath.IsAbsolutePath()) {
            continue;
        }
        if (entry.primType == HdPrimTypeTokens->instancer) {
            readInstancer(entry.primPath, _GetInputSceneIndex()->GetPrim(entry.primPath));
        } else if (!_instancers.empty()) {
            forgetInstancer(entry.primPath);
        }
    }
    if (_instancersChanged) {
        refreshRules();
    }
    HdSceneIndexObserver::AddedPrimEntries rewritten;
    std::vector<Node*> arrived;
    bool deferred = false;
    for (std::size_t i = 0; i < std::size_t(entries.size()); ++i) {
        const HdSceneIndexObserver::AddedPrimEntry& entry = entries[i];
        const bool arriving = !_arrivalRoots.empty() && entry.primPath.IsAbsolutePath()
            && (SdfPathFindLongestPrefix(_arrivalRoots, entry.primPath) != _arrivalRoots.end());
        bool known = false;
        OutputEnum before = eOutputShown;
        if (arriving || _admitting) {
            const auto found = _nodes.find(entry.primPath);
            // A node made only as the parent of one the input announced first was never in Hydra.
            known = (found != _nodes.end()) && (arriving || !found->second.type.IsEmpty());
            before = known ? found->second.output : eOutputShown;
        }
        Node* node = indexPrim(entry.primPath, entry.primType);
        if (!node) {
            continue;
        }
        if (arriving && !node->arriving) {
            node->arriving = true;
            arrived.push_back(node);
        }
        OutputEnum output = eOutputShown;
        if (node->hideable && !_rules.empty()) {
            const RulePtr rule = governing(_rules, entry.primPath);
            if (hiddenFor(rule, entry.primPath, node->light)) {
                // A prim Hydra holds invisible stays held: passed on, its GetPrim() keeps it invisible.
                output = ((node->output == eOutputInvisible) && !underHiddenRow(rule)) ? eOutputInvisible : eOutputAbsent;
            }
        }
        if ((output == eOutputShown) && arriving && defersArrival(*node, known, before)) {
            output = eOutputInvisible;
            deferred = true;
        }
        if (queues(*node)) {
            const bool held = known ? (before != eOutputAbsent) : (!_removedHeld.empty() && (_removedHeld.count(entry.primPath) != 0));
            const bool waits = (output == eOutputShown) && !held;
            if (waits) {
                output = eOutputAbsent;
                enqueue(*node);
            } else {
                dequeue(*node);
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
    // A frame for each arrival whose parent did not arrive with it, which is the payload's own prim
    // where a load resyncs its subtree.
    for (std::size_t i = arrived.size(); i-- > 0;) {
        Node* node = arrived[i];
        if (deferred && !(node->parent && node->parent->arriving)) {
            _revealStack.push_back(rootFrame(node, true));
        }
    }
    for (Node* node : arrived) {
        node->arriving = false;
    }
    if (deferred) {
        _revealing = true;
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
    if (_instancersChanged) {
        refreshRules();
    }
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
    if (!isFiltering()) {
        _SendPrimsDirtied(entries);
        if (_instancersChanged) {
            refreshRules();
        }

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
    if (_instancersChanged) {
        refreshRules();
    }
}

const TfToken&
SceneDrawFilter::budgetFacesChildName()
{
    static const TfToken token("__natronBudgetFaces");

    return token;
}

SceneDrawFilter::Budget
SceneDrawFilter::budgetFrom(const SceneViewBudget::Result& result,
                            const std::vector<SceneViewBudget::Unit>& units)
{
    Budget budget;

    if (!result.valid) {
        return budget;
    }
    budget.hidden = result.demoted;
    budget.hidden.insert(result.culled.begin(), result.culled.end());
    const auto bitAt = [](const std::vector<bool>& bits, std::size_t index, bool fallback) {
        return (index < bits.size()) ? bits[index] : fallback;
    };
    for (const auto& entry : result.instancers) {
        const SceneViewBudget::InstancerResult& record = entry.second;
        const std::size_t size = (std::max)({ record.demoted.size(), record.culled.size(), record.kept.size() });
        std::vector<bool> off(size, false);
        bool any = false;
        for (std::size_t index = 0; index < size; ++index) {
            off[index] = bitAt(record.demoted, index, false) || bitAt(record.culled, index, false) || !bitAt(record.kept, index, true);
            any = any || off[index];
        }
        if (any) {
            budget.instances.emplace(entry.first, std::move(off));
        }
    }
    if (result.keptFaceFraction.empty()) {
        return budget;
    }
    for (const SceneViewBudget::Unit& unit : units) {
        if (!unit.mesh || unit.instances) {
            continue;
        }
        const auto fraction = result.keptFaceFraction.find(unit.path);
        if (fraction == result.keptFaceFraction.end()) {
            continue;
        }
        const U64 seed = SceneViewBudget::pathSeed(unit.path);
        VtIntArray omitted;
        for (U64 face = 0; face < unit.faces; ++face) {
            if (!SceneViewBudget::keeps(seed, std::size_t(face), fraction->second)) {
                omitted.push_back(int(face));
            }
        }
        if (!omitted.empty()) {
            budget.invisibleFaces.emplace(unit.path, std::move(omitted));
        }
    }

    return budget;
} // SceneDrawFilter::budgetFrom

void
SceneDrawFilter::setBudgetBound(bool bound)
{
    if (!bound && _budgetBound) {
        clearBudget();
    }
    _budgetBound = bound;
}

const std::pair<SdfPath, std::size_t>*
SceneDrawFilter::nativeInstanceOf(const SdfPath& path) const
{
    if (_locations.empty()) {
        return nullptr;
    }
    for (SdfPath at = path; !at.IsEmpty() && !at.IsAbsoluteRootPath(); at = at.GetParentPath()) {
        const auto found = _locations.find(at);
        if (found != _locations.end()) {
            return &found->second;
        }
    }

    return nullptr;
}

void
SceneDrawFilter::applyBudget(const Budget& budget)
{
    if (!_budgetBound || (budget.empty() && !hasBudgetLayer())) {
        return;
    }
    const auto drawn = [this](const SdfPath& path) {
        const auto node = _nodes.find(path);

        return (node != _nodes.end()) && (node->second.output == eOutputShown);
    };

    // A prim inside native instances is not in Hydra under its own path: its prototype's prim draws
    // it. Where no other instance draws that prototype, the prim itself goes; otherwise hiding it
    // would take it from every instance, so the outermost instance whose prototype is shared goes.
    std::unordered_set<SdfPath, SdfPath::Hash> hidden;
    std::unordered_map<SdfPath, std::vector<bool>, SdfPath::Hash> masks;
    hidden.reserve(budget.hidden.size());
    for (const SdfPath& path : budget.hidden) {
        std::pair<SdfPath, std::size_t> instance;
        const SdfPath drawnAt = prototypePathOf(path, &instance);
        const auto node = drawnAt.IsEmpty() ? _nodes.end() : _nodes.find(drawnAt);
        if (node != _nodes.end()) {
            if (HdPrimTypeIsGprim(node->second.type)) {
                hidden.insert(drawnAt);
            }
            continue;
        }
        if (instance.first.IsEmpty()) {
            continue;
        }
        const auto record = _instancers.find(instance.first);
        if (record == _instancers.end()) {
            continue;
        }
        std::vector<bool>& bits = masks[instance.first];
        bits.resize(record->second.locations.size(), false);
        bits[instance.second] = true;
    }
    for (const auto& entry : budget.instances) {
        std::pair<SdfPath, std::size_t> instance;
        const SdfPath drawnAt = prototypePathOf(entry.first, &instance);
        const auto node = drawnAt.IsEmpty() ? _nodes.end() : _nodes.find(drawnAt);
        if ((node == _nodes.end()) || (node->second.type != HdPrimTypeTokens->instancer)) {
            continue;
        }
        std::vector<bool>& bits = masks[drawnAt];
        if (bits.empty()) {
            // A word at a time: an instancer may hold tens of millions.
            bits = entry.second;
            continue;
        }
        if (bits.size() < entry.second.size()) {
            bits.resize(entry.second.size(), false);
        }
        for (std::size_t index = 0; index < entry.second.size(); ++index) {
            if (entry.second[index]) {
                bits[index] = true;
            }
        }
    }
    std::unordered_map<SdfPath, VtIntArray, SdfPath::Hash> faces;
    for (const auto& entry : budget.invisibleFaces) {
        std::pair<SdfPath, std::size_t> instance;
        const SdfPath drawnAt = entry.second.empty() ? SdfPath() : prototypePathOf(entry.first, &instance);
        const auto node = drawnAt.IsEmpty() ? _nodes.end() : _nodes.find(drawnAt);
        if ((node != _nodes.end()) && (node->second.type == HdPrimTypeTokens->mesh)) {
            faces.emplace(drawnAt, entry.second);
        }
    }

    HdSceneIndexObserver::DirtiedPrimEntries dirtied;
    HdSceneIndexObserver::AddedPrimEntries added;
    HdSceneIndexObserver::RemovedPrimEntries removed;
    for (const SdfPath& path : _budgetHidden) {
        if ((hidden.count(path) == 0) && drawn(path)) {
            dirtied.emplace_back(path, visibilityLocators());
        }
    }
    for (const SdfPath& path : hidden) {
        if ((_budgetHidden.count(path) == 0) && drawn(path)) {
            dirtied.emplace_back(path, visibilityLocators());
        }
    }
    std::vector<SdfPath> maskChanged;
    for (const auto& entry : masks) {
        const auto old = _budgetMasks.find(entry.first);
        if ((old == _budgetMasks.end()) || (old->second != entry.second)) {
            maskChanged.push_back(entry.first);
        }
    }
    for (const auto& entry : _budgetMasks) {
        if (masks.count(entry.first) == 0) {
            maskChanged.push_back(entry.first);
        }
    }
    for (const SdfPath& path : maskChanged) {
        const auto node = _nodes.find(path);
        if ((node != _nodes.end()) && (node->second.output != eOutputAbsent)) {
            dirtied.emplace_back(path, maskLocators());
        }
    }
    for (const auto& entry : _budgetFaces) {
        if (faces.count(entry.first) == 0) {
            removed.emplace_back(entry.first.AppendChild(budgetFacesChildName()));
            dirtied.emplace_back(entry.first, topologyLocators());
        }
    }
    for (const auto& entry : faces) {
        const auto old = _budgetFaces.find(entry.first);
        if (old == _budgetFaces.end()) {
            added.emplace_back(entry.first.AppendChild(budgetFacesChildName()), HdPrimTypeTokens->geomSubset);
            dirtied.emplace_back(entry.first, topologyLocators());
        } else if (old->second != entry.second) {
            dirtied.emplace_back(entry.first.AppendChild(budgetFacesChildName()), subsetLocators());
            dirtied.emplace_back(entry.first, topologyLocators());
        }
    }

    _budgetHidden.swap(hidden);
    _budgetMasks.swap(masks);
    _budgetFaces.swap(faces);
    _budgetMaskedCount = 0;
    for (const auto& entry : _budgetMasks) {
        _budgetMaskedCount += countSet(entry.second);
    }

    if (!removed.empty()) {
        _SendPrimsRemoved(removed);
    }
    if (!added.empty()) {
        _SendPrimsAdded(added);
    }
    const std::size_t dirtiedBatch = 4096;
    for (std::size_t begin = 0; begin < dirtied.size(); begin += dirtiedBatch) {
        const std::size_t end = (std::min)(begin + dirtiedBatch, dirtied.size());
        _SendPrimsDirtied(HdSceneIndexObserver::DirtiedPrimEntries(dirtied.begin() + begin, dirtied.begin() + end));
    }
} // SceneDrawFilter::applyBudget

std::size_t
SceneDrawFilter::getBudgetHiddenCount() const
{
    std::size_t count = 0;

    for (const SdfPath& path : _budgetHidden) {
        const auto node = _nodes.find(path);
        if ((node != _nodes.end()) && (node->second.output == eOutputShown)) {
            ++count;
        }
    }

    return count;
}

VtIntArray
SceneDrawFilter::getBudgetInvisibleFaces(const SdfPath& mesh) const
{
    const auto found = _budgetFaces.find(mesh);

    return (found != _budgetFaces.end()) ? found->second : VtIntArray();
}

bool
SceneDrawFilter::isInstanceMasked(const SdfPath& instancer,
                                  std::size_t index) const
{
    const HdSceneIndexPrim prim = GetPrim(instancer);
    const HdBoolArrayDataSourceHandle mask = HdInstancerTopologySchema::GetFromParent(prim.dataSource).GetMask();
    if (!mask) {
        return false;
    }
    const VtArray<bool> bits = mask->GetTypedValue(0.f);

    return (index < bits.size()) && !bits[index];
}

void
SceneDrawFilter::setAdmitting(bool admitting)
{
    if (admitting == _admitting) {
        return;
    }
    if (!admitting) {
        admit(Order(), (std::numeric_limits<std::size_t>::max)());
    }
    _admitting = admitting;
    _order.reset();
    _orderNext = 0;
}

void
SceneDrawFilter::enqueue(Node& node)
{
    if (node.queued) {
        return;
    }
    if (!isFilling()) {
        _admitted = 0;
    }
    node.queued = true;
    for (Node* at = &node; at; at = at->parent) {
        ++at->queuedBelow;
    }
}

void
SceneDrawFilter::dequeue(Node& node)
{
    if (!node.queued) {
        return;
    }
    node.queued = false;
    for (Node* at = &node; at; at = at->parent) {
        --at->queuedBelow;
    }
}

void
SceneDrawFilter::admitNode(Node& node)
{
    dequeue(node);
    if (node.output == eOutputAbsent) {
        transition(node, eOutputShown);
    }
    ++_admitted;
}

// The outermost instancer drawing node, through the prims below each instancer and the prototypes it
// names: a prototype in Hydra before its instancer would draw once, untransformed.
SceneDrawFilter::Node*
SceneDrawFilter::unitOwner(Node& node)
{
    Node* owner = nullptr;
    // Prototypes naming their own ancestors would otherwise go round for ever.
    std::size_t steps = 0;
    for (Node* at = &node; at && (at != _root) && (steps < 4096); ++steps) {
        if (at->type == HdPrimTypeTokens->instancer) {
            owner = at;
        }
        if (!_prototypeOwners.empty()) {
            const auto named = _prototypeOwners.find(*at->path);
            if (named != _prototypeOwners.end()) {
                const auto instancer = _nodes.find(named->second);
                if ((instancer != _nodes.end()) && (&instancer->second != at)) {
                    at = &instancer->second;
                    continue;
                }
            }
        }
        at = at->parent;
    }

    return owner;
}

std::size_t
SceneDrawFilter::admitUnit(Node& node)
{
    std::size_t admitted = 0;
    if (Node* owner = unitOwner(node)) {
        std::vector<Node*> tops { owner };
        std::unordered_set<const Node*> seen;
        while (!tops.empty()) {
            Node* top = tops.back();
            tops.pop_back();
            if (!seen.insert(top).second) {
                continue;
            }
            std::vector<Node*> stack { top };
            while (!stack.empty()) {
                Node* at = stack.back();
                stack.pop_back();
                if (at->type == HdPrimTypeTokens->instancer) {
                    const auto record = _instancers.find(*at->path);
                    if (record != _instancers.end()) {
                        for (const SdfPath& prototype : record->second.prototypes) {
                            const auto found = _nodes.find(prototype);
                            if ((found != _nodes.end()) && (found->second.queuedBelow != 0)) {
                                tops.push_back(&found->second);
                            }
                        }
                    }
                }
                if (at->queued) {
                    admitNode(*at);
                    ++admitted;
                }
                for (Node* child = at->lastChild; child; child = child->prev) {
                    if (child->queuedBelow != 0) {
                        stack.push_back(child);
                    }
                }
            }
        }
    }
    if (node.queued) {
        admitNode(node);
        ++admitted;
    }

    return admitted;
}

std::size_t
SceneDrawFilter::admitBelow(Node& top,
                            std::size_t budget,
                            Clock::time_point deadline)
{
    if (top.queuedBelow == 0) {
        return 0;
    }
    const unsigned clockEvery = 512;
    const bool timed = (deadline != (Clock::time_point::max)());
    unsigned untilClock = clockEvery;
    std::size_t admitted = 0;
    std::vector<Node*> stack { &top };
    while (!stack.empty() && (admitted < budget)) {
        if (timed && (--untilClock == 0)) {
            untilClock = clockEvery;
            if (Clock::now() >= deadline) {
                break;
            }
        }
        Node* at = stack.back();
        stack.pop_back();
        if (at->queuedBelow == 0) {
            continue;
        }
        if (at->queued) {
            admitted += admitUnit(*at);
        }
        for (Node* child = at->lastChild; child; child = child->prev) {
            if (child->queuedBelow != 0) {
                stack.push_back(child);
            }
        }
    }

    return admitted;
}

std::size_t
SceneDrawFilter::admitInstance(const SdfPath& instancer,
                               Instancer& record,
                               std::size_t index)
{
    std::size_t admitted = 0;
    const auto node = _nodes.find(instancer);
    if ((node != _nodes.end()) && node->second.queued) {
        admitted += admitUnit(node->second);
    }
    setInstanceWaiting(instancer, record, index, false);
    ++_admitted;
    if (++_instanceCredit == kInstancesPerPrim) {
        _instanceCredit = 0;
        ++admitted;
    }

    return admitted;
}

std::size_t
SceneDrawFilter::admitAt(const SdfPath& path,
                         std::size_t budget,
                         Clock::time_point deadline)
{
    std::size_t admitted = 0;
    const auto node = _nodes.find(path);
    if (node != _nodes.end()) {
        admitted += admitBelow(node->second, budget, deadline);
    }
    if ((_waitingCount == 0) || (admitted >= budget)) {
        return admitted;
    }
    const unsigned clockEvery = 512;
    const bool timed = (deadline != (Clock::time_point::max)());
    unsigned untilClock = clockEvery;
    Locations::const_iterator it = scanStart(path);
    for (; (it != _locations.end()) && it->first.HasPrefix(path) && (admitted < budget); ++it) {
        if (timed && (--untilClock == 0)) {
            untilClock = clockEvery;
            if (Clock::now() >= deadline) {
                break;
            }
        }
        const auto record = _instancers.find(it->second.first);
        if ((record != _instancers.end()) && record->second.waiting[it->second.second]) {
            admitted += admitInstance(it->second.first, record->second, it->second.second);
        }
    }
    _scannedTo[path] = it;

    return admitted;
}

std::size_t
SceneDrawFilter::admitAnyInstances(std::size_t budget,
                                   Clock::time_point deadline)
{
    const unsigned clockEvery = 512;
    const bool timed = (deadline != (Clock::time_point::max)());
    unsigned untilClock = clockEvery;
    std::size_t admitted = 0;
    for (auto& instancer : _instancers) {
        Instancer& record = instancer.second;
        if (record.waitingCount == 0) {
            continue;
        }
        std::size_t i = record.waitingFrom;
        for (; (i < record.waiting.size()) && (admitted < budget); ++i) {
            if (timed && (--untilClock == 0)) {
                untilClock = clockEvery;
                if (Clock::now() >= deadline) {
                    break;
                }
            }
            if (record.waiting[i]) {
                admitted += admitInstance(instancer.first, record, i);
            }
        }
        record.waitingFrom = i;
        if (admitted >= budget) {
            break;
        }
    }

    return admitted;
}

std::size_t
SceneDrawFilter::admit(const Order& order,
                       std::size_t maxPrims,
                       Clock::time_point deadline)
{
    if (order != _order) {
        _order = order;
        _orderNext = 0;
    }
    const std::size_t budget = (std::max)(maxPrims, std::size_t(1));
    const bool timed = (deadline != (Clock::time_point::max)());
    const std::size_t clockEvery = 64;
    std::size_t admitted = 0;
    if (_order) {
        const std::vector<SdfPath>& rows = *_order;
        for (std::size_t looked = 0; isFilling() && (_orderNext < rows.size()) && (admitted < budget); ++looked) {
            if (timed && (admitted != 0) && ((looked % clockEvery) == 0) && (Clock::now() >= deadline)) {
                break;
            }
            const SdfPath& row = rows[_orderNext];
            if (row.IsAbsolutePath() && hasPendingAt(row)) {
                admitted += admitAt(row, budget - admitted, deadline);
                if (hasPendingAt(row)) {
                    break;
                }
            }
            ++_orderNext;
        }
    }
    if ((admitted < budget) && isFilling() && ((admitted == 0) || !timed || (Clock::now() < deadline))) {
        admitted += admitBelow(*_root, budget - admitted, deadline);
        if ((admitted < budget) && (_waitingCount != 0)) {
            admitted += admitAnyInstances(budget - admitted, deadline);
        }
    }
    sendChanges();

    return admitted;
}

bool
SceneDrawFilter::hasPendingAt(const SdfPath& path) const
{
    const auto node = _nodes.find(path);
    if ((node != _nodes.end()) && (node->second.queuedBelow != 0)) {
        return true;
    }
    if (_waitingCount == 0) {
        return false;
    }
    Locations::const_iterator it = scanStart(path);
    for (; (it != _locations.end()) && it->first.HasPrefix(path); ++it) {
        const auto record = _instancers.find(it->second.first);
        if ((record != _instancers.end()) && record->second.waiting[it->second.second]) {
            break;
        }
    }
    _scannedTo[path] = it;

    return (it != _locations.end()) && it->first.HasPrefix(path);
}

SceneDrawFilter::Locations::const_iterator
SceneDrawFilter::scanStart(const SdfPath& path) const
{
    const auto scanned = _scannedTo.find(path);

    return (scanned != _scannedTo.end()) ? scanned->second : _locations.lower_bound(path);
}

SceneRevealPacer::SceneRevealPacer(double targetMs,
                                   std::size_t firstBatch,
                                   std::size_t minBatch)
    : _targetMs(targetMs)
    , _batch((std::max)(firstBatch, std::size_t(1)))
    , _minBatch((std::max)(minBatch, std::size_t(1)))
    , _samples()
{
}

void
SceneRevealPacer::observe(std::size_t prims,
                          double ms)
{
    if ((prims == 0) || !(ms >= 0.)) {
        return;
    }
    _samples[_nextSample] = Sample { double(prims), ms };
    _nextSample = (_nextSample + 1) % kSamples;
    _sampleCount = (std::min)(_sampleCount + 1, kSamples);

    double meanPrims = 0.;
    double meanMs = 0.;
    for (std::size_t i = 0; i < _sampleCount; ++i) {
        meanPrims += _samples[i].prims;
        meanMs += _samples[i].ms;
    }
    meanPrims /= double(_sampleCount);
    meanMs /= double(_sampleCount);
    double spread = 0.;
    double covariance = 0.;
    for (std::size_t i = 0; i < _sampleCount; ++i) {
        spread += (_samples[i].prims - meanPrims) * (_samples[i].prims - meanPrims);
        covariance += (_samples[i].prims - meanPrims) * (_samples[i].ms - meanMs);
    }
    const double minPerPrimMs = 1e-9;
    const double current = double(_batch);
    const double latest = double(prims);
    double wanted = current;
    // Batches of one size cannot tell the fixed part from the rest, nor can noise between close ones.
    const double relative = 0.1 * meanPrims;
    if ((_sampleCount >= 3) && (spread > relative * relative * double(_sampleCount))) {
        _perPrimMs = (std::max)(covariance / spread, minPerPrimMs);
        _fitted = true;
    } else if (!_fitted) {
        _perPrimMs = (std::max)(ms / latest, minPerPrimMs);
        wanted = (ms <= _targetMs) ? (current * 2.) : (current / 2.);
    }
    if (_fitted) {
        // From the latest frame alone: during a fill the fixed part grows with the prims Hydra holds,
        // which the older frames the slope comes from did not hold yet.
        _fixedMs = ms - _perPrimMs * latest;
        if (_fixedMs < 0.) {
            _fixedMs = 0.;
            _perPrimMs = (std::max)(ms / latest, minPerPrimMs);
        }
        const double fits = (_targetMs - _fixedMs) / _perPrimMs;
        if (fits >= double(_minBatch)) {
            wanted = fits;
        } else if (_fixedMs >= _targetMs) {
            // Every pass costs the fixed part however few prims it shows, so a batch whose prim work
            // is at most that keeps a pass within twice the unavoidable cost and the fill in a number
            // of passes that does not depend on the prim count.
            wanted = (std::max)(double(_minBatch), _fixedMs / _perPrimMs);
        } else {
            wanted = (std::min)(current, double(_minBatch));
        }
    } else {
        _fixedMs = 0.;
    }
    const double bounded = (std::min)((std::max)(wanted, current / 4.), current * 2.);
    // The upper bound only keeps the conversion defined.
    _batch = std::size_t((std::min)((std::max)(bounded, 1.), 1e15));
}

NATRON_NAMESPACE_EXIT
