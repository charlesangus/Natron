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

#ifndef Engine_Nodes_Generator_Rectangle_h
#define Engine_Nodes_Generator_Rectangle_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Generator/ShapeGenerator.h"
#include "Engine/Nodes/Image/NativeGenerator.h"

#define PLUGINID_NATRON_RECTANGLE "Rectangle"
#define PLUGIN_MAJOR_NATRON_RECTANGLE 1

#define kRectangleParamColor0 kShapeParamColor0
#define kRectangleParamColor1 kShapeParamColor1
#define kRectangleParamFalloff kShapeParamFalloff
#define kRectangleParamSoftness kShapeParamSoftness
#define kRectangleParamCornerRadius "cornerRadius"

NATRON_NAMESPACE_ENTER

inline void
rectanglePixel(const ShapeBoxPixelParams& p,
               int x,
               int y,
               float rgba[4])
{
    const float px = ((float)x + 0.5f) * p.pixelW;
    const float py = ((float)y + 0.5f) * p.pixelH;
    const ShapeFalloff::ShapeBox box = { p.x1, p.y1, p.x2, p.y2 };
    const ShapeFalloff::ShapeEdge edge = ShapeFalloff::roundedBoxEdge(px, py, box, p.cornerX, p.cornerY);
    const float w = ShapeFalloff::shapeWeight(edge, p.softness, p.falloff, p.pixelW, p.pixelH);

    ShapeGenerator::lerpColor(p.color0, p.color1, w, rgba);
}

/**
 * @brief Rectangle: colour 1 inside the extent's box with rounded corners, colour 0 outside it, the
 * edge antialiased over the pixel and softened inward by softness canonical units through the
 * falloff curve. The corner radii are clamped to half the box's shorter side. Colours are
 * scene-linear and unclamped.
 **/
struct RectangleOp {
    struct Params {
        double x1;
        double y1;
        double x2;
        double y2;
        double par;
        double cornerX;
        double cornerY;
        float softness;
        int falloff;
        float color0[4];
        float color1[4];
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_RECTANGLE,
        .label = "Rectangle",
        .description = "Draw a rectangle filling the extent, optionally with rounded corners.\n"
                       "The rectangle is colour 1, anything outside it colour 0. Softness fades the edge "
                       "inward over that distance, through the falloff curve; at 0 the edge is hard "
                       "but antialiased. Colours are scene-linear and pass through unclamped.\n"
                       "To draw over an image, merge the Rectangle over it.",
        .grouping = PLUGIN_GROUP_IMAGE,
        .majorVersion = PLUGIN_MAJOR_NATRON_RECTANGLE,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/Rectangle.png",
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = {},
        .layers = LayerPolicy::targetSelect(),
        .op = OpKind::Point,
    };

    // The shape is the extent's box, so the box and its overlay are there from the start.
    static constexpr ExtentKnobs::ExtentEnum kDefaultExtent = ExtentKnobs::eExtentSize;

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

    static ShapeBoxPixelParams pixelParams(const Params& params,
                                           double renderScaleX,
                                           double renderScaleY);

    const NativeGenerator* generator = nullptr;
    KnobDoubleWPtr cornerRadius;
    KnobDoubleWPtr softness;
    KnobChoiceWPtr falloff;
    KnobColorWPtr color0;
    KnobColorWPtr color1;
};

static_assert(ScaledPointOp<RectangleOp>);

using Rectangle = GeneratorNode<RectangleOp>;

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Generator_Rectangle_h
