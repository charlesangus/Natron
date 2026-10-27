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

#ifndef Engine_Nodes_Merge_Merge_h
#define Engine_Nodes_Merge_Merge_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <optional>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Merge/MergeOps.h"

#define PLUGINID_NATRON_NATIVE_MERGE "Merge"
#define PLUGIN_MAJOR_NATRON_NATIVE_MERGE 1

#define kNativeMergeParamOperation "operation"
#define kNativeMergeParamSetBBox "setBBox"
#define kNativeMergeParamMetadataFrom "metadataFrom"
#define kNativeMergeParamRangeFrom "rangeFrom"
#define kNativeMergeParamALayer "aLayers"
#define kNativeMergeParamAAlpha "aAlpha"
#define kNativeMergeParamBAlpha "bAlpha"
#define kNativeMergeParamAAlphaSource "aAlphaSource"
#define kNativeMergeParamBAlphaSource "bAlphaSource"
#define kNativeMergeParamAAlphaLayer "aAlphaLayer"
#define kNativeMergeParamBAlphaLayer "bAlphaLayer"
#define kNativeMergeParamLayers "bLayers"

// Option IDs of the set bbox, metadata from and frame range from choices.
#define kNativeMergeBBoxUnion "union"
#define kNativeMergeBBoxIntersection "intersection"
#define kNativeMergeBBoxA "A"
#define kNativeMergeBBoxB "B"
#define kNativeMergeFromA "A"
#define kNativeMergeFromB "B"

// Option IDs of the A and B alpha source choices.
#define kNativeMergeAlphaUniversal "universal"
#define kNativeMergeAlphaPerLayer "perLayer"
#define kNativeMergeAlphaPerChannel "perChannel"

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
 * @brief Merge: A over B with one of MergeOps' operators, then A2 over the result, and so on, in
 * the channels of each layer of B the Layers list names (every one under All). Each such layer
 * reads A's layer of the same name, or the one A layer chosen, with an alpha from each side's own
 * source. Every other layer and channel of B passes through.
 **/
struct NativeMergeOp {
    /**
     * @brief Where one side's alpha comes from. Universal is one channel for every merged channel;
     * PerLayer is the read layer's own A or alpha channel, else the universal channel; PerChannel
     * is an alpha layer whose channel i is the alpha of merged channel i.
     **/
    enum class AlphaSource : unsigned char {
        Universal = 0,
        PerLayer,
        PerChannel
    };

    struct Params {
        MergeOps::Operator op;
        // Whether a transparent A (A and its alpha all zero) leaves B unchanged under op.
        bool identityForTransparentA;
        AlphaSource aAlpha;
        AlphaSource bAlpha;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_NATIVE_MERGE,
        .label = "Merge",
        .description = "Composites A over B with one of Nuke's operators, then A2 over the result, and so on. "
                       "Each layer of B the Layers list names, or every layer of B under All, is merged in place, "
                       "reading A's layer of the same name or one chosen A layer; every other layer of B passes "
                       "through. Each side's alpha is one channel, each layer's own alpha or a per-channel alpha "
                       "layer. The operators are made HDR-safe where Nuke's clamp or jump outside 0 to 1.",
        .grouping = PLUGIN_GROUP_MERGE,
        .majorVersion = PLUGIN_MAJOR_NATRON_NATIVE_MERGE,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/Merge.png",
        .inputs = kNativeMergeInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::MaskMix, Stage::PassThroughUnprocessed },
        // The Layers list picks the merged planes (addProducedPlanes()), so no host layer knob.
        .layers = LayerPolicy::none(),
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
     * @brief The render error for a non-separable operator given a per-channel alpha layer on
     * either side: it blends whole colours, so no one channel's alpha is its alpha.
     **/
    std::optional<std::string> error(const Params& params) const;

    /**
     * @brief The merge kernel (Kernels/merge.slang) with the operator. A non-separable operator
     * blends a colour, so it runs on the GPU for three or four components only.
     **/
    GpuOpRef gpu(const Params& params) const;

