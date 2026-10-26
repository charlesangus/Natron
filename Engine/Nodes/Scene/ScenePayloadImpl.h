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

#ifndef Engine_Nodes_Scene_ScenePayloadImpl_h
#define Engine_Nodes_Scene_ScenePayloadImpl_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <memory>
#include <vector>

#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief What a layer composes from outside the stack it sits in: anonymous layers its references
 * name by identifier, and the stacks those sublayer. An anonymous layer lives only while something
 * holds it, and a reference to a dead one composes an empty prim without any error, so the entry
 * holding the referencing layer holds these too.
 **/
struct SceneLayerDependencies {
    std::vector<PXR_NS::SdfLayerRefPtr> layers;
    std::vector<ScenePayload> stacks;
};

struct SceneLayerEntry {
    PXR_NS::SdfLayerRefPtr layer;
    U64 hash = 0;
    // Null for a layer that composes nothing outside its stack.
    std::shared_ptr<const SceneLayerDependencies> dependencies;
};

/**
 * @brief The shared state behind a non-empty ScenePayload: its strongest layer and a link to the
 * payload it was appended to, which holds the weaker ones.
 **/
struct ScenePayloadImpl {
    std::shared_ptr<const ScenePayloadImpl> base;
    SceneLayerEntry top;
    std::size_t numLayers = 0;
    U64 stackHash = 0;
    SceneMetadata metadata;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_ScenePayloadImpl_h
