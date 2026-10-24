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
 * A port of the raw-domain part of darktable's white balance module, DARKTABLE_REF
 * release-5.6.2 (commit 20891f6c6fa7), src/iop/temperature.c, Copyright (C) 2009-2026 darktable
 * developers, distributed under the GNU GPL version 3 or later. Its camera presets are read
 * through RawWbPresets, from darktable's data/wb_presets.json.
 */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "RawTemperature.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Raw/RawGpuParams.h"
#include "Engine/Nodes/Raw/RawWbPresets.h"
#include "Engine/Project.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
KnobDoublePtr
addDoubleKnob(KnobHolder* holder,
              const KnobPagePtr& page,
              const char* name,
              const QString& label,
              const QString& hint,
              double defaultValue,
              double minimum,
              double maximum)
{
    KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(holder, label.toStdString());

    knob->setName(name);
    knob->setHintToolTip(hint.toStdString());
    knob->setDefaultValue(defaultValue);
    knob->setMinimum(minimum);
    knob->setMaximum(maximum);
    knob->setDisplayMinimum(minimum);
    knob->setDisplayMaximum(maximum);
    page->addKnob(knob);

    return knob;
}

bool
usableCoefficients(const std::array<double, 3>& coeffs)
{
    for (double c : coeffs) {
        if (!std::isnormal(c) || (c == 0.)) {
            return false;
        }
    }

    return true;
}

std::string
sourceMake(const ImageMetadata& source)
{
    return RawMetadata::getMake(source).value_or(std::string());
}

std::string
sourceModel(const ImageMetadata& source)
{
    return RawMetadata::getModel(source).value_or(std::string());
}

// The multipliers of every mode but Custom and From Image Area, which store theirs in the knobs. A
// preset the source's camera does not have gives the as-shot multipliers; As Shot to Reference
// applies the as-shot multipliers too.
RawColorMath::Vec3
resolvedMultipliers(int modeIndex,
                    const ImageMetadata& source,
                    const std::string& presetName,
                    int tune)
{
    if (modeIndex == (int)eRawTemperatureModeCameraPreset) {
        if (const std::optional<RawColorMath::Vec3> coeffs = RawTemperatureOp::presetMultipliers(source, presetName, tune)) {
            return *coeffs;
        }

        return RawTemperatureOp::modeMultipliers(eRawTemperatureModeAsShot, source);
    }

    return RawTemperatureOp::modeMultipliers((modeIndex == (int)eRawTemperatureModeCameraReference) ? eRawTemperatureModeCameraReference : eRawTemperatureModeAsShot, source);
}

// dt_colorspaces_conversion_matrices_rgb() and _xyz() take d65_color_matrix over
// adobe_XYZ_to_CAM: only the DNG matrix carries AnalogBalance, which rawspeed does not apply to
// the data.
std::optional<RawColorMath::Mat3>
sourceXyzToCam(const ImageMetadata& source)
{
    if (const std::optional<std::array<double, 9>> dng = RawMetadata::getDngD65Matrix(source)) {
        return *dng;
    }
    if (const std::optional<std::array<double, 9>> camToXyz = RawMetadata::getCamToXyz(source)) {
        return RawColorMath::invert(*camToXyz);
    }

    return std::nullopt;
}
} // namespace

