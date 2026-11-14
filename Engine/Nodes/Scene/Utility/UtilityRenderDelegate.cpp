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
#include "Engine/Nodes/Scene/Utility/WavefrontCpu.h"
#include "Engine/Nodes/Scene/Utility/WavefrontGpu.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRuntime.h"
#endif

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
std::atomic<int> gLiveDelegates { 0 };
} // anon namespace

UtilityRenderParam::UtilityRenderParam(UtilityScene* scene,
                                       WavefrontExecutor* executor)
    : _scene(scene)
    , _executor(executor)
{
}

UtilityRenderParam::~UtilityRenderParam()
{
    cancelFrameAndWait();
    dropGpuExecutor();
}

UtilityScene*
UtilityRenderParam::scene() const
{
    return _scene;
}

WavefrontExecutor*
UtilityRenderParam::executor() const
{
    return _executor;
}

WavefrontGpu*
UtilityRenderParam::gpuExecutor(std::string* unavailable)
{
#ifdef NATRON_HAVE_GPU
    if (_gpu) {
        if (!_gpu->deviceLost()) {
            return _gpu.get();
        }
        dropGpuExecutor();
    }

    UtilityGpu gpu = acquireUtilityGpu();

    if (!gpu.runtime) {
        *unavailable = gpu.unavailable;

        return nullptr;
    }

    const std::string missing = WavefrontGpu::missingSupport(*gpu.runtime);

    if (!missing.empty()) {
        *unavailable = missing;

        return nullptr;
    }

    std::unique_ptr<WavefrontGpu> executor(new WavefrontGpu(gpu.runtime->registry(), gpu.runtime->pool(), gpu.hold));
    const Tracer::Outcome outcome = executor->status();

    if (outcome.status != Tracer::Status::Ok) {
        *unavailable = outcome.message;

        return nullptr;
    }
    _gpuHold = gpu.hold;
    _gpu = std::move(executor);

    return _gpu.get();
#else
    *unavailable = acquireUtilityGpu().unavailable;

    return nullptr;
#endif
}

void
UtilityRenderParam::dropGpuExecutor()
{
#ifdef NATRON_HAVE_GPU
    _gpu.reset();
#endif
    _gpuHold.reset();
}

void
UtilityRenderParam::rememberGpuOutOfMemory(std::uint64_t version,
                                           std::uint64_t budget,
                                           std::uint64_t generation,
                                           const std::string& message)
{
    _outOfMemory = true;
    _outOfMemoryVersion = version;
    _outOfMemoryBudget = budget;
    _outOfMemoryGeneration = generation;
    _outOfMemoryMessage = message;
}

bool
UtilityRenderParam::gpuOutOfMemory(std::uint64_t version,
                                   std::uint64_t budget,
                                   std::uint64_t generation,
                                   std::string* message) const
{
    if (!_outOfMemory || (version != _outOfMemoryVersion) || (budget != _outOfMemoryBudget) || (generation != _outOfMemoryGeneration)) {
        return false;
    }
    *message = _outOfMemoryMessage;

    return true;
}

void
UtilityRenderParam::releaseGpu()
{
    cancelFrameAndWait();
    dropGpuExecutor();
    // Memory given back may be what the remembered scene lacked.
    _outOfMemory = false;

    std::set<UtilityRenderBuffer*> buffers;
    {
        const std::lock_guard<std::mutex> l(_buffersMutex);
        buffers = _buffers;
    }
    for (UtilityRenderBuffer* buffer : buffers) {
        buffer->releaseDevice();
    }
}

void
UtilityRenderParam::releaseGpuIfLost()
{
    if (frameRunning()) {
        return;
    }
#ifdef NATRON_HAVE_GPU
    if (_gpu && _gpu->deviceLost()) {
        dropGpuExecutor();
    }
#endif
}

void
UtilityRenderParam::addBuffer(UtilityRenderBuffer* buffer)
{
    const std::lock_guard<std::mutex> l(_buffersMutex);

    _buffers.insert(buffer);
}

void
UtilityRenderParam::removeBuffer(UtilityRenderBuffer* buffer)
{
    const std::lock_guard<std::mutex> l(_buffersMutex);

    _buffers.erase(buffer);
}

void
UtilityRenderParam::setFrameReport(const std::string& device,
                                   const std::string& warning)
{
    const std::lock_guard<std::mutex> l(_reportMutex);

    _reportDevice = device;
    _reportWarning = warning;
}

void
UtilityRenderParam::frameReport(std::string* device,
                                std::string* warning) const
{
    const std::lock_guard<std::mutex> l(_reportMutex);

    *device = _reportDevice;
    *warning = _reportWarning;
}

const std::atomic<bool>&
UtilityRenderParam::cancelFlag() const
{
    return _cancel;
}

bool
UtilityRenderParam::isStopped() const
{
    return _stop.load();
}

