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

#ifndef Engine_Nodes_Color_Threshold_h
#define Engine_Nodes_Color_Threshold_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"

#define PLUGINID_NATRON_THRESHOLD "Threshold"
#define PLUGIN_MAJOR_NATRON_THRESHOLD 1

#define kThresholdParamLevel "level"
#define kThresholdParamSoftness "softness"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kThresholdInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

/**
 * @brief Each processed channel becomes 1 at or above level + softness, 0 at or below
 * level - softness, and the linear ramp (v - low) / (high - low) between. At softness 0 a value
 * equal to the level gives 1. Per channel, in channel-bit order (a one-channel pixel is alpha).
 **/
struct ThresholdOp {
    struct Params {
        // level - softness and level + softness per channel, rounded to float as the GPU takes them.
        float low[4];
        float high[4];
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_THRESHOLD,
        .label = "Threshold",
        .description = "Threshold the selected channels: values below the Threshold Level become 0, and values at or above "
                       "it become 1.\n"
                       "With a nonzero Threshold Softness, values at or below level - softness become 0, values at or "
                       "above level + softness become 1, and values in between ramp linearly.\n"
                       "Colour values with a non-opaque alpha should in general be unpremultiplied for thresholding.",
        .grouping = PLUGIN_GROUP_COLOR,
        .majorVersion = PLUGIN_MAJOR_NATRON_THRESHOLD,
        .minorVersion = 0,
        .inputs = kThresholdInputs,
    };
    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::Premult, Stage::MaskMix, Stage::PassThroughUnprocessed },
        .layers = LayerPolicy::color(true, true, true, true),
        .op = OpKind::Point,
    };

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page);
    Params bake(const KnobReader& reader) const;
    bool isIdentity(const Params& params) const;
    void row(const Params& params,
             const RowIO& io) const;
    GpuOpRef gpu(const Params& params) const;

    KnobColorWPtr level;
    KnobColorWPtr softness;
};

static_assert(PointOp<ThresholdOp>);

class Threshold
    : public ImageNode<ThresholdOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new Threshold(node);
    }

    explicit Threshold(NodePtr node);

    virtual ~Threshold();

    /**
     * @brief The Params the op renders with at (time, view).
     **/
    ThresholdOp::Params bakeThreshold(double time,
                                      ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief The op's CPU kernel over one row.
     **/
    void thresholdRow(const ThresholdOp::Params& params,
                      const RowIO& io) const;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Color_Threshold_h
