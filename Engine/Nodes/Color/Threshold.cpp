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

#include "Threshold.h"

#include <cfloat>
#include <string>

#include "Engine/AppManager.h"
#include "Engine/KnobTypes.h"
#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {

#ifdef NATRON_HAVE_GPU
// The parameter block of ThresholdOp in colourutil.slang, which has no padding between members.
struct ThresholdGpuParams {
    float low[4];
    float high[4];
};

static_assert(sizeof(ThresholdGpuParams) == 32, "must match ThresholdOp in colourutil.slang");
#endif

KnobColorPtr
addThresholdKnob(KnobHolder* holder,
                 const KnobPagePtr& page,
                 const char* name,
                 const std::string& label,
                 const std::string& hint,
                 double minimum,
                 double displayMinimum)
{
    KnobColorPtr knob = AppManager::createKnob<KnobColor>(holder, label, 4);

    knob->setName(name);
    knob->setHintToolTip(hint);
    for (int i = 0; i < 4; ++i) {
        knob->setDefaultValue(0., i);
        knob->setMinimum(minimum, i);
        knob->setMaximum(DBL_MAX, i);
        knob->setDisplayMinimum(displayMinimum, i);
        knob->setDisplayMaximum(1., i);
    }
    page->addKnob(knob);

    return knob;
}

} // anonymous namespace

void
ThresholdOp::createKnobs(KnobHolder* holder,
                         const KnobPagePtr& page)
{
    level = addThresholdKnob(holder, page, kThresholdParamLevel, EffectInstance::tr("Threshold Level").toStdString(), EffectInstance::tr("Threshold level for the selected channels.").toStdString(), -DBL_MAX, -1.);
    softness = addThresholdKnob(holder, page, kThresholdParamSoftness, EffectInstance::tr("Threshold Softness").toStdString(), EffectInstance::tr("Threshold softness for the selected channels.").toStdString(), 0., 0.);
}

ThresholdOp::Params
ThresholdOp::bake(const KnobReader& reader) const
{
    Params p;

    for (int i = 0; i < 4; ++i) {
        const double l = reader.value(level, i);
        const double s = reader.value(softness, i);
        p.low[i] = (float)(l - s);
        p.high[i] = (float)(l + s);
    }

    return p;
}

bool
ThresholdOp::isIdentity(const Params& /*params*/) const
{
    // Every level and softness changes some value.
    return false;
}

void
ThresholdOp::row(const Params& params,
                 const RowIO& io) const
{
    const int nComps = io.nComps;
    int bit[4] = { 0, 1, 2, 3 };
    bool process[4] = { false, false, false, false };
    for (int c = 0; (c < nComps) && (c < 4); ++c) {
        bit[c] = pixelKernelChannelBit(nComps, c);
        process[c] = io.channels[bit[c]];
    }
    const float* src = io.src[0];
    float* dst = io.dst;
    for (int x = 0; x < io.width; ++x, src += nComps, dst += nComps) {
        for (int c = 0; (c < nComps) && (c < 4); ++c) {
            if (!process[c]) {
                continue;
            }
            const float v = src[c];
            const float low = params.low[bit[c]];
            const float high = params.high[bit[c]];
            // The top end first, so that at softness 0 a value equal to the level gives 1.
            if (v >= high) {
                dst[c] = 1.f;
            } else if (v <= low) {
                dst[c] = 0.f;
            } else {
                const float t = v - low;
                const float w = high - low;
                dst[c] = t / w;
            }
        }
    }
}

GpuOpRef
ThresholdOp::gpu(const Params& params) const
{
#ifdef NATRON_HAVE_GPU
    ThresholdGpuParams p = {};
    for (int i = 0; i < 4; ++i) {
        p.low[i] = params.low[i];
        p.high[i] = params.high[i];
    }

    return GpuOpRef::compiled((int)gpu::GpuKernelId::threshold_wrapped, p);
#else
    Q_UNUSED(params);

    return GpuOpRef::none();
#endif
}

Threshold::Threshold(NodePtr node)
    : ImageNode<ThresholdOp>(node)
{
}

Threshold::~Threshold()
{
}

ThresholdOp::Params
Threshold::bakeThreshold(double time,
                         ViewIdx view) const
{
    return bakeParams(op(), time, view);
}

void
Threshold::thresholdRow(const ThresholdOp::Params& params,
                        const RowIO& io) const
{
    op().row(params, io);
}

NATRON_NAMESPACE_EXIT
