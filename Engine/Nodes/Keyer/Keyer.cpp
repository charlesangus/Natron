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
#include <cstdint>
#include <string>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/KnobTypes.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/ColorMath.h"
#include "Engine/Nodes/Keyer/KeyerMatte.h"
#include "Engine/Nodes/Keyer/KeyerRange.h"
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

// Outer low, inner low, centre, inner high and outer high: the points 0, 0.1, 0.9 and 1.
const double kRangeDefaults[KnobDouble::eRangeWidgetDimensionCount] = { 0.1, 0.4, 0.5, 0.4, 0.1 };

#ifdef NATRON_HAVE_GPU
// The parameter block of KeyerOp in keyer.slang, which has no padding between members.
struct KeyerGpuParams {
    uint32_t flags;
    float weights[3];
    float range[4];
};
static_assert(sizeof(KeyerGpuParams) == 8 * 4, "must match KeyerOp in keyer.slang");

const uint32_t kKeyerGpuLumaMax = 1u;
// Bits 1 and 2 hold the key channel plus one, 0 keying by the weights.
const uint32_t kKeyerGpuChannelShift = 1u;
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

    {
        KnobChoicePtr knob = AppManager::createKnob<KnobChoice>(knobHolder, EffectInstance::tr("Key Source").toStdString());
        knob->setName(kKeyerParamKeySource);
        knob->setHintToolTip(EffectInstance::tr("What the key value is computed from.").toStdString());
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("luminance", "Luminance", EffectInstance::tr("The luminance of RGB, by the Luminance Math formula.").toStdString()));
        options.push_back(ChoiceOption("red", "Red", EffectInstance::tr("The red channel.").toStdString()));
        options.push_back(ChoiceOption("green", "Green", EffectInstance::tr("The green channel.").toStdString()));
        options.push_back(ChoiceOption("blue", "Blue", EffectInstance::tr("The blue channel.").toStdString()));
        knob->populateChoices(options);
        knob->setDefaultValue((int)eKeySourceLuminance);
        knob->setAnimationEnabled(false);
        page->addKnob(knob);
        keySource = knob;
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
        KnobDoublePtr knob = createKeyerRangeKnob(knobHolder, page, kKeyerParamRange, EffectInstance::tr("Range").toStdString(),
                                                  EffectInstance::tr("The trapezoid from the key value to alpha.").toStdString() + "\n" + keyerRangeOffsetsHint(),
                                                  kRangeDefaults);
        // The key value is not a colour component, so the strip has no source colour, model,
        // enable or smooth knob.
        knob->setRangeWidget(KnobDouble::eRangeWidgetRoleLuma, KnobColorPtr(), KnobChoicePtr(), KnobBoolPtr());
        range = knob;
    }
}

KeyerOp::Params
KeyerOp::bake(const KnobReader& reader) const
{
    Params p;
    const int source = reader.value(keySource);
    if ((source >= (int)eKeySourceRed) && (source <= (int)eKeySourceBlue)) {
        p.keySource = (KeySourceEnum)source;
        p.channel = source - (int)eKeySourceRed;
        p.weights[p.channel] = 1.f;
    } else {
        p.keySource = eKeySourceLuminance;
        double w[3] = { 0., 0., 0. };
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
        for (int i = 0; i < 3; ++i) {
            p.weights[i] = (float)w[i];
        }
    }
    keyerRangePoints(reader, range, p.range);

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
    if (params.channel >= 0) {
        return rgb[params.channel];
    }
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
    if (params.channel >= 0) {
        p.flags |= (uint32_t)(params.channel + 1) << kKeyerGpuChannelShift;
    }
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
{
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
Keyer::initializeKnobs()
{
    ImageNode<KeyerOp>::initializeKnobs();
    refreshKeySourceDependents();
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
    KnobChoicePtr math = o.luminanceMath.lock();
    if (math) {
        math->setSecret(index != (int)KeyerOp::eKeySourceLuminance);
    }
    setSublabel(source->getEntry(index).label);
}

bool
Keyer::knobChanged(KnobI* k,
                   ValueChangedReasonEnum reason,
                   ViewSpec view,
                   double time,
                   bool originatedFromMainThread)
{
    KnobChoicePtr source = op().keySource.lock();

    if (source && (k == source.get())) {
        refreshKeySourceDependents();

        return true;
    }

    return ImageNode<KeyerOp>::knobChanged(k, reason, view, time, originatedFromMainThread);
}

void
Keyer::onKnobsLoaded()
{
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
