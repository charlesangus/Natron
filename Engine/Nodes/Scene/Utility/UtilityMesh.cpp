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

#include "UtilityMesh.h"

#include <algorithm>
#include <cmath>

#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/extComputationUtils.h>
#include <pxr/imaging/hd/instancer.h>
#include <pxr/imaging/hd/meshUtil.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/pxOsd/tokens.h>

#include "Engine/Nodes/Scene/Utility/UtilityInstancer.h"
#include "Engine/Nodes/Scene/Utility/UtilityRenderDelegate.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
// Finer rates sort later: combining two channels takes the finer one's.
enum ChannelRate {
    eChannelRateConstant,
    eChannelRateUniform,
    eChannelRateVertex,
    eChannelRateCorner,
};

template <class T>
struct Channel {
    ChannelRate rate = eChannelRateConstant;
    VtArray<T> values;
};

template <class In, class Out>
bool
convertChannelValue(const VtValue& value,
                    VtArray<Out>* out)
{
    if (value.IsHolding<In>()) {
        *out = VtArray<Out>(1, Out(value.UncheckedGet<In>()));

        return true;
    }
    if (value.IsHolding<VtArray<In>>()) {
        const VtArray<In>& in = value.UncheckedGet<VtArray<In>>();

        out->resize(in.size());
        Out* data = out->data();
        for (std::size_t i = 0; i < in.size(); ++i) {
            data[i] = Out(in.cdata()[i]);
        }

        return true;
    }

    return false;
}

bool
convertChannel(const VtValue& value,
               VtVec3fArray* out)
{
    return convertChannelValue<GfVec3f>(value, out) || convertChannelValue<GfVec3d>(value, out);
}

bool
convertChannel(const VtValue& value,
               VtFloatArray* out)
{
    return convertChannelValue<float>(value, out) || convertChannelValue<double>(value, out);
}

HdType
channelType(const VtVec3fArray&)
{
    return HdTypeFloatVec3;
}

HdType
channelType(const VtFloatArray&)
{
    return HdTypeFloat;
}

// A primvar of the wrong type or length falls back rather than reading past the array.
template <class T>
Channel<T>
pullChannel(HdSceneDelegate* sceneDelegate,
            const SdfPath& id,
            const TfToken& name,
            const HdMeshTopology& topology,
            std::size_t pointCount,
            std::size_t triangleCount,
            const T& fallback,
            const UtilitySubdivRefinement* subdiv,
            int faceVaryingChannel)
{
    Channel<T> channel;

    channel.values = VtArray<T>(1, fallback);

    const HdInterpolation interpolations[] = { HdInterpolationConstant,
                                               HdInterpolationUniform,
                                               HdInterpolationVertex,
                                               HdInterpolationVarying,
                                               HdInterpolationFaceVarying };

    for (const HdInterpolation interpolation : interpolations) {
        const HdPrimvarDescriptorVector primvars = sceneDelegate->GetPrimvarDescriptors(id, interpolation);
        const bool found = std::any_of(primvars.begin(), primvars.end(), [&name](const HdPrimvarDescriptor& pv) { return pv.name == name; });

        if (!found) {
            continue;
        }
        VtArray<T> values;

        if (!convertChannel(sceneDelegate->Get(id, name), &values)) {
            return channel;
        }
        switch (interpolation) {
        case HdInterpolationConstant:
            if (!values.empty()) {
                channel.rate = eChannelRateConstant;
                channel.values = values;
            }
            break;
        case HdInterpolationUniform:
            if (values.size() == static_cast<std::size_t>(topology.GetNumFaces())) {
                channel.rate = eChannelRateUniform;
                channel.values = values;
            }
            break;
        case HdInterpolationVertex:
        case HdInterpolationVarying:
            if (values.size() == pointCount) {
                const VtArray<T> refined = subdiv ? subdiv->interpolate(values, interpolation == HdInterpolationVarying) : values;

                if (!refined.empty()) {
                    channel.rate = eChannelRateVertex;
                    channel.values = refined;
                }
            }
            break;
        case HdInterpolationFaceVarying:
            if (subdiv) {
                VtIntArray indices;

                if (faceVaryingChannel >= 0 && convertChannel(sceneDelegate->GetIndexedPrimvar(id, name, &indices), &values)) {
                    const VtArray<T> refined = subdiv->interpolateFaceVarying(faceVaryingChannel, values);
                    const VtVec3iArray& corners = subdiv->faceVaryingTriangles(faceVaryingChannel);

                    if (!refined.empty() && corners.size() == triangleCount) {
                        channel.rate = eChannelRateCorner;
                        channel.values.resize(triangleCount * 3);
                        T* out = channel.values.data();
                        for (std::size_t t = 0; t < triangleCount; ++t) {
                            for (int corner = 0; corner < 3; ++corner) {
                                out[3 * t + corner] = refined.cdata()[corners.cdata()[t][corner]];
                            }
                        }
                    }
                }
            } else if (values.size() == static_cast<std::size_t>(topology.GetNumFaceVaryings())) {
                HdMeshUtil util(&topology, id);
                VtValue triangulated;
                const HdMeshComputationResult result = util.ComputeTriangulatedFaceVaryingPrimvar(values.cdata(), static_cast<int>(values.size()), channelType(values), &triangulated);

                if (result == HdMeshComputationResult::Unchanged) {
                    triangulated = VtValue(values);
                }
                if (result != HdMeshComputationResult::Error && triangulated.IsHolding<VtArray<T>>()
                    && triangulated.UncheckedGet<VtArray<T>>().size() == triangleCount * 3) {
                    channel.rate = eChannelRateCorner;
                    channel.values = triangulated.UncheckedGet<VtArray<T>>();
                }
            }
            break;
        default:
            break;
        }

        return channel;
    }

    return channel;
}

