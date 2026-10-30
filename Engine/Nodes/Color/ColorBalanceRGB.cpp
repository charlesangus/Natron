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
 * src/iop/colorbalancergb.c (commit_params(), process(), init_presets() and the parameters and
 * sliders of gui_init()). darktable is distributed under the GNU GPL, version 3 or (at your
 * option) any later version, and so is this port: GPL-3.0-or-later.
 *
 *     This file is part of darktable,
 *     Copyright (C) 2021-2025 darktable developers.
 */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "ColorBalanceRGB.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/IO/IoColorSpace.h"
#include "Engine/Nodes/Image/ColorMath.h"
#include "Engine/Nodes/Raw/RawColorMath.h"
#include "Engine/Nodes/Raw/RawColorProfiles.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"
#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

using namespace DtColorSpaces;

namespace {

// darktable's hue sliders read 0° as the hue of Yrg angle -30°.
const float kAngleShift = -30.f;
// The exponent that puts middle grey at 50 % for the luminance masks.
const float kMaskExponent = 0.4101205819200422f;
const char* const kXyzD65Role = "cie_xyz_d65_interchange";
// Pixels per OCIO call on the non-matrix working-space path, kept on the stack.
const int kChunk = 128;

/**
 * @brief darktable's pipeline matrices for its default working profile, linear Rec.2020 as
 * Little CMS stores it (colorants adapted to D50).
 **/
struct PipelineMatrices {
    Mat3 input; // Rec.2020 to CIE 2006 LMS D65
    Mat3 output; // XYZ D65 to Rec.2020
    Mat3 rec2020ToXyz; // Rec.2020 to XYZ D65 through CAT16, as the gamut LUTs sample it
    double xyzToRec2020[9];
    double rec2020ToXyzD65[9];
};

Mat3
toFloat(const RawColorMath::Mat3& m)
{
    Mat3 o = {};

    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            o.m[r][c] = (float)m[r * 3 + c];
        }
    }

    return o;
}

Mat3
toFloat(const double m[9])
{
    Mat3 o = {};

    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            o.m[r][c] = (float)m[r * 3 + c];
        }
    }

    return o;
}

const PipelineMatrices&
pipelineMatrices()
{
    static const PipelineMatrices matrices = [] {
        PipelineMatrices p = {};
        const RawColorMath::Mat3 toXyz = RawColorProfiles::toXyz(RawColorProfiles::Space::LinearRec2020);
        const RawColorMath::Mat3 fromXyz = RawColorProfiles::fromXyz(RawColorProfiles::Space::LinearRec2020);
        const std::optional<RawColorMath::Mat3> toD50 = RawColorMath::bradford(RawColorMath::d65White(), RawColorMath::lcmsD50White());
        const RawColorMath::Mat3 matrixIn = RawColorMath::multiply(*toD50, toXyz);
        const std::optional<RawColorMath::Mat3> matrixOut = RawColorMath::invert(matrixIn);

        p.rec2020ToXyz = multiply(kXyzD50ToD65Cat16, toFloat(matrixIn));
        p.input = multiply(kXyzD65ToLms2006, p.rec2020ToXyz);
        p.output = multiply(toFloat(*matrixOut), kXyzD65ToD50Cat16);
        for (int i = 0; i < 9; ++i) {
            p.xyzToRec2020[i] = fromXyz[i];
            p.rec2020ToXyzD65[i] = toXyz[i];
        }

        return p;
    }();

    return matrices;
}

const float*
gamutLut(int formula)
{
    if (formula == ColorBalanceRGBOp::eSaturationFormulaJzAzBz) {
        static const std::array<float, kGamutLutSize> jzazbz = [] {
            std::array<float, kGamutLutSize> lut = {};
            buildJzAzBzGamutLut(pipelineMatrices().rec2020ToXyz, lut.data());

            return lut;
        }();

        return jzazbz.data();
    }
    static const std::array<float, kGamutLutSize> ucs = [] {
        std::array<float, kGamutLutSize> lut = {};
        buildUcsGamutLut(pipelineMatrices().rec2020ToXyz, lut.data());

        return lut;
    }();

    return ucs.data();
}

void
multiply3(const double a[9],
          const double b[9],
          double o[9])
{
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            o[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
        }
    }
}

/**
 * @brief Fills the conversions between the project's working space and linear Rec.2020: one
 * matrix each way when the working space has a matrix to XYZ (ACEScg when there is no project),
 * otherwise OCIO processors to and from XYZ D65 followed by the Rec.2020 matrices.
 **/
void
resolveWorkingSpace(const KnobHolder* holder,
                    ColorBalanceRGBOp::Params* p)
{
    const PipelineMatrices& pm = pipelineMatrices();
    AppInstancePtr app = holder ? holder->getApp() : AppInstancePtr();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    ProjectColorManagementPtr cm = project ? project->getColorManagement() : ProjectColorManagementPtr();
    double w[9];

    if (cm && !cm->getWorkingToXYZ(w)) {
        const std::string working = project->getWorkingColorSpace();
        std::string error;
        OCIO_NAMESPACE::ConstCPUProcessorRcPtr to;
        OCIO_NAMESPACE::ConstCPUProcessorRcPtr from;
        if (IoColorSpace::processor(*project, working, kXyzD65Role, {}, &to, &error) && IoColorSpace::processor(*project, kXyzD65Role, working, {}, &from, &error)) {
            p->matrixWorkingSpace = false;
            p->toXyz = to;
            p->fromXyz = from;
            p->toPipe = toFloat(pm.xyzToRec2020);
            p->fromPipe = toFloat(pm.rec2020ToXyzD65);

            return;
        }
    }
    ColorMath::workingToXYZ(holder, w);
    const std::optional<RawColorMath::Mat3> xyzToWorking = RawColorMath::invert(RawColorMath::Mat3 { w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8] });
    double toPipe[9];
    double fromPipe[9];
    multiply3(pm.xyzToRec2020, w, toPipe);
    if (xyzToWorking) {
        multiply3(xyzToWorking->data(), pm.rec2020ToXyzD65, fromPipe);
    } else {
        std::copy(pm.rec2020ToXyzD65, pm.rec2020ToXyzD65 + 9, fromPipe);
    }
    p->matrixWorkingSpace = true;
    p->toPipe = toFloat(toPipe);
    p->fromPipe = toFloat(fromPipe);
}

