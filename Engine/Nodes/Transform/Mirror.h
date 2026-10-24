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

#ifndef Engine_Nodes_Transform_Mirror_h
#define Engine_Nodes_Transform_Mirror_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <list>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/ResampleExecutor.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Image/TransformMath.h"
#include "Engine/RectD.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

#define PLUGINID_NATRON_MIRROR "Mirror"
#define PLUGIN_MAJOR_NATRON_MIRROR 1

#define kMirrorParamFlip "flip"
#define kMirrorParamFlop "flop"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kMirrorInputs[] = {
    InputSpec { .label = "Source" },
};

/**
 * @brief Mirror's op: a reflection of the source about the centre of its region of definition,
 * vertical for flip and horizontal for flop. It is a matrix Resample op, so the host
 * concatenates it with neighbouring transforms. On its own it samples with the impulse filter
 * at the centre of every pixel, which makes the render an exact reversal of pixel indices.
 *
 * The reflection is taken about the source's region of definition in pixels at the render
 * scale, so an odd-sized or offset region reverses onto itself pixel for pixel.
 **/
struct MirrorOp {
    struct Params {
        bool flip = false;
        bool flop = false;
        double time = 0.;
        ViewIdx view;
    };

    /**
     * @brief The source's region of definition at (time, view, scale); false when it has none.
     * Set by the node, which owns the input.
     **/
    using SourceRegionFn = std::function<bool(double time, ViewIdx view, const RenderScale& scale, RectD* rod)>;

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_MIRROR,
        .label = "Mirror",
        .description = "Flip (reverse the rows) and flop (reverse the columns) an image about the centre of its region of definition.\n"
                       "This plugin concatenates transforms.",
        .grouping = PLUGIN_GROUP_TRANSFORM,
        .majorVersion = PLUGIN_MAJOR_NATRON_MIRROR,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/Mirror.png",
        .inputs = kMirrorInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .layers = LayerPolicy::all(),
        .op = OpKind::Resample,
    };

    static constexpr bool kResamplesOnGpu = true;

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page);

    Params bake(const KnobReader& reader) const;

    bool isIdentity(const Params& params,
                    const PassContext& context) const;

    ResampleSettings settings(const Params& params,
                              const PassContext& context) const;

    /**
     * @brief The canonical reflection matrix, the same in both directions. It is the identity
     * when neither knob is on or the source has no finite region of definition.
     **/
    bool transform(const Params& params,
                   const PassContext& context,
                   double amount,
                   bool invert,
                   TransformMath::Mat3* matrix) const;

    KnobBoolWPtr flip;
    KnobBoolWPtr flop;
    SourceRegionFn sourceRegion;
};

static_assert(GpuResampleOp<MirrorOp>);

/**
 * @brief Mirror: reverses the rows (flip) and the columns (flop) of the image about the centre
 * of its region of definition. It concatenates with the transforms around it, so a chain of
 * them is resampled once with the filter of the node that renders; alone it only reverses pixel
 * indices.
 **/
class Mirror
    : public ImageNode<MirrorOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new Mirror(node);
    }

    explicit Mirror(NodePtr node);

    virtual ~Mirror();

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_Mirror_h
