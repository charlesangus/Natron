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

#include "SceneOps.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <list>
#include <map>
#include <mutex>
#include <ostream>
#include <set>
#include <streambuf>
#include <string>
#include <unordered_map>
#include <vector>

#include <pxr/usd/sdf/listOp.h>
#include <pxr/usd/sdf/payload.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/sdf/variantSetSpec.h>
#include <pxr/usd/sdf/variantSpec.h>

#include "Engine/ContentHasher.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/PrimSelector.h"
#include "Engine/Nodes/Scene/SceneCaches.h"
#include "Engine/Nodes/Scene/SceneLayerMemo.h"
#include "Engine/Nodes/Scene/SceneProjectionCameras.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/Nodes/Scene/SceneWriteBarrier.h"

NATRON_NAMESPACE_ENTER

namespace {
// A property under one of an input's roots whose composed targets leave that root.
struct InputLink {
    PXR_NS::SdfPath property;
    std::vector<PXR_NS::SdfPath> targets;
    bool isConnection = false;
    bool custom = false;
    PXR_NS::SdfVariability variability = PXR_NS::SdfVariabilityUniform;
    PXR_NS::SdfValueTypeName typeName;
};

// One input of a re-parenting node, as last grafted.
struct GraftSlot {
    ScenePayload input;
    // The input's root prim names, in the composed stage's order.
    std::vector<std::string> rootNames;
    // The properties of its link index that its stage composes with targets out of their root.
    std::vector<InputLink> links;
    // Sublayers input's stack; created once the input has roots.
    PXR_NS::SdfLayerRefPtr wrapper;
    std::string destinationPath;
    // The root left at its own path rather than grafted, empty for none.
    std::string passThroughRoot;
    // Whether the input's passThroughRoot holds a UsdGeomGprim; checked when the input has it.
    bool passThroughHoldsGprim = false;
    // The name each root is grafted under, index for index with rootNames; a passed-through
    // root keeps its own.
    std::vector<std::string> childNames;
    // Null layer for an input without roots.
    SceneLayerEntry graft;
    // The load set rootNames, links and passThroughHoldsGprim were read on.
    SceneLoadSetPtr loadSet;
    // Where a load set change can change the reads of rootNames and links, and of
    // passThroughHoldsGprim, given to every evaluation reusing them.
    std::vector<std::string> readReach;
    std::vector<std::string> passThroughReach;
};

// Evaluation contexts a node keeps a layer for at once: the renders' and a few viewers'.
const std::size_t kSceneLayerMemoSlots = 4;

// What reparentRoots() last grafted for one evaluation context.
struct GraftContext {
    U64 context = 0;
    // The inputs of the context's last successful reparentRoots(), in input order.
    std::vector<GraftSlot> slots;
    ScenePayload output;
};
} // anonymous namespace

struct SceneGraftMemo::State {
    std::mutex mutex;
    std::string structurePath;
    std::string structureDestinationType;
    std::string structureAncestorType;
    SceneLayerEntry structure;
    SceneLayerMemo overrides;
    // Most recently used first, at most kSceneLayerMemoSlots.
    std::list<GraftContext> contexts;
    U64 graftLayerAuthorCount = 0;
    U64 inputReadCount = 0;

    GraftContext* find(U64 context)
    {
        for (GraftContext& entry : contexts) {
            if (entry.context == context) {
                return &entry;
            }
        }

        return nullptr;
    }

    // The entry of context, moved first, or a new one there, evicting the least recently used.
    GraftContext& use(U64 context)
    {
        for (std::list<GraftContext>::iterator it = contexts.begin(); it != contexts.end(); ++it) {
            if (it->context == context) {
                contexts.splice(contexts.begin(), contexts, it);

                return contexts.front();
            }
        }
        contexts.push_front(GraftContext());
        contexts.front().context = context;
        if (contexts.size() > kSceneLayerMemoSlots) {
            contexts.pop_back();
        }

        return contexts.front();
    }
};

namespace {
// The root prim path lies under, or the empty path when it lies under none.
PXR_NS::SdfPath
rootPrimOf(const PXR_NS::SdfPath& path)
{
    PXR_NS::SdfPath prim = path.StripAllVariantSelections().GetPrimPath();

    if (!prim.IsAbsolutePath()) {
        return PXR_NS::SdfPath();
    }
    while (prim.IsPrimPath() && !prim.IsRootPrimPath()) {
        prim = prim.GetParentPath();
    }

    return prim.IsRootPrimPath() ? prim : PXR_NS::SdfPath();
}

bool
leavesRoot(const PXR_NS::SdfPath& property,
           const std::vector<PXR_NS::SdfPath>& targets)
{
    const PXR_NS::SdfPath root = rootPrimOf(property);

    for (const PXR_NS::SdfPath& target : targets) {
        if (root.IsEmpty() || (rootPrimOf(target) != root)) {
            return true;
        }
    }

    return false;
}

void
appendLayerLinks(const PXR_NS::SdfLayerRefPtr& layer,
                 SceneLinkIndex* links)
{
    layer->Traverse(PXR_NS::SdfPath::AbsoluteRootPath(), [&layer, links](const PXR_NS::SdfPath& path) {
        if (!path.IsPrimPropertyPath()) {
            return;
        }
        PXR_NS::SdfPathListOp listOp;
        if (!layer->HasField(path, PXR_NS::SdfFieldKeys->TargetPaths, &listOp) && !layer->HasField(path, PXR_NS::SdfFieldKeys->ConnectionPaths, &listOp)) {
            return;
        }
        const PXR_NS::SdfPath property = path.StripAllVariantSelections();
        std::vector<PXR_NS::SdfPath> targets = listOp.GetAppliedItems();
        for (PXR_NS::SdfPath& target : targets) {
            target = target.MakeAbsolutePath(property.GetPrimPath());
        }
        if (leavesRoot(property, targets)) {
            links->push_back(property);
        }
    });
}

void
sortLinks(SceneLinkIndex* links)
{
    std::sort(links->begin(), links->end());
    links->erase(std::unique(links->begin(), links->end()), links->end());
}

std::shared_ptr<const SceneLinkIndex>
shareLinks(SceneLinkIndex links)
{
    if (links.empty()) {
        return std::shared_ptr<const SceneLinkIndex>();
    }
    sortLinks(&links);

    return std::make_shared<const SceneLinkIndex>(std::move(links));
}
} // anonymous namespace

namespace SceneOps {
bool
checkUsdRuntime(std::string* errorMessage)
{
    static const std::string missing = []() {
        // A no-op once AppManager has registered them; reached first only by code running without it.
        SceneCaches::registerLiveTextures(nullptr);
        std::string names;
        PXR_NS::PlugRegistry& registry = PXR_NS::PlugRegistry::GetInstance();
        for (const char* name : { "ar", "sdf", "sdr", "usd", "usdGeom", "usdLux", "usdShade" }) {
            if (!registry.GetPluginWithName(name)) {
                names += names.empty() ? name : (std::string(", ") + name);
            }
        }

        return names;
    }();

    if (missing.empty()) {
        return true;
    }
    if (errorMessage) {
        static const char* const hint = ". The plugInfo.json files belong in the usd directory beside the OpenUSD libraries; "
                                        "run with TF_DEBUG=PLUG_INFO_SEARCH to see where OpenUSD looked.";
        *errorMessage = "OpenUSD is not usable: it found no plug-in registration for " + missing + hint;
    }

    return false;
}

PXR_NS::SdfLayerRefPtr
createNodeLayer(const std::string& tag)
{
    return PXR_NS::SdfLayer::CreateAnonymous(tag);
}

ScenePayload
appendLayer(const ScenePayload& base,
            const PXR_NS::SdfLayerRefPtr& layer,
            U64 layerHash)
{
    SceneLayerEntry entry;

    entry.layer = layer;
    entry.hash = layerHash;
    entry.links = shareLinks(findLayerLinks(layer));

    return appendEntry(base, entry);
}

SceneLinkIndex
findLayerLinks(const PXR_NS::SdfLayerRefPtr& layer)
{
    SceneLinkIndex links;

    if (layer) {
        appendLayerLinks(layer, &links);
    }
    sortLinks(&links);

    return links;
}

SceneLinkIndex
findLayerStackLinks(const PXR_NS::SdfLayerRefPtr& layer)
{
    SceneLinkIndex links;

    if (!layer) {
        return links;
    }
    std::set<std::string> visited;
    std::vector<PXR_NS::SdfLayerRefPtr> pending(1, layer);
    PXR_NS::TfErrorMark mark;
    while (!pending.empty()) {
        const PXR_NS::SdfLayerRefPtr current = pending.back();
        pending.pop_back();
        if (!visited.insert(current->GetIdentifier()).second) {
            continue;
        }
        appendLayerLinks(current, &links);
        for (const std::string& subLayerPath : current->GetSubLayerPaths()) {
            const PXR_NS::SdfLayerRefPtr subLayer = PXR_NS::SdfLayer::FindOrOpenRelativeToLayer(current, subLayerPath);
            if (subLayer) {
                pending.push_back(subLayer);
            }
        }
    }
    // A missing sublayer is the stage's error to report when it composes.
    mark.Clear();
    sortLinks(&links);

    return links;
}

SceneLinkIndex
getLinks(const ScenePayload& payload)
{
    SceneLinkIndex links;

    for (const ScenePayloadImpl* link = payload.getImpl().get(); link; link = link->base.get()) {
        if (link->top.links) {
            links.insert(links.end(), link->top.links->begin(), link->top.links->end());
        }
    }
    sortLinks(&links);

    return links;
}

ScenePayload
appendEntry(const ScenePayload& base,
            const SceneLayerEntry& entry)
{
    const PXR_NS::SdfLayerRefPtr& layer = entry.layer;
    const U64 layerHash = entry.hash;

    if (!layer) {
        return base;
    }

    std::shared_ptr<ScenePayloadImpl> imp = std::make_shared<ScenePayloadImpl>();
    imp->base = base.getImpl();
    imp->top = entry;
    imp->numLayers = base.getNumLayers() + 1;

    ContentHasher h;
    h.appendU64(base.getStackHash());
    h.appendU64(layerHash);
    imp->stackHash = h.finalize();
    // 0 stands for the empty stack.
    if (imp->stackHash == 0) {
        imp->stackHash = 1;
    }

    imp->metadata = readLayerMetadata(layer, base.getMetadata());

    return ScenePayload(imp);
}

std::vector<SceneLayerEntry>
getLayers(const ScenePayload& payload)
{
    std::vector<SceneLayerEntry> layers;

    layers.reserve(payload.getNumLayers());
    for (const ScenePayloadImpl* link = payload.getImpl().get(); link; link = link->base.get()) {
        layers.push_back(link->top);
    }
    std::reverse(layers.begin(), layers.end());

    return layers;
}

namespace {
    void
    collectLiveTextures(const ScenePayloadImpl* link,
                        std::set<const ScenePayloadImpl*>* visited,
                        std::set<const LiveTextureSourceList*>* seen,
                        LiveTextureSourceList* sources)
    {
        // A link already visited was reached with everything below it.
        for (; link && visited->insert(link).second; link = link->base.get()) {
            const SceneLayerEntry& entry = link->top;
            if (entry.liveTextures && seen->insert(entry.liveTextures.get()).second) {
                sources->insert(sources->end(), entry.liveTextures->begin(), entry.liveTextures->end());
            }
            if (entry.dependencies) {
                for (const ScenePayload& stack : entry.dependencies->stacks) {
                    collectLiveTextures(stack.getImpl().get(), visited, seen, sources);
                }
            }
        }
    }
} // anonymous namespace

LiveTextureSourceList
getLiveTextures(const ScenePayload& payload)
{
    LiveTextureSourceList sources;
    std::set<const ScenePayloadImpl*> visited;
    std::set<const LiveTextureSourceList*> seen;

    collectLiveTextures(payload.getImpl().get(), &visited, &seen, &sources);

    return sources;
}

namespace {
    bool
    findDeferredProjections(const ScenePayloadImpl* link,
                            std::set<const ScenePayloadImpl*>* visited)
    {
        for (; link && visited->insert(link).second; link = link->base.get()) {
            if (link->top.deferredProjections) {
                return true;
            }
            if (link->top.dependencies) {
                for (const ScenePayload& stack : link->top.dependencies->stacks) {
                    if (findDeferredProjections(stack.getImpl().get(), visited)) {
                        return true;
                    }
                }
            }
        }

        return false;
    }
} // anonymous namespace

bool
hasDeferredProjections(const ScenePayload& payload)
{
    std::set<const ScenePayloadImpl*> visited;

    return findDeferredProjections(payload.getImpl().get(), &visited);
}

SceneMetadata
readLayerMetadata(const PXR_NS::SdfLayerRefPtr& layer,
                  const SceneMetadata& weaker)
{
    SceneMetadata metadata = weaker;

    if (!layer) {
        return metadata;
    }

    const PXR_NS::SdfPath& root = PXR_NS::SdfPath::AbsoluteRootPath();
    PXR_NS::TfToken upAxis;
    if (layer->HasField(root, PXR_NS::UsdGeomTokens->upAxis, &upAxis)) {
        if (upAxis == PXR_NS::UsdGeomTokens->z) {
            metadata.upAxis = SceneMetadata::eUpAxisZ;
        } else if (upAxis == PXR_NS::UsdGeomTokens->y) {
            metadata.upAxis = SceneMetadata::eUpAxisY;
        }
    }
    double metersPerUnit = 0.;
    if (layer->HasField(root, PXR_NS::UsdGeomTokens->metersPerUnit, &metersPerUnit) && (metersPerUnit > 0.)) {
        metadata.metersPerUnit = metersPerUnit;
    }
    if (layer->HasDefaultPrim()) {
        metadata.defaultPrim = layer->GetDefaultPrim().GetString();
    }
    if (layer->HasTimeCodesPerSecond()) {
        metadata.timeCodesPerSecond = layer->GetTimeCodesPerSecond();
    }
    std::string renderSettingsPrimPath;
    if (layer->HasField(root, PXR_NS::UsdRenderTokens->renderSettingsPrimPath, &renderSettingsPrimPath)) {
        metadata.renderSettingsPrimPath = renderSettingsPrimPath;
    }

    return metadata;
}

std::vector<std::string>
getSubLayerPaths(const ScenePayload& payload)
{
    std::vector<std::string> subLayerPaths;

    subLayerPaths.reserve(payload.getNumLayers());
    for (const ScenePayloadImpl* link = payload.getImpl().get(); link; link = link->base.get()) {
        subLayerPaths.push_back(link->top.layer->GetIdentifier());
    }

    return subLayerPaths;
}

void
writeRootMetadata(const PXR_NS::SdfLayerRefPtr& root,
                  const SceneMetadata& metadata,
                  const SceneMetadata* applied)
{
    if (!root) {
        return;
    }

    // upAxis and metersPerUnit are plug-in metadata (UsdGeom's plugInfo.json), and the stage
    // scene index resyncs the whole stage when plug-in metadata changes on / (OpenUSD v26.08
    // usdImaging/stageSceneIndex.cpp, _OnUsdObjectsChanged), so unchanged fields are not
    // rewritten. Writing a field its current value sends no notice either, but skipping it costs
    // nothing.
    const PXR_NS::SdfPath& rootPath = PXR_NS::SdfPath::AbsoluteRootPath();
    if (!applied || (applied->upAxis != metadata.upAxis)) {
        root->SetField(rootPath, PXR_NS::UsdGeomTokens->upAxis,
                       (metadata.upAxis == SceneMetadata::eUpAxisZ) ? PXR_NS::UsdGeomTokens->z : PXR_NS::UsdGeomTokens->y);
    }
    if (!applied || (applied->metersPerUnit != metadata.metersPerUnit)) {
        root->SetField(rootPath, PXR_NS::UsdGeomTokens->metersPerUnit, metadata.metersPerUnit);
    }
    if (!applied || (applied->defaultPrim != metadata.defaultPrim)) {
        if (metadata.defaultPrim.empty()) {
            root->ClearDefaultPrim();
        } else {
            root->SetDefaultPrim(PXR_NS::TfToken(metadata.defaultPrim));
        }
    }
    if (!applied || (applied->timeCodesPerSecond != metadata.timeCodesPerSecond)) {
        root->SetTimeCodesPerSecond(metadata.timeCodesPerSecond);
    }
    if (!applied || (applied->renderSettingsPrimPath != metadata.renderSettingsPrimPath)) {
        if (metadata.renderSettingsPrimPath.empty()) {
            root->EraseField(rootPath, PXR_NS::UsdRenderTokens->renderSettingsPrimPath);
        } else {
            root->SetField(rootPath, PXR_NS::UsdRenderTokens->renderSettingsPrimPath, metadata.renderSettingsPrimPath);
        }
    }
}

namespace {
    std::atomic<U64> stageOpenCount(0);
}

PXR_NS::UsdStageRefPtr
openStage(const PXR_NS::SdfLayerRefPtr& root,
          PXR_NS::UsdStage::InitialLoadSet load)
{
    if (!root) {
        return PXR_NS::UsdStageRefPtr();
    }
    ++stageOpenCount;

    // The session layer is passed explicitly as null: the Open overload without one creates an
    // anonymous session layer (stage.h). Nothing edits these stages but their owner, through the
    // root layer.
    return PXR_NS::UsdStage::Open(root, PXR_NS::SdfLayerHandle(), load);
}

U64
getStageOpenCount()
{
    return stageOpenCount.load();
}

PXR_NS::UsdStageRefPtr
compose(const ScenePayload& payload)
{
    if (!checkUsdRuntime(nullptr)) {
        return PXR_NS::UsdStageRefPtr();
    }

    PXR_NS::SdfLayerRefPtr root = PXR_NS::SdfLayer::CreateAnonymous("natron-scene-root");

    if (!root) {
        return PXR_NS::UsdStageRefPtr();
    }
    root->SetSubLayerPaths(getSubLayerPaths(payload));
    writeRootMetadata(root, payload.getMetadata(), nullptr);
    PXR_NS::UsdStageRefPtr stage = openStage(root);
    if (!stage || !hasDeferredProjections(payload)) {
        return stage;
    }
    // Resolved into a root no stage listens to yet, then composed once more, rather than edited
    // into the open stage one recomposition per value.
    PXR_NS::SdfLayerRefPtr resolved = PXR_NS::SdfLayer::CreateAnonymous("natron-scene-root");
    if (!resolved) {
        return stage;
    }
    resolved->SetSubLayerPaths(getSubLayerPaths(payload));
    writeRootMetadata(resolved, payload.getMetadata(), nullptr);
    SceneProjectionCameras::resolve(stage, resolved, nullptr);
    stage = PXR_NS::UsdStageRefPtr();

    return openStage(resolved);
}

namespace {
    void
    collectAuthoredPayloads(const PXR_NS::SdfPrimSpecHandle& spec,
                            std::set<std::string>* assets)
    {
        for (const PXR_NS::SdfPayload& payload : spec->GetFieldAs<PXR_NS::SdfPayloadListOp>(PXR_NS::SdfFieldKeys->Payload).GetAppliedItems()) {
            assets->insert(payload.GetAssetPath());
        }
        for (const PXR_NS::SdfPrimSpecHandle& child : spec->GetNameChildren()) {
            collectAuthoredPayloads(child, assets);
        }
        for (const auto& variantSet : spec->GetVariantSets()) {
            const PXR_NS::SdfVariantSetSpecHandle set = variantSet.second;
            for (const PXR_NS::SdfVariantSpecHandle& variant : set->GetVariantList()) {
                collectAuthoredPayloads(variant->GetPrimSpec(), assets);
            }
        }
    }
} // anonymous namespace

std::set<std::string>
getAuthoredPayloadAssets(const PXR_NS::SdfLayerRefPtr& layer)
{
    std::set<std::string> assets;

    if (layer) {
        collectAuthoredPayloads(layer->GetPseudoRoot(), &assets);
    }

    return assets;
}

std::vector<PXR_NS::SdfLayerRefPtr>
getFileLayerClosure(const PXR_NS::SdfLayerRefPtr& layer,
                    std::vector<std::string>* missingPaths,
                    std::vector<std::string>* payloadPaths)
{
    std::vector<PXR_NS::SdfLayerRefPtr> closure;

    if (!layer) {
        return closure;
    }

    // GetCompositionAssetDependencies lists one layer's own sublayer, reference and payload asset
    // paths, without recursing (sdf/layer.h), so the walk recurses itself. It does not say which
    // is which, so the layer's payloads are subtracted: an asset both referenced and payloaded
    // goes unopened here, and the stage composing the reference opens it.
    std::set<std::string> visited;
    std::set<std::string> missing;
    std::set<std::string> payloads;
    std::vector<PXR_NS::SdfLayerRefPtr> pending(1, layer);
    PXR_NS::TfErrorMark mark;
    while (!pending.empty()) {
        const PXR_NS::SdfLayerRefPtr current = pending.back();
        pending.pop_back();
        if (!visited.insert(current->GetIdentifier()).second) {
            continue;
        }
        if (!current->IsAnonymous()) {
            closure.push_back(current);
        }
        const std::set<std::string> authoredPayloads = getAuthoredPayloadAssets(current);
        for (const std::string& assetPath : current->GetCompositionAssetDependencies()) {
            if (assetPath.empty()) {
                continue;
            }
            if (authoredPayloads.count(assetPath)) {
                const std::string anchored = PXR_NS::SdfComputeAssetPathRelativeToLayer(current, assetPath);
                if (payloadPaths && !anchored.empty() && payloads.insert(anchored).second) {
                    payloadPaths->push_back(anchored);
                }
                continue;
            }
            // Resolves assetPath against current's location; FindOrOpenRelativeToLayer needs a
            // valid anchor, which current is.
            const PXR_NS::SdfLayerRefPtr dependency = PXR_NS::SdfLayer::FindOrOpenRelativeToLayer(current, assetPath);
            if (dependency) {
                pending.push_back(dependency);
            } else if (missingPaths) {
                const std::string anchored = PXR_NS::SdfComputeAssetPathRelativeToLayer(current, assetPath);
                if (!anchored.empty() && missing.insert(anchored).second) {
                    missingPaths->push_back(anchored);
                }
            }
        }
    }
    // A missing or unreadable dependency is the stage's error to report when it composes, not
    // this walk's.
    mark.Clear();

    return closure;
}

std::vector<PXR_NS::SdfLayerRefPtr>
getOpenFileLayers(const PXR_NS::SdfLayerRefPtr& layer)
{
    std::vector<PXR_NS::SdfLayerRefPtr> open;

    if (!layer) {
        return open;
    }
    std::set<std::string> visited;
    std::vector<PXR_NS::SdfLayerRefPtr> pending(1, layer);
    while (!pending.empty()) {
        const PXR_NS::SdfLayerRefPtr current = pending.back();
        pending.pop_back();
        if (!visited.insert(current->GetIdentifier()).second) {
            continue;
        }
        if (!current->IsAnonymous()) {
            open.push_back(current);
        }
        for (const std::string& assetPath : current->GetCompositionAssetDependencies()) {
            if (assetPath.empty()) {
                continue;
            }
            const PXR_NS::SdfLayerHandle dependency = PXR_NS::SdfLayer::FindRelativeToLayer(current, assetPath);
            if (dependency) {
                pending.push_back(PXR_NS::SdfLayerRefPtr(dependency));
            }
        }
    }

    return open;
}

namespace {
    std::atomic<std::uint64_t> fileLayerReloadCount(0);
} // anonymous namespace

bool
reloadFileLayers(const std::vector<PXR_NS::SdfLayerRefPtr>& layers,
                 std::string* errorMessage)
{
    std::set<PXR_NS::SdfLayerHandle> fileLayers;

    for (const PXR_NS::SdfLayerRefPtr& layer : layers) {
        // UsdStage::Reload documents that reloading an anonymous layer clears its content.
        if (layer && !layer->IsAnonymous()) {
            fileLayers.insert(layer);
        }
    }
    if (fileLayers.empty()) {
        return true;
    }

    bool reloaded = true;
    if (!SceneWriteBarrier::runExclusive([&fileLayers, &reloaded]() {
            // Only layers whose file changed since they were read reload (force = false compares
            // modification times, sdf/layer.h). All of them in one call: ReloadLayers wraps the
            // reloads in one change block (OpenUSD v26.08 sdf/layer.cpp), so each stage recomposes
            // once.
            reloaded = PXR_NS::SdfLayer::ReloadLayers(fileLayers, false);
            // Counted even when some layers failed: the others may have reloaded.
            fileLayerReloadCount.fetch_add(1);
        },
                                         errorMessage)) {
        return false;
    }
    if (!reloaded && errorMessage) {
        *errorMessage = "Some USD files could not be read again.";
    }

    return reloaded;
}

std::uint64_t
getFileLayerReloadCount()
{
    return fileLayerReloadCount.load();
}

double
frameToTimeCode(const ScenePayload& payload,
                double frame,
                double framesPerSecond)
{
    if (framesPerSecond <= 0.) {
        return frame;
    }

    return frame * payload.getMetadata().timeCodesPerSecond / framesPerSecond;
}

bool
parsePrimExpression(const std::string& text,
                    PXR_NS::SdfPathExpression* expression,
                    std::string* errorMessage)
{
    *expression = PXR_NS::SdfPathExpression();
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) {
        return true;
    }
    PXR_NS::SdfPathExpression parsed;
    {
        PXR_NS::TfErrorMark mark;
        parsed = PXR_NS::SdfPathExpression(text);
        mark.Clear();
    }
    if (!parsed.GetParseError().empty()) {
        *errorMessage = "\"" + text + "\" is not a valid prim selection: " + parsed.GetParseError();

        return false;
    }
    if (!parsed.IsAbsolute()) {
        *errorMessage = "\"" + text + "\" holds a relative path; prim selections name absolute paths such as /World/Geo or //*Light*.";

        return false;
    }
    if (parsed.ContainsExpressionReferences()) {
        *errorMessage = "\"" + text + "\" references a collection (%), which a prim selection cannot resolve.";

        return false;
    }
    *expression = parsed;

