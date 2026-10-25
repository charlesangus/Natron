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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "EdgeExtend.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Filter/BlurKernels.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuBlur.h"
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
// The running result, filled with the source; the copy of it each slice blurs; the source's alpha,
// in its plane 3 only.
const int kSlotResult = 0;
const int kSlotBlur = 1;
const int kSlotAlpha = 2;

// The planes of a pixel the node works on: red, green, blue and alpha, in that order.
const int kNPlanes = 4;
const int kAlphaPlane = 3;

/// The size of each axis in pixels at the render scale.
struct PixelSizes {
    double x;
    double y;
};

PixelSizes
pixelSizes(const EdgeExtendParams& params,
           const PassContext& context)
{
    const OfxPointD rs = context.scale.toOfxPointD();
    PixelSizes sizes;

    sizes.x = rs.x * (params.sizeX / context.pixelAspectRatio);
    sizes.y = rs.y * params.sizeY;

    return sizes;
}

/// One slice's blur: the standard deviation and the FIR radius of each axis.
struct Slice {
    double sigmaX;
    double sigmaY;
    int radiusX;
    int radiusY;
};

/// The slices whose blur changes something, in order; the others merge the result over itself
/// unblurred, which the node leaves out.
std::vector<Slice>
activeSlices(const EdgeExtendParams& params,
             const PixelSizes& s)
{
    const std::vector<double> sx = EdgeExtend::sliceSigmas(s.x, params.slices);
    const std::vector<double> sy = EdgeExtend::sliceSigmas(s.y, params.slices);
    std::vector<Slice> out;

    for (std::size_t i = 0; i < sx.size(); ++i) {
        Slice slice;
        slice.sigmaX = sx[i];
        slice.sigmaY = sy[i];
        slice.radiusX = BlurKernels::LineFilter::firGaussianRadius(sx[i]);
        slice.radiusY = BlurKernels::LineFilter::firGaussianRadius(sy[i]);
        if (slice.radiusX || slice.radiusY) {
            out.push_back(slice);
        }
    }

    return out;
}

using LineFilterPtr = std::shared_ptr<const BlurKernels::LineFilter>;

/// Filters each line the executor hands over with filter, through a scratch owned by the chunk.
/// A FIR column block always goes through applyColumns(), even a last block of one column.
std::function<PassLineFn()>
lineWorker(const LineFilterPtr& filter,
           bool columns)
{
    if (filter->isIdentity()) {
        return std::function<PassLineFn()>();
    }

    return [filter, columns]() -> PassLineFn {
        std::shared_ptr<BlurKernels::LineScratch> scratch = std::make_shared<BlurKernels::LineScratch>();

        return [filter, columns, scratch](float* line, int length, std::ptrdiff_t stride, int count) {
            if (columns) {
                filter->applyColumns(line, length, stride, count, *scratch);
            } else {
                filter->apply(line, length, stride, *scratch);
            }
        };
    };
}

// The source's alpha saved, and its colour premultiplied by it when asked. Like the passes after
// it, a no-op unless the four channels are processed.
std::function<PassRowFn(const PassScratch&)>
prepareWorker(bool premultiply)
{
    return [premultiply](const PassScratch& scratch) -> PassRowFn {
        return [&scratch, premultiply](int row) {
            if (scratch.nPlanes != kNPlanes) {
                return;
            }
            const std::size_t offset = (std::size_t)row * scratch.width;
            const float* alpha = scratch.plane(kSlotResult, kAlphaPlane) + offset;

            std::copy(alpha, alpha + scratch.width, scratch.plane(kSlotAlpha, kAlphaPlane) + offset);
            if (!premultiply) {
                return;
            }
            for (int p = 0; p < kAlphaPlane; ++p) {
                float* colour = scratch.plane(kSlotResult, p) + offset;
                for (int i = 0; i < scratch.width; ++i) {
                    colour[i] *= alpha[i];
                }
            }
        };
    };
}

