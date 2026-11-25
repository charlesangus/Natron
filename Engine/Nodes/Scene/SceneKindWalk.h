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

#ifndef Engine_Nodes_Scene_SceneKindWalk_h
#define Engine_Nodes_Scene_SceneKindWalk_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <functional>
#include <vector>

#include "Engine/Nodes/Scene/UsdIncludes.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Where a kind sits in the model hierarchy, from the top down. Group, model, unknown kinds
 * and prims with no kind pass through: a walk goes on below them.
 **/
enum SceneKindLevelEnum {
    eSceneKindLevelPassThrough = 0,
    eSceneKindLevelAssembly,
    eSceneKindLevelComponent,
    eSceneKindLevelSubcomponent
};

/**
 * @brief The level of kind. A registered custom kind takes the level of the built-in kind it
 * derives from.
 **/
SceneKindLevelEnum sceneKindLevel(const PXR_NS::TfToken& kind);

// Whether kind is assembly, component or subcomponent.
bool isBuiltInKindScope(const PXR_NS::TfToken& kind);

// The kinds a walk can target: the built-in scopes and every registered custom kind.
bool isKindScope(const PXR_NS::TfToken& kind);

// The kinds the registry knows other than USD's built-in ones, sorted by name.
std::vector<PXR_NS::TfToken> registeredCustomKinds();

/**
 * @brief Whether a walk to target stops at a prim of kind, with *matches set to whether that prim
 * is one of the target's: walkToKind()'s rule, for walks over something other than the stage.
 **/
bool kindWalkStopsAt(const PXR_NS::TfToken& kind,
                     const PXR_NS::TfToken& target,
                     bool* matches);

/**
 * @brief The prims under some roots that a kind scope acts on, as walkToKind() finds them.
 **/
struct SceneKindWalkResult {
    // The prims of the target kind, in walk order.
    PXR_NS::SdfPathVector matches;
    // The prims the walk went through, not of the target kind nor below its level, whose payload
    // is unloaded: what they would compose is not walked.
    PXR_NS::SdfPathVector unloadedPassThrough;
    bool cancelled = false;
};

/**
 * @brief Walks the composed prims depth first from each of roots, a root inside another's subtree
 * walked once. The walk descends through prims that pass through or sit above the target's level
 * and stops at the first prim at or below that level, which is a match if its kind is the target:
 * a built-in target matches the kinds that derive from it, a custom one only itself. A custom
 * target derived from no built-in level stops the walk only where it matches. Instances are
 * visited but not walked into, and inactive, undefined and abstract prims are skipped.
 *
 * Reads the stage only. cancelled, if set, is polled every few hundred prims; a walk it stops
 * comes back with cancelled set and what it had found.
 **/
SceneKindWalkResult walkToKind(const PXR_NS::UsdStageRefPtr& stage,
                               const PXR_NS::SdfPathVector& roots,
                               const PXR_NS::TfToken& target,
                               const std::function<bool()>& cancelled = std::function<bool()>());

/**
 * @brief The top-most composed descendants of prim that have payloads authored, loaded or not; prim
 * itself is not one of them.
 **/
PXR_NS::SdfPathVector payloadsBelow(const PXR_NS::UsdPrim& prim);

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_SceneKindWalk_h
