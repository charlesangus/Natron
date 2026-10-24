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
 * The parameters and interface of darktable's color calibration module, DARKTABLE_REF
 * release-5.6.2 (commit 20891f6c6fa7), src/iop/channelmixerrgb.c, Copyright (C) 2020-2026
 * darktable developers, distributed under the GNU GPL version 3 or later.
 */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "RawColorCalibration.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <optional>
#include <vector>

#include <QStringList>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Raw/RawColorMath.h"
#include "Engine/Nodes/Raw/RawTemperature.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"

NATRON_NAMESPACE_ENTER

using namespace RawColorCalibrationMath;
using CalibrationSettings = RawColorCalibrationMath::Settings;

namespace {
struct IlluminantOption {
    Illuminant illuminant;
    const char* id;
};

// The illuminants darktable's combobox offers, the camera's last: reload_defaults() appends it for
// a raw image only. The detection modes need darktable's AI build.
const IlluminantOption kIlluminantOptions[] = {
    { Illuminant::Pipe, "pipe" },
    { Illuminant::A, "a" },
    { Illuminant::D, "d" },
    { Illuminant::E, "e" },
    { Illuminant::F, "f" },
    { Illuminant::Led, "led" },
    { Illuminant::BlackBody, "blackBody" },
    { Illuminant::Custom, "custom" },
    { Illuminant::Camera, "camera" },
};

const int kIlluminantCount = (int)(sizeof(kIlluminantOptions) / sizeof(kIlluminantOptions[0]));

std::vector<ChoiceOption>
illuminantChoiceOptions(bool withCamera)
{
    std::vector<ChoiceOption> options;

    for (const IlluminantOption& option : kIlluminantOptions) {
        if (withCamera || (option.illuminant != Illuminant::Camera)) {
            options.push_back(ChoiceOption(option.id, EffectInstance::tr(illuminantLabel(option.illuminant)).toStdString(), std::string()));
        }
    }

    return options;
}

const char* const kAdaptationIds[] = { "linearBradford", "cat16", "nonLinearBradford", "xyz", "none" };
const char* const kFluorescentIds[] = { "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12" };
const char* const kLedIds[] = { "b1", "b2", "b3", "b4", "b5", "bh1", "rgb1", "v1", "v2" };

struct RowSpec {
    const char* name;
    const char* tab;
    const char* tabLabel;
    const char* tabHint;
    double minimum;
    double maximum;
    double displayMinimum;
    double displayMaximum;
};

const RowSpec kRows[eRawColorCalibrationRowCount] = {
    { kRawColorCalibrationParamRed, kRawColorCalibrationTabRed, "R", "Output R.", -2., 2., -2., 2. },
    { kRawColorCalibrationParamGreen, kRawColorCalibrationTabGreen, "G", "Output G.", -2., 2., -2., 2. },
    { kRawColorCalibrationParamBlue, kRawColorCalibrationTabBlue, "B", "Output B.", -2., 2., -2., 2. },
    { kRawColorCalibrationParamSaturation, kRawColorCalibrationTabColorfulness, "Colorfulness", "Output colorfulness.", -2., 2., -1., 1. },
    { kRawColorCalibrationParamLightness, kRawColorCalibrationTabBrightness, "Brightness", "Output brightness.", -2., 2., -1., 1. },
    { kRawColorCalibrationParamGrey, kRawColorCalibrationTabGray, "Gray", "Output gray.", -2., 2., 0., 1. },
};

const char* const kRowHints[eRawColorCalibrationRowCount] = {
    "The weight of input %1 in output R. In the adaptation's space (LMS, XYZ, or the working RGB when the adaptation is none).",
    "The weight of input %1 in output G. In the adaptation's space (LMS, XYZ, or the working RGB when the adaptation is none).",
    "The weight of input %1 in output B. In the adaptation's space (LMS, XYZ, or the working RGB when the adaptation is none).",
    "How much channel %1 drives the colorfulness change: positive values make colours more colourful, negative ones less.",
    "How much channel %1 adds to or takes from the brightness.",
    "The weight of channel %1 in the gray output. Any non-zero weight turns the image monochrome.",
};

const char* const kChannelNames[3] = { "R", "G", "B" };

const char* const kGreyNormalizationDisabled = QT_TRANSLATE_NOOP("Natron::EffectInstance", "color calibration: the sum of the gray channel parameters is zero, normalization will be disabled.");
const char* const kDoubleCat = QT_TRANSLATE_NOOP("Natron::EffectInstance", "double CAT applied: a RawColorCalibration upstream already provides chromatic adaptation. "
                                                                           "This can lead to inconsistencies unless you know what you are doing.");
const char* const kWhiteBalanceTwice = QT_TRANSLATE_NOOP("Natron::EffectInstance", "white balance applied twice: RawTemperature is not using the camera reference illuminant, "
                                                                                   "which will cause issues here with chromatic adaptation. Either set it to Camera Reference "
                                                                                   "(or As Shot to Reference) or disable chromatic adaptation here.");

using Coeffs = std::array<double, 3>;

// dt_dev_equal_chroma(): equal within 1e-5 per channel, compared in float.
bool
equalChroma(const Coeffs& a,
            const Coeffs& b)
{
    for (int c = 0; c < 3; ++c) {
        if (!(std::fabs((float)a[c] - (float)b[c]) < 0.00001f)) {
            return false;
        }
    }

    return true;
}

Coeffs
toCoeffs(const RawColorMath::Vec3& v)
{
    return Coeffs { v[0], v[1], v[2] };
}

// chroma D65coeffs: what RawTemperature published, else what it would publish.
Coeffs
d65Coeffs(const ImageMetadata& source)
{
    if (const std::optional<Coeffs> d65 = RawMetadata::getWbD65Coeffs(source)) {
        return *d65;
    }

    return toCoeffs(RawTemperatureOp::modeMultipliers(eRawTemperatureModeCameraReference, source));
}

// chroma wb_coeffs: the multipliers RawTemperature applied, 1 without one. Once RawInputColor has
// applied the late correction it rewrites raw/wb_applied to the D65 multipliers, where darktable's
// chroma keeps the white balance module's: D65 / late correction.
Coeffs
chromaWbCoeffs(const ImageMetadata& source)
{
    const std::optional<Coeffs> applied = RawMetadata::getWbApplied(source);

    if (!applied) {
        return Coeffs { 1., 1., 1. };
    }
    const std::optional<Coeffs> late = RawMetadata::getWbLateCorrection(source);
    const std::optional<Coeffs> d65 = RawMetadata::getWbD65Coeffs(source);
    if (late && d65 && equalChroma(*applied, *d65)) {
        Coeffs wb = *applied;
        for (int c = 0; c < 3; ++c) {
            if ((*late)[c] != 0.) {
                wb[c] = (*d65)[c] / (*late)[c];
            }
        }

        return wb;
    }

    return *applied;
}

// _dev_is_D65_chroma().
bool
isD65Chroma(const ImageMetadata& source)
{
    const Coeffs wb = chromaWbCoeffs(source);

    if (RawMetadata::getWbLateCorrection(source)) {
        return equalChroma(wb, toCoeffs(RawTemperatureOp::modeMultipliers(eRawTemperatureModeAsShot, source)));
    }

    return equalChroma(wb, d65Coeffs(source));
}

// The image fields find_temperature_from_raw_coeffs() reads: the as-shot coefficients and the
// XYZ to camera matrices. raw/cam_to_xyz is the inverse of darktable's adobe_XYZ_to_CAM.
RawCameraWhite
cameraWhite(const ImageMetadata& source)
{
    RawCameraWhite camera;

    camera.matrixCorrectionSupported = rawColorCalibrationIsRawSource(source);
    if (const std::optional<Coeffs> wb = RawMetadata::getWbCoeffs(source)) {
        camera.wbCoeffs = { (float)(*wb)[0], (float)(*wb)[1], (float)(*wb)[2], 0.f };
    }
    if (const std::optional<std::array<double, 9>> dng = RawMetadata::getDngD65Matrix(source)) {
        Mat3 m;
        for (int i = 0; i < 9; ++i) {
            m[i] = (float)(*dng)[i];
        }
        camera.d65ColorMatrix = m;
    }
    if (const std::optional<std::array<double, 9>> camToXyz = RawMetadata::getCamToXyz(source)) {
        if (const std::optional<RawColorMath::Mat3> xyzToCam = RawColorMath::invert(*camToXyz)) {
            Mat3 m;
            for (int i = 0; i < 9; ++i) {
                m[i] = (float)(*xyzToCam)[i];
            }
            camera.adobeXyzToCam = m;
        }
    }

    return camera;
}

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
          const std::string& name,
          const QString& label,
          const QString& hint,
          double defaultValue,
          double minimum,
          double maximum,
          double displayMinimum,
          double displayMaximum)
{
    KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(holder, label.toStdString());

    knob->setName(name);
    knob->setHintToolTip(hint.toStdString());
    knob->setDefaultValue(defaultValue);
    knob->setMinimum(minimum);
    knob->setMaximum(maximum);
    knob->setDisplayMinimum(displayMinimum);
    knob->setDisplayMaximum(displayMaximum);
    tab->addKnob(knob);

    return knob;
}

