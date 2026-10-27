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
 * src/iop/colorbalancergb.c and data/kernels/colorbalancergb.cl. darktable is distributed under
 * the GNU GPL, version 3 or (at your option) any later version, and so is this port:
 * GPL-3.0-or-later.
 *
 *     This file is part of darktable,
 *     Copyright (C) 2021-2025 darktable developers.
 */

#ifndef Engine_Nodes_Color_ColorBalanceRGB_h
#define Engine_Nodes_Color_ColorBalanceRGB_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <memory>
#include <vector>

#include <OpenColorIO/OpenColorIO.h>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/Color/DtColorSpaces.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"

#define PLUGINID_NATRON_COLORBALANCERGB "ColorBalanceRGB"
#define PLUGIN_MAJOR_NATRON_COLORBALANCERGB 1

#define kColorBalanceRGBParamPreset "preset"
#define kColorBalanceRGBParamApplyPreset "applyPreset"

#define kColorBalanceRGBTabMaster "masterTab"
#define kColorBalanceRGBTabFourWays "fourWaysTab"
#define kColorBalanceRGBTabMasks "masksTab"

#define kColorBalanceRGBParamHueShift "hueShift"
#define kColorBalanceRGBParamVibrance "vibrance"
#define kColorBalanceRGBParamContrast "contrast"
#define kColorBalanceRGBParamChromaGlobal "chromaGlobal"
#define kColorBalanceRGBParamChromaShadows "chromaShadows"
#define kColorBalanceRGBParamChromaMidtones "chromaMidtones"
#define kColorBalanceRGBParamChromaHighlights "chromaHighlights"
#define kColorBalanceRGBParamSaturationGlobal "saturationGlobal"
#define kColorBalanceRGBParamSaturationShadows "saturationShadows"
#define kColorBalanceRGBParamSaturationMidtones "saturationMidtones"
#define kColorBalanceRGBParamSaturationHighlights "saturationHighlights"
#define kColorBalanceRGBParamBrillianceGlobal "brillianceGlobal"
#define kColorBalanceRGBParamBrillianceShadows "brillianceShadows"
#define kColorBalanceRGBParamBrillianceMidtones "brillianceMidtones"
#define kColorBalanceRGBParamBrillianceHighlights "brillianceHighlights"

#define kColorBalanceRGBParamOffsetLuminance "offsetLuminance"
#define kColorBalanceRGBParamOffsetHue "offsetHue"
#define kColorBalanceRGBParamOffsetChroma "offsetChroma"
#define kColorBalanceRGBParamLiftLuminance "liftLuminance"
#define kColorBalanceRGBParamLiftHue "liftHue"
#define kColorBalanceRGBParamLiftChroma "liftChroma"
#define kColorBalanceRGBParamGainLuminance "gainLuminance"
#define kColorBalanceRGBParamGainHue "gainHue"
#define kColorBalanceRGBParamGainChroma "gainChroma"
#define kColorBalanceRGBParamPowerLuminance "powerLuminance"
#define kColorBalanceRGBParamPowerHue "powerHue"
#define kColorBalanceRGBParamPowerChroma "powerChroma"

#define kColorBalanceRGBParamSaturationFormula "saturationFormula"
#define kColorBalanceRGBParamShadowsWeight "shadowsWeight"
#define kColorBalanceRGBParamMaskGreyFulcrum "maskGreyFulcrum"
#define kColorBalanceRGBParamHighlightsWeight "highlightsWeight"
#define kColorBalanceRGBParamWhiteFulcrum "whiteFulcrum"
#define kColorBalanceRGBParamContrastGreyFulcrum "contrastGreyFulcrum"
#define kColorBalanceRGBParamOutput "output"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kColorBalanceRGBInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

struct ColorBalanceRGBOp {
    enum SaturationFormulaEnum {
        eSaturationFormulaJzAzBz = 0,
        eSaturationFormulaUcs,
    };

    enum OutputEnum {
        eOutputResult = 0,
        eOutputShadowsMask,
        eOutputMidtonesMask,
        eOutputHighlightsMask,
    };

    /**
     * @brief One of the four wheels of the 4 Ways tab: its luminance, hue (degrees) and chroma.
     **/
    struct WheelKnobs {
        KnobDoubleWPtr luminance;
        KnobDoubleWPtr hue;
        KnobDoubleWPtr chroma;
    };

    enum WheelEnum {
        eWheelOffset = 0,
        eWheelLift,
        eWheelGain,
        eWheelPower,
        eWheelCount
    };

    struct Params {
        int output = eOutputResult;
        int saturationFormula = eSaturationFormulaUcs;

        // True when the working space reaches XYZ through a matrix, so toPipe and fromPipe hold
        // the whole conversion; otherwise toXyz and fromXyz convert through OCIO first.
        bool matrixWorkingSpace = true;
        OCIO_NAMESPACE::ConstCPUProcessorRcPtr toXyz;
        OCIO_NAMESPACE::ConstCPUProcessorRcPtr fromXyz;
        // Working RGB (or XYZ D65 on the OCIO path) to linear Rec.2020, and back.
        DtColorSpaces::Mat3 toPipe = {};
        DtColorSpaces::Mat3 fromPipe = {};
        // darktable's premultiplied pipeline matrices: Rec.2020 to CIE 2006 LMS, XYZ D65 to Rec.2020.
        DtColorSpaces::Mat3 inputMatrix = {};
        DtColorSpaces::Mat3 outputMatrix = {};
        const float* gamutLut = nullptr;

