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
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/RenderScale.h"

NATRON_NAMESPACE_ENTER

namespace {
struct GradeChannel {
    double a;
    double b;
    double gamma;
};

struct GradeValues {
    double blackPoint[4];
    double whitePoint[4];
    double black[4];
    double white[4];
    double multiply[4];
    double offset[4];
    double gamma[4];
};

class GradeKernel
    : public PixelKernel {
public:
    GradeKernel(const GradeValues& values,
                bool reverse,
                bool clampBlack,
                bool clampWhite)
        : _reverse(reverse)
        , _clampBlack(clampBlack)
        , _clampWhite(clampWhite)
    {
        for (int i = 0; i < 4; ++i) {
            const double d = values.whitePoint[i] - values.blackPoint[i];
            const double A = (d != 0) ? values.multiply[i] * (values.white[i] - values.black[i]) / d : 0;
            _channels[i].a = A;
            _channels[i].b = values.offset[i] + values.black[i] - A * values.blackPoint[i];
            _channels[i].gamma = values.gamma[i];
        }
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
                const GradeChannel& ch = _channels[bits[c]];
                double v = src[c];
                v = _reverse ? invgrade(v, ch) : grade(v, ch);
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
    static double grade(double v,
                        const GradeChannel& ch)
    {
        const double x = (ch.a * v) + ch.b;

        if (ch.gamma <= 0) {
            if (x < 1.) {
                return 0.;
            } else if (x == 1.) {
                return 1.;
            }

            return std::numeric_limits<double>::infinity();
        }
        if (ch.gamma == 1.) {
            return x;
        }
        // pow would give NaN; negatives pass through ungammaed, as in Nuke.
        if (x <= 0) {
            return x;
        }

        return std::pow(x, 1. / ch.gamma);
    }

    static double invgrade(double v,
                           const GradeChannel& ch)
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

    GradeChannel _channels[4];
    bool _reverse;
    bool _clampBlack;
    bool _clampWhite;
};

NativeImageTraits
gradeTraits()
{
    NativeImageTraits traits;

    traits.hostUnPremult = true;
    traits.defaultChannels[3] = false;

    return traits;
}

KnobColorPtr
addGradeColorKnob(KnobHolder* holder,
                  const KnobPagePtr& page,
                  const std::string& name,
                  const std::string& label,
                  const std::string& hint,
                  double defaultValue,
                  double displayMin,
                  double displayMax)
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

Grade::Grade(NodePtr node)
    : NativeImageEffect(node, gradeTraits())
{
}

Grade::~Grade()
{
}

NativePluginDescription
Grade::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_GRADE;
    desc.label = "Grade";
    desc.description = tr("Modify the tonal spread of an image from the white and black points.\n"
                          "This node can also be used to match colors of 2 images: The darkest and lightest points of "
                          "the target image are converted to black and white using the blackpoint and whitepoint values. "
                          "These 2 values are then moved to new values using the black(for dark point) and white(for white point). "
                          "You can also apply multiply/offset/gamma for other color fixing you may need.\n"
                          "Here is the formula used:\n"
                          "A = multiply * (white - black) / (whitepoint - blackpoint)\n"
                          "B = offset + black - A * blackpoint\n"
                          "output = pow(A * input + B, 1 / gamma).\n"
                          "\n"
                          "A special use for Grade is to generate a mask image with soft edges by thresholding an input image. "
                          "Set the \"Black Point\" and \"White Point\" to values just below and just above the threshold, and "
                          "check the \"Clamp Black\" and \"Clamp White\" options. If a binary mask containing only 0 and 1 is "
                          "preferred, the Clamp node can be used instead.")
                           .toStdString();
    desc.grouping = PLUGIN_GROUP_COLOR;
    desc.majorVersion = PLUGIN_MAJOR_NATRON_GRADE;
    desc.minorVersion = 0;
    desc.inputs.push_back(NativeInputDescription("Source", false, eDataKindImage));
    desc.inputs.push_back(NativeInputDescription("Mask", true, eDataKindImage, true));
    desc.outputKind = eDataKindImage;

    return desc;
}

void
Grade::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    _blackPoint = addGradeColorKnob(this, page, kGradeParamBlackPoint, tr("Black Point").toStdString(), tr("Set the color of the darkest pixels in the image.").toStdString(), 0., -1., 1.);
    _whitePoint = addGradeColorKnob(this, page, kGradeParamWhitePoint, tr("White Point").toStdString(), tr("Set the color of the brightest pixels in the image.").toStdString(), 1., 0., 4.);
    _black = addGradeColorKnob(this, page, kGradeParamBlack, tr("Lift").toStdString(), tr("Colors corresponding to the blackpoint are set to this value.").toStdString(), 0., -1., 1.);
    _white = addGradeColorKnob(this, page, kGradeParamWhite, tr("Gain").toStdString(), tr("Colors corresponding to the whitepoint are set to this value.").toStdString(), 1., 0., 4.);
    _multiply = addGradeColorKnob(this, page, kGradeParamMultiply, tr("Multiply").toStdString(), tr("Multiplies the result by this value.").toStdString(), 1., 0., 4.);
    _offset = addGradeColorKnob(this, page, kGradeParamOffset, tr("Offset").toStdString(), tr("Adds this value to the result (this applies to black and white).").toStdString(), 0., -1., 1.);
    _gamma = addGradeColorKnob(this, page, kGradeParamGamma, tr("Gamma").toStdString(), tr("Final gamma correction. Negative values are not affected by gamma.").toStdString(), 1., 0.2, 5.);