/**
 * @brief darktable's opacity_masks(): the shadows, mid-tones and highlights opacities of x, the
 * luminance raised to kMaskExponent, and their complements.
 **/
inline void
opacityMasks(float x,
             float shadowsWeight,
             float highlightsWeight,
             float midtonesWeight,
             float maskGreyFulcrum,
             float out[3],
             float comp[3])
{
    const float xOffset = x - maskGreyFulcrum;
    const float xOffsetNorm = xOffset / maskGreyFulcrum;
    const float alpha = 1.f / (1.f + std::exp(xOffsetNorm * shadowsWeight));
    const float beta = 1.f / (1.f + std::exp(-xOffsetNorm * highlightsWeight));
    const float alphaComp = 1.f - alpha;
    const float betaComp = 1.f - beta;
    const float gamma = std::exp(-square(xOffset) * midtonesWeight / 4.f) * square(alphaComp) * square(betaComp) * 8.f;

    out[0] = alpha;
    out[1] = gamma;
    out[2] = beta;
    comp[0] = alphaComp;
    comp[1] = 1.f - gamma;
    comp[2] = betaComp;
}

inline float
dot3(const float a[3],
     const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

struct ColorBalanceRGBPreset {
    const char* id;
    const char* label;
    int saturationFormula;
    float chromaGlobal;
    float saturationGlobal;
    float saturationShadows;
    float saturationMidtones;
    float saturationHighlights;
};

// darktable's init_presets(): every other parameter at its default.
const ColorBalanceRGBPreset kPresets[] = {
    { "basicColorfulnessLegacy", "Basic Colorfulness | Legacy", ColorBalanceRGBOp::eSaturationFormulaJzAzBz, 0.2f, 0.f, 0.1f, 0.05f, -0.05f },
    { "basicColorfulnessNaturalSkin", "Basic Colorfulness | Natural Skin", ColorBalanceRGBOp::eSaturationFormulaUcs, 0.f, 0.2f, 0.30f, 0.f, -0.5f },
    { "basicColorfulnessVibrantColors", "Basic Colorfulness | Vibrant Colors", ColorBalanceRGBOp::eSaturationFormulaUcs, 0.f, 0.2f, 0.5f, 0.f, -0.25f },
    { "basicColorfulnessStandard", "Basic Colorfulness | Standard", ColorBalanceRGBOp::eSaturationFormulaUcs, 0.f, 0.2f, 0.25f, 0.f, -0.25f },
};
const int kPresetCount = (int)(sizeof(kPresets) / sizeof(kPresets[0]));

KnobGroupPtr
addTab(KnobHolder* holder,
       const KnobPagePtr& page,
       const char* name,
       const char* label,
       const char* hint)
{
    KnobGroupPtr group = AppManager::createKnob<KnobGroup>(holder, EffectInstance::tr(label).toStdString());

    group->setName(name);
    group->setHintToolTip(EffectInstance::tr(hint).toStdString());
    group->setAsTab();
    page->addKnob(group);

    return group;
}

void
addSeparator(KnobHolder* holder,
             const KnobGroupPtr& tab,
             const char* name,
             const char* label)
{
    KnobSeparatorPtr separator = AppManager::createKnob<KnobSeparator>(holder, EffectInstance::tr(label).toStdString(), 1, false);

    separator->setName(name);
    tab->addKnob(separator);
}

KnobChoicePtr
addChoice(KnobHolder* holder,
          const KnobGroupPtr& tab,
          const char* name,
          const char* label,
          const char* hint,
          const std::vector<ChoiceOption>& options,
          int defaultIndex)
{
    KnobChoicePtr choice = AppManager::createKnob<KnobChoice>(holder, EffectInstance::tr(label).toStdString());

    choice->setName(name);
    choice->setHintToolTip(EffectInstance::tr(hint).toStdString());
    choice->populateChoices(options);
    choice->setDefaultValue(defaultIndex);
    choice->setAnimationEnabled(false);
    tab->addKnob(choice);

    return choice;
}

KnobDoublePtr
addDouble(KnobHolder* holder,
          const KnobGroupPtr& tab,
          const char* name,
          const char* label,
          const char* hint,
          double defaultValue,
          double minimum,
          double maximum,
          double displayMinimum,
          double displayMaximum)
{
    KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr(label).toStdString());

    knob->setName(name);
    knob->setHintToolTip(EffectInstance::tr(hint).toStdString());
    knob->setDefaultValue(defaultValue);
    knob->setMinimum(minimum);
    knob->setMaximum(maximum);
    knob->setDisplayMinimum(displayMinimum);
    knob->setDisplayMaximum(displayMaximum);
    knob->setDecimals(4);
    tab->addKnob(knob);

    return knob;
}

// The wheels' colours: each hue at a chroma that reaches kWheelShownChroma at the chroma knob's
// display maximum, so every wheel reads alike however small its range, at kWheelShownLuminance.
const float kWheelShownChroma = 0.12f;
const float kWheelShownLuminance = 0.35f;