void
RawTemperatureOp::createKnobs(KnobHolder* holder,
                              const KnobPagePtr& page)
{
    KnobChoicePtr modeKnob = AppManager::createKnob<KnobChoice>(holder, EffectInstance::tr("Mode").toStdString());
    modeKnob->setName(kRawTemperatureParamMode);
    modeKnob->setHintToolTip(EffectInstance::tr("Where the white balance multipliers come from.").toStdString());
    std::vector<ChoiceOption> modes;
    modes.push_back(ChoiceOption("asShot", EffectInstance::tr("As Shot").toStdString(), EffectInstance::tr("The multipliers the camera recorded in the raw file.").toStdString()));
    modes.push_back(ChoiceOption("cameraReference", EffectInstance::tr("Camera Reference").toStdString(), EffectInstance::tr("The multipliers that make D65 white neutral through the camera's colour matrix.").toStdString()));
    modes.push_back(ChoiceOption("custom", EffectInstance::tr("Custom").toStdString(), EffectInstance::tr("The red, green and blue multipliers below, which the temperature and the tint set.").toStdString()));
    modes.push_back(ChoiceOption("cameraPreset", EffectInstance::tr("Camera Preset").toStdString(), EffectInstance::tr("The camera's white balance setting the Preset knob names, at the Finetune knob's tuning.").toStdString()));
    modes.push_back(ChoiceOption("fromImageArea", EffectInstance::tr("From Image Area").toStdString(), EffectInstance::tr("The multipliers that make the mean of the spot area neutral, stored in the red, green and blue knobs when Pick is pressed. Choosing this mode picks.").toStdString()));
    modes.push_back(ChoiceOption("asShotToReference", EffectInstance::tr("As Shot to Reference").toStdString(), EffectInstance::tr("The as-shot multipliers, which RawInputColor corrects to the camera reference ones before its colour matrix, so the highlights and demosaic work on as-shot data and the colour ends at D65.").toStdString()));
    modeKnob->populateChoices(modes);
    modeKnob->setDefaultValue((int)eRawTemperatureModeAsShot);
    modeKnob->setAnimationEnabled(false);
    page->addKnob(modeKnob);
    mode = modeKnob;

    // Filled from the source's camera by RawTemperature::refreshPresets(); the project saves the
    // chosen setting by name.
    KnobChoicePtr presetKnob = AppManager::createKnob<KnobChoice>(holder, EffectInstance::tr("Preset").toStdString());
    presetKnob->setName(kRawTemperatureParamPreset);
    presetKnob->setHintToolTip(EffectInstance::tr("The white balance settings of the source's camera, from darktable's presets. Choosing one switches the mode to Camera Preset.").toStdString());
    presetKnob->setAnimationEnabled(false);
    page->addKnob(presetKnob);
    preset = presetKnob;

    KnobIntPtr finetuneKnob = AppManager::createKnob<KnobInt>(holder, EffectInstance::tr("Finetune").toStdString());
    finetuneKnob->setName(kRawTemperatureParamFinetune);
    finetuneKnob->setHintToolTip(EffectInstance::tr("Fine tunes the camera's white balance setting, in the camera's own steps. Its range is the setting's tunings; values between two the camera gives are interpolated.").toStdString());
    finetuneKnob->setDefaultValue(0);
    finetuneKnob->setMinimum(-9);
    finetuneKnob->setMaximum(9);
    finetuneKnob->setDisplayMinimum(-9);
    finetuneKnob->setDisplayMaximum(9);
    finetuneKnob->setAnimationEnabled(false);
    page->addKnob(finetuneKnob);
    finetune = finetuneKnob;

    temperature = addDoubleKnob(holder, page, kRawTemperatureParamTemperature, EffectInstance::tr("Temperature"),
                                EffectInstance::tr("The colour temperature, in kelvin, of the light to make neutral. Editing it sets the multipliers through the camera's colour matrix."),
                                5000., RawColorMath::kLowestTemperature, RawColorMath::kHighestTemperature);
    tint = addDoubleKnob(holder, page, kRawTemperatureParamTint, EffectInstance::tr("Tint"),
                         EffectInstance::tr("Below 1 towards magenta, above 1 towards green. Editing it sets the multipliers through the camera's colour matrix."),
                         1., RawColorMath::kLowestTint, RawColorMath::kHighestTint);
    red = addDoubleKnob(holder, page, kRawTemperatureParamRed, EffectInstance::tr("Red"), EffectInstance::tr("The red multiplier."), 1., 0., 8.);
    green = addDoubleKnob(holder, page, kRawTemperatureParamGreen, EffectInstance::tr("Green"), EffectInstance::tr("The green multiplier."), 1., 0., 8.);
    blue = addDoubleKnob(holder, page, kRawTemperatureParamBlue, EffectInstance::tr("Blue"), EffectInstance::tr("The blue multiplier."), 1., 0., 8.);

    // darktable's default picker box covers 2% to 98% of the image. It starts on the project's
    // format here; RawTemperature::refreshSpotDefaults() moves it onto the source's.
    Format format;
    if (AppInstancePtr app = holder->getApp()) {
        app->getProject()->getProjectDefaultFormat(&format);
    }
    KnobDoublePtr bottomLeftKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Spot Bottom Left").toStdString(), 2);
    bottomLeftKnob->setName(kRawTemperatureParamSpotBottomLeft);
    bottomLeftKnob->setHintToolTip(EffectInstance::tr("The bottom left corner of the area Pick averages, in pixels of the source.").toStdString());
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
    page->addKnob(bottomLeftKnob);
    spotBottomLeft = bottomLeftKnob;

    KnobDoublePtr sizeKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Spot Size").toStdString(), 2);
    sizeKnob->setName(kRawTemperatureParamSpotSize);
    sizeKnob->setHintToolTip(EffectInstance::tr("The width and height of the area Pick averages, in pixels of the source.").toStdString());
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
    page->addKnob(sizeKnob);
    spotSize = sizeKnob;

    KnobButtonPtr pickKnob = AppManager::createKnob<KnobButton>(holder, EffectInstance::tr("Pick").toStdString());
    pickKnob->setName(kRawTemperatureParamPick);
    pickKnob->setHintToolTip(EffectInstance::tr("Sets the multipliers that make the mean of the spot area neutral, and the mode to From Image Area. The multipliers are kept until the next pick.").toStdString());
    pickKnob->setEvaluateOnChange(false);
    page->addKnob(pickKnob);
    pick = pickKnob;
}

