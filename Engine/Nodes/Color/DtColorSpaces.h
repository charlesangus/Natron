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

/*
 * Ported from darktable, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7): the XYZ, xyY,
 * JzAzBz, CIE 2006 LMS, Filmlight Yrg and darktable UCS 22 conversions of
 * src/common/colorspaces_inline_conversions.h, the CAT16 matrices of
 * src/common/chromatic_adaptation.h, src/common/darktable_ucs_22_helpers.h and the JzAzBz
 * gamut LUT of src/iop/colorbalancergb.c. darktable is distributed under the GNU GPL, version 3
 * or (at your option) any later version, and so is this port: GPL-3.0-or-later.
 *
 *     This file is part of darktable,
 *     Copyright (C) 2020-2025 darktable developers.
 *
 *     darktable is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     darktable is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef Engine_Nodes_Color_DtColorSpaces_h
#define Engine_Nodes_Color_DtColorSpaces_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

NATRON_NAMESPACE_ENTER

/**
 * @brief darktable's scene-referred colour-space helpers, shared by the native ports of its
 * colour modules. Everything is in single precision, as in darktable, so that a port follows
 * darktable's rounding. Colours are float[4] (the fourth component is carried, as darktable's
 * dt_aligned_pixel_t, and only some functions touch it). A Mat3 is row-major: out = m·in.
 * The GPU twin is Engine/Gpu/Kernels/dtcolorspaces.slang; keep both in step.
 **/
namespace DtColorSpaces {
constexpr int kGamutLutSize = 512;

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647693f;
constexpr float kHalfPi = 1.57079632679489661923f;

// The D65 white in xyY, as darktable's D65xyY.
constexpr float kD65x = 0.31271f;
constexpr float kD65y = 0.32902f;

// The Yrg chromaticity of the D65 white.
constexpr float kYrgWhiteR = 0.21902143f;
constexpr float kYrgWhiteG = 0.54371398f;

constexpr float kUcsLStarRange = 2.098883786377f;
constexpr float kUcsLStarUpperLimit = 2.09885f;

struct Mat3 {
    float m[3][3];
};

inline void
apply(const Mat3& a,
      const float in[3],
      float out[3])
{
    const float x = in[0];
    const float y = in[1];
    const float z = in[2];

    for (int r = 0; r < 3; ++r) {
        out[r] = a.m[r][0] * x + a.m[r][1] * y + a.m[r][2] * z;
    }
}

inline Mat3
multiply(const Mat3& a,
         const Mat3& b)
{
    Mat3 o = {};

    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            o.m[r][c] = a.m[r][0] * b.m[0][c] + a.m[r][1] * b.m[1][c] + a.m[r][2] * b.m[2][c];
        }
    }

    return o;
}

// darktable builds with -ffast-math, where dt_fast_hypotf is the plain square root.
inline float
fastHypot(float x,
          float y)
{
    return std::sqrt(x * x + y * y);
}

inline float
square(float x)
{
    return x * x;
}

inline float
degToRad(float deg)
{
    return deg * (kPi / 180.f);
}

constexpr Mat3 kXyzD65ToLms2006 = { { { 0.257085f, 0.859943f, -0.031061f },
                                      { -0.394427f, 1.175800f, 0.106423f },
                                      { 0.064856f, -0.076250f, 0.559067f } } };

constexpr Mat3 kLms2006ToXyzD65 = { { { 1.80794659f, -1.29971660f, 0.34785879f },
                                      { 0.61783960f, 0.39595453f, -0.04104687f },
                                      { -0.12546960f, 0.20478038f, 1.74274183f } } };

// Filmlight grading RGB (D65) and CIE 2006 LMS (D65), from darktable's transposed tables.
constexpr Mat3 kGradingRgbToLms = { { { 0.95f, 0.38f, 0.00f },
                                      { 0.05f, 0.62f, 0.03f },
                                      { 0.00f, 0.00f, 0.97f } } };

constexpr Mat3 kLmsToGradingRgb = { { { 1.08771930f, -0.66666667f, 0.02061856f },
                                      { -0.0877193f, 1.66666667f, -0.05154639f },
                                      { 0.f, 0.f, 1.03092784f } } };

constexpr Mat3 kXyzD50ToD65Cat16 = { { { 9.89466254e-01f, -4.00304626e-02f, 4.40530317e-02f },
                                       { -5.40518733e-03f, 1.00666069e+00f, -1.75551955e-03f },
                                       { -4.03920992e-04f, 1.50768030e-02f, 1.30210211e+00f } } };

