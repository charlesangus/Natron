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

#ifndef Engine_Nodes_Scene_MoonRayAovs_h
#define Engine_Nodes_Scene_MoonRayAovs_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <pxr/base/vt/dictionary.h>
#include <pxr/base/vt/value.h>

#include "Engine/Nodes/Scene/UsdIncludes.h"

PXR_NAMESPACE_OPEN_SCOPE
class UsdImagingGLEngine;
PXR_NAMESPACE_CLOSE_SCOPE

// hdMoonray's HdRendererPlugin type name.
#define kMoonRayRendererId "HdMoonrayRendererPlugin"

NATRON_NAMESPACE_ENTER

/**
 * @brief What a render through hdMoonray needs beyond what every Hydra delegate does: the AOV
 * names it renders USDRender's layers to, how it reports a render that cannot finish, the setting
 * its thread count is taken from, and its log, which MoonRay writes through log4cplus.
 *
 * hdMoonray renders in an Arras session: a local execComp process MoonRay's threads run in, which
 * the delegate starts on the first render and ends when it is destroyed. Its AOVs are host buffers.
 **/
namespace MoonRayAovs {
bool isMoonRay(const std::string& rendererId);

/**
 * @brief The AOVs a render asks for: color (beauty and alpha, scene-linear), cameraDepth (the
 * distance along the view axis, which hdMoonray's depth remaps to a perspective window depth),
 * primId and instanceId (Hydra's ids, as Storm writes them, -1 where nothing was hit) and normal
 * (the shading normal in MoonRay's render space, which is the camera's space: unlike Hydra's
 * normal AOV, not in world space).
 **/
const PXR_NS::TfTokenVector& aovs();
const PXR_NS::TfToken& depthAov();
const PXR_NS::TfToken& normalAov();

// A cameraDepth value as depth.Z holds it: +inf where nothing was hit.
float cameraDepth(float value) WARN_UNUSED_RETURN;

/**
 * @brief The cores the Arras session leaves free so that MoonRay renders on threadBudget threads of
 * hardwareThreads: Arras gives the render hardware_concurrency() minus this. 1, hdMoonray's own
 * default, when there is no budget; never all of them.
 **/
int reservedCores(int threadBudget,
                  int hardwareThreads) WARN_UNUSED_RETURN;

/**
 * @brief The renderer settings a render with threadBudget threads (0: no budget) sets before
 * rendering. A change of the thread count restarts the session on the next render.
 **/
std::vector<std::pair<PXR_NS::TfToken, PXR_NS::VtValue>> renderSettings(int threadBudget);

// Sets renderSettings(threadBudget) on an engine rendering through hdMoonray.
void configureEngine(PXR_NS::UsdImagingGLEngine* engine,
                     int threadBudget);

// A message MoonRay logged.
struct Notice {
    enum LevelEnum {
        eLevelDebug = 0,
        eLevelInfo,
        eLevelWarning,
        eLevelError
    };

    LevelEnum level = eLevelDebug;
    std::string text;
};

/**
 * @brief While alive, takes what MoonRay logs in this process away from the console: every message
 * goes to the natron.render.moonray logging category at debug level, and is kept for notices().
 * Several may be alive at once, each keeping every message. Without log4cplus in the build it
 * keeps nothing and MoonRay prints as it would.
 **/
class LogCapture {
public:
    LogCapture();
    ~LogCapture();

    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;

    std::vector<Notice> notices() const;

    // Whether messages can be taken at all: false without log4cplus.
    static bool available();

    struct Imp;

private:
    std::shared_ptr<Imp> _imp;
};

/**
 * @brief Why a render can never converge, or empty: the Arras session MoonRay renders in could not
 * be made or stopped, which hdMoonray reports in the render stats' renderProgressAnnotation and in
 * its log, and after which it never finishes a frame.
 **/
std::string sessionFailure(const PXR_NS::VtDictionary& renderStats,
                           const std::vector<Notice>& notices) WARN_UNUSED_RETURN;

// A light on the stage, and its prim type.
struct Light {
    PXR_NS::SdfPath path;
    std::string type;
};

/**
 * @brief The light prims on stage that UsdImaging's adapters for their type and applied API schemas
 * turn into a Hydra prim of a type not in supportedPrimTypes (a mesh with MeshLightAPI is a mesh
 * and a meshLight), which Hydra drops without a word.
 **/
std::vector<Light> unsupportedLights(const PXR_NS::UsdStageRefPtr& stage,
                                     const PXR_NS::TfTokenVector& supportedPrimTypes) WARN_UNUSED_RETURN;

// hdMoonray's supported sprim, rprim and bprim types, from a delegate made once; empty when
// MoonRay is not loadable.
const PXR_NS::TfTokenVector& supportedPrimTypes();

/**
 * @brief The node's warning about what MoonRay could not render, one line per kind (lights,
 * materials, other prims) naming the prims; empty when it rendered everything. A warning or error
 * notice is of the prim its leading path names (or the nearest prim above it on stage), and of the
 * kind of that prim.
 **/
std::string warningText(const PXR_NS::UsdStageRefPtr& stage,
                        const std::vector<Light>& unsupported,
                        const std::vector<Notice>& notices) WARN_UNUSED_RETURN;

// The error notices that name no prim of stage, one per line; empty if none.
std::string errorText(const PXR_NS::UsdStageRefPtr& stage,
                      const std::vector<Notice>& notices) WARN_UNUSED_RETURN;
} // namespace MoonRayAovs

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_MoonRayAovs_h
