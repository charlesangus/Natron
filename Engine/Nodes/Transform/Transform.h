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

#ifndef Engine_Nodes_Transform_Transform_h
#define Engine_Nodes_Transform_Transform_h

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
#include "Engine/RectD.h"

#define PLUGINID_NATRON_TRANSFORM "Transform"
#define PLUGIN_MAJOR_NATRON_TRANSFORM 1

#define kTransformNodeParamTranslate "translate"
#define kTransformNodeParamRotate "rotate"
#define kTransformNodeParamScale "scale"
#define kTransformNodeParamSkewX "skewX"
#define kTransformNodeParamSkewY "skewY"
#define kTransformNodeParamSkewOrder "skewOrder"
#define kTransformNodeParamAmount "transformAmount"
#define kTransformNodeParamCenter "center"
#define kTransformNodeParamResetCenter "resetCenter"
#define kTransformNodeParamCenterChanged "transformCenterChanged"
#define kTransformNodeParamInteractive "interactive"
#define kTransformNodeParamHiDPI "hidpi"
#define kTransformNodeParamInvert "invert"
#define kTransformNodeParamMotionBlur "motionBlur"
#define kTransformNodeParamDirectionalBlur "directionalBlur"
#define kTransformNodeParamShutter "shutter"

NATRON_NAMESPACE_ENTER

/**
 * @brief The knob values one Transform evaluation reads.
 **/
struct TransformNodeParams {
    TransformMath::TransformParams transform;
    bool invert = false;
    Resampler::FilterEnum filter = Resampler::eFilterCubic;
    bool clamp = false;
    bool blackOutside = true;
    Resampler::BlurSettings blur;
};

inline constexpr InputSpec kTransformInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

inline constexpr NodeSpec kTransformNodeSpec = {
    .id = PLUGINID_NATRON_TRANSFORM,
    .label = "Transform",
    .description = "Translate / Rotate / Scale a 2D image, with an optional mask.\n"
                   "Without a mask and at a mix of 1, it concatenates with the transforms around it.\n"
                   "See also https://web.archive.org/web/20220627030948/http://www.opticalenquiry.com/nuke/index.php?title=Transform",
    .grouping = PLUGIN_GROUP_TRANSFORM,
    .majorVersion = PLUGIN_MAJOR_NATRON_TRANSFORM,
    .minorVersion = 0,
    .icon = NATRON_IMAGES_PATH "NativeNodes/Transform.png",
    .inputs = kTransformInputs,
};

inline constexpr ImagePipelineSpec kTransformPipeline = {
    .stages = { Stage::MaskMix },
    .layers = LayerPolicy::all(),
    .op = OpKind::Resample,
};

/**
 * @brief Transform's op: the matrix of the transform knobs about the centre, resampled with one
 * of the ten OpenFX filters, with optional motion blur over the shutter or directional blur along
 * the transform. Knob names, defaults, regions, identity rules and pixel maths are the
 * openfx-misc TransformPlugin's (through Resampler and TransformMath).
 *
 * It holds every knob on the Controls page before the stage knobs, including those only the
 * node and its interact use.
 **/
struct TransformOp {
    using Params = TransformNodeParams;

