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

#include "HueCorrect.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Color/HueCurves.h"
#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {

// The background ramp is sliced every this many screen pixels, into at most this many slices.
const int kRampSliceWidthPixels = 8;
const int kRampMaxSlices = 1024;
// The brightness and chroma of the background hues.
const float kRampValue = 0.3f;

#ifdef NATRON_HAVE_GPU
// The parameter block of HueCorrectOp in hue.slang, in words: a header, then the tables.
const int kGpuFlags = 0;
const int kGpuLumaKind = 1;
const int kGpuLumaWeights = 2;
const int kGpuMixLuminance = 5;
const int kGpuHeaderWords = 8;
const int kGpuTotalWords = kGpuHeaderWords + HueCorrectOp::eCurveCount * HueCurves::kLutSize;
const std::uint32_t kGpuFlagClampBlack = 1u;
const std::uint32_t kGpuFlagClampWhite = 2u;

static_assert(kGpuTotalWords * 4 <= 65536, "the parameter block fits a packed op");

void
putFloat(std::vector<std::uint32_t>& words,
         int index,
         float value)
{
    std::memcpy(&words[index], &value, sizeof(float));
}
#endif // NATRON_HAVE_GPU

// A curve's default value: 1, or 0 for the saturation threshold.
float
defaultValue(int curve)
{
    return (curve == HueCorrectOp::eCurveSatThrsh) ? 0.f : 1.f;
}

HueCorrectOp::Tables
buildTables(const KnobParametricPtr& knob)
{
    HueCorrectOp::Tables tables;

    tables.lut.resize(HueCorrectOp::eCurveCount * HueCurves::kLutSize);
    for (int curve = 0; curve < HueCorrectOp::eCurveCount; ++curve) {
        float* lut = &tables.lut[curve * HueCurves::kLutSize];
        if (!knob) {
            std::fill(lut, lut + HueCurves::kLutSize, defaultValue(curve));
            continue;
        }
        HueCurves::sampleLut([&knob, curve](double position) {
            double value = 0.;
            const StatusEnum status = knob->getValue(curve, position, &value);
            if (status != eStatusOK) {
                value = defaultValue(curve);
            }

            // Negative values and NaN read as 0, as in OpenFX.
            return (std::max)(0., value);
        },
                             lut);
        for (int i = 0; i < HueCurves::kLutSize; ++i) {
            if (lut[i] != defaultValue(curve)) {
                tables.atDefaults = false;
            }
        }
    }

    return tables;
}

float
clampOut(const HueCorrectOp::Params& params,
         float v)
{
    if (params.clampBlack && (v < 0.f)) {
        return 0.f;
    } else if (params.clampWhite && (v > 1.f)) {
        return 1.f;
    }

    return v;
}

// c moved towards m by the suppression factor k when it is above m.
inline float
suppress(float c,
         float m,
         float k)
{
    if (c > m) {
        const float d = c - m;
        const float e = k * d;

        return m + e;
    }

    return c;
}

// The background colour of curve position x, red at 1.
void
rampColour(double x,
           float* rgb)
{
    const float h = HueCurves::wrapTurn((float)((x - 1.) / 6.));

    ColorMath::colorspaceHexToRgb(h, kRampValue, kRampValue, rgb);
}
} // anonymous namespace

float
HueCorrectOp::luminance(const Params& params,
                        const float* rgb)
{
    switch (params.lumaKind) {
    case eLumaKindAverage: {
        const float s1 = rgb[0] + rgb[1];
        const float s = s1 + rgb[2];

        return s / 3.f;
    }
    case eLumaKindMax:
        return (std::max)((std::max)(rgb[0], rgb[1]), rgb[2]);
    case eLumaKindWeighted:
    default: {
        const float a = params.lumaWeights[0] * rgb[0];
        const float b = params.lumaWeights[1] * rgb[1];
        const float c = params.lumaWeights[2] * rgb[2];
        const float ab = a + b;

        return ab + c;
    }
    }
}

