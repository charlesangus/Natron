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
#include "Engine/Nodes/Image/NativeImageEffect.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/RectI.h"

#define PLUGINID_NATRON_BLUR "net.sf.cimg.CImgBlur"
#define PLUGIN_MAJOR_NATRON_BLUR 5
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
 * @brief The knob values one Blur render, region of definition or region of interest reads, at
 * render scale 1: sizeX is already divided by the source's pixel aspect ratio, as the openfx-misc
 * CImgBlurPlugin::getValuesAtTime() computes it.
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

/**
 * @brief Blur: a separable FIR Gaussian (the default), quasi-Gaussian (Deriche), IIR Gaussian
 * (Young / van Vliet), box, triangle or quadratic blur, black outside the image, with the region of
 * definition always grown by the filter's reach (and cropped to the format on request).
 * Apart from the FIR Gaussian and the Filter default, the region of definition, region of
 * interest, identity rule and arithmetic are the openfx-misc CImgBlur's (its
 * CImgFilterPluginHelper base included); it registers under that plug-in's ID one major above.
 *
 * Each render filters one float buffer covering the render window plus the filter's halo,
 * clipped to the output's region of definition, whatever the mask. The IIR filters' result
 * depends on the extent of the lines they run over, so that extent must not depend on how the
 * work is shared. The buffer is therefore never split by the host (FullySafe),
 * and the horizontal pass runs over whole rows and the vertical pass over whole columns on the
 * global pool, which gives the same pixels at any thread count.
 *
 * The "(Un)premult by" division, mask and mix and the unprocessed-channel pass-through follow
 * NativeImageEffect's pipeline: the source is divided before it is blurred, and the blurred
 * value is multiplied back and mixed with the undivided source.
 **/
class Blur
    : public NativeImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new Blur(node);
    }

    explicit Blur(NodePtr node);

    virtual ~Blur();

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

    void getParams(double time, ViewIdx view, BlurParams* params) const;

    /**
     * @brief The pixels needed to blur `rect` (pixel coordinates at `scale`), before clipping
     * to the output's region of definition: CImgBlurPlugin::getRoI().
     **/
    static RectI getSourceRoI(const RectI& rect, const RenderScale& scale, const BlurParams& params) WARN_UNUSED_RETURN;

    /**
     * @brief Whether params leave the image unchanged at scale: CImgBlurPlugin::isIdentity().
     **/
    static bool paramsAreIdentity(const RenderScale& scale, const BlurParams& params) WARN_UNUSED_RETURN;

    /**
     * @brief sigma = size / 2.4 of the larger axis at the mipmap level, and whether the filter is
     * one of the IIR Gaussians, whatever the filter.
     **/
    virtual std::optional<GpuCostHint> gpuCostHint(double time, ViewIdx view, unsigned int mipmapLevel) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief Every filter, on float images of 1 to 4 components, with any
     * "(Un)premult by" channel in the rendered plane; the FIR Gaussian also needs a radius the GPU
     * kernel's tile holds.
     **/
    virtual bool canRenderOnGpu(const GpuQuery& query) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief getSourceRoI() at the current time and view.
     **/
    virtual RectI getGpuInputRoI(int inputNb, const RectI& outputRect, const RenderScale& scale) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The pixels render() gives for the same query: the filtered extent is the source
     * region of interest of the window clipped to this effect's region of definition, and the
     * source is resolved against the input's region of definition.
     **/
    virtual StatusEnum renderGpu(const GpuRenderArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

protected:
    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE FINAL;

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool isIdentityOp(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    KnobDoubleWPtr _size;
    KnobChoiceWPtr _filter;
    KnobBoolWPtr _cropToFormat;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_Blur_h
