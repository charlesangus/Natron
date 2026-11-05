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

#ifndef Engine_Nodes_Keyer_Difference_h
#define Engine_Nodes_Keyer_Difference_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"

#define PLUGINID_NATRON_DIFFERENCE "Difference"
#define PLUGIN_MAJOR_NATRON_DIFFERENCE 1

#define kDifferenceParamBlackPoint "blackPoint"
#define kDifferenceParamWhitePoint "whitePoint"
#define kDifferenceParamGain "gain"

#define kDifferenceInputB 0
#define kDifferenceInputA 1
#define kDifferenceInputMask 2

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kDifferenceInputs[] = {
    InputSpec { .label = "B", .optional = true },
    InputSpec { .label = "A", .optional = true },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

/**
 * @brief Difference: a rough matte from a subject (B) and its clean plate (A). Colour is B's. With
 * d the sum over R, G, B of (B - A)^2, alpha is gain x max(0, (d - blackPoint) / (whitePoint -
 * blackPoint)), limited to 0 to 1 (a NaN giving 0), and after the mask and mix as well, so that a blend
 * with an alpha of B outside 0 to 1 stays within it; when
 * whitePoint <= blackPoint it is a hard threshold, gain where d > blackPoint and 0 elsewhere. With A
 * unconnected the plate is B itself, a zero difference; with B unconnected the output is
 * transparent black.
 **/
struct DifferenceOp {
    struct Params {
        float blackPoint;
        // 1 / (whitePoint - blackPoint), unused for a hard threshold.
        float scale;
        float gain;
        bool threshold;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_DIFFERENCE,
        .label = "Difference",
        .description = "Produces a rough matte from the difference of two images. B is the subject over the "
                       "background, A the background alone (the clean plate). Colour is copied from B and alpha "
                       "is the squared distance between B's and A's colours, remapped so that Black Point gives 0 "
                       "and White Point gives 1, times Gain.",
        .grouping = PLUGIN_GROUP_KEYER,
        .majorVersion = PLUGIN_MAJOR_NATRON_DIFFERENCE,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/Difference.png",
        .inputs = kDifferenceInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::MaskMix, Stage::PassThroughUnprocessed, Stage::ClampAlpha },
        .layers = LayerPolicy::color(false, false, false, true),
        .op = OpKind::Composite,
    };

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page);

    Params bake(const KnobReader& reader) const;

    bool isIdentity(const Params& params) const;

    Route route(const Params& params,
                const RouteContext& context) const;

    CompositeInputs inputs(const Params& params,
                           const RouteContext& context) const;

    void step(const Params& params,
              const RowIO& io) const;

    GpuOpRef gpu(const Params& params) const;

    KnobDoubleWPtr blackPoint;
    KnobDoubleWPtr whitePoint;
    KnobDoubleWPtr gain;
};

static_assert(CompositeOp<DifferenceOp>);
static_assert(GpuCompositeOp<DifferenceOp>);

class Difference
    : public ImageNode<DifferenceOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new Difference(node);
    }

    explicit Difference(NodePtr node);

    virtual ~Difference();
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Keyer_Difference_h
