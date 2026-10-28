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

#include "SlitScan.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Engine/ChoiceOption.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/StagePlan.h"
#include "Engine/RectD.h"

#ifdef NATRON_HAVE_GPU
#include <span>

#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
// The frames fetched at once: fetching renders upstream, and every fetched frame stays in memory
// until its chunk is gathered.
constexpr std::size_t kFetchChunk = 8;

// The weight frame f has in a pixel at time t, as the slitscan kernel computes it.
inline float
frameWeight(SlitScanTime::Filter filter,
            float t,
            float f)
{
    if (filter == SlitScanTime::eFilterNearest) {
        return (std::floor(t + 0.5f) == f) ? 1.f : 0.f;
    }
    const float f0 = std::floor(t);
    const float w = t - f0;
    if (f0 == f) {
        return 1.f - w;
    }

    return (f0 + 1.f == f) ? w : 0.f;
}
} // anonymous namespace

namespace SlitScanTime {
Window
window(const Settings& settings,
       double time)
{
    Window result;
    const bool map = (settings.function == eRetimeFunctionRetimeMap);
    const double base = settings.offset + (settings.absolute ? 0. : time);
    double a = 0.;
    double b = 0.;
    if (map) {
        const double shift = settings.absolute ? 0. : time;
        a = std::min(settings.rangeA, settings.rangeB) + shift;
        b = std::max(settings.rangeA, settings.rangeB) + shift;
    } else {
        a = base + std::min(0., settings.gain);
        b = base + std::max(0., settings.gain);
    }
    if (settings.filter == eFilterNearest) {
        a = std::floor(a + 0.5);
        b = std::floor(b + 0.5);
    } else {
        a = std::floor(a);
        b = std::ceil(b);
    }
    result.first = (int)a;
    result.last = (int)b;
    if (result.last - result.first + 1 > kSlitScanMaxFrames) {
        result.clipped = true;
        if (std::abs(base - a) <= std::abs(b - base)) {
            result.last = result.first + kSlitScanMaxFrames - 1;
        } else {
            result.first = result.last - kSlitScanMaxFrames + 1;
        }
    }
    result.base = base;
    result.gain = settings.gain;
    // Without a map every pixel reads 0.
    result.constant = (settings.gain == 0.) || (map && !settings.hasMap);
    if (result.constant) {
        const double t = std::clamp(base, (double)result.first, (double)result.last);
        if (settings.filter == eFilterNearest) {
            result.first = result.last = (int)std::floor(t + 0.5);
            result.time = result.first;
        } else {
            result.first = (int)std::floor(t);
            result.last = (t == std::floor(t)) ? result.first : result.first + 1;
            result.time = t;
        }
        result.base = result.time;
        result.gain = 0.;
    }

    return result;
} // window
} // namespace SlitScanTime

SlitScan::SlitScan(NodePtr node)
    : StagedImageEffect(node)
{
}

SlitScan::~SlitScan()
{
}

const NodeSpec*
SlitScan::nodeSpec() const
{
    return staticNodeSpec<kSlitScanNodeSpec>();
}

const ImagePipelineSpec&
SlitScan::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kSlitScanPipeline, kSlitScanNodeSpec>();
}

