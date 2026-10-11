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

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

struct GradeValues {
    double blackPoint[4];
    double whitePoint[4];
    double black[4];
    double white[4];
    double multiply[4];
    double offset[4];
    double gamma[4];
};

namespace {
struct GradeChannel {
    double a;
    double b;
    double gamma;
};

// The per-channel affine terms of the grade formula; the CPU kernel and the GPU parameters both
// take them from here so the two paths cannot drift.
void
gradeChannels(const GradeValues& values,
              GradeChannel out[4])
{
    for (int i = 0; i < 4; ++i) {
        const double d = values.whitePoint[i] - values.blackPoint[i];
        const double A = (d != 0) ? values.multiply[i] * (values.white[i] - values.black[i]) / d : 0;
        out[i].a = A;
        out[i].b = values.offset[i] + values.black[i] - A * values.blackPoint[i];
        out[i].gamma = values.gamma[i];
    }
}

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
        gradeChannels(values, _channels);
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

#ifdef NATRON_HAVE_GPU
// Mirrors GradeParams in grade.slang, which has no padding between members.
struct GradeGpuParams {
    float a[4];
    float b[4];
    float gamma[4];
    float invGamma[4];
    uint32_t width;
    uint32_t height;
    uint32_t nComps;
    uint32_t channelMask;
    uint32_t flags;
    uint32_t srcOffset;
    uint32_t srcStride;
    uint32_t dstOffset;
    uint32_t dstStride;
};

static_assert(sizeof(GradeGpuParams) == 100, "must match GradeParams in grade.slang");

constexpr uint32_t kGradeGpuReverse = 1;
constexpr uint32_t kGradeGpuClampBlack = 2;
constexpr uint32_t kGradeGpuClampWhite = 4;

// Float offset of the first pixel of `rect` inside view, or false when the view cannot hold it.
bool
windowOffset(const GpuImageView& view,
             const RectI& rect,
             uint32_t& offset)
{
    if (!view.buffer || (view.byteOffset % sizeof(float) != 0) || (view.rowStrideFloats <= 0) ||
        !view.bounds.contains(rect)) {
        return false;
    }
    const uint64_t floats = view.byteOffset / sizeof(float) +
                            (uint64_t)(rect.y1 - view.bounds.y1) * (uint64_t)view.rowStrideFloats +
                            (uint64_t)(rect.x1 - view.bounds.x1) * (uint64_t)view.nComps;
    if (floats > UINT32_MAX) {
        return false;
    }
    offset = (uint32_t)floats;

    return true;
}
#endif
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