std::array<float, 3>
RawTemperatureOp::pickedCoefficients(const std::array<float, 3>& mean)
{
    // darktable's color_picker_apply(), in float as there.
    const float gnormal = (mean[1] > 0.001f) ? (1.0f / mean[1]) : 1.0f;
    std::array<float, 3> coeffs;

    for (int c = 0; c < 3; ++c) {
        coeffs[c] = std::fmax(0.0f, std::fmin(8.0f, ((mean[c] > 0.001f) ? (1.0f / mean[c]) : 1.0f) / gnormal));
    }
    coeffs[1] = 1.0f;

    return coeffs;
}

RawColorMath::CameraMatrices
RawTemperatureOp::sourceMatrices(const ImageMetadata& source)
{
    if (const std::optional<RawColorMath::Mat3> xyzToCam = sourceXyzToCam(source)) {
        return RawColorMath::cameraMatrices(*xyzToCam);
    }

    return RawColorMath::srgbCameraMatrices();
}

RawColorMath::Vec3
RawTemperatureOp::modeMultipliers(RawTemperatureModeEnum mode,
                                  const ImageMetadata& source)
{
    if (mode == eRawTemperatureModeAsShot) {
        const std::optional<std::array<double, 3>> asShot = RawMetadata::getWbCoeffs(source);
        if (asShot && usableCoefficients(*asShot)) {
            return RawColorMath::Vec3 { (*asShot)[0] / (*asShot)[1], 1., (*asShot)[2] / (*asShot)[1] };
        }
    }
    if (const std::optional<RawColorMath::Mat3> xyzToCam = sourceXyzToCam(source)) {
        if (const std::optional<RawColorMath::Vec3> reference = RawColorMath::cameraReferenceMultipliers(*xyzToCam)) {
            return *reference;
        }
    }
    // No colour matrix: darktable's _find_coeffs() takes the camera's first preset, whatever
    // its name and tuning, and reload_defaults() its daylight preset for the D65 multipliers.
    const RawWbPresets::Database& presets = RawWbPresets::database();
    if (mode == eRawTemperatureModeAsShot) {
        if (const std::optional<std::array<double, 3>> first = RawWbPresets::firstPreset(presets, sourceMake(source), sourceModel(source))) {
            if (usableCoefficients(*first)) {
                return RawColorMath::Vec3 { (*first)[0] / (*first)[1], 1., (*first)[2] / (*first)[1] };
            }
        }
    } else if (const std::optional<std::array<double, 3>> daylight = RawWbPresets::daylightPreset(presets, sourceMake(source), sourceModel(source))) {
        return *daylight;
    }

    return RawColorMath::Vec3 { 2., 1., 1.5 };
}

std::optional<RawColorMath::Vec3>
RawTemperatureOp::presetMultipliers(const ImageMetadata& source,
                                    const std::string& name,
                                    int tune)
{
    if (name.empty()) {
        return std::nullopt;
    }

    return RawWbPresets::presetMultipliers(RawWbPresets::database(), sourceMake(source), sourceModel(source), name, tune);
}

