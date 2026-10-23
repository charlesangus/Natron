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
 * developers, distributed under the GNU GPL version 3 or later.
 */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "RawTemperature.h"

#include <cmath>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Raw/RawGpuParams.h"
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
    modeKnob->populateChoices(modes);
    modeKnob->setDefaultValue((int)eRawTemperatureModeAsShot);
    modeKnob->setAnimationEnabled(false);
    page->addKnob(modeKnob);
    mode = modeKnob;

    temperature = addDoubleKnob(holder, page, kRawTemperatureParamTemperature, EffectInstance::tr("Temperature"),
                                EffectInstance::tr("The colour temperature, in kelvin, of the light to make neutral. Editing it sets the multipliers through the camera's colour matrix."),
                                5000., RawColorMath::kLowestTemperature, RawColorMath::kHighestTemperature);
    tint = addDoubleKnob(holder, page, kRawTemperatureParamTint, EffectInstance::tr("Tint"),
                         EffectInstance::tr("Below 1 towards magenta, above 1 towards green. Editing it sets the multipliers through the camera's colour matrix."),
                         1., RawColorMath::kLowestTint, RawColorMath::kHighestTint);
    red = addDoubleKnob(holder, page, kRawTemperatureParamRed, EffectInstance::tr("Red"), EffectInstance::tr("The red multiplier."), 1., 0., 8.);
    green = addDoubleKnob(holder, page, kRawTemperatureParamGreen, EffectInstance::tr("Green"), EffectInstance::tr("The green multiplier."), 1., 0., 8.);
    blue = addDoubleKnob(holder, page, kRawTemperatureParamBlue, EffectInstance::tr("Blue"), EffectInstance::tr("The blue multiplier."), 1., 0., 8.);
}

RawColorMath::CameraMatrices
RawTemperatureOp::sourceMatrices(const ImageMetadata& source)
{
    const std::optional<std::array<double, 9>> camToXyz = RawMetadata::getCamToXyz(source);

    if (camToXyz) {
        if (const std::optional<RawColorMath::Mat3> xyzToCam = RawColorMath::invert(*camToXyz)) {
            return RawColorMath::cameraMatrices(*xyzToCam);
        }
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
    const std::optional<std::array<double, 9>> camToXyz = RawMetadata::getCamToXyz(source);
    if (camToXyz) {
        if (const std::optional<RawColorMath::Mat3> xyzToCam = RawColorMath::invert(*camToXyz)) {
            if (const std::optional<RawColorMath::Vec3> reference = RawColorMath::cameraReferenceMultipliers(*xyzToCam)) {
                return *reference;
            }
        }
    }

    return RawColorMath::Vec3 { 2., 1., 1.5 };
}

RawTemperatureOp::Params
RawTemperatureOp::bake(const KnobReader& reader,
                       const ImageMetadata& source) const
{
    Params params;

    params.layout = RawLayout::fromMetadata(source).value_or(RawLayout());
    const int modeIndex = reader.value(mode);
    RawColorMath::Vec3 coeffs;
    if (modeIndex == eRawTemperatureModeCustom) {
        coeffs = { reader.value(red), reader.value(green), reader.value(blue) };
    } else {
        coeffs = modeMultipliers((modeIndex == eRawTemperatureModeCameraReference) ? eRawTemperatureModeCameraReference : eRawTemperatureModeAsShot, source);
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
RawTemperature::onKnobsLoaded()
{
    refreshEnabled();
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
                            ViewSpec /*view*/,
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
    const bool isMode = modeKnob && (k == modeKnob.get());
    const bool isTemperature = (temperatureKnob && (k == temperatureKnob.get())) || (tintKnob && (k == tintKnob.get()));
    const bool isCoefficient = (redKnob && (k == redKnob.get())) || (greenKnob && (k == greenKnob.get())) || (blueKnob && (k == blueKnob.get()));

    if (!isMode && !isTemperature && !isCoefficient) {
        return false;
    }
    if (isMode) {
        refreshEnabled();
    }
    AppInstancePtr app = getApp();
    const bool loading = app && app->getProject() && app->getProject()->isLoadingProject();
    // A project load or a time change sets every knob to what was saved, which is already
    // consistent; only an edit moves the others.
    if (_syncing || loading || (reason == eValueChangedReasonTimeChanged) || (reason == eValueChangedReasonSlaveRefresh) || (reason == eValueChangedReasonRestoreDefault)) {
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
        // Showing what a non-custom mode resolves to is also where Custom starts from.
        const int modeIndex = modeKnob->getValue();
        if (modeIndex != (int)eRawTemperatureModeCustom) {
            const RawColorMath::Vec3 coeffs = RawTemperatureOp::modeMultipliers((RawTemperatureModeEnum)modeIndex, source);
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

    return metadata;
}

NATRON_NAMESPACE_EXIT
