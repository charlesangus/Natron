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

#ifndef Engine_Nodes_Scene_StormPtexBindingFilter_h
#define Engine_Nodes_Scene_StormPtexBindingFilter_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include <pxr/imaging/hd/sceneIndex.h>

NATRON_NAMESPACE_ENTER

/**
 * @brief Keeps Storm from binding a Ptex-textured material to geometry that has no Ptex face
 * coordinates.
 *
 * Storm looks a Ptex texture up through GetPatchCoord(), which only its mesh shaders define.
 * Bound to curves or points, such a material makes a program that fails to link; Storm then draws
 * the prim with its fallback material, and as it keeps no record of the failure, it compiles and
 * links the doomed program again every time it rebuilds its draw batches, which is every frame of
 * a progressive reveal. The filter drops those bindings up front, so the prim gets the same
 * fallback material without the compile.
 *
 * Mesh bindings are untouched, as are a curve's or point cloud's bindings to materials without
 * Ptex. A binding comes back as soon as its material stops using Ptex.
 **/
namespace StormPtexBindingFilter {
// Puts the filter on every Storm render index made from now on. Once per process; MT-safe.
void registerForStorm();

PXR_NS::HdSceneIndexBaseRefPtr New(const PXR_NS::HdSceneIndexBaseRefPtr& input);
} // namespace StormPtexBindingFilter

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_StormPtexBindingFilter_h
