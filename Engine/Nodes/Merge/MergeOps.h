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

#ifndef Engine_Nodes_Merge_MergeOps_h
#define Engine_Nodes_Merge_MergeOps_h

// No Python.h: Tests/Gpu compiles MergeOps.cpp without Python to hold it to its Slang twin.
#include <string>

#include "Global/Macros.h"

NATRON_NAMESPACE_ENTER

/**
 * The Merge operators on premultiplied pixels, A over B. The CPU twin of
 * Engine/Gpu/Kernels/mergeops.slang, whose header lists the formulas and every deviation from
 * Nuke's. Operator values are the Slang module's indices and the option order of the node's
 * operation choice.
 */
namespace MergeOps {

// X(enumerator, option name, separable, hint)
#define NATRON_MERGE_OPERATORS(X)                                                                \
    X(eATop, "atop", true, "Ab + B(1 - a)")                                                      \
    X(eAverage, "average", true, "(A + B) / 2")                                                  \
    X(eColor, "color", false, "SetLum(A, Lum(B))")                                               \
    X(eColorBurn, "color-burn", true, "1 - (1 - B) / A, A if A <= 0")                            \
    X(eColorDodge, "color-dodge", true, "B / (1 - A), A if A >= 1")                              \
    X(eConjointOver, "conjoint-over", true, "A + B(1 - a/b), A if a >= b")                       \
    X(eCopy, "copy", true, "A")                                                                  \
    X(eDifference, "difference", true, "abs(A - B)")                                             \
    X(eDisjointOver, "disjoint-over", true, "A + B(1 - a)/b, A + B if a + b <= 1")               \
    X(eDivide, "divide", true, "A / B, 0 if B = 0")                                              \
    X(eExclusion, "exclusion", true, "A + B - 2AB")                                              \
    X(eFreeze, "freeze", true, "1 - sqrt(1 - A) / B")                                            \
    X(eFrom, "from", true, "B - A")                                                              \
    X(eGeometric, "geometric", true, "2AB / (A + B)")                                            \
    X(eGrainExtract, "grain-extract", true, "B - A + 0.5")                                       \
    X(eGrainMerge, "grain-merge", true, "B + A - 0.5")                                           \
    X(eHardLight, "hard-light", true, "multiply(2A, B) if A < 0.5, screen(2A - 1, B) otherwise") \
    X(eHue, "hue", false, "SetLum(SetSat(A, Sat(B)), Lum(B))")                                   \
    X(eHypot, "hypot", true, "sqrt(A*A + B*B)")                                                  \
    X(eIn, "in", true, "Ab")                                                                     \
    X(eLuminosity, "luminosity", false, "SetLum(B, Lum(A))")                                     \
    X(eMask, "mask", true, "Ba")                                                                 \
    X(eMatte, "matte", true, "Aa + B(1 - a)")                                                    \
    X(eMax, "max", true, "max(A, B)")                                                            \
    X(eMin, "min", true, "min(A, B)")                                                            \
    X(eMinus, "minus", true, "A - B")                                                            \
    X(eMultiply, "multiply", true, "AB")                                                         \
    X(eOut, "out", true, "A(1 - b)")                                                             \
    X(eOver, "over", true, "A + B(1 - a)")                                                       \
    X(eOverlay, "overlay", true, "multiply(A, 2B) if B < 0.5, screen(A, 2B - 1) otherwise")      \
    X(ePinLight, "pinlight", true, "min(max(B, 2A - 1), 2A)")                                    \
    X(ePlus, "plus", true, "A + B")                                                              \
    X(eReflect, "reflect", true, "A*A / (1 - B), B if B >= 1")                                   \
    X(eSaturation, "saturation", false, "SetLum(SetSat(B, Sat(A)), Lum(B))")                     \
    X(eScreen, "screen", true, "A + B - AB if A and B <= 1, max(A, B) otherwise")                \
    X(eSoftLight, "soft-light", true, "B(2A + B(1 - AB)) if AB < 1, 2AB otherwise")              \
    X(eStencil, "stencil", true, "B(1 - a)")                                                     \
    X(eUnder, "under", true, "A(1 - b) + B")                                                     \
    X(eXor, "xor", true, "A(1 - b) + B(1 - a)")

enum Operator {
#define NATRON_MERGE_ENUMERATOR(e, name, separable, hint) e,
    NATRON_MERGE_OPERATORS(NATRON_MERGE_ENUMERATOR)
#undef NATRON_MERGE_ENUMERATOR
        eOperatorCount
};

const char* operatorName(Operator op);

const char* operatorHint(Operator op);

bool isSeparable(Operator op);

bool operatorFromName(const std::string& name, Operator* op);

/**
 * Where a row's alphas are: the alpha of channel c of pixel x is data[x * pixelStride + c * channelStride].
 * A channelStride of 0 broadcasts one alpha to every channel; 1 reads a per-channel alpha layer.
 */
struct AlphaSpan {
    const float* data;
    int pixelStride;
    int channelStride;
};

/**
 * Merges width pixels of A over B into out, all nComps-interleaved. Channels are taken in
 * Natron's channel-bit order, a single channel being alpha, and a non-separable operator blends
 * the colour channels with the alphas of the first channel, absent colour channels reading zero.
 * out may alias A or B.
 */
typedef void (*RowFunction)(const float* A, AlphaSpan a, const float* B, AlphaSpan b, int width, float* out);

RowFunction rowFunction(Operator op, int nComps);

void mergeRow(Operator op, int nComps, const float* A, AlphaSpan a, const float* B, AlphaSpan b, int width, float* out);

} // namespace MergeOps

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Merge_MergeOps_h
