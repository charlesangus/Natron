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

#ifndef Engine_Nodes_Time_FrameRange_h
#define Engine_Nodes_Time_FrameRange_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"

#define PLUGINID_NATRON_FRAMERANGE "FrameRange"
#define PLUGIN_MAJOR_NATRON_FRAMERANGE 1

#define kFrameRangeParamFirstFrame "firstFrame"
#define kFrameRangeParamLastFrame "lastFrame"
#define kFrameRangeParamReset "reset"
#define kFrameRangeParamBefore "before"
#define kFrameRangeParamAfter "after"

#define kFrameRangeOriginal "original"
#define kFrameRangeHold "hold"
#define kFrameRangeLoop "loop"
#define kFrameRangeBounce "bounce"
#define kFrameRangeBlack "black"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kFrameRangeInputs[] = {
    InputSpec { .label = "Source", .time = InputTime::Any },
};

/**
 * @brief FrameRange: its Source over [firstFrame, lastFrame], which becomes its frame range, and
 * before or after it the Source as is, held at the nearest end, looped, bounced, or black. It
 * renders nothing: the host reads the Source at the time chosen, and Black is an empty image.
 **/
struct FrameRangeOp {
    // Integer values equal the option indices of the before and after knobs.
    enum Mode {
        eOriginal = 0,
        eHold,
        eLoop,
        eBounce,
        eBlack,
    };

    struct Params {
        int first;
        int last;
        Mode before;
        Mode after;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_FRAMERANGE,
        .label = "FrameRange",
        .description = "Sets the frame range of the input, which a Write or the viewer's timeline then uses, and "
                       "chooses what the frames before and after that range show.",
        .grouping = PLUGIN_GROUP_TIME,
        .majorVersion = PLUGIN_MAJOR_NATRON_FRAMERANGE,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/FrameRange.png",
        .inputs = kFrameRangeInputs,
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
    KnobIntWPtr firstFrame;
    KnobIntWPtr lastFrame;
    KnobButtonWPtr reset;
    KnobChoiceWPtr before;
    KnobChoiceWPtr after;
};

static_assert(RouteOp<FrameRangeOp>);

class FrameRange
    : public ImageNode<FrameRangeOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new FrameRange(node);
    }

    explicit FrameRange(NodePtr node);

    virtual ~FrameRange();

    /**
     * @brief [firstFrame, lastFrame], a last frame before the first counting as the first.
     **/
    virtual void getFrameRange(double* first,
                               double* last) OVERRIDE FINAL;

private:
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    // Sets the range to the Source's, when it is bounded.
    void resetToInputRange();

    void refreshSubLabel();
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Time_FrameRange_h