// The running result copied into the buffer a slice blurs.
std::function<PassRowFn(const PassScratch&)>
copyWorker()
{
    return [](const PassScratch& scratch) -> PassRowFn {
        return [&scratch](int line) {
            const int p = line / scratch.height;
            const int row = line % scratch.height;
            const std::size_t offset = (std::size_t)row * scratch.width;
            const float* result = scratch.plane(kSlotResult, p) + offset;

            std::copy(result, result + scratch.width, scratch.plane(kSlotBlur, p) + offset);
        };
    };
}

// The running result over its blur, a + b * (1 - alpha a): what edgeextend.slang's over computes.
std::function<PassRowFn(const PassScratch&)>
overWorker()
{
    return [](const PassScratch& scratch) -> PassRowFn {
        return [&scratch](int row) {
            if (scratch.nPlanes != kNPlanes) {
                return;
            }
            const std::size_t offset = (std::size_t)row * scratch.width;
            float* alpha = scratch.plane(kSlotResult, kAlphaPlane) + offset;

            for (int i = 0; i < scratch.width; ++i) {
                const float t = 1.f - alpha[i];
                for (int p = 0; p < kNPlanes; ++p) {
                    float* a = scratch.plane(kSlotResult, p) + offset;
                    a[i] = a[i] + scratch.plane(kSlotBlur, p)[offset + i] * t;
                }
            }
        };
    };
}

// The colour divided by the grown alpha where that exceeds the epsilon, and the source's alpha
// restored.
std::function<PassRowFn(const PassScratch&)>
unpremultiplyWorker()
{
    return [](const PassScratch& scratch) -> PassRowFn {
        return [&scratch](int row) {
            if (scratch.nPlanes != kNPlanes) {
                return;
            }
            const std::size_t offset = (std::size_t)row * scratch.width;
            float* alpha = scratch.plane(kSlotResult, kAlphaPlane) + offset;

            for (int p = 0; p < kAlphaPlane; ++p) {
                float* colour = scratch.plane(kSlotResult, p) + offset;
                for (int i = 0; i < scratch.width; ++i) {
                    if (alpha[i] > EdgeExtend::kAlphaEpsilon) {
                        colour[i] = colour[i] / alpha[i];
                    }
                }
            }
            const float* source = scratch.plane(kSlotAlpha, kAlphaPlane) + offset;
            std::copy(source, source + scratch.width, alpha);
        };
    };
}

Pass
atSlot(Pass pass,
       int slot)
{
    pass.slot = slot;

    return pass;
}

#ifdef NATRON_HAVE_GPU
// Mirrors EdgeExtendParams in Kernels/edgeextend.slang, which has no padding between members.
struct EdgeExtendGpuParams {
    int32_t bufX1;
    int32_t bufY1;
    uint32_t bufWidth;
    uint32_t bufHeight;
    int32_t extX1;
    int32_t extY1;
    uint32_t extWidth;
    uint32_t extHeight;
    int32_t outX1;
    int32_t outY1;
    uint32_t outWidth;
    uint32_t outHeight;
    uint32_t dstOffset;
    uint32_t dstStride;
    uint32_t channels;
    uint32_t active;
    int32_t srcX1;
    int32_t srcY1;
    int32_t srcX2;
    int32_t srcY2;
    uint32_t srcOffset;
    uint32_t srcStride;
    uint32_t premultiply;
    uint32_t unpremultiply;
    float epsilon;
};
static_assert(sizeof(EdgeExtendGpuParams) == 100);
static_assert(offsetof(EdgeExtendGpuParams, dstOffset) == 48);
static_assert(offsetof(EdgeExtendGpuParams, srcX1) == 64);
static_assert(offsetof(EdgeExtendGpuParams, epsilon) == 96);

// The slices of edgeExtendPasses() as the GPU recorder runs them.
struct EdgeExtendGpuPlan {
    bool premultiply = true;
    bool unpremultiply = true;
    struct GpuSlice {
        int radiusX = 0;
        int radiusY = 0;
        std::vector<float> weightsX;
        std::vector<float> weightsY;
    };
    std::vector<GpuSlice> slices;
};

// A tightly packed scratch buffer of nComps floats per pixel of rect.
gpu::GpuBlurImage
scratchImage(gpu::GpuBuffer* buffer,
             const RectI& rect,
             int nComps)
{
    gpu::GpuBlurImage image;

    image.buffer = buffer;
    image.bounds = PassExec::toGpuBlurRect(rect);
    image.strideFloats = (uint32_t)(rect.width() * nComps);

    return image;
}

