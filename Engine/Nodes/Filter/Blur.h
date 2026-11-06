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

#ifndef Engine_Nodes_Filter_Blur_h
#define Engine_Nodes_Filter_Blur_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <optional>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Filter/BlurKernels.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/RectI.h"

#define PLUGINID_NATRON_BLUR "Blur"
#define PLUGIN_MAJOR_NATRON_BLUR 1
#define PLUGIN_MINOR_NATRON_BLUR 1

#define kBlurParamSize "size"
#define kBlurParamFilter "filter"
#define kBlurParamFilterQuasiGaussian "quasigaussian"
#define kBlurParamFilterGaussian "gaussian"
#define kBlurParamFilterBox "box"
#define kBlurParamFilterTriangle "triangle"
#define kBlurParamFilterQuadratic "quadratic"
#define kBlurParamFilterFIRGaussian "firgaussian"
#define kBlurParamCropToFormat "cropToFormat"

NATRON_NAMESPACE_ENTER

/**
 * @brief The knob values one Blur evaluation reads. sizeX and sizeY are the size knob's, in
 * canonical units; the op divides sizeX by the source's pixel aspect ratio and scales both to the
 * render scale, as the openfx-misc CImgBlurPlugin::getValuesAtTime() and its callers do.
 **/
struct BlurParams {
    double sizeX;
    double sizeY;
    BlurKernels::Filter filter;
    bool cropToFormat;

    BlurParams()
        : sizeX(0.)
        , sizeY(0.)
        , filter(BlurKernels::eFilterGaussian)
        , cropToFormat(true)
    {
    }
};

inline constexpr InputSpec kBlurInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

/**
 * @brief Blur: a separable FIR Gaussian (the default), quasi-Gaussian (Deriche), IIR Gaussian
 * (Young / van Vliet), box, triangle or quadratic blur, black outside the image, with the region of
 * definition always grown by the filter's reach (and cropped to the format on request).
 * Apart from the FIR Gaussian and the Filter default, the region of definition, region of
 * interest, identity rule and arithmetic are the openfx-misc CImgBlur's (its
 * CImgFilterPluginHelper base included); it registers under that plug-in's ID one major above.
 *
 * Its passes are a horizontal then a vertical line filter over one buffer covering the render
 * window plus the filter's halo, clipped to the output's region of definition, whatever the mask.
 * The IIR filters' result depends on the extent of the lines they run over, so the executor runs
 * each pass over whole lines, which gives the same pixels at any thread count.
 **/
struct BlurOp {
    using Params = BlurParams;

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_BLUR,
        .label = "Blur",
        .description = "Blur the input stream.\n"
                       "The blur filter can be a Gaussian (FIR), a quasi-Gaussian or a Gaussian (both IIR), a box, a triangle or a quadratic filter.\n"
                       "\n"
                       "The default filter, Gaussian (FIR), is a Gaussian [1] implemented as a FIR (finite impulse response) filter, as in most compositing software: the Gaussian impulse response, of standard deviation sigma = size/2.4, is cropped at 3 sigma (rounded up to a whole pixel) and normalised. Its result is exactly zero further than that from any non-zero pixel.\n"
                       "\n"
                       "The Gaussian filter is implemented as an IIR (infinite impulse response) filter [2][3]. Consequently, when blurring a white dot on black background, it produces very small values very far away from the dot. The quasi-Gaussian filter is also IIR.\n"
                       "\n"
                       "A very common process in compositing to expand colors on the edge of a matte is to use the premult-blur-unpremult combination [4][5]. The very small values produced by the IIR filters produce undesirable artifacts after unpremult. For this process, the FIR filters (Gaussian (FIR), quadratic, or the faster triangle or box) should be preferred over the IIR ones.\n"
                       "\n"
                       "References:\n"
                       "[1] https://en.wikipedia.org/wiki/Gaussian_filter\n"
                       "[2] I.T. Young, L.J. van Vliet, M. van Ginkel, Recursive Gabor filtering. IEEE Trans. Sig. Proc., vol. 50, pp. 2799-2805, 2002. (this is an improvement over Young-Van Vliet, Sig. Proc. 44, 1995)\n"
                       "[3] B. Triggs and M. Sdika. Boundary conditions for Young-van Vliet recursive filtering. IEEE Trans. Signal Processing, vol. 54, pp. 2365-2367, 2006.\n"
                       "[4] Nuke Expand Edges or how to get rid of outlines. http://franzbrandstaetter.com/?p=452\n"
                       "[5] Colour Smear for Nuke. http://richardfrazer.com/tools-tutorials/colour-smear-for-nuke/\n"
                       "\n"
                       "The IIR, box, triangle and quadratic filters are ports of the 'vanvliet', 'deriche' and 'boxfilter' functions of the CImg library (http://cimg.eu), "
                       "distributed under the CeCILL-C licence.",
        .grouping = PLUGIN_GROUP_FILTER,
        .majorVersion = PLUGIN_MAJOR_NATRON_BLUR,
        .minorVersion = PLUGIN_MINOR_NATRON_BLUR,
        .icon = NATRON_IMAGES_PATH "NativeNodes/Blur.png",
        .inputs = kBlurInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::MaskMix, Stage::PassThroughUnprocessed },
        .layers = LayerPolicy::all(),
        .op = OpKind::Passes,
    };

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page);

    Params bake(const KnobReader& reader) const;

    /**
     * @brief CImgBlurPlugin::isIdentity() at the context's scale.
     **/
    bool isIdentity(const Params& params,
                    const PassContext& context) const;

    /**
     * @brief The horizontal then the vertical line filter, each with CImgBlurPlugin's halo
     * (getRoI()) and spread (getRegionOfDefinition()) along its axis, which differ for the box
     * family. A filter that leaves its lines unchanged only contributes its halo and spread. A
     * Gaussian-family blur at a sigma below 0.1 on both axes, and a FIR Gaussian of radius 0 on
     * both, have no pass. The GPU recorder is left out where the GPU kernels cannot run the list:
     * a FIR Gaussian radius beyond their tile.
     **/
    PassList passes(const Params& params,
                    const PassContext& context) const;

    /**
     * @brief The Filter weight of the larger axis's size at the context's scale for the filters
     * whose cost grows with it; empty for the IIR Gaussians, whose cost does not, and so for the
     * plain Filter weight.
     **/
    std::optional<GpuWeight> gpuWeight(const Params& params,
                                       const PassContext& context) const;

    KnobDoubleWPtr size;
    KnobChoiceWPtr filter;
    KnobBoolWPtr cropToFormat;
};

static_assert(PassesOp<BlurOp>);

class Blur
    : public ImageNode<BlurOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new Blur(node);
    }

    explicit Blur(NodePtr node);

    virtual ~Blur();

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    /**
     * @brief The Params the op renders with at (time, view).
     **/
    void getParams(double time, ViewIdx view, BlurParams* params) const;

    /**
     * @brief The pixels needed to blur `rect` (pixel coordinates at `scale`) with params on a
     * square-pixel source, before clipping to the output's region of definition:
     * CImgBlurPlugin::getRoI().
     **/
    static RectI getSourceRoI(const RectI& rect, const RenderScale& scale, const BlurParams& params) WARN_UNUSED_RETURN;

    /**
     * @brief Whether params leave a square-pixel source unchanged at scale:
     * CImgBlurPlugin::isIdentity().
     **/
    static bool paramsAreIdentity(const RenderScale& scale, const BlurParams& params) WARN_UNUSED_RETURN;

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

#endif // Engine_Nodes_Filter_Blur_h
