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

#include "Retime.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/IO/ReadTimeDomain.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Time/TimeBlend.h"
#include "Engine/RectD.h"

NATRON_NAMESPACE_ENTER

namespace {
// Integrating the speed and evaluating the warp curve leave rounding error on times that are
// meant to be whole frames; without this a whole frame would blend with its neighbour at a
// weight of nearly zero.
constexpr double kWholeFrameTolerance = 1e-6;

// The widest input span Box averages, so that a high speed cannot ask for an unbounded number of
// frames per output frame.
constexpr double kMaxBoxSpan = 255.;

bool
boundedRange(const EffectInstancePtr& input,
             double* first,
             double* last)
{
    if (!input) {
        return false;
    }
    input->getFrameRange_public(input->getRenderHash(), first, last);

    return (*first > std::numeric_limits<int>::min()) && (*last < std::numeric_limits<int>::max()) && (*first <= *last);
}

KnobChoicePtr
createOutsideKnob(KnobHolder* holder,
                  const KnobPagePtr& page,
                  const char* name,
                  const QString& label,
                  const QString& hint)
{
    KnobChoicePtr knob = AppManager::createKnob<KnobChoice>(holder, label.toStdString());
    knob->setName(name);
    knob->setHintToolTip(hint.toStdString());
    std::vector<ChoiceOption> options;
    options.push_back(ChoiceOption(kRetimeOutsideContinue, EffectInstance::tr("Continue").toStdString(), EffectInstance::tr("The retime carried on past the end of the output range at the speed there.").toStdString()));
    options.push_back(ChoiceOption(kRetimeOutsideHold, EffectInstance::tr("Hold").toStdString(), EffectInstance::tr("The nearest end of the output range.").toStdString()));
    options.push_back(ChoiceOption(kRetimeOutsideLoop, EffectInstance::tr("Loop").toStdString(), EffectInstance::tr("The output range repeated.").toStdString()));
    options.push_back(ChoiceOption(kRetimeOutsideBounce, EffectInstance::tr("Bounce").toStdString(), EffectInstance::tr("The output range played forwards and backwards in turn.").toStdString()));
    options.push_back(ChoiceOption(kRetimeOutsideBlack, EffectInstance::tr("Black").toStdString(), EffectInstance::tr("An empty image.").toStdString()));
    knob->populateChoices(options);
    knob->setDefaultValue((int)Retime::eOutsideHold);
    knob->setAnimationEnabled(false);
    page->addKnob(knob);

    return knob;
}

KnobDoublePtr
createEndKnob(KnobHolder* holder,
              const KnobPagePtr& page,
              const char* name,
              const QString& label,
              const QString& hint,
              double defaultValue)
{
    KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(holder, label.toStdString());
    knob->setName(name);
    knob->setHintToolTip(hint.toStdString());
    knob->setDefaultValue(defaultValue);
    knob->setDisplayMinimum(1.);
    knob->setDisplayMaximum(100.);
    knob->setAnimationEnabled(false);
    knob->setDefaultAllDimensionsEnabled(false);
    knob->setAddNewLine(false);
    page->addKnob(knob);

    return knob;
}

KnobBoolPtr
createLockKnob(KnobHolder* holder,
               const KnobPagePtr& page,
               const char* name,
               const QString& label,
               const QString& hint)
{
    KnobBoolPtr knob = AppManager::createKnob<KnobBool>(holder, label.toStdString());
    knob->setName(name);
    knob->setHintToolTip(hint.toStdString());
    knob->setDefaultValue(false);
    knob->setAnimationEnabled(false);
    page->addKnob(knob);

    return knob;
}

ReadTimeDomain::BeforeAfter
toBeforeAfter(Retime::Outside outside)
{
    return (outside == Retime::eOutsideBounce) ? ReadTimeDomain::eBeforeAfterBounce : ReadTimeDomain::eBeforeAfterLoop;
}

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

Retime::Retime(NodePtr node)
    : StagedImageEffect(node)
{
}

Retime::~Retime()
{
}

const NodeSpec*
Retime::nodeSpec() const
{
    return staticNodeSpec<kRetimeNodeSpec>();
}

const ImagePipelineSpec&
Retime::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kRetimePipeline, kRetimeNodeSpec>();
}

