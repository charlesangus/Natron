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

#include "MergeOperators.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <utility>

NATRON_NAMESPACE_ENTER

namespace MergeOperators {
namespace {
    // Expressions below keep ofxsMerging.h's mix of float and double arithmetic on purpose: the
    // native nodes must round exactly where the OFX plugin does.
    const int maxValue = 1;
    typedef float PIX;

    PIX
    grainExtractFunc(PIX A,
                     PIX B)
    {
        return B - A + (PIX)maxValue / 2;
    }

    PIX
    grainMergeFunc(PIX A,
                   PIX B)
    {
        return B + A - (PIX)maxValue / 2;
    }

    PIX
    divideFunc(PIX A,
               PIX B)
    {
        if (B <= 0) {
            return 0;
        }

        return A / B;
    }

    PIX
    exclusionFunc(PIX A,
                  PIX B)
    {
        return PIX(A + B - 2 * A * B / (double)maxValue);
    }

    PIX
    geometricFunc(PIX A,
                  PIX B)
    {
        double sum = (double)A + (double)B;

        if (sum == 0) {
            return 0;
        }

        return 2 * A * B / sum;
    }

    PIX
    multiplyFunc(PIX A,
                 PIX B)
    {
        if ((A < 0) && (B < 0)) {
            return A;
        }

        return PIX(A * B / (double)maxValue);
    }

    PIX
    screenFunc(PIX A,
               PIX B)
    {
        if ((A <= maxValue) || (B <= maxValue)) {
            return PIX(A + B - A * B / (double)maxValue);
        }

        return (std::max)(A, B);
    }

    PIX
    hardLightFunc(PIX A,
                  PIX B)
    {
        if (2 * A < maxValue) {
            return multiplyFunc(2 * A, B);
        }

        return screenFunc(2 * A - maxValue, B);
    }

    PIX
    softLightFunc(PIX A,
                  PIX B)
    {
        double An = A / (double)maxValue;
        double Bn = B / (double)maxValue;

        if (2 * An <= 1) {
            return PIX(maxValue * (Bn - (1 - 2 * An) * Bn * (1 - Bn)));
        } else if (4 * Bn <= 1) {
            return PIX(maxValue * (Bn + (2 * An - 1) * (((16 * Bn - 12) * Bn + 4) * Bn - Bn)));
        }

        return PIX(maxValue * (Bn + (2 * An - 1) * (std::sqrt(Bn) - Bn)));
    }

    PIX
    hypotFunc(PIX A,
              PIX B)
    {
        return PIX(std::sqrt((double)(A * A + B * B)));
    }

    PIX
    overlayFunc(PIX A,
                PIX B)
    {
        return hardLightFunc(B, A);
    }

    PIX
    colorDodgeFunc(PIX A,
                   PIX B)
    {
        if (A >= maxValue) {
            return A;
        }

        return PIX(maxValue * (std::min)(1., B / (maxValue - (double)A)));
    }

    PIX
    colorBurnFunc(PIX A,
                  PIX B)
    {
        if (A <= 0) {
            return A;
        }

        return PIX(maxValue * (1. - (std::min)(1., (maxValue - B) / (double)A)));
    }

    PIX
    pinLightFunc(PIX A,
                 PIX B)
    {
        PIX max2 = PIX((double)maxValue / 2.);

        return A >= max2 ? (std::max)(B, (A - max2) * 2) : (std::min)(B, A * 2);
    }

    PIX
    reflectFunc(PIX A,
                PIX B)
    {
        if (B >= maxValue) {
            return maxValue;
        }

        return PIX((std::min)((double)maxValue, A * A / (double)(maxValue - B)));
    }

    PIX
    freezeFunc(PIX A,
               PIX B)
    {
        if (B <= 0) {
            return 0;
        }

        double An = A / (double)maxValue;
        double Bn = B / (double)maxValue;

        return PIX((std::max)(0., maxValue * (1 - std::sqrt((std::max)(0., 1. - An)) / Bn)));
    }

