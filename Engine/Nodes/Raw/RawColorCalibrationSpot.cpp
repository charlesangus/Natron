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
 * The area color mapping and the built-in presets of darktable's color calibration module,
 * DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7), src/iop/channelmixerrgb.c:
 * _auto_set_illuminant(), _spot_settings_changed_callback(), the mapping section of gui_init()
 * and init_presets(), Copyright (C) 2020-2026 darktable developers, distributed under the GNU
 * GPL version 3 or later.
 */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "RawColorCalibration.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_ENTER

using namespace RawColorCalibrationMath;
using CalibrationSettings = RawColorCalibrationMath::Settings;

namespace {
const float kNormMin = 1.52587890625e-05f;

Vec3
multiply(const Mat3& m,
         const Vec3& v)
{
    return { m[0] * v[0] + m[1] * v[1] + m[2] * v[2],
             m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
             m[6] * v[0] + m[7] * v[1] + m[8] * v[2] };
}

// The R, G and B rows as commit_params() normalizes them.
Mat3
mixMatrix(const CalibrationSettings& s)
{
    const float normR = s.normalizeR ? (std::max)(kNormMin, s.red[0] + s.red[1] + s.red[2]) : 1.f;
    const float normG = s.normalizeG ? (std::max)(kNormMin, s.green[0] + s.green[1] + s.green[2]) : 1.f;
    const float normB = s.normalizeB ? (std::max)(kNormMin, s.blue[0] + s.blue[1] + s.blue[2]) : 1.f;
    Mat3 mix;

    for (int i = 0; i < 3; ++i) {
        mix[i] = s.red[i] / normR;
        mix[3 + i] = s.green[i] / normG;
        mix[6 + i] = s.blue[i] / normB;
    }

    return mix;
}

// matrice_pseudoinverse() returns the transposed inverse, which darktable transposes back.
Mat3
inverseMix(const Mat3& mix)
{
    float in[3][3];
    float out[3][3];

    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            in[i][j] = mix[3 * i + j];
        }
    }
    matricePseudoinverse(in, out, 3);
    Mat3 inverse;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            inverse[3 * i + j] = out[j][i];
        }
    }

    return inverse;
}

Vec3
divided(const Vec3& v,
        float d)
{
    return { v[0] / d, v[1] / d, v[2] / d };
}

RawColorCalibrationSpotLch
lchOf(const Vec3& xyz)
{
    const Vec3 lch = labToLch(xyzToLab(xyz));

    return { lch[0], lch[1], lch[2] * 360.f };
}

void
setIfChanged(const KnobDoublePtr& knob,
             double value)
{
    if (knob && (knob->getValue() != value)) {
        knob->setValue(value);
    }
}

void
setIfChanged(const KnobBoolPtr& knob,
             bool value)
{
    if (knob && (knob->getValue() != value)) {
        knob->setValue(value);
    }
}

void
setIfChanged(const KnobChoicePtr& knob,
             int value)
{
    if (knob && (knob->getValue() != value)) {
        knob->setValue(value);
    }
}

