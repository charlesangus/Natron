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

#include "FrameRange.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include <QString>

#include <ofxNatron.h>

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/IO/ReadTimeDomain.h"

NATRON_NAMESPACE_ENTER

namespace {
KnobChoicePtr
createModeKnob(KnobHolder* holder,
               const KnobPagePtr& page,
               const char* name,
               const QString& label,
               const QString& hint)
{
    KnobChoicePtr knob = AppManager::createKnob<KnobChoice>(holder, label.toStdString());
    knob->setName(name);
    knob->setHintToolTip(hint.toStdString());
    std::vector<ChoiceOption> options;
    options.push_back(ChoiceOption(kFrameRangeOriginal, EffectInstance::tr("Original").toStdString(), EffectInstance::tr("The input at the same frame.").toStdString()));
    options.push_back(ChoiceOption(kFrameRangeHold, EffectInstance::tr("Hold").toStdString(), EffectInstance::tr("The nearest end of the range.").toStdString()));
    options.push_back(ChoiceOption(kFrameRangeLoop, EffectInstance::tr("Loop").toStdString(), EffectInstance::tr("The range repeated.").toStdString()));
    options.push_back(ChoiceOption(kFrameRangeBounce, EffectInstance::tr("Bounce").toStdString(), EffectInstance::tr("The range played forwards and backwards in turn.").toStdString()));
    options.push_back(ChoiceOption(kFrameRangeBlack, EffectInstance::tr("Black").toStdString(), EffectInstance::tr("An empty image.").toStdString()));
    knob->populateChoices(options);
    knob->setDefaultValue((int)FrameRangeOp::eBlack);
    knob->setAnimationEnabled(false);
    page->addKnob(knob);

    return knob;
}

FrameRangeOp::Mode
toMode(int value)
{
    return (FrameRangeOp::Mode)std::clamp(value, (int)FrameRangeOp::eOriginal, (int)FrameRangeOp::eBlack);
}

ReadTimeDomain::BeforeAfter
toBeforeAfter(FrameRangeOp::Mode mode)
{
    switch (mode) {
    case FrameRangeOp::eLoop:
        return ReadTimeDomain::eBeforeAfterLoop;
    case FrameRangeOp::eBounce:
        return ReadTimeDomain::eBeforeAfterBounce;
    case FrameRangeOp::eBlack:
        return ReadTimeDomain::eBeforeAfterBlack;
    case FrameRangeOp::eOriginal:
    case FrameRangeOp::eHold:
        break;
    }

    return ReadTimeDomain::eBeforeAfterHold;
}
} // anonymous namespace

void
FrameRangeOp::createKnobs(KnobHolder* holder,
                          const KnobPagePtr& page)
{
    // Node.cpp shows this knob's value in parentheses next to the node's label.
    KnobStringPtr subLabelKnob = AppManager::createKnob<KnobString>(holder, std::string(kNatronOfxParamStringSublabelName));
    subLabelKnob->setName(kNatronOfxParamStringSublabelName);
    subLabelKnob->setSecretByDefault(true);
    subLabelKnob->setDefaultAllDimensionsEnabled(false);
    subLabelKnob->setIsPersistent(false);
    subLabelKnob->setEvaluateOnChange(false);
    subLabelKnob->setAnimationEnabled(false);
    subLabelKnob->setDefaultValue(std::string("1 - 100"));
    page->addKnob(subLabelKnob);
    subLabel = subLabelKnob;

    KnobIntPtr firstKnob = AppManager::createKnob<KnobInt>(holder, EffectInstance::tr("First Frame").toStdString());
    firstKnob->setName(kFrameRangeParamFirstFrame);
    firstKnob->setHintToolTip(EffectInstance::tr("The first frame of the range.").toStdString());
    firstKnob->setDefaultValue(1);
    firstKnob->setDisplayMinimum(1);
    firstKnob->setDisplayMaximum(100);
    firstKnob->setAnimationEnabled(false);
    firstKnob->setAddNewLine(false);
    page->addKnob(firstKnob);
    firstFrame = firstKnob;

    KnobIntPtr lastKnob = AppManager::createKnob<KnobInt>(holder, EffectInstance::tr("Last Frame").toStdString());
    lastKnob->setName(kFrameRangeParamLastFrame);
    lastKnob->setHintToolTip(EffectInstance::tr("The last frame of the range. A last frame before the first counts as the first.").toStdString());
    lastKnob->setDefaultValue(100);
    lastKnob->setDisplayMinimum(1);
    lastKnob->setDisplayMaximum(100);
    lastKnob->setAnimationEnabled(false);
    lastKnob->setAddNewLine(false);
    page->addKnob(lastKnob);
    lastFrame = lastKnob;

    KnobButtonPtr resetKnob = AppManager::createKnob<KnobButton>(holder, EffectInstance::tr("Reset").toStdString());
    resetKnob->setName(kFrameRangeParamReset);
    resetKnob->setHintToolTip(EffectInstance::tr("Set the range to the input's frame range.").toStdString());
    resetKnob->setEvaluateOnChange(false);
    page->addKnob(resetKnob);
    reset = resetKnob;

    before = createModeKnob(holder, page, kFrameRangeParamBefore, EffectInstance::tr("Before"), EffectInstance::tr("What the frames before the first frame show."));
    after = createModeKnob(holder, page, kFrameRangeParamAfter, EffectInstance::tr("After"), EffectInstance::tr("What the frames after the last frame show."));
}