template <class T>
const T&
sampleCorner(const Channel<T>& channel,
             int triangle,
             int corner,
             int face,
             int point)
{
    switch (channel.rate) {
    case eChannelRateUniform:
        return channel.values.cdata()[face];
    case eChannelRateVertex:
        return channel.values.cdata()[point];
    case eChannelRateCorner:
        return channel.values.cdata()[3 * triangle + corner];
    default:
        return channel.values.cdata()[0];
    }
}

int
intValue(const VtValue& value,
         int fallback)
{
    if (value.IsHolding<int>()) {
        return value.UncheckedGet<int>();
    }
    if (value.IsHolding<VtIntArray>() && !value.UncheckedGet<VtIntArray>().empty()) {
        return value.UncheckedGet<VtIntArray>()[0];
    }
    if (value.CanCast<int>()) {
        return VtValue::Cast<int>(value).UncheckedGet<int>();
    }

    return fallback;
}

// The face-varying primvars a refinement carries a channel for, in channel order.
TfTokenVector
faceVaryingChannelNames(HdSceneDelegate* sceneDelegate,
                        const SdfPath& id)
{
    TfTokenVector names;

    for (const HdPrimvarDescriptor& pv : sceneDelegate->GetPrimvarDescriptors(id, HdInterpolationFaceVarying)) {
        if (pv.name == HdTokens->displayColor || pv.name == HdTokens->displayOpacity) {
            names.push_back(pv.name);
        }
    }

    return names;
}

GfVec4f
straightRGBA(const GfVec3f& color,
             float opacity)
{
    return GfVec4f(color[0], color[1], color[2], opacity);
}
} // anon namespace

UtilityMesh::UtilityMesh(const SdfPath& id)
    : HdMesh(id)
{
}

UtilityMesh::~UtilityMesh()
{
}

HdDirtyBits
UtilityMesh::GetInitialDirtyBitsMask() const
{
    return HdChangeTracker::Clean
        | HdChangeTracker::InitRepr
        | HdChangeTracker::DirtyPrimID
        | HdChangeTracker::DirtyPoints
        | HdChangeTracker::DirtyTopology
        | HdChangeTracker::DirtyTransform
        | HdChangeTracker::DirtyVisibility
        | HdChangeTracker::DirtyDisplayStyle
        | HdChangeTracker::DirtyPrimvar
        | HdChangeTracker::DirtyInstancer
        | HdChangeTracker::DirtyInstanceIndex;
}

HdDirtyBits
UtilityMesh::_PropagateDirtyBits(HdDirtyBits bits) const
{
    return bits;
}

void
UtilityMesh::_InitRepr(const TfToken& reprToken,
                       HdDirtyBits* /*dirtyBits*/)
{
    if (std::find_if(_reprs.begin(), _reprs.end(), _ReprComparator(reprToken)) == _reprs.end()) {
        _reprs.emplace_back(reprToken, HdReprSharedPtr());
    }
}

