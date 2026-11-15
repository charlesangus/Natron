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
#include <memory>

#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/extComputationUtils.h>
#include <pxr/imaging/hd/instancer.h>
#include <pxr/imaging/hd/meshSchema.h>
#include <pxr/imaging/hd/meshTopologySchema.h>
#include <pxr/imaging/hd/meshUtil.h>
#include <pxr/imaging/hd/primvarSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/imaging/hd/timeSampleArray.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/pxOsd/subdivTags.h>
#include <pxr/imaging/pxOsd/tokens.h>

#include "Engine/Nodes/Scene/Utility/UtilityInstancer.h"
#include "Engine/Nodes/Scene/Utility/UtilityMaterial.h"
#include "Engine/Nodes/Scene/Utility/UtilityRenderDelegate.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
const std::size_t kStopChunk = 1 << 14;

// Polled once a chunk, so a mesh smaller than one is never interrupted.
bool
haltedAt(std::size_t i,
         const UtilitySubdivRefinement::StopCheck& stopped)
{
    return i > 0 && i % kStopChunk == 0 && stopped && stopped();
}

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
    // False when values holds only the fallback, the primvar missing or unusable.
    bool found = false;
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

bool
convertChannel(const VtValue& value,
               VtVec2fArray* out)
{
    return convertChannelValue<GfVec2f>(value, out) || convertChannelValue<GfVec2d>(value, out);
}

