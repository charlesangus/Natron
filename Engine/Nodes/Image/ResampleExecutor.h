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

#ifndef Engine_Nodes_Image_ResampleExecutor_h
#define Engine_Nodes_Image_ResampleExecutor_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>

#include "Engine/EffectInstance.h"
#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Image/TransformMath.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/Transform.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

struct StagePlan;

/**
 * @brief How a Resample op of the matrix form samples, for one set of Params: the filter, clamp
 * and black-outside settings Resampler takes, the node's invert switch, its motion or directional
 * blur, and whether its matrix at amount 1 is the identity. The last one is what the region
 * helpers call isIdentity: it keeps the source's region unchanged, whatever the blur says, so it
 * can differ from the op's own identity, which also looks at the blur.
 **/
struct ResampleSettings {
    Resampler::FilterEnum filter = Resampler::eFilterCubic;
    bool clamp = false;
    bool blackOutside = true;
    bool invert = false;
    Resampler::BlurSettings blur;
    bool transformIsIdentity = false;
};

/**
 * @brief A whole-pixel translation at the render scale: output pixel (i, j) shows source pixel
 * (i - x, j - y).
 **/
struct PixelOffset {
    int x = 0;
    int y = 0;
};

/**
 * @brief A separable weight over the output pixels of an offset-form render at the render scale:
 * pixel (x, y) weighs w = column(x) * row(y), in double. A pixel weighing 0 is written 0, never
 * multiplied, so a NaN or infinite source pixel there becomes 0; one weighing 1 or more keeps its
 * value; any other has each channel multiplied by w in double and rounded to float. An unset
 * function weighs 1 everywhere, and a matte with neither set weighs nothing. The executors sample
 * both over the render window on the thread running the action or recording, never from band
 * threads.
 **/
struct ResampleMatte {
    std::function<double(int x)> column;
    std::function<double(int y)> row;

    bool isSet() const
    {
        return bool(column) || bool(row);
    }
};

/**
 * @brief What a Resample op resolves to for one action at one time, view and scale.
 *
 * Matrix: transform is the op's canonical matrix as Resampler::CanonicalTransformFn describes it,
 * reading the op's Params at whatever time it is asked for, and settings says how to sample
 * through it. Offset: the source is shifted by offset whole pixels; then, when set, matte
 * weights each shifted pixel of a plane whose source was fetched, and with clipSourceToRoI the
 * source reads zero outside the region of interest it was fetched for, as an OpenFX host shows
 * it. None: the node has no Resample op.
 *
 * transform is only valid while the node is, and is called on the thread running the action,
 * never from band threads.
 **/
struct ResampleRequest {
    enum class Form : unsigned char {
        None,
        Matrix,
        Offset
    };

    Form form = Form::None;
    ResampleSettings settings;
    Resampler::CanonicalTransformFn transform;
    PixelOffset offset;
    ResampleMatte matte;
    bool clipSourceToRoI = false;
};

/**
 * @brief The stage state the region actions of a Resample op depend on: whether the node has the
 * MaskMix stage, whether its mask is connected and enabled (doMasking), its mix knob (1 without
 * the stage), and its mask input, -1 for none.
 **/
struct ResampleRegionStages {
    bool maskMix = false;
    bool doMasking = false;
    double mix = 1.;
    int maskInput = -1;
};

/**
 * @brief Renders a Resample op through plan on the CPU.
 *
 * The source is fetched with the transform the host concatenated into it, if any, and read as a
 * whole image in the output plane's layout. Each output row is the source resampled through the
 * request's matrices (Resampler::buildSamplingTransforms() then resampleRow(), with the impulse
 * filter for a draft render) or the source shifted by the request's offset, weighted by its
 * matte and clipped to its region of interest as the request asks. That row is blended
 * with the reference row by mask x mix, and with PassThroughUnprocessed the unprocessed channels
 * are copied from it, exactly as runStagesCpu() does. The reference row is the source under the
 * output pixel, or, when the host concatenated a transform and a stage reads the reference, the
 * source resampled through that transform alone with black outside, so that it shows the node's
 * immediate input. The Premult stage is not supported and ignored.
 **/
StatusEnum runResampleCpu(EffectInstance& effect,
                          const StagePlan& plan,
                          const EffectInstance::RenderActionArgs& args,
                          const ResampleRequest& request) WARN_UNUSED_RETURN;

