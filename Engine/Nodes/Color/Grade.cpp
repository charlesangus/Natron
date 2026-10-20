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

#include "Grade.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <limits>
#include <memory>
#include <string>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/RenderScale.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
// Gamma at or below 0 has no meaningful power, so it is raised to this floor. Below it the GPU's
// float rounding of A * input + B, multiplied by the exponent 1 / gamma, would exceed the
// transcendental parity bound against the double-precision CPU path.
constexpr double kGradeMinGamma = 0.01;

double
gradeValue(double v,
           const GradeOp::Channel& ch)
{
    const double x = (ch.a * v) + ch.b;

    if (ch.gamma == 1.) {
        return x;
    }
    // pow would give NaN; negatives pass through ungammaed, as in Nuke.
    if (x <= 0) {
        return x;
    }

    return std::pow(x, 1. / ch.gamma);
}

double
invgradeValue(double v,
              const GradeOp::Channel& ch)
{
    if ((ch.gamma != 1.) && (v > 0)) {
        v = std::pow(v, ch.gamma);
    }
    v = v - ch.b;
    if (ch.a != 0) {
        v /= ch.a;
    }

    return v;
}

KnobColorPtr
addGradeColorKnob(KnobHolder* holder,
                  const KnobPagePtr& page,
                  const std::string& name,
                  const std::string& label,
                  const std::string& hint,
                  double defaultValue,
                  double displayMin,
                  double displayMax,
                  ColorKnobRoleEnum role)
{
    KnobColorPtr knob = AppManager::createKnob<KnobColor>(holder, label, 4);

    knob->setName(name);
    knob->setHintToolTip(hint);
    for (int i = 0; i < 4; ++i) {
        knob->setDefaultValue(defaultValue, i);
        knob->setMinimum(-DBL_MAX, i);
        knob->setMaximum(DBL_MAX, i);
        knob->setDisplayMinimum(displayMin, i);
        knob->setDisplayMaximum(displayMax, i);
    }
    knob->setColorRole(role);
    page->addKnob(knob);

    return knob;
}

#ifdef NATRON_HAVE_GPU
// The parameter block of GradeOp in pixelwrap.slang, which has no padding between members.
struct GradeGpuParams {
    float a[4];
    float b[4];
    float gamma[4];
    float invGamma[4];
    uint32_t channelMask;
    uint32_t flags;
};

static_assert(sizeof(GradeGpuParams) == 72, "must match GradeOp in pixelwrap.slang");

constexpr uint32_t kGradeGpuReverse = 1;
constexpr uint32_t kGradeGpuClampBlack = 2;
constexpr uint32_t kGradeGpuClampWhite = 4;
#endif
} // anonymous namespace

