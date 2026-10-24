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
 * The driver of darktable's demosaic module, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7),
 * src/iop/demosaic.c, Copyright (C) 2010-2026 darktable developers, distributed under the GNU
 * GPL version 3 or later.
 */

#ifndef Engine_Nodes_Raw_RawDemosaic_h
#define Engine_Nodes_Raw_RawDemosaic_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <mutex>
#include <optional>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Raw/RawNodeSupport.h"

#define PLUGINID_NATRON_RAWDEMOSAIC "fr.natron.RawDemosaic"
#define PLUGIN_MAJOR_NATRON_RAWDEMOSAIC 1

#define kRawDemosaicParamBayerMethod "method"
#define kRawDemosaicParamXTransMethod "xtransMethod"
#define kRawDemosaicParamGreenEq "greenEquilibration"
#define kRawDemosaicParamEdgeThreshold "edgeThreshold"
#define kRawDemosaicParamColorSmoothing "colorSmoothing"
#define kRawDemosaicParamLmmseRefine "lmmseRefine"
#define kRawDemosaicParamDualThreshold "dualThreshold"
#define kRawDemosaicParamCaptureSharpen "captureSharpen"
#define kRawDemosaicParamCaptureIterations "captureIterations"
#define kRawDemosaicParamCaptureRadius "captureRadius"
#define kRawDemosaicParamCaptureThreshold "captureContrastSensitivity"
#define kRawDemosaicParamCaptureThresholdOverride "captureContrastSensitivityOverride"
#define kRawDemosaicParamCaptureBoost "captureCornerBoost"
#define kRawDemosaicParamCaptureCenter "captureSharpCenter"

NATRON_NAMESPACE_ENTER

enum RawDemosaicBayerMethodEnum {
    eRawDemosaicBayerRCD = 0,
    eRawDemosaicBayerPPG,
    eRawDemosaicBayerAMaZE,
    eRawDemosaicBayerVNG4,
    eRawDemosaicBayerPassthroughMonochrome,
    eRawDemosaicBayerPhotositeColor,
    eRawDemosaicBayerLMMSE,
    eRawDemosaicBayerRCDDual,
    eRawDemosaicBayerAMaZEDual
};

enum RawDemosaicXTransMethodEnum {
    eRawDemosaicXTransMarkesteijn1 = 0,
    eRawDemosaicXTransMarkesteijn3,
    eRawDemosaicXTransVNG,
    eRawDemosaicXTransPassthroughMonochrome,
    eRawDemosaicXTransPhotositeColor,
    eRawDemosaicXTransMarkesteijn3Dual,
    eRawDemosaicXTransFDC
};

enum RawDemosaicLmmseRefineEnum {
    eRawDemosaicLmmseBasic = 0,
    eRawDemosaicLmmseMedian,
    eRawDemosaicLmmse3xMedian,
    eRawDemosaicLmmseRefineMedians,
    eRawDemosaicLmmse2xRefineMedians
};

enum RawDemosaicGreenEqEnum {
    eRawDemosaicGreenEqNone = 0,
    eRawDemosaicGreenEqLocal,
    eRawDemosaicGreenEqFull,
    eRawDemosaicGreenEqBoth
};

/**
 * @brief Raw to image: darktable's demosaic module on prepared raw data. A Bayer mosaic is
 * demosaiced by RCD (the default), PPG, AMaZE, VNG4 or LMMSE, an X-Trans one by Markesteijn 1-pass (the
 * default), Markesteijn 3-pass, VNG or Frequency Domain Chroma, or either left as photosites (passthrough monochrome, photosite
 * colour), after the optional green equilibration (Bayer) and
 * followed by the optional colour smoothing passes. The dual methods (RCD, AMaZE, Markesteijn
 * 3-pass) blend the method's output with VNG's where the image has little detail, by Dual
 * Threshold, and turn colour smoothing off as darktable does. Capture Sharpen deconvolves the
 * demosaiced output (or monochrome data without a CFA) before the dual blend and colour
 * smoothing, by darktable's capture sharpening. Data without a CFA passes through. The output
 * is RGBA with an alpha of 1, and its metadata keeps the camera matrix and white balance keys
 * and drops the CFA and the raw stage.
 *
 * Each render reads its window grown by what the method reads around a pixel, aligned to the CFA
 * period (and, for AMaZE, LMMSE and Frequency Domain Chroma, to their own tile grids), so a tile is bit for bit the same pixels
 * as the full frame. Full green equilibration and capture sharpening's automatic radius measure
 * the whole image, once per source and settings. Renders at full resolution only.
 **/
