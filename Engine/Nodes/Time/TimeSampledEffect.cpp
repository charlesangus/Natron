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

#include "TimeSampledEffect.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "Engine/ChoiceOption.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Time/TimeBlend.h"
#include "Engine/RectD.h"

NATRON_NAMESPACE_ENTER

namespace {
// Integrating a speed or evaluating a curve leaves rounding error on times that are meant to be
// whole frames; without this a whole frame would blend with its neighbour at a weight of nearly
// zero.
constexpr double kWholeFrameTolerance = 1e-6;

// The widest input span Box averages, so that a high speed cannot ask for an unbounded number of
// frames per output frame.
constexpr double kMaxBoxSpan = 255.;

bool
isWholeFrame(double time,
             double* frame)
{
    const double rounded = std::floor(time + 0.5);
    if (std::abs(time - rounded) > kWholeFrameTolerance) {
        return false;
    }
    *frame = rounded;

    return true;
}
} // anonymous namespace

namespace TimeSample {
Sample
filter(Filter filter,
       double s,
       double low,
       double high,
       double keepLow,
       double keepHigh)
{
    Sample result;
    result.kind = Sample::eInput;
    double frame = 0.;
    if ((filter != eFilterBox) && isWholeFrame(s, &frame)) {
        result.times.push_back(frame);

        return result;
    }
    switch (filter) {
    case eFilterNearest:
        result.times.push_back(std::floor(s + 0.5));
        break;
    case eFilterFractional:
        result.times.push_back(s);
        break;
    case eFilterBlend: {
        const double from = std::floor(s);
        result.kind = Sample::eBlend;
        result.times = { from, from + 1. };
        result.weights = { 1. - (s - from), s - from };
        break;
    }
    case eFilterBox: {
        if ((s >= keepLow) && (s <= keepHigh)) {
            low = std::max(low, keepLow - 0.5);
            high = std::min(high, keepHigh + 0.5);
        }
        low = std::max(low, s - 0.5 * kMaxBoxSpan);
        high = std::min(high, s + 0.5 * kMaxBoxSpan);
        // Frame f covers [f - 0.5, f + 0.5).
        for (double f = std::floor(low + 0.5); f <= std::floor(high + 0.5); f += 1.) {
            const double weight = std::min(high, f + 0.5) - std::max(low, f - 0.5);
            if (weight > kWholeFrameTolerance) {
                result.times.push_back(f);
                result.weights.push_back(weight);
            }
        }
        if (result.times.size() > 1) {
            result.kind = Sample::eBlend;
        } else {
            if (result.times.empty()) {
                result.times.push_back(isWholeFrame(s, &frame) ? frame : std::floor(s + 0.5));
            }
            result.weights.clear();
        }
        break;
    }
    }

    return result;
} // filter
} // namespace TimeSample

TimeSampledEffect::TimeSampledEffect(NodePtr node)
    : StagedImageEffect(node)
{
}

TimeSampledEffect::~TimeSampledEffect()
{
}

void
TimeSampledEffect::createFilterKnobs(const KnobPagePtr& page)
{
    KnobChoicePtr filterKnob = createKnob<KnobChoice>(tr("Filter").toStdString());
    filterKnob->setName(kTimeSampleParamFilter);
    filterKnob->setHintToolTip(tr("How an output frame that falls between two input frames is made.").toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kTimeSampleFilterNearest, tr("Nearest").toStdString(), tr("The nearest input frame.").toStdString()));
        options.push_back(ChoiceOption(kTimeSampleFilterBlend, tr("Blend").toStdString(), tr("The two nearest input frames blended linearly.").toStdString()));
        options.push_back(ChoiceOption(kTimeSampleFilterFractional, tr("Fractional").toStdString(),
                                       tr("The input at the fractional time, for an input that can render between frames.").toStdString()));
        options.push_back(ChoiceOption(kTimeSampleFilterBox, tr("Box").toStdString(),
                                       tr("The input frames the shutter covers, averaged, each weighted by how much of it the shutter covers.").toStdString()));
        filterKnob->populateChoices(options);
    }
    filterKnob->setDefaultValue((int)TimeSample::eFilterBlend);
    filterKnob->setAnimationEnabled(false);
    page->addKnob(filterKnob);
    _filter = filterKnob;

    KnobDoublePtr shutterKnob = createKnob<KnobDouble>(tr("Shutter").toStdString());
    shutterKnob->setName(kTimeSampleParamShutter);
    shutterKnob->setHintToolTip(tr("With the Box filter, the output frames each output frame averages over, centred on it: the "
                                   "input frames that span covers are blended. Lower values blend fewer frames; 0 reads the "
                                   "nearest frame.")
                                    .toStdString());
    shutterKnob->setDefaultValue(1.);
    shutterKnob->setMinimum(0.);
    shutterKnob->setDisplayMinimum(0.);
    shutterKnob->setDisplayMaximum(2.);
    page->addKnob(shutterKnob);
    _shutter = shutterKnob;
}

