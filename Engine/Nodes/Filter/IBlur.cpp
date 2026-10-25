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

#include "IBlur.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>

#include <QString>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/VariableBlurKernels.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/Project.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuBlur.h"
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
constexpr InputSpec kIBlurInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
    kMapInputSpec,
};

constexpr NodeSpec kIBlurNodeSpec = {
    .id = PLUGINID_NATRON_IBLUR,
    .label = "IBlur",
    .description = "Blur the input stream by a size that varies per pixel with the Map input.\n"
                   "\n"
                   "Where the Map is white the blur has the Size, where it is black the Size At Black (0 by default, which leaves those pixels exactly unchanged), "
                   "and in between a size in between. Map Channel picks the Map's channel in any of its layers, Invert Map inverts it, and Map Black and Map White "
                   "set the Map values that give Size At Black and Size. Outside the Map image, the Map reads black. With no Map connected, the blur has the Size everywhere.\n"
                   "\n"
                   "The blur is a box of the pixel's size applied three times, an approximation of the Gaussian of standard deviation size/2.4 that Blur uses "
                   "(within about 2% at a constant size), whose cost does not depend on the size. It is black outside the image, and the Mask and Mix limit the result as usual.",
    .grouping = PLUGIN_GROUP_FILTER,
    .majorVersion = PLUGIN_MAJOR_NATRON_IBLUR,
    .icon = NATRON_IMAGES_PATH "NativeNodes/IBlur.png",
    .inputs = kIBlurInputs,
};

constexpr ImagePipelineSpec kIBlurPipeline = {
    .stages = { Stage::MaskMix, Stage::PassThroughUnprocessed },
    .layers = LayerPolicy::all(),
    .op = OpKind::Passes,
};

const int kBoxIterations = 3;

/// The box radii of a Map value, in pixels at the context's scale: the size at that value,
/// horizontally divided by the pixel aspect ratio as Blur does, as the standard deviation of
/// the Gaussian the three boxes approximate.
struct BoxRadii {
    double x;
    double y;
};

BoxRadii
radiiAt(const IBlurParams& params,
        const PassContext& context,
        float map)
{
    const OfxPointD rs = context.scale.toOfxPointD();
    const double sizeX = std::max(0., rs.x * params.map.sizeAt(map, 0, params.sizeX) / context.pixelAspectRatio);
    const double sizeY = std::max(0., rs.y * params.map.sizeAt(map, 1, params.sizeY));
    BoxRadii radii;

    radii.x = VariableBlurKernels::boxRadius(sizeX / 2.4);
    radii.y = VariableBlurKernels::boxRadius(sizeY / 2.4);

    return radii;
}

/**
 * @brief What the passes of one render share: the Map and the knobs, the per-pixel radii over
 * the scratch rect, and a summed-area table per plane. The first pass sizes the storage for the
 * plane job it runs in, so later passes of the job only read it; only a job with more planes
 * than any before reallocates it, and never while a pass of another job runs. The storage is
 * written whole before it is read, so it starts uninitialised.
 **/
struct VariableBoxState {
    std::shared_ptr<const MapPlane> map;
    IBlurParams params;
    PassContext context;

    std::mutex mutex;
    std::unique_ptr<double[]> sat;
    std::size_t satCapacity = 0;
    std::unique_ptr<float[]> rx;
    std::unique_ptr<float[]> ry;
    std::size_t radiiCapacity = 0;

    std::size_t satPlaneSize(const PassScratch& scratch) const
    {
        return ((std::size_t)scratch.width + 1) * ((std::size_t)scratch.height + 1);
    }

    void reserve(const PassScratch& scratch)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        const std::size_t satSize = satPlaneSize(scratch) * (std::size_t)scratch.nPlanes;
        if (satCapacity < satSize) {
            sat.reset(new double[satSize]);
            satCapacity = satSize;
        }
        const std::size_t radiiSize = (std::size_t)scratch.width * scratch.height;
        if (radiiCapacity < radiiSize) {
            rx.reset(new float[radiiSize]);
            ry.reset(new float[radiiSize]);
            radiiCapacity = radiiSize;
        }
    }
};

using VariableBoxStatePtr = std::shared_ptr<VariableBoxState>;

