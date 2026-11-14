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

#ifndef Engine_Nodes_Scene_USDEmissiveMaterial_h
#define Engine_Nodes_Scene_USDEmissiveMaterial_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"

// Free of USD: NativeRegistryAll.inc includes every node header into translation units that keep
// Py_LIMITED_API, which USD's headers cannot be compiled under.

#define PLUGINID_NATRON_USDEMISSIVEMATERIAL "USDEmissiveMaterial"

#define kUSDEmissiveMaterialParamEmissiveColor "emissiveColor"
#define kUSDEmissiveMaterialParamEmissiveTexture "emissiveTexture"
#define kUSDEmissiveMaterialParamUseAlpha "useAlpha"

// The one image input, whose image the surface shows.
#define kUSDEmissiveMaterialInputEmissive 0

NATRON_NAMESPACE_ENTER

struct SceneLayerEdit;
struct LiveTextureSource;

/**
 * @brief Authors an unlit material at its prim path (the node's name under the root by default),
 * as a scene source: a UsdPreviewSurface that gives off its emission unchanged by any light, with
 * no diffuse or specular response, as an image projected or shown on a card wants. A geometry node
 * binds it by taking it on its Material input, as it does a USDMaterial.
 *
 * The emission is the image connected to the Emissive input, live (a natron:// URI per frame, or
 * one for an image that does not change over time); else the Emissive Texture file; else Emissive
 * Color alone. With an image or a file, Emissive Color multiplies it, white by default, so the
 * image shows as it is.
 **/
class USDEmissiveMaterial
    : public NativeEffectBase {
public:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Emissive", .kind = eDataKindImage, .role = InputRole::Aux, .optional = true, .time = InputTime::Any },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_USDEMISSIVEMATERIAL,
        .label = "USDEmissiveMaterial",
        .description = "Author an unlit material that shows an image as it is, whatever the lights: no diffuse or "
                       "specular response, only emission. For images on cards and projections. Connect the image "
                       "to the Emissive input, live and animated with no file on disk, or set Emissive Texture to "
                       "a file; Emissive Color multiplies it, or is the emission alone when neither is set. Connect "
                       "the node to the Material input of a geometry node, which places the material under its own "
                       "prim and binds itself to it.",
        .grouping = PLUGIN_GROUP_3D_USD,
        .majorVersion = 1,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/USDEmissiveMaterial.png",
        .isWriter = false,
        .outputKind = eDataKindScene,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new USDEmissiveMaterial(node);
    }

    explicit USDEmissiveMaterial(NodePtr node);

    virtual ~USDEmissiveMaterial();

private:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;
    virtual void onInputSourceDeleted(int inputNo,
                                      const std::string& sourceName) OVERRIDE FINAL;
    virtual void onScriptNameChanged(const std::string& fullyQualifiedName) OVERRIDE FINAL;
    virtual StatusEnum renderScene(const SceneRenderActionArgs& args,
                                   ScenePayload* outputScene,
                                   std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // Authors the material into edit's layer, its emission from live when that has a node, else
    // from the texture file or the colour.
    bool authorLayer(const SceneLayerEdit& edit,
                     const LiveTextureSource& live,
                     std::string* errorMessage) const;

    KnobStringWPtr _primPath;
    KnobColorWPtr _emissiveColor;
    KnobFileWPtr _emissiveTexture;
    KnobBoolWPtr _useAlpha;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_USDEmissiveMaterial_h
