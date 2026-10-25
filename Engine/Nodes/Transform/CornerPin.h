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

#ifndef Engine_Nodes_Transform_CornerPin_h
#define Engine_Nodes_Transform_CornerPin_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <optional>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Image/TransformMath.h"
#include "Engine/Nodes/Transform/CornerPinKnobs.h"
#include "Engine/RectD.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The pin knob values one CornerPin evaluation reads, in canonical coordinates.
 **/
struct CornerPinPoints {
    double from[4][2] = { { 0., 0. }, { 0., 0. }, { 0., 0. }, { 0., 0. } };
    double to[4][2] = { { 0., 0. }, { 0., 0. }, { 0., 0. }, { 0., 0. } };
    bool enable[4] = { true, true, true, true };
    double amount = 1.;
    TransformMath::Mat3 extraMatrix = TransformMath::Mat3::identity();
};

/**
 * @brief The knob values one CornerPin evaluation reads.
 **/
struct CornerPinNodeParams {
    CornerPinPoints pin;
    bool invert = false;
    Resampler::FilterEnum filter = Resampler::eFilterCubic;
    bool clamp = false;
    bool blackOutside = true;
    Resampler::BlurSettings blur;
};

inline constexpr InputSpec kCornerPinInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

inline constexpr NodeSpec kCornerPinNodeSpec = {
    .id = PLUGINID_NATRON_CORNERPIN,
    .label = "CornerPin",
    .description = "Warps the image so that the \"from\" points land on the \"to\" points.\n"
                   "The transform is a translation if 1 point is enabled, a similarity if 2 are enabled, an affine "
                   "transform if 3 are enabled and a homography if all 4 are. When the enabled points are degenerate "
                   "(3 or 4 of them aligned, 2 of them coincident) the next simpler transform is used. With no point "
                   "enabled, only the extra matrix applies.\n"
                   "Amount blends the \"to\" points from the \"from\" points; the extra matrix is always applied.\n"
                   "This node concatenates transforms while its mask is not used and mix is 1.",
    .grouping = PLUGIN_GROUP_TRANSFORM,
    .majorVersion = PLUGIN_MAJOR_NATRON_CORNERPIN,
    .minorVersion = 0,
    .icon = NATRON_IMAGES_PATH "NativeNodes/CornerPin.png",
    .inputs = kCornerPinInputs,
};

inline constexpr ImagePipelineSpec kCornerPinPipeline = {
    .stages = { Stage::MaskMix },
    .layers = LayerPolicy::all(),
    .op = OpKind::Resample,
};

/**
 * @brief CornerPin's op: the pin's projective matrix composed with the extra matrix, resampled
 * with one of the ten OpenFX filters, with optional motion blur over the shutter.
 **/
struct CornerPinOp {
    using Params = CornerPinNodeParams;