    return true;
}

namespace {
    // Prims visited between two polls of a selection's cancel function.
    const std::size_t kSelectCancelPollPrims = 1024;

    bool
    selectPrimsImpl(const PXR_NS::UsdStageRefPtr& stage,
                    const std::string& expression,
                    std::vector<PXR_NS::SdfPath>* paths,
                    std::string* errorMessage,
                    const std::function<bool()>& cancelled,
                    const PXR_NS::Usd_PrimFlagsPredicate& predicate = PXR_NS::UsdPrimDefaultPredicate)
    {
        paths->clear();
        PXR_NS::SdfPathExpression parsed;
        if (!parsePrimExpression(expression, &parsed, errorMessage)) {
            return false;
        }
        if (!stage || parsed.IsEmpty()) {
            return true;
        }
        const PXR_NS::UsdObjectCollectionExpressionEvaluator evaluator(stage, parsed);
        PXR_NS::UsdObjectCollectionExpressionEvaluator::IncrementalSearcher searcher = evaluator.MakeIncrementalSearcher();
        PXR_NS::UsdPrimRange range = stage->Traverse(predicate);
        std::size_t visited = 0;
        for (PXR_NS::UsdPrimRange::iterator it = range.begin(); it != range.end(); ++it) {
            if (cancelled && ((++visited % kSelectCancelPollPrims) == 0) && cancelled()) {
                paths->clear();

                return false;
            }
            const PXR_NS::SdfPredicateFunctionResult result = searcher.Next(it->GetPath());
            if (result.GetValue()) {
                paths->push_back(it->GetPath());
            } else if (result.IsConstant()) {
                // Nothing below can match: a literal path such as /World/Box costs its depth, not
                // the stage's size.
                it.PruneChildren();
            }
        }

        return true;
    }
} // anonymous namespace

bool
selectPrims(const PXR_NS::UsdStageRefPtr& stage,
            const std::string& expression,
            std::vector<PXR_NS::SdfPath>* paths,
            std::string* errorMessage)
{
    return selectPrimsImpl(stage, expression, paths, errorMessage, std::function<bool()>());
}

bool
selectPrims(const PXR_NS::UsdStageRefPtr& stage,
            const std::string& expression,
            std::vector<PXR_NS::SdfPath>* paths,
            std::string* errorMessage,
            const std::function<bool()>& cancelled)
{
    return selectPrimsImpl(stage, expression, paths, errorMessage, cancelled);
}

namespace {
    // Unlike a node's selection, a viewer's rules match an unloaded payload's own prim: it is
    // what the viewer draws a box for.
    const PXR_NS::Usd_PrimFlagsPredicate&
    drawRulePredicate()
    {
        static const PXR_NS::Usd_PrimFlagsPredicate predicate = PXR_NS::UsdPrimIsActive && PXR_NS::UsdPrimIsDefined && !PXR_NS::UsdPrimIsAbstract;

        return predicate;
    }

    bool
    isUnloadedPayload(const PXR_NS::UsdPrim& prim)
    {
        return prim.HasAuthoredPayloads() && !prim.IsLoaded();
    }

    // Whether path is root or below it.
    bool
    isAtOrBelowPath(const std::string& path,
                    const std::string& root)
    {
        if (root == "/") {
            return true;
        }

        return (path.size() >= root.size()) && (path.compare(0, root.size(), root) == 0) && ((path.size() == root.size()) || (path[root.size()] == '/'));
    }

    // Whether path is at or below one of roots, which are sorted with PathLess and none below another.
    bool
    isUnderAny(const std::string& path,
               const std::vector<std::string>& roots)
    {
        // Each root's subtree follows it in that order, so only the last root not after path can hold it.
        std::vector<std::string>::const_iterator it = std::upper_bound(roots.begin(), roots.end(), path, SceneDrawStates::PathLess());

        return (it != roots.begin()) && isAtOrBelowPath(path, *(it - 1));
    }

    // Whether one of roots, sorted as for isUnderAny(), is strictly below path.
    bool
    hasRootBelow(const std::string& path,
                 const std::vector<std::string>& roots)
    {
        std::vector<std::string>::const_iterator it = std::upper_bound(roots.begin(), roots.end(), path, SceneDrawStates::PathLess());

        return (it != roots.end()) && isAtOrBelowPath(*it, path);
    }

    struct DrawRuleWalk {
        const SceneDrawRule& rule;
        PXR_NS::TfToken kind;
        const std::function<bool()>& cancelled;
        // The layers of the stage's own layer stack that are the rule's and that are not; both
        // empty when the rule keeps to no layers.
        std::vector<PXR_NS::SdfLayerHandle> ownLayers;
        std::vector<PXR_NS::SdfLayerHandle> otherLayers;
        std::set<const PXR_NS::SdfLayer*> otherLayerSet;
        std::vector<std::pair<std::string, SceneDrawStateEnum>>* rows = nullptr;
        std::size_t visited = 0;
        bool stopped = false;

        DrawRuleWalk(const SceneDrawRule& rule,
                     const PXR_NS::UsdStageRefPtr& stage,
                     const std::function<bool()>& cancelled)
            : rule(rule)
            , kind(rule.toKind)
            , cancelled(cancelled)
        {
            if (rule.layers.empty() || !stage) {
                return;
            }
            const std::set<std::string> own(rule.layers.begin(), rule.layers.end());
            for (const PXR_NS::SdfLayerHandle& layer : stage->GetLayerStack(true)) {
                if (!layer) {
                    continue;
                }
                if (own.find(layer->GetIdentifier()) == own.end()) {
                    otherLayers.push_back(layer);
                    otherLayerSet.insert(PXR_NS::get_pointer(layer));
                } else {
                    ownLayers.push_back(layer);
                }
            }
            if (otherLayers.empty()) {
                ownLayers.clear();
            }
        }

        bool walks() const
        {
            return (rule.levels > 0) || !rule.toKind.empty();
        }

        bool poll()
        {
            if (!stopped && cancelled && ((++visited % kSelectCancelPollPrims) == 0) && cancelled()) {
                stopped = true;
            }

            return !stopped;
        }

        // Whether only the other layers of the stage's own stack author prim: a node other than the
        // rule's added it, so its draw is not the rule's to set. A spec through a reference or a
        // payload is in none of them, and keeps the prim.
        bool addedElsewhere(const PXR_NS::UsdPrim& prim) const
        {
            if (otherLayers.empty()) {
                return false;
            }
            const PXR_NS::SdfPath& path = prim.GetPath();
            for (const PXR_NS::SdfLayerHandle& layer : ownLayers) {
                if (layer->HasSpec(path)) {
                    return false;
                }
            }
            bool other = false;
            for (const PXR_NS::SdfLayerHandle& layer : otherLayers) {
                if (layer->HasSpec(path)) {
                    other = true;
                    break;
                }
            }
            if (!other) {
                return false;
            }
            // Composing the prim's stack is costly, so only a prim another layer authors gets it,
            // to find whether a reference or a payload authors it too.
            for (const PXR_NS::SdfPrimSpecHandle& spec : prim.GetPrimStack()) {
                if (spec && (otherLayerSet.find(PXR_NS::get_pointer(spec->GetLayer())) == otherLayerSet.end())) {
                    return false;
                }
            }

            return true;
        }

        void add(const PXR_NS::UsdPrim& prim,
                 SceneDrawStateEnum state)
        {
            rows->emplace_back(prim.GetPath().GetString(), state);
        }

        void levels(const PXR_NS::UsdPrim& prim,
                    int depth)
        {
            if (!poll()) {
                return;
            }
            if ((depth >= rule.levels) || isUnloadedPayload(prim)) {
                add(prim, rule.state);

                return;
            }
            add(prim, eSceneDrawStateChildren);
            for (const PXR_NS::UsdPrim& child : prim.GetFilteredChildren(drawRulePredicate())) {
                if (!addedElsewhere(child)) {
                    levels(child, depth + 1);
                }
            }
        }

        // Whether prim or a prim below it took the rule's state.
        bool toKind(const PXR_NS::UsdPrim& prim)
        {
            if (!poll()) {
                return false;
            }
            PXR_NS::TfToken primKind;
            if ((PXR_NS::UsdModelAPI(prim).GetKind(&primKind) && PXR_NS::KindRegistry::IsA(primKind, kind)) || isUnloadedPayload(prim)) {
                add(prim, rule.state);

                return true;
            }
            bool reached = false;
            for (const PXR_NS::UsdPrim& child : prim.GetFilteredChildren(drawRulePredicate())) {
                if (!addedElsewhere(child)) {
                    reached = toKind(child) || reached;
                }
            }
            if (reached) {
                // After the prims below: Children blocks nothing, so it switches none of them.
                add(prim, eSceneDrawStateChildren);
            }

            return reached;
        }

        // Fills match's rows. False once cancelled.
        bool match(const PXR_NS::UsdStageRefPtr& stage,
                   SceneDrawRuleMatch* match)
        {
            match->rows.clear();
            if (!walks()) {
                match->rows.emplace_back(match->path, rule.state);

                return true;
            }
            if (!stage) {
                return true;
            }
            const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(PXR_NS::SdfPath(match->path));
            if (!prim || !prim.IsActive() || !prim.IsDefined() || prim.IsAbstract()) {
                return true;
            }
            rows = &match->rows;
            if (rule.toKind.empty()) {
                levels(prim, 0);
            } else {
                toKind(prim);
            }
            rows = nullptr;

            return !stopped;
        }
    };

    void
    getLiteralPaths(const PrimSelector::Reach& reach,
                    std::vector<std::string>* paths)
    {
        for (const std::string& literal : reach.literals) {
            if (PXR_NS::SdfPath::IsValidPathString(literal) && PXR_NS::SdfPath(literal).IsAbsoluteRootOrPrimPath()) {
                paths->push_back(literal);
            }
        }
    }