RawTemperatureOp::Params
RawTemperatureOp::bake(const KnobReader& reader,
                       const ImageMetadata& source) const
{
    Params params;

    params.layout = RawLayout::fromMetadata(source).value_or(RawLayout());
    const int modeIndex = reader.value(mode);
    RawColorMath::Vec3 coeffs;
    if ((modeIndex == eRawTemperatureModeCustom) || (modeIndex == eRawTemperatureModeFromImageArea)) {
        coeffs = { reader.value(red), reader.value(green), reader.value(blue) };
    } else if (modeIndex == eRawTemperatureModeCameraPreset) {
        const KnobChoicePtr presetKnob = preset.lock();
        (void)reader.value(presetKnob);
        coeffs = resolvedMultipliers(modeIndex, source, presetKnob->getActiveEntry().id, reader.value(finetune));
    } else {
        coeffs = resolvedMultipliers(modeIndex, source, std::string(), 0);
    }
    for (int c = 0; c < 3; ++c) {
        params.coeffs[c] = (float)coeffs[c];
    }

    return params;
}

bool
RawTemperatureOp::isIdentity(const Params& params) const
{
    return (params.coeffs[0] == 1.f) && (params.coeffs[1] == 1.f) && (params.coeffs[2] == 1.f);
}

void
RawTemperatureOp::row(const Params& params,
                      const RowIO& io) const
{
    const float* src = io.src[0];
    float* dst = io.dst;

    if (params.layout.hasCfa()) {
        for (int x = 0; x < io.width; ++x) {
            const float coeff = params.coeffs[params.layout.colourAt(io.x0 + x, io.y)];
            for (int c = 0; c < io.nComps; ++c) {
                dst[(std::size_t)x * io.nComps + c] = src[(std::size_t)x * io.nComps + c] * coeff;
            }
        }

        return;
    }
    // Without a CFA, three or four channels are red, green and blue (and alpha); a single one
    // has no colour to balance.
    for (int x = 0; x < io.width; ++x) {
        for (int c = 0; c < io.nComps; ++c) {
            const float coeff = ((io.nComps >= 3) && (c < 3)) ? params.coeffs[c] : 1.f;
            dst[(std::size_t)x * io.nComps + c] = src[(std::size_t)x * io.nComps + c] * coeff;
        }
    }
}

GpuOpRef
RawTemperatureOp::gpu(const Params& params) const
{
#ifdef NATRON_HAVE_GPU
    RawTemperatureGpuParams p = {};
    p.layout = toRawLayoutGpu(params.layout);
    for (int c = 0; c < 3; ++c) {
        p.coeffs[c] = params.coeffs[c];
    }

    return GpuOpRef::compiled((int)gpu::GpuKernelId::rawtemperature_wrapped, p);
#else
    Q_UNUSED(params);

    return GpuOpRef::none();
#endif
}

RawTemperature::RawTemperature(NodePtr node)
    : ImageNode<RawTemperatureOp>(node)
    , _syncing(false)
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

RawTemperature::~RawTemperature()
{
}

void
RawTemperature::initializeKnobs()
{
    ImageNode<RawTemperatureOp>::initializeKnobs();

    NodePtr node = getNode();
    if (node) {
        node->addRectangleInteract(op().spotBottomLeft.lock(), op().spotSize.lock(), KnobBoolPtr(), KnobBoolPtr());
    }
}