constexpr Mat3 kXyzD65ToD50Cat16 = { { { 1.01085433e+00f, 4.07086103e-02f, -3.41445825e-02f },
                                       { 5.42814201e-03f, 9.93581926e-01f, 1.15592039e-03f },
                                       { 2.50722468e-04f, -1.14918759e-02f, 7.67964947e-01f } } };

inline void
xyzToLms(const float xyz[3],
         float lms[3])
{
    apply(kXyzD65ToLms2006, xyz, lms);
}

inline void
lmsToXyz(const float lms[3],
         float xyz[3])
{
    apply(kLms2006ToXyzD65, lms, xyz);
}

inline void
gradingRgbToLms(const float rgb[3],
                float lms[3])
{
    apply(kGradingRgbToLms, rgb, lms);
}

inline void
lmsToGradingRgb(const float lms[3],
                float rgb[3])
{
    apply(kLmsToGradingRgb, lms, rgb);
}

inline void
lmsToYrg(const float lms[3],
         float yrg[3])
{
    const float y = 0.68990272f * lms[0] + 0.34832189f * lms[1];
    const float a = lms[0] + lms[1] + lms[2];
    float n[3];
    for (int c = 0; c < 3; ++c) {
        n[c] = (a == 0.f) ? 0.f : lms[c] / a;
    }
    float rgb[3];
    lmsToGradingRgb(n, rgb);
    yrg[0] = y;
    yrg[1] = rgb[0];
    yrg[2] = rgb[1];
}

inline void
yrgToLms(const float yrg[3],
         float lms[3])
{
    const float rgb[3] = { yrg[1], yrg[2], 1.f - yrg[1] - yrg[2] };
    float n[3];
    gradingRgbToLms(rgb, n);
    const float denom = 0.68990272f * n[0] + 0.34832189f * n[1];
    const float a = (denom == 0.f) ? 0.f : yrg[0] / denom;
    for (int c = 0; c < 3; ++c) {
        lms[c] = n[c] * a;
    }
}

/**
 * @brief Yrg in polar form: Y, chroma, and the cosine and sine of the hue around the D65 white.
 **/
inline void
yrgToYch(const float yrg[3],
         float ych[4])
{
    const float r = yrg[1] - kYrgWhiteR;
    const float g = yrg[2] - kYrgWhiteG;
    const float c = fastHypot(g, r);
    ych[0] = yrg[0];
    ych[1] = c;
    ych[2] = (c != 0.f) ? r / c : 1.f;
    ych[3] = (c != 0.f) ? g / c : 0.f;
}

inline void
ychToYrg(const float ych[4],
         float yrg[3])
{
    yrg[0] = ych[0];
    yrg[1] = ych[1] * ych[2] + kYrgWhiteR;
    yrg[2] = ych[1] * ych[3] + kYrgWhiteG;
}

inline void
makeYch(float y,
        float c,
        float h,
        float ych[4])
{
    ych[0] = y;
    ych[1] = c;
    ych[2] = std::cos(h);
    ych[3] = std::sin(h);
}

inline void
ychToGradingRgb(const float ych[4],
                float rgb[3])
{
    float yrg[3];
    float lms[3];
    ychToYrg(ych, yrg);
    yrgToLms(yrg, lms);
    lmsToGradingRgb(lms, rgb);
}

/**
 * @brief Clips the chroma of ych at constant hue and luminance so that it stays inside Yrg.
 **/
inline void
gamutCheckYrg(float ych[4])
{
    float yrg[3];
    ychToYrg(ych, yrg);
    float maxC = ych[1];
    const float cosH = ych[2];
    const float sinH = ych[3];
    if (yrg[1] < 0.f) {
        maxC = std::fmin(-kYrgWhiteR / cosH, maxC);
    }
    if (yrg[2] < 0.f) {
        maxC = std::fmin(-kYrgWhiteG / sinH, maxC);
    }
    if (yrg[1] + yrg[2] > 1.f) {
        maxC = std::fmin((1.f - kYrgWhiteR - kYrgWhiteG) / (cosH + sinH), maxC);
    }
    ych[1] = maxC;
}

