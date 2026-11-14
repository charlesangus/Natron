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

#ifndef Engine_Nodes_Scene_Utility_UtilityRendererPlugin_h
#define Engine_Nodes_Scene_Utility_UtilityRendererPlugin_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include <string>

#include <pxr/imaging/hd/rendererPlugin.h>
#include <pxr/imaging/hd/sceneIndexPlugin.h>

#include "Global/Macros.h"

// These two sit outside Natron's namespace because Hydra's plug-in id is the TfType name, which
// plugInfo.json declares and kRenderDelegateUtilityId repeats.

/**
 * @brief Makes the Natron Utility render delegate. It needs no GPU, so it is supported wherever
 * Embree can open a device.
 **/
class HdNatronUtilityRendererPlugin : public PXR_NS::HdRendererPlugin {
public:
    HdNatronUtilityRendererPlugin();
    virtual ~HdNatronUtilityRendererPlugin();

    virtual PXR_NS::HdRenderDelegate* CreateRenderDelegate() OVERRIDE;
    virtual PXR_NS::HdRenderDelegate* CreateRenderDelegate(const PXR_NS::HdRenderSettingsMap& settings) OVERRIDE;
    virtual void DeleteRenderDelegate(PXR_NS::HdRenderDelegate* delegate) OVERRIDE;
    virtual bool IsSupported(const PXR_NS::HdRendererCreateArgsSchema& rendererCreateArgs,
                             std::string* reasonWhyNot = nullptr) const OVERRIDE;
    virtual PXR_NS::HdContainerDataSourceHandle GetSceneIndexCreateArgs() const OVERRIDE;
};

/**
 * @brief Tessellates implicit prims (Cube, Sphere and the like) for the delegate, whose only rprim
 * is the mesh.
 **/
class HdNatronUtilityImplicitSurfaceSceneIndexPlugin : public PXR_NS::HdSceneIndexPlugin {
public:
    HdNatronUtilityImplicitSurfaceSceneIndexPlugin();

protected:
    virtual PXR_NS::HdSceneIndexBaseRefPtr _AppendSceneIndex(const PXR_NS::HdSceneIndexBaseRefPtr& inputScene,
                                                             const PXR_NS::HdContainerDataSourceHandle& inputArgs) OVERRIDE;
};

/**
 * @brief Resolves the lights' light-link and shadow-link collections into category tokens on the lights
 * and the prims they match, which the delegate reads through GetCategories and the light's params.
 **/
class HdNatronUtilityLightLinkingSceneIndexPlugin : public PXR_NS::HdSceneIndexPlugin {
public:
    HdNatronUtilityLightLinkingSceneIndexPlugin();

protected:
    virtual PXR_NS::HdSceneIndexBaseRefPtr _AppendSceneIndex(const PXR_NS::HdSceneIndexBaseRefPtr& inputScene,
                                                             const PXR_NS::HdContainerDataSourceHandle& inputArgs) OVERRIDE;
};

NATRON_NAMESPACE_ENTER

/**
 * @brief Defines the renderer's plug-in types, whose factories live in this executable. Must run
 * before plugInfo.json is registered and before Hydra discovers its renderers.
 **/
void defineUtilityRendererTypes();

/**
 * @brief Adds the renderer's scene indices to Hydra's chain. Must run after plugInfo.json is registered:
 * the scene index registry discovers its plug-ins once, when first touched, and drops any type whose
 * metadata is not known by then.
 **/
void registerUtilitySceneIndexPlugins();

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_Utility_UtilityRendererPlugin_h
