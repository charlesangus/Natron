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

#include "FrameBlend.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <list>
#include <optional>
#include <utility>
#include <vector>

#include "Engine/ChoiceOption.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/Nodes/Image/StagePlan.h"
#include "Engine/RectD.h"

NATRON_NAMESPACE_ENTER

namespace {
// The frames fetched, and so held in memory, at once while folding.
constexpr std::size_t kFrameChunk = 8;

// The Average fold's step blends: frame i weighs factor^(n - 1 - i), so End Frame weighs 1.
std::vector<float>
foldBlends(const FrameBlendSamples& samples)
{
    const std::size_t n = samples.times.size();
    std::vector<double> weights(n, 1.);
    for (std::size_t i = n; i > 1; --i) {
        weights[i - 2] = weights[i - 1] * (double)samples.factor;
    }

    return TimeBlend::stepBlends(weights);
}
} // anonymous namespace

FrameBlendSamples
frameBlendSamples(double time,
                  int start,
                  int end,
                  bool absolute,
                  int interval,
                  double decay)
{
    FrameBlendSamples result;
    interval = std::max(1, interval);
    decay = std::clamp(decay, 0., 1.);
    result.factor = (float)std::pow(1. - decay, (double)interval);
    const long long span = std::llabs((long long)end - (long long)start) + 1;
    const long long n = std::max(1LL, span / interval);
    const long long step = (start > end) ? -interval : interval;
    const double offset = absolute ? 0. : time;
    const long long first = (long long)end - (n - 1) * step;
    result.times.reserve((std::size_t)n);
    for (long long i = 0; i < n; ++i) {
        result.times.push_back(offset + (double)(first + i * step));
    }

    return result;
}

FrameBlend::FrameBlend(NodePtr node)
    : StagedImageEffect(node)
{
}

FrameBlend::~FrameBlend()
{
}

const NodeSpec*
FrameBlend::nodeSpec() const
{
    return staticNodeSpec<kFrameBlendNodeSpec>();
}

const ImagePipelineSpec&
FrameBlend::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kFrameBlendPipeline, kFrameBlendNodeSpec>();
}