std::vector<RawColorCalibrationPreset>
makePresets()
{
    std::vector<RawColorCalibrationPreset> presets;
    CalibrationSettings p;

    p.version = SaturationVersion::V3;
    p.illuminant = Illuminant::Pipe;
    p.adaptation = Adaptation::Xyz;
    p.gamut = 0.f;
    p.clip = false;
    p.fluorescent = Fluorescent::F3;
    p.led = Led::B5;
    p.temperature = 5003.f;
    if (const std::optional<Xy> d50 = illuminantToXy(Illuminant::Pipe, nullptr, nullptr, p.temperature, Fluorescent::Last, Led::Last)) {
        p.x = d50->x;
        p.y = d50->y;
    }
    p.red = { 1.f, 0.f, 0.f };
    p.green = { 0.f, 1.f, 0.f };
    p.blue = { 0.f, 0.f, 1.f };
    p.saturation = { 0.f, 0.f, 0.f };
    p.lightness = { 0.f, 0.f, 0.f };
    p.grey = { 0.f, 0.f, 0.f };
    p.normalizeR = false;
    p.normalizeG = false;
    p.normalizeB = false;
    p.normalizeSaturation = false;
    p.normalizeLightness = false;
    p.normalizeGrey = true;

    p.clip = true;
    p.grey = { 0.f, 1.f, 0.f };
    presets.push_back({ "monochromeLuminance", "monochrome | luminance-based", p });

    p.grey = { 0.25304098f, 0.25958747f, 0.48737156f };
    presets.push_back({ "monochromeIlfordHp5", "monochrome | ILFORD HP5+", p });

    p.grey = { 0.24552374f, 0.25366007f, 0.50081619f };
    presets.push_back({ "monochromeIlfordDelta100", "monochrome | ILFORD DELTA 100", p });

    p.grey = { 0.24376712f, 0.23613559f, 0.52009729f };
    presets.push_back({ "monochromeIlfordDelta400", "monochrome | ILFORD DELTA 400 - 3200", p });

    p.grey = { 0.24149085f, 0.22149272f, 0.53701643f };
    presets.push_back({ "monochromeIlfordFp4", "monochrome | ILFORD FP4+", p });

    p.grey = { 0.333f, 0.313f, 0.353f };
    presets.push_back({ "monochromeFujiAcros100", "monochrome | Fuji Acros 100", p });

    p.adaptation = Adaptation::Rgb;
    p.grey = { 0.f, 0.f, 0.f };
    p.normalizeR = true;
    p.normalizeG = true;
    p.normalizeB = true;
    p.normalizeGrey = false;
    p.clip = false;
    presets.push_back({ "basicChannelMixer", "basic channel mixer", p });

    p.red = { 1.f, 0.f, 0.f };
    p.green = { 0.f, 0.f, 1.f };
    p.blue = { 0.f, 1.f, 0.f };
    presets.push_back({ "swapGB", "channel swap | swap G and B", p });

    p.red = { 0.f, 1.f, 0.f };
    p.green = { 1.f, 0.f, 0.f };
    p.blue = { 0.f, 0.f, 1.f };
    presets.push_back({ "swapGR", "channel swap | swap G and R", p });

    p.red = { 0.f, 0.f, 1.f };
    p.green = { 0.f, 1.f, 0.f };
    p.blue = { 1.f, 0.f, 0.f };
    presets.push_back({ "swapRB", "channel swap | swap R and B", p });

    return presets;
}

KnobDoublePtr
addSpotDouble(KnobHolder* holder,
              const KnobGroupPtr& group,
              const char* name,
              const QString& label,
              const QString& hint,
              double defaultValue,
              double maximum)
{
    KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(holder, label.toStdString());

    knob->setName(name);
    knob->setHintToolTip(hint.toStdString());
    knob->setDefaultValue(defaultValue);
    knob->setMinimum(0.);
    knob->setMaximum(maximum);
    knob->setDisplayMinimum(0.);
    knob->setDisplayMaximum(maximum);
    knob->setAnimationEnabled(false);
    knob->setEvaluateOnChange(false);
    group->addKnob(knob);

    return knob;
}
} // namespace

Adaptation
rawColorCalibrationEffectiveAdaptation(const CalibrationSettings& settings)
{
    return (settings.illuminant == Illuminant::Camera) ? Adaptation::Cat16 : settings.adaptation;
}

RawColorCalibrationSpotLch
rawColorCalibrationSpotInput(const Vec3& rgb,
                             const Mat3& workingToXyzD50)
{
    return lchOf(multiply(workingToXyzD50, rgb));
}

RawColorCalibrationSpotLch
rawColorCalibrationMeasureSpot(const Vec3& rgb,
                               const Mat3& workingToXyzD50,
                               const CalibrationSettings& s,
                               const RawCameraWhite* camera,
                               const Vec4* customWb,
                               bool useMixing)
{
    const Vec3 xyz = multiply(workingToXyzD50, rgb);
    float x = s.x;
    float y = s.y;

    if (const std::optional<Xy> found = illuminantToXy(s.illuminant, camera, customWb, s.temperature, s.fluorescent, s.led)) {
        x = found->x;
        y = found->y;
    }
    const Adaptation adaptation = rawColorCalibrationEffectiveAdaptation(s);
    const Vec3 lmsIlluminant = convertAnyXyzToLms(illuminantXyToXyz(x, y), adaptation);
    const float p = std::pow(0.818155f / (std::max)(kNormMin, lmsIlluminant[2]), 0.0834f);
    Vec3 output = chromaAdaptPixel(xyz, lmsIlluminant, adaptation, p);

    if (useMixing) {
        output = convertAnyLmsToXyz(multiply(mixMatrix(s), convertAnyXyzToLms(output, adaptation)), adaptation);
    }

    return lchOf(output);
}

