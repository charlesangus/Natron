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

#ifndef Engine_Nodes_Color_Invert_h
#define Engine_Nodes_Color_Invert_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"

#define PLUGINID_NATRON_INVERT "net.sf.openfx.Invert"
#define PLUGIN_MAJOR_NATRON_INVERT 3

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kInvertInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

/**
 * @brief Invert: output = 1 - input on every processed channel, unclamped. It has no knob of its
 * own: the host's channel selector, "(Un)premult by", mask and mix do the rest. Its ID is the
 * openfx-misc Invert plugin's, one major above.
 **/
struct InvertOp {
    struct Params {
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_INVERT,
        .label = "Invert",
        .description = "Inverse the selected channels",
        .grouping = PLUGIN_GROUP_COLOR,
        .majorVersion = PLUGIN_MAJOR_NATRON_INVERT,
        .minorVersion = 0,
        .inputs = kInvertInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::Premult, Stage::MaskMix, Stage::PassThroughUnprocessed },
        .layers = LayerPolicy::color(true, true, true, true),
        .op = OpKind::Point,
    };

    Params bake(const KnobReader& reader) const;

    bool isIdentity(const Params& params) const;

    void row(const Params& params,
             const RowIO& io) const;

    GpuOpRef gpu(const Params& params) const;
};

static_assert(PointOp<InvertOp>);

class Invert
    : public ImageNode<InvertOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new Invert(node);
    }

    explicit Invert(NodePtr node);

    virtual ~Invert();

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Color_Invert_h