void
RawTemperature::refreshSpotDefaults()
{
    const KnobDoublePtr bottomLeft = op().spotBottomLeft.lock();
    const KnobDoublePtr size = op().spotSize.lock();
    EffectInstancePtr input = getInput(0);

    if (!bottomLeft || !size || !input) {
        return;
    }
    const RectI format = input->getOutputFormat();
    if (format.isNull()) {
        return;
    }
    // A knob still at its default follows the source; one the user moved keeps its place.
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
RawTemperature::pickSpot(double time,
                         ViewIdx view)
{
    const KnobDoublePtr bottomLeft = op().spotBottomLeft.lock();
    const KnobDoublePtr size = op().spotSize.lock();
    const KnobChoicePtr modeKnob = op().mode.lock();

    if (!bottomLeft || !size || !modeKnob || !getInput(0)) {
        return false;
    }
    // darktable's dt_color_picker_box(): whole pixels, at least one sampled.
    const double x = bottomLeft->getValueAtTime(time, 0);
    const double y = bottomLeft->getValueAtTime(time, 1);
    RectI box;
    box.x1 = (int)std::floor(x);
    box.y1 = (int)std::floor(y);
    box.x2 = (std::max)((int)std::floor(x + size->getValueAtTime(time, 0)), box.x1 + 1);
    box.y2 = (std::max)((int)std::floor(y + size->getValueAtTime(time, 1)), box.y1 + 1);
    const RectD bounds(box.x1, box.y1, box.x2, box.y2);
    RectI roi;
    ImagePtr src = getImage(0, time, RenderScale::identity, view, &bounds, NULL, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &roi);
    if (!src || (src->getBitDepth() != eImageBitDepthFloat)) {
        return false;
    }
    const RectI area = box.intersect(src->getBounds());
    if (area.isNull()) {
        return false;
    }
    const int nComps = (int)src->getComponentsCount();
    const RawLayout layout = RawLayout::fromMetadata(getSourceMetadata(time, view)).value_or(RawLayout());
    // darktable's _color_picker_bayer() and _color_picker_xtrans() average each colour of the
    // CFA over its own photosites; _color_picker_rgb_or_lab() each channel over the box.
    std::array<double, 3> sum = { 0., 0., 0. };
    std::array<std::size_t, 3> weights = { 0, 0, 0 };
    {
        Image::ReadAccess access(src.get());
        for (int py = area.y1; py < area.y2; ++py) {
            const float* pix = (const float*)access.pixelAt(area.x1, py);
            if (!pix) {
                continue;
            }
            for (int px = area.x1; px < area.x2; ++px, pix += nComps) {
                if (layout.hasCfa()) {
                    const int c = (int)layout.colourAt(px, py);
                    sum[c] += pix[0];
                    ++weights[c];
                } else {
                    for (int c = 0; c < 3; ++c) {
                        // One channel without a CFA is monochrome, read as grey.
                        sum[c] += pix[(nComps >= 3) ? c : 0];
                        ++weights[c];
                    }
                }
            }
        }
    }
    std::array<float, 3> mean;
    for (int c = 0; c < 3; ++c) {
        mean[c] = weights[c] ? (float)(sum[c] / (double)weights[c]) : 0.f;
    }
    const std::array<float, 3> picked = RawTemperatureOp::pickedCoefficients(mean);
    const RawColorMath::Vec3 coeffs = { picked[0], picked[1], picked[2] };

    AppInstancePtr app = getApp();
    const bool oneUndoStep = app && !app->isCreatingPythonGroup();
    if (oneUndoStep) {
        setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOnCreateNewCommand);
    }
    const bool wasSyncing = _syncing;
    _syncing = true;
    setCoefficientKnobs(coeffs);
    setTemperatureKnobs(RawTemperatureOp::sourceMatrices(getSourceMetadata(time, view)), coeffs);
    modeKnob->setValue((int)eRawTemperatureModeFromImageArea);
    _syncing = wasSyncing;
    if (oneUndoStep) {
        setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOff);
    }
    refreshEnabled();

    return true;
} // RawTemperature::pickSpot

void
RawTemperature::refreshEnabled()
{
    const KnobChoicePtr modeKnob = op().mode.lock();

    if (!modeKnob) {
        return;
    }
    const bool custom = modeKnob->getValue() == (int)eRawTemperatureModeCustom;
    for (const KnobDoubleWPtr& weak : { op().red, op().green, op().blue }) {
        if (KnobDoublePtr knob = weak.lock()) {
            knob->setAllDimensionsEnabled(custom);
        }
    }
}

void
RawTemperature::refreshPresets()
{
    const KnobChoicePtr presetKnob = op().preset.lock();
    const KnobIntPtr finetuneKnob = op().finetune.lock();

    if (!presetKnob || !finetuneKnob) {
        return;
    }
    const ImageMetadata source = getSourceMetadata(getCurrentTime(), ViewIdx(0));
    const RawWbPresets::Database& database = RawWbPresets::database();
    const std::vector<RawWbPresets::CameraPreset> presets = RawWbPresets::cameraPresets(database, sourceMake(source), sourceModel(source));
    std::vector<ChoiceOption> options;
    for (const RawWbPresets::CameraPreset& p : presets) {
        options.push_back(ChoiceOption(p.name, p.name, std::string()));
    }
    const std::vector<ChoiceOption> current = presetKnob->getEntries_mt_safe();
    const bool same = std::equal(current.begin(), current.end(), options.begin(), options.end(),
                                 [](const ChoiceOption& a, const ChoiceOption& b) { return a.id == b.id; });
    if (!same) {
        // Pins the chosen name first, so the new menu restores it by name, not by position.
        (void)presetKnob->getActiveEntry();
        presetKnob->populateChoices(options);
    }

    // darktable shows Finetune only for a setting with tunings, over their range.
    const std::string chosen = presetKnob->getActiveEntry().id;
    const auto found = std::find_if(presets.begin(), presets.end(), [&](const RawWbPresets::CameraPreset& p) { return p.name == chosen; });
    const bool tuned = (found != presets.end()) && (found->minTuningPos != found->maxTuningPos);
    if (tuned) {
        const int lowest = database[found->minTuningPos].tuning;
        const int highest = database[found->maxTuningPos].tuning;
        finetuneKnob->setMinimum(lowest);
        finetuneKnob->setMaximum(highest);
        finetuneKnob->setDisplayMinimum(lowest);
        finetuneKnob->setDisplayMaximum(highest);
    }
    finetuneKnob->setSecret(!tuned);
} // RawTemperature::refreshPresets