void
HueCorrectOp::createKnobs(KnobHolder* holder,
                          const KnobPagePtr& page)
{
    {
        KnobParametricPtr knob = AppManager::createKnob<KnobParametric>(holder, std::string("Hue Curves"), (int)eCurveCount);
        knob->setName(kHueCorrectParamHue);
        knob->setHintToolTip(std::string("Hue-dependent adjustment lookup curves:\n"
                                         "hue: hue shift.\n"
                                         "sat: saturation gain. This modification is applied last.\n"
                                         "lum: luminance gain\n"
                                         "red: red gain\n"
                                         "green: green gain\n"
                                         "blue: blue gain\n"
                                         "r_sup: red suppression. If r > min(g,b),  r = min(g,b) + r_sup * (r-min(g,b))\n"
                                         "g_sup: green suppression\n"
                                         "b_sup: blue suppression\n"
                                         "sat_thrsh: if source saturation is below this value, do not apply the lum, red, green, blue gains. Above this value, apply gain progressively."));
        std::vector<double> mins(eCurveCount, 0.);
        std::vector<double> maxs(eCurveCount, DBL_MAX);
        mins[eCurveHue] = -DBL_MAX;
        maxs[eCurveSatThrsh] = 1.;
        knob->setMinimumsAndMaximums(mins, maxs);
        knob->setDisplayMinimumsAndMaximums(std::vector<double>(eCurveCount, 0.), std::vector<double>(eCurveCount, 2.));
        knob->setPeriodic(true);
        const char* names[eCurveCount] = { "hue", "sat", "lum", "red", "green", "blue", "r_sup", "g_sup", "b_sup", "sat_thrsh" };
        // The red, green and blue colours share one Rec. 709 luminance with the grey.
        const double red[3] = { 1., (0.7152 - 0.2126) / (0.7152 + 0.0722), (0.7152 - 0.2126) / (0.7152 + 0.0722) };
        const double green[3] = { 0., 1., 0. };
        const double blue[3] = { (0.7152 - 0.0722) / (0.7152 + 0.2126), (0.7152 - 0.0722) / (0.7152 + 0.2126), 1. };
        const double grey[3] = { 0.7152, 0.7152, 0.7152 };
        for (int curve = 0; curve < eCurveCount; ++curve) {
            const double* colour = grey;
            if ((curve == eCurveRed) || (curve == eCurveRSup)) {
                colour = red;
            } else if ((curve == eCurveGreen) || (curve == eCurveGSup)) {
                colour = green;
            } else if ((curve == eCurveBlue) || (curve == eCurveBSup)) {
                colour = blue;
            }
            knob->setCurveColor(curve, colour[0], colour[1], colour[2]);
            knob->setDimensionName(curve, names[curve]);
        }
        knob->setParametricRange(0., 6.);
        // A periodic curve repeats its first point at 6, so the points are at 0 to 5.
        for (int curve = 0; curve < eCurveCount; ++curve) {
            for (int p = 0; p <= 5; ++p) {
                const StatusEnum status = knob->addControlPoint(eValueChangedReasonPluginEdited, curve, (double)p, (double)defaultValue(curve), eKeyframeTypeSmooth);
                Q_UNUSED(status);
            }
        }
        knob->setDefaultCurvesFromCurves();
        page->addKnob(knob);
        hue = knob;
    }
    {
        KnobBoolPtr knob = AppManager::createKnob<KnobBool>(holder, std::string("Hue Vs Hue Guide"));
        knob->setName(kHueCorrectParamHueVsHue);
        knob->setHintToolTip(std::string("Display a curve background guide designed for hue vs. hue tuning."));
        knob->setDefaultValue(false);
        knob->setEvaluateOnChange(false);
        knob->setIsPersistent(false);
        knob->setAnimationEnabled(false);
        page->addKnob(knob);
        hueVsHue = knob;
    }
    luminanceMath = ColorMath::addLuminanceMathKnob(holder, page);
    if (KnobChoicePtr knob = luminanceMath.lock()) {
        knob->setHintToolTip(std::string("Formula used to compute luminance from RGB values, for the sat curve and Mix Luminance."));
    }
    {
        KnobBoolPtr knob = AppManager::createKnob<KnobBool>(holder, std::string("Clamp Black"));
        knob->setName(kHueCorrectParamClampBlack);
        knob->setHintToolTip(std::string("All colors below 0 on output are set to 0."));
        knob->setDefaultValue(false);
        knob->setAddNewLine(false);
        page->addKnob(knob);
        clampBlack = knob;
    }
    {
        KnobBoolPtr knob = AppManager::createKnob<KnobBool>(holder, std::string("Clamp White"));
        knob->setName(kHueCorrectParamClampWhite);
        knob->setHintToolTip(std::string("All colors above 1 on output are set to 1."));
        knob->setDefaultValue(false);
        page->addKnob(knob);
        clampWhite = knob;
    }
    {
        KnobBoolPtr knob = AppManager::createKnob<KnobBool>(holder, std::string("Mix Luminance"));
        knob->setName(kHueCorrectParamMixLuminanceEnable);
        knob->setHintToolTip(std::string("Mix luminance"));
        knob->setDefaultValue(true);
        knob->setAnimationEnabled(false);
        knob->setAddNewLine(false);
        page->addKnob(knob);
        mixLuminanceEnable = knob;
    }
    {
        KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(holder, std::string());
        knob->setName(kHueCorrectParamMixLuminance);
        knob->setHintToolTip(std::string("Mix luminance"));
        knob->setDefaultValue(0.);
        knob->setMinimum(0.);
        knob->setMaximum(1.);
        knob->setDisplayMinimum(0.);
        knob->setDisplayMaximum(1.);
        page->addKnob(knob);
        mixLuminance = knob;
    }
} // HueCorrectOp::createKnobs

