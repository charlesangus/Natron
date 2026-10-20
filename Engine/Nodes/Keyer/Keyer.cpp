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

#include "Keyer.h"

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <ofxNatron.h>

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/ColorMath.h"
#include "Engine/PoolParallelFor.h"
#include "Engine/RectD.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
const int kAbortCheckRows = 16;

constexpr InputSpec kKeyerInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "InM", .role = InputRole::Mask, .optional = true },
    InputSpec { .label = "OutM", .role = InputRole::Mask, .optional = true },
    InputSpec { .label = "Bg", .role = InputRole::Aux, .optional = true },
};

constexpr NodeSpec kKeyerNodeSpec = {
    .id = PLUGINID_NATRON_KEYER,
    .label = "Keyer",
    .description = "A collection of simple keyers. These work by computing a foreground key from the RGB values of the input image (see the keyerMode parameter).\n"
                   "This foreground key is is a scalar from 0 to 1. From the foreground key, a background key (or transparency) is computed.\n"
                   "The function that maps the foreground key to the background key is piecewise linear:\n"
                   "- it is 0 below A = (center+toleranceLower+softnessLower)\n"
                   "- it is linear between A = (center+toleranceLower+softnessLower) and B = (center+toleranceLower)\n"
                   " -it is 1 between B = (center+toleranceLower) and C = (center+toleranceUpper)\n"
                   "- it is linear between C = (center+toleranceUpper) and D = (center+toleranceUpper+softnessUpper)\n"
                   "- it is 0 above D = (center+toleranceUpper+softnessUpper)\n"
                   "\n"
                   "Keyer can pull mattes that correspond to the RGB channels, the luminance and the red, green and blue colors. "
                   "One very useful application for a luminance mask is to mask out a sky (almost always it is the brightest thing in a landscape).\n"
                   "Conversion from A, B, C, D to Keyer parameters is:\n"
                   "softnessLower = (A-B)\n"
                   "toleranceLower = (B-C)/2\n"
                   "center = (B+C)/2\n"
                   "toleranceUpper = (C-B)/2\n"
                   "softnessUpper = (D-C)\n"
                   "\n"
                   "See also:\n"
                   "- https://web.archive.org/web/20220524000628/http://www.opticalenquiry.com/nuke/index.php?title=The_Keyer_Nodes#Keyer\n"
                   "- http://opticalenquiry.com/nuke/index.php?title=Green_Screen\n"
                   "- https://web.archive.org/web/20211023071843/http://opticalenquiry.com/nuke/index.php?title=Keying_Tips",
    .grouping = PLUGIN_GROUP_KEYER,
    .majorVersion = PLUGIN_MAJOR_NATRON_KEYER,
    .minorVersion = 0,
    .inputs = kKeyerInputs,
};

constexpr ImagePipelineSpec kKeyerPipeline = {
    .stages = { Stage::PassThroughUnprocessed },
    .layers = LayerPolicy::color(true, true, true, true),
    .op = OpKind::Custom,
};

#ifndef M_PI
#define M_PI 3.14159265358979323846264338327950288
#endif

enum KeyerModeEnum {
    eKeyerModeLuminance = 0,
    eKeyerModeColor,
    eKeyerModeScreen,
    eKeyerModeNone
};

enum ShowEnum {
    eShowIntermediate = 0,
    eShowPremultiplied,
    eShowUnpremultiplied,
    eShowComposite
};

enum SourceAlphaEnum {
    eSourceAlphaIgnore = 0,
    eSourceAlphaInsideMask,
    eSourceAlphaNormal
};

struct KeyerSettings {
    float keyColor[3];
    KeyerModeEnum mode;
    ColorMath::LuminanceMathEnum luminanceMath;
    double softnessLower;
    double toleranceLower;
    double center;
    double toleranceUpper;
    double softnessUpper;
    double despill;
    double despillAngle;
    ShowEnum show;
    SourceAlphaEnum sourceAlpha;

    KeyerSettings()
        : mode(eKeyerModeLuminance)
        , luminanceMath(ColorMath::eLuminanceMathRec709)
        , softnessLower(-0.5)
        , toleranceLower(0.)
        , center(1.)
        , toleranceUpper(0.)
        , softnessUpper(0.5)
        , despill(1.)
        , despillAngle(120.)
        , show(eShowIntermediate)
        , sourceAlpha(eSourceAlphaIgnore)
    {
        keyColor[0] = keyColor[1] = keyColor[2] = 0.f;
    }
};

// The bounded key colour and its single-precision component sum and squared norm, as the OpenFX
// plug-in sums them.
struct KeyColorTerms {
    float key[3];
    float sum;
    float norm2;

    explicit KeyColorTerms(const float keyColor[3])
        : sum(0.f)
        , norm2(0.f)
    {
        for (int i = 0; i < 3; ++i) {
            key[i] = ColorMath::boundForKeying(keyColor[i]);
        }
        sum = key[0] + key[1] + key[2];
        norm2 = (key[0] * key[0]) + (key[1] * key[1]) + (key[2] * key[2]);
    }
};

// The slope of the despill cone's edge against the key colour axis.
double
despillClosing(double despillAngle)
{
    return std::tan((90 - 0.5 * despillAngle) * M_PI / 180.);
}

class KeyKernel {
public:
    explicit KeyKernel(const KeyerSettings& settings)
        : _keyColor()
        , _mode(settings.mode)
        , _luminanceMath(settings.luminanceMath)
        , _softnessLower(settings.softnessLower)
        , _toleranceLower(settings.toleranceLower)
        , _center(settings.center)
        , _toleranceUpper(settings.toleranceUpper)
        , _softnessUpper(settings.softnessUpper)
        , _despill(0.)
        , _despillClosing(0.)
        , _show(settings.show)
        , _sourceAlpha(settings.sourceAlpha)
        , _keyColor111(0.f)
        , _keyColorNorm2(0.f)
    {
        const KeyColorTerms terms(settings.keyColor);
        for (int i = 0; i < 3; ++i) {
            _keyColor[i] = terms.key[i];
        }
        _keyColor111 = terms.sum;
        _keyColorNorm2 = terms.norm2;
        if (_mode == eKeyerModeScreen) {
            _toleranceUpper = 1.;
            _softnessUpper = 1.;
        }
        if ((_mode == eKeyerModeScreen) || (_mode == eKeyerModeNone)) {
            _despill = settings.despill;
            _despillClosing = despillClosing(settings.despillAngle);
        }
    }

