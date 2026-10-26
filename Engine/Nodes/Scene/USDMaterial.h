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

#ifndef Engine_Nodes_Scene_USDMaterial_h
#define Engine_Nodes_Scene_USDMaterial_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"

// Free of USD: NativeRegistryAll.inc includes every node header into translation units that keep
// Py_LIMITED_API, which USD's headers cannot be compiled under.

#define PLUGINID_NATRON_USDMATERIAL "USDMaterial"

#define kUSDMaterialParamPrimPattern "primPattern"
#define kUSDMaterialParamDiffuseColor "diffuseColor"
#define kUSDMaterialParamDiffuseTexture "diffuseTexture"
#define kUSDMaterialParamMetallic "metallic"
#define kUSDMaterialParamMetallicTexture "metallicTexture"
#define kUSDMaterialParamRoughness "roughness"
#define kUSDMaterialParamRoughnessTexture "roughnessTexture"
#define kUSDMaterialParamEmissiveColor "emissiveColor"
#define kUSDMaterialParamEmissiveTexture "emissiveTexture"

NATRON_NAMESPACE_ENTER

struct SceneLayerEdit;

/**
 * @brief Authors a UsdPreviewSurface material at its prim path (the node's name under the root by
 * default) and binds it to the prims a prim pattern names in its input scene. Diffuse Color,
 * Metallic, Roughness and Emissive Color set the surface's inputs; each has a texture file that,
 * when set, drives the input from a UsdUVTexture read through the "st" primvar instead. Colours
 * are in the project working space and written unchanged. A texture path that does not resolve
 * is authored anyway, with a node warning naming it; a pattern that matches no prim authors the
 * material unbound, with a node warning.
 **/
class USDMaterial
    : public NativeEffectBase {
public:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Scene", .kind = eDataKindScene, .optional = true },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_USDMATERIAL,
        .label = "USDMaterial",
        .description = "Author a UsdPreviewSurface material (diffuse colour, metallic, roughness and emissive colour, each "
                       "optionally from a texture file) and bind it to the prims a pattern names (paths, with * and ? "
                       "wildcards) in the input scene. Colours are in the project working space and written unchanged.",
        .grouping = PLUGIN_GROUP_3D_USD,
        .majorVersion = 1,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/USDMaterial.png",
        .isWriter = false,
        .outputKind = eDataKindScene,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new USDMaterial(node);
    }

    explicit USDMaterial(NodePtr node);

    virtual ~USDMaterial();

private:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void onScriptNameChanged(const std::string& fullyQualifiedName) OVERRIDE FINAL;
    virtual StatusEnum renderScene(const SceneRenderActionArgs& args,
                                   ScenePayload* outputScene,
                                   std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // The path a texture file knob holds, anchored (SceneNodeAuthoring::getAnchoredFilePath), empty for none.
    static std::string getTexturePath(const KnobFilePtr& knob);

    // Authors the material, and its binding to each of bindPaths, into edit's layer.
    bool authorLayer(const SceneLayerEdit& edit,
                     const std::vector<std::string>& bindPaths,
                     std::string* errorMessage) const;

    KnobStringWPtr _primPath;
    KnobStringWPtr _primPattern;
    KnobColorWPtr _diffuseColor;
    KnobFileWPtr _diffuseTexture;
    KnobDoubleWPtr _metallic;
    KnobFileWPtr _metallicTexture;
    KnobDoubleWPtr _roughness;
    KnobFileWPtr _roughnessTexture;
    KnobColorWPtr _emissiveColor;
    KnobFileWPtr _emissiveTexture;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_USDMaterial_h
