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

#include "FrameHold.h"

#include <cmath>
#include <string>

#include <QString>

#include <ofxNatron.h>

#include "Engine/AppManager.h"
#include "Engine/KnobTypes.h"

NATRON_NAMESPACE_ENTER

double
frameHoldSourceTime(double time,
                    double firstFrame,
                    int increment,
                    bool fractional)
{
    double sourceTime = firstFrame;
    if (increment > 0) {
        sourceTime = firstFrame + increment * std::floor((time - firstFrame) / increment);
    }

    return TimeRounding::apply(sourceTime, fractional);
}

void
FrameHoldOp::createKnobs(KnobHolder* holder,
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
    subLabelKnob->setDefaultValue(std::string("frame 1"));
    page->addKnob(subLabelKnob);
    subLabel = subLabelKnob;

    roundingMode = TimeRounding::createKnob(holder, page, kFrameHoldParamRoundingMode);

    KnobDoublePtr firstKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("First Frame").toStdString());
    firstKnob->setName(kFrameHoldParamFirstFrame);
    firstKnob->setHintToolTip(EffectInstance::tr("The frame to hold, or with an increment, the first of the frames held.").toStdString());
    firstKnob->setDefaultValue(1.);
    firstKnob->setDisplayMinimum(1.);
    firstKnob->setDisplayMaximum(100.);
    firstKnob->setAddNewLine(false);
    page->addKnob(firstKnob);
    firstFrame = firstKnob;

    KnobButtonPtr setKnob = AppManager::createKnob<KnobButton>(holder, EffectInstance::tr("Set To Current Frame").toStdString());
    setKnob->setName(kFrameHoldParamSetToCurrentFrame);
    setKnob->setHintToolTip(EffectInstance::tr("Set First Frame to the timeline's current frame.").toStdString());
    setKnob->setEvaluateOnChange(false);
    page->addKnob(setKnob);
    setToCurrentFrame = setKnob;

    KnobIntPtr incrementKnob = AppManager::createKnob<KnobInt>(holder, EffectInstance::tr("Increment").toStdString());
    incrementKnob->setName(kFrameHoldParamIncrement);
    incrementKnob->setHintToolTip(EffectInstance::tr("With 0, First Frame is held at every frame. Otherwise every increment-th frame from First Frame is held until the next one: with 10, frames 1, 11, 21 and so on.").toStdString());
    incrementKnob->setDefaultValue(0);
    incrementKnob->setMinimum(0);
    incrementKnob->setDisplayMinimum(0);
    incrementKnob->setDisplayMaximum(10);
    page->addKnob(incrementKnob);
    increment = incrementKnob;
}

FrameHoldOp::Params
FrameHoldOp::bake(const KnobReader& reader) const
{
    Params params;
    params.fractional = (reader.value(roundingMode) == (int)TimeRounding::eFractional);
    params.firstFrame = reader.value(firstFrame);
    params.increment = reader.value(increment);

    return params;
}

Route
FrameHoldOp::route(const Params& params,
                   const RouteContext& context) const
{
    if (!context.isConnected(0)) {
        return Route::black();
    }

    return Route::input(0, frameHoldSourceTime(context.time(), params.firstFrame, params.increment, params.fractional));
}

FrameHold::FrameHold(NodePtr node)
    : ImageNode<FrameHoldOp>(node)
{
}

FrameHold::~FrameHold()
{
}

bool
FrameHold::knobChanged(KnobI* k,
                       ValueChangedReasonEnum /*reason*/,
                       ViewSpec /*view*/,
                       double time,
                       bool /*originatedFromMainThread*/)
{
    KnobButtonPtr setButton = op().setToCurrentFrame.lock();
    KnobDoublePtr first = op().firstFrame.lock();
    KnobIntPtr inc = op().increment.lock();

    if (setButton && first && (k == setButton.get())) {
        first->setValue(time);

        return true;
    }
    if ((first && (k == first.get())) || (inc && (k == inc.get()))) {
        refreshSubLabel(time);

        return true;
    }

    return false;
}

void
FrameHold::onKnobsLoaded()
{
    refreshSubLabel(getCurrentTime());
}

void
FrameHold::refreshSubLabel(double time)
{
    KnobStringPtr label = op().subLabel.lock();
    KnobDoublePtr first = op().firstFrame.lock();
    KnobIntPtr inc = op().increment.lock();

    if (!label || !first || !inc) {
        return;
    }
    const QString frame = QString::number(first->getValueAtTime(time), 'g', 10);
    const int step = inc->getValueAtTime(time);
    if (step > 0) {
        label->setValue(QString::fromUtf8("frame %1+n*%2").arg(frame).arg(step).toStdString());
    } else {
        label->setValue(QString::fromUtf8("frame %1").arg(frame).toStdString());
    }
}

NATRON_NAMESPACE_EXIT