KnobBoolPtr
addBool(KnobHolder* holder,
        const KnobGroupPtr& tab,
        const std::string& name,
        const QString& label,
        const QString& hint,
        bool defaultValue)
{
    KnobBoolPtr knob = AppManager::createKnob<KnobBool>(holder, label.toStdString());

    knob->setName(name);
    knob->setHintToolTip(hint.toStdString());
    knob->setDefaultValue(defaultValue);
    knob->setAnimationEnabled(false);
    tab->addKnob(knob);

    return knob;
}

// The hue in degrees and the chroma of an illuminant chromaticity, dt_xyY_to_Lch() at Y = 1.
Vec3
illuminantLch(float x,
              float y)
{
    return xyYToLch({ x, y, 1.f });
}

float
radiansToDegrees(float radians)
{
    return radians * 180.f / 3.14159265358979323846f;
}

float
degreesToRadians(float degrees)
{
    return degrees * 3.14159265358979323846f / 180.f;
}

template <typename E>
E
enumAt(int index,
       E last,
       E fallback)
{
    return (index >= 0 && index < static_cast<int>(last)) ? static_cast<E>(index) : fallback;
}
} // namespace

Illuminant
rawColorCalibrationIlluminantAt(int index)
{
    return (index >= 0 && index < kIlluminantCount) ? kIlluminantOptions[index].illuminant : Illuminant::D;
}

int
rawColorCalibrationIlluminantIndex(Illuminant illuminant)
{
    for (int i = 0; i < kIlluminantCount; ++i) {
        if (kIlluminantOptions[i].illuminant == illuminant) {
            return i;
        }
    }

    return -1;
}

RawCameraWhite
rawColorCalibrationCameraWhite(const ImageMetadata& source)
{
    return cameraWhite(source);
}

bool
rawColorCalibrationIsRawSource(const ImageMetadata& source)
{
    return RawMetadata::getWbCoeffs(source) || RawMetadata::getCamToXyz(source) || RawMetadata::getDngD65Matrix(source) || RawMetadata::getMake(source);
}

bool
rawColorCalibrationWhiteBalanceRatio(const ImageMetadata& source,
                                     Vec4* customWb)
{
    *customWb = { 1.f, 1.f, 1.f, 1.f };
    if (!rawColorCalibrationIsRawSource(source)) {
        return true;
    }
    if (isD65Chroma(source)) {
        return false;
    }
    const Coeffs d65 = d65Coeffs(source);
    const Coeffs wb = chromaWbCoeffs(source);
    const bool valid = (d65[0] > 0.) && (d65[1] > 0.) && (d65[2] > 0.);
    const bool changed = ((float)wb[0] > 1.f) || ((float)wb[1] > 1.f) || ((float)wb[2] > 1.f);
    // A white balance other than the reference, a preset say: the ratio that takes it to D65.
    if (valid && changed) {
        for (int c = 0; c < 3; ++c) {
            (*customWb)[c] = (float)d65[c] / (float)wb[c];
        }
    }

    return false;
}

RawColorCalibrationSourceDefaults
rawColorCalibrationSourceDefaults(const ImageMetadata& source)
{
    const CalibrationSettings defaults;
    RawColorCalibrationSourceDefaults d = { defaults.illuminant, defaults.adaptation, defaults.temperature, defaults.x, defaults.y };

    // darktable also falls back to the plain channel mixer for a monochrome image, which nothing
    // marks once RawDemosaic has dropped the mosaic's description.
    if (RawMetadata::getCatApplied(source)) {
        d.illuminant = Illuminant::Pipe;
        d.adaptation = Adaptation::Rgb;

        return d;
    }
    d.adaptation = Adaptation::Cat16;
    Vec4 customWb;
    if (!rawColorCalibrationWhiteBalanceRatio(source, &customWb)) {
        if (const std::optional<Xy> xy = findTemperatureFromRawCoeffs(cameraWhite(source), &customWb)) {
            d.x = xy->x;
            d.y = xy->y;
            d.illuminant = Illuminant::Camera;
        }
        const DaylightCheck check = checkIfCloseToDaylight(d.x, d.y);
        d.temperature = check.temperature;
        d.illuminant = check.illuminant;
        d.adaptation = check.adaptation;
    }

    return d;
}

