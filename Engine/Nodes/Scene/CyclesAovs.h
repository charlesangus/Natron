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

#ifndef Engine_Nodes_Scene_CyclesAovs_h
#define Engine_Nodes_Scene_CyclesAovs_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstdint>
#include <functional>
#include <string>

#include "Engine/Nodes/Scene/UsdIncludes.h"

PXR_NAMESPACE_OPEN_SCOPE
class UsdImagingGLEngine;
PXR_NAMESPACE_CLOSE_SCOPE

// Cycles' HdRendererPlugin type name.
#define kRenderDelegateCyclesId "HdCyclesPlugin"
// Cycles' HdRendererPlugin display name, which Hydra's scene index plug-in registry is keyed by.
#define kRenderDelegateCyclesDisplayName "Cycles"

NATRON_NAMESPACE_ENTER

/**
 * @brief Where Cycles' Hydra delegate departs from the AOV conventions Storm's reading of them
 * assumes, and how an engine of it is set up.
 **/
namespace CyclesAovs {
bool isCycles(const std::string& renderer) WARN_UNUSED_RETURN;

/**
 * @brief Has Hydra tessellate the implicit surfaces (UsdGeom's Cube, Sphere, Cone, Cylinder,
 * Capsule and Plane) into meshes for every Cycles engine made after it. The engine hands a
 * delegate those prims as implicit-surface rprims, which Cycles does not take, and only Storm
 * registers the scene index that turns them into meshes, so Cycles would render none of them.
 * Must run before the engine is made: its render index builds its scene index chain once. Safe
 * to call any number of times from any thread.
 **/
void registerSceneIndices();

/**
 * @brief Sets up a Cycles engine just made, before its first render. Cycles scales the camera and
 * the geometry by its stageMetersPerUnit setting (0.01 unless set), which would put depth in
 * metres; it is set to 1 so that depth is in scene units, as Storm's.
 **/
void configureEngine(PXR_NS::UsdImagingGLEngine* engine);

/**
 * @brief The distance along the view axis of a value of Cycles' depth AOV. Cycles writes the
 * camera-space depth itself, not the window depth Storm does, and 1e10 where nothing was hit,
 * which becomes +inf.
 **/
float depthToDistance(float value) WARN_UNUSED_RETURN;

/**
 * @brief The instance index UsdImagingGLEngine::DecodeIntersection takes for a value of Cycles'
 * instanceId AOV. Cycles writes every ID AOV less one, so that the primId AOV holds Hydra's prim
 * index, but its instanceId attribute is already -1 on a prim that is not instanced and the
 * instance index on one that is; adding the one back gives those.
 **/
int instanceIndex(std::int32_t value) WARN_UNUSED_RETURN;

/**
 * @brief Runs job, which makes a Cycles engine, so that its CPU device renders on at most threads
 * threads; any count when threads <= 0. Cycles sizes its device from the TBB arena its session is
 * made in, once, and every session made while another lives takes the first one's count.
 **/
void withThreadBudget(int threads,
                      const std::function<void()>& job);
} // namespace CyclesAovs

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_CyclesAovs_h
