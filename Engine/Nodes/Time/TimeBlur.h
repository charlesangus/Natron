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

#ifndef Engine_Nodes_Time_TimeBlur_h
#define Engine_Nodes_Time_TimeBlur_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Time/TimeSampledEffect.h"

#define PLUGINID_NATRON_TIMEBLUR "TimeBlur"
#define PLUGIN_MAJOR_NATRON_TIMEBLUR 1

#define kTimeBlurParamDivisions "divisions"
#define kTimeBlurParamShutter "shutter"
#define kTimeBlurParamShutterOffset kResamplerParamShutterOffset
#define kTimeBlurParamShutterCustomOffset kResamplerParamShutterCustomOffset

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kTimeBlurInputs[] = {
    InputSpec { .label = "Source", .time = InputTime::Any },
};

inline constexpr NodeSpec kTimeBlurNodeSpec = {
    .id = PLUGINID_NATRON_TIMEBLUR,
    .label = "TimeBlur",
    .description = "Averages the input over the shutter interval: Divisions evenly spaced, possibly fractional, times "
                   "from the shutter's opening, its closing excluded. Gives motion blur to anything upstream that "
                   "renders between frames; put a NoTimeBlur under the nodes that should not.",
    .grouping = PLUGIN_GROUP_TIME,
    .majorVersion = PLUGIN_MAJOR_NATRON_TIMEBLUR,
    .minorVersion = 0,
    .icon = NATRON_IMAGES_PATH "NativeNodes/TimeBlur.png",
    .inputs = kTimeBlurInputs,
};

inline constexpr ImagePipelineSpec kTimeBlurPipeline = {
    .stages = {},
    .layers = LayerPolicy::none(),
    .op = OpKind::Custom,
};

/**
 * @brief The times TimeBlur averages at time: divisions of them from the shutter's opening, spaced
 * (close - open) / divisions apart, so the closing is left out. One time, the opening, when the
 * shutter is 0 or there is one division.
 **/
std::vector<double> timeBlurSampleTimes(double time,
                                        int divisions,
                                        double shutter,
                                        Resampler::ShutterOffsetEnum offset,
                                        double customOffset);

/**
 * @brief TimeBlur: the equally weighted mean of its input at timeBlurSampleTimes(), or a route to
 * the input at the one time there is.
 **/
class TimeBlur
    : public TimeSampledEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new TimeBlur(node);
    }

    explicit TimeBlur(NodePtr node);

    virtual ~TimeBlur();

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    // Black without an input.
    virtual Sample sample(double time, ViewIdx view) const OVERRIDE FINAL WARN_UNUSED_RETURN;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;

    KnobIntWPtr _divisions;
    KnobDoubleWPtr _shutter;
    KnobChoiceWPtr _shutterOffset;
    KnobDoubleWPtr _shutterCustomOffset;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Time_TimeBlur_h
