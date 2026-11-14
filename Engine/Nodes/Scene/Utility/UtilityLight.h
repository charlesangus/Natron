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

#ifndef Engine_Nodes_Scene_Utility_UtilityLight_h
#define Engine_Nodes_Scene_Utility_UtilityLight_h

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
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/tf/token.h>
#include <pxr/imaging/hd/light.h>

#include "Engine/Nodes/Scene/Utility/UtilityMaterial.h"
#include "Global/Macros.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief An IES (LM-63) photometric profile resampled onto a regular grid: thetaCount polar angles
 * from 0 (the light's axis) to pi, by phiCount azimuths from 0 up to 2 pi, theta varying fastest.
 * Values are candelas times the file's multiplier.
 **/
struct UtilityIesProfile {
    int thetaCount = 0;
    int phiCount = 0;
    std::vector<float> candela;
    // The profile's average over the sphere of directions.
    float mean = 0.f;

    // Bilinear in theta, wrapping in phi; zero outside the file's angles.
    float evaluate(float theta,
                   float phi) const;
};
typedef std::shared_ptr<const UtilityIesProfile> UtilityIesProfileConstPtr;

/**
 * @brief Parses the text of an IES LM-63 file (1986, 1991, 1995 and 2002 forms) and resamples it.
 * Returns null and says why in error when the text is not a profile.
 **/
UtilityIesProfileConstPtr utilityParseIes(const std::string& text,
                                          std::string* error);

/**
 * @brief A light as the scene keeps it: its UsdLux inputs folded into what the kernel reads, in the
 * light's own space, which transform places in the world.
 **/
struct UtilityLightDesc {
    // The values of Kernels/UtilityTypes.slang's kLight constants.
    enum Type {
        eTypeDistant,
        eTypeSphere,
        eTypeRect,
        eTypeDisk,
        eTypeCylinder,
        eTypeDome,
    };

    Type type = eTypeDistant;
    PXR_NS::GfMatrix4d transform = PXR_NS::GfMatrix4d(1.);
    // intensity * 2^exposure * color, times the blackbody colour when it is enabled; normalize divides
    // it by the light's size where the scene is assembled, as that needs the transform.
    PXR_NS::GfVec3f emission = PXR_NS::GfVec3f(1.f);
    bool normalize = false;
    float angleDegrees = 0.53f;
    float radius = 0.5f;
    float width = 1.f;
    float height = 1.f;
    float length = 1.f;
    bool treatAsPoint = false;
    // 180 when the light has no ShapingAPI, which leaves its emission uncut.
    float coneAngleDegrees = 180.f;
    float coneSoftness = 0.f;
    float focus = 0.f;
    PXR_NS::GfVec3f focusTint = PXR_NS::GfVec3f(0.f);
    UtilityIesProfileConstPtr ies;
    float iesAngleScale = 0.f;
    bool iesNormalize = false;
    // The dome's lat-long image; its asset is empty for a dome of constant colour.
    UtilityTextureRef domeTexture;
    bool shadowEnable = true;
    // The category tokens of the light's light-link and shadow-link collections; empty for the trivial
    // collection that holds every prim.
    PXR_NS::TfToken lightLink;
    PXR_NS::TfToken shadowLink;
    // Why the IES file could not be used; empty when it could or there is none.
    std::string iesProblem;
};

/**
 * @brief The light Sprims, one class for the six UsdLux types: hands its record to the UtilityScene
 * each sync, and takes it out while the light is invisible.
 **/
class UtilityLight : public PXR_NS::HdLight {
public:
    UtilityLight(const PXR_NS::SdfPath& id,
                 const PXR_NS::TfToken& lightType);
    virtual ~UtilityLight();

    virtual void Sync(PXR_NS::HdSceneDelegate* sceneDelegate,
                      PXR_NS::HdRenderParam* renderParam,
                      PXR_NS::HdDirtyBits* dirtyBits) OVERRIDE;
    virtual PXR_NS::HdDirtyBits GetInitialDirtyBitsMask() const OVERRIDE;
    virtual void Finalize(PXR_NS::HdRenderParam* renderParam) OVERRIDE;

    // The Hydra light types the delegate makes these for.
    static const PXR_NS::TfTokenVector& supportedTypes();

private:
    UtilityLightDesc::Type _type;
    UtilityLightDesc _desc;
    // The resolved IES path the profile was read from, so a sync rereads it only when it changes.
    std::string _iesPath;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_Utility_UtilityLight_h
