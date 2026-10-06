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
#include <memory>
#include <string>

#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Image/ColorMath.h"
#include "Engine/RenderScale.h"

NATRON_NAMESPACE_ENTER

namespace {
class SaturationKernel
    : public PixelKernel {
public:
    SaturationKernel(double saturation,
                     ColorMath::LuminanceMathEnum luminanceMath,
                     bool clampBlack,
                     bool clampWhite)
        : _saturation(saturation)
        , _luminanceMath(luminanceMath)
        , _clampBlack(clampBlack)
        , _clampWhite(clampWhite)
    {
    }

    virtual void processRow(const RowIO& io) const OVERRIDE FINAL
    {
        const int nComps = io.nComps;
        bool process[4] = { false, false, false, false };

        for (int c = 0; (c < nComps) && (c < 4); ++c) {
            process[c] = io.channels[pixelKernelChannelBit(nComps, c)];
        }
        const float* src = io.src[0];
        float* dst = io.dst;
        for (int x = 0; x < io.width; ++x, src += nComps, dst += nComps) {
            const double l = ColorMath::luminance(_luminanceMath, src[0], src[1], src[2]);
            for (int c = 0; c < nComps; ++c) {
                if (!process[c]) {
                    continue;
                }
                double v = src[c];
                if (c < 3) {
                    v = (1. - _saturation) * l + _saturation * v;
                }
                if (_clampBlack) {
                    v = (std::max)(0., v);
                }
                if (_clampWhite) {
                    v = (std::min)(1., v);
                }
                dst[c] = (float)v;
            }
        }
    }

private:
    double _saturation;
    ColorMath::LuminanceMathEnum _luminanceMath;
    bool _clampBlack;
    bool _clampWhite;
};

NativeImageTraits
saturationTraits()
{
    NativeImageTraits traits;

    traits.hostUnPremult = true;
    traits.defaultChannels[3] = false;

    return traits;
}

bool
readBool(const KnobBoolWPtr& weak,
         double time,
         ViewIdx view,
         bool defaultValue)
{
    KnobBoolPtr knob = weak.lock();

    return knob ? knob->getValueAtTime(time, 0, view) : defaultValue;
}

double
readSaturation(const KnobDoubleWPtr& weak,
               double time,
               ViewIdx view)
{
    KnobDoublePtr knob = weak.lock();

    return knob ? knob->getValueAtTime(time, 0, view) : 1.;
}
} // anonymous namespace

Saturation::Saturation(NodePtr node)
    : NativeImageEffect(node, saturationTraits())
{
}

Saturation::~Saturation()
{
}

void
Saturation::addAcceptedComponents(int inputNb,
                                  std::list<ImageLayerDesc>* comps)
{
    if (isInputMask(inputNb)) {
        NativeImageEffect::addAcceptedComponents(inputNb, comps);

        return;
    }
    comps->push_back(ImageLayerDesc::getRGBAComponents());
    comps->push_back(ImageLayerDesc::getRGBComponents());
}

NativePluginDescription
Saturation::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_SATURATION;
    desc.label = "Saturation";
    desc.description = tr("Modify the color saturation of an image.\n"
                          "Each processed channel is moved away from (saturation above 1) or towards "
                          "(saturation below 1) the pixel's luminance. A saturation of 0 produces grayscale.")
                           .toStdString();
    desc.grouping = PLUGIN_GROUP_COLOR;
    desc.majorVersion = PLUGIN_MAJOR_NATRON_SATURATION;
    desc.minorVersion = 0;
    desc.inputs.push_back(NativeInputDescription("Source", false, eDataKindImage));
    desc.inputs.push_back(NativeInputDescription("Mask", true, eDataKindImage, true));
    desc.outputKind = eDataKindImage;

    return desc;
}

void
Saturation::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobDoublePtr saturation = createKnob<KnobDouble>(tr("Saturation"));
    saturation->setName(kSaturationParamSaturation);
    saturation->setHintToolTip(tr("Color saturation factor to apply. 0 produces grayscale."));
    saturation->setDefaultValue(1.);
    saturation->setMinimum(0.);
    saturation->setMaximum(DBL_MAX);
    saturation->setDisplayMinimum(0.);
    saturation->setDisplayMaximum(4.);
    page->addKnob(saturation);
    _saturation = saturation;

    _luminanceMath = ColorMath::addLuminanceMathKnob(this, page);

    KnobBoolPtr clampBlack = createKnob<KnobBool>(tr("Clamp Black"));
    clampBlack->setName(kSaturationParamClampBlack);
    clampBlack->setHintToolTip(tr("All colors below 0 on output are set to 0."));
    clampBlack->setDefaultValue(true);
    clampBlack->setAddNewLine(false);
    page->addKnob(clampBlack);
    _clampBlack = clampBlack;

    KnobBoolPtr clampWhite = createKnob<KnobBool>(tr("Clamp White"));
    clampWhite->setName(kSaturationParamClampWhite);
    clampWhite->setHintToolTip(tr("All colors above 1 on output are set to 1."));
    clampWhite->setDefaultValue(false);
    page->addKnob(clampWhite);
    _clampWhite = clampWhite;

    addMaskMixKnobs(page);
}

PixelKernelPtr
Saturation::makeKernel(const KernelContext& context)
{
    KnobChoicePtr math = _luminanceMath.lock();
    int mathIndex = math ? math->getValueAtTime(context.time, 0, context.view) : (int)ColorMath::eLuminanceMathRec709;

    if ((mathIndex < (int)ColorMath::eLuminanceMathRec709) || (mathIndex > (int)ColorMath::eLuminanceMathMax)) {
        mathIndex = (int)ColorMath::eLuminanceMathRec709;
    }

    return std::make_shared<SaturationKernel>(readSaturation(_saturation, context.time, context.view),
                                              (ColorMath::LuminanceMathEnum)mathIndex,
                                              readBool(_clampBlack, context.time, context.view, true),
                                              readBool(_clampWhite, context.time, context.view, false));
}

bool
Saturation::isIdentityOp(double time,
                         const RenderScale& /*scale*/,
                         const RectI& /*roi*/,
                         ViewIdx view)
{
    if (readBool(_clampBlack, time, view, true) || readBool(_clampWhite, time, view, false)) {
        return false;
    }

    return readSaturation(_saturation, time, view) == 1.;
}

NATRON_NAMESPACE_EXIT
