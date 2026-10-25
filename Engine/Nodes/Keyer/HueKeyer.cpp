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

#include "HueKeyer.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <string>

#include "Engine/AppManager.h"
#include "Engine/KnobTypes.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Color/HueCurves.h"
#include "Engine/Nodes/Image/ColorMath.h"
#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {

#ifdef NATRON_HAVE_GPU
// The parameter block of HueKeyerOp in hue.slang, which has no padding between members.
struct HueKeyerGpuParams {
    uint32_t flags;
    float hueLow;
    float hueRise;
    float hueFull;
    float hueFall;
    float satLow;
    float satLowFull;
    float satHighFull;
    float satHigh;
};

static_assert(sizeof(HueKeyerGpuParams) == 9 * 4, "must match HueKeyerOp in hue.slang");

const uint32_t kGpuFlagInvert = 1u;
#endif

KnobDoublePtr
addDouble(KnobHolder* holder,
          const KnobPagePtr& page,
          const char* name,
          const char* label,
          const char* hint,
          double defaultValue,
          double minimum,
          double maximum,
          bool newLine)
{
    KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(holder, std::string(label));

    knob->setName(name);
    knob->setHintToolTip(std::string(hint));
    knob->setMinimum(minimum);
    knob->setMaximum(maximum);
    knob->setDisplayMinimum(0.);
    knob->setDisplayMaximum(1.);
    knob->setDefaultValue(defaultValue);
    knob->setAddNewLine(newLine);
    page->addKnob(knob);

    return knob;
}
} // anonymous namespace

void
HueKeyerOp::createKnobs(KnobHolder* holder,
                        const KnobPagePtr& page)
{
    hueLow = addDouble(holder, page, kHueKeyerParamHueLow, "Hue Low",
                       "The hue, in turns from red at 0, where the key starts to rise from 0. Hues wrap, so the four hue points may cross red.",
                       0.25, -DBL_MAX, DBL_MAX, true);
    hueLowFull = addDouble(holder, page, kHueKeyerParamHueLowFull, "Hue Low Full",
                           "The hue where the key reaches 1, going up the hue circle from Hue Low.",
                           0.30, -DBL_MAX, DBL_MAX, true);
    hueHighFull = addDouble(holder, page, kHueKeyerParamHueHighFull, "Hue High Full",
                            "The hue where the key starts to fall from 1, going up from Hue Low Full.",
                            0.37, -DBL_MAX, DBL_MAX, true);
    hueHigh = addDouble(holder, page, kHueKeyerParamHueHigh, "Hue High",
                        "The hue where the key reaches 0, going up from Hue High Full.",
                        0.42, -DBL_MAX, DBL_MAX, true);
    satLow = addDouble(holder, page, kHueKeyerParamSatLow, "Saturation Low",
                       "The saturation where the key starts to rise from 0. Saturation is chroma divided by the largest of R, G and B; grey is 0 and is keyed only when this is at most 0.",
                       0.1, -DBL_MAX, DBL_MAX, true);
    satLowFull = addDouble(holder, page, kHueKeyerParamSatLowFull, "Saturation Low Full",
                           "The saturation where the key reaches 1.",
                           0.3, -DBL_MAX, DBL_MAX, true);
    satHighFull = addDouble(holder, page, kHueKeyerParamSatHighFull, "Saturation High Full",
                            "The saturation where the key starts to fall from 1.",
                            1., -DBL_MAX, DBL_MAX, true);
    satHigh = addDouble(holder, page, kHueKeyerParamSatHigh, "Saturation High",
                        "The saturation where the key reaches 0. Saturation above 1 comes from negative channels.",
                        1., -DBL_MAX, DBL_MAX, true);

    KnobBoolPtr inv = AppManager::createKnob<KnobBool>(holder, std::string("Invert"));
    inv->setName(kHueKeyerParamInvert);
    inv->setHintToolTip(std::string("Output one minus the key."));
    inv->setDefaultValue(false);
    page->addKnob(inv);
    invert = inv;
}

