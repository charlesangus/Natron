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

#include "ColorMathNode.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <memory>
#include <string>

#include "Engine/AppManager.h"
#include "Engine/KnobTypes.h"
#include "Engine/RenderScale.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
constexpr double
neutralValue(ColorMathOperationEnum operation)
{
    return (operation == eColorMathOperationAdd) ? 0. : 1.;
}

double
gammaExponent(double value,
              bool invert)
{
    return invert ? value : (1. / (std::max)(1e-8, value));
}

#ifdef NATRON_HAVE_GPU
struct ColorMathGpuParams {
    float value[4];
};

static_assert(sizeof(ColorMathGpuParams) == 16, "must match the parameter block of the wrapped color math kernels");

constexpr gpu::GpuKernelId
colorMathKernelId(ColorMathOperationEnum operation)
{
    switch (operation) {
    case eColorMathOperationAdd:

        return gpu::GpuKernelId::add_wrapped;
    case eColorMathOperationMultiply:

        return gpu::GpuKernelId::multiply_wrapped;
    case eColorMathOperationGamma:
        break;
    }

    return gpu::GpuKernelId::gamma_wrapped;
}
#endif
} // anonymous namespace

template <ColorMathOperationEnum KIND>
void
ColorMathOp<KIND>::createKnobs(KnobHolder* holder,
                               const KnobPagePtr& page)
{
    KnobColorPtr valueKnob = AppManager::createKnob<KnobColor>(holder, EffectInstance::tr("Value").toStdString(), 4);

    valueKnob->setName(kColorMathParamValue);
    switch (KIND) {
    case eColorMathOperationAdd:
        valueKnob->setHintToolTip(EffectInstance::tr("Constant to add to the selected channels.").toStdString());
        break;
    case eColorMathOperationMultiply:
        valueKnob->setHintToolTip(EffectInstance::tr("Constant to multiply with the selected channels.").toStdString());
        break;
    case eColorMathOperationGamma:
        valueKnob->setHintToolTip(EffectInstance::tr("Gamma value to apply to the selected channels.").toStdString());
        break;
    }
    const double minimum = (KIND == eColorMathOperationGamma) ? 0. : -DBL_MAX;
    for (int i = 0; i < 4; ++i) {
        valueKnob->setDefaultValue(neutralValue(KIND), i);
        valueKnob->setMinimum(minimum, i);
        valueKnob->setMaximum(DBL_MAX, i);
        valueKnob->setDisplayMinimum(0., i);
        valueKnob->setDisplayMaximum(4., i);
    }
    page->addKnob(valueKnob);
    value = valueKnob;

    if constexpr (KIND == eColorMathOperationGamma) {
        KnobBoolPtr invertKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr("Invert").toStdString());
        invertKnob->setName(kColorMathParamInvert);
        invertKnob->setHintToolTip(EffectInstance::tr("Invert the gamma transform.").toStdString());
        invertKnob->setDefaultValue(false);
        invertKnob->setAnimationEnabled(false);
        page->addKnob(invertKnob);
        invert = invertKnob;
    }
}

template <ColorMathOperationEnum KIND>
typename ColorMathOp<KIND>::Params
ColorMathOp<KIND>::bake(const KnobReader& reader) const
{
    Params p;
    bool invertValue = false;

    if constexpr (KIND == eColorMathOperationGamma) {
        invertValue = reader.value(invert);
    }
    p.isNeutral = true;
    for (int i = 0; i < 4; ++i) {
        const double v = reader.value(value, i);
        if (v != neutralValue(KIND)) {
            p.isNeutral = false;
        }
        if constexpr (KIND == eColorMathOperationGamma) {
            const double exponent = gammaExponent(v, invertValue);
            p.value[i] = (float)exponent;
            p.isUnit[i] = (exponent == 1.);
        } else {
            p.value[i] = (float)v;
            p.isUnit[i] = false;
        }
    }

    return p;
}

template <ColorMathOperationEnum KIND>
bool
ColorMathOp<KIND>::isIdentity(const Params& params) const
{
    return params.isNeutral;
}

template <ColorMathOperationEnum KIND>
void
ColorMathOp<KIND>::row(const Params& params,
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
            const int bit = bits[c];
            if (bit < 0) {
                continue;
            }
            if constexpr (KIND == eColorMathOperationAdd) {
                dst[c] = src[c] + params.value[bit];
            } else if constexpr (KIND == eColorMathOperationMultiply) {
                dst[c] = src[c] * params.value[bit];
            } else {
                const float v = src[c];
                // pow of a negative base is NaN; such values pass through, as in Nuke.
                dst[c] = ((v <= 0.f) || params.isUnit[bit]) ? v : std::pow(v, params.value[bit]);
            }
        }
    }
}

template <ColorMathOperationEnum KIND>
GpuOpRef
ColorMathOp<KIND>::gpu(const Params& params) const
{
#ifdef NATRON_HAVE_GPU
    ColorMathGpuParams p;

    for (int i = 0; i < 4; ++i) {
        p.value[i] = params.value[i];
    }

    return GpuOpRef::compiled((int)colorMathKernelId(KIND), p);
#else
    Q_UNUSED(params);

    return GpuOpRef::none();
#endif
}

template struct ColorMathOp<eColorMathOperationAdd>;
template struct ColorMathOp<eColorMathOperationMultiply>;
template struct ColorMathOp<eColorMathOperationGamma>;

NATRON_NAMESPACE_EXIT
