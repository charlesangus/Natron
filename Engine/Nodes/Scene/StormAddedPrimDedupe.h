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

#ifndef Engine_Nodes_Scene_StormAddedPrimDedupe_h
#define Engine_Nodes_Scene_StormAddedPrimDedupe_h

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
 * @brief Sends each prim at most once per added notice into Storm's dependency forwarding.
 *
 * Storm prunes a material no prim binds by adding it with no type, and adds it again, typed, once
 * a prim binds it. The pruning index appends that second add once for every prim in the notice
 * that binds the material, so a notice bringing K newly bound prims carries K adds of the same
 * material. The dependency forwarding index downstream dirties every dependent of an added prim,
 * per entry and without deduplicating, so those K adds cost K times every prim already drawn
 * with the material: hundreds of millions of dirty entries when the draw filter admits a few
 * thousand prims of a heavily shared material. Only the last add of a path in a notice is kept,
 * which is the one Hydra would have acted on.
 **/
namespace StormAddedPrimDedupe {
// Puts the filter just ahead of Storm's dependency forwarding on every Storm render index made
// from now on. Once per process; MT-safe.
void registerForStorm();

PXR_NS::HdSceneIndexBaseRefPtr New(const PXR_NS::HdSceneIndexBaseRefPtr& input);
} // namespace StormAddedPrimDedupe

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_StormAddedPrimDedupe_h