    /**
     * @brief Whether op merging a transparent A (A = 0, a = 0) over any B gives B exactly, so a
     * missing A, or a stretch of row with nothing in A, leaves B unchanged.
     **/
    static bool isIdentityForTransparentA(MergeOps::Operator op);

    KnobChoiceWPtr operation;
    KnobChoiceWPtr setBBox;
    KnobChoiceWPtr metadataFrom;
    KnobChoiceWPtr rangeFrom;
    KnobLayerSelectWPtr aLayer;
    KnobChannelSelectWPtr aAlpha;
    KnobChannelSelectWPtr bAlpha;
    KnobChoiceWPtr aAlphaSource;
    KnobChoiceWPtr bAlphaSource;
    KnobLayerSelectWPtr aAlphaLayer;
    KnobLayerSelectWPtr bAlphaLayer;
    KnobChannelSetWPtr layers;
    KnobStringWPtr subLabel;
};

static_assert(FailingCompositeOp<NativeMergeOp>);
static_assert(GpuCompositeOp<NativeMergeOp>);

/**
 * @brief The Merge node. Named apart from the Merge namespace of MergingEnum.h.
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
     * @brief The input number of A input k (0..63).
     **/
    static int aInput(int k)
    {
        return nativeMergeAInput(k);
    }

    /**
     * @brief Points a Merge of a container's internal tree (RotoPaint's) at layerID, the layer
     * the container writes: Layers is that layer alone, A is read in its namesake, and each side's
     * alpha is the layer's own A or alpha channel, or 0 when it has none. Does nothing to another
     * node.
     **/
    static void followContainerLayer(const NodePtr& node,
                                     const std::string& layerID);

protected:
    /**
     * @brief The layers of B the Layers list merges, and B's alpha layer when B's alpha source
     * is per channel, where the merged alphas are written. B's other layers pass through.
     **/
    virtual void addProducedPlanes(double time,
                                   ViewIdx view,
                                   std::list<ImageLayerDesc>* planes) OVERRIDE FINAL;

    /**
     * @brief Each merged plane processes the channels its Layers row has on, every one under
     * All or with all of a row's on; B's others pass through. Every other output plane is
     * processed on every channel.
     **/
    virtual void addProducedPlanesChannels(double time,
                                           ViewIdx view,
                                           const std::list<ImageLayerDesc>& outputPlanes,
                                           ProcessChannelsPerPlaneMap* processChannelsPerPlane) OVERRIDE FINAL;

    /**
     * @brief Reads B in plane and A in plane's namesake under Matching, else in the A layer. B's
     * per-channel alpha layer, unless Layers merges it, is written the carried alpha of the fold,
     * and another plane Layers leaves out (the colour plane, always an output) is B copied.
     **/
    virtual void adjustInputsForPlane(const ImageLayerDesc& plane,
                                      double time,
                                      ViewIdx view,
                                      CompositeInputs* inputs) const OVERRIDE FINAL;

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

    // Shows each side's universal channel or alpha layer as its alpha source uses them.
    void refreshAlphaKnobs();

    // The layers of B the Layers list resolves to, every one of them under All.
    std::vector<ResolvedLayer> mergedLayers(double time,
                                            ViewIdx view) const WARN_UNUSED_RETURN;

    // Whether plane is one of mergedLayers(), any colour layer standing for any other.
    bool isMergedPlane(const ImageLayerDesc& plane,
                       double time,
                       ViewIdx view) const WARN_UNUSED_RETURN;

    // Whether A is read in each plane's namesake rather than in one chosen A layer.
    bool isMatchingALayer() const WARN_UNUSED_RETURN;

    // B's alpha layer, when B's alpha source is per channel and that layer resolves on B.
    bool bAlphaPlane(double time,
                     ViewIdx view,
                     ImageLayerDesc* layer) const WARN_UNUSED_RETURN;

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

#endif // Engine_Nodes_Merge_Merge_h
