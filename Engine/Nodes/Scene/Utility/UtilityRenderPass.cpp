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
#include <chrono>
#include <cmath>
#include <cstddef>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include <pxr/base/gf/rect2i.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/vt/types.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/tokens.h>

#include "Engine/AppManager.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/Utility/Tracer.h"
#include "Engine/Nodes/Scene/Utility/UtilityLight.h"
#include "Engine/Nodes/Scene/Utility/UtilityProfile.h"
#include "Engine/Nodes/Scene/Utility/UtilityRenderBuffer.h"
#include "Engine/Nodes/Scene/Utility/UtilityRenderDelegate.h"
#include "Engine/Nodes/Scene/Utility/UtilitySubdiv.h"
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

// The framing's display window, which overscan leaves inside the data window, in the rows from the
// bottom that the window counts; empty without a framing.
WavefrontRect
displayWindow(const HdRenderPassState& state,
              const WavefrontRect& window)
{
    WavefrontRect display;
    const CameraUtilFraming& framing = state.GetFraming();

    if (!framing.IsValid()) {
        return display;
    }
    const GfRange2f& shown = framing.displayWindow;
    const int dataBottom = framing.dataWindow.GetMinY() + framing.dataWindow.GetHeight();

    display.x = static_cast<int>(std::lround(shown.GetMin()[0]));
    display.y = window.y + dataBottom - static_cast<int>(std::lround(shown.GetMax()[1]));
    display.width = static_cast<int>(std::lround(shown.GetMax()[0])) - display.x;
    display.height = static_cast<int>(std::lround(shown.GetMax()[1] - shown.GetMin()[1]));

    return display;
}

