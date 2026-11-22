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

#include "USDPrune.h"

#include <algorithm>
#include <list>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "Engine/ChoiceOption.h"
#include "Engine/ContentHasher.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/SceneLoadSet.h"

#include <pxr/usd/usdLux/boundableLightBase.h>
#include <pxr/usd/usdLux/lightFilter.h>
#include <pxr/usd/usdLux/nonboundableLightBase.h>
#include <pxr/usd/usdRender/pass.h>
#include <pxr/usd/usdRender/product.h>
#include <pxr/usd/usdRender/var.h>

NATRON_NAMESPACE_ENTER

namespace {
typedef std::unordered_set<PXR_NS::SdfPath, PXR_NS::SdfPath::Hash> PathSet;

struct IsolateKeeps {
    bool lights = true;
    bool cameras = true;
    bool materials = true;
    bool renderSettings = true;

    bool any() const
    {
        return lights || cameras || materials || renderSettings;
    }
};

// The prims an isolate walks: what is on the stage, unloaded payloads included, since a viewer
// draws those.
const PXR_NS::Usd_PrimFlagsPredicate&
isolatePredicate()
{
    static const PXR_NS::Usd_PrimFlagsPredicate predicate = PXR_NS::UsdPrimIsActive && PXR_NS::UsdPrimIsDefined && !PXR_NS::UsdPrimIsAbstract;

    return predicate;
}

bool
isUnloadedPayload(const PXR_NS::UsdPrim& prim)
{
    return prim.HasAuthoredPayloads() && !prim.IsLoaded();
}

bool
isKeptType(const PXR_NS::UsdPrim& prim,
           const IsolateKeeps& keeps)
{
    if (keeps.lights && (prim.IsA<PXR_NS::UsdLuxBoundableLightBase>() || prim.IsA<PXR_NS::UsdLuxNonboundableLightBase>() || prim.IsA<PXR_NS::UsdLuxLightFilter>())) {
        return true;
    }
    if (keeps.cameras && prim.IsA<PXR_NS::UsdGeomCamera>()) {
        return true;
    }
    if (keeps.materials && prim.IsA<PXR_NS::UsdShadeMaterial>()) {
        return true;
    }

    return keeps.renderSettings && (prim.IsA<PXR_NS::UsdRenderSettings>() || prim.IsA<PXR_NS::UsdRenderProduct>() || prim.IsA<PXR_NS::UsdRenderVar>() || prim.IsA<PXR_NS::UsdRenderPass>());
}

// Whether a pattern reaching below prefixes can select path or a prim below or above it.
bool
reachTouches(const PXR_NS::SdfPath& path,
             const std::vector<PXR_NS::SdfPath>& prefixes)
{
    for (const PXR_NS::SdfPath& prefix : prefixes) {
        if (prefix.HasPrefix(path) || path.HasPrefix(prefix)) {
            return true;
        }
    }

    return false;
}

class AbortPoll {
public:
    explicit AbortPoll(const std::function<bool()>& aborted)
        : _aborted(aborted)
        , _count(0)
    {
    }

