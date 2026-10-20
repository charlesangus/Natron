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

#include "Clamp.h"

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include "Engine/AppManager.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
KnobColorPtr
addClampColorKnob(KnobHolder* holder,
                  const KnobPagePtr& page,
                  const std::string& name,
                  const std::string& label,
                  const std::string& hint,
                  double defaultValue)
{
    KnobColorPtr knob = AppManager::createKnob<KnobColor>(holder, label, 4);

    knob->setName(name);
    knob->setHintToolTip(hint);
    for (int i = 0; i < 4; ++i) {
        knob->setDefaultValue(defaultValue, i);
        knob->setMinimum(-DBL_MAX, i);
        knob->setMaximum(DBL_MAX, i);
        knob->setDisplayMinimum(0., i);
        knob->setDisplayMaximum(1., i);
    }
    knob->setAddNewLine(false);
    page->addKnob(knob);

    return knob;
}

KnobBoolPtr
addClampEnableKnob(KnobHolder* holder,
                   const KnobPagePtr& page,
                   const std::string& name,
                   const std::string& label,
                   const std::string& hint,
                   bool defaultValue)
{
    KnobBoolPtr knob = AppManager::createKnob<KnobBool>(holder, label);

    knob->setName(name);
    knob->setHintToolTip(hint);
    knob->setDefaultValue(defaultValue);
    page->addKnob(knob);

    return knob;
}

#ifdef NATRON_HAVE_GPU
// The parameter block of ClampOp in clamp.slang, which has no padding between members.
struct ClampGpuParams {
    float minTest[4];
    float maxTest[4];
    float minValue[4];
    float maxValue[4];
    float minClampTo[4];
    float maxClampTo[4];
    uint32_t flags;
};

static_assert(sizeof(ClampGpuParams) == 100, "must match ClampOp in clamp.slang");

constexpr uint32_t kClampGpuMinimumEnable = 1;
constexpr uint32_t kClampGpuMaximumEnable = 2;
constexpr uint32_t kClampGpuMinClampToEnable = 4;
constexpr uint32_t kClampGpuMaxClampToEnable = 8;

// The smallest float not below d: a float v is below d exactly when it is below this.
float
ceilToFloat(double d)
{
    float f = (float)d;

    if ((double)f < d) {
        f = std::nextafter(f, std::numeric_limits<float>::infinity());
    }

    return f;
}

// The largest float not above d: a float v is above d exactly when it is above this.
float
floorToFloat(double d)
{
    float f = (float)d;

    if ((double)f > d) {
        f = std::nextafter(f, -std::numeric_limits<float>::infinity());
    }

    return f;
}
#endif
} // anonymous namespace

void
ClampOp::createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page)
{
    minimum = addClampColorKnob(holder, page, kClampParamMinimum, EffectInstance::tr("Minimum").toStdString(),
                                EffectInstance::tr("If enabled, all values that are lower than this number are set to this value, or to the minClampTo value if minClampTo is enabled.").toStdString(), 0.);
    minimumEnable = addClampEnableKnob(holder, page, kClampParamMinimumEnable, EffectInstance::tr("Enable Minimum").toStdString(),
                                       EffectInstance::tr("Whether to clamp selected channels to a minimum value.").toStdString(), true);
    maximum = addClampColorKnob(holder, page, kClampParamMaximum, EffectInstance::tr("Maximum").toStdString(),
                                EffectInstance::tr("If enabled, all values that are higher than this number are set to this value, or to the maxClampTo value if maxClampTo is enabled.").toStdString(), 1.);
    maximumEnable = addClampEnableKnob(holder, page, kClampParamMaximumEnable, EffectInstance::tr("Enable Maximum").toStdString(),
                                       EffectInstance::tr("Whether to clamp selected channels to a maximum value.").toStdString(), true);
    minClampTo = addClampColorKnob(holder, page, kClampParamMinClampTo, EffectInstance::tr("MinClampTo").toStdString(),
                                   EffectInstance::tr("The value to which values below minimum are clamped when minClampTo is enabled. Setting this to a custom color helps visualizing the clamped areas or create graphic effects.").toStdString(), 0.);
    minClampToEnable = addClampEnableKnob(holder, page, kClampParamMinClampToEnable, EffectInstance::tr("Enable MinClampTo").toStdString(),
                                          EffectInstance::tr("When enabled, all values below minimum are set to the minClampTo value.\nWhen disabled, all values below minimum are clamped to the minimum value.").toStdString(), false);
    maxClampTo = addClampColorKnob(holder, page, kClampParamMaxClampTo, EffectInstance::tr("MaxClampTo").toStdString(),
                                   EffectInstance::tr("The value to which values above maximum are clamped when maxClampTo is enabled. Setting this to a custom color helps visualizing the clamped areas or create graphic effects.").toStdString(), 1.);
    maxClampToEnable = addClampEnableKnob(holder, page, kClampParamMaxClampToEnable, EffectInstance::tr("Enable MaxClampTo").toStdString(),
                                          EffectInstance::tr("When enabled, all values above maximum are set to the maxClampTo value.\nWhen disabled, all values above maximum are clamped to the maximum value.").toStdString(), false);
}