    PIX
    atopFunc(PIX A,
             PIX B,
             PIX alphaA,
             PIX alphaB)
    {
        return PIX(A * alphaB / (double)maxValue + B * (1. - alphaA / (double)maxValue));
    }

    PIX
    conjointOverFunc(PIX A,
                     PIX B,
                     PIX alphaA,
                     PIX alphaB)
    {
        if (alphaA > alphaB) {
            return A;
        } else if (alphaB <= 0) {
            return A + B;
        }

        return A + B * (1. - (alphaA / (double)alphaB));
    }

    PIX
    disjointOverFunc(PIX A,
                     PIX B,
                     PIX alphaA,
                     PIX alphaB)
    {
        if (alphaA >= maxValue) {
            return A;
        } else if ((alphaA + alphaB) < maxValue) {
            return A + B;
        } else if (alphaB <= 0) {
            return A + B * (1 - alphaA / (double)maxValue);
        }

        return A + B * (maxValue - alphaA) / alphaB;
    }

    PIX
    inFunc(PIX A,
           PIX alphaB)
    {
        return PIX(A * alphaB / (double)maxValue);
    }

    PIX
    matteFunc(PIX A,
              PIX B,
              PIX alphaA)
    {
        return PIX(A * alphaA / (double)maxValue + B * (1. - alphaA / (double)maxValue));
    }

    PIX
    maskFunc(PIX B,
             PIX alphaA)
    {
        return PIX(B * alphaA / (double)maxValue);
    }

    PIX
    outFunc(PIX A,
            PIX alphaB)
    {
        return PIX(A * (1. - alphaB / (double)maxValue));
    }

    PIX
    overFunc(PIX A,
             PIX B,
             PIX alphaA)
    {
        return PIX(A + B * (1 - alphaA / (double)maxValue));
    }

    PIX
    stencilFunc(PIX B,
                PIX alphaA)
    {
        return PIX(B * (1 - alphaA / (double)maxValue));
    }

    PIX
    underFunc(PIX A,
              PIX B,
              PIX alphaB)
    {
        return PIX(A * (1 - alphaB / (double)maxValue) + B);
    }

    PIX
    xorFunc(PIX A,
            PIX B,
            PIX alphaA,
            PIX alphaB)
    {
        return PIX(A * (1 - alphaB / (double)maxValue) + B * (1 - alphaA / (double)maxValue));
    }

    // The non-separable modes are Soren Sandmann Pedersen's pixman-combine-float.c, in double.
    typedef double pixman_float_t;

    struct pixman_rgb_t {
        pixman_float_t r;
        pixman_float_t g;
        pixman_float_t b;
    };

    bool
    isZero(pixman_float_t f)
    {
        return -DBL_MIN < f && f < DBL_MIN;
    }

    pixman_float_t
    channelMin(const pixman_rgb_t* c)
    {
        return (std::min)((std::min)(c->r, c->g), c->b);
    }

    pixman_float_t
    channelMax(const pixman_rgb_t* c)
    {
        return (std::max)((std::max)(c->r, c->g), c->b);
    }

    pixman_float_t
    getLum(const pixman_rgb_t* c)
    {
        return c->r * 0.3f + c->g * 0.59f + c->b * 0.11f;
    }

    pixman_float_t
    getSat(const pixman_rgb_t* c)
    {
        return channelMax(c) - channelMin(c);
    }

    void
    clipColor(pixman_rgb_t* color,
              pixman_float_t a)
    {
        pixman_float_t l = getLum(color);
        pixman_float_t n = channelMin(color);
        pixman_float_t x = channelMax(color);
        pixman_float_t t;

        if (n < 0.0f) {
            t = l - n;
            if (isZero(t)) {
                color->r = 0.0f;
                color->g = 0.0f;
                color->b = 0.0f;
            } else {
                color->r = l + (((color->r - l) * l) / t);
                color->g = l + (((color->g - l) * l) / t);
                color->b = l + (((color->b - l) * l) / t);
            }
        }
        if (x > a) {
            t = x - l;
            if (isZero(t)) {
                color->r = a;
                color->g = a;
                color->b = a;
            } else {
                color->r = l + (((color->r - l) * (a - l) / t));
                color->g = l + (((color->g - l) * (a - l) / t));
                color->b = l + (((color->b - l) * (a - l) / t));
            }
        }
    }

