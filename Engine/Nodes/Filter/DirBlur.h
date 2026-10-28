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

#ifndef Engine_Nodes_Filter_DirBlur_h
#define Engine_Nodes_Filter_DirBlur_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Image/TransformMath.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

#define PLUGINID_NATRON_DIRBLUR "DirBlur"
#define PLUGIN_MAJOR_NATRON_DIRBLUR 1

#define kDirBlurParamMode "mode"
#define kDirBlurParamModeDirectional "directional"
#define kDirBlurParamModeRadial "radial"
#define kDirBlurParamModeZoom "zoom"
#define kDirBlurParamLength "length"
#define kDirBlurParamCenter "center"
#define kDirBlurParamDirection "direction"
#define kDirBlurParamSamples "samples"
#define kDirBlurParamCentered "centered"
#define kDirBlurParamFalloff "falloff"
#define kDirBlurParamUseTarget "useTarget"
#define kDirBlurParamTarget "target"
#define kDirBlurParamPixelOffset "pixelOffset"
#define kDirBlurParamTranslate "translate"
#define kDirBlurParamRotate "rotate"
#define kDirBlurParamScale "scale"
#define kDirBlurParamUniform "uniform"
#define kDirBlurParamSkewX "skewX"
#define kDirBlurParamSkewY "skewY"
#define kDirBlurParamSkewOrder "skewOrder"
#define kDirBlurParamTransformAmount "transformAmount"
#define kDirBlurParamInvert "invert"
#define kDirBlurParamInteractive "interactive"
#define kDirBlurParamResetCenter "resetCenter"
#define kDirBlurParamFromColor "fromColor"
#define kDirBlurParamToColor "toColor"
#define kDirBlurParamRamp "ramp"
#define kDirBlurParamRampLinear "linear"
#define kDirBlurParamRampExponential "exponential"
#define kDirBlurParamMax "max"
#define kDirBlurParamHoldoutChannel "holdoutChannel"
#define kDirBlurParamTintGroup "tint"
#define kDirBlurParamTransformGroup "transform"
#define kDirBlurParamHoldoutGroup "holdout"

#define kDirBlurInputSource 0
#define kDirBlurInputMask 1
#define kDirBlurInputHoldout 2

NATRON_NAMESPACE_ENTER

enum DirBlurModeEnum {
    eDirBlurModeDirectional = 0,
    eDirBlurModeRadial = 1,
    eDirBlurModeZoom = 2,
};

enum DirBlurRampEnum {
    eDirBlurRampLinear = 0,
    eDirBlurRampExponential = 1,
};

struct DirBlurParams {
    DirBlurModeEnum mode = eDirBlurModeZoom;
    // Canonical pixels for Directional and Zoom, degrees for Radial.
    double length = 20.;
    double centerX = 0.;
    double centerY = 0.;
    // Degrees, counter-clockwise from +x.
    double direction = 45.;
    // Directional only: the direction from the centre to the target replaces direction.
    bool useTarget = false;
    double targetX = 0.;
    double targetY = 0.;
    int samples = 32;
    bool centered = false;
    double falloff = 0.;
    // Zoom only: canonical pixels added to every sample's zoom distance.
    double pixelOffset = 0.;
    // Applied along the blur in every mode, about the centre: a sample at position t takes the
    // transform at amount t.
    TransformMath::TransformParams transform;
    bool invert = false;
    double fromColor[4] = { 1., 1., 1., 1. };
    double toColor[4] = { 1., 1., 1., 1. };
    DirBlurRampEnum ramp = eDirBlurRampLinear;
    bool max = false;
    // How every copy is resampled, as the Transform node's Filter, Clamp and Black outside.
    Resampler::FilterEnum filter = Resampler::eFilterCubic;
    bool clamp = false;
    bool blackOutside = true;
};

/**
 * @brief One sample of the blur: the destination-to-source matrix, canonical or in pixels, the
 * factor each channel's sample is multiplied by, in channel-bit order (R, G, B, A), and its
 * weight. Averaging, the factors are the weight times the tint, and the weights are normalised to
 * sum to one; with Max, the factors are the falloff times the tint, unnormalised.
 **/
struct DirBlurSample {
    TransformMath::Mat3 inverse;
    float factor[4];
    float weight;
};