void
wheelColour(const KnobHolder* holder,
            double displayMaximum,
            double hueDegrees,
            double chroma,
            double rgb[3])
{
    const double t = (displayMaximum > 0.) ? std::min(1., std::max(0., chroma / displayMaximum)) : 0.;
    float ych[4];
    float grading[3];
    float lms[3];
    float xyz[3];

    makeYch(kWheelShownLuminance, (float)t * kWheelShownChroma, degToRad((float)hueDegrees + kAngleShift), ych);
    ychToGradingRgb(ych, grading);
    gradingRgbToLms(grading, lms);
    lmsToXyz(lms, xyz);

    double w[9];
    ColorMath::workingToXYZ(holder, w);
    const std::optional<RawColorMath::Mat3> toWorking = RawColorMath::invert(RawColorMath::Mat3 { w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8] });
    for (int c = 0; c < 3; ++c) {
        rgb[c] = toWorking ? ((*toWorking)[3 * c] * xyz[0] + (*toWorking)[3 * c + 1] * xyz[1] + (*toWorking)[3 * c + 2] * xyz[2]) : xyz[c];
        rgb[c] = std::max(0., rgb[c]);
    }
}

} // anonymous namespace

void
ColorBalanceRGBOp::createKnobs(KnobHolder* knobHolder,
                               const KnobPagePtr& page)
{
    holder = knobHolder;

    {
        std::vector<ChoiceOption> options;
        for (const ColorBalanceRGBPreset& entry : kPresets) {
            options.push_back(ChoiceOption(entry.id, EffectInstance::tr(entry.label).toStdString(), std::string()));
        }
        KnobChoicePtr choice = AppManager::createKnob<KnobChoice>(knobHolder, EffectInstance::tr("Preset").toStdString());
        choice->setName(kColorBalanceRGBParamPreset);
        choice->setHintToolTip(EffectInstance::tr("One of darktable's built-in presets. Apply writes it into every control.").toStdString());
        choice->populateChoices(options);
        choice->setDefaultValue(0);
        choice->setAnimationEnabled(false);
        choice->setEvaluateOnChange(false);
        choice->setAddNewLine(false);
        page->addKnob(choice);
        preset = choice;

        KnobButtonPtr apply = AppManager::createKnob<KnobButton>(knobHolder, EffectInstance::tr("Apply Preset").toStdString());
        apply->setName(kColorBalanceRGBParamApplyPreset);
        apply->setHintToolTip(EffectInstance::tr("Sets the preset's controls to its values and every other grading and mask control to its default.").toStdString());
        apply->setEvaluateOnChange(false);
        page->addKnob(apply);
        applyPreset = apply;
    }

    KnobGroupPtr master = addTab(knobHolder, page, kColorBalanceRGBTabMaster, "Master", "Global grading.");
    hueShift = addDouble(knobHolder, master, kColorBalanceRGBParamHueShift, "Hue Shift",
                         "Rotate all hues by an angle in degrees, at the same luminance.", 0., -180., 180., -180., 180.);
    vibrance = addDouble(knobHolder, master, kColorBalanceRGBParamVibrance, "Vibrance",
                         "Increase colorfulness mostly on low-chroma colors.", 0., -1., 1., -0.5, 0.5);
    contrast = addDouble(knobHolder, master, kColorBalanceRGBParamContrast, "Contrast",
                         "Increase the contrast at constant chromaticity, around Contrast Grey Fulcrum (Masks tab).", 0., -1., 1., -0.5, 0.5);

    addSeparator(knobHolder, master, "linearChromaGrading", "Linear Chroma Grading");
    chromaGlobal = addDouble(knobHolder, master, kColorBalanceRGBParamChromaGlobal, "Chroma Global",
                             "Increase colorfulness at same luminance globally.", 0., -1., 1., -0.5, 0.5);
    chromaShadows = addDouble(knobHolder, master, kColorBalanceRGBParamChromaShadows, "Chroma Shadows",
                              "Increase colorfulness at same luminance mostly in shadows.", 0., -1., 1., -1., 1.);
    chromaMidtones = addDouble(knobHolder, master, kColorBalanceRGBParamChromaMidtones, "Chroma Midtones",
                               "Increase colorfulness at same luminance mostly in mid-tones.", 0., -1., 1., -1., 1.);
    chromaHighlights = addDouble(knobHolder, master, kColorBalanceRGBParamChromaHighlights, "Chroma Highlights",
                                 "Increase colorfulness at same luminance mostly in highlights.", 0., -1., 1., -1., 1.);

    addSeparator(knobHolder, master, "perceptualSaturationGrading", "Perceptual Saturation Grading");
    saturationGlobal = addDouble(knobHolder, master, kColorBalanceRGBParamSaturationGlobal, "Saturation Global",
                                 "Add or remove saturation by an absolute amount.", 0., -1., 1., -1., 1.);
    saturationShadows = addDouble(knobHolder, master, kColorBalanceRGBParamSaturationShadows, "Saturation Shadows",
                                  "Increase or decrease saturation proportionally to the original pixel saturation, mostly in shadows.", 0., -1., 1., -1., 1.);
    saturationMidtones = addDouble(knobHolder, master, kColorBalanceRGBParamSaturationMidtones, "Saturation Midtones",
                                   "Increase or decrease saturation proportionally to the original pixel saturation, mostly in mid-tones.", 0., -1., 1., -1., 1.);
    saturationHighlights = addDouble(knobHolder, master, kColorBalanceRGBParamSaturationHighlights, "Saturation Highlights",
                                     "Increase or decrease saturation proportionally to the original pixel saturation, mostly in highlights.", 0., -1., 1., -1., 1.);

    addSeparator(knobHolder, master, "perceptualBrillianceGrading", "Perceptual Brilliance Grading");
    brillianceGlobal = addDouble(knobHolder, master, kColorBalanceRGBParamBrillianceGlobal, "Brilliance Global",
                                 "Add or remove brilliance by an absolute amount.", 0., -1., 1., -1., 1.);
    brillianceShadows = addDouble(knobHolder, master, kColorBalanceRGBParamBrillianceShadows, "Brilliance Shadows",
                                  "Increase or decrease brilliance proportionally to the original pixel brilliance, mostly in shadows.", 0., -1., 1., -1., 1.);
    brillianceMidtones = addDouble(knobHolder, master, kColorBalanceRGBParamBrillianceMidtones, "Brilliance Midtones",
                                   "Increase or decrease brilliance proportionally to the original pixel brilliance, mostly in mid-tones.", 0., -1., 1., -1., 1.);
    brillianceHighlights = addDouble(knobHolder, master, kColorBalanceRGBParamBrillianceHighlights, "Brilliance Highlights",
                                     "Increase or decrease brilliance proportionally to the original pixel brilliance, mostly in highlights.", 0., -1., 1., -1., 1.);

    // The wheels come first, two to a row, each editing its hue, chroma and luminance knobs;
    // the knobs' own rows sit in a collapsed group below for typed values, with darktable's soft
    // ranges.
    KnobGroupPtr fourWays = addTab(knobHolder, page, kColorBalanceRGBTabFourWays, "4 Ways", "Selective color grading.");
    KnobSeparatorPtr wheelGrid = AppManager::createKnob<KnobSeparator>(knobHolder, EffectInstance::tr("Wheels").toStdString(), 1, false);
    wheelGrid->setName("wheels");
    wheelGrid->setHintToolTip(EffectInstance::tr("Angle is hue, distance from the centre is chroma, the slider beneath is luminance.").toStdString());
    fourWays->addKnob(wheelGrid);
    KnobGroupPtr values = AppManager::createKnob<KnobGroup>(knobHolder, EffectInstance::tr("Values").toStdString());
    values->setName("values");
    values->setHintToolTip(EffectInstance::tr("The wheels' hue, chroma and luminance as numbers.").toStdString());
    values->setDefaultValue(false);
    fourWays->addKnob(values);
    addSeparator(knobHolder, values, "offsetSection", "Global Offset");
    wheels[eWheelOffset].hue = addDouble(knobHolder, values, kColorBalanceRGBParamOffsetHue, "Offset Hue",
                                         "Hue of the global color offset, in degrees.", 0., 0., 360., 0., 360.);
    wheels[eWheelOffset].chroma = addDouble(knobHolder, values, kColorBalanceRGBParamOffsetChroma, "Offset Chroma",
                                            "Chroma of the global color offset.", 0., 0., 1., 0., 0.01);
    wheels[eWheelOffset].luminance = addDouble(knobHolder, values, kColorBalanceRGBParamOffsetLuminance, "Offset Luminance",
                                               "Global luminance offset.", 0., -1., 1., -0.05, 0.05);
    addSeparator(knobHolder, values, "liftSection", "Shadows Lift");
    wheels[eWheelLift].hue = addDouble(knobHolder, values, kColorBalanceRGBParamLiftHue, "Lift Hue",
                                       "Hue of the color gain in shadows, in degrees.", 0., 0., 360., 0., 360.);
    wheels[eWheelLift].chroma = addDouble(knobHolder, values, kColorBalanceRGBParamLiftChroma, "Lift Chroma",
                                          "Chroma of the color gain in shadows.", 0., 0., 1., 0., 0.5);
    wheels[eWheelLift].luminance = addDouble(knobHolder, values, kColorBalanceRGBParamLiftLuminance, "Lift Luminance",
                                             "Luminance gain in shadows.", 0., -1., 1., -1., 1.);
    addSeparator(knobHolder, values, "gainSection", "Highlights Gain");
    wheels[eWheelGain].hue = addDouble(knobHolder, values, kColorBalanceRGBParamGainHue, "Gain Hue",
                                       "Hue of the color gain in highlights, in degrees.", 0., 0., 360., 0., 360.);
    wheels[eWheelGain].chroma = addDouble(knobHolder, values, kColorBalanceRGBParamGainChroma, "Gain Chroma",
                                          "Chroma of the color gain in highlights.", 0., 0., 1., 0., 0.2);
    wheels[eWheelGain].luminance = addDouble(knobHolder, values, kColorBalanceRGBParamGainLuminance, "Gain Luminance",
                                             "Luminance gain in highlights.", 0., -1., 1., -0.5, 0.5);
    addSeparator(knobHolder, values, "powerSection", "Power");
    wheels[eWheelPower].hue = addDouble(knobHolder, values, kColorBalanceRGBParamPowerHue, "Power Hue",
                                        "Hue of the color exponent in mid-tones, in degrees.", 0., 0., 360., 0., 360.);
    wheels[eWheelPower].chroma = addDouble(knobHolder, values, kColorBalanceRGBParamPowerChroma, "Power Chroma",
                                           "Chroma of the color exponent in mid-tones.", 0., 0., 1., 0., 0.1);
    wheels[eWheelPower].luminance = addDouble(knobHolder, values, kColorBalanceRGBParamPowerLuminance, "Power Luminance",
                                              "Luminance exponent in mid-tones.", 0., -1., 1., -0.25, 0.25);
    const char* const wheelLabels[eWheelCount] = { "Global Offset", "Shadows Lift", "Highlights Gain", "Power" };
    std::vector<KnobSeparator::ColorWheelSpec> wheelSpecs(eWheelCount);
    for (int i = 0; i < eWheelCount; ++i) {
        const double displayMaximum = wheels[i].chroma.lock()->getDisplayMaximum();
        wheelSpecs[i].label = EffectInstance::tr(wheelLabels[i]).toStdString();
        wheelSpecs[i].hue = wheels[i].hue;
        wheelSpecs[i].chroma = wheels[i].chroma;
        wheelSpecs[i].luminance = wheels[i].luminance;
        wheelSpecs[i].colour = [knobHolder, displayMaximum](double hueDegrees, double c, double rgb[3]) {
            wheelColour(knobHolder, displayMaximum, hueDegrees, c, rgb);
        };
    }
    wheelGrid->setColorWheelGrid(wheelSpecs);

    KnobGroupPtr masks = addTab(knobHolder, page, kColorBalanceRGBTabMasks, "Masks", "Isolate luminances.");
    saturationFormula = addChoice(knobHolder, masks, kColorBalanceRGBParamSaturationFormula, "Saturation Formula",
                                  "Choose in which uniform color space the saturation and brilliance are computed.",
                                  { ChoiceOption("jzazbz", "JzAzBz (2021)", ""), ChoiceOption("darktableUcs", "darktable UCS (2022)", "") },
                                  eSaturationFormulaUcs);
    addSeparator(knobHolder, masks, "luminanceRanges", "Luminance Ranges");
    shadowsWeight = addDouble(knobHolder, masks, kColorBalanceRGBParamShadowsWeight, "Shadows Weight",
                              "Weight of the shadows over the whole tonal range (darktable's shadows fall-off).", 1., 0., 3., 0., 3.);
    maskGreyFulcrum = addDouble(knobHolder, masks, kColorBalanceRGBParamMaskGreyFulcrum, "Mask Grey Fulcrum",
                                "Position of the middle-grey reference for masking.", 0.1845, 0., 1., 0., 1.);
    highlightsWeight = addDouble(knobHolder, masks, kColorBalanceRGBParamHighlightsWeight, "Highlights Weight",
                                 "Weight of the highlights over the whole tonal range (darktable's highlights fall-off).", 1., 0., 3., 0., 3.);
    addSeparator(knobHolder, masks, "threshold", "Threshold");
    whiteFulcrum = addDouble(knobHolder, masks, kColorBalanceRGBParamWhiteFulcrum, "White Fulcrum",
                             "Peak white luminance, in EV, used to normalize the power function.", 0., -16., 16., -2., 2.);
    contrastGreyFulcrum = addDouble(knobHolder, masks, kColorBalanceRGBParamContrastGreyFulcrum, "Contrast Grey Fulcrum",
                                    "Middle-grey luminance the contrast pivots around.", 0.1845, 0., 1., 0.1, 0.5);
    addSeparator(knobHolder, masks, "maskPreview", "Mask Preview");
    output = addChoice(knobHolder, masks, kColorBalanceRGBParamOutput, "Output",
                       "Result renders the grade. The mask outputs write the opacity of the shadows, mid-tones or highlights "
                       "controls as grey, to check the luminance ranges.",
                       { ChoiceOption("result", "Result", ""), ChoiceOption("shadowsMask", "Shadows Mask", ""),
                         ChoiceOption("midtonesMask", "Midtones Mask", ""), ChoiceOption("highlightsMask", "Highlights Mask", "") },
                       eOutputResult);
} // ColorBalanceRGBOp::createKnobs

