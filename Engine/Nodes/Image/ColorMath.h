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

#ifndef Engine_Nodes_Image_ColorMath_h
#define Engine_Nodes_Image_ColorMath_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/EngineFwd.h"
#include "Engine/KnobTypes.h"

#define kColorMathParamLuminanceMath "luminanceMath"
#define kColorMathParamLuminanceMathLabel "Luminance Math"
#define kColorMathParamLuminanceMathHint "Formula used to compute luminance from RGB values."

NATRON_NAMESPACE_ENTER

/**
 * @brief Colour maths shared by the native colour, generator and keying nodes. The formulas and
 * coefficients are the openfx-misc ones (SupportExt/ofxsLut.h), written the same way so that
 * the native nodes compute the same values, with one deliberate exception: ACES AP1 uses the
 * AP1 coefficients, where the OpenFX Saturation fell through to CCIR 601.
 **/
namespace ColorMath {
enum LuminanceMathEnum {
    eLuminanceMathRec709 = 0,
    eLuminanceMathRec2020,
    eLuminanceMathAcesAP0,
    eLuminanceMathAcesAP1,
    eLuminanceMathCcir601,
    eLuminanceMathAverage,
    eLuminanceMathMax
};

/**
 * @brief The AP1 (ACEScg) primaries' luma weights, matching the eLuminanceMathAcesAP1 case
 * above, and its RGB-to-XYZ matrix at the ACES white point (about D60, with no chromatic
 * adaptation to D65), row-major so that XYZ = m·rgb. A colour knob with no
 * project to read a working space's colorimetry from (ProjectColorManagement::getWorkingLuma,
 * getWorkingToXYZ) uses these as its stand-in, since ACEScg is the default working space.
 **/
inline const double*
acesCgLumaWeights()
{
    static const double w[3] = { 0.2722287168, 0.6740817658, 0.0536895174 };

    return w;
}

inline const double*
acesCgToXYZ()
{
    static const double m[9] = {
        0.6624541811, 0.1340042065, 0.1561876870,
        0.2722287168, 0.6740817658, 0.0536895174,
        -0.0055746495, 0.0040607335, 1.0103391003
    };

    return m;
}

inline double
luminance(LuminanceMathEnum math,
          double r,
          double g,
          double b)
{
    switch (math) {
    case eLuminanceMathRec709:
        // Single-precision constants, as in ofxsLut.h's rgb709_to_y().
        return 0.2126729f * r + 0.7151522f * g + 0.0721750f * b;
    case eLuminanceMathRec2020:
        return 0.2627002119 * r + 0.6779980711 * g + 0.0593017165 * b;
    case eLuminanceMathAcesAP0:
        return 0.3439664498 * r + 0.7281660966 * g + -0.0721325464 * b;
    case eLuminanceMathAcesAP1:
        return 0.2722287168 * r + 0.6740817658 * g + 0.0536895174 * b;
    case eLuminanceMathCcir601:
        return 0.2989 * r + 0.5866 * g + 0.1145 * b;
    case eLuminanceMathAverage:
        return (r + g + b) / 3;
    case eLuminanceMathMax:
        return (std::max)((std::max)(r, g), b);
    }

    return 0.;
}

/**
 * @brief Declares the luminanceMath choice with the OpenFX option IDs, labels and hints,
 * defaulting to Rec. 709, and adds it to page.
 **/
inline KnobChoicePtr
addLuminanceMathKnob(KnobHolder* holder,
                     const KnobPagePtr& page)
{
    KnobChoicePtr knob = AppManager::createKnob<KnobChoice>(holder, std::string(kColorMathParamLuminanceMathLabel));

    knob->setName(kColorMathParamLuminanceMath);
    knob->setHintToolTip(std::string(kColorMathParamLuminanceMathHint));
    std::vector<ChoiceOption> options;
    options.push_back(ChoiceOption("rec709", "Rec. 709", "Use Rec. 709 (0.2126r + 0.7152g + 0.0722b)."));
    options.push_back(ChoiceOption("rec2020", "Rec. 2020", "Use Rec. 2020 (0.2627r + 0.6780g + 0.0593b)."));
    options.push_back(ChoiceOption("acesap0", "ACES AP0", "Use ACES AP0 (0.3439664498r + 0.7281660966g + -0.0721325464b)."));
    options.push_back(ChoiceOption("acesap1", "ACES AP1", "Use ACES AP1 (0.2722287168r +  0.6740817658g +  0.0536895174b)."));
    options.push_back(ChoiceOption("ccir601", "CCIR 601", "Use CCIR 601 (0.2989r + 0.5866g + 0.1145b)."));
    options.push_back(ChoiceOption("average", "Average", "Use average of r, g, b."));
    options.push_back(ChoiceOption("max", "Max", "Use max or r, g, b."));
    knob->populateChoices(options);
    knob->setDefaultValue((int)eLuminanceMathRec709);
    if (page) {
        page->addKnob(knob);
    }

    return knob;
}

/**
 * @brief The sRGB and Rec. 709 transfer functions: from_* decodes to linear, to_* encodes from
 * linear, negatives clamped to 0. The Rec. 709 pair deliberately uses the BT.2020 constants
 * (1.0993, 0.0181) rather than BT.709's (1.099, 0.018); only ChromaKeyer relies on them.
 **/
inline float
fromSRGB(float v)
{
    if (v < 0.04045f) {
        return (v < 0.0f) ? 0.0f : v * (1.0f / 12.92f);
    }

    return std::pow((v + 0.055f) * (1.0f / 1.055f), 2.4f);
}

inline float
toSRGB(float v)
{
    if (v < 0.0031308f) {
        return (v < 0.0f) ? 0.0f : v * 12.92f;
    }

    return 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

inline float
fromRec709(float v)
{
    if (v < 0.08145f) {
        return (v < 0.0f) ? 0.0f : v * (1.0f / 4.5f);
    }

    return std::pow((v + 0.0993f) * (1.0f / 1.0993f), (1.0f / 0.45f));
}

// Bounds a value a keyer is given so the sums and differences of its colour maths cannot
// overflow: the OpenFX keyers turn an infinite or near-FLT_MAX channel into inf - inf or
// inf * 0, a NaN, while any value up to the bound keys as the limit of ever larger values does.
// NaN is left as it came.
inline float
boundForKeying(float v)
{
    const float bound = std::numeric_limits<float>::max() / 4;

    if (v > bound) {
        return bound;
    }
    if (v < -bound) {
        return -bound;
    }

    return v;
}

inline float
toRec709(float v)
{
    if (v < 0.0181f) {
        return (v < 0.0f) ? 0.0f : v * 4.5f;
    }

    return 1.0993f * std::pow(v, 0.45f) - (1.0993f - 1.f);
}

/**
 * @brief The working space's RGB-to-XYZ matrix, row-major so that XYZ = m·rgb, from the project of
 * holder (ProjectColorManagement::getWorkingToXYZ), or acesCgToXYZ() when holder has no project or
 * the working space has no matrix to XYZ.
 **/
void workingToXYZ(const KnobHolder* holder, double m[9]);

/**
 * @brief The models of the Colorspace node. The luma/chroma models apply their matrix to the
 * linear values, with no transfer function. Hue is in turns, 0 to 1. XYZ, xyY and Lab are
 * relative to the working space's white, its XYZ for RGB (1, 1, 1), with Lab unscaled
 * (L from 0 to 100 for 0 to the white's Y).
 **/
enum ColorspaceModelEnum {
    eColorspaceModelRGB = 0,
    eColorspaceModelHSV,
    eColorspaceModelHSL,
    eColorspaceModelHSI,
    eColorspaceModelYPbPr601,
    eColorspaceModelYPbPr709,
    eColorspaceModelYPbPr2020,
    eColorspaceModelYCbCr601,
    eColorspaceModelYCbCr709,
    eColorspaceModelYCbCr2020,
    eColorspaceModelYUV601,
    eColorspaceModelYUV709,
    eColorspaceModelXYZ,
    eColorspaceModelXyY,
    eColorspaceModelLab,
    eColorspaceModelCount
};

/**
 * @brief The coefficients of one side of a conversion, for its luma/chroma model: luma weights,
 * the scale and offset of Y, and the chroma offset and scales, each with its reciprocal.
 **/
struct ColorspaceSide {
    ColorspaceModelEnum model;
    float kr, kg, kb, kgInv;
    float yScale, yOffset, yInv;
    float cOffset, cb, cr, cbInv, crInv;
};

/**
 * @brief The working space's colorimetry a conversion uses: its matrices to and from XYZ, its
 * white XYZ with the reciprocals, and the white's xy chromaticity.
 **/
struct ColorspaceColorimetry {
    float toXYZ[9];
    float fromXYZ[9];
    float white[3];
    float whiteInv[3];
    float whiteXy[2];
};

/**
 * @brief The coefficients of model, computed in double.
 **/
ColorspaceSide colorspaceSide(ColorspaceModelEnum model);

/**
 * @brief The colorimetry of the working space whose RGB-to-XYZ matrix is toXYZ, computed in
 * double. A singular matrix takes the ACEScg colorimetry.
 **/
ColorspaceColorimetry colorspaceColorimetry(const double toXYZ[9]);

/*
 * The conversions below are written with one rounding per statement, in the order the
 * Colorspace kernel in colorspace.slang computes them, and with no library transcendental: the
 * cube root, arctangent and sine/cosine are polynomials of adds and multiplies, so that the GPU
 * agrees with the CPU to float rounding.
 */

inline float
colorspaceAsFloat(uint32_t u)
{
    float f;

    std::memcpy(&f, &u, sizeof(f));

    return f;
}

inline uint32_t
colorspaceAsUint(float f)
{
    uint32_t u;

    std::memcpy(&u, &f, sizeof(u));

    return u;
}

// x > 0. Three Newton steps on the reciprocal cube root from a bit-level first guess.
inline float
colorspaceCbrtPositive(float x)
{
    float r = colorspaceAsFloat(0x54a21d2au - colorspaceAsUint(x) / 3u);

    for (int i = 0; i < 3; ++i) {
        const float r2 = r * r;
        const float r3 = r2 * r;
        const float xr3 = x * r3;
        const float third = xr3 * 0.3333333432674408f;
        const float d = 1.3333333730697632f - third;
        r = r * d;
    }
    const float r2 = r * r;

    return x * r2;
}

// The arctangent of t in [0, 1], Cephes atanf's reduction and polynomial.
inline float
colorspaceAtanUnit(float t)
{
    float base = 0.f;

    if (t > 0.4142135679721832f) {
        const float num = t - 1.f;
        const float den = t + 1.f;
        t = num / den;
        base = 0.7853981852531433f;
    }
    const float z = t * t;
    float p = 0.0805374458432197f * z;
    p = p - 0.13877685368061066f;
    p = p * z;
    p = p + 0.19977711141109467f;
    p = p * z;
    p = p - 0.3333294987678528f;
    p = p * z;
    p = p * t;
    p = p + t;

    return base + p;
}

// atan2(y, x) in turns, 0 to 1, and 0 at the origin.
inline float
colorspaceAtan2Turns(float y,
                     float x)
{
    const float ax = std::fabs(x);
    const float ay = std::fabs(y);

    if ((ax == 0.f) && (ay == 0.f)) {
        return 0.f;
    }
    float a;
    if (ay <= ax) {
        const float q = ay / ax;
        a = colorspaceAtanUnit(q);
    } else {
        const float q = ax / ay;
        const float t = colorspaceAtanUnit(q);
        a = 1.5707963705062866f - t;
    }
    if (x < 0.f) {
        a = 3.1415927410125732f - a;
    }
    if (y < 0.f) {
        a = -a;
    }
    float turns = a * 0.15915493667125702f;
    if (turns < 0.f) {
        turns = turns + 1.f;
    }

    return turns;
}

// The tangent of u, |u| <= pi/3, from Taylor series of sine and cosine.
inline float
colorspaceTanNear(float u)
{
    const float z = u * u;
    float p = z * -2.505210838544172e-08f;

    p = p + 2.7557319223985893e-06f;
    p = p * z;
    p = p - 0.0001984126984126984f;
    p = p * z;
    p = p + 0.008333333333333333f;
    p = p * z;
    p = p - 0.16666666666666666f;
    p = p * z;
    p = p * u;
    const float s = u + p;
    float q = z * 2.08767569878681e-09f;
    q = q - 2.755731922398589e-07f;
    q = q * z;
    q = q + 2.48015873015873e-05f;
    q = q * z;
    q = q - 0.001388888888888889f;
    q = q * z;
    q = q + 0.041666666666666664f;
    q = q * z;
    q = q - 0.5f;
    q = q * z;
    const float c = 1.f + q;

    return s / c;
}

// Hue in turns from the six sectors of the RGB hexagon; 0 at zero chroma.
inline float
colorspaceHexHue(float r,
                 float g,
                 float b,
                 float maxv,
                 float delta)
{
    if (!(delta > 0.f)) {
        return 0.f;
    }
    float h;
    if (r == maxv) {
        const float d = g - b;
        h = d / delta;
    } else if (g == maxv) {
        const float d = b - r;
        const float q = d / delta;
        h = 2.f + q;
    } else {
        const float d = r - g;
        const float q = d / delta;
        h = 4.f + q;
    }
    if (h < 0.f) {
        h = h + 6.f;
    }

    return h * 0.1666666716337204f;
}

// The sector of hue h among count, and the fraction of h through it. A non-finite or huge hue
// is sector 0 at fraction 0 rather than an undefined integer conversion.
inline void
colorspaceHueSector(float h,
                    float sectors,
                    int count,
                    int* sector,
                    float* fraction)
{
    const float hs = h * sectors;
    const float fl = std::floor(hs);

    if (!(std::fabs(fl) < 16777216.f)) {
        *sector = 0;
        *fraction = 0.f;

        return;
    }
    *fraction = hs - fl;
    int k = (int)fl % count;
    if (k < 0) {
        k += count;
    }
    *sector = k;
}

// The RGB of hue h with largest channel hi and chroma c.
inline void
colorspaceHexToRgb(float h,
                   float hi,
                   float c,
                   float* v)
{
    int i;
    float f;

    colorspaceHueSector(h, 6.f, 6, &i, &f);
    const float lo = hi - c;
    const float cf = c * f;
    const float up = lo + cf;
    const float down = hi - cf;
    switch (i) {
    case 0:
        v[0] = hi;
        v[1] = up;
        v[2] = lo;
        break;
    case 1:
        v[0] = down;
        v[1] = hi;
        v[2] = lo;
        break;
    case 2:
        v[0] = lo;
        v[1] = hi;
        v[2] = up;
        break;
    case 3:
        v[0] = lo;
        v[1] = down;
        v[2] = hi;
        break;
    case 4:
        v[0] = up;
        v[1] = lo;
        v[2] = hi;
        break;
    default:
        v[0] = hi;
        v[1] = lo;
        v[2] = down;
        break;
    }
}

inline void
colorspaceRgbToHsv(float* v)
{
    const float maxv = (std::max)((std::max)(v[0], v[1]), v[2]);
    const float minv = (std::min)((std::min)(v[0], v[1]), v[2]);
    const float delta = maxv - minv;
    const float h = colorspaceHexHue(v[0], v[1], v[2], maxv, delta);
    float s = 0.f;

    if (maxv != 0.f) {
        s = delta / maxv;
    }
    v[0] = h;
    v[1] = s;
    v[2] = maxv;
}

inline void
colorspaceHsvToRgb(float* v)
{
    const float h = v[0];
    const float val = v[2];
    const float c = val * v[1];

    colorspaceHexToRgb(h, val, c, v);
}

// Lightness and saturation are not clamped, so values outside 0 to 1 round-trip.
inline void
colorspaceRgbToHsl(float* v)
{
    const float maxv = (std::max)((std::max)(v[0], v[1]), v[2]);
    const float minv = (std::min)((std::min)(v[0], v[1]), v[2]);
    const float delta = maxv - minv;
    const float h = colorspaceHexHue(v[0], v[1], v[2], maxv, delta);
    const float sum = maxv + minv;
    const float l = sum * 0.5f;
    const float den = (l <= 0.5f) ? sum : (2.f - sum);
    float s = 0.f;

    if (den != 0.f) {
        s = delta / den;
    }
    v[0] = h;
    v[1] = s;
    v[2] = l;
}

inline void
colorspaceHslToRgb(float* v)
{
    const float h = v[0];
    const float l = v[2];
    const float sum = l + l;
    const float den = (l <= 0.5f) ? sum : (2.f - sum);
    const float c = den * v[1];
    const float half = c * 0.5f;
    const float hi = l + half;

    colorspaceHexToRgb(h, hi, c, v);
}

// The geometric hue, atan2(sqrt(3)(g - b), 2r - g - b), and S = 1 - min/I.
inline void
colorspaceRgbToHsi(float* v)
{
    const float r = v[0];
    const float g = v[1];
    const float b = v[2];
    const float minv = (std::min)((std::min)(r, g), b);
    const float rg = r + g;
    const float sum = rg + b;
    const float i = sum * 0.3333333432674408f;
    float s = 0.f;

    if (i != 0.f) {
        const float q = minv / i;
        s = 1.f - q;
    }
    const float x1 = r - g;
    const float x2 = r - b;
    const float x = x1 + x2;
    const float gb = g - b;
    const float y = gb * 1.7320508075688772f;
    v[0] = colorspaceAtan2Turns(y, x);
    v[1] = s;
    v[2] = i;
}

// Within each third of the hue circle the smallest channel is I(1 - S) and the next is
// I(1 + S cos(H)/cos(60° - H)), written with the angle measured from the sector's middle so the
// series stays within ±60°.
inline void
colorspaceHsiToRgb(float* v)
{
    const float h = v[0];
    const float s = v[1];
    const float i = v[2];
    const float oneMinusS = 1.f - s;
    const float a = i * oneMinusS;
    const float is = i * s;
    int k;
    float f;

    colorspaceHueSector(h, 3.f, 3, &k, &f);
    const float fc = f - 0.5f;
    const float u = fc * 2.094395160675049f;
    const float tanU = colorspaceTanNear(u);
    const float st = tanU * 0.8660253882408142f;
    const float ratio = 0.5f - st;
    const float isr = is * ratio;
    const float c1 = i + isr;
    const float threeI = i * 3.f;
    const float pair = c1 + a;
    const float rest = threeI - pair;
    switch (k) {
    case 0:
        v[0] = c1;
        v[1] = rest;
        v[2] = a;
        break;
    case 1:
        v[0] = a;
        v[1] = c1;
        v[2] = rest;
        break;
    default:
        v[0] = rest;
        v[1] = a;
        v[2] = c1;
        break;
    }
}

inline void
colorspaceRgbToLumaChroma(const ColorspaceSide& sd,
                          float* v)
{
    const float r = v[0];
    const float g = v[1];
    const float b = v[2];
    float y = sd.kr * r;
    const float tg = sd.kg * g;

    y = y + tg;
    const float tb = sd.kb * b;
    y = y + tb;
    const float bd = b - y;
    const float rd = r - y;
    const float pb = bd * sd.cb;
    const float pr = rd * sd.cr;
    const float ys = y * sd.yScale;
    v[0] = ys + sd.yOffset;
    v[1] = pb + sd.cOffset;
    v[2] = pr + sd.cOffset;
}

inline void
colorspaceLumaChromaToRgb(const ColorspaceSide& sd,
                          float* v)
{
    const float yd = v[0] - sd.yOffset;
    const float y = yd * sd.yInv;
    const float pb = v[1] - sd.cOffset;
    const float pr = v[2] - sd.cOffset;
    const float bd = pb * sd.cbInv;
    const float rd = pr * sd.crInv;
    const float r = y + rd;
    const float b = y + bd;
    const float tr = sd.kr * r;
    const float tb = sd.kb * b;
    float gy = y - tr;

    gy = gy - tb;
    v[0] = r;
    v[1] = gy * sd.kgInv;
    v[2] = b;
}

inline void
colorspaceMul3(const float* m,
               float* v)
{
    float o[3];

    for (int r = 0; r < 3; ++r) {
        float s = m[r * 3] * v[0];
        const float g = m[r * 3 + 1] * v[1];
        s = s + g;
        const float b = m[r * 3 + 2] * v[2];
        s = s + b;
        o[r] = s;
    }
    v[0] = o[0];
    v[1] = o[1];
    v[2] = o[2];
}

// CIE's f(t), whose linear segment below (6/29)^3 also takes negative t.
inline float
colorspaceLabF(float t)
{
    if (t > 0.008856451679035631f) {
        return colorspaceCbrtPositive(t);
    }
    const float l = t * 7.787037037037037f;

    return l + 0.13793103448275862f;
}

inline float
colorspaceLabFInv(float u)
{
    if (u > 0.20689655172413793f) {
        const float u2 = u * u;

        return u2 * u;
    }
    const float d = u - 0.13793103448275862f;

    return d * 0.12841854934601665f;
}

inline void
colorspaceXyzToLab(const ColorspaceColorimetry& cm,
                   float* v)
{
    const float x = v[0] * cm.whiteInv[0];
    const float y = v[1] * cm.whiteInv[1];
    const float z = v[2] * cm.whiteInv[2];
    const float fx = colorspaceLabF(x);
    const float fy = colorspaceLabF(y);
    const float fz = colorspaceLabF(z);
    const float l = fy * 116.f;
    const float dxy = fx - fy;
    const float dyz = fy - fz;

    v[0] = l - 16.f;
    v[1] = dxy * 500.f;
    v[2] = dyz * 200.f;
}

inline void
colorspaceLabToXyz(const ColorspaceColorimetry& cm,
                   float* v)
{
    const float l = v[0] + 16.f;
    const float fy = l * 0.008620689655172414f;
    const float da = v[1] * 0.002f;
    const float db = v[2] * 0.005f;
    const float fx = fy + da;
    const float fz = fy - db;

    v[0] = colorspaceLabFInv(fx) * cm.white[0];
    v[1] = colorspaceLabFInv(fy) * cm.white[1];
    v[2] = colorspaceLabFInv(fz) * cm.white[2];
}

// Black, or any colour with no luminance or no chromaticity, takes the white's xy.
inline void
colorspaceXyzToXyY(const ColorspaceColorimetry& cm,
                   float* v)
{
    const float xy = v[0] + v[1];
    const float sum = xy + v[2];
    const float bigY = v[1];

    if ((bigY == 0.f) || (sum == 0.f)) {
        v[0] = cm.whiteXy[0];
        v[1] = cm.whiteXy[1];
        v[2] = bigY;

        return;
    }
    v[0] = v[0] / sum;
    v[1] = bigY / sum;
    v[2] = bigY;
}

inline void
colorspaceXyYToXyz(float* v)
{
    const float x = v[0];
    const float y = v[1];
    const float bigY = v[2];

    if (y == 0.f) {
        v[0] = 0.f;
        v[1] = bigY;
        v[2] = 0.f;

        return;
    }
    const float k = bigY / y;
    const float oneMinusX = 1.f - x;
    const float zc = oneMinusX - y;
    v[0] = x * k;
    v[1] = bigY;
    v[2] = zc * k;
}

inline bool
colorspaceIsXyzModel(ColorspaceModelEnum m)
{
    return (m == eColorspaceModelXYZ) || (m == eColorspaceModelXyY) || (m == eColorspaceModelLab);
}

// To linear RGB, or to XYZ for the XYZ models.
inline void
colorspaceToReference(const ColorspaceColorimetry& cm,
                      const ColorspaceSide& sd,
                      float* v)
{
    switch (sd.model) {
    case eColorspaceModelRGB:
    case eColorspaceModelXYZ:
    case eColorspaceModelCount:
        break;
    case eColorspaceModelHSV:
        colorspaceHsvToRgb(v);
        break;
    case eColorspaceModelHSL:
        colorspaceHslToRgb(v);
        break;
    case eColorspaceModelHSI:
        colorspaceHsiToRgb(v);
        break;
    case eColorspaceModelXyY:
        colorspaceXyYToXyz(v);
        break;
    case eColorspaceModelLab:
        colorspaceLabToXyz(cm, v);
        break;
    default:
        colorspaceLumaChromaToRgb(sd, v);
        break;
    }
}

inline void
colorspaceFromReference(const ColorspaceColorimetry& cm,
                        const ColorspaceSide& sd,
                        float* v)
{
    switch (sd.model) {
    case eColorspaceModelRGB:
    case eColorspaceModelXYZ:
    case eColorspaceModelCount:
        break;
    case eColorspaceModelHSV:
        colorspaceRgbToHsv(v);
        break;
    case eColorspaceModelHSL:
        colorspaceRgbToHsl(v);
        break;
    case eColorspaceModelHSI:
        colorspaceRgbToHsi(v);
        break;
    case eColorspaceModelXyY:
        colorspaceXyzToXyY(cm, v);
        break;
    case eColorspaceModelLab:
        colorspaceXyzToLab(cm, v);
        break;
    default:
        colorspaceRgbToLumaChroma(sd, v);
        break;
    }
}

/**
 * @brief Converts v from the in model to the out model, through linear RGB or XYZ.
 **/
inline void
colorspaceConvert(const ColorspaceColorimetry& cm,
                  const ColorspaceSide& in,
                  const ColorspaceSide& out,
                  float* v)
{
    if (in.model == out.model) {
        return;
    }
    colorspaceToReference(cm, in, v);
    const bool inXyz = colorspaceIsXyzModel(in.model);
    if (inXyz != colorspaceIsXyzModel(out.model)) {
        colorspaceMul3(inXyz ? cm.fromXYZ : cm.toXYZ, v);
    }
    colorspaceFromReference(cm, out, v);
}
} // namespace ColorMath

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Image_ColorMath_h