    // fg is the source pixel (r, g, b, a) or null outside the source; fgHasAlpha says whether
    // fg[3] is a real alpha. bg is always readable, zero where Bg has no pixel. out receives the
    // four output channels.
    void process(const float* fg,
                 bool fgHasAlpha,
                 const float* bg,
                 float inMaskValue,
                 float outMaskValue,
                 float* out) const
    {
        float inMask = inMaskValue;
        if ((_sourceAlpha == eSourceAlphaInsideMask) && fg && fgHasAlpha) {
            inMask = (std::max)(inMask, fg[3]);
        }
        float outMask = outMaskValue;
        double Kbg = 0.;

        inMask = (std::max)(0.f, (std::min)(inMask, 1.f));
        outMask = (std::max)(0.f, (std::min)(outMask, 1.f));

        double fgr = fg ? ColorMath::boundForKeying(fg[0]) : 0.;
        double fgg = fg ? ColorMath::boundForKeying(fg[1]) : 0.;
        double fgb = fg ? ColorMath::boundForKeying(fg[2]) : 0.;
        const double bgr = bg[0];
        const double bgg = bg[1];
        const double bgb = bg[2];

        if (!fg) {
            Kbg = 1.;
            fgr = fgg = fgb = 0.;
        } else if (outMask >= 1.) {
            Kbg = 1.;
            fgr = fgg = fgb = 0.;
        } else {
            double Kfg = 0.;
            double scalarProd = 0.;
            double norm2 = 0.;
            // The norm of the projection of the foreground orthogonal to the key colour.
            double d = 0.;
            switch (_mode) {
            case eKeyerModeLuminance:
                Kfg = ColorMath::luminance(_luminanceMath, fgr, fgg, fgb);
                break;
            case eKeyerModeColor:
                scalarProd = fgr * _keyColor[0] + fgg * _keyColor[1] + fgb * _keyColor[2];
                Kfg = (_keyColor111 == 0) ? ColorMath::luminance(_luminanceMath, fgr, fgg, fgb) : (scalarProd / _keyColor111);
                break;
            case eKeyerModeScreen:
                scalarProd = fgr * _keyColor[0] + fgg * _keyColor[1] + fgb * _keyColor[2];
                norm2 = fgr * fgr + fgg * fgg + fgb * fgb;
                d = std::sqrt((std::max)(0., norm2 - ((_keyColorNorm2 == 0) ? 0. : (scalarProd * scalarProd / _keyColorNorm2))));
                Kfg = (_keyColor111 == 0) ? ColorMath::luminance(_luminanceMath, fgr, fgg, fgb) : (scalarProd / _keyColor111);
                Kfg -= d;
                break;
            case eKeyerModeNone:
                scalarProd = fgr * _keyColor[0] + fgg * _keyColor[1] + fgb * _keyColor[2];
                norm2 = fgr * fgr + fgg * fgg + fgb * fgb;
                d = std::sqrt((std::max)(0., norm2 - ((_keyColorNorm2 == 0) ? 0. : (scalarProd * scalarProd / _keyColorNorm2))));
                break;
            }

            Kbg = (_mode == eKeyerModeNone) ? 1. : keyBackground(Kfg);
            // The outside mask has priority over the inside mask, so the inside one is applied first.
            if ((inMask > 0.) && (Kbg > 1. - inMask)) {
                Kbg = 1. - inMask;
            }
            if ((outMask > 0.) && (Kbg < outMask)) {
                Kbg = outMask;
            }

            if ((_despill > 0.) && ((_mode == eKeyerModeNone) || (_mode == eKeyerModeScreen)) && (_show != eShowIntermediate) && (_keyColorNorm2 > 0.)) {
                // Single precision, as the OpenFX plug-in takes it.
                double keyColorNorm = std::sqrt(_keyColorNorm2);
                if (scalarProd / keyColorNorm > d * _despillClosing) {
                    // Within [0, 1] only the regions that are partly background are despilled;
                    // above 1 the foreground is despilled too.
                    double maxdespill = Kbg * (std::min)(_despill, 1.) + (1 - Kbg) * (std::max)(0., _despill - 1);
                    // Subtract maxdespill key colours, without going past the despill cone.
                    double colorshift = maxdespill * (std::max)(keyColorNorm, (scalarProd / keyColorNorm - d * _despillClosing));
                    colorshift = (std::min)(colorshift, scalarProd / keyColorNorm - d * _despillClosing);
                    fgr -= colorshift * _keyColor[0] / keyColorNorm;
                    fgg -= colorshift * _keyColor[1] / keyColorNorm;
                    fgb -= colorshift * _keyColor[2] / keyColorNorm;
                }
            }

            if (_show != eShowUnpremultiplied) {
                fgr *= (1. - Kbg);
                fgg *= (1. - Kbg);
                fgb *= (1. - Kbg);
            }

            fgr = (std::max)(0., (std::min)(fgr, 1.));
            fgg = (std::max)(0., (std::min)(fgg, 1.));
            fgb = (std::max)(0., (std::min)(fgb, 1.));
        }

        const double fga = 1. - Kbg;
        const double compAlpha = ((_show == eShowComposite) && (_sourceAlpha == eSourceAlphaNormal) && fg) ? (double)(fgHasAlpha ? fg[3] : 1.f) : 1.;
        switch (_show) {
        case eShowIntermediate:
            for (int c = 0; c < 3; ++c) {
                out[c] = fg ? fg[c] : 0.f;
            }
            break;
        case eShowPremultiplied:
        case eShowUnpremultiplied:
            out[0] = (float)fgr;
            out[1] = (float)fgg;
            out[2] = (float)fgb;
            break;
        case eShowComposite:
            out[0] = (float)(compAlpha * (fgr + bgr * Kbg) + (1. - compAlpha) * bgr);
            out[1] = (float)(compAlpha * (fgg + bgg * Kbg) + (1. - compAlpha) * bgg);
            out[2] = (float)(compAlpha * (fgb + bgb * Kbg) + (1. - compAlpha) * bgb);
            break;
        }
        out[3] = (float)fga;
    }

private:
    // The trapezoid from the foreground key to the background key.
    double keyBackground(double Kfg) const
    {
        if (((_center + _toleranceLower) <= 0.) && (Kfg <= 0.)) {
            return 1.;
        } else if (Kfg < (_center + _toleranceLower + _softnessLower)) {
            return 0.;
        } else if ((Kfg < (_center + _toleranceLower)) && (_softnessLower < 0.)) {
            return (Kfg - (_center + _toleranceLower + _softnessLower)) / -_softnessLower;
        } else if (Kfg <= _center + _toleranceUpper) {
            return 1.;
        } else if ((1. <= (_center + _toleranceUpper)) && (1. <= Kfg)) {
            return 1.;
        } else if ((Kfg < (_center + _toleranceUpper + _softnessUpper)) && (_softnessUpper > 0.)) {
            return ((_center + _toleranceUpper + _softnessUpper) - Kfg) / _softnessUpper;
        }

        return 0.;
    }

    float _keyColor[3];
    KeyerModeEnum _mode;
    ColorMath::LuminanceMathEnum _luminanceMath;
    double _softnessLower;
    double _toleranceLower;
    double _center;
    double _toleranceUpper;
    double _softnessUpper;
    double _despill;
    double _despillClosing;
    ShowEnum _show;
    SourceAlphaEnum _sourceAlpha;
    float _keyColor111;
    float _keyColorNorm2;
};

// An image locked for reading by the render's calling thread. Image::getBounds() takes the
// image's lock, which a band thread must never ask for: behind a writer waiting on an image this
// render holds for reading, it blocks forever. The bounds are read once, here, instead.
struct LockedImage {
    const Image* image;
    std::shared_ptr<Image::ReadAccess> access;
    RectI bounds;

    LockedImage()
        : image(NULL)
        , access()
        , bounds()
    {
    }
};