void
GradeOp::createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page)
{
    blackPoint = addGradeColorKnob(holder, page, kGradeParamBlackPoint, EffectInstance::tr("Black Point").toStdString(), EffectInstance::tr("Set the color of the darkest pixels in the image.").toStdString(), 0., -1., 1., eColorKnobRoleOffset);
    whitePoint = addGradeColorKnob(holder, page, kGradeParamWhitePoint, EffectInstance::tr("White Point").toStdString(), EffectInstance::tr("Set the color of the brightest pixels in the image.").toStdString(), 1., 0., 4., eColorKnobRoleGain);
    black = addGradeColorKnob(holder, page, kGradeParamBlack, EffectInstance::tr("Lift").toStdString(), EffectInstance::tr("Colors corresponding to the blackpoint are set to this value.").toStdString(), 0., -1., 1., eColorKnobRoleOffset);
    white = addGradeColorKnob(holder, page, kGradeParamWhite, EffectInstance::tr("Gain").toStdString(), EffectInstance::tr("Colors corresponding to the whitepoint are set to this value.").toStdString(), 1., 0., 4., eColorKnobRoleGain);
    multiply = addGradeColorKnob(holder, page, kGradeParamMultiply, EffectInstance::tr("Multiply").toStdString(), EffectInstance::tr("Multiplies the result by this value.").toStdString(), 1., 0., 4., eColorKnobRoleGain);
    offset = addGradeColorKnob(holder, page, kGradeParamOffset, EffectInstance::tr("Offset").toStdString(), EffectInstance::tr("Adds this value to the result (this applies to black and white).").toStdString(), 0., -1., 1., eColorKnobRoleOffset);
    gamma = addGradeColorKnob(holder, page, kGradeParamGamma, EffectInstance::tr("Gamma").toStdString(), EffectInstance::tr("Final gamma correction. Negative values are not affected by gamma. A gamma below 0.01 is treated as 0.01.").toStdString(), 1., 0.2, 5., eColorKnobRolePower);

    KnobButtonPtr normalizeKnob = AppManager::createKnob<KnobButton>(holder, EffectInstance::tr("Normalize").toStdString());
    normalizeKnob->setName(kGradeParamNormalize);
    normalizeKnob->setHintToolTip(EffectInstance::tr("Normalize the image by setting the white point and black point from the minimum and maximum values of the input.").toStdString());
    normalizeKnob->setEvaluateOnChange(false);
    page->addKnob(normalizeKnob);
    normalize = normalizeKnob;

    KnobBoolPtr reverseKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr("Reverse").toStdString());
    reverseKnob->setName(kGradeParamReverse);
    reverseKnob->setHintToolTip(EffectInstance::tr("Apply the inverse correction.  Useful to apply the inverse of a Grade downstream: copy-and-paste or clone the upstream node, and invert the downstream one.").toStdString());
    reverseKnob->setDefaultValue(false);
    reverseKnob->setAddNewLine(false);
    page->addKnob(reverseKnob);
    reverse = reverseKnob;

    KnobBoolPtr clampBlackKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr("Clamp Black").toStdString());
    clampBlackKnob->setName(kGradeParamClampBlack);
    clampBlackKnob->setHintToolTip(EffectInstance::tr("All colors below 0 on output are set to 0.").toStdString());
    clampBlackKnob->setDefaultValue(false);
    clampBlackKnob->setAddNewLine(false);
    page->addKnob(clampBlackKnob);
    clampBlack = clampBlackKnob;

    KnobBoolPtr clampWhiteKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr("Clamp White").toStdString());
    clampWhiteKnob->setName(kGradeParamClampWhite);
    clampWhiteKnob->setHintToolTip(EffectInstance::tr("All colors above 1 on output are set to 1.").toStdString());
    clampWhiteKnob->setDefaultValue(false);
    page->addKnob(clampWhiteKnob);
    clampWhite = clampWhiteKnob;
}

GradeOp::Params
GradeOp::bake(const KnobReader& reader) const
{
    double values[7][4];
    const KnobColorWPtr* const knobs[7] = { &blackPoint, &whitePoint, &black, &white, &multiply, &offset, &gamma };

    for (int k = 0; k < 7; ++k) {
        for (int i = 0; i < 4; ++i) {
            values[k][i] = reader.value(*knobs[k], i);
        }
    }
    const double (&bp)[4] = values[0];
    const double (&wp)[4] = values[1];
    const double (&bl)[4] = values[2];
    const double (&wh)[4] = values[3];
    const double (&mul)[4] = values[4];
    const double (&off)[4] = values[5];
    const double (&gam)[4] = values[6];

    Params p;
    p.isDefault = true;
    for (int i = 0; i < 4; ++i) {
        const double d = wp[i] - bp[i];
        const double A = (d != 0) ? mul[i] * (wh[i] - bl[i]) / d : 0;
        p.channels[i].a = A;
        p.channels[i].b = off[i] + bl[i] - A * bp[i];
        p.channels[i].gamma = (gam[i] > kGradeMinGamma) ? gam[i] : kGradeMinGamma;
        if ((bp[i] != 0.) || (wp[i] != 1.) || (bl[i] != 0.) || (wh[i] != 1.) || (mul[i] != 1.) || (off[i] != 0.) || (gam[i] != 1.)) {
            p.isDefault = false;
        }
    }
    p.reverse = reader.value(reverse);
    p.clampBlack = reader.value(clampBlack);
    p.clampWhite = reader.value(clampWhite);

    return p;
}

bool
GradeOp::isIdentity(const Params& params) const
{
    return params.isDefault && !params.clampBlack && !params.clampWhite;
}