std::string
rawColorCalibrationRowKnobName(RawColorCalibrationRowEnum row,
                               int c)
{
    return std::string(kRows[row].name) + kChannelNames[c];
}

std::string
rawColorCalibrationNormalizeKnobName(RawColorCalibrationRowEnum row)
{
    return std::string(kRows[row].name) + kRawColorCalibrationParamNormalizeSuffix;
}

std::string
rawColorCalibrationCctText(float x,
                           float y)
{
    const DaylightCheck check = checkIfCloseToDaylight(x, y);
    const float t = check.temperature;
    char buffer[64];

    if (!(t > 1667.f && t < 25000.f)) {
        return EffectInstance::tr("CCT: undefined").toStdString();
    }
    std::snprintf(buffer, sizeof(buffer), "%.0f", t);
    const QString temperature = QString::fromUtf8(buffer);
    if (check.illuminant == Illuminant::D) {
        return EffectInstance::tr("CCT: %1 K (daylight)").arg(temperature).toStdString();
    }
    if (check.illuminant == Illuminant::BlackBody) {
        return EffectInstance::tr("CCT: %1 K (black body)").arg(temperature).toStdString();
    }

    return EffectInstance::tr("CCT: %1 K (invalid)").arg(temperature).toStdString();
}

void
RawColorCalibrationOp::createKnobs(KnobHolder* knobHolder,
                                   const KnobPagePtr& page)
{
    holder = knobHolder;
    const CalibrationSettings defaults;

    createPresetKnobs(knobHolder, page);

    KnobGroupPtr cat = addTab(knobHolder, page, kRawColorCalibrationTabCat, "CAT", "Chromatic adaptation transform.");

    catOverride = addBool(knobHolder, cat, kRawColorCalibrationParamCatOverride, EffectInstance::tr("Override File Defaults"),
                          EffectInstance::tr("Use the adaptation, illuminant and temperature below instead of the ones the source calls for: "
                                             "for a raw file, CAT16 on the camera's white balance, as a daylight, a black body or a custom illuminant; "
                                             "pipe and none when a RawColorCalibration upstream already adapts; D 5003 K otherwise."),
                          false);

    std::vector<ChoiceOption> adaptationOptions;
    for (int i = 0; i < static_cast<int>(Adaptation::Last); ++i) {
        adaptationOptions.push_back(ChoiceOption(kAdaptationIds[i], EffectInstance::tr(adaptationLabel(static_cast<Adaptation>(i))).toStdString(), std::string()));
    }
    adaptation = addChoice(knobHolder, cat, kRawColorCalibrationParamAdaptation, "Adaptation",
                           "The method to adapt the illuminant and the colour space in which the node works:\n"
                           "- Linear Bradford (1985) is consistent with the ICC v4 toolchain.\n"
                           "- CAT16 (2016) is more robust and accurate.\n"
                           "- Non-linear Bradford (1985) is the original Bradford; it can produce better results than the linear version, but is unreliable.\n"
                           "- XYZ is a simple scaling in XYZ space. It is not recommended in general.\n"
                           "- None disables any adaptation and uses the working RGB.",
                           adaptationOptions, static_cast<int>(defaults.adaptation));

    KnobStringPtr cct = AppManager::createKnob<KnobString>(knobHolder, std::string());
    cct->setName(kRawColorCalibrationParamApproxCct);
    cct->setHintToolTip(EffectInstance::tr("The approximate correlated colour temperature of the illuminant. "
                                           "Daylight or black body when that spectrum models the illuminant accurately, so that its temperature is meaningful with a D or Planckian illuminant; "
                                           "invalid when neither does, and a custom illuminant is needed.")
                            .toStdString());
    cct->setAsLabel();
    cct->setIsPersistent(false);
    cct->setEvaluateOnChange(false);
    cct->setDefaultValue(rawColorCalibrationCctText(defaults.x, defaults.y));
    cat->addKnob(cct);
    approxCct = cct;

    illuminant = addChoice(knobHolder, cat, kRawColorCalibrationParamIlluminant, "Illuminant",
                           "The illuminant of the scene, which the adaptation turns into D50 white. "
                           "As shot in camera, offered for a raw file, follows the white balance RawTemperature applies, at every render.",
                           illuminantChoiceOptions(true), rawColorCalibrationIlluminantIndex(defaults.illuminant));

    std::vector<ChoiceOption> fluorescentOptions;
    for (int i = 0; i < static_cast<int>(Fluorescent::Last); ++i) {
        fluorescentOptions.push_back(ChoiceOption(kFluorescentIds[i], fluorescentLabel(static_cast<Fluorescent>(i)), std::string()));
    }
    fluorescent = addChoice(knobHolder, cat, kRawColorCalibrationParamFluorescent, "F Source",
                            "The CIE fluorescent illuminant.",
                            fluorescentOptions, static_cast<int>(defaults.fluorescent));

    std::vector<ChoiceOption> ledOptions;
    for (int i = 0; i < static_cast<int>(Led::Last); ++i) {
        ledOptions.push_back(ChoiceOption(kLedIds[i], ledLabel(static_cast<Led>(i)), std::string()));
    }
    led = addChoice(knobHolder, cat, kRawColorCalibrationParamLed, "LED Source",
                    "The CIE LED illuminant.",
                    ledOptions, static_cast<int>(defaults.led));

    KnobDoublePtr temperatureKnob = addDouble(knobHolder, cat, kRawColorCalibrationParamTemperature, EffectInstance::tr("Temperature"),
                                              EffectInstance::tr("The temperature of the illuminant in kelvin, on the daylight locus for D and on the Planckian locus for a black body."),
                                              defaults.temperature, 1667., 25000., 3000., 7000.);
    temperatureKnob->setAnimationEnabled(false);
    temperature = temperatureKnob;

    const Vec3 lch = illuminantLch(defaults.x, defaults.y);
    KnobDoublePtr hueKnob = addDouble(knobHolder, cat, kRawColorCalibrationParamHue, EffectInstance::tr("Hue"),
                                      EffectInstance::tr("The hue of the custom illuminant in degrees, its CIE Luv hue."),
                                      radiansToDegrees(lch[2]), 0., 360., 0., 360.);
    hueKnob->setAnimationEnabled(false);
    // Shown from x and y, which are what a project saves.
    hueKnob->setIsPersistent(false);
    hue = hueKnob;

    KnobDoublePtr chromaKnob = addDouble(knobHolder, cat, kRawColorCalibrationParamChroma, EffectInstance::tr("Chroma"),
                                         EffectInstance::tr("The chroma of the custom illuminant, its CIE Luv chroma at L = 100, in percent."),
                                         lch[1], 0., 300., 0., 100.);
    chromaKnob->setAnimationEnabled(false);
    chromaKnob->setIsPersistent(false);
    chroma = chromaKnob;

    const double unbounded = 1e300;
    KnobDoublePtr xKnob = addDouble(knobHolder, cat, kRawColorCalibrationParamX, EffectInstance::tr("Illuminant x"),
                                    EffectInstance::tr("The CIE 1931 x chromaticity of the illuminant."),
                                    defaults.x, -unbounded, unbounded, 0., 1.);
    xKnob->setAnimationEnabled(false);
    xKnob->setSecret(true);
    x = xKnob;

    KnobDoublePtr yKnob = addDouble(knobHolder, cat, kRawColorCalibrationParamY, EffectInstance::tr("Illuminant y"),
                                    EffectInstance::tr("The CIE 1931 y chromaticity of the illuminant."),
                                    defaults.y, -unbounded, unbounded, 0., 1.);
    yKnob->setAnimationEnabled(false);
    yKnob->setSecret(true);
    y = yKnob;

    gamut = addDouble(knobHolder, cat, kRawColorCalibrationParamGamut, EffectInstance::tr("Gamut Compression"),
                      EffectInstance::tr("Pull the most saturated colours toward white in CIE u'v', more strongly the larger the value, to bring colours the adaptation pushed outside the visible gamut back inside. 0 disables it."),
                      defaults.gamut, 0., 12., 0., 4.);

    clip = addBool(knobHolder, cat, kRawColorCalibrationParamClip, EffectInstance::tr("Clip Negative RGB from Gamut"),
                   EffectInstance::tr("Clip negative values to 0 at every step, in RGB, LMS and XYZ, and keep the gamut compression from producing negative chromaticities."),
                   defaults.clip);

    createSpotKnobs(knobHolder, cat);

    const Vec3* rowDefaults[eRawColorCalibrationRowCount] = { &defaults.red, &defaults.green, &defaults.blue, &defaults.saturation, &defaults.lightness, &defaults.grey };
    const bool normalizeDefaults[eRawColorCalibrationRowCount] = { defaults.normalizeR, defaults.normalizeG, defaults.normalizeB, defaults.normalizeSaturation, defaults.normalizeLightness, defaults.normalizeGrey };
    for (int r = 0; r < eRawColorCalibrationRowCount; ++r) {
        const RowSpec& spec = kRows[r];
        KnobGroupPtr tab = addTab(knobHolder, page, spec.tab, spec.tabLabel, spec.tabHint);
        for (int c = 0; c < 3; ++c) {
            rows[r][c] = addDouble(knobHolder, tab, rawColorCalibrationRowKnobName((RawColorCalibrationRowEnum)r, c),
                                   EffectInstance::tr("Input %1").arg(QString::fromUtf8(kChannelNames[c])),
                                   EffectInstance::tr(kRowHints[r]).arg(QString::fromUtf8(kChannelNames[c])),
                                   (*rowDefaults[r])[c], spec.minimum, spec.maximum, spec.displayMinimum, spec.displayMaximum);
        }
        normalize[r] = addBool(knobHolder, tab, rawColorCalibrationNormalizeKnobName((RawColorCalibrationRowEnum)r), EffectInstance::tr("Normalize Channels"),
                               (r == eRawColorCalibrationRowGrey)                                                          ? EffectInstance::tr("Divide the weights by their sum, so that the gray keeps the brightness of a neutral input.")
                                   : ((r == eRawColorCalibrationRowSaturation) || (r == eRawColorCalibrationRowLightness)) ? EffectInstance::tr("Subtract the weights' mean from each, so that the adjustment shifts the balance between channels without an overall change.")
                                                                                                                           : EffectInstance::tr("Divide the weights by their sum, so that a neutral input stays neutral."),
                               normalizeDefaults[r]);
        if (r == eRawColorCalibrationRowSaturation) {
            std::vector<ChoiceOption> versionOptions;
            versionOptions.push_back(ChoiceOption("v1", EffectInstance::tr("version 1 (2020)").toStdString(), std::string()));
            versionOptions.push_back(ChoiceOption("v2", EffectInstance::tr("version 2 (2021)").toStdString(), std::string()));
            versionOptions.push_back(ChoiceOption("v3", EffectInstance::tr("version 3 (Apr 2021)").toStdString(), std::string()));
            KnobChoicePtr versionKnob = addChoice(knobHolder, tab, kRawColorCalibrationParamVersion, "Saturation Algorithm",
                                                  "The colorfulness algorithm. Version 3 keeps grey pixels grey and the norm of the colour unchanged; the earlier ones are kept for edits made with them.",
                                                  versionOptions, static_cast<int>(defaults.version));
            versionKnob->setSecret(true);
            version = versionKnob;
        }
    }
}

