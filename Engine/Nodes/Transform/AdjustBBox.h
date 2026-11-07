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

#ifndef Engine_Nodes_Transform_AdjustBBox_h
#define Engine_Nodes_Transform_AdjustBBox_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"

#define PLUGINID_NATRON_ADJUSTBBOX "AdjustBBox"
#define PLUGIN_MAJOR_NATRON_ADJUSTBBOX 1

#define kAdjustBBoxParamNumPixels "numpixels"
#define kAdjustBBoxParamBoundary "boundary"
#define kAdjustBBoxBoundaryBlack "black"
#define kAdjustBBoxBoundaryNearest "nearest"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kAdjustBBoxInputs[] = {
    InputSpec { .label = "Source" },
};

/**
 * @brief AdjustBBox: grows the source's region of definition by numpixels on each side, or
 * shrinks it for negative values. The added border is black or repeats the source's edge pixels.
 **/
struct AdjustBBoxOp {
    enum Boundary {
        eBoundaryBlack = 0,
        eBoundaryNearest
    };

    struct Params {
        double padX = 0.;
        double padY = 0.;
        bool nearest = false;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_ADJUSTBBOX,
        .label = "AdjustBBox",
        .description = "Enlarges the input's region of definition (bounding box) by Num Pixels on each side, "
                       "or crops it when they are negative. Boundary sets what fills the added border: black, "
                       "or the nearest pixel of the input's edge.",
        .grouping = PLUGIN_GROUP_TRANSFORM,
        .majorVersion = PLUGIN_MAJOR_NATRON_ADJUSTBBOX,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/AdjustBBox.png",
        .inputs = kAdjustBBoxInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = {},
        .layers = LayerPolicy::all(),
        .op = OpKind::Passes,
    };

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page);

    Params bake(const KnobReader& reader) const;

    /**
     * @brief Whether numpixels is 0 on both axes.
     **/
    bool isIdentity(const Params& params,
                    const PassContext& context) const;

    /**
     * @brief Nothing to compute with a black border: the executor reads zero outside the source.
     * A nearest border reads the source clamped to its pixels, through a pass that only widens
     * the region read by the border's width, so that the edge pixels are always among them.
     **/
    PassList passes(const Params& params,
                    const PassContext& context) const;

    KnobDoubleWPtr numPixels;
    KnobChoiceWPtr boundary;
};

static_assert(PassesOp<AdjustBBoxOp>);

class AdjustBBox
    : public ImageNode<AdjustBBoxOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new AdjustBBox(node);
    }

    explicit AdjustBBox(NodePtr node);

    virtual ~AdjustBBox();

protected:
    /**
     * @brief The source's region grown by numpixels in canonical coordinates, then enclosed in
     * pixels; empty when a crop leaves nothing.
     **/
    virtual bool clipRegionOfDefinition(double time,
                                        const RenderScale& scale,
                                        ViewIdx view,
                                        const EffectInstancePtr& source,
                                        const RectI& srcRoD,
                                        RectI* rod) OVERRIDE FINAL;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_AdjustBBox_h
