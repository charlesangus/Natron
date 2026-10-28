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

#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
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
#define kDirBlurParamFromColor "fromColor"
#define kDirBlurParamToColor "toColor"

#define kDirBlurInputSource 0
#define kDirBlurInputMask 1

NATRON_NAMESPACE_ENTER

enum DirBlurModeEnum {
    eDirBlurModeDirectional = 0,
    eDirBlurModeRadial = 1,
    eDirBlurModeZoom = 2,
};

struct DirBlurParams {
    DirBlurModeEnum mode = eDirBlurModeZoom;
    // Canonical pixels for Directional and Zoom, degrees for Radial.
    double length = 20.;
    double centerX = 0.;
    double centerY = 0.;
    // Degrees, counter-clockwise from +x.
    double direction = 45.;
    int samples = 32;
    bool centered = false;
    double falloff = 0.;
    double fromColor[4] = { 1., 1., 1., 1. };
    double toColor[4] = { 1., 1., 1., 1. };
};

/**
 * @brief One sample of the blur: the destination-to-source matrix, canonical or in pixels, and
 * the factor each channel's sample is multiplied by before it is summed, in channel-bit order
 * (R, G, B, A). The factors are the sample's weight times its tint over the sum of the weights.
 **/
struct DirBlurSample {
    TransformMath::Mat3 inverse;
    float factor[4];
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
     **/
    static RectD sourceRegion(const std::vector<DirBlurSample>& canonical,
                              const RectD& window,
                              const RenderScale& scale,
                              double par,
                              const RectD& srcRoD) WARN_UNUSED_RETURN;

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

private:
    virtual void initializeKnobs() OVERRIDE FINAL;

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
    KnobColorWPtr _fromColor;
    KnobColorWPtr _toColor;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_DirBlur_h
