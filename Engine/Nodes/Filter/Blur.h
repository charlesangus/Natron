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

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Filter/BlurKernels.h"
#include "Engine/Nodes/Image/NativeImageEffect.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/RectI.h"

#define PLUGINID_NATRON_BLUR "net.sf.cimg.CImgBlur"
#define PLUGIN_MAJOR_NATRON_BLUR 5

#define kBlurParamSize "size"
#define kBlurParamUniform "uniform"
#define kBlurParamOrderX "orderX"
#define kBlurParamOrderY "orderY"
#define kBlurParamBoundary "boundary"
#define kBlurParamBoundaryBlack "black"
#define kBlurParamBoundaryNearest "nearest"
#define kBlurParamFilter "filter"
#define kBlurParamFilterQuasiGaussian "quasigaussian"
#define kBlurParamFilterGaussian "gaussian"
#define kBlurParamFilterBox "box"
#define kBlurParamFilterTriangle "triangle"
#define kBlurParamFilterQuadratic "quadratic"
#define kBlurParamExpandRoD "expandRoD"
#define kBlurParamCropToFormat "cropToFormat"
#define kBlurParamAlphaThreshold "alphaThreshold"

NATRON_NAMESPACE_ENTER

/**
 * @brief The knob values one Blur render, region of definition or region of interest reads, at
 * render scale 1: sizeX is already divided by the source's pixel aspect ratio and sizeY equals
 * sizeX's knob value when uniform is on, as the openfx-misc CImgBlurPlugin::getValuesAtTime()
 * computes them.
 **/
struct BlurParams {
    double sizeX;
    double sizeY;
    int orderX;
    int orderY;
    BlurKernels::Filter filter;
    bool neumann;
    bool expandRoD;
    bool cropToFormat;
    double alphaThreshold;

    BlurParams()
        : sizeX(0.)
        , sizeY(0.)
        , orderX(0)
        , orderY(0)
        , filter(BlurKernels::eFilterGaussian)
        , neumann(false)
        , expandRoD(true)
        , cropToFormat(true)
        , alphaThreshold(0.)
    {
    }
};

/**
 * @brief Blur: a separable quasi-Gaussian (Deriche), Gaussian (Young / van Vliet), box, triangle
 * or quadratic blur, optionally a derivative of order up to 2 per axis, with black or nearest
 * boundary conditions. Knob names, defaults, region of definition (expandRoD, cropToFormat),
 * region of interest, identity rule and arithmetic are the openfx-misc CImgBlur's (its
 * CImgFilterPluginHelper base included); it registers under that plug-in's ID one major above.
 *
 * Each render filters one float buffer covering the render window plus the filter's halo,
 * clipped to the output's region of definition, exactly as the OpenFX plug-in does: the IIR
 * filters' result depends on the extent of the lines they run over, so that extent must not
 * depend on how the work is shared. The buffer is therefore never split by the host (FullySafe),
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

protected:
    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool isIdentityOp(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief Hides uniform while it is off and not animated, as the OpenFX plug-in does under
     * Natron, whose own dimension folding already gives a uniform size.
     **/
    void updateUniformVisibility();

    KnobDoubleWPtr _size;
    KnobBoolWPtr _uniform;
    KnobIntWPtr _orderX;
    KnobIntWPtr _orderY;
    KnobChoiceWPtr _boundary;
    KnobChoiceWPtr _filter;
    KnobBoolWPtr _expandRoD;
    KnobBoolWPtr _cropToFormat;
    KnobDoubleWPtr _alphaThreshold;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_Blur_h