    void
    setLum(pixman_rgb_t* color,
           pixman_float_t sa,
           pixman_float_t l)
    {
        pixman_float_t d = l - getLum(color);

        color->r = color->r + d;
        color->g = color->g + d;
        color->b = color->b + d;

        clipColor(color, sa);
    }

    void
    setSat(pixman_rgb_t* src,
           pixman_float_t sat)
    {
        pixman_float_t* max;
        pixman_float_t* mid;
        pixman_float_t* min;

        if (src->r > src->g) {
            if (src->r > src->b) {
                max = &(src->r);

                if (src->g > src->b) {
                    mid = &(src->g);
                    min = &(src->b);
                } else {
                    mid = &(src->b);
                    min = &(src->g);
                }
            } else {
                max = &(src->b);
                mid = &(src->r);
                min = &(src->g);
            }
        } else {
            if (src->r > src->b) {
                max = &(src->g);
                mid = &(src->r);
                min = &(src->b);
            } else {
                min = &(src->r);

                if (src->g > src->b) {
                    max = &(src->g);
                    mid = &(src->b);
                } else {
                    max = &(src->b);
                    mid = &(src->g);
                }
            }
        }

        pixman_float_t t = *max - *min;

        if (isZero(t)) {
            *mid = *max = 0.0f;
        } else {
            *mid = ((*mid - *min) * sat) / t;
            *max = sat;
        }

        *min = 0.0f;
    }

    void
    blendHslHue(pixman_rgb_t* res,
                const pixman_rgb_t* dest,
                pixman_float_t da,
                const pixman_rgb_t* src,
                pixman_float_t sa)
    {
        res->r = src->r * da;
        res->g = src->g * da;
        res->b = src->b * da;

        setSat(res, getSat(dest) * sa);
        setLum(res, sa * da, getLum(dest) * sa);
    }

    void
    blendHslSaturation(pixman_rgb_t* res,
                       const pixman_rgb_t* dest,
                       pixman_float_t da,
                       const pixman_rgb_t* src,
                       pixman_float_t sa)
    {
        res->r = dest->r * sa;
        res->g = dest->g * sa;
        res->b = dest->b * sa;

        setSat(res, getSat(src) * da);
        setLum(res, sa * da, getLum(dest) * sa);
    }

    void
    blendHslColor(pixman_rgb_t* res,
                  const pixman_rgb_t* dest,
                  pixman_float_t da,
                  const pixman_rgb_t* src,
                  pixman_float_t sa)
    {
        res->r = src->r * da;
        res->g = src->g * da;
        res->b = src->b * da;

        setLum(res, sa * da, getLum(dest) * sa);
    }

    void
    blendHslLuminosity(pixman_rgb_t* res,
                       const pixman_rgb_t* dest,
                       pixman_float_t da,
                       const pixman_rgb_t* src,
                       pixman_float_t sa)
    {
        res->r = dest->r * sa;
        res->g = dest->g * sa;
        res->b = dest->b * sa;

        setLum(res, sa * da, getLum(src) * da);
    }

    struct OperationInfo {
        const char* id;
        const char* hint;
        bool maskable;
        bool identityForBOnly;
        bool separable;
    };

