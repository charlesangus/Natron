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

#ifndef Engine_Nodes_Color_ColorMathNode_h
#define Engine_Nodes_Color_ColorMathNode_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"

#define PLUGINID_NATRON_ADD "net.sf.openfx.AddPlugin"
#define PLUGINID_NATRON_MULTIPLY "net.sf.openfx.MultiplyPlugin"
#define PLUGINID_NATRON_GAMMA "net.sf.openfx.GammaPlugin"
#define PLUGIN_MAJOR_NATRON_COLORMATH 3

#define kColorMathParamValue "value"
#define kColorMathParamInvert "invert"

NATRON_NAMESPACE_ENTER

enum ColorMathOperationEnum {
    eColorMathOperationAdd,
    eColorMathOperationMultiply,
    eColorMathOperationGamma
};

inline constexpr InputSpec kColorMathInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

constexpr NodeSpec
colorMathNodeSpec(ColorMathOperationEnum operation)
{
    NodeSpec spec;

    switch (operation) {
    case eColorMathOperationAdd:
        spec.id = PLUGINID_NATRON_ADD;
        spec.icon = NATRON_IMAGES_PATH "NativeNodes/net.sf.openfx.AddPlugin.png";
        spec.label = "Add";
        spec.description = "Add a constant to the selected channels.";
        break;
    case eColorMathOperationMultiply:
        spec.id = PLUGINID_NATRON_MULTIPLY;
        spec.icon = NATRON_IMAGES_PATH "NativeNodes/net.sf.openfx.MultiplyPlugin.png";
        spec.label = "Multiply";
        spec.description = "Multiply the selected channels by a constant.";
        break;
    case eColorMathOperationGamma:
        spec.id = PLUGINID_NATRON_GAMMA;
        spec.icon = NATRON_IMAGES_PATH "NativeNodes/net.sf.openfx.GammaPlugin.png";
        spec.label = "Gamma";
        spec.description = "Apply gamma function to the selected channels. The actual function is pow(x,1/max(1e-8,value)).";
        break;
    }
    spec.grouping = PLUGIN_GROUP_COLOR "/Math";
    spec.majorVersion = PLUGIN_MAJOR_NATRON_COLORMATH;
    spec.minorVersion = 0;
    spec.inputs = kColorMathInputs;

    return spec;
}

/**
 * @brief Add, Multiply and Gamma: one constant per channel, applied to the processed channels.
 *   Add:      out = in + value
 *   Multiply: out = in * value
 *   Gamma:    out = pow(in, e) for in > 0 and out = in otherwise, with e = 1 / max(1e-8, value),
 *             or e = value when invert is set.
 * The arithmetic is single-precision, as in the openfx-misc plug-ins of the same IDs, and the
 * node is registered under each of them one major above.
 **/
template <ColorMathOperationEnum KIND>
struct ColorMathOp {
    // For Gamma, value holds the exponent.
    struct Params {
        float value[4];
        bool isUnit[4];
        // Every channel's knob value is the operation's neutral one, compared in double.
        bool isNeutral;
    };

    static constexpr NodeSpec kNodeSpec = colorMathNodeSpec(KIND);

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::Premult, Stage::MaskMix, Stage::PassThroughUnprocessed },
        .layers = LayerPolicy::color(true, true, true, false),
        .op = OpKind::Point,
    };

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page);

    Params bake(const KnobReader& reader) const;

    bool isIdentity(const Params& params) const;

    void row(const Params& params,
             const RowIO& io) const;

    GpuOpRef gpu(const Params& params) const;

    KnobColorWPtr value;
    KnobBoolWPtr invert;
};

static_assert(PointOp<ColorMathOp<eColorMathOperationAdd>>);
static_assert(PointOp<ColorMathOp<eColorMathOperationMultiply>>);
static_assert(PointOp<ColorMathOp<eColorMathOperationGamma>>);

template <ColorMathOperationEnum KIND>
class ColorMathNode
    : public ImageNode<ColorMathOp<KIND>> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new ColorMathNode(node);
    }

    explicit ColorMathNode(NodePtr node)
        : ImageNode<ColorMathOp<KIND>>(node)
    {
    }

    virtual ~ColorMathNode()
    {
    }

    /**
     * @brief The Params the op renders with at (time, view).
     **/
    typename ColorMathOp<KIND>::Params bakeColorMath(double time,
                                                     ViewIdx view) const WARN_UNUSED_RETURN
    {
        return bakeParams(this->op(), time, view);
    }

    /**
     * @brief The op's CPU kernel over one row.
     **/
    void colorMathRow(const typename ColorMathOp<KIND>::Params& params,
                      const RowIO& io) const
    {
        this->op().row(params, io);
    }

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL
    {
        ImageNode<ColorMathOp<KIND>>::addAcceptedComponents(inputNb, comps);
        if ((KIND != eColorMathOperationAdd) && !this->isInputMask(inputNb)) {
            comps->push_back(ImageLayerDesc::getXYComponents());
        }
    }
};

using ColorMathAdd = ColorMathNode<eColorMathOperationAdd>;
using ColorMathMultiply = ColorMathNode<eColorMathOperationMultiply>;
using ColorMathGamma = ColorMathNode<eColorMathOperationGamma>;

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Color_ColorMathNode_h
