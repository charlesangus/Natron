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

#include "KeyMix.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Format.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
#ifdef NATRON_HAVE_GPU
// The parameter block of StepKeyMix in routing.slang: one reserved word, as it reads none.
struct KeyMixGpuParams {
    std::uint32_t reserved;
};
static_assert(sizeof(KeyMixGpuParams) == 4, "must match StepKeyMix in routing.slang");
#endif
} // anonymous namespace

void
KeyMixOp::createKnobs(KnobHolder* holder,
                      const KnobPagePtr& page)
{
    KnobChoicePtr setBBoxKnob = AppManager::createKnob<KnobChoice>(holder, EffectInstance::tr("Set BBox").toStdString());
    setBBoxKnob->setName(kKeyMixParamSetBBox);
    setBBoxKnob->setHintToolTip(EffectInstance::tr("The output's region of definition.").toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kKeyMixBBoxUnion, EffectInstance::tr("union").toStdString(), EffectInstance::tr("The union of A and B.").toStdString()));
        options.push_back(ChoiceOption(kKeyMixBBoxB, "B", EffectInstance::tr("Input B's.").toStdString()));
        options.push_back(ChoiceOption(kKeyMixBBoxA, "A", EffectInstance::tr("Input A's.").toStdString()));
        setBBoxKnob->populateChoices(options);
    }
    setBBoxKnob->setDefaultValue((int)eBBoxUnion);
    page->addKnob(setBBoxKnob);
    setBBox = setBBoxKnob;
}

KeyMixOp::Params
KeyMixOp::bake(const KnobReader& reader) const
{
    Params params;
    params.bbox = reader.valueOr(setBBox, (int)eBBoxUnion);

    return params;
}

bool
KeyMixOp::isIdentity(const Params& /*params*/) const
{
    // Whether A is connected is not in the Params: route() answers that identity. No channel
    // selected, a mix of 0 and a mask missing the window are the host's.
    return false;
}

Route
KeyMixOp::route(const Params& /*params*/,
                const RouteContext& context) const
{
    if (context.isConnected(kKeyMixInputA)) {
        return Route::none();
    }

    return context.isConnected(kKeyMixInputB) ? Route::input(kKeyMixInputB, context.time()) : Route::black();
}

CompositeInputs
KeyMixOp::inputs(const Params& /*params*/,
                 const RouteContext& /*context*/) const
{
    CompositeInputs folded;
    folded.base = CompositeInput { kKeyMixInputB };
    folded.add(kKeyMixInputA);

    return folded;
}

void
KeyMixOp::step(const Params& /*params*/,
               const RowIO& io) const
{
    const int nComps = io.nComps;
    bool processed[4] = { false, false, false, false };
    for (int c = 0; (c < nComps) && (c < 4); ++c) {
        processed[c] = io.channels[pixelKernelChannelBit(nComps, c)];
    }
    const float* a = io.src[1];
    float* dst = io.dst;
    for (int x = 0; x < io.width; ++x, a += nComps, dst += nComps) {
        for (int c = 0; (c < nComps) && (c < 4); ++c) {
            if (processed[c]) {
                dst[c] = a[c];
            }
        }
    }
}

GpuOpRef
KeyMixOp::gpu(const Params& /*params*/) const
{
#ifdef NATRON_HAVE_GPU
    KeyMixGpuParams block = {};

    return GpuOpRef::compiled((int)gpu::GpuKernelId::keymix, block);
#else

    return GpuOpRef::none();
#endif
}

KeyMix::KeyMix(NodePtr node)
    : ImageNode<KeyMixOp>(node)
{
}

KeyMix::~KeyMix()
{
}

bool
KeyMix::getInputRoD(int inputNb,
                    double time,
                    const RenderScale& scale,
                    ViewIdx view,
                    RectD* rod) const
{
    EffectInstancePtr input = getInput(inputNb);

    if (!input) {
        return false;
    }
    const RenderScale inputScale = input->supportsRenderScale() ? scale : RenderScale::identity;
    bool isProjectFormat = false;
    if (input->getRegionOfDefinition_public(input->getRenderHash(), time, inputScale, view, rod, &isProjectFormat) == eStatusFailed) {
        *rod = RectD();
    }

    return true;
}

StatusEnum
KeyMix::getRegionOfDefinition(U64 /*hash*/,
                              double time,
                              const RenderScale& scale,
                              ViewIdx view,
                              RectD* rod)
{
    bool found = false;
    const Route answer = route(time, view);

    if ((answer.kind == Route::Kind::Input) || (getMixValue(time, view) == 0.)) {
        found = getInputRoD(kKeyMixInputB, time, scale, view, rod);
    } else {
        KnobChoicePtr setBBoxKnob = op().setBBox.lock();
        const int bbox = setBBoxKnob ? setBBoxKnob->getValueAtTime(time, 0, view) : (int)KeyMixOp::eBBoxUnion;
        RectD inputRoD;
        if (bbox != KeyMixOp::eBBoxA) {
            found = getInputRoD(kKeyMixInputB, time, scale, view, rod);
        }
        if ((bbox != KeyMixOp::eBBoxB) && getInputRoD(kKeyMixInputA, time, scale, view, &inputRoD)) {
            if (!found) {
                *rod = inputRoD;
            } else if (rod->isNull()) {
                *rod = inputRoD;
            } else if (!inputRoD.isNull()) {
                rod->merge(inputRoD);
            }
            found = true;
        }
    }

    if (!found) {
        Format format;
        getApp()->getProject()->getProjectDefaultFormat(&format);
        const RectD canonical = format.toCanonical_noClipping(0, format.getPixelAspectRatio());
        *rod = RectD(0., 0., canonical.right(), canonical.top());
    }

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