HdType
channelType(const VtVec2fArray&)
{
    return HdTypeFloatVec2;
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

// A primvar of the wrong type or length falls back rather than reading past the array. *halted is
// set, and the channel is not to be used, when stopped turned true.
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
            int faceVaryingChannel,
            const UtilitySubdivRefinement::StopCheck& stopped,
            bool* halted)
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
                channel.found = true;
            }
            break;
        case HdInterpolationUniform:
            if (values.size() == static_cast<std::size_t>(topology.GetNumFaces())) {
                channel.rate = eChannelRateUniform;
                channel.values = values;
                channel.found = true;
            }
            break;
        case HdInterpolationVertex:
        case HdInterpolationVarying:
            if (values.size() == pointCount) {
                const VtArray<T> refined = subdiv ? subdiv->interpolate(values, interpolation == HdInterpolationVarying) : values;

                if (!refined.empty()) {
                    channel.rate = eChannelRateVertex;
                    channel.values = refined;
                    channel.found = true;
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
                            if (haltedAt(t, stopped)) {
                                *halted = true;

                                return channel;
                            }
                            for (int corner = 0; corner < 3; ++corner) {
                                out[3 * t + corner] = refined.cdata()[corners.cdata()[t][corner]];
                            }
                        }
                        channel.found = true;
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
                    channel.found = true;
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

TfTokenVector
faceVaryingChannelNames(HdSceneDelegate* sceneDelegate,
                        const SdfPath& id,
                        const TfToken& stName)
{
    TfTokenVector names;

    for (const HdPrimvarDescriptor& pv : sceneDelegate->GetPrimvarDescriptors(id, HdInterpolationFaceVarying)) {
        if (pv.name == HdTokens->displayColor || pv.name == HdTokens->displayOpacity || pv.name == stName) {
            names.push_back(pv.name);
        }
    }

    return names;
}

int
channelIndex(const TfTokenVector& names,
             const TfToken& name)
{
    const auto it = std::find(names.begin(), names.end(), name);

    return it == names.end() ? -1 : static_cast<int>(it - names.begin());
}

// Each channel's value per face-vertex; an unindexed primvar holds one value per face-vertex.
std::vector<VtIntArray>
faceVaryingChannelIndices(HdSceneDelegate* sceneDelegate,
                          const SdfPath& id,
                          const TfTokenVector& names)
{
    std::vector<VtIntArray> channels;

    for (const TfToken& name : names) {
        VtIntArray indices;
        const VtValue values = sceneDelegate->GetIndexedPrimvar(id, name, &indices);

        if (indices.empty()) {
            const std::size_t count = values.IsArrayValued() ? values.GetArraySize() : 0;

            indices.resize(count);
            for (std::size_t i = 0; i < count; ++i) {
                indices[i] = static_cast<int>(i);
            }
        }
        channels.push_back(indices);
    }

    return channels;
}

GfVec4f
straightRGBA(const GfVec3f& color,
             float opacity)
{
    return GfVec4f(color[0], color[1], color[2], opacity);
}
// The scene delegate serves a mesh's topology at the frame only; its sampled sources in the
// terminal scene index are what can be asked about other times within the shutter.
HdContainerDataSourceHandle
primSource(HdSceneDelegate* sceneDelegate,
           const SdfPath& id)
{
    const HdSceneIndexBaseRefPtr index = sceneDelegate->GetRenderIndex().GetTerminalSceneIndex();

    return index ? index->GetPrim(id).dataSource : HdContainerDataSourceHandle();
}

bool
variesWithin(const HdIntArrayDataSourceHandle& source,
             float open,
             float close)
{
    std::vector<HdSampledDataSource::Time> times;

    if (!source || !source->GetContributingSampleTimesForInterval(open, close, &times)) {
        return false;
    }
    const VtIntArray atFrame = source->GetTypedValue(0.f);

    return std::any_of(times.begin(), times.end(), [&source, &atFrame](HdSampledDataSource::Time t) { return source->GetTypedValue(t) != atFrame; });
}

VtIntArray
valueAt(const HdIntArrayDataSourceHandle& source,
        float time,
        const VtIntArray& fallback)
{
    return source ? source->GetTypedValue(time) : fallback;
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
        | HdChangeTracker::DirtyInstanceIndex
        | HdChangeTracker::DirtyMaterialId
        | HdChangeTracker::DirtyCategories;
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
    // A mid-shutter mesh replaced the frame's points and topology, which are needed again to tell
    // whether the topology still varies.
    const HdDirtyBits pulled = _midShutter ? (bits | HdChangeTracker::DirtyPoints | HdChangeTracker::DirtyTopology) : bits;
    const bool pointsDirty = pullPoints(sceneDelegate, pulled);
    bool topologyDirty = false;

    if (HdChangeTracker::IsTopologyDirty(pulled, id)) {
        _topology = HdMeshTopology(GetMeshTopology(sceneDelegate), 0);
        // Hydra serves creases, corners and the interpolation rules apart from the topology.
        _topology.SetSubdivTags(sceneDelegate->GetSubdivTags(id));
        topologyDirty = true;
    }
    if (bits & HdChangeTracker::DirtyMaterialId) {
        SetMaterialId(sceneDelegate->GetMaterialId(id));
        scene->setMaterialId(_geometry, GetMaterialId());
    }
    // A topology that changes within the shutter is replaced by the shutter's middle one, and that
    // is the one to refine.
    if (pointsDirty || topologyDirty) {
        _coarseMotion = samplePointMotion(sceneDelegate);
        topologyDirty = topologyDirty || _midShutter;
    }
    // The level primvar and the face-varying channels are primvars, and the material names the
    // texture coordinates' one.
    if (topologyDirty || HdChangeTracker::IsDisplayStyleDirty(bits, id) || (bits & (HdChangeTracker::DirtyPrimvar | HdChangeTracker::DirtyMaterialId))) {
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

        std::shared_ptr<const UtilityScene::PointMotion> motion = _coarseMotion;

        if (_subdiv && _coarseMotion) {
            const std::shared_ptr<UtilityScene::PointMotion> refined = std::make_shared<UtilityScene::PointMotion>();

            refined->times = _coarseMotion->times;
            for (const VtVec3fArray& coarse : _coarseMotion->points) {
                VtVec3fArray limit;
                VtVec3fArray normals;

                if (!_subdiv->evaluate(coarse, &limit, &normals, stopped)) {
                    syncAgain();

                    return;
                }
                refined->points.push_back(limit);
                refined->normals.push_back(normals);
            }
            motion = refined;
        }
        if (topologyDirty) {
            // The kept list indexes the triangulation, so the same list can name other triangles.
            _kept.clear();
        }
        bool filterChanged = false;

        if (!filterTriangles(stopped, &filterChanged)) {
            syncAgain();

            return;
        }
        if (topologyDirty || filterChanged) {
            scene->setTopology(_geometry, scenePoints(), _triangles);
            topologyDirty = true;
        } else {
            scene->setPoints(_geometry, scenePoints());
        }
        scene->setTopologyVaries(_geometry, _midShutter);
        scene->setPointMotion(_geometry, motion);
    }

    const bool pointCountChanged = _points.size() != previousPointCount;
    // Limit normals move with the points; authored ones only follow the triangles they are bound to.
    const bool normalsDirty = _subdiv ? (pointsDirty || topologyDirty)
                                      : (topologyDirty || pointCountChanged || HdChangeTracker::IsPrimvarDirty(bits, id, HdTokens->normals));

    if (normalsDirty && !syncNormals(sceneDelegate, scene, stopped)) {
        syncAgain();

        return;
    }
    if ((topologyDirty || pointCountChanged
         || HdChangeTracker::IsPrimvarDirty(bits, id, HdTokens->displayColor)
         || HdChangeTracker::IsPrimvarDirty(bits, id, HdTokens->displayOpacity))
        && !syncColors(sceneDelegate, scene, stopped)) {
        syncAgain();

        return;
    }
    if ((topologyDirty || pointCountChanged || (bits & (HdChangeTracker::DirtyMaterialId | HdChangeTracker::DirtyPrimvar)))
        && !syncMaterial(sceneDelegate, scene, stopped)) {
        syncAgain();

        return;
    }
    if (HdChangeTracker::IsTransformDirty(bits, id)) {
        _transform = sceneDelegate->GetTransform(id);
    }
    if (HdChangeTracker::IsVisibilityDirty(bits, id) || HdChangeTracker::IsTransformDirty(bits, id)
        || HdChangeTracker::IsInstancerDirty(bits, id) || HdChangeTracker::IsInstanceIndexDirty(bits, id)
        || (bits & HdChangeTracker::DirtyCategories)) {
        _UpdateInstancer(sceneDelegate, dirtyBits);
        syncInstances(sceneDelegate, scene);
    }
    if ((bits & HdChangeTracker::DirtyCategories) || HdChangeTracker::IsInstancerDirty(bits, id)) {
        // The light-linking scene index puts a prim's collections on it and an instance's on its
        // instancer, so an instanced mesh answers to both.
        const VtArray<TfToken> own = sceneDelegate->GetCategories(id);
        TfTokenVector categories(own.begin(), own.end());
        const SdfPath& instancerId = GetInstancerId();

        if (!instancerId.IsEmpty()) {
            const VtArray<TfToken> inherited = sceneDelegate->GetCategories(instancerId);

            categories.insert(categories.end(), inherited.begin(), inherited.end());
        }
        scene->setCategories(_geometry, categories);
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

UtilityMesh::SubdivSync
UtilityMesh::syncSubdivTopology(HdSceneDelegate* sceneDelegate,
                                UtilityScene* scene,
                                bool topologyChanged,
                                const UtilitySubdivRefinement::StopCheck& stopped)
{
    const SdfPath& id = GetId();
    const bool subdivides = _topology.GetScheme() != PxOsdOpenSubdivTokens->none;
    const int requested = subdivides ? subdivLevel(sceneDelegate) : 0;
    TfTokenVector faceVaryingNames = requested > 0 ? faceVaryingChannelNames(sceneDelegate, id, stPrimvarName(sceneDelegate)) : TfTokenVector();
    std::vector<VtIntArray> faceVaryingIndices = faceVaryingChannelIndices(sceneDelegate, id, faceVaryingNames);

    // A malformed primvar loses its own channel only; the others are still refined.
    for (std::size_t i = faceVaryingNames.size(); i-- > 0;) {
        if (!UtilitySubdivRefinement::faceVaryingChannelFits(_topology.GetPxOsdMeshTopology(), faceVaryingIndices[i])) {
            faceVaryingNames.erase(faceVaryingNames.begin() + static_cast<std::ptrdiff_t>(i));
            faceVaryingIndices.erase(faceVaryingIndices.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }

    if (!topologyChanged && requested == _subdivLevel && faceVaryingNames == _faceVaryingNames && faceVaryingIndices == _faceVaryingIndices) {
        return SubdivSync::Unchanged;
    }
    _subdivLevel = requested;
    _faceVaryingNames = faceVaryingNames;
    _faceVaryingIndices = faceVaryingIndices;
    _channelNames = faceVaryingNames;
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
        std::string error;
        UtilitySubdivRefinement::Outcome outcome = UtilitySubdivRefinement::Outcome::Failed;

        _subdiv = UtilitySubdivRefinement::get(_topology.GetPxOsdMeshTopology(), level, faceVaryingIndices, stopped, &outcome, &error);
        if (outcome == UtilitySubdivRefinement::Outcome::Failed && !faceVaryingIndices.empty()) {
            // Face-varying data OpenSubdiv still rejects should cost the channels, not the refinement;
            // _faceVaryingNames stays so that an unchanged mesh is not refined again.
            _channelNames.clear();
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
            if (haltedAt(t, stopped)) {
                return SubdivSync::Stopped;
            }
            params[t] = HdMeshUtil::EncodeCoarseFaceParam(coarseFaces.cdata()[t], false);
        }
    } else {
        const HdMeshUtil util(&_topology, id);

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
                _pointsComputed = true;

                return true;
            }
        }
    }
    _points = sceneDelegate->Get(id, HdTokens->points).GetWithDefault<VtVec3fArray>();
    _pointsComputed = false;

    return true;
}

// Computed points (skinning and the like) give no samples over an interval, so they do not deform
// within the shutter. A mesh whose topology changes within it cannot be interpolated, so it stays
// still as it is at the shutter's middle, the time the rest of the scene is centred on.
std::shared_ptr<UtilityScene::PointMotion>
UtilityMesh::samplePointMotion(HdSceneDelegate* sceneDelegate)
{
    const SdfPath& id = GetId();
    const GfVec2d shutter = utilityMotionShutter(*sceneDelegate->GetRenderIndex().GetRenderDelegate(), kUtilitySettingDeformationMotionBlur);

    _midShutter = false;
    if (!(shutter[1] > shutter[0]) || _pointsComputed) {
        return nullptr;
    }
    const float open = static_cast<float>(shutter[0]);
    const float close = static_cast<float>(shutter[1]);
    HdTimeSampleArray<VtValue, 4> samples;
    std::shared_ptr<UtilityScene::PointMotion> motion = std::make_shared<UtilityScene::PointMotion>();
    bool topologyVaries = false;

    sceneDelegate->SamplePrimvar(id, HdTokens->points, open, close, &samples);
    for (std::size_t i = 0; i < samples.count; ++i) {
        const VtVec3fArray points = samples.values[i].GetWithDefault<VtVec3fArray>();

        if (points.size() != _points.size()) {
            topologyVaries = true;
            break;
        }
        motion->times.push_back(samples.times[i]);
        motion->points.push_back(points);
    }

    const HdContainerDataSourceHandle prim = primSource(sceneDelegate, id);
    const HdMeshTopologySchema topology = HdMeshSchema::GetFromParent(prim).GetTopology();
    const HdIntArrayDataSourceHandle counts = topology.GetFaceVertexCounts();
    const HdIntArrayDataSourceHandle indices = topology.GetFaceVertexIndices();

    topologyVaries = topologyVaries || variesWithin(counts, open, close) || variesWithin(indices, open, close);
    if (topologyVaries) {
        const float middle = 0.5f * (open + close);
        const HdSampledDataSourceHandle points = HdPrimvarsSchema::GetFromParent(prim).GetPrimvar(HdTokens->points).GetPrimvarValue();

        if (points) {
            _points = points->GetValue(middle).GetWithDefault<VtVec3fArray>();
        }
        const PxOsdSubdivTags& frameTags = _topology.GetSubdivTags();
        // Creases and corners name the frame's vertices, which the middle topology need not share;
        // the interpolation rules hold for any topology.
        PxOsdSubdivTags rules;

        rules.SetVertexInterpolationRule(frameTags.GetVertexInterpolationRule());
        rules.SetFaceVaryingInterpolationRule(frameTags.GetFaceVaryingInterpolationRule());
        rules.SetCreaseMethod(frameTags.GetCreaseMethod());
        rules.SetTriangleSubdivision(frameTags.GetTriangleSubdivision());
        _topology = HdMeshTopology(_topology.GetScheme(), _topology.GetOrientation(), valueAt(counts, middle, _topology.GetFaceVertexCounts()),
                                   valueAt(indices, middle, _topology.GetFaceVertexIndices()),
                                   valueAt(topology.GetHoleIndices(), middle, _topology.GetHoleIndices()), 0);
        _topology.SetSubdivTags(rules);
        _midShutter = true;

        return nullptr;
    }
    const bool moves = std::any_of(motion->points.begin(), motion->points.end(), [&motion](const VtVec3fArray& points) { return points != motion->points.front(); });

    return moves ? motion : nullptr;
}

// A triangle with no area has no normal, and one NaN vertex would poison the bounds of whatever
// acceleration structure is built over it, so neither reaches the scene. An unchanged list lets a
// points edit stay a refit.
bool
UtilityMesh::filterTriangles(const UtilitySubdivRefinement::StopCheck& stopped,
                             bool* changed)
{
    const VtVec3fArray& scene = scenePoints();
    const std::size_t pointCount = scene.size();
    const GfVec3f* points = scene.cdata();
    const GfVec3i* all = _allTriangles.cdata();
    std::vector<int> kept;

    *changed = false;
    kept.reserve(_allTriangles.size());
    for (std::size_t t = 0; t < _allTriangles.size(); ++t) {
        if (haltedAt(t, stopped)) {
            return false;
        }
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
        return true;
    }
    VtVec3iArray triangles(kept.size());
    GfVec3i* out = triangles.data();

    for (std::size_t t = 0; t < kept.size(); ++t) {
        if (haltedAt(t, stopped)) {
            return false;
        }
        out[t] = all[kept[t]];
    }
    _kept.swap(kept);
    _triangles = triangles;
    *changed = true;

    return true;
}

bool
UtilityMesh::syncColors(HdSceneDelegate* sceneDelegate,
                        UtilityScene* scene,
                        const UtilitySubdivRefinement::StopCheck& stopped)
{
    const SdfPath& id = GetId();
    bool halted = false;
    const Channel<GfVec3f> color = pullChannel<GfVec3f>(sceneDelegate, id, HdTokens->displayColor, _topology, _points.size(), _allTriangles.size(), GfVec3f(0.18f), _subdiv.get(), channelIndex(_channelNames, HdTokens->displayColor), stopped, &halted);
    const Channel<float> opacity = pullChannel<float>(sceneDelegate, id, HdTokens->displayOpacity, _topology, _points.size(), _allTriangles.size(), 1.f, _subdiv.get(), channelIndex(_channelNames, HdTokens->displayOpacity), stopped, &halted);

    if (halted) {
        return false;
    }

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
            if (haltedAt(t, stopped)) {
                return false;
            }
            const int face = faceOf(t);

            out[t] = straightRGBA(sampleCorner(color, _kept[t], 0, face, 0), sampleCorner(opacity, _kept[t], 0, face, 0));
        }
    } else if (rate == eChannelRateVertex && !mixedFinerRates) {
        binding = UtilityScene::eColorBindingVertex;
        colors.resize(scenePoints().size());
        GfVec4f* out = colors.data();
        for (std::size_t p = 0; p < colors.size(); ++p) {
            if (haltedAt(p, stopped)) {
                return false;
            }
            out[p] = straightRGBA(sampleCorner(color, 0, 0, 0, static_cast<int>(p)), sampleCorner(opacity, 0, 0, 0, static_cast<int>(p)));
        }
    } else {
        binding = UtilityScene::eColorBindingCorner;
        colors.resize(_kept.size() * 3);
        GfVec4f* out = colors.data();
        const GfVec3i* triangles = _triangles.cdata();
        for (std::size_t t = 0; t < _kept.size(); ++t) {
            if (haltedAt(t, stopped)) {
                return false;
            }
            const int face = faceOf(t);

            for (int corner = 0; corner < 3; ++corner) {
                const int point = triangles[t][corner];

                out[t * 3 + corner] = straightRGBA(sampleCorner(color, _kept[t], corner, face, point), sampleCorner(opacity, _kept[t], corner, face, point));
            }
        }
    }
    scene->setColors(_geometry, binding, colors, color.found, opacity.found);

    return true;
}