namespace JzAzBzConstants {
    constexpr float b = 1.15f;
    constexpr float g = 0.66f;
    constexpr float c1 = 0.8359375f;
    constexpr float c2 = 18.8515625f;
    constexpr float c3 = 18.6875f;
    constexpr float n = 0.159301758f;
    constexpr float p = 134.034375f;
    constexpr float d = -0.56f;
    constexpr float d0 = 1.6295499532821566e-11f;

    constexpr Mat3 kM = { { { 0.41478972f, 0.57999900f, 0.01464800f },
                            { -0.2015100f, 1.1206490f, 0.0531008f },
                            { -0.0166008f, 0.2648000f, 0.6684799f } } };

    constexpr Mat3 kA = { { { 0.5f, 0.5f, 0.0f },
                            { 3.524000f, -4.066708f, 0.542708f },
                            { 0.199076f, 1.096799f, -1.295875f } } };

    // IzAzBz to LMS: LMS[i] = Iz + az·kAI[1][i] + bz·kAI[2][i] (darktable's AI_trans).
    constexpr float kAI[3][3] = { { 1.0f, 1.0f, 1.0f },
                                  { 0.1386050432715393f, -0.1386050432715393f, -0.0960192420263190f },
                                  { 0.0580473161561189f, -0.0580473161561189f, -0.8118918960560390f } };

    constexpr Mat3 kMI = { { { 1.9242264357876067f, -1.0047923125953657f, 0.0376514040306180f },
                             { 0.3503167620949991f, 0.7264811939316552f, -0.0653844229480850f },
                             { -0.0909828109828475f, -0.3127282905230739f, 1.5227665613052603f } } };
} // namespace JzAzBzConstants

/**
 * @brief CIE XYZ D65 (Y = 1 for diffuse white) to JzAzBz.
 **/
inline void
xyzToJzAzBz(const float xyzD65[3],
            float jab[3])
{
    using namespace JzAzBzConstants;
    const float xyz[3] = { b * xyzD65[0] - (b - 1.0f) * xyzD65[2],
                           g * xyzD65[1] - (g - 1.0f) * xyzD65[0],
                           xyzD65[2] };
    float lms[3];
    apply(kM, xyz, lms);
    for (int i = 0; i < 3; ++i) {
        lms[i] = std::pow(std::fmax(lms[i] / 10000.f, 0.0f), n);
        lms[i] = std::pow((c1 + c2 * lms[i]) / (1.0f + c3 * lms[i]), p);
    }
    apply(kA, lms, jab);
    jab[0] = std::fmax(((1.0f + d) * jab[0]) / (1.0f + d * jab[0]) - d0, 0.f);
}

/**
 * @brief The Iz of a JzAzBz lightness, as darktable's inverse computes it.
 **/
inline float
jzToIz(float jz)
{
    using namespace JzAzBzConstants;
    const float iz = jz + d0;

    return std::fmax(iz / (1.0f + d - d * iz), 0.f);
}

inline void
jzAzBzToXyz(const float jab[3],
            float xyzD65[3])
{
    using namespace JzAzBzConstants;
    const float iab[3] = { jzToIz(jab[0]), jab[1], jab[2] };
    float lms[3];
    for (int i = 0; i < 3; ++i) {
        lms[i] = iab[0] * kAI[0][i] + iab[1] * kAI[1][i] + iab[2] * kAI[2][i];
        lms[i] = std::fmax(lms[i], 0.f);
        lms[i] = std::pow(lms[i], 1.0f / p);
        lms[i] = (c1 - lms[i]) / (c3 * lms[i] - c2);
        lms[i] = std::fmax(lms[i], 0.f);
        lms[i] = 10000.f * std::pow(lms[i], 1.0f / n);
    }
    float xyz[3];
    apply(kMI, lms, xyz);
    xyzD65[0] = (xyz[0] + (b - 1.0f) * xyz[2]) / b;
    xyzD65[1] = (xyz[1] + (g - 1.0f) * xyzD65[0]) / g;
    xyzD65[2] = xyz[2];
}

/**
 * @brief XYZ D65 to xyY; negative XYZ are clipped first and black takes the D65 white's xy.
 **/
inline void
d65XyzToXyY(const float sxyz[3],
            float xyY[3])
{
    const float xyz[3] = { std::fmax(sxyz[0], 0.f), std::fmax(sxyz[1], 0.f), std::fmax(sxyz[2], 0.f) };
    const float sum = xyz[0] + xyz[1] + xyz[2];
    xyY[0] = (sum > 0.0f) ? xyz[0] / sum : kD65x;
    xyY[1] = (sum > 0.0f) ? xyz[1] / sum : kD65y;
    xyY[2] = xyz[1];
}