/// Row y of the radii, from the Map at the matching pixels; a run of equal Map values reuses
/// the radii of its first.
std::function<PassRowFn(const PassScratch&)>
radiusWorker(const VariableBoxStatePtr& state)
{
    return [state](const PassScratch& scratch) -> PassRowFn {
        state->reserve(scratch);
        const VariableBoxState* s = state.get();
        float* const rx = s->rx.get();
        float* const ry = s->ry.get();
        const int width = scratch.width;
        const RectI rect = scratch.rect;

        return [s, rx, ry, width, rect](int y) {
            float* const rowX = rx + (std::size_t)y * width;
            float* const rowY = ry + (std::size_t)y * width;
            float last = 0.f;
            BoxRadii radii = radiiAt(s->params, s->context, last);
            for (int x = 0; x < width; ++x) {
                const float m = s->map->at(rect.x1 + x, rect.y1 + y);
                if (m != last) {
                    last = m;
                    radii = radiiAt(s->params, s->context, m);
                }
                rowX[x] = (float)radii.x;
                rowY[x] = (float)radii.y;
            }
        };
    };
}

/// The row sums of slot `slot` into the table, row 0 zeroed along with the first image row.
std::function<PassRowFn(const PassScratch&)>
rowSumWorker(const VariableBoxStatePtr& state,
             int slot)
{
    return [state, slot](const PassScratch& scratch) -> PassRowFn {
        state->reserve(scratch);
        double* const sat = state->sat.get();
        const std::size_t satPlane = state->satPlaneSize(scratch);
        const std::size_t stride = (std::size_t)scratch.width + 1;
        const int width = scratch.width;
        const int height = scratch.height;
        const PassScratch* source = &scratch;

        return [sat, satPlane, stride, width, height, source, slot](int line) {
            const int p = line / height;
            const int y = line % height;
            double* const table = sat + (std::size_t)p * satPlane;
            if (y == 0) {
                std::fill(table, table + stride, 0.);
            }
            VariableBlurKernels::prefixRow(source->plane(slot, p) + (std::size_t)y * width, width, table + (std::size_t)(y + 1) * stride);
        };
    };
}

/// The column sums that complete the table, one column of one plane per line.
std::function<PassRowFn(const PassScratch&)>
columnSumWorker(const VariableBoxStatePtr& state)
{
    return [state](const PassScratch& scratch) -> PassRowFn {
        state->reserve(scratch);
        double* const sat = state->sat.get();
        const std::size_t satPlane = state->satPlaneSize(scratch);
        const std::size_t stride = (std::size_t)scratch.width + 1;
        const int width = scratch.width;
        const int height = scratch.height;

        return [sat, satPlane, stride, width, height](int line) {
            const int p = line / width;
            const int column = line % width;
            VariableBlurKernels::accumulateColumn(sat + (std::size_t)p * satPlane, stride, height, column + 1);
        };
    };
}

/// One box pass, from slot `from` (for the pixels of radius 0) and the table into slot `to`.
std::function<PassRowFn(const PassScratch&)>
boxWorker(const VariableBoxStatePtr& state,
          int from,
          int to)
{
    return [state, from, to](const PassScratch& scratch) -> PassRowFn {
        state->reserve(scratch);
        const double* const sat = state->sat.get();
        const float* const rx = state->rx.get();
        const float* const ry = state->ry.get();
        const std::size_t satPlane = state->satPlaneSize(scratch);
        const int width = scratch.width;
        const int height = scratch.height;
        const PassScratch* buffers = &scratch;

        return [sat, rx, ry, satPlane, width, height, buffers, from, to](int line) {
            const int p = line / height;
            const int y = line % height;
            const std::size_t row = (std::size_t)y * width;
            VariableBlurKernels::boxRow(sat + (std::size_t)p * satPlane, width, height, y, rx + row, ry + row, buffers->plane(from, p) + row, buffers->plane(to, p) + row);
        };
    };
}
#ifdef NATRON_HAVE_GPU
// The largest box radius, in pixels, the GPU records. The table covers the whole domain at once
// and its rows and columns are summed one thread each, so a larger blur stays on the CPU.
const int kGpuMaxBoxHalo = 512;

// Mirror kFinal and kMap* in Kernels/iblur.slang.
const uint32_t kGpuFinal = 1;
const uint32_t kGpuMapConnected = 1;
const uint32_t kGpuMapRead = 2;
const uint32_t kGpuMapInvert = 4;