Xy
rawColorCalibrationCorrectSpot(const Vec3& rgb,
                               const Mat3& workingToXyzD50,
                               const CalibrationSettings& s,
                               const RawColorCalibrationSpotLch& target,
                               bool useMixing)
{
    const Adaptation adaptation = rawColorCalibrationEffectiveAdaptation(s);
    Vec3 xyzTarget = labToXyz(lchToLab({ target.lightness, target.chroma, target.hue / 360.f }));
    xyzTarget = divided(xyzTarget, (std::max)(kNormMin, xyzTarget[1]));
    Vec3 lmsTarget = convertAnyXyzToLms(xyzTarget, adaptation);

    if (useMixing) {
        // The target is where the mix lands: undo it, then bring Y back to 1 for a mix that is
        // not normalized.
        xyzTarget = convertAnyLmsToXyz(multiply(inverseMix(mixMatrix(s)), lmsTarget), adaptation);
        xyzTarget = divided(xyzTarget, (std::max)(kNormMin, xyzTarget[1]));
        lmsTarget = convertAnyXyzToLms(xyzTarget, adaptation);
    }

    Vec3 xyz = multiply(workingToXyzD50, rgb);
    xyz = divided(xyz, (std::max)(kNormMin, xyz[1]));
    const Vec3 lms = convertAnyXyzToLms(xyz, adaptation);
    const Vec3 d50 = convertD50ToLms(adaptation);

    // target / input = D50 / illuminant, solved for the illuminant.
    Vec3 lmsIlluminant;
    for (int c = 0; c < 3; ++c) {
        lmsIlluminant[c] = d50[c] * lms[c] / lmsTarget[c];
    }
    const Vec3 xyzIlluminant = convertAnyLmsToXyz(lmsIlluminant, adaptation);
    const float sum = std::fmax(xyzIlluminant[0] + xyzIlluminant[1] + xyzIlluminant[2], kNormMin);

    return { xyzIlluminant[0] / sum, xyzIlluminant[1] / sum };
}

const std::vector<RawColorCalibrationPreset>&
rawColorCalibrationPresets()
{
    static const std::vector<RawColorCalibrationPreset> presets = makePresets();

    return presets;
}

void
RawColorCalibrationOp::createPresetKnobs(KnobHolder* knobHolder,
                                         const KnobPagePtr& page)
{
    std::vector<ChoiceOption> options;

    for (const RawColorCalibrationPreset& entry : rawColorCalibrationPresets()) {
        options.push_back(ChoiceOption(entry.id, EffectInstance::tr(entry.label).toStdString(), std::string()));
    }
    KnobChoicePtr choice = AppManager::createKnob<KnobChoice>(knobHolder, EffectInstance::tr("Preset").toStdString());
    choice->setName(kRawColorCalibrationParamPreset);
    choice->setHintToolTip(EffectInstance::tr("One of darktable's built-in presets: monochrome film emulations, the plain channel mixer and channel swaps. "
                                              "Apply writes it into every control.")
                               .toStdString());
    choice->populateChoices(options);
    choice->setDefaultValue(0);
    choice->setAnimationEnabled(false);
    choice->setEvaluateOnChange(false);
    choice->setAddNewLine(false);
    page->addKnob(choice);
    preset = choice;

    KnobButtonPtr apply = AppManager::createKnob<KnobButton>(knobHolder, EffectInstance::tr("Apply").toStdString());
    apply->setName(kRawColorCalibrationParamApplyPreset);
    apply->setHintToolTip(EffectInstance::tr("Sets every control to the chosen preset, and turns Override File Defaults on so that its illuminant and adaptation are used.").toStdString());
    apply->setEvaluateOnChange(false);
    page->addKnob(apply);
    applyPreset = apply;
}