void
RawTemperature::onKnobsLoaded()
{
    refreshPresets();
    refreshSpotDefaults();
    refreshEnabled();
}

void
RawTemperature::onInputChanged(int /*inputNo*/)
{
    refreshPresets();
    refreshSpotDefaults();
}

void
RawTemperature::onMetadataRefreshed(const NodeMetadata& metadata)
{
    ImageNode<RawTemperatureOp>::onMetadataRefreshed(metadata);
    refreshPresets();
    refreshSpotDefaults();
}

void
RawTemperature::setCoefficientKnobs(const RawColorMath::Vec3& coeffs)
{
    const KnobDoubleWPtr knobs[3] = { op().red, op().green, op().blue };

    for (int c = 0; c < 3; ++c) {
        if (KnobDoublePtr knob = knobs[c].lock()) {
            knob->setValue(coeffs[c]);
        }
    }
}

void
RawTemperature::setTemperatureKnobs(const RawColorMath::CameraMatrices& matrices,
                                    const RawColorMath::Vec3& coeffs)
{
    float tempK = 0.f;
    float tintValue = 0.f;

    RawColorMath::temperatureForMultipliers(matrices, coeffs, &tempK, &tintValue);
    if (KnobDoublePtr knob = op().temperature.lock()) {
        knob->setValue((double)tempK);
    }
    if (KnobDoublePtr knob = op().tint.lock()) {
        knob->setValue((double)tintValue);
    }
}

