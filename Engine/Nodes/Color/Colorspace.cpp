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

#include "Colorspace.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/KnobTypes.h"
#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {

#ifdef NATRON_HAVE_GPU
// The parameter block of ColorspaceOp in colorspace.slang, which has no padding between members.
struct ColorspaceGpuSide {
    float kr, kg, kb, kgInv;
    float yScale, yOffset, yInv;
    float cOffset, cb, cr, cbInv, crInv;
};

struct ColorspaceGpuParams {
    uint32_t inModel;
    uint32_t outModel;
    float toXYZ[9];
    float fromXYZ[9];
    float white[3];
    float whiteInv[3];
    float whiteXy[2];
    ColorspaceGpuSide in;
    ColorspaceGpuSide out;
};

static_assert(sizeof(ColorspaceGpuParams) == 52 * 4, "must match ColorspaceOp in colorspace.slang");

ColorspaceGpuSide
gpuSide(const ColorMath::ColorspaceSide& s)
{
    ColorspaceGpuSide g;

    g.kr = s.kr;
    g.kg = s.kg;
    g.kb = s.kb;
    g.kgInv = s.kgInv;
    g.yScale = s.yScale;
    g.yOffset = s.yOffset;
    g.yInv = s.yInv;
    g.cOffset = s.cOffset;
    g.cb = s.cb;
    g.cr = s.cr;
    g.cbInv = s.cbInv;
    g.crInv = s.crInv;

    return g;
}
#endif

KnobChoicePtr
addModelKnob(KnobHolder* holder,
             const KnobPagePtr& page,
             const char* name,
             const std::string& label,
             const std::string& hint)
{
    KnobChoicePtr knob = AppManager::createKnob<KnobChoice>(holder, label);

    knob->setName(name);
    knob->setHintToolTip(hint);
    std::vector<ChoiceOption> options;
    options.push_back(ChoiceOption("rgb", "RGB", EffectInstance::tr("Linear RGB in the project's working space.").toStdString()));
    options.push_back(ChoiceOption("hsv", "HSV", EffectInstance::tr("Hue (0 to 1), saturation and value, the largest of R, G and B.").toStdString()));
    options.push_back(ChoiceOption("hsl", "HSL", EffectInstance::tr("Hue (0 to 1), saturation and lightness, the mean of the largest and smallest of R, G and B.").toStdString()));
    options.push_back(ChoiceOption("hsi", "HSI", EffectInstance::tr("Hue (0 to 1), saturation and intensity, the mean of R, G and B.").toStdString()));
    options.push_back(ChoiceOption("ypbpr601", "YPbPr (Rec. 601)", EffectInstance::tr("Luma and colour differences with the Rec. 601 coefficients, Pb and Pr from -0.5 to 0.5.").toStdString()));
    options.push_back(ChoiceOption("ypbpr709", "YPbPr (Rec. 709)", EffectInstance::tr("Luma and colour differences with the Rec. 709 coefficients, Pb and Pr from -0.5 to 0.5.").toStdString()));
    options.push_back(ChoiceOption("ypbpr2020", "YPbPr (Rec. 2020)", EffectInstance::tr("Luma and colour differences with the Rec. 2020 coefficients, Pb and Pr from -0.5 to 0.5.").toStdString()));
    options.push_back(ChoiceOption("ycbcr601", "YCbCr (Rec. 601)", EffectInstance::tr("YPbPr (Rec. 601) in studio range: Y from 16/255 to 235/255, Cb and Cr from 16/255 to 240/255.").toStdString()));
    options.push_back(ChoiceOption("ycbcr709", "YCbCr (Rec. 709)", EffectInstance::tr("YPbPr (Rec. 709) in studio range: Y from 16/255 to 235/255, Cb and Cr from 16/255 to 240/255.").toStdString()));
    options.push_back(ChoiceOption("ycbcr2020", "YCbCr (Rec. 2020)", EffectInstance::tr("YPbPr (Rec. 2020) in studio range: Y from 16/255 to 235/255, Cb and Cr from 16/255 to 240/255.").toStdString()));
    options.push_back(ChoiceOption("yuv601", "YUV (Rec. 601)", EffectInstance::tr("Analog YUV with the Rec. 601 coefficients: U from -0.436 to 0.436, V from -0.615 to 0.615.").toStdString()));
    options.push_back(ChoiceOption("yuv709", "YUV (Rec. 709)", EffectInstance::tr("Analog YUV with the Rec. 709 coefficients: U from -0.436 to 0.436, V from -0.615 to 0.615.").toStdString()));
    options.push_back(ChoiceOption("xyz", "XYZ", EffectInstance::tr("CIE XYZ, the working space's white being RGB (1, 1, 1).").toStdString()));
    options.push_back(ChoiceOption("xyy", "xyY", EffectInstance::tr("CIE xyY: the chromaticity x, y and the luminance Y. A colour with no luminance has the white's chromaticity.").toStdString()));
    options.push_back(ChoiceOption("lab", "L*a*b*", EffectInstance::tr("CIE L*a*b* relative to the working space's white, L* from 0 to 100.").toStdString()));
    knob->populateChoices(options);
    knob->setDefaultValue((int)ColorMath::eColorspaceModelRGB);
    knob->setAnimationEnabled(false);
    page->addKnob(knob);

    return knob;
}