void
RawColorCalibrationOp::createSpotKnobs(KnobHolder* knobHolder,
                                       const KnobGroupPtr& tab)
{
    KnobGroupPtr group = AppManager::createKnob<KnobGroup>(knobHolder, EffectInstance::tr("Area Color Mapping").toStdString());
    group->setName(kRawColorCalibrationGroupSpot);
    group->setHintToolTip(EffectInstance::tr("Define a target chromaticity (hue and chroma) for a particular region of the image (the control sample), "
                                             "which you then match against the same target chromaticity in other images. The control sample can either be "
                                             "a critical part of your subject or a non-moving and consistently-lit surface over your series of images.")
                              .toStdString());
    tab->addKnob(group);

    std::vector<ChoiceOption> modes;
    modes.push_back(ChoiceOption("correction", EffectInstance::tr("correction").toStdString(), std::string()));
    modes.push_back(ChoiceOption("measure", EffectInstance::tr("measure").toStdString(), std::string()));
    KnobChoicePtr mode = AppManager::createKnob<KnobChoice>(knobHolder, EffectInstance::tr("Area Mode").toStdString());
    mode->setName(kRawColorCalibrationParamSpotMode);
    mode->setHintToolTip(EffectInstance::tr("\"correction\" automatically adjusts the illuminant such that the input color is mapped to the target.\n"
                                            "\"measure\" simply shows how an input color is mapped by the CAT and can be used to sample a target.")
                             .toStdString());
    mode->populateChoices(modes);
    mode->setDefaultValue((int)eRawColorCalibrationSpotModeCorrection);
    mode->setAnimationEnabled(false);
    mode->setEvaluateOnChange(false);
    group->addKnob(mode);
    spotMode = mode;

    KnobBoolPtr mixing = AppManager::createKnob<KnobBool>(knobHolder, EffectInstance::tr("Take Channel Mixing Into Account").toStdString());
    mixing->setName(kRawColorCalibrationParamSpotUseMixing);
    mixing->setHintToolTip(EffectInstance::tr("Compute the target by taking the channel mixing into account. If disabled, only the CAT is considered.").toStdString());
    mixing->setDefaultValue(true);
    mixing->setAnimationEnabled(false);
    mixing->setEvaluateOnChange(false);
    group->addKnob(mixing);
    spotUseMixing = mixing;

    KnobStringPtr input = AppManager::createKnob<KnobString>(knobHolder, EffectInstance::tr("Input").toStdString());
    input->setName(kRawColorCalibrationParamSpotInput);
    input->setHintToolTip(EffectInstance::tr("The lightness, hue and chroma of the area at the last Pick, the input color that should be mapped to the target.").toStdString());
    input->setAsLabel();
    input->setIsPersistent(false);
    input->setEvaluateOnChange(false);
    group->addKnob(input);
    spotInput = input;

    spotLightness = addSpotDouble(knobHolder, group, kRawColorCalibrationParamSpotLightness, EffectInstance::tr("Target Lightness"),
                                  EffectInstance::tr("The CIE Lab lightness of the target, in percent. Correction matches the chromaticity only; measure sets it."),
                                  50., 100.);
    spotHue = addSpotDouble(knobHolder, group, kRawColorCalibrationParamSpotHue, EffectInstance::tr("Target Hue"),
                            EffectInstance::tr("The CIE LCh hue of the target, in degrees."),
                            0., 360.);
    spotChroma = addSpotDouble(knobHolder, group, kRawColorCalibrationParamSpotChroma, EffectInstance::tr("Target Chroma"),
                               EffectInstance::tr("The CIE LCh chroma of the target. 0 makes the area neutral."),
                               0., 128.);

    Format format;
    if (AppInstancePtr app = knobHolder->getApp()) {
        app->getProject()->getProjectDefaultFormat(&format);
    }
    KnobDoublePtr bottomLeftKnob = AppManager::createKnob<KnobDouble>(knobHolder, EffectInstance::tr("Area Bottom Left").toStdString(), 2);
    bottomLeftKnob->setName(kRawColorCalibrationParamSpotBottomLeft);
    bottomLeftKnob->setHintToolTip(EffectInstance::tr("The bottom left corner of the area Pick averages, in pixels of the input.").toStdString());
    bottomLeftKnob->setSpatial(true);
    bottomLeftKnob->disableSlider();
    bottomLeftKnob->setAnimationEnabled(false);
    bottomLeftKnob->setEvaluateOnChange(false);
    for (int d = 0; d < 2; ++d) {
        const double extent = (d == 0) ? (double)format.width() : (double)format.height();
        const double origin = (d == 0) ? (double)format.x1 : (double)format.y1;
        bottomLeftKnob->setDecimals(0, d);
        bottomLeftKnob->setIncrement(1., d);
        bottomLeftKnob->setDefaultValue(origin + 0.02 * extent, d);
    }
    bottomLeftKnob->setAddNewLine(false);
    group->addKnob(bottomLeftKnob);
    spotBottomLeft = bottomLeftKnob;

    KnobDoublePtr sizeKnob = AppManager::createKnob<KnobDouble>(knobHolder, EffectInstance::tr("Area Size").toStdString(), 2);
    sizeKnob->setName(kRawColorCalibrationParamSpotSize);
    sizeKnob->setHintToolTip(EffectInstance::tr("The width and height of the area Pick averages, in pixels of the input.").toStdString());
    sizeKnob->setSpatial(true);
    sizeKnob->disableSlider();
    sizeKnob->setAnimationEnabled(false);
    sizeKnob->setEvaluateOnChange(false);
    sizeKnob->setDimensionName(0, "w");
    sizeKnob->setDimensionName(1, "h");
    for (int d = 0; d < 2; ++d) {
        const double extent = (d == 0) ? (double)format.width() : (double)format.height();
        sizeKnob->setMinimum(0., d);
        sizeKnob->setDecimals(0, d);
        sizeKnob->setIncrement(1., d);
        sizeKnob->setDefaultValue(0.96 * extent, d);
    }
    group->addKnob(sizeKnob);
    spotSize = sizeKnob;

    KnobButtonPtr pickKnob = AppManager::createKnob<KnobButton>(knobHolder, EffectInstance::tr("Pick").toStdString());
    pickKnob->setName(kRawColorCalibrationParamPick);
    pickKnob->setHintToolTip(EffectInstance::tr("Averages the input over the area. In correction mode, sets a custom illuminant that maps the area to the target; "
                                                "in measure mode, sets the target to what the node makes of the area. The values are kept until the next pick.")
                                 .toStdString());
    pickKnob->setEvaluateOnChange(false);
    group->addKnob(pickKnob);
    pick = pickKnob;
}

