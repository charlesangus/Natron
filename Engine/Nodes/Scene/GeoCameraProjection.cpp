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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "GeoCameraProjection.h"

#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneProjectionCameras.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"

NATRON_NAMESPACE_ENTER

namespace GeoCameraProjection {
bool
worldToFrame(const ScenePayload& scene,
             const std::string& cameraPath,
             double frame,
             double framesPerSecond,
             double aspect,
             int fitMode,
             std::array<double, 16>* matrix,
             std::string* usedPath,
             std::string* errorMessage)
{
    if (scene.isEmpty()) {
        *errorMessage = "The camera input holds no scene.";

        return false;
    }
    SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(scene, errorMessage);
    const PXR_NS::UsdStageRefPtr stage = lease.getStage();
    if (!stage) {
        if (errorMessage->empty()) {
            *errorMessage = "USD could not compose the camera scene.";
        }

        return false;
    }
    std::string path = cameraPath;
    if (path.empty()) {
        const std::vector<PXR_NS::SdfPath> cameras = HydraRender::cameraPaths(stage);
        if (cameras.empty()) {
            *errorMessage = "The camera input has no camera to project through.";

            return false;
        }
        path = cameras.front().GetString();
    }
    if (!PXR_NS::SdfPath::IsValidPathString(path)) {
        *errorMessage = "\"" + path + "\" is not a valid prim path.";

        return false;
    }
    const HydraRender::FitModeEnum fit = (fitMode >= (int)HydraRender::eFitModeFill) && (fitMode <= (int)HydraRender::eFitModeVertical)
        ? (HydraRender::FitModeEnum)fitMode
        : HydraRender::eFitModeFill;
    const PXR_NS::UsdTimeCode time(SceneOps::frameToTimeCode(scene, frame, framesPerSecond));
    PXR_NS::GfMatrix4d worldToProjector;
    PXR_NS::GfVec4f origin;
    if (!SceneProjectionCameras::computeProjector(stage, PXR_NS::SdfPath(path), time, aspect, fit, &worldToProjector, &origin,
                                                  errorMessage)) {
        return false;
    }
    const double* data = worldToProjector.GetArray();
    for (int i = 0; i < 16; ++i) {
        (*matrix)[(std::size_t)i] = data[i];
    }
    *usedPath = path;

    return true;
} // worldToFrame

std::vector<std::string>
cameraPaths(const ScenePayload& scene)
{
    std::vector<std::string> out;
    if (scene.isEmpty()) {
        return out;
    }
    std::string error;
    SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(scene, &error);
    const PXR_NS::UsdStageRefPtr stage = lease.getStage();
    if (!stage) {
        return out;
    }
    for (const PXR_NS::SdfPath& path : HydraRender::cameraPaths(stage)) {
        out.push_back(path.GetString());
    }

    return out;
}
} // namespace GeoCameraProjection

NATRON_NAMESPACE_EXIT