// Mirrors IBlurParams in Kernels/iblur.slang, which has no padding between members.
struct IBlurGpuParams {
    int32_t domX1;
    int32_t domY1;
    uint32_t domWidth;
    uint32_t domHeight;
    uint32_t channels;
    uint32_t comp;
    uint32_t fromOffset;
    uint32_t toOffset;
    uint32_t radiiOffset;
    uint32_t valueBits;
    uint32_t flags;
    uint32_t satOffset;
    int32_t srcX1;
    int32_t srcY1;
    int32_t srcX2;
    int32_t srcY2;
    uint32_t srcOffset;
    uint32_t srcStride;
    int32_t outX1;
    int32_t outY1;
    uint32_t outWidth;
    uint32_t outHeight;
    uint32_t dstOffset;
    uint32_t dstStride;
};
static_assert(sizeof(IBlurGpuParams) == 96);
static_assert(offsetof(IBlurGpuParams, srcX1) == 48);
static_assert(offsetof(IBlurGpuParams, outX1) == 72);

// Mirrors MapConstants in Kernels/iblur.slang, which has no padding between members.
struct IBlurGpuConstants {
    uint32_t mapFlags;
    uint32_t mapChannel;
    uint32_t mapChannels;
    uint32_t mapOffset;
    uint32_t mapStride;
    int32_t mapX1;
    int32_t mapY1;
    int32_t mapX2;
    int32_t mapY2;
    float black;
    float white;
    float sizeAtBlackX;
    float sizeAtBlackY;
    float sizeX;
    float sizeY;
    float sigmaScaleX;
    float sigmaScaleY;
    float blackRx;
    float blackRy;
    float whiteRx;
    float whiteRy;
    float outsideRx;
    float outsideRy;
    uint32_t pad;
};
static_assert(sizeof(IBlurGpuConstants) == 96);
static_assert(offsetof(IBlurGpuConstants, black) == 36);
static_assert(offsetof(IBlurGpuConstants, blackRx) == 68);

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

// Every kernel of Kernels/iblur.slang binds these, in this order.
struct IBlurGpuBuffers {
    const gpu::GpuBuffer* source = nullptr;
    const gpu::GpuBuffer* map = nullptr;
    const gpu::GpuBuffer* constants = nullptr;
    const gpu::GpuBuffer* work = nullptr;
    const gpu::GpuBuffer* sat = nullptr;
    const gpu::GpuBuffer* out = nullptr;
};

bool
dispatchIBlur(gpu::GpuRecordContext& ctx,
              gpu::GpuKernelId kernel,
              const IBlurGpuBuffers& b,
              const IBlurGpuParams& p,
              uint32_t width,
              uint32_t height)
{
    if (!width || !height) {
        return true;
    }

    return (bool)ctx.dispatch(kernel, { b.source, b.map, b.constants, b.work, b.sat, b.out }, p, { width, height, 1 });
}

/**
 * @brief The passes of IBlur::passes() as the GPU records them over io.domain, the CPU's scratch
 * rect: the source into slot 0 and the radii from the Map, then per box and per component the
 * table's row maxima, its quantisation step, its rows and columns, and the box into the other
 * slot, the last box into io.filtered. The Map's channel is mapChannel of io.inputs[Map], or none
 * when that is negative. Fails, so that the CPU renders, when a buffer exceeds the kernels'
 * 32-bit indices.
 **/
