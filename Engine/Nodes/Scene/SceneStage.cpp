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

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <list>
#include <memory>
#include <set>
#include <thread>

#include <pxr/base/tf/errorMark.h>
#include <pxr/usd/sdf/layerUtils.h>
#include <pxr/usd/sdf/listOp.h>
#include <pxr/usd/sdf/payload.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/sdf/variantSetSpec.h>
#include <pxr/usd/sdf/variantSpec.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/pointInstancer.h>

#include "Engine/FileStampRegistry.h"
#include "Engine/Nodes/Scene/SceneKindWalk.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneTeardown.h"

NATRON_NAMESPACE_ENTER

namespace {
PXR_NS::SdfPayloadVector
getAuthoredPayloads(const PXR_NS::SdfPrimSpecHandle& spec)
{
    return spec->GetFieldAs<PXR_NS::SdfPayloadListOp>(PXR_NS::SdfFieldKeys->Payload).GetAppliedItems();
}

// Opens asset and the sublayers and references it needs to compose, holding each in held. A nested
// payload stays closed unless nested is set: its prim comes up unloaded unless it is asked for.
// Returns the error opening asset itself gave; one of its dependencies failing is the stage's to
// report when it composes. outcomes keeps each asset's result, so that assets are read once a batch.
// An asset that is, or needs, a closed scene's stale layer goes in deferred, and that layer in
// retired rather than held.
std::string
openPayloadClosure(const std::string& asset,
                   bool nested,
                   std::map<std::string, std::string>* outcomes,
                   std::vector<PXR_NS::SdfLayerRefPtr>* held,
                   std::set<std::string>* deferred,
                   std::vector<PXR_NS::SdfLayerRefPtr>* retired,
                   const std::function<bool()>& abandoned)
{
    const std::map<std::string, std::string>::const_iterator known = outcomes->find(asset);
    if (known != outcomes->end()) {
        return known->second;
    }
    if (abandoned && abandoned()) {
        return std::string();
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
    if (SceneOps::isStalePendingLayer(layer)) {
        deferred->insert(asset);
        retired->push_back(layer);
        mark.Clear();

        return std::string();
    }
    held->push_back(layer);

    const std::set<std::string> closedPayloads = nested ? std::set<std::string>() : SceneOps::getAuthoredPayloadAssets(layer);
    for (const std::string& dependency : layer->GetCompositionAssetDependencies()) {
        if (!dependency.empty() && !closedPayloads.count(dependency)) {
            const std::string dependencyAsset = PXR_NS::SdfComputeAssetPathRelativeToLayer(layer, dependency);
            openPayloadClosure(dependencyAsset, nested, outcomes, held, deferred, retired, abandoned);
            if (deferred->count(dependencyAsset) != 0) {
                deferred->insert(asset);
            }
        }
    }
    mark.Clear();

    return std::string();
}

using PathAssets = std::map<PXR_NS::SdfPath, std::vector<std::string>>;

// The files of a load batch and what opening them gave. Touches no stage, so that a thread the
// stage's owner stopped waiting for can finish with it.
struct OpenedBatch {
    PathAssets ownLoads;
    PathAssets descendantLoads;
    std::map<std::string, std::string> outcomes;
    std::vector<PXR_NS::SdfLayerRefPtr> held;
    std::map<PXR_NS::SdfPath, std::string> failures;
    PXR_NS::SdfPathSet ownPaths;
    PXR_NS::SdfPathSet descendantPaths;
    std::set<std::string> deferredAssets;
    std::vector<PXR_NS::SdfLayerRefPtr> retired;
    std::vector<std::pair<std::string, PXR_NS::SdfLayerHandle>> stale;
    // The loads that would compose a closed scene's stale layer, neither committed nor failed.
    PXR_NS::SdfPathSet deferredPaths;

    void open(const std::function<bool()>& abandoned)
    {
        // Descendant loads first: an asset is read once a batch, and they open its nested payloads.
        openAll(descendantLoads, true, &descendantPaths, abandoned);
        openAll(ownLoads, false, &ownPaths, abandoned);
        if (!retired.empty()) {
            for (const PXR_NS::SdfLayerRefPtr& layer : retired) {
                stale.push_back(std::make_pair(layer->GetRealPath(), PXR_NS::SdfLayerHandle(layer)));
            }
            // Ours may be the last reference, and destroying a layer takes as long as reading it.
            SceneTeardown::release(std::move(retired));
            retired.clear();
        }
    }

private:
    void openAll(const PathAssets& loads,
                 bool nested,
                 PXR_NS::SdfPathSet* paths,
                 const std::function<bool()>& abandoned)
    {
        for (const std::pair<const PXR_NS::SdfPath, std::vector<std::string>>& load : loads) {
            if (abandoned && abandoned()) {
                return;
            }
            std::string error;
            bool deferred = false;
            for (const std::string& asset : load.second) {
                const std::string assetError = openPayloadClosure(asset, nested, &outcomes, &held, &deferredAssets, &retired, abandoned);
                if (!assetError.empty()) {
                    error += (error.empty() ? "" : "; ") + assetError;
                }
                deferred = deferred || (deferredAssets.count(asset) != 0);
            }
            if (deferred) {
                deferredPaths.insert(load.first);
            } else if (error.empty()) {
                paths->insert(load.first);
            } else {
                failures[load.first] = error;
            }
        }
    }
};

struct OpeningBatch {
    OpenedBatch batch;
    std::mutex mutex;
    std::condition_variable doneChanged;
    bool done = false;
    std::atomic<bool> abandoned { false };
};

// The threads opening load batches. Joined at exit, so that one whose caller stopped waiting does
// not read a file into a registry that static destruction is taking down.
class OpenThreads {
public:
    static OpenThreads& instance()
    {
        static OpenThreads threads;

        return threads;
    }

    void start(const std::function<void()>& job)
    {
        std::lock_guard<std::mutex> lock(_mutex);

        for (std::list<Entry>::iterator it = _threads.begin(); it != _threads.end();) {
            if (it->done->load()) {
                it->thread.join();
                it = _threads.erase(it);
            } else {
                ++it;
            }
        }
        const std::shared_ptr<std::atomic<bool>> done = std::make_shared<std::atomic<bool>>(false);
        Entry entry;
        entry.done = done;
        entry.thread = std::thread([job, done]() {
            job();
            *done = true;
        });
        _threads.push_back(std::move(entry));
    }

    ~OpenThreads()
    {
        for (Entry& entry : _threads) {
            entry.thread.join();
        }
    }

private:
    struct Entry {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };

    std::mutex _mutex;
    std::list<Entry> _threads;
};

const std::chrono::milliseconds kCancelPollInterval(10);
const std::size_t kCancelPollPrims = 1024;

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
// Deep enough for any real nesting of instances, short of a prototype that instances itself.
constexpr int kMaxFaceCountNesting = 16;
constexpr std::size_t kFaceCountPollPrims = 4096;

U64
facesUnder(const PXR_NS::UsdPrim& root,
           std::map<PXR_NS::SdfPath, U64>* prototypes,
           const std::function<bool()>& cancelled,
           int nesting);

U64
prototypeFaces(const PXR_NS::UsdPrim& prototype,
               std::map<PXR_NS::SdfPath, U64>* prototypes,
               const std::function<bool()>& cancelled,
               int nesting)
{
    if (!prototype || (nesting >= kMaxFaceCountNesting)) {
        return 0;
    }
    const std::map<PXR_NS::SdfPath, U64>::const_iterator found = prototypes->find(prototype.GetPath());
    if (found != prototypes->end()) {
        return found->second;
    }
    const U64 faces = facesUnder(prototype, prototypes, cancelled, nesting + 1);
    if (!(cancelled && cancelled())) {
        (*prototypes)[prototype.GetPath()] = faces;
    }

    return faces;
}

U64
facesUnder(const PXR_NS::UsdPrim& root,
           std::map<PXR_NS::SdfPath, U64>* prototypes,
           const std::function<bool()>& cancelled,
           int nesting)
{
    U64 faces = 0;
    std::size_t visited = 0;
    const PXR_NS::UsdPrimRange range(root, PXR_NS::UsdPrimDefaultPredicate);

    for (PXR_NS::UsdPrimRange::const_iterator it = range.begin(); it != range.end(); ++it) {
        if (((++visited % kFaceCountPollPrims) == 0) && cancelled && cancelled()) {
            return faces;
        }
        const PXR_NS::UsdPrim& prim = *it;
        if (prim.IsInstance()) {
            faces += prototypeFaces(prim.GetPrototype(), prototypes, cancelled, nesting);
            continue;
        }
        if (prim.IsA<PXR_NS::UsdGeomMesh>()) {
            faces += PXR_NS::UsdGeomMesh(prim).GetFaceCount();
            continue;
        }
        if (!prim.IsA<PXR_NS::UsdGeomPointInstancer>()) {
            continue;
        }
        const PXR_NS::UsdGeomPointInstancer instancer(prim);
        PXR_NS::VtIntArray indices;
        PXR_NS::SdfPathVector targets;
        instancer.GetProtoIndicesAttr().Get(&indices);
        instancer.GetPrototypesRel().GetForwardedTargets(&targets);
        std::vector<U64> instances(targets.size(), 0);
        for (const int index : indices) {
            if ((index >= 0) && (std::size_t(index) < instances.size())) {
                ++instances[index];
            }
        }
        for (std::size_t k = 0; k < targets.size(); ++k) {
            if (instances[k] != 0) {
                faces += instances[k] * prototypeFaces(prim.GetStage()->GetPrimAtPath(targets[k]), prototypes, cancelled, nesting);
            }
        }
        // Its prototypes draw only as its instances.
        it.PruneChildren();
    }

    return faces;
}
} // anonymous namespace

// What the callback SceneTeardown runs once idle shares with the stage.
struct SceneStage::DeferredLoadsWake {
    std::mutex mutex;
    std::function<void()> wake;
    // The stale layers' files, whose stamps are dropped once nothing holds the layer, so that the
    // file as read again is not taken for stale.
    std::vector<std::pair<std::string, PXR_NS::SdfLayerHandle>> stale;
    // Whether a callback is registered, and whether one has run since the loads were put off.
    std::atomic<bool> waiting { false };
    std::atomic<bool> ready { false };
};

SceneStage::SceneStage(PXR_NS::UsdStage::InitialLoadSet load)
    : _deferredWake(std::make_shared<DeferredLoadsWake>())
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
SceneStage::isSyncedTo(const ScenePayload& payload) const
{
    return _stage && (payload.getMetadata() == _metadata) && (SceneOps::getSubLayerPaths(payload) == _subLayerPaths);
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
    {
        // Held loads of prims the new stack no longer composes would keep loading reported stopped.
        std::lock_guard<std::mutex> lock(_loadMutex);
        for (std::map<PXR_NS::SdfPath, PendingLoadEnum>::iterator it = _heldLoads.begin(); it != _heldLoads.end();) {
            if (_stage->GetPrimAtPath(it->first)) {
                ++it;
            } else {
                _heldExclusions.erase(it->first);
                _loadOrigins.erase(it->first);
                it = _heldLoads.erase(it);
            }
        }
    }

    return true;
}

void
SceneStage::syncProjections()
{
    _unresolvedProjections.clear();
    const bool deferred = SceneOps::hasDeferredProjections(_payload);
    if (!deferred && (_projectionsDigest == 0)) {
        return;
    }
    // Resolved apart and swapped in whole: the stage hears of one change, and of none when the
    // digest of what would be authored is the one it has.
    PXR_NS::SdfLayerRefPtr resolved = PXR_NS::SdfLayer::CreateAnonymous("natron-scene-projections");
    if (!resolved) {
        return;
    }
    const U64 digest = deferred ? SceneProjectionCameras::resolve(_stage, resolved, &_unresolvedProjections, _projectionsDigest) : 0;
    if (digest == _projectionsDigest) {
        return;
    }
    _projections->TransferContent(resolved);
    _projectionsDigest = digest;
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

void
SceneStage::noteLayerEdit()
{
    ++_editCount;
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
    _previewDeltas = deltas;
    _previewTimeCode = timeCode;
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
    _previewDeltas.clear();
    _previewTimeCode = 0.;
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
                        bool withDescendants,
                        const PXR_NS::SdfPathVector& excluded)
{
    std::lock_guard<std::mutex> lock(_loadMutex);

    for (const PXR_NS::SdfPath& path : paths) {
        if (path.IsAbsoluteRootOrPrimPath()) {
            bool again = false;
            for (const std::pair<const PXR_NS::SdfPath, PendingLoadEnum>& held : _heldLoads) {
                const std::map<PXR_NS::SdfPath, PXR_NS::SdfPath>::const_iterator origin = _loadOrigins.find(held.first);
                if ((held.first.HasPrefix(path)) || ((origin != _loadOrigins.end()) && origin->second.HasPrefix(path))) {
                    again = true;
                    break;
                }
            }
            if (again) {
                _overriddenLoads.insert(path);
            } else {
                _overriddenLoads.erase(path);
            }
            _deferredLoads.erase(path);
            _deferredExclusions.erase(path);
            _loadOrigins.erase(path);
            _heldLoads.erase(path);
            _heldExclusions.erase(path);
            _pendingLoads[path] = withDescendants ? ePendingLoadWithDescendants : ePendingLoadOwn;
            if (withDescendants && !excluded.empty()) {
                _pendingExclusions[path] = PXR_NS::SdfPathSet(excluded.begin(), excluded.end());
            } else {
                _pendingExclusions.erase(path);
            }
            _loadFailures.erase(path);
        }
    }
    // The admission decides again with what the request changes.
    releaseHeldLoadsLocked();
}

void
SceneStage::requestUnload(const PXR_NS::SdfPathVector& paths)
{
    std::lock_guard<std::mutex> lock(_loadMutex);

    for (const PXR_NS::SdfPath& path : paths) {
        if (path.IsAbsoluteRootOrPrimPath()) {
            _deferredLoads.erase(path);
            _deferredExclusions.erase(path);
            _loadOrigins.erase(path);
            _heldLoads.erase(path);
            _heldExclusions.erase(path);
            _overriddenLoads.erase(path);
            _pendingLoads[path] = ePendingUnload;
            _pendingExclusions.erase(path);
            _loadFailures.erase(path);
        }
    }
}

void
SceneStage::requestLoadToKind(const PXR_NS::SdfPathVector& roots,
                              const PXR_NS::TfToken& kind)
{
    std::lock_guard<std::mutex> lock(_loadMutex);

    for (const PXR_NS::SdfPath& root : roots) {
        if (root.IsAbsoluteRootOrPrimPath() && !kind.IsEmpty()) {
            KindRequest& request = _kindRequests[root];
            request = KindRequest();
            request.kind = kind;
        }
    }
}

void
SceneStage::requestUnloadBelowKind(const PXR_NS::SdfPathVector& roots,
                                   const PXR_NS::TfToken& kind)
{
    std::lock_guard<std::mutex> lock(_loadMutex);

    for (const PXR_NS::SdfPath& root : roots) {
        if (root.IsAbsoluteRootOrPrimPath() && !kind.IsEmpty()) {
            KindRequest& request = _kindRequests[root];
            request = KindRequest();
            request.kind = kind;
            request.unload = true;
        }
    }
}

void
SceneStage::cancelPendingLoads()
{
    std::lock_guard<std::mutex> lock(_loadMutex);

    _pendingLoads.clear();
    _pendingExclusions.clear();
    _kindRequests.clear();
    _deferredLoads.clear();
    _deferredExclusions.clear();
    _loadOrigins.clear();
    _heldLoads.clear();
    _heldExclusions.clear();
    _overriddenLoads.clear();
    if (_servicingLoads) {
        _allCancelledWhileServicing = true;
    }
}

void
SceneStage::cancelPendingLoads(const PXR_NS::SdfPathVector& paths)
{
    std::lock_guard<std::mutex> lock(_loadMutex);

    for (const PXR_NS::SdfPath& path : paths) {
        const std::map<PXR_NS::SdfPath, PendingLoadEnum>::iterator found = _pendingLoads.find(path);
        if ((found != _pendingLoads.end()) && (found->second != ePendingUnload)) {
            _pendingLoads.erase(found);
            _pendingExclusions.erase(path);
        }
        _deferredLoads.erase(path);
        _deferredExclusions.erase(path);
        _overriddenLoads.erase(path);
        const std::map<PXR_NS::SdfPath, KindRequest>::iterator kindFound = _kindRequests.find(path);
        if ((kindFound != _kindRequests.end()) && !kindFound->second.unload) {
            _kindRequests.erase(kindFound);
        }
        if (_servicingLoads) {
            _cancelledWhileServicing.insert(path);
        }
    }
    const PXR_NS::SdfPathSet cancelled(paths.begin(), paths.end());
    for (std::map<PXR_NS::SdfPath, PXR_NS::SdfPath>::iterator it = _loadOrigins.begin(); it != _loadOrigins.end();) {
        if (cancelled.count(it->second) == 0) {
            ++it;
            continue;
        }
        _pendingLoads.erase(it->first);
        _pendingExclusions.erase(it->first);
        _heldLoads.erase(it->first);
        _heldExclusions.erase(it->first);
        it = _loadOrigins.erase(it);
    }
}

void
SceneStage::setLoadChunk(std::size_t payloads)
{
    std::lock_guard<std::mutex> lock(_loadMutex);

    _loadChunk = payloads;
}

void
SceneStage::setLoadAdmission(const std::function<bool(bool overridden)>& admit)
{
    std::lock_guard<std::mutex> lock(_loadMutex);

    _loadAdmission = admit;
}

void
SceneStage::releaseHeldLoads()
{
    std::lock_guard<std::mutex> lock(_loadMutex);

    releaseHeldLoadsLocked();
}

void
SceneStage::releaseHeldLoadsLocked()
{
    for (const std::pair<const PXR_NS::SdfPath, PendingLoadEnum>& load : _heldLoads) {
        if (!_pendingLoads.insert(load).second) {
            continue;
        }
        const std::map<PXR_NS::SdfPath, PXR_NS::SdfPathSet>::const_iterator excluded = _heldExclusions.find(load.first);
        if (excluded != _heldExclusions.end()) {
            _pendingExclusions[load.first] = excluded->second;
        }
    }
    _heldLoads.clear();
    _heldExclusions.clear();
}

bool
SceneStage::hasHeldLoads() const
{
    std::lock_guard<std::mutex> lock(_loadMutex);

    return !_heldLoads.empty();
}

void
SceneStage::takeCommittedLoads(PXR_NS::SdfPathVector* loaded,
                               PXR_NS::SdfPathVector* unloaded)
{
    std::lock_guard<std::mutex> lock(_loadMutex);

    loaded->clear();
    unloaded->clear();
    loaded->swap(_committedLoads);
    unloaded->swap(_committedUnloads);
}

bool
SceneStage::hasPendingLoads() const
{
    std::lock_guard<std::mutex> lock(_loadMutex);

    return !_pendingLoads.empty() || !_kindRequests.empty() || (!_deferredLoads.empty() && _deferredWake->ready);
}

bool
SceneStage::hasOutstandingLoads() const
{
    std::lock_guard<std::mutex> lock(_loadMutex);

    return !_pendingLoads.empty() || !_kindRequests.empty() || !_deferredLoads.empty() || _servicingLoads;
}

void
SceneStage::setLoadsWake(const std::function<void()>& wake)
{
    std::lock_guard<std::mutex> lock(_deferredWake->mutex);

    _deferredWake->wake = wake;
}

void
SceneStage::takeReadyDeferredLoads()
{
    if (_deferredLoads.empty() || !_deferredWake->ready.exchange(false)) {
        return;
    }
    for (const std::pair<const PXR_NS::SdfPath, PendingLoadEnum>& load : _deferredLoads) {
        if (!_pendingLoads.insert(load).second) {
            continue;
        }
        const std::map<PXR_NS::SdfPath, PXR_NS::SdfPathSet>::const_iterator excluded = _deferredExclusions.find(load.first);
        if (excluded != _deferredExclusions.end()) {
            _pendingExclusions[load.first] = excluded->second;
        }
    }
    _deferredLoads.clear();
    _deferredExclusions.clear();
}

bool
SceneStage::serviceLoads(const LockedSection& readLocked,
                         const LockedSection& writeLocked,
                         const std::function<bool()>& cancelled)
{
    return serviceLoads(readLocked, writeLocked, cancelled, LoadPreparer());
}

bool
SceneStage::serviceLoads(const LockedSection& readLocked,
                         const LockedSection& writeLocked,
                         const std::function<bool()>& cancelled,
                         const LoadPreparer& prepare)
{
    std::map<PXR_NS::SdfPath, PendingLoadEnum> batch;
    std::map<PXR_NS::SdfPath, PXR_NS::SdfPathSet> exclusions;
    // The request each request of the batch came from, itself for one made with requestLoad().
    std::map<PXR_NS::SdfPath, PXR_NS::SdfPath> origins;
    std::map<PXR_NS::SdfPath, KindRequest> kindRequests;
    std::size_t chunk = 0;
    std::function<bool(bool overridden)> admit;
    bool followUpsPending = false;
    bool overriddenPending = false;
    {
        std::lock_guard<std::mutex> lock(_loadMutex);
        admit = _loadAdmission;
        PXR_NS::SdfPathSet stillOverridden;
        for (const std::pair<const PXR_NS::SdfPath, PXR_NS::SdfPath>& origin : _loadOrigins) {
            const bool overridden = _overriddenLoads.count(origin.second) != 0;
            if (overridden) {
                stillOverridden.insert(origin.second);
            }
            if (_pendingLoads.count(origin.first) != 0) {
                (overridden ? overriddenPending : followUpsPending) = true;
            }
        }
        // An override lasts as long as loads of its request remain.
        for (const PXR_NS::SdfPath& path : _overriddenLoads) {
            if (_pendingLoads.count(path) != 0) {
                stillOverridden.insert(path);
            }
        }
        _overriddenLoads.swap(stillOverridden);
    }
    const bool admitted = !followUpsPending || !admit || admit(false);
    const bool overriddenAdmitted = !overriddenPending || !admit || admit(true);
    {
        std::lock_guard<std::mutex> lock(_loadMutex);
        takeReadyDeferredLoads();
        if (!admitted || !overriddenAdmitted) {
            for (const std::pair<const PXR_NS::SdfPath, PXR_NS::SdfPath>& origin : _loadOrigins) {
                const std::map<PXR_NS::SdfPath, PendingLoadEnum>::iterator pending = _pendingLoads.find(origin.first);
                if (pending == _pendingLoads.end()) {
                    continue;
                }
                if ((_overriddenLoads.count(origin.second) != 0) ? overriddenAdmitted : admitted) {
                    continue;
                }
                _heldLoads[origin.first] = pending->second;
                _pendingLoads.erase(pending);
                const std::map<PXR_NS::SdfPath, PXR_NS::SdfPathSet>::iterator excluded = _pendingExclusions.find(origin.first);
                if (excluded != _pendingExclusions.end()) {
                    _heldExclusions[origin.first] = std::move(excluded->second);
                    _pendingExclusions.erase(excluded);
                }
            }
        }
        if (_pendingLoads.empty() && _kindRequests.empty()) {
            return false;
        }
        batch.swap(_pendingLoads);
        exclusions.swap(_pendingExclusions);
        for (const std::pair<const PXR_NS::SdfPath, PendingLoadEnum>& request : batch) {
            const std::map<PXR_NS::SdfPath, PXR_NS::SdfPath>::iterator origin = _loadOrigins.find(request.first);
            if (origin == _loadOrigins.end()) {
                origins[request.first] = request.first;
            } else {
                origins[request.first] = origin->second;
                _loadOrigins.erase(origin);
            }
        }
        chunk = _loadChunk;
        kindRequests.swap(_kindRequests);
        _cancelledWhileServicing.clear();
        _allCancelledWhileServicing = false;
        _servicingLoads = true;
    }
    struct ServicingScope {
        SceneStage* stage;

        ~ServicingScope()
        {
            std::lock_guard<std::mutex> lock(stage->_loadMutex);
            stage->_servicingLoads = false;
            stage->_cancelledWhileServicing.clear();
        }
    } servicingScope { this };
    if (!_stage) {
        return true;
    }
    const auto isCancelled = [&cancelled]() { return cancelled && cancelled(); };

    // Shared with the thread that opens the files, which may outlive this call.
    const std::shared_ptr<OpeningBatch> opening = std::make_shared<OpeningBatch>();
    OpenedBatch& opened = opening->batch;
    // The payloads a descendant load names are the unloaded ones found below its path, not the path
    // itself: it is how one whose file does not open stays out of the batch.
    PXR_NS::SdfPathSet unloads;
    struct SplitLoad {
        // The request whose walk found the payload.
        PXR_NS::SdfPath request;
        PXR_NS::SdfPathSet excluded;
    };
    // The payloads loaded on their own for having an excluded subtree, with the exclusions below
    // each, which a request for their descendants takes once they are loaded.
    std::map<PXR_NS::SdfPath, SplitLoad> splitLoads;
    // The request whose walk found each payload a load with descendants names.
    std::map<PXR_NS::SdfPath, PXR_NS::SdfPath> foundBy;
    // The payloads found past the chunk, asked for with their descendants once the walk is done.
    std::map<PXR_NS::SdfPath, SplitLoad> pastChunk;
    std::size_t taken = 0;
    const auto originOf = [&origins](const PXR_NS::SdfPath& request) {
        const std::map<PXR_NS::SdfPath, PXR_NS::SdfPath>::const_iterator found = origins.find(request);

        return (found != origins.end()) ? found->second : request;
    };
    // The root of the kind request whose walk found each payload it loads, which walks again once
    // they are loaded.
    std::map<PXR_NS::SdfPath, PXR_NS::SdfPath> kindLoads;
    // The kind requests as they were taken, put back as they were if the batch is.
    const std::map<PXR_NS::SdfPath, KindRequest> takenKindRequests = kindRequests;
    U64 composition = 0;
    bool walkCancelled = false;
    PXR_NS::UsdStageLoadRules rules;
    readLocked([&]() {
        composition = _editCount;
        rules = _stage->GetLoadRules();
        std::size_t visited = 0;
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
                    opened.ownLoads[request.first] = getPayloadAssets(prim);
                    ++taken;
                }
                break;
            case ePendingLoadWithDescendants: {
                const std::map<PXR_NS::SdfPath, PXR_NS::SdfPathSet>::const_iterator excludedFound = exclusions.find(request.first);
                const PXR_NS::SdfPathSet noExclusions;
                const PXR_NS::SdfPathSet& excluded = (excludedFound != exclusions.end()) ? excludedFound->second : noExclusions;
                // The predicate leaves IsLoaded out, or the walk would not see what it looks for.
                const PXR_NS::UsdPrimRange range(prim, PXR_NS::UsdPrimIsActive && PXR_NS::UsdPrimIsDefined && !PXR_NS::UsdPrimIsAbstract);
                for (PXR_NS::UsdPrimRange::const_iterator it = range.begin(); it != range.end(); ++it) {
                    if (((++visited % kCancelPollPrims) == 0) && isCancelled()) {
                        walkCancelled = true;

                        return;
                    }
                    const PXR_NS::SdfPath& path = it->GetPath();
                    if (!excluded.empty() && (excluded.count(path) != 0)) {
                        it.PruneChildren();
                        continue;
                    }
                    if (isUnloadedPayload(*it)) {
                        // SdfPath order puts a path's descendants right after it.
                        PXR_NS::SdfPathSet below;
                        for (PXR_NS::SdfPathSet::const_iterator candidate = excluded.upper_bound(path); (candidate != excluded.end()) && candidate->HasPrefix(path); ++candidate) {
                            below.insert(*candidate);
                        }
                        foundBy[path] = request.first;
                        if (chunk != 0) {
                            if (taken < chunk) {
                                ++taken;
                                opened.ownLoads[path] = getPayloadAssets(*it);
                                splitLoads[path] = SplitLoad { request.first, std::move(below) };
                            } else {
                                pastChunk[path] = SplitLoad { request.first, std::move(below) };
                            }
                        } else if (below.empty()) {
                            opened.descendantLoads[path] = getPayloadAssets(*it);
                        } else {
                            // Loading its nested payloads would load the excluded ones too.
                            opened.ownLoads[path] = getPayloadAssets(*it);
                            splitLoads[path] = SplitLoad { request.first, std::move(below) };
                        }
                        // Its children do not compose until it is loaded.
                        it.PruneChildren();
                    }
                }
                break;
            }
            }
        }
        for (std::pair<const PXR_NS::SdfPath, KindRequest>& request : kindRequests) {
            const SceneKindWalkResult walk = walkToKind(_stage, PXR_NS::SdfPathVector(1, request.first), request.second.kind, isCancelled);
            if (walk.cancelled) {
                walkCancelled = true;

                return;
            }
            if (request.second.unload) {
                for (const PXR_NS::SdfPath& match : walk.matches) {
                    for (const PXR_NS::SdfPath& payload : payloadsBelow(_stage->GetPrimAtPath(match))) {
                        if (_stage->GetPrimAtPath(payload).IsLoaded()) {
                            unloads.insert(payload);
                        }
                    }
                }
                continue;
            }
            PXR_NS::SdfPathVector found = walk.unloadedPassThrough;
            for (const PXR_NS::SdfPath& match : walk.matches) {
                if (isUnloadedPayload(_stage->GetPrimAtPath(match))) {
                    found.push_back(match);
                }
            }
            for (const PXR_NS::SdfPath& path : found) {
                if ((request.second.asked.count(path) != 0) || (opened.descendantLoads.count(path) != 0)) {
                    continue;
                }
                request.second.asked.insert(path);
                kindLoads[path] = request.first;
                if (opened.ownLoads.count(path) == 0) {
                    opened.ownLoads[path] = getPayloadAssets(_stage->GetPrimAtPath(path));
                }
            }
        }
    });
    if (walkCancelled) {
        return true;
    }
    if (!pastChunk.empty()) {
        std::lock_guard<std::mutex> lock(_loadMutex);
        if (!_allCancelledWhileServicing) {
            for (std::pair<const PXR_NS::SdfPath, SplitLoad>& past : pastChunk) {
                const PXR_NS::SdfPath origin = originOf(past.second.request);
                if ((_cancelledWhileServicing.count(origin) != 0) || (_cancelledWhileServicing.count(past.second.request) != 0) || (_pendingLoads.count(past.first) != 0)) {
                    continue;
                }
                _pendingLoads[past.first] = ePendingLoadWithDescendants;
                if (past.second.excluded.empty()) {
                    _pendingExclusions.erase(past.first);
                } else {
                    _pendingExclusions[past.first] = std::move(past.second.excluded);
                }
                _loadOrigins[past.first] = origin;
            }
        }
    }

    if (!cancelled) {
        opened.open(std::function<bool()>());
    } else {
        if (cancelled()) {
            return true;
        }
        OpenThreads::instance().start([opening]() {
            OpeningBatch* state = opening.get();
            state->batch.open([state]() { return state->abandoned.load(); });
            std::lock_guard<std::mutex> lock(state->mutex);
            state->done = true;
            state->doneChanged.notify_all();
        });
        std::unique_lock<std::mutex> lock(opening->mutex);
        while (!opening->doneChanged.wait_for(lock, kCancelPollInterval, [&opening]() { return opening->done; })) {
            if (cancelled()) {
                // The file being read finishes on the opening thread, which then lets go of the batch.
                opening->abandoned = true;

                return true;
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(_loadMutex);
        for (const std::pair<const PXR_NS::SdfPath, std::vector<std::string>>& load : opened.descendantLoads) {
            _loadFailures.erase(load.first);
        }
        _loadFailures.insert(opened.failures.begin(), opened.failures.end());
    }
    if (!opened.deferredPaths.empty()) {
        {
            std::lock_guard<std::mutex> lock(_loadMutex);
            if (!_allCancelledWhileServicing) {
                for (const PXR_NS::SdfPath& path : opened.deferredPaths) {
                    const std::map<PXR_NS::SdfPath, PXR_NS::SdfPath>::const_iterator found = foundBy.find(path);
                    const PXR_NS::SdfPath& request = (found != foundBy.end()) ? found->second : path;
                    // A cancel of the payload or of the request that found it, or a newer request
                    // for the payload, says otherwise.
                    if ((_cancelledWhileServicing.count(path) != 0) || (_cancelledWhileServicing.count(request) != 0) || (_pendingLoads.count(path) != 0)) {
                        continue;
                    }
                    const std::map<PXR_NS::SdfPath, SplitLoad>::const_iterator split = splitLoads.find(path);
                    if (split != splitLoads.end()) {
                        _deferredLoads[path] = ePendingLoadWithDescendants;
                        _deferredExclusions[path] = split->second.excluded;
                    } else {
                        _deferredLoads[path] = (found != foundBy.end()) ? ePendingLoadWithDescendants : ePendingLoadOwn;
                        _deferredExclusions.erase(path);
                    }
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(_deferredWake->mutex);
            _deferredWake->stale.insert(_deferredWake->stale.end(), opened.stale.begin(), opened.stale.end());
        }
        if (!_deferredWake->waiting.exchange(true)) {
            const std::shared_ptr<DeferredLoadsWake> state = _deferredWake;
            SceneTeardown::notifyWhenIdle([state]() {
                std::function<void()> wake;
                std::vector<std::pair<std::string, PXR_NS::SdfLayerHandle>> stale;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    wake = state->wake;
                    stale.swap(state->stale);
                }
                FileStampRegistry& stamps = FileStampRegistry::instance();
                for (const std::pair<std::string, PXR_NS::SdfLayerHandle>& file : stale) {
                    // A layer something still holds keeps its stamp, so the stamp names what it holds.
                    if (!file.second) {
                        stamps.invalidate(file.first);
                    }
                }
                state->waiting = false;
                state->ready = true;
                if (wake) {
                    wake();
                }
            });
        }
    }
    PXR_NS::SdfPathSet& ownPaths = opened.ownPaths;
    PXR_NS::SdfPathSet& descendantPaths = opened.descendantPaths;
    if ((ownPaths.empty() && descendantPaths.empty() && unloads.empty()) || isCancelled()) {
        return true;
    }

    std::function<void()> commit;
    U64 prepared = 0;
    if (prepare) {
        rules.LoadAndUnload(descendantPaths, unloads, PXR_NS::UsdLoadWithDescendants);
        rules.LoadAndUnload(ownPaths, PXR_NS::SdfPathSet(), PXR_NS::UsdLoadWithoutDescendants);
        readLocked([&]() { prepared = _editCount; });
        commit = prepare(SceneOps::makeLoadSet(rules));
        if (isCancelled()) {
            return true;
        }
    }

    bool requeue = false;
    writeLocked([&]() {
        if (commit && (_editCount != prepared)) {
            requeue = true;

            return;
        }
        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        if (commit) {
            commit();
        }
        // A sync while the files were opening may have taken a prim away, or composed another
        // payload at its path, which was not asked for there and whose files are not open.
        const bool recomposed = (_editCount != composition);
        const auto keepComposed = [this, recomposed](PXR_NS::SdfPathSet* paths, const PathAssets* assets) {
            for (PXR_NS::SdfPathSet::iterator it = paths->begin(); it != paths->end();) {
                const PXR_NS::UsdPrim prim = _stage->GetPrimAtPath(*it);
                bool keep = bool(prim);
                if (keep && recomposed && assets) {
                    const PathAssets::const_iterator found = assets->find(*it);
                    keep = (found != assets->end()) && (getPayloadAssets(prim) == found->second);
                }
                it = keep ? std::next(it) : paths->erase(it);
            }
        };
        keepComposed(&ownPaths, &opened.ownLoads);
        keepComposed(&descendantPaths, &opened.descendantLoads);
        keepComposed(&unloads, nullptr);
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
    if (!requeue) {
        std::lock_guard<std::mutex> lock(_loadMutex);
        _committedLoads.insert(_committedLoads.end(), descendantPaths.begin(), descendantPaths.end());
        _committedLoads.insert(_committedLoads.end(), ownPaths.begin(), ownPaths.end());
        _committedUnloads.insert(_committedUnloads.end(), unloads.begin(), unloads.end());
    }
    if (requeue) {
        std::lock_guard<std::mutex> lock(_loadMutex);
        if (!_allCancelledWhileServicing) {
            for (const std::pair<const PXR_NS::SdfPath, PendingLoadEnum>& request : batch) {
                // A cancel since the batch was taken withdrew the request; requeueing would revive it.
                if ((request.second != ePendingUnload) && (_cancelledWhileServicing.count(request.first) != 0)) {
                    continue;
                }
                // A request made meanwhile for one of the paths is the newer one.
                if (!_pendingLoads.insert(request).second) {
                    continue;
                }
                const std::map<PXR_NS::SdfPath, PXR_NS::SdfPathSet>::const_iterator excluded = exclusions.find(request.first);
                if (excluded != exclusions.end()) {
                    _pendingExclusions[request.first] = excluded->second;
                }
                const PXR_NS::SdfPath origin = originOf(request.first);
                if (origin != request.first) {
                    _loadOrigins[request.first] = origin;
                }
            }
            for (const std::pair<const PXR_NS::SdfPath, KindRequest>& request : takenKindRequests) {
                if (request.second.unload || (_cancelledWhileServicing.count(request.first) == 0)) {
                    _kindRequests.insert(request);
                }
            }
        }
    } else if (!kindLoads.empty()) {
        std::lock_guard<std::mutex> lock(_loadMutex);
        if (!_allCancelledWhileServicing) {
            for (const std::pair<const PXR_NS::SdfPath, PXR_NS::SdfPath>& load : kindLoads) {
                // A cancel of the request or a newer one at its root says otherwise.
                const std::map<PXR_NS::SdfPath, KindRequest>::const_iterator request = kindRequests.find(load.second);
                if ((request == kindRequests.end()) || (_cancelledWhileServicing.count(load.second) != 0)) {
                    continue;
                }
                _kindRequests.insert(*request);
            }
        }
    }
    if (!requeue && !splitLoads.empty()) {
        std::lock_guard<std::mutex> lock(_loadMutex);
        if (!_allCancelledWhileServicing) {
            for (std::pair<const PXR_NS::SdfPath, SplitLoad>& split : splitLoads) {
                // A load that failed or went, a cancel of the payload or of the request that found it,
                // or a newer request for either, says otherwise. A newer request for the one that
                // found it walks again, past the payload now loaded, with its own exclusions.
                const PXR_NS::SdfPath origin = originOf(split.second.request);
                if ((ownPaths.count(split.first) == 0) || (_cancelledWhileServicing.count(split.first) != 0) || (_cancelledWhileServicing.count(split.second.request) != 0)
                    || (_cancelledWhileServicing.count(origin) != 0) || (_pendingLoads.count(split.first) != 0) || (_pendingLoads.count(split.second.request) != 0)) {
                    continue;
                }
                _pendingLoads[split.first] = ePendingLoadWithDescendants;
                if (split.second.excluded.empty()) {
                    _pendingExclusions.erase(split.first);
                } else {
                    _pendingExclusions[split.first] = std::move(split.second.excluded);
                }
                if (origin != split.first) {
                    _loadOrigins[split.first] = origin;
                }
            }
        }
    }

    return true;
} // SceneStage::serviceLoads

SceneLoadSetPtr
SceneStage::getLoadSet() const
{
    return _stage ? SceneOps::makeLoadSet(_stage->GetLoadRules()) : SceneLoadSetPtr();
}

bool
SceneStage::applyLoadSet(const SceneLoadSetPtr& loadSet)
{
    _appliedLoadSet = loadSet;
    if (!SceneOps::applyLoadSet(_stage, loadSet.get())) {
        return false;
    }
    ++_loadEditCount;

    return true;
}

const SceneLoadSetPtr&
SceneStage::getAppliedLoadSet() const
{
    return _appliedLoadSet;
}

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

SceneStageSnapshot
SceneStage::getSnapshot() const
{
    SceneStageSnapshot snapshot;
    snapshot.payload = _payload;
    snapshot.loadSet = getLoadSet();
    if (!_preview.empty()) {
        snapshot.previewDeltas = _previewDeltas;
        snapshot.previewTimeCode = _previewTimeCode;
    }

    return snapshot;
}

bool
SceneStage::syncTo(const SceneStageSnapshot& snapshot)
{
    if (!_stage) {
        return false;
    }
    bool edited = sync(snapshot.payload);
    // After the sync: the rules apply to the prims it composes.
    const SceneLoadSetPtr& applied = getAppliedLoadSet();
    const bool sameLoadSet = applied && snapshot.loadSet && ((applied == snapshot.loadSet) || (*applied == *snapshot.loadSet));
    if (!sameLoadSet && applyLoadSet(snapshot.loadSet)) {
        edited = true;
    }
    if (snapshot.previewDeltas.empty()) {
        if (!_preview.empty()) {
            clearPreviewTransforms();
            edited = true;
        }

        return edited;
    }
    const bool same = (snapshot.previewDeltas == _previewDeltas) && (snapshot.previewTimeCode == _previewTimeCode);
    if (!same || _preview.empty()) {
        const bool had = !_preview.empty();
        if (setPreviewTransforms(snapshot.previewDeltas, snapshot.previewTimeCode) || had) {
            edited = true;
        }
    }

    return edited;
}

U64
sceneFacesUnder(const PXR_NS::UsdPrim& root,
                std::map<PXR_NS::SdfPath, U64>* prototypes,
                const std::function<bool()>& cancelled)
{
    return root ? facesUnder(root, prototypes, cancelled, 0) : 0;
}

NATRON_NAMESPACE_EXIT