    /**
     * @brief selectPrimsImpl() over the subtrees at roots only, sorted as for isUnderAny(). The
     * ancestors of each root go through the search first, as a whole traversal would take them,
     * since a pattern can match below a prim only because of what is above it.
     **/
    bool
    selectPrimsUnder(const PXR_NS::UsdStageRefPtr& stage,
                     const std::string& expression,
                     const std::vector<std::string>& roots,
                     std::vector<std::string>* paths,
                     const std::function<bool()>& cancelled)
    {
        PXR_NS::SdfPathExpression parsed;
        std::string error;
        if (!stage || !parsePrimExpression(expression, &parsed, &error) || parsed.IsEmpty()) {
            return true;
        }
        const PXR_NS::Usd_PrimFlagsPredicate& predicate = drawRulePredicate();
        const PXR_NS::UsdObjectCollectionExpressionEvaluator evaluator(stage, parsed);
        std::size_t visited = 0;
        for (const std::string& root : roots) {
            const PXR_NS::SdfPath rootPath(root);
            const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(rootPath);
            if (!prim || !predicate(prim)) {
                continue;
            }
            PXR_NS::UsdObjectCollectionExpressionEvaluator::IncrementalSearcher searcher = evaluator.MakeIncrementalSearcher();
            bool pruned = false;
            for (const PXR_NS::SdfPath& ancestor : rootPath.GetPrefixes()) {
                if (ancestor == rootPath) {
                    break;
                }
                // A whole traversal never reaches below a prim the predicate leaves out.
                const PXR_NS::UsdPrim above = stage->GetPrimAtPath(ancestor);
                if (!above || !predicate(above)) {
                    pruned = true;
                    break;
                }
                const PXR_NS::SdfPredicateFunctionResult result = searcher.Next(ancestor);
                if (!result.GetValue() && result.IsConstant()) {
                    pruned = true;
                    break;
                }
            }
            if (pruned) {
                continue;
            }
            PXR_NS::UsdPrimRange range(prim, predicate);
            for (PXR_NS::UsdPrimRange::iterator it = range.begin(); it != range.end(); ++it) {
                if (cancelled && ((++visited % kSelectCancelPollPrims) == 0) && cancelled()) {
                    return false;
                }
                const PXR_NS::SdfPredicateFunctionResult result = searcher.Next(it->GetPath());
                if (result.GetValue()) {
                    paths->push_back(it->GetPath().GetString());
                } else if (result.IsConstant()) {
                    it.PruneChildren();
                }
            }
        }

        return true;
    }

    void
    sortMatches(std::vector<SceneDrawRuleMatch>* matches)
    {
        const SceneDrawStates::PathLess less;
        std::sort(matches->begin(), matches->end(), [&less](const SceneDrawRuleMatch& a, const SceneDrawRuleMatch& b) { return less(a.path, b.path); });
    }
} // anonymous namespace

bool
drawRulesNeedStage(const SceneDrawRules& rules)
{
    for (const SceneDrawRule& rule : rules) {
        if ((rule.levels > 0) || !rule.toKind.empty()) {
            return true;
        }
        PrimSelector::Reach reach;
        std::string error;
        if (PrimSelector::getReach(rule.pattern, &reach, &error) && !reach.literalOnly) {
            return true;
        }
    }

    return false;
}

bool
matchDrawRules(const PXR_NS::UsdStageRefPtr& stage,
               const SceneDrawRules& rules,
               SceneDrawRuleMatches* matches,
               const std::function<bool()>& cancelled)
{
    matches->assign(rules.size(), std::vector<SceneDrawRuleMatch>());
    for (std::size_t i = 0; i < rules.size(); ++i) {
        const SceneDrawRule& rule = rules[i];
        PrimSelector::Reach reach;
        std::string error;
        if (PrimSelector::isBlank(rule.pattern) || !PrimSelector::getReach(rule.pattern, &reach, &error)) {
            // The node that set the rule reports its own bad pattern.
            continue;
        }
        std::vector<std::string> paths;
        if (reach.literalOnly) {
            getLiteralPaths(reach, &paths);
        } else if (stage) {
            std::vector<PXR_NS::SdfPath> selected;
            if (!selectPrimsImpl(stage, rule.pattern, &selected, &error, cancelled, drawRulePredicate())) {
                if (cancelled && cancelled()) {
                    matches->clear();

                    return false;
                }
                continue;
            }
            paths.reserve(selected.size());
            for (const PXR_NS::SdfPath& path : selected) {
                paths.push_back(path.GetString());
            }
        }
        DrawRuleWalk walk(rule, stage, cancelled);
        std::vector<SceneDrawRuleMatch>& ruleMatches = (*matches)[i];
        ruleMatches.resize(paths.size());
        for (std::size_t j = 0; j < paths.size(); ++j) {
            ruleMatches[j].path = paths[j];
            if (!walk.match(stage, &ruleMatches[j])) {
                matches->clear();

                return false;
            }
        }
        sortMatches(&ruleMatches);
    }

    return true;
} // matchDrawRules

bool
rematchDrawRulesUnder(const PXR_NS::UsdStageRefPtr& stage,
                      const SceneDrawRules& rules,
                      const std::vector<std::string>& roots,
                      SceneDrawRuleMatches* matches,
                      const std::function<bool()>& cancelled)
{
    if (matches->size() != rules.size()) {
        return matchDrawRules(stage, rules, matches, cancelled);
    }
    std::vector<std::string> covering;
    {
        // A changed load below a payload changes what the outermost payload above it composes.
        std::vector<std::string> sorted = getLoadReach(stage, roots);
        std::sort(sorted.begin(), sorted.end(), SceneDrawStates::PathLess());
        for (const std::string& root : sorted) {
            if (covering.empty() || !isAtOrBelowPath(root, covering.back())) {
                covering.push_back(root);
            }
        }
    }
    if (covering.empty()) {
        return true;
    }
    for (std::size_t i = 0; i < rules.size(); ++i) {
        const SceneDrawRule& rule = rules[i];
        PrimSelector::Reach reach;
        std::string error;
        if (PrimSelector::isBlank(rule.pattern) || !PrimSelector::getReach(rule.pattern, &reach, &error)) {
            continue;
        }
        DrawRuleWalk walk(rule, stage, cancelled);
        if (reach.literalOnly && !walk.walks()) {
            // Plain paths, as they are, whatever the stage holds.
            continue;
        }
        std::vector<SceneDrawRuleMatch>& ruleMatches = (*matches)[i];
        std::vector<SceneDrawRuleMatch> next;
        std::vector<std::string> redo;
        next.reserve(ruleMatches.size());
        for (SceneDrawRuleMatch& match : ruleMatches) {
            const bool under = isUnderAny(match.path, covering);
            if (under && !reach.literalOnly) {
                // Selected again below, with what the subtree holds now.
                continue;
            }
            if (under || (walk.walks() && hasRootBelow(match.path, covering))) {
                redo.push_back(match.path);
            } else {
                next.push_back(std::move(match));
            }
        }
        if (!reach.literalOnly && !selectPrimsUnder(stage, rule.pattern, covering, &redo, cancelled)) {
            return false;
        }
        for (const std::string& path : redo) {
            next.emplace_back();
            next.back().path = path;
            if (!walk.match(stage, &next.back())) {
                return false;
            }
        }
        sortMatches(&next);
        ruleMatches.swap(next);
    }

    return true;
} // rematchDrawRulesUnder

void
getDrawRulePolicy(const SceneDrawRules& rules,
                  const SceneDrawRuleMatches& matches,
                  SceneDrawStates::PolicyStates* policy)
{
    policy->clear();
    for (std::size_t i = 0; (i < rules.size()) && (i < matches.size()); ++i) {
        for (const SceneDrawRuleMatch& match : matches[i]) {
            for (const std::pair<std::string, SceneDrawStateEnum>& row : match.rows) {
                sceneAddDrawRuleMatch(row.first, row.second, rules[i].origin, policy);
            }
        }
    }
}

bool
matchDrawRules(const PXR_NS::UsdStageRefPtr& stage,
               const SceneDrawRules& rules,
               SceneDrawStates::PolicyStates* policy,
               const std::function<bool()>& cancelled)
{
    SceneDrawRuleMatches matches;
    if (!matchDrawRules(stage, rules, &matches, cancelled)) {
        policy->clear();

        return false;
    }
    getDrawRulePolicy(rules, matches, policy);

    return true;
}

PXR_NS::UsdStageLoadRules
getLoadRules(const SceneLoadSet* loadSet)
{
    PXR_NS::UsdStageLoadRules rules;

    if (!loadSet) {
        return rules;
    }
    std::vector<std::pair<PXR_NS::SdfPath, PXR_NS::UsdStageLoadRules::Rule>> entries;
    entries.reserve(loadSet->getEntries().size());
    for (const SceneLoadSet::Entry& entry : loadSet->getEntries()) {
        const PXR_NS::SdfPath path(entry.path);
        if (!path.IsAbsoluteRootOrPrimPath()) {
            continue;
        }
        PXR_NS::UsdStageLoadRules::Rule rule = PXR_NS::UsdStageLoadRules::AllRule;
        if (entry.rule == SceneLoadSet::eRuleOnly) {
            rule = PXR_NS::UsdStageLoadRules::OnlyRule;
        } else if (entry.rule == SceneLoadSet::eRuleNone) {
            rule = PXR_NS::UsdStageLoadRules::NoneRule;
        }
        entries.emplace_back(path, rule);
    }
    // USD loads the ancestors of every path a rule loads, so an OnlyRule above another loading
    // rule says nothing for its own path; and UsdStageLoadRules::Minimize() drops any rule
    // nested under an OnlyRule as redundant, which would unload the inner path. Where the rule
    // inherited from above excludes the siblings anyway, such an OnlyRule is dropped; under an
    // AllRule or no rule it is what keeps the rest of its subtree unloaded, so it becomes a
    // NoneRule, which does the same and which Minimize() leaves the inner rules under.
    std::sort(entries.begin(), entries.end(), [](const std::pair<PXR_NS::SdfPath, PXR_NS::UsdStageLoadRules::Rule>& a, const std::pair<PXR_NS::SdfPath, PXR_NS::UsdStageLoadRules::Rule>& b) {
        return a.first < b.first;
    });
    std::vector<std::pair<PXR_NS::SdfPath, PXR_NS::UsdStageLoadRules::Rule>> kept;
    kept.reserve(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        bool implied = false;
        bool inheritsExclusion = false;
        for (std::size_t j = i; j > 0; --j) {
            if (entries[i].first.HasPrefix(entries[j - 1].first)) {
                inheritsExclusion = (entries[j - 1].second != PXR_NS::UsdStageLoadRules::AllRule);
                break;
            }
        }
        bool loadsBelow = false;
        if (entries[i].second == PXR_NS::UsdStageLoadRules::OnlyRule) {
            // Sorted, so the paths below entries[i] follow it.
            for (std::size_t j = i + 1; (j < entries.size()) && entries[j].first.HasPrefix(entries[i].first); ++j) {
                if (entries[j].second != PXR_NS::UsdStageLoadRules::NoneRule) {
                    loadsBelow = true;
                    break;
                }
            }
        }
        implied = loadsBelow && inheritsExclusion;
        if (!implied) {
            kept.push_back(entries[i]);
            if (loadsBelow) {
                kept.back().second = PXR_NS::UsdStageLoadRules::NoneRule;
            }
        }
    }
    rules.SetRules(std::move(kept));

    return rules;
}

SceneLoadSetPtr
makeLoadSet(const PXR_NS::UsdStageLoadRules& rules)
{
    PXR_NS::UsdStageLoadRules minimal = rules;
    minimal.Minimize();
    std::vector<SceneLoadSet::Entry> entries;
    entries.reserve(minimal.GetRules().size());
    for (const std::pair<PXR_NS::SdfPath, PXR_NS::UsdStageLoadRules::Rule>& rule : minimal.GetRules()) {
        SceneLoadSet::RuleEnum value = SceneLoadSet::eRuleAll;
        if (rule.second == PXR_NS::UsdStageLoadRules::OnlyRule) {
            value = SceneLoadSet::eRuleOnly;
        } else if (rule.second == PXR_NS::UsdStageLoadRules::NoneRule) {
            value = SceneLoadSet::eRuleNone;
        }
        entries.push_back(SceneLoadSet::Entry { rule.first.GetString(), value });
    }

    return std::make_shared<const SceneLoadSet>(entries);
}

namespace {
    // Whether LoadAndUnload() takes path on stage: USD reports an error for a path with no prim at or
    // above it but the root, and for one at or under an inactive prim or a prototype.
    bool
    canLoadAt(const PXR_NS::UsdStageRefPtr& stage,
              const PXR_NS::SdfPath& path)
    {
        if (path.IsAbsoluteRootPath()) {
            return true;
        }
        PXR_NS::SdfPath at = path;
        PXR_NS::UsdPrim prim;
        while (!at.IsAbsoluteRootPath() && !(prim = stage->GetPrimAtPath(at))) {
            at = at.GetParentPath();
        }

        return prim && prim.IsActive() && !prim.IsInPrototype() && !prim.IsPrototype();
    }

    // The paths of paths that no other of them is an ancestor of.
    PXR_NS::SdfPathSet
    getOutermost(const PXR_NS::SdfPathSet& paths)
    {
        PXR_NS::SdfPathSet outermost;

        for (const PXR_NS::SdfPath& path : paths) {
            // Sorted, so an ancestor comes before the paths below it.
            if (outermost.empty() || !path.HasPrefix(*outermost.rbegin())) {
                outermost.insert(path);
            }
        }

        return outermost;
    }
} // anonymous namespace

std::vector<std::string>
getLoadReach(const PXR_NS::UsdStageRefPtr& stage,
             const std::vector<std::string>& prefixes)
{
    std::vector<std::string> reach;

    for (const std::string& text : prefixes) {
        const PXR_NS::SdfPath prefix(text);
        PXR_NS::SdfPath widened = prefix;
        if (stage && prefix.IsPrimPath()) {
            // The range climbs from the parent, so the last payload it meets is the outermost.
            for (const PXR_NS::SdfPath& ancestor : prefix.GetParentPath().GetAncestorsRange()) {
                const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(ancestor);
                if (prim && prim.HasAuthoredPayloads()) {
                    widened = ancestor;
                }
            }
        }
        const std::string path = widened.IsEmpty() ? text : widened.GetString();
        if (std::find(reach.begin(), reach.end(), path) == reach.end()) {
            reach.push_back(path);
        }
    }

    return reach;
} // getLoadReach

bool
loadChangeReaches(const SceneLoadSet* from,
                  const SceneLoadSet* to,
                  const std::vector<std::string>& prefixes)
{
    if (prefixes.empty()) {
        return false;
    }
    // A changed rule changes the effective rule of its own path, of what lies below it and of
    // its ancestors. An ancestor's own payload is the only thing of an ancestor that loads, and
    // getLoadReach() has already widened every prefix to the outermost payload above it.
    for (const std::string& path : SceneLoadSet::getChangedPaths(from, to)) {
        const PXR_NS::SdfPath changed(path);
        for (const std::string& prefix : prefixes) {
            const PXR_NS::SdfPath reach(prefix);
            if (changed.HasPrefix(reach) || reach.HasPrefix(changed)) {
                return true;
            }
        }
    }

    return false;
} // loadChangeReaches

bool
applyLoadSet(const PXR_NS::UsdStageRefPtr& stage,
             const SceneLoadSet* loadSet)
{
    if (!stage) {
        return false;
    }
    const PXR_NS::UsdStageLoadRules target = getLoadRules(loadSet);
    PXR_NS::UsdStageLoadRules current = stage->GetLoadRules();
    PXR_NS::UsdStageLoadRules minimalTarget = target;
    current.Minimize();
    minimalTarget.Minimize();
    if (current == minimalTarget) {
        return false;
    }

    // What is loaded now and the target leaves out goes first, which reads no file.
    PXR_NS::SdfPathSet unloads;
    for (const PXR_NS::SdfPath& loaded : stage->GetLoadSet()) {
        if (!target.IsLoaded(loaded)) {
            unloads.insert(loaded);
        }
    }
    unloads = getOutermost(unloads);
    bool edited = false;
    if (!unloads.empty()) {
        stage->LoadAndUnload(PXR_NS::SdfPathSet(), unloads, PXR_NS::UsdLoadWithDescendants);
        edited = true;
    }

    // Then the target's own rules, the outer ones first: an inner rule only has a prim to apply
    // to once the payload above it is in. Each LoadAndUnload() call composes the payloads it
    // loads before the next call can restrict them, so a subtree that loads in full except for
    // a rule below it is never loaded with descendants: it loads without them and its children
    // are visited one depth further down, each loading in full unless a rule lies below it too.
    PXR_NS::SdfPathSet ruled;
    PXR_NS::SdfPathSet allRules, onlyRules;
    for (const std::pair<PXR_NS::SdfPath, PXR_NS::UsdStageLoadRules::Rule>& rule : minimalTarget.GetRules()) {
        ruled.insert(rule.first);
        if (rule.second == PXR_NS::UsdStageLoadRules::AllRule) {
            allRules.insert(rule.first);
        } else if (rule.second == PXR_NS::UsdStageLoadRules::OnlyRule) {
            onlyRules.insert(rule.first);
        }
    }
    const PXR_NS::SdfPath& root = PXR_NS::SdfPath::AbsoluteRootPath();
    if (ruled.find(root) == ruled.end()) {
        allRules.insert(root);
    }
    const auto hasRuleBelow = [&ruled](const PXR_NS::SdfPath& path) {
        // Sorted, so the paths below path follow it.
        const PXR_NS::SdfPathSet::const_iterator next = ruled.upper_bound(path);

        return (next != ruled.end()) && next->HasPrefix(path);
    };
    std::map<std::size_t, std::pair<PXR_NS::SdfPathSet, PXR_NS::SdfPathSet>> byDepth;
    for (const PXR_NS::SdfPath& path : allRules) {
        byDepth[path.GetPathElementCount()].first.insert(path);
    }
    for (const PXR_NS::SdfPath& path : onlyRules) {
        byDepth[path.GetPathElementCount()].second.insert(path);
    }
    // Inserting deeper keys while iterating is safe: std::map iterators stay valid and the
    // iteration reaches the new keys in order.
    for (std::pair<const std::size_t, std::pair<PXR_NS::SdfPathSet, PXR_NS::SdfPathSet>>& depth : byDepth) {
        const auto loadable = [&stage](PXR_NS::SdfPathSet* paths) {
            for (PXR_NS::SdfPathSet::iterator it = paths->begin(); it != paths->end();) {
                it = canLoadAt(stage, *it) ? std::next(it) : paths->erase(it);
            }
        };
        loadable(&depth.second.first);
        loadable(&depth.second.second);
        PXR_NS::SdfPathSet whole, carved;
        for (const PXR_NS::SdfPath& path : depth.second.first) {
            // An instance has no children of its own to visit; its prototype loads as a whole.
            const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(path);
            const bool instance = prim && prim.IsInstance();
            ((hasRuleBelow(path) && !instance) ? carved : whole).insert(path);
        }
        PXR_NS::SdfPathSet alone = depth.second.second;
        for (const PXR_NS::SdfPath& path : carved) {
            if (!path.IsAbsoluteRootPath()) {
                alone.insert(path);
            }
        }
        if (!whole.empty()) {
            stage->LoadAndUnload(whole, PXR_NS::SdfPathSet(), PXR_NS::UsdLoadWithDescendants);
            edited = true;
        }
        if (!alone.empty()) {
            stage->LoadAndUnload(alone, PXR_NS::SdfPathSet(), PXR_NS::UsdLoadWithoutDescendants);
            edited = true;
        }
        for (const PXR_NS::SdfPath& path : carved) {
            const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(path);
            if (!prim) {
                continue;
            }
            for (const PXR_NS::UsdPrim& child : prim.GetAllChildren()) {
                // A child with a rule of its own is already queued under that rule.
                if (ruled.find(child.GetPath()) == ruled.end()) {
                    byDepth[depth.first + 1].first.insert(child.GetPath());
                }
            }
        }
    }
    PXR_NS::SdfPathSet reloaded;
    for (const PXR_NS::SdfPath& loaded : stage->GetLoadSet()) {
        if (!target.IsLoaded(loaded)) {
            reloaded.insert(loaded);
        }
    }
    reloaded = getOutermost(reloaded);
    if (!reloaded.empty()) {
        stage->LoadAndUnload(PXR_NS::SdfPathSet(), reloaded, PXR_NS::UsdLoadWithDescendants);
        edited = true;
    }

    return edited;
} // applyLoadSet