StatusEnum
recordIBlurGpu(const PassGpuIO& io,
               const IBlurParams& params,
               const PassContext& context,
               bool mapConnected,
               int mapChannel)
{
    gpu::GpuRecordContext& ctx = io.context;
    const RectI& domain = io.domain;
    const int nComps = io.nComps;
    if (PassExec::isEmptyRect(domain) || (nComps < 1) || (nComps > 4) || !io.filtered.buffer) {
        return eStatusFailed;
    }
    const uint64_t width = (uint64_t)domain.width();
    const uint64_t height = (uint64_t)domain.height();
    const uint64_t pixels = width * height;
    const uint64_t slotFloats = pixels * (uint64_t)nComps;
    const uint64_t workFloats = 2 * slotFloats + 2 * pixels;
    // The row maxima and the step's exponent come first.
    const uint64_t satOffset = height + 1;
    const uint64_t satWords = satOffset + 2 * (width + 1) * (height + 1);
    if ((workFloats > UINT32_MAX) || (satWords > UINT32_MAX)) {
        return eStatusFailed;
    }
    // Room in 63 bits for the sum of every pixel, each below 2^valueBits.
    int pixelBits = 0;
    while ((pixelBits < 62) && ((1ull << pixelBits) < pixels)) {
        ++pixelBits;
    }

    IBlurGpuConstants k {};
    const OfxPointD rs = context.scale.toOfxPointD();
    const BoxRadii white = radiiAt(params, context, 1.f);
    const BoxRadii black = radiiAt(params, context, 0.f);
    const BoxRadii outside = radiiAt(params, context, params.map.remap(0.f));
    k.mapFlags = (mapConnected ? kGpuMapConnected : 0u) | (params.map.invert ? kGpuMapInvert : 0u);
    k.black = (float)params.map.black;
    k.white = (float)params.map.white;
    k.sizeAtBlackX = (float)params.map.sizeAtBlack[0];
    k.sizeAtBlackY = (float)params.map.sizeAtBlack[1];
    k.sizeX = (float)params.sizeX;
    k.sizeY = (float)params.sizeY;
    k.sigmaScaleX = (float)(rs.x / context.pixelAspectRatio / 2.4);
    k.sigmaScaleY = (float)(rs.y / 2.4);
    k.blackRx = (float)black.x;
    k.blackRy = (float)black.y;
    k.whiteRx = (float)white.x;
    k.whiteRy = (float)white.y;
    k.outsideRx = (float)outside.x;
    k.outsideRy = (float)outside.y;

    gpu::GpuBuffer* constants = nullptr;
    gpu::GpuBuffer* work = nullptr;
    gpu::GpuBuffer* sat = nullptr;
    if (!ctx.scratch(sizeof(k), constants) || !constants || !ctx.scratch(workFloats * sizeof(float), work) || !work
        || !ctx.scratch(satWords * sizeof(uint32_t), sat) || !sat) {
        return eStatusFailed;
    }

    IBlurGpuBuffers buffers;
    // Unread placeholders for a missing source or Map, which the kernels leave alone.
    buffers.source = constants;
    buffers.map = constants;
    buffers.constants = constants;
    buffers.work = work;
    buffers.sat = sat;
    buffers.out = io.filtered.buffer;

    const GpuImageView map = (kIBlurInputMap < (int)io.inputs.size()) ? io.inputs[kIBlurInputMap] : GpuImageView();
    if (mapConnected && map.buffer && (mapChannel >= 0) && (mapChannel < map.nComps) && !PassExec::isEmptyRect(map.bounds)) {
        const gpu::GpuBlurImage image = PassExec::toGpuBlurImage(map);
        if (!gpuFloatIndex(image, map.bounds.x1, map.bounds.y1, map.nComps, &k.mapOffset)) {
            return eStatusFailed;
        }
        k.mapFlags |= kGpuMapRead;
        k.mapChannel = (uint32_t)mapChannel;
        k.mapChannels = (uint32_t)map.nComps;
        k.mapStride = image.strideFloats;
        k.mapX1 = map.bounds.x1;
        k.mapY1 = map.bounds.y1;
        k.mapX2 = map.bounds.x2;
        k.mapY2 = map.bounds.y2;
        buffers.map = map.buffer;
    }
    if (!ctx.updateBuffer(*constants, 0, std::as_bytes(std::span<const IBlurGpuConstants>(&k, 1)))) {
        return eStatusFailed;
    }

    IBlurGpuParams base {};
    base.domX1 = domain.x1;
    base.domY1 = domain.y1;
    base.domWidth = (uint32_t)width;
    base.domHeight = (uint32_t)height;
    base.channels = (uint32_t)nComps;
    base.radiiOffset = (uint32_t)(2 * slotFloats);
    base.valueBits = (uint32_t)(62 - pixelBits);
    base.satOffset = (uint32_t)satOffset;
    const gpu::GpuBlurImage filtered = PassExec::toGpuBlurImage(io.filtered);
    base.outX1 = io.output.x1;
    base.outY1 = io.output.y1;
    base.outWidth = (uint32_t)std::max(0, io.output.width());
    base.outHeight = (uint32_t)std::max(0, io.output.height());
    base.dstStride = filtered.strideFloats;
    if (!PassExec::isEmptyRect(io.output) && !gpuFloatIndex(filtered, io.output.x1, io.output.y1, nComps, &base.dstOffset)) {
        return eStatusFailed;
    }

    IBlurGpuParams load = base;
    if (io.source.buffer && !PassExec::isEmptyRect(io.sourceValid)) {
        const gpu::GpuBlurImage source = PassExec::toGpuBlurImage(io.source);
        load.srcX1 = io.sourceValid.x1;
        load.srcY1 = io.sourceValid.y1;
        load.srcX2 = io.sourceValid.x2;
        load.srcY2 = io.sourceValid.y2;
        load.srcStride = source.strideFloats;
        if (!gpuFloatIndex(source, io.sourceValid.x1, io.sourceValid.y1, nComps, &load.srcOffset)) {
            return eStatusFailed;
        }
        buffers.source = io.source.buffer;
    }
    // Both write their own part of the work buffer.
    if (!dispatchIBlur(ctx, gpu::GpuKernelId::iblur_load, buffers, load, base.domWidth, base.domHeight)
        || !dispatchIBlur(ctx, gpu::GpuKernelId::iblur_radii, buffers, base, base.domWidth, base.domHeight)) {
        return eStatusFailed;
    }
    ctx.computeBarrier();

    const uint32_t slots[2] = { 0u, (uint32_t)slotFloats };
    for (int iteration = 0; iteration < kBoxIterations; ++iteration) {
        const bool last = (iteration == kBoxIterations - 1);
        for (int c = 0; c < nComps; ++c) {
            IBlurGpuParams p = base;
            p.comp = (uint32_t)c;
            p.fromOffset = slots[iteration % 2];
            p.toOffset = slots[1 - iteration % 2];
            p.flags = last ? kGpuFinal : 0u;
            if (!dispatchIBlur(ctx, gpu::GpuKernelId::iblur_rowmax, buffers, p, base.domHeight, 1)) {
                return eStatusFailed;
            }
            ctx.computeBarrier();
            if (!dispatchIBlur(ctx, gpu::GpuKernelId::iblur_shift, buffers, p, 1, 1)) {
                return eStatusFailed;
            }
            ctx.computeBarrier();
            if (!dispatchIBlur(ctx, gpu::GpuKernelId::iblur_rows, buffers, p, base.domHeight, 1)) {
                return eStatusFailed;
            }
            ctx.computeBarrier();
            if (!dispatchIBlur(ctx, gpu::GpuKernelId::iblur_columns, buffers, p, base.domWidth, 1)) {
                return eStatusFailed;
            }
            ctx.computeBarrier();
            if (!dispatchIBlur(ctx, gpu::GpuKernelId::iblur_box, buffers, p, last ? base.outWidth : base.domWidth, last ? base.outHeight : base.domHeight)) {
                return eStatusFailed;
            }
            // The next component reuses the table; the next box reads this one's slot.
            if (!last || (c < nComps - 1)) {
                ctx.computeBarrier();
            }
        }
    }

    return eStatusOK;
} // recordIBlurGpu
#endif // NATRON_HAVE_GPU
} // anonymous namespace