// Hydra updates a dirty tag even for a prim it will not sync, and that is the only call a prim whose
// purpose moved out of the tracked tags gets.
void
UtilityMesh::UpdateRenderTag(HdSceneDelegate* delegate,
                             HdRenderParam* renderParam)
{
    HdMesh::UpdateRenderTag(delegate, renderParam);
    if (_geometry) {
        static_cast<UtilityRenderParam*>(renderParam)->scene()->setRenderTag(_geometry, GetRenderTag());
    }
}

void
UtilityMesh::Sync(HdSceneDelegate* sceneDelegate,
                  HdRenderParam* renderParam,
                  HdDirtyBits* dirtyBits,
                  const TfToken& /*reprToken*/)
{
    const SdfPath& id = GetId();
    const HdDirtyBits bits = *dirtyBits;
    UtilityRenderParam* param = static_cast<UtilityRenderParam*>(renderParam);
    UtilityScene* scene = param->scene();
    const UtilitySubdivRefinement::StopCheck stopped = [param]() { return param->isStopped(); };
    // A stopped refinement leaves nothing half made: the whole sync runs again once restarted.
    const auto syncAgain = [&]() {
        _subdivLevel = -1;
        _subdiv.reset();
        _refinedPoints = VtVec3fArray();
        _normals = VtVec3fArray();
        param->syncAgain(id, (bits & HdChangeTracker::AllSceneDirtyBits) | HdChangeTracker::DirtyTopology | HdChangeTracker::DirtyPoints);
        *dirtyBits &= ~HdChangeTracker::AllSceneDirtyBits;
    };

    if (_geometry) {
        scene->setPrimId(_geometry, GetPrimId());
    } else {
        _geometry = scene->addGeometry(id, GetPrimId());
    }
    scene->setRenderTag(_geometry, GetRenderTag());
    _UpdateVisibility(sceneDelegate, dirtyBits);
    if (!IsVisible()) {
        scene->setInstances(_geometry, std::vector<GfMatrix4d>());
        // Hydra skips an invisible prim until its visibility is dirtied again, then hands back
        // what is left here, so a hidden mesh costs nothing while it stays hidden.
        *dirtyBits &= ~(HdChangeTracker::DirtyVisibility | HdChangeTracker::NewRepr);

        return;
    }

    const std::size_t previousPointCount = _points.size();
    const bool pointsDirty = pullPoints(sceneDelegate, bits);
    bool topologyDirty = false;

    if (HdChangeTracker::IsTopologyDirty(bits, id)) {
        _topology = HdMeshTopology(GetMeshTopology(sceneDelegate), 0);
        topologyDirty = true;
    }
    if (topologyDirty || HdChangeTracker::IsDisplayStyleDirty(bits, id)
        || HdChangeTracker::IsPrimvarDirty(bits, id, TfToken(kUtilityPrimvarSubdivLevel))
        || (_subdiv && (HdChangeTracker::IsPrimvarDirty(bits, id, HdTokens->displayColor) || HdChangeTracker::IsPrimvarDirty(bits, id, HdTokens->displayOpacity)))) {
        const SubdivSync subdiv = syncSubdivTopology(sceneDelegate, scene, topologyDirty, stopped);

        if (subdiv == SubdivSync::Stopped) {
            syncAgain();

            return;
        }
        topologyDirty = subdiv == SubdivSync::Changed || topologyDirty;
    }
    if (pointsDirty || topologyDirty) {
        if (_subdiv && !_subdiv->evaluate(_points, &_refinedPoints, &_normals, stopped)) {
            syncAgain();

            return;
        }

        const bool filterChanged = filterTriangles();

        if (topologyDirty || filterChanged) {
            scene->setTopology(_geometry, scenePoints(), _triangles);
            topologyDirty = true;
        } else {
            scene->setPoints(_geometry, scenePoints());
        }
        scene->setNormals(_geometry, _normals);
    }
    if (topologyDirty || _points.size() != previousPointCount
        || HdChangeTracker::IsPrimvarDirty(bits, id, HdTokens->displayColor)
        || HdChangeTracker::IsPrimvarDirty(bits, id, HdTokens->displayOpacity)) {
        syncColors(sceneDelegate, scene);
    }
    if (HdChangeTracker::IsTransformDirty(bits, id)) {
        _transform = sceneDelegate->GetTransform(id);
    }
    if (HdChangeTracker::IsVisibilityDirty(bits, id) || HdChangeTracker::IsTransformDirty(bits, id)
        || HdChangeTracker::IsInstancerDirty(bits, id) || HdChangeTracker::IsInstanceIndexDirty(bits, id)) {
        _UpdateInstancer(sceneDelegate, dirtyBits);
        syncInstances(sceneDelegate, scene);
    }
    *dirtyBits &= ~HdChangeTracker::AllSceneDirtyBits;
}

