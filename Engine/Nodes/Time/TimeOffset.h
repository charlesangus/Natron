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

#ifndef Engine_Nodes_Time_TimeOffset_h
#define Engine_Nodes_Time_TimeOffset_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Time/TimeRounding.h"

#define PLUGINID_NATRON_TIMEOFFSET "TimeOffset"
#define PLUGIN_MAJOR_NATRON_TIMEOFFSET 1

#define kTimeOffsetParamRoundingMode "roundingMode"
#define kTimeOffsetParamTimeOffset "timeOffset"
#define kTimeOffsetParamReverseInput "reverseInput"
#define kTimeOffsetParamClipToInputRange "clipToInputRange"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kTimeOffsetInputs[] = {
    InputSpec { .label = "Source", .time = InputTime::Any },
};

/**
 * @brief TimeOffset: its Source at time - timeOffset, optionally reversed over the Source's frame
 * range and clipped to it, then rounded down to a whole frame unless the rounding mode is
 * Fractional. It renders nothing: the host reads the Source at that time. Its frame range is the
 * Source's moved by the offset.
 **/
struct TimeOffsetOp {
    struct Params {
        bool fractional;
        double offset;
        bool reverse;
        bool clip;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_TIMEOFFSET,
        .label = "TimeOffset",
        .description = "Moves the input through time: frame t shows the input at t minus the offset, so a "
                       "positive offset shows each input frame later. The input can also be played backwards "
                       "over its frame range, and held at the ends of that range.",
        .grouping = PLUGIN_GROUP_TIME,
        .majorVersion = PLUGIN_MAJOR_NATRON_TIMEOFFSET,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/TimeOffset.png",
        .inputs = kTimeOffsetInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = {},
        .layers = LayerPolicy::none(),
        .op = OpKind::Route,
    };

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page);

    Params bake(const KnobReader& reader) const;

    Route route(const Params& params,
                const RouteContext& context) const;

    KnobChoiceWPtr roundingMode;
    KnobDoubleWPtr timeOffset;
    KnobBoolWPtr reverseInput;
    KnobBoolWPtr clipToInputRange;
};

static_assert(RouteOp<TimeOffsetOp>);

class TimeOffset
    : public ImageNode<TimeOffsetOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new TimeOffset(node);
    }

    explicit TimeOffset(NodePtr node);

    virtual ~TimeOffset();

    /**
     * @brief The Source's frame range moved by the offset; unbounded with an unbounded Source.
     * With an animated offset each end is the output frame that shows that end of the Source.
     **/
    virtual void getFrameRange(double* first,
                               double* last) OVERRIDE FINAL;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Time_TimeOffset_h
