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

#ifndef Engine_Nodes_Image_Op_h
#define Engine_Nodes_Image_Op_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <concepts>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/ImagePipelineSpec.h"
#include "Engine/Nodes/Image/PassExecutor.h"
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/Nodes/Image/ResampleExecutor.h"
#include "Engine/Nodes/Image/TransformMath.h"
#include "Engine/Nodes/KnobReader.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief How an op runs on the GPU for one set of Params: not at all (None), as a kernel of
 * the GPU registry with its parameter block (Kernel, held in kernel as for
 * PixelKernelGpu), or as a runtime shader with the uniforms and textures it is dispatched with
 * (RuntimeShader). A Kernel's block is either a trivially copyable struct (compiled()) or words
 * the op lays out itself (packed()); a block over the recorder's 64 KB limit stays on the CPU.
 * acceptedComponents is the set of pixel component counts the GPU work handles, as bits
 * componentsBit(1) to componentsBit(4); a render of any other count stays on the CPU.
 **/
struct GpuOpRef {
    enum class Kind : unsigned char {
        None,
        Kernel,
        RuntimeShader
    };

    static constexpr unsigned char componentsBit(int nComps)
    {
        return (unsigned char)(1u << nComps);
    }

    static constexpr unsigned char kAllComponents = (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4);

    Kind kind = Kind::None;
    PixelKernelGpu kernel;
    GpuRuntimeShader shader;
    GpuShaderInputs shaderInputs;
    unsigned char acceptedComponents = kAllComponents;

    bool acceptsComponents(int nComps) const
    {
        return (nComps >= 1) && (nComps <= 4) && ((acceptedComponents & componentsBit(nComps)) != 0);
    }

    static GpuOpRef none()
    {
        return GpuOpRef();
    }

    template <typename P>
    static GpuOpRef compiled(int kernelId,
                             const P& params)
    {
        GpuOpRef ref;

        ref.kind = Kind::Kernel;
        ref.kernel.kernel = kernelId;
        ref.kernel.setParams(params);

        return ref;
    }

    static GpuOpRef packed(int kernelId,
                           std::vector<std::uint32_t> words)
    {
        GpuOpRef ref;

        ref.kind = Kind::Kernel;
        ref.kernel.kernel = kernelId;
        ref.kernel.params.resize(words.size() * sizeof(std::uint32_t));
        if (!words.empty()) {
            std::memcpy(ref.kernel.params.data(), words.data(), ref.kernel.params.size());
        }

        return ref;
    }

    static GpuOpRef runtime(const GpuRuntimeShader& shader,
                            const GpuShaderInputs& inputs)
    {
        GpuOpRef ref;

        ref.kind = Kind::RuntimeShader;
        ref.shader = shader;
        ref.shaderInputs = inputs;

        return ref;
    }
};

/**
 * @brief A Point op: the per-pixel maths of a node whose output pixel depends only on the same
 * pixel of its source.
 *
 * bake() reads the op's knobs once per evaluation into Params, which is copyable so that band
 * threads take it by value (see bakeParams()). isIdentity() says whether
 * those Params leave the source unchanged, whatever the stages do. row() is pure on its RowIO:
 * no knob access, no allocation and no I/O, called concurrently for different rows. gpu() says how the same Params run on the GPU.
 *
 * An op whose maths depends on the render scale takes the render's PassContext as well, in all
 * three members: isIdentity(params, context), row(params, context, io) and gpu(params, context).
 * An op has one form or the other, never a mix of both.
 **/
template <typename OP>
concept ScaleFreePointMembers = requires(const OP& op, const typename OP::Params& params, const RowIO& io) {
    { op.isIdentity(params) } -> std::convertible_to<bool>;
    { op.row(params, io) } -> std::same_as<void>;
    { op.gpu(params) } -> std::same_as<GpuOpRef>;
};

template <typename OP>
concept ScaledPointMembers = requires(const OP& op, const typename OP::Params& params, const PassContext& context, const RowIO& io) {
    { op.isIdentity(params, context) } -> std::convertible_to<bool>;
    { op.row(params, context, io) } -> std::same_as<void>;
    { op.gpu(params, context) } -> std::same_as<GpuOpRef>;
};

template <typename OP>
concept AnyScaleFreePointMember = requires(const OP& op, const typename OP::Params& params) { op.isIdentity(params); } || requires(const OP& op, const typename OP::Params& params, const RowIO& io) { op.row(params, io); } || requires(const OP& op, const typename OP::Params& params) { op.gpu(params); };

template <typename OP>
concept AnyScaledPointMember = requires(const OP& op, const typename OP::Params& params, const PassContext& context) { op.isIdentity(params, context); } || requires(const OP& op, const typename OP::Params& params, const PassContext& context, const RowIO& io) { op.row(params, context, io); } || requires(const OP& op, const typename OP::Params& params, const PassContext& context) { op.gpu(params, context); };

