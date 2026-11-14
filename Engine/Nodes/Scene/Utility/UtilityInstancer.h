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

#ifndef Engine_Nodes_Scene_Utility_UtilityInstancer_h
#define Engine_Nodes_Scene_Utility_UtilityInstancer_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include <vector>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/quatd.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/vt/types.h>
#include <pxr/imaging/hd/instancer.h>
#include <pxr/imaging/hd/timeSampleArray.h>

#include "Global/Macros.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief A PointInstancer or native instance's placements, flattened through any nesting into one
 * transform per placement of a prototype.
 **/
class UtilityInstancer : public PXR_NS::HdInstancer {
public:
    UtilityInstancer(PXR_NS::HdSceneDelegate* delegate,
                     const PXR_NS::SdfPath& id);
    virtual ~UtilityInstancer();

    virtual void Sync(PXR_NS::HdSceneDelegate* delegate,
                      PXR_NS::HdRenderParam* renderParam,
                      PXR_NS::HdDirtyBits* dirtyBits) OVERRIDE;

    /**
     * @brief The world transform of every placement of prototypeId, empty while this instancer or
     * one above it is hidden.
     **/
    PXR_NS::VtMatrix4dArray computeInstanceTransforms(const PXR_NS::SdfPath& prototypeId);

    /**
     * @brief The frame-relative times, within the transform motion blur shutter and increasing, at
     * which the placements of prototypeId (this instancer's and those above it) have samples, the
     * shutter's ends included. Empty when they do not move within it.
     **/
    std::vector<float> motionTimes(const PXR_NS::SdfPath& prototypeId);

    // As computeInstanceTransforms, at a frame-relative time within the shutter.
    PXR_NS::VtMatrix4dArray computeInstanceTransformsAt(const PXR_NS::SdfPath& prototypeId,
                                                        float time);

private:
    void syncPrimvars(PXR_NS::HdSceneDelegate* delegate,
                      PXR_NS::HdDirtyBits dirtyBits);
    void sampleMotion(PXR_NS::HdSceneDelegate* delegate);
    PXR_NS::VtMatrix4dArray placements(const PXR_NS::SdfPath& prototypeId,
                                       const float* time);

    bool _visible = true;
    std::vector<PXR_NS::GfVec3d> _translations;
    std::vector<PXR_NS::GfQuatd> _rotations;
    std::vector<PXR_NS::GfVec3d> _scales;
    std::vector<PXR_NS::GfMatrix4d> _transforms;
    // Over the shutter, while transform motion blur is on; no samples otherwise.
    PXR_NS::GfVec2d _shutter = PXR_NS::GfVec2d(0.);
    PXR_NS::HdTimeSampleArray<PXR_NS::VtValue, 4> _translationSamples;
    PXR_NS::HdTimeSampleArray<PXR_NS::VtValue, 4> _rotationSamples;
    PXR_NS::HdTimeSampleArray<PXR_NS::VtValue, 4> _scaleSamples;
    PXR_NS::HdTimeSampleArray<PXR_NS::VtValue, 4> _transformSamples;
    PXR_NS::HdTimeSampleArray<PXR_NS::GfMatrix4d, 4> _instancerTransformSamples;
};

/**
 * @brief times clamped to shutter, with its ends, sorted and without repeats: the times a prim
 * moving by samples at times is evaluated at. Empty when times is or the shutter is closed.
 **/
std::vector<float> utilityMotionTimes(std::vector<float> times,
                                      const PXR_NS::GfVec2d& shutter);

// samples at time, rotations slerped between the samples around it and held beyond them.
PXR_NS::GfMatrix4d utilityTransformAt(const PXR_NS::HdTimeSampleArray<PXR_NS::GfMatrix4d, 4>& samples,
                                      float time);

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_Utility_UtilityInstancer_h
