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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "HSVKeyer.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/KnobTypes.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Color/HueCurves.h"
#include "Engine/Project.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
#ifdef NATRON_HAVE_GPU
// The parameter block of HSVKeyerOp in hue.slang, which has no padding between members.
struct HSVKeyerGpuParams {
    uint32_t flags;
    float hue[4];
    float sat[4];
    float luma[4];
    float toLms[9];
};
static_assert(sizeof(HSVKeyerGpuParams) == 22 * 4, "must match HSVKeyerOp in hue.slang");

// Flag bits, as hue.slang reads them.
const uint32_t kGpuFlagInvert = 1u;
const uint32_t kGpuFlagUseHue = 2u;
const uint32_t kGpuFlagUseSat = 4u;
const uint32_t kGpuFlagUseLuma = 8u;
const uint32_t kGpuModelShift = 4u;
const uint32_t kGpuOutputShift = 6u;
#endif

KnobDoublePtr
addRange(KnobHolder* holder,
         const KnobPagePtr& page,
         const char* name,
         const std::string& label,
         const std::string& hint,
         const double defaults[4])
{
    KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(holder, label, 4);

    knob->setName(name);
    knob->setHintToolTip(hint);
    knob->setDimensionName(0, "outerLow");
    knob->setDimensionName(1, "innerLow");
    knob->setDimensionName(2, "innerHigh");
    knob->setDimensionName(3, "outerHigh");
    for (int i = 0; i < 4; ++i) {
        knob->setMinimum(-DBL_MAX, i);
        knob->setMaximum(DBL_MAX, i);
        knob->setDisplayMinimum(0., i);
        knob->setDisplayMaximum(1., i);
        knob->setDefaultValue(defaults[i], i);
    }
    page->addKnob(knob);

    return knob;
}

KnobBoolPtr
addBool(KnobHolder* holder,
        const KnobPagePtr& page,
        const char* name,
        const std::string& label,
        const std::string& hint,
        bool defaultValue,
        bool newLine)
{
    KnobBoolPtr knob = AppManager::createKnob<KnobBool>(holder, label);

    knob->setName(name);
    knob->setHintToolTip(hint);
    knob->setDefaultValue(defaultValue);
    knob->setAddNewLine(newLine);
    page->addKnob(knob);

    return knob;
}

// x wrapped into [0, 1).
double
wrapUnit(double x)
{
    const double w = x - std::floor(x);

    return (w >= 1.) ? 0. : w;
}

ColorMath::KeyModelEnum
modelAt(int index)
{
    if ((index < 0) || (index >= (int)ColorMath::eKeyModelCount)) {
        return ColorMath::eKeyModelHSV;
    }

    return (ColorMath::KeyModelEnum)index;
}

HSVKeyerOp::OutputModeEnum
outputAt(int index)
{
    if ((index < 0) || (index > (int)HSVKeyerOp::eOutputModeMatte)) {
        return HSVKeyerOp::eOutputModeAlpha;
    }

    return (HSVKeyerOp::OutputModeEnum)index;
}

ColorMath::OkLabBasis
basisOf(const KnobHolder* holder)
{
    double toXYZ[9];

    ColorMath::workingToXYZ(holder, toXYZ);

    return ColorMath::okLabBasis(toXYZ);
}
} // anonymous namespace