void
SlitScan::initializeKnobs()
{
    const KnobPagePtr page = createControlsPage();

    KnobChoicePtr functionKnob = createKnob<KnobChoice>(tr("Retime Function").toStdString());
    functionKnob->setName(kSlitScanParamRetimeFunction);
    functionKnob->setHintToolTip(tr("What gives each pixel its time, before Retime Gain and Retime Offset.").toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kSlitScanRetimeFunctionHorizontalSlit, tr("Horizontal Slit").toStdString(),
                                       tr("A vertical ramp: 0 at the centre of the Source's bottom row, 1 at the centre of its top row.").toStdString()));
        options.push_back(ChoiceOption(kSlitScanRetimeFunctionVerticalSlit, tr("Vertical Slit").toStdString(),
                                       tr("A horizontal ramp: 0 at the centre of the Source's leftmost column, 1 at the centre of its rightmost column.").toStdString()));
        options.push_back(ChoiceOption(kSlitScanRetimeFunctionRetimeMap, tr("Retime Map").toStdString(),
                                       tr("Retime Map Channel of the Retime Map input; 0 when it is not connected.").toStdString()));
        functionKnob->populateChoices(options);
    }
    functionKnob->setDefaultValue((int)SlitScanTime::eRetimeFunctionHorizontalSlit);
    page->addKnob(functionKnob);
    _retimeFunction = functionKnob;

    KnobChannelSelectPtr channelKnob = createKnob<KnobChannelSelect>(tr("Retime Map Channel").toStdString());
    channelKnob->setName(kSlitScanParamRetimeMapChannel);
    channelKnob->setAnimationEnabled(false);
    channelKnob->setDefaultValue(channelKnob->encode(std::string(kNatronColorViewRGBA) + ".A"));
    channelKnob->setHintToolTip(tr("The channel of the Retime Map input, in any of its layers, read with the Retime Map "
                                   "function. A channel the input does not have reads 0.")
                                    .toStdString());
    page->addKnob(channelKnob);
    _retimeMapChannel = channelKnob;

    KnobDoublePtr offsetKnob = createKnob<KnobDouble>(tr("Retime Offset").toStdString());
    offsetKnob->setName(kSlitScanParamRetimeOffset);
    offsetKnob->setHintToolTip(tr("Added to each pixel's time, after Retime Gain.").toStdString());
    offsetKnob->setDefaultValue(0.);
    offsetKnob->setDisplayMinimum(-100.);
    offsetKnob->setDisplayMaximum(100.);
    page->addKnob(offsetKnob);
    _retimeOffset = offsetKnob;

    KnobDoublePtr gainKnob = createKnob<KnobDouble>(tr("Retime Gain").toStdString());
    gainKnob->setName(kSlitScanParamRetimeGain);
    gainKnob->setHintToolTip(tr("Multiplies the retime function. For one row or column per frame with a slit, use the "
                                "Source's height - 1 or width - 1, negated to show older frames further along.")
                                 .toStdString());
    gainKnob->setDefaultValue(-10.);
    gainKnob->setDisplayMinimum(-100.);
    gainKnob->setDisplayMaximum(100.);
    page->addKnob(gainKnob);
    _retimeGain = gainKnob;

    KnobBoolPtr absoluteKnob = createKnob<KnobBool>(tr("Retime Absolute").toStdString());
    absoluteKnob->setName(kSlitScanParamRetimeAbsolute);
    absoluteKnob->setHintToolTip(tr("The times are frames; otherwise they are relative to the current frame.").toStdString());
    absoluteKnob->setDefaultValue(false);
    page->addKnob(absoluteKnob);
    _retimeAbsolute = absoluteKnob;

    KnobIntPtr rangeKnob = createKnob<KnobInt>(tr("Frame Range").toStdString(), 2);
    rangeKnob->setName(kSlitScanParamFrameRange);
    rangeKnob->setHintToolTip(tr("With the Retime Map, the frames read, relative to the current frame unless Retime "
                                 "Absolute is on; times outside it are clamped to it. At most 64 frames are read.")
                                  .toStdString());
    rangeKnob->setDimensionName(0, "min");
    rangeKnob->setDimensionName(1, "max");
    rangeKnob->setDefaultValue(-10, 0);
    rangeKnob->setDefaultValue(0, 1);
    page->addKnob(rangeKnob);
    _frameRange = rangeKnob;

    KnobChoicePtr filterKnob = createKnob<KnobChoice>(tr("Filter").toStdString());
    filterKnob->setName(kSlitScanParamFilter);
    filterKnob->setHintToolTip(tr("How a time between two frames is read.").toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kSlitScanFilterNearest, tr("Nearest").toStdString(), tr("The frame nearest the time.").toStdString()));
        options.push_back(ChoiceOption(kSlitScanFilterLinear, tr("Linear").toStdString(), tr("The two frames around the time, blended linearly.").toStdString()));
        filterKnob->populateChoices(options);
    }
    filterKnob->setDefaultValue((int)SlitScanTime::eFilterNearest);
    page->addKnob(filterKnob);
    _filter = filterKnob;

    if (NodePtr node = getNode()) {
        node->declareLayerKnob(channelKnob, kSlitScanInputRetimeMap, LayerKnobSpec::eRoleInputBound);
    }

    addStageKnobs(page);
} // SlitScan::initializeKnobs