std::vector<KnobDoubleWPtr>
ColorBalanceRGBOp::doubleKnobs() const
{
    std::vector<KnobDoubleWPtr> knobs = {
        hueShift,
        vibrance,
        contrast,
        chromaGlobal,
        chromaShadows,
        chromaMidtones,
        chromaHighlights,
        saturationGlobal,
        saturationShadows,
        saturationMidtones,
        saturationHighlights,
        brillianceGlobal,
        brillianceShadows,
        brillianceMidtones,
        brillianceHighlights,
        shadowsWeight,
        maskGreyFulcrum,
        highlightsWeight,
        whiteFulcrum,
        contrastGreyFulcrum,
    };

    for (const WheelKnobs& wheel : wheels) {
        knobs.push_back(wheel.luminance);
        knobs.push_back(wheel.hue);
        knobs.push_back(wheel.chroma);
    }

    return knobs;
}

ColorBalanceRGBOp::Params
ColorBalanceRGBOp::bake(const KnobReader& reader) const
{
    Params p;
    const PipelineMatrices& pm = pipelineMatrices();

    p.output = std::clamp((int)reader.value(output), (int)eOutputResult, (int)eOutputHighlightsMask);
    p.saturationFormula = (reader.value(saturationFormula) == eSaturationFormulaJzAzBz) ? eSaturationFormulaJzAzBz : eSaturationFormulaUcs;
    p.inputMatrix = pm.input;
    p.outputMatrix = pm.output;
    p.gamutLut = gamutLut(p.saturationFormula);
    resolveWorkingSpace(holder, &p);

    // commit_params()
    p.vibrance = (float)reader.value(vibrance);
    p.contrast = 1.f + (float)reader.value(contrast);
    p.greyFulcrum = (float)reader.value(contrastGreyFulcrum);
    p.chromaGlobal = (float)reader.value(chromaGlobal);
    p.chroma[0] = (float)reader.value(chromaShadows);
    p.chroma[1] = (float)reader.value(chromaMidtones);
    p.chroma[2] = (float)reader.value(chromaHighlights);
    p.saturationGlobal = (float)reader.value(saturationGlobal);
    p.saturation[0] = (float)reader.value(saturationShadows);
    p.saturation[1] = (float)reader.value(saturationMidtones);
    p.saturation[2] = (float)reader.value(saturationHighlights);
    p.brillianceGlobal = (float)reader.value(brillianceGlobal);
    p.brilliance[0] = (float)reader.value(brillianceShadows);
    p.brilliance[1] = (float)reader.value(brillianceMidtones);
    p.brilliance[2] = (float)reader.value(brillianceHighlights);
    const float hueAngle = degToRad((float)reader.value(hueShift));
    p.hueCos = std::cos(hueAngle);
    p.hueSin = std::sin(hueAngle);

    const float ychNorm[4] = { 1.f, 0.f, 1.f, 0.f };
    float rgbNorm[3];
    ychToGradingRgb(ychNorm, rgbNorm);
    const auto wheelRgb = [&](const WheelKnobs& wheel,
                              float rgb[3]) {
        float ych[4];
        makeYch(1.f, (float)reader.value(wheel.chroma), degToRad((float)reader.value(wheel.hue) + kAngleShift), ych);
        ychToGradingRgb(ych, rgb);
    };
    float rgb[3];

    wheelRgb(wheels[eWheelOffset], rgb);
    const float offsetY = (float)reader.value(wheels[eWheelOffset].luminance);
    for (int c = 0; c < 3; ++c) {
        p.global[c] = (rgb[c] - rgbNorm[c]) + rgbNorm[c] * offsetY;
    }
    wheelRgb(wheels[eWheelLift], rgb);
    const float liftY = (float)reader.value(wheels[eWheelLift].luminance);
    for (int c = 0; c < 3; ++c) {
        p.shadows[c] = 1.f + (rgb[c] - rgbNorm[c]) + liftY;
    }
    p.shadowsWeight = 2.f + (float)reader.value(shadowsWeight) * 2.f;
    wheelRgb(wheels[eWheelGain], rgb);
    const float gainY = (float)reader.value(wheels[eWheelGain].luminance);
    for (int c = 0; c < 3; ++c) {
        p.highlights[c] = 1.f + (rgb[c] - rgbNorm[c]) + gainY;
    }
    p.highlightsWeight = 2.f + (float)reader.value(highlightsWeight) * 2.f;
    wheelRgb(wheels[eWheelPower], rgb);
    for (int c = 0; c < 3; ++c) {
        p.midtones[c] = 1.f / (1.f + (rgb[c] - rgbNorm[c]));
    }
    p.midtonesY = 1.f / (1.f + (float)reader.value(wheels[eWheelPower].luminance));
    p.whiteFulcrum = std::exp2((float)reader.value(whiteFulcrum));
    p.midtonesWeight = square(p.shadowsWeight) * square(p.highlightsWeight) / (square(p.shadowsWeight) + square(p.highlightsWeight));
    p.maskGreyFulcrum = std::pow((float)reader.value(maskGreyFulcrum), kMaskExponent);
    p.lWhite = yToUcsLStar(p.whiteFulcrum);

    return p;
} // ColorBalanceRGBOp::bake