CalibrationSettings
RawColorCalibrationOp::settings(const KnobReader& reader) const
{
    CalibrationSettings s;
    Vec3* rowValues[eRawColorCalibrationRowCount] = { &s.red, &s.green, &s.blue, &s.saturation, &s.lightness, &s.grey };
    bool* normalizeValues[eRawColorCalibrationRowCount] = { &s.normalizeR, &s.normalizeG, &s.normalizeB, &s.normalizeSaturation, &s.normalizeLightness, &s.normalizeGrey };

    for (int r = 0; r < eRawColorCalibrationRowCount; ++r) {
        for (int c = 0; c < 3; ++c) {
            (*rowValues[r])[c] = (float)reader.valueOr(rows[r][c], (double)(*rowValues[r])[c]);
        }
        *normalizeValues[r] = reader.valueOr(normalize[r], *normalizeValues[r]);
    }
    s.illuminant = rawColorCalibrationIlluminantAt(reader.valueOr(illuminant, rawColorCalibrationIlluminantIndex(s.illuminant)));
    s.fluorescent = enumAt(reader.valueOr(fluorescent, static_cast<int>(s.fluorescent)), Fluorescent::Last, s.fluorescent);
    s.led = enumAt(reader.valueOr(led, static_cast<int>(s.led)), Led::Last, s.led);
    s.adaptation = enumAt(reader.valueOr(adaptation, static_cast<int>(s.adaptation)), Adaptation::Last, s.adaptation);
    s.x = (float)reader.valueOr(x, (double)s.x);
    s.y = (float)reader.valueOr(y, (double)s.y);
    s.temperature = (float)reader.valueOr(temperature, (double)s.temperature);
    s.gamut = (float)reader.valueOr(gamut, (double)s.gamut);
    s.clip = reader.valueOr(clip, s.clip);
    s.version = enumAt(reader.valueOr(version, static_cast<int>(s.version)), static_cast<SaturationVersion>(3), s.version);

    return s;
}