bool
SlitScan::knobChanged(KnobI* k,
                      ValueChangedReasonEnum /*reason*/,
                      ViewSpec /*view*/,
                      double time,
                      bool /*originatedFromMainThread*/)
{
    const KnobIPtr watched[] = { _retimeFunction.lock(), _retimeOffset.lock(), _retimeGain.lock(), _retimeAbsolute.lock(), _frameRange.lock(), _filter.lock() };
    for (const KnobIPtr& knob : watched) {
        if (knob && (knob.get() == k)) {
            updateClipMessage(time);

            return true;
        }
    }

    return false;
}

void
SlitScan::updateClipMessage(double time)
{
    const SlitScanTime::Window w = SlitScanTime::window(settingsAt(time, ViewIdx(0)), time);
    if (w.clipped) {
        setPersistentMessage(eMessageTypeWarning, tr("SlitScan reads at most %1 frames; the window is clipped to frames %2 to %3.").arg(kSlitScanMaxFrames).arg(w.first).arg(w.last).toStdString());
    } else {
        clearPersistentMessage(false);
    }
}

bool
SlitScan::resolveMapChannel(double time,
                            ViewIdx view,
                            ImageLayerDesc* layer,
                            int* channel) const
{
    *channel = -1;
    KnobChannelSelectPtr channelKnob = _retimeMapChannel.lock();
    if (!channelKnob || channelKnob->isNone() || !getInput(kSlitScanInputRetimeMap)) {
        return false;
    }
    std::list<ImageLayerDesc> present;
    const_cast<SlitScan*>(this)->getPresentLayers(time, view, kSlitScanInputRetimeMap, &present);

    return channelKnob->resolve(present, layer, channel) && (*channel >= 0);
}

SlitScanTime::Settings
SlitScan::settingsAt(double time,
                     ViewIdx view) const
{
    SlitScanTime::Settings s;
    if (KnobChoicePtr knob = _retimeFunction.lock()) {
        s.function = (SlitScanTime::RetimeFunction)std::clamp(knob->getValueAtTime(time, 0, view), (int)SlitScanTime::eRetimeFunctionHorizontalSlit, (int)SlitScanTime::eRetimeFunctionRetimeMap);
    }
    if (KnobChoicePtr knob = _filter.lock()) {
        s.filter = (SlitScanTime::Filter)std::clamp(knob->getValueAtTime(time, 0, view), (int)SlitScanTime::eFilterNearest, (int)SlitScanTime::eFilterLinear);
    }
    if (KnobDoublePtr knob = _retimeOffset.lock()) {
        s.offset = knob->getValueAtTime(time, 0, view);
    }
    if (KnobDoublePtr knob = _retimeGain.lock()) {
        s.gain = knob->getValueAtTime(time, 0, view);
    }
    if (KnobBoolPtr knob = _retimeAbsolute.lock()) {
        s.absolute = knob->getValueAtTime(time, 0, view);
    }
    if (KnobIntPtr knob = _frameRange.lock()) {
        s.rangeA = knob->getValueAtTime(time, 0, view);
        s.rangeB = knob->getValueAtTime(time, 1, view);
    }
    if (s.function == SlitScanTime::eRetimeFunctionRetimeMap) {
        ImageLayerDesc layer;
        int channel = -1;
        s.hasMap = resolveMapChannel(time, view, &layer, &channel);
    }

    return s;
}

