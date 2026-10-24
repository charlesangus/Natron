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

#include <cmath>
#include <cstdio>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Raw/RawColorMath.h"
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

// The illuminants darktable's combobox offers without a raw image. The detection modes need its
// AI build, and the camera illuminant needs the raw file's white balance.
const IlluminantOption kIlluminantOptions[] = {
    { Illuminant::Pipe, "pipe" },
    { Illuminant::A, "a" },
    { Illuminant::D, "d" },
    { Illuminant::E, "e" },
    { Illuminant::F, "f" },
    { Illuminant::Led, "led" },
    { Illuminant::BlackBody, "blackBody" },
    { Illuminant::Custom, "custom" },
};

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
    const int count = (int)(sizeof(kIlluminantOptions) / sizeof(kIlluminantOptions[0]));

    return (index >= 0 && index < count) ? kIlluminantOptions[index].illuminant : Illuminant::D;
}

int
rawColorCalibrationIlluminantIndex(Illuminant illuminant)
{
    const int count = (int)(sizeof(kIlluminantOptions) / sizeof(kIlluminantOptions[0]));

    for (int i = 0; i < count; ++i) {
        if (kIlluminantOptions[i].illuminant == illuminant) {
            return i;
        }
    }

    return -1;
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

    KnobGroupPtr cat = addTab(knobHolder, page, kRawColorCalibrationTabCat, "CAT", "Chromatic adaptation transform.");

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

    std::vector<ChoiceOption> illuminantOptions;
    for (const IlluminantOption& option : kIlluminantOptions) {
        illuminantOptions.push_back(ChoiceOption(option.id, EffectInstance::tr(illuminantLabel(option.illuminant)).toStdString(), std::string()));
    }
    illuminant = addChoice(knobHolder, cat, kRawColorCalibrationParamIlluminant, "Illuminant",
                           "The illuminant of the scene, which the adaptation turns into D50 white.",
                           illuminantOptions, rawColorCalibrationIlluminantIndex(defaults.illuminant));

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
                            const ImageMetadata& /*source*/) const
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

    return makeKernel(settings(reader), toXyz, fromXyz, nullptr, nullptr);
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
    , _lastDaylightTemperature(CalibrationSettings().temperature)
    , _lastBlackBodyTemperature(CalibrationSettings().temperature)
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
RawColorCalibration::showIlluminant()
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
    const Vec3 lch = illuminantLch(px, py);
    // A grey illuminant has no hue: the slider stays where it was, for when the chroma comes back.
    if (lch[1] > 0.f) {
        hueKnob->setValue(radiansToDegrees(lch[2]));
    }
    chromaKnob->setValue(lch[1]);

    Xy xy = { px, py };
    const std::optional<Xy> found = illuminantToXy(rawColorCalibrationIlluminantAt(illuminantKnob->getValue()), nullptr, nullptr, (float)temperatureKnob->getValue(),
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
    const Illuminant illuminant = rawColorCalibrationIlluminantAt(illuminantKnob->getValue());
    float temperature = (float)temperatureKnob->getValue();

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
    float px = (float)xKnob->getValue();
    float py = (float)yKnob->getValue();
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
    _syncing = wasSyncing;
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
RawColorCalibration::refreshGreyWarning()
{
    const RawColorCalibrationOp& o = op();
    const KnobBoolPtr normalizeGrey = o.normalize[eRawColorCalibrationRowGrey].lock();
    float grey[3] = { 0.f, 0.f, 0.f };
    bool used = false;

    for (int c = 0; c < 3; ++c) {
        const KnobDoublePtr knob = o.rows[eRawColorCalibrationRowGrey][c].lock();
        grey[c] = knob ? (float)knob->getValue() : 0.f;
        used = used || (grey[c] != 0.f);
    }
    if (used && normalizeGrey && normalizeGrey->getValue() && (grey[0] + grey[1] + grey[2] == 0.f)) {
        _message.post(getNode(), eMessageTypeWarning, tr(kGreyNormalizationDisabled).toStdString());
    } else {
        _message.withdraw(getNode());
    }
}

void
RawColorCalibration::onKnobsLoaded()
{
    const KnobDoublePtr temperatureKnob = op().temperature.lock();

    if (temperatureKnob) {
        _lastDaylightTemperature = (float)temperatureKnob->getDefaultValue(0);
        _lastBlackBodyTemperature = _lastDaylightTemperature;
    }
    refreshVisibility();
    refreshVersionVisibility();
    showIlluminant();
    refreshGreyWarning();
}

bool
RawColorCalibration::knobChanged(KnobI* k,
                                 ValueChangedReasonEnum reason,
                                 ViewSpec /*view*/,
                                 double /*time*/,
                                 bool /*originatedFromMainThread*/)
{
    const RawColorCalibrationOp& o = op();
    const auto is = [k](const auto& weak) {
        const auto knob = weak.lock();

        return knob && (k == knob.get());
    };
    const bool isIlluminant = is(o.illuminant);
    const bool isSource = is(o.fluorescent) || is(o.led);
    const bool isTemperature = is(o.temperature);
    const bool isHueChroma = is(o.hue) || is(o.chroma);
    const bool isXy = is(o.x) || is(o.y);
    const bool isAdaptation = is(o.adaptation);
    bool isGrey = is(o.normalize[eRawColorCalibrationRowGrey]);
    for (int c = 0; c < 3; ++c) {
        isGrey = isGrey || is(o.rows[eRawColorCalibrationRowGrey][c]);
    }

    if (!isIlluminant && !isSource && !isTemperature && !isHueChroma && !isXy && !isAdaptation && !isGrey) {
        return false;
    }
    if (isGrey) {
        refreshGreyWarning();

        return true;
    }
    if (isIlluminant || isAdaptation) {
        refreshVisibility();
    }
    if (isAdaptation) {
        return true;
    }
    AppInstancePtr app = getApp();
    const bool loading = app && app->getProject() && app->getProject()->isLoadingProject();
    // A load or a time change sets every knob to what was saved, which is already consistent;
    // only an edit moves the others.
    if (_syncing || loading || (reason == eValueChangedReasonTimeChanged) || (reason == eValueChangedReasonSlaveRefresh) || (reason == eValueChangedReasonRestoreDefault)) {
        return true;
    }
    if (isHueChroma) {
        applyHueChroma();
    } else if (isIlluminant || isSource || isTemperature) {
        syncIlluminant(isIlluminant, isTemperature);
    }
    showIlluminant();

    return true;
}

NATRON_NAMESPACE_EXIT
