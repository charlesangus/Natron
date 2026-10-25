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

#ifndef Engine_Nodes_Filter_EdgeExtend_h
#define Engine_Nodes_Filter_EdgeExtend_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"

#define PLUGINID_NATRON_EDGEEXTEND "EdgeExtend"
#define PLUGIN_MAJOR_NATRON_EDGEEXTEND 1

#define kEdgeExtendParamSize "size"
#define kEdgeExtendParamSlices "slices"
#define kEdgeExtendParamPremultiplyInput "premultiplyInput"
#define kEdgeExtendParamUnpremultiplyOutput "unpremultiplyOutput"

NATRON_NAMESPACE_ENTER

/**
 * @brief The knob values one EdgeExtend evaluation reads. sizeX and sizeY are the size knob's,
 * in canonical units, scaled to pixels at the render scale and divided by the pixel aspect ratio
 * as Blur's are.
 **/
struct EdgeExtendParams {
    double sizeX;
    double sizeY;
    int slices;
    bool premultiplyInput;
    bool unpremultiplyOutput;

    EdgeExtendParams()
        : sizeX(10.)
        , sizeY(10.)
        , slices(5)
        , premultiplyInput(true)
        , unpremultiplyOutput(true)
    {
    }
};

inline constexpr InputSpec kEdgeExtendInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

/**
 * @brief EdgeExtend: smears the colour of a matte's edge outwards, into where the matte is clear.
 *
 * With premultiplyInput the source's colour is first multiplied by its alpha. Then each of the
 * `slices` slices blurs the running result, premultiplied colour and alpha alike, and merges the
 * result over its blur (a + b * (1 - alpha a)), the blur growing linearly from slice to slice:
 * slice i of n has i times the size size / sqrt(n (n + 1) (n + 0.5) / 3), so that the slices'
 * blurs add up to one of `size`. The colour so spreads past the edge while the pixels the matte
 * covers keep theirs: an opaque pixel is unchanged. With unpremultiplyOutput the colour is then
 * divided by the grown alpha where that exceeds kAlphaEpsilon, and the alpha is the source's, so
 * the output is the extended colour under the original matte; without it the output is the
 * premultiplied colour with the grown alpha.
 *
 * The blurs are FIR Gaussians (Blur's "Gaussian (FIR)") with the nearest pixel repeated outside
 * the image, so the region of definition is the source's, and nothing spreads past the reach of
 * the slices' kernels. The node works on the four channels of a pixel at once; with fewer of them
 * processed, there is no matte to extend from, and the processed channels are passed through, as
 * the OpenFX CImg EdgeExtend does. Nothing is clamped.
 **/
struct EdgeExtendOp {
    using Params = EdgeExtendParams;

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_EDGEEXTEND,
        .label = "EdgeExtend",
        .description = "Extend the colour at the edges of a matte outwards, into the area the matte leaves clear. This fills the transparent areas of a keyed plate with the colour of the nearest edge, so that later filtering and despill do not pull in black.\n"
                       "\n"
                       "The colour, premultiplied by the alpha, is blurred and the result merged over its blur, several times (the slices) with a growing blur. Size is the total size of the extension, and more slices keep the colour sharper near the original edge. Opaque pixels are unchanged.\n"
                       "\n"
                       "Premultiply Input multiplies the colour by the alpha first; leave it off for an input that is already premultiplied. Unpremultiply Output divides the extended colour by the extended alpha and outputs the source's alpha, so the output is the extended colour under the original matte; without it, the output is premultiplied, with the extended alpha.\n"
                       "\n"
                       "The node needs the red, green, blue and alpha channels: with fewer of them processed, it passes the input through.",
        .grouping = PLUGIN_GROUP_FILTER,
        .majorVersion = PLUGIN_MAJOR_NATRON_EDGEEXTEND,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/EdgeExtend.png",
        .inputs = kEdgeExtendInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::MaskMix, Stage::PassThroughUnprocessed },
        .layers = LayerPolicy::color(true, true, true, true),
        .op = OpKind::Passes,
    };

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page);

    Params bake(const KnobReader& reader) const;

    /**
     * @brief When every slice's blur leaves every line unchanged at the context's scale: with
     * nothing to extend, the node is off.
     **/
    bool isIdentity(const Params& params,
                    const PassContext& context) const;

    /**
     * @brief The premultiplication, then per slice a copy, the horizontal and the vertical blur
     * and the merge, then the unpremultiplication. The GPU recorder is left out where a slice's
     * radius exceeds the GPU blur's.
     **/
    PassList passes(const Params& params,
                    const PassContext& context) const;

    KnobDoubleWPtr size;
    KnobIntWPtr slices;
    KnobBoolWPtr premultiplyInput;
    KnobBoolWPtr unpremultiplyOutput;
};

static_assert(PassesOp<EdgeExtendOp>);

class EdgeExtend
    : public ImageNode<EdgeExtendOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new EdgeExtend(node);
    }

    explicit EdgeExtend(NodePtr node);

    virtual ~EdgeExtend();

    /**
     * @brief The pixels needed to extend `rect` (pixel coordinates at `scale`) with params on a
     * square-pixel source, before clipping to the source's region of definition: rect grown by
     * the sum of the slices' radii.
     **/
    static RectI getSourceRoI(const RectI& rect, const RenderScale& scale, const EdgeExtendParams& params) WARN_UNUSED_RETURN;

    /**
     * @brief The standard deviation, in pixels, of each slice's blur along one axis whose total
     * size (diameter) is `size` pixels: slice i of n has i * size / (2.4 * sqrt(n (n + 1) (n +
     * 0.5) / 3)), so that their variances add up to that of size / 2.4.
     **/
    static std::vector<double> sliceSigmas(double size, int slices) WARN_UNUSED_RETURN;

    /**
     * @brief The alpha at or below which unpremultiplying leaves the colour as it is.
     **/
    static constexpr float kAlphaEpsilon = 1e-6f;

    static const int kMaxSlices = 50;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_EdgeExtend_h
