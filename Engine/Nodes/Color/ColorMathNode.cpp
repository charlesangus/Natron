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

NATRON_NAMESPACE_ENTER

namespace {
struct ColorMathValues {
    double value[4];
};

NativeImageTraits
colorMathTraits()
{
    NativeImageTraits traits;

    traits.hostUnPremult = true;
    traits.defaultChannels[3] = false;

    return traits;
}

// Visits the channels the kernel processes: `bits[c]` is the bit of channel c of an nComps-channel
// pixel, or -1 when that channel is not processed.
inline void
processedBits(const RowIO& io,
              int bits[4])
{
    for (int c = 0; c < 4; ++c) {
        bits[c] = -1;
    }
    for (int c = 0; (c < io.nComps) && (c < 4); ++c) {
        const int bit = pixelKernelChannelBit(io.nComps, c);
        bits[c] = io.channels[bit] ? bit : -1;
    }
}

class AddKernel
    : public PixelKernel {
public:
    explicit AddKernel(const ColorMathValues& values)
    {
        for (int i = 0; i < 4; ++i) {
            _value[i] = (float)values.value[i];
        }
    }

    virtual void processRow(const RowIO& io) const OVERRIDE FINAL
    {
        int bits[4];

        processedBits(io, bits);
        const int nComps = io.nComps;
        const float* src = io.src[0];
        float* dst = io.dst;
        for (int x = 0; x < io.width; ++x, src += nComps, dst += nComps) {
            for (int c = 0; (c < nComps) && (c < 4); ++c) {
                if (bits[c] >= 0) {
                    dst[c] = src[c] + _value[bits[c]];
                }
            }
        }
    }

private:
    float _value[4];
};

class MultiplyKernel
    : public PixelKernel {
public:
    explicit MultiplyKernel(const ColorMathValues& values)
    {
        for (int i = 0; i < 4; ++i) {
            _value[i] = (float)values.value[i];
        }
    }

    virtual void processRow(const RowIO& io) const OVERRIDE FINAL
    {
        int bits[4];

        processedBits(io, bits);
        const int nComps = io.nComps;
        const float* src = io.src[0];
        float* dst = io.dst;
        for (int x = 0; x < io.width; ++x, src += nComps, dst += nComps) {
            for (int c = 0; (c < nComps) && (c < 4); ++c) {
                if (bits[c] >= 0) {
                    dst[c] = src[c] * _value[bits[c]];
                }
            }
        }
    }

private:
    float _value[4];
};

class GammaKernel
    : public PixelKernel {
public:
    GammaKernel(const ColorMathValues& values,
                bool invert)
    {
        for (int i = 0; i < 4; ++i) {
            _exponent[i] = invert ? values.value[i] : (1. / (std::max)(1e-8, values.value[i]));
            _isUnit[i] = (_exponent[i] == 1.);
            _exponentFloat[i] = (float)_exponent[i];
        }
    }

    virtual void processRow(const RowIO& io) const OVERRIDE FINAL
    {
        int bits[4];

        processedBits(io, bits);
        const int nComps = io.nComps;
        const float* src = io.src[0];
        float* dst = io.dst;
        for (int x = 0; x < io.width; ++x, src += nComps, dst += nComps) {
            for (int c = 0; (c < nComps) && (c < 4); ++c) {
                const int bit = bits[c];
                if (bit < 0) {
                    continue;
                }
                const float v = src[c];
                // pow of a negative base is NaN; such values pass through, as in Nuke.
                dst[c] = ((v <= 0.f) || _isUnit[bit]) ? v : std::pow(v, _exponentFloat[bit]);
            }
        }
    }

private:
    double _exponent[4];
    float _exponentFloat[4];
    bool _isUnit[4];
};

void
readValue(const KnobColorWPtr& weak,
          double time,
          ViewIdx view,
          double defaultValue,
          ColorMathValues* values)
{
    KnobColorPtr knob = weak.lock();

    for (int i = 0; i < 4; ++i) {
        values->value[i] = knob ? knob->getValueAtTime(time, i, view) : defaultValue;
    }
}

double
neutralValue(ColorMathOperationEnum operation)
{
    return (operation == eColorMathOperationAdd) ? 0. : 1.;
}
} // anonymous namespace

ColorMathNode::ColorMathNode(NodePtr node,
                             ColorMathOperationEnum operation)
    : NativeImageEffect(node, colorMathTraits())
    , _operation(operation)
{
}

ColorMathNode::~ColorMathNode()
{
}