    constexpr OperationInfo kOperations[eOperationCount] = {
        // id, hint, maskable, identityForBOnly, separable
        { "atop", "Ab + B(1 - a) (a.k.a. src-atop)", false, true, true },
        { "average", "(A + B) / 2", true, false, true },
        { "color", "SetLum(A, Lum(B))", false, false, false },
        { "color-burn", "darken B towards A", true, false, true },
        { "color-dodge", "brighten B towards A", true, false, true },
        { "conjoint-over", "A + B(1-a)/b, A if a > b", false, false, true },
        { "copy", "A (a.k.a. src)", false, false, true },
        { "difference", "abs(A-B) (a.k.a. absminus)", true, false, true },
        { "disjoint-over", "A+B(1-a)/b, A+B if a+b < 1", false, false, true },
        { "divide", "A/B, 0 if A < 0 and B < 0", true, false, true },
        { "exclusion", "A+B-2AB", true, true, true },
        { "freeze", "1-sqrt(1-A)/B", true, false, true },
        { "from", "B-A (a.k.a. subtract)", true, false, true },
        { "geometric", "2AB/(A+B)", true, false, true },
        { "grain-extract", "B - A + 0.5", true, false, true },
        { "grain-merge", "B + A - 0.5", true, false, true },
        { "hard-light", "multiply(2*A, B) if A < 0.5, screen(2*A - 1, B) if A > 0.5", true, false, true },
        { "hue", "SetLum(SetSat(A, Sat(B)), Lum(B))", false, false, false },
        { "hypot", "sqrt(A*A+B*B)", true, false, true },
        { "in", "Ab (a.k.a. src-in)", false, false, true },
        { "luminosity", "SetLum(B, Lum(A))", false, false, false },
        { "mask", "Ba (a.k.a dst-in)", false, false, true },
        { "matte", "Aa + B(1-a) (unpremultiplied over)", false, true, true },
        { "max", "max(A, B) (a.k.a. lighten only)", true, false, true },
        { "min", "min(A, B) (a.k.a. darken only)", true, false, true },
        { "minus", "A-B", true, false, true },
        { "multiply", "AB, A if A < 0 and B < 0", true, false, true },
        { "out", "A(1-b) (a.k.a. src-out)", false, false, true },
        { "over", "A+B(1-a) (a.k.a. src-over)", false, true, true },
        { "overlay", "multiply(A, 2*B) if B < 0.5, screen(A, 2*B - 1) if B > 0.5", true, false, true },
        { "pinlight", "if B >= 0.5 then max(A, 2*B - 1), min(A, B * 2) else", true, false, true },
        { "plus", "A+B (a.k.a. add)", true, true, true },
        { "reflect", "A*A / (1 - B)", true, false, true },
        { "saturation", "SetLum(SetSat(B, Sat(A)), Lum(B))", false, false, false },
        { "screen", "A+B-AB if A or B <= 1, otherwise max(A, B)", false, true, true },
        { "soft-light", "burn-in if A < 0.5, lighten if A > 0.5", true, false, true },
        { "stencil", "B(1-a) (a.k.a. dst-out)", false, true, true },
        { "under", "A(1-b)+B (a.k.a. dst-over)", false, true, true },
        { "xor", "A(1-b)+B(1-a)", false, true, true }
    };

    template <Operation OP>
    PIX
    separable(PIX A,
              PIX B,
              PIX a,
              PIX b)
    {
        switch (OP) {
        case eATop:

            return atopFunc(A, B, a, b);
        case eAverage:

            return (A + B) / 2;
        case eColorBurn:

            return colorBurnFunc(A, B);
        case eColorDodge:

            return colorDodgeFunc(A, B);
        case eConjointOver:

            return conjointOverFunc(A, B, a, b);
        case eCopy:

            return A;
        case eDifference:

            return std::abs(A - B);
        case eDisjointOver:

            return disjointOverFunc(A, B, a, b);
        case eDivide:

            return divideFunc(A, B);
        case eExclusion:

            return exclusionFunc(A, B);
        case eFreeze:

            return freezeFunc(A, B);
        case eFrom:

            return B - A;
        case eGeometric:

            return geometricFunc(A, B);
        case eGrainExtract:

            return grainExtractFunc(A, B);
        case eGrainMerge:

            return grainMergeFunc(A, B);
        case eHardLight:

            return hardLightFunc(A, B);
        case eHypot:

            return hypotFunc(A, B);
        case eIn:

            return inFunc(A, b);
        case eMask:

            return maskFunc(B, a);
        case eMatte:

            return matteFunc(A, B, a);
        case eMax:

            return (std::max)(A, B);
        case eMin:

            return (std::min)(A, B);
        case eMinus:

            return A - B;
        case eMultiply:

            return multiplyFunc(A, B);
        case eOut:

            return outFunc(A, b);
        case eOver:

            return overFunc(A, B, a);
        case eOverlay:

            return overlayFunc(A, B);
        case ePinLight:

            return pinLightFunc(A, B);
        case ePlus:

            return A + B;
        case eReflect:

            return reflectFunc(A, B);
        case eScreen:

            return screenFunc(A, B);
        case eSoftLight:

            return softLightFunc(A, B);
        case eStencil:

            return stencilFunc(B, a);
        case eUnder:

            return underFunc(A, B, b);
        case eXOR:

            return xorFunc(A, B, a, b);
        case eColor:
        case eHue:
        case eLuminosity:
        case eSaturation:
        case eOperationCount:
            break;
        }
        assert(false);

        return 0;
    }

