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

#include "Engine/ContentHasher.h"
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
        const PXR_NS::SdfPath material = projection.shader.GetParentPath();
        std::vector<std::vector<ShaderValue>> samples;
        for (std::size_t i = 0; i < projection.worldToProjector.size(); ++i) {
            samples.push_back(projectorValues(projection.worldToProjector[i], projection.origin[i]));
        }
        if (samples.empty()) {
            return false;
        }
        for (std::size_t v = 0; v < samples.front().size(); ++v) {
            const ShaderValue& value = samples.front()[v];
            const PXR_NS::SdfPrimSpecHandle owner = PXR_NS::SdfCreatePrimInLayer(layer, material.AppendChild(value.shader));
            const PXR_NS::SdfAttributeSpecHandle attribute = owner ? PXR_NS::SdfAttributeSpec::New(owner, "inputs:" + value.input.GetString(), value.type) : PXR_NS::SdfAttributeSpecHandle();
            if (!attribute) {
                return false;
            }
            if (projection.times.empty()) {
                if (!attribute->SetDefaultValue(value.value)) {
                    return false;
                }
                continue;
            }
            for (std::size_t i = 0; i < projection.times.size(); ++i) {
                layer->SetTimeSample(attribute->GetPath(), projection.times[i].GetValue(), samples[i][v].value);
            }
        }

        return true;
    }
} // anonymous namespace

std::string
cyclesProjectorDot(int component)
{
    return std::string("CyclesProjectorDot") + "XYZW"[component & 3];
}

std::string
cyclesProjectorOffset(int component)
{
    return std::string("CyclesProjectorOffset") + "XYZW"[component & 3];
}

std::string
cyclesFrameColumn(int column)
{
    return "CyclesFrameColumn" + std::to_string(column);
}

std::string
moonRayProjectorDot(int component)
{
    return std::string("MoonRayProjectorDot") + "XYZW"[component & 3];
}

std::string
moonRayProjectorOffset(int component)
{
    return std::string("MoonRayProjectorOffset") + "XYZW"[component & 3];
}

std::string
moonRayFrameReader(int part)
{
    return "MoonRayFramePart" + std::to_string(part);
}

std::vector<ShaderValue>
projectorValues(const PXR_NS::GfMatrix4d& worldToProjector,
                const PXR_NS::GfVec4f& origin)
{
    const PXR_NS::TfToken surface(kUSDProject3DShaderName);
    std::vector<ShaderValue> values = {
        { surface, PXR_NS::TfToken(kUSDProject3DInputWorldToProjector), PXR_NS::SdfValueTypeNames->Matrix4d, PXR_NS::VtValue(worldToProjector) },
        { surface, PXR_NS::TfToken(kUSDProject3DInputProjectorOrigin), PXR_NS::SdfValueTypeNames->Float4, PXR_NS::VtValue(origin) },
    };
    // Row vectors: component k of p * worldToProjector is p's dot with column k plus row 3's k.
    for (int k = 0; k < 4; ++k) {
        const PXR_NS::GfVec3f column((float)worldToProjector[0][k], (float)worldToProjector[1][k], (float)worldToProjector[2][k]);
        values.push_back({ PXR_NS::TfToken(cyclesProjectorDot(k)), PXR_NS::TfToken("vector2"), PXR_NS::SdfValueTypeNames->Vector3f, PXR_NS::VtValue(column) });
        values.push_back({ PXR_NS::TfToken(cyclesProjectorOffset(k)), PXR_NS::TfToken("value2"), PXR_NS::SdfValueTypeNames->Float, PXR_NS::VtValue((float)worldToProjector[3][k]) });
        values.push_back({ PXR_NS::TfToken(moonRayProjectorDot(k)), PXR_NS::TfToken("op2"), PXR_NS::SdfValueTypeNames->Color3f, PXR_NS::VtValue(column) });
        values.push_back({ PXR_NS::TfToken(moonRayProjectorOffset(k)), PXR_NS::TfToken("op2"), PXR_NS::SdfValueTypeNames->Color3f, PXR_NS::VtValue(PXR_NS::GfVec3f((float)worldToProjector[3][k])) });
    }
    // origin.xyz - P * origin.w, toward the camera's position or along its view when orthographic.
    const PXR_NS::TfToken toward(kSceneProjectionCyclesToward);
    values.push_back({ toward, PXR_NS::TfToken("vector2"), PXR_NS::SdfValueTypeNames->Vector3f, PXR_NS::VtValue(PXR_NS::GfVec3f(-origin[3])) });
    values.push_back({ toward, PXR_NS::TfToken("vector3"), PXR_NS::SdfValueTypeNames->Vector3f, PXR_NS::VtValue(PXR_NS::GfVec3f(origin[0], origin[1], origin[2])) });
    const PXR_NS::TfToken moonRayToward(kSceneProjectionMoonRayToward);
    values.push_back({ moonRayToward, PXR_NS::TfToken("op1_factor"), PXR_NS::SdfValueTypeNames->Float, PXR_NS::VtValue(-origin[3]) });
    values.push_back({ moonRayToward, PXR_NS::TfToken("op2"), PXR_NS::SdfValueTypeNames->Color3f, PXR_NS::VtValue(PXR_NS::GfVec3f(origin[0], origin[1], origin[2])) });

    return values;
}

