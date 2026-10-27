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
 * Ported from darktable, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7):
 * src/iop/colorequal.c, src/iop/choleski.h and data/kernels/colorequal.cl. darktable is
 * distributed under the GNU GPL, version 3 or (at your option) any later version, and so is
 * this port: GPL-3.0-or-later.
 *
 *     This file is part of darktable,
 *     Copyright (C) 2024-2025 darktable developers.
 */

#ifndef Engine_Nodes_Color_ColorEqualizer_h
#define Engine_Nodes_Color_ColorEqualizer_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>
#include <list>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/Color/DtColorSpaces.h"
#include "Engine/Nodes/Filter/EdgeAware/EdgeAwareCommon.h"
#include "Engine/Nodes/Image/PassExecutor.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"

#define PLUGINID_NATRON_COLOREQUALIZER "ColorEqualizer"
#define PLUGIN_MAJOR_NATRON_COLOREQUALIZER 1

#define kColorEqualizerParamPreset "preset"
#define kColorEqualizerParamApplyPreset "applyPreset"
#define kColorEqualizerParamCurves "curves"
#define kColorEqualizerTabHue "hueTab"
#define kColorEqualizerTabSaturation "saturationTab"
#define kColorEqualizerTabBrightness "brightnessTab"
#define kColorEqualizerTabOptions "optionsTab"
// The per-node knobs are these prefixes followed by the node's name: hueRed, saturationRed, ...
#define kColorEqualizerParamHuePrefix "hue"
#define kColorEqualizerParamSaturationPrefix "saturation"
#define kColorEqualizerParamBrightnessPrefix "brightness"
#define kColorEqualizerParamNodePlacement "nodePlacement"
#define kColorEqualizerParamHueCurveSmoothing "hueCurveSmoothing"
#define kColorEqualizerParamSaturationThreshold "saturationThreshold"
#define kColorEqualizerParamContrast "contrast"
#define kColorEqualizerParamWhiteLevel "whiteLevel"
#define kColorEqualizerParamUseFilter "useFilter"
#define kColorEqualizerParamChromaSize "chromaSize"
#define kColorEqualizerParamParamSize "paramSize"
#define kColorEqualizerParamOutput "output"

#define kColorEqualizerInputSource 0
#define kColorEqualizerInputMask 1

NATRON_NAMESPACE_ENTER

/**
 * @brief The node's controls as darktable's dt_iop_colorequal_params_t holds them: hue shifts in
 * degrees, saturation and brightness gains, the node placement in degrees and the white level in EV.
 **/
struct ColorEqualizerParams {
    static constexpr int kNodes = 8;

    enum OutputEnum {
        eOutputResult = 0,
        eOutputHue,
        eOutputSaturation,
        eOutputBrightness,
        eOutputSaturationThreshold,
        eOutputBrightnessThreshold,
    };

    float threshold = 0.1f;
    float smoothingHue = 1.f;
    float contrast = 0.f;
    float whiteLevel = 1.f;
    float chromaSize = 1.5f;
    float paramSize = 1.f;
    bool useFilter = true;
    float saturation[kNodes] = { 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f };
    float hue[kNodes] = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f };
    float brightness[kNodes] = { 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f };
    float hueShift = 0.f;
    int output = eOutputResult;
};

/**
 * @brief What darktable's commit_params() derives from the controls for process().
 **/
struct ColorEqualizerData {
    static constexpr int kSatWeights = 2 * 4096 + 1;

    std::array<float, DtColorSpaces::kGamutLutSize> lutHue = {};
    std::array<float, DtColorSpaces::kGamutLutSize> lutSaturation = {};
    std::array<float, DtColorSpaces::kGamutLutSize> lutBrightness = {};
    // The logistic weights of the saturation contrast, at kSatWeights samples over [-1, 1].
    std::vector<float> satWeights;
    float whiteLevel = 2.f;
    float threshold = 0.f;
    float maxBrightness = 1.f;
    float chromaSize = 1.5f;
    float paramSize = 1.f;
    float chromaFeathering = 1e-5f;
    float paramFeathering = 1e-6f;
    bool useFilter = true;
    int output = ColorEqualizerParams::eOutputResult;
};

/**
 * @brief The derived quantities of darktable's _prepare_process() at a render scale.
 **/
struct ColorEqualizerScaled {
    float white = 1.f;
    float satShift = 0.f;
    float brightShift = 0.f;
    float gradientAmp = 0.f;
    float hueSigma = 0.f;
    float paramSigma = 0.f;
    float satSigma = 0.5f;
    float scharrSigma = 0.5f;
};

