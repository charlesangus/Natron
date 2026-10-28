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

#ifndef Engine_Nodes_Merge_KeyMix_h
#define Engine_Nodes_Merge_KeyMix_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"

#define PLUGINID_NATRON_KEYMIX "KeyMix"
#define PLUGIN_MAJOR_NATRON_KEYMIX 1

#define kKeyMixParamSetBBox "setBBox"
#define kKeyMixBBoxUnion "union"
#define kKeyMixBBoxB "B"
#define kKeyMixBBoxA "A"

#define kKeyMixInputB 0
#define kKeyMixInputA 1
#define kKeyMixInputMask 2

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kKeyMixInputs[] = {
    InputSpec { .label = "B", .optional = true },
    InputSpec { .label = "A", .optional = true },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

/**
 * @brief KeyMix: copies A's processed channels over B through the mask. Each processed channel
 * is B + (A - B) x mask x mix, the other channels are B's, and A reads zero outside its region.
 * With A unconnected the node is B; with neither connected it is transparent black.
 **/
struct KeyMixOp {
    enum BBox {
        eBBoxUnion = 0,
        eBBoxB,
        eBBoxA
    };

    struct Params {
        int bbox;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_KEYMIX,
        .label = "KeyMix",
        .description = "Copies the selected channels of A over B where the mask is, B elsewhere. The mask's "
                       "value blends between them, so a soft mask gives a soft transition; without a mask A "
                       "covers everywhere. Unselected channels are B's.",
        .grouping = PLUGIN_GROUP_MERGE,
        .majorVersion = PLUGIN_MAJOR_NATRON_KEYMIX,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/Merge.png",
        .inputs = kKeyMixInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::MaskMix, Stage::PassThroughUnprocessed },
        .layers = LayerPolicy::all(),
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

    KnobChoiceWPtr setBBox;
};

static_assert(CompositeOp<KeyMixOp>);
static_assert(GpuCompositeOp<KeyMixOp>);

class KeyMix
    : public ImageNode<KeyMixOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new KeyMix(node);
    }

    explicit KeyMix(NodePtr node);

    virtual ~KeyMix();

    /**
     * @brief B's region when the node is B (A unconnected, or a mix of 0); otherwise the union of
     * A and B, B's or A's as setBBox chooses. The project format when the chosen inputs have none.
     **/
    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

private:
    // Whether inputNb is connected, its region in *rod (empty when it reports none).
    bool getInputRoD(int inputNb,
                     double time,
                     const RenderScale& scale,
                     ViewIdx view,
                     RectD* rod) const;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Merge_KeyMix_h
