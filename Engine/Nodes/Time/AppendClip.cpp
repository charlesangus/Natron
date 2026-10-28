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

#include "AppendClip.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Time/TimeBlend.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"

NATRON_NAMESPACE_ENTER

namespace {
struct ClipPlace {
    int clip = -1;
    int min = -1;
    int outMin = 0;
    int outMax = 0;
};

// Lays the clips out up to time, or to the end with lastOnly, leaving the last two placed in
// *previous and *current, as OFX AppendClip does. Returns false when the answer is already in
// *early: before the first clip, or no clip at all. Each clip placed is appended to placed.
bool
placeClips(std::span<const std::optional<AppendClipRange>> clips,
           const AppendClipSettings& s,
           double time,
           bool lastOnly,
           ClipPlace* previous,
           ClipPlace* current,
           int* firstClip,
           int* lastClip,
           AppendClipSources* early,
           std::vector<ClipPlace>* placed = nullptr)
{
    *previous = ClipPlace { -1, -1, s.firstFrame, s.firstFrame - 1 };
    *current = ClipPlace { -1, -1, s.firstFrame, s.firstFrame - 1 };
    *early = AppendClipSources();
    early->t0 = -1.;
    early->t1 = -1.;
    *lastClip = (int)clips.size() - 1;
    while ((*lastClip >= 0) && !clips[*lastClip]) {
        --*lastClip;
    }
    *firstClip = 0;
    while ((*firstClip < (int)clips.size()) && !clips[*firstClip]) {
        ++*firstClip;
    }
    if (*lastClip < 0) {
        return false;
    }
    const int dissolve = std::max(0, s.crossDissolve);
    for (int i = *firstClip; i <= *lastClip; ++i) {
        if (!clips[i]) {
            continue;
        }
        const AppendClipRange& r = *clips[i];
        if (current->clip == -1) {
            if (!lastOnly && (time < s.firstFrame)) {
                early->clip0 = i;
                early->t0 = r.first + (time - s.firstFrame);
                early->alpha0 = (s.fadeIn <= 0) ? 1. : 0.;

                return false;
            }
            *current = ClipPlace { i, r.first, s.firstFrame, std::max(s.firstFrame + (r.last - r.first), s.firstFrame) };
            if (placed) {
                placed->push_back(*current);
            }
            continue;
        }
        // Never more than two clips at a time: the next one starts after the one before the
        // previous ends.
        ClipPlace next;
        next.clip = i;
        next.min = r.first;
        next.outMin = std::max(previous->outMax + 1, current->outMax + 1 - dissolve);
        next.outMax = std::max(current->outMax, next.outMin + (r.last - r.first));
        if (!lastOnly && (time < next.outMin)) {
            break;
        }
        *previous = *current;
        *current = next;
        if (placed) {
            placed->push_back(next);
        }
    }

    return true;
} // placeClips
} // anonymous namespace