// The float index of pixel (x, y) of image, or false when the image exceeds the kernels' 32-bit
// indices.
bool
gpuFloatIndex(const gpu::GpuBlurImage& image,
              int x,
              int y,
              int nComps,
              uint32_t* out)
{
    const uint64_t index = image.offsetFloats + (uint64_t)(y - image.bounds.y1) * image.strideFloats + (uint64_t)(x - image.bounds.x1) * nComps;
    const uint64_t end = image.offsetFloats + (uint64_t)(image.bounds.y2 - image.bounds.y1) * image.strideFloats;
    if ((index > UINT32_MAX) || (end > UINT32_MAX)) {
        return false;
    }
    *out = (uint32_t)index;

    return true;
}

// A scratch buffer for nComps floats per pixel of rect, at least one float.
bool
scratchFor(gpu::GpuRecordContext& ctx,
           const RectI& rect,
           int nComps,
           gpu::GpuBuffer** buffer)
{
    const uint64_t floats = PassExec::isEmptyRect(rect) ? 0 : (uint64_t)rect.width() * rect.height() * nComps;
    if (floats > UINT32_MAX) {
        return false;
    }
    gpu::GpuBuffer* out = nullptr;
    if (!ctx.scratch((VkDeviceSize)std::max<uint64_t>(floats, 1) * sizeof(float), out) || !out) {
        return false;
    }
    *buffer = out;

    return true;
}

bool
dispatchEdgeExtend(gpu::GpuRecordContext& ctx,
                   gpu::GpuKernelId kernel,
                   const gpu::GpuBuffer* in,
                   const gpu::GpuBuffer* blurred,
                   const gpu::GpuBuffer* out,
                   const EdgeExtendGpuParams& p,
                   uint32_t width,
                   uint32_t height)
{
    if (!width || !height) {
        return true;
    }

    return (bool)ctx.dispatch(kernel, { in, blurred, out }, p, { width, height, 1 });
}

