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

#ifndef Engine_Nodes_Generator_ColorWheel_h
#define Engine_Nodes_Generator_ColorWheel_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cmath>
#include <cstdint>
#include <type_traits>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Generator/ShapeGenerator.h"
#include "Engine/Nodes/Image/NativeGenerator.h"

#define PLUGINID_NATRON_COLORWHEEL "ColorWheel"
#define PLUGIN_MAJOR_NATRON_COLORWHEEL 1

#define kColorWheelParamCenterSaturation "centerSaturation"
#define kColorWheelParamEdgeSaturation "edgeSaturation"
#define kColorWheelParamCenterValue "centerValue"
#define kColorWheelParamEdgeValue "edgeValue"
#define kColorWheelParamRotate "rotate"
#define kColorWheelParamSoftness kShapeParamSoftness

NATRON_NAMESPACE_ENTER

/**
 * @brief A ColorWheel at one render scale, in plain 32-bit words: the block the GPU kernel reads and
 * the CPU row evaluates. The disc is in canonical coordinates; a radius of zero draws nothing.
 * rotateTurns is the rotation in turns, in [0, 1).
 **/
struct ColorWheelPixelParams {
    float cx;
    float cy;
    float radius;
    float softness;
    float pixelW;
    float pixelH;
    float centerSaturation;
    float edgeSaturation;
    float centerValue;
    float edgeValue;
    float rotateTurns;
    std::uint32_t reserved;
};

static_assert(std::is_trivially_copyable<ColorWheelPixelParams>::value, "ColorWheelPixelParams is copied to the GPU");
static_assert(sizeof(ColorWheelPixelParams) == 48, "ColorWheelPixelParams is a fixed GPU block");

/**
 * @brief HSV to RGB with hue in turns, as the openfx-misc Color::hsv_to_rgb with a unit hue circle.
 **/
inline void
colorWheelHsvToRgb(float h,
                   float s,
                   float v,
                   float rgb[3])
{
    if (s == 0.f) {
        rgb[0] = rgb[1] = rgb[2] = v;

        return;
    }
    h *= 6.f;
    const float sector = std::floor(h);
    const float f = h - sector;
    int i = (int)sector % 6;
    if (i < 0) {
        i += 6;
    }
    const float p = v * (1.f - s);
    const float q = v * (1.f - s * f);
    const float t = v * (1.f - s * (1.f - f));
    switch (i) {
    case 0:
        rgb[0] = v;
        rgb[1] = t;
        rgb[2] = p;
        break;
    case 1:
        rgb[0] = q;
        rgb[1] = v;
        rgb[2] = p;
        break;
    case 2:
        rgb[0] = p;
        rgb[1] = v;
        rgb[2] = t;
        break;
    case 3:
        rgb[0] = p;
        rgb[1] = q;
        rgb[2] = v;
        break;
    case 4:
        rgb[0] = t;
        rgb[1] = p;
        rgb[2] = v;
        break;
    default:
        rgb[0] = v;
        rgb[1] = p;
        rgb[2] = q;
        break;
    }
}

inline void
colorWheelPixel(const ColorWheelPixelParams& p,
                int x,
                int y,
                float rgba[4])
{
    rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0.f;
    if (!(p.radius > 0.f)) {
        return;
    }
    const float px = ((float)x + 0.5f) * p.pixelW;
    const float py = ((float)y + 0.5f) * p.pixelH;
    const ShapeFalloff::ShapeBox disc = { p.cx - p.radius, p.cy - p.radius, p.cx + p.radius, p.cy + p.radius };
    const float alpha = ShapeFalloff::bandCoverage(ShapeFalloff::ellipseEdge(px, py, disc), p.softness, p.pixelW, p.pixelH);
    if (!(alpha > 0.f)) {
        return;
    }
    const float dx = (px - p.cx) / p.radius;
    const float dy = (py - p.cy) / p.radius;
    const float d = std::sqrt(dx * dx + dy * dy);
    // Hue runs clockwise from red on +x, as the openfx-misc ColorWheel's does.
    float hue = d > 0.f ? -std::atan2(dy, dx) * 0.15915494309189535f : 0.f;
    hue += p.rotateTurns;
    hue -= std::floor(hue);
    const float saturation = p.centerSaturation + d * (p.edgeSaturation - p.centerSaturation);
    const float value = p.centerValue + d * (p.edgeValue - p.centerValue);
    float rgb[3];
    colorWheelHsvToRgb(hue, saturation, value, rgb);
    rgba[0] = rgb[0] * alpha;
    rgba[1] = rgb[1] * alpha;
    rgba[2] = rgb[2] * alpha;
    rgba[3] = alpha;
}

/**
 * @brief ColorWheel: an HSV hue wheel in the disc inscribed in the extent, hue around it and
 * saturation and value blended from centre to edge, in scene-linear RGB with no gamma. The disc's
 * edge is antialiased and softened inward by softness canonical units, and the output is
 * premultiplied by that coverage; outside the disc it is zero.
 **/
struct ColorWheelOp {
    struct Params {
        double x1;
        double y1;
        double x2;
        double y2;
        double par;
        double rotate;
        float softness;
        float centerSaturation;
        float edgeSaturation;
        float centerValue;
        float edgeValue;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_COLORWHEEL,
        .label = "ColorWheel",
        .description = "Generate an image with a color wheel.\n"
                       "The wheel is the disc inscribed in the extent: hue goes around it, and "
                       "saturation and value blend from their centre values to their edge values. The "
                       "output is linear HSV-derived RGB with no gamma, premultiplied by the disc's "
                       "antialiased, softened edge; outside the disc it is transparent black.",
        .grouping = PLUGIN_GROUP_IMAGE,
        .majorVersion = PLUGIN_MAJOR_NATRON_COLORWHEEL,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/ColorWheel.png",
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = {},
        .layers = LayerPolicy::targetSelect(),
        .op = OpKind::Point,
    };

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page);

    Params bake(const KnobReader& reader) const;

    bool isIdentity(const Params& params,
                    const PassContext& context) const;

    void row(const Params& params,
             const PassContext& context,
             const RowIO& io) const;

    GpuOpRef gpu(const Params& params,
                 const PassContext& context) const;

    static ColorWheelPixelParams pixelParams(const Params& params,
                                             double renderScaleX,
                                             double renderScaleY);

    const NativeGenerator* generator = nullptr;
    KnobDoubleWPtr centerSaturation;
    KnobDoubleWPtr edgeSaturation;
    KnobDoubleWPtr centerValue;
    KnobDoubleWPtr edgeValue;
    KnobDoubleWPtr rotate;
    KnobDoubleWPtr softness;
};

static_assert(ScaledPointOp<ColorWheelOp>);

using ColorWheel = GeneratorNode<ColorWheelOp>;

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Generator_ColorWheel_h
