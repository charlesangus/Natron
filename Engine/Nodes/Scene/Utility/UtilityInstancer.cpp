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
} // anon namespace

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
    if (!_visible) {
        return VtMatrix4dArray();
    }
    const GfMatrix4d instancerTransform = GetDelegate()->GetInstancerTransform(GetId());
    const VtIntArray indices = GetDelegate()->GetInstanceIndices(GetId(), prototypeId);
    VtMatrix4dArray transforms(indices.size());
    GfMatrix4d* out = transforms.data();

    for (std::size_t i = 0; i < indices.size(); ++i) {
        const std::size_t index = static_cast<std::size_t>(indices[i]);
        GfMatrix4d transform = instancerTransform;

        if (index < _translations.size()) {
            transform = GfMatrix4d(1.).SetTranslate(_translations[index]) * transform;
        }
        if (index < _rotations.size()) {
            transform = GfMatrix4d(1.).SetRotate(_rotations[index]) * transform;
        }
        if (index < _scales.size()) {
            transform = GfMatrix4d(1.).SetScale(_scales[index]) * transform;
        }
        if (index < _transforms.size()) {
            transform = _transforms[index] * transform;
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
    const VtMatrix4dArray parentTransforms = static_cast<UtilityInstancer*>(parent)->computeInstanceTransforms(GetId());
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
