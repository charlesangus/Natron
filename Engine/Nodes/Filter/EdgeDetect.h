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

/*
 * The finite-difference gradient schemes in this node follow CImg's CImg<float>::get_gradient()
 * (CImg 2.9.9, as bundled with openfx-misc), by David Tschumperle and contributors,
 * <http://cimg.eu>. CImg is distributed under the CeCILL-C licence, which is compatible with the
 * GNU GPL.
 */

#ifndef Engine_Nodes_Filter_EdgeDetect_h
#define Engine_Nodes_Filter_EdgeDetect_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Filter/BlurKernels.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/RectI.h"

#define PLUGINID_NATRON_EDGEDETECT "eu.cimg.EdgeDetect"
#define PLUGIN_MAJOR_NATRON_EDGEDETECT 5

#define kEdgeDetectParamFilter "filter"
#define kEdgeDetectParamFilterSimple "simple"
#define kEdgeDetectParamFilterSobel "sobel"
#define kEdgeDetectParamFilterRotationInvariant "rotinvariant"
#define kEdgeDetectParamFilterQuasiGaussian "quasigaussian"
#define kEdgeDetectParamFilterGaussian "gaussian"
#define kEdgeDetectParamFilterBox "box"
#define kEdgeDetectParamFilterTriangle "triangle"
#define kEdgeDetectParamFilterQuadratic "quadratic"
#define kEdgeDetectParamMultiChannel "multiChannel"
#define kEdgeDetectParamMultiChannelSeparate "separate"
#define kEdgeDetectParamMultiChannelRMS "rms"
#define kEdgeDetectParamMultiChannelMax "max"
#define kEdgeDetectParamMultiChannelTensor "tensor"
#define kEdgeDetectParamBlurSize "blurSize"
#define kEdgeDetectParamErodeSize "erodeSize"
#define kEdgeDetectParamNMS "nms"
#define kEdgeDetectParamExpandRoD "expandRoD"
#define kEdgeDetectParamCropToFormat "cropToFormat"

NATRON_NAMESPACE_ENTER

/// The order of EdgeDetect's `filter` choice.
enum EdgeDetectFilterEnum {
    eEdgeDetectFilterSimple = 0,
    eEdgeDetectFilterSobel,
    eEdgeDetectFilterRotationInvariant,
    eEdgeDetectFilterQuasiGaussian,
    eEdgeDetectFilterGaussian,
    eEdgeDetectFilterBox,
    eEdgeDetectFilterTriangle,
    eEdgeDetectFilterQuadratic
};

/// The order of EdgeDetect's `multiChannel` choice.
enum EdgeDetectMultiChannelEnum {
    eEdgeDetectMultiChannelSeparate = 0,
    eEdgeDetectMultiChannelRMS,
    eEdgeDetectMultiChannelMax,
    eEdgeDetectMultiChannelTensor
};

/**
 * @brief The knob values one EdgeDetect evaluation reads, at render scale 1. sizeX and sizeY
 * are blurSize; the op divides sizeX by the source's pixel aspect ratio, as the openfx-misc
 * CImgBlurPlugin::getValuesAtTime() does for EdgeDetect, so params handed to getSourceRoI()
 * already carry sizeX divided by par. par is the source's pixel aspect ratio, which also scales
 * the erosion radius along x.
 **/
struct EdgeDetectParams {
    double sizeX;
    double sizeY;
    double par;
    double erodeSize;
    EdgeDetectFilterEnum filter;
    EdgeDetectMultiChannelEnum multiChannel;
    bool nms;
    bool expandRoD;
    bool cropToFormat;

    EdgeDetectParams()
        : sizeX(0.)
        , sizeY(0.)
        , par(1.)
        , erodeSize(0.)
        , filter(eEdgeDetectFilterGaussian)
        , multiChannel(eEdgeDetectMultiChannelTensor)
        , nms(false)
        , expandRoD(true)
        , cropToFormat(true)
    {
    }
};

inline constexpr InputSpec kEdgeDetectInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