bool
SlitScan::planAt(double time,
                 ViewIdx view,
                 const RenderScale& scale,
                 Plan* plan) const
{
    EffectInstancePtr source = getInput(kSlitScanInputSource);
    if (!source) {
        return false;
    }
    plan->settings = settingsAt(time, view);
    plan->window = SlitScanTime::window(plan->settings, time);
    plan->rampOrigin = 0;
    plan->rampScale = 0.f;
    plan->mapChannel = -1;
    if (plan->window.constant) {
        return true;
    }
    if (plan->settings.function == SlitScanTime::eRetimeFunctionRetimeMap) {
        return resolveMapChannel(time, view, &plan->mapLayer, &plan->mapChannel);
    }
    // The ramp runs across the Source's region at the current frame, in pixels at scale.
    const RenderScale inputScale = source->supportsRenderScale() ? scale : RenderScale::identity;
    RectD rod;
    bool isProjectFormat = false;
    if ((source->getRegionOfDefinition_public(source->getRenderHash(), time, inputScale, view, &rod, &isProjectFormat) == eStatusFailed) || rod.isNull()) {
        return true;
    }
    NodeMetadataConstPtr metadata = getRenderMetadata(time, view);
    const double par = metadata ? metadata->getPixelAspectRatio(kSlitScanInputSource) : 1.;
    const RectI pixels = rod.toPixelEnclosing(scale, par);
    const bool vertical = (plan->settings.function == SlitScanTime::eRetimeFunctionVerticalSlit);
    const int origin = vertical ? pixels.x1 : pixels.y1;
    const int extent = vertical ? pixels.width() : pixels.height();
    plan->rampOrigin = origin;
    plan->rampScale = (extent > 1) ? (float)(1. / (double)(extent - 1)) : 0.f;

    return true;
} // SlitScan::planAt

Route
SlitScan::route(double time,
                ViewIdx view)
{
    if (!getInput(kSlitScanInputSource)) {
        return Route::black();
    }
    const SlitScanTime::Window w = SlitScanTime::window(settingsAt(time, view), time);
    // Every pixel reads that one frame whole: a constant time, or a window rounded to one frame.
    if (w.first == w.last) {
        return Route::input(kSlitScanInputSource, w.first);
    }

    return Route::none();
}

FramesNeededMap
SlitScan::getFramesNeeded(double time,
                          ViewIdx view)
{
    if (route(time, view).kind != Route::Kind::None) {
        return StagedImageEffect::getFramesNeeded(time, view);
    }
    const SlitScanTime::Settings s = settingsAt(time, view);
    const SlitScanTime::Window w = SlitScanTime::window(s, time);
    FramesNeededMap frames;
    for (int f = w.first; f <= w.last; ++f) {
        RangeD range;
        range.min = range.max = f;
        frames[kSlitScanInputSource][view].push_back(range);
    }
    if (!w.constant && (s.function == SlitScanTime::eRetimeFunctionRetimeMap) && s.hasMap) {
        RangeD range;
        range.min = range.max = time;
        frames[kSlitScanInputRetimeMap][view].push_back(range);
    }

    return frames;
}