HueCorrectOp::Params
HueCorrectOp::bake(const KnobReader& reader) const
{
    Params p;
    const KnobParametricPtr knob = hue.lock();
    const auto build = [&knob]() {
        return buildTables(knob);
    };

    p.tables = knob ? _tables.get(*knob, build) : std::make_shared<const Tables>(build());

    const ColorMath::LuminanceMathEnum math = (ColorMath::LuminanceMathEnum)reader.value(luminanceMath);
    if (math == ColorMath::eLuminanceMathAverage) {
        p.lumaKind = eLumaKindAverage;
    } else if (math == ColorMath::eLuminanceMathMax) {
        p.lumaKind = eLumaKindMax;
    } else {
        p.lumaKind = eLumaKindWeighted;
        p.lumaWeights[0] = (float)ColorMath::luminance(math, 1., 0., 0.);
        p.lumaWeights[1] = (float)ColorMath::luminance(math, 0., 1., 0.);
        p.lumaWeights[2] = (float)ColorMath::luminance(math, 0., 0., 1.);
    }
    p.mixLuminance = reader.value(mixLuminanceEnable) ? (float)reader.value(mixLuminance) : 0.f;
    p.clampBlack = reader.value(clampBlack);
    p.clampWhite = reader.value(clampWhite);

    return p;
}

bool
HueCorrectOp::isIdentity(const Params& params) const
{
    // With every curve at its default each gain is exactly 1; the luminance mix would still
    // turn a colour of no positive luminance grey.
    return params.tables && params.tables->atDefaults && !params.clampBlack && !params.clampWhite && !(params.mixLuminance > 0.f);
}