void
GradeOp::row(const Params& params,
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
            const Channel& ch = params.channels[bits[c]];
            double v = src[c];
            v = params.reverse ? invgradeValue(v, ch) : gradeValue(v, ch);
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
GradeOp::gpu(const Params& params) const
{
#ifdef NATRON_HAVE_GPU
    GradeGpuParams p = {};
    for (int i = 0; i < 4; ++i) {
        p.a[i] = (float)params.channels[i].a;
        p.b[i] = (float)params.channels[i].b;
        p.gamma[i] = (float)params.channels[i].gamma;
        p.invGamma[i] = (float)(1. / params.channels[i].gamma);
    }
    p.channelMask = 0xF;
    p.flags = (params.reverse ? kGradeGpuReverse : 0) | (params.clampBlack ? kGradeGpuClampBlack : 0) | (params.clampWhite ? kGradeGpuClampWhite : 0);

    return GpuOpRef::compiled((int)gpu::GpuKernelId::grade_wrapped, p);
#else
    Q_UNUSED(params);

    return GpuOpRef::none();
#endif
}

Grade::Grade(NodePtr node)
    : ImageNode<GradeOp>(node)
{
}

Grade::~Grade()
{
}

GradeOp::Params
Grade::bakeGrade(double time,
                 ViewIdx view) const
{
    return bakeParams(op(), time, view);
}

void
Grade::gradeRow(const GradeOp::Params& params,
                const RowIO& io) const
{
    op().row(params, io);
}

bool
Grade::knobChanged(KnobI* k,
                   ValueChangedReasonEnum /*reason*/,
                   ViewSpec view,
                   double time,
                   bool /*originatedFromMainThread*/)
{
    KnobButtonPtr normalizeButton = op().normalize.lock();

    if (!normalizeButton || (k != normalizeButton.get())) {
        return false;
    }
    normalize(time, (view.isAll() || view.isCurrent()) ? ViewIdx(0) : ViewIdx(view));

    return true;
}

void
Grade::normalize(double time,
                 ViewIdx view)
{
    KnobColorPtr blackPoint = op().blackPoint.lock();
    KnobColorPtr whitePoint = op().whitePoint.lock();

    if (!blackPoint || !whitePoint || !getInput(0)) {
        return;
    }

    RectI roi;
    ImagePtr src = getImage(0, time, getOverlayInteractRenderScale(), view, NULL, NULL, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &roi);
    if (!src || (src->getBitDepth() != eImageBitDepthFloat)) {
        return;
    }

    const int nComps = (int)src->getComponentsCount();
    if ((nComps != 1) && (nComps != 3) && (nComps != 4)) {
        return;
    }
    double minimum[4];
    double maximum[4];
    std::fill(minimum, minimum + 4, std::numeric_limits<double>::infinity());
    std::fill(maximum, maximum + 4, -std::numeric_limits<double>::infinity());
    const RectI& bounds = src->getBounds();
    bool any = false;
    {
        Image::ReadAccess access(src.get());
        for (int y = bounds.y1; y < bounds.y2; ++y) {
            const float* pix = (const float*)access.pixelAt(bounds.x1, y);
            if (!pix) {
                continue;
            }
            for (int x = bounds.x1; x < bounds.x2; ++x, pix += nComps) {
                for (int c = 0; c < nComps; ++c) {
                    const double v = pix[c];
                    minimum[c] = (std::min)(minimum[c], v);
                    maximum[c] = (std::max)(maximum[c], v);
                }
                any = true;
            }
        }
    }

    // In RGBA order. A channel the image lacks reads 0, then the OpenFX Grade's rule applies:
    // an RGB image whose three channels agree gives alpha their value, and an alpha image gives
    // every channel its own.
    double minRGBA[4];
    double maxRGBA[4];
    std::fill(minRGBA, minRGBA + 4, std::numeric_limits<double>::infinity());
    std::fill(maxRGBA, maxRGBA + 4, -std::numeric_limits<double>::infinity());
    if (any) {
        for (int i = 0; i < 4; ++i) {
            const int c = (nComps == 1) ? ((i == 3) ? 0 : -1) : ((i < nComps) ? i : -1);
            minRGBA[i] = (c >= 0) ? minimum[c] : 0.;
            maxRGBA[i] = (c >= 0) ? maximum[c] : 0.;
        }
    }
    if (nComps == 3) {
        if ((minRGBA[0] == minRGBA[1]) && (minRGBA[0] == minRGBA[2])) {
            minRGBA[3] = minRGBA[0];
        }
        if ((maxRGBA[0] == maxRGBA[1]) && (maxRGBA[0] == maxRGBA[2])) {
            maxRGBA[3] = maxRGBA[0];
        }
    } else if (nComps == 1) {
        minRGBA[0] = minRGBA[1] = minRGBA[2] = minRGBA[3];
        maxRGBA[0] = maxRGBA[1] = maxRGBA[2] = maxRGBA[3];
    }

    AppInstancePtr app = getApp();
    const bool oneUndoStep = app && !app->isCreatingPythonGroup();
    if (oneUndoStep) {
        setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOnCreateNewCommand);
    }
    for (int i = 0; i < 4; ++i) {
        blackPoint->setValue(minRGBA[i], ViewSpec::all(), i);
        whitePoint->setValue(maxRGBA[i], ViewSpec::all(), i);
    }
    if (oneUndoStep) {
        setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOff);
    }
} // Grade::normalize

NATRON_NAMESPACE_EXIT