/**
 * @brief The region of definition of a Resample op whose source is input sourceInput, which must
 * be connected. A source whose region fails fails. Matrix: Resampler::getRegionOfDefinition()
 * with the request's settings. Offset: the source's region shifted by the offset in canonical
 * units, a default reply for an empty source region, and under a mask the source's region for a
 * mix of 0, else unioned with it.
 **/
StatusEnum resampleRegionOfDefinition(EffectInstance& effect,
                                      int sourceInput,
                                      const ResampleRequest& request,
                                      const ResampleRegionStages& stages,
                                      double time,
                                      const RenderScale& scale,
                                      ViewIdx view,
                                      RectD* rod) WARN_UNUSED_RETURN;

/**
 * @brief Replaces the source's entry of ret, when the source is connected, with what a render of
 * renderWindow reads from it. Matrix: Resampler::getRegionOfInterest() with the request's
 * settings and the project's rectangle, then, with the MaskMix stage and a mask applied or a mix
 * other than 1, unioned with renderWindow grown by the filter's reach, where the reference row is
 * read. Offset: renderWindow shifted back by the offset and clipped to the source's region, or
 * renderWindow itself for a zero offset or an empty source region, then the same union with
 * renderWindow. A mask that is the source's effect also reads renderWindow.
 **/
void resampleRegionsOfInterest(EffectInstance& effect,
                               int sourceInput,
                               const ResampleRequest& request,
                               const ResampleRegionStages& stages,
                               double time,
                               const RenderScale& scale,
                               const RectD& renderWindow,
                               ViewIdx view,
                               RoIMap* ret);

/**
 * @brief The pixel matrix a matrix-form Resample op hands the host for concatenation, from source
 * pixels to output pixels at renderScale, and the source input it applies to. A default reply for
 * the offset form, for a matrix that does not exist or is singular, and, outside a draft render,
 * for a render that blurs over time or along the transform. Fails without a connected source.
 **/
StatusEnum resampleTransform(EffectInstance& effect,
                             int sourceInput,
                             const ResampleRequest& request,
                             double time,
                             const RenderScale& renderScale,
                             bool draftRender,
                             EffectInstancePtr* inputToTransform,
                             Transform::Matrix3x3* transform) WARN_UNUSED_RETURN;

/**
 * @brief Whether recordResampleGpu() can render request through plan for query: a plan the GPU
 * stages run (canRunStagesGpu()) for no more components than the kernels take, then, for the
 * matrix form, no transform concatenated into the source, and sampling matrices at the query's
 * time, view and scale that are all affine and no more than the kernel takes (motion and
 * directional blur are accepted). The offset form, matte included, always qualifies: the host
 * never concatenates into its source. Depends only on the query and the node's settings, so
 * placement and recording agree.
 **/
bool canRecordResampleGpu(const EffectInstance& effect,
                          const StagePlan& plan,
                          const GpuQuery& query,
                          const ResampleRequest& request) WARN_UNUSED_RETURN;

/**
 * @brief The pixels of the source, input sourceInput, that recordResampleGpu() reads to produce
 * outputRect at time, view and scale: the source's entry of the node's own regions of interest
 * (EffectInstance::getRegionsOfInterest_public()) over outputRect, enclosed in pixels, so that a
 * node overriding them is read as the CPU reads it. outputRect itself without a connected source,
 * for an empty outputRect, or when the node names no region for the source.
 **/
RectI resampleGpuSourceRoI(const EffectInstance& effect,
                           int sourceInput,
                           double time,
                           ViewIdx view,
                           const RectI& outputRect,
                           const RenderScale& scale) WARN_UNUSED_RETURN;

/**
 * @brief Records request over args.outputRect of args.output through plan, as runResampleCpu()
 * renders it. The source is clipped to its region of definition and, for the matrix form or with
 * clipSourceToRoI, to resampleGpuSourceRoI(). The matrix form resamples it through the request's
 * matrices (averaged for a blur) with the impulse filter for a draft render; the offset form
 * shifts it by the request's offset, zero outside it, and weighs it by the request's matte as
 * runResampleCpu() does, the weights sampled over outputRect. With a mask, a mix other than 1 or
 * unprocessed channels, the result goes through recordStagesGpuFinish() against that same
 * clipped source, unshifted. Fails for a request canRecordResampleGpu() refuses.
 **/
StatusEnum recordResampleGpu(EffectInstance& effect,
                             const StagePlan& plan,
                             const GpuRenderArgs& args,
                             const ResampleRequest& request) WARN_UNUSED_RETURN;

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Image_ResampleExecutor_h