void
HSVKeyerOp::createKnobs(KnobHolder* knobHolder,
                        const KnobPagePtr& page)
{
    holder = knobHolder;
    {
        KnobColorPtr knob = AppManager::createKnob<KnobColor>(knobHolder, EffectInstance::tr("Source Color").toStdString(), 3);
        knob->setName(kHSVKeyerParamSourceColor);
        knob->setHintToolTip(EffectInstance::tr("The colour to key, picked from the viewer with the swatch button or typed. Setting it moves the three ranges so their plateaus are centred on its components, keeping their widths.").toStdString());
        for (int i = 0; i < 3; ++i) {
            knob->setMinimum(-DBL_MAX, i);
            knob->setMaximum(DBL_MAX, i);
            knob->setDisplayMinimum(0., i);
            knob->setDisplayMaximum(1., i);
        }
        knob->setDefaultValue(0., 0);
        knob->setDefaultValue(1., 1);
        knob->setDefaultValue(0., 2);
        // The source colour drives the ranges, not the render.
        knob->setEvaluateOnChange(false);
        page->addKnob(knob);
        sourceColor = knob;
    }
    {
        KnobChoicePtr knob = AppManager::createKnob<KnobChoice>(knobHolder, EffectInstance::tr("Color Model").toStdString());
        knob->setName(kHSVKeyerParamColorModel);
        knob->setHintToolTip(EffectInstance::tr("The model the hue, saturation and luma ranges are in. Changing it centres the ranges on the source colour's components in the model.").toStdString());
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("hsv", "HSV", EffectInstance::tr("Hue, saturation (max - min) / max and value, the largest of R, G and B.").toStdString()));
        options.push_back(ChoiceOption("hsl", "HSL", EffectInstance::tr("Hue, saturation and lightness, the mean of the largest and smallest of R, G and B.").toStdString()));
        options.push_back(ChoiceOption("oklch", "OkLCh", EffectInstance::tr("OkLab's hue, chroma (about 0 to 0.4) and lightness, from the working space adapted to D65: perceptually even steps.").toStdString()));
        knob->populateChoices(options);
        knob->setDefaultValue((int)ColorMath::eKeyModelHSV);
        knob->setAnimationEnabled(false);
        knob->setAddNewLine(false);
        page->addKnob(knob);
        colorModel = knob;
    }
    {
        KnobButtonPtr knob = AppManager::createKnob<KnobButton>(knobHolder, EffectInstance::tr("Recentre").toStdString());
        knob->setName(kHSVKeyerParamRecentre);
        knob->setHintToolTip(EffectInstance::tr("Centre the three ranges on the source colour's components, keeping their widths.").toStdString());
        knob->setEvaluateOnChange(false);
        page->addKnob(knob);
        recentre = knob;
    }
    const double hueDefaults[4] = { 0.25, 0.30, 0.37, 0.42 };
    const double satDefaults[4] = { 0.1, 0.3, 1., 1. };
    const double lumaDefaults[4] = { 0., 0.05, 1., 2. };
    KnobDoublePtr hue = addRange(knobHolder, page, kHSVKeyerParamHueRange, EffectInstance::tr("Hue Range").toStdString(),
                                 EffectInstance::tr("The hue trapezoid, in turns from red at 0: the key rises from 0 at the outer low point to 1 at the inner low point, stays 1 to the inner high point and falls to 0 at the outer high point, each point going up the hue circle from the one before, so the range may cross red. Hue counts for less as the chroma nears 0.").toStdString(),
                                 hueDefaults);
    KnobDoublePtr sat = addRange(knobHolder, page, kHSVKeyerParamSaturationRange, EffectInstance::tr("Saturation Range").toStdString(),
                                 EffectInstance::tr("The saturation trapezoid (chroma in OkLCh). A point below the one before counts as equal to it.").toStdString(),
                                 satDefaults);
    KnobDoublePtr luma = addRange(knobHolder, page, kHSVKeyerParamLumaRange, EffectInstance::tr("Luma Range").toStdString(),
                                  EffectInstance::tr("The trapezoid over the model's value, lightness or OkLab lightness. A point below the one before counts as equal to it.").toStdString(),
                                  lumaDefaults);
    KnobColorPtr src = sourceColor.lock();
    KnobChoicePtr model = colorModel.lock();
    hue->setRangeWidget(KnobDouble::eRangeWidgetRoleHue, src, model);
    sat->setRangeWidget(KnobDouble::eRangeWidgetRoleSaturation, src, model);
    luma->setRangeWidget(KnobDouble::eRangeWidgetRoleLuma, src, model);
    hueRange = hue;
    satRange = sat;
    lumaRange = luma;
    useHue = addBool(knobHolder, page, kHSVKeyerParamUseHue, EffectInstance::tr("Use Hue").toStdString(),
                     EffectInstance::tr("Key by the hue range.").toStdString(), true, false);
    useSat = addBool(knobHolder, page, kHSVKeyerParamUseSaturation, EffectInstance::tr("Use Saturation").toStdString(),
                     EffectInstance::tr("Key by the saturation range.").toStdString(), true, false);
    useLuma = addBool(knobHolder, page, kHSVKeyerParamUseLuma, EffectInstance::tr("Use Luma").toStdString(),
                      EffectInstance::tr("Key by the luma range.").toStdString(), true, true);
    invert = addBool(knobHolder, page, kHSVKeyerParamInvert, EffectInstance::tr("Invert").toStdString(),
                     EffectInstance::tr("Output one minus the key.").toStdString(), false, true);
    {
        KnobChoicePtr knob = AppManager::createKnob<KnobChoice>(knobHolder, EffectInstance::tr("Output Mode").toStdString());
        knob->setName(kHSVKeyerParamOutputMode);
        knob->setHintToolTip(EffectInstance::tr("What the key does to RGB. Alpha is the key in every mode.").toStdString());
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("alpha", "Alpha", EffectInstance::tr("RGB passes unchanged.").toStdString()));
        options.push_back(ChoiceOption("premultiplied", "Premultiplied", EffectInstance::tr("RGB is multiplied by the key.").toStdString()));
        options.push_back(ChoiceOption("matte", "Matte", EffectInstance::tr("RGB is the key, to view it as grey.").toStdString()));
        knob->populateChoices(options);
        knob->setDefaultValue((int)eOutputModeAlpha);
        knob->setAnimationEnabled(false);
        page->addKnob(knob);
        outputMode = knob;
    }
} // HSVKeyerOp::createKnobs

