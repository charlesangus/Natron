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

#ifndef Engine_Nodes_Scene_RenderSettingsPrim_h
#define Engine_Nodes_Scene_RenderSettingsPrim_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <map>
#include <string>
#include <vector>

#include "Engine/Nodes/Scene/UsdIncludes.h"

// The attribute of a UsdRenderSettings prim naming the Hydra renderer plug-in (e.g.
// "HdCyclesPlugin") that renders it by default. No delegate's namespace is "natron", so it is
// never sent to a delegate.
#define kRenderSettingsPrimRendererAttribute "natron:renderer"

NATRON_NAMESPACE_ENTER

/**
 * @brief Reads a UsdRenderSettings prim for a Hydra render delegate: the delegate's settings
 * are the prim's attributes in the delegate's namespaces, which the host sends through
 * SetRenderSetting, as neither Cycles' nor MoonRay's delegate reads render settings prims.
 *
 * A delegate setting key that starts with one of the renderer's namespaces and a colon
 * ("cycles:samples") is its attribute name; any other key ("debug") is prefixed with the first
 * namespace ("moonray:debug").
 **/
namespace RenderSettingsPrim {
typedef std::map<std::string, PXR_NS::VtValue> Values;

/**
 * @brief The namespaces of the renderer's attributes: what its delegate returns from
 * GetRenderSettingsNamespaces(), or, when that is empty, "cycles", "moonray" or "storm" for
 * those delegates and RenderSettingsKnobs::scriptPrefix() for any other.
 **/
std::vector<std::string> namespaces(const std::string& rendererId);

std::string attributeName(const std::string& rendererId,
                          const std::string& key);

/**
 * @brief The delegate key the attribute holds: the name itself, or the name without its
 * namespace when the delegate declares (or curates) the key that way, or when it is a Hydra key
 * the delegate reads undeclared ("storm:domeLightCameraVisibility" holds
 * "domeLightCameraVisibility"). Empty when the attribute is in none of the renderer's namespaces.
 **/
std::string keyFromAttribute(const std::string& rendererId,
                             const std::string& attributeName);

/**
 * @brief The renderer's settings prim authors, by delegate key, valued at time: the attributes
 * UsdRenderComputeNamespacedSettings() finds in the renderer's namespaces. A value whose type
 * differs from the type the delegate declares is cast to it when it can be. Keys the renderer's
 * curated group excludes or disables are left out: Natron sets those itself or keeps them off.
 **/
Values settings(const PXR_NS::UsdPrim& prim,
                const std::string& rendererId,
                PXR_NS::UsdTimeCode time);

/**
 * @brief The stage's active render settings prim: the one at overridePath when it is not empty,
 * else the one the stage's renderSettingsPrimPath metadata names. Invalid when neither names one,
 * with *errorMessage set when a path names no UsdRenderSettings prim.
 **/
PXR_NS::UsdRenderSettings activePrim(const PXR_NS::UsdStageRefPtr& stage,
                                     const std::string& overridePath,
                                     std::string* errorMessage);

// The UsdRenderSettingsBase attributes a render takes from the prim.
struct StandardAttributes {
    // Empty when the camera relationship has no target.
    PXR_NS::SdfPath camera;
    PXR_NS::GfVec2i resolution = PXR_NS::GfVec2i(2048, 1080);
    float pixelAspectRatio = 1.f;
    PXR_NS::TfToken aspectRatioConformPolicy;
    PXR_NS::GfVec4f dataWindowNDC = PXR_NS::GfVec4f(0.f, 0.f, 1.f, 1.f);
    bool disableMotionBlur = false;
    bool disableDepthOfField = false;
};

// The prim's values, the schema's fallbacks for those it does not author.
StandardAttributes standardAttributes(const PXR_NS::UsdRenderSettings& settings);

/**
 * @brief The renderer the prim's natron:renderer attribute names. True with *rendererId empty when
 * it authors none; false with *errorMessage naming the id when no Hydra renderer plug-in has it.
 **/
bool rendererFor(const PXR_NS::UsdPrim& prim,
                 std::string* rendererId,
                 std::string* errorMessage);
} // namespace RenderSettingsPrim

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_RenderSettingsPrim_h
