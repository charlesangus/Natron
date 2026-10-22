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

#ifndef Engine_Nodes_Image_ImagePipelineSpec_h
#define Engine_Nodes_Image_ImagePipelineSpec_h

#include "Global/Macros.h"

#include <initializer_list>

#include "Engine/Nodes/NodeSpec.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief A stage the host wraps an image op in.
 *
 * Premult divides the source by the "(Un)premult by" channel before the op and multiplies the
 * processed channels back after it. MaskMix blends the op's result with the undivided source by
 * mask x mix. PassThroughUnprocessed writes the channels the op does not process from the
 * source; without it they are left for the host to copy.
 **/
enum class Stage : unsigned char {
    Premult = 1 << 0,
    MaskMix = 1 << 1,
    PassThroughUnprocessed = 1 << 2
};

class StageSet {
public:
    constexpr StageSet() = default;

    constexpr StageSet(std::initializer_list<Stage> stages)
    {
        for (Stage s : stages) {
            _bits |= (unsigned char)s;
        }
    }

    constexpr bool has(Stage s) const
    {
        return (_bits & (unsigned char)s) != 0;
    }

    constexpr bool empty() const
    {
        return _bits == 0;
    }

    constexpr StageSet with(Stage s) const
    {
        StageSet out = *this;

        out._bits |= (unsigned char)s;

        return out;
    }

    constexpr bool operator==(const StageSet&) const = default;

private:
    unsigned char _bits = 0;
};

/**
 * @brief Which layers and channels a node processes, which decides the layer knob the host
 * gives it. Color and All get the input-bound channel set, starting on the colour layer or on
 * "All"; TargetSelect gets a layer select naming the layer the node writes; None gets no knob.
 * defaults is the R, G, B, A row the channel checkboxes start with.
 **/
struct LayerPolicy {
    enum class Kind : unsigned char {
        Color,
        All,
        TargetSelect,
        None
    };

    Kind kind = Kind::Color;
    bool defaults[4] = { true, true, true, true };

    static constexpr LayerPolicy color(bool r,
                                       bool g,
                                       bool b,
                                       bool a)
    {
        return LayerPolicy { Kind::Color, { r, g, b, a } };
    }

    static constexpr LayerPolicy all()
    {
        return LayerPolicy { Kind::All, { true, true, true, true } };
    }

    static constexpr LayerPolicy targetSelect()
    {
        return LayerPolicy { Kind::TargetSelect, { true, true, true, true } };
    }

    static constexpr LayerPolicy none()
    {
        return LayerPolicy { Kind::None, { true, true, true, true } };
    }
};

/**
 * @brief What an image op computes per evaluation: one row of pixels from the same row of its
 * sources (Point), one row from several sources (Composite), nothing but a choice of input
 * (Route), a list of passes over scratch buffers (Passes), a resampling of its source
 * (Resample), or anything, through the node's own code (Custom).
 **/
enum class OpKind : unsigned char {
    Point,
    Composite,
    Route,
    Passes,
    Resample,
    Custom
};

/**
 * @brief The raw stage (raw/stage) a node needs its Source input at. Any for an image node and
 * for a raw node that works at either stage. Raw or Prepared make the host check the source's
 * metadata before rendering: on a mismatch the node is never an identity, never placed on the
 * GPU, and its render fails with RawMetadata::requireStage()'s message as its persistent error.
 **/
enum class RawStageRequirement : unsigned char {
    Any,
    Raw,
    Prepared
};

/**
 * @brief How the host runs an image node: the stages around its op, its layer policy, the kind
 * of its op and, for a node reading raw data, the raw stage it needs.
 **/
struct ImagePipelineSpec {
    StageSet stages = {};
    LayerPolicy layers = {};
    OpKind op = OpKind::Point;
    RawStageRequirement rawStage = RawStageRequirement::Any;
};

constexpr bool
pipelinePremultHasSource(const ImagePipelineSpec& pipeline,
                         const NodeSpec& spec)
{
    return !pipeline.stages.has(Stage::Premult) || (nodeSpecSourceInput(spec) >= 0);
}

constexpr bool
pipelineRouteHasNoStages(const ImagePipelineSpec& pipeline)
{
    return (pipeline.op != OpKind::Route) || (pipeline.stages.empty() && (pipeline.rawStage == RawStageRequirement::Any));
}

constexpr bool
pipelineReadsRaw(const NodeSpec& spec)
{
    return nodeSpecSourceKind(spec) == eDataKindRaw;
}

/**
 * @brief An image node outputs an image, or raw data read from a raw Source input.
 **/
constexpr bool
pipelineOutputKindIsValid(const NodeSpec& spec)
{
    return (spec.outputKind == eDataKindImage) || ((spec.outputKind == eDataKindRaw) && pipelineReadsRaw(spec));
}

constexpr bool
pipelineRawStageHasRawSource(const ImagePipelineSpec& pipeline,
                             const NodeSpec& spec)
{
    return (pipeline.rawStage == RawStageRequirement::Any) || pipelineReadsRaw(spec);
}

/**
 * @brief A raw source is a mosaic, or linear data before colour management: no alpha to divide
 * by, no layers or channels to pick, and no mask to blend a mosaic against.
 **/
constexpr bool
pipelineRawSourceHasNoImageStages(const ImagePipelineSpec& pipeline,
                                  const NodeSpec& spec)
{
    return !pipelineReadsRaw(spec) || (!pipeline.stages.has(Stage::Premult) && !pipeline.stages.has(Stage::MaskMix) && (pipeline.layers.kind == LayerPolicy::Kind::None));
}

constexpr bool
isValidImagePipelineSpec(const ImagePipelineSpec& pipeline,
                         const NodeSpec& spec)
{
    return pipelinePremultHasSource(pipeline, spec) && pipelineRouteHasNoStages(pipeline) && pipelineOutputKindIsValid(spec) && pipelineRawStageHasRawSource(pipeline, spec) && pipelineRawSourceHasNoImageStages(pipeline, spec);
}

/**
 * @brief What StagedImageEffect::imagePipelineSpec() returns for a constexpr pipeline. Checking
 * it here, against the node's spec, makes an invalid pairing a compile error in the node.
 **/
template <const ImagePipelineSpec& PIPELINE, const NodeSpec& SPEC>
const ImagePipelineSpec&
staticImagePipelineSpec()
{
    static_assert(pipelinePremultHasSource(PIPELINE, SPEC), "a Premult stage needs a Source input to divide");
    static_assert(pipelineRouteHasNoStages(PIPELINE), "a Route op renders nothing, so it has no stages and no raw stage");
    static_assert(pipelineOutputKindIsValid(SPEC), "an image pipeline outputs eDataKindImage, or eDataKindRaw from an eDataKindRaw Source input");
    static_assert(pipelineRawStageHasRawSource(PIPELINE, SPEC), "a raw stage other than Any needs an eDataKindRaw Source input");
    static_assert(!pipelineReadsRaw(SPEC) || !PIPELINE.stages.has(Stage::Premult), "a raw Source input has no alpha to divide by, so no Premult stage");
    static_assert(!pipelineReadsRaw(SPEC) || !PIPELINE.stages.has(Stage::MaskMix), "a raw Source input is not blended against a mask, so no MaskMix stage");
    static_assert(!pipelineReadsRaw(SPEC) || (PIPELINE.layers.kind == LayerPolicy::Kind::None), "a raw Source input has no layers or channels to pick, so its layer policy is None");

    return PIPELINE;
}

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Image_ImagePipelineSpec_h