// Fills row (four floats per pixel: r, g, b, a; `width` pixels from x0) from row y of the image,
// zero where the image has no pixel, and returns the pixel range [*xStart, *xEnd) it does have.
// An image without alpha reads as alpha one, and *hasAlpha tells whether the alpha is real.
void
readColorRow(const LockedImage* locked,
             int x0,
             int y,
             int width,
             float* row,
             int* xStart,
             int* xEnd,
             bool* hasAlpha)
{
    std::fill(row, row + (std::size_t)width * 4, 0.f);
    *xStart = x0;
    *xEnd = x0;
    *hasAlpha = false;
    if (!locked || !locked->image) {
        return;
    }
    const int nComps = (int)locked->image->getComponentsCount();
    *hasAlpha = (nComps >= 4) || (nComps == 1);
    const RectI& bounds = locked->bounds;
    if ((y < bounds.y1) || (y >= bounds.y2)) {
        return;
    }
    const int start = std::max(x0, bounds.x1);
    const int end = std::min(x0 + width, bounds.x2);
    if (start >= end) {
        return;
    }
    *xStart = start;
    *xEnd = end;
    const float* srcPix = (const float*)locked->access->pixelAt(start, y);
    float* dstPix = row + (std::size_t)(start - x0) * 4;
    for (int x = start; x < end; ++x, srcPix += nComps, dstPix += 4) {
        if (nComps >= 4) {
            dstPix[0] = srcPix[0];
            dstPix[1] = srcPix[1];
            dstPix[2] = srcPix[2];
            dstPix[3] = srcPix[3];
        } else if (nComps == 1) {
            dstPix[3] = srcPix[0];
        } else {
            dstPix[0] = srcPix[0];
            dstPix[1] = srcPix[1];
            dstPix[2] = (nComps > 2) ? srcPix[2] : 0.f;
            dstPix[3] = 1.f;
        }
    }
}

// One value per pixel of `channel` of row y, zero wherever the image has no pixel.
void
readChannelRow(const LockedImage* locked,
               int channel,
               int x0,
               int y,
               int width,
               float* row)
{
    std::fill(row, row + width, 0.f);
    if (!locked || !locked->image) {
        return;
    }
    const RectI& bounds = locked->bounds;
    if ((y < bounds.y1) || (y >= bounds.y2)) {
        return;
    }
    const int start = std::max(x0, bounds.x1);
    const int end = std::min(x0 + width, bounds.x2);
    if (start >= end) {
        return;
    }
    const int nComps = (int)locked->image->getComponentsCount();
    const float* pix = (const float*)locked->access->pixelAt(start, y);
    for (int x = start; x < end; ++x, pix += nComps) {
        row[x - x0] = pix[channel];
    }
}

bool
isFloatImage(const ImagePtr& image)
{
    return !image || (image->getBitDepth() == eImageBitDepthFloat);
}

// The bits processed in a dstNComps-channel image holding `plane`. A one-channel plane rendered
// into a wider image sits where EffectInstance's copy back reads it: alpha for a colour plane,
// channel 0 otherwise.
std::bitset<4>
processedBitsForImage(const ImageLayerDesc& plane,
                      int dstNComps,
                      const std::bitset<4>& planeBits)
{
    if ((plane.getNumComponents() != 1) || (dstNComps == 1)) {
        return planeBits;
    }
    std::bitset<4> bits;
    bits[plane.isColorLayer() ? 3 : 0] = planeBits[3];

    return bits;
}

struct KeyJob {
    ImagePtr dst;
    ImagePtr src;
    ImagePtr bg;
    std::bitset<4> channels;
    int srcIndex;
    int bgIndex;
    std::shared_ptr<Image::WriteAccess> dstAccess;

    KeyJob()
        : dst()
        , src()
        , bg()
        , channels()
        , srcIndex(-1)
        , bgIndex(-1)
    {
    }
};

struct KeyBand {
    std::size_t job;
    int y1;
    int y2;

    KeyBand(std::size_t jobIndex,
            int firstRow,
            int endRow)
        : job(jobIndex)
        , y1(firstRow)
        , y2(endRow)
    {
    }
};

KnobDoublePtr
createRangedKnob(KnobHolder* holder,
                 const KnobPagePtr& page,
                 const char* name,
                 const std::string& label,
                 const std::string& hint,
                 double defaultValue,
                 double minimum,
                 double maximum,
                 int decimals)
{
    KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(holder, label);

    knob->setName(name);
    knob->setHintToolTip(hint);
    knob->setDefaultValue(defaultValue);
    knob->setMinimum(minimum);
    knob->setMaximum(maximum);
    knob->setDisplayMinimum(minimum);
    knob->setDisplayMaximum(maximum);
    if (decimals > 0) {
        knob->setDecimals(decimals);
    }
    page->addKnob(knob);

    return knob;
}

KnobChoicePtr
makeChoiceKnob(KnobHolder* holder,
               const KnobPagePtr& page,
               const char* name,
               const std::string& label,
               const std::string& hint,
               const std::vector<ChoiceOption>& options,
               int defaultValue,
               bool animates)
{
    KnobChoicePtr knob = AppManager::createKnob<KnobChoice>(holder, label);

    knob->setName(name);
    knob->setHintToolTip(hint);
    knob->populateChoices(options);
    knob->setDefaultValue(defaultValue);
    knob->setAnimationEnabled(animates);
    page->addKnob(knob);

    return knob;
}

int
readChoice(const KnobChoiceWPtr& weak,
           double time,
           ViewIdx view,
           int defaultValue,
           int maxValue)
{
    KnobChoicePtr knob = weak.lock();
    const int value = knob ? knob->getValueAtTime(time, 0, view) : defaultValue;

    return ((value < 0) || (value > maxValue)) ? defaultValue : value;
}

double
readDouble(const KnobDoubleWPtr& weak,
           double time,
           ViewIdx view,
           double defaultValue)
{
    KnobDoublePtr knob = weak.lock();

    return knob ? knob->getValueAtTime(time, 0, view) : defaultValue;
}

bool
isEditByUser(ValueChangedReasonEnum reason)
{
    return (reason != eValueChangedReasonPluginEdited) && (reason != eValueChangedReasonTimeChanged);
}

void
setKnobVisible(const KnobIPtr& knob,
               bool visible)
{
    if (knob) {
        knob->setSecret(!visible);
        knob->setAllDimensionsEnabled(visible);
    }
}

#ifdef NATRON_HAVE_GPU
// Mirror KeyerView, KeyerParams and KeyerPush in Engine/Gpu/Kernels/keyer.slang.
struct KeyerGpuView {
    uint32_t offset;
    uint32_t stride;
    uint32_t comps;
    uint32_t channel;
    int32_t x1;
    int32_t y1;
    int32_t x2;
    int32_t y2;
};

struct KeyerGpuParams {
    KeyerGpuView src;
    KeyerGpuView inMask;
    KeyerGpuView outMask;
    KeyerGpuView bg;
    uint32_t mode;
    uint32_t show;
    uint32_t sourceAlpha;
    uint32_t flags;
    uint32_t channelMask;
    uint32_t lumMax;
    uint32_t lumAverage;
    float lumR;
    float lumG;
    float lumB;
    float lowerA;
    float lowerB;
    float negSoftnessLower;
    float upperC;
    float upperD;
    float softnessUpper;
    float unitR;
    float unitG;
    float unitB;
    float sumRatio;
    float projectionResidue;
    float despillGain;
    float keyNorm;
    float shiftR;
    float shiftG;
    float shiftB;
    float despillLower;
    float despillUpper;
    float despillClosing;
};
static_assert(sizeof(KeyerGpuParams) == 61 * 4, "KeyerGpuParams must match KeyerParams in keyer.slang");