ClampOp::Params
ClampOp::bake(const KnobReader& reader) const
{
    Params p;

    for (int i = 0; i < 4; ++i) {
        p.minimum[i] = reader.value(minimum, i);
        p.maximum[i] = reader.value(maximum, i);
        p.minClampTo[i] = reader.value(minClampTo, i);
        p.maxClampTo[i] = reader.value(maxClampTo, i);
    }
    p.minimumEnable = reader.value(minimumEnable);
    p.maximumEnable = reader.value(maximumEnable);
    p.minClampToEnable = reader.value(minClampToEnable);
    p.maxClampToEnable = reader.value(maxClampToEnable);

    return p;
}

bool
ClampOp::isIdentity(const Params& params) const
{
    return !params.minimumEnable && !params.maximumEnable;
}

void
ClampOp::row(const Params& params,
             const RowIO& io) const
{
    const int nComps = io.nComps;
    int bits[4] = { -1, -1, -1, -1 };

    for (int c = 0; (c < nComps) && (c < 4); ++c) {
        const int bit = pixelKernelChannelBit(nComps, c);
        bits[c] = io.channels[bit] ? bit : -1;
    }
    const float* src = io.src[0];
    float* dst = io.dst;
    for (int x = 0; x < io.width; ++x, src += nComps, dst += nComps) {
        for (int c = 0; (c < nComps) && (c < 4); ++c) {
            if (bits[c] < 0) {
                continue;
            }
            const int i = bits[c];
            const double v = src[c];
            double out = v;
            if (params.minimumEnable && (v < params.minimum[i])) {
                out = params.minClampToEnable ? params.minClampTo[i] : params.minimum[i];
            } else if (params.maximumEnable && (v > params.maximum[i])) {
                out = params.maxClampToEnable ? params.maxClampTo[i] : params.maximum[i];
            }
            dst[c] = (float)out;
        }
    }
}

GpuOpRef
ClampOp::gpu(const Params& params) const
{
#ifdef NATRON_HAVE_GPU
    ClampGpuParams p = {};
    for (int i = 0; i < 4; ++i) {
        p.minTest[i] = ceilToFloat(params.minimum[i]);
        p.maxTest[i] = floorToFloat(params.maximum[i]);
        p.minValue[i] = (float)params.minimum[i];
        p.maxValue[i] = (float)params.maximum[i];
        p.minClampTo[i] = (float)params.minClampTo[i];
        p.maxClampTo[i] = (float)params.maxClampTo[i];
    }
    p.flags = (params.minimumEnable ? kClampGpuMinimumEnable : 0) | (params.maximumEnable ? kClampGpuMaximumEnable : 0) | (params.minClampToEnable ? kClampGpuMinClampToEnable : 0) | (params.maxClampToEnable ? kClampGpuMaxClampToEnable : 0);

    return GpuOpRef::compiled((int)gpu::GpuKernelId::clamp_wrapped, p);
#else
    Q_UNUSED(params);

    return GpuOpRef::none();
#endif
}

Clamp::Clamp(NodePtr node)
    : ImageNode<ClampOp>(node)
{
}

Clamp::~Clamp()
{
}

ClampOp::Params
Clamp::bakeClamp(double time,
                 ViewIdx view) const
{
    return bakeParams(op(), time, view);
}

void
Clamp::clampRow(const ClampOp::Params& params,
                const RowIO& io) const
{
    op().row(params, io);
}

void
Clamp::addAcceptedComponents(int inputNb,
                             std::list<ImageLayerDesc>* comps)
{
    StagedImageEffect::addAcceptedComponents(inputNb, comps);
    if (!isInputMask(inputNb)) {
        comps->push_back(ImageLayerDesc::getXYComponents());
    }
}

NATRON_NAMESPACE_EXIT