IBlur::IBlur(NodePtr node)
    : StagedImageEffect(node)
    , _map()
    , _size()
    , _cropToFormat()
{
}

IBlur::~IBlur()
{
}

const NodeSpec*
IBlur::nodeSpec() const
{
    return staticNodeSpec<kIBlurNodeSpec>();
}

const ImagePipelineSpec&
IBlur::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kIBlurPipeline, kIBlurNodeSpec>();
}

std::string
IBlur::getInputHint(int inputNb) const
{
    switch (inputNb) {
    case kIBlurInputSource:
        return tr("The image to blur.").toStdString();
    case kIBlurInputMask:
        return tr("The mask the blurred result is mixed with the source through.").toStdString();
    case kIBlurInputMap:
        return tr("The Map: the blur has the Size where it is white and the Size At Black where it is black.").toStdString();
    }

    return std::string();
}

void
IBlur::addAcceptedComponents(int inputNb,
                             std::list<ImageLayerDesc>* comps)
{
    StagedImageEffect::addAcceptedComponents(inputNb, comps);
    if (!isInputMask(inputNb)) {
        comps->push_back(ImageLayerDesc::getXYComponents());
    }
}

void
IBlur::initializeKnobs()
{
    const KnobPagePtr page = createControlsPage();

    KnobDoublePtr sizeKnob = AppManager::createKnob<KnobDouble>(this, tr("Size"), 2);
    sizeKnob->setName(kIBlurParamSize);
    sizeKnob->setHintToolTip(tr("Size (diameter) of the blur where the Map is white, in pixel units (>=0). The standard deviation of the Gaussian the blur approximates is size/2.4."));
    sizeKnob->setSpatial(true);
    sizeKnob->setCanAutoFoldDimensions(true);
    for (int d = 0; d < 2; ++d) {
        sizeKnob->setMinimum(0., d);
        sizeKnob->setMaximum(1000., d);
        sizeKnob->setDisplayMinimum(0., d);
        sizeKnob->setDisplayMaximum(100., d);
        sizeKnob->setIncrement(0.1, d);
        sizeKnob->setDecimals(1, d);
        sizeKnob->setDefaultValue(0., d);
    }
    page->addKnob(sizeKnob);
    _size = sizeKnob;

    _map.createKnobs(this, page, 2);

    KnobBoolPtr cropToFormatKnob = AppManager::createKnob<KnobBool>(this, tr("Crop To Format"));
    cropToFormatKnob->setName(kIBlurParamCropToFormat);
    cropToFormatKnob->setHintToolTip(tr("If the source is inside the format and the effect extends it outside of the format, crop it to avoid unnecessary calculations. To avoid unwanted crops, only the borders that were inside of the format in the source clip will be cropped."));
    cropToFormatKnob->setDefaultValue(true);
    page->addKnob(cropToFormatKnob);
    _cropToFormat = cropToFormatKnob;

    addStageKnobs(page);

    _map.declareLayerKnob(getNode(), kIBlurInputMap);
} // IBlur::initializeKnobs