struct KeyerGpuPush {
    int32_t outX;
    int32_t outY;
    uint32_t width;
    uint32_t height;
    uint32_t dstOffset;
    uint32_t dstStride;
};
static_assert(sizeof(KeyerGpuPush) == 24, "KeyerGpuPush must match KeyerPush in keyer.slang");

const uint32_t kKeyerFlagSrcHasAlpha = 1;
const uint32_t kKeyerFlagLowerBNonPositive = 2;
const uint32_t kKeyerFlagSoftnessLowerNegative = 4;
const uint32_t kKeyerFlagUpperCAtLeastOne = 8;
const uint32_t kKeyerFlagSoftnessUpperPositive = 16;
const uint32_t kKeyerFlagKeyFromLuminance = 32;
const uint32_t kKeyerFlagProjectOut = 64;
const uint32_t kKeyerFlagDespill = 128;

// Whether KeyKernel despills with these settings.
bool
despills(const KeyerSettings& settings,
         const KeyColorTerms& terms)
{
    return (settings.despill > 0.) && ((settings.mode == eKeyerModeNone) || (settings.mode == eKeyerModeScreen)) && (settings.show != eShowIntermediate) && (terms.norm2 > 0.f);
}

// The kernel's parameters for KeyKernel(settings), every comparison against a double threshold
// decided here or against the threshold rounded to float, where the key is continuous.
void
fillKeyerGpuSettings(const KeyerSettings& settings,
                     KeyerGpuParams* p)
{
    const KeyColorTerms terms(settings.keyColor);
    const double toleranceUpper = (settings.mode == eKeyerModeScreen) ? 1. : settings.toleranceUpper;
    const double softnessUpper = (settings.mode == eKeyerModeScreen) ? 1. : settings.softnessUpper;
    const double lowerA = settings.center + settings.toleranceLower + settings.softnessLower;
    const double lowerB = settings.center + settings.toleranceLower;
    const double upperC = settings.center + toleranceUpper;
    const double upperD = settings.center + toleranceUpper + softnessUpper;

    p->mode = (uint32_t)settings.mode;
    p->show = (uint32_t)settings.show;
    p->sourceAlpha = (uint32_t)settings.sourceAlpha;
    p->flags = 0;
    if (lowerB <= 0.) {
        p->flags |= kKeyerFlagLowerBNonPositive;
    }
    if (settings.softnessLower < 0.) {
        p->flags |= kKeyerFlagSoftnessLowerNegative;
    }
    if (1. <= upperC) {
        p->flags |= kKeyerFlagUpperCAtLeastOne;
    }
    if (softnessUpper > 0.) {
        p->flags |= kKeyerFlagSoftnessUpperPositive;
    }
    p->lowerA = (float)lowerA;
    p->lowerB = (float)lowerB;
    p->negSoftnessLower = (float)-settings.softnessLower;
    p->upperC = (float)upperC;
    p->upperD = (float)upperD;
    p->softnessUpper = (float)softnessUpper;

    p->lumMax = (settings.luminanceMath == ColorMath::eLuminanceMathMax) ? 1u : 0u;
    p->lumAverage = (settings.luminanceMath == ColorMath::eLuminanceMathAverage) ? 1u : 0u;
    p->lumR = (float)ColorMath::luminance(settings.luminanceMath, 1., 0., 0.);
    p->lumG = (float)ColorMath::luminance(settings.luminanceMath, 0., 1., 0.);
    p->lumB = (float)ColorMath::luminance(settings.luminanceMath, 0., 0., 1.);

    const double k[3] = { (double)terms.key[0], (double)terms.key[1], (double)terms.key[2] };
    const double norm2 = (k[0] * k[0]) + (k[1] * k[1]) + (k[2] * k[2]);
    const double norm = std::sqrt(norm2);
    p->unitR = (norm > 0.) ? (float)(k[0] / norm) : 0.f;
    p->unitG = (norm > 0.) ? (float)(k[1] / norm) : 0.f;
    p->unitB = (norm > 0.) ? (float)(k[2] / norm) : 0.f;
    if (terms.sum == 0) {
        p->flags |= kKeyerFlagKeyFromLuminance;
        p->sumRatio = 0.f;
    } else {
        p->sumRatio = (float)(norm / (double)terms.sum);
    }
    // KeyKernel takes nothing off the squared norm for a float norm^2 of zero, and nothing for an
    // infinite one either, since the squared scalar product over it is zero.
    const bool projectOut = (norm > 0.) && (terms.norm2 != 0) && std::isfinite(terms.norm2);
    if (projectOut) {
        p->flags |= kKeyerFlagProjectOut;
        p->projectionResidue = (float)(1. - norm2 / (double)terms.norm2);
    } else {
        p->projectionResidue = 0.f;
    }

    p->despillGain = 0.f;
    p->keyNorm = 0.f;
    p->shiftR = p->shiftG = p->shiftB = 0.f;
    p->despillLower = 0.f;
    p->despillUpper = 0.f;
    p->despillClosing = 0.f;
    if (despills(settings, terms) && projectOut) {
        p->flags |= kKeyerFlagDespill;
        const double keyColorNorm = std::sqrt((double)terms.norm2);
        p->despillGain = (float)(norm / keyColorNorm);
        p->keyNorm = (float)keyColorNorm;
        p->shiftR = (float)(k[0] / keyColorNorm);
        p->shiftG = (float)(k[1] / keyColorNorm);
        p->shiftB = (float)(k[2] / keyColorNorm);
        p->despillLower = (float)(std::min)(settings.despill, 1.);
        p->despillUpper = (float)(std::max)(0., settings.despill - 1);
        p->despillClosing = (float)despillClosing(settings.despillAngle);
    }
}

// view as the kernel addresses it, absent when it has no buffer or no pixels. False when it does
// not fit the kernel's 32-bit float indices or holds fewer than minComps components.
bool
toKeyerGpuView(const GpuImageView& view,
               int minComps,
               uint32_t channel,
               KeyerGpuView* out)
{
    *out = KeyerGpuView();
    if (!view.buffer || (view.bounds.x2 <= view.bounds.x1) || (view.bounds.y2 <= view.bounds.y1)) {
        return true;
    }
    if (((view.byteOffset % sizeof(float)) != 0) || (view.nComps < minComps) || (view.rowStrideFloats < view.bounds.width() * view.nComps)) {
        return false;
    }
    const uint64_t offset = view.byteOffset / sizeof(float);
    if ((offset + (uint64_t)view.bounds.height() * (uint64_t)view.rowStrideFloats) > UINT32_MAX) {
        return false;
    }
    out->offset = (uint32_t)offset;
    out->stride = (uint32_t)view.rowStrideFloats;
    out->comps = (uint32_t)view.nComps;
    out->channel = channel;
    out->x1 = view.bounds.x1;
    out->y1 = view.bounds.y1;
    out->x2 = view.bounds.x2;
    out->y2 = view.bounds.y2;

    return true;
}

#endif // NATRON_HAVE_GPU
} // anonymous namespace

Keyer::Keyer(NodePtr node)
    : StagedImageEffect(node)
    , _subLabel()
    , _keyColor()
    , _mode()
    , _luminanceMath()
    , _softnessLower()
    , _toleranceLower()
    , _center()
    , _toleranceUpper()
    , _softnessUpper()
    , _despill()
    , _despillAngle()
    , _show()
    , _sourceAlpha()
{
}