AppendClipSources
appendClipSources(std::span<const std::optional<AppendClipRange>> clips,
                  const AppendClipSettings& settings,
                  double time)
{
    ClipPlace c0;
    ClipPlace c1;
    int firstClip = 0;
    int lastClip = -1;
    AppendClipSources result;
    if (!placeClips(clips, settings, time, false, &c0, &c1, &firstClip, &lastClip, &result)) {
        return result;
    }
    const int dissolve = std::max(0, settings.crossDissolve);
    const ClipPlace none { -1, -1, settings.firstFrame, settings.firstFrame - 1 };
    const auto in = [time](const ClipPlace& c) {
        return (c.clip != -1) && (c.outMin <= time) && (time <= c.outMax);
    };
    if (in(c0) && in(c1)) {
        result.clip0 = c0.clip;
        result.t0 = c0.min + (time - c0.outMin);
        result.alpha0 = 1. - (time + 1 - c1.outMin) / (double)(dissolve + 1);
        result.clip1 = c1.clip;
        result.t1 = c1.min + (time - c1.outMin);
        result.alpha1 = std::max(0., 1. - result.alpha0);
    } else if (in(c0)) {
        c1 = none;
        result.clip0 = c0.clip;
        result.t0 = c0.min + (time - c0.outMin);
        result.alpha0 = 1.;
    } else if (in(c1)) {
        c0 = c1;
        c1 = none;
        result.clip0 = c0.clip;
        result.t0 = c0.min + (time - c0.outMin);
        result.alpha0 = 1.;
        result.t1 = -1.;
    } else {
        // After the last clip, its last frame holds.
        c0 = c1;
        c1 = none;
        result.clip0 = c0.clip;
        result.t0 = c0.min + (time - c0.outMin);
        result.alpha0 = (settings.fadeOut <= 0) ? 1. : 0.;
        result.t1 = -1.;
    }
    // fadeIn = n ramps the first n frames, fadeOut = n the last n.
    if ((settings.fadeIn > 0) && (c0.clip == firstClip) && (c0.outMin <= time) && ((time - settings.firstFrame) < settings.fadeIn)) {
        const double fade = (time - settings.firstFrame + 1) / (settings.fadeIn + 1);
        result.alpha0 *= fade;
        result.alpha1 *= fade;
    }
    if (settings.fadeOut > 0) {
        for (const ClipPlace* c : { &c0, &c1 }) {
            if ((c->clip == lastClip) && (time <= c->outMax) && ((c->outMax - time) < settings.fadeOut)) {
                const double fade = (c->outMax - time + 1) / (settings.fadeOut + 1);
                result.alpha0 *= fade;
                result.alpha1 *= fade;
            }
        }
    }

    return result;
} // appendClipSources

int
appendClipLastFrame(std::span<const std::optional<AppendClipRange>> clips,
                    const AppendClipSettings& settings)
{
    ClipPlace c0;
    ClipPlace c1;
    int firstClip = 0;
    int lastClip = -1;
    AppendClipSources early;
    placeClips(clips, settings, 0., true, &c0, &c1, &firstClip, &lastClip, &early);

    return c1.outMax;
}

std::vector<AppendClipPlacement>
appendClipPlacements(std::span<const std::optional<AppendClipRange>> clips,
                     const AppendClipSettings& settings)
{
    ClipPlace c0;
    ClipPlace c1;
    int firstClip = 0;
    int lastClip = -1;
    AppendClipSources early;
    std::vector<ClipPlace> placed;
    placeClips(clips, settings, 0., true, &c0, &c1, &firstClip, &lastClip, &early, &placed);
    std::vector<AppendClipPlacement> result;
    result.reserve(placed.size());
    for (const ClipPlace& c : placed) {
        result.push_back(AppendClipPlacement { c.clip, c.min, c.outMin, c.outMax });
    }

    return result;
}

AppendClip::AppendClip(NodePtr node)
    : StagedImageEffect(node)
{
}

AppendClip::~AppendClip()
{
}

const NodeSpec*
AppendClip::nodeSpec() const
{
    return staticNodeSpec<kAppendClipNodeSpec>();
}

const ImagePipelineSpec&
AppendClip::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kAppendClipPipeline, kAppendClipNodeSpec>();
}

