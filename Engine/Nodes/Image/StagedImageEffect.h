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

#ifndef Engine_Nodes_Image_StagedImageEffect_h
#define Engine_Nodes_Image_StagedImageEffect_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <concepts>
#include <list>
#include <optional>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/Image/ImagePipelineSpec.h"
#include "Engine/Nodes/Image/Op.h"
#include "Engine/Nodes/Image/PassExecutor.h"
#include "Engine/Nodes/Image/ResampleExecutor.h"
#include "Engine/Nodes/Image/StagePlan.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/NodeSpec.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/Transform.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Base for flat 2D native image nodes described by a NodeSpec and an ImagePipelineSpec:
 * float-only, tile-capable, multi-resolution, render scale supported, and FullySafe.
 *
 * The host's side of the node is fixed here and follows the pipeline spec: the layer knob, the
 * channel checkboxes, the "(Un)premult by" selector, and how render() and isIdentity() wrap the
 * op. A node supplies only its op, through renderOp() and isOpIdentity(), which ImageNode
 * implements from an Op type.
 *
 * Identity is composed: a route() other than None, then, with the MaskMix stage, a mix of 0,
 * then the op's own identity, then, with the MaskMix stage, a non-inverted mask whose region
 * misses the window. Every stage identity is one of the node's first Source input at the same
 * time.
 *
 * A Passes op also gives the node its regions: the source's region of interest is the window
 * grown by the halo of the op's passes, and the region of definition the source's grown by their
 * spread, which clipRegionOfDefinition() may then narrow.
 *
 * A Route op renders nothing: route() makes the node an identity everywhere, and its region of
 * definition, frames needed and metadata are those of the input it routes to, or empty for
 * Black. Its render() fails, as the engine never calls it for an identity.
 *
 * A Resample op gives the node its regions through ResampleExecutor, and, for the matrix form,
 * the transform concatenation answers: the node applies the transforms the host concatenates into
 * its source, and offers its own matrix unless it has the MaskMix stage. It renders on the CPU,
 * and on the GPU too when the op opts in (GpuResampleOp).
 **/