Keyer::~Keyer()
{
}

const NodeSpec*
Keyer::nodeSpec() const
{
    return staticNodeSpec<kKeyerNodeSpec>();
}

const ImagePipelineSpec&
Keyer::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kKeyerPipeline, kKeyerNodeSpec>();
}

std::string
Keyer::getInputHint(int inputNb) const
{
    switch (inputNb) {
    case kKeyerInputSource:
        return tr("The foreground image to key.").toStdString();
    case kKeyerInputInsideMask:
        return tr("The Inside Mask, or holdout matte, or core matte, used to confirm areas that are definitely foreground.").toStdString();
    case kKeyerInputOutsideMask:
        return tr("The Outside Mask, or garbage matte, used to remove unwanted objects (lighting rigs, and so on) from the foreground. "
                  "The Outside Mask has priority over the Inside Mask, so that areas where both are one are considered to be outside.")
            .toStdString();
    case kKeyerInputBg:
        return tr("The background image to replace the blue/green screen in the foreground.").toStdString();
    }

    return EffectInstance::getInputHint(inputNb);
}

void
Keyer::addAcceptedComponents(int inputNb,
                             std::list<ImageLayerDesc>* comps)
{
    if (isInputMask(inputNb)) {
        StagedImageEffect::addAcceptedComponents(inputNb, comps);

        return;
    }
    comps->push_back(ImageLayerDesc::getRGBAComponents());
    comps->push_back(ImageLayerDesc::getRGBComponents());
}

void
Keyer::initializeKnobs()
{
    KnobPagePtr page = createControlsPage();

    // Node.cpp shows this knob's value in parentheses next to the node's label.
    KnobStringPtr subLabel = createKnob<KnobString>(std::string(kNatronOfxParamStringSublabelName));
    subLabel->setName(kNatronOfxParamStringSublabelName);
    subLabel->setSecretByDefault(true);
    subLabel->setDefaultAllDimensionsEnabled(false);
    subLabel->setIsPersistent(false);
    subLabel->setEvaluateOnChange(false);
    subLabel->setAnimationEnabled(false);
    subLabel->setDefaultValue(std::string("Luminance"));
    page->addKnob(subLabel);
    _subLabel = subLabel;

    KnobColorPtr keyColor = createKnob<KnobColor>(tr("Key Color"), 3);
    keyColor->setName(kKeyerParamKeyColor);
    keyColor->setHintToolTip(tr("Foreground key color. foreground areas containing the key color are replaced with the background image."));
    for (int i = 0; i < 3; ++i) {
        keyColor->setDefaultValue(0., i);
        keyColor->setMinimum(-DBL_MAX, i);
        keyColor->setMaximum(DBL_MAX, i);
        keyColor->setDisplayMinimum(0., i);
        keyColor->setDisplayMaximum(1., i);
    }
    page->addKnob(keyColor);
    _keyColor = keyColor;

    std::vector<ChoiceOption> modes;
    modes.push_back(ChoiceOption("luminance", "Luminance", tr("Use the luminance for keying. The foreground key value is in luminance.").toStdString()));
    modes.push_back(ChoiceOption("color", "Color", tr("Use the color for keying. If the key color is pure green, this corresponds a green keyer, etc.").toStdString()));
    modes.push_back(ChoiceOption("screen", "Screen", tr("Use the color minus the other components for keying. If the key color is pure green, this corresponds a greenscreen, etc. When in screen mode, the upper tolerance should be set to 1.").toStdString()));
    modes.push_back(ChoiceOption("none", "None", tr("No keying, just despill color values. You can control despill areas using either set the inside mask, or use with 'Source Alpha' set to 'Add to Inside Mask'. If 'Output Mode' is set to 'Unpremultiplied', this despills the image even if no mask is present.").toStdString()));
    _mode = makeChoiceKnob(this, page, kKeyerParamMode, tr("Keyer Mode").toStdString(), tr("The operation used to compute the foreground key.").toStdString(), modes, (int)eKeyerModeLuminance, true);

    _luminanceMath = ColorMath::addLuminanceMathKnob(this, page);

    _softnessLower = createRangedKnob(this, page, kKeyerParamSoftnessLower, tr("Softness (lower)").toStdString(), tr("Width of the lower softness range [key-tolerance-softness,key-tolerance]. Background key value goes from 0 to 1 when foreground key is  over this range.").toStdString(), -0.5, -1., 0., 5);
    _toleranceLower = createRangedKnob(this, page, kKeyerParamToleranceLower, tr("Tolerance (lower)").toStdString(), tr("Width of the lower tolerance range [key-tolerance,key]. Background key value is 1 when foreground key is  over this range.").toStdString(), 0., -1., 0., 5);
    _center = createRangedKnob(this, page, kKeyerParamCenter, tr("Center").toStdString(), tr("Foreground key value forresponding to the key color, where the background key should be 1.").toStdString(), 1., 0., 1., 5);
    _toleranceUpper = createRangedKnob(this, page, kKeyerParamToleranceUpper, tr("Tolerance (upper)").toStdString(), tr("Width of the upper tolerance range [key,key+tolerance]. Background key value is 1 when foreground key is over this range. Ignored in Screen keyer mode.").toStdString(), 0., 0., 1., 5);
    _softnessUpper = createRangedKnob(this, page, kKeyerParamSoftnessUpper, tr("Softness (upper)").toStdString(), tr("Width of the upper softness range [key+tolerance,key+tolerance+softness]. Background key value goes from 1 to 0 when foreground key is  over this range. Ignored in Screen keyer mode.").toStdString(), 0.5, 0., 1., 5);

    _despill = createRangedKnob(this, page, kKeyerParamDespill, tr("Despill").toStdString(), tr("Reduces color spill on the foreground object (Screen mode only). Between 0 and 1, only mixed foreground/background regions are despilled. Above 1, foreground regions are despilled too.").toStdString(), 1., 0., 2., 0);
    _despill.lock()->setSecretByDefault(true);
    _despillAngle = createRangedKnob(this, page, kKeyerParamDespillAngle, tr("Despill Angle").toStdString(), tr("Opening of the cone centered around the keyColor where colors are despilled. A larger angle means that more colors are modified.").toStdString(), 120., 0., 180., 0);
    _despillAngle.lock()->setSecretByDefault(true);

    std::vector<ChoiceOption> shows;
    shows.push_back(ChoiceOption("intermediate", "Intermediate", tr("Color is the source color. Alpha is the foreground key. Use for multi-pass keying.").toStdString()));
    shows.push_back(ChoiceOption("premultiplied", "Premultiplied", tr("Color is the Source color after key color suppression, multiplied by alpha. Alpha is the foreground key.").toStdString()));
    shows.push_back(ChoiceOption("unpremultiplied", "Unpremultiplied", tr("Color is the Source color after key color suppression. Alpha is the foreground key.").toStdString()));
    shows.push_back(ChoiceOption("composite", "Composite", tr("Color is the composite of Source and Bg. Alpha is the foreground key.").toStdString()));
    _show = makeChoiceKnob(this, page, kKeyerParamShow, tr("Output Mode").toStdString(), tr("What image to output.").toStdString(), shows, (int)eShowIntermediate, false);

    std::vector<ChoiceOption> alphas;
    alphas.push_back(ChoiceOption("ignore", "Ignore", tr("Ignore the source alpha.").toStdString()));
    alphas.push_back(ChoiceOption("inside", "Add to Inside Mask", tr("Source alpha is added to the inside mask. Use for multi-pass keying.").toStdString()));
    alphas.push_back(ChoiceOption("normal", "Normal", tr("Foreground key is multiplied by source alpha when compositing.").toStdString()));
    _sourceAlpha = makeChoiceKnob(this, page, kKeyerParamSourceAlpha, tr("Source Alpha").toStdString(), tr("How the alpha embedded in the Source input should be used").toStdString(), alphas, (int)eSourceAlphaIgnore, true);
}