void
AppendClip::initializeKnobs()
{
    const KnobPagePtr page = createControlsPage();

    KnobIntPtr fadeInKnob = createKnob<KnobInt>(tr("Fade In").toStdString());
    fadeInKnob->setName(kAppendClipParamFadeIn);
    fadeInKnob->setHintToolTip(tr("Number of frames to fade in from black at the start of the first input.").toStdString());
    fadeInKnob->setDefaultValue(0);
    fadeInKnob->setMinimum(0);
    fadeInKnob->setDisplayMinimum(0);
    fadeInKnob->setDisplayMaximum(50);
    fadeInKnob->setAnimationEnabled(false);
    page->addKnob(fadeInKnob);
    _fadeIn = fadeInKnob;

    KnobIntPtr fadeOutKnob = createKnob<KnobInt>(tr("Fade Out").toStdString());
    fadeOutKnob->setName(kAppendClipParamFadeOut);
    fadeOutKnob->setHintToolTip(tr("Number of frames to fade out to black at the end of the last input.").toStdString());
    fadeOutKnob->setDefaultValue(0);
    fadeOutKnob->setMinimum(0);
    fadeOutKnob->setDisplayMinimum(0);
    fadeOutKnob->setDisplayMaximum(50);
    fadeOutKnob->setAnimationEnabled(false);
    page->addKnob(fadeOutKnob);
    _fadeOut = fadeOutKnob;

    KnobIntPtr crossDissolveKnob = createKnob<KnobInt>(tr("Cross Dissolve").toStdString());
    crossDissolveKnob->setName(kAppendClipParamCrossDissolve);
    crossDissolveKnob->setHintToolTip(tr("Number of frames over which each input cross-dissolves into the next.").toStdString());
    crossDissolveKnob->setDefaultValue(0);
    crossDissolveKnob->setMinimum(0);
    crossDissolveKnob->setDisplayMinimum(0);
    crossDissolveKnob->setDisplayMaximum(50);
    crossDissolveKnob->setAnimationEnabled(false);
    page->addKnob(crossDissolveKnob);
    _crossDissolve = crossDissolveKnob;

    KnobIntPtr firstFrameKnob = createKnob<KnobInt>(tr("First Frame").toStdString());
    firstFrameKnob->setName(kAppendClipParamFirstFrame);
    firstFrameKnob->setHintToolTip(tr("Output frame the first input starts at.").toStdString());
    firstFrameKnob->setDefaultValue(1);
    firstFrameKnob->setAnimationEnabled(false);
    page->addKnob(firstFrameKnob);
    _firstFrame = firstFrameKnob;

    KnobIntPtr lastFrameKnob = createKnob<KnobInt>(tr("Last Frame").toStdString());
    lastFrameKnob->setName(kAppendClipParamLastFrame);
    lastFrameKnob->setHintToolTip(tr("Last frame of the appended inputs (read-only).").toStdString());
    lastFrameKnob->setDefaultValue(0);
    lastFrameKnob->setAnimationEnabled(false);
    lastFrameKnob->setDefaultAllDimensionsEnabled(false);
    lastFrameKnob->setIsPersistent(false);
    lastFrameKnob->setEvaluateOnChange(false);
    lastFrameKnob->setAddNewLine(false);
    page->addKnob(lastFrameKnob);
    _lastFrame = lastFrameKnob;

    KnobButtonPtr updateKnob = createKnob<KnobButton>(tr("Update Last Frame").toStdString());
    updateKnob->setName(kAppendClipParamUpdateLastFrame);
    updateKnob->setHintToolTip(tr("Recompute Last Frame from the inputs' current frame ranges.").toStdString());
    updateKnob->setEvaluateOnChange(false);
    page->addKnob(updateKnob);
    _updateLastFrame = updateKnob;

    addStageKnobs(page);
} // AppendClip::initializeKnobs

bool
AppendClip::knobChanged(KnobI* k,
                        ValueChangedReasonEnum /*reason*/,
                        ViewSpec /*view*/,
                        double /*time*/,
                        bool /*originatedFromMainThread*/)
{
    KnobButtonPtr updateKnob = _updateLastFrame.lock();
    KnobIntPtr firstFrameKnob = _firstFrame.lock();
    KnobIntPtr crossDissolveKnob = _crossDissolve.lock();
    if (!k || ((k != updateKnob.get()) && (k != firstFrameKnob.get()) && (k != crossDissolveKnob.get()))) {
        return false;
    }
    updateLastFrame();

    return true;
}

void
AppendClip::onInputChanged(int /*inputNo*/)
{
    updateLastFrame();
}

void
AppendClip::updateLastFrame()
{
    KnobIntPtr lastFrameKnob = _lastFrame.lock();
    if (!lastFrameKnob) {
        return;
    }
    const std::vector<std::optional<AppendClipRange>> clips = clipRanges();
    lastFrameKnob->setValue(appendClipLastFrame(clips, settings()));
}

AppendClipSettings
AppendClip::settings() const
{
    AppendClipSettings s;
    if (KnobIntPtr knob = _firstFrame.lock()) {
        s.firstFrame = knob->getValue();
    }
    if (KnobIntPtr knob = _fadeIn.lock()) {
        s.fadeIn = std::max(0, knob->getValue());
    }
    if (KnobIntPtr knob = _fadeOut.lock()) {
        s.fadeOut = std::max(0, knob->getValue());
    }
    if (KnobIntPtr knob = _crossDissolve.lock()) {
        s.crossDissolve = std::max(0, knob->getValue());
    }

    return s;
}