/**
 * @brief Where a buffer lies in the effect's region of definition, for the threshold outputs'
 * weighting curve, which spans the region of definition: the buffer's first pixel at (x, y) from
 * the region's bottom-left corner, the region width x height pixels.
 **/
struct ColorEqualizerFrame {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

/**
 * @brief darktable's color equalizer: hue-keyed hue, saturation and brightness edits in darktable
 * UCS, over eight hue nodes, the selection smoothed by a guided filter on chromaticity.
 *
 * The node works in linear Rec.2020, darktable's pipeline space, converting from and back to the
 * project's working space. The whole buffer (the render window and the guided filter's reach) is
 * processed at once, as darktable processes a tile.
 **/
class ColorEqualizer
    : public StagedImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new ColorEqualizer(node);
    }

    explicit ColorEqualizer(NodePtr node);

    virtual ~ColorEqualizer();

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual std::string getInputHint(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    /// The node names, red to magenta, as the per-node knobs end.
    static const char* nodeName(int node) WARN_UNUSED_RETURN;

    ColorEqualizerParams getParams(double time, ViewIdx view) const WARN_UNUSED_RETURN;

    /// darktable's commit_params().
    static ColorEqualizerData commit(const ColorEqualizerParams& params) WARN_UNUSED_RETURN;

    /// darktable's _prepare_process() at render scale `scale`.
    static ColorEqualizerScaled scaled(const ColorEqualizerData& data, float scale) WARN_UNUSED_RETURN;

    /**
     * @brief darktable's _periodic_RBF_interpolate(): the periodic curve through the eight node
     * values (solved in place into the interpolation weights), sampled into lut over the UCS hue.
     **/
    static void periodicRbfInterpolate(float nodes[ColorEqualizerParams::kNodes],
                                       float smoothing,
                                       float* lut,
                                       float hueShift,
                                       bool clip);

    /// darktable's _get_satweight().
    static float satWeight(const ColorEqualizerData& data, float sat) WARN_UNUSED_RETURN;

    /// The UCS hue in radians of a hue in degrees as darktable's sliders show it.
    static float conventionalHueToUcs(float degrees) WARN_UNUSED_RETURN;

    /**
     * @brief The pixels the guided filter reads about each output pixel at render scale `scale`,
     * one more than the reach of its gaussians.
     **/
    static int halo(const ColorEqualizerParams& params, float scale) WARN_UNUSED_RETURN;

    /**
     * @brief darktable's process() over a width x height buffer of linear Rec.2020 pixels with
     * alpha (4 floats each, rows bottom-up), into out (4 floats each): the equalized Rec.2020
     * colour, or the weight the output knob shows; alpha is copied.
     **/
    static void process(const ColorEqualizerData& data,
                        float scale,
                        const float* rgba,
                        float* out,
                        int width,
                        int height,
                        const ColorEqualizerFrame& frame,
                        const EdgeAware::ParallelFor& parallel);

    static int presetCount() WARN_UNUSED_RETURN;
    static const char* presetLabel(int index) WARN_UNUSED_RETURN;
    static ColorEqualizerParams presetParams(int index) WARN_UNUSED_RETURN;

    /**
     * @brief Writes the preset at index (in the Preset choice's order) into every control, as the
     * Apply button does; the output knob is left as it is.
     **/
    void applyPresetAt(int index);

protected:
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual bool isOpIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& plan) OVERRIDE FINAL WARN_UNUSED_RETURN;
    // One pass without work carrying the halo: the region actions grow by it, and renderOp()
    // processes the whole buffer itself.
    virtual PassList opPasses(double time,
                              ViewIdx view,
                              const RenderScale& scale) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    StatusEnum recordGpu(const PassGpuIO& io,
                         const ColorEqualizerParams& params,
                         float scale) const WARN_UNUSED_RETURN;

    void installCurvePainter();

    KnobChoiceWPtr _preset;
    KnobButtonWPtr _applyPreset;
    KnobParametricWPtr _curves;
    KnobDoubleWPtr _hue[ColorEqualizerParams::kNodes];
    KnobDoubleWPtr _saturation[ColorEqualizerParams::kNodes];
    KnobDoubleWPtr _brightness[ColorEqualizerParams::kNodes];
    KnobDoubleWPtr _nodePlacement;
    KnobDoubleWPtr _hueCurveSmoothing;
    KnobDoubleWPtr _saturationThreshold;
    KnobDoubleWPtr _contrast;
    KnobDoubleWPtr _whiteLevel;
    KnobBoolWPtr _useFilter;
    KnobDoubleWPtr _chromaSize;
    KnobDoubleWPtr _paramSize;
    KnobChoiceWPtr _output;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Color_ColorEqualizer_h
