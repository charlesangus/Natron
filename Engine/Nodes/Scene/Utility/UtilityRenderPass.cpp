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

#include "UtilityRenderPass.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include <pxr/base/gf/rect2i.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/vt/types.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/tokens.h>

#include "Engine/AppManager.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/Utility/Tracer.h"
#include "Engine/Nodes/Scene/Utility/UtilityRenderBuffer.h"
#include "Engine/Nodes/Scene/Utility/UtilityRenderDelegate.h"
#include "Engine/Nodes/Scene/Utility/WavefrontGpu.h"
#ifdef NATRON_HAVE_GPU
#include "Engine/GpuContext.h"
#endif

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
WavefrontRect
renderWindow(const HdRenderPassState& state)
{
    WavefrontRect window;
    const CameraUtilFraming& framing = state.GetFraming();

    if (framing.IsValid()) {
        window.x = framing.dataWindow.GetMinX();
        window.y = framing.dataWindow.GetMinY();
        window.width = framing.dataWindow.GetWidth();
        window.height = framing.dataWindow.GetHeight();
    } else {
        const GfVec4f& viewport = state.GetViewport();

        window.x = static_cast<int>(viewport[0]);
        window.y = static_cast<int>(viewport[1]);
        window.width = static_cast<int>(viewport[2]);
        window.height = static_cast<int>(viewport[3]);
    }

    return window;
}