void
UtilityMesh::Finalize(HdRenderParam* renderParam)
{
    if (_geometry) {
        static_cast<UtilityRenderParam*>(renderParam)->scene()->removeGeometry(_geometry);
        _geometry = 0;
    }
}

// The level asked for: the prim's primvar, then a display complexity the viewer set, then the setting.
int
UtilityMesh::subdivLevel(HdSceneDelegate* sceneDelegate) const
{
    const SdfPath& id = GetId();
    const TfToken primvar(kUtilityPrimvarSubdivLevel);

    for (const HdPrimvarDescriptor& pv : sceneDelegate->GetPrimvarDescriptors(id, HdInterpolationConstant)) {
        if (pv.name == primvar) {
            const int level = intValue(sceneDelegate->Get(id, primvar), -1);

            if (level >= 0) {
                return std::min(level, UtilitySubdivRefinement::kMaxLevel);
            }
        }
    }

    const int display = sceneDelegate->GetDisplayStyle(id).refineLevel;

    if (display > 0) {
        return std::min(display, UtilitySubdivRefinement::kMaxLevel);
    }

    const VtValue setting = sceneDelegate->GetRenderIndex().GetRenderDelegate()->GetRenderSetting(TfToken(kUtilitySettingSubdivLevel));

    return std::max(0, std::min(intValue(setting, UtilitySubdivRefinement::kDefaultLevel), UtilitySubdivRefinement::kMaxLevel));
}

// Refines or triangulates the cage into _allTriangles. They change whenever the topology did.
UtilityMesh::SubdivSync
UtilityMesh::syncSubdivTopology(HdSceneDelegate* sceneDelegate,
                                UtilityScene* scene,
                                bool topologyChanged,
                                const UtilitySubdivRefinement::StopCheck& stopped)
{
    const SdfPath& id = GetId();
    const bool subdivides = _topology.GetScheme() != PxOsdOpenSubdivTokens->none;
    const int requested = subdivides ? subdivLevel(sceneDelegate) : 0;
    const TfTokenVector faceVaryingNames = requested > 0 ? faceVaryingChannelNames(sceneDelegate, id) : TfTokenVector();

    if (!topologyChanged && requested == _subdivLevel && faceVaryingNames == _faceVaryingNames) {
        return SubdivSync::Unchanged;
    }
    _subdivLevel = requested;
    _faceVaryingNames = faceVaryingNames;
    _subdiv.reset();
    _refinedPoints = VtVec3fArray();
    _normals = VtVec3fArray();

    std::string warning;
    int level = requested;

    if (level > 0) {
        const VtValue budgetSetting = sceneDelegate->GetRenderIndex().GetRenderDelegate()->GetRenderSetting(TfToken(kUtilitySettingMaxSubdivTriangles));
        const int budget = intValue(budgetSetting, static_cast<int>(UtilitySubdivRefinement::kDefaultMaxTriangles));

        level = UtilitySubdivRefinement::fitLevel(_topology.GetPxOsdMeshTopology(), level, static_cast<std::size_t>(std::max(0, budget)));
        if (level < requested) {
            warning = id.GetString() + ": subdivision level lowered from " + std::to_string(requested) + " to " + std::to_string(level)
                + " to stay within " + std::to_string(budget) + " subdivided triangles";
        }
    }
    if (level > 0) {
        std::vector<VtIntArray> faceVaryingIndices;

        for (const TfToken& name : _faceVaryingNames) {
            VtIntArray indices;
            const VtValue values = sceneDelegate->GetIndexedPrimvar(id, name, &indices);

            if (indices.empty()) {
                const std::size_t count = values.IsArrayValued() ? values.GetArraySize() : 0;

                indices.resize(count);
                for (std::size_t i = 0; i < count; ++i) {
                    indices[i] = static_cast<int>(i);
                }
            }
            faceVaryingIndices.push_back(indices);
        }

        std::string error;
        UtilitySubdivRefinement::Outcome outcome = UtilitySubdivRefinement::Outcome::Failed;

        _subdiv = UtilitySubdivRefinement::get(_topology.GetPxOsdMeshTopology(), level, faceVaryingIndices, stopped, &outcome, &error);
        if (outcome == UtilitySubdivRefinement::Outcome::Failed && !faceVaryingIndices.empty()) {
            // A broken face-varying primvar should cost only its own channel, not the refinement.
            _faceVaryingNames.clear();
            _subdiv = UtilitySubdivRefinement::get(_topology.GetPxOsdMeshTopology(), level, std::vector<VtIntArray>(), stopped, &outcome, &error);
        }
        if (outcome == UtilitySubdivRefinement::Outcome::Stopped) {
            return SubdivSync::Stopped;
        }
        if (!_subdiv) {
            warning = id.GetString() + ": rendered unrefined, " + error;
        }
    }
    if (_subdiv) {
        const VtIntArray& coarseFaces = _subdiv->coarseFaces();

        _allTriangles = _subdiv->triangles();
        _allParams.resize(coarseFaces.size());
        int* params = _allParams.data();
        for (std::size_t t = 0; t < coarseFaces.size(); ++t) {
            params[t] = HdMeshUtil::EncodeCoarseFaceParam(coarseFaces.cdata()[t], false);
        }
    } else {
        const HdMeshUtil util(&_topology, id);

        _faceVaryingNames.clear();
        util.ComputeTriangleIndices(&_allTriangles, &_allParams);
    }
    scene->setWarning(_geometry, warning);

    return SubdivSync::Changed;
}

