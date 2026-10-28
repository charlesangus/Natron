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
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/IO/ReadTimeDomain.h"

NATRON_NAMESPACE_ENTER

namespace {
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
    options.push_back(ChoiceOption(kRetimeOutsideContinue, EffectInstance::tr("Continue").toStdString(), EffectInstance::tr("The retime carried on past the end of the new range at the speed there.").toStdString()));
    options.push_back(ChoiceOption(kRetimeOutsideHold, EffectInstance::tr("Hold").toStdString(), EffectInstance::tr("The nearest end of the new range.").toStdString()));
    options.push_back(ChoiceOption(kRetimeOutsideLoop, EffectInstance::tr("Loop").toStdString(), EffectInstance::tr("The new range repeated.").toStdString()));
    options.push_back(ChoiceOption(kRetimeOutsideBounce, EffectInstance::tr("Bounce").toStdString(), EffectInstance::tr("The new range played forwards and backwards in turn.").toStdString()));
    options.push_back(ChoiceOption(kRetimeOutsideBlack, EffectInstance::tr("Black").toStdString(), EffectInstance::tr("An empty image.").toStdString()));
    knob->populateChoices(options);
    knob->setDefaultValue((int)Retime::eOutsideHold);
    knob->setAnimationEnabled(false);
    page->addKnob(knob);

    return knob;
}

KnobDoublePtr
createFrameKnob(KnobHolder* holder,
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
    knob->setAddNewLine(false);
    page->addKnob(knob);

    return knob;
}

KnobBoolPtr
createUseKnob(KnobHolder* holder,
              const KnobPagePtr& page,
              const char* name,
              const QString& label,
              const QString& hint,
              bool defaultValue)
{
    KnobBoolPtr knob = AppManager::createKnob<KnobBool>(holder, label.toStdString());
    knob->setName(name);
    knob->setHintToolTip(hint.toStdString());
    knob->setDefaultValue(defaultValue);
    knob->setAnimationEnabled(false);
    page->addKnob(knob);

    return knob;
}

ReadTimeDomain::BeforeAfter
toBeforeAfter(Retime::Outside outside)
{
    return (outside == Retime::eOutsideBounce) ? ReadTimeDomain::eBeforeAfterBounce : ReadTimeDomain::eBeforeAfterLoop;
}

// The integral of knob from from to to, negative when to is before from.
double
integrate(const KnobDoublePtr& knob,
          double from,
          double to,
          ViewIdx view)
{
    if (to < from) {
        return -knob->getIntegrateFromTimeToTime(to, from, view, 0);
    }

    return knob->getIntegrateFromTimeToTime(from, to, view, 0);
}
} // anonymous namespace