void
Retime::initializeKnobs()
{
    const KnobPagePtr page = createControlsPage();

    _inputFirst = createEndKnob(this, page, kRetimeParamInputFirst, tr("Input First"),
                                tr("The first input frame the retime uses; the input's first frame unless Input First Lock is on."), 1.);
    _inputFirstLock = createLockKnob(this, page, kRetimeParamInputFirstLock, tr("Input First Lock"),
                                     tr("Use Input First rather than the input's first frame."));
    _inputLast = createEndKnob(this, page, kRetimeParamInputLast, tr("Input Last"),
                               tr("The last input frame the retime uses; the input's last frame unless Input Last Lock is on."), 100.);
    _inputLastLock = createLockKnob(this, page, kRetimeParamInputLastLock, tr("Input Last Lock"),
                                    tr("Use Input Last rather than the input's last frame."));

    KnobBoolPtr reverseKnob = createKnob<KnobBool>(tr("Reverse").toStdString());
    reverseKnob->setName(kRetimeParamReverse);
    reverseKnob->setHintToolTip(tr("Play the input range backwards, from its last frame to its first.").toStdString());
    reverseKnob->setDefaultValue(false);
    page->addKnob(reverseKnob);
    _reverse = reverseKnob;

    _outputFirst = createEndKnob(this, page, kRetimeParamOutputFirst, tr("Output First"),
                                 tr("The first output frame of the retime. Unless Output First Lock is on, the input's first frame, "
                                    "or, when only Output Last Lock is on, what the speed makes it."),
                                 1.);
    _outputFirstLock = createLockKnob(this, page, kRetimeParamOutputFirstLock, tr("Output First Lock"),
                                      tr("Start the output at Output First. With Output Last Lock also on, the speed is the one "
                                         "that fits the input range into the output range."));
    _outputLast = createEndKnob(this, page, kRetimeParamOutputLast, tr("Output Last"),
                                tr("The last output frame of the retime; unless Output Last Lock is on, what the speed makes it."), 100.);
    _outputLastLock = createLockKnob(this, page, kRetimeParamOutputLastLock, tr("Output Last Lock"),
                                     tr("End the output at Output Last. With Output First Lock also on, the speed is the one "
                                        "that fits the input range into the output range."));

    KnobDoublePtr speedKnob = createKnob<KnobDouble>(tr("Speed").toStdString());
    speedKnob->setName(kRetimeParamSpeed);
    speedKnob->setHintToolTip(tr("Input frames per output frame. It is integrated from the first output frame, so an animated "
                                 "speed accelerates (above 1), decelerates (below 1) or reverses (below 0) the input. The output "
                                 "range's length uses the speed at its locked end, or at its first frame. Ignored when both "
                                 "output ends are locked.")
                                  .toStdString());
    speedKnob->setDefaultValue(1.);
    speedKnob->setDisplayMinimum(0.1);
    speedKnob->setDisplayMaximum(10.);
    page->addKnob(speedKnob);
    _speed = speedKnob;

    _before = createOutsideKnob(this, page, kRetimeParamBefore, tr("Before"), tr("What the frames before the output range show."));
    _after = createOutsideKnob(this, page, kRetimeParamAfter, tr("After"), tr("What the frames after the output range show."));

    KnobChoicePtr filterKnob = createKnob<KnobChoice>(tr("Filter").toStdString());
    filterKnob->setName(kRetimeParamFilter);
    filterKnob->setHintToolTip(tr("How an output frame that falls between two input frames is made.").toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kRetimeFilterNearest, tr("Nearest").toStdString(), tr("The nearest input frame.").toStdString()));
        options.push_back(ChoiceOption(kRetimeFilterBlend, tr("Blend").toStdString(), tr("The two nearest input frames blended linearly.").toStdString()));
        options.push_back(ChoiceOption(kRetimeFilterFractional, tr("Fractional").toStdString(),
                                       tr("The input at the fractional time, for an input that can render between frames.").toStdString()));
        options.push_back(ChoiceOption(kRetimeFilterBox, tr("Box").toStdString(),
                                       tr("The input frames the shutter covers, averaged, each weighted by how much of it the shutter covers.").toStdString()));
        filterKnob->populateChoices(options);
    }
    filterKnob->setDefaultValue((int)eFilterBlend);
    filterKnob->setAnimationEnabled(false);
    page->addKnob(filterKnob);
    _filter = filterKnob;

    KnobDoublePtr shutterKnob = createKnob<KnobDouble>(tr("Shutter").toStdString());
    shutterKnob->setName(kRetimeParamShutter);
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

    addStageKnobs(page);

    KnobPagePtr warpPage = createKnob<KnobPage>(tr("Time Warp").toStdString());
    KnobParametricPtr warpKnob = createKnob<KnobParametric>(tr("Warp").toStdString(), 1);
    warpKnob->setName(kRetimeParamWarp);
    warpKnob->setHintToolTip(tr("Maps the input range, after the speed, onto itself across the output range: a shallow slope "
                                "slows the input down and a negative slope plays it backwards.")
                                 .toStdString());
    warpKnob->setDimensionName(0, "warp");
    warpKnob->setCurveColor(0, 0.5, 0.5, 1.);
    warpKnob->setParametricRange(0., 1.);
    warpKnob->setDisplayMinimumsAndMaximums(std::vector<double>(1, 0.), std::vector<double>(1, 1.));
    for (double x : { 0., 1. }) {
        StatusEnum stat = warpKnob->addControlPoint(eValueChangedReasonPluginEdited, 0, x, x, eKeyframeTypeCubic);
        Q_UNUSED(stat);
    }
    warpKnob->setDefaultCurvesFromCurves();
    warpPage->addKnob(warpKnob);
    _warp = warpKnob;
} // Retime::initializeKnobs

