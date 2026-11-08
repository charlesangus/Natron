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

#ifndef Engine_Nodes_Filter_GodRays_h
#define Engine_Nodes_Filter_GodRays_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Filter/DirBlur.h"
#include "Engine/Nodes/Image/FilterKnobs.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Image/TransformMath.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

#define PLUGINID_NATRON_GODRAYS "GodRays"
#define PLUGIN_MAJOR_NATRON_GODRAYS 1

#define kGodRaysParamTranslate "translate"
#define kGodRaysParamRotate "rotate"
#define kGodRaysParamScale "scale"
#define kGodRaysParamUniform "uniform"
#define kGodRaysParamSkewX "skewX"
#define kGodRaysParamSkewY "skewY"
#define kGodRaysParamSkewOrder "skewOrder"
#define kGodRaysParamCenter "center"
#define kGodRaysParamFromColor "fromColor"
#define kGodRaysParamToColor "toColor"
#define kGodRaysParamGamma "gamma"
#define kGodRaysParamSamples "samples"
#define kGodRaysParamMax "max"

#define kGodRaysInputSource 0
#define kGodRaysInputMask 1

NATRON_NAMESPACE_ENTER

struct GodRaysParams {
    // The transform the last copy takes, about its centre; the copies between take a share of it.
    TransformMath::TransformParams transform;
    double fromColor[4] = { 1., 1., 1., 1. };
    double toColor[4] = { 1., 1., 1., 1. };
    double gamma = 1.;
    int samples = 32;
    bool max = false;
    // How every copy is resampled, as the Transform node's Filter, Clamp and Black outside.
    Resampler::FilterSpec filter;
    bool clamp = false;
    bool blackOutside = true;
};

/**
 * @brief Light rays: copies of the image transformed by even steps from none of the transform to
 * all of it, tinted from From Color to To Color along the way, averaged, or their maximum with
 * Max. The copies are DirBlur's samples and render through DirBlur's kernel.
 **/
class GodRays
    : public StagedImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new GodRays(node);
    }

    explicit GodRays(NodePtr node);

    virtual ~GodRays();

    /// The most samples the Samples knob takes: as many as DirBlur, which the GPU block fits.
    static constexpr int kMaxSamples = DirBlur::kMaxSamples;

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual std::string getInputHint(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

    GodRaysParams getParams(double time, ViewIdx view) const WARN_UNUSED_RETURN;

    /// Whether params leave the image unchanged: no transform, white tints and no Max.
    static bool isIdentity(const GodRaysParams& params) WARN_UNUSED_RETURN;

    /**
     * @brief The tint of channel c at fraction a of the rays, in [0, 1]: From Color at 0, To
     * Color at 1, linearly in a^(1/Gamma).
     **/
    static double tintAt(const GodRaysParams& params, int c, double a) WARN_UNUSED_RETURN;

    /// The copies' fractions of the transform: Samples even steps from 0 to 1, both included.
    static std::vector<double> samplePositions(const GodRaysParams& params) WARN_UNUSED_RETURN;

    /// The canonical copies of params, in DirBlur's layout.
    static std::vector<DirBlurSample> canonicalSamples(const GodRaysParams& params) WARN_UNUSED_RETURN;

    virtual bool canRenderOpOnGpu(const GpuQuery& query) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOpGpu(const GpuRenderArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool getOpGpuInputLayer(int inputNb,
                                    const ImageLayerDesc& plane,
                                    double time,
                                    ViewIdx view,
                                    ImageLayerDesc* layer) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual RectI getOpGpuInputRoI(int inputNb,
                                   double time,
                                   ViewIdx view,
                                   const RectI& outputRect,
                                   const RenderScale& scale) const OVERRIDE FINAL WARN_UNUSED_RETURN;

protected:
    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;


    virtual bool isOpIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& plan) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // The source's region of definition at time, view and scale; null when it has none.
    RectD sourceRoD(double time, ViewIdx view, const RenderScale& scale) const WARN_UNUSED_RETURN;

    double outputPar(double time, ViewIdx view) const WARN_UNUSED_RETURN;

    KnobDoubleWPtr _translate;
    KnobDoubleWPtr _rotate;
    KnobDoubleWPtr _scale;
    KnobBoolWPtr _uniform;
    KnobDoubleWPtr _skewX;
    KnobDoubleWPtr _skewY;
    KnobChoiceWPtr _skewOrder;
    KnobDoubleWPtr _center;
    KnobColorWPtr _fromColor;
    KnobColorWPtr _toColor;
    KnobDoubleWPtr _gamma;
    KnobIntWPtr _samples;
    KnobBoolWPtr _max;
    KnobChoiceWPtr _filter;
    KnobBoolWPtr _clamp;
    KnobBoolWPtr _blackOutside;
    Resampler::FilterKnobs _filterKnobs;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_GodRays_h