HSVKeyerOp::Params
HSVKeyerOp::bake(const KnobReader& reader) const
{
    Params p;

    p.model = modelAt(reader.value(colorModel));
    p.output = outputAt(reader.value(outputMode));
    p.useHue = reader.value(useHue);
    p.useSat = reader.value(useSat);
    p.useLuma = reader.value(useLuma);
    p.invert = reader.value(invert);

    const double low = reader.value(hueRange, 0);
    const double lowFull = reader.value(hueRange, 1);
    const double highFull = reader.value(hueRange, 2);
    const double high = reader.value(hueRange, 3);
    // The distances going up the hue circle, each less than a turn.
    const double rise = lowFull - low - std::floor(lowFull - low);
    const double full = highFull - lowFull - std::floor(highFull - lowFull);
    const double fall = high - highFull - std::floor(high - highFull);
    p.hueLow = (float)wrapUnit(low);
    p.hueRise = (float)rise;
    p.hueFull = (float)(rise + full);
    p.hueFall = (float)(rise + full + fall);

    // Out-of-order points are pushed up to the one before.
    double s[4];
    double l[4];
    for (int i = 0; i < 4; ++i) {
        s[i] = reader.value(satRange, i);
        l[i] = reader.value(lumaRange, i);
        if (i > 0) {
            s[i] = (std::max)(s[i], s[i - 1]);
            l[i] = (std::max)(l[i], l[i - 1]);
        }
        p.sat[i] = (float)s[i];
        p.luma[i] = (float)l[i];
    }
    if (p.model == ColorMath::eKeyModelOkLCh) {
        p.basis = basisOf(holder);
    }

    return p;
} // HSVKeyerOp::bake

bool
HSVKeyerOp::isIdentity(const Params& params) const
{
    // The key always replaces alpha.
    Q_UNUSED(params);

    return false;
}

float
HSVKeyerOp::key(const Params& params,
                const float* rgb)
{
    float comps[3];
    float hueWeight;

    ColorMath::keyModelComponents(params.model, params.basis, rgb, comps, &hueWeight);
    float k = 1.f;
    if (params.useHue) {
        const float hk = HueCurves::hueTrapezoid(comps[0], params.hueLow, params.hueRise, params.hueFull, params.hueFall);
        const float weighted = hk * hueWeight;
        k = k * weighted;
    }
    if (params.useSat) {
        const float sk = HueCurves::trapezoid(comps[1], params.sat[0], params.sat[1], params.sat[2], params.sat[3]);
        k = k * sk;
    }
    if (params.useLuma) {
        const float lk = HueCurves::trapezoid(comps[2], params.luma[0], params.luma[1], params.luma[2], params.luma[3]);
        k = k * lk;
    }

    return params.invert ? (1.f - k) : k;
}

