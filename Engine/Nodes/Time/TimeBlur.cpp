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

#include "TimeBlur.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "Engine/ChoiceOption.h"
#include "Engine/KnobTypes.h"

NATRON_NAMESPACE_ENTER

std::vector<double>
timeBlurSampleTimes(double time,
                    int divisions,
                    double shutter,
                    Resampler::ShutterOffsetEnum offset,
                    double customOffset)
{
    shutter = std::max(0., shutter);
    double open = time;
    double close = time;
    Resampler::shutterRange(time, shutter, offset, customOffset, &open, &close);
    std::vector<double> times;
    if ((shutter == 0.) || (divisions <= 1)) {
        times.push_back(open);

        return times;
    }
    times.reserve((std::size_t)divisions);
    for (int i = 0; i < divisions; ++i) {
        times.push_back(open + i * (close - open) / divisions);
    }

    return times;
}

TimeBlur::TimeBlur(NodePtr node)
    : TimeSampledEffect(node)
{
}

TimeBlur::~TimeBlur()
{
}

const NodeSpec*
TimeBlur::nodeSpec() const
{
    return staticNodeSpec<kTimeBlurNodeSpec>();
}

const ImagePipelineSpec&
TimeBlur::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kTimeBlurPipeline, kTimeBlurNodeSpec>();
}

void
TimeBlur::initializeKnobs()
{
    const KnobPagePtr page = createControlsPage();

    KnobIntPtr divisionsKnob = createKnob<KnobInt>(tr("Divisions").toStdString());
    divisionsKnob->setName(kTimeBlurParamDivisions);
    divisionsKnob->setHintToolTip(tr("The number of times averaged over the shutter. The first is the shutter's opening and the "
                                     "shutter is divided by Divisions; its closing is not read. With 4, a Shutter of 1 and a "
                                     "Centered offset, the input at t - 0.5, t - 0.25, t and t + 0.25 is averaged.")
                                      .toStdString());
    divisionsKnob->setDefaultValue(10);
    divisionsKnob->setMinimum(1);
    divisionsKnob->setDisplayMinimum(1);
    divisionsKnob->setDisplayMaximum(10);
    page->addKnob(divisionsKnob);
    _divisions = divisionsKnob;

    KnobDoublePtr shutterKnob = createKnob<KnobDouble>(tr("Shutter").toStdString());
    shutterKnob->setName(kTimeBlurParamShutter);
    shutterKnob->setHintToolTip(tr("How long, in frames, the shutter stays open. 0 reads the input at the opening only.").toStdString());
    shutterKnob->setDefaultValue(0.5);
    shutterKnob->setMinimum(0.);
    shutterKnob->setDisplayMinimum(0.);
    shutterKnob->setDisplayMaximum(2.);
    shutterKnob->setIncrement(0.01);
    page->addKnob(shutterKnob);
    _shutter = shutterKnob;

    KnobChoicePtr offsetKnob = createKnob<KnobChoice>(tr("Shutter Offset").toStdString());
    offsetKnob->setName(kTimeBlurParamShutterOffset);
    offsetKnob->setHintToolTip(tr("When the shutter opens and closes around the current frame.").toStdString());
    {
        const std::vector<Resampler::ChoiceOption>& offsets = Resampler::shutterOffsetOptions();
        std::vector<ChoiceOption> options;
        for (const Resampler::ChoiceOption& option : offsets) {
            options.push_back(ChoiceOption(option.id, option.label, option.hint));
        }
        offsetKnob->populateChoices(options);
    }
    offsetKnob->setDefaultValue((int)Resampler::eShutterOffsetStart);
    page->addKnob(offsetKnob);
    _shutterOffset = offsetKnob;

    KnobDoublePtr customKnob = createKnob<KnobDouble>(tr("Shutter Custom Offset").toStdString());
    customKnob->setName(kTimeBlurParamShutterCustomOffset);
    customKnob->setHintToolTip(tr("With a Custom Shutter Offset, the shutter opens at the current frame plus this many frames.").toStdString());
    customKnob->setDefaultValue(0.);
    customKnob->setDisplayMinimum(-1.);
    customKnob->setDisplayMaximum(1.);
    customKnob->setIncrement(0.1);
    page->addKnob(customKnob);
    _shutterCustomOffset = customKnob;

    addStageKnobs(page);
} // TimeBlur::initializeKnobs

TimeBlur::Sample
TimeBlur::sample(double time,
                 ViewIdx view) const
{
    Sample result;
    KnobIntPtr divisionsKnob = _divisions.lock();
    KnobDoublePtr shutterKnob = _shutter.lock();
    KnobChoicePtr offsetKnob = _shutterOffset.lock();
    KnobDoublePtr customKnob = _shutterCustomOffset.lock();
    if (!getInput(0) || !divisionsKnob || !shutterKnob || !offsetKnob || !customKnob) {
        return result;
    }
    const int divisions = divisionsKnob->getValueAtTime(time, 0, view);
    const double shutter = shutterKnob->getValueAtTime(time, 0, view);
    const Resampler::ShutterOffsetEnum offset = (Resampler::ShutterOffsetEnum)std::clamp(offsetKnob->getValueAtTime(time, 0, view), (int)Resampler::eShutterOffsetCentered, (int)Resampler::eShutterOffsetCustom);
    const double custom = customKnob->getValueAtTime(time, 0, view);
    result.times = timeBlurSampleTimes(time, divisions, shutter, offset, custom);
    if (result.times.size() == 1) {
        result.kind = Sample::eInput;

        return result;
    }
    result.kind = Sample::eBlend;
    result.weights.assign(result.times.size(), 1.);

    return result;
}

NATRON_NAMESPACE_EXIT