void
RawColorCalibration::initializeKnobs()
{
    ImageNode<RawColorCalibrationOp>::initializeKnobs();

    if (NodePtr node = getNode()) {
        node->addRectangleInteract(op().spotBottomLeft.lock(), op().spotSize.lock(), KnobBoolPtr(), KnobBoolPtr());
    }
}

void
RawColorCalibration::refreshSpotDefaults()
{
    const KnobDoublePtr bottomLeft = op().spotBottomLeft.lock();
    const KnobDoublePtr size = op().spotSize.lock();
    EffectInstancePtr input = getInput(getSourceInput());

    if (!bottomLeft || !size || !input) {
        return;
    }
    const RectI format = input->getOutputFormat();
    if (format.isNull()) {
        return;
    }
    for (int d = 0; d < 2; ++d) {
        const double extent = (d == 0) ? (double)format.width() : (double)format.height();
        const double origin = (d == 0) ? (double)format.x1 : (double)format.y1;
        if (!bottomLeft->hasModifications(d)) {
            bottomLeft->setDefaultValue(origin + 0.02 * extent, d);
        }
        if (!size->hasModifications(d)) {
            size->setDefaultValue(0.96 * extent, d);
        }
    }
}

bool
RawColorCalibration::pickSpot(double time,
                              ViewIdx view)
{
    const KnobDoublePtr bottomLeft = op().spotBottomLeft.lock();
    const KnobDoublePtr size = op().spotSize.lock();
    const int inputNb = getSourceInput();

    if (!bottomLeft || !size || !getInput(inputNb)) {
        return false;
    }
    const double x = bottomLeft->getValueAtTime(time, 0);
    const double y = bottomLeft->getValueAtTime(time, 1);
    RectI box;
    box.x1 = (int)std::floor(x);
    box.y1 = (int)std::floor(y);
    box.x2 = (std::max)((int)std::floor(x + size->getValueAtTime(time, 0)), box.x1 + 1);
    box.y2 = (std::max)((int)std::floor(y + size->getValueAtTime(time, 1)), box.y1 + 1);
    const RectD bounds(box.x1, box.y1, box.x2, box.y2);
    RectI roi;
    ImagePtr src = getImage(inputNb, time, RenderScale::identity, view, &bounds, NULL, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &roi);
    if (!src || (src->getBitDepth() != eImageBitDepthFloat)) {
        return false;
    }
    const RectI area = box.intersect(src->getBounds());
    if (area.isNull()) {
        return false;
    }
    const int nComps = (int)src->getComponentsCount();
    std::array<double, 3> sum = { 0., 0., 0. };
    std::size_t count = 0;
    {
        Image::ReadAccess access(src.get());
        for (int py = area.y1; py < area.y2; ++py) {
            const float* pix = (const float*)access.pixelAt(area.x1, py);
            if (!pix) {
                continue;
            }
            for (int px = area.x1; px < area.x2; ++px, pix += nComps) {
                for (int c = 0; c < 3; ++c) {
                    sum[c] += pix[(nComps >= 3) ? c : 0];
                }
                ++count;
            }
        }
    }
    if (count == 0) {
        return false;
    }
    _spotMean = Vec3 { (float)(sum[0] / (double)count), (float)(sum[1] / (double)count), (float)(sum[2] / (double)count) };

    AppInstancePtr app = getApp();
    applySpot(time, view, app && !app->isCreatingPythonGroup());

    return true;
} // RawColorCalibration::pickSpot