void
HSVKeyerOp::row(const Params& params,
                const RowIO& io) const
{
    const int nComps = io.nComps;

    // getPreferredMetadata() makes the output RGBA; anything else has no alpha for the key.
    if (nComps != 4) {
        std::copy(io.src[0], io.src[0] + io.width * nComps, io.dst);

        return;
    }
    bool process[4];
    for (int c = 0; c < 4; ++c) {
        process[c] = io.channels[pixelKernelChannelBit(nComps, c)];
    }
    // In Alpha mode RGB is the source, which the pass-through already wrote.
    const bool writeRgb = (params.output != eOutputModeAlpha) && (process[0] || process[1] || process[2]);
    if (!writeRgb && !process[3]) {
        return;
    }
    const float* src = io.src[0];
    float* dst = io.dst;
    for (int x = 0; x < io.width; ++x, src += 4, dst += 4) {
        const float k = key(params, src);
        if (writeRgb) {
            for (int c = 0; c < 3; ++c) {
                if (process[c]) {
                    dst[c] = (params.output == eOutputModeMatte) ? k : (src[c] * k);
                }
            }
        }
        if (process[3]) {
            dst[3] = k;
        }
    }
}

GpuOpRef
HSVKeyerOp::gpu(const Params& params) const
{
#ifdef NATRON_HAVE_GPU
    HSVKeyerGpuParams p = {};
    p.flags = (params.invert ? kGpuFlagInvert : 0u)
        | (params.useHue ? kGpuFlagUseHue : 0u)
        | (params.useSat ? kGpuFlagUseSat : 0u)
        | (params.useLuma ? kGpuFlagUseLuma : 0u)
        | ((uint32_t)params.model << kGpuModelShift)
        | ((uint32_t)params.output << kGpuOutputShift);
    p.hue[0] = params.hueLow;
    p.hue[1] = params.hueRise;
    p.hue[2] = params.hueFull;
    p.hue[3] = params.hueFall;
    for (int i = 0; i < 4; ++i) {
        p.sat[i] = params.sat[i];
        p.luma[i] = params.luma[i];
    }
    for (int i = 0; i < 9; ++i) {
        p.toLms[i] = params.basis.toLms[i];
    }
    GpuOpRef ref = GpuOpRef::compiled((int)gpu::GpuKernelId::hsvkeyer_wrapped, p);
    ref.acceptedComponents = GpuOpRef::componentsBit(4);

    return ref;
#else
    Q_UNUSED(params);

    return GpuOpRef::none();
#endif
}

void
HSVKeyerOp::recentreHue(double* points,
                        double hue)
{
    const double low = points[0];
    const double lowFull = points[1];
    const double highFull = points[2];
    const double rise = lowFull - low - std::floor(lowFull - low);
    const double full = highFull - lowFull - std::floor(highFull - lowFull);
    const double centre = low + rise + full * 0.5;
    // The shortest way round, so a range near red does not travel a whole turn.
    double shift = wrapUnit(hue - centre);
    if (shift >= 0.5) {
        shift -= 1.;
    }
    for (int i = 0; i < 4; ++i) {
        points[i] = wrapUnit(points[i] + shift);
    }
}

void
HSVKeyerOp::recentreLinear(double* points,
                           double value)
{
    double ordered[4];

    for (int i = 0; i < 4; ++i) {
        ordered[i] = (i > 0) ? (std::max)(points[i], ordered[i - 1]) : points[i];
    }
    const double shift = value - (ordered[1] + ordered[2]) * 0.5;
    for (int i = 0; i < 4; ++i) {
        points[i] += shift;
    }
}

HSVKeyer::HSVKeyer(NodePtr node)
    : ImageNode<HSVKeyerOp>(node)
{
}