CalibrationSettings
RawColorCalibrationOp::effectiveSettings(const KnobReader& reader,
                                         const ImageMetadata& source) const
{
    CalibrationSettings s = settings(reader);

    if (!reader.valueOr(catOverride, false)) {
        const RawColorCalibrationSourceDefaults d = rawColorCalibrationSourceDefaults(source);
        s.illuminant = d.illuminant;
        s.adaptation = d.adaptation;
        s.temperature = d.temperature;
        s.x = d.x;
        s.y = d.y;
    }

    return s;
}

Mat3
RawColorCalibrationOp::workingToXyzD50(const KnobHolder* knobHolder)
{
    RawColorMath::Mat3 toXyz = RawColorMath::srgbToXyz();
    AppInstancePtr app = knobHolder ? knobHolder->getApp() : AppInstancePtr();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    ProjectColorManagementPtr cm = project ? project->getColorManagement() : ProjectColorManagementPtr();
    double workingToXyz[9];

    if (cm && cm->getWorkingToXYZ(workingToXyz)) {
        for (int i = 0; i < 9; ++i) {
            toXyz[i] = workingToXyz[i];
        }
    }
    // darktable's working profile matrix is relative to the D50 profile connection space, the
    // project's to D65.
    const std::optional<RawColorMath::Mat3> d65ToD50 = RawColorMath::bradford(RawColorMath::d65White(), RawColorMath::lcmsD50White());
    const RawColorMath::Mat3 toD50 = d65ToD50 ? RawColorMath::multiply(*d65ToD50, toXyz) : toXyz;
    Mat3 out;
    for (int i = 0; i < 9; ++i) {
        out[i] = (float)toD50[i];
    }

    return out;
}

RawColorCalibrationOp::Params
RawColorCalibrationOp::bake(const KnobReader& reader,
                            const ImageMetadata& source) const
{
    const Mat3 toXyz = workingToXyzD50(holder);
    RawColorMath::Mat3 toXyzDouble;
    for (int i = 0; i < 9; ++i) {
        toXyzDouble[i] = toXyz[i];
    }
    const std::optional<RawColorMath::Mat3> inverse = RawColorMath::invert(toXyzDouble);
    Mat3 fromXyz;
    for (int i = 0; i < 9; ++i) {
        fromXyz[i] = inverse ? (float)(*inverse)[i] : (((i % 4) == 0) ? 1.f : 0.f);
    }

    const CalibrationSettings s = effectiveSettings(reader, source);
    // The camera illuminant is found again at every render, from the white balance upstream.
    const RawCameraWhite camera = cameraWhite(source);
    Vec4 customWb;
    (void)rawColorCalibrationWhiteBalanceRatio(source, &customWb);

    return makeKernel(s, toXyz, fromXyz, &camera, &customWb);
}

bool
RawColorCalibrationOp::isIdentity(const Params& params) const
{
    if ((params.adaptation != Adaptation::Rgb) || params.clip || (params.gamut != 0.f) || params.applyGrey) {
        return false;
    }
    for (int c = 0; c < 3; ++c) {
        if ((params.saturation[c] != 0.f) || (params.lightness[c] != 0.f)) {
            return false;
        }
    }

    // An identity mix leaves the working RGB to XYZ matrix exactly as it was.
    return params.mixToXyz == params.rgbToXyz;
}

void
RawColorCalibrationOp::row(const Params& params,
                           const RowIO& io) const
{
    const float* src = io.src[0];
    float* dst = io.dst;

    for (int i = 0; i < io.width; ++i, src += io.nComps, dst += io.nComps) {
        if (io.nComps < 3) {
            for (int c = 0; c < io.nComps; ++c) {
                dst[c] = src[c];
            }
            continue;
        }
        processPixel(params, src, dst);
        if (io.nComps > 3) {
            dst[3] = src[3];
        }
    }
}

GpuOpRef
RawColorCalibrationOp::gpu(const Params& /*params*/) const
{
    return GpuOpRef::none();
}

RawColorCalibration::RawColorCalibration(NodePtr node)
    : ImageNode<RawColorCalibrationOp>(node)
    , _message()
    , _lastIlluminant(CalibrationSettings().illuminant)
    , _lastDaylightTemperature(CalibrationSettings().temperature)
    , _lastBlackBodyTemperature(CalibrationSettings().temperature)
    , _spotMean()
    , _syncing(false)
{
}

RawColorCalibration::~RawColorCalibration()
{
}

void
RawColorCalibration::refreshVisibility()
{
    const RawColorCalibrationOp& o = op();
    const KnobChoicePtr adaptationKnob = o.adaptation.lock();
    const KnobChoicePtr illuminantKnob = o.illuminant.lock();

    if (!adaptationKnob || !illuminantKnob) {
        return;
    }
    const Adaptation adaptation = enumAt(adaptationKnob->getValue(), Adaptation::Last, Adaptation::Rgb);
    const Illuminant illuminant = rawColorCalibrationIlluminantAt(illuminantKnob->getValue());
    const bool cat = (adaptation != Adaptation::Rgb);
    const bool showTemperature = cat && ((illuminant == Illuminant::D) || (illuminant == Illuminant::BlackBody));
    const bool showFluorescent = cat && (illuminant == Illuminant::F);
    const bool showLed = cat && (illuminant == Illuminant::Led);
    const bool showCustom = cat && (illuminant == Illuminant::Custom);
    const std::pair<KnobIPtr, bool> visibility[] = {
        { illuminantKnob, cat },
        { o.approxCct.lock(), cat },
        { o.temperature.lock(), showTemperature },
        { o.fluorescent.lock(), showFluorescent },
        { o.led.lock(), showLed },
        { o.hue.lock(), showCustom },
        { o.chroma.lock(), showCustom },
    };
    for (const std::pair<KnobIPtr, bool>& entry : visibility) {
        if (entry.first && (entry.first->getIsSecret() == entry.second)) {
            entry.first->setSecret(!entry.second);
        }
    }
}