    bool operator()()
    {
        return ((++_count & 1023) == 0) && _aborted();
    }

private:
    const std::function<bool()>& _aborted;
    U64 _count;
};

struct IsolateFrame {
    PXR_NS::SdfPath path;
    // In the kept set: left alone with everything below it.
    bool kept = false;
    // A prim below is kept.
    bool holdsKept = false;
    // Its children cannot be authored on: an instance, or an unloaded payload.
    bool leaf = false;
    // Hiding it hides something: it can be drawn, or it is a leaf, which can hold what can.
    bool hideable = false;
    // The prune roots found below it, when it holds a kept prim.
    std::vector<PXR_NS::SdfPath> roots;
    // What pruning each child holding no kept prim authors on.
    std::vector<PXR_NS::SdfPath> candidates;
};

/**
 * The highest prims of stage off every path to matches and to the prims keeps names: what
 * isolating matches prunes, sorted. In a partial evaluation an unloaded payload stays when the
 * pattern can reach it (reach). An instance is kept whole when it holds a kept prim, since USD
 * cannot author inside it. Hide replaces a root that cannot be drawn by the highest prims below
 * it that can, or that hold what an instance or an unloaded payload draws. False when aborted
 * turned true.
 **/
bool
findIsolateRoots(const PXR_NS::UsdStageRefPtr& stage,
                 const std::vector<PXR_NS::SdfPath>& matches,
                 const std::vector<PXR_NS::SdfPath>& reach,
                 bool partial,
                 const IsolateKeeps& keeps,
                 bool hide,
                 const std::function<bool()>& aborted,
                 std::vector<PXR_NS::SdfPath>* roots)
{
    AbortPoll poll(aborted);
    const PathSet matched(matches.begin(), matches.end());
    PathSet aboveMatches;
    for (const PXR_NS::SdfPath& path : matches) {
        for (PXR_NS::SdfPath parent = path.GetParentPath(); !parent.IsEmpty() && !parent.IsAbsoluteRootPath(); parent = parent.GetParentPath()) {
            if (!aboveMatches.insert(parent).second) {
                break;
            }
        }
    }
    // Only kept types and reachable unloaded payloads are found by looking below a prim.
    const bool searchBelow = partial || keeps.any();

    std::vector<IsolateFrame> stack(1);
    stack.front().holdsKept = true;
    PXR_NS::UsdPrimRange range = PXR_NS::UsdPrimRange::PreAndPostVisit(stage->GetPseudoRoot(), PXR_NS::UsdTraverseInstanceProxies(isolatePredicate()));
    for (PXR_NS::UsdPrimRange::iterator it = range.begin(); it != range.end(); ++it) {
        if (poll()) {
            return false;
        }
        const PXR_NS::UsdPrim& prim = *it;
        if (prim.IsPseudoRoot()) {
            continue;
        }
        if (!it.IsPostVisit()) {
            IsolateFrame frame;
            frame.path = prim.GetPath();
            const bool unloaded = partial && isUnloadedPayload(prim);
            frame.leaf = prim.IsInstance() || unloaded;
            frame.hideable = frame.leaf || prim.IsA<PXR_NS::UsdGeomImageable>();
            if (matched.count(frame.path) || (frame.leaf && aboveMatches.count(frame.path))) {
                frame.kept = true;
            } else if (unloaded) {
                frame.kept = reachTouches(frame.path, reach);
            } else if (isKeptType(prim, keeps)) {
                frame.kept = true;
            }
            const bool descend = !frame.kept && (searchBelow || aboveMatches.count(frame.path) || (hide && !frame.hideable));
            if (!descend) {
                it.PruneChildren();
            }
            stack.push_back(frame);
            continue;
        }

        if (stack.size() < 2) {
            continue;
        }
        IsolateFrame frame = std::move(stack.back());
        stack.pop_back();
        IsolateFrame& parent = stack.back();
        if (frame.leaf && frame.holdsKept) {
            frame.kept = true;
        }
        if (frame.kept) {
            parent.holdsKept = true;
        } else if (frame.holdsKept) {
            parent.holdsKept = true;
            parent.roots.insert(parent.roots.end(), frame.roots.begin(), frame.roots.end());
            parent.roots.insert(parent.roots.end(), frame.candidates.begin(), frame.candidates.end());
        } else if (!hide || frame.hideable) {
            parent.candidates.push_back(frame.path);
        } else {
            // Hiding a prim that cannot be drawn hides nothing below it.
            parent.candidates.insert(parent.candidates.end(), frame.candidates.begin(), frame.candidates.end());
        }
    }
    IsolateFrame& top = stack.front();
    top.roots.insert(top.roots.end(), top.candidates.begin(), top.candidates.end());
    std::sort(top.roots.begin(), top.roots.end());
    *roots = std::move(top.roots);

    return true;
} // findIsolateRoots
// Evaluation contexts isolating's prune roots are kept for at once.
const std::size_t kIsolateCacheEntries = 4;
} // anonymous namespace

struct USDPrune::IsolateCache {
    struct Entry {
        U64 context = 0;
        ScenePayload upstream;
        SceneLoadSetPtr loadSet;
        std::string pattern;
        // The Mode and the Keep checkboxes.
        U64 settings = 0;
        std::vector<PXR_NS::SdfPath> targets;
    };

    std::mutex mutex;
    // Most recently used first.
    std::list<Entry> entries;

