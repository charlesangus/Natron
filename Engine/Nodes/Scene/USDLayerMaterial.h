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

#ifndef Engine_Nodes_Scene_USDLayerMaterial_h
#define Engine_Nodes_Scene_USDLayerMaterial_h

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
#include "Engine/Nodes/Scene/PrimSelector.h"

// Free of USD, like every node header.

#define PLUGINID_NATRON_USDLAYERMATERIAL "USDLayerMaterial"

#define kUSDLayerMaterialParamPrimPattern "primPattern"
#define kUSDLayerMaterialParamMaterialPurpose "materialPurpose"
#define kUSDLayerMaterialParamMix "mix"
// Each mixable input's knob is named <input>Layer, such as diffuseColorLayer.
#define kUSDLayerMaterialParamLayerSuffix "Layer"

// The entries of an <input>Layer menu besides the Paint input's planes, which are listed by ID.
#define kUSDLayerMaterialLayerFromMetadata "fromMetadata"
#define kUSDLayerMaterialLayerNone "none"

// The specialized material is a sibling of the original named <original><suffix>.
#define kUSDLayerMaterialPathSuffix "_layered"
// On a layered material: how many layers it stacks over the original.
#define kUSDLayerMaterialAttrLayerDepth "natron:layerDepth"
// The texture of layer <k> (from 1) over input <input> is the shader Layer<k>_<input> under the material.
#define kUSDLayerMaterialTexturePrefix "Layer"
// The layered terminal of a layered UsdPreviewSurface material.
#define kUSDLayerMaterialSurfaceName "NatronLayeredSurface"
#define kUSDLayerMaterialShaderFile "natronLayeredPreviewSurface.glslfx"

#define kUSDLayerMaterialInputScene 0
#define kUSDLayerMaterialInputPaint 1

NATRON_NAMESPACE_ENTER

/**
 * @brief Lays the paint of its Paint input over the materials bound to the prims a pattern
 * matches, without baking. For each bound material it authors <material>_layered beside it, which
 * specializes the original, and binds the matched geometry to it through a collection binding on
 * their root prim, the strongest there; prims the pattern leaves out keep the original.
 *
 * A UsdPreviewSurface material gains a glslfx terminal (natronLayeredPreviewSurface.glslfx) taking
 * the original surface's inputs and, per mixable input, a premultiplied paint texture mixed over
 * it before lighting. A USDProject3D material lays the paint over its plate. The universal surface
 * stays the original's, which every other renderer reads, and MaterialBake evaluates the layers
 * over it. A layered material can be layered again; Storm then shows only the latest layer of an
 * input two layers paint, and the node warns.
 *
 * Which plane of the Paint input lies over which input comes from the purposes USDTexturePaint
 * writes in its metadata, or from each input's <input>Layer menu.
 **/
class USDLayerMaterial
    : public NativeEffectBase {
public:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Scene", .kind = eDataKindScene, .optional = true },
        InputSpec { .label = "Paint", .kind = eDataKindImage, .role = InputRole::Aux, .optional = true, .time = InputTime::Any },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_USDLAYERMATERIAL,
        .label = "USDLayerMaterial",
        .description = "Lay paint over the materials of the prims a pattern matches, live, without baking. Connect "
                       "the scene to the Scene input and the paint, such as a USDTexturePaint in Paint Only mode, to "
                       "the Paint input: each material input takes the plane its purpose names, or the one its Layer "
                       "menu picks, mixed over the original by the paint's alpha. Works on UsdPreviewSurface and "
                       "USDProject3D materials; the original material is left as it is for the prims not matched.",
        .grouping = PLUGIN_GROUP_3D_USD,
        .majorVersion = 1,
        .minorVersion = 0,
        .isWriter = false,
        .outputKind = eDataKindScene,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new USDLayerMaterial(node);
    }

    explicit USDLayerMaterial(NodePtr node);

    virtual ~USDLayerMaterial();

    // The UsdPreviewSurface inputs paint can lie over, in menu order.
    static const std::vector<std::string>& getMixableInputs();

    /**
     * @brief The plane of the Paint input that lies over input at time: the menu's plane, or with
     * From Metadata the plane whose USDTexturePaint purpose is input. Empty for none.
     **/
    std::string getInputPlane(const std::string& input,
                              double time) const;

private:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;
    virtual void onInputSourceDeleted(int inputNo,
                                      const std::string& sourceName) OVERRIDE FINAL;
    virtual void onCurrentFrameMetadataChanged(const NodeMetadata& metadata) OVERRIDE FINAL;
    virtual StatusEnum renderScene(const SceneRenderActionArgs& args,
                                   ScenePayload* outputScene,
                                   std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // Lists the Paint input's planes in every <input>Layer menu. Main thread only.
    void refreshLayerMenus();

    PrimSelector _primPattern;
    KnobChoiceWPtr _materialPurpose;
    KnobDoubleWPtr _mix;
    // One per getMixableInputs(), in its order.
    std::vector<KnobChoiceWPtr> _layers;
    PrimPatternCache _matches;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_USDLayerMaterial_h