const VtVec3fArray&
UtilityMesh::scenePoints() const
{
    return _subdiv ? _refinedPoints : _points;
}

bool
UtilityMesh::pullPoints(HdSceneDelegate* sceneDelegate,
                        HdDirtyBits dirtyBits)
{
    const SdfPath& id = GetId();

    if (!HdChangeTracker::IsPrimvarDirty(dirtyBits, id, HdTokens->points)) {
        return false;
    }
    for (std::size_t i = 0; i < HdInterpolationCount; ++i) {
        for (const HdExtComputationPrimvarDescriptor& pv : sceneDelegate->GetExtComputationPrimvarDescriptors(id, static_cast<HdInterpolation>(i))) {
            if (pv.name == HdTokens->points) {
                const HdExtComputationUtils::ValueStore values = HdExtComputationUtils::GetComputedPrimvarValues({ pv }, sceneDelegate);
                const auto it = values.find(pv.name);

                _points = it == values.end() ? VtVec3fArray() : it->second.GetWithDefault<VtVec3fArray>();

                return true;
            }
        }
    }
    _points = sceneDelegate->Get(id, HdTokens->points).GetWithDefault<VtVec3fArray>();

    return true;
}

// A triangle with no area has no normal, and one NaN vertex would poison the bounds of whatever
// acceleration structure is built over it, so neither reaches the scene. Returns whether the
// surviving list differs from the last one: an unchanged list lets a points edit stay a refit.
bool
UtilityMesh::filterTriangles()
{
    const VtVec3fArray& scene = scenePoints();
    const std::size_t pointCount = scene.size();
    const GfVec3f* points = scene.cdata();
    const GfVec3i* all = _allTriangles.cdata();
    std::vector<int> kept;

    kept.reserve(_allTriangles.size());
    for (std::size_t t = 0; t < _allTriangles.size(); ++t) {
        const GfVec3i& triangle = all[t];
        const bool inRange = triangle[0] >= 0 && triangle[1] >= 0 && triangle[2] >= 0
            && static_cast<std::size_t>(triangle[0]) < pointCount
            && static_cast<std::size_t>(triangle[1]) < pointCount
            && static_cast<std::size_t>(triangle[2]) < pointCount;

        if (!inRange || triangle[0] == triangle[1] || triangle[1] == triangle[2] || triangle[0] == triangle[2]) {
            continue;
        }
        const GfVec3d a(points[triangle[0]]);
        const GfVec3d cross = GfCross(GfVec3d(points[triangle[1]]) - a, GfVec3d(points[triangle[2]]) - a);
        const double areaSquared = cross.GetLengthSq();

        if (std::isfinite(areaSquared) && areaSquared > 0.) {
            kept.push_back(static_cast<int>(t));
        }
    }
    if (kept == _kept && _triangles.size() == kept.size()) {
        return false;
    }
    _kept.swap(kept);
    _triangles.resize(_kept.size());
    GfVec3i* triangles = _triangles.data();
    for (std::size_t t = 0; t < _kept.size(); ++t) {
        triangles[t] = all[_kept[t]];
    }

    return true;
}