bool
sameWindow(const WavefrontRect& a,
           const WavefrontRect& b)
{
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

// Writes tiles to the buffers bound under the AOV names the shading produces. Any other AOV keeps its
// clear value.
class AovFilm : public WavefrontFilm {
public:
    explicit AovFilm(const HdRenderPassAovBindingVector& bindings)
    {
        for (const HdRenderPassAovBinding& binding : bindings) {
            UtilityRenderBuffer* buffer = dynamic_cast<UtilityRenderBuffer*>(binding.renderBuffer);

            if (!buffer) {
                continue;
            }
            if (binding.aovName == HdAovTokens->color) {
                _color.push_back(buffer);
            } else if (binding.aovName == HdAovTokens->depth) {
                _depth.push_back(buffer);
            } else if (binding.aovName == HdAovTokens->primId) {
                _primId.push_back(buffer);
            } else if (binding.aovName == HdAovTokens->instanceId) {
                _instanceId.push_back(buffer);
            }
        }
    }

    // Only the colour is filtered; the other AOVs take the heaviest sample's value.
    virtual void writeTile(const WavefrontTile& tile) OVERRIDE
    {
        std::size_t index = 0;

        for (int row = 0; row < tile.rect.height; ++row) {
            const unsigned int y = static_cast<unsigned int>(tile.rect.y + row);

            for (int column = 0; column < tile.rect.width; ++column, ++index) {
                const bool hit = tile.hitGeometry[index] != Tracer::kNoHit;
                const float weight = tile.weight ? tile.weight[index] : 1.f;
                const unsigned int x = static_cast<unsigned int>(tile.rect.x + column);

                if (tile.color) {
                    for (UtilityRenderBuffer* buffer : _color) {
                        buffer->addSample(x, y, hit ? tile.color + index * 4 : nullptr, 4, weight, true);
                    }
                }
                for (UtilityRenderBuffer* buffer : _depth) {
                    buffer->addSample(x, y, hit ? tile.depth + index : nullptr, 1, weight, false);
                }
                for (UtilityRenderBuffer* buffer : _primId) {
                    buffer->addSample(x, y, hit ? tile.primId + index : nullptr, weight);
                }
                for (UtilityRenderBuffer* buffer : _instanceId) {
                    buffer->addSample(x, y, hit ? tile.instanceId + index : nullptr, weight);
                }
            }
        }
    }

    virtual UtilityRenderBuffer* deviceColor() const OVERRIDE
    {
        return (_color.size() == 1) ? _color.front() : nullptr;
    }

    virtual WavefrontAovs aovs() const OVERRIDE
    {
        WavefrontAovs aovs;

        aovs.color = !_color.empty();
        aovs.depth = !_depth.empty();
        aovs.primId = !_primId.empty();
        aovs.instanceId = !_instanceId.empty();

        return aovs;
    }

private:
    std::vector<UtilityRenderBuffer*> _color;
    std::vector<UtilityRenderBuffer*> _depth;
    std::vector<UtilityRenderBuffer*> _primId;
    std::vector<UtilityRenderBuffer*> _instanceId;
};

void
setBuffersConverged(const HdRenderPassAovBindingVector& bindings,
                    bool converged)
{
    for (const HdRenderPassAovBinding& binding : bindings) {
        UtilityRenderBuffer* buffer = dynamic_cast<UtilityRenderBuffer*>(binding.renderBuffer);

        if (buffer) {
            buffer->setConverged(converged);
        }
    }
}

void
clearBuffers(const HdRenderPassAovBindingVector& bindings)
{
    for (const HdRenderPassAovBinding& binding : bindings) {
        UtilityRenderBuffer* buffer = dynamic_cast<UtilityRenderBuffer*>(binding.renderBuffer);

        if (buffer) {
            buffer->clear(binding.clearValue);
        }
    }
}

std::string
deviceSettingText(const VtValue& value)
{
    if (value.IsHolding<TfToken>()) {
        return value.UncheckedGet<TfToken>().GetString();
    }
    if (value.IsHolding<std::string>()) {
        return value.UncheckedGet<std::string>();
    }

    return value.IsEmpty() ? std::string(kUtilityDeviceAuto) : std::string();
}

int
intSetting(const VtValue& value,
           int fallback)
{
    if (value.IsHolding<int>()) {
        return value.UncheckedGet<int>();
    }
    if (value.IsHolding<long>()) {
        return static_cast<int>(value.UncheckedGet<long>());
    }
    if (value.IsHolding<double>()) {
        return static_cast<int>(value.UncheckedGet<double>());
    }
    if (value.IsHolding<float>()) {
        return static_cast<int>(value.UncheckedGet<float>());
    }

    return fallback;
}

float
floatSetting(const VtValue& value,
             float fallback)
{
    if (value.IsHolding<float>()) {
        return value.UncheckedGet<float>();
    }
    if (value.IsHolding<double>()) {
        return static_cast<float>(value.UncheckedGet<double>());
    }
    if (value.IsHolding<int>()) {
        return static_cast<float>(value.UncheckedGet<int>());
    }

    return fallback;
}

WavefrontSampling
samplingSettings(const HdRenderDelegate& delegate)
{
    WavefrontSampling sampling;

    sampling.samplesPerSide = std::clamp(intSetting(delegate.GetRenderSetting(TfToken(kUtilitySettingPixelSamples)), kUtilityDefaultPixelSamples), 1, kUtilityMaxPixelSamples);

    const float width = floatSetting(delegate.GetRenderSetting(TfToken(kUtilitySettingFilterWidth)), kUtilityDefaultFilterWidth);

    if (!(width > 0.f) || !std::isfinite(width)) {
        throw std::runtime_error("The Utility renderer's filter width must be a positive number of pixels, not " + std::to_string(width));
    }
    sampling.filterWidth = width;
    sampling.seed = static_cast<std::uint32_t>(intSetting(delegate.GetRenderSetting(TfToken(kUtilitySettingSeed)), 0));

    const VtValue filterValue = delegate.GetRenderSetting(TfToken(kUtilitySettingPixelFilter));
    const std::string filter = filterValue.IsEmpty() ? std::string(kUtilityFilterBlackmanHarris) : deviceSettingText(filterValue);

    if (filter == kUtilityFilterBox) {
        sampling.filter = eKernelPixelFilterBox;
    } else if (filter == kUtilityFilterTriangle) {
        sampling.filter = eKernelPixelFilterTriangle;
    } else if (filter == kUtilityFilterGaussian) {
        sampling.filter = eKernelPixelFilterGaussian;
    } else if (filter == kUtilityFilterBlackmanHarris) {
        sampling.filter = eKernelPixelFilterBlackmanHarris;
    } else {
        throw std::runtime_error("The Utility renderer's pixel filter must be " kUtilityFilterBox ", " kUtilityFilterTriangle ", " kUtilityFilterGaussian " or " kUtilityFilterBlackmanHarris ", not \"" + filter + "\"");
    }

    return sampling;
}

bool
sameSampling(const WavefrontSampling& a,
             const WavefrontSampling& b)
{
    return a.samplesPerSide == b.samplesPerSide && a.filter == b.filter && a.filterWidth == b.filterWidth && a.seed == b.seed;
}

bool
buffersAreConverged(const HdRenderPassAovBindingVector& bindings)
{
    for (const HdRenderPassAovBinding& binding : bindings) {
        if (binding.renderBuffer && !binding.renderBuffer->IsConverged()) {
            return false;
        }
    }

    return true;
}
} // anon namespace