std::size_t
countUnloadedPayloads(const PXR_NS::UsdStageRefPtr& stage,
                      const std::vector<std::string>& paths)
{
    if (!stage) {
        return 0;
    }
    PXR_NS::SdfPathSet roots;
    for (const std::string& text : paths) {
        const PXR_NS::SdfPath path(text);
        if (path.IsAbsoluteRootOrPrimPath()) {
            roots.insert(path);
        }
    }
    PXR_NS::SdfPathSet counted;
    for (const PXR_NS::SdfPath& root : getOutermost(roots)) {
        PXR_NS::SdfPath at = root;
        PXR_NS::UsdPrim prim;
        while (!(prim = stage->GetPrimAtPath(at)) && !at.IsAbsoluteRootPath()) {
            at = at.GetParentPath();
        }
        if (!prim || !prim.IsActive()) {
            continue;
        }
        if (at != root) {
            // The path lies inside a payload not loaded, or names no prim at all.
            if (prim.HasAuthoredPayloads() && !prim.IsLoaded()) {
                counted.insert(at);
            }
            continue;
        }
        // The predicate leaves IsLoaded out, or the walk would not see what it counts.
        const PXR_NS::UsdPrimRange range(prim, PXR_NS::UsdPrimIsActive && PXR_NS::UsdPrimIsDefined && !PXR_NS::UsdPrimIsAbstract);
        for (PXR_NS::UsdPrimRange::const_iterator it = range.begin(); it != range.end(); ++it) {
            if (it->HasAuthoredPayloads() && !it->IsLoaded()) {
                counted.insert(it->GetPath());
                it.PruneChildren();
            }
        }
    }

    return counted.size();
} // countUnloadedPayloads

double
getMetricsCorrectionAngle(const SceneMetricsCorrection& correction)
{
    if (!correction.correctUpAxis || (correction.fromUpAxis == correction.toUpAxis)) {
        return 0.;
    }

    return (correction.toUpAxis == SceneMetadata::eUpAxisZ) ? 90. : -90.;
}

double
getMetricsCorrectionScale(const SceneMetricsCorrection& correction)
{
    if (!correction.correctUnits || (correction.fromMetersPerUnit <= 0.) || (correction.toMetersPerUnit <= 0.)
        || PXR_NS::UsdGeomLinearUnitsAre(correction.fromMetersPerUnit, correction.toMetersPerUnit)) {
        return 1.;
    }

    return correction.fromMetersPerUnit / correction.toMetersPerUnit;
}

namespace {
    // name, or name with the first "_<n>" suffix that neither prim nor order has.
    PXR_NS::TfToken
    getFreeCorrectionOpName(const PXR_NS::UsdPrim& prim,
                            const PXR_NS::VtTokenArray& order,
                            const std::string& name)
    {
        const auto isFree = [&prim, &order](const PXR_NS::TfToken& candidate) {
            return !prim.HasAttribute(candidate) && (std::find(order.begin(), order.end(), candidate) == order.end());
        };
        PXR_NS::TfToken candidate(name);
        for (int i = 2; !isFree(candidate); ++i) {
            candidate = PXR_NS::TfToken(name + "_" + std::to_string(i));
        }

        return candidate;
    }

    PXR_NS::SdfAttributeSpecHandle
    getAttributeSpec(const PXR_NS::SdfLayerRefPtr& layer,
                     const PXR_NS::SdfPath& primPath,
                     const PXR_NS::TfToken& name,
                     const PXR_NS::SdfValueTypeName& typeName,
                     PXR_NS::SdfVariability variability)
    {
        const PXR_NS::SdfPrimSpecHandle prim = PXR_NS::SdfCreatePrimInLayer(layer, primPath);
        if (!prim) {
            return PXR_NS::SdfAttributeSpecHandle();
        }
        PXR_NS::SdfAttributeSpecHandle spec = layer->GetAttributeAtPath(primPath.AppendProperty(name));

        return spec ? spec : PXR_NS::SdfAttributeSpec::New(prim, name.GetString(), typeName, variability);
    }

    // Authors attr's value in layer scaled by scale, its default when it authors one and every
    // time sample, the sample times mapped by sourceToLayer.
    template <typename T, typename SCALE>
    void
    rescaleAttribute(const PXR_NS::SdfLayerRefPtr& layer,
                     const PXR_NS::UsdAttribute& attr,
                     const PXR_NS::SdfLayerOffset& sourceToLayer,
                     const SCALE& scale)
    {
        if (!attr || !attr.HasAuthoredValue()) {
            return;
        }
        const PXR_NS::SdfAttributeSpecHandle spec = getAttributeSpec(layer, attr.GetPrim().GetPath(), attr.GetName(), attr.GetTypeName(), attr.GetVariability());
        if (!spec) {
            return;
        }
        T value;
        if ((attr.GetResolveInfo(PXR_NS::UsdTimeCode::Default()).GetSource() == PXR_NS::UsdResolveInfoSourceDefault) && attr.Get(&value, PXR_NS::UsdTimeCode::Default())) {
            spec->SetDefaultValue(PXR_NS::VtValue(scale(value)));
        }
        std::vector<double> times;
        attr.GetTimeSamples(&times);
        for (double time : times) {
            if (attr.Get(&value, time)) {
                layer->SetTimeSample(spec->GetPath(), sourceToLayer * time, scale(value));
            }
        }
    }

    void
    rescaleCamera(const PXR_NS::SdfLayerRefPtr& layer,
                  const PXR_NS::UsdGeomCamera& camera,
                  const PXR_NS::SdfLayerOffset& sourceToLayer,
                  double scale)
    {
        const float s = (float)scale;
        const auto scaleFloat = [s](float v) { return v * s; };

        rescaleAttribute<PXR_NS::GfVec2f>(layer, camera.GetClippingRangeAttr(), sourceToLayer, [s](const PXR_NS::GfVec2f& v) { return v * s; });
        // A clipping plane (a, b, c, d) in camera space keeps its normal; its distance scales.
        rescaleAttribute<PXR_NS::VtVec4fArray>(layer, camera.GetClippingPlanesAttr(), sourceToLayer, [s](const PXR_NS::VtVec4fArray& planes) {
            PXR_NS::VtVec4fArray scaled(planes);
            for (PXR_NS::GfVec4f& plane : scaled) {
                plane[3] *= s;
            }

            return scaled;
        });
        // Lens lengths are tenths of a scene unit: scaling them keeps the lens the same physical
        // size, and the field of view, which only their ratios set, unchanged.
        for (const PXR_NS::UsdAttribute& attr : { camera.GetFocusDistanceAttr(), camera.GetFocalLengthAttr(), camera.GetHorizontalApertureAttr(),
                                                  camera.GetVerticalApertureAttr(), camera.GetHorizontalApertureOffsetAttr(), camera.GetVerticalApertureOffsetAttr() }) {
            rescaleAttribute<float>(layer, attr, sourceToLayer, scaleFloat);
        }
    }
} // anonymous namespace

std::size_t
authorMetricsCorrection(const PXR_NS::SdfLayerRefPtr& layer,
                        const PXR_NS::UsdStageRefPtr& source,
                        const SceneMetricsCorrection& correction)
{
    const double angle = getMetricsCorrectionAngle(correction);
    const double scale = getMetricsCorrectionScale(correction);

    if (!layer || !source || ((angle == 0.) && (scale == 1.))) {
        return 0;
    }

    // A prim is corrected when no xformable ancestor carries the correction to it: it is the
    // topmost xformable on its branch, or its op stack resets and so ignores its ancestors'.
    std::vector<PXR_NS::UsdPrim> targets;
    std::vector<PXR_NS::UsdPrim> cameras;
    {
        int xformableAncestors = 0;
        PXR_NS::UsdPrimRange range = PXR_NS::UsdPrimRange::PreAndPostVisit(source->GetPseudoRoot());
        for (PXR_NS::UsdPrimRange::iterator it = range.begin(); it != range.end(); ++it) {
            const PXR_NS::UsdPrim& prim = *it;
            const bool xformable = prim.IsA<PXR_NS::UsdGeomXformable>();
            if (it.IsPostVisit()) {
                if (xformable) {
                    --xformableAncestors;
                }
                continue;
            }
            if (xformable) {
                if ((xformableAncestors == 0) || PXR_NS::UsdGeomXformable(prim).GetResetXformStack()) {
                    targets.push_back(prim);
                }
                ++xformableAncestors;
            }
            if ((scale != 1.) && prim.IsA<PXR_NS::UsdGeomCamera>()) {
                cameras.push_back(prim);
            }
        }
    }

    PXR_NS::SdfChangeBlock block;
    for (const PXR_NS::UsdPrim& prim : targets) {
        PXR_NS::VtTokenArray order;
        PXR_NS::UsdGeomXformable(prim).GetXformOpOrderAttr().Get(&order);
        const bool resets = !order.empty() && (order[0] == PXR_NS::UsdGeomXformOpTypes->resetXformStack);

        PXR_NS::VtTokenArray corrected;
        if (resets) {
            corrected.push_back(order[0]);
        }
        if (angle != 0.) {
            const PXR_NS::TfToken name = getFreeCorrectionOpName(prim, order, "xformOp:rotateX:upAxisCorrection");
            const PXR_NS::SdfAttributeSpecHandle op = getAttributeSpec(layer, prim.GetPath(), name, PXR_NS::SdfValueTypeNames->Double, PXR_NS::SdfVariabilityVarying);
            if (op) {
                op->SetDefaultValue(PXR_NS::VtValue(angle));
                corrected.push_back(name);
            }
        }
        if (scale != 1.) {
            const PXR_NS::TfToken name = getFreeCorrectionOpName(prim, order, "xformOp:scale:unitsCorrection");
            const PXR_NS::SdfAttributeSpecHandle op = getAttributeSpec(layer, prim.GetPath(), name, PXR_NS::SdfValueTypeNames->Double3, PXR_NS::SdfVariabilityVarying);
            if (op) {
                op->SetDefaultValue(PXR_NS::VtValue(PXR_NS::GfVec3d(scale)));
                corrected.push_back(name);
            }
        }
        for (std::size_t i = resets ? 1 : 0; i < order.size(); ++i) {
            corrected.push_back(order[i]);
        }
        const PXR_NS::SdfAttributeSpecHandle orderSpec = getAttributeSpec(layer, prim.GetPath(), PXR_NS::UsdGeomTokens->xformOpOrder,
                                                                          PXR_NS::SdfValueTypeNames->TokenArray, PXR_NS::SdfVariabilityUniform);
        if (orderSpec) {
            orderSpec->SetDefaultValue(PXR_NS::VtValue(corrected));
        }
    }
    for (const PXR_NS::UsdPrim& prim : cameras) {
        rescaleCamera(layer, PXR_NS::UsdGeomCamera(prim), correction.sourceToLayer, scale);
    }

    return targets.size();
} // authorMetricsCorrection

namespace {
    struct LayerOwner {
        // Expired once the layer is gone, so an entry outliving its layer never answers for
        // another layer allocated at the same address.
        PXR_NS::SdfLayerHandle layer;
        NodeWPtr owner;
    };

    struct LayerOwnerRegistry {
        std::mutex mutex;
        std::unordered_map<const PXR_NS::SdfLayer*, LayerOwner> owners;
        std::size_t sweepAt = 1024;
    };

    LayerOwnerRegistry&
    layerOwnerRegistry()
    {
        static LayerOwnerRegistry registry;

        return registry;
    }

    void
    recordLayerOwner(LayerOwnerRegistry& registry,
                     const PXR_NS::SdfLayerHandle& layer,
                     const NodePtr& owner)
    {
        if (!layer) {
            return;
        }
        LayerOwner& entry = registry.owners[PXR_NS::get_pointer(layer)];
        entry.layer = layer;
        entry.owner = owner;
        if (registry.owners.size() >= registry.sweepAt) {
            for (auto it = registry.owners.begin(); it != registry.owners.end();) {
                if (!it->second.layer || it->second.owner.expired()) {
                    it = registry.owners.erase(it);
                } else {
                    ++it;
                }
            }
            registry.sweepAt = std::max<std::size_t>(1024, registry.owners.size() * 2);
        }
    }
} // anonymous namespace

void
setLayerOwner(const ScenePayload& payload,
              const NodePtr& owner,
              bool topOnly)
{
    if (payload.isEmpty() || !owner) {
        return;
    }
    LayerOwnerRegistry& registry = layerOwnerRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    if (topOnly) {
        const std::shared_ptr<const ScenePayloadImpl>& impl = payload.getImpl();
        if (impl) {
            recordLayerOwner(registry, impl->top.layer, owner);
        }

        return;
    }
    for (const SceneLayerEntry& entry : getLayers(payload)) {
        recordLayerOwner(registry, entry.layer, owner);
    }
}

NodePtr
getLayerOwner(const PXR_NS::SdfLayerHandle& layer)
{
    if (!layer) {
        return NodePtr();
    }
    LayerOwnerRegistry& registry = layerOwnerRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    const auto found = registry.owners.find(PXR_NS::get_pointer(layer));

    if ((found == registry.owners.end()) || (found->second.layer != layer)) {
        return NodePtr();
    }

    return found->second.owner.lock();
}

std::vector<std::string>
getAuthoredPrimPaths(const ScenePayload& payload,
                     const NodePtr& owner)
{
    std::set<PXR_NS::SdfPath> paths;

    if (!owner) {
        return std::vector<std::string>();
    }
    for (const SceneLayerEntry& entry : getLayers(payload)) {
        if (!entry.layer || (getLayerOwner(entry.layer) != owner)) {
            continue;
        }
        entry.layer->Traverse(PXR_NS::SdfPath::AbsoluteRootPath(), [&paths](const PXR_NS::SdfPath& path) {
            const PXR_NS::SdfPath primPath = path.GetPrimPath().StripAllVariantSelections();
            if (!primPath.IsEmpty() && (primPath != PXR_NS::SdfPath::AbsoluteRootPath())) {
                paths.insert(primPath);
            }
        });
    }
    std::vector<std::string> result;

    result.reserve(paths.size());
    for (const PXR_NS::SdfPath& path : paths) {
        result.push_back(path.GetString());
    }

    return result;
}

std::vector<PXR_NS::SdfLayerRefPtr>
getOwnedLayers(const ScenePayload& payload,
               const NodePtr& owner)
{
    std::vector<PXR_NS::SdfLayerRefPtr> layers;

    if (!owner) {
        return layers;
    }
    for (const SceneLayerEntry& entry : getLayers(payload)) {
        if (entry.layer && (getLayerOwner(entry.layer) == owner)) {
            layers.push_back(entry.layer);
        }
    }

    return layers;
}

std::string
exportLayersText(const std::vector<PXR_NS::SdfLayerRefPtr>& layers)
{
    std::string text;

    for (const PXR_NS::SdfLayerRefPtr& layer : layers) {
        std::string layerText;

        if (!layer->ExportToString(&layerText)) {
            continue;
        }
        if (!text.empty()) {
            text += "\n";
        }
        text += "# Layer: " + layer->GetIdentifier() + "\n" + layerText;
    }

    return text;
}

PXR_NS::UsdStageRefPtr
beginPrimDefinition(const PXR_NS::UsdPrim& prim)
{
    if (!prim) {
        return PXR_NS::UsdStageRefPtr();
    }
    const PXR_NS::UsdStageRefPtr definition = PXR_NS::UsdStage::CreateInMemory();
    PXR_NS::UsdPrim copy;

    switch (prim.GetSpecifier()) {
    case PXR_NS::SdfSpecifierClass:
        copy = definition->CreateClassPrim(prim.GetPath());
        break;
    case PXR_NS::SdfSpecifierOver:
        copy = definition->OverridePrim(prim.GetPath());
        break;
    default:
        copy = definition->DefinePrim(prim.GetPath(), prim.GetTypeName());
        break;
    }
    if (!copy) {
        return definition;
    }
    static const std::set<PXR_NS::TfToken> arcFields = {
        PXR_NS::SdfFieldKeys->Specifier,
        PXR_NS::SdfFieldKeys->TypeName,
        PXR_NS::SdfFieldKeys->References,
        PXR_NS::SdfFieldKeys->Payload,
        PXR_NS::SdfFieldKeys->InheritPaths,
        PXR_NS::SdfFieldKeys->Specializes,
        PXR_NS::SdfFieldKeys->VariantSetNames,
        PXR_NS::SdfFieldKeys->VariantSelection,
        PXR_NS::SdfFieldKeys->PrimOrder,
        PXR_NS::SdfFieldKeys->PropertyOrder,
    };
    for (const auto& field : prim.GetAllAuthoredMetadata()) {
        if (arcFields.find(field.first) == arcFields.end()) {
            copy.SetMetadata(field.first, field.second);
        }
    }

    return definition;
}

void
flattenPropertyInto(const PXR_NS::UsdStageRefPtr& definition,
                    const PXR_NS::UsdProperty& property)
{
    if (!definition || !property) {
        return;
    }
    const PXR_NS::UsdPrim target = definition->GetPrimAtPath(property.GetPrimPath());

    if (target) {
        property.FlattenTo(target);
    }
}