bool
RawTemperature::knobChanged(KnobI* k,
                            ValueChangedReasonEnum reason,
                            ViewSpec view,
                            double time,
                            bool /*originatedFromMainThread*/)
{
    const RawTemperatureOp& o = op();
    const KnobChoicePtr modeKnob = o.mode.lock();
    const KnobDoublePtr temperatureKnob = o.temperature.lock();
    const KnobDoublePtr tintKnob = o.tint.lock();
    const KnobDoublePtr redKnob = o.red.lock();
    const KnobDoublePtr greenKnob = o.green.lock();
    const KnobDoublePtr blueKnob = o.blue.lock();
    const KnobChoicePtr presetKnob = o.preset.lock();
    const KnobIntPtr finetuneKnob = o.finetune.lock();
    const KnobButtonPtr pickKnob = o.pick.lock();
    const bool isPick = pickKnob && (k == pickKnob.get());
    const bool isMode = modeKnob && (k == modeKnob.get());
    const bool isPreset = (presetKnob && (k == presetKnob.get())) || (finetuneKnob && (k == finetuneKnob.get()));
    const bool isTemperature = (temperatureKnob && (k == temperatureKnob.get())) || (tintKnob && (k == tintKnob.get()));
    const bool isCoefficient = (redKnob && (k == redKnob.get())) || (greenKnob && (k == greenKnob.get())) || (blueKnob && (k == blueKnob.get()));

    if (!isMode && !isTemperature && !isCoefficient && !isPreset && !isPick) {
        return false;
    }
    if (isMode) {
        refreshEnabled();
    }
    if (presetKnob && (k == presetKnob.get())) {
        refreshPresets();
    }
    AppInstancePtr app = getApp();
    const bool loading = app && app->getProject() && app->getProject()->isLoadingProject();
    // A project load or a time change sets every knob to what was saved, which is already
    // consistent; only an edit moves the others.
    if (_syncing || loading || (reason == eValueChangedReasonTimeChanged) || (reason == eValueChangedReasonSlaveRefresh) || (reason == eValueChangedReasonRestoreDefault)) {
        return true;
    }
    const ViewIdx viewIdx = (view.isAll() || view.isCurrent()) ? ViewIdx(0) : ViewIdx(view);
    if (isPick) {
        (void)pickSpot(time, viewIdx);

        return true;
    }
    _syncing = true;
    const ImageMetadata source = getSourceMetadata(time, ViewIdx(0));
    const RawColorMath::CameraMatrices matrices = RawTemperatureOp::sourceMatrices(source);
    if (isTemperature) {
        const RawColorMath::Vec3 coeffs = RawColorMath::multipliersForTemperature(matrices, temperatureKnob->getValueAtTime(time), tintKnob->getValueAtTime(time));
        setCoefficientKnobs(coeffs);
        modeKnob->setValue((int)eRawTemperatureModeCustom);
    } else if (isCoefficient) {
        setTemperatureKnobs(matrices, { redKnob->getValueAtTime(time), greenKnob->getValueAtTime(time), blueKnob->getValueAtTime(time) });
        modeKnob->setValue((int)eRawTemperatureModeCustom);
    } else {
        // Choosing a preset or its tuning chooses the Camera Preset mode, as darktable's
        // settings menu does.
        if (isPreset) {
            modeKnob->setValue((int)eRawTemperatureModeCameraPreset);
        }
        // Showing what a non-custom mode resolves to is also where Custom starts from. Choosing
        // From Image Area picks, as darktable's mode menu switches its picker on.
        const int modeIndex = modeKnob->getValue();
        if (modeIndex == (int)eRawTemperatureModeFromImageArea) {
            (void)pickSpot(time, viewIdx);
        } else if (modeIndex != (int)eRawTemperatureModeCustom) {
            const std::string presetName = presetKnob ? presetKnob->getActiveEntry().id : std::string();
            const int tune = finetuneKnob ? finetuneKnob->getValue() : 0;
            const RawColorMath::Vec3 coeffs = resolvedMultipliers(modeIndex, source, presetName, tune);
            setCoefficientKnobs(coeffs);
            setTemperatureKnobs(matrices, coeffs);
        }
    }
    refreshEnabled();
    _syncing = false;

    return true;
} // RawTemperature::knobChanged

ImageMetadata
RawTemperature::deriveOutputMetadata(double time,
                                     ViewIdx view)
{
    ImageMetadata metadata = ImageNode<RawTemperatureOp>::deriveOutputMetadata(time, view);
    const std::optional<RawStageEnum> stage = RawMetadata::getStage(metadata);

    if (!stage || (*stage != eRawStagePrepared)) {
        return metadata;
    }
    const RawTemperatureOp::Params params = bakeParams(op(), time, view, metadata);
    const std::array<double, 3> applied = { params.coeffs[0], params.coeffs[1], params.coeffs[2] };
    std::array<double, 3> maximum = RawMetadata::getProcessedMaximum(metadata).value_or(std::array<double, 3> { 1., 1., 1. });
    for (int c = 0; c < 3; ++c) {
        maximum[c] *= applied[c];
    }
    RawMetadata::setWbApplied(&metadata, applied);
    RawMetadata::setProcessedMaximum(&metadata, maximum);
    // darktable's reload_defaults() keeps both in dev->chroma whatever the mode, and
    // _publish_chroma() sets late_correction for "as shot to reference" alone; RawInputColor
    // multiplies by D65coeffs / as_shot before its matrix while it is set.
    const RawColorMath::Vec3 d65 = RawTemperatureOp::modeMultipliers(eRawTemperatureModeCameraReference, metadata);
    RawMetadata::setWbD65Coeffs(&metadata, { d65[0], d65[1], d65[2] });
    const KnobChoicePtr modeKnob = op().mode.lock();
    if (modeKnob && (modeKnob->getValueAtTime(time) == (int)eRawTemperatureModeAsShotToReference)) {
        const RawColorMath::Vec3 asShot = RawTemperatureOp::modeMultipliers(eRawTemperatureModeAsShot, metadata);
        RawMetadata::setWbLateCorrection(&metadata, { d65[0] / asShot[0], d65[1] / asShot[1], d65[2] / asShot[2] });
    } else {
        metadata.remove(kRawMetadataWbLateCorrection);
    }

    return metadata;
}

NATRON_NAMESPACE_EXIT