Retime::Ranges
Retime::ranges(ViewIdx view) const
{
    Ranges r;
    KnobDoublePtr inputFirstKnob = _inputFirst.lock();
    KnobDoublePtr inputLastKnob = _inputLast.lock();
    KnobBoolPtr inputFirstLockKnob = _inputFirstLock.lock();
    KnobBoolPtr inputLastLockKnob = _inputLastLock.lock();
    KnobDoublePtr outputFirstKnob = _outputFirst.lock();
    KnobDoublePtr outputLastKnob = _outputLast.lock();
    KnobBoolPtr outputFirstLockKnob = _outputFirstLock.lock();
    KnobBoolPtr outputLastLockKnob = _outputLastLock.lock();
    KnobDoublePtr speedKnob = _speed.lock();
    if (!inputFirstKnob || !inputLastKnob || !inputFirstLockKnob || !inputLastLockKnob || !outputFirstKnob || !outputLastKnob || !outputFirstLockKnob || !outputLastLockKnob || !speedKnob) {
        return r;
    }

    const bool inputFirstLocked = inputFirstLockKnob->getValue();
    const bool inputLastLocked = inputLastLockKnob->getValue();
    r.inputFirst = inputFirstKnob->getValue();
    r.inputLast = inputLastKnob->getValue();
    double first = 0.;
    double last = 0.;
    if ((!inputFirstLocked || !inputLastLocked) && boundedRange(getInput(0), &first, &last)) {
        if (!inputFirstLocked) {
            r.inputFirst = first;
        }
        if (!inputLastLocked) {
            r.inputLast = last;
        }
    }

    const double length = r.inputLast - r.inputFirst;
    const bool outputFirstLocked = outputFirstLockKnob->getValue();
    const bool outputLastLocked = outputLastLockKnob->getValue();
    if (outputFirstLocked && outputLastLocked) {
        r.outputFirst = outputFirstKnob->getValue();
        r.outputLast = outputLastKnob->getValue();
        r.fixedSpeed = true;
        r.speed = (r.outputLast != r.outputFirst) ? length / (r.outputLast - r.outputFirst) : 0.;
    } else if (outputLastLocked) {
        r.outputLast = outputLastKnob->getValue();
        r.speed = speedKnob->getValueAtTime(r.outputLast, 0, view);
        r.outputFirst = (r.speed != 0.) ? r.outputLast - length / std::abs(r.speed) : r.outputLast;
    } else {
        r.outputFirst = outputFirstLocked ? outputFirstKnob->getValue() : r.inputFirst;
        r.speed = speedKnob->getValueAtTime(r.outputFirst, 0, view);
        r.outputLast = (r.speed != 0.) ? r.outputFirst + length / std::abs(r.speed) : r.outputFirst;
    }

    return r;
} // Retime::ranges