// A refinement's limit normals, else the authored ones normalised; none, for faceted shading, when
// the refinement is not smooth or the mesh has no usable normals.
bool
UtilityMesh::syncNormals(HdSceneDelegate* sceneDelegate,
                         UtilityScene* scene,
                         const UtilitySubdivRefinement::StopCheck& stopped)
{
    if (_subdiv) {
        scene->setNormals(_geometry, UtilityScene::eColorBindingVertex, _normals);

        return true;
    }

    const SdfPath& id = GetId();
    bool halted = false;
    const Channel<GfVec3f> authored = pullChannel<GfVec3f>(sceneDelegate, id, HdTokens->normals, _topology, _points.size(), _allTriangles.size(), GfVec3f(0.f), nullptr, -1, stopped, &halted);

    if (halted) {
        return false;
    }
    if (!authored.found) {
        scene->setNormals(_geometry, UtilityScene::eColorBindingVertex, VtVec3fArray());

        return true;
    }

    const auto unit = [](const GfVec3f& n) {
        const float length = n.GetLength();

        return length > 0.f && std::isfinite(length) ? n / length : GfVec3f(0.f);
    };
    VtVec3fArray normals;
    UtilityScene::ColorBinding binding = UtilityScene::eColorBindingConstant;

    switch (authored.rate) {
    case eChannelRateConstant:
        normals = VtVec3fArray(1, unit(authored.values.cdata()[0]));
        break;
    case eChannelRateUniform:
        binding = UtilityScene::eColorBindingTriangle;
        normals.resize(_kept.size());
        for (std::size_t t = 0; t < _kept.size(); ++t) {
            if (haltedAt(t, stopped)) {
                return false;
            }
            normals[t] = unit(sampleCorner(authored, _kept[t], 0, HdMeshUtil::DecodeFaceIndexFromCoarseFaceParam(_allParams.cdata()[_kept[t]]), 0));
        }
        break;
    case eChannelRateVertex:
        binding = UtilityScene::eColorBindingVertex;
        normals.resize(authored.values.size());
        for (std::size_t p = 0; p < normals.size(); ++p) {
            if (haltedAt(p, stopped)) {
                return false;
            }
            normals[p] = unit(authored.values.cdata()[p]);
        }
        break;
    case eChannelRateCorner:
        binding = UtilityScene::eColorBindingCorner;
        normals.resize(_kept.size() * 3);
        for (std::size_t t = 0; t < _kept.size(); ++t) {
            if (haltedAt(t, stopped)) {
                return false;
            }
            for (int corner = 0; corner < 3; ++corner) {
                normals[t * 3 + corner] = unit(sampleCorner(authored, _kept[t], corner, 0, 0));
            }
        }
        break;
    }
    scene->setNormals(_geometry, binding, normals);

    return true;
}