inline void
xyYToXyz(const float xyY[3],
         float xyz[3])
{
    const bool bad = xyY[1] == 0.0f;
    xyz[0] = bad ? 0.0f : xyY[2] * xyY[0] / xyY[1];
    xyz[1] = bad ? 0.0f : xyY[2];
    xyz[2] = bad ? 0.0f : xyY[2] * (1.f - xyY[0] - xyY[1]) / xyY[1];
}

inline float
yToUcsLStar(float y)
{
    const float yHat = std::pow(y, 0.631651345306265f);

    return kUcsLStarRange * yHat / (yHat + 1.12426773749357f);
}

inline float
ucsLStarToY(float lStar)
{
    return std::pow((1.12426773749357f * lStar / (kUcsLStarRange - lStar)), 1.5831518565279648f);
}

inline void
xyYToUcsUV(const float xyY[3],
           float uvStarPrime[2])
{
    const float xFactors[3] = { -0.783941002840055f, 0.745273540913283f, 0.318707282433486f };
    const float yFactors[3] = { 0.277512987809202f, -0.205375866083878f, 2.16743692732158f };
    const float offsets[3] = { 0.153836578598858f, -0.165478376301988f, 0.291320554395942f };
    float uvd[3];
    for (int c = 0; c < 3; ++c) {
        uvd[c] = xFactors[c] * xyY[0] + yFactors[c] * xyY[1] + offsets[c];
    }
    const float div = (uvd[2] >= 0.0f) ? std::max(FLT_MIN, uvd[2]) : std::min(-FLT_MIN, uvd[2]);
    uvd[0] /= div;
    uvd[1] /= div;
    const float factors[2] = { 1.39656225667f, 1.4513954287f };
    const float halfValues[2] = { 1.49217352929f, 1.52488637914f };
    float uvStar[2];
    for (int c = 0; c < 2; ++c) {
        uvStar[c] = factors[c] * uvd[c] / (std::fabs(uvd[c]) + halfValues[c]);
    }
    uvStarPrime[0] = -1.124983854323892f * uvStar[0] - 0.980483721769325f * uvStar[1];
    uvStarPrime[1] = 1.86323315098672f * uvStar[0] + 1.971853092390862f * uvStar[1];
}

/**
 * @brief The chroma darktable UCS gives lightness L* (in units of L_white) and squared
 * colourfulness M².
 **/
inline float
ucsChroma(float lStar,
          float lWhite,
          float m2)
{
    return 15.932993652962535f * std::pow(lStar, 0.6523997524738018f) * std::pow(m2, 0.6007557017508491f) / lWhite;
}

inline void
ucsLUVToJCH(float lStar,
            float lWhite,
            const float uvStarPrime[2],
            float jch[3])
{
    const float m2 = uvStarPrime[0] * uvStarPrime[0] + uvStarPrime[1] * uvStarPrime[1];
    jch[0] = lStar / lWhite;
    jch[1] = ucsChroma(lStar, lWhite, m2);
    jch[2] = std::atan2(uvStarPrime[1], uvStarPrime[0]);
}

/**
 * @brief xyY (Y = 1 for diffuse white) to darktable UCS 22 JCH, lightness relative to lWhite,
 * the L* of the scene's peak white (yToUcsLStar of it).
 **/
inline void
xyYToUcsJCH(const float xyY[3],
            float lWhite,
            float jch[3])
{
    float uv[2];
    xyYToUcsUV(xyY, uv);
    ucsLUVToJCH(yToUcsLStar(xyY[2]), lWhite, uv, jch);
}