class StagedImageEffect
    : public NativeEffectBase {
public:
    explicit StagedImageEffect(NodePtr node);

    virtual ~StagedImageEffect();

    /**
     * @brief How the host runs this node. Must outlive the node and never change.
     **/
    virtual const ImagePipelineSpec& imagePipelineSpec() const WARN_UNUSED_RETURN = 0;

    virtual bool supportsTiles() const OVERRIDE WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual bool supportsMultiResolution() const OVERRIDE WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE WARN_UNUSED_RETURN
    {
        return eRenderSafetyFullySafe;
    }

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE;
    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE;

    virtual bool wantsHostUnPremultSelector() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool rendersUnprocessedChannels() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual LayerKnobSpec getLayerKnobSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool defaultProcessesAllLayers() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool isHostChannelSelectorSupported(bool* defaultR,
                                                bool* defaultG,
                                                bool* defaultB,
                                                bool* defaultA) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The op kind of imagePipelineSpec().
     **/
    virtual std::optional<OpKind> opKind() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief For a Point op, whether the GPU executor can run the StagePlan of query
     * (canRunStagesGpu()) and the op's gpu() description at the query's time, view and scale names work
     * the device runs for pixels of the query's component count. A runtime shader that does not build is reported once and never built
     * again for this node. For a Passes op, canRecordPassesGpu() with the op's passes at the
     * query's time, view and scale. For a Resample op that opts into the GPU, canRecordResampleGpu()
     * with the op's request at the query's time, view and scale. Every other op stays on the CPU.
     **/
    virtual bool canRenderOnGpu(const GpuQuery& query) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief When inputNb is the source, at the current time and view: for a Passes op, outputRect
     * grown by the halo of the op's passes; for a Resample op that opts into the GPU,
     * resampleGpuSourceRoI(). outputRect otherwise.
     **/
    virtual RectI getGpuInputRoI(int inputNb,
                                 const RectI& outputRect,
                                 const RenderScale& scale) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief For a Route op, the region of the input route() chooses, at the time it chooses,
     * and an empty region for Black or an unconnected input.
     * For a Resample op with a connected source, resampleRegionOfDefinition() with the mask and
     * mix the MaskMix stage applies.
     * For a Passes op with a connected source whose region is not empty: the default
     * region when the op is an identity, else the source's region grown by the spread of the op's
     * passes (passListRoD()). Then clipRegionOfDefinition() may change it; the default region when
     * neither did, and for every other op kind.
     **/
    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE WARN_UNUSED_RETURN;

    /**
     * @brief For a Point or Passes op, or a Resample op that opts into the GPU, the layer its plane
     * is read in, except for the mask input, which is read in the layer its mask channel resolves
     * in. False for every other op.
     **/
    virtual bool getGpuInputLayer(int inputNb,
                                  const ImageLayerDesc& plane,
                                  double time,
                                  ViewIdx view,
                                  ImageLayerDesc* layer) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief Records the op's gpu() description through the StagePlan of args.query with
     * recordStagesGpu(), a Passes op's passes with recordPassesGpu(), or a Resample op's request
     * with recordResampleGpu(). Only called when canRenderOnGpu() accepted args.query.
     **/
    virtual StatusEnum renderGpu(const GpuRenderArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The mask, mix and divisor of the StagePlan a render at time and view resolves.
     **/
    virtual GpuStageSummary getGpuStageSummary(double time,
                                               ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief For a matrix-form Resample op, the source input; false for every other op.
     **/
    virtual bool getInputsHoldingTransform(std::list<int>* inputs) const OVERRIDE WARN_UNUSED_RETURN;

    /**
     * @brief For a matrix-form Resample op without the MaskMix stage, whether the op lets the node
     * offer its matrix (true unless the op's canConcatenate() says otherwise); false for every
     * other op.
     **/
    virtual bool getCanTransform() const OVERRIDE WARN_UNUSED_RETURN;

    /**
     * @brief For a Resample op, resampleTransform() at time, view and renderScale; the default
     * reply for every other op.
     **/
    virtual StatusEnum getTransform(double time,
                                    const RenderScale& renderScale,
                                    bool draftRender,
                                    ViewIdx view,
                                    EffectInstancePtr* inputToTransform,
                                    Transform::Matrix3x3* transform) OVERRIDE WARN_UNUSED_RETURN;

    /**
     * @brief The input the op reads and the stages blend against: the first Source input of
     * nodeSpec(), -1 when it has none, and input 0 for a node without a spec.
     **/
    int getSourceInput() const WARN_UNUSED_RETURN;

    /**
     * @brief The input the MaskMix stage reads as its mask: the first input described as a mask,
     * or -1.
     **/
    int getMaskInput() const WARN_UNUSED_RETURN;

protected:
    /**
     * @brief Declares maskInvert (false) and mix (1, range 0..1) with the OpenFX names, labels
     * and hints, and adds them to page. The host moves both to the end of the page.
     **/
    void addMaskMixKnobs(const KnobPagePtr& page);

    /**
     * @brief The page the op's knobs go on, which ImageNode creates before its op's knobs.
     **/
    KnobPagePtr createControlsPage();

    /**
     * @brief Adds the stage knobs the pipeline spec asks for to page, after the op's knobs.
     **/
    void addStageKnobs(const KnobPagePtr& page);

    double getMixValue(double time, ViewIdx view) const WARN_UNUSED_RETURN;
    bool getMaskInvertValue(double time, ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief Whether the MaskMix stage reads a mask: the mask input is connected and enabled.
     **/
    bool isMaskApplied() const WARN_UNUSED_RETURN;

    /**
     * @brief What a Passes op's passes, or a scaled Point op's members, are built for at scale: the
     * scale, and the pixel aspect ratio of the source input when it is connected and has one.
     **/
    PassContext getPassContext(const RenderScale& scale) const WARN_UNUSED_RETURN;

    /**
     * @brief For a Passes op, the default regions of interest, with the source's replaced by the
     * window grown by the halo of the op's passes, merged with the window under a mask with a mix
     * other than 1. A mask with a mix of 0 keeps the defaults, as does every other op kind except
     * Resample, whose source entry resampleRegionsOfInterest() replaces.
     **/
    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE;

    /**
     * @brief For a Route op, only the input route() chooses, at the time it chooses, and no
     * input for Black; the default for every other op kind.
     **/
    virtual FramesNeededMap getFramesNeeded(double time,
                                            ViewIdx view) OVERRIDE WARN_UNUSED_RETURN;

    /**
     * @brief For a Route op, the metadata of the input route() chooses, at the time it chooses,
     * and none for Black; the upstream metadata for every other op kind.
     **/
    virtual ImageMetadata deriveOutputMetadata(double time,
                                               ViewIdx view) OVERRIDE WARN_UNUSED_RETURN;

    virtual bool isIdentity(double time,
                            const RenderScale& scale,
                            const RectI& roi,
                            ViewIdx view,
                            double* inputTime,
                            ViewIdx* inputView,
                            int* inputNb) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief Resolves the StagePlan once and hands it to renderOp().
     **/
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief Whether the op alone leaves its source unchanged at (time, view) over roi.
     **/
    virtual bool isOpIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view)
        = 0;

    /**
     * @brief Renders args through plan, the stages resolved for this render.
     **/
    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& plan)
        = 0;

    /**
     * @brief How the op runs on the GPU at time, view and scale; none, the default, keeps it on
     * the CPU.
     **/
    virtual GpuOpRef gpuOp(double /*time*/,
                           ViewIdx /*view*/,
                           const RenderScale& /*scale*/) const WARN_UNUSED_RETURN
    {
        return GpuOpRef::none();
    }

    /**
     * @brief A Passes op's passes at time, view and scale; the default is an empty list.
     **/
    virtual PassList opPasses(double /*time*/,
                              ViewIdx /*view*/,
                              const RenderScale& /*scale*/) const WARN_UNUSED_RETURN
    {
        return PassList();
    }

    /**
     * @brief What a Resample op resolves to at time, view and scale; the default is Form::None.
     **/
    virtual ResampleRequest opResample(double /*time*/,
                                       ViewIdx /*view*/,
                                       const RenderScale& /*scale*/) const WARN_UNUSED_RETURN
    {
        return ResampleRequest();
    }

    /**
     * @brief Whether the op is a Resample op of the matrix form; the default is false.
     **/
    virtual bool opHasMatrix() const WARN_UNUSED_RETURN
    {
        return false;
    }

    /**
     * @brief Whether the op is a Resample op that runs on the GPU (GpuResampleOp); the default is
     * false.
     **/
    virtual bool opResamplesOnGpu() const WARN_UNUSED_RETURN
    {
        return false;
    }

    /**
     * @brief Whether a matrix-form Resample op lets the node offer its matrix to the host; the
     * default is false.
     **/
    virtual bool opCanConcatenate() const WARN_UNUSED_RETURN
    {
        return false;
    }

    /**
     * @brief Lets a node narrow the region of definition its passes give, in pixels at scale:
     * rod, which is srcRoD, the source's region, grown unless that is empty. Returns whether it
     * changed rod; the default changes nothing.
     **/
    virtual bool clipRegionOfDefinition(double /*time*/,
                                        const RenderScale& /*scale*/,
                                        ViewIdx /*view*/,
                                        const EffectInstancePtr& /*source*/,
                                        const RectI& /*srcRoD*/,
                                        RectI* /*rod*/)
    {
        return false;
    }

private:
    StagePlan resolveGpuPlan(const GpuQuery& query) const WARN_UNUSED_RETURN;

    ResampleRegionStages getResampleRegionStages(double time,
                                                 ViewIdx view) const WARN_UNUSED_RETURN;

    StageKnobs _stageKnobs;
    mutable StageGpuShaderGate _gpuShaderGate;
};

/**
 * @brief An image node made of an Op, its NodeSpec and its ImagePipelineSpec, the last two
 * defaulting to OP::kNodeSpec and OP::kPipeline. The op is a member, so its bake() can read
 * knob handles it holds. When OP has createKnobs(KnobHolder*, const KnobPagePtr&), the
 * node's knobs are the op's on a "Controls" page, followed by the stage knobs; a subclass that
 * needs more overrides initializeKnobs() and calls addStageKnobs() itself.
 **/
template <typename OP, const NodeSpec& SPEC = OP::kNodeSpec, const ImagePipelineSpec& PIPELINE = OP::kPipeline>
    requires OpOfKind<OP, PIPELINE.op>
class ImageNode
    : public StagedImageEffect {
    static_assert((PIPELINE.op != OpKind::Resample) || !PIPELINE.stages.has(Stage::Premult), "a Resample op resamples the source as stored, so it has no Premult stage");

public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new ImageNode(node);
    }

    explicit ImageNode(NodePtr node)
        : StagedImageEffect(node)
        , _op()
    {
    }

    virtual ~ImageNode()
    {
    }

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<SPEC>();
    }

    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticImagePipelineSpec<PIPELINE, SPEC>();
    }

    /**
     * @brief For a Route op, the op's route() at time and view; None for every other op kind.
     **/
    virtual Route route(double time,
                        ViewIdx view) OVERRIDE WARN_UNUSED_RETURN
    {
        if constexpr (PIPELINE.op == OpKind::Route) {
            return _op.route(bakeParams(_op, time, view), RouteContext(*this, time, view));
        } else {
            return StagedImageEffect::route(time, view);
        }
    }

    /**
     * @brief For a Passes op, the op's costHint(params, context) when it has one, else
     * passListCostHint() of its passes, at time, view and mipmapLevel. For a Resample op, its
     * costHint(params, context) when it has one. Empty otherwise.
     **/
    virtual std::optional<GpuCostHint> gpuCostHint(double time,
                                                   ViewIdx view,
                                                   unsigned int mipmapLevel) const OVERRIDE WARN_UNUSED_RETURN
    {
        if constexpr (PIPELINE.op == OpKind::Passes) {
            const typename OP::Params params = bakeParams(_op, time, view);
            const PassContext context = getPassContext(RenderScale::fromMipmapLevel(mipmapLevel));

            if constexpr (requires(const OP& o, const typename OP::Params& p, const PassContext& c) {
                              { o.costHint(p, c) } -> std::same_as<std::optional<GpuCostHint>>;
                          }) {
                return _op.costHint(params, context);
            } else {
                return passListCostHint(_op.passes(params, context));
            }
        } else if constexpr (PIPELINE.op == OpKind::Resample) {
            if constexpr (requires(const OP& o, const typename OP::Params& p, const PassContext& c) {
                              { o.costHint(p, c) } -> std::same_as<std::optional<GpuCostHint>>;
                          }) {
                return _op.costHint(bakeParams(_op, time, view), getPassContext(RenderScale::fromMipmapLevel(mipmapLevel)));
            } else {
                return std::nullopt;
            }
        } else {
            return std::nullopt;
        }
    }

