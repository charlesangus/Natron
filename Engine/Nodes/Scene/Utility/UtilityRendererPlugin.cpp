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

#include "UtilityRendererPlugin.h"

#include <pxr/imaging/hd/renderDelegateInfo.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/sceneIndexCreateArgsSchema.h>
#include <pxr/imaging/hd/sceneIndexPluginRegistry.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdsi/implicitSurfaceSceneIndex.h>
#include <pxr/imaging/hdsi/velocityMotionResolvingSceneIndex.h>

#include <embree4/rtcore.h>

#include "Engine/Nodes/Scene/Utility/UtilityRenderDelegate.h"

PXR_NAMESPACE_USING_DIRECTIVE

namespace {
const char* const kDisplayName = "Natron Utility";
const char* const kImplicitSurfacePluginId = "HdNatronUtilityImplicitSurfaceSceneIndexPlugin";
const char* const kVelocityMotionPluginId = "HdNatronUtilityVelocityMotionSceneIndexPlugin";

// Embree refuses a device on a CPU without the instruction sets it was built for. An empty string
// means it can open one.
std::string
probeEmbree()
{
    RTCDevice device = rtcNewDevice(nullptr);

    if (!device) {
        return "Embree could not open a device (error " + std::to_string(static_cast<int>(rtcGetDeviceError(nullptr))) + ")";
    }
    rtcReleaseDevice(device);

    return std::string();
}

HdRenderDelegateInfo
renderDelegateInfo()
{
    HdRenderDelegateInfo info;

    info.materialBindingPurpose = HdTokens->full;
    info.isPrimvarFilteringNeeded = false;
    info.isCoordSysSupported = false;

    return info;
}
} // anon namespace

HdNatronUtilityRendererPlugin::HdNatronUtilityRendererPlugin()
{
}

HdNatronUtilityRendererPlugin::~HdNatronUtilityRendererPlugin()
{
}

HdRenderDelegate*
HdNatronUtilityRendererPlugin::CreateRenderDelegate()
{
    return new NATRON_NAMESPACE::UtilityRenderDelegate();
}

HdRenderDelegate*
HdNatronUtilityRendererPlugin::CreateRenderDelegate(const HdRenderSettingsMap& settings)
{
    return new NATRON_NAMESPACE::UtilityRenderDelegate(settings);
}

void
HdNatronUtilityRendererPlugin::DeleteRenderDelegate(HdRenderDelegate* delegate)
{
    delete delegate;
}

bool
HdNatronUtilityRendererPlugin::IsSupported(const HdRendererCreateArgsSchema& /*rendererCreateArgs*/,
                                           std::string* reasonWhyNot) const
{
    static const std::string reason = probeEmbree();

    if (!reason.empty() && reasonWhyNot) {
        *reasonWhyNot = reason;
    }

    return reason.empty();
}

HdContainerDataSourceHandle
HdNatronUtilityRendererPlugin::GetSceneIndexCreateArgs() const
{
    static const HdContainerDataSourceHandle args = HdSceneIndexCreateArgsSchema::Builder()
                                                        .SetMotionBlurSupport(HdRetainedTypedSampledDataSource<bool>::New(true))
                                                        .SetCameraMotionBlurSupport(HdRetainedTypedSampledDataSource<bool>::New(true))
                                                        .SetLegacyRenderDelegateInfo(HdRetainedTypedSampledDataSource<HdRenderDelegateInfo>::New(renderDelegateInfo()))
                                                        .Build();

    return args;
}

HdNatronUtilityImplicitSurfaceSceneIndexPlugin::HdNatronUtilityImplicitSurfaceSceneIndexPlugin()
{
}

HdSceneIndexBaseRefPtr
HdNatronUtilityImplicitSurfaceSceneIndexPlugin::_AppendSceneIndex(const HdSceneIndexBaseRefPtr& inputScene,
                                                                  const HdContainerDataSourceHandle& /*inputArgs*/)
{
    const HdDataSourceBaseHandle toMesh = HdRetainedTypedSampledDataSource<TfToken>::New(HdsiImplicitSurfaceSceneIndexTokens->toMesh);
    const HdContainerDataSourceHandle args = HdRetainedContainerDataSource::New(HdPrimTypeTokens->sphere, toMesh,
                                                                                HdPrimTypeTokens->cube, toMesh,
                                                                                HdPrimTypeTokens->cone, toMesh,
                                                                                HdPrimTypeTokens->cylinder, toMesh,
                                                                                HdPrimTypeTokens->capsule, toMesh,
                                                                                HdPrimTypeTokens->plane, toMesh);

    return HdsiImplicitSurfaceSceneIndex::New(inputScene, args);
}

HdNatronUtilityVelocityMotionSceneIndexPlugin::HdNatronUtilityVelocityMotionSceneIndexPlugin()
{
}

HdSceneIndexBaseRefPtr
HdNatronUtilityVelocityMotionSceneIndexPlugin::_AppendSceneIndex(const HdSceneIndexBaseRefPtr& inputScene,
                                                                 const HdContainerDataSourceHandle& inputArgs)
{
    return HdsiVelocityMotionResolvingSceneIndex::New(inputScene, inputArgs);
}

NATRON_NAMESPACE_ENTER

void
defineUtilityRendererTypes()
{
    HdRendererPluginRegistry::Define<HdNatronUtilityRendererPlugin>();
    HdSceneIndexPluginRegistry::Define<HdNatronUtilityImplicitSurfaceSceneIndexPlugin>();
    HdSceneIndexPluginRegistry::Define<HdNatronUtilityVelocityMotionSceneIndexPlugin>();
}

void
registerUtilitySceneIndexPlugins()
{
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(kDisplayName,
                                                                            TfToken(kImplicitSurfacePluginId),
                                                                            nullptr,
                                                                            0,
                                                                            HdSceneIndexPluginRegistry::InsertionOrderAtStart);
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(kDisplayName,
                                                                            TfToken(kVelocityMotionPluginId),
                                                                            nullptr,
                                                                            0,
                                                                            HdSceneIndexPluginRegistry::InsertionOrderAtStart);
}

NATRON_NAMESPACE_EXIT