void
RawColorCalibration::applySpot(double time,
                               ViewIdx view,
                               bool oneUndoStep)
{
    const RawColorCalibrationOp& o = op();

    if (!_spotMean) {
        return;
    }
    const Vec3 rgb = *_spotMean;
    const Mat3 toXyz = RawColorCalibrationOp::workingToXyzD50(this);
    const ImageMetadata source = getSourceMetadata(time, view);
    const KnobReader reader(time, view);
    const CalibrationSettings s = o.effectiveSettings(reader, source);
    const KnobChoicePtr modeKnob = o.spotMode.lock();
    const KnobBoolPtr mixingKnob = o.spotUseMixing.lock();
    const KnobDoublePtr lightnessKnob = o.spotLightness.lock();
    const KnobDoublePtr hueKnob = o.spotHue.lock();
    const KnobDoublePtr chromaKnob = o.spotChroma.lock();
    if (!modeKnob || !mixingKnob || !lightnessKnob || !hueKnob || !chromaKnob) {
        return;
    }
    const bool useMixing = mixingKnob->getValue();
    const bool measure = modeKnob->getValue() == (int)eRawColorCalibrationSpotModeMeasure;

    if (oneUndoStep) {
        setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOnCreateNewCommand);
    }
    const bool wasSyncing = _syncing;
    _syncing = true;
    if (const KnobStringPtr inputKnob = o.spotInput.lock()) {
        const RawColorCalibrationSpotLch lch = rawColorCalibrationSpotInput(rgb, toXyz);
        char buffer[96];
        std::snprintf(buffer, sizeof(buffer), "L: %.1f %%  h: %.1f°  c: %.1f", lch.lightness, lch.hue, lch.chroma);
        inputKnob->setValue(std::string(buffer));
    }
    if (measure) {
        const RawCameraWhite camera = rawColorCalibrationCameraWhite(source);
        Vec4 customWb;
        (void)rawColorCalibrationWhiteBalanceRatio(source, &customWb);
        const RawColorCalibrationSpotLch lch = rawColorCalibrationMeasureSpot(rgb, toXyz, s, &camera, &customWb, useMixing);
        setIfChanged(lightnessKnob, (double)lch.lightness);
        setIfChanged(chromaKnob, (double)lch.chroma);
        setIfChanged(hueKnob, (double)lch.hue);
    } else {
        const RawColorCalibrationSpotLch target = { (float)lightnessKnob->getValue(), (float)chromaKnob->getValue(), (float)hueKnob->getValue() };
        const Xy xy = rawColorCalibrationCorrectSpot(rgb, toXyz, s, target, useMixing);
        // The daylight and black body models are not accurate enough for colour matching.
        setIfChanged(o.catOverride.lock(), true);
        setIfChanged(o.adaptation.lock(), static_cast<int>(rawColorCalibrationEffectiveAdaptation(s)));
        setIfChanged(o.illuminant.lock(), rawColorCalibrationIlluminantIndex(Illuminant::Custom));
        setIfChanged(o.x.lock(), (double)xy.x);
        setIfChanged(o.y.lock(), (double)xy.y);
        setIfChanged(o.temperature.lock(), (double)checkIfCloseToDaylight(xy.x, xy.y).temperature);
        _lastIlluminant = Illuminant::Custom;
    }
    _syncing = wasSyncing;
    if (oneUndoStep) {
        setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOff);
    }
    if (!measure) {
        refreshAfterWrite();
    }
} // RawColorCalibration::applySpot