UtilityRenderPass::UtilityRenderPass(HdRenderIndex* index,
                                     const HdRprimCollection& collection,
                                     UtilityRenderParam* renderParam)
    : HdRenderPass(index, collection)
    , _renderParam(renderParam)
{
}

UtilityRenderPass::~UtilityRenderPass()
{
    _renderParam->cancelFrameAndWait();
}

bool
UtilityRenderPass::FrameKey::operator==(const FrameKey& other) const
{
    return device == other.device && version == other.version && worldToView == other.worldToView && projection == other.projection
        && sameWindow(window, other.window) && sameSampling(sampling, other.sampling) && bindings == other.bindings;
}

bool
UtilityRenderPass::IsConverged() const
{
    return _converged.load();
}

void
UtilityRenderPass::_Execute(const HdRenderPassStateSharedPtr& renderPassState,
                            const TfTokenVector& renderTags)
{
    const HdRenderPassAovBindingVector& bindings = renderPassState->GetAovBindings();

    {
        const std::lock_guard<std::mutex> l(_landingMutex);

        if (!_frameFailure.empty()) {
            const std::string failure = _frameFailure;

            _frameFailure.clear();
            throw std::runtime_error(failure);
        }
        if (_renderParam->isStopped()) {
            _converged.store(false);
            setBuffersConverged(bindings, false);
            _frameFinished = false;

            return;
        }
    }

    UtilityScene* const scene = _renderParam->scene();

    const VtValue shown = GetRenderIndex()->GetRenderDelegate()->GetRenderSetting(TfToken(kRenderDelegateUtilityShownTagsSetting));

    if (shown.IsHolding<VtTokenArray>()) {
        const VtTokenArray& tags = shown.UncheckedGet<VtTokenArray>();

        scene->setRenderTags(TfTokenVector(tags.begin(), tags.end()));
    } else {
        scene->setRenderTags(renderTags);
    }

    const std::string deviceText = deviceSettingText(GetRenderIndex()->GetRenderDelegate()->GetRenderSetting(TfToken(kUtilitySettingDevice)));
    Device device = Device::Auto;

    if (deviceText == kUtilityDeviceGpu) {
        device = Device::Gpu;
    } else if (deviceText == kUtilityDeviceCpu) {
        device = Device::Cpu;
    } else if (deviceText != kUtilityDeviceAuto) {
        throw std::runtime_error("The Utility renderer's device must be " kUtilityDeviceAuto ", " kUtilityDeviceGpu " or " kUtilityDeviceCpu ", not \"" + deviceText + "\"");
    }

    WavefrontFrame frame;

    frame.window = renderWindow(*renderPassState);
    frame.sampling = samplingSettings(*GetRenderIndex()->GetRenderDelegate());

    const bool hasCamera = renderPassState->GetCamera() != nullptr;

    if (hasCamera) {
        frame.worldToView = renderPassState->GetWorldToViewMatrix();
        frame.projection = renderPassState->GetProjectionMatrix();
    }

    FrameKey key;

    key.device = device;
    key.version = scene->version();
    key.worldToView = frame.worldToView;
    key.projection = frame.projection;
    key.window = frame.window;
    key.sampling = frame.sampling;
    key.bindings = bindings;

    const bool sameFrame = _hasFrame && key == _frameKey;

    if (_renderParam->frameRunning()) {
        // Waiting for the frame here would hold up whoever holds the caller's locks, so the next
        // Execute() starts the new frame.
        if (!sameFrame) {
            const std::lock_guard<std::mutex> l(_landingMutex);

            _renderParam->cancelFrame();
            _converged.store(false);
            setBuffersConverged(bindings, false);
        }

        return;
    }

    {
        const std::lock_guard<std::mutex> l(_landingMutex);

        if (sameFrame && _frameFinished && buffersAreConverged(bindings)) {
            _converged.store(true);

            return;
        }
        _frameFinished = false;
        _converged.store(false);
        setBuffersConverged(bindings, false);
    }
    clearBuffers(bindings);

    const std::size_t samplesWanted = static_cast<std::size_t>(frame.sampling.samplesPerPixel());

    _renderParam->setSampleProgress(0, samplesWanted);

    const std::shared_ptr<const UtilityScene::Snapshot> snapshot = std::make_shared<UtilityScene::Snapshot>(scene->snapshot());

    key.version = snapshot->version;
    _frameKey = key;
    _hasFrame = true;

    if (!hasCamera || frame.window.width <= 0 || frame.window.height <= 0 || snapshot->instances.empty() || !wavefrontCameraIsUsable(frame)) {
        const std::lock_guard<std::mutex> l(_landingMutex);

        _frameFinished = true;
        _renderParam->setSampleProgress(samplesWanted, samplesWanted);
        setBuffersConverged(bindings, true);
        _converged.store(true);

        return;
    }

    // The thread budget belongs to this thread, not the frame's.
    const int threads = std::max(1, appPTR->getNCPUsAvailableForEffect());
    const HdRenderPassAovBindingVector frameBindings = bindings;

    (void)_renderParam->startFrame([this, snapshot, frame, frameBindings, threads, device, samplesWanted]() {
        const std::atomic<bool>& cancel = _renderParam->cancelFlag();
        bool finished = false;
        std::string failure;

        try {
            AovFilm film(frameBindings);
            // One sample per pixel per call, so that each lands whole in the buffers before the next is drawn.
            const auto drawSamples = [&](WavefrontExecutor& executor) {
                _renderParam->setSampleProgress(0, samplesWanted);
                for (std::size_t sample = 0; sample < samplesWanted; ++sample) {
                    WavefrontFrame pass = frame;

                    pass.sampling.sampleIndex = static_cast<int>(sample);
                    if (!executor.render(*snapshot, pass, film, cancel, threads)) {
                        return false;
                    }
                    _renderParam->setSampleProgress(sample + 1, samplesWanted);
                }

                return true;
            };
            std::string renderedOn = "CPU";
            std::string warning;
            bool onCpu = device == Device::Cpu;

#ifdef NATRON_HAVE_GPU
            const std::uint64_t budget = (device == Device::Auto) ? utilityGpuBudget() : 0;
            const GpuContext* gpuContext = appPTR ? appPTR->getGpuContext() : nullptr;
            const std::uint64_t generation = gpuContext ? gpuContext->generation() : 0;
            std::string remembered;

            if ((device == Device::Auto) && _renderParam->gpuOutOfMemory(snapshot->version, budget, generation, &remembered)) {
                warning = "The GPU ran out of memory, so the frame was rendered on the CPU: " + remembered;
                onCpu = true;
            }
#endif
            if (!onCpu) {
                std::string unavailable;
                WavefrontGpu* gpu = _renderParam->gpuExecutor(&unavailable);

                if (!gpu) {
                    if (device == Device::Gpu) {
                        throw std::runtime_error("The Utility renderer cannot render on the GPU: " + unavailable);
                    }
                    onCpu = true;
                }
#ifdef NATRON_HAVE_GPU
                else {
                    renderedOn = "GPU (" + gpu->deviceName() + ")";
                    finished = drawSamples(*gpu);

                    const Tracer::Outcome outcome = finished ? Tracer::Outcome() : gpu->status();
                    const bool outOfMemory = outcome.status == Tracer::Status::OutOfMemory;
                    const bool lost = outcome.status == Tracer::Status::DeviceLost;

                    const bool failed = outcome.status != Tracer::Status::Ok && outcome.status != Tracer::Status::Stopped;

                    if (outOfMemory && device == Device::Auto) {
                        // The scene is not tried on the GPU again until it or the budget changes, so nothing of it stays there.
                        _renderParam->rememberGpuOutOfMemory(snapshot->version, budget, generation, outcome.message);
                        _renderParam->dropGpuExecutor();
                    } else if (lost || (outOfMemory && gpu->sceneFailed())) {
                        _renderParam->dropGpuExecutor();
                    }
                    if (failed && device == Device::Auto && (outOfMemory || lost)) {
                        warning = std::string(outOfMemory ? "The GPU ran out of memory" : "The GPU was lost") + ", so the frame was rendered on the CPU: " + outcome.message;
                        renderedOn = "CPU";
                        onCpu = true;
                        // The GPU's batches that landed before the failure must not show through.
                        clearBuffers(frameBindings);
                    } else if (failed) {
                        throw std::runtime_error("The Utility renderer failed on the GPU: " + outcome.message);
                    }
                }
#endif
            }
            if (onCpu) {
                finished = drawSamples(*_renderParam->executor());
            }
            _renderParam->setFrameReport(renderedOn, warning);
        } catch (const std::exception& e) {
            failure = e.what();
            if (failure.empty()) {
                failure = "unknown exception";
            }
        } catch (...) {
            failure = "unknown exception";
        }

        const std::lock_guard<std::mutex> l(_landingMutex);

        _frameFailure = failure;
        if (finished && !cancel.load()) {
            _frameFinished = true;
            setBuffersConverged(frameBindings, true);
            _converged.store(true);
        }
    });
}

NATRON_NAMESPACE_EXIT
