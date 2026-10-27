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

#include "MergeOps.h"

#include <cmath>
#include <cstddef>
#include <cstring>

NATRON_NAMESPACE_ENTER

namespace MergeOps {
namespace {

    struct OperatorInfo {
        const char* name;
        bool separable;
        const char* hint;
    };

    constexpr OperatorInfo kOperators[eOperatorCount] = {
#define NATRON_MERGE_INFO(e, name, separable, hint) { name, separable, hint },
        NATRON_MERGE_OPERATORS(NATRON_MERGE_INFO)
#undef NATRON_MERGE_INFO
    };

    // Each operation keeps mergeops.slang's operand order and float literals, so both round alike.
    // Selects are spelled out, as there, so NaN lands on the same side.
    inline float
    fmax2(float x,
          float y)
    {
        return (x < y) ? y : x;
    }

    inline float
    fmin2(float x,
          float y)
    {
        return (y < x) ? y : x;
    }

    inline float
    screen(float A,
           float B)
    {
        if (A <= 1.f && B <= 1.f) {
            return A + B - A * B;
        }

        return fmax2(A, B);
    }

    inline float
    hardLight(float A,
              float B)
    {
        if (A < 0.5f) {
            return 2.f * A * B;
        }

        return screen(2.f * A - 1.f, B);
    }

    inline float
    signedSqrt(float x)
    {
        return (x < 0.f) ? -std::sqrt(-x) : std::sqrt(x);
    }

    template <Operator OP>
    inline float
    separable(float A,
              float a,
              float B,
              float b)
    {
        if constexpr (OP == eATop) {
            return A * b + B * (1.f - a);
        } else if constexpr (OP == eAverage) {
            return (A + B) * 0.5f;
        } else if constexpr (OP == eColorBurn) {
            return (A > 0.f) ? 1.f - (1.f - B) / A : A;
        } else if constexpr (OP == eColorDodge) {
            return (A < 1.f) ? B / (1.f - A) : A;
        } else if constexpr (OP == eConjointOver) {
            if (a >= b) {
                return A;
            }
            if (b <= 0.f) {
                return A + B;
            }

            return A + B * (1.f - a / b);
        } else if constexpr (OP == eCopy) {
            return A;
        } else if constexpr (OP == eDifference) {
            return std::abs(A - B);
        } else if constexpr (OP == eDisjointOver) {
            if (a >= 1.f) {
                return A;
            }
            if (a + b <= 1.f) {
                return A + B;
            }

            return A + B * (1.f - a) / b;
        } else if constexpr (OP == eDivide) {
            return (B != 0.f) ? A / B : 0.f;
        } else if constexpr (OP == eExclusion) {
            return A + B - 2.f * A * B;
        } else if constexpr (OP == eFreeze) {
            return (B > 0.f) ? 1.f - signedSqrt(1.f - A) / B : 0.f;
        } else if constexpr (OP == eFrom) {
            return B - A;
        } else if constexpr (OP == eGeometric) {
            const float s = A + B;

            return (s != 0.f) ? 2.f * A * B / s : 0.f;
        } else if constexpr (OP == eGrainExtract) {
            return B - A + 0.5f;
        } else if constexpr (OP == eGrainMerge) {
            return B + A - 0.5f;
        } else if constexpr (OP == eHardLight) {
            return hardLight(A, B);
        } else if constexpr (OP == eHypot) {
            return std::sqrt(A * A + B * B);
        } else if constexpr (OP == eIn) {
            return A * b;
        } else if constexpr (OP == eMask) {
            return B * a;
        } else if constexpr (OP == eMatte) {
            return A * a + B * (1.f - a);
        } else if constexpr (OP == eMax) {
            return fmax2(A, B);
        } else if constexpr (OP == eMin) {
            return fmin2(A, B);
        } else if constexpr (OP == eMinus) {
            return A - B;
        } else if constexpr (OP == eMultiply) {
            return A * B;
        } else if constexpr (OP == eOut) {
            return A * (1.f - b);
        } else if constexpr (OP == eOver) {
            return A + B * (1.f - a);
        } else if constexpr (OP == eOverlay) {
            return hardLight(B, A);
        } else if constexpr (OP == ePinLight) {
            return fmin2(fmax2(B, 2.f * A - 1.f), 2.f * A);
        } else if constexpr (OP == ePlus) {
            return A + B;
        } else if constexpr (OP == eReflect) {
            return (B < 1.f) ? A * A / (1.f - B) : B;
        } else if constexpr (OP == eScreen) {
            return screen(A, B);
        } else if constexpr (OP == eSoftLight) {
            const float ab = A * B;

            return (ab < 1.f) ? B * (2.f * A + B * (1.f - ab)) : 2.f * ab;
        } else if constexpr (OP == eStencil) {
            return B * (1.f - a);
        } else if constexpr (OP == eUnder) {
            return A * (1.f - b) + B;
        } else {
            static_assert(OP == eXor, "every separable operator has a formula");

            return A * (1.f - b) + B * (1.f - a);
        }
    }

    struct Rgb {
        float r, g, b;
    };

    inline float
    lum(Rgb c)
    {
        return c.r * 0.3f + c.g * 0.59f + c.b * 0.11f;
    }