void
Keyer::refreshModeDependents(double time,
                             ViewIdx view)
{
    const KeyerModeEnum mode = (KeyerModeEnum)readChoice(_mode, time, view, (int)eKeyerModeLuminance, (int)eKeyerModeNone);
    const bool none = (mode == eKeyerModeNone);
    const bool screen = (mode == eKeyerModeScreen);

    setKnobVisible(_luminanceMath.lock(), mode == eKeyerModeLuminance);
    setKnobVisible(_softnessLower.lock(), !none);
    setKnobVisible(_toleranceLower.lock(), !none);
    setKnobVisible(_center.lock(), !none);
    setKnobVisible(_toleranceUpper.lock(), !none && !screen);
    setKnobVisible(_softnessUpper.lock(), !none && !screen);
    setKnobVisible(_despill.lock(), none || screen);
    setKnobVisible(_despillAngle.lock(), none || screen);

    KnobStringPtr subLabel = _subLabel.lock();
    KnobChoicePtr modeKnob = _mode.lock();
    if (subLabel && modeKnob) {
        subLabel->setValue(modeKnob->getEntry((int)mode).label);
    }
}

void
Keyer::setThresholdsFromKeyColor(double time,
                                 ViewIdx view)
{
    KnobColorPtr keyColor = _keyColor.lock();
    KnobDoublePtr softnessLower = _softnessLower.lock();
    KnobDoublePtr toleranceLower = _toleranceLower.lock();
    KnobDoublePtr center = _center.lock();
    KnobDoublePtr toleranceUpper = _toleranceUpper.lock();
    KnobDoublePtr softnessUpper = _softnessUpper.lock();

    if (!keyColor || !softnessLower || !toleranceLower || !center || !toleranceUpper || !softnessUpper) {
        return;
    }
    const double r = keyColor->getValueAtTime(time, 0, view);
    const double g = keyColor->getValueAtTime(time, 1, view);
    const double b = keyColor->getValueAtTime(time, 2, view);
    const KeyerModeEnum mode = (KeyerModeEnum)readChoice(_mode, time, view, (int)eKeyerModeLuminance, (int)eKeyerModeNone);
    double l;

    switch (mode) {
    case eKeyerModeLuminance: {
        const ColorMath::LuminanceMathEnum math = (ColorMath::LuminanceMathEnum)readChoice(_luminanceMath, time, view, (int)ColorMath::eLuminanceMathRec709, (int)ColorMath::eLuminanceMathMax);
        l = ColorMath::luminance(math, r, g, b);
        break;
    }
    case eKeyerModeColor:
    case eKeyerModeScreen: {
        // The key colour's own foreground key, the scalar product of RGB with it over its component sum.
        const double keyColor111 = r + g + b;
        const double keyColorNorm2 = (r * r) + (g * g) + (b * b);
        l = (keyColor111 == 0.) ? 0. : (keyColorNorm2 / keyColor111);
        break;
    }
    case eKeyerModeNone:
        return;
    }
    softnessLower->setValue(-l);
    toleranceLower->setValue(0.);
    center->setValue(l);
    toleranceUpper->setValue(0.);
    softnessUpper->setValue(1. - l);
}

bool
Keyer::knobChanged(KnobI* k,
                   ValueChangedReasonEnum reason,
                   ViewSpec view,
                   double time,
                   bool /*originatedFromMainThread*/)
{
    KnobColorPtr keyColor = _keyColor.lock();
    KnobChoicePtr mode = _mode.lock();
    const ViewIdx viewIdx = (view.isAll() || view.isCurrent()) ? ViewIdx(0) : ViewIdx(view);

    if (keyColor && (k == keyColor.get())) {
        if (isEditByUser(reason)) {
            setThresholdsFromKeyColor(time, viewIdx);
        }

        return true;
    }
    if (mode && (k == mode.get())) {
        refreshModeDependents(time, viewIdx);
        if (isEditByUser(reason)) {
            setThresholdsFromKeyColor(time, viewIdx);
        }

        return true;
    }

    return false;
}

void
Keyer::onKnobsLoaded()
{
    refreshModeDependents(getCurrentTime(), ViewIdx(0));
}

StatusEnum
Keyer::getPreferredMetadata(NodeMetadata& metadata)
{
    // The output is RGBA whatever the source: the foreground key is its alpha.
    metadata.setNComps(-1, 4);

    return eStatusOK;
}

StatusEnum
Keyer::getRegionOfDefinition(U64 /*hash*/,
                             double time,
                             const RenderScale& scale,
                             ViewIdx view,
                             RectD* rod)
{
    // The OpenFX default for its general context is the union of the non-optional clips, which
    // is the source alone.
    EffectInstancePtr input = getInput(kKeyerInputSource);

    if (!input) {
        return eStatusReplyDefault;
    }
    const RenderScale inputScale = input->supportsRenderScale() ? scale : RenderScale::identity;
    bool isProjectFormat = false;

    return (input->getRegionOfDefinition_public(input->getRenderHash(), time, inputScale, view, rod, &isProjectFormat) == eStatusFailed) ? eStatusFailed : eStatusOK;
}

bool
Keyer::isOpIdentity(double /*time*/,
                    const RenderScale& /*scale*/,
                    const RectI& /*roi*/,
                    ViewIdx /*view*/)
{
    return false;
}

bool
Keyer::isKeyMaskUsed(int inputNb) const
{
    return getInput(inputNb) && isMaskEnabled(inputNb);
}

struct Keyer::Settings
    : public KeyerSettings {
};

void
Keyer::readSettings(double time,
                    ViewIdx view,
                    Settings* settings) const
{
    KnobColorPtr keyColor = _keyColor.lock();
    if (keyColor) {
        for (int i = 0; i < 3; ++i) {
            settings->keyColor[i] = (float)keyColor->getValueAtTime(time, i, view);
        }
    }
    settings->mode = (KeyerModeEnum)readChoice(_mode, time, view, (int)eKeyerModeLuminance, (int)eKeyerModeNone);
    settings->luminanceMath = (ColorMath::LuminanceMathEnum)readChoice(_luminanceMath, time, view, (int)ColorMath::eLuminanceMathRec709, (int)ColorMath::eLuminanceMathMax);
    settings->softnessLower = readDouble(_softnessLower, time, view, -0.5);
    settings->toleranceLower = readDouble(_toleranceLower, time, view, 0.);
    settings->center = readDouble(_center, time, view, 1.);
    settings->toleranceUpper = readDouble(_toleranceUpper, time, view, 0.);
    settings->softnessUpper = readDouble(_softnessUpper, time, view, 0.5);
    settings->despill = readDouble(_despill, time, view, 1.);
    settings->despillAngle = readDouble(_despillAngle, time, view, 120.);
    settings->show = (ShowEnum)readChoice(_show, time, view, (int)eShowIntermediate, (int)eShowComposite);
    settings->sourceAlpha = (SourceAlphaEnum)readChoice(_sourceAlpha, time, view, (int)eSourceAlphaIgnore, (int)eSourceAlphaNormal);
}