void
FrameBlend::initializeKnobs()
{
    const KnobPagePtr page = createControlsPage();

    KnobIntPtr startKnob = createKnob<KnobInt>(tr("Start Frame").toStdString());
    startKnob->setName(kFrameBlendParamStartFrame);
    startKnob->setHintToolTip(tr("The first frame of the range blended, relative to the current frame unless Absolute is on.").toStdString());
    startKnob->setDefaultValue(-5);
    startKnob->setDisplayMinimum(-10);
    startKnob->setDisplayMaximum(10);
    startKnob->setAddNewLine(false);
    page->addKnob(startKnob);
    _startFrame = startKnob;

    KnobIntPtr endKnob = createKnob<KnobInt>(tr("End Frame").toStdString());
    endKnob->setName(kFrameBlendParamEndFrame);
    endKnob->setHintToolTip(tr("The last frame of the range blended, relative to the current frame unless Absolute is on. It is "
                               "always read, and the frames before it every Frame Interval frames back to Start Frame.")
                                .toStdString());
    endKnob->setDefaultValue(0);
    endKnob->setDisplayMinimum(-10);
    endKnob->setDisplayMaximum(10);
    page->addKnob(endKnob);
    _endFrame = endKnob;

    KnobBoolPtr absoluteKnob = createKnob<KnobBool>(tr("Absolute").toStdString());
    absoluteKnob->setName(kFrameBlendParamAbsolute);
    absoluteKnob->setHintToolTip(tr("Read Start Frame and End Frame as frame numbers rather than offsets from the current frame.").toStdString());
    absoluteKnob->setDefaultValue(false);
    absoluteKnob->setAddNewLine(false);
    page->addKnob(absoluteKnob);
    _absolute = absoluteKnob;

    KnobButtonPtr inputRangeKnob = createKnob<KnobButton>(tr("Input Range").toStdString());
    inputRangeKnob->setName(kFrameBlendParamInputRange);
    inputRangeKnob->setHintToolTip(tr("Set Start Frame and End Frame to the Source's frame range and turn Absolute on.").toStdString());
    inputRangeKnob->setEvaluateOnChange(false);
    page->addKnob(inputRangeKnob);
    _inputRange = inputRangeKnob;

    KnobIntPtr intervalKnob = createKnob<KnobInt>(tr("Frame Interval").toStdString());
    intervalKnob->setName(kFrameBlendParamFrameInterval);
    intervalKnob->setHintToolTip(tr("Read one frame out of this many, counting back from End Frame.").toStdString());
    intervalKnob->setDefaultValue(1);
    intervalKnob->setMinimum(1);
    intervalKnob->setDisplayMinimum(1);
    intervalKnob->setDisplayMaximum(10);
    page->addKnob(intervalKnob);
    _frameInterval = intervalKnob;

    KnobChoicePtr operationKnob = createKnob<KnobChoice>(tr("Operation").toStdString());
    operationKnob->setName(kFrameBlendParamOperation);
    operationKnob->setHintToolTip(tr("How the frames are combined, from the oldest to End Frame.").toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kFrameBlendOperationAverage, tr("Average").toStdString(), tr("The weighted mean of the frames.").toStdString()));
        options.push_back(ChoiceOption(kFrameBlendOperationMin, tr("Min").toStdString(), tr("The minimum of the frames.").toStdString()));
        options.push_back(ChoiceOption(kFrameBlendOperationMax, tr("Max").toStdString(), tr("The maximum of the frames.").toStdString()));
        options.push_back(ChoiceOption(kFrameBlendOperationSum, tr("Sum").toStdString(), tr("The sum of the frames.").toStdString()));
        options.push_back(ChoiceOption(kFrameBlendOperationProduct, tr("Product").toStdString(), tr("The product of the frames.").toStdString()));
        options.push_back(ChoiceOption(kFrameBlendOperationOver, tr("Over").toStdString(),
                                       tr("Each frame over the frames before it, through its alpha.").toStdString()));
        operationKnob->populateChoices(options);
    }
    operationKnob->setDefaultValue((int)TimeBlend::eOperationAverage);
    page->addKnob(operationKnob);
    _operation = operationKnob;

    KnobDoublePtr decayKnob = createKnob<KnobDouble>(tr("Decay").toStdString());
    decayKnob->setName(kFrameBlendParamDecay);
    decayKnob->setHintToolTip(tr("How much a frame fades per frame it is older than End Frame: each frame weighs (1 - decay) "
                                 "times the next one. 0 weighs every frame alike; 1 keeps End Frame only.")
                                  .toStdString());
    decayKnob->setDefaultValue(0.);
    decayKnob->setMinimum(0.);
    decayKnob->setMaximum(1.);
    decayKnob->setDisplayMinimum(0.);
    decayKnob->setDisplayMaximum(1.);
    page->addKnob(decayKnob);
    _decay = decayKnob;

    KnobBoolPtr outputCountKnob = createKnob<KnobBool>(tr("Output Count").toStdString());
    outputCountKnob->setName(kFrameBlendParamOutputCount);
    outputCountKnob->setHintToolTip(tr("Write the number of frames blended to the alpha of an RGBA or Alpha output.").toStdString());
    outputCountKnob->setDefaultValue(false);
    page->addKnob(outputCountKnob);
    _outputCount = outputCountKnob;

    addStageKnobs(page);
} // FrameBlend::initializeKnobs