/// The pass list on the GPU, over the CPU's buffer, io.domain. The running result is filled there
/// with the CPU's nearest-pixel boundary. Each slice repeats the result's edges past the buffer by
/// the slice's radius, which gives the CPU's Neumann line ends to a blur that reads black past its
/// domain, blurs that back over the buffer and merges the result over it. The finish then writes
/// io.filtered over io.output, zero outside the CPU's buffer as the CPU writes it. With fewer than
/// the four channels processed every pass leaves the source as it is, as on the CPU.
PassGpuRecorder
edgeExtendGpuRecorder(const std::shared_ptr<const EdgeExtendGpuPlan>& plan)
{
    return [plan](const PassGpuIO& io) -> StatusEnum {
        gpu::GpuRecordContext& ctx = io.context;
        const int nComps = io.nComps;
        const bool active = (nComps == kNPlanes) && io.channels.all();
        const gpu::GpuBlurImage filtered = PassExec::toGpuBlurImage(io.filtered);
        const RectI& domain = io.domain;

        EdgeExtendGpuParams base {};
        base.channels = (uint32_t)nComps;
        base.active = active ? 1u : 0u;
        base.premultiply = plan->premultiply ? 1u : 0u;
        base.unpremultiply = plan->unpremultiply ? 1u : 0u;
        base.epsilon = EdgeExtend::kAlphaEpsilon;
        if (!PassExec::isEmptyRect(domain)) {
            base.bufX1 = domain.x1;
            base.bufY1 = domain.y1;
            base.bufWidth = (uint32_t)domain.width();
            base.bufHeight = (uint32_t)domain.height();
        }
        base.outX1 = io.output.x1;
        base.outY1 = io.output.y1;
        base.outWidth = (uint32_t)std::max(0, io.output.width());
        base.outHeight = (uint32_t)std::max(0, io.output.height());
        base.dstStride = filtered.strideFloats;
        if (!PassExec::isEmptyRect(io.output) && !gpuFloatIndex(filtered, io.output.x1, io.output.y1, nComps, &base.dstOffset)) {
            return eStatusFailed;
        }
        const gpu::GpuBuffer* sourceBuffer = io.source.buffer;
        const bool hasSource = sourceBuffer && !PassExec::isEmptyRect(io.sourceValid);
        if (hasSource) {
            const gpu::GpuBlurImage source = PassExec::toGpuBlurImage(io.source);
            base.srcX1 = io.sourceValid.x1;
            base.srcY1 = io.sourceValid.y1;
            base.srcX2 = io.sourceValid.x2;
            base.srcY2 = io.sourceValid.y2;
            base.srcStride = source.strideFloats;
            if (!gpuFloatIndex(source, io.sourceValid.x1, io.sourceValid.y1, nComps, &base.srcOffset)) {
                return eStatusFailed;
            }
        }

        gpu::GpuBuffer* result = nullptr;
        if (!scratchFor(ctx, domain, nComps, &result)) {
            return eStatusFailed;
        }
        if (!hasSource) {
            // The kernels still bind a source, which the empty source rect keeps unread.
            sourceBuffer = result;
        }
        if (PassExec::isEmptyRect(domain)) {
            // Every pixel of the window lies outside the empty buffer, so the finish writes zero.
            return dispatchEdgeExtend(ctx, gpu::GpuKernelId::edgeextend_finish, result, sourceBuffer, filtered.buffer, base, base.outWidth, base.outHeight) ? eStatusOK : eStatusFailed;
        }

        EdgeExtendGpuParams fill = base;
        fill.extX1 = domain.x1;
        fill.extY1 = domain.y1;
        fill.extWidth = base.bufWidth;
        fill.extHeight = base.bufHeight;
        if (!dispatchEdgeExtend(ctx, gpu::GpuKernelId::edgeextend_fill, sourceBuffer, sourceBuffer, result, fill, fill.extWidth, fill.extHeight)) {
            return eStatusFailed;
        }
        ctx.computeBarrier();

        if (active && !plan->slices.empty()) {
            int maxX = 0;
            int maxY = 0;
            for (const EdgeExtendGpuPlan::GpuSlice& slice : plan->slices) {
                maxX = std::max(maxX, slice.radiusX);
                maxY = std::max(maxY, slice.radiusY);
            }
            gpu::GpuBuffer* extended = nullptr;
            gpu::GpuBuffer* blurred = nullptr;
            if (!scratchFor(ctx, RectI(domain.x1 - maxX, domain.y1 - maxY, domain.x2 + maxX, domain.y2 + maxY), nComps, &extended) || !scratchFor(ctx, domain, nComps, &blurred)) {
                return eStatusFailed;
            }
            const gpu::GpuBlurImage blurImage = scratchImage(blurred, domain, nComps);
            for (const EdgeExtendGpuPlan::GpuSlice& slice : plan->slices) {
                const RectI extRect(domain.x1 - slice.radiusX, domain.y1 - slice.radiusY, domain.x2 + slice.radiusX, domain.y2 + slice.radiusY);
                EdgeExtendGpuParams extend = base;
                extend.extX1 = extRect.x1;
                extend.extY1 = extRect.y1;
                extend.extWidth = (uint32_t)extRect.width();
                extend.extHeight = (uint32_t)extRect.height();
                if (!dispatchEdgeExtend(ctx, gpu::GpuKernelId::edgeextend_extend, result, result, extended, extend, extend.extWidth, extend.extHeight)) {
                    return eStatusFailed;
                }
                ctx.computeBarrier();

                const gpu::GpuBlurImage extImage = scratchImage(extended, extRect, nComps);
                gpu::GpuSeparableBlur blur;
                blur.src = extImage;
                blur.srcValid = extImage.bounds;
                blur.dst = blurImage;
                blur.output = blurImage.bounds;
                blur.domain = extImage.bounds;
                blur.channels = (uint32_t)nComps;
                blur.weightsX = slice.weightsX;
                blur.weightsY = slice.weightsY;
                if (!gpu::recordSeparableBlur(ctx, blur)) {
                    return eStatusFailed;
                }
                ctx.computeBarrier();

                if (!dispatchEdgeExtend(ctx, gpu::GpuKernelId::edgeextend_over, blurred, blurred, result, base, base.bufWidth, base.bufHeight)) {
                    return eStatusFailed;
                }
                ctx.computeBarrier();
            }
        }

        return dispatchEdgeExtend(ctx, gpu::GpuKernelId::edgeextend_finish, result, sourceBuffer, filtered.buffer, base, base.outWidth, base.outHeight) ? eStatusOK : eStatusFailed;
    };
} // edgeExtendGpuRecorder
#endif // NATRON_HAVE_GPU