/**
 * @brief EdgeDetect: the image gradient magnitude, from first-order derivative blurs
 * (quasi-Gaussian, Gaussian, box, triangle, quadratic) or from simple, Sobel or
 * rotation-invariant finite differences after a Gaussian pre-blur, with the per-channel
 * gradients combined separately, by RMS, by maximum or by the Di Zenzo tensor norm, then
 * optionally eroded (or dilated, for a negative erode size) and thinned by non-maxima
 * suppression. Knob names, defaults, region of definition and arithmetic are the openfx-misc
 * EdgeDetect's (a CImgBlur plug-in); it registers under that plug-in's ID one major above.
 *
 * The source region of interest adds the erosion radius and the non-maxima suppression's one
 * pixel to the gradient filter's halo, which the OpenFX plug-in omits, so a tile renders what a
 * full-frame render shows there. As in Blur, the passes run over one buffer covering the window
 * plus that halo, clipped to the output's region of definition, each over whole rows or whole
 * columns, giving the same pixels at any thread count. It runs on the CPU only.
 **/
struct EdgeDetectOp {
    using Params = EdgeDetectParams;

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_EDGEDETECT,
        .label = "EdgeDetect",
        .description = "Perform edge detection by computing the image gradient magnitude. Optionally, edge detection can be preceded by blurring, and followed by erosion and thresholding. In most cases, EdgeDetect is followed a Grade node to extract the proper edges and generate a mask from these.\n"
                       "\n"
                       "For color or multi-channel images, several edge detection algorithms are proposed to combine the gradients computed in each channel:\n"
                       "- Separate: the gradient magnitude is computed in each channel separately, and the output is a color edge image.\n"
                       "- RMS: the RMS of per-channel gradients magnitudes is computed.\n"
                       "- Max: the maximum per-channel gradient magnitude is computed.\n"
                       "- Tensor: the tensor gradient norm [1].\n"
                       "\n"
                       "References:\n"
                       "- [1] Silvano Di Zenzo, A note on the gradient of a multi-image, CVGIP 33, 116-125 (1986). http://people.csail.mit.edu/tieu/notebook/imageproc/dizenzo86.pdf\n"
                       "\n"
                       "The filters are ports of functions of the CImg library (http://cimg.eu), "
                       "distributed under the CeCILL-C licence.",
        .grouping = PLUGIN_GROUP_FILTER,
        .majorVersion = PLUGIN_MAJOR_NATRON_EDGEDETECT,
        .minorVersion = 0,
        .inputs = kEdgeDetectInputs,
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
     * @brief Never: the gradient of any image is a new image.
     **/
    bool isIdentity(const Params& params,
                    const PassContext& context) const;

    /**
     * @brief The passes of one render: the gradient components, their normalisation and channel
     * combination, then the erosion and the non-maxima suppression when asked for. The CPU runs
     * them; no GPU recorder is given.
     **/
    PassList passes(const Params& params,
                    const PassContext& context) const;

    KnobChoiceWPtr filter;
    KnobChoiceWPtr multiChannel;
    KnobDoubleWPtr blurSize;
    KnobDoubleWPtr erodeSize;
    KnobBoolWPtr nms;
    KnobBoolWPtr expandRoD;
    KnobBoolWPtr cropToFormat;
};

static_assert(PassesOp<EdgeDetectOp>);

class EdgeDetect
    : public ImageNode<EdgeDetectOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new EdgeDetect(node);
    }

    explicit EdgeDetect(NodePtr node);

    virtual ~EdgeDetect();

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    /// The CImgBlur filter that `filter` blurs with: Gaussian for the finite-difference schemes.
    static BlurKernels::Filter blurFilter(EdgeDetectFilterEnum filter) WARN_UNUSED_RETURN;

    /// The erosion (or dilation) radius in pixels along each axis at `scale`.
    static void getErodeRadius(const RenderScale& scale, const EdgeDetectParams& params, int* rx, int* ry);

    /**
     * @brief The pixels needed to render `rect` (pixel coordinates at `scale`), before clipping
     * to the output's region of definition: the gradient filter's halo (CImgBlurPlugin::getRoI()
     * with first-order derivatives), plus the erosion radius, plus one pixel with non-maxima
     * suppression.
     **/
    static RectI getSourceRoI(const RectI& rect, const RenderScale& scale, const EdgeDetectParams& params) WARN_UNUSED_RETURN;

protected:
    /**
     * @brief With cropToFormat, clamps back to the format each border of rod the source had
     * inside it: CImgBlurPlugin::getRegionOfDefinition().
     **/
    virtual bool clipRegionOfDefinition(double time,
                                        const RenderScale& scale,
                                        ViewIdx view,
                                        const EffectInstancePtr& source,
                                        const RectI& srcRoD,
                                        RectI* rod) OVERRIDE FINAL;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_EdgeDetect_h
