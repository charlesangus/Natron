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
#include <mutex>
#include <set>

#include "Engine/ContentHasher.h"
#include "Engine/Nodes/Scene/SceneLayerMemo.h"
#include "Engine/Nodes/Scene/SceneWriteBarrier.h"

NATRON_NAMESPACE_ENTER

namespace SceneOps {
bool
checkUsdRuntime(std::string* errorMessage)
{
    static const std::string missing = []() {
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
    if (!layer) {
        return base;
    }

    std::shared_ptr<ScenePayloadImpl> imp = std::make_shared<ScenePayloadImpl>();
    imp->base = base.getImpl();
    imp->top.layer = layer;
    imp->top.hash = layerHash;
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
}

namespace {
    std::atomic<U64> stageOpenCount(0);
}

PXR_NS::UsdStageRefPtr
openStage(const PXR_NS::SdfLayerRefPtr& root)
{
    if (!root) {
        return PXR_NS::UsdStageRefPtr();
    }
    ++stageOpenCount;

    // The session layer is passed explicitly as null: the Open overload without one creates an
    // anonymous session layer (stage.h). Nothing edits these stages but their owner, through the
    // root layer.
    return PXR_NS::UsdStage::Open(root, PXR_NS::SdfLayerHandle(), PXR_NS::UsdStage::LoadAll);
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

    return openStage(root);
}

std::vector<PXR_NS::SdfLayerRefPtr>
getFileLayerClosure(const PXR_NS::SdfLayerRefPtr& layer)
{
    std::vector<PXR_NS::SdfLayerRefPtr> closure;

    if (!layer) {
        return closure;
    }

    // GetCompositionAssetDependencies lists one layer's own sublayer, reference and payload asset
    // paths, without recursing (sdf/layer.h), so the walk recurses itself.
    std::set<std::string> visited;
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
        for (const std::string& assetPath : current->GetCompositionAssetDependencies()) {
            if (assetPath.empty()) {
                continue;
            }
            // Resolves assetPath against current's location; FindOrOpenRelativeToLayer needs a
            // valid anchor, which current is.
            const PXR_NS::SdfLayerRefPtr dependency = PXR_NS::SdfLayer::FindOrOpenRelativeToLayer(current, assetPath);
            if (dependency) {
                pending.push_back(dependency);
            }
        }
    }
    // A missing or unreadable dependency is the stage's error to report when it composes, not
    // this walk's.
    mark.Clear();

    return closure;
}

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
        },
                                         errorMessage)) {
        return false;
    }
    if (!reloaded && errorMessage) {
        *errorMessage = "Some USD files could not be read again.";
    }

    return reloaded;
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

} // namespace SceneOps

struct SceneLayerMemo::Implementation {
    std::mutex mutex;
    bool valid = false;
    U64 layerHash = 0;
    ScenePayload output;
    U64 authorCount = 0;
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
                        std::string* errorMessage)
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
    const U64 layerHash = h.finalize();

    if (!SceneOps::checkUsdRuntime(errorMessage)) {
        return false;
    }

    // Authoring reads the upstream layers and opens files: no layer reload may run meanwhile.
    // Taken before the memo's mutex, so a thread waiting on the mutex holds no write off.
    SceneWriteBarrier::ReadScope read;
    // Held while authoring: two threads evaluating the same node author one layer, not two.
    std::lock_guard<std::mutex> lock(_imp->mutex);
    if (_imp->valid && (_imp->layerHash == layerHash)) {
        // An upstream re-authored to the same content hashes the same but holds other layers:
        // the memoized layer goes on top of the stack actually handed in.
        const ScenePayload memoizedBase(_imp->output.getImpl()->base);
        if (!memoizedBase.isSameStack(upstream)) {
            _imp->output = SceneOps::appendLayer(upstream, _imp->output.getImpl()->top.layer, layerHash);
        }
        *output = _imp->output;

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

    _imp->output = SceneOps::appendLayer(upstream, edit.layer, layerHash);
    _imp->layerHash = layerHash;
    _imp->valid = true;
    *output = _imp->output;

    return true;
} // SceneLayerMemo::produce

void
SceneLayerMemo::invalidate()
{
    std::lock_guard<std::mutex> lock(_imp->mutex);

    _imp->valid = false;
    _imp->output = ScenePayload();
}

U64
SceneLayerMemo::getAuthorCount() const
{
    std::lock_guard<std::mutex> lock(_imp->mutex);

    return _imp->authorCount;
}

NATRON_NAMESPACE_EXIT