bool
Retime::outputTime(double time,
                   const Ranges& r,
                   double* mapped,
                   bool* moving) const
{
    KnobChoicePtr beforeKnob = _before.lock();
    KnobChoicePtr afterKnob = _after.lock();
    if (!beforeKnob || !afterKnob) {
        return false;
    }
    const double low = std::min(r.outputFirst, r.outputLast);
    const double high = std::max(r.outputFirst, r.outputLast);
    *mapped = time;
    *moving = true;
    if ((time >= low) && (time <= high)) {
        return true;
    }
    const Outside outside = (Outside)std::clamp((time < low) ? beforeKnob->getValue() : afterKnob->getValue(), (int)eOutsideContinue, (int)eOutsideBlack);
    switch (outside) {
    case eOutsideContinue:
        return true;
    case eOutsideHold:
        *mapped = std::clamp(time, low, high);
        *moving = false;

        return true;
    case eOutsideLoop:
    case eOutsideBounce: {
        const ReadTimeDomain::BeforeAfter mode = toBeforeAfter(outside);
        const ReadTimeDomain::RangeTime wrapped = ReadTimeDomain::mapIntoRange(time, (int)std::floor(low), (int)std::ceil(high), mode, mode);
        if (wrapped.kind != ReadTimeDomain::RangeTime::eWithin) {
            return false;
        }
        *mapped = wrapped.time;

        return true;
    }
    case eOutsideBlack:
        break;
    }

    return false;
}

double
Retime::inputTime(double mapped,
                  const Ranges& r,
                  bool reverse,
                  ViewIdx view) const
{
    KnobDoublePtr speedKnob = _speed.lock();
    KnobParametricPtr warpKnob = _warp.lock();
    if (!speedKnob || !warpKnob) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double integrated = r.fixedSpeed ? r.speed * (mapped - r.outputFirst) : speedKnob->getIntegrateFromTimeToTime(r.outputFirst, mapped, view, 0);
    double s = reverse ? (r.inputLast - integrated) : (r.inputFirst + integrated);
    const double length = r.inputLast - r.inputFirst;
    if (length != 0.) {
        double warped = 0.;
        if (warpKnob->getValue(0, (s - r.inputFirst) / length, &warped) == eStatusOK) {
            s = r.inputFirst + length * warped;
        }
    }

    return s;
}

bool
Retime::sourceTime(double time,
                   ViewIdx view,
                   double* source) const
{
    KnobBoolPtr reverseKnob = _reverse.lock();
    if (!reverseKnob || !getInput(0)) {
        return false;
    }
    const Ranges r = ranges(view);
    double mapped = 0.;
    bool moving = true;
    if (!outputTime(time, r, &mapped, &moving)) {
        return false;
    }
    const double s = inputTime(mapped, r, reverseKnob->getValueAtTime(time, 0, view), view);
    if (!std::isfinite(s)) {
        return false;
    }
    *source = s;

    return true;
} // Retime::sourceTime