StatusEnum
Keyer::renderOp(const RenderActionArgs& args,
                const StagePlan& /*plan*/)
{
    Settings settings;
    readSettings(args.time, args.view, &settings);
    const KeyKernel kernel(settings);
    // Every image is fetched before any is locked: fetching renders upstream, which may write into a
    // cached image this render would otherwise already hold a read lock on.
    const int maskInputs[2] = { kKeyerInputInsideMask, kKeyerInputOutsideMask };
    ImagePtr masks[2];
    int maskChannels[2] = { -1, -1 };
    for (int k = 0; k < 2; ++k) {
        if (!isKeyMaskUsed(maskInputs[k])) {
            continue;
        }
        ImageLayerDesc maskLayer;
        if (resolveInputPlaneForRender(maskInputs[k], args.time, args.view, &maskLayer, &maskChannels[k]) && (maskChannels[k] >= 0)) {
            RectI maskRoI;
            masks[k] = getImage(maskInputs[k], args.time, args.mappedScale, args.view, NULL, &maskLayer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &maskRoI);
        }
        if (masks[k] && (maskChannels[k] >= (int)masks[k]->getComponentsCount())) {
            masks[k].reset();
        }
        if (!isFloatImage(masks[k])) {
            return eStatusFailed;
        }
    }

    const std::function<ImagePtr(int)> fetch = [&](int inputNb) {
        ImageLayerDesc layer;
        ImagePtr image;

        if (getInput(inputNb) && resolveInputPlaneForRender(inputNb, args.time, args.view, &layer, NULL)) {
            RectI roiPixel;
            image = getImage(inputNb, args.time, args.mappedScale, args.view, NULL, &layer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &roiPixel);
        }

        return image;
    };

    std::vector<KeyJob> jobs;
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        KeyJob job;
        job.dst = it->second;
        if (job.dst) {
            job.channels = processedBitsForImage(it->first, (int)job.dst->getComponentsCount(), args.processChannels);
            job.src = fetch(kKeyerInputSource);
            job.bg = fetch(kKeyerInputBg);
            if (!isFloatImage(job.dst) || !isFloatImage(job.src) || !isFloatImage(job.bg)) {
                return eStatusFailed;
            }
        }
        jobs.push_back(job);
    }

    const RectI& roi = args.roi;
    const int width = roi.width();
    if ((width <= 0) || (roi.height() <= 0)) {
        return eStatusOK;
    }

    // Images are locked here, on the calling thread, for the whole render; the band threads only
    // compute pixel addresses through these accesses. An image reached through two inputs is
    // locked once.
    std::vector<LockedImage> locked;
    const std::function<int(const ImagePtr&)> lockImage = [&](const ImagePtr& image) {
        if (!image) {
            return -1;
        }
        for (std::size_t i = 0; i < locked.size(); ++i) {
            if (locked[i].image == image.get()) {
                return (int)i;
            }
        }
        LockedImage entry;
        entry.image = image.get();
        entry.bounds = image->getBounds();
        entry.access = std::make_shared<Image::ReadAccess>(image.get());
        locked.push_back(entry);

        return (int)locked.size() - 1;
    };
    int maskIndices[2] = { lockImage(masks[0]), lockImage(masks[1]) };

    const int nThreads = appPTR->getNCPUsAvailableForEffect();
    std::vector<RectI> bandRects;
    makeStageRowBands(roi, nThreads, &bandRects);
    std::vector<KeyBand> bands;
    for (std::size_t j = 0; j < jobs.size(); ++j) {
        KeyJob& job = jobs[j];
        if (!job.dst) {
            continue;
        }
        job.srcIndex = lockImage(job.src);
        job.bgIndex = lockImage(job.bg);
        job.dstAccess = std::make_shared<Image::WriteAccess>(job.dst.get());
        for (std::size_t b = 0; b < bandRects.size(); ++b) {
            bands.push_back(KeyBand(j, bandRects[b].y1, bandRects[b].y2));
        }
    }

    RenderCancellation cancel(this);

    const std::function<void(int)> renderBand = [&](int bandIndex) {
        const KeyBand& band = bands[bandIndex];
        const KeyJob& job = jobs[band.job];
        const int nComps = (int)job.dst->getComponentsCount();
        std::vector<float> srcRow((std::size_t)width * 4);
        std::vector<float> bgRow((std::size_t)width * 4);
        std::vector<float> inMaskRow(width);
        std::vector<float> outMaskRow(width);
        const LockedImage* srcImage = (job.srcIndex >= 0) ? &locked[job.srcIndex] : NULL;
        const LockedImage* bgImage = (job.bgIndex >= 0) ? &locked[job.bgIndex] : NULL;

        for (int y = band.y1; y < band.y2; ++y) {
            if ((((y - band.y1) % kAbortCheckRows) == 0) && cancel.check()) {
                return;
            }

            int srcStart, srcEnd, bgStart, bgEnd;
            bool srcHasAlpha, bgHasAlpha;
            readColorRow(srcImage, roi.x1, y, width, &srcRow[0], &srcStart, &srcEnd, &srcHasAlpha);
            readColorRow(bgImage, roi.x1, y, width, &bgRow[0], &bgStart, &bgEnd, &bgHasAlpha);
            static_cast<void>(bgHasAlpha);
            readChannelRow((maskIndices[0] >= 0) ? &locked[maskIndices[0]] : NULL, maskChannels[0], roi.x1, y, width, &inMaskRow[0]);
            readChannelRow((maskIndices[1] >= 0) ? &locked[maskIndices[1]] : NULL, maskChannels[1], roi.x1, y, width, &outMaskRow[0]);

            float* dstPix = (float*)job.dstAccess->pixelAt(roi.x1, y);
            if (!dstPix) {
                continue;
            }
            for (int i = 0; i < width; ++i, dstPix += nComps) {
                const int x = roi.x1 + i;
                const bool hasSrc = (x >= srcStart) && (x < srcEnd);
                const float* srcPix = &srcRow[(std::size_t)i * 4];
                float out[4];
                kernel.process(hasSrc ? srcPix : NULL, srcHasAlpha, &bgRow[(std::size_t)i * 4], inMaskRow[i], outMaskRow[i], out);
                for (int c = 0; c < nComps; ++c) {
                    const int bit = (c < 4) ? pixelKernelChannelBit(nComps, c) : -1;
                    if ((bit < 0) || !job.channels[bit]) {
                        dstPix[c] = ((bit >= 0) && hasSrc && ((bit < 3) || srcHasAlpha)) ? srcPix[bit] : 0.f;
                        continue;
                    }
                    dstPix[c] = out[bit];
                }
            }
        }
    };
    parallelForCancellable((int)bands.size(), nThreads, cancel, renderBand);

    return eStatusOK;
} // Keyer::renderOp

int
Keyer::getResolvedInputNComps(int inputNb,
                              double time,
                              ViewIdx view) const
{
    if (!getInput(inputNb)) {
        return 0;
    }
    ImageLayerDesc layer;
    // Resolving only reads the node's selectors and its inputs' layers.
    Keyer* self = const_cast<Keyer*>(this);
    if (!self->resolveInputPlaneForRender(inputNb, time, view, &layer, NULL)) {
        return 0;
    }

    return layer.getNumComponents();
}

