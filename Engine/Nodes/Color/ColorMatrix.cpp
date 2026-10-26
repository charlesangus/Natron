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
constexpr uint32_t kColorMatrixGpuThreeByThree = 4;
#endif

} // anonymous namespace

void
ColorMatrixOp::createKnobs(KnobHolder* holder,
                           const KnobPagePtr& page)
{
    KnobChoicePtr sizeKnob = AppManager::createKnob<KnobChoice>(holder, EffectInstance::tr("Matrix").toStdString());
    sizeKnob->setName(kColorMatrixParamMatrixSize);
    sizeKnob->setHintToolTip(EffectInstance::tr("The size of the matrix. At 3x3 the matrix mixes R, G and B and alpha passes through untouched; "
                                                "at 4x4 alpha takes part as a fourth input and output. Switching keeps every coefficient.")
                                 .toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("3x3", "3x3", EffectInstance::tr("Mix R, G and B; alpha passes through untouched.").toStdString()));
        options.push_back(ChoiceOption("4x4", "4x4", EffectInstance::tr("Mix R, G, B and A.").toStdString()));
        sizeKnob->populateChoices(options);
    }
    sizeKnob->setDefaultValue((int)eMatrixSize3x3);
    sizeKnob->setAnimationEnabled(false);
    page->addKnob(sizeKnob);
    matrixSize = sizeKnob;

    KnobDoublePtr matrixKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Coefficients").toStdString(), 16);
    matrixKnob->setName(kColorMatrixParamMatrix);
    matrixKnob->setHintToolTip(EffectInstance::tr("The matrix: each row holds the weights of the input R, G, B and A in "
                                                  "the output channel the row is named after.")
                                   .toStdString());
    const char* const channelNames[4] = { "r", "g", "b", "a" };
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            const int dim = r * 4 + c;
            matrixKnob->setDefaultValue((r == c) ? 1. : 0., dim);
            matrixKnob->setMinimum(-DBL_MAX, dim);
            matrixKnob->setMaximum(DBL_MAX, dim);
            matrixKnob->setDimensionName(dim, std::string(channelNames[r]) + channelNames[c]);
        }
    }
    matrixKnob->setMatrixDisplay(4, 4, std::vector<std::string> { "R", "G", "B", "A" }, std::vector<std::string> { "R", "G", "B", "A" });
    matrixKnob->setMatrixVisibleExtent(3, 3);
    page->addKnob(matrixKnob);
    matrix = matrixKnob;

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
            p.m[r][c] = (float)reader.value(matrix, r * 4 + c);
        }
    }
    p.threeByThree = (reader.value(matrixSize) != (int)eMatrixSize4x4);
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
    const int n = params.threeByThree ? 3 : 4;
    for (int r = 0; r < n; ++r) {
        for (int c = 0; c < n; ++c) {
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
    const bool threeByThree = params.threeByThree;
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
            if (threeByThree && (bit[c] == 3)) {
                dst[c] = src[c];
                continue;
            }
            const float* m = params.m[bit[c]];
            // One product and one sum per statement, in the kernel's order.
            float s = m[0] * v[0];
            const float g = m[1] * v[1];
            s = s + g;
            const float b = m[2] * v[2];
            s = s + b;
            if (!threeByThree) {
                const float a = m[3] * v[3];
                s = s + a;
            }
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
    p.flags = (params.clampBlack ? kColorMatrixGpuClampBlack : 0) | (params.clampWhite ? kColorMatrixGpuClampWhite : 0) | (params.threeByThree ? kColorMatrixGpuThreeByThree : 0);

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

void
ColorMatrix::refreshMatrixExtent()
{
    KnobChoicePtr sizeKnob = op().matrixSize.lock();
    KnobDoublePtr matrixKnob = op().matrix.lock();
    if (!sizeKnob || !matrixKnob) {
        return;
    }
    const int n = (sizeKnob->getValue() == (int)ColorMatrixOp::eMatrixSize4x4) ? 4 : 3;
    matrixKnob->setMatrixVisibleExtent(n, n);
}

bool
ColorMatrix::knobChanged(KnobI* k,
                         ValueChangedReasonEnum /*reason*/,
                         ViewSpec /*view*/,
                         double /*time*/,
                         bool /*originatedFromMainThread*/)
{
    KnobChoicePtr sizeKnob = op().matrixSize.lock();
    if (!sizeKnob || (k != sizeKnob.get())) {
        return false;
    }
    refreshMatrixExtent();

    return true;
}

void
ColorMatrix::onKnobsLoaded()
{
    refreshMatrixExtent();
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