Retime::Retime(NodePtr node)
    : TimeSampledEffect(node)
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

    _originalIn = createFrameKnob(this, page, kRetimeParamOriginalIn, tr("Original In"),
                                  tr("The input frame that lands on New In. With Use In Mapping off, the input's first frame."), 1.);
    _newIn = createFrameKnob(this, page, kRetimeParamNewIn, tr("New In"),
                             tr("The output frame Original In lands on, where the new range starts. With Use In Mapping off, "
                                "what the speed (or Original In, with neither mapping on) makes it."),
                             1.);
    _useInMapping = createUseKnob(this, page, kRetimeParamUseInMapping, tr("Use In Mapping"),
                                  tr("Land Original In on New In. With Use Out Mapping also on, the speed is the one that fits "
                                     "the original range into the new range."),
                                  true);
    _originalOut = createFrameKnob(this, page, kRetimeParamOriginalOut, tr("Original Out"),
                                   tr("The input frame that lands on New Out. With Use Out Mapping off, the input's last frame."), 100.);
    _originalOut.lock()->setDefaultAllDimensionsEnabled(false);
    _newOut = createFrameKnob(this, page, kRetimeParamNewOut, tr("New Out"),
                              tr("The output frame Original Out lands on, where the new range ends. With Use Out Mapping off, "
                                 "what the speed makes it."),
                              100.);
    _newOut.lock()->setDefaultAllDimensionsEnabled(false);
    _useOutMapping = createUseKnob(this, page, kRetimeParamUseOutMapping, tr("Use Out Mapping"),
                                   tr("Land Original Out on New Out. With Use In Mapping also on, the speed is the one that fits "
                                      "the original range into the new range."),
                                   false);

    KnobDoublePtr speedKnob = createKnob<KnobDouble>(tr("Speed").toStdString());
    speedKnob->setName(kRetimeParamSpeed);
    speedKnob->setHintToolTip(tr("Input frames per output frame. It is integrated from the mapped end, so an animated speed "
                                 "accelerates (above 1), decelerates (below 1) or reverses (below 0) the input. The new range's "
                                 "length uses the speed at the mapped end. With both mappings on it is the one that fits the "
                                 "ranges, and read-only.")
                                  .toStdString());
    speedKnob->setDefaultValue(1.);
    speedKnob->setDisplayMinimum(0.1);
    speedKnob->setDisplayMaximum(10.);
    page->addKnob(speedKnob);
    _speed = speedKnob;

    KnobBoolPtr reverseKnob = createKnob<KnobBool>(tr("Reverse").toStdString());
    reverseKnob->setName(kRetimeParamReverse);
    reverseKnob->setHintToolTip(tr("Play the original range backwards, from Original Out to Original In.").toStdString());
    reverseKnob->setDefaultValue(false);
    page->addKnob(reverseKnob);
    _reverse = reverseKnob;

    _before = createOutsideKnob(this, page, kRetimeParamBefore, tr("Before"), tr("What the frames before the new range show."));
    _after = createOutsideKnob(this, page, kRetimeParamAfter, tr("After"), tr("What the frames after the new range show."));

    createFilterKnobs(page);

    addStageKnobs(page);
} // Retime::initializeKnobs

