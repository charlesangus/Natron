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

#ifndef Engine_Nodes_Filter_Sharpen_h
#define Engine_Nodes_Filter_Sharpen_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Filter/BlurKernels.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"

#define PLUGINID_NATRON_SHARPEN "Sharpen"
#define PLUGIN_MAJOR_NATRON_SHARPEN 1

#define kSharpenParamMode "mode"
#define kSharpenParamModeSharpen "sharpen"
#define kSharpenParamModeSoften "soften"
#define kSharpenParamModeLaplacian "laplacian"
#define kSharpenParamSize "size"
// The filter's options are Blur's kBlurParamFilter* IDs.
#define kSharpenParamFilter "filter"
#define kSharpenParamAmount "amount"
#define kSharpenParamThreshold "threshold"

NATRON_NAMESPACE_ENTER

enum SharpenModeEnum {
    eSharpenModeSharpen = 0,
    eSharpenModeSoften,
    eSharpenModeLaplacian,
};

/**
 * @brief The knob values one Sharpen evaluation reads. sizeX and sizeY are the size knob's, in
 * canonical units, scaled to pixels at the render scale and divided by the pixel aspect ratio as
 * Blur's are. threshold is relative to the blurred value, and 0 turns it off.
 **/
struct SharpenParams {
    SharpenModeEnum mode;
    double sizeX;
    double sizeY;
    BlurKernels::Filter filter;
    double amount;
    double threshold;

    SharpenParams()
        : mode(eSharpenModeSharpen)
        , sizeX(3.)
        , sizeY(3.)
        , filter(BlurKernels::eFilterFIRGaussian)
        , amount(1.)
        , threshold(0.)
    {
    }
};

inline constexpr InputSpec kSharpenInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

/**
 * @brief Sharpen: an unsharp mask. With d = in - blur, the detail the blur removes, Sharpen
 * writes in + amount * w * d, Soften in - amount * w * d and Laplacian amount * d. w is the
 * threshold's weight, from the detail relative to the blurred value, r = |d| / max(|blur|, eps):
 * 1 with the threshold off, else 0 up to r = threshold, rising linearly to 1 at twice it. Being
 * relative, it judges detail alike on a dark and on a bright plate. Nothing is clamped, so
 * negative and over-range values go through.
 *
 * The blur is one of Blur's filters with the nearest pixel repeated outside the image (Neumann),
 * so the region of definition is the source's. Its passes copy the source into a second buffer,
 * blur that one with Blur's halo, and combine both into the first.
 **/
struct SharpenOp {
    using Params = SharpenParams;

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_SHARPEN,
        .label = "Sharpen",
        .description = "Sharpen, soften or extract the detail of the input stream with an unsharp mask.\n"
                       "\n"
                       "The detail is the input minus its blur. Sharpen adds amount times the detail to the input, Soften subtracts it, and Laplacian outputs amount times the detail alone.\n"
                       "\n"
                       "The threshold leaves out detail that is small relative to the local blurred value: detail below threshold times the blurred value is not sharpened, detail above twice that is fully sharpened, and in between it ramps linearly. The threshold is relative, so it treats a dark and a bright plate alike. It does not apply in Laplacian mode. 0 turns it off.\n"
                       "\n"
                       "The blur repeats the edge pixels outside the image, and nothing is clamped: values below 0 and above 1 go through.",
        .grouping = PLUGIN_GROUP_FILTER,
        .majorVersion = PLUGIN_MAJOR_NATRON_SHARPEN,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/Sharpen.png",
        .inputs = kSharpenInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::Premult, Stage::MaskMix, Stage::PassThroughUnprocessed },
        .layers = LayerPolicy::all(),
        .op = OpKind::Passes,
    };

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page);

    Params bake(const KnobReader& reader) const;

    /**
     * @brief Sharpen and Soften at amount 0, or with a blur that leaves every line unchanged at
     * the context's scale. Laplacian never is: it writes the detail, not the source.
     **/
    bool isIdentity(const Params& params,
                    const PassContext& context) const;

    /**
     * @brief The copy, the horizontal and the vertical blur with Blur's halo and no spread, and
     * the combination. The GPU recorder is left out where a FIR Gaussian radius exceeds the GPU
     * blur's.
     **/
    PassList passes(const Params& params,
                    const PassContext& context) const;

    KnobChoiceWPtr mode;
    KnobDoubleWPtr size;
    KnobChoiceWPtr filter;
    KnobDoubleWPtr amount;
    KnobDoubleWPtr threshold;
};

static_assert(PassesOp<SharpenOp>);

class Sharpen
    : public ImageNode<SharpenOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new Sharpen(node);
    }

    explicit Sharpen(NodePtr node);

    virtual ~Sharpen();

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    /**
     * @brief The pixels needed to sharpen `rect` (pixel coordinates at `scale`) with params on a
     * square-pixel source, before clipping to the source's region of definition.
     **/
    static RectI getSourceRoI(const RectI& rect, const RenderScale& scale, const SharpenParams& params) WARN_UNUSED_RETURN;

    /**
     * @brief The weight w of the threshold for detail d over blurred value b: 1 at threshold 0.
     * The GPU combine computes the same in float.
     **/
    static float thresholdWeight(float d, float b, float threshold) WARN_UNUSED_RETURN;

    /**
     * @brief The floor of |blur| in the relative detail, which keeps it finite on black.
     **/
    static constexpr float kRelativeEpsilon = 1e-6f;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_Sharpen_h