std::string
exportDefinitionText(const PXR_NS::UsdStageRefPtr& definition)
{
    std::string text;

    if (definition) {
        definition->GetRootLayer()->ExportToString(&text);
    }

    return text;
}

NodePtr
authoringNodeOf(const PXR_NS::UsdAttribute& attribute,
                PXR_NS::UsdTimeCode time,
                std::string* layerIdentifier)
{
    if (layerIdentifier) {
        layerIdentifier->clear();
    }
    if (!attribute) {
        return NodePtr();
    }
    for (const PXR_NS::SdfPropertySpecHandle& spec : attribute.GetPropertyStack(time)) {
        if (!spec) {
            continue;
        }
        const PXR_NS::SdfLayerHandle layer = spec->GetLayer();
        const PXR_NS::SdfAttributeSpecHandle attributeSpec = PXR_NS::TfDynamic_cast<PXR_NS::SdfAttributeSpecHandle>(spec);
        const bool holdsValue = spec->HasDefaultValue() || (layer && (layer->GetNumTimeSamplesForPath(spec->GetPath()) > 0)) || (attributeSpec && attributeSpec->HasSpline());
        if (!holdsValue) {
            continue;
        }
        if (layerIdentifier && layer) {
            *layerIdentifier = layer->GetIdentifier();
        }

        return getLayerOwner(layer);
    }

    return NodePtr();
}

namespace {
    ScenePrimArc::Type
    arcTypeOf(PXR_NS::PcpArcType type)
    {
        switch (type) {
        case PXR_NS::PcpArcTypeInherit:
            return ScenePrimArc::eInherit;
        case PXR_NS::PcpArcTypeVariant:
            return ScenePrimArc::eVariant;
        case PXR_NS::PcpArcTypeRelocate:
            return ScenePrimArc::eRelocate;
        case PXR_NS::PcpArcTypeReference:
            return ScenePrimArc::eReference;
        case PXR_NS::PcpArcTypePayload:
            return ScenePrimArc::ePayload;
        case PXR_NS::PcpArcTypeSpecialize:
            return ScenePrimArc::eSpecialize;
        default:
            break;
        }

        return ScenePrimArc::eRoot;
    }

    std::string
    identifierOf(const PXR_NS::SdfLayerHandle& layer)
    {
        return layer ? layer->GetIdentifier() : std::string();
    }

    // The query's target layer is the root of the target's layer stack. In a node graph that root
    // is the stage's composed root or a graft's wrapper, which only sublayer the nodes' layers and
    // belong to no node, so the layer to name is the strongest that holds the target's spec.
    PXR_NS::SdfLayerHandle
    targetSpecLayerOf(const PXR_NS::UsdPrimCompositionQueryArc& arc)
    {
        const PXR_NS::PcpNodeRef node = arc.GetTargetNode();

        for (const PXR_NS::SdfLayerRefPtr& layer : node.GetLayerStack()->GetLayers()) {
            if (layer->HasSpec(node.GetPath())) {
                return layer;
            }
        }

        return arc.GetTargetLayer();
    }
} // anonymous namespace

ScenePrimComposition
getPrimComposition(const PXR_NS::UsdPrim& prim)
{
    ScenePrimComposition composition;

    if (!prim) {
        return composition;
    }
    if (prim.IsPrototype()) {
        composition.isPrototype = true;

        return composition;
    }
    if (prim.IsInstanceProxy()) {
        composition.isInstanceProxy = true;
        composition.prototypePath = prim.GetPrimInPrototype().GetPath().GetString();

        return composition;
    }
    PXR_NS::UsdPrimCompositionQuery query(prim);
    for (const PXR_NS::UsdPrimCompositionQueryArc& queried : query.GetCompositionArcs()) {
        ScenePrimArc arc;
        arc.type = arcTypeOf(queried.GetArcType());
        for (PXR_NS::PcpNodeRef node = queried.GetTargetNode().GetParentNode(); node; node = node.GetParentNode()) {
            ++arc.depth;
        }
        arc.ancestral = queried.IsAncestral();
        arc.implicit = queried.IsImplicit();
        arc.targetPath = queried.GetTargetPrimPath().GetString();
        const PXR_NS::SdfLayerHandle target = targetSpecLayerOf(queried);
        arc.targetLayer = identifierOf(target);
        arc.targetOwner = getLayerOwner(target);
        if (arc.type != ScenePrimArc::eRoot) {
            const PXR_NS::SdfLayerHandle introducing = queried.GetIntroducingLayer();
            arc.introducingLayer = identifierOf(introducing);
            arc.introducingOwner = getLayerOwner(introducing);
        }
        composition.arcs.push_back(std::move(arc));
    }
    if (prim.HasAuthoredPayloads() && !prim.IsLoaded()) {
        PXR_NS::SdfLayerHandle introducing;
        for (const PXR_NS::SdfPrimSpecHandle& spec : prim.GetPrimStack()) {
            if (spec && spec->HasField(PXR_NS::SdfFieldKeys->Payload)) {
                introducing = spec->GetLayer();
                break;
            }
        }
        PXR_NS::VtValue value;
        prim.GetMetadata(PXR_NS::SdfFieldKeys->Payload, &value);
        if (value.IsHolding<PXR_NS::SdfPayloadListOp>()) {
            for (const PXR_NS::SdfPayload& payload : value.UncheckedGet<PXR_NS::SdfPayloadListOp>().GetAppliedItems()) {
                ScenePrimArc arc;
                arc.type = ScenePrimArc::ePayload;
                arc.depth = 1;
                arc.loaded = false;
                arc.targetPath = payload.GetPrimPath().GetString();
                arc.targetLayer = payload.GetAssetPath();
                arc.introducingLayer = identifierOf(introducing);
                arc.introducingOwner = getLayerOwner(introducing);
                composition.arcs.push_back(std::move(arc));
            }
        }
    }

    return composition;
}

namespace {
    SceneOpinion
    opinionOf(const PXR_NS::SdfLayerHandle& layer,
              const PXR_NS::SdfLayerOffset& offset)
    {
        SceneOpinion opinion;

        opinion.owner = getLayerOwner(layer);
        opinion.layerIdentifier = identifierOf(layer);
        opinion.layerOffset = offset.GetOffset();
        opinion.layerScale = offset.GetScale();

        return opinion;
    }
} // anonymous namespace

std::vector<SceneOpinion>
getPrimOpinions(const PXR_NS::UsdPrim& prim)
{
    std::vector<SceneOpinion> opinions;

    if (!prim) {
        return opinions;
    }
    for (const auto& entry : prim.GetPrimStackWithLayerOffsets()) {
        const PXR_NS::SdfPrimSpecHandle& spec = entry.first;
        if (!spec) {
            continue;
        }
        SceneOpinion opinion = opinionOf(spec->GetLayer(), entry.second);
        switch (spec->GetSpecifier()) {
        case PXR_NS::SdfSpecifierDef:
            opinion.primDescription = "def";
            break;
        case PXR_NS::SdfSpecifierOver:
            opinion.primDescription = "over";
            break;
        default:
            opinion.primDescription = "class";
            break;
        }
        if (!spec->GetTypeName().IsEmpty()) {
            opinion.primDescription += " " + spec->GetTypeName().GetString();
        }
        opinions.push_back(std::move(opinion));
    }

    return opinions;
}

std::vector<SceneOpinion>
getPropertyOpinions(const PXR_NS::UsdPrim& prim,
                    const PXR_NS::TfToken& name,
                    PXR_NS::UsdTimeCode time)
{
    std::vector<SceneOpinion> opinions;

    if (!prim) {
        return opinions;
    }
    const PXR_NS::UsdProperty property = prim.GetProperty(name);
    if (!property) {
        return opinions;
    }
    for (const auto& entry : property.GetPropertyStackWithLayerOffsets(time)) {
        const PXR_NS::SdfPropertySpecHandle& spec = entry.first;
        if (!spec) {
            continue;
        }
        const PXR_NS::SdfLayerHandle layer = spec->GetLayer();
        SceneOpinion opinion = opinionOf(layer, entry.second);
        opinion.propertySpec = spec;
        if (layer) {
            opinion.numTimeSamples = layer->GetNumTimeSamplesForPath(spec->GetPath());
        }
        const PXR_NS::SdfAttributeSpecHandle attributeSpec = PXR_NS::TfDynamic_cast<PXR_NS::SdfAttributeSpecHandle>(spec);
        opinion.hasSpline = attributeSpec && attributeSpec->HasSpline();
        opinions.push_back(std::move(opinion));
    }

    return opinions;
}

void
readOpinionValue(SceneOpinion* opinion)
{
    const PXR_NS::SdfPropertySpecHandle& spec = opinion->propertySpec;

    if (!spec) {
        return;
    }
    if (spec->HasDefaultValue()) {
        opinion->value = spec->GetDefaultValue();
    } else if (spec->HasField(PXR_NS::SdfFieldKeys->TargetPaths)) {
        opinion->value = spec->GetField(PXR_NS::SdfFieldKeys->TargetPaths);
    }
}

namespace {
    struct ValueTextStopped {
    };

    // Throwing is the only way out of a value's stream operator, which otherwise goes on formatting
    // every element of an array after the stream has failed.
    class BoundedTextBuffer
        : public std::streambuf {
    public:
        BoundedTextBuffer(std::size_t maxLength,
                          const std::atomic<bool>* cancelled)
            : _maxLength(maxLength)
            , _cancelled(cancelled)
        {
        }

        std::string text;
        bool cut = false;

    protected:
        int_type overflow(int_type c) override
        {
            if (!traits_type::eq_int_type(c, traits_type::eof())) {
                const char asChar = traits_type::to_char_type(c);
                xsputn(&asChar, 1);
            }

            return traits_type::not_eof(c);
        }

        std::streamsize xsputn(const char* s,
                               std::streamsize n) override
        {
            if (_cancelled && *_cancelled) {
                throw ValueTextStopped();
            }
            const std::size_t room = _maxLength - text.size();
            if (room >= (std::size_t)n) {
                text.append(s, (std::size_t)n);

                return n;
            }
            text.append(s, room);
            cut = true;
            throw ValueTextStopped();
        }

    private:
        std::size_t _maxLength;
        const std::atomic<bool>* _cancelled;
    };

    PXR_NS::UsdPrim
    primAt(const SceneStage& stage,
           const std::string& primPath)
    {
        if (!stage.getStage() || !PXR_NS::SdfPath::IsValidPathString(primPath)) {
            return PXR_NS::UsdPrim();
        }

        return stage.getStage()->GetPrimAtPath(PXR_NS::SdfPath(primPath));
    }

    SceneAuthor
    authorOfLayer(const PXR_NS::SdfLayerHandle& layer,
                  bool schemaFallback)
    {
        SceneAuthor author;

        author.node = getLayerOwner(layer);
        author.layerIdentifier = identifierOf(layer);
        author.schemaFallback = !layer && schemaFallback;

        return author;
    }

    SceneValueSource
    valueSourceOf(PXR_NS::UsdResolveInfoSource source)
    {
        switch (source) {
        case PXR_NS::UsdResolveInfoSourceFallback:
            return eSceneValueSourceFallback;
        case PXR_NS::UsdResolveInfoSourceDefault:
            return eSceneValueSourceDefault;
        case PXR_NS::UsdResolveInfoSourceTimeSamples:
            return eSceneValueSourceTimeSamples;
        case PXR_NS::UsdResolveInfoSourceValueClips:
            return eSceneValueSourceValueClips;
        case PXR_NS::UsdResolveInfoSourceSpline:
            return eSceneValueSourceSpline;
        case PXR_NS::UsdResolveInfoSourceNone:
            break;
        }

        return eSceneValueSourceNone;
    }

    // The strongest spec authoring targets, else the strongest declaring the relationship.
    PXR_NS::SdfLayerHandle
    targetsLayer(const PXR_NS::UsdRelationship& relationship)
    {
        PXR_NS::SdfLayerHandle declaring;

        for (const PXR_NS::SdfPropertySpecHandle& spec : relationship.GetPropertyStack()) {
            if (!spec) {
                continue;
            }
            if (spec->HasField(PXR_NS::SdfFieldKeys->TargetPaths)) {
                return spec->GetLayer();
            }
            if (!declaring) {
                declaring = spec->GetLayer();
            }
        }

        return declaring;
    }

    // A prim's specifier composes to its strongest non-over, whichever layer last wrote an over.
    PXR_NS::SdfLayerHandle
    metadataLayer(const PXR_NS::SdfPrimSpecHandleVector& stack,
                  const PXR_NS::TfToken& key)
    {
        const bool isSpecifier = (key == PXR_NS::SdfFieldKeys->Specifier);
        PXR_NS::SdfLayerHandle strongest;

        for (const PXR_NS::SdfPrimSpecHandle& spec : stack) {
            if (!spec || !spec->HasField(key)) {
                continue;
            }
            if (!strongest) {
                strongest = spec->GetLayer();
            }
            if (!isSpecifier || (spec->GetSpecifier() != PXR_NS::SdfSpecifierOver)) {
                return spec->GetLayer();
            }
        }

        return strongest;
    }
} // anonymous namespace

std::string
valueText(const PXR_NS::VtValue& value,
          std::size_t maxLength,
          const std::atomic<bool>* cancelled,
          bool* cut)
{
    if (cut) {
        *cut = false;
    }
    if (value.IsEmpty()) {
        return std::string();
    }
    BoundedTextBuffer buffer(maxLength, cancelled);
    std::ostream stream(&buffer);

    // Without this the stream swallows the buffer's exception and the operator carries on.
    stream.exceptions(std::ios::badbit);
    try {
        stream << value;
    } catch (const ValueTextStopped&) {
    } catch (const std::exception&) {
    }
    if (buffer.cut) {
        // The cut may split a UTF-8 sequence, which would not convert to text.
        while (!buffer.text.empty() && ((buffer.text.back() & 0xC0) == 0x80)) {
            buffer.text.pop_back();
        }
        if (!buffer.text.empty() && (buffer.text.back() & 0x80)) {
            buffer.text.pop_back();
        }
    }
    if (cut) {
        *cut = buffer.cut;
    }

    return std::move(buffer.text);
}

std::string
layerFileOf(const std::string& layerIdentifier)
{
    if (layerIdentifier.empty() || PXR_NS::SdfLayer::IsAnonymousLayerIdentifier(layerIdentifier)) {
        return std::string();
    }

    return PXR_NS::SdfLayer::SplitIdentifier(layerIdentifier).first;
}

std::vector<std::string>
getAttributeNames(const SceneStage& stage,
                  const std::string& primPath)
{
    std::vector<std::string> names;

    if (const PXR_NS::UsdPrim prim = primAt(stage, primPath)) {
        for (const PXR_NS::UsdAttribute& attribute : prim.GetAttributes()) {
            names.push_back(attribute.GetName().GetString());
        }
    }

    return names;
}

bool
readAttribute(const SceneStage& stage,
              const std::string& primPath,
              const std::string& name,
              double frame,
              double framesPerSecond,
              ScenePrimAttribute* attribute)
{
    const PXR_NS::UsdPrim prim = primAt(stage, primPath);
    const PXR_NS::UsdAttribute usdAttribute = prim ? prim.GetAttribute(PXR_NS::TfToken(name)) : PXR_NS::UsdAttribute();

    if (!usdAttribute.IsDefined()) {
        return false;
    }
    const PXR_NS::UsdTimeCode time(frameToTimeCode(stage.getPayload(), frame, framesPerSecond));

    attribute->typeName = usdAttribute.GetTypeName().GetAsToken().GetString();
    usdAttribute.Get(&attribute->value, time);
    attribute->source = valueSourceOf(usdAttribute.GetResolveInfo(time).GetSource());
    attribute->author.node = authoringNodeOf(usdAttribute, time, &attribute->author.layerIdentifier);
    attribute->author.schemaFallback = (attribute->source == eSceneValueSourceFallback);

    return true;
}

std::vector<std::string>
getRelationshipNames(const SceneStage& stage,
                     const std::string& primPath)
{
    std::vector<std::string> names;

    if (const PXR_NS::UsdPrim prim = primAt(stage, primPath)) {
        for (const PXR_NS::UsdRelationship& relationship : prim.GetRelationships()) {
            names.push_back(relationship.GetName().GetString());
        }
    }

    return names;
}

bool
readRelationship(const SceneStage& stage,
                 const std::string& primPath,
                 const std::string& name,
                 ScenePrimRelationship* relationship)
{
    const PXR_NS::UsdPrim prim = primAt(stage, primPath);
    const PXR_NS::UsdRelationship usdRelationship = prim ? prim.GetRelationship(PXR_NS::TfToken(name)) : PXR_NS::UsdRelationship();

    if (!usdRelationship.IsDefined()) {
        return false;
    }
    PXR_NS::SdfPathVector targets;
    usdRelationship.GetTargets(&targets);
    for (const PXR_NS::SdfPath& target : targets) {
        relationship->targets.push_back(target.GetString());
    }
    relationship->author = authorOfLayer(targetsLayer(usdRelationship), true);

    return true;
}

bool
readMetadata(const SceneStage& stage,
             const std::string& primPath,
             ScenePrimMetadata* metadata)
{
    const PXR_NS::UsdPrim prim = primAt(stage, primPath);

    if (!prim) {
        return false;
    }
    const PXR_NS::UsdMetadataValueMap all = prim.GetAllMetadata();
    const PXR_NS::SdfPrimSpecHandleVector stack = prim.GetPrimStack();
    // A prototype has no prim index, so its empty stack says nothing of who authored its metadata.
    const bool schemaFallback = !prim.IsPrototype();

    metadata->fields.reserve(all.size());
    for (const auto& item : all) {
        metadata->fields.push_back({ item.first.GetString(), item.second, authorOfLayer(metadataLayer(stack, item.first), schemaFallback) });
    }
    metadata->isPrototype = prim.IsPrototype();
    if (prim.IsInstance()) {
        metadata->prototypePath = prim.GetPrototype().GetPath().GetString();
    } else if (prim.IsInstanceProxy()) {
        metadata->prototypePath = prim.GetPrimInPrototype().GetPath().GetString();
    }

    return true;
}

bool
getPrimComposition(const SceneStage& stage,
                   const std::string& primPath,
                   ScenePrimComposition* composition)
{
    const PXR_NS::UsdPrim prim = primAt(stage, primPath);

    if (!prim) {
        return false;
    }
    *composition = getPrimComposition(prim);

    return true;
}

