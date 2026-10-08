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

#ifndef Engine_Nodes_Merge_MergeOperators_h
#define Engine_Nodes_Merge_MergeOperators_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>

#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The 39 Merge operators as pure float functions over premultiplied colours, with the
 * formulas, branch conditions and evaluation precision of openfx-misc's ofxsMerging.h
 * (instantiated there as PIX = float, maxValue = 1).
 **/
namespace MergeOperators {
/// Declared in the OFX choice order (alphabetical), which is also the knob's option index.
enum Operation {
    eATop = 0,
    eAverage,
    eColor,
    eColorBurn,
    eColorDodge,
    eConjointOver,
    eCopy,
    eDifference,
    eDisjointOver,
    eDivide,
    eExclusion,
    eFreeze,
    eFrom,
    eGeometric,
    eGrainExtract,
    eGrainMerge,
    eHardLight,
    eHue,
    eHypot,
    eIn,
    eLuminosity,
    eMask,
    eMatte,
    eMax,
    eMin,
    eMinus,
    eMultiply,
    eOut,
    eOver,
    eOverlay,
    ePinLight,
    ePlus,
    eReflect,
    eSaturation,
    eScreen,
    eSoftLight,
    eStencil,
    eUnder,
    eXOR,
    eOperationCount
};

/// Alpha masking applies to this operator (otherwise the "Alpha masking" knob is disabled).
bool isMaskable(Operation op);

/// True if the operator returns B when A is black and transparent.
bool isIdentityForBOnly(Operation op);

/// False for Hue, Saturation, Color and Luminosity, which need R, G and B together.
bool isSeparable(Operation op);

/// Option ID of the OFX choice: lower-case, hyphenated ("color-burn", "pinlight", "xor").
const char* operationId(Operation op);

/// Option hint of the OFX choice ("A+B (a.k.a. add)").
const char* operationHint(Operation op);

/// Inverse of operationId(). Returns false and leaves @p op untouched for an unknown ID.
bool operationFromId(const std::string& id,
                     Operation* op);

/**
 * @brief Merges one pixel of premultiplied A over B in [0, 1] float space. @p A, @p B and @p out
 * hold @p nComps (1 to 4) channels and @p out may alias @p B. @p a and @p b are the alphas of
 * the two sides as the caller derived them.
 *
 * With alpha masking (forced on for Matte, ignored for non-maskable operators) and four
 * components, the colour channels go through the operator and alpha is a + b - a*b. The four
 * non-separable operators always write alpha that way, and read colour only when @p nComps >= 3.
 **/
void mergePixel(Operation op,
                bool alphaMasking,
                const float* A,
                float a,
                const float* B,
                float b,
                int nComps,
                float* out);

/**
 * @brief As above with the alphas taken from the pixels the way OpenFX Merge does for a pixel
 * that is present: channel 3 for four components, channel 0 for one component, and 1 otherwise.
 **/
void mergePixel(Operation op,
                bool alphaMasking,
                const float A[4],
                const float B[4],
                int nComps,
                float out[4]);

/**
 * @brief One side of a row merged by a RowFunction. @p pixels holds nComps floats per pixel and
 * is already zero in every channel the caller turned off and in every pixel the side lacks.
 * @p present flags the pixels the side has, null meaning all of them. @p opaqueAlpha is the alpha
 * of a pixel the side has when there are two or three components.
 **/
struct RowSide {
    const float* pixels;
    const unsigned char* present;
    float opaqueAlpha;
};

/**
 * @brief mergePixel() over @p width pixels, with the operator and the component count fixed when
 * the function is looked up, so the row loop does no per-pixel dispatch. A side's alpha is its
 * channel 3 with four components, its channel 0 with one, and otherwise its opaqueAlpha where it
 * has the pixel and 0 where it does not. @p out may alias B's pixels.
 **/
typedef void (*RowFunction)(bool alphaMasking,
                            const RowSide& A,
                            const RowSide& B,
                            int width,
                            float* out);

/// Merges A over B into out, writing zero where neither side has the pixel. Null for an
/// operator or component count out of range.
RowFunction mergeRowFunction(Operation op,
                             int nComps);

/// Merges A over B into out, leaving out as it is where A lacks the pixel. Null for an
/// operator or component count out of range.
RowFunction mergeOverRowFunction(Operation op,
                                 int nComps);
} // namespace MergeOperators

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Merge_MergeOperators_h
