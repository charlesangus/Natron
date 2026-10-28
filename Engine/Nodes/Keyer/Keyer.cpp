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

#include "Keyer.h"

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <string>
#include <vector>

#include <ofxNatron.h>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/KnobTypes.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/ColorMath.h"
#include "Engine/Nodes/Keyer/KeyerMatte.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
// The luminanceMath option that takes the project's working-space luma weights; the options
// after it are ColorMath::luminanceMathOptions(), shifted by one.
const int kLuminanceMathWorkingSpace = 0;

const double kRangeDefaults[4] = { 0., 0.1, 0.9, 1. };

#ifdef NATRON_HAVE_GPU
// The parameter block of KeyerOp in keyer.slang, which has no padding between members.
struct KeyerGpuParams {
    uint32_t flags;
    float weights[3];
    float range[4];
};
static_assert(sizeof(KeyerGpuParams) == 8 * 4, "must match KeyerOp in keyer.slang");

const uint32_t kKeyerGpuLumaMax = 1u;
#endif

void
workingLuma(const KnobHolder* holder,
            double w[3])
{
    const double* fallback = ColorMath::acesCgLumaWeights();

    for (int i = 0; i < 3; ++i) {
        w[i] = fallback[i];
    }
    AppInstancePtr app = holder ? holder->getApp() : AppInstancePtr();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    ProjectColorManagementPtr cm = project ? project->getColorManagement() : ProjectColorManagementPtr();
    if (cm) {
        cm->getWorkingLuma(w);
    }
}
} // anonymous namespace

void
KeyerOp::createKnobs(KnobHolder* knobHolder,
                     const KnobPagePtr& page)
{
    holder = knobHolder;

    // Node.cpp shows this knob's value in parentheses next to the node's label. It moves to a
    // native sublabel hook once the node graph has one.
    {
        KnobStringPtr knob = AppManager::createKnob<KnobString>(knobHolder, std::string(kNatronOfxParamStringSublabelName));
        knob->setName(kNatronOfxParamStringSublabelName);
        knob->setSecretByDefault(true);
        knob->setDefaultAllDimensionsEnabled(false);
        knob->setIsPersistent(false);
        knob->setEvaluateOnChange(false);
        knob->setAnimationEnabled(false);
        knob->setDefaultValue(std::string("Luminance"));
        page->addKnob(knob);
        subLabel = knob;
    }
    {
        KnobChoicePtr knob = AppManager::createKnob<KnobChoice>(knobHolder, EffectInstance::tr("Key Source").toStdString());
        knob->setName(kKeyerParamKeySource);
        knob->setHintToolTip(EffectInstance::tr("What the key value is computed from.").toStdString());
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("luminance", "Luminance", EffectInstance::tr("The luminance of RGB, by the Luminance Math formula.").toStdString()));
        options.push_back(ChoiceOption("color", "Color", EffectInstance::tr("The dot product of RGB with the key colour over the sum of the key colour's components: 1 at a pure primary key colour.").toStdString()));
        knob->populateChoices(options);
        knob->setDefaultValue((int)eKeySourceLuminance);
        page->addKnob(knob);
        keySource = knob;
    }
    {
        KnobColorPtr knob = AppManager::createKnob<KnobColor>(knobHolder, EffectInstance::tr("Key Color").toStdString(), 3);
        knob->setName(kKeyerParamKeyColor);
        knob->setHintToolTip(EffectInstance::tr("The colour Color mode projects RGB on. A key colour whose components sum to 0 keys by luminance.").toStdString());
        for (int i = 0; i < 3; ++i) {
            knob->setDefaultValue(0., i);
            knob->setMinimum(-DBL_MAX, i);
            knob->setMaximum(DBL_MAX, i);
            knob->setDisplayMinimum(0., i);
            knob->setDisplayMaximum(1., i);
        }
        // Hidden under the default Luminance key source; refreshKeySourceDependents() shows it for Colour.
        knob->setSecret(true);
        page->addKnob(knob);
        keyColor = knob;
    }
    {
        KnobChoicePtr knob = AppManager::createKnob<KnobChoice>(knobHolder, std::string(kColorMathParamLuminanceMathLabel));
        knob->setName(kKeyerParamLuminanceMath);
        knob->setHintToolTip(std::string(kColorMathParamLuminanceMathHint));
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("working", "Working Space", EffectInstance::tr("Use the luma weights of the project's working colour space.").toStdString()));
        const std::vector<ChoiceOption> formulas = ColorMath::luminanceMathOptions();
        options.insert(options.end(), formulas.begin(), formulas.end());
        knob->populateChoices(options);
        knob->setDefaultValue(kLuminanceMathWorkingSpace);
        page->addKnob(knob);
        luminanceMath = knob;
    }
    {
        KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(knobHolder, EffectInstance::tr("Range").toStdString(), 4);
        knob->setName(kKeyerParamRange);
        knob->setHintToolTip(EffectInstance::tr("The trapezoid from the key value to alpha: 0 up to A, rising to 1 at B, 1 up to C, falling to 0 at D. "
                                                "Moving one value pushes its neighbours so that A <= B <= C <= D.")
                                 .toStdString());
        const char* const names[4] = { "A", "B", "C", "D" };
        for (int i = 0; i < 4; ++i) {
            knob->setDimensionName(i, names[i]);
            knob->setDefaultValue(kRangeDefaults[i], i);
            knob->setMinimum(-DBL_MAX, i);
            knob->setMaximum(DBL_MAX, i);
            knob->setDisplayMinimum(0., i);
            knob->setDisplayMaximum(1., i);
        }
        page->addKnob(knob);
        range = knob;
    }
}