void
HueCorrectOp::pixel(const Params& params,
                    float* rgb)
{
    const float* luts = params.tables->lut.data();
    const float lIn = (params.mixLuminance > 0.f) ? luminance(params, rgb) : 0.f;
    const float maxv = (std::max)(rgb[0], (std::max)(rgb[1], rgb[2]));
    const float minv = (std::min)(rgb[0], (std::min)(rgb[1], rgb[2]));
    const float delta = maxv - minv;
    const float h0 = ColorMath::colorspaceHexHue(rgb[0], rgb[1], rgb[2], maxv, delta);
    const float s = (maxv > 0.f) ? (delta / maxv) : 0.f;
    const float x = HueCurves::curvePosition(h0);
    float k[eCurveCount];

    for (int curve = 0; curve < eCurveCount; ++curve) {
        k[curve] = HueCurves::lutAt(luts + curve * HueCurves::kLutSize, x);
    }
    float r = rgb[0];
    float g = rgb[1];
    float b = rgb[2];

    if (k[eCurveHue] != 1.f) {
        const float d = k[eCurveHue] - 1.f;
        const float half = d * 0.5f;
        const float h1 = h0 + half;
        float v[3];
        // The colour keeps its largest channel and its chroma.
        ColorMath::colorspaceHexToRgb(HueCurves::wrapTurn(h1), maxv, delta, v);
        r = v[0];
        g = v[1];
        b = v[2];
    }
    if (k[eCurveRSup] != 1.f) {
        r = suppress(r, (std::min)(g, b), k[eCurveRSup]);
    }
    if (k[eCurveGSup] != 1.f) {
        g = suppress(g, (std::min)(r, b), k[eCurveGSup]);
    }
    if (k[eCurveBSup] != 1.f) {
        b = suppress(b, (std::min)(r, g), k[eCurveBSup]);
    }
    const float thr = k[eCurveSatThrsh];
    const float lum = k[eCurveLum];
    const float rl = k[eCurveRed] * lum;
    const float gl = k[eCurveGreen] * lum;
    const float bl = k[eCurveBlue] * lum;
    if (s > thr) {
        // The gains rise from 1 at the threshold to their full value at saturation 1.
        const float e = s - thr;
        const float tr = e * rl;
        const float tg = e * gl;
        const float tb = e * bl;
        const float nr = thr + tr;
        const float ng = thr + tg;
        const float nb = thr + tb;
        r = r * (nr / s);
        g = g * (ng / s);
        b = b * (nb / s);
    } else if (thr == 0.f) {
        r = r * rl;
        g = g * gl;
        b = b * bl;
    }
    const float sat = k[eCurveSat];
    if (sat != 1.f) {
        const float c[3] = { r, g, b };
        const float l = luminance(params, c);
        const float om = 1.f - sat;
        const float a = om * l;
        const float sr = sat * r;
        const float sg = sat * g;
        const float sb = sat * b;
        r = a + sr;
        g = a + sg;
        b = a + sb;
    }
    if (params.mixLuminance > 0.f) {
        const float c[3] = { r, g, b };
        const float lOut = luminance(params, c);
        if (lOut <= 0.f) {
            r = lIn;
            g = lIn;
            b = lIn;
        } else {
            const float q = lIn / lOut;
            const float q1 = q - 1.f;
            const float mq = params.mixLuminance * q1;
            const float f = 1.f + mq;
            r = r * f;
            g = g * f;
            b = b * f;
        }
    }
    rgb[0] = clampOut(params, r);
    rgb[1] = clampOut(params, g);
    rgb[2] = clampOut(params, b);
} // HueCorrectOp::pixel

void
HueCorrectOp::row(const Params& params,
                  const RowIO& io) const
{
    const int nComps = io.nComps;
    // addAcceptedComponents() admits RGB and RGBA only; a narrower pixel has no hue.
    if ((nComps < 3) || !params.tables) {
        std::copy(io.src[0], io.src[0] + io.width * nComps, io.dst);

        return;
    }
    bool process[3] = { false, false, false };

    for (int c = 0; c < 3; ++c) {
        process[c] = io.channels[pixelKernelChannelBit(nComps, c)];
    }
    const float* src = io.src[0];
    float* dst = io.dst;
    for (int x = 0; x < io.width; ++x, src += nComps, dst += nComps) {
        float rgb[3] = { src[0], src[1], src[2] };
        pixel(params, rgb);
        for (int c = 0; c < 3; ++c) {
            if (process[c]) {
                dst[c] = rgb[c];
            }
        }
    }
}

GpuOpRef
HueCorrectOp::gpu(const Params& params) const
{
#ifdef NATRON_HAVE_GPU
    if (!params.tables) {
        return GpuOpRef::none();
    }
    std::vector<std::uint32_t> words(kGpuTotalWords, 0);
    words[kGpuFlags] = (params.clampBlack ? kGpuFlagClampBlack : 0u) | (params.clampWhite ? kGpuFlagClampWhite : 0u);
    words[kGpuLumaKind] = (std::uint32_t)params.lumaKind;
    for (int c = 0; c < 3; ++c) {
        putFloat(words, kGpuLumaWeights + c, params.lumaWeights[c]);
    }
    putFloat(words, kGpuMixLuminance, params.mixLuminance);
    const std::vector<float>& lut = params.tables->lut;
    std::memcpy(&words[kGpuHeaderWords], lut.data(), lut.size() * sizeof(float));
    GpuOpRef ref = GpuOpRef::packed((int)gpu::GpuKernelId::huecorrect_wrapped, std::move(words));
    ref.acceptedComponents = GpuOpRef::componentsBit(3) | GpuOpRef::componentsBit(4);

    return ref;
#else
    Q_UNUSED(params);

    return GpuOpRef::none();
#endif
}