ColorMath::ColorspaceModelEnum
modelAt(int index)
{
    if ((index < 0) || (index >= (int)ColorMath::eColorspaceModelCount)) {
        return ColorMath::eColorspaceModelRGB;
    }

    return (ColorMath::ColorspaceModelEnum)index;
}

} // anonymous namespace

void
ColorspaceOp::createKnobs(KnobHolder* knobHolder,
                          const KnobPagePtr& page)
{
    holder = knobHolder;
    KnobChoicePtr inKnob = addModelKnob(knobHolder, page, kColorspaceParamIn, EffectInstance::tr("In").toStdString(), EffectInstance::tr("The model the input's RGB channels are in.").toStdString());
    inKnob->setAddNewLine(false);
    in = inKnob;
    out = addModelKnob(knobHolder, page, kColorspaceParamOut, EffectInstance::tr("Out").toStdString(), EffectInstance::tr("The model the output's RGB channels are converted to.").toStdString());
}

ColorspaceOp::Params
ColorspaceOp::bake(const KnobReader& reader) const
{
    Params p;

    p.in = ColorMath::colorspaceSide(modelAt(reader.value(in)));
    p.out = ColorMath::colorspaceSide(modelAt(reader.value(out)));
    double toXYZ[9];
    ColorMath::workingToXYZ(holder, toXYZ);
    p.colorimetry = ColorMath::colorspaceColorimetry(toXYZ);

    return p;
}

bool
ColorspaceOp::isIdentity(const Params& params) const
{
    return params.in.model == params.out.model;
}

void
ColorspaceOp::row(const Params& params,
                  const RowIO& io) const
{
    const int nComps = io.nComps;
    // addAcceptedComponents() admits RGB and RGBA only; a narrower pixel has no colour to convert.
    if (nComps < 3) {
        std::copy(io.src[0], io.src[0] + io.width * nComps, io.dst);

        return;
    }
    bool process[3] = { false, false, false };

    for (int c = 0; (c < nComps) && (c < 3); ++c) {
        process[c] = io.channels[pixelKernelChannelBit(nComps, c)];
    }
    const float* src = io.src[0];
    float* dst = io.dst;
    for (int x = 0; x < io.width; ++x, src += nComps, dst += nComps) {
        float v[3] = { src[0], src[1], src[2] };
        ColorMath::colorspaceConvert(params.colorimetry, params.in, params.out, v);
        for (int c = 0; c < 3; ++c) {
            if (process[c]) {
                dst[c] = v[c];
            }
        }
    }
}

GpuOpRef
ColorspaceOp::gpu(const Params& params) const
{
#ifdef NATRON_HAVE_GPU
    ColorspaceGpuParams p = {};
    p.inModel = (uint32_t)params.in.model;
    p.outModel = (uint32_t)params.out.model;
    const ColorMath::ColorspaceColorimetry& cm = params.colorimetry;
    for (int i = 0; i < 9; ++i) {
        p.toXYZ[i] = cm.toXYZ[i];
        p.fromXYZ[i] = cm.fromXYZ[i];
    }
    for (int i = 0; i < 3; ++i) {
        p.white[i] = cm.white[i];
        p.whiteInv[i] = cm.whiteInv[i];
    }
    p.whiteXy[0] = cm.whiteXy[0];
    p.whiteXy[1] = cm.whiteXy[1];
    p.in = gpuSide(params.in);
    p.out = gpuSide(params.out);

    return GpuOpRef::compiled((int)gpu::GpuKernelId::colorspace_wrapped, p);
#else
    Q_UNUSED(params);

    return GpuOpRef::none();
#endif
}

Colorspace::Colorspace(NodePtr node)
    : ImageNode<ColorspaceOp>(node)
{
}

Colorspace::~Colorspace()
{
}

ColorspaceOp::Params
Colorspace::bakeColorspace(double time,
                           ViewIdx view) const
{
    return bakeParams(op(), time, view);
}

void
Colorspace::colorspaceRow(const ColorspaceOp::Params& params,
                          const RowIO& io) const
{
    op().row(params, io);
}

void
Colorspace::addAcceptedComponents(int inputNb,
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