HueKeyerOp::Params
HueKeyerOp::bake(const KnobReader& reader) const
{
    Params p;
    const double low = reader.value(hueLow);
    const double lowFull = reader.value(hueLowFull);
    const double highFull = reader.value(hueHighFull);
    const double high = reader.value(hueHigh);
    // The distances going up the hue circle, each less than a turn.
    const double rise = lowFull - low - std::floor(lowFull - low);
    const double full = highFull - lowFull - std::floor(highFull - lowFull);
    const double fall = high - highFull - std::floor(high - highFull);

    p.hueLow = (float)(low - std::floor(low));
    p.hueRise = (float)rise;
    p.hueFull = (float)(rise + full);
    p.hueFall = (float)(rise + full + fall);

    // Out-of-order saturation points are pushed up to the one before.
    double s[4] = { reader.value(satLow), reader.value(satLowFull), reader.value(satHighFull), reader.value(satHigh) };
    for (int i = 1; i < 4; ++i) {
        s[i] = (std::max)(s[i], s[i - 1]);
    }
    p.satLow = (float)s[0];
    p.satLowFull = (float)s[1];
    p.satHighFull = (float)s[2];
    p.satHigh = (float)s[3];
    p.invert = reader.value(invert);

    return p;
}

bool
HueKeyerOp::isIdentity(const Params& params) const
{
    // The key always replaces alpha.
    Q_UNUSED(params);

    return false;
}

float
HueKeyerOp::key(const Params& params,
                const float* rgb)
{
    const float maxv = (std::max)(rgb[0], (std::max)(rgb[1], rgb[2]));
    const float minv = (std::min)(rgb[0], (std::min)(rgb[1], rgb[2]));
    const float delta = maxv - minv;
    const float h = ColorMath::colorspaceHexHue(rgb[0], rgb[1], rgb[2], maxv, delta);
    const float s = (maxv > 0.f) ? (delta / maxv) : 0.f;
    const float hk = HueCurves::hueTrapezoid(h, params.hueLow, params.hueRise, params.hueFull, params.hueFall);
    const float sk = HueCurves::trapezoid(s, params.satLow, params.satLowFull, params.satHighFull, params.satHigh);
    const float k = hk * sk;

    return params.invert ? (1.f - k) : k;
}

void
HueKeyerOp::row(const Params& params,
                const RowIO& io) const
{
    const int nComps = io.nComps;
    // getPreferredMetadata() makes the output RGBA; with no alpha there is nothing to write.
    if ((nComps != 4) || !io.channels[pixelKernelChannelBit(nComps, 3)]) {
        std::copy(io.src[0], io.src[0] + io.width * nComps, io.dst);

        return;
    }
    const float* src = io.src[0];
    float* dst = io.dst;
    for (int x = 0; x < io.width; ++x, src += 4, dst += 4) {
        dst[3] = key(params, src);
    }
}

GpuOpRef
HueKeyerOp::gpu(const Params& params) const
{
#ifdef NATRON_HAVE_GPU
    HueKeyerGpuParams p = {};
    p.flags = params.invert ? kGpuFlagInvert : 0u;
    p.hueLow = params.hueLow;
    p.hueRise = params.hueRise;
    p.hueFull = params.hueFull;
    p.hueFall = params.hueFall;
    p.satLow = params.satLow;
    p.satLowFull = params.satLowFull;
    p.satHighFull = params.satHighFull;
    p.satHigh = params.satHigh;
    GpuOpRef ref = GpuOpRef::compiled((int)gpu::GpuKernelId::huekeyer_wrapped, p);
    ref.acceptedComponents = GpuOpRef::componentsBit(4);

    return ref;
#else
    Q_UNUSED(params);

    return GpuOpRef::none();
#endif
}

HueKeyer::HueKeyer(NodePtr node)
    : ImageNode<HueKeyerOp>(node)
{
}

HueKeyer::~HueKeyer()
{
}

HueKeyerOp::Params
HueKeyer::bakeHueKeyer(double time,
                       ViewIdx view) const
{
    return bakeParams(op(), time, view);
}

void
HueKeyer::hueKeyerRow(const HueKeyerOp::Params& params,
                      const RowIO& io) const
{
    op().row(params, io);
}

void
HueKeyer::addAcceptedComponents(int inputNb,
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
HueKeyer::getPreferredMetadata(NodeMetadata& metadata)
{
    metadata.setNComps(-1, 4);

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
