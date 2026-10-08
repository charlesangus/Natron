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

#include "ChromaKeyer.h"

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/ColorMath.h"
#include "Engine/PoolParallelFor.h"
#include "Engine/RectD.h"

NATRON_NAMESPACE_ENTER

namespace {
const int kAbortCheckRows = 16;

#ifndef M_PI
#define M_PI 3.14159265358979323846264338327950288
#endif

enum ColorspaceEnum {
    eColorspaceCcir601 = 0,
    eColorspaceRec709,
    eColorspaceRec2020
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

typedef void (*ToYPbPrFunc)(float r, float g, float b, float* y, float* pb, float* pr);
typedef void (*ToRGBFunc)(float y, float pb, float pr, float* r, float* g, float* b);

// The Y'PbPr conversions of openfx-misc's ofxsLut.cpp, in single precision as there.
#define CHROMAKEYER_YPBPR(NAME, KR, KB)                                              \
    void rgbToYPbPr##NAME(float r, float g, float b, float* y, float* pb, float* pr) \
    {                                                                                \
        const float Kr = KR;                                                         \
        const float Kb = KB;                                                         \
        *y = Kr * r + (1 - Kr - Kb) * g + Kb * b;                                    \
        *pb = (b - *y) / (2 * (1 - Kb));                                             \
        *pr = (r - *y) / (2 * (1 - Kr));                                             \
    }                                                                                \
    void yPbPrToRGB##NAME(float y, float pb, float pr, float* r, float* g, float* b) \
    {                                                                                \
        const float Kr = KR;                                                         \
        const float Kb = KB;                                                         \
        *b = pb * (2 * (1 - Kb)) + y;                                                \
        *r = pr * (2 * (1 - Kr)) + y;                                                \
        *g = (y - Kr * *r - Kb * *b) / (1 - Kr - Kb);                                \
    }

CHROMAKEYER_YPBPR(601, 0.299f, 0.114f)
CHROMAKEYER_YPBPR(709, 0.2126390058f, 0.07219231534f)
CHROMAKEYER_YPBPR(2020, 0.2627f, 0.0593f)

#undef CHROMAKEYER_YPBPR

struct KeyerSettings {
    float keyColor[3];
    ColorspaceEnum colorspace;
    bool linear;
    double acceptanceAngle;
    double suppressionAngle;
    double keyLift;
    double keyGain;
    ShowEnum show;
    SourceAlphaEnum sourceAlpha;

