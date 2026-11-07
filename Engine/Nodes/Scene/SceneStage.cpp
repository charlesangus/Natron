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

#include "SceneStage.h"

#include <chrono>
#include <set>

#include <pxr/base/tf/errorMark.h>
#include <pxr/usd/sdf/layerUtils.h>
#include <pxr/usd/sdf/listOp.h>
#include <pxr/usd/sdf/payload.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/sdf/variantSetSpec.h>
#include <pxr/usd/sdf/variantSpec.h>
#include <pxr/usd/usd/primRange.h>

#include "Engine/Nodes/Scene/SceneOps.h"

NATRON_NAMESPACE_ENTER

namespace {
PXR_NS::SdfPayloadVector
getAuthoredPayloads(const PXR_NS::SdfPrimSpecHandle& spec)
{
    return spec->GetFieldAs<PXR_NS::SdfPayloadListOp>(PXR_NS::SdfFieldKeys->Payload).GetAppliedItems();
}

// The payload asset paths spec and everything under it author, as authored.
void
collectAuthoredPayloads(const PXR_NS::SdfPrimSpecHandle& spec,
                        std::set<std::string>* assets)
{
    for (const PXR_NS::SdfPayload& payload : getAuthoredPayloads(spec)) {
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

// Opens asset and the sublayers and references it needs to compose, holding each in held. A nested
// payload stays closed: its prim comes up unloaded unless it is asked for. Returns the error
// opening asset itself gave; one of its dependencies failing is the stage's to report when it
// composes. outcomes keeps each asset's result, so that assets are read once a batch.
std::string
openPayloadClosure(const std::string& asset,
                   std::map<std::string, std::string>* outcomes,
                   std::vector<PXR_NS::SdfLayerRefPtr>* held)
{
    const std::map<std::string, std::string>::const_iterator known = outcomes->find(asset);
    if (known != outcomes->end()) {
        return known->second;
    }
    PXR_NS::TfErrorMark mark;
    const PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::FindOrOpen(asset);
    if (!layer) {
        std::string error;
        for (PXR_NS::TfErrorMark::Iterator it = mark.GetBegin(); it != mark.GetEnd(); ++it) {
            error += (error.empty() ? "" : "; ") + it->GetCommentary();
        }
        mark.Clear();
        if (error.empty()) {
            error = "Could not open " + asset + ".";
        }
        (*outcomes)[asset] = error;

        return error;
    }
    (*outcomes)[asset] = std::string();
    held->push_back(layer);

    std::set<std::string> nested;
    collectAuthoredPayloads(layer->GetPseudoRoot(), &nested);
    for (const std::string& dependency : layer->GetCompositionAssetDependencies()) {
        if (!dependency.empty() && !nested.count(dependency)) {
            openPayloadClosure(PXR_NS::SdfComputeAssetPathRelativeToLayer(layer, dependency), outcomes, held);
        }
    }
    mark.Clear();

    return std::string();
}

bool
isUnloadedPayload(const PXR_NS::UsdPrim& prim)
{
    return prim.HasAuthoredPayloads() && !prim.IsLoaded();
}

// The payload assets of prim, whichever layer of its composition authors them, anchored to it.
std::vector<std::string>
getPayloadAssets(const PXR_NS::UsdPrim& prim)
{
    std::vector<std::string> assets;

    for (const PXR_NS::SdfPrimSpecHandle& spec : prim.GetPrimStack()) {
        for (const PXR_NS::SdfPayload& payload : getAuthoredPayloads(spec)) {
            // An internal payload has no asset: it is in a layer the stage already holds.
            if (!payload.GetAssetPath().empty()) {
                assets.push_back(PXR_NS::SdfComputeAssetPathRelativeToLayer(spec->GetLayer(), payload.GetAssetPath()));
            }
        }
    }

    return assets;
}
} // anonymous namespace

SceneStage::SceneStage(PXR_NS::UsdStage::InitialLoadSet load)
{
    if (!SceneOps::checkUsdRuntime(nullptr)) {
        return;
    }
    _root = PXR_NS::SdfLayer::CreateAnonymous("natron-scene-root");
    _projections = PXR_NS::SdfLayer::CreateAnonymous("natron-scene-projections");
    if (!_root || !_projections) {
        return;
    }
    _root->SetSubLayerPaths(std::vector<std::string>(1, _projections->GetIdentifier()));
    // The root carries every metadata field from the start, so later syncs only rewrite the
    // fields that change.
    SceneOps::writeRootMetadata(_root, _metadata, nullptr);
    _stage = SceneOps::openStage(_root, load);
}

SceneStage::~SceneStage()
{
}

bool
SceneStage::isValid() const
{
    return bool(_stage);
}

const PXR_NS::UsdStageRefPtr&
SceneStage::getStage() const
{
    return _stage;
}

const ScenePayload&
SceneStage::getPayload() const
{
    return _payload;
}

bool
SceneStage::sync(const ScenePayload& payload)
{
    if (!_stage) {
        return false;
    }

    std::vector<std::string> subLayerPaths = SceneOps::getSubLayerPaths(payload);
    const bool stackChanged = (subLayerPaths != _subLayerPaths);
    const SceneMetadata& metadata = payload.getMetadata();
    const bool metadataChanged = (metadata != _metadata);

    if (!stackChanged && !metadataChanged) {
        // Same layers, possibly through another payload object: keep the new one, which holds
        // the same layers.
        _payload = payload;

        return false;
    }

    {
        // One change block: the stage recomposes once, after both edits, and its notices name
        // only the paths whose opinions changed. SetSubLayerPaths replaces the list; USD diffs
        // the old and new layer stacks itself when it processes the change.
        PXR_NS::SdfChangeBlock block;
        if (stackChanged) {
            std::vector<std::string> rootSubLayers(1, _projections->GetIdentifier());
            rootSubLayers.insert(rootSubLayers.end(), subLayerPaths.begin(), subLayerPaths.end());
            _root->SetSubLayerPaths(rootSubLayers);
        }
        if (metadataChanged) {
            SceneOps::writeRootMetadata(_root, metadata, &_metadata);
        }
    }

    // The old payload goes only now: the stage has let go of layers no longer sublayered.
    _payload = payload;
    _subLayerPaths.swap(subLayerPaths);
    _metadata = metadata;
    syncProjections();
    ++_editCount;

    return true;
}

void
SceneStage::syncProjections()
{
    _unresolvedProjections.clear();
    const bool deferred = SceneOps::hasDeferredProjections(_payload);
    if (!deferred && _projections->GetRootPrims().empty()) {
        return;
    }
    // Resolved apart and swapped in whole: the stage hears of one change, and of none when the
    // values are the ones it has.
    PXR_NS::SdfLayerRefPtr resolved = PXR_NS::SdfLayer::CreateAnonymous("natron-scene-projections");
    if (!resolved) {
        return;
    }
    if (deferred) {
        SceneProjectionCameras::resolve(_stage, resolved, &_unresolvedProjections);
    }
    std::string wanted, current;
    if (resolved->ExportToString(&wanted) && _projections->ExportToString(&current) && (wanted == current)) {
        return;
    }
    _projections->TransferContent(resolved);
}

const std::vector<SceneProjectionCameras::Unresolved>&
SceneStage::getUnresolvedProjections() const
{
    return _unresolvedProjections;
}

std::size_t
SceneStage::getCommonBaseLength(const ScenePayload& payload) const
{
    const std::vector<SceneLayerEntry> applied = SceneOps::getLayers(_payload);
    const std::vector<SceneLayerEntry> wanted = SceneOps::getLayers(payload);
    std::size_t n = 0;

    while ((n < applied.size()) && (n < wanted.size()) && (applied[n].layer == wanted[n].layer)) {
        ++n;
    }

    return n;
}

U64
SceneStage::getEditCount() const
{
    return _editCount;
}

bool
SceneStage::setPreviewTransforms(const std::vector<std::pair<std::string, PXR_NS::GfMatrix4d>>& deltas,
                                 double timeCode)
{
    if (!_stage) {
        return false;
    }
    std::vector<std::string> requested;
    requested.reserve(deltas.size());
    for (const std::pair<std::string, PXR_NS::GfMatrix4d>& delta : deltas) {
        requested.push_back(delta.first);
    }
    const bool samePrims = !_preview.empty() && (requested == _previewRequested);
    if (!samePrims) {
        clearPreviewTransforms();

        // Read everything before the first write, so the stage is not recomposed in between.
        static const PXR_NS::TfToken kOpName("xformOp:transform:natronPreview");
        std::vector<PreviewPrim> prims;
        std::vector<PXR_NS::VtTokenArray> orders;
        PXR_NS::UsdGeomXformCache xforms((PXR_NS::UsdTimeCode(timeCode)));
        for (const std::pair<std::string, PXR_NS::GfMatrix4d>& delta : deltas) {
            if (!PXR_NS::SdfPath::IsValidPathString(delta.first)) {
                continue;
            }
            const PXR_NS::SdfPath path(delta.first);
            if (!path.IsAbsolutePath() || !path.IsPrimPath()) {
                continue;
            }
            const PXR_NS::UsdPrim prim = _stage->GetPrimAtPath(path);
            const PXR_NS::UsdGeomXformable xformable(prim);
            if (!xformable || prim.IsInstanceProxy() || prim.IsInPrototype()) {
                continue;
            }
            PXR_NS::VtTokenArray baseOrder;
            xformable.GetXformOpOrderAttr().Get(&baseOrder);
            PreviewPrim preview;
            preview.path = path;
            preview.resetsXformStack = !baseOrder.empty() && (baseOrder[0] == PXR_NS::UsdGeomXformOpTypes->resetXformStack);
            preview.parentToWorld = xforms.GetParentToWorldTransform(prim);
            preview.worldToParent = preview.parentToWorld.GetInverse();
            PXR_NS::VtTokenArray order;
            if (preview.resetsXformStack) {
                order.push_back(PXR_NS::UsdGeomXformOpTypes->resetXformStack);
            }
            order.push_back(kOpName);
            for (std::size_t i = preview.resetsXformStack ? 1 : 0; i < baseOrder.size(); ++i) {
                order.push_back(baseOrder[i]);
            }
            prims.push_back(preview);
            orders.push_back(order);
        }
        if (prims.empty()) {
            return false;
        }
        _previewRequested = requested;
        PXR_NS::SdfChangeBlock block;
        for (std::size_t i = 0; i < prims.size(); ++i) {
            const PXR_NS::SdfPrimSpecHandle spec = PXR_NS::SdfCreatePrimInLayer(_root, prims[i].path);
            if (!spec) {
                continue;
            }
            PXR_NS::SdfAttributeSpecHandle orderSpec = PXR_NS::SdfAttributeSpec::New(spec, PXR_NS::UsdGeomTokens->xformOpOrder.GetString(), PXR_NS::SdfValueTypeNames->TokenArray, PXR_NS::SdfVariabilityUniform);
            prims[i].op = PXR_NS::SdfAttributeSpec::New(spec, kOpName.GetString(), PXR_NS::SdfValueTypeNames->Matrix4d);
            if (!orderSpec || !prims[i].op) {
                continue;
            }
            orderSpec->SetDefaultValue(PXR_NS::VtValue(orders[i]));
            _preview.push_back(prims[i]);
        }
    }

    ++_previewGeneration;
    PXR_NS::SdfChangeBlock block;
    for (PreviewPrim& preview : _preview) {
        for (const std::pair<std::string, PXR_NS::GfMatrix4d>& delta : deltas) {
            if (delta.first != preview.path.GetString()) {
                continue;
            }
            // The op goes outside the prim's own ops: W * delta = local * op * parentToWorld.
            const PXR_NS::GfMatrix4d op = preview.resetsXformStack ? delta.second : preview.parentToWorld * delta.second * preview.worldToParent;
            if (preview.op) {
                preview.op->SetDefaultValue(PXR_NS::VtValue(op));
            }
            break;
        }
    }

    return !_preview.empty();
} // SceneStage::setPreviewTransforms

void
SceneStage::clearPreviewTransforms()
{
    _previewRequested.clear();
    if (_preview.empty() || !_root) {
        _preview.clear();

        return;
    }
    _preview.clear();
    ++_previewGeneration;
    _root->SetRootPrims(PXR_NS::SdfPrimSpecHandleVector());
}

bool
SceneStage::hasPreviewTransforms() const
{
    return !_preview.empty();
}

U64
SceneStage::getPreviewGeneration() const
{
    return _previewGeneration;
}

void
SceneStage::requestLoad(const PXR_NS::SdfPathVector& paths,
                        bool withDescendants)
{
    std::lock_guard<std::mutex> lock(_loadMutex);

    for (const PXR_NS::SdfPath& path : paths) {
        if (path.IsAbsoluteRootOrPrimPath()) {
            _pendingLoads[path] = withDescendants ? ePendingLoadWithDescendants : ePendingLoadOwn;
        }
    }
}

void
SceneStage::requestUnload(const PXR_NS::SdfPathVector& paths)
{
    std::lock_guard<std::mutex> lock(_loadMutex);

    for (const PXR_NS::SdfPath& path : paths) {
        if (path.IsAbsoluteRootOrPrimPath()) {
            _pendingLoads[path] = ePendingUnload;
        }
    }
}

void
SceneStage::cancelPendingLoads()
{
    std::lock_guard<std::mutex> lock(_loadMutex);

    _pendingLoads.clear();
}

bool
SceneStage::hasPendingLoads() const
{
    std::lock_guard<std::mutex> lock(_loadMutex);

    return !_pendingLoads.empty();
}

bool
SceneStage::serviceLoads(const LockedSection& readLocked,
                         const LockedSection& writeLocked)
{
    std::map<PXR_NS::SdfPath, PendingLoadEnum> batch;
    {
        std::lock_guard<std::mutex> lock(_loadMutex);
        if (_pendingLoads.empty()) {
            return false;
        }
        batch.swap(_pendingLoads);
        for (const std::pair<const PXR_NS::SdfPath, PendingLoadEnum>& request : batch) {
            _loadFailures.erase(request.first);
        }
    }
    if (!_stage) {
        return true;
    }

    // The payloads a descendant load names are the unloaded ones found below its path, not the path
    // itself: it is how one whose file does not open stays out of the batch.
    std::map<PXR_NS::SdfPath, std::vector<std::string>> ownLoads, descendantLoads;
    PXR_NS::SdfPathSet unloads;
    readLocked([&]() {
        for (const std::pair<const PXR_NS::SdfPath, PendingLoadEnum>& request : batch) {
            const PXR_NS::UsdPrim prim = _stage->GetPrimAtPath(request.first);
            if (!prim) {
                continue;
            }
            switch (request.second) {
            case ePendingUnload:
                unloads.insert(request.first);
                break;
            case ePendingLoadOwn:
                if (isUnloadedPayload(prim)) {
                    ownLoads[request.first] = getPayloadAssets(prim);
                }
                break;
            case ePendingLoadWithDescendants: {
                // The predicate leaves IsLoaded out, or the walk would not see what it looks for.
                const PXR_NS::UsdPrimRange range(prim, PXR_NS::UsdPrimIsActive && PXR_NS::UsdPrimIsDefined && !PXR_NS::UsdPrimIsAbstract);
                for (PXR_NS::UsdPrimRange::const_iterator it = range.begin(); it != range.end(); ++it) {
                    if (isUnloadedPayload(*it)) {
                        descendantLoads[it->GetPath()] = getPayloadAssets(*it);
                        // Its children do not compose until it is loaded.
                        it.PruneChildren();
                    }
                }
                break;
            }
            }
        }
    });

    std::map<std::string, std::string> outcomes;
    std::vector<PXR_NS::SdfLayerRefPtr> held;
    std::map<PXR_NS::SdfPath, std::string> failures;
    const auto openAll = [&](std::map<PXR_NS::SdfPath, std::vector<std::string>>* loads, PXR_NS::SdfPathSet* paths) {
        for (const std::pair<const PXR_NS::SdfPath, std::vector<std::string>>& load : *loads) {
            std::string error;
            for (const std::string& asset : load.second) {
                const std::string assetError = openPayloadClosure(asset, &outcomes, &held);
                if (!assetError.empty()) {
                    error += (error.empty() ? "" : "; ") + assetError;
                }
            }
            if (error.empty()) {
                paths->insert(load.first);
            } else {
                failures[load.first] = error;
            }
        }
    };
    PXR_NS::SdfPathSet ownPaths, descendantPaths;
    openAll(&ownLoads, &ownPaths);
    openAll(&descendantLoads, &descendantPaths);
    {
        std::lock_guard<std::mutex> lock(_loadMutex);
        for (const std::pair<const PXR_NS::SdfPath, std::vector<std::string>>& load : descendantLoads) {
            _loadFailures.erase(load.first);
        }
        _loadFailures.insert(failures.begin(), failures.end());
    }
    if (ownPaths.empty() && descendantPaths.empty() && unloads.empty()) {
        return true;
    }

    writeLocked([&]() {
        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        // A sync may have taken any of these away while the files were opening.
        const auto keepComposed = [this](PXR_NS::SdfPathSet* paths) {
            for (PXR_NS::SdfPathSet::iterator it = paths->begin(); it != paths->end();) {
                it = _stage->GetPrimAtPath(*it) ? std::next(it) : paths->erase(it);
            }
        };
        keepComposed(&ownPaths);
        keepComposed(&descendantPaths);
        keepComposed(&unloads);
        if (!descendantPaths.empty() || !unloads.empty()) {
            _stage->LoadAndUnload(descendantPaths, unloads, PXR_NS::UsdLoadWithDescendants);
            ++_loadEditCount;
        }
        if (!ownPaths.empty()) {
            _stage->LoadAndUnload(ownPaths, PXR_NS::SdfPathSet(), PXR_NS::UsdLoadWithoutDescendants);
            ++_loadEditCount;
        }
        _lastLoadHoldMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    });

    return true;
} // SceneStage::serviceLoads

std::vector<SceneStage::LoadFailure>
SceneStage::getLoadFailures() const
{
    std::lock_guard<std::mutex> lock(_loadMutex);
    std::vector<LoadFailure> failures;

    failures.reserve(_loadFailures.size());
    for (const std::pair<const PXR_NS::SdfPath, std::string>& failure : _loadFailures) {
        failures.push_back(LoadFailure { failure.first, failure.second });
    }

    return failures;
}

U64
SceneStage::getLoadEditCount() const
{
    return _loadEditCount;
}

double
SceneStage::getLastLoadHoldMs() const
{
    return _lastLoadHoldMs;
}

NATRON_NAMESPACE_EXIT