std::vector<std::optional<AppendClipRange>>
AppendClip::clipRanges() const
{
    std::vector<std::optional<AppendClipRange>> clips(kAppendClipInputCount);
    std::optional<AppendClipRange> project;
    for (int i = 0; i < kAppendClipInputCount; ++i) {
        EffectInstancePtr input = getInput(i);
        if (!input) {
            continue;
        }
        double first = 0.;
        double last = 0.;
        input->getFrameRange_public(input->getRenderHash(), &first, &last);
        // An unbounded input, such as a generator, plays for the project's range.
        if ((first <= std::numeric_limits<int>::min()) || (last >= std::numeric_limits<int>::max()) || (first > last)) {
            if (!project) {
                double projectFirst = 1.;
                double projectLast = 1.;
                if (AppInstancePtr app = getApp()) {
                    app->getProject()->getFrameRange(&projectFirst, &projectLast);
                }
                project = AppendClipRange { (int)std::floor(projectFirst), (int)std::floor(std::max(projectFirst, projectLast)) };
            }
            clips[i] = project;
            continue;
        }
        clips[i] = AppendClipRange { (int)std::floor(first), (int)std::floor(last) };
    }

    return clips;
}

AppendClip::Blend
AppendClip::blendAt(double time) const
{
    const std::vector<std::optional<AppendClipRange>> clips = clipRanges();
    const AppendClipSources s = appendClipSources(clips, settings(), time);
    Blend blend;
    const std::pair<int, std::pair<double, double>> shown[] = {
        { s.clip0, { s.t0, s.alpha0 } },
        { s.clip1, { s.t1, s.alpha1 } },
    };
    double total = 0.;
    for (const auto& [clip, sample] : shown) {
        if ((clip < 0) || !(sample.second > 0.)) {
            continue;
        }
        blend.inputs.push_back(clip);
        blend.times.push_back(sample.first);
        blend.weights.push_back(sample.second);
        total += sample.second;
    }
    blend.black = std::max(0., 1. - total);

    return blend;
}

Route
AppendClip::route(double time,
                  ViewIdx /*view*/)
{
    const Blend blend = blendAt(time);
    if (blend.inputs.empty()) {
        return Route::black();
    }
    if ((blend.inputs.size() == 1) && (blend.weights[0] >= 1.)) {
        return Route::input(blend.inputs[0], blend.times[0]);
    }

    return Route::none();
}

FramesNeededMap
AppendClip::getFramesNeeded(double time,
                            ViewIdx view)
{
    if (route(time, view).kind != Route::Kind::None) {
        return StagedImageEffect::getFramesNeeded(time, view);
    }
    const Blend blend = blendAt(time);
    FramesNeededMap frames;
    for (std::size_t i = 0; i < blend.inputs.size(); ++i) {
        RangeD range;
        range.min = range.max = blend.times[i];
        frames[blend.inputs[i]][view].push_back(range);
    }

    return frames;
}

