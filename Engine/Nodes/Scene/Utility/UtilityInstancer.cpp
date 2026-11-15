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

#include "UtilityInstancer.h"

#include <algorithm>
#include <cmath>

#include <pxr/base/gf/matrix4f.h>
#include <pxr/base/gf/quatf.h>
#include <pxr/base/gf/quath.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3h.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/tf/diagnostic.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/tokens.h>

#include "Engine/Nodes/Scene/Utility/UtilityRenderDelegate.h"
#include "Engine/Nodes/Scene/Utility/UtilityScene.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
template <class In, class Out>
bool
convertArray(const VtValue& value,
             std::vector<Out>* out)
{
    if (!value.IsHolding<VtArray<In>>()) {
        return false;
    }
    const VtArray<In>& in = value.UncheckedGet<VtArray<In>>();

    out->clear();
    out->reserve(in.size());
    for (const In& element : in) {
        out->emplace_back(element);
    }

    return true;
}

void
convertVectors(const VtValue& value,
               std::vector<GfVec3d>* out)
{
    if (!convertArray<GfVec3f>(value, out) && !convertArray<GfVec3d>(value, out) && !convertArray<GfVec3h>(value, out)) {
        out->clear();
    }
}

template <class Vec4>
bool
convertVec4Rotations(const VtValue& value,
                     std::vector<GfQuatd>* out)
{
    if (!value.IsHolding<VtArray<Vec4>>()) {
        return false;
    }
    const VtArray<Vec4>& in = value.UncheckedGet<VtArray<Vec4>>();

    out->clear();
    out->reserve(in.size());
    for (const Vec4& q : in) {
        out->emplace_back(q[0], GfVec3d(q[1], q[2], q[3]));
    }

    return true;
}

// Rotations reach an instancer either as quaternions or as vectors in <real, i, j, k> order.
void
convertRotations(const VtValue& value,
                 std::vector<GfQuatd>* out)
{
    if (!convertArray<GfQuatf>(value, out) && !convertArray<GfQuatd>(value, out) && !convertArray<GfQuath>(value, out)
        && !convertVec4Rotations<GfVec4f>(value, out) && !convertVec4Rotations<GfVec4d>(value, out)) {
        out->clear();
    }
}

void
convertMatrices(const VtValue& value,
                std::vector<GfMatrix4d>* out)
{
    if (!convertArray<GfMatrix4d>(value, out) && !convertArray<GfMatrix4f>(value, out)) {
        out->clear();
    }
}

template <class T, unsigned int N>
bool
varies(const HdTimeSampleArray<T, N>& samples)
{
    for (std::size_t i = 1; i < samples.count; ++i) {
        if (!(samples.values[i] == samples.values[0])) {
            return true;
        }
    }

    return false;
}

template <class T, unsigned int N>
void
addTimes(const HdTimeSampleArray<T, N>& samples,
         std::vector<float>* times)
{
    if (varies(samples)) {
        times->insert(times->end(), samples.times.begin(), samples.times.begin() + samples.count);
    }
}
} // anon namespace

std::vector<float>
utilityMotionTimes(std::vector<float> times,
                   const GfVec2d& shutter)
{
    if (times.empty() || !(shutter[1] > shutter[0])) {
        return std::vector<float>();
    }
    const float open = static_cast<float>(shutter[0]);
    const float close = static_cast<float>(shutter[1]);

    for (float& time : times) {
        time = std::min(std::max(time, open), close);
    }
    times.push_back(open);
    times.push_back(close);
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());

    return times;
}

GfMatrix4d
utilityTransformAt(const HdTimeSampleArray<GfMatrix4d, 4>& samples,
                   float time)
{
    if (samples.count == 0) {
        return GfMatrix4d(1.);
    }
    std::size_t next = 0;

    while (next < samples.count && samples.times[next] < time) {
        ++next;
    }
    if (next == 0) {
        return samples.values[0];
    }
    if (next == samples.count) {
        return samples.values[samples.count - 1];
    }
    const float span = samples.times[next] - samples.times[next - 1];

    return UtilityScene::interpolate(samples.values[next - 1], samples.values[next], span > 0.f ? (time - samples.times[next - 1]) / span : 0.);
}

UtilityInstancer::UtilityInstancer(HdSceneDelegate* delegate,
                                   const SdfPath& id)
    : HdInstancer(delegate, id)
{
}