FrameRangeOp::Params
FrameRangeOp::bake(const KnobReader& reader) const
{
    Params params;
    params.first = reader.value(firstFrame);
    params.last = std::max(params.first, (int)reader.value(lastFrame));
    params.before = toMode(reader.value(before));
    params.after = toMode(reader.value(after));

    return params;
}

Route
FrameRangeOp::route(const Params& params,
                    const RouteContext& context) const
{
    if (!context.isConnected(0)) {
        return Route::black();
    }
    const double time = context.time();
    const bool isBefore = (time < params.first);
    const bool isAfter = (time > params.last);
    const Mode mode = isBefore ? params.before : params.after;
    if ((!isBefore && !isAfter) || (mode == eOriginal)) {
        return Route::input(0, time);
    }
    const ReadTimeDomain::BeforeAfter outside = toBeforeAfter(mode);
    const ReadTimeDomain::RangeTime mapped = ReadTimeDomain::mapIntoRange(time, params.first, params.last, outside, outside);
    if (mapped.kind != ReadTimeDomain::RangeTime::eWithin) {
        return Route::black();
    }

    return Route::input(0, mapped.time);
}

FrameRange::FrameRange(NodePtr node)
    : ImageNode<FrameRangeOp>(node)
{
}

FrameRange::~FrameRange()
{
}

void
FrameRange::getFrameRange(double* first,
                          double* last)
{
    KnobIntPtr firstKnob = op().firstFrame.lock();
    KnobIntPtr lastKnob = op().lastFrame.lock();

    if (!firstKnob || !lastKnob) {
        EffectInstance::getFrameRange(first, last);

        return;
    }
    *first = firstKnob->getValue();
    *last = std::max(*first, (double)lastKnob->getValue());
}

bool
FrameRange::knobChanged(KnobI* k,
                        ValueChangedReasonEnum /*reason*/,
                        ViewSpec /*view*/,
                        double /*time*/,
                        bool /*originatedFromMainThread*/)
{
    KnobButtonPtr resetButton = op().reset.lock();
    KnobIntPtr firstKnob = op().firstFrame.lock();
    KnobIntPtr lastKnob = op().lastFrame.lock();

    if (resetButton && (k == resetButton.get())) {
        resetToInputRange();

        return true;
    }
    if ((firstKnob && (k == firstKnob.get())) || (lastKnob && (k == lastKnob.get()))) {
        refreshSubLabel();

        return true;
    }

    return false;
}

void
FrameRange::onKnobsLoaded()
{
    refreshSubLabel();
}

void
FrameRange::resetToInputRange()
{
    KnobIntPtr firstKnob = op().firstFrame.lock();
    KnobIntPtr lastKnob = op().lastFrame.lock();
    EffectInstancePtr input = getInput(0);

    if (!firstKnob || !lastKnob || !input) {
        return;
    }
    double first = 0.;
    double last = 0.;
    input->getFrameRange_public(input->getRenderHash(), &first, &last);
    if ((first <= std::numeric_limits<int>::min()) || (last >= std::numeric_limits<int>::max()) || (first > last)) {
        return;
    }
    firstKnob->setValue((int)std::floor(first));
    lastKnob->setValue((int)std::ceil(last));
}

void
FrameRange::refreshSubLabel()
{
    KnobStringPtr label = op().subLabel.lock();
    KnobIntPtr firstKnob = op().firstFrame.lock();
    KnobIntPtr lastKnob = op().lastFrame.lock();

    if (!label || !firstKnob || !lastKnob) {
        return;
    }
    const int first = firstKnob->getValue();
    const int last = std::max(first, lastKnob->getValue());
    label->setValue(QString::fromUtf8("%1 - %2").arg(first).arg(last).toStdString());
}

NATRON_NAMESPACE_EXIT
