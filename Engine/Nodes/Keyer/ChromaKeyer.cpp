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
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/ColorMath.h"
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/Nodes/Image/StagePlan.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"
#include "Engine/RectD.h"

#ifdef NATRON_HAVE_GPU
#include <span>

#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
constexpr InputSpec kChromaKeyerInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Bg", .role = InputRole::Aux, .optional = true },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

constexpr NodeSpec kChromaKeyerNodeSpec = {
    .id = PLUGINID_NATRON_CHROMAKEYER,
    .label = "ChromaKeyer",
    .description = "Chroma keyer working in the scene-linear working space, with no clamping.\n"
                   "The key colour and the Source are split into luma and chroma with the working space's luma weights; "
                   "the chroma inside the acceptance angle around the key colour gives the background key.\n"
                   "Algorithm description:\n"
                   "Keith Jack, \"Video Demystified\", Independent Pub Group (Computer), 1996, pp. 214-222, http://www.ee-techs.com/circuit/video-demy5.pdf\n"
                   "A simplified version is described in:\n"
                   "[2] High Quality Chroma Key, Michael Ashikhmin, http://www.cs.utah.edu/~michael/chroma/\n",
    .grouping = PLUGIN_GROUP_KEYER,
    .majorVersion = PLUGIN_MAJOR_NATRON_CHROMAKEYER,
    .minorVersion = 0,
    .icon = NATRON_IMAGES_PATH "NativeNodes/net.sf.openfx.ChromaKeyerPlugin.png",
    .inputs = kChromaKeyerInputs,
};

constexpr ImagePipelineSpec kChromaKeyerPipeline = {
    .stages = { Stage::MaskMix, Stage::PassThroughUnprocessed },
    .layers = LayerPolicy::color(true, true, true, true),
    .op = OpKind::Custom,
};

#ifndef M_PI
#define M_PI 3.14159265358979323846264338327950288
#endif

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
} // anonymous namespace

struct ChromaKeyerSettings {
    float keyColor[3];
    // The working space's luma weights of red and blue; green's is what remains.
    float kr;
    float kb;
    double acceptanceAngle;
    double suppressionAngle;
    double keyLift;
    double keyGain;
    ShowEnum show;
    SourceAlphaEnum sourceAlpha;
    bool sourceHasAlpha;

    ChromaKeyerSettings()
        : kr(0.f)
        , kb(0.f)
        , acceptanceAngle(120.)
        , suppressionAngle(40.)
        , keyLift(0.)
        , keyGain(1.)
        , show(eShowComposite)
        , sourceAlpha(eSourceAlphaIgnore)
        , sourceHasAlpha(false)
    {
        keyColor[0] = keyColor[1] = keyColor[2] = 0.f;
    }
};