class RawDemosaic
    : public StagedImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new RawDemosaic(node);
    }

    explicit RawDemosaic(NodePtr node);

    virtual ~RawDemosaic();

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    struct Settings;

    /**
     * @brief The source pixels a render of window needs at time and view, or window itself when
     * the source has no CFA.
     **/
    RectI sourceRegionFor(const RectI& window, double time, ViewIdx view) WARN_UNUSED_RETURN;

    /**
     * @brief The automatic capture sharpening radius last measured, for the frame a render with
     * Radius 0 last ran on, or nothing before any has.
     **/
    std::optional<double> lastAutoCaptureRadius() const WARN_UNUSED_RETURN;

private:
    std::optional<Settings> resolveSettings(double time, ViewIdx view) WARN_UNUSED_RETURN;

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;

    virtual void onMetadataRefreshed(const NodeMetadata& metadata) OVERRIDE FINAL;

    // darktable's gui_changed() for the source's sensor, read from its CFA: only that sensor's
    // method list and the knobs its method uses show. With no source, or one without a CFA, both
    // lists show.
    void refreshMethodKnobsVisibility();

    // RawHighlights' override pattern for Contrast Sensitivity: the knob is editable only while
    // overridden, and otherwise shows darktable's _get_variance_threshold() for the source.
    void refreshCaptureOverride();

    // gui_changed()'s capture sharpening part, support being darktable's capture_support.
    void refreshCaptureKnobsVisibility(bool support);

    void showSourceCaptureThreshold(double time);

    // The radius an automatic run measured, in the Radius tooltip, set on the main thread.
    void showAutoCaptureRadius(double radius, bool reliable);

    virtual bool isOpIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& plan) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief PPG and RCD, without green equilibration or colour smoothing, and the pass-through of
     * data without a CFA. AMaZE, VNG, Markesteijn, Frequency Domain Chroma and the dual methods have no GPU path yet. Full green
     * equilibration measures the whole source once per frame, which a region's strips cannot
     * share, and local equilibration and colour smoothing are not ported, so any of them keeps the
     * node on the CPU. LMMSE has no GPU path, as in darktable.
     **/
    virtual bool canRenderOpOnGpu(const GpuQuery& query) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOpGpu(const GpuRenderArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool getOpGpuInputLayer(int inputNb,
                                    const ImageLayerDesc& plane,
                                    double time,
                                    ViewIdx view,
                                    ImageLayerDesc* layer) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual RectI getOpGpuInputRoI(int inputNb,
                                   const RectI& outputRect,
                                   const RenderScale& scale) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE FINAL;

    virtual StatusEnum getPreferredMetadata(NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual ImageMetadata deriveOutputMetadata(double time,
                                               ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    KnobChoiceWPtr _bayerMethod;
    KnobChoiceWPtr _xtransMethod;
    KnobChoiceWPtr _greenEq;
    KnobDoubleWPtr _edgeThreshold;
    KnobIntWPtr _colorSmoothing;
    KnobChoiceWPtr _lmmseRefine;
    KnobDoubleWPtr _dualThreshold;
    KnobBoolWPtr _captureSharpen;
    KnobIntWPtr _captureIterations;
    KnobDoubleWPtr _captureRadius;
    KnobDoubleWPtr _captureThreshold;
    KnobBoolWPtr _captureThresholdOverride;
    KnobDoubleWPtr _captureBoost;
    KnobDoubleWPtr _captureCenter;

    // The ratio of full green equilibration, for the source and settings its key names.
    std::mutex _greenMutex;
    bool _greenValid;
    U64 _greenKey;
    double _greenRatio;

    // The automatic capture sharpening radius, for the source and settings its key names.
    mutable std::mutex _captureMutex;
    bool _captureValid;
    U64 _captureKey;
    double _captureRadiusValue;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Raw_RawDemosaic_h