StatusEnum
SlitScan::getRegionOfDefinition(U64 /*hash*/,
                                double time,
                                const RenderScale& scale,
                                ViewIdx view,
                                RectD* rod)
{
    *rod = RectD();
    EffectInstancePtr source = getInput(kSlitScanInputSource);
    if (!source) {
        return eStatusOK;
    }
    const SlitScanTime::Window w = SlitScanTime::window(settingsAt(time, view), time);
    const RenderScale inputScale = source->supportsRenderScale() ? scale : RenderScale::identity;
    for (int f = w.first; f <= w.last; ++f) {
        RectD inputRoD;
        bool isProjectFormat = false;
        // An input reports its empty region as a failure; it is still the region it has.
        if ((source->getRegionOfDefinition_public(source->getRenderHash(), f, inputScale, view, &inputRoD, &isProjectFormat) == eStatusFailed) && !inputRoD.isNull()) {
            return eStatusFailed;
        }
        if (inputRoD.isNull()) {
            continue;
        }
        if (rod->isNull()) {
            *rod = inputRoD;
        } else {
            rod->merge(inputRoD);
        }
    }

    return eStatusOK;
}

bool
SlitScan::getInputLayersRead(int inputNb,
                             double time,
                             ViewIdx view,
                             std::list<ImageLayerDesc>* layers)
{
    if (inputNb != kSlitScanInputRetimeMap) {
        return false;
    }
    ImageLayerDesc layer;
    int channel = -1;
    if (!resolveMapChannel(time, view, &layer, &channel)) {
        return false;
    }
    layers->push_back(layer);

    return true;
}

bool
SlitScan::isOpIdentity(double /*time*/,
                       const RenderScale& /*scale*/,
                       const RectI& /*roi*/,
                       ViewIdx /*view*/)
{
    return false;
}