void
ColorMathNode::addAcceptedComponents(int inputNb,
                                     std::list<ImageLayerDesc>* comps)
{
    NativeImageEffect::addAcceptedComponents(inputNb, comps);
    if ((_operation != eColorMathOperationAdd) && !isInputMask(inputNb)) {
        comps->push_back(ImageLayerDesc::getXYComponents());
    }
}

NativePluginDescription
ColorMathNode::getNativePluginDescription() const
{
    NativePluginDescription desc;

    switch (_operation) {
    case eColorMathOperationAdd:
        desc.id = PLUGINID_NATRON_ADD;
        desc.label = "Add";
        desc.description = tr("Add a constant to the selected channels.").toStdString();
        break;
    case eColorMathOperationMultiply:
        desc.id = PLUGINID_NATRON_MULTIPLY;
        desc.label = "Multiply";
        desc.description = tr("Multiply the selected channels by a constant.").toStdString();
        break;
    case eColorMathOperationGamma:
        desc.id = PLUGINID_NATRON_GAMMA;
        desc.label = "Gamma";
        desc.description = tr("Apply gamma function to the selected channels. The actual function is pow(x,1/max(1e-8,value)).").toStdString();
        break;
    }
    desc.grouping = PLUGIN_GROUP_COLOR "/Math";
    desc.majorVersion = PLUGIN_MAJOR_NATRON_COLORMATH;
    desc.minorVersion = 0;
    desc.inputs.push_back(NativeInputDescription("Source", false, eDataKindImage));
    desc.inputs.push_back(NativeInputDescription("Mask", true, eDataKindImage, true));
    desc.outputKind = eDataKindImage;

    return desc;
}

void
ColorMathNode::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobColorPtr value = createKnob<KnobColor>(tr("Value").toStdString(), 4);
    value->setName(kColorMathParamValue);
    switch (_operation) {
    case eColorMathOperationAdd:
        value->setHintToolTip(tr("Constant to add to the selected channels."));
        break;
    case eColorMathOperationMultiply:
        value->setHintToolTip(tr("Constant to multiply with the selected channels."));
        break;
    case eColorMathOperationGamma:
        value->setHintToolTip(tr("Gamma value to apply to the selected channels."));
        break;
    }
    const double minimum = (_operation == eColorMathOperationGamma) ? 0. : -DBL_MAX;
    for (int i = 0; i < 4; ++i) {
        value->setDefaultValue(neutralValue(_operation), i);
        value->setMinimum(minimum, i);
        value->setMaximum(DBL_MAX, i);
        value->setDisplayMinimum(0., i);
        value->setDisplayMaximum(4., i);
    }
    page->addKnob(value);
    _value = value;

    if (_operation == eColorMathOperationGamma) {
        KnobBoolPtr invert = createKnob<KnobBool>(tr("Invert"));
        invert->setName(kColorMathParamInvert);
        invert->setHintToolTip(tr("Invert the gamma transform."));
        invert->setDefaultValue(false);
        invert->setAnimationEnabled(false);
        page->addKnob(invert);
        _invert = invert;
    }

    addMaskMixKnobs(page);
}

PixelKernelPtr
ColorMathNode::makeKernel(const KernelContext& context)
{
    ColorMathValues values;

    readValue(_value, context.time, context.view, neutralValue(_operation), &values);
    switch (_operation) {
    case eColorMathOperationAdd:
        return std::make_shared<AddKernel>(values);
    case eColorMathOperationMultiply:
        return std::make_shared<MultiplyKernel>(values);
    case eColorMathOperationGamma: {
        KnobBoolPtr invert = _invert.lock();

        return std::make_shared<GammaKernel>(values, invert ? invert->getValueAtTime(context.time, 0, context.view) : false);
    }
    }

    return PixelKernelPtr();
}

bool
ColorMathNode::isIdentityOp(double time,
                            const RenderScale& /*scale*/,
                            const RectI& /*roi*/,
                            ViewIdx view)
{
    ColorMathValues values;

    readValue(_value, time, view, neutralValue(_operation), &values);
    for (int i = 0; i < 4; ++i) {
        if (values.value[i] != neutralValue(_operation)) {
            return false;
        }
    }

    return true;
}

EffectInstance*
ColorMathAdd::BuildEffect(NodePtr node)
{
    return new ColorMathNode(node, eColorMathOperationAdd);
}

EffectInstance*
ColorMathMultiply::BuildEffect(NodePtr node)
{
    return new ColorMathNode(node, eColorMathOperationMultiply);
}

EffectInstance*
ColorMathGamma::BuildEffect(NodePtr node)
{
    return new ColorMathNode(node, eColorMathOperationGamma);
}

NATRON_NAMESPACE_EXIT