Retime::Sample
Retime::sample(double time,
               ViewIdx view) const
{
    Sample result;
    KnobChoicePtr filterKnob = _filter.lock();
    KnobBoolPtr reverseKnob = _reverse.lock();
    KnobDoublePtr shutterKnob = _shutter.lock();
    if (!filterKnob || !reverseKnob || !shutterKnob || !getInput(0)) {
        return result;
    }
    const Ranges r = ranges(view);
    double mapped = 0.;
    bool moving = true;
    if (!outputTime(time, r, &mapped, &moving)) {
        return result;
    }
    const bool reverse = reverseKnob->getValueAtTime(time, 0, view);
    const double s = inputTime(mapped, r, reverse, view);
    if (!std::isfinite(s)) {
        return result;
    }

    result.kind = Sample::eInput;
    const Filter filter = (Filter)std::clamp(filterKnob->getValue(), (int)eFilterNearest, (int)eFilterBox);
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
        // The shutter's ends, mapped like its centre but without wrapping, so that a loop's seam
        // or a held end does not stretch the span; the centre counts too, for a warp that turns.
        double low = s;
        double high = s;
        const double half = moving ? 0.5 * std::max(0., shutterKnob->getValueAtTime(time, 0, view)) : 0.;
        if (half > 0.) {
            for (double end : { mapped - half, mapped + half }) {
                const double e = inputTime(end, r, reverse, view);
                if (std::isfinite(e)) {
                    low = std::min(low, e);
                    high = std::max(high, e);
                }
            }
        }
        // Within the input range the span stays inside it; Continue reads past it on purpose.
        const double inputLow = std::min(r.inputFirst, r.inputLast);
        const double inputHigh = std::max(r.inputFirst, r.inputLast);
        if ((s >= inputLow) && (s <= inputHigh)) {
            low = std::max(low, inputLow - 0.5);
            high = std::min(high, inputHigh + 0.5);
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
} // Retime::sample

Route
Retime::route(double time,
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
Retime::getFramesNeeded(double time,
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
Retime::getRegionOfDefinition(U64 /*hash*/,
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

void
Retime::getFrameRange(double* first,
                      double* last)
{
    const Ranges r = ranges(ViewIdx(0));
    *first = std::floor(std::min(r.outputFirst, r.outputLast));
    *last = std::ceil(std::max(r.outputFirst, r.outputLast));
}

bool
Retime::knobChanged(KnobI* k,
                    ValueChangedReasonEnum /*reason*/,
                    ViewSpec /*view*/,
                    double /*time*/,
                    bool /*originatedFromMainThread*/)
{
    if (_refreshingKnobs) {
        return false;
    }
    const KnobIPtr rangeKnobs[] = {
        _inputFirst.lock(),
        _inputFirstLock.lock(),
        _inputLast.lock(),
        _inputLastLock.lock(),
        _outputFirst.lock(),
        _outputFirstLock.lock(),
        _outputLast.lock(),
        _outputLastLock.lock(),
        _speed.lock(),
    };
    for (const KnobIPtr& knob : rangeKnobs) {
        if (knob && (k == knob.get())) {
            refreshDerivedKnobs();

            return true;
        }
    }

    return false;
}

void
Retime::onKnobsLoaded()
{
    refreshDerivedKnobs();
}

void
Retime::onInputChanged(int /*inputNb*/)
{
    refreshDerivedKnobs();
}

void
Retime::refreshDerivedKnobs()
{
    if (_refreshingKnobs) {
        return;
    }
    _refreshingKnobs = true;
    const Ranges r = ranges(ViewIdx(0));
    const std::pair<KnobDoubleWPtr, KnobBoolWPtr> ends[] = {
        { _inputFirst, _inputFirstLock },
        { _inputLast, _inputLastLock },
        { _outputFirst, _outputFirstLock },
        { _outputLast, _outputLastLock },
    };
    const double values[] = { r.inputFirst, r.inputLast, r.outputFirst, r.outputLast };
    for (int i = 0; i < 4; ++i) {
        KnobDoublePtr valueKnob = ends[i].first.lock();
        KnobBoolPtr lockKnob = ends[i].second.lock();
        if (!valueKnob || !lockKnob) {
            continue;
        }
        const bool locked = lockKnob->getValue();
        valueKnob->setAllDimensionsEnabled(locked);
        if (!locked && (valueKnob->getValue() != values[i])) {
            valueKnob->setValue(values[i]);
        }
    }
    _refreshingKnobs = false;
}

bool
Retime::isOpIdentity(double /*time*/,
                     const RenderScale& /*scale*/,
                     const RectI& /*roi*/,
                     ViewIdx /*view*/)
{
    return false;
}

StatusEnum
Retime::renderOp(const RenderActionArgs& args,
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
Retime::canRenderOpOnGpu(const GpuQuery& query) const
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
Retime::getOpGpuInputLayer(int /*inputNb*/,
                           const ImageLayerDesc& plane,
                           double /*time*/,
                           ViewIdx /*view*/,
                           ImageLayerDesc* layer) const
{
    *layer = plane;

    return true;
}

StatusEnum
Retime::renderOpGpu(const GpuRenderArgs& args)
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
