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

#include "Median.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <list>
#include <memory>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
const int kSlotSource = 0;
const int kSlotResult = 1;

/// The radius the direct kernel sorts in registers; larger ones go through the tiled kernel.
const int kGpuDirectMaxRadius = 2;

// An unsigned key in the order of a float that is not NaN. Selecting on keys rather than floats
// keeps denormals ordered where a thread flushes them, as the GPU kernels select.
inline uint32_t
orderKey(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, sizeof(u));

    return (u & 0x80000000u) ? ~u : (u | 0x80000000u);
}

inline float
keyValue(uint32_t k)
{
    const uint32_t u = (k & 0x80000000u) ? (k & 0x7fffffffu) : ~k;
    float f;
    std::memcpy(&f, &u, sizeof(f));

    return f;
}

inline uint32_t
floatBits(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, sizeof(u));

    return u;
}

std::function<PassRowFn(const PassScratch&)>
medianWorker(int radius,
             float threshold)
{
    return [radius, threshold](const PassScratch& scratch) -> PassRowFn {
        const int width = scratch.width;
        const int height = scratch.height;
        const std::size_t planeSize = scratch.planeSize;
        const float* const source = scratch.buffers[kSlotSource];
        float* const result = scratch.buffers[kSlotResult];
        const int side = 2 * radius + 1;
        const std::size_t count = (std::size_t)side * (std::size_t)side;
        std::shared_ptr<std::vector<uint32_t>> window = std::make_shared<std::vector<uint32_t>>(count);

        return [radius, threshold, width, height, planeSize, source, result, count, window](int line) {
            const int p = line / height;
            const int row = line % height;
            const float* in = source + (std::size_t)p * planeSize;
            float* out = result + (std::size_t)p * planeSize + (std::size_t)row * (std::size_t)width;
            uint32_t* values = &(*window)[0];
            uint32_t* const mid = values + count / 2;

            for (int x = 0; x < width; ++x) {
                std::size_t k = 0;
                for (int dy = -radius; dy <= radius; ++dy) {
                    const int y = std::min(std::max(row + dy, 0), height - 1);
                    const float* srcRow = in + (std::size_t)y * (std::size_t)width;
                    for (int dx = -radius; dx <= radius; ++dx) {
                        const int xx = std::min(std::max(x + dx, 0), width - 1);
                        values[k++] = orderKey(Median::windowSample(srcRow[xx]));
                    }
                }
                std::nth_element(values, mid, values + count);
                out[x] = Median::applyThreshold(in[(std::size_t)row * (std::size_t)width + (std::size_t)x], keyValue(*mid), threshold);
            }
        };
    };
}

#ifdef NATRON_HAVE_GPU
/// Mirrored by MedianParams in Engine/Gpu/Kernels/median.slang.
struct MedianGpuParams {
    // The source pixels that exist, the first at srcOffset floats.
    int32_t srcX1;
    int32_t srcY1;
    int32_t srcX2;
    int32_t srcY2;
    uint32_t srcOffset;
    uint32_t srcStride;
    // The CPU's buffer: the window grown by the radius, clipped to the region of definition.
    int32_t domX1;
    int32_t domY1;
    int32_t domX2;
    int32_t domY2;
    // The rect written, its first pixel at dstOffset floats.
    int32_t outX1;
    int32_t outY1;
    int32_t outX2;
    int32_t outY2;
    uint32_t dstOffset;
    uint32_t dstStride;
    uint32_t channels;
    int32_t radius;
    float threshold;
    uint32_t padding0;
};

static_assert(sizeof(MedianGpuParams) == 80);
static_assert(offsetof(MedianGpuParams, domX1) == 24);
static_assert(offsetof(MedianGpuParams, outX1) == 40);
static_assert(offsetof(MedianGpuParams, channels) == 64);

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

/// One dispatch over io.output that reads the source straight, as the CPU's buffer over io.domain
/// holds it, and writes zero outside the domain as the CPU's window does.
PassGpuRecorder
medianGpuRecorder(int radius,
                  float threshold)
{
    return [radius, threshold](const PassGpuIO& io) -> StatusEnum {
        gpu::GpuRecordContext& ctx = io.context;
        const RectI& output = io.output;
        if (PassExec::isEmptyRect(output)) {
            return eStatusOK;
        }
        MedianGpuParams p = {};
        p.channels = (uint32_t)io.nComps;
        p.radius = radius;
        p.threshold = threshold;
        p.domX1 = io.domain.x1;
        p.domY1 = io.domain.y1;
        p.domX2 = io.domain.x2;
        p.domY2 = io.domain.y2;
        if (PassExec::isEmptyRect(io.domain)) {
            p.domX2 = p.domX1;
            p.domY2 = p.domY1;
        }

        const gpu::GpuBuffer* sourceBuffer = nullptr;
        if (io.source.buffer && !PassExec::isEmptyRect(io.sourceValid)) {
            const gpu::GpuBlurImage source = PassExec::toGpuBlurImage(io.source);
            sourceBuffer = io.source.buffer;
            p.srcX1 = io.sourceValid.x1;
            p.srcY1 = io.sourceValid.y1;
            p.srcX2 = io.sourceValid.x2;
            p.srcY2 = io.sourceValid.y2;
            p.srcStride = source.strideFloats;
            if (!gpuFloatIndex(source, io.sourceValid.x1, io.sourceValid.y1, io.nComps, &p.srcOffset)) {
                return eStatusFailed;
            }
        } else {
            gpu::GpuBuffer* placeholder = nullptr;
            if (!ctx.scratch(4 * sizeof(float), placeholder) || !placeholder) {
                return eStatusFailed;
            }
            sourceBuffer = placeholder;
        }

        const gpu::GpuBlurImage filtered = PassExec::toGpuBlurImage(io.filtered);
        p.outX1 = output.x1;
        p.outY1 = output.y1;
        p.outX2 = output.x2;
        p.outY2 = output.y2;
        p.dstStride = filtered.strideFloats;
        if (!gpuFloatIndex(filtered, output.x1, output.y1, io.nComps, &p.dstOffset)) {
            return eStatusFailed;
        }
        const gpu::GpuKernelId kernel = (radius <= kGpuDirectMaxRadius) ? gpu::GpuKernelId::median_direct : gpu::GpuKernelId::median_tiled;

        return ctx.dispatch(kernel, { sourceBuffer, filtered.buffer }, p, { (uint32_t)output.width(), (uint32_t)output.height(), 1 }) ? eStatusOK : eStatusFailed;
    };
}
#endif // NATRON_HAVE_GPU