    // What isolating prunes for these, when an evaluation remembered it.
    bool find(const ScenePayload& upstream,
              const SceneEvaluationContext& context,
              const std::string& pattern,
              U64 settings,
              std::vector<PXR_NS::SdfPath>* targets)
    {
        std::lock_guard<std::mutex> lock(mutex);

        for (std::list<Entry>::iterator it = entries.begin(); it != entries.end(); ++it) {
            if ((it->pattern != pattern) || (it->settings != settings) || !it->upstream.isSameStack(upstream)
                || (it->upstream.getStackHash() != upstream.getStackHash()) || !SceneEvaluationContext(0, it->loadSet).hasSameLoadSet(context)) {
                continue;
            }
            *targets = it->targets;
            Entry entry = *it;
            entry.context = context.id;
            entry.upstream = upstream;
            entry.loadSet = context.loadSet;
            entries.erase(it);
            removeContext(context.id);
            entries.push_front(entry);

            return true;
        }

        return false;
    }

    void store(const ScenePayload& upstream,
               const SceneEvaluationContext& context,
               const std::string& pattern,
               U64 settings,
               const std::vector<PXR_NS::SdfPath>& targets)
    {
        std::lock_guard<std::mutex> lock(mutex);

        removeContext(context.id);
        Entry entry;
        entry.context = context.id;
        entry.upstream = upstream;
        entry.loadSet = context.loadSet;
        entry.pattern = pattern;
        entry.settings = settings;
        entry.targets = targets;
        entries.push_front(entry);
        if (entries.size() > kIsolateCacheEntries) {
            entries.pop_back();
        }
    }

private:
    void removeContext(U64 id)
    {
        for (std::list<Entry>::iterator it = entries.begin(); it != entries.end(); ++it) {
            if (it->context == id) {
                entries.erase(it);

                return;
            }
        }
    }
};

USDPrune::USDPrune(NodePtr node)
    : NativeEffectBase(node)
    , _isolateCache(new IsolateCache)
{
}

USDPrune::~USDPrune()
{
}

U64
USDPrune::getPatternQueryCount() const
{
    return _matches.getQueryCount();
}

const NodeSpec*
USDPrune::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
USDPrune::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    _primPattern.create(this, page, kUSDPruneParamPrimPattern, tr("Prim Pattern"),
                        tr("The prims to remove, or with Prune Unselected on, the prims to keep."));

    KnobChoicePtr mode = createKnob<KnobChoice>(tr("Mode"));
    mode->setName(kUSDPruneParamMode);
    mode->setHintToolTip(tr("How the pruned prims are removed: the matches, or with Prune Unselected on, the highest prims "
                            "off every kept path."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("deactivate", tr("Deactivate").toStdString(),
                                       tr("Makes each match inactive: it and its descendants are no longer on the stage, for "
                                          "drawing, rendering or later patterns.")
                                           .toStdString()));
        options.push_back(ChoiceOption("hide", tr("Hide").toStdString(),
                                       tr("Makes each match that can be drawn invisible, and so its descendants: they stay on "
                                          "the stage, and later nodes can still match and show them.")
                                           .toStdString()));
        mode->populateChoices(options);
    }
    mode->setDefaultValue((int)eModeDeactivate);
    mode->setAnimationEnabled(false);
    page->addKnob(mode);
    _mode = mode;

    const auto addBool = [this, &page](const QString& label,
                                       const char* name,
                                       bool defaultValue,
                                       const QString& toolTip) {
        KnobBoolPtr knob = createKnob<KnobBool>(label);
        knob->setName(name);
        knob->setHintToolTip(toolTip);
        knob->setDefaultValue(defaultValue);
        knob->setAnimationEnabled(false);
        page->addKnob(knob);

        return knob;
    };
    _pruneUnselected = addBool(tr("Prune Unselected"), kUSDPruneParamPruneUnselected, false,
                               tr("Keeps what Prim Pattern selects, with its ancestors and descendants, and prunes the rest "
                                  "of the scene by Mode, authoring only on the highest prims off every kept path; a pattern "
                                  "that matches nothing prunes all but what the Keep checkboxes keep. In a viewer that leaves "
                                  "payloads unloaded, only loaded prims match, and an unloaded branch is kept only when a "
                                  "loaded prim matched and the pattern could match inside the branch; renders load "
                                  "everything and are exact."));
    _keepLights = addBool(tr("Keep Lights"), kUSDPruneParamKeepLights, true,
                          tr("With Prune Unselected on, also keeps every light and light filter, and their ancestors."));
    _keepCameras = addBool(tr("Keep Cameras"), kUSDPruneParamKeepCameras, true,
                           tr("With Prune Unselected on, also keeps every camera, and its ancestors."));
    _keepMaterials = addBool(tr("Keep Materials"), kUSDPruneParamKeepMaterials, true,
                             tr("With Prune Unselected on, also keeps every material with the shaders and node graphs "
                                "inside it, and its ancestors, so kept prims still resolve their bindings."));
    _keepRenderSettings = addBool(tr("Keep Render Settings"), kUSDPruneParamKeepRenderSettings, true,
                                  tr("With Prune Unselected on, also keeps every render settings, render product, render "
                                     "var and render pass prim, and its ancestors."));
    refreshKeepKnobs();
} // USDPrune::initializeKnobs