    KnobButtonPtr normalize = createKnob<KnobButton>(tr("Normalize"));
    normalize->setName(kGradeParamNormalize);
    normalize->setHintToolTip(tr("Normalize the image by setting the white point and black point from the minimum and maximum values of the input."));
    normalize->setEvaluateOnChange(false);
    page->addKnob(normalize);
    _normalize = normalize;

    KnobBoolPtr reverse = createKnob<KnobBool>(tr("Reverse"));
    reverse->setName(kGradeParamReverse);
    reverse->setHintToolTip(tr("Apply the inverse correction.  Useful to apply the inverse of a Grade downstream: copy-and-paste or clone the upstream node, and invert the downstream one."));
    reverse->setDefaultValue(false);
    reverse->setAddNewLine(false);
    page->addKnob(reverse);
    _reverse = reverse;

    KnobBoolPtr clampBlack = createKnob<KnobBool>(tr("Clamp Black"));
    clampBlack->setName(kGradeParamClampBlack);
    clampBlack->setHintToolTip(tr("All colors below 0 on output are set to 0."));
    clampBlack->setDefaultValue(true);
    clampBlack->setAddNewLine(false);
    page->addKnob(clampBlack);
    _clampBlack = clampBlack;

    KnobBoolPtr clampWhite = createKnob<KnobBool>(tr("Clamp White"));
    clampWhite->setName(kGradeParamClampWhite);
    clampWhite->setHintToolTip(tr("All colors above 1 on output are set to 1."));
    clampWhite->setDefaultValue(false);
    page->addKnob(clampWhite);
    _clampWhite = clampWhite;

    addMaskMixKnobs(page);
} // Grade::initializeKnobs

PixelKernelPtr
Grade::makeKernel(const KernelContext& context)
{
    GradeValues values;

    readColor(_blackPoint, context.time, context.view, 0., values.blackPoint);
    readColor(_whitePoint, context.time, context.view, 1., values.whitePoint);
    readColor(_black, context.time, context.view, 0., values.black);
    readColor(_white, context.time, context.view, 1., values.white);
    readColor(_multiply, context.time, context.view, 1., values.multiply);
    readColor(_offset, context.time, context.view, 0., values.offset);
    readColor(_gamma, context.time, context.view, 1., values.gamma);

    return std::make_shared<GradeKernel>(values,
                                         readBool(_reverse, context.time, context.view, false),
                                         readBool(_clampBlack, context.time, context.view, true),
                                         readBool(_clampWhite, context.time, context.view, false));
}

bool
Grade::isIdentityOp(double time,
                    const RenderScale& /*scale*/,
                    const RectI& /*roi*/,
                    ViewIdx view)
{
    if (readBool(_clampBlack, time, view, true) || readBool(_clampWhite, time, view, false)) {
        return false;
    }

    GradeValues values;
    readColor(_blackPoint, time, view, 0., values.blackPoint);
    readColor(_whitePoint, time, view, 1., values.whitePoint);
    readColor(_black, time, view, 0., values.black);
    readColor(_white, time, view, 1., values.white);
    readColor(_multiply, time, view, 1., values.multiply);
    readColor(_offset, time, view, 0., values.offset);
    readColor(_gamma, time, view, 1., values.gamma);
    for (int i = 0; i < 4; ++i) {
        if ((values.blackPoint[i] != 0.) || (values.whitePoint[i] != 1.) || (values.black[i] != 0.) || (values.white[i] != 1.) || (values.multiply[i] != 1.) || (values.offset[i] != 0.) || (values.gamma[i] != 1.)) {
            return false;
        }
    }

    return true;
}

bool
Grade::knobChanged(KnobI* k,
                   ValueChangedReasonEnum /*reason*/,
                   ViewSpec view,
                   double time,
                   bool /*originatedFromMainThread*/)
{
    KnobButtonPtr normalizeButton = _normalize.lock();

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
    KnobColorPtr blackPoint = _blackPoint.lock();
    KnobColorPtr whitePoint = _whitePoint.lock();

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