UtilityInstancer::~UtilityInstancer()
{
}

void
UtilityInstancer::Sync(HdSceneDelegate* delegate,
                       HdRenderParam* /*renderParam*/,
                       HdDirtyBits* dirtyBits)
{
    if (*dirtyBits & HdChangeTracker::DirtyVisibility) {
        _visible = delegate->GetVisible(GetId());
    }
    _UpdateInstancer(delegate, dirtyBits);
    if (HdChangeTracker::IsAnyPrimvarDirty(*dirtyBits, GetId())) {
        syncPrimvars(delegate, *dirtyBits);
    }
    sampleMotion(delegate);
}

void
UtilityInstancer::sampleMotion(HdSceneDelegate* delegate)
{
    const SdfPath& id = GetId();

    _shutter = utilityMotionShutter(*delegate->GetRenderIndex().GetRenderDelegate(), kUtilitySettingTransformMotionBlur);
    _translationSamples.Resize(0);
    _rotationSamples.Resize(0);
    _scaleSamples.Resize(0);
    _transformSamples.Resize(0);
    _instancerTransformSamples.Resize(0);
    if (!(_shutter[1] > _shutter[0])) {
        return;
    }
    const float open = static_cast<float>(_shutter[0]);
    const float close = static_cast<float>(_shutter[1]);

    if (!_translations.empty()) {
        delegate->SamplePrimvar(id, HdInstancerTokens->instanceTranslations, open, close, &_translationSamples);
    }
    if (!_rotations.empty()) {
        delegate->SamplePrimvar(id, HdInstancerTokens->instanceRotations, open, close, &_rotationSamples);
    }
    if (!_scales.empty()) {
        delegate->SamplePrimvar(id, HdInstancerTokens->instanceScales, open, close, &_scaleSamples);
    }
    if (!_transforms.empty()) {
        delegate->SamplePrimvar(id, HdInstancerTokens->instanceTransforms, open, close, &_transformSamples);
    }
    delegate->SampleInstancerTransform(id, open, close, &_instancerTransformSamples);
}

std::vector<float>
UtilityInstancer::motionTimes(const SdfPath& /*prototypeId*/)
{
    std::vector<float> times;

    if (!_visible || !(_shutter[1] > _shutter[0])) {
        return times;
    }
    addTimes(_translationSamples, &times);
    addTimes(_rotationSamples, &times);
    addTimes(_scaleSamples, &times);
    addTimes(_transformSamples, &times);
    addTimes(_instancerTransformSamples, &times);
    if (!GetParentId().IsEmpty()) {
        HdInstancer* parent = GetDelegate()->GetRenderIndex().GetInstancer(GetParentId());

        if (TF_VERIFY(parent)) {
            const std::vector<float> above = static_cast<UtilityInstancer*>(parent)->motionTimes(GetId());

            times.insert(times.end(), above.begin(), above.end());
        }
    }

    return utilityMotionTimes(times, _shutter);
}

VtMatrix4dArray
UtilityInstancer::computeInstanceTransformsAt(const SdfPath& prototypeId,
                                              float time)
{
    return placements(prototypeId, &time);
}

void
UtilityInstancer::syncPrimvars(HdSceneDelegate* delegate,
                               HdDirtyBits dirtyBits)
{
    const SdfPath& id = GetId();
    const HdPrimvarDescriptorVector primvars = delegate->GetPrimvarDescriptors(id, HdInterpolationInstance);
    const auto isAuthored = [&primvars](const TfToken& name) {
        return std::any_of(primvars.begin(), primvars.end(), [&name](const HdPrimvarDescriptor& pv) { return pv.name == name; });
    };
    const auto needsPull = [&](const TfToken& name) {
        return isAuthored(name) && HdChangeTracker::IsPrimvarDirty(dirtyBits, id, name);
    };

    if (needsPull(HdInstancerTokens->instanceTranslations)) {
        convertVectors(delegate->Get(id, HdInstancerTokens->instanceTranslations), &_translations);
    } else if (!isAuthored(HdInstancerTokens->instanceTranslations)) {
        _translations.clear();
    }
    if (needsPull(HdInstancerTokens->instanceRotations)) {
        convertRotations(delegate->Get(id, HdInstancerTokens->instanceRotations), &_rotations);
    } else if (!isAuthored(HdInstancerTokens->instanceRotations)) {
        _rotations.clear();
    }
    if (needsPull(HdInstancerTokens->instanceScales)) {
        convertVectors(delegate->Get(id, HdInstancerTokens->instanceScales), &_scales);
    } else if (!isAuthored(HdInstancerTokens->instanceScales)) {
        _scales.clear();
    }
    if (needsPull(HdInstancerTokens->instanceTransforms)) {
        convertMatrices(delegate->Get(id, HdInstancerTokens->instanceTransforms), &_transforms);
    } else if (!isAuthored(HdInstancerTokens->instanceTransforms)) {
        _transforms.clear();
    }
}

