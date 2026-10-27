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
 * Ported from darktable, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7), src/iop/toneequal.c.
 * GPL-3.0-or-later; see ToneEqualizerKernels.h.
 */

#ifndef Engine_Nodes_Color_ToneEqualizer_h
#define Engine_Nodes_Color_ToneEqualizer_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Color/ToneEqualizerInteract.h"
#include "Engine/Nodes/Color/ToneEqualizerKernels.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Raw/RawNodeSupport.h"
#include "Engine/RectI.h"

#define PLUGINID_NATRON_TONEEQUALIZER "ToneEqualizer"
#define PLUGIN_MAJOR_NATRON_TONEEQUALIZER 1

#define kToneEqualizerParamBlacks "blacks"
#define kToneEqualizerParamDeepShadows "deepShadows"
#define kToneEqualizerParamShadows "shadows"
#define kToneEqualizerParamLightShadows "lightShadows"
#define kToneEqualizerParamMidtones "midtones"
#define kToneEqualizerParamDarkHighlights "darkHighlights"
#define kToneEqualizerParamHighlights "highlights"
#define kToneEqualizerParamWhites "whites"
#define kToneEqualizerParamSpeculars "speculars"
#define kToneEqualizerParamCurveSmoothing "curveSmoothing"
#define kToneEqualizerParamMaskExposureCompensation "maskExposureCompensation"
#define kToneEqualizerParamAutoMaskExposureCompensation "autoMaskExposureCompensation"
#define kToneEqualizerParamMaskContrastCompensation "maskContrastCompensation"
#define kToneEqualizerParamAutoMaskContrastCompensation "autoMaskContrastCompensation"
#define kToneEqualizerParamLuminanceEstimator "luminanceEstimator"
#define kToneEqualizerParamPreserveDetails "preserveDetails"
#define kToneEqualizerParamFilterDiffusion "filterDiffusion"
#define kToneEqualizerParamSmoothingDiameter "smoothingDiameter"
#define kToneEqualizerParamEdgesRefinement "edgesRefinement"
#define kToneEqualizerParamMaskQuantization "maskQuantization"
#define kToneEqualizerParamOutput "output"
#define kToneEqualizerParamPreset "preset"
#define kToneEqualizerParamApplyPreset "applyPreset"
#define kToneEqualizerParamCursorReadout "cursorReadout"

// What RawNodeSupport::noteFrameComputed() names the whole-frame luminance mask.
#define kToneEqualizerFrameMask "ToneEqualizer.mask"

NATRON_NAMESPACE_ENTER

/**
 * @brief darktable's tone equalizer: a gain per exposure band, nine bands from -8 to 0 EV joined
 * by a curve of gaussians, driven by a luminance mask that an edge-aware filter smooths so that
 * local contrast inside a band is kept. Works on the colour channels of linear data; alpha passes
 * through.
 *
 * The mask's filter runs over the whole frame, so the node reads the whole source and keeps the
 * last mask, which band edits reuse and the viewer readout and auto-adjust buttons read.
 **/
class ToneEqualizer
    : public StagedImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new ToneEqualizer(node);
    }

    explicit ToneEqualizer(NodePtr node);

    virtual ~ToneEqualizer();

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The last mask rendered, over the source's region at scale, rows from the top.
     **/
    struct Mask {
        ToneEqualizerKernels::Settings settings;
        double time = 0.;
        ViewIdx view;
        double scale = 1.;
        double par = 1.;
        RectI bounds;
        std::shared_ptr<const std::vector<float>> values;
    };

    std::optional<Mask> lastMask() const WARN_UNUSED_RETURN;

    /**
     * @brief The readout at a canonical position over mask, as the viewer cursor shows it.
     **/
    ToneEqualizerInteract::Reading readingAt(const Mask& mask,
                                             const QPointF& canonical,
                                             double time,
                                             ViewIdx view) WARN_UNUSED_RETURN;

private:
    struct Resolved;

    std::optional<Resolved> resolve(double time, ViewIdx view) WARN_UNUSED_RETURN;

    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;
    virtual void purgeCaches() OVERRIDE FINAL;

    void applyPreset();

    /**
     * @brief Sets a compensation from the mask of the current frame, as darktable's magic wands
     * do: exposure when exposure is true, else contrast.
     **/
    void autoAdjust(bool exposure);

    /**
     * @brief The mask of the source at time, view and scale for the current knobs: the kept one
     * when it matches, else computed from the source.
     **/
    std::optional<Mask> maskForAdjust(double time,
                                      ViewIdx view,
                                      const RenderScale& scale) WARN_UNUSED_RETURN;

    void keepMask(const Mask& mask);

    void showReadout(const ToneEqualizerInteract::Reading& reading);

    virtual bool isOpIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& plan) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE FINAL;

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

    virtual bool hasOverlay() const OVERRIDE FINAL
    {
        return true;
    }

    virtual void drawOverlay(double time,
                             const RenderScale& renderScale,
                             ViewIdx view) OVERRIDE FINAL;
    virtual bool onOverlayPenMotion(double time,
                                    const RenderScale& renderScale,
                                    ViewIdx view,
                                    const QPointF& viewportPos,
                                    const QPointF& pos,
                                    double pressure,
                                    double timestamp) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool onOverlayFocusLost(double time,
                                    const RenderScale& renderScale,
                                    ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    std::array<KnobDoubleWPtr, ToneEqualizerKernels::kBands> _gains;
    KnobDoubleWPtr _curveSmoothing;
    KnobDoubleWPtr _maskExposureCompensation;
    KnobButtonWPtr _autoMaskExposureCompensation;
    KnobDoubleWPtr _maskContrastCompensation;
    KnobButtonWPtr _autoMaskContrastCompensation;
    KnobChoiceWPtr _luminanceEstimator;
    KnobChoiceWPtr _preserveDetails;
    KnobIntWPtr _filterDiffusion;
    KnobDoubleWPtr _smoothingDiameter;
    KnobDoubleWPtr _edgesRefinement;
    KnobDoubleWPtr _maskQuantization;
    KnobChoiceWPtr _output;
    KnobChoiceWPtr _preset;
    KnobButtonWPtr _applyPreset;
    KnobStringWPtr _cursorReadout;

    mutable std::mutex _maskMutex;
    std::string _maskKey;
    std::optional<Mask> _mask;

    ToneEqualizerInteract _interact;

    RawNodeSupport::OwnedMessage _message;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Color_ToneEqualizer_h
