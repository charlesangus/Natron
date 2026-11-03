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

#ifndef Engine_Nodes_Time_NoTimeBlur_h
#define Engine_Nodes_Time_NoTimeBlur_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"

#define PLUGINID_NATRON_NOTIMEBLUR "NoTimeBlur"
#define PLUGIN_MAJOR_NATRON_NOTIMEBLUR 1

#define kNoTimeBlurParamRounding "rounding"

#define kNoTimeBlurRoundingRint "rint"
#define kNoTimeBlurRoundingFloor "floor"
#define kNoTimeBlurRoundingCeil "ceil"
#define kNoTimeBlurRoundingNone "none"

NATRON_NAMESPACE_ENTER

namespace NoTimeBlurRounding {
// Integer values equal the option indices of the Rounding knob.
enum Mode {
    eRint = 0,
    eFloor,
    eCeil,
    eNone,
};

// time through mode; Rint rounds a half to the even frame.
double apply(Mode mode, double time) WARN_UNUSED_RETURN;
} // namespace NoTimeBlurRounding

inline constexpr InputSpec kNoTimeBlurInputs[] = {
    InputSpec { .label = "Source", .time = InputTime::Any },
};

/**
 * @brief NoTimeBlur: its Source at the requested time rounded to a whole frame, so that nodes
 * below a TimeBlur, or anything else asking for fractional frames, render whole frames only. It
 * renders nothing: the host reads the Source at the rounded time.
 **/
struct NoTimeBlurOp {
    struct Params {
        NoTimeBlurRounding::Mode rounding;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_NOTIMEBLUR,
        .label = "NoTimeBlur",
        .description = "Rounds the time asked of the input to a whole frame, so that the nodes above it render whole "
                       "frames only, even under a TimeBlur or a retime between frames.",
        .grouping = PLUGIN_GROUP_TIME,
        .majorVersion = PLUGIN_MAJOR_NATRON_NOTIMEBLUR,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/NoTimeBlur.png",
        .inputs = kNoTimeBlurInputs,
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

    KnobChoiceWPtr rounding;
};

static_assert(RouteOp<NoTimeBlurOp>);

class NoTimeBlur
    : public ImageNode<NoTimeBlurOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new NoTimeBlur(node);
    }

    explicit NoTimeBlur(NodePtr node);

    virtual ~NoTimeBlur();
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Time_NoTimeBlur_h