bool
ColorBalanceRGBOp::isIdentity(const Params& /*params*/) const
{
    // Even at its defaults darktable clips negative Rec.2020 and gamut-maps the saturation.
    return false;
}

void
ColorBalanceRGBOp::pixel(const Params& d,
                         const float in[3],
                         float out[3],
                         float opacities[3])
{
    float rgb[3] = { std::fmax(in[0], 0.f), std::fmax(in[1], 0.f), std::fmax(in[2], 0.f) };
    float lms[3];
    float yrg[3];
    float ych[4];

    apply(d.inputMatrix, rgb, lms);
    lmsToYrg(lms, yrg);
    yrgToYch(yrg, ych);
    ych[0] = std::fmax(ych[0], 0.f);

    float comp[3];
    opacityMasks(std::pow(ych[0], kMaskExponent), d.shadowsWeight, d.highlightsWeight, d.midtonesWeight, d.maskGreyFulcrum, opacities, comp);

    const float cosH = ych[2];
    const float sinH = ych[3];
    ych[2] = d.hueCos * cosH - d.hueSin * sinH;
    ych[3] = d.hueSin * cosH + d.hueCos * sinH;

    const float chromaBoost = d.chromaGlobal + dot3(opacities, d.chroma);
    const float vibrance = d.vibrance * (1.0f - std::pow(ych[1], std::fabs(d.vibrance)));
    const float chromaFactor = std::fmax(1.f + chromaBoost + vibrance, 0.f);
    ych[1] *= chromaFactor;
    gamutCheckYrg(ych);
    ychToYrg(ych, yrg);
    yrgToLms(yrg, lms);
    lmsToGradingRgb(lms, rgb);

    for (int c = 0; c < 3; ++c) {
        rgb[c] += d.global[c];
    }
    for (int c = 0; c < 3; ++c) {
        rgb[c] *= comp[2] * (comp[0] + opacities[0] * d.shadows[c]) + opacities[2] * d.highlights[c];
    }
    for (int c = 0; c < 3; ++c) {
        const float sign = (rgb[c] < 0.f) ? -1.f : 1.f;
        rgb[c] = std::pow(std::fabs(rgb[c]) / d.whiteFulcrum, d.midtones[c]) * sign * d.whiteFulcrum;
    }

    gradingRgbToLms(rgb, lms);
    lmsToYrg(lms, yrg);
    yrg[0] = std::pow(std::fmax(yrg[0] / d.whiteFulcrum, 0.f), d.midtonesY) * d.whiteFulcrum;
    yrg[0] = d.greyFulcrum * std::pow(yrg[0] / d.greyFulcrum, d.contrast);
    yrgToLms(yrg, lms);
    float xyz[3];
    lmsToXyz(lms, xyz);

    if (d.saturationFormula == eSaturationFormulaJzAzBz) {
        float jab[3];
        xyzToJzAzBz(xyz, jab);
        float jc[2] = { jab[0], fastHypot(jab[1], jab[2]) };
        const float h = std::atan2(jab[2], jab[1]);
        const float t = std::atan2(jc[1], jc[0]);
        const float sinT = std::sin(t);
        const float cosT = std::cos(t);
        const float boosts[2] = { 1.f + d.brillianceGlobal + dot3(opacities, d.brilliance),
                                  d.saturationGlobal + dot3(opacities, d.saturation) };
        float so[2];
        so[0] = jc[0] * cosT + jc[1] * sinT;
        so[1] = so[0] * std::fmin(std::fmax(t * boosts[1], -t), kHalfPi - t);
        so[0] = std::fmax(so[0] * boosts[0], 0.f);
        jc[0] = std::fmax(so[0] * cosT - so[1] * sinT, 0.f);
        jc[1] = std::fmax(so[0] * sinT + so[1] * cosT, 0.f);

        const float outMaxSatH = lookupGamut(d.gamutLut, h);
        const float sat = (jc[0] > 0.f) ? softClip(jc[1] / jc[0], 0.8f * outMaxSatH, outMaxSatH) : outMaxSatH;
        const float maxCAtSat = jc[0] * sat;
        const float maxJAtSat = (sat > 0.f) ? jc[1] / sat : jc[0];
        jc[0] = (jc[0] + maxJAtSat) / 2.f;
        jc[1] = (jc[1] + maxCAtSat) / 2.f;

        // The largest chroma at this hue and lightness that keeps L'M'S' non-negative.
        const float cosHue = std::cos(h);
        const float sinHue = std::sin(h);
        const float iz = jzToIz(jc[0]);
        using JzAzBzConstants::kAI;
        float maxC = jc[1];
        for (int i = 0; i < 3; ++i) {
            const float l = iz + jc[1] * cosHue * kAI[1][i] + jc[1] * sinHue * kAI[2][i];
            if (l < 0.f) {
                maxC = std::fmin(-iz / (kAI[1][i] * cosHue + kAI[2][i] * sinHue), maxC);
            }
        }
        jab[0] = jc[0];
        jab[1] = maxC * cosHue;
        jab[2] = maxC * sinHue;
        jzAzBzToXyz(jab, xyz);
    } else {
        float xyY[3];
        float jch[3];
        float hcb[3];
        d65XyzToXyY(xyz, xyY);
        xyYToUcsJCH(xyY, d.lWhite, jch);
        ucsJCHToHCB(jch, hcb);
        const float radius = fastHypot(hcb[1], hcb[2]);
        const float sinT = (radius > 0.f) ? hcb[1] / radius : 0.f;
        const float cosT = (radius > 0.f) ? hcb[2] / radius : 0.f;
        const float pp = std::fmax(FLT_MIN, hcb[1]);
        const float w = sinT * hcb[1] + cosT * hcb[2];
        float a = std::fmax(1.f + d.saturationGlobal + dot3(opacities, d.saturation), 0.f);
        const float b = std::fmax(1.f + d.brillianceGlobal + dot3(opacities, d.brilliance), 0.f);
        const float maxA = fastHypot(pp, w) / pp;
        a = softClip(a, 0.5f * maxA, maxA);
        const float pPrime = (a - 1.f) * pp;
        const float wPrime = std::sqrt(square(pp) * (1.f - square(a)) + square(w)) * b;
        hcb[1] = std::fmax(cosT * pPrime + sinT * wPrime, 0.f);
        hcb[2] = std::fmax(-sinT * pPrime + cosT * wPrime, 0.f);
        ucsHCBToJCH(hcb, jch);

        // The gamut LUT holds M², the squared colourfulness.
        const float maxColorfulness = lookupGamut(d.gamutLut, jch[2]);
        const float maxChroma = ucsChroma(jch[0] * d.lWhite, d.lWhite, maxColorfulness);
        const float boundaryJch[3] = { jch[0], maxChroma, jch[2] };
        float boundaryHsb[3];
        ucsJCHToHSB(boundaryJch, boundaryHsb);
        float hsb[3] = { hcb[0], (hcb[2] > 0.f) ? hcb[1] / hcb[2] : 0.f, hcb[2] };
        hsb[1] = softClip(hsb[1], 0.8f * boundaryHsb[1], boundaryHsb[1]);
        ucsHSBToJCH(hsb, jch);
        ucsJCHToXyY(jch, d.lWhite, xyY);
        xyYToXyz(xyY, xyz);
    }

    apply(d.outputMatrix, xyz, out);
    for (int c = 0; c < 3; ++c) {
        out[c] = std::fmax(out[c], 0.f);
    }
} // ColorBalanceRGBOp::pixel

