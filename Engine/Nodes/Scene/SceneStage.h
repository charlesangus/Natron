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

#ifndef Engine_Nodes_Scene_SceneStage_h
#define Engine_Nodes_Scene_SceneStage_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "Engine/Nodes/Scene/SceneProjectionCameras.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/SceneLoadSet.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief A long-lived stage that follows a changing layer stack. It owns a private anonymous root
 * layer and one stage opened on it for its whole life; sync() points the root's sublayers at a
 * payload's layers and writes the payload's stage metadata onto the root, inside one change
 * block. USD then notifies the stage only of what the change touched: swapping one node's layer
 * resyncs the paths that layer and the one it replaces author, not the whole stage, and consumers
 * of the stage (a Hydra engine, a bbox cache) update incrementally instead of starting over.
 *
 * The node layers themselves are never edited; the root layer is the only thing sync() writes,
 * and nothing else can reach it. The stage holds the layers it sublayers, and so does the payload
 * kept from the last sync().
 *
 * Not thread-safe: a stage must not be read while it is being edited (OpenUSD's threading model
 * is many readers or one writer per stage), so the owner serializes sync() with every read of
 * getStage(), including Hydra renders of it. The exceptions are the requests for payload loads and
 * what reports on them, which any thread may call at any time.
 **/
class SceneStage {
public:
    // Opens the stage on an empty root, with the payloads of whatever it later composes loaded as
    // load says. isValid() is false when OpenUSD is unusable.
    explicit SceneStage(PXR_NS::UsdStage::InitialLoadSet load = PXR_NS::UsdStage::LoadAll);

    ~SceneStage();

    bool isValid() const;

    // The stage, the same object for the SceneStage's whole life.
    const PXR_NS::UsdStageRefPtr& getStage() const;

    // The payload last synced, the empty one before any sync.
    const ScenePayload& getPayload() const;

    /**
     * @brief Makes the stage compose payload's layers and metadata. Returns true if anything was
     * edited; syncing the stack already applied edits nothing and sends no notice.
     **/
    bool sync(const ScenePayload& payload);

    /**
     * @brief Whether sync(payload) would edit nothing: the stage composes payload's layers and
     * metadata already. Takes no lock of the stage's and edits nothing.
     **/
    bool isSyncedTo(const ScenePayload& payload) const;

    /**
     * @brief How many of payload's layers, from the weakest up, are the ones already applied, by
     * identity. A sync to a payload sharing a longer base touches less.
     **/
    std::size_t getCommonBaseLength(const ScenePayload& payload) const;

    // How many sync() calls edited the stage.
    U64 getEditCount() const;

    // The projections of the payload whose camera the stage lacks, as last synced.
    const std::vector<SceneProjectionCameras::Unresolved>& getUnresolvedProjections() const;

    /**
     * @brief Draws each prim of deltas moved in the world by its matrix (a Gf matrix, so the
     * prim's world transform W becomes W * delta), until clearPreviewTransforms(): an op on the
     * root layer goes first in the prim's op order. The parent transforms and op orders are read
     * by the first call after a clear; a later call with the same prims only changes the matrices,
     * and one with other prims starts over. Prims that are not xformable, or are instance proxies,
     * are left as they are. Returns whether any prim is previewed. sync() keeps the preview.
     **/
    bool setPreviewTransforms(const std::vector<std::pair<std::string, PXR_NS::GfMatrix4d>>& deltas,
                              double timeCode);

    void clearPreviewTransforms();

    bool hasPreviewTransforms() const;

    // Moves whenever the preview changes what the stage composes, which getEditCount() does not count.
    U64 getPreviewGeneration() const;

    /**
     * @brief Asks for the payloads of paths to load, each prim's own or also those below it, on the
     * next serviceLoads(). Never touches the stage. A later request for a path replaces an earlier
     * one not yet serviced. A load with descendants leaves out the subtrees at excluded: a payload
     * above one of them loads on its own, and its descendants are asked for again once it has.
     **/
    void requestLoad(const PXR_NS::SdfPathVector& paths,
                     bool withDescendants,
                     const PXR_NS::SdfPathVector& excluded = PXR_NS::SdfPathVector());

    void requestUnload(const PXR_NS::SdfPathVector& paths);

    /**
     * @brief Drops the requests no serviceLoads() has taken yet. A batch being serviced commits what
     * it has opened but puts none of its requests back and asks for none of its follow-up loads.
     **/
    void cancelPendingLoads();

    /**
     * @brief Drops the load requests at paths no serviceLoads() has taken yet; unloads stay. A batch
     * being serviced puts back none of these paths' requests, and asks for no follow-up load of a
     * payload at one of them or of a request made at one of them. Other paths are left alone.
     **/
    void cancelPendingLoads(const PXR_NS::SdfPathVector& paths);

    bool hasPendingLoads() const;

    // Whether a request is still waiting or in a serviceLoads() that has not returned.
    bool hasOutstandingLoads() const;