std::vector<ShaderValue>
plateValues(const PXR_NS::GfVec4f& plateTransform)
{
    const PXR_NS::TfToken window(kSceneProjectionCyclesPlateTransform);

    return {
        { PXR_NS::TfToken(kUSDProject3DShaderName), PXR_NS::TfToken(kUSDProject3DInputPlateTransform), PXR_NS::SdfValueTypeNames->Float4, PXR_NS::VtValue(plateTransform) },
        { window, PXR_NS::TfToken("vector2"), PXR_NS::SdfValueTypeNames->Vector3f, PXR_NS::VtValue(PXR_NS::GfVec3f(plateTransform[0], plateTransform[1], 0.f)) },
        { window, PXR_NS::TfToken("vector3"), PXR_NS::SdfValueTypeNames->Vector3f, PXR_NS::VtValue(PXR_NS::GfVec3f(plateTransform[2], plateTransform[3], 0.f)) },
        { PXR_NS::TfToken(kSceneProjectionMoonRayPlateScale), PXR_NS::TfToken("op2"), PXR_NS::SdfValueTypeNames->Color3f, PXR_NS::VtValue(PXR_NS::GfVec3f(plateTransform[0], plateTransform[1], 0.f)) },
        { PXR_NS::TfToken(kSceneProjectionMoonRayPlateOffset), PXR_NS::TfToken("op2"), PXR_NS::SdfValueTypeNames->Color3f, PXR_NS::VtValue(PXR_NS::GfVec3f(plateTransform[2], plateTransform[3], 0.f)) },
    };
}

std::string
frameColumnPrimvar(const std::string& framePrimvar,
                   int column)
{
    return framePrimvar + "Column" + std::to_string(column);
}

PXR_NS::VtVec4fArray
frameColumn(const PXR_NS::GfMatrix4d& frame,
            int column)
{
    PXR_NS::GfVec4f value((float)frame[0][column], (float)frame[1][column], (float)frame[2][column], (float)frame[3][column]);
    if (column < 3) {
        value[column] -= 1.f;
    }

    return PXR_NS::VtVec4fArray(1, value);
}

std::string
moonRayFramePrimvar(const std::string& framePrimvar,
                    int part)
{
    return framePrimvar + "MoonRay" + std::to_string(part);
}

std::string
cyclesReferencePresentPrimvar(const std::string& referencePrimvar)
{
    // Not under the reference points' own prefix, which names the per-vertex primvars only.
    const std::string prefix("natronReferencePoints");
    const std::string hold = (referencePrimvar.compare(0, prefix.size(), prefix) == 0) ? referencePrimvar.substr(prefix.size()) : referencePrimvar;

    return "natronReferencePresent" + hold;
}

PXR_NS::GfVec3f
moonRayFramePart(const PXR_NS::GfMatrix4d& frame,
                 int part)
{
    if (part >= 3) {
        return PXR_NS::GfVec3f((float)frame[3][0], (float)frame[3][1], (float)frame[3][2]);
    }
    PXR_NS::GfVec3f value((float)frame[0][part], (float)frame[1][part], (float)frame[2][part]);
    value[part] -= 1.f;

    return value;
}

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

U64
resolve(const PXR_NS::UsdStageRefPtr& stage,
        const PXR_NS::SdfLayerRefPtr& layer,
        std::vector<Unresolved>* unresolved,
        U64 skipIfDigest)
{
    if (!stage || !layer) {
        return 0;
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
        return 0;
    }
    ContentHasher hasher;
    hasher.appendDouble(timeCodesPerSecond);
    for (const Projection& projection : projections) {
        hasher.appendString(projection.shader.GetString());
        hasher.appendU64(projection.times.size());
        for (const PXR_NS::UsdTimeCode& time : projection.times) {
            hasher.appendDouble(time.GetValue());
        }
        hasher.appendU64(projection.worldToProjector.size());
        for (std::size_t i = 0; i < projection.worldToProjector.size(); ++i) {
            hasher.appendBytes(projection.worldToProjector[i].GetArray(), 16 * sizeof(double));
            hasher.appendBytes(projection.origin[i].GetArray(), 4 * sizeof(float));
        }
    }
    const U64 digest = hasher.finalize();
    if (digest == skipIfDigest) {
        return digest;
    }
    // The samples are in the stage's time codes, which the layer's own rate keeps unscaled.
    layer->SetTimeCodesPerSecond(timeCodesPerSecond);
    for (const Projection& projection : projections) {
        setProjection(layer, projection);
    }

    return digest;
} // resolve
} // namespace SceneProjectionCameras

NATRON_NAMESPACE_EXIT
