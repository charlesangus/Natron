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

#ifndef Engine_Nodes_Scene_Utility_UtilityMaterial_h
#define Engine_Nodes_Scene_Utility_UtilityMaterial_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include <memory>
#include <string>
#include <vector>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/imaging/hd/material.h>

#include "Global/Macros.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief One UsdUVTexture read, with the UsdTransform2d and UsdPrimvarReader_float2 before it.
 **/
struct UtilityTextureRef {
    // The values of Kernels/UtilityTypes.slang's kWrap constants.
    enum Wrap {
        eWrapBlack,
        eWrapClamp,
        eWrapRepeat,
        eWrapMirror,
    };

    enum ColorSpace {
        eColorSpaceAuto,
        eColorSpaceRaw,
        eColorSpaceSRGB,
    };

    // The resolved path or URI; empty when no file is authored. May hold <UDIM>.
    std::string asset;
    PXR_NS::GfVec4f scale = PXR_NS::GfVec4f(1.f);
    PXR_NS::GfVec4f bias = PXR_NS::GfVec4f(0.f);
    PXR_NS::GfVec4f fallback = PXR_NS::GfVec4f(0.f, 0.f, 0.f, 1.f);
    PXR_NS::GfVec2f uvScale = PXR_NS::GfVec2f(1.f);
    PXR_NS::GfVec2f uvTranslation = PXR_NS::GfVec2f(0.f);
    float uvRotationDegrees = 0.f;
    Wrap wrapS = eWrapBlack;
    Wrap wrapT = eWrapBlack;
    ColorSpace colorSpace = eColorSpaceAuto;
    // The channel the output starts at: 0 for r, rgb and rgba, 1 for g, and so on.
    int firstChannel = 0;
    std::string primvar = "st";
};

/**
 * @brief The inputs of natronProject3D.glslfx, the surface USDProject3D authors.
 **/
struct UtilityProjection {
    PXR_NS::GfMatrix4d worldToProjector = PXR_NS::GfMatrix4d(1.);
    PXR_NS::GfMatrix4d geometryFrame = PXR_NS::GfMatrix4d(1.);
    PXR_NS::GfVec4f plateTransform = PXR_NS::GfVec4f(1.f, 1.f, 0.f, 0.f);
    PXR_NS::GfVec4f projectorOrigin = PXR_NS::GfVec4f(0.f, 0.f, 0.f, 1.f);
    int projectionSpace = 0;
    int projectOn = 0;
    int crop = 1;
    int clip = 0;
    int outside = 0;
    // Into UtilityMaterialDesc::textureRefs, -1 without a plate.
    int plate = -1;
};

/**
 * @brief A material as the utility renderer shades it: the UsdPreviewSurface subset as constants and
 * texture reads, USDProject3D's projection, or nothing it understands.
 **/
struct UtilityMaterialDesc {
    // The values of Kernels/UtilityTypes.slang's kMaterial kinds.
    enum Kind {
        eKindPreviewSurface,
        eKindUnsupported,
        eKindProject3D,
    };

    // The values of Kernels/UtilityTypes.slang's kSlot constants.
    enum Slot {
        eSlotDiffuseColor,
        eSlotEmissiveColor,
        eSlotSpecularColor,
        eSlotMetallic,
        eSlotRoughness,
        eSlotClearcoat,
        eSlotClearcoatRoughness,
        eSlotOpacity,
        eSlotNormal,
        eSlotIor,
        eSlotCount,
    };

    Kind kind = eKindPreviewSurface;
    PXR_NS::GfVec3f diffuseColor = PXR_NS::GfVec3f(0.18f);
    PXR_NS::GfVec3f emissiveColor = PXR_NS::GfVec3f(0.f);
    PXR_NS::GfVec3f specularColor = PXR_NS::GfVec3f(0.f);
    bool useSpecularWorkflow = false;
    float metallic = 0.f;
    float roughness = 0.5f;
    float clearcoat = 0.f;
    float clearcoatRoughness = 0.01f;
    float opacity = 1.f;
    float opacityThreshold = 0.f;
    float ior = 1.5f;
    // Inputs read from the displayColor and displayOpacity primvars, as for geometry with no material.
    bool diffuseFromDisplayColor = false;
    bool emissiveFromDisplayColor = false;
    bool opacityFromDisplayOpacity = false;
    // Into textureRefs, -1 for the constant.
    int textures[eSlotCount] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
    std::vector<UtilityTextureRef> textureRefs;
    UtilityProjection projection;
    // Why the material is unsupported: the shader ids not understood.
    std::string unsupportedReason;

    // What geometry without a material shades as: displayColor and displayOpacity through the
    // UsdPreviewSurface defaults.
    static const UtilityMaterialDesc& fallback();

    // The texture coordinate primvar the textures read.
    std::string stPrimvar() const;
    bool readsTextureCoordinates() const;
};

/**
 * @brief Folds the network from its surface terminal into a material record. Anything outside the
 * UsdPreviewSurface subset or USDProject3D's surface makes it unsupported.
 **/
UtilityMaterialDesc utilityMaterialFromNetwork(const PXR_NS::HdMaterialNetwork2& network);

// Whether a shader node is natronProject3D.glslfx, which has no shader id of its own: Sdr names a
// source-asset node by a hash of its asset path.
bool utilityIsProject3DShader(const PXR_NS::TfToken& nodeTypeId);

/**
 * @brief The material Sprim: hands its record to the UtilityScene each sync.
 **/
class UtilityMaterial : public PXR_NS::HdMaterial {
public:
    explicit UtilityMaterial(const PXR_NS::SdfPath& id);
    virtual ~UtilityMaterial();

    virtual void Sync(PXR_NS::HdSceneDelegate* sceneDelegate,
                      PXR_NS::HdRenderParam* renderParam,
                      PXR_NS::HdDirtyBits* dirtyBits) OVERRIDE;
    virtual PXR_NS::HdDirtyBits GetInitialDirtyBitsMask() const OVERRIDE;
    virtual void Finalize(PXR_NS::HdRenderParam* renderParam) OVERRIDE;

    // The texture coordinate primvar of the last sync, which meshes bound to it read.
    const std::string& stPrimvar() const;

private:
    std::string _stPrimvar = "st";
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_Utility_UtilityMaterial_h