KeyerOp::Params
KeyerOp::bake(const KnobReader& reader) const
{
    Params p;
    const int source = reader.value(keySource);

    p.keySource = (source == (int)eKeySourceColor) ? eKeySourceColor : eKeySourceLuminance;

    double w[3] = { 0., 0., 0. };
    bool weighted = false;
    if (p.keySource == eKeySourceColor) {
        const double key[3] = { reader.value(keyColor, 0), reader.value(keyColor, 1), reader.value(keyColor, 2) };
        const double sum = key[0] + key[1] + key[2];
        if (sum != 0.) {
            for (int i = 0; i < 3; ++i) {
                w[i] = key[i] / sum;
            }
            weighted = true;
        }
    }
    if (!weighted) {
        const int math = reader.value(luminanceMath) - 1;
        if ((math < (int)ColorMath::eLuminanceMathRec709) || (math > (int)ColorMath::eLuminanceMathMax)) {
            workingLuma(holder, w);
        } else if (math == (int)ColorMath::eLuminanceMathMax) {
            p.lumaMax = true;
        } else {
            const ColorMath::LuminanceMathEnum e = (ColorMath::LuminanceMathEnum)math;
            w[0] = ColorMath::luminance(e, 1., 0., 0.);
            w[1] = ColorMath::luminance(e, 0., 1., 0.);
            w[2] = ColorMath::luminance(e, 0., 0., 1.);
        }
    }
    for (int i = 0; i < 3; ++i) {
        p.weights[i] = (float)w[i];
    }

    const KeyerTrapezoid t = sortedTrapezoid(reader.value(range, 0), reader.value(range, 1), reader.value(range, 2), reader.value(range, 3));
    p.range[0] = (float)t.a;
    p.range[1] = (float)t.b;
    p.range[2] = (float)t.c;
    p.range[3] = (float)t.d;

    return p;
}

bool
KeyerOp::isIdentity(const Params& params) const
{
    // The key always replaces alpha.
    Q_UNUSED(params);

    return false;
}

float
KeyerOp::keyValue(const Params& params,
                  const float* rgb)
{
    if (params.lumaMax) {
        return (std::max)((std::max)(rgb[0], rgb[1]), rgb[2]);
    }
    float s = params.weights[0] * rgb[0];
    const float g = params.weights[1] * rgb[1];
    s = s + g;
    const float b = params.weights[2] * rgb[2];
    s = s + b;

    return s;
}

void
KeyerOp::row(const Params& params,
             const RowIO& io) const
{
    const int nComps = io.nComps;

    // getPreferredMetadataAt() makes the output RGBA; anything else has no alpha for the key.
    if (nComps != 4) {
        std::copy(io.src[0], io.src[0] + io.width * nComps, io.dst);

        return;
    }
    bool process[4];
    for (int c = 0; c < 4; ++c) {
        process[c] = io.channels[pixelKernelChannelBit(nComps, c)];
    }
    const float* src = io.src[0];
    float* dst = io.dst;
    for (int x = 0; x < io.width; ++x, src += 4, dst += 4) {
        // io.dst starts undefined and the stages read back every processed channel.
        for (int c = 0; c < 3; ++c) {
            if (process[c]) {
                dst[c] = src[c];
            }
        }
        if (process[3]) {
            dst[3] = trapezoidMatteFloat(keyValue(params, src), params.range);
        }
    }
}

GpuOpRef
KeyerOp::gpu(const Params& params) const
{
#ifdef NATRON_HAVE_GPU
    KeyerGpuParams p = {};
    p.flags = params.lumaMax ? kKeyerGpuLumaMax : 0u;
    for (int i = 0; i < 3; ++i) {
        p.weights[i] = params.weights[i];
    }
    for (int i = 0; i < 4; ++i) {
        p.range[i] = params.range[i];
    }
    GpuOpRef ref = GpuOpRef::compiled((int)gpu::GpuKernelId::keyer_wrapped, p);
    ref.acceptedComponents = GpuOpRef::componentsBit(4);

    return ref;
#else
    Q_UNUSED(params);

    return GpuOpRef::none();
#endif
}