void
RawColorCalibration::refreshVersionVisibility()
{
    const KnobChoicePtr versionKnob = op().version.lock();

    if (versionKnob) {
        versionKnob->setSecret(versionKnob->getValue() == static_cast<int>(SaturationVersion::V3));
    }
}

void
RawColorCalibration::showIlluminant(bool keepHueChroma)
{
    const RawColorCalibrationOp& o = op();
    const KnobChoicePtr illuminantKnob = o.illuminant.lock();
    const KnobChoicePtr fluorescentKnob = o.fluorescent.lock();
    const KnobChoicePtr ledKnob = o.led.lock();
    const KnobDoublePtr temperatureKnob = o.temperature.lock();
    const KnobDoublePtr xKnob = o.x.lock();
    const KnobDoublePtr yKnob = o.y.lock();
    const KnobDoublePtr hueKnob = o.hue.lock();
    const KnobDoublePtr chromaKnob = o.chroma.lock();
    const KnobStringPtr cctKnob = o.approxCct.lock();

    if (!illuminantKnob || !fluorescentKnob || !ledKnob || !temperatureKnob || !xKnob || !yKnob || !hueKnob || !chromaKnob || !cctKnob) {
        return;
    }
    const bool wasSyncing = _syncing;
    _syncing = true;
    const float px = (float)xKnob->getValue();
    const float py = (float)yKnob->getValue();
    if (!keepHueChroma) {
        const Vec3 lch = illuminantLch(px, py);
        // A grey illuminant has no hue: the slider stays where it was, for when the chroma comes back.
        if (lch[1] > 0.f) {
            hueKnob->setValue(radiansToDegrees(lch[2]));
        }
        chromaKnob->setValue(lch[1]);
    }

    Xy xy = { px, py };
    const ImageMetadata source = getSourceMetadata(getCurrentTime(), ViewIdx(0));
    const RawCameraWhite camera = cameraWhite(source);
    Vec4 customWb;
    (void)rawColorCalibrationWhiteBalanceRatio(source, &customWb);
    const std::optional<Xy> found = illuminantToXy(rawColorCalibrationIlluminantAt(illuminantKnob->getValue()), &camera, &customWb, (float)temperatureKnob->getValue(),
                                                   enumAt(fluorescentKnob->getValue(), Fluorescent::Last, Fluorescent::F3), enumAt(ledKnob->getValue(), Led::Last, Led::B5));
    if (found) {
        xy = *found;
    }
    cctKnob->setValue(rawColorCalibrationCctText(xy.x, xy.y));
    _syncing = wasSyncing;
}

void
RawColorCalibration::syncIlluminant(bool illuminantChanged,
                                    bool temperatureChanged)
{
    const RawColorCalibrationOp& o = op();
    const KnobChoicePtr illuminantKnob = o.illuminant.lock();
    const KnobChoicePtr fluorescentKnob = o.fluorescent.lock();
    const KnobChoicePtr ledKnob = o.led.lock();
    const KnobDoublePtr temperatureKnob = o.temperature.lock();
    const KnobDoublePtr xKnob = o.x.lock();
    const KnobDoublePtr yKnob = o.y.lock();

    if (!illuminantKnob || !fluorescentKnob || !ledKnob || !temperatureKnob || !xKnob || !yKnob) {
        return;
    }
    const KnobChoicePtr adaptationKnob = o.adaptation.lock();
    const Illuminant illuminant = rawColorCalibrationIlluminantAt(illuminantKnob->getValue());
    float temperature = (float)temperatureKnob->getValue();
    float px = (float)xKnob->getValue();
    float py = (float)yKnob->getValue();
    std::optional<Adaptation> adaptation;

    // Leaving the camera illuminant starts from the camera's white balance, and so does choosing
    // it: x and y from the raw coefficients, the temperature and the adaptation that suit them.
    if (illuminantChanged && ((_lastIlluminant == Illuminant::Camera) || (illuminant == Illuminant::Camera)) && (_lastIlluminant != illuminant)) {
        const ImageMetadata source = getSourceMetadata(getCurrentTime(), ViewIdx(0));
        Vec4 customWb;
        (void)rawColorCalibrationWhiteBalanceRatio(source, &customWb);
        if (const std::optional<Xy> xy = findTemperatureFromRawCoeffs(cameraWhite(source), &customWb)) {
            px = xy->x;
            py = xy->y;
        }
        const DaylightCheck check = checkIfCloseToDaylight(px, py);
        temperature = check.temperature;
        adaptation = check.adaptation;
    }
    if (illuminantChanged) {
        if (illuminant == Illuminant::D) {
            temperature = _lastDaylightTemperature;
        } else if (illuminant == Illuminant::BlackBody) {
            temperature = _lastBlackBodyTemperature;
        }
    }
    if (temperatureChanged) {
        if (illuminant == Illuminant::D) {
            _lastDaylightTemperature = temperature;
        } else if (illuminant == Illuminant::BlackBody) {
            _lastBlackBodyTemperature = temperature;
        }
    }
    if ((illuminant != Illuminant::Custom) && (illuminant != Illuminant::Camera)) {
        const std::optional<Xy> found = illuminantToXy(illuminant, nullptr, nullptr, temperature,
                                                       enumAt(fluorescentKnob->getValue(), Fluorescent::Last, Fluorescent::F3), enumAt(ledKnob->getValue(), Led::Last, Led::B5));
        if (found) {
            px = found->x;
            py = found->y;
        }
    }
    if ((illuminant != Illuminant::D) && (illuminant != Illuminant::BlackBody) && (illuminant != Illuminant::Camera)) {
        temperature = checkIfCloseToDaylight(px, py).temperature;
    }

    const bool wasSyncing = _syncing;
    _syncing = true;
    if ((float)xKnob->getValue() != px) {
        xKnob->setValue(px);
    }
    if ((float)yKnob->getValue() != py) {
        yKnob->setValue(py);
    }
    if ((float)temperatureKnob->getValue() != temperature) {
        temperatureKnob->setValue(temperature);
    }
    if (adaptation && adaptationKnob && (adaptationKnob->getValue() != static_cast<int>(*adaptation))) {
        adaptationKnob->setValue(static_cast<int>(*adaptation));
    }
    _syncing = wasSyncing;
    _lastIlluminant = illuminant;
}