template <typename OP>
concept PointOp = BakesParams<OP> && std::copyable<typename OP::Params> && ((ScaleFreePointMembers<OP> && !AnyScaledPointMember<OP>) || (ScaledPointMembers<OP> && !AnyScaleFreePointMember<OP>));

/**
 * @brief A Point op of the form that takes the render's PassContext.
 **/
template <typename OP>
concept ScaledPointOp = PointOp<OP> && ScaledPointMembers<OP>;

/**
 * @brief A Passes op: a node whose output pixel depends on a neighbourhood of its source,
 * computed as a list of passes over planar scratch buffers.
 *
 * bake() and Params are as for a Point op. isIdentity() says whether those Params leave the
 * source unchanged at the context's scale. passes() builds the pass list for one render at that
 * scale; it reads no knob, so that the CPU and GPU executors and the regions the host derives
 * from the list (halo, region of definition) all see the same list for the same Params.
 **/
template <typename OP>
concept PassesOp = BakesParams<OP> && std::copyable<typename OP::Params> && requires(const OP& op, const typename OP::Params& params, const PassContext& context) {
    { op.isIdentity(params, context) } -> std::convertible_to<bool>;
    { op.passes(params, context) } -> std::same_as<PassList>;
};

/**
 * @brief What a Route op chooses from: the time and view asked for, and which inputs of its
 * node are connected.
 **/
class RouteContext {
public:
    RouteContext(const EffectInstance& node,
                 double time,
                 ViewIdx view)
        : _node(node)
        , _time(time)
        , _view(view)
    {
    }

    double time() const
    {
        return _time;
    }

    ViewIdx view() const
    {
        return _view;
    }

    bool isConnected(int inputNb) const
    {
        return bool(_node.getInput(inputNb));
    }

private:
    const EffectInstance& _node;
    double _time;
    ViewIdx _view;
};

/**
 * @brief A Route op: a node whose output is one of its inputs, possibly at another time, or
 * transparent black, and which never renders pixels itself.
 *
 * bake() and Params are as for a Point op. route() chooses from those Params and the context
 * which input the output is; the host answers identity, region of definition, frames needed and
 * metadata from that choice alone.
 **/
template <typename OP>
concept RouteOp = BakesParams<OP> && std::copyable<typename OP::Params> && requires(const OP& op, const typename OP::Params& params, const RouteContext& context) {
    { op.route(params, context) } -> std::same_as<Route>;
};

/**
 * @brief Whether an op of any kind has route(params, context), which the host then asks before
 * anything else: an answer other than None makes the node an identity of that input.
 **/
template <typename OP>
concept RoutesOp = requires(const OP& op, const typename OP::Params& params, const RouteContext& context) {
    { op.route(params, context) } -> std::same_as<Route>;
};

/**
 * @brief One input a Composite op reads in an evaluation. Every input is read at the evaluation's
 * time: frames needed and the GPU fold only ever ask for that time.
 **/
struct CompositeInput {
    int inputNb = -1;
};

/**
 * @brief What a Composite op folds in one evaluation: the base input the result starts from, the
 * further inputs its step() combines into that result one after the other, any number of them,
 * and the input the stages blend against.
 *
 * blendInput is the input the MaskMix and PassThroughUnprocessed stages blend with and copy from,
 * read at the evaluation's time, and the input every composed identity answers; -1 stands for the
 * base input. It need not be one of the folded inputs.
 **/
struct CompositeInputs {
    CompositeInput base;
    std::vector<CompositeInput> further;
    int blendInput = -1;

    void add(int inputNb)
    {
        further.push_back(CompositeInput { inputNb });
    }

    int blend() const
    {
        return (blendInput >= 0) ? blendInput : base.inputNb;
    }
};

/**
 * @brief A Composite op: a node whose output pixel is a fold over the same pixel of any number of
 * inputs.
 *
 * bake() and Params are as for a Point op. isIdentity() says whether those Params leave the blend
 * input unchanged. inputs() chooses from the Params and the context the inputs this evaluation
 * folds. The result starts as the base input's row, and step() combines it with each further
 * input's row in turn; with no further input the result is the base row.
 *
 * step() is pure on its RowIO, as a Point op's row() is. io.src[0] is the result so far and
 * io.src[1] the next input's row, io.nComps floats per pixel each, and step() writes the processed
 * channels (io.channels) of the new result to io.dst; the other channels of a result are never
 * read back as output. An input is read in the layer its InputSpec::layer names, and reads zero
 * wherever it has no pixel. No row is divided: a Composite op has no Premult stage, since no one
 * input's divisor applies to the others. Mask and mix apply once, to the final result.
 **/
