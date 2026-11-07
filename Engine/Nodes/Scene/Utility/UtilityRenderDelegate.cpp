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

#include "UtilityRenderDelegate.h"

#include <atomic>

#include <pxr/base/gf/vec4f.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/bprim.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/extComputation.h>
#include <pxr/imaging/hd/resourceRegistry.h>
#include <pxr/imaging/hd/tokens.h>

#include "Engine/Nodes/Scene/Utility/UtilityInstancer.h"
#include "Engine/Nodes/Scene/Utility/UtilityMesh.h"
#include "Engine/Nodes/Scene/Utility/UtilityRenderBuffer.h"
#include "Engine/Nodes/Scene/Utility/UtilityRenderPass.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
std::atomic<int> gLiveDelegates { 0 };
} // anon namespace

UtilityRenderParam::UtilityRenderParam(UtilityScene* scene)
    : _scene(scene)
{
}

UtilityScene*
UtilityRenderParam::scene() const
{
    return _scene;
}

UtilityRenderDelegate::UtilityRenderDelegate()
{
    construct();
}

UtilityRenderDelegate::UtilityRenderDelegate(const HdRenderSettingsMap& settings)
    : HdRenderDelegate(settings)
{
    construct();
}

void
UtilityRenderDelegate::construct()
{
    _resourceRegistry = std::make_shared<HdResourceRegistry>();
    _scene.reset(new UtilityScene());
    _renderParam.reset(new UtilityRenderParam(_scene.get()));
    ++gLiveDelegates;
}

UtilityRenderDelegate::~UtilityRenderDelegate()
{
    --gLiveDelegates;
}

int
UtilityRenderDelegate::liveCount()
{
    return gLiveDelegates.load();
}

HdRenderParam*
UtilityRenderDelegate::GetRenderParam() const
{
    return _renderParam.get();
}

const TfTokenVector&
UtilityRenderDelegate::GetSupportedRprimTypes() const
{
    static const TfTokenVector types = { HdPrimTypeTokens->mesh };

    return types;
}

const TfTokenVector&
UtilityRenderDelegate::GetSupportedSprimTypes() const
{
    static const TfTokenVector types = { HdPrimTypeTokens->camera, HdPrimTypeTokens->extComputation };

    return types;
}

const TfTokenVector&
UtilityRenderDelegate::GetSupportedBprimTypes() const
{
    static const TfTokenVector types = { HdPrimTypeTokens->renderBuffer };

    return types;
}

HdResourceRegistrySharedPtr
UtilityRenderDelegate::GetResourceRegistry() const
{
    return _resourceRegistry;
}

HdRenderPassSharedPtr
UtilityRenderDelegate::CreateRenderPass(HdRenderIndex* index,
                                        const HdRprimCollection& collection)
{
    return std::make_shared<UtilityRenderPass>(index, collection);
}

HdInstancer*
UtilityRenderDelegate::CreateInstancer(HdSceneDelegate* delegate,
                                       const SdfPath& id)
{
    return new UtilityInstancer(delegate, id);
}

void
UtilityRenderDelegate::DestroyInstancer(HdInstancer* instancer)
{
    delete instancer;
}

HdRprim*
UtilityRenderDelegate::CreateRprim(const TfToken& typeId,
                                   const SdfPath& rprimId)
{
    if (typeId == HdPrimTypeTokens->mesh) {
        return new UtilityMesh(rprimId);
    }
    TF_CODING_ERROR("Unknown Rprim type %s", typeId.GetText());

    return nullptr;
}

void
UtilityRenderDelegate::DestroyRprim(HdRprim* rprim)
{
    delete rprim;
}

HdSprim*
UtilityRenderDelegate::CreateSprim(const TfToken& typeId,
                                   const SdfPath& sprimId)
{
    if (typeId == HdPrimTypeTokens->camera) {
        return new HdCamera(sprimId);
    }
    if (typeId == HdPrimTypeTokens->extComputation) {
        return new HdExtComputation(sprimId);
    }
    TF_CODING_ERROR("Unknown Sprim type %s", typeId.GetText());

    return nullptr;
}

HdSprim*
UtilityRenderDelegate::CreateFallbackSprim(const TfToken& typeId)
{
    return CreateSprim(typeId, SdfPath::EmptyPath());
}

void
UtilityRenderDelegate::DestroySprim(HdSprim* sprim)
{
    delete sprim;
}

HdBprim*
UtilityRenderDelegate::CreateBprim(const TfToken& typeId,
                                   const SdfPath& bprimId)
{
    if (typeId == HdPrimTypeTokens->renderBuffer) {
        return new UtilityRenderBuffer(bprimId);
    }
    TF_CODING_ERROR("Unknown Bprim type %s", typeId.GetText());

    return nullptr;
}

HdBprim*
UtilityRenderDelegate::CreateFallbackBprim(const TfToken& typeId)
{
    return CreateBprim(typeId, SdfPath::EmptyPath());
}

void
UtilityRenderDelegate::DestroyBprim(HdBprim* bprim)
{
    delete bprim;
}

void
UtilityRenderDelegate::CommitResources(HdChangeTracker* /*tracker*/)
{
}

HdAovDescriptor
UtilityRenderDelegate::GetDefaultAovDescriptor(const TfToken& name) const
{
    if (name == HdAovTokens->color) {
        return HdAovDescriptor(HdFormatFloat32Vec4, false, VtValue(GfVec4f(0.f)));
    }
    if (name == HdAovTokens->depth) {
        return HdAovDescriptor(HdFormatFloat32, false, VtValue(1.f));
    }
    if (name == HdAovTokens->primId || name == HdAovTokens->instanceId) {
        return HdAovDescriptor(HdFormatInt32, false, VtValue(-1));
    }

    return HdAovDescriptor();
}

VtDictionary
UtilityRenderDelegate::GetRenderStats() const
{
    const UtilityScene::Stats stats = _scene->stats();
    VtDictionary dictionary;

    dictionary[kUtilityStatGeometryCount] = VtValue(stats.geometryCount);
    dictionary[kUtilityStatInstanceCount] = VtValue(stats.instanceCount);
    dictionary[kUtilityStatTriangleCount] = VtValue(stats.triangleCount);
    dictionary[kUtilityStatTopologyBuilds] = VtValue(stats.topologyBuilds);
    dictionary[kUtilityStatPointUpdates] = VtValue(stats.pointUpdates);
    dictionary[kUtilityStatColorUpdates] = VtValue(stats.colorUpdates);
    dictionary[kUtilityStatSceneVersion] = VtValue(stats.version);

    return dictionary;
}

NATRON_NAMESPACE_EXIT
