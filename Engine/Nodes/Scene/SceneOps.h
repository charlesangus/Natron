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

#ifndef Engine_Nodes_Scene_SceneOps_h
#define Engine_Nodes_Scene_SceneOps_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <vector>

#include "Engine/Nodes/Scene/ScenePayloadImpl.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief What a scene node's authoring function is handed: the fresh layer to author its edits
 * into, the payload it reads from (its input's), and the project time settings animated knobs
 * sample over. Knobs that are not animated author defaults; animated ones author time samples at
 * every frame of [firstFrame, lastFrame]. The layer is sealed against edits once authoring
 * returns.
 **/
struct SceneLayerEdit {
    PXR_NS::SdfLayerRefPtr layer;
    ScenePayload upstream;
    double firstFrame = 0.;
    double lastFrame = 0.;
    double framesPerSecond = 24.;
};

/**
 * @brief The thin interface scene nodes use to author layers, stack them into payloads and
 * compose them. It is deliberately not an abstraction over USD's semantics: nodes author with
 * the Sdf and Usd APIs directly.
 **/
namespace SceneOps {
/**
 * @brief Whether OpenUSD found the plugInfo.json files of the libraries Natron links, which USD
 * looks for in usd/ beside libusd_plug.so. Without them USD's first lookup of a type they declare
 * (the usda file format, behind every anonymous layer) is a fatal error that aborts the process,
 * so every scene entry point checks this before calling into USD. Returns false with
 * *errorMessage set when any is missing. Checked once per process.
 **/
bool checkUsdRuntime(std::string* errorMessage);

/**
 * @brief An empty anonymous layer for one node's edits. The tag only shows up in the layer's
 * identifier, for debugging.
 **/
PXR_NS::SdfLayerRefPtr createNodeLayer(const std::string& tag);

/**
 * @brief The payload made of base's layers with layer on top as the strongest. layerHash must
 * identify layer's content: equal hashes over equal bases are taken to mean equal stacks.
 * The metadata a layer authors on its pseudo-root overrides base's.
 **/
ScenePayload appendLayer(const ScenePayload& base,
                         const PXR_NS::SdfLayerRefPtr& layer,
                         U64 layerHash);

/**
 * @brief The payload's layers, weakest first.
 **/
std::vector<SceneLayerEntry> getLayers(const ScenePayload& payload);

/**
 * @brief The stage-level metadata layer authors on its pseudo-root, over weaker for whatever it
 * does not author.
 **/
SceneMetadata readLayerMetadata(const PXR_NS::SdfLayerRefPtr& layer,
                                const SceneMetadata& weaker);

/**
 * @brief The identifiers of payload's layers in the order a root layer sublayers them, strongest
 * at index 0: the stack's order reversed.
 **/
std::vector<std::string> getSubLayerPaths(const ScenePayload& payload);

/**
 * @brief Writes metadata onto root's pseudo-root, as the stage-level metadata of the stage root
 * is the root layer of. When applied is given, only the fields that differ from it are written,
 * so a stage on root hears of nothing that did not change. An empty defaultPrim clears it.
 **/
void writeRootMetadata(const PXR_NS::SdfLayerRefPtr& root,
                       const SceneMetadata& metadata,
                       const SceneMetadata* applied);

/**
 * @brief Opens a stage on root with no session layer and every payload loaded. Every stage Natron
 * composes is opened here, so getStageOpenCount() counts them all.
 **/
PXR_NS::UsdStageRefPtr openStage(const PXR_NS::SdfLayerRefPtr& root);

/**
 * @brief How many stages openStage() has opened in this process.
 **/
U64 getStageOpenCount();

/**
 * @brief Composes payload into a new stage: an anonymous root layer carrying the payload's
 * metadata and sublayering its layers, strongest first. Each call opens a new stage, so it is
 * for one-off uses (tests, writing a file); consumers that follow a changing stack keep a
 * SceneStage, and nodes reading their upstream borrow one from SceneQueryPool.
 **/
PXR_NS::UsdStageRefPtr compose(const ScenePayload& payload);

/**
 * @brief The file layers layer composes: layer itself when it is backed by a file, then every
 * layer its sublayers, references and payloads name, recursively, each once. Asset paths resolve
 * relative to the layer naming them. Files that cannot be opened are skipped, without posting
 * errors to the caller's TfErrorMark. Opens the layers not yet open, and the returned references
 * keep them open.
 **/
std::vector<PXR_NS::SdfLayerRefPtr> getFileLayerClosure(const PXR_NS::SdfLayerRefPtr& layer);

/**
 * @brief Reloads the file layers among layers in place (SdfLayer::ReloadLayers: only those whose
 * file changed since it was read), so every stage composing them, live ones included, sees the
 * files' current content. Anonymous layers are skipped: reloading one clears it. Runs inside
 * SceneWriteBarrier::runExclusive, so it fails when the calling thread holds a ReadScope.
 * Returns false with *errorMessage set when the barrier refuses the write or a layer fails to
 * reload.
 **/
bool reloadFileLayers(const std::vector<PXR_NS::SdfLayerRefPtr>& layers,
                      std::string* errorMessage);

/**
 * @brief The timecode of payload's composed stage at frame of a project running at
 * framesPerSecond: the same instant in seconds, frame x the stage's time codes per second /
 * framesPerSecond. Identical to frame when the rates match.
 **/
double frameToTimeCode(const ScenePayload& payload,
                       double frame,
                       double framesPerSecond);
} // namespace SceneOps

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_SceneOps_h