inline void
ucsJCHToXyY(const float jch[3],
            float lWhite,
            float xyY[3])
{
    const float lStar = std::min(std::max(jch[0] * lWhite, 0.f), kUcsLStarUpperLimit);
    const float m = (lStar != 0.f)
        ? std::pow(jch[1] * lWhite / (15.932993652962535f * std::pow(lStar, 0.6523997524738018f)), 0.8322850678616855f)
        : 0.f;
    const float uStarPrime = m * std::cos(jch[2]);
    const float vStarPrime = m * std::sin(jch[2]);
    const float uvStar[2] = { -5.037522385190711f * uStarPrime - 2.504856328185843f * vStarPrime,
                              4.760029407436461f * uStarPrime + 2.874012963239247f * vStarPrime };
    const float factors[2] = { 1.39656225667f, 1.4513954287f };
    const float halfValues[2] = { 1.49217352929f, 1.52488637914f };
    float uv[2];
    for (int c = 0; c < 2; ++c) {
        uv[c] = -halfValues[c] * uvStar[c] / (std::fabs(uvStar[c]) - factors[c]);
    }
    const float uFactors[3] = { 0.167171472114775f, -0.150959086409163f, 0.940254742367256f };
    const float vFactors[3] = { 0.141299802443708f, -0.155185060382272f, 1.000000000000000f };
    const float offsets[3] = { -0.00801531300850582f, -0.00843312433578007f, -0.0256325967652889f };
    float xyD[3];
    for (int c = 0; c < 3; ++c) {
        xyD[c] = uFactors[c] * uv[0] + vFactors[c] * uv[1] + offsets[c];
    }
    const float div = (xyD[2] >= 0.0f) ? std::max(FLT_MIN, xyD[2]) : std::min(-FLT_MIN, xyD[2]);
    xyY[0] = xyD[0] / div;
    xyY[1] = xyD[1] / div;
    xyY[2] = ucsLStarToY(lStar);
}

constexpr float kUcsBrightnessExponent = 1.33654221029386f;

inline void
ucsJCHToHSB(const float jch[3],
            float hsb[3])
{
    hsb[2] = jch[0] * (std::pow(jch[1], kUcsBrightnessExponent) + 1.f);
    hsb[1] = (hsb[2] > 0.f) ? jch[1] / hsb[2] : 0.f;
    hsb[0] = jch[2];
}

inline void
ucsHSBToJCH(const float hsb[3],
            float jch[3])
{
    const float h = hsb[0];
    const float c = hsb[1] * hsb[2];
    jch[2] = h;
    jch[1] = c;
    jch[0] = hsb[2] / (std::pow(c, kUcsBrightnessExponent) + 1.f);
}

inline void
ucsJCHToHCB(const float jch[3],
            float hcb[3])
{
    const float b = jch[0] * (std::pow(jch[1], kUcsBrightnessExponent) + 1.f);
    hcb[1] = jch[1];
    hcb[0] = jch[2];
    hcb[2] = b;
}

inline void
ucsHCBToJCH(const float hcb[3],
            float jch[3])
{
    const float h = hcb[0];
    const float c = hcb[1];
    jch[0] = hcb[2] / (std::pow(c, kUcsBrightnessExponent) + 1.f);
    jch[1] = c;
    jch[2] = h;
}

/**
 * @brief h1 - h2 wrapped into [-pi, pi].
 **/
inline float
deltaH(float h1,
       float h2)
{
    float diff = h1 - h2;
    diff += (diff < -kPi) ? kTwoPi : 0.f;
    diff -= (diff > kPi) ? kTwoPi : 0.f;

    return diff;
}

/**
 * @brief x past softThreshold eases exponentially towards hardThreshold, which must exceed it.
 **/
inline float
softClip(float x,
         float softThreshold,
         float hardThreshold)
{
    const float norm = hardThreshold - softThreshold;

    return (x > softThreshold) ? softThreshold + (1.f - std::exp(-(x - softThreshold) / norm)) * norm : x;
}

/**
 * @brief The gamut LUT at hue (radians in [-pi, pi)), linearly interpolated around the ring.
 **/
inline float
lookupGamut(const float lut[kGamutLutSize],
            float hue)
{
    const float x = (float)kGamutLutSize * (hue + kPi) / kTwoPi;
    const float xPrev = std::floor(x);
    const float xNext = std::ceil(x);
    const int xi = (int)xPrev & (kGamutLutSize - 1);
    const int xii = (int)xNext & (kGamutLutSize - 1);
    const float yPrev = lut[xi];

    return yPrev + ((xi != xii) ? (x - xPrev) * (lut[xii] - yPrev) : 0.0f);
}

inline int
gamutLutIndex(float hue)
{
    int index = (int)std::round((float)(kGamutLutSize - 1) * (hue + kPi) / kTwoPi);
    index += (index < 0) ? kGamutLutSize : 0;
    index -= (index >= kGamutLutSize) ? kGamutLutSize : 0;

    return index;
}

/**
 * @brief The JzAzBz gamut boundary of the RGB space whose matrix to XYZ D65 is rgbToXyzD65: the
 * largest saturation C/J per hue over a 92³ sampling of the unit cube, box-filtered over five
 * entries around the ring.
 **/
