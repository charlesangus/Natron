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

#ifndef Engine_Nodes_Color_Clamp_h
#define Engine_Nodes_Color_Clamp_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"

#define PLUGINID_NATRON_CLAMP "Clamp"
#define PLUGIN_MAJOR_NATRON_CLAMP 1

#define kClampParamMinimum "minimum"
#define kClampParamMinimumEnable "minimumEnable"
#define kClampParamMaximum "maximum"
#define kClampParamMaximumEnable "maximumEnable"
#define kClampParamMinClampTo "minClampTo"
#define kClampParamMinClampToEnable "minClampToEnable"
#define kClampParamMaxClampTo "maxClampTo"
#define kClampParamMaxClampToEnable "maxClampToEnable"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kClampInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

/**
 * @brief Clamp: per processed channel, a value below `minimum` becomes minClampTo when
 * minClampToEnable is on and `minimum` otherwise, and a value above `maximum` becomes maxClampTo
 * or `maximum` likewise, each side only when its enable is on. The minimum side is tested first.
 * Knob names, defaults and maths are the openfx-misc Clamp plugin's; it registers under that
 * plugin's ID one major above.
 **/
struct ClampOp {
    // Doubles, as the CPU row compares and clamps in double; the GPU block takes floats from them.
    struct Params {
        double minimum[4];
        double maximum[4];
        double minClampTo[4];
        double maxClampTo[4];
        bool minimumEnable;
        bool maximumEnable;
        bool minClampToEnable;
        bool maxClampToEnable;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_CLAMP,
        .label = "Clamp",
        .description = "Clamp the values of the selected channels.\n"
                       "\n"
                       "A special use case for the Clamp plugin is to generate a binary mask image "
                       "(i.e. each pixel is either 0 or 1) by thresholding an image. Let us say one wants "
                       "all input pixels whose value is above or equal to some threshold value to "
                       "become 1, and all values below this threshold to become 0. Set the \"Minimum\" value "
                       "to the threshold, set the \"Maximum\" to any value strictly below the threshold "
                       "(e.g. 0 if the threshold is positive), and "
                       "check \"Enable MinClampTo\" and \"Enable MaxClampTo\" while keeping the default "
                       "values for \"MinClampTo\" (0.0) and \"MaxClampTo\" (1.0). The result is a binary "
                       "mask image. To create a non-binary mask, with softer edges, either blur the output "
                       "of Clamp, or use the Grade plugin instead, setting the \"Black Point\" and \"White Point\" "
                       "to values close to the threshold, and checking the \"Clamp Black\" and \"Clamp "
                       "White\" options.\n"
                       "\n"
                       "See also: https://web.archive.org/web/20220627024614/http://www.opticalenquiry.com/nuke/index.php?title=Clamp",
        .grouping = PLUGIN_GROUP_COLOR,
        .majorVersion = PLUGIN_MAJOR_NATRON_CLAMP,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/Clamp.png",
        .inputs = kClampInputs,
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

    KnobColorWPtr minimum;
    KnobBoolWPtr minimumEnable;
    KnobColorWPtr maximum;
    KnobBoolWPtr maximumEnable;
    KnobColorWPtr minClampTo;
    KnobBoolWPtr minClampToEnable;
    KnobColorWPtr maxClampTo;
    KnobBoolWPtr maxClampToEnable;
};

static_assert(PointOp<ClampOp>);

class Clamp
    : public ImageNode<ClampOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new Clamp(node);
    }

    explicit Clamp(NodePtr node);

    virtual ~Clamp();

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    /**
     * @brief The Params the op renders with at (time, view).
     **/
    ClampOp::Params bakeClamp(double time,
                              ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief The op's CPU kernel over one row.
     **/
    void clampRow(const ClampOp::Params& params,
                  const RowIO& io) const;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Color_Clamp_h
