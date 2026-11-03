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

#ifndef Engine_Nodes_Time_FrameHold_h
#define Engine_Nodes_Time_FrameHold_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Time/TimeRounding.h"

#define PLUGINID_NATRON_FRAMEHOLD "FrameHold"
#define PLUGIN_MAJOR_NATRON_FRAMEHOLD 1

#define kFrameHoldParamRoundingMode "roundingMode"
#define kFrameHoldParamFirstFrame "firstFrame"
#define kFrameHoldParamSetToCurrentFrame "setToCurrentFrame"
#define kFrameHoldParamIncrement "increment"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kFrameHoldInputs[] = {
    InputSpec { .label = "Source", .time = InputTime::Any },
};

/**
 * @brief The frame FrameHold reads at time: firstFrame with no increment, else the last of
 * firstFrame + n * increment at or before time, rounded down to a whole frame unless fractional.
 **/
double frameHoldSourceTime(double time, double firstFrame, int increment, bool fractional);

/**
 * @brief FrameHold: its Source held at firstFrame, or, with an increment, at every increment-th
 * frame from firstFrame, each held until the next. It renders nothing: the host reads the Source
 * at the held frame.
 **/
struct FrameHoldOp {
    struct Params {
        bool fractional;
        double firstFrame;
        int increment;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_FRAMEHOLD,
        .label = "FrameHold",
        .description = "Holds one frame of the input at every frame, or, with an increment, holds every "
                       "increment-th frame from the first frame until the next one.",
        .grouping = PLUGIN_GROUP_TIME,
        .majorVersion = PLUGIN_MAJOR_NATRON_FRAMEHOLD,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/FrameHold.png",
        .inputs = kFrameHoldInputs,
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

    KnobStringWPtr subLabel;
    KnobChoiceWPtr roundingMode;
    KnobDoubleWPtr firstFrame;
    KnobButtonWPtr setToCurrentFrame;
    KnobIntWPtr increment;
};

static_assert(RouteOp<FrameHoldOp>);

class FrameHold
    : public ImageNode<FrameHoldOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new FrameHold(node);
    }

    explicit FrameHold(NodePtr node);

    virtual ~FrameHold();

private:
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    // Shows the held frame, and the increment, next to the node's label.
    void refreshSubLabel(double time);
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Time_FrameHold_h