VtMatrix4dArray
UtilityInstancer::computeInstanceTransforms(const SdfPath& prototypeId)
{
    return placements(prototypeId, nullptr);
}

VtMatrix4dArray
UtilityInstancer::placements(const SdfPath& prototypeId,
                             const float* time)
{
    if (!_visible) {
        return VtMatrix4dArray();
    }
    const bool sampled = time && _shutter[1] > _shutter[0];
    std::vector<GfVec3d> sampledTranslations;
    std::vector<GfQuatd> sampledRotations;
    std::vector<GfVec3d> sampledScales;
    std::vector<GfMatrix4d> sampledTransforms;
    const std::vector<GfVec3d>* translations = &_translations;
    const std::vector<GfQuatd>* rotations = &_rotations;
    const std::vector<GfVec3d>* scales = &_scales;
    const std::vector<GfMatrix4d>* matrices = &_transforms;

    if (sampled && varies(_translationSamples)) {
        convertVectors(_translationSamples.Resample(*time), &sampledTranslations);
        translations = &sampledTranslations;
    }
    if (sampled && varies(_rotationSamples)) {
        convertRotations(_rotationSamples.Resample(*time), &sampledRotations);
        for (GfQuatd& rotation : sampledRotations) {
            rotation.Normalize();
        }
        rotations = &sampledRotations;
    }
    if (sampled && varies(_scaleSamples)) {
        convertVectors(_scaleSamples.Resample(*time), &sampledScales);
        scales = &sampledScales;
    }
    if (sampled && varies(_transformSamples)) {
        convertMatrices(_transformSamples.Resample(*time), &sampledTransforms);
        matrices = &sampledTransforms;
    }
    const GfMatrix4d instancerTransform = (sampled && _instancerTransformSamples.count > 0) ? utilityTransformAt(_instancerTransformSamples, *time) : GetDelegate()->GetInstancerTransform(GetId());
    const VtIntArray indices = GetDelegate()->GetInstanceIndices(GetId(), prototypeId);
    VtMatrix4dArray transforms(indices.size());
    GfMatrix4d* out = transforms.data();

    for (std::size_t i = 0; i < indices.size(); ++i) {
        const std::size_t index = static_cast<std::size_t>(indices[i]);
        GfMatrix4d transform = instancerTransform;

        if (index < translations->size()) {
            transform = GfMatrix4d(1.).SetTranslate((*translations)[index]) * transform;
        }
        if (index < rotations->size()) {
            transform = GfMatrix4d(1.).SetRotate((*rotations)[index]) * transform;
        }
        if (index < scales->size()) {
            transform = GfMatrix4d(1.).SetScale((*scales)[index]) * transform;
        }
        if (index < matrices->size()) {
            transform = (*matrices)[index] * transform;
        }
        out[i] = transform;
    }
    if (GetParentId().IsEmpty()) {
        return transforms;
    }
    HdInstancer* parent = GetDelegate()->GetRenderIndex().GetInstancer(GetParentId());

    if (!TF_VERIFY(parent)) {
        return transforms;
    }
    const VtMatrix4dArray parentTransforms = static_cast<UtilityInstancer*>(parent)->placements(GetId(), time);
    VtMatrix4dArray flattened(parentTransforms.size() * transforms.size());
    GfMatrix4d* flat = flattened.data();

    for (std::size_t i = 0; i < parentTransforms.size(); ++i) {
        for (std::size_t j = 0; j < transforms.size(); ++j) {
            flat[i * transforms.size() + j] = transforms.cdata()[j] * parentTransforms.cdata()[i];
        }
    }

    return flattened;
}

NATRON_NAMESPACE_EXIT