bool
getPrimOpinions(const SceneStage& stage,
                const std::string& primPath,
                std::vector<SceneOpinion>* opinions)
{
    const PXR_NS::UsdPrim prim = primAt(stage, primPath);

    if (!prim) {
        return false;
    }
    *opinions = getPrimOpinions(prim);

    return true;
}

std::vector<SceneOpinion>
getPropertyOpinions(const SceneStage& stage,
                    const std::string& primPath,
                    const std::string& name,
                    double frame,
                    double framesPerSecond)
{
    const PXR_NS::UsdTimeCode time(frameToTimeCode(stage.getPayload(), frame, framesPerSecond));

    return getPropertyOpinions(primAt(stage, primPath), PXR_NS::TfToken(name), time);
}

PXR_NS::UsdStageRefPtr
beginPrimDefinition(const SceneStage& stage,
                    const std::string& primPath,
                    std::vector<std::string>* propertyNames)
{
    const PXR_NS::UsdPrim prim = primAt(stage, primPath);

    if (!prim) {
        return PXR_NS::UsdStageRefPtr();
    }
    for (const PXR_NS::UsdProperty& property : prim.GetAuthoredProperties()) {
        propertyNames->push_back(property.GetName().GetString());
    }

    return beginPrimDefinition(prim);
}

void
flattenPropertyInto(const PXR_NS::UsdStageRefPtr& definition,
                    const SceneStage& stage,
                    const std::string& primPath,
                    const std::string& name)
{
    if (const PXR_NS::UsdPrim prim = primAt(stage, primPath)) {
        flattenPropertyInto(definition, prim.GetProperty(PXR_NS::TfToken(name)));
    }
}

std::vector<const void*>
getLayerIdentities(const std::vector<PXR_NS::SdfLayerRefPtr>& layers)
{
    std::vector<const void*> identities;

    identities.reserve(layers.size());
    for (const PXR_NS::SdfLayerRefPtr& layer : layers) {
        identities.push_back(get_pointer(layer));
    }

    return identities;
}

namespace {
    bool
    sameLoadSet(const SceneLoadSetPtr& a,
                const SceneLoadSetPtr& b)
    {
        const bool aEverything = !a || a->getEntries().empty();
        const bool bEverything = !b || b->getEntries().empty();

        if (aEverything || bEverything) {
            return aEverything == bEverything;
        }

        return *a == *b;
    }

    bool
    sameLinks(const std::vector<InputLink>& a,
              const std::vector<InputLink>& b)
    {
        if (a.size() != b.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i) {
            if ((a[i].property != b[i].property) || (a[i].targets != b[i].targets) || (a[i].isConnection != b[i].isConnection) || (a[i].custom != b[i].custom) || (a[i].variability != b[i].variability) || (a[i].typeName != b[i].typeName)) {
                return false;
            }
        }

        return true;
    }

    // The defined, non-abstract root prims of input's stage composed with loadSet, and the
    // properties under them its link index lists whose composed targets leave their root. Pure
    // overs define nothing to graft, and classes stay where the input's inherits expect them.
    // Adds to *reach the paths whose loading can change what was read.
    bool
    readInput(const ScenePayload& input,
              const SceneLoadSetPtr& loadSet,
              std::vector<std::string>* names,
              std::vector<InputLink>* links,
              std::vector<std::string>* reach,
              std::string* errorMessage)
    {
        if (input.isEmpty()) {
            return true;
        }
        SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(input, loadSet, errorMessage);
        if (!lease.isValid()) {
            return false;
        }
        const PXR_NS::UsdStageRefPtr stage = lease.getStage();
        for (const PXR_NS::UsdPrim& root : stage->GetPseudoRoot().GetAllChildren()) {
            if (root.IsDefined() && !root.IsAbstract()) {
                names->push_back(root.GetName().GetString());
            } else if (!root.IsDefined() && root.HasAuthoredPayloads() && !root.IsLoaded()) {
                // An over's specifier can come from its payload, which may define it.
                reach->push_back(root.GetPath().GetString());
            }
        }

        const std::set<std::string> roots(names->begin(), names->end());
        // A payload on the way to a link's prim can hold the prim or opinions on its targets; the
        // link is read only as far as the load set includes them.
        std::vector<std::string> linkPrims;
        for (const PXR_NS::SdfPath& property : getLinks(input)) {
            const PXR_NS::SdfPath root = rootPrimOf(property);
            if (root.IsEmpty() || !roots.count(root.GetName())) {
                continue;
            }
            linkPrims.push_back(property.GetPrimPath().GetString());
            const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(property.GetPrimPath());
            if (!prim) {
                continue;
            }
            InputLink link;
            link.property = property;
            const PXR_NS::UsdProperty composed = prim.GetProperty(property.GetNameToken());
            if (composed.Is<PXR_NS::UsdRelationship>()) {
                const PXR_NS::UsdRelationship relationship = composed.As<PXR_NS::UsdRelationship>();
                relationship.GetTargets(&link.targets);
                link.custom = relationship.IsCustom();
            } else if (composed.Is<PXR_NS::UsdAttribute>()) {
                const PXR_NS::UsdAttribute attribute = composed.As<PXR_NS::UsdAttribute>();
                attribute.GetConnections(&link.targets);
                link.isConnection = true;
                link.custom = attribute.IsCustom();
                link.variability = attribute.GetVariability();
                link.typeName = attribute.GetTypeName();
            } else {
                continue;
            }
            if (leavesRoot(property, link.targets)) {
                links->push_back(link);
            }
        }
        const std::vector<std::string> linkReach = getLoadReach(stage, linkPrims);
        reach->insert(reach->end(), linkReach.begin(), linkReach.end());

        return true;
    }

    bool
    isPassedThrough(const GraftSlot& slot,
                    std::size_t rootIndex)
    {
        return !slot.passThroughRoot.empty() && (slot.rootNames[rootIndex] == slot.passThroughRoot);
    }

    PXR_NS::SdfPath
    graftedRootOf(const GraftSlot& slot,
                  const PXR_NS::SdfPath& destination,
                  std::size_t rootIndex)
    {
        if (isPassedThrough(slot, rootIndex)) {
            return PXR_NS::SdfPath::AbsoluteRootPath().AppendChild(PXR_NS::TfToken(slot.rootNames[rootIndex]));
        }

        return destination.AppendChild(PXR_NS::TfToken(slot.childNames[rootIndex]));
    }

    // Whether the root prim name of input's stage composed with loadSet is or holds a gprim.
    // Adds the root to *reach: loading or unloading a payload under it can change the answer.
    bool
    rootHoldsGprim(const ScenePayload& input,
                   const SceneLoadSetPtr& loadSet,
                   const std::string& name,
                   bool* holdsGprim,
                   std::vector<std::string>* reach,
                   std::string* errorMessage)
    {
        *holdsGprim = false;
        reach->push_back("/" + name);
        SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(input, loadSet, errorMessage);
        if (!lease.isValid()) {
            return false;
        }
        const PXR_NS::UsdPrim root = lease.getStage()->GetPrimAtPath(PXR_NS::SdfPath::AbsoluteRootPath().AppendChild(PXR_NS::TfToken(name)));
        if (!root) {
            return true;
        }
        if (root.IsA<PXR_NS::UsdGeomGprim>()) {
            *holdsGprim = true;

            return true;
        }
        for (const PXR_NS::UsdPrim& prim : root.GetDescendants()) {
            if (prim.IsA<PXR_NS::UsdGeomGprim>()) {
                *holdsGprim = true;
                break;
            }
        }

        return true;
    }

    // Where path lies once slot's roots are grafted under destination; the empty path when it lies
    // under none of them.
    PXR_NS::SdfPath
    graftedPathOf(const GraftSlot& slot,
                  const PXR_NS::SdfPath& destination,
                  const PXR_NS::SdfPath& path)
    {
        const PXR_NS::SdfPath root = rootPrimOf(path);

        if (root.IsEmpty()) {
            return root;
        }
        for (std::size_t i = 0; i < slot.rootNames.size(); ++i) {
            if (slot.rootNames[i] == root.GetName()) {
                return path.ReplacePrefix(root, graftedRootOf(slot, destination, i));
            }
        }

        return PXR_NS::SdfPath();
    }

    // Copies onto layer the stage metadata input's layers author, each field from the strongest
    // layer authoring it. Fields no layer authors stay unauthored, so a layer stack composing layer
    // falls back for them as it would for input's own layers: an unauthored timeCodesPerSecond
    // takes the composing stack's rate, and an unauthored upAxis does not override a stronger one
    // merged in later.
    void
    carryAuthoredMetadata(const ScenePayload& input,
                          const PXR_NS::SdfLayerRefPtr& layer)
    {
        const PXR_NS::SdfPath& root = PXR_NS::SdfPath::AbsoluteRootPath();
        bool hasUpAxis = false;
        bool hasMetersPerUnit = false;
        bool hasTimeCodesPerSecond = false;
        const std::vector<SceneLayerEntry> layers = getLayers(input);

        for (std::vector<SceneLayerEntry>::const_reverse_iterator it = layers.rbegin(); it != layers.rend(); ++it) {
            const PXR_NS::SdfLayerRefPtr& source = it->layer;
            PXR_NS::VtValue value;
            if (!hasUpAxis && source->HasField(root, PXR_NS::UsdGeomTokens->upAxis, &value)) {
                layer->SetField(root, PXR_NS::UsdGeomTokens->upAxis, value);
                hasUpAxis = true;
            }
            if (!hasMetersPerUnit && source->HasField(root, PXR_NS::UsdGeomTokens->metersPerUnit, &value)) {
                layer->SetField(root, PXR_NS::UsdGeomTokens->metersPerUnit, value);
                hasMetersPerUnit = true;
            }
            if (!hasTimeCodesPerSecond && source->HasTimeCodesPerSecond()) {
                layer->SetTimeCodesPerSecond(source->GetTimeCodesPerSecond());
                hasTimeCodesPerSecond = true;
            }
        }
    }

    // The root layer of the layer stack a graft layer references: it sublayers input's stack,
    // strongest first, as a stage composing input on its own would.
    PXR_NS::SdfLayerRefPtr
    createWrapper(const ScenePayload& input,
                  const std::string& tag)
    {
        PXR_NS::SdfLayerRefPtr wrapper = PXR_NS::SdfLayer::CreateAnonymous(tag + "-wrapper");

        if (!wrapper) {
            return wrapper;
        }
        wrapper->SetSubLayerPaths(getSubLayerPaths(input));
        carryAuthoredMetadata(input, wrapper);
        wrapper->SetPermissionToEdit(false);

        return wrapper;
    }

    PXR_NS::SdfLayerRefPtr
    authorStructureLayer(const PXR_NS::SdfPath& destination,
                         const std::string& destinationType,
                         const std::string& ancestorType,
                         const std::string& tag)
    {
        PXR_NS::SdfLayerRefPtr layer = createNodeLayer(tag + "-structure");

        if (!layer) {
            return layer;
        }
        for (const PXR_NS::SdfPath& prefix : destination.GetPrefixes()) {
            const PXR_NS::SdfPrimSpecHandle spec = PXR_NS::SdfCreatePrimInLayer(layer, prefix);
            if (!spec) {
                return PXR_NS::SdfLayerRefPtr();
            }
            spec->SetSpecifier(PXR_NS::SdfSpecifierDef);
            const std::string& typeName = (prefix == destination) ? destinationType : ancestorType;
            if (!typeName.empty()) {
                spec->SetTypeName(typeName);
            }
        }
        layer->SetPermissionToEdit(false);

        return layer;
    }

    // Defines destination/<child name> for each of slot's roots, or /<root> for a passed-through
    // one, referencing the root in slot's wrapper. Everything above the children is an "over": the
    // structure layer defines it, and an over sent or withdrawn with this layer changes no prim's
    // definition, so swapping this layer resyncs only the grafted roots.
    PXR_NS::SdfLayerRefPtr
    authorGraftLayer(const GraftSlot& slot,
                     const PXR_NS::SdfPath& destination,
                     const std::string& tag)
    {
        PXR_NS::SdfLayerRefPtr layer = createNodeLayer(tag + "-graft");

        if (!layer) {
            return layer;
        }
        // Graft layers sit in the consumer's stack, which scales each one's time samples by its
        // timeCodesPerSecond against the stage's; carrying the input's keeps the wrapper's samples
        // at the rate they were authored for.
        carryAuthoredMetadata(slot.input, layer);
        const std::string& wrapperIdentifier = slot.wrapper->GetIdentifier();
        for (std::size_t i = 0; i < slot.rootNames.size(); ++i) {
            const PXR_NS::SdfPrimSpecHandle child = PXR_NS::SdfCreatePrimInLayer(layer, graftedRootOf(slot, destination, i));
            if (!child) {
                return PXR_NS::SdfLayerRefPtr();
            }
            child->SetSpecifier(PXR_NS::SdfSpecifierDef);
            child->GetReferenceList().Prepend(PXR_NS::SdfReference(wrapperIdentifier, PXR_NS::SdfPath::AbsoluteRootPath().AppendChild(PXR_NS::TfToken(slot.rootNames[i]))));
        }
        // The reference drops the targets that leave the root; an explicit list here, stronger
        // than the reference, stands in for its whole list.
        for (const InputLink& link : slot.links) {
            const PXR_NS::SdfPath property = graftedPathOf(slot, destination, link.property);
            const PXR_NS::SdfPrimSpecHandle owner = property.IsEmpty() ? PXR_NS::SdfPrimSpecHandle() : PXR_NS::SdfCreatePrimInLayer(layer, property.GetPrimPath());
            if (!owner) {
                return PXR_NS::SdfLayerRefPtr();
            }
            std::vector<PXR_NS::SdfPath> targets;
            for (const PXR_NS::SdfPath& target : link.targets) {
                const PXR_NS::SdfPath grafted = graftedPathOf(slot, destination, target);
                if (!grafted.IsEmpty()) {
                    targets.push_back(grafted);
                }
            }
            if (link.isConnection) {
                if (!PXR_NS::SdfAttributeSpec::New(owner, property.GetName(), link.typeName, link.variability, link.custom)) {
                    return PXR_NS::SdfLayerRefPtr();
                }
                layer->SetField(property, PXR_NS::SdfFieldKeys->ConnectionPaths, PXR_NS::SdfPathListOp::CreateExplicit(targets));
            } else {
                if (!PXR_NS::SdfRelationshipSpec::New(owner, property.GetName(), link.custom)) {
                    return PXR_NS::SdfLayerRefPtr();
                }
                layer->SetField(property, PXR_NS::SdfFieldKeys->TargetPaths, PXR_NS::SdfPathListOp::CreateExplicit(targets));
            }
        }
        layer->SetPermissionToEdit(false);

        return layer;
    }

    // Whether layer authors nothing but "over" prim specs.
    bool
    authorsOnlyOvers(const PXR_NS::SdfLayerRefPtr& layer)
    {
        bool onlyOvers = true;

        layer->Traverse(PXR_NS::SdfPath::AbsoluteRootPath(), [&layer, &onlyOvers](const PXR_NS::SdfPath& path) {
            if (onlyOvers && path.IsPrimPath()) {
                const PXR_NS::SdfPrimSpecHandle spec = layer->GetPrimAtPath(path);
                onlyOvers = !spec || (spec->GetSpecifier() == PXR_NS::SdfSpecifierOver);
            }
        });

        return onlyOvers;
    }

    // The first slot for input not yet consumed whose reads were made on *loadSet, when set, and
    // whose graft layer is one for graft's destination, pass-through root, names and links, when
    // set. With consumed, marks it consumed, so two inputs connected to the same stack each keep
    // a slot of their own.
    const GraftSlot*
    findSlot(const std::vector<GraftSlot>& slots,
             std::vector<bool>* consumed,
             const ScenePayload& input,
             const SceneLoadSetPtr* loadSet,
             const GraftSlot* graft)
    {
        for (std::size_t i = 0; i < slots.size(); ++i) {
            const GraftSlot& slot = slots[i];
            if ((consumed && (*consumed)[i]) || !slot.input.isSameStack(input)) {
                continue;
            }
            if (loadSet && !sameLoadSet(slot.loadSet, *loadSet)) {
                continue;
            }
            if (graft && (!slot.graft.layer || (slot.destinationPath != graft->destinationPath) || (slot.passThroughRoot != graft->passThroughRoot) || (slot.childNames != graft->childNames) || !sameLinks(slot.links, graft->links))) {
                continue;
            }
            if (consumed) {
                (*consumed)[i] = true;
            }

            return &slot;
        }

        return NULL;
    }

    // findSlot() in the slots of context, then, without consuming, in those of the other
    // contexts: what another evaluation read or authored for the same stack is the same.
    const GraftSlot*
    findSlotShared(const SceneGraftMemo::State& state,
                   const GraftContext* context,
                   std::vector<bool>* consumed,
                   const ScenePayload& input,
                   const SceneLoadSetPtr* loadSet,
                   const GraftSlot* graft)
    {
        if (context) {
            if (const GraftSlot* found = findSlot(context->slots, consumed, input, loadSet, graft)) {
                return found;
            }
        }
        for (const GraftContext& other : state.contexts) {
            if (&other == context) {
                continue;
            }
            if (const GraftSlot* found = findSlot(other.slots, NULL, input, loadSet, graft)) {
                return found;
            }
        }

        return NULL;
    }
} // anonymous namespace

