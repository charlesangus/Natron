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

#ifndef Engine_Nodes_Filter_Glow_h
#define Engine_Nodes_Filter_Glow_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Filter/BlurKernels.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"

#define PLUGINID_NATRON_GLOW "Glow"
#define PLUGIN_MAJOR_NATRON_GLOW 1

#define kGlowParamThreshold "threshold"
#define kGlowParamKnee "knee"
#define kGlowParamPreview "preview"
#define kGlowParamSize "size"
#define kGlowParamStretch "stretch"
#define kGlowParamRatio "ratio"
#define kGlowParamCount "count"
// The filter's options are Blur's kBlurParamFilter* IDs.
#define kGlowParamFilter "filter"
#define kGlowParamTint "tint"
#define kGlowParamBrightness "brightness"
#define kGlowParamSaturation "saturation"
#define kGlowParamGamma "gamma"
#define kGlowParamScreen "screen"
#define kGlowParamAddInput "addInput"
#define kGlowParamGlowOnly "glowOnly"
#define kGlowParamExpandRoD "expandRoD"

NATRON_NAMESPACE_ENTER

/**
 * @brief The knob values one Glow evaluation reads. size is the size knob's, in canonical units;
 * the bloom's horizontal size is size divided by the pixel aspect ratio, its vertical one
 * size * (1 - stretch), both scaled to the render scale. luma holds the project working space's
 * luma weights.
 **/
struct GlowParams {
    double threshold = 0.25;
    double knee = 0.5;
    bool preview = false;
    double size = 3.;
    double stretch = 0.;
    double ratio = 2.;
    int count = 7;
    BlurKernels::Filter filter = BlurKernels::eFilterFIRGaussian;
    double tint[3] = { 1., 1., 1. };
    double brightness = 1.;
    double saturation = 1.;
    double gamma = 1.;
    bool screen = false;
    bool addInput = false;
    bool glowOnly = false;
    bool expandRoD = false;
    double luma[3] = { 0., 0., 0. };
};

inline constexpr InputSpec kGlowInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

/**
 * @brief Glow: the highlights of the source, bloomed and added back.
 *
 * The map keeps the pixels whose luminance L, with the project working space's luma weights,
 * exceeds the threshold, through a soft knee: each processed channel is scaled by c(L) / L, where
 * c is 0 up to the threshold, a parabola over the knee's width and then L - threshold - knee / 2.
 * The map is graded (saturation about its luminance, tint and brightness), bloomed with Bloom's
 * kernel (BloomKernels.h), optionally added to its bloom, and gamma-corrected into the glow. The
 * output is the source plus (or screened with) the glow, the glow alone, or the ungraded map as a
 * preview. A processed alpha channel is glowed without tint or saturation, and clamped to [0, 1]
 * outside the preview. Black lies outside the image; expand RoD grows the region of definition by
 * the bloom's reach. Scene-linear: nothing else is clamped.
 **/
struct GlowOp {
    using Params = GlowParams;

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_GLOW,
        .label = "Glow",
        .description = "Make the highlights of the input glow.\n"
                       "\n"
                       "The pixels brighter than the threshold (by their luminance in the project's working space, with a soft knee) form the glow map. It is graded, bloomed (the mean of count blurs of growing size, as Bloom) and added on top of the input, or screened over it.\n"
                       "\n"
                       "Glow Only outputs the glow alone, to merge later; Preview shows the glow map, to adjust the threshold.",
        .grouping = PLUGIN_GROUP_FILTER,
        .majorVersion = PLUGIN_MAJOR_NATRON_GLOW,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/Glow.png",
        .inputs = kGlowInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::Premult, Stage::MaskMix, Stage::PassThroughUnprocessed },
        .layers = LayerPolicy::color(true, true, true, false),
        .op = OpKind::Passes,
    };

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page);

    /// Reads the project's working luma weights too, ACEScg's without a project.
    Params bake(const KnobReader& reader) const;

    /// Never: even a bloom that is the identity adds the map.
    bool isIdentity(const Params& params,
                    const PassContext& context) const;

    /// The map, the bloom and the combination over the scratch slots, and their GPU recorder.
    PassList passes(const Params& params,
                    const PassContext& context) const;

    KnobHolder* holder = nullptr;
    KnobDoubleWPtr threshold;
    KnobDoubleWPtr knee;
    KnobBoolWPtr preview;
    KnobDoubleWPtr size;
    KnobDoubleWPtr stretch;
    KnobDoubleWPtr ratio;
    KnobIntWPtr count;
    KnobChoiceWPtr filter;
    KnobColorWPtr tint;
    KnobDoubleWPtr brightness;
    KnobDoubleWPtr saturation;
    KnobDoubleWPtr gamma;
    KnobBoolWPtr screen;
    KnobBoolWPtr addInput;
    KnobBoolWPtr glowOnly;
    KnobBoolWPtr expandRoD;
};

static_assert(PassesOp<GlowOp>);

class Glow
    : public ImageNode<GlowOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new Glow(node);
    }

    explicit Glow(NodePtr node);

    virtual ~Glow();

    /**
     * @brief The pixels needed to glow `rect` (pixel coordinates at `scale`) with params on a
     * square-pixel source, before clipping to the source's region of definition.
     **/
    static RectI getSourceRoI(const RectI& rect, const RenderScale& scale, const GlowParams& params) WARN_UNUSED_RETURN;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_Glow_h