        float global[3] = { 0.f, 0.f, 0.f };
        float shadows[3] = { 1.f, 1.f, 1.f };
        float highlights[3] = { 1.f, 1.f, 1.f };
        float midtones[3] = { 1.f, 1.f, 1.f };
        float chroma[3] = { 0.f, 0.f, 0.f };
        float saturation[3] = { 0.f, 0.f, 0.f };
        float brilliance[3] = { 0.f, 0.f, 0.f };
        float chromaGlobal = 0.f;
        float saturationGlobal = 0.f;
        float brillianceGlobal = 0.f;
        float vibrance = 0.f;
        float contrast = 1.f;
        float greyFulcrum = 0.1845f;
        float whiteFulcrum = 1.f;
        float lWhite = 1.f;
        float midtonesY = 1.f;
        float shadowsWeight = 4.f;
        float highlightsWeight = 4.f;
        float midtonesWeight = 8.f;
        float maskGreyFulcrum = 0.5f;
        float hueCos = 1.f;
        float hueSin = 0.f;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_COLORBALANCERGB,
        .label = "ColorBalanceRGB",
        .description = "darktable's color balance rgb: scene-referred colour grading with perceptual saturation and brilliance.\n"
                       "\n"
                       "The Master tab rotates hues, adds vibrance and contrast around a grey fulcrum, and grades chroma "
                       "linearly and saturation and brilliance perceptually, globally and per luminance range. The 4 Ways tab "
                       "is a lift, gamma, gain and offset grade with a luminance, hue and chroma per wheel. The Masks tab sets "
                       "the luminance ranges the shadows, mid-tones and highlights controls act on, and Output shows them.\n"
                       "\n"
                       "The node works in linear Rec.2020, darktable's pipeline space, converting from and back to the "
                       "project's working space. Negative input is clipped to zero there, as darktable does, and the "
                       "result is gamut-mapped into Rec.2020. Alpha passes through.",
        .grouping = PLUGIN_GROUP_COLOR,
        .majorVersion = PLUGIN_MAJOR_NATRON_COLORBALANCERGB,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/ColorBalanceRGB.png",
        .inputs = kColorBalanceRGBInputs,
    };
    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::Premult, Stage::MaskMix, Stage::PassThroughUnprocessed },
        .layers = LayerPolicy::color(true, true, true, false),
        .op = OpKind::Point,
    };

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page);
    Params bake(const KnobReader& reader) const;
    bool isIdentity(const Params& params) const;
    void row(const Params& params,
             const RowIO& io) const;
    GpuOpRef gpu(const Params& params) const;

    /**
     * @brief darktable's per-pixel maths on linear Rec.2020: rgb in, the graded rgb out, and the
     * shadows, mid-tones and highlights opacities of the pixel.
     **/
    static void pixel(const Params& params,
                      const float rgb[3],
                      float out[3],
                      float opacities[3]);

    /**
     * @brief The double knobs a preset writes, in darktable's parameter order; the rest of the
     * node's double knobs are reset to their defaults when a preset is applied.
     **/
    std::vector<KnobDoubleWPtr> doubleKnobs() const;

    KnobChoiceWPtr preset;
    KnobButtonWPtr applyPreset;

    KnobDoubleWPtr hueShift;
    KnobDoubleWPtr vibrance;
    KnobDoubleWPtr contrast;
    KnobDoubleWPtr chromaGlobal;
    KnobDoubleWPtr chromaShadows;
    KnobDoubleWPtr chromaMidtones;
    KnobDoubleWPtr chromaHighlights;
    KnobDoubleWPtr saturationGlobal;
    KnobDoubleWPtr saturationShadows;
    KnobDoubleWPtr saturationMidtones;
    KnobDoubleWPtr saturationHighlights;
    KnobDoubleWPtr brillianceGlobal;
    KnobDoubleWPtr brillianceShadows;
    KnobDoubleWPtr brillianceMidtones;
    KnobDoubleWPtr brillianceHighlights;

    WheelKnobs wheels[eWheelCount];

    KnobChoiceWPtr saturationFormula;
    KnobDoubleWPtr shadowsWeight;
    KnobDoubleWPtr maskGreyFulcrum;
    KnobDoubleWPtr highlightsWeight;
    KnobDoubleWPtr whiteFulcrum;
    KnobDoubleWPtr contrastGreyFulcrum;
    KnobChoiceWPtr output;

    KnobHolder* holder = nullptr;
};

static_assert(PointOp<ColorBalanceRGBOp>);

class ColorBalanceRGB
    : public ImageNode<ColorBalanceRGBOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new ColorBalanceRGB(node);
    }

    explicit ColorBalanceRGB(NodePtr node);

    virtual ~ColorBalanceRGB();

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    /**
     * @brief The Params the op renders with at (time, view).
     **/
    ColorBalanceRGBOp::Params bakeColorBalanceRGB(double time,
                                                  ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief The op's CPU kernel over one row.
     **/
    void colorBalanceRGBRow(const ColorBalanceRGBOp::Params& params,
                            const RowIO& io) const;

    /**
     * @brief Writes the preset at index (in the Preset choice's order) into every control, as
     * the Apply button does: the preset's values, and the default for every other control.
     **/
    void applyPresetAt(int index);

private:
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Color_ColorBalanceRGB_h
