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

#ifndef Engine_Nodes_Filter_Bloom_h
#define Engine_Nodes_Filter_Bloom_h

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

#define PLUGINID_NATRON_BLOOM "Bloom"
#define PLUGIN_MAJOR_NATRON_BLOOM 1

#define kBloomParamSize "size"
#define kBloomParamRatio "ratio"
#define kBloomParamCount "count"
// The filter's options are Blur's kBlurParamFilter* IDs.
#define kBloomParamFilter "filter"

NATRON_NAMESPACE_ENTER

/**
 * @brief The knob values one Bloom evaluation reads. sizeX and sizeY are the size knob's, in
 * canonical units, scaled to pixels at the render scale and divided by the pixel aspect ratio as
 * Blur's are.
 **/
struct BloomParams {
    double sizeX;
    double sizeY;
    double ratio;
    int count;
    BlurKernels::Filter filter;

    BloomParams()
        : sizeX(0.)
        , sizeY(0.)
        , ratio(2.)
        , count(5)
        , filter(BlurKernels::eFilterFIRGaussian)
    {
    }
};

inline constexpr InputSpec kBloomInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

/**
 * @brief Bloom: the mean of count blurs of the source, of sizes size * ratio^i for i = 0 to
 * count - 1, as CImg's Bloom. The Gaussian (FIR) blurs run on a decimated pyramid
 * (BloomKernels.h), so the cost stays bounded at any size; the other filters run at full
 * resolution as Blur's. The nearest pixel is repeated outside the image, so the region of
 * definition is the source's. Scene-linear: nothing is clamped.
 **/
struct BloomOp {
    using Params = BloomParams;

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_BLOOM,
        .label = "Bloom",
        .description = "Bloom the input stream: the mean of several blurs of growing size.\n"
                       "\n"
                       "The first blur has the given size, and each next one is ratio times larger, count blurs in all. Their mean keeps the overall level of the image, so a constant stays the same.\n"
                       "\n"
                       "The Gaussian (FIR) filter, the default, blurs large sizes on a downsampled copy of the image, so its cost hardly grows with the size. The other filters are Blur's, at full resolution.\n"
                       "\n"
                       "The edge pixels are repeated outside the image, and nothing is clamped: values below 0 and above 1 go through.",
        .grouping = PLUGIN_GROUP_FILTER,
        .majorVersion = PLUGIN_MAJOR_NATRON_BLOOM,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/Bloom.png",
        .inputs = kBloomInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::MaskMix, Stage::PassThroughUnprocessed },
        .layers = LayerPolicy::all(),
        .op = OpKind::Passes,
    };

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page);

    Params bake(const KnobReader& reader) const;

    /// When the largest blur leaves every line unchanged at the context's scale.
    bool isIdentity(const Params& params,
                    const PassContext& context) const;

    /// BloomKernels' passes from slot 0 into slot 1, with the nearest pixel repeated, and its GPU
    /// recorder.
    PassList passes(const Params& params,
                    const PassContext& context) const;

    KnobDoubleWPtr size;
    KnobDoubleWPtr ratio;
    KnobIntWPtr count;
    KnobChoiceWPtr filter;
};

static_assert(PassesOp<BloomOp>);

class Bloom
    : public ImageNode<BloomOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new Bloom(node);
    }

    explicit Bloom(NodePtr node);

    virtual ~Bloom();

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    /**
     * @brief The pixels needed to bloom `rect` (pixel coordinates at `scale`) with params on a
     * square-pixel source, before clipping to the source's region of definition.
     **/
    static RectI getSourceRoI(const RectI& rect, const RenderScale& scale, const BloomParams& params) WARN_UNUSED_RETURN;

    /// Blur's filter choice, with the Gaussian (FIR) the default; Glow's filter knob too.
    static KnobChoicePtr createFilterKnob(KnobHolder* holder, const KnobPagePtr& page, const char* name);
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_Bloom_h
