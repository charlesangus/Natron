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

#ifndef Engine_Nodes_Filter_Median_h
#define Engine_Nodes_Filter_Median_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <optional>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/RectI.h"

#define PLUGINID_NATRON_MEDIAN "Median"
#define PLUGIN_MAJOR_NATRON_MEDIAN 1

#define kMedianParamSize "size"
#define kMedianParamThreshold "threshold"

NATRON_NAMESPACE_ENTER

/**
 * @brief The knob values one Median evaluation reads. size is the window's radius in pixels at
 * render scale 1; threshold is relative to the median, and 0 always replaces.
 **/
struct MedianParams {
    int size;
    double threshold;

    MedianParams()
        : size(1)
        , threshold(0.)
    {
    }
};

inline constexpr InputSpec kMedianInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

/**
 * @brief Median: each processed channel of a pixel becomes the median of that channel over the
 * square window of 2r + 1 pixels a side centred on it, r being floor(size * render scale). With a
 * threshold t > 0 a value v is kept when |v - m| <= t * max(|m|, eps), m being the median, and
 * replaced by m otherwise; being relative to m, the test judges a dark and a bright plate alike.
 *
 * The window reads the nearest pixel of the region of definition beyond its edges, and zero where
 * the region holds no source pixel. A NaN sample counts as 0 and -0 as +0, so every window holds
 * (2r + 1)^2 ordered values and the median is one of them; a NaN source value is always replaced.
 * One gather pass computes it over the render window plus r pixels, clipped to the region of
 * definition, which the node does not grow.
 **/
struct MedianOp {
    using Params = MedianParams;

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_MEDIAN,
        .label = "Median",
        .description = "Replace each pixel by the median of the square window of 2*size+1 pixels a side centred on it, channel by channel.\n"
                       "\n"
                       "The median removes impulse noise (isolated bright or dark pixels) while keeping edges sharp.\n"
                       "\n"
                       "With a threshold above 0, a pixel is only replaced where it differs from the median by more than threshold times the median, so the test is relative and treats a dark and a bright plate alike. At 0 every pixel is replaced.\n"
                       "\n"
                       "Beyond the edges of the image the window repeats the nearest pixel. NaNs count as 0 and are always replaced. Nothing is clamped: values below 0 and above 1 go through.",
        .grouping = PLUGIN_GROUP_FILTER,
        .majorVersion = PLUGIN_MAJOR_NATRON_MEDIAN,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/Median.png",
        .inputs = kMedianInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::MaskMix, Stage::PassThroughUnprocessed },
        .layers = LayerPolicy::color(true, true, true, false),
        .op = OpKind::Passes,
    };

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page);

    Params bake(const KnobReader& reader) const;

    /**
     * @brief A radius of 0 pixels at the context's scale.
     **/
    bool isIdentity(const Params& params,
                    const PassContext& context) const;

    /**
     * @brief One gather pass from the source slot into a second slot, with a halo of the radius
     * and no spread; none at radius 0. The GPU recorder is left out above kMedianGpuMaxRadius.
     **/
    PassList passes(const Params& params,
                    const PassContext& context) const;

    /**
     * @brief The radius in pixels at the context's scale.
     **/
    std::optional<GpuCostHint> costHint(const Params& params,
                                        const PassContext& context) const;

    KnobIntWPtr size;
    KnobDoubleWPtr threshold;
};

static_assert(PassesOp<MedianOp>);

class Median
    : public ImageNode<MedianOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new Median(node);
    }

    explicit Median(NodePtr node);

    virtual ~Median();

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    void getParams(double time, ViewIdx view, MedianParams* params) const;

    /**
     * @brief The window radius in pixels for params at scale.
     **/
    static int pixelRadius(const RenderScale& scale, const MedianParams& params) WARN_UNUSED_RETURN;

    /**
     * @brief The pixels needed to filter `rect` (pixel coordinates at `scale`) with params, before
     * clipping to the region of definition.
     **/
    static RectI getSourceRoI(const RectI& rect, const RenderScale& scale, const MedianParams& params) WARN_UNUSED_RETURN;

    /**
     * @brief A sample as the window holds it: 0 for a NaN or either zero, the value otherwise.
     **/
    static float windowSample(float v) WARN_UNUSED_RETURN;

    /**
     * @brief What a pixel of value `in` becomes for window median m: m at threshold 0, for a NaN
     * `in`, or when |in - m| > threshold * max(|m|, kRelativeEpsilon); `in` otherwise. The GPU
     * kernels compute the same in float.
     **/
    static float applyThreshold(float in, float m, float threshold) WARN_UNUSED_RETURN;

    /**
     * @brief The floor of |median| in the relative test, which keeps it meaningful on black.
     **/
    static constexpr float kRelativeEpsilon = 1e-6f;

    /**
     * @brief The largest radius the GPU kernels take; a larger one renders on the CPU.
     **/
    static constexpr int kMedianGpuMaxRadius = 16;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_Median_h