void
RawColorCalibration::applyHueChroma()
{
    const RawColorCalibrationOp& o = op();
    const KnobDoublePtr temperatureKnob = o.temperature.lock();
    const KnobDoublePtr xKnob = o.x.lock();
    const KnobDoublePtr yKnob = o.y.lock();
    const KnobDoublePtr hueKnob = o.hue.lock();
    const KnobDoublePtr chromaKnob = o.chroma.lock();

    if (!temperatureKnob || !xKnob || !yKnob || !hueKnob || !chromaKnob) {
        return;
    }
    const Vec3 xyY = lchToXyY({ 100.f, (float)chromaKnob->getValue(), degreesToRadians((float)hueKnob->getValue()) });
    float t = xyToCct(xyY[0], xyY[1]);
    // xyToCct() holds from 3000 K only.
    if (t < 3000.f) {
        t = cctReverseLookup(xyY[0], xyY[1]);
    }

    const bool wasSyncing = _syncing;
    _syncing = true;
    xKnob->setValue(xyY[0]);
    yKnob->setValue(xyY[1]);
    temperatureKnob->setValue(t);
    _syncing = wasSyncing;
}

void
RawColorCalibration::refreshWarnings()
{
    const RawColorCalibrationOp& o = op();
    const KnobBoolPtr normalizeGrey = o.normalize[eRawColorCalibrationRowGrey].lock();
    float grey[3] = { 0.f, 0.f, 0.f };
    bool used = false;
    QStringList warnings;

    for (int c = 0; c < 3; ++c) {
        const KnobDoublePtr knob = o.rows[eRawColorCalibrationRowGrey][c].lock();
        grey[c] = knob ? (float)knob->getValue() : 0.f;
        used = used || (grey[c] != 0.f);
    }
    if (used && normalizeGrey && normalizeGrey->getValue() && (grey[0] + grey[1] + grey[2] == 0.f)) {
        warnings.push_back(tr(kGreyNormalizationDisabled));
    }

    // _set_trouble_messages(). RawTemperature applying a white balance stands for darktable's
    // enabled white balance module, without which darktable posts nothing; so its third
    // problem, a white balance disabled but enabled by default, cannot arise.
    const KnobChoicePtr illuminantKnob = o.illuminant.lock();
    const KnobChoicePtr adaptationKnob = o.adaptation.lock();
    if (getInput(getSourceInput()) && illuminantKnob && adaptationKnob) {
        const ImageMetadata source = getSourceMetadata(getCurrentTime(), ViewIdx(0));
        Illuminant illuminant = rawColorCalibrationIlluminantAt(illuminantKnob->getValue());
        Adaptation adaptation = enumAt(adaptationKnob->getValue(), Adaptation::Last, Adaptation::Rgb);
        const KnobBoolPtr overrideKnob = o.catOverride.lock();
        if (overrideKnob && !overrideKnob->getValue()) {
            const RawColorCalibrationSourceDefaults d = rawColorCalibrationSourceDefaults(source);
            illuminant = d.illuminant;
            adaptation = d.adaptation;
        }
        const bool valid = (illuminant != Illuminant::Pipe) && (adaptation != Adaptation::Rgb);
        const bool temperatureEnabled = RawMetadata::getWbApplied(source).has_value();
        const bool upstreamCat = RawMetadata::getCatApplied(source);
        if (valid && temperatureEnabled && upstreamCat) {
            warnings.push_back(tr(kDoubleCat));
        } else if (valid && temperatureEnabled && !upstreamCat && rawColorCalibrationIsRawSource(source) && !isD65Chroma(source)) {
            warnings.push_back(tr(kWhiteBalanceTwice));
        }
    }

    if (warnings.isEmpty()) {
        _message.withdraw(getNode());
    } else {
        _message.post(getNode(), eMessageTypeWarning, warnings.join(QString::fromUtf8("\n")).toStdString());
    }
}

void
RawColorCalibration::refreshEnabled()
{
    const RawColorCalibrationOp& o = op();
    const KnobBoolPtr overrideKnob = o.catOverride.lock();
    const KnobChoicePtr illuminantKnob = o.illuminant.lock();

    if (!overrideKnob || !illuminantKnob) {
        return;
    }
    const bool overridden = overrideKnob->getValue();
    const KnobIPtr knobs[] = { illuminantKnob, o.temperature.lock(), o.hue.lock(), o.chroma.lock(), o.x.lock(), o.y.lock() };
    for (const KnobIPtr& knob : knobs) {
        if (knob) {
            knob->setAllDimensionsEnabled(overridden);
        }
    }
    // gui_update(): the camera illuminant sets its own adaptation.
    if (const KnobChoicePtr adaptationKnob = o.adaptation.lock()) {
        adaptationKnob->setAllDimensionsEnabled(overridden && (rawColorCalibrationIlluminantAt(illuminantKnob->getValue()) != Illuminant::Camera));
    }
}

void
RawColorCalibration::showSourceDefaults()
{
    const RawColorCalibrationOp& o = op();
    const KnobBoolPtr overrideKnob = o.catOverride.lock();
    const KnobChoicePtr illuminantKnob = o.illuminant.lock();
    const KnobChoicePtr adaptationKnob = o.adaptation.lock();
    const KnobDoublePtr temperatureKnob = o.temperature.lock();
    const KnobDoublePtr xKnob = o.x.lock();
    const KnobDoublePtr yKnob = o.y.lock();

    if (!overrideKnob || !illuminantKnob || !adaptationKnob || !temperatureKnob || !xKnob || !yKnob || overrideKnob->getValue() || !getInput(getSourceInput())) {
        return;
    }
    const RawColorCalibrationSourceDefaults d = rawColorCalibrationSourceDefaults(getSourceMetadata(getCurrentTime(), ViewIdx(0)));
    const bool wasSyncing = _syncing;
    _syncing = true;
    const int illuminantIndex = rawColorCalibrationIlluminantIndex(d.illuminant);
    if (illuminantKnob->getValue() != illuminantIndex) {
        illuminantKnob->setValue(illuminantIndex);
    }
    if (adaptationKnob->getValue() != static_cast<int>(d.adaptation)) {
        adaptationKnob->setValue(static_cast<int>(d.adaptation));
    }
    if ((float)temperatureKnob->getValue() != d.temperature) {
        temperatureKnob->setValue(d.temperature);
    }
    if ((float)xKnob->getValue() != d.x) {
        xKnob->setValue(d.x);
    }
    if ((float)yKnob->getValue() != d.y) {
        yKnob->setValue(d.y);
    }
    _syncing = wasSyncing;
    _lastIlluminant = d.illuminant;
    _lastDaylightTemperature = d.temperature;
    _lastBlackBodyTemperature = d.temperature;
    refreshVisibility();
    showIlluminant();
}

