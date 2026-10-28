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

#ifndef Engine_Nodes_Generator_Ramp_h
#define Engine_Nodes_Generator_Ramp_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstdint>
#include <type_traits>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Generator/ShapeGenerator.h"
#include "Engine/Nodes/Image/NativeGenerator.h"

#define PLUGINID_NATRON_RAMP "Ramp"

#define PLUGIN_MAJOR_NATRON_RAMP 1

#define kRampParamPoint0 "point0"
#define kRampParamPoint1 "point1"
#define kRampParamColor0 kShapeParamColor0
#define kRampParamColor1 kShapeParamColor1
#define kRampParamFalloff kShapeParamFalloff

NATRON_NAMESPACE_ENTER

/**
 * @brief A Ramp at one render scale, in plain 32-bit words: the block the GPU kernel reads and the
 * CPU row evaluates. The points are canonical; coincident points are stored equal.
 **/
struct RampPixelParams {
    float p0x;
    float p0y;
    float p1x;
    float p1y;
    float pixelW;
    float pixelH;
    std::int32_t falloff;
    std::uint32_t reserved;
    float color0[4];
    float color1[4];
};

static_assert(std::is_trivially_copyable<RampPixelParams>::value, "RampPixelParams is copied to the GPU");
static_assert(sizeof(RampPixelParams) == 64, "RampPixelParams is a fixed GPU block");

inline void
rampPixel(const RampPixelParams& p,
          int x,
          int y,
          float rgba[4])
{
    const float px = ((float)x + 0.5f) * p.pixelW;
    const float py = ((float)y + 0.5f) * p.pixelH;
    const float t = ShapeFalloff::rampT(px, py, p.p0x, p.p0y, p.p1x, p.p1y);

    ShapeGenerator::lerpColor(p.color0, p.color1, ShapeFalloff::ease(t, p.falloff), rgba);
}

/**
 * @brief Ramp: colour 0 at point 0 to colour 1 at point 1 along the line between them, constant
 * across it, through the falloff curve, in scene-linear and unclamped. Coincident points give
 * colour 1 everywhere. The ramp lives in canonical space, so every render scale samples the same
 * gradient.
 **/
struct RampOp {
    struct Params {
        double p0x;
        double p0y;
        double p1x;
        double p1y;
        double par;
        int falloff;
        float color0[4];
        float color1[4];
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_RAMP,
        .label = "Ramp",
        .description = "Draw a ramp between 2 edges.\n"
                       "The ramp goes from colour 0 at point 0 to colour 1 at point 1, along the line "
                       "between them, and is constant across that line. The falloff shapes the "
                       "transition. Colours are scene-linear and pass through unclamped.\n"
                       "To ramp over an image, merge the Ramp over it.",
        .grouping = PLUGIN_GROUP_IMAGE,
        .majorVersion = PLUGIN_MAJOR_NATRON_RAMP,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/Ramp.png",
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

    static RampPixelParams pixelParams(const Params& params,
                                       double renderScaleX,
                                       double renderScaleY);

    const NativeGenerator* generator = nullptr;
    KnobDoubleWPtr point0;
    KnobDoubleWPtr point1;
    KnobColorWPtr color0;
    KnobColorWPtr color1;
    KnobChoiceWPtr falloff;
};

static_assert(ScaledPointOp<RampOp>);

/**
 * @brief The Ramp node: the generator with the host ramp overlay on its two points.
 **/
class RampNode
    : public GeneratorNode<RampOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new RampNode(node);
    }

    explicit RampNode(NodePtr node)
        : GeneratorNode<RampOp>(node)
    {
    }

    virtual ~RampNode()
    {
    }

protected:
    virtual void initializeKnobs() OVERRIDE FINAL;
};

using Ramp = RampNode;

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Generator_Ramp_h
