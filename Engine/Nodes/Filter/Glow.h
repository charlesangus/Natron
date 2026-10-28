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

#include <optional>

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
#define kGlowParamRotate "rotate"
#define kGlowParamIsolateHue "isolateHue"
#define kGlowParamPickHue "pickHue"

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
    // Per channel, red, green, blue and alpha: the gain, times brightness, and the gamma, at least
    // 0.01. The saturation is per colour channel.
    double tint[4] = { 1., 1., 1., 1. };
    double brightness = 1.;
    double saturation[3] = { 1., 1., 1. };
    double gamma[4] = { 1., 1., 1., 1. };
    bool screen = false;
    bool addInput = false;
    bool glowOnly = false;
    bool expandRoD = false;
    // Degrees: the bloom runs on the image turned counter-clockwise by rotate, as the PyPlug's
    // transforms turned it, so the stretch turns clockwise by rotate.
    double rotate = 0.;
    bool isolateHue = false;
    // The hue of the picked colour in degrees, in [0, 360).
    double hue = 0.;
    double luma[3] = { 0., 0., 0. };
    // The output's blend with the source, in [0, 1]: 0 is the source, 1 the full output.
    double mix = 1.;
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
 * The map's colour channels are saturated about its luminance, each by its own amount; the map is
 * then bloomed with Bloom's kernel (BloomKernels.h), optionally added to its bloom, and graded
 * into the glow as ColorCorrect grades: each channel gamma-corrected, then multiplied by its tint
 * and the brightness. The output is the source plus (or screened with) the glow, the glow alone,
 * or the ungraded map as a preview. A processed alpha channel is glowed without saturation,
 * with its own gamma and tint, and clamped to [0, 1] outside the preview. Mix blends each processed
 * channel of the output, preview included, with the source's: source + mix * (output - source).
 * Black lies outside the image; expand RoD grows the region of definition by the bloom's reach.
 * Scene-linear: nothing else is clamped. The source is glowed as it comes, with no unpremultiply:
 * light adds to a premultiplied image as it is.
 *
 * The map is multiplied by the mask and, with isolate hue, by the closeness of each pixel's hue to
 * the picked one: 1 at it, falling linearly to 0 at 50 degrees away, as the PyPlug's HSVTool keyed
 * it. With a stretch, rotate turns the bloom: the map is resampled bilinearly into the turned
 * frame, bloomed there, and resampled back.
 **/
struct GlowOp {
    using Params = GlowParams;

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_GLOW,
        .label = "Glow",
        .description = "Make the highlights of the input glow.\n"
                       "\n"
                       "The pixels brighter than the threshold (by their luminance in the project's working space, with a soft knee) form the glow map, which the mask and, optionally, a picked hue restrict. It is saturated, bloomed (the mean of count blurs of growing size, as Bloom, stretched along the rotated direction), graded per channel (gamma, then tint and brightness) and added on top of the input, or screened over it.\n"
                       "\n"
                       "Glow Only outputs the glow alone, to merge later; Preview shows the glow map, to adjust the threshold and the hue isolation.\n"
                       "\n"
                       "Mix blends the output with the input: 0 leaves the input unchanged.",
        .grouping = PLUGIN_GROUP_FILTER,
        .majorVersion = PLUGIN_MAJOR_NATRON_GLOW,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/Glow.png",
        .inputs = kGlowInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        // The mask multiplies the glow map inside the op (PassList::opMask), so there is no MaskMix:
        // the op makes its own Mix knob and blends with the source itself.
        .stages = { Stage::PassThroughUnprocessed },
        .layers = LayerPolicy::color(true, true, true, false),
        .op = OpKind::Passes,
    };

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page);

    /// Reads the project's working luma weights too, ACEScg's without a project. Every node's render
    /// hash folds in the project's content digest, which covers its working space.
    Params bake(const KnobReader& reader) const;

    /// Only at a mix of 0: even a bloom that is the identity adds the map.
    bool isIdentity(const Params& params,
                    const PassContext& context) const;

    /// The map, the bloom and the combination over the scratch slots, and their GPU recorder.
    PassList passes(const Params& params,
                    const PassContext& context) const;

    /**
     * @brief The Filter weight the bloom's filter and largest blur call for (BloomKernels::gpuWeight).
     **/
    std::optional<GpuWeight> gpuWeight(const Params& params,
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
    KnobColorWPtr saturation;
    KnobColorWPtr gamma;
    KnobBoolWPtr screen;
    KnobBoolWPtr addInput;
    KnobBoolWPtr glowOnly;
    KnobBoolWPtr expandRoD;
    KnobDoubleWPtr rotate;
    KnobBoolWPtr isolateHue;
    KnobColorWPtr pickHue;
    KnobDoubleWPtr mix;
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
