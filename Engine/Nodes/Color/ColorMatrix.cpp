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

#include "ColorMatrix.h"

#include <cfloat>
#include <cstdint>
#include <string>

#include "Engine/AppManager.h"
#include "Engine/KnobTypes.h"
#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {

#ifdef NATRON_HAVE_GPU
// The parameter block of ColorMatrixOp in colourutil.slang, which has no padding between members.
struct ColorMatrixGpuParams {
    float m[4][4];
    uint32_t flags;
};

static_assert(sizeof(ColorMatrixGpuParams) == 68, "must match ColorMatrixOp in colourutil.slang");

constexpr uint32_t kColorMatrixGpuClampBlack = 1;
constexpr uint32_t kColorMatrixGpuClampWhite = 2;
#endif

KnobColorPtr
addOutputKnob(KnobHolder* holder,
              const KnobPagePtr& page,
              const char* name,
              const std::string& label,
              const std::string& hint,
              int row)
{
    KnobColorPtr knob = AppManager::createKnob<KnobColor>(holder, label, 4);

    knob->setName(name);
    knob->setHintToolTip(hint);
    for (int i = 0; i < 4; ++i) {
        knob->setDefaultValue((i == row) ? 1. : 0., i);
        knob->setMinimum(-DBL_MAX, i);
        knob->setMaximum(DBL_MAX, i);
        knob->setDisplayMinimum(0., i);
        knob->setDisplayMaximum(1., i);
    }
    page->addKnob(knob);

    return knob;
}

} // anonymous namespace

void
ColorMatrixOp::createKnobs(KnobHolder* holder,
                           const KnobPagePtr& page)
{
    outputs[0] = addOutputKnob(holder, page, kColorMatrixParamOutputRed, EffectInstance::tr("Output Red").toStdString(), EffectInstance::tr("The weights of the input R, G, B and A in the red output.").toStdString(), 0);
    outputs[1] = addOutputKnob(holder, page, kColorMatrixParamOutputGreen, EffectInstance::tr("Output Green").toStdString(), EffectInstance::tr("The weights of the input R, G, B and A in the green output.").toStdString(), 1);
    outputs[2] = addOutputKnob(holder, page, kColorMatrixParamOutputBlue, EffectInstance::tr("Output Blue").toStdString(), EffectInstance::tr("The weights of the input R, G, B and A in the blue output.").toStdString(), 2);
    outputs[3] = addOutputKnob(holder, page, kColorMatrixParamOutputAlpha, EffectInstance::tr("Output Alpha").toStdString(), EffectInstance::tr("The weights of the input R, G, B and A in the alpha output.").toStdString(), 3);

    KnobBoolPtr clampBlackKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr("Clamp Black").toStdString());
    clampBlackKnob->setName(kColorMatrixParamClampBlack);
    clampBlackKnob->setHintToolTip(EffectInstance::tr("All colors below 0 on output are set to 0.").toStdString());
    clampBlackKnob->setDefaultValue(false);
    clampBlackKnob->setAddNewLine(false);
    page->addKnob(clampBlackKnob);
    clampBlack = clampBlackKnob;

    KnobBoolPtr clampWhiteKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr("Clamp White").toStdString());
    clampWhiteKnob->setName(kColorMatrixParamClampWhite);
    clampWhiteKnob->setHintToolTip(EffectInstance::tr("All colors above 1 on output are set to 1.").toStdString());
    clampWhiteKnob->setDefaultValue(false);
    page->addKnob(clampWhiteKnob);
    clampWhite = clampWhiteKnob;
}

ColorMatrixOp::Params
ColorMatrixOp::bake(const KnobReader& reader) const
{
    Params p;

    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            p.m[r][c] = (float)reader.value(outputs[r], c);
        }
    }
    p.clampBlack = reader.value(clampBlack);
    p.clampWhite = reader.value(clampWhite);

    return p;
}

bool
ColorMatrixOp::isIdentity(const Params& params) const
{
    if (params.clampBlack || params.clampWhite) {
        return false;
    }
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            if (params.m[r][c] != ((r == c) ? 1.f : 0.f)) {
                return false;
            }
        }
    }

    return true;
}

void
ColorMatrixOp::row(const Params& params,
                   const RowIO& io) const
{
    const int nComps = io.nComps;
    // Channel c of the pixel is matrix row and column bit[c]: a one-channel pixel is alpha.
    int bit[4] = { 0, 1, 2, 3 };
    bool process[4] = { false, false, false, false };
    for (int c = 0; (c < nComps) && (c < 4); ++c) {
        bit[c] = pixelKernelChannelBit(nComps, c);
        process[c] = io.channels[bit[c]];
    }
    // Alpha reads 1 where the pixel has none, as the OFX ColorMatrix reads it.
    const float missingAlpha = ((nComps == 2) || (nComps == 3)) ? 1.f : 0.f;
    const float* src = io.src[0];
    float* dst = io.dst;
    for (int x = 0; x < io.width; ++x, src += nComps, dst += nComps) {
        float v[4] = { 0.f, 0.f, 0.f, missingAlpha };
        for (int c = 0; (c < nComps) && (c < 4); ++c) {
            v[bit[c]] = src[c];
        }
        for (int c = 0; (c < nComps) && (c < 4); ++c) {
            if (!process[c]) {
                continue;
            }
            const float* m = params.m[bit[c]];
            // One product and one sum per statement, in the kernel's order.
            float s = m[0] * v[0];
            const float g = m[1] * v[1];
            s = s + g;
            const float b = m[2] * v[2];
            s = s + b;
            const float a = m[3] * v[3];
            s = s + a;
            if (params.clampBlack && (s < 0.f)) {
                s = 0.f;
            } else if (params.clampWhite && (s > 1.f)) {
                s = 1.f;
            }
            dst[c] = s;
        }
    }
}

GpuOpRef
ColorMatrixOp::gpu(const Params& params) const
{
#ifdef NATRON_HAVE_GPU
    ColorMatrixGpuParams p = {};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            p.m[r][c] = params.m[r][c];
        }
    }
    p.flags = (params.clampBlack ? kColorMatrixGpuClampBlack : 0) | (params.clampWhite ? kColorMatrixGpuClampWhite : 0);

    return GpuOpRef::compiled((int)gpu::GpuKernelId::colormatrix_wrapped, p);
#else
    Q_UNUSED(params);

    return GpuOpRef::none();
#endif
}

ColorMatrix::ColorMatrix(NodePtr node)
    : ImageNode<ColorMatrixOp>(node)
{
}

ColorMatrix::~ColorMatrix()
{
}

ColorMatrixOp::Params
ColorMatrix::bakeColorMatrix(double time,
                             ViewIdx view) const
{
    return bakeParams(op(), time, view);
}

void
ColorMatrix::colorMatrixRow(const ColorMatrixOp::Params& params,
                            const RowIO& io) const
{
    op().row(params, io);
}

void
ColorMatrix::addAcceptedComponents(int inputNb,
                                   std::list<ImageLayerDesc>* comps)
{
    StagedImageEffect::addAcceptedComponents(inputNb, comps);
    if (!isInputMask(inputNb)) {
        comps->push_back(ImageLayerDesc::getXYComponents());
    }
}

NATRON_NAMESPACE_EXIT