void
UtilityMesh::syncColors(HdSceneDelegate* sceneDelegate,
                        UtilityScene* scene)
{
    const SdfPath& id = GetId();
    const auto channelOf = [this](const TfToken& name) {
        const auto it = std::find(_faceVaryingNames.begin(), _faceVaryingNames.end(), name);

        return it == _faceVaryingNames.end() ? -1 : static_cast<int>(it - _faceVaryingNames.begin());
    };
    const Channel<GfVec3f> color = pullChannel<GfVec3f>(sceneDelegate, id, HdTokens->displayColor, _topology, _points.size(), _allTriangles.size(), GfVec3f(0.18f), _subdiv.get(), channelOf(HdTokens->displayColor));
    const Channel<float> opacity = pullChannel<float>(sceneDelegate, id, HdTokens->displayOpacity, _topology, _points.size(), _allTriangles.size(), 1.f, _subdiv.get(), channelOf(HdTokens->displayOpacity));
    const ChannelRate rate = std::max(color.rate, opacity.rate);
    const bool mixedFinerRates = (color.rate == eChannelRateUniform && opacity.rate == eChannelRateVertex)
        || (color.rate == eChannelRateVertex && opacity.rate == eChannelRateUniform);
    const auto faceOf = [this](std::size_t t) {
        return HdMeshUtil::DecodeFaceIndexFromCoarseFaceParam(_allParams.cdata()[_kept[t]]);
    };
    VtVec4fArray colors;
    UtilityScene::ColorBinding binding = UtilityScene::eColorBindingConstant;

    if (rate == eChannelRateConstant) {
        colors = VtVec4fArray(1, straightRGBA(color.values.cdata()[0], opacity.values.cdata()[0]));
    } else if (rate == eChannelRateUniform) {
        binding = UtilityScene::eColorBindingTriangle;
        colors.resize(_kept.size());
        GfVec4f* out = colors.data();
        for (std::size_t t = 0; t < _kept.size(); ++t) {
            const int face = faceOf(t);

            out[t] = straightRGBA(sampleCorner(color, _kept[t], 0, face, 0), sampleCorner(opacity, _kept[t], 0, face, 0));
        }
    } else if (rate == eChannelRateVertex && !mixedFinerRates) {
        binding = UtilityScene::eColorBindingVertex;
        colors.resize(scenePoints().size());
        GfVec4f* out = colors.data();
        for (std::size_t p = 0; p < colors.size(); ++p) {
            out[p] = straightRGBA(sampleCorner(color, 0, 0, 0, static_cast<int>(p)), sampleCorner(opacity, 0, 0, 0, static_cast<int>(p)));
        }
    } else {
        binding = UtilityScene::eColorBindingCorner;
        colors.resize(_kept.size() * 3);
        GfVec4f* out = colors.data();
        const GfVec3i* triangles = _triangles.cdata();
        for (std::size_t t = 0; t < _kept.size(); ++t) {
            const int face = faceOf(t);

            for (int corner = 0; corner < 3; ++corner) {
                const int point = triangles[t][corner];

                out[t * 3 + corner] = straightRGBA(sampleCorner(color, _kept[t], corner, face, point), sampleCorner(opacity, _kept[t], corner, face, point));
            }
        }
    }
    scene->setColors(_geometry, binding, colors);
}

void
UtilityMesh::syncInstances(HdSceneDelegate* sceneDelegate,
                           UtilityScene* scene)
{
    const SdfPath& instancerId = GetInstancerId();
    std::vector<GfMatrix4d> transforms;

    if (instancerId.IsEmpty()) {
        transforms.push_back(_transform);
    } else {
        HdRenderIndex& renderIndex = sceneDelegate->GetRenderIndex();

        HdInstancer::_SyncInstancerAndParents(renderIndex, instancerId);
        HdInstancer* instancer = renderIndex.GetInstancer(instancerId);

        if (TF_VERIFY(instancer)) {
            const VtMatrix4dArray instances = static_cast<UtilityInstancer*>(instancer)->computeInstanceTransforms(GetId());
            transforms.reserve(instances.size());
            for (const GfMatrix4d& instance : instances) {
                transforms.push_back(_transform * instance);
            }
        }
    }
    scene->setInstances(_geometry, transforms);
}

NATRON_NAMESPACE_EXIT