bool
Keyer::canRenderOpOnGpu(const GpuQuery& query) const
{
#ifdef NATRON_HAVE_GPU
    if ((query.bitDepth != eImageBitDepthFloat) || (query.nComps != 4)) {
        return false;
    }
    // renderOp() reads a one- or two-channel image in its own layout, which the region would hand
    // over converted to RGBA.
    const int inputs[2] = { kKeyerInputSource, kKeyerInputBg };
    for (int i = 0; i < 2; ++i) {
        const int nComps = getResolvedInputNComps(inputs[i], query.time, query.view);
        if ((nComps != 0) && (nComps != 3) && (nComps != 4)) {
            return false;
        }
    }
    Settings settings;
    readSettings(query.time, query.view, &settings);
    const KeyColorTerms terms(settings.keyColor);

    // An infinite float norm makes renderOp() despill by the distance to the axis alone.
    return !despills(settings, terms) || std::isfinite(terms.norm2);
#else
    Q_UNUSED(query);

    return false;
#endif
}

bool
Keyer::getOpGpuInputLayer(int inputNb,
                          const ImageLayerDesc& plane,
                          double time,
                          ViewIdx view,
                          ImageLayerDesc* layer) const
{
    stageGpuInputLayer(*this, inputNb, plane, time, view, layer);

    return true;
}

StatusEnum
Keyer::renderOpGpu(const GpuRenderArgs& args)
{
#ifdef NATRON_HAVE_GPU
    const GpuQuery& query = args.query;
    const RectI& rect = args.outputRect;
    const GpuImageView& dst = args.output;
    if (!canRenderOpOnGpu(query) || rect.isNull() || !PassExec::isUsableGpuView(dst, 4) || !dst.bounds.contains(rect)) {
        return eStatusFailed;
    }
    const double time = query.time;
    const ViewIdx view = query.view;
    const RenderScale& scale = query.mappedScale;

    Settings settings;
    readSettings(time, view, &settings);
    KeyerGpuParams params = KeyerGpuParams();
    fillKeyerGpuSettings(settings, &params);
    std::bitset<4> channels;
    for (int i = 0; i < 4; ++i) {
        channels[i] = query.processChannels[i];
    }
    params.channelMask = (uint32_t)channels.to_ulong();

    // renderOp() reads each input over the image it fetched, which ends at the input's region of
    // definition; a view may reach past it.
    const std::function<GpuImageView(int)> validView = [&](int inputNb) {
        GpuImageView none;
        EffectInstancePtr input = getInput(inputNb);
        if (!input || (inputNb >= (int)args.inputs.size()) || !args.inputs[inputNb].buffer) {
            return none;
        }
        const GpuImageView& v = args.inputs[inputNb];
        const RenderScale inputScale = input->supportsRenderScale() ? scale : RenderScale::identity;
        RectD rod;
        bool isProjectFormat = false;
        if (input->getRegionOfDefinition_public(input->getRenderHash(), time, inputScale, view, &rod, &isProjectFormat) == eStatusFailed) {
            return none;
        }

        return PassExec::cropGpuView(v, v.bounds.intersect(rod.toPixelEnclosing(scale, getAspectRatio(inputNb))));
    };

    const int srcNComps = getResolvedInputNComps(kKeyerInputSource, time, view);
    const bool srcHasAlpha = (srcNComps >= 4);
    if (srcHasAlpha) {
        params.flags |= kKeyerFlagSrcHasAlpha;
    }
    if (srcNComps > 0) {
        if (!toKeyerGpuView(validView(kKeyerInputSource), srcHasAlpha ? 4 : 3, 0, &params.src)) {
            return eStatusFailed;
        }
    }
    if (getResolvedInputNComps(kKeyerInputBg, time, view) > 0) {
        if (!toKeyerGpuView(validView(kKeyerInputBg), 3, 0, &params.bg)) {
            return eStatusFailed;
        }
    }
    const int maskInputs[2] = { kKeyerInputInsideMask, kKeyerInputOutsideMask };
    KeyerGpuView* maskViews[2] = { &params.inMask, &params.outMask };
    for (int k = 0; k < 2; ++k) {
        if (!isKeyMaskUsed(maskInputs[k])) {
            continue;
        }
        ImageLayerDesc maskLayer;
        int maskChannel = -1;
        // renderOp() reads zero for a mask it cannot fetch, or whose channel its image lacks.
        if (!resolveInputPlaneForRender(maskInputs[k], time, view, &maskLayer, &maskChannel) || (maskChannel < 0) || (maskChannel >= maskLayer.getNumComponents())) {
            continue;
        }
        const GpuImageView maskView = validView(maskInputs[k]);
        if (!maskView.buffer) {
            continue;
        }
        const int component = stageGpuMaskViewComponent(maskLayer, maskChannel, maskView.nComps);
        if ((component < 0) || (component >= maskView.nComps) || !toKeyerGpuView(maskView, 1, (uint32_t)component, maskViews[k])) {
            return eStatusFailed;
        }
    }

    const uint64_t dstOffset = dst.byteOffset / sizeof(float) + (uint64_t)(rect.y1 - dst.bounds.y1) * (uint64_t)dst.rowStrideFloats + (uint64_t)(rect.x1 - dst.bounds.x1) * 4;
    if ((dst.byteOffset / sizeof(float) + (uint64_t)dst.bounds.height() * (uint64_t)dst.rowStrideFloats) > UINT32_MAX) {
        return eStatusFailed;
    }
    KeyerGpuPush push;
    push.outX = rect.x1;
    push.outY = rect.y1;
    push.width = (uint32_t)rect.width();
    push.height = (uint32_t)rect.height();
    push.dstOffset = (uint32_t)dstOffset;
    push.dstStride = (uint32_t)dst.rowStrideFloats;

    gpu::GpuRecordContext& ctx = args.context;
    gpu::GpuBuffer* paramsBuffer = nullptr;
    if (!ctx.scratch(sizeof(params), paramsBuffer) || !paramsBuffer || !ctx.updateBuffer(*paramsBuffer, 0, std::as_bytes(std::span<const KeyerGpuParams>(&params, 1)))) {
        return eStatusFailed;
    }
    // An absent input binds the parameter block, which the kernel never reads through it.
    const std::function<const gpu::GpuBuffer*(const KeyerGpuView&, int)> bufferOf = [&](const KeyerGpuView& v, int inputNb) -> const gpu::GpuBuffer* {
        return (v.comps != 0) ? args.inputs[inputNb].buffer : paramsBuffer;
    };

    return ctx.dispatch(gpu::GpuKernelId::keyer,
                        { bufferOf(params.src, kKeyerInputSource), bufferOf(params.inMask, kKeyerInputInsideMask), bufferOf(params.outMask, kKeyerInputOutsideMask),
                          bufferOf(params.bg, kKeyerInputBg), dst.buffer, paramsBuffer },
                        push, { push.width, push.height, 1 })
        ? eStatusOK
        : eStatusFailed;
#else
    Q_UNUSED(args);

    return eStatusFailed;
#endif
} // Keyer::renderOpGpu

NATRON_NAMESPACE_EXIT