void
ColorBalanceRGBOp::row(const Params& params,
                       const RowIO& io) const
{
    const int nComps = io.nComps;
    // addAcceptedComponents() admits RGB and RGBA only.
    if (nComps < 3) {
        std::copy(io.src[0], io.src[0] + io.width * nComps, io.dst);

        return;
    }
    bool process[3] = { false, false, false };

    for (int c = 0; c < 3; ++c) {
        process[c] = io.channels[pixelKernelChannelBit(nComps, c)];
    }
    const bool viaOcio = !params.matrixWorkingSpace;
    const bool result = (params.output == eOutputResult);
    float buffer[kChunk * 3];
    std::string error;

    for (int x0 = 0; x0 < io.width; x0 += kChunk) {
        const int n = std::min(kChunk, io.width - x0);
        const float* src = io.src[0] + (size_t)x0 * nComps;
        float* dst = io.dst + (size_t)x0 * nComps;
        for (int i = 0; i < n; ++i) {
            for (int c = 0; c < 3; ++c) {
                buffer[i * 3 + c] = src[i * nComps + c];
            }
        }
        if (viaOcio && params.toXyz) {
            (void)IoColorSpace::convertRow(*params.toXyz, buffer, n, 3, &error);
        }
        for (int i = 0; i < n; ++i) {
            float* px = buffer + i * 3;
            float pipe[3];
            float graded[3];
            float opacities[3];
            apply(params.toPipe, px, pipe);
            pixel(params, pipe, graded, opacities);
            if (result) {
                apply(params.fromPipe, graded, px);
            } else {
                const float grey = opacities[params.output - eOutputShadowsMask];
                px[0] = px[1] = px[2] = grey;
            }
        }
        if (viaOcio && result && params.fromXyz) {
            (void)IoColorSpace::convertRow(*params.fromXyz, buffer, n, 3, &error);
        }
        for (int i = 0; i < n; ++i) {
            for (int c = 0; c < 3; ++c) {
                if (process[c]) {
                    dst[i * nComps + c] = buffer[i * 3 + c];
                }
            }
        }
    }
} // ColorBalanceRGBOp::row