inline void
buildJzAzBzGamutLut(const Mat3& rgbToXyzD65,
                    float lut[kGamutLutSize])
{
    constexpr int kSteps = 92;
    float sampler[kGamutLutSize] = {};

    for (int r = 0; r < kSteps; ++r) {
        for (int g = 0; g < kSteps; ++g) {
            for (int b = 0; b < kSteps; ++b) {
                const float rgb[3] = { (float)r / (float)(kSteps - 1), (float)g / (float)(kSteps - 1), (float)b / (float)(kSteps - 1) };
                float xyz[3];
                apply(rgbToXyzD65, rgb, xyz);
                float jab[3];
                xyzToJzAzBz(xyz, jab);
                const float c = fastHypot(jab[2], jab[1]);
                const float saturation = (jab[0] > 0.f) ? c / jab[0] : 0.f;
                const int index = gamutLutIndex(std::atan2(jab[2], jab[1]));
                sampler[index] = std::fmax(sampler[index], saturation);
            }
        }
    }
    for (int k = 0; k < kGamutLutSize; ++k) {
        float sum = 0.f;
        for (int o = -2; o <= 2; ++o) {
            sum += sampler[(k + o + kGamutLutSize) & (kGamutLutSize - 1)];
        }
        lut[k] = sum / 5.f;
    }
}

/**
 * @brief The darktable UCS 22 gamut boundary of the RGB space whose matrix to XYZ D65 is
 * rgbToXyzD65, as the squared colourfulness M² per hue, marching the primaries' triangle in xy.
 **/
inline void
buildUcsGamutLut(const Mat3& rgbToXyzD65,
                 float lut[kGamutLutSize])
{
    float sampler[kGamutLutSize] = {};
    for (int k = 0; k < kGamutLutSize; ++k) {
        lut[k] = 0.f;
    }

    float xyY[3][3];
    for (int p = 0; p < 3; ++p) {
        const float xyz[3] = { rgbToXyzD65.m[0][p], rgbToXyzD65.m[1][p], rgbToXyzD65.m[2][p] };
        d65XyzToXyY(xyz, xyY[p]);
    }
    const float* red = xyY[0];
    const float* green = xyY[1];
    const float* blue = xyY[2];
    const float hRed = std::atan2(red[1] - kD65y, red[0] - kD65x);
    const float hGreen = std::atan2(green[1] - kD65y, green[0] - kD65x);
    const float hBlue = std::atan2(blue[1] - kD65y, blue[0] - kD65x);

    const auto inUnit = [](float t) {
        return t == std::min(std::max(t, 0.f), 1.f);
    };
    const int count = 50 * kGamutLutSize;
    for (int i = 0; i < count; ++i) {
        const float angle = -kPi + ((float)i) / (float)count * kTwoPi;
        const float tanAngle = std::tan(angle);

        const float t1 = deltaH(angle, hBlue) / deltaH(hRed, hBlue);
        const float t2 = deltaH(angle, hRed) / deltaH(hGreen, hRed);
        const float t3 = deltaH(angle, hGreen) / deltaH(hBlue, hGreen);

        const float* from = nullptr;
        const float* to = nullptr;
        if (inUnit(t1)) {
            from = blue;
            to = red;
        } else if (inUnit(t2)) {
            from = red;
            to = green;
        } else if (inUnit(t3)) {
            from = green;
            to = blue;
        }
        float xt = 0.f;
        float yt = 0.f;
        if (from) {
            const float t = (kD65y - from[1] + tanAngle * (from[0] - kD65x))
                / (to[1] - from[1] + tanAngle * (from[0] - to[0]));
            xt = from[0] + t * (to[0] - from[0]);
            yt = from[1] + t * (to[1] - from[1]);
        }

        const float edge[3] = { xt, yt, 1.f };
        float uv[2];
        xyYToUcsUV(edge, uv);
        const int index = gamutLutIndex(std::atan2(uv[1], uv[0]));
        lut[index] += uv[0] * uv[0] + uv[1] * uv[1];
        sampler[index] += 1.0f;
    }
    for (int k = 0; k < kGamutLutSize; ++k) {
        lut[k] = lut[k] / std::fmax(1.0f, sampler[k]);
    }
}
} // namespace DtColorSpaces

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Color_DtColorSpaces_h