IBlurParams
IBlur::getParams(double time,
                 ViewIdx view) const
{
    const KnobReader reader(time, view);
    IBlurParams params;

    params.sizeX = reader.value(_size, 0);
    params.sizeY = reader.value(_size, 1);
    params.cropToFormat = reader.value(_cropToFormat);
    params.map = _map.bake(reader);

    return params;
}

PassList
IBlur::passes(const IBlurParams& params,
              const PassContext& context,
              const MapRange& range,
              const std::shared_ptr<const MapPlane>& map)
{
    PassList list;
    // The size is linear in the Map value, so the largest radius over the range is at one end.
    const BoxRadii low = radiiAt(params, context, range.min);
    const BoxRadii high = radiiAt(params, context, range.max);
    const double rx = std::max(low.x, high.x);
    const double ry = std::max(low.y, high.y);

    if ((rx <= 0.) && (ry <= 0.)) {
        return list;
    }
    const int haloX = (int)std::ceil(rx);
    const int haloY = (int)std::ceil(ry);

    VariableBoxStatePtr state;
    if (map) {
        state = std::make_shared<VariableBoxState>();
        state->map = map;
        state->params = params;
        state->context = context;
    }

    list.scratchSlots = 2;
    list.resultSlot = (kBoxIterations % 2) ? 1 : 0;

    Pass radii = Pass::point(false, state ? radiusWorker(state) : std::function<PassRowFn(const PassScratch&)>());
    list.passes.push_back(radii);
    for (int k = 0; k < kBoxIterations; ++k) {
        const int from = k % 2;
        const int to = 1 - from;

        Pass rows = Pass::point(true, state ? rowSumWorker(state, from) : std::function<PassRowFn(const PassScratch&)>());
        rows.slot = from;
        list.passes.push_back(rows);

        Pass columns = Pass::point(true, state ? columnSumWorker(state) : std::function<PassRowFn(const PassScratch&)>());
        columns.slot = from;
        columns.columns = true;
        list.passes.push_back(columns);

        Pass box = Pass::gather(haloX, haloY, true, state ? boxWorker(state, from, to) : std::function<PassRowFn(const PassScratch&)>());
        box.slot = to;
        // Four bilinear lookups of four entries each.
        box.tapsPerSample = 16;
        list.passes.push_back(box);
    }

    return list;
} // IBlur::passes

