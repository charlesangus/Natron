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

#ifndef Engine_Nodes_Transform_Position_h
#define Engine_Nodes_Transform_Position_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"

#define PLUGINID_NATRON_POSITION "Position"
#define PLUGIN_MAJOR_NATRON_POSITION 1

#define kPositionParamTranslate "translate"
#define kPositionParamInteractive "interactive"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kPositionInputs[] = {
    InputSpec { .label = "Source" },
};

/**
 * @brief Position: translates the image by a whole number of pixels, the pixels outside the
 * shifted source being zero. Knob names, defaults, rounding, region of definition, region of
 * interest and identity rule are the openfx-misc PositionPlugin's; it registers under that
 * plug-in's ID one major above. It does not concatenate transforms.
 *
 * The OpenFX plug-in's even-row rounding for field-both never applies here: the host renders
 * and describes every frame without fields.
 **/
struct PositionOp {
    struct Params {
        double translateX = 0.;
        double translateY = 0.;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_POSITION,
        .label = "Position",
        .description = "Translate an image by an integer number of pixels.\n"
                       "This plugin does not concatenate transforms.",
        .grouping = PLUGIN_GROUP_TRANSFORM,
        .majorVersion = PLUGIN_MAJOR_NATRON_POSITION,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/Position.png",
        .inputs = kPositionInputs,
    };

    static constexpr bool kResamplesOnGpu = true;

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::PassThroughUnprocessed },
        .layers = LayerPolicy::all(),
        .op = OpKind::Resample,
    };

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page);

    Params bake(const KnobReader& reader) const;

    bool isIdentity(const Params& params,
                    const PassContext& context) const;

    /**
     * @brief The translation in whole pixels at the context's scale: the knob value in pixels,
     * rounded to nearest, x through the pixel aspect ratio and both axes through the horizontal
     * render scale.
     **/
    PixelOffset offset(const Params& params,
                       const PassContext& context) const;

    KnobDoubleWPtr translate;
};

static_assert(ResampleOp<PositionOp> && !MatrixResampleOp<PositionOp>);
static_assert(GpuOffsetResampleOp<PositionOp>);

class Position
    : public ImageNode<PositionOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new Position(node);
    }

    explicit Position(NodePtr node);

    virtual ~Position();

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_Position_h