#ifdef NATRON_HAVE_GPU
namespace {
// The parameter block colorbalancergb.slang reads, in words; matrices row-major.
const int kGpuToPipe = 0;
const int kGpuFromPipe = 9;
const int kGpuInputMatrix = 18;
const int kGpuOutputMatrix = 27;
const int kGpuOutput = 36;
const int kGpuFormula = 37;
const int kGpuGlobal = 38;
const int kGpuShadows = 41;
const int kGpuHighlights = 44;
const int kGpuMidtones = 47;
const int kGpuChroma = 50;
const int kGpuSaturation = 53;
const int kGpuBrilliance = 56;
const int kGpuScalars = 59;
const int kGpuScalarCount = 15;
const int kGpuLut = 80;
const int kGpuTotalWords = kGpuLut + kGamutLutSize;

static_assert(kGpuScalars + kGpuScalarCount <= kGpuLut, "the scalars end before the gamut LUT");
static_assert(kGpuTotalWords * 4 <= 65536, "the parameter block fits a packed op");

void
putFloats(std::vector<std::uint32_t>& words,
          int index,
          const float* values,
          int count)
{
    std::memcpy(&words[index], values, count * sizeof(float));
}

void
putMat3(std::vector<std::uint32_t>& words,
        int index,
        const Mat3& m)
{
    for (int r = 0; r < 3; ++r) {
        putFloats(words, index + r * 3, m.m[r], 3);
    }
}
} // anonymous namespace
#endif // NATRON_HAVE_GPU