void
RawColorCalibration::onSpotSettingsChanged(ValueChangedReasonEnum reason)
{
    AppInstancePtr app = getApp();
    const bool loading = app && app->getProject() && app->getProject()->isLoadingProject();
    const KnobChoicePtr modeKnob = op().spotMode.lock();

    if (_syncing || loading || !_spotMean || !modeKnob || (reason == eValueChangedReasonTimeChanged) || (reason == eValueChangedReasonSlaveRefresh)) {
        return;
    }
    if (modeKnob->getValue() == (int)eRawColorCalibrationSpotModeCorrection) {
        applySpot(getCurrentTime(), ViewIdx(0), false);
    }
}

void
RawColorCalibration::writeSettings(const CalibrationSettings& s)
{
    const RawColorCalibrationOp& o = op();
    const Vec3* rowValues[eRawColorCalibrationRowCount] = { &s.red, &s.green, &s.blue, &s.saturation, &s.lightness, &s.grey };
    const bool normalizeValues[eRawColorCalibrationRowCount] = { s.normalizeR, s.normalizeG, s.normalizeB, s.normalizeSaturation, s.normalizeLightness, s.normalizeGrey };

    setIfChanged(o.catOverride.lock(), true);
    for (int r = 0; r < eRawColorCalibrationRowCount; ++r) {
        for (int c = 0; c < 3; ++c) {
            setIfChanged(o.rows[r][c].lock(), (double)(*rowValues[r])[c]);
        }
        setIfChanged(o.normalize[r].lock(), normalizeValues[r]);
    }
    setIfChanged(o.adaptation.lock(), static_cast<int>(s.adaptation));
    setIfChanged(o.illuminant.lock(), rawColorCalibrationIlluminantIndex(s.illuminant));
    setIfChanged(o.fluorescent.lock(), static_cast<int>(s.fluorescent));
    setIfChanged(o.led.lock(), static_cast<int>(s.led));
    setIfChanged(o.temperature.lock(), (double)s.temperature);
    setIfChanged(o.x.lock(), (double)s.x);
    setIfChanged(o.y.lock(), (double)s.y);
    setIfChanged(o.gamut.lock(), (double)s.gamut);
    setIfChanged(o.clip.lock(), s.clip);
    setIfChanged(o.version.lock(), static_cast<int>(s.version));
    _lastIlluminant = s.illuminant;
    if (s.illuminant == Illuminant::D) {
        _lastDaylightTemperature = s.temperature;
    } else if (s.illuminant == Illuminant::BlackBody) {
        _lastBlackBodyTemperature = s.temperature;
    }
}

void
RawColorCalibration::refreshAfterWrite()
{
    refreshVisibility();
    refreshEnabled();
    refreshVersionVisibility();
    showIlluminant();
    refreshWarnings();
}

void
RawColorCalibration::applyChosenPreset()
{
    const KnobChoicePtr presetKnob = op().preset.lock();
    const std::vector<RawColorCalibrationPreset>& presets = rawColorCalibrationPresets();

    if (!presetKnob) {
        return;
    }
    const int index = presetKnob->getValue();
    if ((index < 0) || (index >= (int)presets.size())) {
        return;
    }
    AppInstancePtr app = getApp();
    const bool oneUndoStep = app && !app->isCreatingPythonGroup();
    if (oneUndoStep) {
        setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOnCreateNewCommand);
    }
    const bool wasSyncing = _syncing;
    _syncing = true;
    writeSettings(presets[index].settings);
    _syncing = wasSyncing;
    if (oneUndoStep) {
        setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOff);
    }
    refreshAfterWrite();
}

NATRON_NAMESPACE_EXIT