bool
FrameBlend::knobChanged(KnobI* k,
                        ValueChangedReasonEnum /*reason*/,
                        ViewSpec /*view*/,
                        double /*time*/,
                        bool /*originatedFromMainThread*/)
{
    KnobButtonPtr inputRangeKnob = _inputRange.lock();
    if (!inputRangeKnob || (k != inputRangeKnob.get())) {
        return false;
    }
    EffectInstancePtr input = getInput(0);
    KnobIntPtr startKnob = _startFrame.lock();
    KnobIntPtr endKnob = _endFrame.lock();
    KnobBoolPtr absoluteKnob = _absolute.lock();
    if (!input || !startKnob || !endKnob || !absoluteKnob) {
        return true;
    }
    double first = 0.;
    double last = 0.;
    input->getFrameRange_public(input->getRenderHash(), &first, &last);
    // An unbounded range, such as a generator's, has no frames to name.
    if ((first <= std::numeric_limits<int>::min()) || (last >= std::numeric_limits<int>::max()) || (first > last)) {
        return true;
    }
    absoluteKnob->setValue(true);
    startKnob->setValue((int)std::floor(first));
    endKnob->setValue((int)std::floor(last));

    return true;
}

FrameBlend::Settings
FrameBlend::settingsAt(double time,
                       ViewIdx view) const
{
    Settings settings;
    KnobIntPtr startKnob = _startFrame.lock();
    KnobIntPtr endKnob = _endFrame.lock();
    KnobBoolPtr absoluteKnob = _absolute.lock();
    KnobIntPtr intervalKnob = _frameInterval.lock();
    KnobChoicePtr operationKnob = _operation.lock();
    KnobDoublePtr decayKnob = _decay.lock();
    KnobBoolPtr outputCountKnob = _outputCount.lock();
    const int start = startKnob ? startKnob->getValueAtTime(time, 0, view) : -5;
    const int end = endKnob ? endKnob->getValueAtTime(time, 0, view) : 0;
    const bool absolute = absoluteKnob ? absoluteKnob->getValueAtTime(time, 0, view) : false;
    const int interval = intervalKnob ? intervalKnob->getValueAtTime(time, 0, view) : 1;
    const double decay = decayKnob ? decayKnob->getValueAtTime(time, 0, view) : 0.;
    settings.samples = frameBlendSamples(time, start, end, absolute, interval, decay);
    if (operationKnob) {
        settings.operation = (TimeBlend::Operation)std::clamp(operationKnob->getValueAtTime(time, 0, view),
                                                              (int)TimeBlend::eOperationAverage, (int)TimeBlend::eOperationOver);
    }
    settings.outputCount = outputCountKnob ? outputCountKnob->getValueAtTime(time, 0, view) : false;

    return settings;
}

bool
FrameBlend::stagesReadSource(double time,
                             ViewIdx view) const
{
    if (isMaskApplied() || (getMixValue(time, view) != 1.)) {
        return true;
    }
    KnobChannelSetPtr channels = std::dynamic_pointer_cast<KnobChannelSet>(getKnobByName(kNodeParamChannelSet));
    if (!channels) {
        return false;
    }
    const std::vector<ResolvedLayer> resolved = channels->resolve(std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()));
    for (const ResolvedLayer& layer : resolved) {
        if (layer.desc.isColorLayer()) {
            return !layer.channels.all();
        }
    }

    // The colour layer is not processed at all: it passes through.
    return true;
}

Route
FrameBlend::route(double time,
                  ViewIdx view)
{
    if (!getInput(0)) {
        return Route::black();
    }
    const Settings settings = settingsAt(time, view);
    if ((settings.samples.times.size() == 1) && !settings.outputCount) {
        const double t = settings.samples.times[0];
        if ((t == time) || !stagesReadSource(time, view)) {
            return Route::input(0, t);
        }
    }

    return Route::none();
}

FramesNeededMap
FrameBlend::getFramesNeeded(double time,
                            ViewIdx view)
{
    if (route(time, view).kind != Route::Kind::None) {
        return StagedImageEffect::getFramesNeeded(time, view);
    }
    FramesNeededMap frames;
    const Settings settings = settingsAt(time, view);
    std::vector<RangeD>& source = frames[0][view];
    for (double t : settings.samples.times) {
        RangeD range;
        range.min = range.max = t;
        source.push_back(range);
    }
    const std::vector<double>& times = settings.samples.times;
    if (stagesReadSource(time, view) && (std::find(times.begin(), times.end(), time) == times.end())) {
        RangeD range;
        range.min = range.max = time;
        source.push_back(range);
    }
    if (isMaskApplied()) {
        RangeD range;
        range.min = range.max = time;
        frames[getMaskInput()][view].push_back(range);
    }

    return frames;
}