PassList
medianPasses(const MedianParams& params,
             const RenderScale& scale)
{
    PassList list;
    const int radius = Median::pixelRadius(scale, params);
    if (radius <= 0) {
        return list;
    }
    const float threshold = (float)std::max(0., params.threshold);
    const int side = 2 * radius + 1;

    list.boundary = PassExec::Boundary::Zero;
    list.scratchSlots = 2;
    list.resultSlot = kSlotResult;
    Pass pass = Pass::gather(radius, radius, true, medianWorker(radius, threshold));
    pass.growX = 0;
    pass.growY = 0;
    pass.tapsPerSample = (std::size_t)side * (std::size_t)side;
    list.passes.push_back(pass);
#ifdef NATRON_HAVE_GPU
    if (radius <= Median::kMedianGpuMaxRadius) {
        list.gpu = medianGpuRecorder(radius, threshold);
    }
#endif

    return list;
}
} // anonymous namespace

void
MedianOp::createKnobs(KnobHolder* holder,
                      const KnobPagePtr& page)
{
    KnobIntPtr sizeKnob = AppManager::createKnob<KnobInt>(holder, EffectInstance::tr("Size"));
    sizeKnob->setName(kMedianParamSize);
    sizeKnob->setHintToolTip(EffectInstance::tr("Radius of the window: its width and height are 2*size+1, in pixel units (>=0). 0 leaves the input unchanged."));
    sizeKnob->setMinimum(0);
    sizeKnob->setMaximum(100);
    sizeKnob->setDisplayMinimum(0);
    sizeKnob->setDisplayMaximum(10);
    sizeKnob->setDefaultValue(1);
    page->addKnob(sizeKnob);
    size = sizeKnob;

    KnobDoublePtr thresholdKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Threshold"));
    thresholdKnob->setName(kMedianParamThreshold);
    thresholdKnob->setHintToolTip(EffectInstance::tr("A pixel is replaced by the median only where it differs from it by more than threshold times the median. Relative, so a dark and a bright plate are treated alike. 0 replaces every pixel."));
    thresholdKnob->setMinimum(0.);
    thresholdKnob->setDisplayMinimum(0.);
    thresholdKnob->setDisplayMaximum(1.);
    thresholdKnob->setDefaultValue(0.);
    page->addKnob(thresholdKnob);
    threshold = thresholdKnob;
}

MedianOp::Params
MedianOp::bake(const KnobReader& reader) const
{
    Params params;

    params.size = std::max(0, reader.value(size));
    params.threshold = std::max(0., (double)reader.value(threshold));

    return params;
}

bool
MedianOp::isIdentity(const Params& params,
                     const PassContext& context) const
{
    return Median::pixelRadius(context.scale, params) <= 0;
}

PassList
MedianOp::passes(const Params& params,
                 const PassContext& context) const
{
    return medianPasses(params, context.scale);
}

std::optional<GpuCostHint>
MedianOp::costHint(const Params& params,
                   const PassContext& context) const
{
    GpuCostHint hint;

    hint.size = Median::pixelRadius(context.scale, params);

    return hint;
}

Median::Median(NodePtr node)
    : ImageNode<MedianOp>(node)
{
}

Median::~Median()
{
}

void
Median::addAcceptedComponents(int inputNb,
                              std::list<ImageLayerDesc>* comps)
{
    ImageNode<MedianOp>::addAcceptedComponents(inputNb, comps);
    if (!isInputMask(inputNb)) {
        comps->push_back(ImageLayerDesc::getXYComponents());
    }
}

void
Median::getParams(double time,
                  ViewIdx view,
                  MedianParams* params) const
{
    *params = bakeParams(op(), time, view);
}

int
Median::pixelRadius(const RenderScale& scale,
                    const MedianParams& params)
{
    return std::max(0, (int)std::floor(params.size * scale.toOfxPointD().x));
}

RectI
Median::getSourceRoI(const RectI& rect,
                     const RenderScale& scale,
                     const MedianParams& params)
{
    return passListSourceRoI(rect, medianPasses(params, scale));
}

float
Median::windowSample(float v)
{
    const uint32_t magnitude = floatBits(v) & 0x7fffffffu;

    return ((magnitude == 0u) || (magnitude > 0x7f800000u)) ? 0.f : v;
}

float
Median::applyThreshold(float in,
                       float m,
                       float threshold)
{
    if ((threshold <= 0.f) || ((floatBits(in) & 0x7fffffffu) > 0x7f800000u)) {
        return m;
    }
    const float limit = threshold * std::max(std::fabs(m), kRelativeEpsilon);

    return (std::fabs(in - m) <= limit) ? in : m;
}

NATRON_NAMESPACE_EXIT