/// The passes of one extension at pixel sizes s. Each slice's blur reads its radius beyond what
/// it writes, so the halo is the sum of the slices' radii; the nearest-pixel boundary spreads
/// nothing, so the region of definition is the source's. Without an active slice the node is an
/// identity, so neither path premultiplies or unpremultiplies: the round trip would zero the colour
/// where the alpha is at most the epsilon, which the pass-through keeps.
PassList
edgeExtendPasses(const EdgeExtendParams& params,
                 const PixelSizes& s)
{
    const std::vector<Slice> slices = activeSlices(params, s);
    const bool neumann = true;
    const bool premultiply = params.premultiplyInput && !slices.empty();
    const bool unpremultiply = params.unpremultiplyOutput && !slices.empty();

    PassList list;
    list.boundary = PassExec::Boundary::Nearest;
    list.scratchSlots = 3;
    list.resultSlot = kSlotResult;

    list.passes.push_back(Pass::point(false, prepareWorker(premultiply)));
    for (const Slice& slice : slices) {
        const LineFilterPtr filterX = std::make_shared<const BlurKernels::LineFilter>(BlurKernels::LineFilter::firGaussian(slice.sigmaX, 0, neumann));
        const LineFilterPtr filterY = std::make_shared<const BlurKernels::LineFilter>(BlurKernels::LineFilter::firGaussian(slice.sigmaY, 0, neumann));
        // A FIR Gaussian sample costs its 2r + 1 taps, so the passes are split by taps rather than
        // by pixels, and it takes the columns a block of adjacent ones at a time.
        Pass passX = Pass::lineX(slice.radiusX, 0, lineWorker(filterX, false));
        passX.tapsPerSample = (std::size_t)(2 * slice.radiusX + 1);
        passX.sigma = slice.sigmaX;
        Pass passY = Pass::lineY(slice.radiusY, 0, lineWorker(filterY, true));
        passY.tapsPerSample = (std::size_t)(2 * slice.radiusY + 1);
        passY.columnBlock = BlurKernels::LineFilter::kColumnBlock;
        passY.sigma = slice.sigmaY;
        list.passes.push_back(Pass::point(true, copyWorker()));
        list.passes.push_back(atSlot(passX, kSlotBlur));
        list.passes.push_back(atSlot(passY, kSlotBlur));
        list.passes.push_back(Pass::point(false, overWorker()));
    }
    if (unpremultiply) {
        list.passes.push_back(Pass::point(false, unpremultiplyWorker()));
    }

#ifdef NATRON_HAVE_GPU
    std::shared_ptr<EdgeExtendGpuPlan> plan = std::make_shared<EdgeExtendGpuPlan>();
    plan->premultiply = premultiply;
    plan->unpremultiply = unpremultiply;
    bool fits = true;
    for (const Slice& slice : slices) {
        EdgeExtendGpuPlan::GpuSlice g;
        g.radiusX = slice.radiusX;
        g.radiusY = slice.radiusY;
        g.weightsX = BlurKernels::LineFilter::firGaussianWeights(slice.sigmaX);
        g.weightsY = BlurKernels::LineFilter::firGaussianWeights(slice.sigmaY);
        plan->slices.push_back(g);
        fits = fits && (slice.radiusX <= (int)gpu::kGpuBlurMaxRadius) && (slice.radiusY <= (int)gpu::kGpuBlurMaxRadius);
    }
    if (fits) {
        list.gpu = edgeExtendGpuRecorder(plan);
    }
#endif

    return list;
} // edgeExtendPasses
} // anonymous namespace