namespace {
#ifdef NATRON_HAVE_GPU
// Mirrors the parameter block of Kernels/chromakeyer.slang, word for word.
struct ChromaKeyGpuView {
    uint32_t offset;
    uint32_t stride;
    int32_t x1;
    int32_t y1;
    int32_t x2;
    int32_t y2;
    uint32_t comps;
};

struct ChromaKeyGpuParams {
    float cosKey;
    float sinKey;
    float ys;
    float tanAcceptance;
    float tanSuppression;
    float keyLift;
    float oneMinusLift;
    float gainXKey;
    float kr;
    float kb;
    float kg;
    float twoOneMinusKb;
    float twoOneMinusKr;
    uint32_t flags;
    uint32_t gainMode;
    uint32_t show;
    uint32_t sourceAlpha;
    ChromaKeyGpuView bg;
};
static_assert(sizeof(ChromaKeyGpuParams) == 24 * 4, "the kernel reads 24 words");

const uint32_t kChromaKeyGpuAcceptAll = 1;
const uint32_t kChromaKeyGpuSuppressAll = 2;
const uint32_t kChromaKeyGpuSourceHasAlpha = 4;

const uint32_t kChromaKeyGpuGainRamp = 0;
const uint32_t kChromaKeyGpuGainNonPositive = 1;
const uint32_t kChromaKeyGpuLiftFull = 2;
#endif // NATRON_HAVE_GPU

class ChromaKeyKernel {
public:
    explicit ChromaKeyKernel(const ChromaKeyerSettings& settings)
        : _kr(settings.kr)
        , _kb(settings.kb)
        , _acceptanceAngle(settings.acceptanceAngle)
        , _tanAcceptanceAngle2(0.)
        , _suppressionAngle(settings.suppressionAngle)
        , _tanSuppressionAngle2(0.)
        , _keyLift(settings.keyLift)
        , _keyGain(settings.keyGain)
        , _show(settings.show)
        , _sourceAlpha(settings.sourceAlpha)
        , _sourceHasAlpha(settings.sourceHasAlpha)
        , _sinKey(0.)
        , _cosKey(0.)
        , _xKey(0.)
        , _ys(0.)
    {
        float y, cb, cr;
        toYPbPr(ColorMath::boundForKeying(settings.keyColor[0]), ColorMath::boundForKeying(settings.keyColor[1]), ColorMath::boundForKeying(settings.keyColor[2]), &y, &cb, &cr);
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

    // fg is the source pixel (r, g, b, a) or null outside the source. bg is always readable,
    // zero where Bg has no pixel. out receives the four output channels.
    void process(const float* fg,
                 const float* bg,
                 float* out) const
    {
        const bool fgHasAlpha = _sourceHasAlpha;
        float inMask = 0.f;
        if ((_sourceAlpha == eSourceAlphaInsideMask) && fg && fgHasAlpha) {
            inMask = (std::max)(0.f, (std::min)(fg[3], 1.f));
        }
        float Kbg = 0.f;

        float fgr = fg ? ColorMath::boundForKeying(fg[0]) : 0.f;
        float fgg = fg ? ColorMath::boundForKeying(fg[1]) : 0.f;
        float fgb = fg ? ColorMath::boundForKeying(fg[2]) : 0.f;
        const float bgr = bg[0];
        const float bgg = bg[1];
        const float bgb = bg[2];

        if (!fg) {
            Kbg = 1.f;
            fgr = fgg = fgb = 0.;
        } else {
            float fgy, fgcb, fgcr;
            toYPbPr(fgr, fgg, fgb, &fgy, &fgcb, &fgcr);

            // Rotate the (Cb, Cr) plane, scaled by two, so the key colour is on the X axis.
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

            double Kfg_new = Kfg;
            if ((inMask > 0.) && (Kfg > 1. - inMask)) {
                Kfg_new = 1. - inMask;
            }
            if (Kfg != 0.) {
                fgx_scaled = Kfg_new + std::abs(fgz) / _tanAcceptanceAngle2;
            }
            Kfg = Kfg_new;

            if (_show != eShowIntermediate) {
                // X and Z are built from twice the chrominance, so removing Kfg from X removes
                // Kfg / 2 from (Cb, Cr). Nothing is clamped: scene-linear values keep their range.
                if ((fgx_scaled > 0) && ((_suppressionAngle <= 0.) || (_suppressionAngle >= 180.) || (fgx_scaled - std::abs(fgz) / _tanSuppressionAngle2 > 0.))) {
                    fgcb = 0;
                    fgcr = 0;
                } else {
                    fgcb = static_cast<float>(fgcb - Kfg * _cosKey / 2);
                    fgcr = static_cast<float>(fgcr - Kfg * _sinKey / 2);
                }

                // _ys is such that the luminance of the key colour goes to zero.
                fgy = static_cast<float>(fgy - _ys * Kfg);
                toRGB(fgy, fgcb, fgcr, &fgr, &fgg, &fgb);
            }

            // The gain scales the key colour's distance to the full key, and the lift is the
            // fraction of that distance where the linear ramp starts.
            if (_keyGain <= 0.) {
                Kbg = (Kfg > 0.) ? 1.f : 0.f;
            } else if (_keyLift >= 1.) {
                Kbg = (Kfg >= _keyGain * _xKey) ? 1.f : 0.f;
            } else {
                // Unclamped, but kept finite: a near-zero key chroma would overflow it, and Bg
                // times an infinite key is NaN where Bg is zero.
                Kbg = ColorMath::boundForKeying((float)((Kfg / (_keyGain * _xKey) - _keyLift) / (1. - _keyLift)));
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

#ifdef NATRON_HAVE_GPU
    // The per-frame constants of process() for the GPU kernel, which keys in float: the branches
    // on the angles, the gain and the lift become flags and modes, and the luma weights and their
    // sums are rounded to float as toYPbPr() and toRGB() round them.
    void getGpuParams(ChromaKeyGpuParams* p) const
    {
        p->cosKey = (float)_cosKey;
        p->sinKey = (float)_sinKey;
        p->ys = (float)_ys;
        p->tanAcceptance = (float)_tanAcceptanceAngle2;
        p->tanSuppression = (float)_tanSuppressionAngle2;
        p->keyLift = (float)_keyLift;
        p->oneMinusLift = (float)(1. - _keyLift);
        p->gainXKey = (float)(_keyGain * _xKey);
        p->kr = _kr;
        p->kb = _kb;
        p->kg = 1 - _kr - _kb;
        p->twoOneMinusKb = 2 * (1 - _kb);
        p->twoOneMinusKr = 2 * (1 - _kr);
        p->flags = ((_acceptanceAngle >= 180.) ? kChromaKeyGpuAcceptAll : 0u) | (((_suppressionAngle <= 0.) || (_suppressionAngle >= 180.)) ? kChromaKeyGpuSuppressAll : 0u) | (_sourceHasAlpha ? kChromaKeyGpuSourceHasAlpha : 0u);
        p->gainMode = (_keyGain <= 0.) ? kChromaKeyGpuGainNonPositive : ((_keyLift >= 1.) ? kChromaKeyGpuLiftFull : kChromaKeyGpuGainRamp);
        p->show = (uint32_t)_show;
        p->sourceAlpha = (uint32_t)_sourceAlpha;
    }
#endif // NATRON_HAVE_GPU

private:
    // Y'PbPr with the working space's weights, in single precision.
    void toYPbPr(float r,
                 float g,
                 float b,
                 float* y,
                 float* pb,
                 float* pr) const
    {
        *y = _kr * r + (1 - _kr - _kb) * g + _kb * b;
        *pb = (b - *y) / (2 * (1 - _kb));
        *pr = (r - *y) / (2 * (1 - _kr));
    }

    void toRGB(float y,
               float pb,
               float pr,
               float* r,
               float* g,
               float* b) const
    {
        *b = pb * (2 * (1 - _kb)) + y;
        *r = pr * (2 * (1 - _kr)) + y;
        *g = (y - _kr * *r - _kb * *b) / (1 - _kr - _kb);
    }

    float _kr;
    float _kb;
    double _acceptanceAngle;
    double _tanAcceptanceAngle2;
    double _suppressionAngle;
    double _tanSuppressionAngle2;
    double _keyLift;
    double _keyGain;
    ShowEnum _show;
    SourceAlphaEnum _sourceAlpha;
    bool _sourceHasAlpha;
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

bool
isFloatImage(const ImagePtr& image)
{
    return !image || (image->getBitDepth() == eImageBitDepthFloat);
}

#ifdef NATRON_HAVE_GPU
// The float index of pixel (x, y) of v, false when the view does not fit the kernel's 32-bit
// indices.
bool
gpuFloatIndex(const GpuImageView& v,
              int x,
              int y,
              uint32_t* index)
{
    const uint64_t base = v.byteOffset / sizeof(float);
    const uint64_t rows = (uint64_t)std::max(0, v.bounds.height());

    if (base + rows * (uint64_t)v.rowStrideFloats > UINT32_MAX) {
        return false;
    }
    *index = (uint32_t)(base + (uint64_t)(y - v.bounds.y1) * v.rowStrideFloats + (uint64_t)(x - v.bounds.x1) * v.nComps);

    return true;
}

// A view without a buffer packs as empty bounds, which the kernel reads as zero.
bool
packGpuView(const GpuImageView& v,
            ChromaKeyGpuView* out)
{
    *out = ChromaKeyGpuView();
    if (!v.buffer || v.bounds.isNull()) {
        return true;
    }
    if (!gpuFloatIndex(v, v.bounds.x1, v.bounds.y1, &out->offset)) {
        return false;
    }
    out->stride = (uint32_t)v.rowStrideFloats;
    out->x1 = v.bounds.x1;
    out->y1 = v.bounds.y1;
    out->x2 = v.bounds.x2;
    out->y2 = v.bounds.y2;
    out->comps = (uint32_t)v.nComps;

    return true;
}
#endif // NATRON_HAVE_GPU

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
    : StagedImageEffect(node)
    , _keyColor()
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

const NodeSpec*
ChromaKeyer::nodeSpec() const
{
    return staticNodeSpec<kChromaKeyerNodeSpec>();
}

const ImagePipelineSpec&
ChromaKeyer::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kChromaKeyerPipeline, kChromaKeyerNodeSpec>();
}

std::string
ChromaKeyer::getInputHint(int inputNb) const
{
    switch (inputNb) {
    case kChromaKeyerInputSource:
        return tr("The foreground image to key.").toStdString();
    case kChromaKeyerInputBg:
        return tr("The background image to replace the blue/green screen in the foreground.").toStdString();
    case kChromaKeyerInputMask:
        return tr("Limits the key: outside the mask the output is the Source.").toStdString();
    }

    return EffectInstance::getInputHint(inputNb);
}

void
ChromaKeyer::addAcceptedComponents(int inputNb,
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
ChromaKeyer::initializeKnobs()
{
    KnobPagePtr page = createControlsPage();

    _keyColor = createKeyColorKnob(this, page, tr("Key Color").toStdString(), tr("Foreground key color; foreground areas containing the key color are replaced with the background image.").toStdString());

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
    _show = makeChoiceKnob(this, page, kChromaKeyerParamShow, tr("Show").toStdString(), tr("What image to output.").toStdString(), shows, (int)eShowComposite, false);

    std::vector<ChoiceOption> alphas;
    alphas.push_back(ChoiceOption("ignore", "Ignore", tr("Ignore the source alpha.").toStdString()));
    alphas.push_back(ChoiceOption("insidemask", "Inside Mask", tr("Source alpha marks areas that are definitely foreground: the key is kept below one minus it. Use for multi-pass keying.").toStdString()));
    alphas.push_back(ChoiceOption("normal", "Normal", tr("Foreground key is multiplied by source alpha when compositing.").toStdString()));
    _sourceAlpha = makeChoiceKnob(this, page, kChromaKeyerParamSourceAlpha, tr("Source Alpha Handling").toStdString(), tr("How the alpha embedded in the Source input should be used. An RGB Source has none.").toStdString(), alphas, (int)eSourceAlphaIgnore, true);

    addStageKnobs(page);
}

StatusEnum
ChromaKeyer::getPreferredMetadataAt(double /*time*/,
                                    ViewIdx /*view*/,
                                    NodeMetadata& metadata)
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
    // The union of the non-optional inputs, which is the source alone.
    EffectInstancePtr input = getInput(kChromaKeyerInputSource);

    if (!input) {
        return eStatusReplyDefault;
    }
    const RenderScale inputScale = input->supportsRenderScale() ? scale : RenderScale::identity;
    bool isProjectFormat = false;

    return (input->getRegionOfDefinition_public(input->getRenderHash(), time, inputScale, view, rod, &isProjectFormat) == eStatusFailed) ? eStatusFailed : eStatusOK;
}

bool
ChromaKeyer::isOpIdentity(double /*time*/,
                          const RenderScale& /*scale*/,
                          const RectI& /*roi*/,
                          ViewIdx /*view*/)
{
    return false;
}

bool
ChromaKeyer::resolveColorInputLayer(int inputNb,
                                    double time,
                                    ViewIdx view,
                                    ImageLayerDesc* layer) const
{
    if (!getInput(inputNb)) {
        return false;
    }
    // Resolving only reads the node's selectors and its inputs' layers.
    ChromaKeyer* self = const_cast<ChromaKeyer*>(this);

    return self->resolveInputPlaneForRender(inputNb, time, view, layer, NULL);
}

bool
ChromaKeyer::sourceHasAlpha(double time,
                            ViewIdx view) const
{
    ImageLayerDesc layer;

    if (!resolveColorInputLayer(kChromaKeyerInputSource, time, view, &layer)) {
        return false;
    }
    const int nComps = layer.getNumComponents();

    return (nComps >= 4) || (nComps == 1);
}

void
ChromaKeyer::readSettings(double time,
                          ViewIdx view,
                          ChromaKeyerSettings* settings) const
{
    KnobColorPtr keyColor = _keyColor.lock();
    if (keyColor) {
        for (int i = 0; i < 3; ++i) {
            settings->keyColor[i] = (float)keyColor->getValueAtTime(time, i, view);
        }
    }
    double luma[3];
    const double* fallback = ColorMath::acesCgLumaWeights();
    for (int i = 0; i < 3; ++i) {
        luma[i] = fallback[i];
    }
    AppInstancePtr app = getApp();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    ProjectColorManagementPtr cm = project ? project->getColorManagement() : ProjectColorManagementPtr();
    if (cm) {
        cm->getWorkingLuma(luma);
    }
    settings->kr = (float)luma[0];
    settings->kb = (float)luma[2];
    settings->acceptanceAngle = readDouble(_acceptanceAngle, time, view, 120.);
    settings->suppressionAngle = readDouble(_suppressionAngle, time, view, 40.);
    settings->keyLift = readDouble(_keyLift, time, view, 0.);
    settings->keyGain = readDouble(_keyGain, time, view, 1.);
    settings->show = (ShowEnum)readChoice(_show, time, view, (int)eShowComposite, (int)eShowComposite);
    settings->sourceAlpha = (SourceAlphaEnum)readChoice(_sourceAlpha, time, view, (int)eSourceAlphaIgnore, (int)eSourceAlphaNormal);
    settings->sourceHasAlpha = sourceHasAlpha(time, view);
}

StatusEnum
ChromaKeyer::renderOp(const RenderActionArgs& args,
                      const StagePlan& plan)
{
    ChromaKeyerSettings settings;
    readSettings(args.time, args.view, &settings);
    const ChromaKeyKernel kernel(settings);

    // The Source's pixels: the stages read it zero outside, where the key is the background.
    RectI sourceRect;
    bool hasSource = false;
    EffectInstancePtr source = getInput(kChromaKeyerInputSource);
    ImageLayerDesc sourceLayer;
    if (source && (plan.sourceInput == kChromaKeyerInputSource) && resolveColorInputLayer(kChromaKeyerInputSource, args.time, args.view, &sourceLayer)) {
        const RenderScale inputScale = source->supportsRenderScale() ? args.mappedScale : RenderScale::identity;
        RectD rod;
        bool isProjectFormat = false;
        if (source->getRegionOfDefinition_public(source->getRenderHash(), args.time, inputScale, args.view, &rod, &isProjectFormat) == eStatusFailed) {
            return eStatusFailed;
        }
        sourceRect = rod.toPixelEnclosing(args.mappedScale, getRenderMetadata(args.time, args.view)->getPixelAspectRatio(kChromaKeyerInputSource));
        hasSource = true;
    }

    // Bg, fetched before runStagesCpu() fetches the rest, and locked here, on the calling thread;
    // the band threads only read through it.
    ImagePtr bg;
    ImageLayerDesc bgLayer;
    if (resolveColorInputLayer(kChromaKeyerInputBg, args.time, args.view, &bgLayer)) {
        RectI bgRoI;
        bg = getImage(kChromaKeyerInputBg, args.time, args.mappedScale, args.view, NULL, &bgLayer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &bgRoI);
    }
    if (!isFloatImage(bg)) {
        return eStatusFailed;
    }
    LockedImage bgLocked;
    if (bg) {
        bgLocked.image = bg.get();
        bgLocked.bounds = bg->getBounds();
        bgLocked.access = std::make_shared<Image::ReadAccess>(bg.get());
    }

    const auto row = [&](const RowIO& io) {
        const int nComps = io.nComps;

        // getPreferredMetadataAt() makes the output RGBA; another plane passes through.
        if (nComps != 4) {
            std::copy(io.src[0], io.src[0] + (std::size_t)io.width * nComps, io.dst);

            return;
        }
        std::vector<float> bgRow((std::size_t)io.width * 4);
        int bgStart, bgEnd;
        bool bgHasAlpha;
        readColorRow(bg ? &bgLocked : NULL, io.x0, io.y, io.width, &bgRow[0], &bgStart, &bgEnd, &bgHasAlpha);
        const bool rowInSource = hasSource && (io.y >= sourceRect.y1) && (io.y < sourceRect.y2);
        for (int i = 0; i < io.width; ++i) {
            const int x = io.x0 + i;
            const bool inSource = rowInSource && (x >= sourceRect.x1) && (x < sourceRect.x2);
            const float* srcPix = io.src[0] + (std::size_t)i * 4;
            kernel.process(inSource ? srcPix : NULL, &bgRow[(std::size_t)i * 4], io.dst + (std::size_t)i * 4);
        }
    };

    return runStagesCpu(*this, plan, args, StageRowFn(row));
} // ChromaKeyer::renderOp

bool
ChromaKeyer::canRenderOpOnGpu(const GpuQuery& query) const
{
#ifdef NATRON_HAVE_GPU
    if ((query.bitDepth != eImageBitDepthFloat) || (query.nComps != 4) || !canRunStagesGpu(resolveGpuPlan(query), query)) {
        return false;
    }
    ImageLayerDesc layer;
    if (resolveColorInputLayer(kChromaKeyerInputBg, query.time, query.view, &layer) && ((layer.getNumComponents() < 1) || (layer.getNumComponents() > 4))) {
        return false;
    }

    return true;
#else
    Q_UNUSED(query);

    return false;
#endif
}

bool
ChromaKeyer::getOpGpuInputLayer(int inputNb,
                                const ImageLayerDesc& plane,
                                double time,
                                ViewIdx view,
                                ImageLayerDesc* layer) const
{
    if (inputNb != kChromaKeyerInputBg) {
        stageGpuInputLayer(*this, inputNb, plane, time, view, layer);

        return true;
    }
    // renderOp() reads an RGB Bg as RGB, whatever plane it renders.
    *layer = plane;
    ImageLayerDesc resolved;
    if (resolveColorInputLayer(inputNb, time, view, &resolved) && (resolved.getNumComponents() > 0)) {
        *layer = resolved;
    }

    return true;
}

StatusEnum
ChromaKeyer::renderOpGpu(const GpuRenderArgs& args)
{
#ifdef NATRON_HAVE_GPU
    const GpuQuery& query = args.query;
    if (!canRenderOpOnGpu(query)) {
        return eStatusFailed;
    }

    ChromaKeyerSettings settings;
    readSettings(query.time, query.view, &settings);
    ChromaKeyGpuParams params = ChromaKeyGpuParams();
    ChromaKeyKernel(settings).getGpuParams(&params);

    const StagePlan plan = resolveGpuPlan(query);
    GpuRenderArgs cropped = args;
    const int sourceInput = plan.sourceInput;
    if ((sourceInput >= 0) && (sourceInput < (int)cropped.inputs.size())) {
        // Cropped to the Source's region of definition, outside which the key is the background.
        if (!stageGpuInputView(*this, args, sourceInput, &cropped.inputs[sourceInput])) {
            return eStatusFailed;
        }
    }

    GpuImageView bgView;
    ImageLayerDesc bgLayer;
    if (resolveColorInputLayer(kChromaKeyerInputBg, query.time, query.view, &bgLayer) && !stageGpuInputView(*this, args, kChromaKeyerInputBg, &bgView)) {
        return eStatusFailed;
    }
    if (!packGpuView(bgView, &params.bg)) {
        return eStatusFailed;
    }

    PixelKernelGpu op;
    op.kernel = (int)gpu::GpuKernelId::chromakeyer_wrapped;
    op.setParams(params);

    return recordStagesGpuKernelInputs(plan, cropped, op, std::span<const GpuImageView>(&bgView, 1));
#else
    Q_UNUSED(args);

    return eStatusFailed;
#endif
} // ChromaKeyer::renderOpGpu

NATRON_NAMESPACE_EXIT