StatusEnum
SlitScan::renderOp(const RenderActionArgs& args,
                   const StagePlan& /*plan*/)
{
    Plan plan;
    if (!planAt(args.time, args.view, args.mappedScale, &plan)) {
        return eStatusFailed;
    }
    const SlitScanTime::Window& w = plan.window;
    const bool useMap = !w.constant && (plan.settings.function == SlitScanTime::eRetimeFunctionRetimeMap);
    const bool vertical = (plan.settings.function == SlitScanTime::eRetimeFunctionVerticalSlit);
    const RectI& roi = args.roi;
    if (roi.isNull()) {
        return eStatusOK;
    }
    const std::size_t width = (std::size_t)roi.width();
    const std::size_t height = (std::size_t)roi.height();
    const float gain = (float)w.gain;
    const float base = (float)w.base;
    const float first = (float)w.first;
    const float last = (float)w.last;

    // Each pixel's time, from the ramp or the map.
    std::vector<float> times(width * height);
    {
        ImagePtr mapImage;
        if (useMap) {
            if (!declaresInputTime(kSlitScanInputRetimeMap, args.time, args.view, args.time)) {
                return eStatusFailed;
            }
            RectI mapRoI;
            // A selected layer is read as it is stored, not in the clip's colour layout.
            mapImage = getImage(kSlitScanInputRetimeMap, args.time, args.mappedScale, args.view, NULL, &plan.mapLayer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &mapRoI);
            if (mapImage && ((mapImage->getBitDepth() != eImageBitDepthFloat) || (plan.mapChannel >= (int)mapImage->getComponentsCount()))) {
                return eStatusFailed;
            }
        }
        std::optional<Image::ReadAccess> mapAccess;
        if (mapImage) {
            mapAccess.emplace(mapImage.get());
        }
        for (int y = roi.y1; y < roi.y2; ++y) {
            for (int x = roi.x1; x < roi.x2; ++x) {
                float v = 0.f;
                if (useMap) {
                    const float* p = mapAccess ? (const float*)mapAccess->pixelAt(x, y) : nullptr;
                    v = p ? p[plan.mapChannel] : 0.f;
                } else if (!w.constant) {
                    v = (float)((vertical ? x : y) - plan.rampOrigin) * plan.rampScale;
                }
                times[(std::size_t)(y - roi.y1) * width + (std::size_t)(x - roi.x1)] = SlitScanTime::pixelTime(v, gain, base, first, last);
            }
        }
    }

    // The frames some pixel reads, oldest first.
    std::vector<int> frames;
    {
        std::vector<char> used((std::size_t)(w.last - w.first + 1), 0);
        for (float t : times) {
            for (int f : { (int)std::floor(t), (int)std::floor(t) + 1, (int)std::floor(t + 0.5f) }) {
                if ((f >= w.first) && (f <= w.last) && (frameWeight(plan.settings.filter, t, (float)f) != 0.f)) {
                    used[(std::size_t)(f - w.first)] = 1;
                }
            }
        }
        for (int f = w.first; f <= w.last; ++f) {
            if (used[(std::size_t)(f - w.first)]) {
                frames.push_back(f);
            }
        }
    }

    for (const std::pair<ImageLayerDesc, ImagePtr>& output : args.outputLayers) {
        const ImagePtr& dst = output.second;
        if (!dst || (dst->getBitDepth() != eImageBitDepthFloat)) {
            return eStatusFailed;
        }
        const int nComps = (int)dst->getComponentsCount();
        {
            Image::WriteAccess dstAccess(dst.get());
            for (int y = roi.y1; y < roi.y2; ++y) {
                float* row = (float*)dstAccess.pixelAt(roi.x1, y);
                if (!row) {
                    return eStatusFailed;
                }
                std::fill(row, row + width * (std::size_t)nComps, 0.f);
            }
        }
        for (std::size_t chunk = 0; chunk < frames.size(); chunk += kFetchChunk) {
            const std::size_t end = std::min(frames.size(), chunk + kFetchChunk);
            // All are fetched before any image is locked: fetching renders upstream. A null image
            // is a frame with nothing there, which reads as black.
            std::vector<ImagePtr> images;
            for (std::size_t i = chunk; i < end; ++i) {
                if (!declaresInputTime(kSlitScanInputSource, args.time, args.view, frames[i])) {
                    return eStatusFailed;
                }
                ImagePtr image = fetchInputAt(kSlitScanInputSource, frames[i], args, &output.first);
                if (image && (image->getBitDepth() != eImageBitDepthFloat)) {
                    return eStatusFailed;
                }
                images.push_back(image);
            }
            Image::WriteAccess dstAccess(dst.get());
            for (std::size_t i = chunk; i < end; ++i) {
                const ImagePtr& image = images[i - chunk];
                if (!image) {
                    continue;
                }
                const int comps = std::min(nComps, (int)image->getComponentsCount());
                const float f = (float)frames[i];
                Image::ReadAccess access(image.get());
                for (int y = roi.y1; y < roi.y2; ++y) {
                    float* row = (float*)dstAccess.pixelAt(roi.x1, y);
                    const float* rowTimes = &times[(std::size_t)(y - roi.y1) * width];
                    for (std::size_t x = 0; x < width; ++x) {
                        const float weight = frameWeight(plan.settings.filter, rowTimes[x], f);
                        if (weight == 0.f) {
                            continue;
                        }
                        const float* p = (const float*)access.pixelAt(roi.x1 + (int)x, y);
                        if (!p) {
                            continue;
                        }
                        float* d = row + x * (std::size_t)nComps;
                        for (int c = 0; c < comps; ++c) {
                            d[c] = d[c] + weight * p[c];
                        }
                    }
                }
            }
        }
    }

    return eStatusOK;
} // SlitScan::renderOp

