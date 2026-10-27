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

#include "TimeOffset.h"

#include <algorithm>
#include <limits>

#include "Engine/AppManager.h"
#include "Engine/KnobTypes.h"

NATRON_NAMESPACE_ENTER

void
TimeOffsetOp::createKnobs(KnobHolder* holder,
                          const KnobPagePtr& page)
{
    roundingMode = TimeRounding::createKnob(holder, page, kTimeOffsetParamRoundingMode);

    KnobDoublePtr offsetKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Time Offset").toStdString());
    offsetKnob->setName(kTimeOffsetParamTimeOffset);
    offsetKnob->setHintToolTip(EffectInstance::tr("Offset in frames: frame t shows the input at t minus the offset. A positive offset delays the input, a negative one advances it.").toStdString());
    offsetKnob->setDefaultValue(0.);
    offsetKnob->setDisplayMinimum(-100.);
    offsetKnob->setDisplayMaximum(100.);
    // The frame range moves by the offset, which an animated offset would leave undefined.
    offsetKnob->setAnimationEnabled(false);
    page->addKnob(offsetKnob);
    timeOffset = offsetKnob;

    KnobBoolPtr reverseKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr("Reverse Input").toStdString());
    reverseKnob->setName(kTimeOffsetParamReverseInput);
    reverseKnob->setHintToolTip(EffectInstance::tr("Play the input backwards over its frame range: its last frame comes first.").toStdString());
    reverseKnob->setDefaultValue(false);
    reverseKnob->setAnimationEnabled(false);
    page->addKnob(reverseKnob);
    reverseInput = reverseKnob;

    KnobBoolPtr clipKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr("Clip To Input Range").toStdString());
    clipKnob->setName(kTimeOffsetParamClipToInputRange);
    clipKnob->setHintToolTip(EffectInstance::tr("Hold the first and last frames of the input's frame range outside of it, instead of reading the input beyond its range.").toStdString());
    clipKnob->setDefaultValue(false);
    clipKnob->setAnimationEnabled(false);
    page->addKnob(clipKnob);
    clipToInputRange = clipKnob;
}

TimeOffsetOp::Params
TimeOffsetOp::bake(const KnobReader& reader) const
{
    Params params;
    params.fractional = (reader.value(roundingMode) == (int)TimeRounding::eFractional);
    params.offset = reader.value(timeOffset);
    params.reverse = reader.value(reverseInput);
    params.clip = reader.value(clipToInputRange);

    return params;
}

Route
TimeOffsetOp::route(const Params& params,
                    const RouteContext& context) const
{
    if (!context.isConnected(0)) {
        return Route::black();
    }
    double sourceTime = context.time() - params.offset;
    double first = 0.;
    double last = 0.;
    // Reversing and clipping need a bounded range; over an unbounded one they do nothing.
    if ((params.reverse || params.clip) && context.inputFrameRange(0, &first, &last)) {
        if (params.reverse) {
            sourceTime = last - sourceTime + first;
        }
        if (params.clip) {
            sourceTime = std::clamp(sourceTime, first, last);
        }
    }

    return Route::input(0, TimeRounding::apply(sourceTime, params.fractional));
}

TimeOffset::TimeOffset(NodePtr node)
    : ImageNode<TimeOffsetOp>(node)
{
}

TimeOffset::~TimeOffset()
{
}

void
TimeOffset::getFrameRange(double* first,
                          double* last)
{
    EffectInstance::getFrameRange(first, last);
    KnobDoublePtr offset = op().timeOffset.lock();
    if (!offset || !getInput(0)) {
        return;
    }
    const double shift = offset->getValue();
    if (*first > std::numeric_limits<int>::min()) {
        *first += shift;
    }
    if (*last < std::numeric_limits<int>::max()) {
        *last += shift;
    }
}

NATRON_NAMESPACE_EXIT
