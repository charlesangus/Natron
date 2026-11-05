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

#ifndef Engine_Nodes_Generator_ShapeGenerator_h
#define Engine_Nodes_Generator_ShapeGenerator_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstdint>
#include <string>
#include <type_traits>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Generator/ShapeFalloff.h"

#define kShapeParamColor0 "color0"
#define kShapeParamColor1 "color1"
#define kShapeParamFalloff "falloff"
#define kShapeParamSoftness "softness"

NATRON_NAMESPACE_ENTER

class KnobReader;
class NativeGenerator;

// What Ramp, Radial, Rectangle and ColorWheel share: their knobs, and the mapping of a pixel at a
// render scale to canonical coordinates. Every shape is evaluated in canonical space, so a proxy
// render samples the same shape as the full one.
namespace ShapeGenerator {

// Bounds on what reaches the float maths, so that knob values anywhere in the double range keep
// every product finite: debug builds trap on overflow and invalid operations.
constexpr double kMaxCoordinate = 1e6;
constexpr double kMaxColor = 1e18;
// A shape narrower than this is empty: its normalised coordinates would overflow.
constexpr double kMinShapeSize = 1e-3;

inline constexpr double kColor0Default[4] = { 0., 0., 0., 0. };
inline constexpr double kColor1Default[4] = { 1., 1., 1., 1. };

/**
 * @brief The canonical box of a node's extent and its pixel aspect ratio.
 **/
struct ExtentBox {
    double x1;
    double y1;
    double x2;
    double y2;
    double par;
};

/**
 * @brief The extent box at the reader's time and view; an empty box at aspect ratio 1 without a
 * generator.
 **/
ExtentBox readExtentBox(const NativeGenerator* generator,
                        const KnobReader& reader);

/**
 * @brief v as a float clamped to [-limit, limit], and fallback for NaN.
 **/
float boundedFloat(double v, double limit, float fallback);

KnobColorPtr addColorKnob(KnobHolder* holder,
                          const KnobPagePtr& page,
                          const char* name,
                          const char* label,
                          const char* hint,
                          const double defaults[4]);

/**
 * @brief The falloff choice, its options in ShapeFalloff's index order.
 **/
KnobChoicePtr addFalloffKnob(KnobHolder* holder,
                             const KnobPagePtr& page,
                             const char* hint,
                             int defaultFalloff);

/**
 * @brief The softness of an edge in canonical units, fading inward from it.
 **/
KnobDoublePtr addSoftnessKnob(KnobHolder* holder,
                              const KnobPagePtr& page,
                              const char* hint);

void readColor(const KnobReader& reader,
               const KnobColorWPtr& knob,
               const double fallback[4],
               float out[4]);

int readFalloff(const KnobReader& reader,
                const KnobChoiceWPtr& knob,
                int fallback);

float readSoftness(const KnobReader& reader,
                   const KnobDoubleWPtr& knob);

/**
 * @brief The canonical size of a pixel at render scale (renderScaleX, renderScaleY) for pixel
 * aspect ratio par: pixel (x, y) covers canonical [x, x + 1) pixelW by [y, y + 1) pixelH.
 **/
void pixelSize(double par,
               double renderScaleX,
               double renderScaleY,
               float* pixelW,
               float* pixelH);

/**
 * @brief The colour at weight w of colour1 against colour0, unclamped. The ends are exact.
 **/
inline void
lerpColor(const float color0[4],
          const float color1[4],
          float w,
          float out[4])
{
    const float v = 1.f - w;

    for (int c = 0; c < 4; ++c) {
        out[c] = v * color0[c] + w * color1[c];
    }
}

/**
 * @brief Copies a node's baked colour0 and colour1 into its pixel-params block.
 **/
template <typename PARAMS, typename PIXEL>
inline void
copyColors(const PARAMS& params,
           PIXEL* pixel)
{
    for (int c = 0; c < 4; ++c) {
        pixel->color0[c] = params.color0[c];
        pixel->color1[c] = params.color1[c];
    }
}

} // namespace ShapeGenerator

/**
 * @brief A box shape at one render scale, in plain 32-bit words: the block Radial's and
 * Rectangle's GPU kernels read and their CPU rows evaluate. The box is in canonical coordinates;
 * cornerX and cornerY are Rectangle's and zero for Radial.
 **/
struct ShapeBoxPixelParams {
    float x1;
    float y1;
    float x2;
    float y2;
    float cornerX;
    float cornerY;
    float softness;
    std::int32_t falloff;
    float pixelW;
    float pixelH;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    float color0[4];
    float color1[4];
};

static_assert(std::is_trivially_copyable<ShapeBoxPixelParams>::value, "ShapeBoxPixelParams is copied to the GPU");
static_assert(sizeof(ShapeBoxPixelParams) == 80, "ShapeBoxPixelParams is a fixed GPU block");

/**
 * @brief The box params for a region in canonical coordinates. A box narrower or shorter than
 * ShapeGenerator::kMinShapeSize collapses to an empty one, which gives colour0 everywhere.
 **/
ShapeBoxPixelParams shapeBoxPixelParams(const ShapeGenerator::ExtentBox& extent,
                                        double renderScaleX,
                                        double renderScaleY);

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Generator_ShapeGenerator_h