protected:
    OP& op()
    {
        return _op;
    }

    const OP& op() const
    {
        return _op;
    }

    virtual void initializeKnobs() OVERRIDE
    {
        const KnobPagePtr page = createControlsPage();

        if constexpr (requires(OP& o, KnobHolder* holder, const KnobPagePtr& p) { o.createKnobs(holder, p); }) {
            _op.createKnobs(static_cast<KnobHolder*>(this), page);
        }
        addStageKnobs(page);
    }

private:
    virtual bool isOpIdentity(double time,
                              const RenderScale& scale,
                              const RectI& /*roi*/,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        if constexpr ((PIPELINE.op == OpKind::Passes) || (PIPELINE.op == OpKind::Resample)) {
            return _op.isIdentity(bakeParams(_op, time, view), getPassContext(scale));
        } else if constexpr (PIPELINE.op == OpKind::Route) {
            return false;
        } else if constexpr (ScaledPointOp<OP>) {
            return _op.isIdentity(bakeParams(_op, time, view), getPassContext(scale));
        } else {
            return _op.isIdentity(bakeParams(_op, time, view));
        }
    }

    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& plan) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        if constexpr (PIPELINE.op == OpKind::Route) {
            Q_UNUSED(args);
            Q_UNUSED(plan);

            return eStatusFailed;
        } else if constexpr (PIPELINE.op == OpKind::Passes) {
            const typename OP::Params params = bakeParams(_op, args.time, args.view);

            return runPassesCpu(*this, plan, args, _op.passes(params, getPassContext(args.mappedScale)));
        } else if constexpr (PIPELINE.op == OpKind::Resample) {
            return runResampleCpu(*this, plan, args, opResample(args.time, args.view, args.mappedScale));
        } else if constexpr (ScaledPointOp<OP>) {
            const typename OP::Params params = bakeParams(_op, args.time, args.view);
            const PassContext context = getPassContext(args.mappedScale);
            const OP& rowOp = _op;
            const auto row = [&rowOp, &params, &context](const RowIO& io) {
                rowOp.row(params, context, io);
            };

            return runStagesCpu(*this, plan, args, StageRowFn(row));
        } else {
            const typename OP::Params params = bakeParams(_op, args.time, args.view);
            const OP& rowOp = _op;
            const auto row = [&rowOp, &params](const RowIO& io) {
                rowOp.row(params, io);
            };

            return runStagesCpu(*this, plan, args, StageRowFn(row));
        }
    }

    virtual GpuOpRef gpuOp(double time,
                           ViewIdx view,
                           const RenderScale& scale) const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        if constexpr (PIPELINE.op != OpKind::Point) {
            return GpuOpRef::none();
        } else if constexpr (ScaledPointOp<OP>) {
            return _op.gpu(bakeParams(_op, time, view), getPassContext(scale));
        } else {
            return _op.gpu(bakeParams(_op, time, view));
        }
    }

    virtual PassList opPasses(double time,
                              ViewIdx view,
                              const RenderScale& scale) const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        if constexpr (PIPELINE.op == OpKind::Passes) {
            return _op.passes(bakeParams(_op, time, view), getPassContext(scale));
        } else {
            return PassList();
        }
    }

    virtual ResampleRequest opResample(double time,
                                       ViewIdx view,
                                       const RenderScale& scale) const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        if constexpr (PIPELINE.op != OpKind::Resample) {
            Q_UNUSED(time);
            Q_UNUSED(view);
            Q_UNUSED(scale);

            return ResampleRequest();
        } else {
            const PassContext context = getPassContext(scale);
            const typename OP::Params params = bakeParams(_op, time, view);
            ResampleRequest request;

            if constexpr (MatrixResampleOp<OP>) {
                request.form = ResampleRequest::Form::Matrix;
                request.settings = _op.settings(params, context);
                const OP* op = &_op;
                request.transform = [op, params, context, time, view](double t, double amount, bool invert, TransformMath::Mat3* matrix) -> bool {
                    if (t == time) {
                        return op->transform(params, context, amount, invert, matrix);
                    }

                    return op->transform(bakeParams(*op, t, view), context, amount, invert, matrix);
                };
            } else {
                request.form = ResampleRequest::Form::Offset;
                request.offset = _op.offset(params, context);
                if constexpr (requires(const OP& o, const typename OP::Params& p, const PassContext& c) {
                                  { o.rowMatte(p, c) } -> std::same_as<ResampleRowMatte>;
                              }) {
                    request.rowMatte = _op.rowMatte(params, context);
                }
                if constexpr (requires { requires OP::kClipsSourceToRoI; }) {
                    request.clipSourceToRoI = true;
                }
            }

            return request;
        }
    }

    virtual bool opHasMatrix() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        if constexpr (PIPELINE.op == OpKind::Resample) {
            return MatrixResampleOp<OP>;
        } else {
            return false;
        }
    }

    virtual bool opResamplesOnGpu() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        if constexpr (PIPELINE.op == OpKind::Resample) {
            return GpuResampleOp<OP>;
        } else {
            return false;
        }
    }

    virtual bool opCanConcatenate() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        if constexpr ((PIPELINE.op == OpKind::Resample) && MatrixResampleOp<OP>) {
            if constexpr (requires(const OP& o) {
                              { o.canConcatenate() } -> std::convertible_to<bool>;
                          }) {
                return _op.canConcatenate();
            } else {
                return true;
            }
        } else {
            return false;
        }
    }

    OP _op;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Image_StagedImageEffect_h