HueCorrect::HueCorrect(NodePtr node)
    : ImageNode<HueCorrectOp>(node)
{
}

HueCorrect::~HueCorrect()
{
}

HueCorrectOp::Params
HueCorrect::bakeHueCorrect(double time,
                           ViewIdx view) const
{
    return bakeParams(op(), time, view);
}

void
HueCorrect::hueCorrectRow(const HueCorrectOp::Params& params,
                          const RowIO& io) const
{
    op().row(params, io);
}

void
HueCorrect::addAcceptedComponents(int inputNb,
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
HueCorrect::initializeKnobs()
{
    ImageNode<HueCorrectOp>::initializeKnobs();
    installBackgroundPainter();
    refreshMixLuminance();
}

void
HueCorrect::onKnobsLoaded()
{
    ImageNode<HueCorrectOp>::onKnobsLoaded();
    refreshMixLuminance();
}

void
HueCorrect::refreshMixLuminance()
{
    const KnobBoolPtr enable = op().mixLuminanceEnable.lock();
    const KnobDoublePtr mix = op().mixLuminance.lock();

    if (enable && mix) {
        mix->setAllDimensionsEnabled(enable->getValue());
    }
}

bool
HueCorrect::knobChanged(KnobI* k,
                        ValueChangedReasonEnum reason,
                        ViewSpec view,
                        double time,
                        bool originatedFromMainThread)
{
    const KnobBoolPtr enable = op().mixLuminanceEnable.lock();
    const KnobBoolPtr guide = op().hueVsHue.lock();

    if (enable && (k == enable.get())) {
        refreshMixLuminance();

        return true;
    }
    if (guide && (k == guide.get())) {
        if (KnobParametricPtr table = op().hue.lock()) {
            table->notifyBackgroundChanged();
        }

        return true;
    }

    return ImageNode<HueCorrectOp>::knobChanged(k, reason, view, time, originatedFromMainThread);
}

void
HueCorrect::installBackgroundPainter()
{
    KnobParametricPtr table = op().hue.lock();

    if (!table) {
        return;
    }
    const KnobBoolWPtr weakHueVsHue = op().hueVsHue;

    table->setBackgroundPainter([weakHueVsHue](const ParametricBackgroundContext& context) {
        ParametricBackground background;
        KnobBoolPtr hueVsHue = weakHueVsHue.lock();
        const bool guide = hueVsHue && hueVsHue->getValue();

        // Hue x at the bottom edge and slant x at the top: a vertical ramp, or for the guide one
        // slanted so that the colour at height y is the hue the hue curve shifts x to.
        const double y0 = context.yMin;
        const double y1 = context.yMax;
        const double slant0 = guide ? -3. * (y0 - 1.) : 0.;
        const double slant1 = guide ? -3. * (y1 - 1.) : 0.;
        const double x0 = context.xMin - (std::max)(slant0, slant1);
        const double x1 = context.xMax - (std::min)(slant0, slant1);
        if (!(x1 > x0) || !(y1 > y0)) {
            return background;
        }
        int slices = 1;
        if (context.pixelScaleX > 0.) {
            slices = (int)std::ceil((x1 - x0) / (kRampSliceWidthPixels * context.pixelScaleX));
        }
        slices = (std::max)(1, (std::min)(slices, kRampMaxSlices));

        std::vector<ParametricBackgroundVertex> bottom(slices + 1);
        std::vector<ParametricBackgroundVertex> top(slices + 1);
        for (int i = 0; i <= slices; ++i) {
            const double position = x0 + (x1 - x0) * double(i) / slices;
            float rgb[3];
            rampColour(position, rgb);
            bottom[i] = ParametricBackgroundVertex(position + slant0, y0, rgb[0], rgb[1], rgb[2]);
            top[i] = ParametricBackgroundVertex(position + slant1, y1, rgb[0], rgb[1], rgb[2]);
        }
        background.quads.resize(slices);
        for (int i = 0; i < slices; ++i) {
            ParametricBackgroundQuad& quad = background.quads[i];
            quad.v[0] = bottom[i];
            quad.v[1] = bottom[i + 1];
            quad.v[2] = top[i + 1];
            quad.v[3] = top[i];
        }

        return background;
    });
} // HueCorrect::installBackgroundPainter

NATRON_NAMESPACE_EXIT
