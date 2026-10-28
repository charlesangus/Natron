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

#include "TimeWarp.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/Curve.h"
#include "Engine/KnobTypes.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_ENTER

TimeWarp::TimeWarp(NodePtr node)
    : TimeSampledEffect(node)
{
}

TimeWarp::~TimeWarp()
{
}

const NodeSpec*
TimeWarp::nodeSpec() const
{
    return staticNodeSpec<kTimeWarpNodeSpec>();
}

const ImagePipelineSpec&
TimeWarp::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kTimeWarpPipeline, kTimeWarpNodeSpec>();
}

void
TimeWarp::initializeKnobs()
{
    const KnobPagePtr page = createControlsPage();

    KnobDoublePtr lookupKnob = createKnob<KnobDouble>(tr("Lookup").toStdString());
    lookupKnob->setName(kTimeWarpParamLookup);
    lookupKnob->setHintToolTip(tr("The input frame shown at the current frame. Animate it to retime the input: a slope of 2 plays "
                                  "it at double speed, a flat stretch holds a frame and a falling one plays it backwards. A new "
                                  "TimeWarp is keyed as the identity at the project's first and last frames; past its first and "
                                  "last keys a cubic key carries the curve on along its slope and a linear or constant key holds.")
                                   .toStdString());
    lookupKnob->setDefaultValue(1.);
    lookupKnob->setDisplayMinimum(1.);
    lookupKnob->setDisplayMaximum(100.);
    page->addKnob(lookupKnob);
    _lookup = lookupKnob;

    createFilterKnobs(page);

    addStageKnobs(page);
}

void
TimeWarp::onEffectCreated(bool /*mayCreateFileDialog*/,
                          const CreateNodeArgs& args)
{
    KnobDoublePtr lookupKnob = _lookup.lock();
    if (!lookupKnob || args.getProperty<Serialization::NodeSerializationPtr>(kCreateNodeArgsPropNodeSerialization) || lookupKnob->isAnimated(0)) {
        return;
    }
    double first = 1.;
    double last = 100.;
    getApp()->getProject()->getFrameRange(&first, &last);
    last = std::max(last, first + 1.);
    for (double frame : { first, last }) {
        lookupKnob->setValueAtTime(frame, frame, ViewSpec::all(), 0);
        // Cubic end keys carry the curve on along their slope; linear ones would hold the end frames.
        KeyFrame key;
        bool set = lookupKnob->setInterpolationAtTime(eCurveChangeReasonInternal, ViewSpec::all(), 0, frame, eKeyframeTypeCubic, &key);
        Q_UNUSED(set);
    }
}

TimeWarp::Sample
TimeWarp::sample(double time,
                 ViewIdx view) const
{
    Sample result;
    KnobDoublePtr lookupKnob = _lookup.lock();
    EffectInstancePtr input = getInput(0);
    if (!lookupKnob || !input) {
        return result;
    }
    const double s = lookupKnob->getValueAtTime(time, 0, view);
    if (!std::isfinite(s)) {
        return result;
    }

    const TimeSample::Filter filter = filterValue();
    double low = s;
    double high = s;
    const double half = (filter == TimeSample::eFilterBox) ? 0.5 * shutterAt(time, view) : 0.;
    if (half > 0.) {
        for (double end : { time - half, time + half }) {
            const double e = lookupKnob->getValueAtTime(end, 0, view);
            if (std::isfinite(e)) {
                low = std::min(low, e);
                high = std::max(high, e);
            }
        }
    }
    double keepLow = -std::numeric_limits<double>::infinity();
    double keepHigh = std::numeric_limits<double>::infinity();
    double first = 0.;
    double last = 0.;
    input->getFrameRange_public(input->getRenderHash(), &first, &last);
    if ((first > std::numeric_limits<int>::min()) && (last < std::numeric_limits<int>::max()) && (first <= last)) {
        keepLow = first;
        keepHigh = last;
    }

    return TimeSample::filter(filter, s, low, high, keepLow, keepHigh);
}

NATRON_NAMESPACE_EXIT
