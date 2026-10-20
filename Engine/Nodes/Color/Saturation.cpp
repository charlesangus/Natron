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

#include "Saturation.h"

#include <algorithm>
#include <cfloat>
#include <string>

#include "Engine/AppManager.h"
#include "Engine/KnobTypes.h"
#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif
#include "Engine/Nodes/Image/ColorMath.h"

NATRON_NAMESPACE_ENTER

namespace {
#ifdef NATRON_HAVE_GPU
// The parameter block of SaturationOp in saturation.slang, which has no padding between members.
struct SaturationGpuParams {
    float saturation;
    uint32_t flags;
    float weights[3];
};

static_assert(sizeof(SaturationGpuParams) == 20, "must match SaturationOp in saturation.slang");

constexpr uint32_t kSaturationGpuClampBlack = 1;
constexpr uint32_t kSaturationGpuClampWhite = 2;
constexpr uint32_t kSaturationGpuLumaMax = 4;
#endif
} // anonymous namespace

void
SaturationOp::createKnobs(KnobHolder* holder,
                          const KnobPagePtr& page)
{
    KnobDoublePtr saturationKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Saturation").toStdString());
    saturationKnob->setName(kSaturationParamSaturation);
    saturationKnob->setHintToolTip(EffectInstance::tr("Color saturation factor to apply. 0 produces grayscale.").toStdString());
    saturationKnob->setDefaultValue(1.);
    saturationKnob->setMinimum(0.);
    saturationKnob->setMaximum(DBL_MAX);
    saturationKnob->setDisplayMinimum(0.);
    saturationKnob->setDisplayMaximum(4.);
    page->addKnob(saturationKnob);
    saturation = saturationKnob;

    luminanceMath = ColorMath::addLuminanceMathKnob(holder, page);

    KnobBoolPtr clampBlackKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr("Clamp Black").toStdString());
    clampBlackKnob->setName(kSaturationParamClampBlack);
    clampBlackKnob->setHintToolTip(EffectInstance::tr("All colors below 0 on output are set to 0.").toStdString());
    clampBlackKnob->setDefaultValue(false);
    clampBlackKnob->setAddNewLine(false);
    page->addKnob(clampBlackKnob);
    clampBlack = clampBlackKnob;

    KnobBoolPtr clampWhiteKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr("Clamp White").toStdString());
    clampWhiteKnob->setName(kSaturationParamClampWhite);
    clampWhiteKnob->setHintToolTip(EffectInstance::tr("All colors above 1 on output are set to 1.").toStdString());
    clampWhiteKnob->setDefaultValue(false);
    page->addKnob(clampWhiteKnob);
    clampWhite = clampWhiteKnob;
}

SaturationOp::Params
SaturationOp::bake(const KnobReader& reader) const
{
    int mathIndex = reader.value(luminanceMath);

    if ((mathIndex < (int)ColorMath::eLuminanceMathRec709) || (mathIndex > (int)ColorMath::eLuminanceMathMax)) {
        mathIndex = (int)ColorMath::eLuminanceMathRec709;
    }

    Params p;
    p.saturation = reader.value(saturation);
    p.luminanceMath = (ColorMath::LuminanceMathEnum)mathIndex;
    p.clampBlack = reader.value(clampBlack);
    p.clampWhite = reader.value(clampWhite);

    return p;
}

bool
SaturationOp::isIdentity(const Params& params) const
{
    if (params.clampBlack || params.clampWhite) {
        return false;
    }

    return params.saturation == 1.;
}

void
SaturationOp::row(const Params& params,
                  const RowIO& io) const
{
    const int nComps = io.nComps;
    bool process[4] = { false, false, false, false };

    for (int c = 0; (c < nComps) && (c < 4); ++c) {
        process[c] = io.channels[pixelKernelChannelBit(nComps, c)];
    }
    const float* src = io.src[0];
    float* dst = io.dst;
    for (int x = 0; x < io.width; ++x, src += nComps, dst += nComps) {
        const double l = ColorMath::luminance(params.luminanceMath, src[0], src[1], src[2]);
        for (int c = 0; c < nComps; ++c) {
            if (!process[c]) {
                continue;
            }
            double v = src[c];
            if (c < 3) {
                v = (1. - params.saturation) * l + params.saturation * v;
            }
            if (params.clampBlack) {
                v = (std::max)(0., v);
            }
            if (params.clampWhite) {
                v = (std::min)(1., v);
            }
            dst[c] = (float)v;
        }
    }
}

GpuOpRef
SaturationOp::gpu(const Params& params) const
{
#ifdef NATRON_HAVE_GPU
    // The weights are the luminance of the unit colours, taken from the function the CPU row
    // calls; Max is no weighted sum and is flagged instead.
    SaturationGpuParams p = {};
    p.saturation = (float)params.saturation;
    p.flags = (params.clampBlack ? kSaturationGpuClampBlack : 0) | (params.clampWhite ? kSaturationGpuClampWhite : 0) | ((params.luminanceMath == ColorMath::eLuminanceMathMax) ? kSaturationGpuLumaMax : 0);
    p.weights[0] = (float)ColorMath::luminance(params.luminanceMath, 1., 0., 0.);
    p.weights[1] = (float)ColorMath::luminance(params.luminanceMath, 0., 1., 0.);
    p.weights[2] = (float)ColorMath::luminance(params.luminanceMath, 0., 0., 1.);

    return GpuOpRef::compiled((int)gpu::GpuKernelId::saturation_wrapped, p);
#else
    Q_UNUSED(params);

    return GpuOpRef::none();
#endif
}

Saturation::Saturation(NodePtr node)
    : ImageNode<SaturationOp>(node)
{
}

Saturation::~Saturation()
{
}

SaturationOp::Params
Saturation::bakeSaturation(double time,
                           ViewIdx view) const
{
    return bakeParams(op(), time, view);
}

void
Saturation::saturationRow(const SaturationOp::Params& params,
                          const RowIO& io) const
{
    op().row(params, io);
}

void
Saturation::addAcceptedComponents(int inputNb,
                                  std::list<ImageLayerDesc>* comps)
{
    if (isInputMask(inputNb)) {
        StagedImageEffect::addAcceptedComponents(inputNb, comps);

        return;
    }
    comps->push_back(ImageLayerDesc::getRGBAComponents());
    comps->push_back(ImageLayerDesc::getRGBComponents());
}

NATRON_NAMESPACE_EXIT