    // Runs its argument with the owner's lock held, shared with other readers or exclusively.
    using LockedSection = std::function<void(const std::function<void()>&)>;

    /**
     * @brief Services every request pending: finds the payload layers under readLocked, opens them
     * with no lock held, then commits the batch under writeLocked with the layers already in the
     * registry, so that the lock is not held across file reads. A descendant load opens the nested
     * payloads its files author too, every variant's, so that its commit reads no file either.
     * Requests made meanwhile wait for the next call. Returns whether there was anything to service.
     *
     * Without cancelled the files open on the caller's thread. With it they open on a thread of
     * their own, and the call returns, committing nothing, as soon as cancelled() turns true, even
     * in the middle of a file: that thread finishes the file against state of its own.
     *
     * A sync while the files open may compose other payloads at the paths requested: a path is
     * committed only if its prim still names the payload files that were opened for it. USD takes
     * one load policy per LoadAndUnload, so a batch mixing own and descendant loads commits in two
     * calls. A prim whose payload file does not open stays unloaded and is reported by
     * getLoadFailures().
     **/
    bool serviceLoads(const LockedSection& readLocked,
                      const LockedSection& writeLocked,
                      const std::function<bool()>& cancelled = std::function<bool()>());

    /**
     * @brief What a batch commits along with its loads. Called once the batch's files are open,
     * with no lock held, with the load set the stage has once the batch commits. The function it
     * returns runs under writeLocked before the loads, in the same step, so a listener never sees
     * the loads without it; an empty one commits the loads alone. If the stage is synced again
     * after the call, the batch is put back to be serviced again rather than commit what was
     * prepared for a stack no longer applied.
     **/
    using LoadPreparer = std::function<std::function<void()>(const SceneLoadSetPtr& loadSet)>;

    bool serviceLoads(const LockedSection& readLocked,
                      const LockedSection& writeLocked,
                      const std::function<bool()>& cancelled,
                      const LoadPreparer& prepare);

    /**
     * @brief The payloads the stage includes, as a load set. Reads the stage, so it is serialized
     * as any read of getStage() is.
     **/
    SceneLoadSetPtr getLoadSet() const;

    /**
     * @brief Makes the stage include the payloads of loadSet, every one for a null set, with no
     * load request involved. Returns whether anything was loaded or unloaded. For stages nothing
     * requests loads on, such as query stages.
     **/
    bool applyLoadSet(const SceneLoadSetPtr& loadSet);

    // The set the last applyLoadSet() applied, null before any.
    const SceneLoadSetPtr& getAppliedLoadSet() const;

    struct LoadFailure {
        PXR_NS::SdfPath prim;
        std::string error;
    };

    // The prims whose last load failed. Asking again clears a prim's failure.
    std::vector<LoadFailure> getLoadFailures() const;

    // How many LoadAndUnload calls edited the stage. getEditCount() does not count them.
    U64 getLoadEditCount() const;

    // How long the last serviced batch held writeLocked.
    double getLastLoadHoldMs() const;

private:
    void syncProjections();

    enum PendingLoadEnum {
        ePendingLoadOwn,
        ePendingLoadWithDescendants,
        ePendingUnload
    };

    struct PreviewPrim {
        PXR_NS::SdfPath path;
        PXR_NS::GfMatrix4d parentToWorld;
        PXR_NS::GfMatrix4d worldToParent;
        bool resetsXformStack = false;
        PXR_NS::SdfAttributeSpecHandle op;
    };

    PXR_NS::SdfLayerRefPtr _root;
    // The strongest sublayer: the projection cameras the payload leaves for the stage to resolve.
    PXR_NS::SdfLayerRefPtr _projections;
    std::vector<SceneProjectionCameras::Unresolved> _unresolvedProjections;
    PXR_NS::UsdStageRefPtr _stage;
    ScenePayload _payload;
    std::vector<std::string> _subLayerPaths;
    SceneMetadata _metadata;
    U64 _editCount = 0;
    std::vector<PreviewPrim> _preview;
    std::vector<std::string> _previewRequested;
    U64 _previewGeneration = 0;

    // Guards the pending requests and the failures, and is never held across a stage operation.
    mutable std::mutex _loadMutex;
    std::map<PXR_NS::SdfPath, PendingLoadEnum> _pendingLoads;
    // The excluded subtrees of the pending loads with descendants that have any.
    std::map<PXR_NS::SdfPath, PXR_NS::SdfPathSet> _pendingExclusions;
    // The load requests cancelled since the batch being serviced was taken, so that the batch does
    // not ask again for them; a cancel of every request sets the flag instead.
    PXR_NS::SdfPathSet _cancelledWhileServicing;
    bool _allCancelledWhileServicing = false;
    std::map<PXR_NS::SdfPath, std::string> _loadFailures;
    bool _servicingLoads = false;
    std::atomic<U64> _loadEditCount { 0 };
    std::atomic<double> _lastLoadHoldMs { 0. };
    SceneLoadSetPtr _appliedLoadSet;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_SceneStage_h