bool
reparentRoots(const SceneReparentArgs& args,
              SceneGraftMemo* memo,
              ScenePayload* output,
              std::vector<SceneRootGraft>* grafts,
              std::vector<SceneUnmappedLink>* unmappedLinks,
              std::string* errorMessage)
{
    if (!memo || !output) {
        return false;
    }
    if (!checkUsdRuntime(errorMessage)) {
        return false;
    }
    if (!PXR_NS::SdfPath::IsValidPathString(args.destinationPath)) {
        if (errorMessage) {
            *errorMessage = "\"" + args.destinationPath + "\" is not a valid prim path.";
        }

        return false;
    }
    const PXR_NS::SdfPath destination(args.destinationPath);
    if (!destination.IsAbsolutePath() || !destination.IsPrimPath()) {
        if (errorMessage) {
            *errorMessage = "\"" + args.destinationPath + "\" is not an absolute prim path.";
        }

        return false;
    }

    // Reading the inputs' stages and layers: no layer reload may run meanwhile. Taken before the
    // memo's mutex, so a thread waiting on the mutex holds no write off.
    SceneWriteBarrier::ReadScope read;
    SceneGraftMemo::State& state = memo->getState();
    std::lock_guard<std::mutex> lock(state.mutex);
    const SceneEvaluationContext& context = SceneEvaluationContext::current();
    const GraftContext* memoized = state.find(context.id);
    const std::size_t memoizedSlots = memoized ? memoized->slots.size() : 0;

    // An input seen on the same load set keeps its root names and links, and one seen at all its
    // wrapper: the same stack composes the same.
    std::vector<GraftSlot> slots(args.inputs.size());
    std::vector<bool> readConsumed(memoizedSlots, false);
    // Where loads change each input's reads, on the paths of the context it was read on.
    std::vector<std::vector<std::string>> reaches(args.inputs.size());
    for (std::size_t i = 0; i < args.inputs.size(); ++i) {
        GraftSlot& slot = slots[i];
        const SceneEvaluationContext& inputContext = (i < args.inputContexts.size()) ? args.inputContexts[i] : context;
        slot.input = args.inputs[i];
        slot.destinationPath = args.destinationPath;
        slot.passThroughRoot = args.passThroughRoot;
        slot.loadSet = inputContext.loadSet;
        bool gprimChecked = false;
        if (const GraftSlot* previous = findSlotShared(state, memoized, &readConsumed, slot.input, &inputContext.loadSet, NULL)) {
            slot.rootNames = previous->rootNames;
            slot.links = previous->links;
            slot.wrapper = previous->wrapper;
            slot.readReach = previous->readReach;
            if (previous->passThroughRoot == slot.passThroughRoot) {
                slot.passThroughHoldsGprim = previous->passThroughHoldsGprim;
                slot.passThroughReach = previous->passThroughReach;
                gprimChecked = true;
            }
        } else {
            if (const GraftSlot* other = findSlotShared(state, memoized, NULL, slot.input, NULL, NULL)) {
                slot.wrapper = other->wrapper;
            }
            if (!readInput(slot.input, inputContext.loadSet, &slot.rootNames, &slot.links, &slot.readReach, errorMessage)) {
                return false;
            }
            ++state.inputReadCount;
        }
        if (!gprimChecked && !slot.passThroughRoot.empty() && (std::find(slot.rootNames.begin(), slot.rootNames.end(), slot.passThroughRoot) != slot.rootNames.end()) && !rootHoldsGprim(slot.input, inputContext.loadSet, slot.passThroughRoot, &slot.passThroughHoldsGprim, &slot.passThroughReach, errorMessage)) {
            return false;
        }
        reaches[i].insert(reaches[i].end(), slot.readReach.begin(), slot.readReach.end());
        reaches[i].insert(reaches[i].end(), slot.passThroughReach.begin(), slot.passThroughReach.end());
        if (destination.IsRootPrimPath()) {
            for (const std::string& root : slot.rootNames) {
                if ((root == destination.GetName()) && (root != slot.passThroughRoot)) {
                    if (errorMessage) {
                        *errorMessage = "Input " + std::to_string(i + 1) + " has a root prim at " + args.destinationPath + ", the path its roots are grafted under: it would be grafted inside itself.";
                    }

                    return false;
                }
            }
        }
    }

    // Names in input order: with autoRename, a root whose name an earlier input took gets the
    // first "_<n>" suffix free of every name taken so far and of its own input's root names. A
    // passed-through root is never renamed: every input's composes at its own path.
    std::set<std::string> taken;
    for (GraftSlot& slot : slots) {
        const std::set<std::string> own(slot.rootNames.begin(), slot.rootNames.end());
        for (const std::string& root : slot.rootNames) {
            std::string name = root;
            if (root == slot.passThroughRoot) {
                slot.childNames.push_back(name);
                continue;
            }
            if (args.autoRename && taken.count(name)) {
                for (U64 n = 1;; ++n) {
                    const std::string candidate = root + "_" + std::to_string(n);
                    if (!taken.count(candidate) && !own.count(candidate)) {
                        name = candidate;
                        break;
                    }
                }
            }
            slot.childNames.push_back(name);
            taken.insert(name);
        }
    }

    SceneLayerEntry structure = state.structure;
    const bool structureChanged = !structure.layer || (state.structurePath != args.destinationPath) || (state.structureDestinationType != args.destinationType) || (state.structureAncestorType != args.ancestorType);
    if (structureChanged) {
        structure = SceneLayerEntry();
        structure.layer = authorStructureLayer(destination, args.destinationType, args.ancestorType, args.layerTag);
        if (!structure.layer) {
            if (errorMessage) {
                *errorMessage = "Could not author the prim " + args.destinationPath + ".";
            }

            return false;
        }
        ContentHasher h;
        h.appendString("structure");
        h.appendString(args.destinationPath);
        h.appendString(args.destinationType);
        h.appendString(args.ancestorType);
        structure.hash = h.finalize();
    }

    U64 graftLayersAuthored = 0;
    std::vector<bool> graftConsumed(memoizedSlots, false);
    for (GraftSlot& slot : slots) {
        if (slot.rootNames.empty()) {
            continue;
        }
        if (const GraftSlot* previous = findSlotShared(state, memoized, &graftConsumed, slot.input, NULL, &slot)) {
            slot.wrapper = previous->wrapper;
            slot.graft = previous->graft;
            continue;
        }
        if (!slot.wrapper) {
            slot.wrapper = createWrapper(slot.input, args.layerTag);
        }
        PXR_NS::SdfLayerRefPtr layer = slot.wrapper ? authorGraftLayer(slot, destination, args.layerTag) : PXR_NS::SdfLayerRefPtr();
        if (!layer) {
            if (errorMessage) {
                *errorMessage = "Could not author the grafts under " + args.destinationPath + ".";
            }

            return false;
        }
        ++graftLayersAuthored;

        std::shared_ptr<SceneLayerDependencies> dependencies = std::make_shared<SceneLayerDependencies>();
        dependencies->layers.push_back(slot.wrapper);
        dependencies->stacks.push_back(slot.input);

        ContentHasher h;
        h.appendString("graft");
        h.appendU64(slot.input.getStackHash());
        h.appendString(slot.destinationPath);
        h.appendString(slot.passThroughRoot);
        for (std::size_t i = 0; i < slot.rootNames.size(); ++i) {
            h.appendString(slot.rootNames[i]);
            h.appendString(slot.childNames[i]);
        }
        // The links read depend on the load set: two evaluations reading different ones author
        // different layers on the same stack.
        h.appendU64(slot.links.size());
        for (const InputLink& link : slot.links) {
            h.appendString(link.property.GetString());
            h.appendU64(link.targets.size());
            for (const PXR_NS::SdfPath& target : link.targets) {
                h.appendString(target.GetString());
            }
        }
        slot.graft.layer = layer;
        slot.graft.hash = h.finalize();
        slot.graft.dependencies = dependencies;
    }

    // Last, as the overrides memo commits what it authors: everything that can fail before it has.
    SceneLayerEntry overrides;
    if (args.authorOverrides) {
        SceneLayerMemo::Key key = args.overridesKey;
        key.upstreamRead = SceneLayerMemo::Key::eUpstreamReadNothing;
        const SceneLayerMemo::AuthorFunc& author = args.authorOverrides;
        const SceneLayerMemo::AuthorFunc checkedAuthor = [&author, &args](const SceneLayerEdit& edit,
                                                                          std::string* authorError) {
            if (!author(edit, authorError)) {
                return false;
            }
            // A def or class here would make every swap of this layer resync the whole graft.
            if (!authorsOnlyOvers(edit.layer)) {
                if (authorError) {
                    *authorError = "The overrides of " + args.destinationPath + " may only author \"over\" prims.";
                }

                return false;
            }

            return true;
        };
        ScenePayload produced;
        if (!state.overrides.produce(key, ScenePayload(), args.layerTag + "-overrides", checkedAuthor, &produced, errorMessage)) {
            return false;
        }
        overrides = produced.getImpl()->top;
    }

    // A payload of its own: the inputs are reached only through the graft layers' references.
    ScenePayload result = appendEntry(ScenePayload(), structure);
    if (overrides.layer) {
        result = appendEntry(result, overrides);
    }
    for (const GraftSlot& slot : slots) {
        if (slot.graft.layer) {
            result = appendEntry(result, slot.graft);
        }
    }
    for (const GraftContext& other : state.contexts) {
        if (result.isSameStack(other.output)) {
            result = other.output;
            break;
        }
    }

    state.structurePath = args.destinationPath;
    state.structureDestinationType = args.destinationType;
    state.structureAncestorType = args.ancestorType;
    state.structure = structure;
    // memoized is not read past here: use() may move or evict entries.
    GraftContext& committed = state.use(context.id);
    committed.slots.swap(slots);
    committed.output = result;
    state.graftLayerAuthorCount += graftLayersAuthored;
    for (std::size_t i = 0; i < reaches.size(); ++i) {
        const SceneEvaluationContext& inputContext = (i < args.inputContexts.size()) ? args.inputContexts[i] : context;
        if (inputContext.reach && !reaches[i].empty()) {
            inputContext.reach->add(reaches[i]);
        }
    }

    if (grafts) {
        grafts->clear();
        for (std::size_t i = 0; i < committed.slots.size(); ++i) {
            const GraftSlot& slot = committed.slots[i];
            for (std::size_t j = 0; j < slot.rootNames.size(); ++j) {
                SceneRootGraft graft;
                graft.inputIndex = i;
                graft.sourcePath = "/" + slot.rootNames[j];
                graft.graftedPath = graftedRootOf(slot, destination, j).GetString();
                graft.passedThrough = isPassedThrough(slot, j);
                graft.holdsGprim = graft.passedThrough && slot.passThroughHoldsGprim;
                grafts->push_back(graft);
            }
        }
    }
    if (unmappedLinks) {
        unmappedLinks->clear();
        for (std::size_t i = 0; i < committed.slots.size(); ++i) {
            const GraftSlot& slot = committed.slots[i];
            for (const InputLink& link : slot.links) {
                for (const PXR_NS::SdfPath& target : link.targets) {
                    if (graftedPathOf(slot, destination, target).IsEmpty()) {
                        SceneUnmappedLink unmapped;
                        unmapped.inputIndex = i;
                        unmapped.propertyPath = graftedPathOf(slot, destination, link.property).GetString();
                        unmapped.targetPath = target.GetString();
                        unmappedLinks->push_back(unmapped);
                    }
                }
            }
        }
    }
    *output = result;

    return true;
} // reparentRoots

namespace {
    // path as the absolute root or an absolute prim path; the empty path when it is neither.
    PXR_NS::SdfPath
    loadSetPath(const std::string& path)
    {
        if (!PXR_NS::SdfPath::IsValidPathString(path)) {
            return PXR_NS::SdfPath();
        }
        const PXR_NS::SdfPath parsed(path);

        return (parsed.IsAbsoluteRootPath() || (parsed.IsAbsolutePath() && parsed.IsPrimPath())) ? parsed : PXR_NS::SdfPath();
    }

    // Whether every entry of loadSet is a valid path loadSetPath() accepts.
    bool
    hasValidPaths(const SceneLoadSet& loadSet)
    {
        for (const SceneLoadSet::Entry& entry : loadSet.getEntries()) {
            if (loadSetPath(entry.path).IsEmpty()) {
                return false;
            }
        }

        return true;
    }

    // The rules of loadSet strictly below from, moved below to.
    void
    appendMovedBelow(const SceneLoadSet& loadSet,
                     const PXR_NS::SdfPath& from,
                     const PXR_NS::SdfPath& to,
                     std::vector<SceneLoadSet::Entry>* entries)
    {
        for (const SceneLoadSet::Entry& entry : loadSet.getEntries()) {
            const PXR_NS::SdfPath path(entry.path);
            if ((path != from) && path.HasPrefix(from)) {
                entries->push_back(SceneLoadSet::Entry { path.ReplacePrefix(from, to).GetString(), entry.rule });
            }
        }
    }

    // What loadSet includes at and below from, at and below to.
    void
    appendMovedSubtree(const SceneLoadSet& loadSet,
                       const PXR_NS::SdfPath& from,
                       const PXR_NS::SdfPath& to,
                       std::vector<SceneLoadSet::Entry>* entries)
    {
        entries->push_back(SceneLoadSet::Entry { to.GetString(), loadSet.getRuleAt(from.GetString()) });
        appendMovedBelow(loadSet, from, to, entries);
    }

    // entries as a load set, null when it includes everything. Redundant entries are dropped so
    // that a mapped set equals, and hashes as, the same set written directly.
    SceneLoadSetPtr
    makeMappedLoadSet(const std::vector<SceneLoadSet::Entry>& entries)
    {
        return SceneLoadSet::minimal(entries);
    }

    void
    addOnce(const std::string& path,
            std::vector<std::string>* out)
    {
        if (std::find(out->begin(), out->end(), path) == out->end()) {
            out->push_back(path);
        }
    }

    // The root name autoRename would have given the suffix of name to, empty when name has none.
    std::string
    getRenamedStem(const std::string& name)
    {
        const std::string::size_type underscore = name.rfind('_');

        if ((underscore == std::string::npos) || (underscore == 0) || (underscore + 1 == name.size()) || (name[underscore + 1] == '0')) {
            return std::string();
        }
        for (std::string::size_type i = underscore + 1; i < name.size(); ++i) {
            if ((name[i] < '0') || (name[i] > '9')) {
                return std::string();
            }
        }

        return name.substr(0, underscore);
    }
} // anonymous namespace

SceneLoadSetPtr
mapGraftLoadSet(const SceneLoadSetPtr& loadSet,
                const std::string& destinationPath,
                const std::string& passThroughRoot,
                bool autoRename)
{
    if (!loadSet || loadSet->getEntries().empty()) {
        return SceneLoadSetPtr();
    }
    const PXR_NS::SdfPath destination = loadSetPath(destinationPath);
    if (destination.IsEmpty() || destination.IsAbsoluteRootPath() || !hasValidPaths(*loadSet)) {
        return SceneLoadSetPtr();
    }
    // The destination prim is the graft's own: nothing loads at it, and what lies above it holds
    // no input prim. Its rule for what lies below is every root's that no rule under it names.
    const SceneLoadSet::RuleEnum destinationRule = (loadSet->getRuleAt(destinationPath) == SceneLoadSet::eRuleAll) ? SceneLoadSet::eRuleAll : SceneLoadSet::eRuleNone;
    const SceneLoadSet::Entry closed { std::string("/"), SceneLoadSet::eRuleNone };

    // The children of the destination that rules name, by the input root each can be grafted from.
    std::map<std::string, std::set<std::string>> childrenByRoot;
    const std::size_t childDepth = destination.GetPathElementCount() + 1;
    for (const SceneLoadSet::Entry& entry : loadSet->getEntries()) {
        const PXR_NS::SdfPath path(entry.path);
        if ((path.GetPathElementCount() < childDepth) || !path.HasPrefix(destination)) {
            continue;
        }
        const std::string child = path.GetPrefixes()[childDepth - 1].GetName();
        childrenByRoot[child].insert(child);
        const std::string stem = autoRename ? getRenamedStem(child) : std::string();
        if (!stem.empty()) {
            childrenByRoot[stem].insert(child);
        }
    }
    childrenByRoot.erase(passThroughRoot);

    std::vector<SceneLoadSet::Entry> entries(1, SceneLoadSet::Entry { std::string("/"), destinationRule });
    for (const std::pair<const std::string, std::set<std::string>>& root : childrenByRoot) {
        const PXR_NS::SdfPath rootPath = PXR_NS::SdfPath::AbsoluteRootPath().AppendChild(PXR_NS::TfToken(root.first));
        std::vector<SceneLoadSetPtr> candidates;
        for (const std::string& child : root.second) {
            std::vector<SceneLoadSet::Entry> candidate(1, closed);
            appendMovedSubtree(*loadSet, destination.AppendChild(PXR_NS::TfToken(child)), rootPath, &candidate);
            candidates.push_back(std::make_shared<const SceneLoadSet>(candidate));
        }
        if (autoRename) {
            // The root may as well have landed under a suffix no rule names.
            candidates.push_back(std::make_shared<const SceneLoadSet>(std::vector<SceneLoadSet::Entry> { closed, SceneLoadSet::Entry { rootPath.GetString(), destinationRule } }));
        }
        SceneLoadSetPtr united = candidates.front();
        for (std::size_t i = 1; i < candidates.size(); ++i) {
            united = SceneLoadSet::unite(united, candidates[i]);
        }
        if (!united) {
            return SceneLoadSetPtr();
        }
        entries.push_back(SceneLoadSet::Entry { rootPath.GetString(), united->getRuleAt(rootPath.GetString()) });
        appendMovedBelow(*united, rootPath, rootPath, &entries);
    }
    if (!passThroughRoot.empty() && PXR_NS::SdfPath::IsValidIdentifier(passThroughRoot)) {
        const PXR_NS::SdfPath passed = PXR_NS::SdfPath::AbsoluteRootPath().AppendChild(PXR_NS::TfToken(passThroughRoot));
        appendMovedSubtree(*loadSet, passed, passed, &entries);
    }

    return makeMappedLoadSet(entries);
} // mapGraftLoadSet

void
mapGraftReach(SceneGraftMemo* memo,
              std::size_t inputIndex,
              const std::string& destinationPath,
              const std::string& passThroughRoot,
              const std::vector<std::string>& in,
              std::vector<std::string>* out)
{
    out->clear();
    if (in.empty()) {
        return;
    }
    const PXR_NS::SdfPath destination = loadSetPath(destinationPath);
    if (destination.IsEmpty() || destination.IsAbsoluteRootPath()) {
        out->push_back("/");

        return;
    }
    std::vector<std::string> rootNames;
    std::vector<std::string> childNames;
    if (memo) {
        SceneGraftMemo::State& state = memo->getState();
        std::lock_guard<std::mutex> lock(state.mutex);
        const GraftContext* grafted = state.find(SceneEvaluationContext::current().id);
        if (grafted && (inputIndex < grafted->slots.size()) && (grafted->slots[inputIndex].destinationPath == destinationPath)) {
            rootNames = grafted->slots[inputIndex].rootNames;
            childNames = grafted->slots[inputIndex].childNames;
        }
    }

    for (const std::string& prefix : in) {
        const PXR_NS::SdfPath path = loadSetPath(prefix);
        if (path.IsEmpty()) {
            addOnce("/", out);
            continue;
        }
        if (!path.IsAbsoluteRootPath()) {
            const PXR_NS::SdfPath root = path.GetPrefixes().front();
            const std::string name = root.GetName();
            if (name == passThroughRoot) {
                addOnce(prefix, out);
                continue;
            }
            const std::vector<std::string>::const_iterator found = std::find(rootNames.begin(), rootNames.end(), name);
            if (found != rootNames.end()) {
                const PXR_NS::TfToken child(childNames[found - rootNames.begin()]);
                addOnce(path.ReplacePrefix(root, destination.AppendChild(child)).GetString(), out);
                continue;
            }
        }
        addOnce(destinationPath, out);
        if (!passThroughRoot.empty()) {
            addOnce("/" + passThroughRoot, out);
        }
    }
} // mapGraftReach

