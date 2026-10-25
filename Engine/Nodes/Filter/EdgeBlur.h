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

#ifndef Engine_Nodes_Filter_EdgeBlur_h
#define Engine_Nodes_Filter_EdgeBlur_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Filter/EdgeDetect.h"
#include "Engine/Nodes/Image/PassExecutor.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/RectI.h"

#define PLUGINID_NATRON_EDGEBLUR "EdgeBlur"
#define PLUGIN_MAJOR_NATRON_EDGEBLUR 1

#define kEdgeBlurParamExternalMatte "externalMatte"
#define kEdgeBlurParamSize "size"
#define kEdgeBlurParamFilter "filter"
#define kEdgeBlurParamCropToFormat "cropToFormat"
#define kEdgeBlurParamEdgeMult "edgeMult"

#define kEdgeBlurInputSource 0
#define kEdgeBlurInputMask 1
#define kEdgeBlurInputMatte 2

#define kEdgeBlurInputMatteLabel "Matte"

NATRON_NAMESPACE_ENTER

class MapPlane;

/**
 * @brief The knob values one EdgeBlur evaluation reads. size is in canonical units, the blur's
 * and the edge detection's alike; filter is the edge detection's only, the blur being always the
 * Gaussian (IIR).
 **/
struct EdgeBlurParams {
    bool externalMatte = false;
    double size = 3.;
    EdgeDetectFilterEnum filter = eEdgeDetectFilterGaussian;
    bool cropToFormat = true;
    double edgeMult = 2.;
};

/**
 * @brief Blurs the image where its matte has edges: the image blurred by a Gaussian of Size, mixed
 * with the image by the matte's edge strength raised to 1 / Edge Mult and clamped to [0, 1]. The
 * matte is the alpha of the plane being processed, or with External Matte the alpha of the Matte
 * input in that plane; a Matte that is disconnected has no edges, which leaves the image as it is.
 *
 * The edges are EdgeDetect's gradient magnitude of the matte alone, pre-blurred by Size with the
 * Filter, as EdgeDetect computes it over the matte's own region of definition grown by its reach
 * and cropped to the matte's format: so a unit step of the matte gives an edge strength of 1 at
 * the step. The CPU runs EdgeDetect's passes over a buffer of their own and Blur's in the pass
 * list, whose last pass mixes; the GPU records both nodes' recorders and Kernels/edgeblur.slang's
 * mix over the same rects.
 **/
class EdgeBlur
    : public StagedImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new EdgeBlur(node);
    }

    explicit EdgeBlur(NodePtr node);

    virtual ~EdgeBlur();

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual std::string getInputHint(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    EdgeBlurParams getParams(double time, ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief The blur of params at context's scale: Blur's passes with the Gaussian (IIR), empty
     * when the blur leaves the image unchanged.
     **/
    static PassList blurPasses(const EdgeBlurParams& params, const PassContext& context) WARN_UNUSED_RETURN;

    /**
     * @brief The edge detection of params at context's scale: EdgeDetect's passes, Separate, with
     * no erosion or suppression.
     **/
    static PassList edgePasses(const EdgeBlurParams& params, const PassContext& context) WARN_UNUSED_RETURN;

    /**
     * @brief The weight a pixel of edge strength edge mixes the blur in with: edge to the power
     * exponent, 1 / Edge Mult, clamped to [0, 1]; 0 for an edge that is not positive.
     **/
    static float edgeWeight(float edge, float exponent) WARN_UNUSED_RETURN;

    /**
     * @brief The exponent edgeWeight() takes for edgeMult.
     **/
    static float edgeExponent(double edgeMult) WARN_UNUSED_RETURN;

    /**
     * @brief The Matte is read only with External Matte on.
     **/
    virtual bool readsGpuInput(int inputNb,
                               const ImageLayerDesc& plane,
                               double time,
                               ViewIdx view) const OVERRIDE FINAL WARN_UNUSED_RETURN;

protected:
    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE FINAL;

    /**
     * @brief Blur's crop to format: only the borders the source had inside its format are clamped
     * back to it.
     **/
    virtual bool clipRegionOfDefinition(double time,
                                        const RenderScale& scale,
                                        ViewIdx view,
                                        const EffectInstancePtr& source,
                                        const RectI& srcRoD,
                                        RectI* rod) OVERRIDE FINAL;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;

    // An identity when the blur leaves the image unchanged, or when External Matte reads a Matte
    // that is not connected.
    virtual bool isOpIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& plan) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // The blur and the mix, without CPU work, with the reach of the edge detection when the matte
    // is the source; with a GPU recorder while both nodes' passes have one.
    virtual PassList opPasses(double time,
                              ViewIdx view,
                              const RenderScale& scale) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    // The Matte over the render window grown by the edge detection's reach.
    virtual RectI opPassesInputRoI(int inputNb,
                                   const RectI& outputRect,
                                   const RenderScale& scale) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    // The input the matte is read from: the Matte with External Matte, else the source.
    int matteInput(const EdgeBlurParams& params) const WARN_UNUSED_RETURN;

    /**
     * @brief The rect EdgeDetect's buffer covers for output: output grown by the edge passes'
     * reach, clipped to the matte's region of definition grown by it and cropped to the matte's
     * format, which *edgeRoD receives. False when the matte has no region of definition.
     **/
    bool edgeRects(const EdgeBlurParams& params,
                   const PassList& edges,
                   double time,
                   ViewIdx view,
                   const RenderScale& scale,
                   const RectI& output,
                   RectI* matteRoD,
                   RectI* edgeRoD,
                   RectI* buffer) const WARN_UNUSED_RETURN;

    // The edge strength over args.roi, 0 where there is none.
    StatusEnum computeEdges(const RenderActionArgs& args,
                            const EdgeBlurParams& params,
                            const PassList& edges,
                            MapPlane* plane);

    // The GPU recorder of opPasses().
    StatusEnum recordGpu(const PassGpuIO& io,
                         double time,
                         ViewIdx view,
                         const RenderScale& scale,
                         const EdgeBlurParams& params,
                         const PassList& blur,
                         const PassList& edges) const WARN_UNUSED_RETURN;

    KnobBoolWPtr _externalMatte;
    KnobDoubleWPtr _size;
    KnobChoiceWPtr _filter;
    KnobBoolWPtr _cropToFormat;
    KnobDoubleWPtr _edgeMult;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_EdgeBlur_h