void
UtilityMesh::syncInstances(HdSceneDelegate* sceneDelegate,
                           UtilityScene* scene)
{
    const SdfPath& instancerId = GetInstancerId();
    HdRenderIndex& renderIndex = sceneDelegate->GetRenderIndex();
    UtilityInstancer* instancer = nullptr;
    std::vector<GfMatrix4d> transforms;
    std::vector<TfTokenVector> categories;

    if (instancerId.IsEmpty()) {
        transforms.push_back(_transform);
    } else {
        HdInstancer::_SyncInstancerAndParents(renderIndex, instancerId);
        instancer = static_cast<UtilityInstancer*>(renderIndex.GetInstancer(instancerId));
        if (TF_VERIFY(instancer)) {
            const VtMatrix4dArray instances = instancer->computeInstanceTransforms(GetId());
            transforms.reserve(instances.size());
            for (const GfMatrix4d& instance : instances) {
                transforms.push_back(_transform * instance);
            }
            categories = static_cast<UtilityInstancer*>(instancer)->computeInstanceCategories(GetId());
            if (categories.size() != transforms.size()) {
                categories.clear();
            }
        }
    }
    scene->setInstances(_geometry, transforms, categories);

    const GfVec2d shutter = utilityMotionShutter(*renderIndex.GetRenderDelegate(), kUtilitySettingTransformMotionBlur);
    std::shared_ptr<UtilityScene::TransformMotion> motion;

    if (shutter[1] > shutter[0] && !transforms.empty()) {
        HdTimeSampleArray<GfMatrix4d, 4> own;
        std::vector<float> times;

        sceneDelegate->SampleTransform(GetId(), static_cast<float>(shutter[0]), static_cast<float>(shutter[1]), &own);
        for (std::size_t i = 1; i < own.count; ++i) {
            if (own.values[i] != own.values[0]) {
                times.assign(own.times.begin(), own.times.begin() + own.count);
                break;
            }
        }
        if (instancer) {
            const std::vector<float> placed = instancer->motionTimes(GetId());

            times.insert(times.end(), placed.begin(), placed.end());
        }
        times = utilityMotionTimes(times, shutter);
        if (!times.empty()) {
            motion = std::make_shared<UtilityScene::TransformMotion>();
        }
        for (const float time : times) {
            const GfMatrix4d transform = own.count > 0 ? utilityTransformAt(own, time) : _transform;
            std::vector<GfMatrix4d> at;

            if (instancer) {
                const VtMatrix4dArray instances = instancer->computeInstanceTransformsAt(GetId(), time);
                at.reserve(instances.size());
                for (const GfMatrix4d& instance : instances) {
                    at.push_back(transform * instance);
                }
            } else {
                at.push_back(transform);
            }
            if (at.size() != transforms.size()) {
                motion.reset();
                break;
            }
            motion->times.push_back(time);
            motion->transforms.push_back(at);
        }
    }
    scene->setTransformMotion(_geometry, motion);
}