void
EdgeExtendOp::createKnobs(KnobHolder* holder,
                          const KnobPagePtr& page)
{
    KnobDoublePtr sizeKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Size"));
    sizeKnob->setName(kEdgeExtendParamSize);
    sizeKnob->setHintToolTip(EffectInstance::tr("How far the colour is extended, in pixel units (>=0): the size (diameter) of the blur the slices add up to. The standard deviation of the corresponding Gaussian is size/2.4."));
    sizeKnob->setSpatial(true);
    sizeKnob->setMinimum(0.);
    sizeKnob->setMaximum(1000.);
    sizeKnob->setDisplayMinimum(0.);
    sizeKnob->setDisplayMaximum(100.);
    sizeKnob->setIncrement(0.1);
    sizeKnob->setDecimals(1);
    sizeKnob->setDefaultValue(10.);
    page->addKnob(sizeKnob);
    size = sizeKnob;

    KnobIntPtr slicesKnob = AppManager::createKnob<KnobInt>(holder, EffectInstance::tr("Slices"));
    slicesKnob->setName(kEdgeExtendParamSlices);
    slicesKnob->setHintToolTip(EffectInstance::tr("How many blurs, of growing size, extend the colour one after the other. More slices keep the colour sharper near the original edge. 1 merges the source over a single blur of the size."));
    slicesKnob->setMinimum(1);
    slicesKnob->setMaximum(EdgeExtend::kMaxSlices);
    slicesKnob->setDisplayMinimum(1);
    slicesKnob->setDisplayMaximum(10);
    slicesKnob->setDefaultValue(5);
    page->addKnob(slicesKnob);
    slices = slicesKnob;

    KnobBoolPtr premultKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr("Premultiply Input"));
    premultKnob->setName(kEdgeExtendParamPremultiplyInput);
    premultKnob->setHintToolTip(EffectInstance::tr("Multiply the input's colour by its alpha before extending it. Turn it off for an input that is already premultiplied."));
    premultKnob->setDefaultValue(true);
    page->addKnob(premultKnob);
    premultiplyInput = premultKnob;

    KnobBoolPtr unpremultKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr("Unpremultiply Output"));
    unpremultKnob->setName(kEdgeExtendParamUnpremultiplyOutput);
    unpremultKnob->setHintToolTip(EffectInstance::tr("Divide the extended colour by the extended alpha and output the input's alpha: the extended colour under the original matte. Off, the output is premultiplied, with the extended alpha."));
    unpremultKnob->setDefaultValue(true);
    page->addKnob(unpremultKnob);
    unpremultiplyOutput = unpremultKnob;
} // EdgeExtendOp::createKnobs

EdgeExtendOp::Params
EdgeExtendOp::bake(const KnobReader& reader) const
{
    Params params;

    params.sizeX = params.sizeY = std::max(0., (double)reader.value(size));
    params.slices = std::min(EdgeExtend::kMaxSlices, std::max(1, (int)reader.value(slices)));
    params.premultiplyInput = reader.value(premultiplyInput);
    params.unpremultiplyOutput = reader.value(unpremultiplyOutput);

    return params;
}

bool
EdgeExtendOp::isIdentity(const Params& params,
                         const PassContext& context) const
{
    return activeSlices(params, pixelSizes(params, context)).empty();
}

PassList
EdgeExtendOp::passes(const Params& params,
                     const PassContext& context) const
{
    return edgeExtendPasses(params, pixelSizes(params, context));
}

EdgeExtend::EdgeExtend(NodePtr node)
    : ImageNode<EdgeExtendOp>(node)
{
}

EdgeExtend::~EdgeExtend()
{
}

RectI
EdgeExtend::getSourceRoI(const RectI& rect,
                         const RenderScale& scale,
                         const EdgeExtendParams& params)
{
    PassContext context;

    context.scale = scale;

    return passListSourceRoI(rect, edgeExtendPasses(params, pixelSizes(params, context)));
}

std::vector<double>
EdgeExtend::sliceSigmas(double size,
                        int slices)
{
    const int n = std::max(1, slices);
    const double base = size / (2.4 * std::sqrt(n * (n + 1) * (n + 0.5) / 3.));
    std::vector<double> sigmas(n);

    for (int i = 0; i < n; ++i) {
        sigmas[i] = base * (i + 1);
    }

    return sigmas;
}

NATRON_NAMESPACE_EXIT
