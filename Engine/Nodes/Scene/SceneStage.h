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

#include <cstddef>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "Engine/Nodes/Scene/UsdIncludes.h"
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
 * getStage(), including Hydra renders of it.
 **/
class SceneStage {
public:
    /**
     * @brief Opens the stage on an empty root. isValid() is false when OpenUSD is unusable. A
     * foldable stage has a session layer of its own, where setFoldedPrims() writes; render and
     * query stages are not foldable, so folding never reaches them.
     **/
    explicit SceneStage(bool foldable = false);

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
     * @brief How many of payload's layers, from the weakest up, are the ones already applied, by
     * identity. A sync to a payload sharing a longer base touches less.
     **/
    std::size_t getCommonBaseLength(const ScenePayload& payload) const;

    // How many sync() calls edited the stage.
    U64 getEditCount() const;

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

    bool isFoldable() const;

    /**
     * @brief Makes exactly paths folded: each is drawn as its bounding box (a GeomModelAPI over in
     * the session layer, model:applyDrawMode true and model:drawMode bounds) and the payloads at
     * and below it are unloaded; unfolding removes the over and loads them again. The folds are
     * kept across sync(), whatever prims the scene has. Returns whether anything changed; false
     * on a stage that is not foldable.
     **/
    bool setFoldedPrims(const std::set<PXR_NS::SdfPath>& paths);

    const std::set<PXR_NS::SdfPath>& getFoldedPrims() const;

private:
    struct PreviewPrim {
        PXR_NS::SdfPath path;
        PXR_NS::GfMatrix4d parentToWorld;
        PXR_NS::GfMatrix4d worldToParent;
        bool resetsXformStack = false;
        PXR_NS::SdfAttributeSpecHandle op;
    };

    PXR_NS::SdfLayerRefPtr _root;
    PXR_NS::SdfLayerRefPtr _session;
    std::set<PXR_NS::SdfPath> _folded;
    PXR_NS::UsdStageRefPtr _stage;
    ScenePayload _payload;
    std::vector<std::string> _subLayerPaths;
    SceneMetadata _metadata;
    U64 _editCount = 0;
    std::vector<PreviewPrim> _preview;
    std::vector<std::string> _previewRequested;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_SceneStage_h