TimeSample::Filter
TimeSampledEffect::filterValue() const
{
    KnobChoicePtr filterKnob = _filter.lock();

    return filterKnob ? (TimeSample::Filter)std::clamp(filterKnob->getValue(), (int)TimeSample::eFilterNearest, (int)TimeSample::eFilterBox) : TimeSample::eFilterBlend;
}

double
TimeSampledEffect::shutterAt(double time,
                             ViewIdx view) const
{
    KnobDoublePtr shutterKnob = _shutter.lock();

    return shutterKnob ? std::max(0., shutterKnob->getValueAtTime(time, 0, view)) : 0.;
}

Route
TimeSampledEffect::route(double time,
                         ViewIdx view)
{
    const Sample s = sample(time, view);
    switch (s.kind) {
    case Sample::eBlack:
        return Route::black();
    case Sample::eInput:
        return Route::input(0, s.times[0]);
    case Sample::eBlend:
        break;
    }

    return Route::none();
}

FramesNeededMap
TimeSampledEffect::getFramesNeeded(double time,
                                   ViewIdx view)
{
    const Sample s = sample(time, view);
    if (s.kind != Sample::eBlend) {
        return StagedImageEffect::getFramesNeeded(time, view);
    }
    FramesNeededMap frames;
    for (double t : s.times) {
        RangeD range;
        range.min = range.max = t;
        frames[0][view].push_back(range);
    }

    return frames;
}

StatusEnum
TimeSampledEffect::getRegionOfDefinition(U64 /*hash*/,
                                         double time,
                                         const RenderScale& scale,
                                         ViewIdx view,
                                         RectD* rod)
{
    *rod = RectD();
    const Sample s = sample(time, view);
    EffectInstancePtr input = getInput(0);
    if ((s.kind == Sample::eBlack) || !input) {
        return eStatusOK;
    }
    const RenderScale inputScale = input->supportsRenderScale() ? scale : RenderScale::identity;
    for (double t : s.times) {
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
TimeSampledEffect::isOpIdentity(double /*time*/,
                                const RenderScale& /*scale*/,
                                const RectI& /*roi*/,
                                ViewIdx /*view*/)
{
    return false;
}

StatusEnum
TimeSampledEffect::renderOp(const RenderActionArgs& args,
                            const StagePlan& /*plan*/)
{
    const Sample s = sample(args.time, args.view);
    if (s.kind != Sample::eBlend) {
        // route() answers every other sample, so the host never renders it.
        return eStatusFailed;
    }
    for (const std::pair<ImageLayerDesc, ImagePtr>& output : args.outputLayers) {
        // All are fetched before any image is locked: fetching renders upstream. A null image
        // at a declared time is an input with nothing there, which reads as black.
        std::vector<ImagePtr> images(s.times.size());
        for (std::size_t i = 0; i < s.times.size(); ++i) {
            if (!declaresInputTime(0, args.time, args.view, s.times[i])) {
                return eStatusFailed;
            }
            images[i] = fetchInputAt(0, s.times[i], args, &output.first);
        }
        if (!TimeBlend::renderCpu(output.second, args.roi, images, s.weights)) {
            return eStatusFailed;
        }
    }

    return eStatusOK;
}

bool
TimeSampledEffect::canRenderOpOnGpu(const GpuQuery& query) const
{
#ifdef NATRON_HAVE_GPU
    // Only the main layer is read at other times on the GPU for now.
    if ((query.bitDepth != eImageBitDepthFloat) || query.hasMask || !query.plane.isColorLayer()) {
        return false;
    }
    const Sample s = sample(query.time, query.view);
    EffectInstancePtr input = getInput(0);
    if ((s.kind != Sample::eBlend) || !input) {
        return false;
    }
    const RenderScale inputScale = input->supportsRenderScale() ? query.mappedScale : RenderScale::identity;
    for (double t : s.times) {
        bool found = false;
        for (const GpuInputTimes& times : query.inputTimes) {
            if ((times.inputNb == 0) && (std::find(times.times.begin(), times.times.end(), t) != times.times.end())) {
                found = true;
            }
        }
        if (!found && (t != query.time)) {
            return false;
        }
        // A frame with nothing there reads as black on the CPU; a region has no view of it, so
        // that blend renders on the CPU.
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
TimeSampledEffect::getOpGpuInputLayer(int /*inputNb*/,
                                      const ImageLayerDesc& plane,
                                      double /*time*/,
                                      ViewIdx /*view*/,
                                      ImageLayerDesc* layer) const
{
    *layer = plane;

    return true;
}

StatusEnum
TimeSampledEffect::renderOpGpu(const GpuRenderArgs& args)
{
    const Sample s = sample(args.query.time, args.query.view);
    if (s.kind != Sample::eBlend) {
        return eStatusFailed;
    }
    std::vector<const GpuImageView*> views;
    for (double t : s.times) {
        views.push_back(args.inputAt(0, t));
    }

    return TimeBlend::recordGpu(args, views, s.weights);
}

NATRON_NAMESPACE_EXIT