class DirBlur
    : public StagedImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new DirBlur(node);
    }

    explicit DirBlur(NodePtr node);

    virtual ~DirBlur();

    /// The most samples the Samples knob takes.
    static constexpr int kMaxSamples = 512;

    /// The smallest Zoom scale a negative Length shrinks to.
    static constexpr double kMinZoomScale = 0.01;

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual std::string getInputHint(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

    DirBlurParams getParams(double time, ViewIdx view) const WARN_UNUSED_RETURN;

    /// Whether params move the image at all; when they do not the node is an identity, whatever
    /// the tints, Max or holdout, as God Rays is.
    static bool hasMotion(const DirBlurParams& params) WARN_UNUSED_RETURN;

    /**
     * @brief The tint of the samples at fraction a of the blur, in [0, 1]: from fromColor at 0 to
     * toColor at 1, linearly, or geometrically with Exponential, each colour held to at least
     * kMinExponentialTint.
     **/
    static double tintAt(const DirBlurParams& params, int c, double a) WARN_UNUSED_RETURN;

    /// The smallest tint an Exponential ramp starts or ends at.
    static constexpr double kMinExponentialTint = 0.001;

    /**
     * @brief The layer and the channel of it Holdout Channel selects on the Holdout input; false
     * when the input is disconnected, the knob is None or the layer is absent.
     **/
    bool resolveHoldoutChannel(double time, ViewIdx view, ImageLayerDesc* layer, int* channel) const WARN_UNUSED_RETURN;

    /**
     * @brief The canonical distance a Zoom Length is measured at: half the diagonal of the
     * output format, so the format's corners move by Length.
     **/
    double zoomReference(double time, ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief The samples' positions along the blur, in [0, 1], or [-1, 1] when centered: the
     * midpoints of `samples` equal steps.
     **/
    static std::vector<double> samplePositions(const DirBlurParams& params) WARN_UNUSED_RETURN;

    /**
     * @brief The canonical samples of params, a Zoom measured at zoomReference. Length 0 gives
     * identity matrices.
     **/
    static std::vector<DirBlurSample> canonicalSamples(const DirBlurParams& params,
                                                       double zoomReference) WARN_UNUSED_RETURN;

    /**
     * @brief The samples in pixels at scale for pixel aspect ratio par, as the render samples
     * them: each canonical matrix converted by Resampler::buildSamplingTransforms().
     **/
    static std::vector<DirBlurSample> pixelSamples(const std::vector<DirBlurSample>& canonical,
                                                   double time,
                                                   const RenderScale& scale,
                                                   double par) WARN_UNUSED_RETURN;

    /**
     * @brief The canonical region of the source the samples read for the canonical window,
     * grown by every sample's filter footprint at scale, and clipped to srcRoD unless it is null.
     * Without blackOutside the samples beyond srcRoD read its edge pixels, so the region is
     * clamped into srcRoD instead, keeping at least its nearest row or column of pixels.
     **/
    static RectD sourceRegion(const std::vector<DirBlurSample>& canonical,
                              const RectD& window,
                              const RenderScale& scale,
                              double par,
                              const RectD& srcRoD,
                              bool blackOutside = true) WARN_UNUSED_RETURN;

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

    virtual bool readsGpuInput(int inputNb,
                               const ImageLayerDesc& plane,
                               double time,
                               ViewIdx view) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool getInputLayersRead(int inputNb,
                                    double time,
                                    ViewIdx view,
                                    std::list<ImageLayerDesc>* layers) OVERRIDE FINAL WARN_UNUSED_RETURN;

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

    // Enables Clamp only for the filters that honour it.
    void updateClampEnabled(double time, ViewIdx view);

    // Moves the centre to the middle of the source's region of definition, or of the project
    // format without one, changing Translate so that the transform's full amount keeps its place.
    void resetCenter(double time);

    virtual bool isOpIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& plan) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // The source's region of definition at time, view and scale; null when it has none.
    RectD sourceRoD(double time, ViewIdx view, const RenderScale& scale) const WARN_UNUSED_RETURN;

    double outputPar(double time, ViewIdx view) const WARN_UNUSED_RETURN;

    KnobChoiceWPtr _mode;
    KnobDoubleWPtr _length;
    KnobDoubleWPtr _center;
    KnobDoubleWPtr _direction;
    KnobIntWPtr _samples;
    KnobBoolWPtr _centered;
    KnobDoubleWPtr _falloff;
    KnobBoolWPtr _useTarget;
    KnobDoubleWPtr _target;
    KnobDoubleWPtr _pixelOffset;
    KnobDoubleWPtr _translate;
    KnobDoubleWPtr _rotate;
    KnobDoubleWPtr _scale;
    KnobBoolWPtr _uniform;
    KnobDoubleWPtr _skewX;
    KnobDoubleWPtr _skewY;
    KnobChoiceWPtr _skewOrder;
    KnobDoubleWPtr _transformAmount;
    KnobBoolWPtr _invert;
    KnobBoolWPtr _interactive;
    KnobButtonWPtr _resetCenter;
    KnobColorWPtr _fromColor;
    KnobColorWPtr _toColor;
    KnobChoiceWPtr _ramp;
    KnobBoolWPtr _max;
    KnobChannelSelectWPtr _holdoutChannel;
    KnobChoiceWPtr _filter;
    KnobBoolWPtr _clamp;
    KnobBoolWPtr _blackOutside;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_DirBlur_h