    static constexpr bool kResamplesOnGpu = true;

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page);

    Params bake(const KnobReader& reader) const;

    /**
     * @brief No motion blur over an open shutter, and transform knobs that leave the image where
     * it is.
     **/
    bool isIdentity(const Params& params,
                    const PassContext& context) const;

    ResampleSettings settings(const Params& params,
                              const PassContext& context) const;

    /**
     * @brief TransformMath::inverseTransformCanonical() of the transform knobs at amount.
     **/
    bool transform(const Params& params,
                   const PassContext& context,
                   double amount,
                   bool invert,
                   TransformMath::Mat3* matrix) const;

    /**
     * @brief Whether params ask a render to average several matrices per pixel: motion blur on,
     * with an open shutter or a directional blur.
     **/
    static bool rendersWithMotionBlur(const Params& params);

    /**
     * @brief Heavy when the render averages several matrices per pixel, as rendersWithMotionBlur()
     * says; empty otherwise, for the Resample default.
     **/
    std::optional<GpuWeight> gpuWeight(const Params& params,
                                       const PassContext& context) const;

    KnobDoubleWPtr translate;
    KnobDoubleWPtr rotate;
    KnobDoubleWPtr scale;
    KnobDoubleWPtr skewX;
    KnobDoubleWPtr skewY;
    KnobChoiceWPtr skewOrder;
    KnobDoubleWPtr amount;
    KnobDoubleWPtr center;
    KnobButtonWPtr resetCenter;
    KnobBoolWPtr centerChanged;
    KnobBoolWPtr interactive;
    KnobBoolWPtr hiDPI;
    KnobBoolWPtr invert;
    KnobChoiceWPtr filter;
    KnobBoolWPtr clamp;
    KnobBoolWPtr blackOutside;
    KnobDoubleWPtr motionBlur;
    KnobBoolWPtr directionalBlur;
    KnobDoubleWPtr shutter;
    KnobChoiceWPtr shutterOffset;
    KnobDoubleWPtr shutterCustomOffset;
};

static_assert(GpuResampleOp<TransformOp>);

/**
 * @brief Moves op's centre to the middle of sourceRoD, changing translate so that the image at
 * full amount does not move, as the OpenFX plug-in's resetCenter does. Shared by the nodes built
 * on TransformOp's knobs.
 **/
void resetTransformCenter(KnobHolder* holder, const TransformOp& op, const RectD& sourceRoD, double time);

/**
 * @brief Transform: translates, rotates, scales and skews the image about a centre, with an
 * optional Mask input and the maskInvert and mix knobs.
 *
 * It always applies the transforms the host concatenates into its source. It offers its own
 * matrix, so a chain of transforms is resampled once, only while no mask is applied and mix is 1:
 * where mask x mix is below 1 it shows its immediate input, which under concatenation it
 * resamples through the upstream transforms alone.
 *
 * The class is not called Transform because that name is the engine's matrix namespace.
 **/
class TransformNode
    : public ImageNode<TransformOp, kTransformNodeSpec, kTransformPipeline> {
    using Base = ImageNode<TransformOp, kTransformNodeSpec, kTransformPipeline>;

public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new TransformNode(node);
    }

    explicit TransformNode(NodePtr node);

    virtual ~TransformNode();

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    /**
     * @brief Whether the node offers its matrix: not while a mask is applied. Mix is decided per
     * time, in getTransform().
     **/
    virtual bool getCanTransform() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The base's matrix, or the default reply while a mask is applied or mix is not 1.
     **/
    virtual StatusEnum getTransform(double time,
                                    const RenderScale& renderScale,
                                    bool draftRender,
                                    ViewIdx view,
                                    EffectInstancePtr* inputToTransform,
                                    Transform::Matrix3x3* transform) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The transform knob values at (time, view).
     **/
    void getTransformParams(double time, ViewIdx view, TransformMath::TransformParams* params) const;

    /**
     * @brief Moves the centre to the middle of the source's region of definition (the project
     * window when that is empty), changing translate so the image does not move, as the
     * OpenFX plug-in's resetCenter does. Nothing happens without a source or with an infinite
     * source region.
     **/
    void resetCenter(double time);

    /**
     * @brief Whether the knobs ask a render to average several matrices per pixel at (time,
     * view): motion blur on, with an open shutter or a directional blur. A transform that does
     * not move over the shutter still counts, although the render then samples it once.
     **/
    bool rendersWithMotionBlur(double time, ViewIdx view) const WARN_UNUSED_RETURN;

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
    bool getSourceRoD(double time, const RenderScale& scale, ViewIdx view, RectD* rod) const;

    RectD getProjectRect() const WARN_UNUSED_RETURN;

    void updateShutterEnabled();

    void updateClampEnabled(double time, ViewSpec view);
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_Transform_h