void
RawColorCalibration::refreshCameraOption()
{
    const KnobChoicePtr illuminantKnob = op().illuminant.lock();

    if (!illuminantKnob || !getInput(getSourceInput())) {
        return;
    }
    const int cameraIndex = rawColorCalibrationIlluminantIndex(Illuminant::Camera);
    const bool offered = illuminantKnob->getNumEntries() > cameraIndex;
    const bool wanted = rawColorCalibrationIsRawSource(getSourceMetadata(getCurrentTime(), ViewIdx(0))) || (illuminantKnob->getValue() == cameraIndex);
    if (offered != wanted) {
        illuminantKnob->populateChoices(illuminantChoiceOptions(wanted));
    }
}

ImageMetadata
RawColorCalibration::deriveOutputMetadata(double time,
                                          ViewIdx view)
{
    ImageMetadata metadata = ImageNode<RawColorCalibrationOp>::deriveOutputMetadata(time, view);
    const RawColorCalibrationOp& o = op();
    const KnobBoolPtr overrideKnob = o.catOverride.lock();
    const KnobChoicePtr illuminantKnob = o.illuminant.lock();
    const KnobChoicePtr adaptationKnob = o.adaptation.lock();

    if (!overrideKnob || !illuminantKnob || !adaptationKnob) {
        return metadata;
    }
    Illuminant illuminant;
    Adaptation adaptation;
    if (overrideKnob->getValue()) {
        illuminant = rawColorCalibrationIlluminantAt(illuminantKnob->getValue());
        adaptation = enumAt(adaptationKnob->getValue(), Adaptation::Last, Adaptation::Rgb);
    } else {
        const RawColorCalibrationSourceDefaults d = rawColorCalibrationSourceDefaults(getSourceMetadata(time, view));
        illuminant = d.illuminant;
        adaptation = d.adaptation;
    }
    // The camera illuminant always adapts by CAT16 (_check_if_close_to_daylight()).
    if (illuminant == Illuminant::Camera) {
        adaptation = Adaptation::Cat16;
    }
    if ((illuminant != Illuminant::Pipe) && (adaptation != Adaptation::Rgb)) {
        RawMetadata::setCatApplied(&metadata);
    }

    return metadata;
}

void
RawColorCalibration::onInputChanged(int /*inputNo*/)
{
    refreshSpotDefaults();
    refreshCameraOption();
    showSourceDefaults();
    refreshEnabled();
    showIlluminant();
    refreshWarnings();
}

void
RawColorCalibration::onMetadataRefreshed(const NodeMetadata& metadata)
{
    ImageNode<RawColorCalibrationOp>::onMetadataRefreshed(metadata);
    refreshSpotDefaults();
    refreshCameraOption();
    showSourceDefaults();
    refreshEnabled();
    showIlluminant();
    refreshWarnings();
}

void
RawColorCalibration::onKnobsLoaded()
{
    const KnobDoublePtr temperatureKnob = op().temperature.lock();

    if (temperatureKnob) {
        _lastDaylightTemperature = (float)temperatureKnob->getDefaultValue(0);
        _lastBlackBodyTemperature = _lastDaylightTemperature;
    }
    if (const KnobChoicePtr illuminantKnob = op().illuminant.lock()) {
        _lastIlluminant = rawColorCalibrationIlluminantAt(illuminantKnob->getValue());
    }
    refreshSpotDefaults();
    refreshCameraOption();
    showSourceDefaults();
    refreshVisibility();
    refreshEnabled();
    refreshVersionVisibility();
    showIlluminant();
    refreshWarnings();
}

bool
RawColorCalibration::knobChanged(KnobI* k,
                                 ValueChangedReasonEnum reason,
                                 ViewSpec view,
                                 double time,
                                 bool /*originatedFromMainThread*/)
{
    const RawColorCalibrationOp& o = op();
    const auto is = [k](const auto& weak) {
        const auto knob = weak.lock();

        return knob && (k == knob.get());
    };

    if (is(o.pick)) {
        (void)pickSpot(time, (view.isAll() || view.isCurrent()) ? ViewIdx(0) : ViewIdx(view));

        return true;
    }
    if (is(o.applyPreset)) {
        applyChosenPreset();

        return true;
    }
    if (is(o.spotMode) || is(o.spotUseMixing) || is(o.spotLightness) || is(o.spotHue) || is(o.spotChroma)) {
        onSpotSettingsChanged(reason);

        return true;
    }
    const bool isIlluminant = is(o.illuminant);
    const bool isSource = is(o.fluorescent) || is(o.led);
    const bool isTemperature = is(o.temperature);
    const bool isHueChroma = is(o.hue) || is(o.chroma);
    const bool isXy = is(o.x) || is(o.y);
    const bool isAdaptation = is(o.adaptation);
    const bool isOverride = is(o.catOverride);
    bool isGrey = is(o.normalize[eRawColorCalibrationRowGrey]);
    for (int c = 0; c < 3; ++c) {
        isGrey = isGrey || is(o.rows[eRawColorCalibrationRowGrey][c]);
    }

    if (!isIlluminant && !isSource && !isTemperature && !isHueChroma && !isXy && !isAdaptation && !isGrey && !isOverride) {
        return false;
    }
    if (isGrey) {
        refreshWarnings();

        return true;
    }
    if (isOverride) {
        showSourceDefaults();
        refreshEnabled();
        refreshWarnings();

        return true;
    }
    if (isIlluminant || isAdaptation) {
        refreshVisibility();
        refreshEnabled();
        refreshWarnings();
    }
    if (isAdaptation) {
        return true;
    }
    AppInstancePtr app = getApp();
    const bool loading = app && app->getProject() && app->getProject()->isLoadingProject();
    // A load or a time change sets every knob to what was saved, which is already consistent;
    // only an edit moves the others.
    if (_syncing || loading || (reason == eValueChangedReasonTimeChanged) || (reason == eValueChangedReasonSlaveRefresh) || (reason == eValueChangedReasonRestoreDefault)) {
        if (isIlluminant && !_syncing) {
            _lastIlluminant = rawColorCalibrationIlluminantAt(op().illuminant.lock()->getValue());
        }

        return true;
    }
    if (isHueChroma) {
        applyHueChroma();
    } else if (isIlluminant || isSource || isTemperature) {
        syncIlluminant(isIlluminant, isTemperature);
    }
    showIlluminant(isHueChroma);

    return true;
}

NATRON_NAMESPACE_EXIT
