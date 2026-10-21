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

#ifndef Engine_Nodes_Generator_Constant_h
#define Engine_Nodes_Generator_Constant_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/Image/NativeGenerator.h"

#define PLUGINID_NATRON_CONSTANT "net.sf.openfx.ConstantPlugin"
#define PLUGINID_NATRON_SOLID "net.sf.openfx.Solid"
#define PLUGIN_MAJOR_NATRON_CONSTANT 2

#define kConstantParamColor "color"

NATRON_NAMESPACE_ENTER

/**
 * @brief Constant and Solid: fill the selected channels of the target layer with `color`,
 * unclamped. Constant's colour is RGBA; Solid's is RGB with an alpha of 1. Knob names, defaults
 * and output are the openfx-misc ConstantPlugin's and Solid's; each registers under that
 * plugin's ID one major above.
 **/
struct ConstantOp {
    struct Params {
        float rgba[4];
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_CONSTANT,
        .label = "Constant",
        .description = "Generate an image with a constant color.\n"
                       "See also: https://web.archive.org/web/20220807183203/http://www.opticalenquiry.com/nuke/index.php?title=Constant,_CheckerBoard,_ColorBars,_ColorWheel",
        .grouping = PLUGIN_GROUP_IMAGE,
        .majorVersion = PLUGIN_MAJOR_NATRON_CONSTANT,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/net.sf.openfx.ConstantPlugin.png",
    };

    static constexpr NodeSpec kSolidNodeSpec = {
        .id = PLUGINID_NATRON_SOLID,
        .label = "Solid",
        .description = "Generate an image with a constant opaque color.",
        .grouping = PLUGIN_GROUP_IMAGE,
        .majorVersion = PLUGIN_MAJOR_NATRON_CONSTANT,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/net.sf.openfx.Solid.png",
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = {},
        .layers = LayerPolicy::targetSelect(),
        .op = OpKind::Point,
    };

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page);

    Params bake(const KnobReader& reader) const;

    bool isIdentity(const Params& params) const;

    void row(const Params& params,
             const RowIO& io) const;

    GpuOpRef gpu(const Params& params) const;

    // Solid: an RGB colour knob, and an alpha of 1.
    bool opaque = false;
    KnobColorWPtr color;
};

static_assert(PointOp<ConstantOp>);

template <const NodeSpec& SPEC, bool OPAQUE>
class ConstantNode
    : public GeneratorNode<ConstantOp, SPEC> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new ConstantNode(node);
    }

    explicit ConstantNode(NodePtr node)
        : GeneratorNode<ConstantOp, SPEC>(node)
    {
        this->op().opaque = OPAQUE;
    }

    virtual ~ConstantNode()
    {
    }

    virtual void addAcceptedComponents(int inputNb,
                                       std::list<ImageLayerDesc>* comps) OVERRIDE FINAL
    {
        GeneratorNode<ConstantOp, SPEC>::addAcceptedComponents(inputNb, comps);
        comps->push_back(ImageLayerDesc::getXYComponents());
    }
};

using Constant = ConstantNode<ConstantOp::kNodeSpec, false>;
using Solid = ConstantNode<ConstantOp::kSolidNodeSpec, true>;

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Generator_Constant_h