StatusEnum
AppendClip::getRegionOfDefinition(U64 /*hash*/,
                                  double time,
                                  const RenderScale& scale,
                                  ViewIdx view,
                                  RectD* rod)
{
    *rod = RectD();
    const Blend blend = blendAt(time);
    for (std::size_t i = 0; i < blend.inputs.size(); ++i) {
        EffectInstancePtr input = getInput(blend.inputs[i]);
        if (!input) {
            continue;
        }
        const RenderScale inputScale = input->supportsRenderScale() ? scale : RenderScale::identity;
        RectD inputRoD;
        bool isProjectFormat = false;
        // An input reports its empty region as a failure; it is still the region it has.
        if ((input->getRegionOfDefinition_public(input->getRenderHash(), blend.times[i], inputScale, view, &inputRoD, &isProjectFormat) == eStatusFailed) && !inputRoD.isNull()) {
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

void
AppendClip::getFrameRange(double* first,
                          double* last)
{
    const std::vector<std::optional<AppendClipRange>> clips = clipRanges();
    if (std::none_of(clips.begin(), clips.end(), [](const std::optional<AppendClipRange>& c) { return c.has_value(); })) {
        EffectInstance::getFrameRange(first, last);

        return;
    }
    const AppendClipSettings s = settings();
    *first = s.firstFrame;
    *last = appendClipLastFrame(clips, s);
}

std::vector<AppendClipPlacement>
AppendClip::placements() const
{
    return appendClipPlacements(clipRanges(), settings());
}

bool
AppendClip::isOpIdentity(double /*time*/,
                         const RenderScale& /*scale*/,
                         const RectI& /*roi*/,
                         ViewIdx /*view*/)
{
    return false;
}

StatusEnum
AppendClip::renderOp(const RenderActionArgs& args,
                     const StagePlan& /*plan*/)
{
    const Blend blend = blendAt(args.time);
    if ((blend.inputs.size() == 1) && (blend.weights[0] >= 1.)) {
        // route() answers a clip shown alone, so the host never renders it.
        return eStatusFailed;
    }
    std::vector<double> weights = blend.weights;
    if (blend.black > 0.) {
        weights.push_back(blend.black);
    }
    for (const std::pair<ImageLayerDesc, ImagePtr>& output : args.outputLayers) {
        // All are fetched before any image is locked: fetching renders upstream. Black is a null
        // image, as is a clip with nothing there.
        std::vector<ImagePtr> images(weights.size());
        for (std::size_t i = 0; i < blend.inputs.size(); ++i) {
            if (!declaresInputTime(blend.inputs[i], args.time, args.view, blend.times[i])) {
                return eStatusFailed;
            }
            images[i] = fetchInputAt(blend.inputs[i], blend.times[i], args, &output.first);
        }
        if (!TimeBlend::renderCpu(output.second, args.roi, images, weights)) {
            return eStatusFailed;
        }
    }

    return eStatusOK;
}

bool
AppendClip::canRenderOpOnGpu(const GpuQuery& query) const
{
#ifdef NATRON_HAVE_GPU
    if ((query.bitDepth != eImageBitDepthFloat) || query.hasMask || !query.plane.isColorLayer()) {
        return false;
    }
    const Blend blend = blendAt(query.time);
    if (blend.inputs.empty()) {
        return false;
    }
    for (std::size_t i = 0; i < blend.inputs.size(); ++i) {
        const int inputNb = blend.inputs[i];
        const double t = blend.times[i];
        EffectInstancePtr input = getInput(inputNb);
        if (!input) {
            return false;
        }
        bool found = false;
        for (const GpuInputTimes& times : query.inputTimes) {
            if ((times.inputNb == inputNb) && (std::find(times.times.begin(), times.times.end(), t) != times.times.end())) {
                found = true;
            }
        }
        if (!found && (t != query.time)) {
            return false;
        }
        // A clip with nothing there reads as black on the CPU; a region has no view of it.
        const RenderScale inputScale = input->supportsRenderScale() ? query.mappedScale : RenderScale::identity;
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
AppendClip::getOpGpuInputLayer(int /*inputNb*/,
                               const ImageLayerDesc& plane,
                               double /*time*/,
                               ViewIdx /*view*/,
                               ImageLayerDesc* layer) const
{
    *layer = plane;

    return true;
}

bool
AppendClip::readsGpuInput(int inputNb,
                          const ImageLayerDesc& /*plane*/,
                          double time,
                          ViewIdx /*view*/) const
{
    const Blend blend = blendAt(time);

    return std::find(blend.inputs.begin(), blend.inputs.end(), inputNb) != blend.inputs.end();
}

StatusEnum
AppendClip::renderOpGpu(const GpuRenderArgs& args)
{
    const Blend blend = blendAt(args.query.time);
    if (blend.inputs.empty() || ((blend.inputs.size() == 1) && (blend.weights[0] >= 1.))) {
        return eStatusFailed;
    }
    std::vector<const GpuImageView*> views;
    for (std::size_t i = 0; i < blend.inputs.size(); ++i) {
        const GpuImageView* view = args.inputAt(blend.inputs[i], blend.times[i]);
        if (!view) {
            return eStatusFailed;
        }
        views.push_back(view);
    }
    std::vector<double> weights = blend.weights;
    if (blend.black > 0.) {
        views.push_back(nullptr);
        weights.push_back(blend.black);
    }

    return TimeBlend::recordGpu(args, views, weights);
}

NATRON_NAMESPACE_EXIT