TfToken
UtilityMesh::stPrimvarName(HdSceneDelegate* sceneDelegate) const
{
    const HdSprim* material = GetMaterialId().IsEmpty() ? nullptr : sceneDelegate->GetRenderIndex().GetSprim(HdPrimTypeTokens->material, GetMaterialId());

    return TfToken(material ? static_cast<const UtilityMaterial*>(material)->stPrimvar() : std::string("st"));
}

bool
UtilityMesh::syncMaterial(HdSceneDelegate* sceneDelegate,
                          UtilityScene* scene,
                          const UtilitySubdivRefinement::StopCheck& stopped)
{
    const SdfPath& id = GetId();
    const TfToken name = stPrimvarName(sceneDelegate);
    bool found = false;

    for (int i = 0; i < HdInterpolationCount && !found; ++i) {
        for (const HdPrimvarDescriptor& pv : sceneDelegate->GetPrimvarDescriptors(id, static_cast<HdInterpolation>(i))) {
            found = found || pv.name == name;
        }
    }
    if (!found) {
        scene->setSt(_geometry, false, UtilityScene::eColorBindingConstant, VtVec2fArray());

        return true;
    }

    bool halted = false;
    const Channel<GfVec2f> st = pullChannel<GfVec2f>(sceneDelegate, id, name, _topology, _points.size(), _allTriangles.size(), GfVec2f(0.f), _subdiv.get(), channelIndex(_channelNames, name), stopped, &halted);

    if (halted) {
        return false;
    }
    if (!st.found) {
        scene->setSt(_geometry, false, UtilityScene::eColorBindingConstant, VtVec2fArray());

        return true;
    }

    VtVec2fArray values;
    UtilityScene::ColorBinding binding = UtilityScene::eColorBindingConstant;

    switch (st.rate) {
    case eChannelRateConstant:
        values = VtVec2fArray(1, st.values.cdata()[0]);
        break;
    case eChannelRateUniform:
        binding = UtilityScene::eColorBindingTriangle;
        values.resize(_kept.size());
        for (std::size_t t = 0; t < _kept.size(); ++t) {
            if (haltedAt(t, stopped)) {
                return false;
            }
            values[t] = sampleCorner(st, _kept[t], 0, HdMeshUtil::DecodeFaceIndexFromCoarseFaceParam(_allParams.cdata()[_kept[t]]), 0);
        }
        break;
    case eChannelRateVertex:
        binding = UtilityScene::eColorBindingVertex;
        values = st.values;
        break;
    case eChannelRateCorner:
        binding = UtilityScene::eColorBindingCorner;
        values.resize(_kept.size() * 3);
        for (std::size_t t = 0; t < _kept.size(); ++t) {
            if (haltedAt(t, stopped)) {
                return false;
            }
            for (int corner = 0; corner < 3; ++corner) {
                values[t * 3 + corner] = sampleCorner(st, _kept[t], corner, 0, 0);
            }
        }
        break;
    }
    scene->setSt(_geometry, true, binding, values);

    return true;
}

NATRON_NAMESPACE_EXIT
