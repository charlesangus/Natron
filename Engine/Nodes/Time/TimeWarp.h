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

#ifndef Engine_Nodes_Time_TimeWarp_h
#define Engine_Nodes_Time_TimeWarp_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Time/TimeSampledEffect.h"

#define PLUGINID_NATRON_TIMEWARP "TimeWarp"
#define PLUGIN_MAJOR_NATRON_TIMEWARP 1

#define kTimeWarpParamLookup "lookup"
#define kTimeWarpParamFilter kTimeSampleParamFilter
#define kTimeWarpParamShutter kTimeSampleParamShutter

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kTimeWarpInputs[] = {
    InputSpec { .label = "Source", .time = InputTime::Any },
};

inline constexpr NodeSpec kTimeWarpNodeSpec = {
    .id = PLUGINID_NATRON_TIMEWARP,
    .label = "TimeWarp",
    .description = "Shows, at each output frame, the input frame a curve looks up: animate Lookup to slow down, speed "
                   "up, hold or reverse parts of the input without changing its length. A new TimeWarp's curve is the "
                   "identity. Chooses how a frame between two input frames is made.",
    .grouping = PLUGIN_GROUP_TIME,
    .majorVersion = PLUGIN_MAJOR_NATRON_TIMEWARP,
    .minorVersion = 0,
    .icon = NATRON_IMAGES_PATH "NativeNodes/TimeWarp.png",
    .inputs = kTimeWarpInputs,
};

inline constexpr ImagePipelineSpec kTimeWarpPipeline = {
    .stages = {},
    .layers = LayerPolicy::none(),
    .op = OpKind::Custom,
};

/**
 * @brief TimeWarp: the output at a frame is the input at lookup(frame), through the filter. A
 * Blend between two input frames renders, on the CPU or the GPU; everything else is a route to the
 * input.
 **/
class TimeWarp
    : public TimeSampledEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new TimeWarp(node);
    }

    explicit TimeWarp(NodePtr node);

    virtual ~TimeWarp();

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief Lookup at time through the filter. Box's shutter, centred on time, goes through the
     * curve like time; while the lookup is inside the input's frame range the span stays inside
     * it. Black without an input.
     **/
    virtual Sample sample(double time, ViewIdx view) const OVERRIDE FINAL WARN_UNUSED_RETURN;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;

    // Keys a new node's lookup as the identity over the project's frame range.
    virtual void onEffectCreated(bool mayCreateFileDialog,
                                 const CreateNodeArgs& args) OVERRIDE FINAL;

    KnobDoubleWPtr _lookup;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Time_TimeWarp_h
