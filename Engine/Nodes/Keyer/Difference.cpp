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

#include "Difference.h"

#include <algorithm>
#include <cfloat>

#include "Engine/AppManager.h"
#include "Engine/KnobTypes.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
#ifdef NATRON_HAVE_GPU
// The parameter block of StepDifference in routing.slang.
struct DifferenceGpuParams {
    float gain;
    float offset;
};
static_assert(sizeof(DifferenceGpuParams) == 8, "must match StepDifference in routing.slang");
#endif
} // anonymous namespace

void
DifferenceOp::createKnobs(KnobHolder* holder,
                          const KnobPagePtr& page)
{
    {
        KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Offset").toStdString());
        knob->setName(kDifferenceParamOffset);
        knob->setHintToolTip(EffectInstance::tr("Subtracted from the gained difference before it is clamped at 0.").toStdString());
        knob->setDefaultValue(0.);
        knob->setMinimum(-DBL_MAX);
        knob->setMaximum(DBL_MAX);
        knob->setDisplayMinimum(0.);
        knob->setDisplayMaximum(1.);
        knob->setIncrement(0.005);
        page->addKnob(knob);
        offset = knob;
    }
    {
        KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Gain").toStdString());
        knob->setName(kDifferenceParamGain);
        knob->setHintToolTip(EffectInstance::tr("Multiplies the squared difference.").toStdString());
        knob->setDefaultValue(1.);
        knob->setMinimum(-DBL_MAX);
        knob->setMaximum(DBL_MAX);
        knob->setDisplayMinimum(0.);
        knob->setDisplayMaximum(1.);
        knob->setIncrement(0.005);
        page->addKnob(knob);
        gain = knob;
    }
}

DifferenceOp::Params
DifferenceOp::bake(const KnobReader& reader) const
{
    Params params;
    params.offset = (float)reader.valueOr(offset, 0.);
    params.gain = (float)reader.valueOr(gain, 1.);

    return params;
}

bool
DifferenceOp::isIdentity(const Params& /*params*/) const
{
    return false;
}

Route
DifferenceOp::route(const Params& /*params*/,
                    const RouteContext& context) const
{
    return context.isConnected(kDifferenceInputB) ? Route::none() : Route::black();
}

CompositeInputs
DifferenceOp::inputs(const Params& /*params*/,
                     const RouteContext& context) const
{
    CompositeInputs folded;
    folded.base = CompositeInput { kDifferenceInputB };
    // Without a clean plate B is compared with itself: a zero difference.
    folded.add(context.isConnected(kDifferenceInputA) ? kDifferenceInputA : kDifferenceInputB);

    return folded;
}

void
DifferenceOp::step(const Params& params,
                   const RowIO& io) const
{
    const int nComps = io.nComps;
    bool processed[4] = { false, false, false, false };
    bool isColor[4] = { false, false, false, false };

    for (int c = 0; (c < nComps) && (c < 4); ++c) {
        const int bit = pixelKernelChannelBit(nComps, c);
        processed[c] = io.channels[bit];
        isColor[c] = (bit < 3);
    }
    const float* b = io.src[0];
    const float* a = io.src[1];
    float* dst = io.dst;
    for (int x = 0; x < io.width; ++x, b += nComps, a += nComps, dst += nComps) {
        float sum = 0.f;
        for (int c = 0; (c < nComps) && (c < 4); ++c) {
            if (isColor[c]) {
                const float d = b[c] - a[c];
                sum += d * d;
            }
        }
        const float matte = std::max(0.f, params.gain * sum - params.offset);
        for (int c = 0; (c < nComps) && (c < 4); ++c) {
            if (processed[c]) {
                dst[c] = isColor[c] ? b[c] : matte;
            }
        }
    }
}

GpuOpRef
DifferenceOp::gpu(const Params& params) const
{
#ifdef NATRON_HAVE_GPU
    DifferenceGpuParams block = {};
    block.gain = params.gain;
    block.offset = params.offset;

    return GpuOpRef::compiled((int)gpu::GpuKernelId::difference, block);
#else
    Q_UNUSED(params);

    return GpuOpRef::none();
#endif
}

Difference::Difference(NodePtr node)
    : ImageNode<DifferenceOp>(node)
{
}

Difference::~Difference()
{
}

NATRON_NAMESPACE_EXIT
