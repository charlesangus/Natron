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
#include "Engine/Nodes/Image/NativeImageEffect.h"
#include "Engine/ParallelRenderArgs.h"
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
 * @brief The knob values one EdgeDetect render, region of definition or region of interest
 * reads, at render scale 1, as the openfx-misc CImgBlurPlugin::getValuesAtTime() computes them
 * for EdgeDetect: sizeX is blurSize divided by the source's pixel aspect ratio (par, 0 when the
 * source is disconnected), sizeY is blurSize.
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
 * full-frame render shows there. As in Blur, the filters run over one buffer covering the
 * window plus that halo, clipped to the output's region of definition; the node is FullySafe and
 * parallelises internally over whole rows and whole columns, giving the same pixels at any
 * thread count.
 **/
class EdgeDetect
    : public NativeImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new EdgeDetect(node);
    }

    explicit EdgeDetect(NodePtr node);

    virtual ~EdgeDetect();

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

    void getParams(double time, ViewIdx view, EdgeDetectParams* params) const;

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

    KnobChoiceWPtr _filter;
    KnobChoiceWPtr _multiChannel;
    KnobDoubleWPtr _blurSize;
    KnobDoubleWPtr _erodeSize;
    KnobBoolWPtr _nms;
    KnobBoolWPtr _expandRoD;
    KnobBoolWPtr _cropToFormat;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_EdgeDetect_h
