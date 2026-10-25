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

#ifndef Engine_Nodes_Merge_NativeMerge_h
#define Engine_Nodes_Merge_NativeMerge_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Merge/MergeOps.h"

#define PLUGINID_NATRON_NATIVE_MERGE "Merge"
#define PLUGIN_MAJOR_NATRON_NATIVE_MERGE 1

#define kNativeMergeParamOperation "operation"
#define kNativeMergeParamBBox "bbox"
#define kNativeMergeParamMetadataFrom "metadataFrom"
#define kNativeMergeParamRangeFrom "rangeFrom"
#define kNativeMergeParamALayer "aLayer"
#define kNativeMergeParamBLayer "bLayer"
#define kNativeMergeParamAAlpha "aAlpha"
#define kNativeMergeParamBAlpha "bAlpha"

// Option IDs of the bbox, metadata from and frame range from choices.
#define kNativeMergeBBoxUnion "union"
#define kNativeMergeBBoxIntersection "intersection"
#define kNativeMergeBBoxA "A"
#define kNativeMergeBBoxB "B"
#define kNativeMergeFromA "A"
#define kNativeMergeFromB "B"

// Inputs: B, A, Mask, then A2..A64, Nuke's order.
#define kNativeMergeInputB 0
#define kNativeMergeInputA 1
#define kNativeMergeInputMask 2
#define kNativeMergeMaxAInputs 64

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kNativeMergeInputs[] = {
    InputSpec { .label = "B", .optional = true },
    InputSpec { .label = "A", .optional = true },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
    InputSpec { .label = "A", .role = InputRole::Variadic, .optional = true, .count = kNativeMergeMaxAInputs - 1, .firstNumber = 2 },
};

/**
 * @brief The input number of A input k (0..63): A is input 1, A2 onwards follow the Mask input.
 **/
constexpr int
nativeMergeAInput(int k)
{
    return (k == 0) ? kNativeMergeInputA : k + kNativeMergeInputMask;
}

/**
 * @brief Merge: A over B with one of MergeOps' operators, then A2 over the result, and so on. A
 * and B are each read in their own layer with a universal alpha channel, and the result goes to
 * the target layer. Every other layer of B passes through.
 **/
struct NativeMergeOp {
    struct Params {
        MergeOps::Operator op;
        // Whether a transparent A (A and its alpha all zero) leaves B unchanged under op.
        bool identityForTransparentA;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_NATIVE_MERGE,
        .label = "Merge",
        .description = "Composites the A inputs over B with a choice of operator.",
        .grouping = PLUGIN_GROUP_MERGE,
        .majorVersion = PLUGIN_MAJOR_NATRON_NATIVE_MERGE,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/net.sf.openfx.MergePlugin.png",
        .inputs = kNativeMergeInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::MaskMix, Stage::PassThroughUnprocessed },
        .layers = LayerPolicy::targetSelect(),
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

    /**
     * @brief Whether op merging a transparent A (A = 0, a = 0) over any B gives B exactly, so a
     * missing A, or a stretch of row with nothing in A, leaves B unchanged.
     **/
    static bool isIdentityForTransparentA(MergeOps::Operator op);

    KnobChoiceWPtr operation;
    KnobChoiceWPtr bbox;
    KnobChoiceWPtr metadataFrom;
    KnobChoiceWPtr rangeFrom;
    KnobLayerSelectWPtr aLayer;
    KnobLayerSelectWPtr bLayer;
    KnobChannelSelectWPtr aAlpha;
    KnobChannelSelectWPtr bAlpha;
    KnobStringWPtr subLabel;
};

static_assert(CompositeOp<NativeMergeOp>);

/**
 * @brief The Merge node. Named apart from MergeNode, the OpenFX-era Merge it replaces, and from
 * the Merge namespace of MergingEnum.h.
 **/
class NativeMerge
    : public ImageNode<NativeMergeOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new NativeMerge(node);
    }

    explicit NativeMerge(NodePtr node);

    virtual ~NativeMerge();

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The op's route(), except that a B identity needs B read in the layer the node
     * writes: with another layer the result is B's selected layer copied over, which renders.
     **/
    virtual Route route(double time,
                        ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The input number of A input k (0..63).
     **/
    static int aInput(int k)
    {
        return nativeMergeAInput(k);
    }

protected:
    virtual void getFrameRange(double* first,
                               double* last) OVERRIDE FINAL;

    virtual ImageMetadata deriveOutputMetadata(double time,
                                               ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    // Shows the operator's name next to the node's label.
    void refreshSubLabel();

    // The region of definition of inputNb in *rod, an empty one for a region it fails to give;
    // false when nothing is connected to it.
    bool getInputRoD(int inputNb,
                     double time,
                     const RenderScale& scale,
                     ViewIdx view,
                     RectD* rod) const;

    // The connected input "from" (A or B) names, falling back to the other side; -1 for none.
    int fromInput(const KnobChoiceWPtr& from,
                  double time,
                  ViewIdx view) const WARN_UNUSED_RETURN;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Merge_NativeMerge_h