    // alphaMasking is the effective setting: already forced on for Matte and off for an operator
    // that is not maskable.
    template <Operation OP, int NC>
    void
    mergePixelT(bool alphaMasking,
                const float* A,
                float a,
                const float* B,
                float b,
                float* out)
    {
        if constexpr (!kOperations[OP].separable) {
            pixman_rgb_t src, dest, res;
            if (isZero(a) || NC < 3) {
                src.r = src.g = src.b = 0;
            } else {
                src.r = A[0] / (pixman_float_t)a;
                src.g = A[1] / (pixman_float_t)a;
                src.b = A[2] / (pixman_float_t)a;
            }
            if (isZero(b) || NC < 3) {
                dest.r = dest.g = dest.b = 0;
            } else {
                dest.r = B[0] / (pixman_float_t)b;
                dest.g = B[1] / (pixman_float_t)b;
                dest.b = B[2] / (pixman_float_t)b;
            }
            pixman_float_t sa = a / (pixman_float_t)maxValue;
            pixman_float_t da = b / (pixman_float_t)maxValue;

            if constexpr (OP == eHue) {
                blendHslHue(&res, &dest, da, &src, sa);
            } else if constexpr (OP == eSaturation) {
                blendHslSaturation(&res, &dest, da, &src, sa);
            } else if constexpr (OP == eColor) {
                blendHslColor(&res, &dest, da, &src, sa);
            } else {
                static_assert(OP == eLuminosity, "the four non-separable operators");
                blendHslLuminosity(&res, &dest, da, &src, sa);
            }
            pixman_float_t R[3] = { res.r, res.g, res.b };
            for (int i = 0; i < (std::min)(NC, 3); ++i) {
                out[i] = PIX((1 - sa) * B[i] + (1 - da) * A[i] + R[i] * maxValue);
            }
            if constexpr (NC == 4) {
                out[3] = PIX(a + b - a * b / (double)maxValue);
            }
        } else {
            int maxComp = NC;
            if (alphaMasking && (NC == 4)) {
                maxComp = 3;
                out[3] = PIX(a + b - a * b / (double)maxValue);
            }
            for (int i = 0; i < maxComp; ++i) {
                out[i] = separable<OP>(A[i], B[i], a, b);
            }
        }
    }

    template <int NC>
    float
    rowSideAlpha(const float* pix,
                 bool present,
                 float opaqueAlpha)
    {
        if constexpr (NC == 4) {
            return pix[3];
        } else if constexpr (NC == 1) {
            return pix[0];
        } else {
            return present ? opaqueAlpha : 0.f;
        }
    }

    // OVER merges A over a running result: a pixel A lacks keeps what out holds.
    template <Operation OP, int NC, bool OVER>
    void
    mergeRowT(bool alphaMasking,
              const RowSide& A,
              const RowSide& B,
              int width,
              float* out)
    {
        const bool masking = (OP == eMatte) || (alphaMasking && kOperations[OP].maskable);
        const float* aPix = A.pixels;
        const float* bPix = B.pixels;

        for (int x = 0; x < width; ++x, aPix += NC, bPix += NC, out += NC) {
            const bool hasA = !A.present || A.present[x];
            const bool hasB = !B.present || B.present[x];
            if constexpr (OVER) {
                if (!hasA) {
                    continue;
                }
            } else if (!hasA && !hasB) {
                for (int c = 0; c < NC; ++c) {
                    out[c] = 0.f;
                }
                continue;
            }
            const float a = rowSideAlpha<NC>(aPix, hasA, A.opaqueAlpha);
            const float b = rowSideAlpha<NC>(bPix, hasB, B.opaqueAlpha);
            mergePixelT<OP, NC>(masking, aPix, a, bPix, b, out);
        }
    }