GpuOpRef
ColorBalanceRGBOp::gpu(const Params& params) const
{
#ifdef NATRON_HAVE_GPU
    // An OCIO working space converts on the CPU only.
    if (!params.matrixWorkingSpace || !params.gamutLut) {
        return GpuOpRef::none();
    }
    std::vector<std::uint32_t> words(kGpuTotalWords, 0);
    putMat3(words, kGpuToPipe, params.toPipe);
    putMat3(words, kGpuFromPipe, params.fromPipe);
    putMat3(words, kGpuInputMatrix, params.inputMatrix);
    putMat3(words, kGpuOutputMatrix, params.outputMatrix);
    words[kGpuOutput] = (std::uint32_t)params.output;
    words[kGpuFormula] = (params.saturationFormula == eSaturationFormulaJzAzBz) ? 0u : 1u;
    putFloats(words, kGpuGlobal, params.global, 3);
    putFloats(words, kGpuShadows, params.shadows, 3);
    putFloats(words, kGpuHighlights, params.highlights, 3);
    putFloats(words, kGpuMidtones, params.midtones, 3);
    putFloats(words, kGpuChroma, params.chroma, 3);
    putFloats(words, kGpuSaturation, params.saturation, 3);
    putFloats(words, kGpuBrilliance, params.brilliance, 3);
    const float scalars[kGpuScalarCount] = {
        params.chromaGlobal,
        params.saturationGlobal,
        params.brillianceGlobal,
        params.vibrance,
        params.contrast,
        params.greyFulcrum,
        params.whiteFulcrum,
        params.lWhite,
        params.midtonesY,
        params.shadowsWeight,
        params.highlightsWeight,
        params.midtonesWeight,
        params.maskGreyFulcrum,
        params.hueCos,
        params.hueSin,
    };
    putFloats(words, kGpuScalars, scalars, kGpuScalarCount);
    putFloats(words, kGpuLut, params.gamutLut, kGamutLutSize);
    GpuOpRef ref = GpuOpRef::packed((int)gpu::GpuKernelId::colorbalancergb_wrapped, std::move(words));
    ref.acceptedComponents = GpuOpRef::componentsBit(3) | GpuOpRef::componentsBit(4);

    return ref;
#else
    Q_UNUSED(params);

    return GpuOpRef::none();
#endif
}

ColorBalanceRGB::ColorBalanceRGB(NodePtr node)
    : ImageNode<ColorBalanceRGBOp>(node)
{
}

ColorBalanceRGB::~ColorBalanceRGB()
{
}

ColorBalanceRGBOp::Params
ColorBalanceRGB::bakeColorBalanceRGB(double time,
                                     ViewIdx view) const
{
    return bakeParams(op(), time, view);
}

void
ColorBalanceRGB::colorBalanceRGBRow(const ColorBalanceRGBOp::Params& params,
                                    const RowIO& io) const
{
    op().row(params, io);
}

void
ColorBalanceRGB::addAcceptedComponents(int inputNb,
                                       std::list<ImageLayerDesc>* comps)
{
    if (isInputMask(inputNb)) {
        StagedImageEffect::addAcceptedComponents(inputNb, comps);

        return;
    }
    comps->push_back(ImageLayerDesc::getRGBAComponents());
    comps->push_back(ImageLayerDesc::getRGBComponents());
}

void
ColorBalanceRGB::applyPresetAt(int index)
{
    if ((index < 0) || (index >= kPresetCount)) {
        return;
    }
    const ColorBalanceRGBOp& o = op();
    const ColorBalanceRGBPreset& entry = kPresets[index];
    AppInstancePtr app = getApp();
    const bool oneUndoStep = app && !app->isCreatingPythonGroup();

    if (oneUndoStep) {
        setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOnCreateNewCommand);
    }
    const auto set = [](const KnobDoubleWPtr& weak,
                        double v) {
        KnobDoublePtr knob = weak.lock();
        if (knob && (knob->getValue() != v)) {
            knob->setValue(v);
        }
    };
    for (const KnobDoubleWPtr& weak : o.doubleKnobs()) {
        if (KnobDoublePtr knob = weak.lock()) {
            set(weak, knob->getDefaultValue(0));
        }
    }
    set(o.chromaGlobal, entry.chromaGlobal);
    set(o.saturationGlobal, entry.saturationGlobal);
    set(o.saturationShadows, entry.saturationShadows);
    set(o.saturationMidtones, entry.saturationMidtones);
    set(o.saturationHighlights, entry.saturationHighlights);
    if (KnobChoicePtr formula = o.saturationFormula.lock()) {
        if (formula->getValue() != entry.saturationFormula) {
            formula->setValue(entry.saturationFormula);
        }
    }
    if (oneUndoStep) {
        setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOff);
    }
}

bool
ColorBalanceRGB::knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread)
{
    const KnobButtonPtr applyButton = op().applyPreset.lock();

    if (applyButton && (k == applyButton.get())) {
        const KnobChoicePtr choice = op().preset.lock();
        if (choice) {
            applyPresetAt(choice->getValue());
        }

        return true;
    }

    return ImageNode<ColorBalanceRGBOp>::knobChanged(k, reason, view, time, originatedFromMainThread);
}

NATRON_NAMESPACE_EXIT