bool
sameWindow(const WavefrontRect& a,
           const WavefrontRect& b)
{
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

// A watched frame's samples are shown no more often than this, as showing them reads the device back.
const std::chrono::milliseconds kProgressivePublishInterval(50);

// Writes tiles to the buffers bound under the AOV names the shading produces. Any other AOV keeps its
// clear value.
class AovFilm : public WavefrontFilm {
public:
    AovFilm(const HdRenderPassAovBindingVector& bindings,
            bool progressive)
        : _progressive(progressive)
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
#ifdef NATRON_HAVE_GPU
        if (!_depth.empty() || !_primId.empty() || !_instanceId.empty()) {
            _deviceAovs.reset(new UtilityDeviceAovs);
            _deviceAovs->setMisses(_depth, _primId, _instanceId);
        }
#endif
    }

    // Only the colour is filtered; the other AOVs take the heaviest sample's value.
    virtual void writeTile(const WavefrontTile& tile) OVERRIDE
    {
        // The buffers were cleared for a frame that may stay on the device; a tile is the first need of their host pixels.
        if (tile.color) {
            for (UtilityRenderBuffer* buffer : _color) {
                buffer->ensureHost();
            }
        }
        for (const std::vector<UtilityRenderBuffer*>* buffers : { &_depth, &_primId, &_instanceId }) {
            for (UtilityRenderBuffer* buffer : *buffers) {
                buffer->ensureHost();
            }
        }

        std::size_t index = 0;

        for (int row = 0; row < tile.rect.height; ++row) {
            const unsigned int y = static_cast<unsigned int>(tile.rect.y + row);

            for (int column = 0; column < tile.rect.width; ++column, ++index) {
                const bool hit = tile.hitGeometry[index] != Tracer::kNoHit;
                const bool shown = hit || tile.background;
                const float weight = tile.weight ? tile.weight[index] : 1.f;
                const unsigned int x = static_cast<unsigned int>(tile.rect.x + column);

                if (tile.color) {
                    for (UtilityRenderBuffer* buffer : _color) {
                        buffer->addSample(x, y, shown ? tile.color + index * 4 : nullptr, 4, weight, true);
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

    virtual UtilityDeviceAovs* deviceAovs() const OVERRIDE
    {
#ifdef NATRON_HAVE_GPU
        return _deviceAovs.get();
#else
        return nullptr;
#endif
    }

    virtual bool showsNextPass() OVERRIDE
    {
        // Latched, so that the passDrawn() that follows shows the pass the executor finished for it.
        _showsNext = _progressive && ((_published == 0) || (std::chrono::steady_clock::now() - _publishedAt >= kProgressivePublishInterval));
        _showsNextLatched = true;

        return _showsNext;
    }

    virtual void passDrawn() OVERRIDE
    {
        const bool shows = _showsNextLatched ? _showsNext : showsNextPass();

        _showsNextLatched = false;
        ++_drawn;
        if (shows) {
            publish();
        }
    }

    virtual void frameDrawn() OVERRIDE
    {
        if (_published != _drawn) {
            publish();
        }
    }

    /**
     * @brief The passes a reader may be shown of done drawn: those published, for a watched frame, whose reader
     * looks again each time the count changes.
     **/
    std::size_t shownPasses(std::size_t done) const
    {
        return _progressive ? _published : done;
    }

    // Before the frame starts over on the host: what the device drew is not shown.
    void leaveDevice()
    {
#ifdef NATRON_HAVE_GPU
        if (_deviceAovs) {
            _deviceAovs->drop();
        }
#endif
        _drawn = 0;
        _published = 0;
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
    void publish()
    {
        const utilityProfile::Scope timed(utilityProfile::Stage::PublishPass);

        for (UtilityRenderBuffer* buffer : _color) {
            buffer->publishPass();
        }
#ifdef NATRON_HAVE_GPU
        if (_deviceAovs) {
            std::string error;

            if (!_deviceAovs->publish(_depth, _primId, _instanceId, &error)) {
                throw std::runtime_error(error);
            }
        } else
#endif
        {
            for (const std::vector<UtilityRenderBuffer*>* buffers : { &_depth, &_primId, &_instanceId }) {
                for (UtilityRenderBuffer* buffer : *buffers) {
                    buffer->publishPass();
                }
            }
        }
        _published = _drawn;
        _publishedAt = std::chrono::steady_clock::now();
    }

    std::vector<UtilityRenderBuffer*> _color;
    std::vector<UtilityRenderBuffer*> _depth;
    std::vector<UtilityRenderBuffer*> _primId;
    std::vector<UtilityRenderBuffer*> _instanceId;
    const bool _progressive;
    std::size_t _drawn = 0;
    std::size_t _published = 0;
    std::chrono::steady_clock::time_point _publishedAt;
    bool _showsNext = false;
    bool _showsNextLatched = false;
#ifdef NATRON_HAVE_GPU
    std::unique_ptr<UtilityDeviceAovs> _deviceAovs;
#endif
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

// deferHost leaves the host pixels unfilled until the frame or a reader needs them.
void
clearBuffers(const HdRenderPassAovBindingVector& bindings,
             bool deferHost)
{
    for (const HdRenderPassAovBinding& binding : bindings) {
        UtilityRenderBuffer* buffer = dynamic_cast<UtilityRenderBuffer*>(binding.renderBuffer);

        if (buffer) {
            buffer->clear(binding.clearValue, deferHost);
        }
    }
}

std::uint32_t
maxCutoutsSetting(const VtValue& value)
{
    const VtValue asInt = VtValue::Cast<int>(value);

    if (asInt.IsEmpty()) {
        return kWavefrontDefaultMaxCutouts;
    }

    return static_cast<std::uint32_t>(std::max(1, asInt.UncheckedGet<int>()));
}

// A ray depth setting, never below 0.
std::uint32_t
depthSetting(const VtValue& value,
             std::uint32_t fallback)
{
    const VtValue asInt = VtValue::Cast<int>(value);

    if (asInt.IsEmpty()) {
        return fallback;
    }

    return static_cast<std::uint32_t>(std::max(0, asInt.UncheckedGet<int>()));
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

// Read wide so that a side too large for a pixel's int sample count is refused rather than wrapped.
int
pixelSamplesSetting(const VtValue& value)
{
    double side = kUtilityDefaultPixelSamples;

    if (value.IsHolding<int>()) {
        side = value.UncheckedGet<int>();
    } else if (value.IsHolding<long>()) {
        side = static_cast<double>(value.UncheckedGet<long>());
    } else if (value.IsHolding<double>()) {
        side = value.UncheckedGet<double>();
    } else if (value.IsHolding<float>()) {
        side = value.UncheckedGet<float>();
    }

    const int largest = static_cast<int>(std::sqrt(static_cast<double>(std::numeric_limits<int>::max())));

    if (!std::isfinite(side) || side >= largest + 1.) {
        throw std::runtime_error("The Utility renderer's pixel samples must be at most " + std::to_string(largest) + ", not " + std::to_string(side));
    }

    return std::max(1, static_cast<int>(side));
}

WavefrontSampling
samplingSettings(const HdRenderDelegate& delegate)
{
    WavefrontSampling sampling;

    sampling.samplesPerSide = pixelSamplesSetting(delegate.GetRenderSetting(TfToken(kUtilitySettingPixelSamples)));

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

WavefrontMapping
mappingSetting(const VtValue& value)
{
    const std::string projection = value.IsEmpty() ? std::string(kUtilityProjectionCamera) : deviceSettingText(value);

    if (projection == kUtilityProjectionCamera) {
        return WavefrontMapping::Camera;
    }
    if (projection == kUtilityProjectionSpherical) {
        return WavefrontMapping::Spherical;
    }
    if (projection == kUtilityProjectionCylindrical) {
        return WavefrontMapping::Cylindrical;
    }
    if (projection == kUtilityProjectionUvUnwrap) {
        return WavefrontMapping::UvUnwrap;
    }
    throw std::runtime_error("The Utility renderer's projection must be " kUtilityProjectionCamera ", " kUtilityProjectionSpherical ", " kUtilityProjectionCylindrical " or " kUtilityProjectionUvUnwrap ", not \"" + projection + "\"");
}

bool
rasterSetting(const VtValue& value)
{
    const std::string mode = value.IsEmpty() ? std::string(kUtilityRenderModeRay) : deviceSettingText(value);

    if (mode == kUtilityRenderModeRay) {
        return false;
    }
    if (mode == kUtilityRenderModeRaster) {
        return true;
    }
    throw std::runtime_error("The Utility renderer's render mode must be " kUtilityRenderModeRay " or " kUtilityRenderModeRaster ", not \"" + mode + "\"");
}

const char*
projectionName(WavefrontMapping mapping)
{
    switch (mapping) {
    case WavefrontMapping::Spherical:
        return "Spherical";
    case WavefrontMapping::Cylindrical:
        return "Cylindrical";
    case WavefrontMapping::UvUnwrap:
        return "UV Unwrap";
    default:
        return "Camera";
    }
}

std::string
joinedWarnings(const std::string& first,
               const std::string& second)
{
    if (first.empty() || second.empty()) {
        return first + second;
    }

    return first + "\n" + second;
}

WavefrontMotion
motionSettings(const HdRenderDelegate& delegate)
{
    WavefrontMotion motion;
    const VtValue shutter = delegate.GetRenderSetting(TfToken(kRenderDelegateUtilityShutterSetting));

    if (!shutter.IsHolding<GfVec2d>() || !(shutter.UncheckedGet<GfVec2d>()[1] > shutter.UncheckedGet<GfVec2d>()[0])) {
        return motion;
    }
    motion.open = shutter.UncheckedGet<GfVec2d>()[0];
    motion.close = shutter.UncheckedGet<GfVec2d>()[1];
    motion.segments = std::max(1, intSetting(delegate.GetRenderSetting(TfToken(kUtilitySettingShutterSegments)), kUtilityDefaultShutterSegments));

    const VtValue times = delegate.GetRenderSetting(TfToken(kRenderDelegateUtilityCameraTimesSetting));
    const VtValue views = delegate.GetRenderSetting(TfToken(kRenderDelegateUtilityCameraViewsSetting));
    const VtValue projections = delegate.GetRenderSetting(TfToken(kRenderDelegateUtilityCameraProjectionsSetting));

    if (times.IsHolding<VtDoubleArray>() && views.IsHolding<VtMatrix4dArray>() && projections.IsHolding<VtMatrix4dArray>()) {
        const VtDoubleArray& t = times.UncheckedGet<VtDoubleArray>();
        const VtMatrix4dArray& v = views.UncheckedGet<VtMatrix4dArray>();
        const VtMatrix4dArray& p = projections.UncheckedGet<VtMatrix4dArray>();

        if (t.size() > 1 && v.size() == t.size() && p.size() == t.size()) {
            motion.cameraTimes.assign(t.begin(), t.end());
            motion.cameraViews.assign(v.begin(), v.end());
            motion.cameraProjections.assign(p.begin(), p.end());
        }
    }

    return motion;
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
// A dome shows behind everything, so a scene of nothing but a dome still renders.
bool
hasDomeLight(const UtilityScene::Snapshot& snapshot)
{
    return std::any_of(snapshot.lights.begin(), snapshot.lights.end(), [](const auto& entry) {
        return entry.second && entry.second->type == UtilityLightDesc::eTypeDome;
    });
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
    return device == other.device && maxCutouts == other.maxCutouts && maxRayDepth == other.maxRayDepth && reflectionDepth == other.reflectionDepth
        && refractionDepth == other.refractionDepth && version == other.version && worldToView == other.worldToView && projection == other.projection
        && sameWindow(window, other.window) && sameWindow(display, other.display) && mapping == other.mapping && raster == other.raster
        && rasterSamples == other.rasterSamples && unwrap == other.unwrap
        && sameSampling(sampling, other.sampling) && motion == other.motion && bindings == other.bindings;
}

bool
UtilityRenderPass::IsConverged() const
{
    return _converged.load();
}

void
UtilityRenderPass::_Sync()
{
    HdRenderIndex* index = GetRenderIndex();
    const HdRenderDelegate* delegate = index->GetRenderDelegate();
    const VtValue level = delegate->GetRenderSetting(TfToken(kUtilitySettingSubdivLevel));
    const VtValue budget = delegate->GetRenderSetting(TfToken(kUtilitySettingMaxSubdivTriangles));

    if (_subdivSettingsSeen && (level != _subdivLevel || budget != _subdivBudget)) {
        index->GetChangeTracker().MarkAllRprimsDirty(HdChangeTracker::DirtyTopology);
    }
    _subdivSettingsSeen = true;
    _subdivLevel = level;
    _subdivBudget = budget;

    // A subdivided mesh refines its UVs with its topology.
    const UtilityUnwrapTarget unwrap = utilityUnwrapTarget(*delegate);

    if (unwrap.prim != _unwrapSeen.prim || unwrap.primvar != _unwrapSeen.primvar) {
        index->GetChangeTracker().MarkAllRprimsDirty(HdChangeTracker::DirtyTopology | HdChangeTracker::DirtyPrimvar);
    }
    _unwrapSeen = unwrap;
    for (const std::pair<SdfPath, HdDirtyBits>& prim : _renderParam->takeSyncsAgain()) {
        if (index->HasRprim(prim.first)) {
            index->GetChangeTracker().MarkRprimDirty(prim.first, prim.second);
        }
    }
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

    frame.maxCutouts = maxCutoutsSetting(GetRenderIndex()->GetRenderDelegate()->GetRenderSetting(TfToken(kUtilitySettingMaxCutouts)));
    frame.maxRayDepth = depthSetting(GetRenderIndex()->GetRenderDelegate()->GetRenderSetting(TfToken(kUtilitySettingMaxRayDepth)), kWavefrontDefaultMaxRayDepth);
    frame.reflectionDepth = depthSetting(GetRenderIndex()->GetRenderDelegate()->GetRenderSetting(TfToken(kUtilitySettingReflectionDepth)), kWavefrontDefaultReflectionDepth);
    frame.refractionDepth = depthSetting(GetRenderIndex()->GetRenderDelegate()->GetRenderSetting(TfToken(kUtilitySettingRefractionDepth)), kWavefrontDefaultRefractionDepth);
    frame.window = renderWindow(*renderPassState);
    frame.display = displayWindow(*renderPassState, frame.window);
    frame.mapping = mappingSetting(GetRenderIndex()->GetRenderDelegate()->GetRenderSetting(TfToken(kUtilitySettingProjection)));
    frame.sampling = samplingSettings(*GetRenderIndex()->GetRenderDelegate());
    frame.raster = rasterSetting(GetRenderIndex()->GetRenderDelegate()->GetRenderSetting(TfToken(kUtilitySettingRenderMode)));
    frame.rasterSamples = std::max(1, intSetting(GetRenderIndex()->GetRenderDelegate()->GetRenderSetting(TfToken(kUtilitySettingRasterSamples)), kWavefrontDefaultRasterSamples));

    const UtilityUnwrapTarget unwrap = utilityUnwrapTarget(*GetRenderIndex()->GetRenderDelegate());

    frame.unwrapPrim = unwrap.prim;
    frame.unwrapPrimvar = unwrap.primvar;
    frame.udimTile = unwrap.udimTile;
    frame.unwrapBounces = unwrap.bounces;

    const WavefrontMotion motion = motionSettings(*GetRenderIndex()->GetRenderDelegate());

    const bool hasCamera = renderPassState->GetCamera() != nullptr;

    if (hasCamera) {
        frame.worldToView = renderPassState->GetWorldToViewMatrix();
        frame.projection = renderPassState->GetProjectionMatrix();
        scene->setViewToWorld(frame.worldToView.GetInverse());
    }

    FrameKey key;

    key.device = device;
    key.version = scene->version();
    key.worldToView = frame.worldToView;
    key.projection = frame.projection;
    key.window = frame.window;
    key.display = frame.display;
    key.mapping = frame.mapping;
    key.raster = frame.raster;
    key.rasterSamples = frame.rasterSamples;
    key.unwrap = unwrap;
    key.maxCutouts = frame.maxCutouts;
    key.maxRayDepth = frame.maxRayDepth;
    key.reflectionDepth = frame.reflectionDepth;
    key.refractionDepth = frame.refractionDepth;
    key.sampling = frame.sampling;
    key.motion = motion;
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
    {
        const utilityProfile::Scope timed(utilityProfile::Stage::Clear);

        clearBuffers(bindings, true);
    }

    const std::size_t samplesWanted = static_cast<std::size_t>(frame.raster ? frame.rasterSamples : frame.sampling.samplesPerPixel());

    _renderParam->setSampleProgress(0, samplesWanted);

    const std::shared_ptr<const UtilityScene::Snapshot> snapshot = std::make_shared<UtilityScene::Snapshot>(scene->snapshot());

    key.version = snapshot->version;
    _frameKey = key;
    _hasFrame = true;

    // A bake of a prim the scene does not draw goes on to fail naming it.
    const bool nothingToSee = snapshot->instances.empty() && !hasDomeLight(*snapshot) && frame.mapping != WavefrontMapping::UvUnwrap;

    if (!hasCamera || frame.window.width <= 0 || frame.window.height <= 0 || nothingToSee || !wavefrontCameraIsUsable(frame)) {
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
    const VtValue progressiveValue = GetRenderIndex()->GetRenderDelegate()->GetRenderSetting(TfToken(kRenderDelegateUtilityProgressiveSetting));
    const bool progressive = progressiveValue.IsHolding<bool>() && progressiveValue.UncheckedGet<bool>();
    const VtValue interactiveValue = GetRenderIndex()->GetRenderDelegate()->GetRenderSetting(TfToken(kRenderDelegateUtilityInteractiveSetting));
    const bool interactive = interactiveValue.IsHolding<bool>() && interactiveValue.UncheckedGet<bool>();

    (void)_renderParam->startFrame([this, snapshot, frame, motion, frameBindings, threads, device, progressive, interactive]() {
        const std::atomic<bool>& cancel = _renderParam->cancelFlag();
        bool finished = false;
        std::string failure;

        try {
            AovFilm film(frameBindings, progressive);
            WavefrontFrame drawn = frame;

            drawn.interactive = interactive;
            std::string rasterWarning;
            // Raster mode runs only on the GPU executor; every other way the frame renders as Ray mode does.
            const auto drawInRay = [&](const std::string& reason) {
                if (drawn.raster) {
                    drawn.raster = false;
                    rasterWarning = "Render Mode is Raster, but the frame was rendered in Ray mode: " + reason + ".";
                }
            };
            std::string renderedOn = "CPU";
            const auto drawSamples = [&](WavefrontExecutor& executor) {
                const std::size_t wanted = static_cast<std::size_t>(drawn.raster ? drawn.rasterSamples : drawn.sampling.samplesPerPixel());
                UtilityProfileFrame profile(drawn.raster ? "Raster" : "Ray", renderedOn, drawn.window.width, drawn.window.height, static_cast<int>(wanted));

                _renderParam->setSampleProgress(0, wanted);

                const bool drew = wavefrontDrawSamples(executor, *snapshot, drawn, motion, film, cancel, threads, [this, wanted, &film](std::size_t done) { _renderParam->setSampleProgress(film.shownPasses(done), wanted); });

                if (drew) {
                    _renderParam->setSampleProgress(film.shownPasses(wanted), wanted);
                }
                profile.setFinished(drew && !cancel.load());

                return drew;
            };
            std::string warning;
            bool onCpu = device == Device::Cpu;

            if (drawn.mapping != WavefrontMapping::Camera) {
                drawInRay(std::string("the ") + projectionName(drawn.mapping) + " projection renders only in Ray mode");
            } else if (onCpu) {
                drawInRay("Device is CPU");
            }

#ifdef NATRON_HAVE_GPU
            const std::uint64_t budget = (device == Device::Auto) ? utilityGpuBudget() : 0;
            const GpuContext* gpuContext = appPTR ? appPTR->getGpuContext() : nullptr;
            const std::uint64_t generation = gpuContext ? gpuContext->generation() : 0;
            std::string remembered;

            if ((device == Device::Auto) && _renderParam->gpuFallback(snapshot->version, budget, generation, &remembered)) {
                warning = remembered;
                onCpu = true;
            }
#endif
            if (!onCpu) {
                std::string unavailable;
                WavefrontGpu* gpu = _renderParam->gpuExecutor(&unavailable);

                if (!gpu) {
                    // A frame stopped while the application's GPU was starting draws nothing, on either device.
                    const bool stopped = cancel.load();

                    if (device == Device::Gpu && !stopped) {
                        throw std::runtime_error("The Utility renderer cannot render on the GPU: " + unavailable);
                    }
                    onCpu = !stopped;
                }
#ifdef NATRON_HAVE_GPU
                else {
                    if (drawn.raster) {
                        const std::string missing = gpu->missingRasterSupport();

                        if (!missing.empty()) {
                            drawInRay(missing);
                        }
                    }
                    renderedOn = "GPU (" + gpu->deviceName() + ")";
                    finished = drawSamples(*gpu);
                    if (!finished) {
                        film.leaveDevice();
                    }

                    const Tracer::Outcome outcome = finished ? Tracer::Outcome() : gpu->status();
                    const bool outOfMemory = outcome.status == Tracer::Status::OutOfMemory;
                    const bool overRange = outcome.status == Tracer::Status::OverStorageRange;
                    const bool lost = outcome.status == Tracer::Status::DeviceLost;
                    const bool unsupported = outcome.status == Tracer::Status::Unsupported;

                    const bool failed = outcome.status != Tracer::Status::Ok && outcome.status != Tracer::Status::Stopped;
                    const std::string fallback = std::string(outOfMemory       ? "The GPU ran out of memory"
                                                                 : overRange   ? "A table is larger than the GPU's storage buffer range"
                                                                 : unsupported ? "The scene is past what the GPU supports"
                                                                               : "The GPU was lost")
                        + ", so the frame was rendered on the CPU: " + outcome.message;

                    if ((outOfMemory || overRange || unsupported) && device == Device::Auto) {
                        // The scene is not tried on the GPU again until it or the budget changes, so nothing of it stays there.
                        _renderParam->rememberGpuFallback(snapshot->version, budget, generation, fallback);
                        _renderParam->dropGpuExecutor();
                    } else if (lost || (outOfMemory && gpu->sceneFailed())) {
                        _renderParam->dropGpuExecutor();
                    }
                    if (failed && device == Device::Auto && (outOfMemory || overRange || unsupported || lost)) {
                        warning = fallback;
                        renderedOn = "CPU";
                        onCpu = true;
                        // The GPU's batches that landed before the failure must not show through.
                        clearBuffers(frameBindings, false);
                    } else if (failed) {
                        throw std::runtime_error("The Utility renderer failed on the GPU: " + outcome.message);
                    }
                }
#endif
            }
            if (onCpu) {
                drawInRay("the frame was rendered on the CPU");
                finished = drawSamples(*_renderParam->executor());
            }
            _renderParam->setFrameReport(renderedOn, joinedWarnings(warning, rasterWarning));
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
