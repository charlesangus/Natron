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

#ifndef Engine_Nodes_Color_Premult_h
#define Engine_Nodes_Color_Premult_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"

#define PLUGINID_NATRON_PREMULT "Premult"
#define PLUGINID_NATRON_UNPREMULT "Unpremult"
#define PLUGIN_MAJOR_NATRON_PREMULT 1

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kPremultInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

constexpr NodeSpec
premultNodeSpec(bool divide)
{
    NodeSpec spec;

    if (divide) {
        spec.id = PLUGINID_NATRON_UNPREMULT;
        spec.label = "Unpremult";
        spec.icon = NATRON_IMAGES_PATH "NativeNodes/Unpremult.png";
        spec.description = "Divide the selected channels by alpha, or by any channel of any layer of the source picked in "
                           "\"Unpremult by\".\n"
                           "Where that channel is not above FLT_EPSILON the pixel is left as it is. With \"Unpremult by\" "
                           "set to None the image is left untouched.";
    } else {
        spec.id = PLUGINID_NATRON_PREMULT;
        spec.label = "Premult";
        spec.icon = NATRON_IMAGES_PATH "NativeNodes/Premult.png";
        spec.description = "Multiply the selected channels by alpha, or by any channel of any layer of the source picked in "
                           "\"Premult by\".\n"
                           "With \"Premult by\" set to None the image is left untouched.";
    }
    spec.grouping = PLUGIN_GROUP_MERGE;
    spec.majorVersion = PLUGIN_MAJOR_NATRON_PREMULT;
    spec.minorVersion = 0;
    spec.inputs = kPremultInputs;

    return spec;
}

/**
 * @brief Premult (DIVIDE false) and Unpremult (DIVIDE true): the processed channels times, or
 * divided by, the "(Un)premult by" channel the Divisor stage hands the row; that channel itself
 * passes through when it is among the processed ones. Unpremult divides
 * only where that channel is usable (Image::unPremultDivisorIsUsable), as the host's own
 * unpremultiply does. The op has no knobs: the channels, the divisor, the mask and the mix are
 * all the stages'.
 **/
template <bool DIVIDE>
struct PremultOp {
    struct Params {
    };

    static constexpr NodeSpec kNodeSpec = premultNodeSpec(DIVIDE);
    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::Divisor, Stage::MaskMix, Stage::PassThroughUnprocessed },
        .layers = LayerPolicy::color(true, true, true, false),
        .op = OpKind::Point,
        .divisorLabel = DIVIDE ? "Unpremult by" : "Premult by",
    };

    Params bake(const KnobReader& reader) const;
    bool isIdentity(const Params& params) const;
    void row(const Params& params,
             const RowIO& io) const;
    GpuOpRef gpu(const Params& params) const;
};

static_assert(PointOp<PremultOp<false>>);
static_assert(PointOp<PremultOp<true>>);

template <bool DIVIDE>
class PremultNode
    : public ImageNode<PremultOp<DIVIDE>> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new PremultNode(node);
    }

    explicit PremultNode(NodePtr node)
        : ImageNode<PremultOp<DIVIDE>>(node)
    {
    }

    virtual ~PremultNode()
    {
    }

    /**
     * @brief The op's CPU kernel over one row.
     **/
    void premultRow(const RowIO& io) const
    {
        this->op().row(typename PremultOp<DIVIDE>::Params(), io);
    }
};

using Premult = PremultNode<false>;
using Unpremult = PremultNode<true>;

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Color_Premult_h