    typedef void (*PixelFunction)(bool, const float*, float, const float*, float, float*);

    // Entry I is operator I / 4 at I % 4 + 1 components.
    template <std::size_t... I>
    constexpr std::array<PixelFunction, sizeof...(I)>
    makePixelTable(std::index_sequence<I...>)
    {
        return { { &mergePixelT<static_cast<Operation>(I / 4), static_cast<int>(I % 4) + 1>... } };
    }

    template <bool OVER, std::size_t... I>
    constexpr std::array<RowFunction, sizeof...(I)>
    makeRowTable(std::index_sequence<I...>)
    {
        return { { &mergeRowT<static_cast<Operation>(I / 4), static_cast<int>(I % 4) + 1, OVER>... } };
    }

    constexpr std::array<PixelFunction, eOperationCount * 4> kPixelFunctions = makePixelTable(std::make_index_sequence<eOperationCount * 4>());
    constexpr std::array<RowFunction, eOperationCount * 4> kRowFunctions = makeRowTable<false>(std::make_index_sequence<eOperationCount * 4>());
    constexpr std::array<RowFunction, eOperationCount * 4> kOverRowFunctions = makeRowTable<true>(std::make_index_sequence<eOperationCount * 4>());

    bool
    isValid(Operation op,
            int nComps)
    {
        return (op >= 0) && (op < eOperationCount) && (nComps >= 1) && (nComps <= 4);
    }
} // namespace

bool
isMaskable(Operation op)
{
    return kOperations[op].maskable;
}

bool
isIdentityForBOnly(Operation op)
{
    return kOperations[op].identityForBOnly;
}

bool
isSeparable(Operation op)
{
    return kOperations[op].separable;
}

const char*
operationId(Operation op)
{
    return kOperations[op].id;
}

const char*
operationHint(Operation op)
{
    return kOperations[op].hint;
}

bool
operationFromId(const std::string& id,
                Operation* op)
{
    for (int i = 0; i < eOperationCount; ++i) {
        if (id == kOperations[i].id) {
            *op = (Operation)i;

            return true;
        }
    }

    return false;
}

void
mergePixel(Operation op,
           bool alphaMasking,
           const float* A,
           float a,
           const float* B,
           float b,
           int nComps,
           float* out)
{
    assert(isValid(op, nComps));
    alphaMasking = (op == eMatte) || (alphaMasking && isMaskable(op));
    kPixelFunctions[(std::size_t)op * 4 + (nComps - 1)](alphaMasking, A, a, B, b, out);
}

void
mergePixel(Operation op,
           bool alphaMasking,
           const float A[4],
           const float B[4],
           int nComps,
           float out[4])
{
    float a, b;
    if (nComps == 4) {
        a = A[3];
        b = B[3];
    } else if (nComps == 1) {
        a = A[0];
        b = B[0];
    } else {
        a = 1.f;
        b = 1.f;
    }
    mergePixel(op, alphaMasking, A, a, B, b, nComps, out);
}

RowFunction
mergeRowFunction(Operation op,
                 int nComps)
{
    return isValid(op, nComps) ? kRowFunctions[(std::size_t)op * 4 + (nComps - 1)] : 0;
}

RowFunction
mergeOverRowFunction(Operation op,
                     int nComps)
{
    return isValid(op, nComps) ? kOverRowFunctions[(std::size_t)op * 4 + (nComps - 1)] : 0;
}
} // namespace MergeOperators

NATRON_NAMESPACE_EXIT