template <typename OP>
concept CompositeOp = BakesParams<OP> && std::copyable<typename OP::Params> && requires(const OP& op, const typename OP::Params& params, const RouteContext& context, const RowIO& io) {
    { op.isIdentity(params) } -> std::convertible_to<bool>;
    { op.inputs(params, context) } -> std::same_as<CompositeInputs>;
    { op.step(params, io) } -> std::same_as<void>;
};

/**
 * @brief A Composite op that runs on the GPU: gpu(params) names a registry kernel whose entry is
 * built with runCompositeStep (Kernels/compositestep.slang), and its parameter block. The kernel
 * is dispatched once per further input. A Composite op without it stays on the CPU.
 **/
template <typename OP>
concept GpuCompositeOp = CompositeOp<OP> && requires(const OP& op, const typename OP::Params& params) {
    { op.gpu(params) } -> std::same_as<GpuOpRef>;
};

/**
 * @brief A Resample op: a node whose output is its source resampled through a 3x3 matrix, or
 * shifted by a whole number of pixels.
 *
 * bake() and Params are as for a Point op, and isIdentity() says whether those Params leave the
 * source unchanged at the context's scale. An op has one of two forms, never both.
 *
 * The matrix form has settings(), the filter, clamp, black-outside, invert and blur settings the
 * Resampler samples with, and transform(), the canonical matrix at a blend amount as
 * Resampler::CanonicalTransformFn describes it: with invert false the inverse (output to source)
 * matrix, with invert true the forward one, false when no matrix exists. The host asks for it at
 * every time a motion blur samples, with the Params baked at that time, and at every amount a
 * directional blur samples; amount is 1 for a plain render. The host concatenates through that
 * matrix: unless the node has the MaskMix stage, it offers it to the host, and it always applies
 * the transforms the host concatenates into its source. An op that has canConcatenate() decides
 * with it whether the node offers its matrix.
 *
 * The offset form has offset(), the whole-pixel translation at the context's scale; it neither
 * offers a matrix nor accepts one. It may also have matte(params, context), returning the
 * separable ResampleMatte that weighs each shifted pixel (an unset one weighs nothing), and
 * `static constexpr bool kClipsSourceToRoI = true` to read the source as zero outside the region
 * of interest it was fetched for. Either form may run on the GPU (GpuResampleOp), matte included.
 **/
template <typename OP>
concept MatrixResampleMembers = requires(const OP& op, const typename OP::Params& params, const PassContext& context, double amount, bool invert, TransformMath::Mat3* matrix) {
    { op.isIdentity(params, context) } -> std::convertible_to<bool>;
    { op.settings(params, context) } -> std::same_as<ResampleSettings>;
    { op.transform(params, context, amount, invert, matrix) } -> std::convertible_to<bool>;
};

template <typename OP>
concept OffsetResampleMembers = requires(const OP& op, const typename OP::Params& params, const PassContext& context) {
    { op.isIdentity(params, context) } -> std::convertible_to<bool>;
    { op.offset(params, context) } -> std::same_as<PixelOffset>;
};

template <typename OP>
concept ResampleOp = BakesParams<OP> && std::copyable<typename OP::Params> && (MatrixResampleMembers<OP> != OffsetResampleMembers<OP>);

template <typename OP>
concept MatrixResampleOp = ResampleOp<OP> && MatrixResampleMembers<OP>;

template <typename OP>
concept OffsetResampleOp = ResampleOp<OP> && OffsetResampleMembers<OP>;

/**
 * @brief A Resample op of either form that also runs on the GPU, which it opts into with
 * `static constexpr bool kResamplesOnGpu = true`; every other Resample op stays on the CPU. The
 * GPU renders the matrix form (GpuMatrixResampleOp) whenever its sampling matrices are affine and
 * no transform is concatenated into its source, motion and directional blur included, and the
 * offset form (GpuOffsetResampleOp) always.
 *
 * Any Resample op may also have costHint(params, context), returning
 * std::optional<GpuCostHint>, to describe its cost to the GPU cost model.
 **/
template <typename OP>
concept GpuResampleOp = ResampleOp<OP> && requires { requires OP::kResamplesOnGpu; };

template <typename OP>
concept GpuMatrixResampleOp = GpuResampleOp<OP> && MatrixResampleOp<OP>;

template <typename OP>
concept GpuOffsetResampleOp = GpuResampleOp<OP> && OffsetResampleOp<OP>;

/**
 * @brief Whether OP satisfies the concept of op kind KIND. Only Point, Composite, Passes, Route
 * and Resample ops are run by the host so far; every other kind is refused here.
 **/
template <typename OP, OpKind KIND>
concept OpOfKind = ((KIND == OpKind::Point) && PointOp<OP>) || ((KIND == OpKind::Composite) && CompositeOp<OP>) || ((KIND == OpKind::Passes) && PassesOp<OP>) || ((KIND == OpKind::Route) && RouteOp<OP>) || ((KIND == OpKind::Resample) && ResampleOp<OP>);

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Image_Op_h