void
UtilityRenderParam::stop(bool blocking)
{
    _stop.store(true);
    _cancel.store(true);
    if (blocking) {
        waitForFrame();
    }
}

void
UtilityRenderParam::restart()
{
    waitForFrame();
    _stop.store(false);
    _cancel.store(false);
}

void
UtilityRenderParam::syncAgain(const SdfPath& id,
                              HdDirtyBits dirtyBits)
{
    const std::lock_guard<std::mutex> l(_syncAgainMutex);

    _syncAgain.emplace_back(id, dirtyBits);
}

std::vector<std::pair<SdfPath, HdDirtyBits>>
UtilityRenderParam::takeSyncsAgain()
{
    std::vector<std::pair<SdfPath, HdDirtyBits>> taken;
    const std::lock_guard<std::mutex> l(_syncAgainMutex);

    taken.swap(_syncAgain);

    return taken;
}

bool
UtilityRenderParam::startFrame(const std::function<void()>& body)
{
    const std::lock_guard<std::mutex> l(_threadMutex);

    if (_thread.joinable()) {
        _thread.join();
    }
    // In this order, or a stop landing between the two could be wiped out.
    _cancel.store(false);
    if (_stop.load()) {
        _cancel.store(true);

        return false;
    }
    _frameRunning.store(true);
    try {
        _thread = std::thread([this, body]() {
            body();
            _frameRunning.store(false);
        });
    } catch (...) {
        _frameRunning.store(false);
        throw;
    }

    return true;
}

bool
UtilityRenderParam::frameRunning() const
{
    return _frameRunning.load();
}

void
UtilityRenderParam::cancelFrame()
{
    _cancel.store(true);
}

void
UtilityRenderParam::cancelFrameAndWait()
{
    _cancel.store(true);
    waitForFrame();
}

void
UtilityRenderParam::waitForFrame()
{
    const std::lock_guard<std::mutex> l(_threadMutex);

    if (_thread.joinable()) {
        _thread.join();
    }
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
    _executor.reset(new WavefrontCpu());
    _renderParam.reset(new UtilityRenderParam(_scene.get(), _executor.get()));
    _PopulateDefaultSettings(GetRenderSettingDescriptors());
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
    return std::make_shared<UtilityRenderPass>(index, collection, _renderParam.get());
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
        UtilityRenderBuffer* buffer = new UtilityRenderBuffer(bprimId);

        _renderParam->addBuffer(buffer);

        return buffer;
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
    // Every Bprim this delegate makes is a render buffer.
    _renderParam->removeBuffer(static_cast<UtilityRenderBuffer*>(bprim));
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

bool
UtilityRenderDelegate::IsStopSupported() const
{
    return true;
}

bool
UtilityRenderDelegate::IsStopped() const
{
    return _renderParam->isStopped();
}

bool
UtilityRenderDelegate::Stop(bool blocking)
{
    _renderParam->stop(blocking);

    return true;
}

bool
UtilityRenderDelegate::Restart()
{
    _renderParam->restart();

    return true;
}

HdCommandDescriptors
UtilityRenderDelegate::GetCommandDescriptors() const
{
    return {
        HdCommandDescriptor(TfToken(kUtilityCommandReleaseGpu), "Give back the GPU memory the renderer keeps between frames"),
        HdCommandDescriptor(TfToken(kUtilityCommandReleaseLostGpu), "Let go of a lost GPU, so that it can be recreated"),
    };
}

bool
UtilityRenderDelegate::InvokeCommand(const TfToken& command,
                                     const HdCommandArgs& /*args*/)
{
    if (command == TfToken(kUtilityCommandReleaseGpu)) {
        _renderParam->releaseGpu();

        return true;
    }
    if (command == TfToken(kUtilityCommandReleaseLostGpu)) {
        _renderParam->releaseGpuIfLost();

        return true;
    }

    return false;
}

HdRenderSettingDescriptorList
UtilityRenderDelegate::GetRenderSettingDescriptors() const
{
    return { HdRenderSettingDescriptor { "Device", TfToken(kUtilitySettingDevice), VtValue(TfToken(kUtilityDeviceAuto)) },
             HdRenderSettingDescriptor { "Subdivision Level", TfToken(kUtilitySettingSubdivLevel), VtValue(UtilitySubdivRefinement::kDefaultLevel) },
             HdRenderSettingDescriptor { "Max Subdivided Triangles", TfToken(kUtilitySettingMaxSubdivTriangles), VtValue(static_cast<int>(UtilitySubdivRefinement::kDefaultMaxTriangles)) } };
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

    std::string device;
    std::string warning;

    _renderParam->frameReport(&device, &warning);
    for (const std::string& line : stats.warnings) {
        warning += (warning.empty() ? "" : "\n") + line;
    }
    dictionary[kUtilityStatDevice] = VtValue(device);
    dictionary[kUtilityStatWarning] = VtValue(warning);

    return dictionary;
}

NATRON_NAMESPACE_EXIT