    inline float
    sat(Rgb c)
    {
        return fmax2(fmax2(c.r, c.g), c.b) - fmin2(fmin2(c.r, c.g), c.b);
    }

    inline Rgb
    setLum(Rgb c,
           float l)
    {
        const float d = l - lum(c);

        return { c.r + d, c.g + d, c.b + d };
    }

    // The PDF's SetSat: the smallest channel goes to 0, the largest to s, the middle one in proportion.
    inline Rgb
    setSat(Rgb c,
           float s)
    {
        const float mn = fmin2(fmin2(c.r, c.g), c.b);
        const float t = fmax2(fmax2(c.r, c.g), c.b) - mn;
        if (!(t > 0.f)) {
            return { 0.f, 0.f, 0.f };
        }
        const float k = s / t;

        return { (c.r - mn) * k, (c.g - mn) * k, (c.b - mn) * k };
    }

    inline Rgb
    scale(Rgb c,
          float k)
    {
        return { c.r * k, c.g * k, c.b * k };
    }

    template <Operator OP>
    inline Rgb
    nonSeparable(Rgb A,
                 float a,
                 Rgb B,
                 float b)
    {
        Rgb blend;
        if constexpr (OP == eHue) {
            blend = setLum(setSat(A, a * sat(B)), a * lum(B));
        } else if constexpr (OP == eSaturation) {
            blend = setLum(setSat(B, b * sat(A)), a * lum(B));
        } else if constexpr (OP == eColor) {
            blend = setLum(scale(A, b), a * lum(B));
        } else {
            static_assert(OP == eLuminosity, "the four non-separable operators");
            blend = setLum(scale(B, a), b * lum(A));
        }
        const float ka = 1.f - a;
        const float kb = 1.f - b;

        return { A.r * kb + B.r * ka + blend.r,
                 A.g * kb + B.g * ka + blend.g,
                 A.b * kb + B.b * ka + blend.b };
    }

    template <Operator OP, int NC>
    void
    rowT(const float* A,
         AlphaSpan a,
         const float* B,
         AlphaSpan b,
         int width,
         float* out)
    {
        for (int x = 0; x < width; ++x) {
            const float* pa = a.data + std::ptrdiff_t(x) * a.pixelStride;
            const float* pb = b.data + std::ptrdiff_t(x) * b.pixelStride;
            float r[NC];
            if constexpr (kOperators[OP].separable) {
#pragma GCC unroll 4
                for (int c = 0; c < NC; ++c) {
                    r[c] = separable<OP>(A[c], pa[c * a.channelStride], B[c], pb[c * b.channelStride]);
                }
            } else if constexpr (NC == 1) {
                r[0] = separable<eOver>(A[0], pa[0], B[0], pb[0]);
            } else {
                // In channel-bit order the colour channels lead and a fourth one is alpha.
                const Rgb ca { A[0], A[1], (NC > 2) ? A[2] : 0.f };
                const Rgb cb { B[0], B[1], (NC > 2) ? B[2] : 0.f };
                const Rgb m = nonSeparable<OP>(ca, pa[0], cb, pb[0]);
                r[0] = m.r;
                r[1] = m.g;
                if constexpr (NC > 2) {
                    r[2] = m.b;
                }
                if constexpr (NC == 4) {
                    r[3] = separable<eOver>(A[3], pa[3 * a.channelStride], B[3], pb[3 * b.channelStride]);
                }
            }
            std::memcpy(out, r, sizeof r);
            A += NC;
            B += NC;
            out += NC;
        }
    }

    template <Operator OP>
    RowFunction
    rowFunctionFor(int nComps)
    {
        switch (nComps) {
        case 1:
            return &rowT<OP, 1>;
        case 2:
            return &rowT<OP, 2>;
        case 3:
            return &rowT<OP, 3>;
        case 4:
            return &rowT<OP, 4>;
        default:
            return nullptr;
        }
    }

} // namespace

const char*
operatorName(Operator op)
{
    return (op >= 0 && op < eOperatorCount) ? kOperators[op].name : "";
}

const char*
operatorHint(Operator op)
{
    return (op >= 0 && op < eOperatorCount) ? kOperators[op].hint : "";
}

bool
isSeparable(Operator op)
{
    return (op >= 0 && op < eOperatorCount) ? kOperators[op].separable : true;
}

bool
operatorFromName(const std::string& name,
                 Operator* op)
{
    for (int i = 0; i < eOperatorCount; ++i) {
        if (name == kOperators[i].name) {
            *op = Operator(i);

            return true;
        }
    }

    return false;
}

RowFunction
rowFunction(Operator op,
            int nComps)
{
    switch (op) {
#define NATRON_MERGE_ROW_CASE(e, name, separable, hint) \
    case e:                                             \
        return rowFunctionFor<e>(nComps);
        NATRON_MERGE_OPERATORS(NATRON_MERGE_ROW_CASE)
#undef NATRON_MERGE_ROW_CASE
    case eOperatorCount:
        break;
    }

    return nullptr;
}

void
mergeRow(Operator op,
         int nComps,
         const float* A,
         AlphaSpan a,
         const float* B,
         AlphaSpan b,
         int width,
         float* out)
{
    const RowFunction f = rowFunction(op, nComps);
    if (f) {
        f(A, a, B, b, width, out);
    }
}

} // namespace MergeOps

NATRON_NAMESPACE_EXIT