void
USDPrune::refreshKeepKnobs()
{
    KnobBoolPtr pruneUnselected = _pruneUnselected.lock();
    const bool enabled = pruneUnselected && pruneUnselected->getValue();
    const KnobBoolPtr keeps[] = { _keepLights.lock(), _keepCameras.lock(), _keepMaterials.lock(), _keepRenderSettings.lock() };

    for (const KnobBoolPtr& knob : keeps) {
        if (knob) {
            knob->setAllDimensionsEnabled(enabled);
        }
    }
}

void
USDPrune::onKnobsLoaded()
{
    refreshKeepKnobs();
}

bool
USDPrune::knobChanged(KnobI* k,
                      ValueChangedReasonEnum /*reason*/,
                      ViewSpec /*view*/,
                      double /*time*/,
                      bool /*originatedFromMainThread*/)
{
    KnobBoolPtr pruneUnselected = _pruneUnselected.lock();

    if (pruneUnselected && (k == pruneUnselected.get())) {
        refreshKeepKnobs();

        return true;
    }

    return false;
}

StatusEnum
USDPrune::renderScene(const SceneRenderActionArgs& args,
                      ScenePayload* outputScene,
                      std::string* errorMessage)
{
    const ScenePayload upstream = args.getInputScene(0);
    KnobChoicePtr modeKnob = _mode.lock();
    const std::string pattern = _primPattern.getExpression();
    const ModeEnum mode = (modeKnob && (modeKnob->getValue() == (int)eModeHide)) ? eModeHide : eModeDeactivate;
    const auto boolValue = [](const KnobBoolWPtr& weak) {
        KnobBoolPtr knob = weak.lock();

        return knob && knob->getValue();
    };
    const bool isolate = boolValue(_pruneUnselected);
    IsolateKeeps keeps;
    keeps.lights = boolValue(_keepLights);
    keeps.cameras = boolValue(_keepCameras);
    keeps.materials = boolValue(_keepMaterials);
    keeps.renderSettings = boolValue(_keepRenderSettings);

    const bool blank = PrimSelector::isBlank(pattern);
    const bool isolating = isolate && !blank && !upstream.isEmpty();
    const U64 isolateSettings = (U64)mode | (keeps.lights ? 2 : 0) | (keeps.cameras ? 4 : 0) | (keeps.materials ? 8 : 0) | (keeps.renderSettings ? 16 : 0);
    std::vector<PXR_NS::SdfPath> targets;
    bool remembered = false;
    if (isolating) {
        // What isolating prunes depends on every loaded prim, not only on where the pattern reaches.
        if (args.context.reach) {
            args.context.reach->add({ std::string("/") });
        }
        remembered = _isolateCache->find(upstream, args.context, pattern, isolateSettings, &targets);
    }

    if (!remembered) {
        SceneNodeAuthoring::PatternFilter filter;
        if ((mode == eModeHide) && !isolate) {
            filter = [](const PXR_NS::UsdPrim& prim) {
                return prim.IsA<PXR_NS::UsdGeomImageable>();
            };
        }
        const U64 filterKey = isolate ? (U64)eModeDeactivate : (U64)mode;
        SceneQueryPool::Lease lease;
        SceneNodeAuthoring::PatternMatches found;
        if (!SceneNodeAuthoring::matchPattern(&_matches, upstream, pattern, args.context, filterKey, filter, &lease, &found, errorMessage, isolate)) {
            setPersistentMessage(eMessageTypeError, *errorMessage);

            return eStatusFailed;
        }
        if (!isolating) {
            if (found.paths.empty() && !isolate && !upstream.isEmpty() && !blank) {
                *outputScene = upstream;
                setPersistentMessage(eMessageTypeWarning,
                                     SceneNodeAuthoring::describeNoMatch(pattern, found, tr("the scene is passed through unchanged").toStdString()));

                return eStatusOK;
            }
            targets = found.paths;
        } else {
            if (!lease.isValid()) {
                lease = SceneQueryPool::instance().borrow(upstream, args.context.loadSet, errorMessage);
            }
            const PXR_NS::UsdStageRefPtr stage = lease.getStage();
            if (!stage) {
                if (errorMessage->empty()) {
                    *errorMessage = tr("USD could not compose the input scene.").toStdString();
                }
                setPersistentMessage(eMessageTypeError, *errorMessage);

                return eStatusFailed;
            }
            // A plain path below an unloaded payload is found without its prim; the viewer never
            // looks into the payload, so it is no loaded match.
            std::vector<PXR_NS::SdfPath> matches;
            for (const PXR_NS::SdfPath& path : found.paths) {
                if (stage->GetPrimAtPath(path)) {
                    matches.push_back(path);
                }
            }
            // With no loaded match the pattern keeps nothing, and every unloaded branch is pruned
            // with the rest.
            std::vector<PXR_NS::SdfPath> reachPaths;
            if (!matches.empty()) {
                PrimSelector::Reach reach;
                if (!PrimSelector::getReach(pattern, &reach, errorMessage)) {
                    setPersistentMessage(eMessageTypeError, *errorMessage);

                    return eStatusFailed;
                }
                for (const std::string& prefix : reach.prefixes) {
                    reachPaths.push_back(PXR_NS::SdfPath(prefix));
                }
            }
            const std::function<bool()> abortedFunc = [this]() {
                return aborted();
            };
            if (!findIsolateRoots(stage, matches, reachPaths, !args.context.includesEverything(), keeps, mode == eModeHide, abortedFunc, &targets)) {
                return eStatusFailed;
            }
            _isolateCache->store(upstream, args.context, pattern, isolateSettings, targets);
        }
    }
    if (targets.empty()) {
        *outputScene = upstream;
        clearPersistentMessage(false);

        return eStatusOK;
    }

    SceneLayerMemo::AuthorFunc author = [this, targets, mode](const SceneLayerEdit& edit,
                                                              std::string* error) {
        PXR_NS::UsdStageRefPtr stage = SceneNodeAuthoring::openLayerStage(edit, error);
        if (!stage) {
            if (error->empty()) {
                *error = tr("USD could not open the node's layer.").toStdString();
            }

            return false;
        }
        for (const PXR_NS::SdfPath& path : targets) {
            PXR_NS::UsdPrim prim = stage->OverridePrim(path);
            bool ok = bool(prim);
            if (ok) {
                if (mode == eModeDeactivate) {
                    ok = prim.SetActive(false);
                } else {
                    const PXR_NS::UsdAttribute visibility = prim.CreateAttribute(PXR_NS::UsdGeomTokens->visibility, PXR_NS::SdfValueTypeNames->Token, false);
                    ok = visibility && visibility.Set(PXR_NS::UsdGeomTokens->invisible);
                }
            }
            if (!ok) {
                *error = tr("USD could not author over %1.").arg(QString::fromStdString(path.GetString())).toStdString();

                return false;
            }
        }

        return true;
    };

    // Of the input, the layer reads only the prims it authors on and the metadata: an edit
    // upstream that keeps them re-stacks the layer without authoring it again.
    ContentHasher facts;
    SceneNodeAuthoring::appendUpstreamMetadata(upstream, &facts);
    facts.appendU64(targets.size());
    for (const PXR_NS::SdfPath& path : targets) {
        facts.appendString(path.GetString());
    }
    SceneLayerReads reads;
    reads.upstream = SceneLayerMemo::Key::eUpstreamReadFacts;
    reads.factsDigest = facts.finalize();
    if (!emitSceneLayer(upstream, reads, author, outputScene, errorMessage)) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }
    clearPersistentMessage(false);

    return eStatusOK;
} // USDPrune::renderScene

NATRON_NAMESPACE_EXIT