Keyer::Keyer(NodePtr node)
    : ImageNode<KeyerOp>(node)
    , _orderingRange(false)
{
    for (int i = 0; i < 4; ++i) {
        _lastRange[i] = kRangeDefaults[i];
    }
}

Keyer::~Keyer()
{
}

KeyerOp::Params
Keyer::bakeKeyer(double time,
                 ViewIdx view) const
{
    return bakeParams(op(), time, view);
}

void
Keyer::keyerRow(const KeyerOp::Params& params,
                const RowIO& io) const
{
    op().row(params, io);
}

void
Keyer::refreshKeySourceDependents()
{
    const KeyerOp& o = op();
    KnobChoicePtr source = o.keySource.lock();

    if (!source) {
        return;
    }
    const int index = source->getValue();
    const bool color = (index == (int)KeyerOp::eKeySourceColor);
    KnobColorPtr key = o.keyColor.lock();
    if (key) {
        key->setSecret(!color);
    }
    KnobStringPtr label = o.subLabel.lock();
    if (label) {
        label->setValue(source->getEntry(index).label);
    }
}

void
Keyer::orderRange(double time,
                  ViewSpec view)
{
    KnobDoublePtr knob = op().range.lock();

    if (!knob) {
        return;
    }
    Q_UNUSED(view);
    double values[4];
    int changed = 0;
    int edited = -1;
    for (int i = 0; i < 4; ++i) {
        values[i] = knob->getValueAtTime(time, i);
        if (values[i] != _lastRange[i]) {
            ++changed;
            edited = i;
        }
    }
    double ordered[4] = { values[0], values[1], values[2], values[3] };
    if (changed == 1) {
        pushTrapezoidNeighbours(ordered, edited);
    } else {
        const KeyerTrapezoid t = sortedTrapezoid(values[0], values[1], values[2], values[3]);
        ordered[0] = t.a;
        ordered[1] = t.b;
        ordered[2] = t.c;
        ordered[3] = t.d;
    }
    // Setting a value calls knobChanged() again, which only records it.
    _orderingRange = true;
    for (int i = 0; i < 4; ++i) {
        if (ordered[i] == values[i]) {
            continue;
        }
        if (knob->isAnimated(i)) {
            knob->setValueAtTime(time, ordered[i], ViewSpec::all(), i);
        } else {
            knob->setValue(ordered[i], ViewSpec::all(), i);
        }
    }
    _orderingRange = false;
    for (int i = 0; i < 4; ++i) {
        _lastRange[i] = ordered[i];
    }
}

bool
Keyer::knobChanged(KnobI* k,
                   ValueChangedReasonEnum reason,
                   ViewSpec view,
                   double time,
                   bool originatedFromMainThread)
{
    const KeyerOp& o = op();
    KnobChoicePtr source = o.keySource.lock();
    KnobDoublePtr rangeKnob = o.range.lock();

    if (source && (k == source.get())) {
        refreshKeySourceDependents();

        return true;
    }
    if (!rangeKnob || (k != rangeKnob.get())) {
        return ImageNode<KeyerOp>::knobChanged(k, reason, view, time, originatedFromMainThread);
    }
    if (_orderingRange) {
        return true;
    }
    AppInstancePtr app = getApp();
    const bool loading = app && app->getProject() && app->getProject()->isLoadingProject();
    if (loading || (reason == eValueChangedReasonTimeChanged) || (reason == eValueChangedReasonSlaveRefresh) || (reason == eValueChangedReasonRestoreDefault)) {
        // The values are what was saved, animated or restored; the render orders them.
        for (int i = 0; i < 4; ++i) {
            _lastRange[i] = rangeKnob->getValueAtTime(time, i);
        }

        return true;
    }
    orderRange(time, view);

    return true;
}

void
Keyer::onKnobsLoaded()
{
    KnobDoublePtr rangeKnob = op().range.lock();

    if (rangeKnob) {
        for (int i = 0; i < 4; ++i) {
            _lastRange[i] = rangeKnob->getValueAtTime(getCurrentTime(), i);
        }
    }
    refreshKeySourceDependents();
}

void
Keyer::addAcceptedComponents(int inputNb,
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
Keyer::getPreferredMetadataAt(double time,
                              ViewIdx view,
                              NodeMetadata& metadata)
{
    Q_UNUSED(time);
    Q_UNUSED(view);
    metadata.setNComps(-1, 4);

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