Retime::Ranges
Retime::ranges(ViewIdx view) const
{
    Ranges r;
    KnobDoublePtr originalInKnob = _originalIn.lock();
    KnobDoublePtr newInKnob = _newIn.lock();
    KnobBoolPtr useInKnob = _useInMapping.lock();
    KnobDoublePtr originalOutKnob = _originalOut.lock();
    KnobDoublePtr newOutKnob = _newOut.lock();
    KnobBoolPtr useOutKnob = _useOutMapping.lock();
    KnobDoublePtr speedKnob = _speed.lock();
    if (!originalInKnob || !newInKnob || !useInKnob || !originalOutKnob || !newOutKnob || !useOutKnob || !speedKnob) {
        return r;
    }

    const bool useIn = useInKnob->getValue();
    const bool useOut = useOutKnob->getValue();
    r.originalIn = originalInKnob->getValue();
    r.originalOut = originalOutKnob->getValue();
    double first = 0.;
    double last = 0.;
    if ((!useIn || !useOut) && boundedRange(getInput(0), &first, &last)) {
        if (!useIn) {
            r.originalIn = first;
        }
        if (!useOut) {
            r.originalOut = last;
        }
    }

    const double length = r.originalOut - r.originalIn;
    if (useIn && useOut) {
        r.newIn = newInKnob->getValue();
        r.newOut = newOutKnob->getValue();
        r.fixedSpeed = true;
        r.speed = (r.newOut != r.newIn) ? length / (r.newOut - r.newIn) : 0.;
    } else if (useOut) {
        r.anchorOut = true;
        r.newOut = newOutKnob->getValue();
        r.speed = speedKnob->getValueAtTime(r.newOut, 0, view);
        r.newIn = (r.speed != 0.) ? r.newOut - length / std::abs(r.speed) : r.newOut;
    } else {
        r.newIn = useIn ? newInKnob->getValue() : r.originalIn;
        r.speed = speedKnob->getValueAtTime(r.newIn, 0, view);
        r.newOut = (r.speed != 0.) ? r.newIn + length / std::abs(r.speed) : r.newIn;
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
    const double low = std::min(r.newIn, r.newOut);
    const double high = std::max(r.newIn, r.newOut);
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
    if (!speedKnob) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double anchor = r.anchorOut ? r.newOut : r.newIn;
    const double advance = r.fixedSpeed ? r.speed * (mapped - anchor) : integrate(speedKnob, anchor, mapped, view);
    // The original end that lands on the anchor: reversed, the other one does.
    const bool fromOut = (r.anchorOut != reverse);
    const double start = fromOut ? r.originalOut : r.originalIn;

    return reverse ? (start - advance) : (start + advance);
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
    KnobBoolPtr reverseKnob = _reverse.lock();
    if (!reverseKnob || !getInput(0)) {
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

    const TimeSample::Filter filter = filterValue();
    // The shutter's ends, mapped like its centre but without wrapping, so that a loop's seam or a
    // held end does not stretch the span; the centre counts too, for a speed that turns.
    double low = s;
    double high = s;
    const double half = (moving && (filter == TimeSample::eFilterBox)) ? 0.5 * shutterAt(time, view) : 0.;
    if (half > 0.) {
        for (double end : { mapped - half, mapped + half }) {
            const double e = inputTime(end, r, reverse, view);
            if (std::isfinite(e)) {
                low = std::min(low, e);
                high = std::max(high, e);
            }
        }
    }

    // Within the original range the span stays inside it; Continue reads past it on purpose.
    return TimeSample::filter(filter, s, low, high, std::min(r.originalIn, r.originalOut), std::max(r.originalIn, r.originalOut));
} // Retime::sample

void
Retime::getFrameRange(double* first,
                      double* last)
{
    const Ranges r = ranges(ViewIdx(0));
    *first = std::floor(std::min(r.newIn, r.newOut));
    *last = std::ceil(std::max(r.newIn, r.newOut));
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
        _originalIn.lock(),
        _newIn.lock(),
        _useInMapping.lock(),
        _originalOut.lock(),
        _newOut.lock(),
        _useOutMapping.lock(),
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
    KnobBoolPtr useInKnob = _useInMapping.lock();
    KnobBoolPtr useOutKnob = _useOutMapping.lock();
    KnobDoublePtr speedKnob = _speed.lock();
    if (_refreshingKnobs || !useInKnob || !useOutKnob || !speedKnob) {
        return;
    }
    _refreshingKnobs = true;
    const Ranges r = ranges(ViewIdx(0));
    const bool useIn = useInKnob->getValue();
    const bool useOut = useOutKnob->getValue();
    const std::pair<KnobDoubleWPtr, bool> frames[] = {
        { _originalIn, useIn },
        { _newIn, useIn },
        { _originalOut, useOut },
        { _newOut, useOut },
    };
    const double values[] = { r.originalIn, r.newIn, r.originalOut, r.newOut };
    for (int i = 0; i < 4; ++i) {
        KnobDoublePtr knob = frames[i].first.lock();
        if (!knob) {
            continue;
        }
        const bool used = frames[i].second;
        knob->setAllDimensionsEnabled(used);
        if (!used && (knob->getValue() != values[i])) {
            knob->setValue(values[i]);
        }
    }
    const bool fixed = useIn && useOut;
    speedKnob->setAllDimensionsEnabled(!fixed);
    // An animated or expression-driven speed keeps its curve; only a constant shows the fit.
    if (fixed && !speedKnob->isAnimated(0) && speedKnob->getExpression(0).empty() && (speedKnob->getValue() != r.speed)) {
        speedKnob->setValue(r.speed);
    }
    _refreshingKnobs = false;
}

NATRON_NAMESPACE_EXIT