    static constexpr bool kResamplesOnGpu = true;

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page);

    Params bake(const KnobReader& reader) const;

    /**
     * @brief No motion blur over an open shutter, and a pin that leaves the image where it is.
     **/
    bool isIdentity(const Params& params,
                    const PassContext& context) const;

    ResampleSettings settings(const Params& params,
                              const PassContext& context) const;

    /**
     * @brief inverseMatrix() of the pin at amount.
     **/
    bool transform(const Params& params,
                   const PassContext& context,
                   double amount,
                   bool invert,
                   TransformMath::Mat3* matrix) const;

    /**
     * @brief The canonical matrix the render samples with (output to source) for pin, the
     * "to" points blended from the "from" points by amount x pin.amount; with invert, the
     * inverse of that, the forward matrix. The pin part is solved from the k enabled points: a
     * homography for 4, affine for 3, similarity for 2, translation for 1. A degenerate set falls
     * back to the next solver over the subsets of one point fewer, later points dropped first,
     * and to the identity when no solver succeeds. The pin part is negated when it maps its first
     * source point to z < 0, so that point keeps a positive z, then composed with the extra matrix
     * (pin x extra, the extra matrix applied first to output coordinates). With no point enabled
     * it is the extra matrix alone. False only when invert is asked of a singular extra matrix.
     **/
    static bool inverseMatrix(const CornerPinPoints& pin,
                              double amount,
                              bool invert,
                              TransformMath::Mat3* matrix);

    /**
     * @brief Whether pin leaves the image unchanged: the extra matrix is the identity and the
     * amount is 0 or every enabled "to" point equals its "from" point.
     **/
    static bool isIdentityPin(const CornerPinPoints& pin);

    static bool rendersWithMotionBlur(const Params& params);

    std::optional<GpuCostHint> costHint(const Params& params,
                                        const PassContext& context) const;

    KnobDoubleWPtr to[4];
    KnobBoolWPtr enable[4];
    KnobDoubleWPtr from[4];
    KnobButtonWPtr copyFrom;
    KnobButtonWPtr copyFromSingle;
    KnobButtonWPtr copyTo;
    KnobButtonWPtr copyToSingle;
    KnobButtonWPtr setToInput;
    KnobDoubleWPtr amount;
    KnobDoubleWPtr extraMatrix;
    KnobChoiceWPtr overlayPoints;
    KnobBoolWPtr interactive;
    KnobBoolWPtr invert;
    KnobChoiceWPtr filter;
    KnobBoolWPtr clamp;
    KnobBoolWPtr blackOutside;
    KnobDoubleWPtr motionBlur;
    KnobDoubleWPtr shutter;
    KnobChoiceWPtr shutterOffset;
    KnobDoubleWPtr shutterCustomOffset;
};

static_assert(GpuResampleOp<CornerPinOp>);

/**
 * @brief CornerPin: maps four "from" points onto four "to" points, with the host mask and mix.
 *
 * It concatenates like Transform: it always applies the transforms the host concatenates into
 * its source, and offers its own matrix while the mask is not applied and mix is 1 at the time
 * asked. With the mask applied it renders itself, mixing with its immediate input.
 **/
class CornerPinNode
    : public ImageNode<CornerPinOp, kCornerPinNodeSpec, kCornerPinPipeline> {
    using Base = ImageNode<CornerPinOp, kCornerPinNodeSpec, kCornerPinPipeline>;

public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new CornerPinNode(node);
    }

    explicit CornerPinNode(NodePtr node);

    virtual ~CornerPinNode();

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    /**
     * @brief True unless the mask is applied: mask x mix is no matrix.
     **/
    virtual bool getCanTransform() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The default reply when mix is not 1 at time or the mask is applied, else the
     * Resample op's matrix as for Transform.
     **/
    virtual StatusEnum getTransform(double time,
                                    const RenderScale& renderScale,
                                    bool draftRender,
                                    ViewIdx view,
                                    EffectInstancePtr* inputToTransform,
                                    Transform::Matrix3x3* transform) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The canonical matrix a render samples with at (time, view), invert included.
     **/
    bool getInverseMatrix(double time, ViewIdx view, TransformMath::Mat3* matrix) const WARN_UNUSED_RETURN;

    /**
     * @brief Sets the "from" points to the corners of the source's region of definition.
     * Nothing happens without a source or with an infinite or empty source region.
     **/
    void setFromToInput(double time, bool alsoTo);

    /**
     * @brief One undoable step copying "from" onto "to" (fromToTo) or "to" onto "from": the
     * values, expressions and curves of each point with withAnimation, else its value at time.
     * Undo restores the destination points and srcClipChanged as they were when it was made.
     * markEdited sets srcClipChanged, as an edit of the points does.
     **/
    UndoCommandPtr makeCopyPointsCommand(bool fromToTo, bool withAnimation, double time, bool markEdited);

protected:
    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;

private:
    bool getSourceRoD(double time, RectD* rod) const;

    void updateClampEnabled(double time, ViewSpec view);

    class CopyPointsCommand;

    static bool isPointsEdit(ValueChangedReasonEnum reason);

    void markPointsEdited(ValueChangedReasonEnum reason);

    KnobBoolWPtr _srcClipChanged;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_CornerPin_h