StatusEnum
FrameBlend::getRegionOfDefinition(U64 /*hash*/,
                                  double time,
                                  const RenderScale& scale,
                                  ViewIdx view,
                                  RectD* rod)
{
    *rod = RectD();
    EffectInstancePtr input = getInput(0);
    if (!input) {
        return eStatusOK;
    }
    std::vector<double> times = settingsAt(time, view).samples.times;
    if (stagesReadSource(time, view)) {
        times.push_back(time);
    }
    const RenderScale inputScale = input->supportsRenderScale() ? scale : RenderScale::identity;
    for (double t : times) {
        RectD inputRoD;
        bool isProjectFormat = false;
        // An input reports its empty region as a failure; it is still the region it has.
        if ((input->getRegionOfDefinition_public(input->getRenderHash(), t, inputScale, view, &inputRoD, &isProjectFormat) == eStatusFailed) && !inputRoD.isNull()) {
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
FrameBlend::isOpIdentity(double /*time*/,
                         const RenderScale& /*scale*/,
                         const RectI& /*roi*/,
                         ViewIdx /*view*/)
{
    return false;
}

StatusEnum
FrameBlend::renderOp(const RenderActionArgs& args,
                     const StagePlan& plan)
{
    const Settings settings = settingsAt(args.time, args.view);
    const std::vector<double>& times = settings.samples.times;
    const std::size_t n = times.size();
    for (double t : times) {
        if (!declaresInputTime(0, args.time, args.view, t)) {
            return eStatusFailed;
        }
    }
    const std::vector<float> blends = foldBlends(settings.samples);
    const RectI& roi = args.roi;
    const int width = roi.x2 - roi.x1;
    const int height = roi.y2 - roi.y1;
    if ((width <= 0) || (height <= 0)) {
        return eStatusOK;
    }

    for (const std::pair<ImageLayerDesc, ImagePtr>& output : args.outputLayers) {
        if (!output.second || (output.second->getBitDepth() != eImageBitDepthFloat)) {
            return eStatusFailed;
        }
        const int nComps = (int)output.second->getComponentsCount();
        std::vector<float> acc((std::size_t)width * height * nComps, 0.f);
        std::vector<float> in(nComps, 0.f);
        for (std::size_t chunk = 0; chunk < n; chunk += kFrameChunk) {
            const std::size_t chunkEnd = std::min(n, chunk + kFrameChunk);
            // Every frame of the chunk is fetched before any image is locked: fetching renders
            // upstream. A null image is an input with nothing there, which reads as black.
            std::vector<ImagePtr> images;
            for (std::size_t k = chunk; k < chunkEnd; ++k) {
                ImagePtr image = fetchInputAt(0, times[k], args, &output.first);
                if (image && (image->getBitDepth() != eImageBitDepthFloat)) {
                    return eStatusFailed;
                }
                images.push_back(image);
            }
            for (std::size_t k = chunk; k < chunkEnd; ++k) {
                const ImagePtr& image = images[k - chunk];
                const int comps = image ? std::min(nComps, (int)image->getComponentsCount()) : 0;
                std::optional<Image::ReadAccess> access;
                if (image) {
                    access.emplace(image.get());
                }
                const float blend = (k > 0) ? blends[k - 1] : 0.f;
                for (int y = roi.y1; y < roi.y2; ++y) {
                    float* row = &acc[(std::size_t)(y - roi.y1) * width * nComps];
                    for (int x = roi.x1; x < roi.x2; ++x) {
                        const float* p = access ? (const float*)access->pixelAt(x, y) : nullptr;
                        for (int c = 0; c < nComps; ++c) {
                            in[c] = (p && (c < comps)) ? p[c] : 0.f;
                        }
                        float* r = &row[(std::size_t)(x - roi.x1) * nComps];
                        if (k == 0) {
                            std::copy(in.begin(), in.end(), r);
                        } else {
                            TimeBlend::foldPixel(settings.operation, settings.samples.factor, blend, nComps, &in[0], r);
                        }
                    }
                }
            }
        }
        if (settings.outputCount && ((nComps == 4) || (nComps == 1))) {
            const int alpha = nComps - 1;
            for (std::size_t i = (std::size_t)alpha; i < acc.size(); i += (std::size_t)nComps) {
                acc[i] = (float)n;
            }
        }

        const auto row = [&](const RowIO& io) {
            const int comps = std::min(io.nComps, nComps);
            const float* from = &acc[((std::size_t)(io.y - roi.y1) * width + (std::size_t)(io.x0 - roi.x1)) * nComps];
            for (int c = 0; c < comps; ++c) {
                if (!io.channels[pixelKernelChannelBit(io.nComps, c)]) {
                    continue;
                }
                for (int i = 0; i < io.width; ++i) {
                    io.dst[(std::size_t)i * io.nComps + c] = from[(std::size_t)i * nComps + c];
                }
            }
        };
        StagePlan layerPlan = plan;
        // A finish that copies the fold reads no source, which then needs no fetch at the current frame.
        if (stageFinishIsCopy(plan.applyMask, (float)plan.mix, false, nComps, plan.processChannels)) {
            layerPlan.sourceInput = -1;
        }
        EffectInstance::RenderActionArgs layerArgs = args;
        layerArgs.outputLayers.clear();
        layerArgs.outputLayers.push_back(output);
        const StatusEnum status = runStagesCpu(*this, layerPlan, layerArgs, StageRowFn(row));
        if (status != eStatusOK) {
            return status;
        }
    }

    return eStatusOK;
} // FrameBlend::renderOp

bool
FrameBlend::canRenderOpOnGpu(const GpuQuery& query) const
{
#ifdef NATRON_HAVE_GPU
    if ((query.bitDepth != eImageBitDepthFloat) || !query.plane.isColorLayer() || !canRunStagesGpu(resolveGpuPlan(query), query)) {
        return false;
    }
    EffectInstancePtr input = getInput(0);
    if (!input) {
        return false;
    }
    const Settings settings = settingsAt(query.time, query.view);
    if (settings.outputCount) {
        return false;
    }
    const RenderScale inputScale = input->supportsRenderScale() ? query.mappedScale : RenderScale::identity;
    for (double t : settings.samples.times) {
        bool found = (t == query.time);
        for (const GpuInputTimes& times : query.inputTimes) {
            if ((times.inputNb == 0) && (std::find(times.times.begin(), times.times.end(), t) != times.times.end())) {
                found = true;
            }
        }
        if (!found) {
            return false;
        }
        RectD rod;
        bool isProjectFormat = false;
        if ((input->getRegionOfDefinition_public(input->getRenderHash(), t, inputScale, query.view, &rod, &isProjectFormat) == eStatusFailed) || rod.isNull()) {
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
FrameBlend::getOpGpuInputLayer(int /*inputNb*/,
                               const ImageLayerDesc& plane,
                               double /*time*/,
                               ViewIdx /*view*/,
                               ImageLayerDesc* layer) const
{
    *layer = plane;

    return true;
}

StatusEnum
FrameBlend::renderOpGpu(const GpuRenderArgs& args)
{
    const Settings settings = settingsAt(args.query.time, args.query.view);
    if (settings.outputCount) {
        return eStatusFailed;
    }
    std::vector<const GpuImageView*> views;
    for (double t : settings.samples.times) {
        views.push_back(args.inputAt(0, t));
    }
    const std::vector<float> blends = foldBlends(settings.samples);
    GpuImageView folded;
    if (TimeBlend::recordFoldGpu(args, views, settings.operation, settings.samples.factor, blends, &folded) != eStatusOK) {
        return eStatusFailed;
    }

    return recordStagesGpuFinish(resolveGpuPlan(args.query), args, folded);
}

NATRON_NAMESPACE_EXIT