std::string
IBlur::mapNotConnectedMessage()
{
    return tr("The Map input is not connected: the blur has the Size everywhere.").toStdString();
}

PassList
IBlur::opPasses(double time,
                ViewIdx view,
                const RenderScale& scale) const
{
    // A disconnected Map reads 1 everywhere; a connected one may hold any value.
    MapRange range;
    range.min = getInput(kIBlurInputMap) ? 0.f : 1.f;
    range.max = 1.f;
    const IBlurParams params = getParams(time, view);
    const PassContext context = getPassContext(scale);
    PassList list = passes(params, context, range, std::shared_ptr<const MapPlane>());

#ifdef NATRON_HAVE_GPU
    bool capped = false;
    for (const Pass& pass : list.passes) {
        capped = capped || (pass.haloX > kGpuMaxBoxHalo) || (pass.haloY > kGpuMaxBoxHalo);
    }
    if (!list.empty() && !capped) {
        const IBlur* self = this;
        list.gpu = [self, time, view, params, context](const PassGpuIO& io) -> StatusEnum {
            return self->recordGpu(io, time, view, params, context);
        };
    }
#endif

    return list;
}

StatusEnum
IBlur::recordGpu(const PassGpuIO& io,
                 double time,
                 ViewIdx view,
                 const IBlurParams& params,
                 const PassContext& context) const
{
#ifdef NATRON_HAVE_GPU
    const bool connected = (bool)getInput(kIBlurInputMap);
    ImageLayerDesc layer;
    int channel = -1;
    // Resolving only reads the Map knob and the Map's layers.
    if (!connected || !_map.resolveChannel(const_cast<IBlur*>(this), kIBlurInputMap, time, view, &layer, &channel)) {
        channel = -1;
    }

    return recordIBlurGpu(io, params, context, connected, channel);
#else
    Q_UNUSED(io);
    Q_UNUSED(time);
    Q_UNUSED(view);
    Q_UNUSED(params);
    Q_UNUSED(context);

    return eStatusFailed;
#endif
}

bool
IBlur::readsGpuInput(int inputNb,
                     const ImageLayerDesc& /*plane*/,
                     double time,
                     ViewIdx view) const
{
    if (inputNb != kIBlurInputMap) {
        return true;
    }
    ImageLayerDesc layer;
    int channel = -1;

    return _map.resolveChannel(const_cast<IBlur*>(this), kIBlurInputMap, time, view, &layer, &channel);
}

bool
IBlur::opPassesInputLayer(int inputNb,
                          const ImageLayerDesc& plane,
                          double time,
                          ViewIdx view,
                          ImageLayerDesc* layer) const
{
    if (inputNb != kIBlurInputMap) {
        *layer = plane;

        return true;
    }
    int channel = -1;

    return _map.resolveChannel(const_cast<IBlur*>(this), kIBlurInputMap, time, view, layer, &channel);
}

RectI
IBlur::opPassesInputRoI(int inputNb,
                        const RectI& outputRect,
                        const RenderScale& scale) const
{
    if (inputNb != kIBlurInputMap) {
        return outputRect;
    }

    return passListSourceRoI(outputRect, opPasses(getCurrentTime(), getCurrentView(), scale));
}

bool
IBlur::isOpIdentity(double time,
                    const RenderScale& scale,
                    const RectI& /*roi*/,
                    ViewIdx view)
{
    return opPasses(time, view, scale).empty();
}

StatusEnum
IBlur::renderOp(const RenderActionArgs& args,
                const StagePlan& plan)
{
    const PassList full = opPasses(args.time, args.view, args.mappedScale);

    if (full.empty()) {
        return runPassesCpu(*this, plan, args, full);
    }
    const IBlurParams params = getParams(args.time, args.view);
    // The Map over every pixel the passes may read; the tile's passes read no further, their halo
    // being at most the full one.
    const RectI window = passListSourceRoI(args.roi, full);
    std::shared_ptr<MapPlane> map = std::make_shared<MapPlane>();
    const StatusEnum status = _map.read(this, kIBlurInputMap, params.map, args.time, args.view, args.mappedScale, window, map.get());
    if (status != eStatusOK) {
        return status;
    }
    const MapRange range = map->tileRange(window);

    return runPassesCpu(*this, plan, args, passes(params, getPassContext(args.mappedScale), range, map));
}