    _blackPoint = addGradeColorKnob(this, page, kGradeParamBlackPoint, tr("Black Point").toStdString(), tr("Set the color of the darkest pixels in the image.").toStdString(), 0., -1., 1., eColorKnobRoleOffset);
    _whitePoint = addGradeColorKnob(this, page, kGradeParamWhitePoint, tr("White Point").toStdString(), tr("Set the color of the brightest pixels in the image.").toStdString(), 1., 0., 4., eColorKnobRoleGain);
    _black = addGradeColorKnob(this, page, kGradeParamBlack, tr("Lift").toStdString(), tr("Colors corresponding to the blackpoint are set to this value.").toStdString(), 0., -1., 1., eColorKnobRoleOffset);
    _white = addGradeColorKnob(this, page, kGradeParamWhite, tr("Gain").toStdString(), tr("Colors corresponding to the whitepoint are set to this value.").toStdString(), 1., 0., 4., eColorKnobRoleGain);
    _multiply = addGradeColorKnob(this, page, kGradeParamMultiply, tr("Multiply").toStdString(), tr("Multiplies the result by this value.").toStdString(), 1., 0., 4., eColorKnobRoleGain);
    _offset = addGradeColorKnob(this, page, kGradeParamOffset, tr("Offset").toStdString(), tr("Adds this value to the result (this applies to black and white).").toStdString(), 0., -1., 1., eColorKnobRoleOffset);
    _gamma = addGradeColorKnob(this, page, kGradeParamGamma, tr("Gamma").toStdString(), tr("Final gamma correction. Negative values are not affected by gamma.").toStdString(), 1., 0.2, 5., eColorKnobRolePower);

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

GradeValues
Grade::readValues(double time,
                  ViewIdx view) const
{
    GradeValues values;

    readColor(_blackPoint, time, view, 0., values.blackPoint);
    readColor(_whitePoint, time, view, 1., values.whitePoint);
    readColor(_black, time, view, 0., values.black);
    readColor(_white, time, view, 1., values.white);
    readColor(_multiply, time, view, 1., values.multiply);
    readColor(_offset, time, view, 0., values.offset);
    readColor(_gamma, time, view, 1., values.gamma);

    return values;
}

PixelKernelPtr
Grade::makeKernel(const KernelContext& context)
{
    return std::make_shared<GradeKernel>(readValues(context.time, context.view),
                                         readBool(_reverse, context.time, context.view, false),
                                         readBool(_clampBlack, context.time, context.view, true),
                                         readBool(_clampWhite, context.time, context.view, false));
}

bool
Grade::canRenderOnGpu(const GpuQuery& query) const
{
#ifdef NATRON_HAVE_GPU
    return query.bitDepth == eImageBitDepthFloat &&
           (query.nComps == 1 || query.nComps == 3 || query.nComps == 4) &&
           !query.hasMask && query.mix >= 1. && query.unPremultChannel == -1;
#else
    Q_UNUSED(query);

    return false;
#endif
}

StatusEnum
Grade::renderGpu(const GpuRenderArgs& args)
{
#ifdef NATRON_HAVE_GPU
    if (!canRenderOnGpu(args.query) || args.inputs.empty()) {
        return eStatusFailed;
    }
    const GpuImageView& src = args.inputs[0];
    const GpuImageView& dst = args.output;
    const int nComps = args.query.nComps;
    const RectI& rect = args.outputRect;
    if ((src.nComps != nComps) || (dst.nComps != nComps) || rect.isNull()) {
        return eStatusFailed;
    }
    GradeGpuParams p = {};
    if (!windowOffset(src, rect, p.srcOffset) || !windowOffset(dst, rect, p.dstOffset)) {
        return eStatusFailed;
    }

    const double time = args.query.time;
    const ViewIdx view = args.query.view;
    GradeChannel channels[4];
    gradeChannels(readValues(time, view), channels);
    for (int i = 0; i < 4; ++i) {
        p.a[i] = (float)channels[i].a;
        p.b[i] = (float)channels[i].b;
        p.gamma[i] = (float)channels[i].gamma;
        p.invGamma[i] = (float)(1. / channels[i].gamma);
        if (args.query.processChannels[i]) {
            p.channelMask |= 1u << i;
        }
    }
    p.width = (uint32_t)rect.width();
    p.height = (uint32_t)rect.height();
    p.nComps = (uint32_t)nComps;
    p.srcStride = (uint32_t)src.rowStrideFloats;
    p.dstStride = (uint32_t)dst.rowStrideFloats;
    if (readBool(_reverse, time, view, false)) {
        p.flags |= kGradeGpuReverse;
    }
    if (readBool(_clampBlack, time, view, true)) {
        p.flags |= kGradeGpuClampBlack;
    }
    if (readBool(_clampWhite, time, view, false)) {
        p.flags |= kGradeGpuClampWhite;
    }

    const gpu::GpuStatus status = args.context.dispatch(gpu::GpuKernelId::grade, {src.buffer, dst.buffer}, p, {p.width, p.height, 1});

    return status ? eStatusOK : eStatusFailed;
#else
    Q_UNUSED(args);

    return eStatusFailed;
#endif
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

    const GradeValues values = readValues(time, view);
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
