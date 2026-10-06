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
#include <memory>
#include <string>

#include "Engine/AppManager.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/RenderScale.h"

NATRON_NAMESPACE_ENTER

namespace {
struct ClampValues {
    double minimum[4];
    double maximum[4];
    double minClampTo[4];
    double maxClampTo[4];
    bool minimumEnable;
    bool maximumEnable;
    bool minClampToEnable;
    bool maxClampToEnable;
};

class ClampKernel
    : public PixelKernel {
public:
    explicit ClampKernel(const ClampValues& values)
        : _values(values)
    {
    }

    virtual void processRow(const RowIO& io) const OVERRIDE FINAL
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
                if (_values.minimumEnable && (v < _values.minimum[i])) {
                    out = _values.minClampToEnable ? _values.minClampTo[i] : _values.minimum[i];
                } else if (_values.maximumEnable && (v > _values.maximum[i])) {
                    out = _values.maxClampToEnable ? _values.maxClampTo[i] : _values.maximum[i];
                }
                dst[c] = (float)out;
            }
        }
    }

private:
    ClampValues _values;
};

NativeImageTraits
clampTraits()
{
    NativeImageTraits traits;

    traits.hostUnPremult = true;
    traits.defaultChannels[3] = true;

    return traits;
}

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

void
readColor(const KnobColorWPtr& weak,
          double time,
          ViewIdx view,
          double defaultValue,
          double out[4])
{
    KnobColorPtr knob = weak.lock();

    for (int i = 0; i < 4; ++i) {
        out[i] = knob ? knob->getValueAtTime(time, i, view) : defaultValue;
    }
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
} // anonymous namespace

Clamp::Clamp(NodePtr node)
    : NativeImageEffect(node, clampTraits())
{
}

Clamp::~Clamp()
{
}

void
Clamp::addAcceptedComponents(int inputNb,
                             std::list<ImageLayerDesc>* comps)
{
    NativeImageEffect::addAcceptedComponents(inputNb, comps);
    if (!isInputMask(inputNb)) {
        comps->push_back(ImageLayerDesc::getXYComponents());
    }
}

NativePluginDescription
Clamp::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_CLAMP;
    desc.label = "Clamp";
    desc.description = tr("Clamp the values of the selected channels.\n"
                          "\n"
                          "A special use case for the Clamp plugin is to generate a binary mask image "
                          "(i.e. each pixel is either 0 or 1) by thresholding an image. Let us say one wants "
                          "all input pixels whose value is above or equal to some threshold value to "
                          "become 1, and all values below this threshold to become 0. Set the \"Minimum\" value "
                          "to the threshold, set the \"Maximum\" to any value strictly below the threshold "
                          "(e.g. 0 if the threshold is positive), and "
                          "check \"Enable MinClampTo\" and \"Enable MaxClampTo\" while keeping the default "
                          "values for \"MinClampTo\" (0.0) and \"MaxClampTop\" (1.0). The result is a binary "
                          "mask image. To create a non-binary mask, with softer edges, either blur the output "
                          "of Clamp, or use the Grade plugin instead, setting the \"Black Point\" and \"White Point\" "
                          "to values close to the threshold, and checking the \"Clamp Black\" and \"Clamp "
                          "White\" options.\n"
                          "\n"
                          "See also: https://web.archive.org/web/20220627024614/http://www.opticalenquiry.com/nuke/index.php?title=Clamp")
                           .toStdString();
    desc.grouping = PLUGIN_GROUP_COLOR;
    desc.majorVersion = PLUGIN_MAJOR_NATRON_CLAMP;
    desc.minorVersion = 0;
    desc.inputs.push_back(NativeInputDescription("Source", false, eDataKindImage));
    desc.inputs.push_back(NativeInputDescription("Mask", true, eDataKindImage, true));
    desc.outputKind = eDataKindImage;

    return desc;
}

void
Clamp::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    _minimum = addClampColorKnob(this, page, kClampParamMinimum, tr("Minimum").toStdString(),
                                 tr("If enabled, all values that are lower than this number are set to this value, or to the minClampTo value if minClampTo is enabled.").toStdString(), 0.);
    _minimumEnable = addClampEnableKnob(this, page, kClampParamMinimumEnable, tr("Enable Minimum").toStdString(),
                                        tr("Whether to clamp selected channels to a minimum value.").toStdString(), true);
    _maximum = addClampColorKnob(this, page, kClampParamMaximum, tr("Maximum").toStdString(),
                                 tr("If enabled, all values that are higher than this number are set to this value, or to the maxClampTo value if maxClampTo is enabled.").toStdString(), 1.);
    _maximumEnable = addClampEnableKnob(this, page, kClampParamMaximumEnable, tr("Enable Maximum").toStdString(),
                                        tr("Whether to clamp selected channels to a maximum value.").toStdString(), true);
    _minClampTo = addClampColorKnob(this, page, kClampParamMinClampTo, tr("MinClampTo").toStdString(),
                                    tr("The value to which values below minimum are clamped when minClampTo is enabled. Setting this to a custom color helps visualizing the clamped areas or create graphic effects.").toStdString(), 0.);
    _minClampToEnable = addClampEnableKnob(this, page, kClampParamMinClampToEnable, tr("Enable MinClampTo").toStdString(),
                                           tr("When enabled, all values below minimum are set to the minClampTo value.\nWhen disabled, all values below minimum are clamped to the minimum value.").toStdString(), false);
    _maxClampTo = addClampColorKnob(this, page, kClampParamMaxClampTo, tr("MaxClampTo").toStdString(),
                                    tr("The value to which values above maximum are clamped when maxClampTo is enabled. Setting this to a custom color helps visualizing the clamped areas or create graphic effects.").toStdString(), 1.);
    _maxClampToEnable = addClampEnableKnob(this, page, kClampParamMaxClampToEnable, tr("Enable MaxClampTo").toStdString(),
                                           tr("When enabled, all values above maximum are set to the maxClampTo value.\nWhen disabled, all values above maximum are clamped to the maximum value.").toStdString(), false);

    addMaskMixKnobs(page);
}

PixelKernelPtr
Clamp::makeKernel(const KernelContext& context)
{
    ClampValues values;

    readColor(_minimum, context.time, context.view, 0., values.minimum);
    readColor(_maximum, context.time, context.view, 1., values.maximum);
    readColor(_minClampTo, context.time, context.view, 0., values.minClampTo);
    readColor(_maxClampTo, context.time, context.view, 1., values.maxClampTo);
    values.minimumEnable = readBool(_minimumEnable, context.time, context.view, true);
    values.maximumEnable = readBool(_maximumEnable, context.time, context.view, true);
    values.minClampToEnable = readBool(_minClampToEnable, context.time, context.view, false);
    values.maxClampToEnable = readBool(_maxClampToEnable, context.time, context.view, false);

    return std::make_shared<ClampKernel>(values);
}

bool
Clamp::isIdentityOp(double time,
                    const RenderScale& /*scale*/,
                    const RectI& /*roi*/,
                    ViewIdx view)
{
    return !readBool(_minimumEnable, time, view, true) && !readBool(_maximumEnable, time, view, true);
}

NATRON_NAMESPACE_EXIT
