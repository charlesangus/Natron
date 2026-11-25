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

#ifndef Engine_Nodes_Scene_GeoCameraProjection_h
#define Engine_Nodes_Scene_GeoCameraProjection_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>
#include <string>
#include <vector>

#include "Engine/SceneLoadSet.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Camera queries for native geometry nodes, whose own sources stay free of USD: the
 * stage is composed and read here.
 **/
namespace GeoCameraProjection {
/**
 * @brief The matrix taking world points, as row vectors [x y z 1], to the camera's frame: after
 * dividing by w, x and y run 0 to 1 across it, y up. The camera is the one at @p cameraPath, or
 * the scene's first camera when that is empty; *usedPath names it. @p frame is a project frame
 * at @p framesPerSecond. The aperture is conformed to @p aspect (width over height) per
 * @p fitMode, a HydraRender::FitModeEnum. Fails, with *errorMessage set, when there is no
 * camera.
 *
 * The stage loads @p loadSet's payloads, widened by those on the way to @p cameraPath when it
 * is given; the first camera is looked for among what @p loadSet loads. The default is the set
 * of the evaluation running on the calling thread, every payload outside one.
 **/
bool worldToFrame(const ScenePayload& scene,
                  const std::string& cameraPath,
                  double frame,
                  double framesPerSecond,
                  double aspect,
                  int fitMode,
                  std::array<double, 16>* matrix,
                  std::string* usedPath,
                  std::string* errorMessage,
                  const SceneLoadSetPtr& loadSet = SceneEvaluationContext::current().loadSet);

/**
 * @brief The path of every camera @p loadSet's payloads hold, in traversal order, so that a menu
 * listing them loads nothing more than the evaluation it serves. Empty when the scene cannot be
 * composed.
 **/
std::vector<std::string> cameraPaths(const ScenePayload& scene,
                                     const SceneLoadSetPtr& loadSet = SceneEvaluationContext::current().loadSet);
}

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_GeoCameraProjection_h