HSVKeyer::~HSVKeyer()
{
}

HSVKeyerOp::Params
HSVKeyer::bakeHSVKeyer(double time,
                       ViewIdx view) const
{
    return bakeParams(op(), time, view);
}

void
HSVKeyer::hsvKeyerRow(const HSVKeyerOp::Params& params,
                      const RowIO& io) const
{
    op().row(params, io);
}

void
HSVKeyer::recentreOnSource(double time)
{
    const HSVKeyerOp& o = op();
    const KnobColorPtr src = o.sourceColor.lock();
    const KnobChoicePtr model = o.colorModel.lock();
    if (!src || !model) {
        return;
    }
    const ColorMath::KeyModelEnum m = modelAt(model->getValue());
    const ColorMath::OkLabBasis basis = (m == ColorMath::eKeyModelOkLCh) ? basisOf(this) : ColorMath::OkLabBasis();
    const float rgb[3] = {
        (float)src->getValueAtTime(time, 0), (float)src->getValueAtTime(time, 1), (float)src->getValueAtTime(time, 2)
    };
    float comps[3];
    float hueWeight;
    ColorMath::keyModelComponents(m, basis, rgb, comps, &hueWeight);

    const KnobDoublePtr ranges[3] = { o.hueRange.lock(), o.satRange.lock(), o.lumaRange.lock() };
    AppInstancePtr app = getApp();
    const bool oneUndoStep = app && !app->isCreatingPythonGroup();
    if (oneUndoStep) {
        setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOnCreateNewCommand);
    }
    for (int r = 0; r < 3; ++r) {
        const KnobDoublePtr& knob = ranges[r];
        // A source with no chroma has no hue to centre on.
        if (!knob || ((r == 0) && !(hueWeight > 0.f))) {
            continue;
        }
        double points[4];
        for (int i = 0; i < 4; ++i) {
            points[i] = knob->getValueAtTime(time, i);
        }
        if (r == 0) {
            HSVKeyerOp::recentreHue(points, comps[0]);
        } else {
            HSVKeyerOp::recentreLinear(points, comps[r]);
        }
        for (int i = 0; i < 4; ++i) {
            knob->setValue(points[i], ViewSpec::all(), i);
        }
    }
    if (oneUndoStep) {
        setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOff);
    }
} // HSVKeyer::recentreOnSource

bool
HSVKeyer::knobChanged(KnobI* k,
                      ValueChangedReasonEnum reason,
                      ViewSpec view,
                      double time,
                      bool originatedFromMainThread)
{
    const HSVKeyerOp& o = op();
    const KnobColorPtr src = o.sourceColor.lock();
    const KnobChoicePtr model = o.colorModel.lock();
    const KnobButtonPtr button = o.recentre.lock();
    const bool isSrc = src && (k == src.get());
    const bool isModel = model && (k == model.get());
    const bool isButton = button && (k == button.get());

    if (!isSrc && !isModel && !isButton) {
        return ImageNode<HSVKeyerOp>::knobChanged(k, reason, view, time, originatedFromMainThread);
    }
    AppInstancePtr app = getApp();
    const bool loading = app && app->getProject() && app->getProject()->isLoadingProject();
    // A project load or a time change sets every knob to what was saved; only an edit moves the
    // ranges.
    if (loading || (reason == eValueChangedReasonTimeChanged) || (reason == eValueChangedReasonSlaveRefresh) || (reason == eValueChangedReasonRestoreDefault)) {
        return true;
    }
    recentreOnSource(time);

    return true;
} // HSVKeyer::knobChanged

void
HSVKeyer::addAcceptedComponents(int inputNb,
                                std::list<ImageLayerDesc>* comps)
{
    if (isInputMask(inputNb)) {
        StagedImageEffect::addAcceptedComponents(inputNb, comps);

        return;
    }
    comps->push_back(ImageLayerDesc::getRGBAComponents());
    comps->push_back(ImageLayerDesc::getRGBComponents());
}

StatusEnum
HSVKeyer::getPreferredMetadata(NodeMetadata& metadata)
{
    metadata.setNComps(-1, 4);

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
