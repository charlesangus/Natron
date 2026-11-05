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

#include "SceneProjectionCameras.h"

#include <pxr/usd/usd/primRange.h>

#include "Engine/Nodes/Scene/USDProject3D.h"

NATRON_NAMESPACE_ENTER

namespace SceneProjectionCameras {
bool
transformMightVary(const PXR_NS::UsdPrim& prim)
{
    for (PXR_NS::UsdPrim p = prim; p && !p.IsPseudoRoot(); p = p.GetParent()) {
        const PXR_NS::UsdGeomXformable xformable(p);
        if (xformable && xformable.TransformMightBeTimeVarying()) {
            return true;
        }
    }

    return false;
}

namespace {
    bool
    attributeMightVary(const PXR_NS::UsdAttribute& attribute)
    {
        return attribute && attribute.ValueMightBeTimeVarying();
    }

    // Clip space x and y from -1..1 to 0..1 of the frame, w kept: (0, 0) at its bottom left.
    const PXR_NS::GfMatrix4d kClipToFrame(0.5, 0., 0., 0.,
                                          0., 0.5, 0., 0.,
                                          0., 0., 1., 0.,
                                          0.5, 0.5, 0., 1.);

    struct Projection {
        PXR_NS::SdfPath shader;
        // Empty for one value, authored as the default.
        std::vector<PXR_NS::UsdTimeCode> times;
        std::vector<PXR_NS::GfMatrix4d> worldToProjector;
        std::vector<PXR_NS::GfVec4f> origin;
    };

    template <typename T>
    T
    getValue(const PXR_NS::UsdPrim& prim,
             const char* name,
             const T& fallback)
    {
        T value = fallback;
        const PXR_NS::UsdAttribute attribute = prim.GetAttribute(PXR_NS::TfToken(name));
        if (!attribute || !attribute.Get(&value)) {
            return fallback;
        }

        return value;
    }