#ifdef NATRON_HAVE_GPU
namespace {
// SlitScanGpuParams mirrors the push block of slitscan.slang.
struct SlitScanGpuParams {
    int32_t outX;
    int32_t outY;
    uint32_t width;
    uint32_t height;
    uint32_t dstOffset;
    uint32_t dstStride;
    uint32_t nComps;
    uint32_t srcOffset;
    uint32_t srcStride;
    int32_t srcX1;
    int32_t srcY1;
    int32_t srcX2;
    int32_t srcY2;
    uint32_t srcComps;
    uint32_t mapOffset;
    uint32_t mapStride;
    int32_t mapX1;
    int32_t mapY1;
    int32_t mapX2;
    int32_t mapY2;
    uint32_t mapComps;
    uint32_t mapChannel;
    uint32_t function;
    uint32_t filter;
    uint32_t firstPass;
    float frame;
    float rampScale;
    int32_t rampOrigin;
    float gain;
    float base;
    float first;
    float last;
};
static_assert(sizeof(SlitScanGpuParams) <= gpu::GpuKernel::kMaxPushConstantBytes);

// The float index of pixel (x, y) of v, false when the view does not fit the kernel's 32-bit
// indices.
bool
gpuFloatIndex(const GpuImageView& v,
              int x,
              int y,
              uint32_t* index)
{
    const uint64_t base = v.byteOffset / sizeof(float);
    const uint64_t rows = (uint64_t)std::max(0, v.bounds.height());

    if (base + rows * (uint64_t)v.rowStrideFloats > UINT32_MAX) {
        return false;
    }
    *index = (uint32_t)(base + (uint64_t)(y - v.bounds.y1) * v.rowStrideFloats + (uint64_t)(x - v.bounds.x1) * v.nComps);

    return true;
}
} // anonymous namespace
#endif // NATRON_HAVE_GPU

bool
SlitScan::canRenderOpOnGpu(const GpuQuery& query) const
{
#ifdef NATRON_HAVE_GPU
    if ((query.bitDepth != eImageBitDepthFloat) || query.hasMask || !query.plane.isColorLayer()) {
        return false;
    }
    Plan plan;
    EffectInstancePtr source = getInput(kSlitScanInputSource);
    if (!source || !planAt(query.time, query.view, query.mappedScale, &plan) || (plan.window.first == plan.window.last)) {
        return false;
    }
    const RenderScale inputScale = source->supportsRenderScale() ? query.mappedScale : RenderScale::identity;
    for (int f = plan.window.first; f <= plan.window.last; ++f) {
        const double t = f;
        bool found = false;
        for (const GpuInputTimes& times : query.inputTimes) {
            if ((times.inputNb == kSlitScanInputSource) && (std::find(times.times.begin(), times.times.end(), t) != times.times.end())) {
                found = true;
            }
        }
        if (!found && (t != query.time)) {
            return false;
        }
        // A frame with nothing there reads as black on the CPU; a region has no view of it.
        RectD rod;
        bool isProjectFormat = false;
        if ((source->getRegionOfDefinition_public(source->getRenderHash(), t, inputScale, query.view, &rod, &isProjectFormat) == eStatusFailed) || rod.isNull()) {
            return false;
        }
    }

    return true;
#else
    Q_UNUSED(query);

    return false;
#endif
}

bool
SlitScan::readsGpuInput(int inputNb,
                        const ImageLayerDesc& /*plane*/,
                        double time,
                        ViewIdx view) const
{
    if (inputNb != kSlitScanInputRetimeMap) {
        return true;
    }
    const SlitScanTime::Settings s = settingsAt(time, view);

    return (s.function == SlitScanTime::eRetimeFunctionRetimeMap) && s.hasMap && !SlitScanTime::window(s, time).constant;
}

bool
SlitScan::getOpGpuInputLayer(int inputNb,
                             const ImageLayerDesc& plane,
                             double time,
                             ViewIdx view,
                             ImageLayerDesc* layer) const
{
    if (inputNb == kSlitScanInputRetimeMap) {
        int channel = -1;
        if (resolveMapChannel(time, view, layer, &channel) && (layer->getNumComponents() > 0)) {
            return true;
        }
        stageGpuInputLayer(*this, inputNb, plane, time, view, layer);

        return true;
    }
    *layer = plane;

    return true;
}