    KeyerSettings()
        : colorspace(eColorspaceRec709)
        , linear(false)
        , acceptanceAngle(120.)
        , suppressionAngle(40.)
        , keyLift(0.)
        , keyGain(1.)
        , show(eShowComposite)
        , sourceAlpha(eSourceAlphaIgnore)
    {
        keyColor[0] = keyColor[1] = keyColor[2] = 0.f;
    }
};

class ChromaKeyKernel {
public:
    explicit ChromaKeyKernel(const KeyerSettings& settings)
        : _toYPbPr(NULL)
        , _toRGB(NULL)
        , _linear(settings.linear)
        , _acceptanceAngle(settings.acceptanceAngle)
        , _tanAcceptanceAngle2(0.)
        , _suppressionAngle(settings.suppressionAngle)
        , _tanSuppressionAngle2(0.)
        , _keyLift(settings.keyLift)
        , _keyGain(settings.keyGain)
        , _show(settings.show)
        , _sourceAlpha(settings.sourceAlpha)
        , _sinKey(0.)
        , _cosKey(0.)
        , _xKey(0.)
        , _ys(0.)
    {
        switch (settings.colorspace) {
        case eColorspaceCcir601:
            _toYPbPr = rgbToYPbPr601;
            _toRGB = yPbPrToRGB601;
            break;
        case eColorspaceRec709:
            _toYPbPr = rgbToYPbPr709;
            _toRGB = yPbPrToRGB709;
            break;
        case eColorspaceRec2020:
            _toYPbPr = rgbToYPbPr2020;
            _toRGB = yPbPrToRGB2020;
            break;
        }

        float y, cb, cr;
        const float keyR = ColorMath::boundForKeying(settings.keyColor[0]);
        const float keyG = ColorMath::boundForKeying(settings.keyColor[1]);
        const float keyB = ColorMath::boundForKeying(settings.keyColor[2]);
        const float r = _linear ? keyR : ColorMath::toRec709(keyR);
        const float g = _linear ? keyG : ColorMath::toRec709(keyG);
        const float b = _linear ? keyB : ColorMath::toRec709(keyB);
        _toYPbPr(r, g, b, &y, &cb, &cr);
        if ((cb == 0.) && (cr == 0.)) {
            // A key without chrominance has no direction: default to a blue screen.
            cb = 1.;
        }
        _xKey = 2 * std::sqrt(cb * cb + cr * cr);
        if (!(_xKey > 0.) || !std::isfinite(_xKey)) {
            // The squares underflowed to zero or overflowed in single precision, which would
            // leave the key direction 0/0 or inf/inf.
            _xKey = 2 * std::sqrt((double)cb * cb + (double)cr * cr);
        }
        _cosKey = 2 * cb / _xKey;
        _sinKey = 2 * cr / _xKey;
        _ys = _xKey == 0. ? 0. : y / _xKey;
        if (_acceptanceAngle < 180.) {
            _tanAcceptanceAngle2 = std::tan((_acceptanceAngle / 2) * M_PI / 180);
        }
        if (_suppressionAngle < 180.) {
            _tanSuppressionAngle2 = std::tan((_suppressionAngle / 2) * M_PI / 180);
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
        float Kbg = 0.f;

        inMask = (std::max)(0.f, (std::min)(inMask, 1.f));
        outMask = (std::max)(0.f, (std::min)(outMask, 1.f));

        float fgr = fg ? ColorMath::boundForKeying(fg[0]) : 0.f;
        float fgg = fg ? ColorMath::boundForKeying(fg[1]) : 0.f;
        float fgb = fg ? ColorMath::boundForKeying(fg[2]) : 0.f;
        const float bgr = bg[0];
        const float bgg = bg[1];
        const float bgb = bg[2];

        if (!fg) {
            Kbg = 1.f;
            fgr = fgg = fgb = 0.;
        } else if (outMask >= 1.) {
            Kbg = 1.f;
            fgr = fgg = fgb = 0.;
        } else {
            if (!_linear) {
                fgr = ColorMath::toRec709(fgr);
                fgg = ColorMath::toRec709(fgg);
                fgb = ColorMath::toRec709(fgb);
            }
            float fgy, fgcb, fgcr;
            _toYPbPr(fgr, fgg, fgb, &fgy, &fgcb, &fgcr);

            // Rotate the (Cb, Cr) plane, normalised to [-1, 1], so the key colour is on the X axis.
            double fgcbp = fgcb * 2;
            double fgcrp = fgcr * 2;
            double fgx = _cosKey * fgcbp + _sinKey * fgcrp;
            double fgz = -_sinKey * fgcbp + _cosKey * fgcrp;

            double Kfg;
            if ((fgx <= 0) || ((_acceptanceAngle >= 180.) && (fgx >= 0)) || (std::abs(fgz) / fgx > _tanAcceptanceAngle2)) {
                Kfg = 0.;
            } else {
                Kfg = _tanAcceptanceAngle2 > 0 ? (fgx - std::abs(fgz) / _tanAcceptanceAngle2) : 0.;
            }
            double fgx_scaled = fgx;

            // The outside mask has priority over the inside mask, so the inside one is applied first.
            double Kfg_new = Kfg;
            if ((inMask > 0.) && (Kfg > 1. - inMask)) {
                Kfg_new = 1. - inMask;
            }
            if ((outMask > 0.) && (Kfg < outMask)) {
                Kfg_new = outMask;
            }
            if (Kfg != 0.) {
                fgx_scaled = Kfg_new + std::abs(fgz) / _tanAcceptanceAngle2;
            }
            Kfg = Kfg_new;

            if (_show != eShowIntermediate) {
                // X and Z are built from twice the chrominance, so removing Kfg from X removes
                // Kfg / 2 from (Cb, Cr).
                if ((fgx_scaled > 0) && ((_suppressionAngle <= 0.) || (_suppressionAngle >= 180.) || (fgx_scaled - std::abs(fgz) / _tanSuppressionAngle2 > 0.))) {
                    fgcb = 0;
                    fgcr = 0;
                } else {
                    fgcb = static_cast<float>(fgcb - Kfg * _cosKey / 2);
                    fgcr = static_cast<float>(fgcr - Kfg * _sinKey / 2);
                    fgcb = (std::max)(-0.5f, (std::min)(fgcb, 0.5f));
                    fgcr = (std::max)(-0.5f, (std::min)(fgcr, 0.5f));
                }

                // _ys is such that the luminance of the key colour goes to zero.
                fgy = static_cast<float>(fgy - _ys * Kfg);
                if (fgy < 0) {
                    fgy = fgr = fgg = fgb = 0;
                } else {
                    _toRGB(fgy, fgcb, fgcr, &fgr, &fgg, &fgb);
                    fgr = (std::max)(0.f, (std::min)(fgr, 1.f));
                    fgg = (std::max)(0.f, (std::min)(fgg, 1.f));
                    fgb = (std::max)(0.f, (std::min)(fgb, 1.f));
                    if (!_linear) {
                        fgr = ColorMath::fromRec709(fgr);
                        fgg = ColorMath::fromRec709(fgg);
                        fgb = ColorMath::fromRec709(fgb);
                    }
                }
            }

            // The gain scales the key colour's distance to the full key, and the lift is the
            // fraction of that distance where the linear ramp starts.
            if (_keyGain <= 0.) {
                Kbg = (Kfg > 0.) ? 1.f : 0.f;
            } else if (_keyLift >= 1.) {
                Kbg = (Kfg >= _keyGain * _xKey) ? 1.f : 0.f;
            } else {
                Kbg = (float)((Kfg / (_keyGain * _xKey) - _keyLift) / (1. - _keyLift));
            }
            if (Kbg > 1.) {
                Kbg = 1.f;
            } else if (Kbg < 0.) {
                Kbg = 0.f;
            }
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
            out[0] = fgr;
            out[1] = fgg;
            out[2] = fgb;
            break;
        case eShowUnpremultiplied:
            if (fga == 0.) {
                out[0] = out[1] = out[2] = 1.f;
            } else {
                out[0] = (float)(fgr / fga);
                out[1] = (float)(fgg / fga);
                out[2] = (float)(fgb / fga);
            }
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
    ToYPbPrFunc _toYPbPr;
    ToRGBFunc _toRGB;
    bool _linear;
    double _acceptanceAngle;
    double _tanAcceptanceAngle2;
    double _suppressionAngle;
    double _tanSuppressionAngle2;
    double _keyLift;
    double _keyGain;
    ShowEnum _show;
    SourceAlphaEnum _sourceAlpha;
    double _sinKey;
    double _cosKey;
    double _xKey;
    double _ys;
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

KnobColorPtr
createKeyColorKnob(KnobHolder* holder,
                   const KnobPagePtr& page,
                   const std::string& label,
                   const std::string& hint)
{
    KnobColorPtr knob = AppManager::createKnob<KnobColor>(holder, label, 3);

    knob->setName(kChromaKeyerParamKeyColor);
    knob->setHintToolTip(hint);
    for (int i = 0; i < 3; ++i) {
        knob->setDefaultValue(0., i);
        knob->setMinimum(-DBL_MAX, i);
        knob->setMaximum(DBL_MAX, i);
        knob->setDisplayMinimum(0., i);
        knob->setDisplayMaximum(1., i);
    }
    page->addKnob(knob);

    return knob;
}

KnobDoublePtr
makeDoubleKnob(KnobHolder* holder,
               const KnobPagePtr& page,
               const char* name,
               const std::string& label,
               const std::string& hint,
               double defaultValue,
               double maximum,
               double displayMaximum,
               bool fine)
{
    KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(holder, label);

    knob->setName(name);
    knob->setHintToolTip(hint);
    knob->setDefaultValue(defaultValue);
    knob->setMinimum(0.);
    knob->setMaximum(maximum);
    knob->setDisplayMinimum(0.);
    knob->setDisplayMaximum(displayMaximum);
    if (fine) {
        knob->setIncrement(0.01);
        knob->setDecimals(4);
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
} // anonymous namespace

ChromaKeyer::ChromaKeyer(NodePtr node)
    : NativeImageEffect(node)
    , _keyColor()
    , _colorspace()
    , _linear()
    , _acceptanceAngle()
    , _suppressionAngle()
    , _keyLift()
    , _keyGain()
    , _show()
    , _sourceAlpha()
{
}

ChromaKeyer::~ChromaKeyer()
{
}

std::string
ChromaKeyer::getInputHint(int inputNb) const
{
    switch (inputNb) {
    case kChromaKeyerInputSource:
        return tr("The foreground image to key.").toStdString();
    case kChromaKeyerInputInsideMask:
        return tr("The Inside Mask, or holdout matte, or core matte, used to confirm areas that are definitely foreground.").toStdString();
    case kChromaKeyerInputOutsideMask:
        return tr("The Outside Mask, or garbage matte, used to remove unwanted objects (lighting rigs, and so on) from the foreground. "
                  "The Outside Mask has priority over the Inside Mask, so that areas where both are one are considered to be outside.")
            .toStdString();
    case kChromaKeyerInputBg:
        return tr("The background image to replace the blue/green screen in the foreground.").toStdString();
    }

    return EffectInstance::getInputHint(inputNb);
}

void
ChromaKeyer::addAcceptedComponents(int inputNb,
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
ChromaKeyer::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_CHROMAKEYER;
    desc.label = "ChromaKeyer";
    desc.description = tr("Simple chroma Keyer.\n"
                          "Algorithm description:\n"
                          "Keith Jack, \"Video Demystified\", Independent Pub Group (Computer), 1996, pp. 214-222, http://www.ee-techs.com/circuit/video-demy5.pdf\n"
                          "A simplified version is described in:\n"
                          "[2] High Quality Chroma Key, Michael Ashikhmin, http://www.cs.utah.edu/~michael/chroma/\n")
                           .toStdString();
    desc.grouping = PLUGIN_GROUP_KEYER;
    desc.majorVersion = PLUGIN_MAJOR_NATRON_CHROMAKEYER;
    desc.minorVersion = 0;
    desc.inputs.push_back(NativeInputDescription("Source", false, eDataKindImage));
    desc.inputs.push_back(NativeInputDescription("InM", true, eDataKindImage, true));
    desc.inputs.push_back(NativeInputDescription("OutM", true, eDataKindImage, true));
    desc.inputs.push_back(NativeInputDescription("Bg", true, eDataKindImage));
    desc.outputKind = eDataKindImage;

    return desc;
}

void
ChromaKeyer::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    _keyColor = createKeyColorKnob(this, page, tr("Key Color").toStdString(), tr("Foreground key color; foreground areas containing the key color are replaced with the background image.").toStdString());

    std::vector<ChoiceOption> colorspaces;
    colorspaces.push_back(ChoiceOption("ccir601", "CCIR 601", tr("Use CCIR 601 (SD footage).").toStdString()));
    colorspaces.push_back(ChoiceOption("rec709", "Rec. 709", tr("Use Rec. 709 (HD footage).").toStdString()));
    colorspaces.push_back(ChoiceOption("rec2020", "Rec. 2020", tr("Use Rec. 2020 (UltraHD/4K footage).").toStdString()));
    _colorspace = makeChoiceKnob(this, page, kChromaKeyerParamColorspace, tr("YCbCr Colorspace").toStdString(), tr("Formula used to compute YCbCr from RGB values.").toStdString(), colorspaces, (int)eColorspaceRec709, false);

    KnobBoolPtr linear = createKnob<KnobBool>(tr("Linear Processing"));
    linear->setName(kChromaKeyerParamLinear);
    linear->setHintToolTip(tr("Do not delinearize RGB values to compute the key value."));
    linear->setDefaultValue(false);
    linear->setAnimationEnabled(false);
    page->addKnob(linear);
    _linear = linear;

    KnobDoublePtr acceptance = makeDoubleKnob(this, page, kChromaKeyerParamAcceptanceAngle, tr("Acceptance Angle").toStdString(), tr("Foreground colors are only suppressed inside the acceptance angle (alpha).").toStdString(), 120., 180., 180., false);
    _acceptanceAngle = acceptance;

    KnobDoublePtr suppression = makeDoubleKnob(this, page, kChromaKeyerParamSuppressionAngle, tr("Suppression Angle").toStdString(),
                                               tr("The chrominance of foreground colors inside the suppression angle (beta) is set to zero on output, to deal with noise. "
                                                  "Use no more than one third of acceptance angle. This has no effect on the alpha channel, or if the output is in Intermediate mode.")
                                                   .toStdString(),
                                               40., 180., 180., false);
    _suppressionAngle = suppression;

    _keyLift = makeDoubleKnob(this, page, kChromaKeyerParamKeyLift, tr("Key Lift").toStdString(), tr("Raise it so that less pixels are classified as background. Makes a sharper transition between foreground and background. Defaults to 0.").toStdString(), 0., 1., 1., true);

    _keyGain = makeDoubleKnob(this, page, kChromaKeyerParamKeyGain, tr("Key Gain").toStdString(), tr("Lower it to classify more colors as background. Defaults to 1.").toStdString(), 1., DBL_MAX, 2., true);

    std::vector<ChoiceOption> shows;
    shows.push_back(ChoiceOption("intermediate", "Intermediate", tr("Color is the source color. Alpha is the foreground key. Use for multi-pass keying.").toStdString()));
    shows.push_back(ChoiceOption("premultiplied", "Premultiplied", tr("Color is the Source color after key color suppression, multiplied by alpha. Alpha is the foreground key.").toStdString()));
    shows.push_back(ChoiceOption("unpremultiplied", "Unpremultiplied", tr("Color is the Source color after key color suppression. Alpha is the foreground key.").toStdString()));
    shows.push_back(ChoiceOption("composite", "Composite", tr("Color is the composite of Source and Bg. Alpha is the foreground key.").toStdString()));
    _show = makeChoiceKnob(this, page, kChromaKeyerParamShow, tr("Output Mode").toStdString(), tr("What image to output.").toStdString(), shows, (int)eShowComposite, false);

    std::vector<ChoiceOption> alphas;
    alphas.push_back(ChoiceOption("ignore", "Ignore", tr("Ignore the source alpha.").toStdString()));
    alphas.push_back(ChoiceOption("insidemask", "Add to Inside Mask", tr("Source alpha is added to the inside mask. Use for multi-pass keying.").toStdString()));
    alphas.push_back(ChoiceOption("normal", "Normal", tr("Foreground key is multiplied by source alpha when compositing.").toStdString()));
    _sourceAlpha = makeChoiceKnob(this, page, kChromaKeyerParamSourceAlpha, tr("Source Alpha").toStdString(), tr("How the alpha embedded in the Source input should be used").toStdString(), alphas, (int)eSourceAlphaIgnore, true);
}

StatusEnum
ChromaKeyer::getPreferredMetadata(NodeMetadata& metadata)
{
    // The output is RGBA whatever the source: the foreground key is its alpha.
    metadata.setNComps(-1, 4);

    return eStatusOK;
}

StatusEnum
ChromaKeyer::getRegionOfDefinition(U64 /*hash*/,
                                   double time,
                                   const RenderScale& scale,
                                   ViewIdx view,
                                   RectD* rod)
{
    // The OpenFX default for its general context is the union of the non-optional clips, which
    // is the source alone.
    EffectInstancePtr input = getInput(kChromaKeyerInputSource);

    if (!input) {
        return eStatusReplyDefault;
    }
    const RenderScale inputScale = input->supportsRenderScale() ? scale : RenderScale::identity;
    bool isProjectFormat = false;

    return (input->getRegionOfDefinition_public(input->getRenderHash(), time, inputScale, view, rod, &isProjectFormat) == eStatusFailed) ? eStatusFailed : eStatusOK;
}

bool
ChromaKeyer::isIdentity(double /*time*/,
                        const RenderScale& /*scale*/,
                        const RectI& /*roi*/,
                        ViewIdx /*view*/,
                        double* /*inputTime*/,
                        ViewIdx* /*inputView*/,
                        int* /*inputNb*/)
{
    return false;
}

bool
ChromaKeyer::isKeyMaskUsed(int inputNb) const
{
    return getInput(inputNb) && isMaskEnabled(inputNb);
}

StatusEnum
ChromaKeyer::render(const RenderActionArgs& args)
{
    KeyerSettings settings;
    KnobColorPtr keyColor = _keyColor.lock();
    if (keyColor) {
        for (int i = 0; i < 3; ++i) {
            settings.keyColor[i] = (float)keyColor->getValueAtTime(args.time, i, args.view);
        }
    }
    settings.colorspace = (ColorspaceEnum)readChoice(_colorspace, args.time, args.view, (int)eColorspaceRec709, (int)eColorspaceRec2020);
    KnobBoolPtr linear = _linear.lock();
    settings.linear = linear ? linear->getValueAtTime(args.time, 0, args.view) : false;
    settings.acceptanceAngle = readDouble(_acceptanceAngle, args.time, args.view, 120.);
    settings.suppressionAngle = readDouble(_suppressionAngle, args.time, args.view, 40.);
    settings.keyLift = readDouble(_keyLift, args.time, args.view, 0.);
    settings.keyGain = readDouble(_keyGain, args.time, args.view, 1.);
    settings.show = (ShowEnum)readChoice(_show, args.time, args.view, (int)eShowComposite, (int)eShowComposite);
    settings.sourceAlpha = (SourceAlphaEnum)readChoice(_sourceAlpha, args.time, args.view, (int)eSourceAlphaIgnore, (int)eSourceAlphaNormal);
    const ChromaKeyKernel kernel(settings);

    // Every image is fetched before any is locked: fetching renders upstream, which may write into a
    // cached image this render would otherwise already hold a read lock on.
    const int maskInputs[2] = { kChromaKeyerInputInsideMask, kChromaKeyerInputOutsideMask };
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
            job.src = fetch(kChromaKeyerInputSource);
            job.bg = fetch(kChromaKeyerInputBg);
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
    NativeImageEffect::makeRowBands(roi, nThreads, &bandRects);
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
} // ChromaKeyer::render

NATRON_NAMESPACE_EXIT