    bool
    setProjection(const PXR_NS::SdfLayerRefPtr& layer,
                  const Projection& projection)
    {
        const PXR_NS::SdfPrimSpecHandle owner = PXR_NS::SdfCreatePrimInLayer(layer, projection.shader);
        if (!owner) {
            return false;
        }
        const PXR_NS::SdfAttributeSpecHandle matrix = PXR_NS::SdfAttributeSpec::New(owner, std::string("inputs:") + kUSDProject3DInputWorldToProjector, PXR_NS::SdfValueTypeNames->Matrix4d);
        const PXR_NS::SdfAttributeSpecHandle origin = PXR_NS::SdfAttributeSpec::New(owner, std::string("inputs:") + kUSDProject3DInputProjectorOrigin, PXR_NS::SdfValueTypeNames->Float4);
        if (!matrix || !origin) {
            return false;
        }
        if (projection.times.empty()) {
            return matrix->SetDefaultValue(PXR_NS::VtValue(projection.worldToProjector.front())) && origin->SetDefaultValue(PXR_NS::VtValue(projection.origin.front()));
        }
        for (std::size_t i = 0; i < projection.times.size(); ++i) {
            layer->SetTimeSample(matrix->GetPath(), projection.times[i].GetValue(), projection.worldToProjector[i]);
            layer->SetTimeSample(origin->GetPath(), projection.times[i].GetValue(), projection.origin[i]);
        }

        return true;
    }
} // anonymous namespace

bool
cameraMightVary(const PXR_NS::UsdGeomCamera& camera)
{
    return transformMightVary(camera.GetPrim())
        || attributeMightVary(camera.GetProjectionAttr())
        || attributeMightVary(camera.GetFocalLengthAttr())
        || attributeMightVary(camera.GetHorizontalApertureAttr())
        || attributeMightVary(camera.GetVerticalApertureAttr())
        || attributeMightVary(camera.GetHorizontalApertureOffsetAttr())
        || attributeMightVary(camera.GetVerticalApertureOffsetAttr())
        || attributeMightVary(camera.GetClippingRangeAttr());
}

bool
computeProjector(const PXR_NS::UsdStageRefPtr& stage,
                 const PXR_NS::SdfPath& cameraPath,
                 PXR_NS::UsdTimeCode time,
                 double aspect,
                 HydraRender::FitModeEnum fit,
                 PXR_NS::GfMatrix4d* worldToProjector,
                 PXR_NS::GfVec4f* origin,
                 std::string* errorMessage)
{
    PXR_NS::GfFrustum frustum;
    if (!HydraRender::cameraFrustum(stage, cameraPath, time, aspect, fit, &frustum, errorMessage)) {
        return false;
    }
    *worldToProjector = frustum.ComputeViewMatrix() * frustum.ComputeProjectionMatrix() * kClipToFrame;
    if (frustum.GetProjectionType() == PXR_NS::GfFrustum::Orthographic) {
        const PXR_NS::GfVec3d back = -frustum.ComputeViewDirection();
        *origin = PXR_NS::GfVec4f((float)back[0], (float)back[1], (float)back[2], 0.f);
    } else {
        const PXR_NS::GfVec3d position = frustum.GetPosition();
        *origin = PXR_NS::GfVec4f((float)position[0], (float)position[1], (float)position[2], 1.f);
    }

    return true;
}

void
resolve(const PXR_NS::UsdStageRefPtr& stage,
        const PXR_NS::SdfLayerRefPtr& layer,
        std::vector<Unresolved>* unresolved)
{
    if (!stage || !layer) {
        return;
    }
    const PXR_NS::TfToken cameraName(kUSDProject3DAttrCamera);
    const double timeCodesPerSecond = stage->GetTimeCodesPerSecond();
    std::vector<Projection> projections;
    for (const PXR_NS::UsdPrim& prim : stage->Traverse()) {
        const PXR_NS::UsdAttribute cameraAttribute = prim.GetAttribute(cameraName);
        std::string camera;
        if (!cameraAttribute || !cameraAttribute.Get(&camera) || camera.empty()) {
            continue;
        }
        const PXR_NS::SdfPath cameraPath = PXR_NS::SdfPath::IsValidPathString(camera) ? PXR_NS::SdfPath(camera) : PXR_NS::SdfPath();
        const PXR_NS::UsdGeomCamera usdCamera = cameraPath.IsAbsolutePath() ? PXR_NS::UsdGeomCamera(stage->GetPrimAtPath(cameraPath)) : PXR_NS::UsdGeomCamera();
        if (!usdCamera) {
            if (unresolved) {
                unresolved->push_back(Unresolved { prim.GetPath(), camera });
            }
            continue;
        }
        const double aspect = getValue<double>(prim, kUSDProject3DAttrCameraAspect, 1.);
        const int fitValue = getValue<int>(prim, kUSDProject3DAttrCameraFit, (int)HydraRender::eFitModeFill);
        const HydraRender::FitModeEnum fit = (fitValue == (int)HydraRender::eFitModeHorizontal) ? HydraRender::eFitModeHorizontal : (fitValue == (int)HydraRender::eFitModeVertical) ? HydraRender::eFitModeVertical
                                                                                                                                                                                     : HydraRender::eFitModeFill;
        const PXR_NS::VtDoubleArray frames = getValue<PXR_NS::VtDoubleArray>(prim, kUSDProject3DAttrCameraFrames, PXR_NS::VtDoubleArray());
        const double framesPerSecond = getValue<double>(prim, kUSDProject3DAttrCameraFramesPerSecond, 0.);
        const auto toTime = [timeCodesPerSecond, framesPerSecond](double frame) {
            return PXR_NS::UsdTimeCode((framesPerSecond > 0.) ? frame * timeCodesPerSecond / framesPerSecond : frame);
        };

        Projection projection;
        projection.shader = prim.GetPath();
        std::vector<PXR_NS::UsdTimeCode> readTimes;
        if (frames.empty()) {
            readTimes.push_back(PXR_NS::UsdTimeCode::Default());
        } else if ((frames.size() == 1) || !cameraMightVary(usdCamera)) {
            readTimes.push_back(toTime(frames[0]));
        } else {
            for (double frame : frames) {
                projection.times.push_back(toTime(frame));
            }
            readTimes = projection.times;
        }
        bool ok = true;
        for (const PXR_NS::UsdTimeCode& time : readTimes) {
            PXR_NS::GfMatrix4d matrix;
            PXR_NS::GfVec4f origin;
            std::string error;
            if (!computeProjector(stage, cameraPath, time, aspect, fit, &matrix, &origin, &error)) {
                ok = false;
                break;
            }
            projection.worldToProjector.push_back(matrix);
            projection.origin.push_back(origin);
        }
        if (!ok) {
            if (unresolved) {
                unresolved->push_back(Unresolved { prim.GetPath(), camera });
            }
            continue;
        }
        projections.push_back(std::move(projection));
    }
    if (projections.empty()) {
        return;
    }
    // The samples are in the stage's time codes, which the layer's own rate keeps unscaled.
    layer->SetTimeCodesPerSecond(timeCodesPerSecond);
    for (const Projection& projection : projections) {
        setProjection(layer, projection);
    }
} // resolve
} // namespace SceneProjectionCameras

NATRON_NAMESPACE_EXIT