StatusEnum
SlitScan::renderOpGpu(const GpuRenderArgs& args)
{
#ifdef NATRON_HAVE_GPU
    const GpuQuery& query = args.query;
    Plan plan;
    if (!planAt(query.time, query.view, query.mappedScale, &plan) || args.outputRect.isNull()) {
        return eStatusFailed;
    }
    const SlitScanTime::Window& w = plan.window;
    const bool useMap = !w.constant && (plan.settings.function == SlitScanTime::eRetimeFunctionRetimeMap);

    SlitScanGpuParams params = SlitScanGpuParams();
    params.outX = args.outputRect.x1;
    params.outY = args.outputRect.y1;
    params.width = (uint32_t)args.outputRect.width();
    params.height = (uint32_t)args.outputRect.height();
    if (!args.output.buffer || !gpuFloatIndex(args.output, args.outputRect.x1, args.outputRect.y1, &params.dstOffset)) {
        return eStatusFailed;
    }
    params.dstStride = (uint32_t)args.output.rowStrideFloats;
    params.nComps = (uint32_t)args.output.nComps;
    params.function = w.constant ? 0u : (uint32_t)plan.settings.function;
    params.filter = (uint32_t)plan.settings.filter;
    params.rampScale = w.constant ? 0.f : plan.rampScale;
    params.rampOrigin = plan.rampOrigin;
    params.gain = (float)w.gain;
    params.base = (float)w.base;
    params.first = (float)w.first;
    params.last = (float)w.last;

    // A map with nothing there packs as empty bounds, which the kernel reads as 0.
    const gpu::GpuBuffer* mapBuffer = nullptr;
    if (useMap && (kSlitScanInputRetimeMap < (int)args.inputs.size())) {
        const GpuImageView& map = args.inputs[kSlitScanInputRetimeMap];
        if (map.buffer && !map.bounds.isNull()) {
            if ((plan.mapChannel < 0) || (plan.mapChannel >= map.nComps) || !gpuFloatIndex(map, map.bounds.x1, map.bounds.y1, &params.mapOffset)) {
                return eStatusFailed;
            }
            mapBuffer = map.buffer;
            params.mapStride = (uint32_t)map.rowStrideFloats;
            params.mapX1 = map.bounds.x1;
            params.mapY1 = map.bounds.y1;
            params.mapX2 = map.bounds.x2;
            params.mapY2 = map.bounds.y2;
            params.mapComps = (uint32_t)map.nComps;
            params.mapChannel = (uint32_t)plan.mapChannel;
        }
    }

    gpu::GpuRecordContext& ctx = args.context;
    for (int f = w.first; f <= w.last; ++f) {
        const GpuImageView* frame = args.inputAt(kSlitScanInputSource, f);
        if (!frame || !frame->buffer || frame->bounds.isNull() || !gpuFloatIndex(*frame, frame->bounds.x1, frame->bounds.y1, &params.srcOffset)) {
            return eStatusFailed;
        }
        params.srcStride = (uint32_t)frame->rowStrideFloats;
        params.srcX1 = frame->bounds.x1;
        params.srcY1 = frame->bounds.y1;
        params.srcX2 = frame->bounds.x2;
        params.srcY2 = frame->bounds.y2;
        params.srcComps = (uint32_t)frame->nComps;
        params.firstPass = (f == w.first) ? 1u : 0u;
        params.frame = (float)f;
        // The kernel reads no map pixel when it has empty bounds, so the frame stands in for it.
        const gpu::GpuStatus status = ctx.dispatch(gpu::GpuKernelId::slitscan, { frame->buffer, args.output.buffer, mapBuffer ? mapBuffer : frame->buffer }, params,
                                                   { params.width, params.height, 1u });
        if (!status) {
            return eStatusFailed;
        }
        ctx.computeBarrier();
    }

    return eStatusOK;
#else
    Q_UNUSED(args);

    return eStatusFailed;
#endif
} // SlitScan::renderOpGpu

NATRON_NAMESPACE_EXIT