SceneLoadSetPtr
mapWrapLoadSet(const SceneLoadSetPtr& loadSet,
               const std::string& wrappedPath,
               const std::string& movedPath)
{
    if (!loadSet || loadSet->getEntries().empty()) {
        return SceneLoadSetPtr();
    }
    const PXR_NS::SdfPath wrapped = loadSetPath(wrappedPath);
    const PXR_NS::SdfPath moved = loadSetPath(movedPath);
    if (wrapped.IsEmpty() || wrapped.IsAbsoluteRootPath() || moved.IsEmpty() || moved.IsAbsoluteRootPath() || !hasValidPaths(*loadSet)) {
        return SceneLoadSetPtr();
    }

    // The wrapped prim is deactivated where it was: only its moved copy loads.
    std::vector<SceneLoadSet::Entry> entries;
    for (const SceneLoadSet::Entry& entry : loadSet->getEntries()) {
        const PXR_NS::SdfPath path(entry.path);
        if (!path.HasPrefix(wrapped) && !path.HasPrefix(moved)) {
            entries.push_back(entry);
        }
    }
    appendMovedSubtree(*loadSet, moved, wrapped, &entries);

    return makeMappedLoadSet(entries);
}

void
mapWrapReach(const std::string& wrappedPath,
             const std::string& movedPath,
             const std::vector<std::string>& in,
             std::vector<std::string>* out)
{
    out->clear();
    const PXR_NS::SdfPath wrapped = loadSetPath(wrappedPath);
    const PXR_NS::SdfPath moved = loadSetPath(movedPath);
    for (const std::string& prefix : in) {
        const PXR_NS::SdfPath path = loadSetPath(prefix);
        if (path.IsEmpty() || wrapped.IsEmpty() || moved.IsEmpty()) {
            addOnce("/", out);
        } else if (path.HasPrefix(wrapped)) {
            addOnce(path.ReplacePrefix(wrapped, moved).GetString(), out);
        } else if (wrapped.HasPrefix(path)) {
            addOnce(prefix, out);
            addOnce(movedPath, out);
        } else {
            addOnce(prefix, out);
        }
    }
}

SceneLoadSetPtr
mapReferenceLoadSet(const SceneLoadSetPtr& loadSet,
                    const std::string& targetPath,
                    const std::string& sourcePath)
{
    if (!loadSet || loadSet->getEntries().empty()) {
        return SceneLoadSetPtr();
    }
    const PXR_NS::SdfPath target = loadSetPath(targetPath);
    const PXR_NS::SdfPath source = loadSetPath(sourcePath);
    if (target.IsEmpty() || target.IsAbsoluteRootPath() || source.IsEmpty() || source.IsAbsoluteRootPath() || !hasValidPaths(*loadSet)) {
        return SceneLoadSetPtr();
    }

    std::vector<SceneLoadSet::Entry> entries(1, SceneLoadSet::Entry { std::string("/"), SceneLoadSet::eRuleNone });
    if (loadSet->getRuleAt(targetPath) != SceneLoadSet::eRuleNone) {
        for (PXR_NS::SdfPath ancestor = source.GetParentPath(); !ancestor.IsAbsoluteRootPath(); ancestor = ancestor.GetParentPath()) {
            entries.push_back(SceneLoadSet::Entry { ancestor.GetString(), SceneLoadSet::eRuleOnly });
        }
    }
    appendMovedSubtree(*loadSet, target, source, &entries);

    return makeMappedLoadSet(entries);
}

void
mapReferenceReach(const std::string& targetPath,
                  const std::string& sourcePath,
                  const std::vector<std::string>& in,
                  std::vector<std::string>* out)
{
    out->clear();
    const PXR_NS::SdfPath source = sourcePath.empty() ? PXR_NS::SdfPath() : loadSetPath(sourcePath);
    for (const std::string& prefix : in) {
        const PXR_NS::SdfPath path = loadSetPath(prefix);
        if (source.IsEmpty() || path.IsEmpty()) {
            addOnce(targetPath, out);
        } else if (path.HasPrefix(source)) {
            addOnce(path.ReplacePrefix(source, PXR_NS::SdfPath(targetPath)).GetString(), out);
        } else if (source.HasPrefix(path)) {
            addOnce(targetPath, out);
        }
    }
}

PXR_NS::SdfLayerRefPtr
createStackLayer(const ScenePayload& input,
                 const std::string& tag)
{
    if (input.isEmpty()) {
        return PXR_NS::SdfLayerRefPtr();
    }

    return createWrapper(input, tag);
}

struct UnloadedSearch::State {
    mutable std::mutex mutex;
    mutable std::condition_variable changed;
    StatusEnum status = eStatusRunning;
    std::vector<std::string> matches;
    std::string error;
    std::atomic<bool> cancelled { false };
    std::function<bool()> stopWhen;

    bool shouldStop() const
    {
        return cancelled.load() || (stopWhen && stopWhen());
    }

    void finish(StatusEnum result,
                const std::vector<PXR_NS::SdfPath>& found,
                const std::string& message)
    {
        std::lock_guard<std::mutex> lock(mutex);
        status = result;
        matches.clear();
        for (const PXR_NS::SdfPath& path : found) {
            matches.push_back(path.GetString());
        }
        error = message;
        changed.notify_all();
    }

    void search(const ScenePayload& scene,
                const std::string& expression)
    {
        std::string errorMessage;
        PXR_NS::SdfPathExpression parsed;
        if (!checkUsdRuntime(&errorMessage) || !parsePrimExpression(expression, &parsed, &errorMessage)) {
            finish(eStatusFailed, std::vector<PXR_NS::SdfPath>(), errorMessage);

            return;
        }
        if (scene.isEmpty() || parsed.IsEmpty()) {
            finish(shouldStop() ? eStatusCancelled : eStatusDone, std::vector<PXR_NS::SdfPath>(), std::string());

            return;
        }
        // Held across the file reads too, as a query stage's lease is: the layers it composes
        // must not be reloaded under it.
        SceneWriteBarrier::ReadScope read;
        // Composed with no payload first, so that every payload file opens where the load can
        // give up on it.
        SceneStage stage(PXR_NS::UsdStage::LoadNone);
        if (!stage.isValid()) {
            finish(eStatusFailed, std::vector<PXR_NS::SdfPath>(), "Could not open a USD stage.");

            return;
        }
        stage.sync(scene);
        stage.requestLoad(PXR_NS::SdfPathVector(1, PXR_NS::SdfPath::AbsoluteRootPath()), true);
        const SceneStage::LockedSection unlocked = [](const std::function<void()>& section) { section(); };
        const std::function<bool()> stop = [this]() { return shouldStop(); };
        while (stage.hasPendingLoads() && !shouldStop()) {
            stage.serviceLoads(unlocked, unlocked, stop);
        }
        std::vector<PXR_NS::SdfPath> found;
        if (shouldStop() || !selectPrims(stage.getStage(), expression, &found, &errorMessage, stop)) {
            if (errorMessage.empty()) {
                finish(eStatusCancelled, std::vector<PXR_NS::SdfPath>(), std::string());
            } else {
                finish(eStatusFailed, std::vector<PXR_NS::SdfPath>(), errorMessage);
            }

            return;
        }
        finish(eStatusDone, found, std::string());
    } // search
};

UnloadedSearch::UnloadedSearch()
    : _state(std::make_shared<State>())
{
}

UnloadedSearch::~UnloadedSearch()
{
    cancel();
    if (_thread.joinable()) {
        _thread.join();
    }
}

std::shared_ptr<UnloadedSearch>
UnloadedSearch::start(const ScenePayload& scene,
                      const std::string& expression,
                      const std::function<bool()>& stopWhen)
{
    std::shared_ptr<UnloadedSearch> search(new UnloadedSearch);
    search->_state->stopWhen = stopWhen;
    const std::shared_ptr<State> state = search->_state;
    search->_thread = std::thread([state, scene, expression]() { state->search(scene, expression); });

    return search;
}

std::shared_ptr<UnloadedSearch>
UnloadedSearch::start(const EffectInstancePtr& effect,
                      int inputNb,
                      const std::string& expression,
                      const std::function<bool()>& stopWhen)
{
    std::shared_ptr<UnloadedSearch> search(new UnloadedSearch);
    search->_state->stopWhen = stopWhen;
    const std::shared_ptr<State> state = search->_state;
    const EffectInstancePtr input = effect ? effect->getInput(inputNb) : EffectInstancePtr();
    const double time = effect ? effect->getCurrentTime() : 0.;
    search->_thread = std::thread([state, input, time, expression]() {
        ScenePayload scene;
        std::string error;
        // As a render sees it: the input's own pattern nodes match every payload too.
        if (input && input->producesSceneData()
            && (input->evaluateScene(EffectInstance::EvaluateSceneArgs(time, ViewIdx(0)), &scene, &error) != EffectInstance::eRenderRoIRetCodeOk)) {
            state->finish(eStatusFailed, std::vector<PXR_NS::SdfPath>(), error);

            return;
        }
        state->search(scene, expression);
    });

    return search;
}

void
UnloadedSearch::cancel()
{
    _state->cancelled = true;
}

UnloadedSearch::StatusEnum
UnloadedSearch::getStatus() const
{
    std::lock_guard<std::mutex> lock(_state->mutex);

    return _state->status;
}

bool
UnloadedSearch::wait(double timeoutMs) const
{
    std::unique_lock<std::mutex> lock(_state->mutex);
    const auto ended = [this]() { return _state->status != eStatusRunning; };
    if (timeoutMs < 0.) {
        _state->changed.wait(lock, ended);

        return true;
    }

    return _state->changed.wait_for(lock, std::chrono::duration<double, std::milli>(timeoutMs), ended);
}

std::vector<std::string>
UnloadedSearch::getMatches() const
{
    std::lock_guard<std::mutex> lock(_state->mutex);

    return _state->matches;
}

std::string
UnloadedSearch::getError() const
{
    std::lock_guard<std::mutex> lock(_state->mutex);

    return _state->error;
}

} // namespace SceneOps

struct SceneLayerMemo::Implementation {
    struct Slot {
        U64 context = 0;
        U64 layerHash = 0;
        ScenePayload output;
    };

    std::mutex mutex;
    // Most recently used first.
    std::list<Slot> slots;
    U64 authorCount = 0;
    NodeWPtr owner;

    // The slot of context, moved first, or a new one there, evicting the least recently used.
    Slot& use(U64 context)
    {
        for (std::list<Slot>::iterator it = slots.begin(); it != slots.end(); ++it) {
            if (it->context == context) {
                slots.splice(slots.begin(), slots, it);

                return slots.front();
            }
        }
        slots.push_front(Slot());
        slots.front().context = context;
        if (slots.size() > kSceneLayerMemoSlots) {
            slots.pop_back();
        }

        return slots.front();
    }
};

SceneLayerMemo::SceneLayerMemo()
    : _imp(new Implementation)
{
}

SceneLayerMemo::~SceneLayerMemo()
{
}

bool
SceneLayerMemo::produce(const Key& key,
                        const ScenePayload& upstream,
                        const std::string& layerTag,
                        const AuthorFunc& author,
                        ScenePayload* output,
                        std::string* errorMessage,
                        const KeyCheckFunc& keyStillValid)
{
    ContentHasher h;
    h.appendU64(key.nodeDigest);
    h.appendI64((std::int64_t)key.upstreamRead);
    switch (key.upstreamRead) {
    case Key::eUpstreamReadWholeStack:
        h.appendU64(upstream.getStackHash());
        break;
    case Key::eUpstreamReadFacts:
        h.appendU64(key.factsDigest);
        break;
    case Key::eUpstreamReadNothing:
        break;
    }
    h.appendBool(key.readsFrameRange);
    if (key.readsFrameRange) {
        h.appendDouble(key.firstFrame);
        h.appendDouble(key.lastFrame);
    }
    // Always: time codes are authored per second of the project rate.
    h.appendDouble(key.framesPerSecond);
    h.appendI64((std::int64_t)key.upAxis);
    h.appendDouble(key.metersPerUnit);
    h.appendString(key.materialsScope);
    const U64 layerHash = h.finalize();

    if (!SceneOps::checkUsdRuntime(errorMessage)) {
        return false;
    }

    // Authoring reads the upstream layers and opens files: no layer reload may run meanwhile.
    // Taken before the memo's mutex, so a thread waiting on the mutex holds no write off.
    SceneWriteBarrier::ReadScope read;
    // Held while authoring: two threads evaluating the same node author one layer, not two.
    std::lock_guard<std::mutex> lock(_imp->mutex);
    const U64 context = SceneEvaluationContext::current().id;
    const Implementation::Slot* memoized = nullptr;
    for (const Implementation::Slot& slot : _imp->slots) {
        if (slot.layerHash != layerHash) {
            continue;
        }
        // Another context's layer for the same key is the same layer: it is shared, not authored again.
        if (!memoized || (slot.context == context)) {
            memoized = &slot;
        }
    }
    if (memoized) {
        // An upstream re-authored to the same content hashes the same but holds other layers:
        // the memoized layer goes on top of the stack actually handed in.
        const ScenePayload found = memoized->output;
        const ScenePayload memoizedBase(found.getImpl()->base);
        Implementation::Slot& slot = _imp->use(context);
        slot.layerHash = layerHash;
        slot.output = memoizedBase.isSameStack(upstream) ? found : SceneOps::appendEntry(upstream, found.getImpl()->top);
        *output = slot.output;

        return true;
    }

    SceneLayerEdit edit;
    edit.layer = SceneOps::createNodeLayer(layerTag);
    // A node that reads nothing upstream is not handed it, so it cannot read what its key misses.
    if (key.upstreamRead != Key::eUpstreamReadNothing) {
        edit.upstream = upstream;
    }
    edit.firstFrame = key.firstFrame;
    edit.lastFrame = key.lastFrame;
    edit.framesPerSecond = key.framesPerSecond;
    edit.upAxis = key.upAxis;
    edit.metersPerUnit = key.metersPerUnit;
    edit.materialsScope = key.materialsScope;
    SceneLinkIndex sublayerLinks;
    edit.sublayerLinks = &sublayerLinks;
    LiveTextureSourceList liveTextures;
    edit.liveTextures = &liveTextures;
    bool deferredProjections = false;
    edit.deferredProjections = &deferredProjections;
    std::shared_ptr<const SceneLayerDependencies> dependencies;
    edit.dependencies = &dependencies;
    if (!edit.layer) {
        if (errorMessage) {
            *errorMessage = "Could not create a layer for " + layerTag;
        }

        return false;
    }

    std::string authorError;
    if (!author || !author(edit, &authorError)) {
        if (errorMessage) {
            *errorMessage = authorError.empty() ? ("Could not author the layer of " + layerTag) : authorError;
        }

        return false;
    }
    ++_imp->authorCount;
    edit.layer->SetPermissionToEdit(false);

    SceneLayerEntry entry;
    entry.layer = edit.layer;
    entry.hash = layerHash;
    SceneLinkIndex links = SceneOps::findLayerLinks(edit.layer);
    links.insert(links.end(), sublayerLinks.begin(), sublayerLinks.end());
    entry.links = shareLinks(std::move(links));
    if (!liveTextures.empty()) {
        entry.liveTextures = std::make_shared<const LiveTextureSourceList>(std::move(liveTextures));
    }
    entry.deferredProjections = deferredProjections;
    entry.dependencies = dependencies;
    if (keyStillValid && !keyStillValid(key)) {
        // The layer is not what key describes, so it must not hash like one authored under it:
        // stack hashes key what downstream caches hold.
        static std::atomic<U64> unmemoized(0);
        ContentHasher u;
        u.appendU64(layerHash);
        u.appendString("unmemoized");
        u.appendU64(++unmemoized);
        entry.hash = u.finalize();
        *output = SceneOps::appendEntry(upstream, entry);
        SceneOps::setLayerOwner(*output, _imp->owner.lock(), true);

        return true;
    }
    Implementation::Slot& slot = _imp->use(context);
    slot.output = SceneOps::appendEntry(upstream, entry);
    slot.layerHash = layerHash;
    *output = slot.output;
    SceneOps::setLayerOwner(slot.output, _imp->owner.lock(), true);

    return true;
} // SceneLayerMemo::produce

void
SceneLayerMemo::invalidate()
{
    std::lock_guard<std::mutex> lock(_imp->mutex);

    _imp->slots.clear();
}

void
SceneLayerMemo::setOwner(const NodePtr& owner)
{
    std::lock_guard<std::mutex> lock(_imp->mutex);

    _imp->owner = owner;
}

U64
SceneLayerMemo::getAuthorCount() const
{
    std::lock_guard<std::mutex> lock(_imp->mutex);

    return _imp->authorCount;
}

SceneGraftMemo::SceneGraftMemo()
    : _state(new State)
{
}

SceneGraftMemo::~SceneGraftMemo()
{
}

void
SceneGraftMemo::invalidate()
{
    std::lock_guard<std::mutex> lock(_state->mutex);

    _state->structure = SceneLayerEntry();
    _state->structurePath.clear();
    _state->overrides.invalidate();
    _state->contexts.clear();
}

U64
SceneGraftMemo::getGraftLayerAuthorCount() const
{
    std::lock_guard<std::mutex> lock(_state->mutex);

    return _state->graftLayerAuthorCount;
}

U64
SceneGraftMemo::getInputReadCount() const
{
    std::lock_guard<std::mutex> lock(_state->mutex);

    return _state->inputReadCount;
}

SceneGraftMemo::State&
SceneGraftMemo::getState()
{
    return *_state;
}

NATRON_NAMESPACE_EXIT
