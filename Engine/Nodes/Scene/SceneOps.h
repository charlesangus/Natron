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
 * @brief Composes payload into a new stage: an anonymous root layer carrying the payload's
 * metadata and sublayering its layers, strongest first. Each call builds a new stage; consumers
 * go through StageCache to share them.
 **/
PXR_NS::UsdStageRefPtr compose(const ScenePayload& payload);
} // namespace SceneOps

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_SceneOps_h