void
IBlur::getRegionsOfInterest(double time,
                            const RenderScale& scale,
                            const RectD& outputRoD,
                            const RectD& renderWindow,
                            ViewIdx view,
                            RoIMap* ret)
{
    StagedImageEffect::getRegionsOfInterest(time, scale, outputRoD, renderWindow, view, ret);

    EffectInstancePtr map = getInput(kIBlurInputMap);
    if (!map) {
        return;
    }
    // The Map is read over the render window grown by the full halo, in the source's pixels.
    const double par = getAspectRatio(kIBlurInputSource);
    const RectI rectPixel = PassExec::toPixelEnclosing(renderWindow, scale, par);
    const RectD mapRoI = PassExec::toCanonical(passListSourceRoI(rectPixel, opPasses(time, view, scale)), scale, par);
    // The Map may be the same effect as the source or the mask, which then needs both regions.
    RoIMap::iterator found = ret->find(map);
    if (found != ret->end()) {
        found->second.merge(mapRoI);
    } else {
        ret->insert(std::make_pair(map, mapRoI));
    }
}

bool
IBlur::clipRegionOfDefinition(double time,
                              const RenderScale& scale,
                              ViewIdx view,
                              const EffectInstancePtr& source,
                              const RectI& srcRoD,
                              RectI* rod)
{
    const KnobReader reader(time, view);

    if (!reader.value(_cropToFormat)) {
        return false;
    }
    RectI format;
    EffectInstancePtr formatSource = source ? source->getNearestNonIdentity(time) : EffectInstancePtr();
    if (formatSource) {
        format = formatSource->getOutputFormat();
    } else {
        Format projectFormat;
        getApp()->getProject()->getProjectDefaultFormat(&projectFormat);
        format = projectFormat;
    }
    const OfxPointD rs = scale.toOfxPointD();
    RectI enclosing;
    enclosing.x1 = (int)std::floor(format.x1 * rs.x);
    enclosing.x2 = (int)std::ceil(format.x2 * rs.x);
    enclosing.y1 = (int)std::floor(format.y1 * rs.y);
    enclosing.y2 = (int)std::ceil(format.y2 * rs.y);
    RectI inner;
    inner.x1 = (int)std::ceil(format.x1 * rs.x);
    inner.x2 = (int)std::floor(format.x2 * rs.x);
    inner.y1 = (int)std::ceil(format.y1 * rs.y);
    inner.y2 = (int)std::floor(format.y2 * rs.y);
    if (PassExec::isEmptyRect(inner)) {
        return false;
    }
    // Only the borders the source had inside its format are clamped back to it.
    bool changed = false;
    if ((rod->x1 < inner.x1) && (srcRoD.x1 >= enclosing.x1)) {
        rod->x1 = inner.x1;
        changed = true;
    }
    if ((rod->x2 > inner.x2) && (srcRoD.x2 <= enclosing.x2)) {
        rod->x2 = inner.x2;
        changed = true;
    }
    if ((rod->y1 < inner.y1) && (srcRoD.y1 >= enclosing.y1)) {
        rod->y1 = inner.y1;
        changed = true;
    }
    if ((rod->y2 > inner.y2) && (srcRoD.y2 <= enclosing.y2)) {
        rod->y2 = inner.y2;
        changed = true;
    }

    return changed;
} // IBlur::clipRegionOfDefinition

void
IBlur::onEffectCreated(bool /*mayCreateFileDialog*/,
                       const CreateNodeArgs& /*args*/)
{
    refreshMapWarning();
}

void
IBlur::onInputChanged(int /*inputNo*/)
{
    refreshMapWarning();
}

void
IBlur::refreshMapWarning()
{
    const NodePtr node = getNode();

    if (!node) {
        return;
    }
    const std::string message = mapNotConnectedMessage();
    if (!getInput(kIBlurInputMap)) {
        node->setPersistentMessage(eMessageTypeWarning, message);

        return;
    }
    // Only our own warning is taken back: anything posted since has replaced it.
    QString current;
    int type = 0;
    node->getPersistentMessage(&current, &type, false);
    if (current == QString::fromUtf8(message.c_str())) {
        node->clearPersistentMessage(false);
    }
}

NATRON_NAMESPACE_EXIT
