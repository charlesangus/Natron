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

#include "Defocus.h"

#include <algorithm>
#include <bit>
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
#include <vector>

#include <QString>

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/KnobReader.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuBlur.h"
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
constexpr InputSpec kDefocusInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
    InputSpec { .label = "Kernel", .role = InputRole::Aux, .optional = true },
};

constexpr NodeSpec kDefocusNodeSpec = {
    .id = PLUGINID_NATRON_DEFOCUS,
    .label = "Defocus",
    .description = "Defocus the input stream, as a lens out of focus does: every pixel spreads into a bokeh of the Size, a disc, an aperture of straight or "
                   "curved blades, or the image connected to the Kernel input.\n"
                   "\n"
                   "Highlight Gain pushes the values above the Highlight Threshold away from it before the defocus and pulls them back after it, so that bright "
                   "out-of-focus highlights bloom into the bokeh's shape while the rest of the image keeps its brightness.\n"
                   "\n"
                   "The defocus is black outside the image, and the Mask and Mix limit the result as usual.",
    .grouping = PLUGIN_GROUP_FILTER,
    .majorVersion = PLUGIN_MAJOR_NATRON_DEFOCUS,
    .icon = NATRON_IMAGES_PATH "NativeNodes/Defocus.png",
    .inputs = kDefocusInputs,
};

constexpr ImagePipelineSpec kDefocusPipeline = {
    .stages = { Stage::MaskMix, Stage::PassThroughUnprocessed },
    .layers = LayerPolicy::all(),
    .op = OpKind::Passes,
};

const double kMaxSize = 500.;
// A defensive bound on either radius, in pixels, past what the knobs reach at any aspect ratio.
const double kMaxRadius = 2048.;
const int kKernelTypeDisc = 0;
const int kKernelTypeBlades = 1;
const int kKernelTypeImage = 2;
// A Kernel image larger than this many pixels is read over its image only.
const std::size_t kMaxKernelImagePixels = (std::size_t)1 << 24;

/**
 * @brief What the passes of one render share: the kernel's runs, the highlight, the render
 * window and the prefix tables, (width + 1) x height doubles per plane. The first pass sizes the
 * tables for the plane job it runs in, so later passes of the job only read them; only a job with
 * more planes than any before reallocates them, and never while a pass of another job runs.
 **/
struct DefocusState {
    std::vector<BokehRun> runs;
    BokehHighlight highlight;
    RectI window;

    std::mutex mutex;
    std::unique_ptr<double[]> tables;
    std::size_t capacity = 0;

    static std::size_t tablePlaneSize(const PassScratch& scratch)
    {
        return ((std::size_t)scratch.width + 1) * (std::size_t)scratch.height;
    }

    void reserve(const PassScratch& scratch)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        const std::size_t size = tablePlaneSize(scratch) * (std::size_t)scratch.nPlanes;
        if (capacity < size) {
            tables.reset(new double[size]);
            capacity = size;
        }
    }
};

using DefocusStatePtr = std::shared_ptr<DefocusState>;

/// Slot 1, every pixel of every plane: slot 0 with its highlights boosted.
std::function<PassRowFn(const PassScratch&)>
boostWorker(const DefocusStatePtr& state)
{
    return [state](const PassScratch& scratch) -> PassRowFn {
        const BokehHighlight highlight = state->highlight;
        const PassScratch* buffers = &scratch;

        return [highlight, buffers](int line) {
            const int height = buffers->height;
            const int width = buffers->width;
            const int p = line / height;
            const std::size_t row = (std::size_t)(line % height) * width;
            const float* const src = buffers->plane(0, p) + row;
            float* const dst = buffers->plane(1, p) + row;
            if (!highlight.isActive()) {
                std::copy(src, src + width, dst);

                return;
            }
            for (int x = 0; x < width; ++x) {
                dst[x] = highlight.boost(src[x]);
            }
        };
    };
}

/// The prefix table of every row of slot 1.
std::function<PassRowFn(const PassScratch&)>
prefixWorker(const DefocusStatePtr& state)
{
    return [state](const PassScratch& scratch) -> PassRowFn {
        state->reserve(scratch);
        double* const tables = state->tables.get();
        const std::size_t planeSize = DefocusState::tablePlaneSize(scratch);
        const PassScratch* buffers = &scratch;

        return [tables, planeSize, buffers](int line) {
            const int height = buffers->height;
            const int width = buffers->width;
            const int p = line / height;
            const int y = line % height;
            bokehPrefixRow(buffers->plane(1, p) + (std::size_t)y * width, width, tables + (std::size_t)p * planeSize + (std::size_t)y * (width + 1));
        };
    };
}

/// Slot 1 over the render window: the gather of the boosted source. Elsewhere slot 1 keeps the
/// boosted source, which nothing reads.
std::function<PassRowFn(const PassScratch&)>
gatherWorker(const DefocusStatePtr& state)
{
    return [state](const PassScratch& scratch) -> PassRowFn {
        state->reserve(scratch);
        const DefocusState* s = state.get();
        const double* const tables = s->tables.get();
        const std::size_t planeSize = DefocusState::tablePlaneSize(scratch);
        const PassScratch* buffers = &scratch;
        const int x1 = std::clamp(s->window.x1 - scratch.rect.x1, 0, scratch.width);
        const int x2 = std::clamp(s->window.x2 - scratch.rect.x1, x1, scratch.width);
        const int y1 = s->window.y1 - scratch.rect.y1;
        const int y2 = s->window.y2 - scratch.rect.y1;
        std::shared_ptr<std::vector<double>> acc = std::make_shared<std::vector<double>>((std::size_t)std::max(1, x2 - x1));

        return [s, tables, planeSize, buffers, x1, x2, y1, y2, acc](int line) {
            const int height = buffers->height;
            const int width = buffers->width;
            const int p = line / height;
            const int y = line % height;
            if ((y < y1) || (y >= y2) || (x1 == x2)) {
                return;
            }
            float* const dst = buffers->plane(1, p) + (std::size_t)y * width + x1;
            bokehGatherRow(tables + (std::size_t)p * planeSize, width, height, s->runs, y, x1, x2, acc->data(), dst);
        };
    };
}

/// Slot 1, every pixel of every plane: the boost undone.
std::function<PassRowFn(const PassScratch&)>
unboostWorker(const DefocusStatePtr& state)
{
    return [state](const PassScratch& scratch) -> PassRowFn {
        const BokehHighlight highlight = state->highlight;
        const PassScratch* buffers = &scratch;

        return [highlight, buffers](int line) {
            const int height = buffers->height;
            const int width = buffers->width;
            const int p = line / height;
            float* const row = buffers->plane(1, p) + (std::size_t)(line % height) * width;
            for (int x = 0; x < width; ++x) {
                row[x] = highlight.unboost(row[x]);
            }
        };
    };
}

// The fewest pixel rows a band of cells gathers together, so that its rows keep the threads busy.
const int kMinBandRows = 32;

/**
 * @brief What the passes of a render with lens aberrations share besides DefocusState: the lens
 * and, for the band being gathered, the runs of each group's kernel per cell, which kernel gives
 * for a group and a cell's clip.
 **/
struct DefocusLensState {
    explicit DefocusLensState(const LensAberration& lensIn)
        : lens(lensIn)
    {
    }

    DefocusStatePtr base;
    LensAberration lens;
    std::function<BokehRunCache::Runs(int, const BokehClip&)> kernel;
    LensBandRuns band;
};

using DefocusLensStatePtr = std::shared_ptr<DefocusLensState>;

/// The kernels of band b, spread over the lines, replacing the band before.
std::function<PassRowFn(const PassScratch&)>
lensBuildWorker(const DefocusLensStatePtr& state,
                const LensBand& b)
{
    return [state, b](const PassScratch& scratch) -> PassRowFn {
        DefocusLensState* s = state.get();
        const int nLines = scratch.height;

        return [s, b, nLines](int line) {
            std::size_t first = 0;
            std::size_t end = 0;
            spreadItems(line, nLines, s->band.size(b), &first, &end);
            for (std::size_t k = first; k < end; ++k) {
                int group = 0;
                int i = 0;
                int j = 0;
                s->band.item(b, k, &group, &i, &j);
                s->band.runs[k] = s->kernel(group, s->lens.clip(i, j));
            }
        };
    };
}

/// Slot 1 over band b's rows of the render window: the gather of the boosted source, cell by cell.
std::function<PassRowFn(const PassScratch&)>
lensGatherWorker(const DefocusLensStatePtr& state,
                 const LensBand& b)
{
    return [state, b](const PassScratch& scratch) -> PassRowFn {
        DefocusState* base = state->base.get();
        base->reserve(scratch);
        const DefocusLensState* s = state.get();
        const double* const tables = base->tables.get();
        const std::size_t planeSize = DefocusState::tablePlaneSize(scratch);
        const PassScratch* buffers = &scratch;
        const int x1 = std::clamp(base->window.x1 - scratch.rect.x1, 0, scratch.width);
        const int x2 = std::clamp(base->window.x2 - scratch.rect.x1, x1, scratch.width);
        const int y1 = std::clamp(b.y1 - scratch.rect.y1, 0, scratch.height);
        const int y2 = std::clamp(b.y2 - scratch.rect.y1, y1, scratch.height);
        std::shared_ptr<std::vector<double>> acc = std::make_shared<std::vector<double>>((std::size_t)std::max(1, x2 - x1));

        return [s, b, tables, planeSize, buffers, x1, x2, y1, y2, acc](int line) {
            const int width = buffers->width;
            const int height = buffers->height;
            const int rows = y2 - y1;
            std::size_t first = 0;
            std::size_t end = 0;
            spreadItems(line, buffers->nPlanes * height, (std::size_t)buffers->nPlanes * rows, &first, &end);
            for (std::size_t item = first; item < end; ++item) {
                const int p = (int)(item / (std::size_t)rows);
                const int y = y1 + (int)(item % (std::size_t)rows);
                const int group = s->lens.groupOf(buffers->channelBits[p]);
                const int j = s->lens.rowOf(buffers->rect.y1 + y);
                const double* const planeTables = tables + (std::size_t)p * planeSize;
                float* const dst = buffers->plane(1, p) + (std::size_t)y * width;
                s->lens.forEachSegment(buffers->rect.x1 + x1, buffers->rect.x1 + x2, [&](int xa, int xb, int i) {
                    const int a = xa - buffers->rect.x1;
                    bokehGatherRow(planeTables, width, height, s->band.at(b, group, i, j), y, a, xb - buffers->rect.x1, acc->data(), dst + a);
                });
            }
        };
    };
}

BokehShape
shapeOf(int kernelType)
{
    switch (kernelType) {
    case kKernelTypeBlades:
        return BokehShape::Blades;
    case kKernelTypeImage:
        return BokehShape::Image;
    default:
        return BokehShape::Disc;
    }
}

#ifdef NATRON_HAVE_GPU
// The largest halo, in pixels, the GPU records; a larger defocus stays on the CPU.
const int kGpuMaxHalo = 256;

// Mirror kBlock and kBoost in Kernels/defocus.slang.
const uint32_t kGpuBlock = 256;
const uint32_t kGpuBoost = 1;

// Mirrors DefocusParams in Kernels/defocus.slang, which has no padding between members.
struct DefocusGpuParams {
    int32_t domX1;
    int32_t domY1;
    uint32_t domWidth;
    uint32_t domHeight;
    uint32_t channels;
    uint32_t nBlocks;
    uint32_t localOffset;
    uint32_t totalOffset;
    uint32_t runCount;
    uint32_t flags;
    float threshold;
    float gain;
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
static_assert(sizeof(DefocusGpuParams) == 96);
static_assert(offsetof(DefocusGpuParams, srcX1) == 48);
static_assert(offsetof(DefocusGpuParams, outX1) == 72);

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

// Every kernel of Kernels/defocus.slang binds these, in this order.
struct DefocusGpuBuffers {
    const gpu::GpuBuffer* source = nullptr;
    const gpu::GpuBuffer* runs = nullptr;
    const gpu::GpuBuffer* work = nullptr;
    const gpu::GpuBuffer* out = nullptr;
};

bool
dispatchDefocus(gpu::GpuRecordContext& ctx,
                gpu::GpuKernelId kernel,
                const DefocusGpuBuffers& b,
                const DefocusGpuParams& p,
                uint32_t width,
                uint32_t height)
{
    if (!width || !height) {
        return true;
    }

    return (bool)ctx.dispatch(kernel, { b.source, b.runs, b.work, b.out }, p, { width, height, 1 });
}

/**
 * @brief The passes of Defocus::passes() as the GPU records them over io.domain, the CPU's
 * scratch rect: the boosted source into the work buffer, each row's block prefixes and block
 * totals, then the gather of every output pixel, unboosted, into io.filtered.
 **/
StatusEnum
recordDefocusGpu(const PassGpuIO& io,
                 const DefocusParams& params,
                 const BokehKernel& kernel)
{
    gpu::GpuRecordContext& ctx = io.context;
    const RectI& domain = io.domain;
    const int nComps = io.nComps;
    if (PassExec::isEmptyRect(domain) || (nComps < 1) || (nComps > 4) || !io.filtered.buffer) {
        return eStatusFailed;
    }
    const std::vector<BokehRun> runs = kernel.runs();
    if (runs.empty()) {
        return eStatusFailed;
    }
    const uint64_t width = (uint64_t)domain.width();
    const uint64_t height = (uint64_t)domain.height();
    const uint64_t nBlocks = (width + kGpuBlock - 1) / kGpuBlock;
    const uint64_t slotFloats = width * height * (uint64_t)nComps;
    const uint64_t localFloats = (width + 1) * height * (uint64_t)nComps;
    const uint64_t totalFloats = nBlocks * height * (uint64_t)nComps;
    const uint64_t workFloats = slotFloats + localFloats + totalFloats;
    if (workFloats > UINT32_MAX) {
        return eStatusFailed;
    }

    std::vector<uint32_t> packed;
    packed.reserve(runs.size() * 4);
    for (const BokehRun& run : runs) {
        packed.push_back((uint32_t)run.dy);
        packed.push_back((uint32_t)run.dx0);
        packed.push_back((uint32_t)run.length);
        packed.push_back(std::bit_cast<uint32_t>(run.weight));
    }
    gpu::GpuBuffer* runBuffer = nullptr;
    gpu::GpuBuffer* work = nullptr;
    const std::size_t runBytes = packed.size() * sizeof(uint32_t);
    if (!ctx.scratch(runBytes, runBuffer) || !runBuffer || !ctx.scratch(workFloats * sizeof(float), work) || !work) {
        return eStatusFailed;
    }
    const std::span<const std::byte> bytes = std::as_bytes(std::span<const uint32_t>(packed));
    const std::size_t kChunk = 65536;
    for (std::size_t offset = 0; offset < bytes.size(); offset += kChunk) {
        if (!ctx.updateBuffer(*runBuffer, offset, bytes.subspan(offset, std::min(kChunk, bytes.size() - offset)))) {
            return eStatusFailed;
        }
    }

    DefocusGpuBuffers buffers;
    buffers.source = runBuffer;
    buffers.runs = runBuffer;
    buffers.work = work;
    buffers.out = io.filtered.buffer;

    DefocusGpuParams base {};
    base.domX1 = domain.x1;
    base.domY1 = domain.y1;
    base.domWidth = (uint32_t)width;
    base.domHeight = (uint32_t)height;
    base.channels = (uint32_t)nComps;
    base.nBlocks = (uint32_t)nBlocks;
    base.localOffset = (uint32_t)slotFloats;
    base.totalOffset = (uint32_t)(slotFloats + localFloats);
    base.runCount = (uint32_t)runs.size();
    base.flags = params.highlight.isActive() ? kGpuBoost : 0u;
    base.threshold = params.highlight.threshold;
    base.gain = params.highlight.gain;
    const gpu::GpuBlurImage filtered = PassExec::toGpuBlurImage(io.filtered);
    base.outX1 = io.output.x1;
    base.outY1 = io.output.y1;
    base.outWidth = (uint32_t)std::max(0, io.output.width());
    base.outHeight = (uint32_t)std::max(0, io.output.height());
    base.dstStride = filtered.strideFloats;
    if (!PassExec::isEmptyRect(io.output) && !gpuFloatIndex(filtered, io.output.x1, io.output.y1, nComps, &base.dstOffset)) {
        return eStatusFailed;
    }

    DefocusGpuParams load = base;
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
    if (!dispatchDefocus(ctx, gpu::GpuKernelId::defocus_load, buffers, load, base.domWidth, base.domHeight)) {
        return eStatusFailed;
    }
    ctx.computeBarrier();
    if (!dispatchDefocus(ctx, gpu::GpuKernelId::defocus_blocks, buffers, base, base.nBlocks, base.domHeight)) {
        return eStatusFailed;
    }
    ctx.computeBarrier();
    if (!dispatchDefocus(ctx, gpu::GpuKernelId::defocus_gather, buffers, base, base.outWidth, base.outHeight)) {
        return eStatusFailed;
    }

    return eStatusOK;
} // recordDefocusGpu
#endif // NATRON_HAVE_GPU
} // anonymous namespace

struct Defocus::ShapeCache {
    std::mutex mutex;
    BokehShapeParams shape;
    double radiusX = -1.;
    double radiusY = -1.;
    std::shared_ptr<const BokehKernel> kernel;
};

Defocus::Defocus(NodePtr node)
    : StagedImageEffect(node)
    , _size()
    , _aspectRatio()
    , _kernelType()
    , _blades()
    , _rotation()
    , _roundness()
    , _kernelChannel()
    , _highlightThreshold()
    , _highlightGain()
    , _bokehKnobs()
    , _shapeCache(new ShapeCache)
    , _runCache(std::make_shared<BokehRunCache>())
{
}

Defocus::~Defocus()
{
}

const NodeSpec*
Defocus::nodeSpec() const
{
    return staticNodeSpec<kDefocusNodeSpec>();
}

const ImagePipelineSpec&
Defocus::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kDefocusPipeline, kDefocusNodeSpec>();
}

std::string
Defocus::getInputHint(int inputNb) const
{
    switch (inputNb) {
    case kDefocusInputSource:
        return tr("The image to defocus.").toStdString();
    case kDefocusInputMask:
        return tr("The mask the defocused result is mixed with the source through.").toStdString();
    case kDefocusInputKernel:
        return tr("The Kernel: with the Image kernel type, the bokeh's shape, its Kernel Channel over its region of definition.").toStdString();
    }

    return std::string();
}

void
Defocus::initializeKnobs()
{
    const KnobPagePtr page = createControlsPage();

    KnobDoublePtr sizeKnob = AppManager::createKnob<KnobDouble>(this, tr("Size"));
    sizeKnob->setName(kDefocusParamSize);
    sizeKnob->setHintToolTip(tr("Diameter of the bokeh, in pixel units (>=0). Below one pixel the image is left unchanged."));
    sizeKnob->setSpatial(true);
    sizeKnob->setMinimum(0.);
    sizeKnob->setMaximum(kMaxSize);
    sizeKnob->setDisplayMinimum(0.);
    sizeKnob->setDisplayMaximum(100.);
    sizeKnob->setIncrement(0.1);
    sizeKnob->setDecimals(1);
    sizeKnob->setDefaultValue(10.);
    page->addKnob(sizeKnob);
    _size = sizeKnob;

    KnobDoublePtr aspectKnob = AppManager::createKnob<KnobDouble>(this, tr("Aspect Ratio"));
    aspectKnob->setName(kDefocusParamAspectRatio);
    aspectKnob->setHintToolTip(tr("Width of the bokeh over its height: the width is the Size times the aspect ratio, the height the Size."));
    aspectKnob->setMinimum(0.1);
    aspectKnob->setMaximum(10.);
    aspectKnob->setDisplayMinimum(0.25);
    aspectKnob->setDisplayMaximum(4.);
    aspectKnob->setIncrement(0.01);
    aspectKnob->setDecimals(2);
    aspectKnob->setDefaultValue(1.);
    page->addKnob(aspectKnob);
    _aspectRatio = aspectKnob;

    KnobChoicePtr typeKnob = AppManager::createKnob<KnobChoice>(this, tr("Kernel Type"));
    typeKnob->setName(kDefocusParamKernelType);
    typeKnob->setHintToolTip(tr("The shape of the bokeh."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kDefocusParamKernelTypeDisc, tr("Disc").toStdString(), tr("A disc, as a round aperture gives.").toStdString()));
        options.push_back(ChoiceOption(kDefocusParamKernelTypeBlades, tr("Blades").toStdString(), tr("An aperture of Blades straight or curved blades.").toStdString()));
        options.push_back(ChoiceOption(kDefocusParamKernelTypeImage, tr("Image").toStdString(), tr("The Kernel input's Kernel Channel, stretched over the Size.").toStdString()));
        typeKnob->populateChoices(options);
    }
    typeKnob->setDefaultValue(kKernelTypeDisc);
    typeKnob->setAnimationEnabled(false);
    page->addKnob(typeKnob);
    _kernelType = typeKnob;

    KnobIntPtr bladesKnob = AppManager::createKnob<KnobInt>(this, tr("Blades"));
    bladesKnob->setName(kDefocusParamBlades);
    bladesKnob->setHintToolTip(tr("Blades kernel only: the number of blades of the aperture."));
    bladesKnob->setMinimum(3);
    bladesKnob->setMaximum(16);
    bladesKnob->setDisplayMinimum(3);
    bladesKnob->setDisplayMaximum(12);
    bladesKnob->setDefaultValue(6);
    page->addKnob(bladesKnob);
    _blades = bladesKnob;

    KnobDoublePtr rotationKnob = AppManager::createKnob<KnobDouble>(this, tr("Rotation"));
    rotationKnob->setName(kDefocusParamRotation);
    rotationKnob->setHintToolTip(tr("Blades kernel only: the aperture's rotation in degrees, counter-clockwise. At 0 a corner points up."));
    rotationKnob->setDisplayMinimum(-180.);
    rotationKnob->setDisplayMaximum(180.);
    rotationKnob->setDefaultValue(0.);
    page->addKnob(rotationKnob);
    _rotation = rotationKnob;

    KnobDoublePtr roundnessKnob = AppManager::createKnob<KnobDouble>(this, tr("Roundness"));
    roundnessKnob->setName(kDefocusParamRoundness);
    roundnessKnob->setHintToolTip(tr("Blades kernel only: how curved the blades are, from straight at 0 to the disc at 1."));
    roundnessKnob->setMinimum(0.);
    roundnessKnob->setMaximum(1.);
    roundnessKnob->setDisplayMinimum(0.);
    roundnessKnob->setDisplayMaximum(1.);
    roundnessKnob->setDefaultValue(0.);
    page->addKnob(roundnessKnob);
    _roundness = roundnessKnob;

    _bokehKnobs.addTextureKnobs(this, page);

    KnobChannelSelectPtr channelKnob = AppManager::createKnob<KnobChannelSelect>(this, tr("Kernel Channel"));
    channelKnob->setName(kDefocusParamKernelChannel);
    channelKnob->setAnimationEnabled(false);
    channelKnob->setDefaultValue(channelKnob->encode(std::string(kNatronColorViewRGBA) + ".A"));
    channelKnob->setHintToolTip(tr("Image kernel only: the channel of the Kernel input, in any of its layers, that holds the bokeh. A channel the input does not have, or None, reads 0."));
    page->addKnob(channelKnob);
    _kernelChannel = channelKnob;

    _bokehKnobs.addLensKnobs(this, page);

    KnobDoublePtr thresholdKnob = AppManager::createKnob<KnobDouble>(this, tr("Highlight Threshold"));
    thresholdKnob->setName(kDefocusParamHighlightThreshold);
    thresholdKnob->setHintToolTip(tr("Values above the threshold are highlights, which Highlight Gain boosts before the defocus."));
    thresholdKnob->setMinimum(0.);
    thresholdKnob->setDisplayMinimum(0.);
    thresholdKnob->setDisplayMaximum(10.);
    thresholdKnob->setDefaultValue(1.);
    page->addKnob(thresholdKnob);
    _highlightThreshold = thresholdKnob;

    KnobDoublePtr gainKnob = AppManager::createKnob<KnobDouble>(this, tr("Highlight Gain"));
    gainKnob->setName(kDefocusParamHighlightGain);
    gainKnob->setHintToolTip(tr("How much the highlights' excess over the threshold is multiplied by before the defocus, and divided by after it, "
                                "so that they bloom into bright bokeh. 1 leaves them as they are."));
    gainKnob->setMinimum(1.);
    gainKnob->setMaximum(1000.);
    gainKnob->setDisplayMinimum(1.);
    gainKnob->setDisplayMaximum(20.);
    gainKnob->setDefaultValue(1.);
    page->addKnob(gainKnob);
    _highlightGain = gainKnob;

    addStageKnobs(page);

    getNode()->declareLayerKnob(channelKnob, kDefocusInputKernel, LayerKnobSpec::eRoleInputBound);
} // Defocus::initializeKnobs

DefocusParams
Defocus::getParams(double time,
                   ViewIdx view) const
{
    const KnobReader reader(time, view);
    DefocusParams params;
    const double size = reader.value(_size);
    const double aspect = reader.value(_aspectRatio);

    params.size = std::isfinite(size) ? std::min(kMaxSize, std::max(0., size)) : 0.;
    params.aspectRatio = std::isfinite(aspect) ? std::min(10., std::max(0.1, aspect)) : 1.;
    params.shape.shape = shapeOf(reader.value(_kernelType));
    params.shape.blades = std::max(3, (int)reader.value(_blades));
    params.shape.rotation = reader.value(_rotation);
    params.shape.roundness = reader.value(_roundness);
    _bokehKnobs.readTexture(reader, &params.shape);
    params.lens = _bokehKnobs.readLens(reader);
    params.highlight.threshold = (float)std::max(0., reader.value(_highlightThreshold));
    params.highlight.gain = (float)std::max(1., reader.value(_highlightGain));
    if (!std::isfinite(params.highlight.threshold) || !std::isfinite(params.highlight.gain)) {
        params.highlight = BokehHighlight();
    }

    return params;
}

void
Defocus::radii(const DefocusParams& params,
               const PassContext& context,
               double* radiusX,
               double* radiusY)
{
    radii(params, context, 1., radiusX, radiusY);
}

void
Defocus::radii(const DefocusParams& params,
               const PassContext& context,
               double scale,
               double* radiusX,
               double* radiusY)
{
    const OfxPointD rs = context.scale.toOfxPointD();
    const double par = (context.pixelAspectRatio > 0.) ? context.pixelAspectRatio : 1.;
    // Each channel's size clamps on its own.
    const double size = (scale == 1.) ? params.size : std::min(kMaxSize, params.size * scale);

    *radiusX = std::min(kMaxRadius, rs.x * size * params.aspectRatio / 2. / par);
    *radiusY = std::min(kMaxRadius, rs.y * size / 2.);
}

PassList
Defocus::passes(const DefocusParams& params,
                const PassContext& context,
                const std::shared_ptr<const BokehKernel>& kernel,
                const RectI& window)
{
    PassList list;
    double rx = 0.;
    double ry = 0.;
    radii(params, context, params.lens.maxScale(), &rx, &ry);
    if (BokehKernel::isIdentityRadius(rx, ry)) {
        return list;
    }
    const int haloX = BokehKernel::extent(rx);
    const int haloY = BokehKernel::extent(ry);

    DefocusStatePtr state;
    if (kernel) {
        state = std::make_shared<DefocusState>();
        state->runs = kernel->runs();
        state->highlight = params.highlight;
        state->window = window;
    }
    using Worker = std::function<PassRowFn(const PassScratch&)>;

    list.scratchSlots = 2;
    list.resultSlot = 1;

    Pass boost = Pass::point(true, state ? boostWorker(state) : Worker());
    boost.slot = 1;
    list.passes.push_back(boost);

    Pass prefix = Pass::point(true, state ? prefixWorker(state) : Worker());
    prefix.slot = 1;
    list.passes.push_back(prefix);

    Pass gather = Pass::gather(haloX, haloY, true, state ? gatherWorker(state) : Worker());
    gather.slot = 1;
    // Two table reads per run.
    gather.tapsPerSample = state ? std::max<std::size_t>(1, 2 * state->runs.size()) : (std::size_t)(2 * (2 * haloY + 1));
    gather.sigma = 2. * std::max(rx, ry) / 2.4;
    list.passes.push_back(gather);

    if (params.highlight.isActive()) {
        Pass unboost = Pass::point(true, state ? unboostWorker(state) : Worker());
        unboost.slot = 1;
        list.passes.push_back(unboost);
    }

    return list;
} // Defocus::passes

PassList
Defocus::lensPasses(const DefocusParams& params,
                    const PassContext& context,
                    const LensAberration& lens,
                    const std::function<BokehRunCache::Runs(int group, const BokehClip& clip)>& kernel,
                    const RectI& window)
{
    PassList list;
    double rx = 0.;
    double ry = 0.;
    radii(params, context, params.lens.maxScale(), &rx, &ry);
    if (BokehKernel::isIdentityRadius(rx, ry)) {
        return list;
    }
    const int haloX = BokehKernel::extent(rx);
    const int haloY = BokehKernel::extent(ry);
    using Worker = std::function<PassRowFn(const PassScratch&)>;

    DefocusStatePtr base = std::make_shared<DefocusState>();
    base->highlight = params.highlight;
    base->window = window;
    DefocusLensStatePtr state = std::make_shared<DefocusLensState>(lens);
    state->base = base;
    state->kernel = kernel;
    state->band.cells = lens.cells(window);
    state->band.groups = lens.groupCount();

    list.scratchSlots = 2;
    list.resultSlot = 1;

    // The whole halo, once, as passes() gives it for the region actions.
    Pass extent = Pass::gather(haloX, haloY, true, Worker());
    extent.slot = 1;
    extent.tapsPerSample = (std::size_t)(2 * (2 * haloY + 1));
    extent.sigma = 2. * std::max(rx, ry) / 2.4;
    list.passes.push_back(extent);

    Pass boost = Pass::point(true, boostWorker(base));
    boost.slot = 1;
    list.passes.push_back(boost);

    Pass prefix = Pass::point(true, prefixWorker(base));
    prefix.slot = 1;
    list.passes.push_back(prefix);

    // A band of cells at a time, so that only its kernels are held.
    const std::vector<LensBand> bands = lens.bands(window, kMinBandRows);
    state->band.reserve(bands);
    for (const LensBand& b : bands) {
        Pass build = Pass::point(false, lensBuildWorker(state, b));
        build.slot = 1;
        list.passes.push_back(build);

        Pass gather = Pass::point(true, lensGatherWorker(state, b));
        gather.slot = 1;
        list.passes.push_back(gather);
    }

    if (params.highlight.isActive()) {
        Pass unboost = Pass::point(true, unboostWorker(base));
        unboost.slot = 1;
        list.passes.push_back(unboost);
    }

    return list;
} // Defocus::lensPasses

bool
Defocus::fitsGpu(const DefocusParams& params,
                 const PassList& passes)
{
#ifdef NATRON_HAVE_GPU
    if ((params.shape.shape == BokehShape::Image) || params.lens.isActive()) {
        return false;
    }
    for (const Pass& pass : passes.passes) {
        if ((pass.haloX > kGpuMaxHalo) || (pass.haloY > kGpuMaxHalo)) {
            return false;
        }
    }

    return true;
#else
    Q_UNUSED(params);
    Q_UNUSED(passes);

    return false;
#endif
}

StatusEnum
Defocus::recordPassesGpu(const PassGpuIO& io,
                         const DefocusParams& params,
                         const PassContext& context)
{
#ifdef NATRON_HAVE_GPU
    if ((params.shape.shape == BokehShape::Image) || params.lens.isActive()) {
        return eStatusFailed;
    }
    double rx = 0.;
    double ry = 0.;
    radii(params, context, &rx, &ry);
    if (BokehKernel::isIdentityRadius(rx, ry)) {
        return eStatusFailed;
    }
    const BokehKernel kernel = BokehKernel::gatherKernel(params.shape, rx, ry);
    if ((kernel.haloX() > kGpuMaxHalo) || (kernel.haloY() > kGpuMaxHalo)) {
        return eStatusFailed;
    }

    return recordDefocusGpu(io, params, kernel);
#else
    Q_UNUSED(io);
    Q_UNUSED(params);
    Q_UNUSED(context);

    return eStatusFailed;
#endif
}

std::string
Defocus::kernelNotConnectedMessage()
{
    return tr("The Kernel input is not connected: the Image kernel leaves the image unchanged.").toStdString();
}

std::string
Defocus::kernelEmptyMessage()
{
    return tr("The Kernel Channel of the Kernel input is empty or black: the Image kernel leaves the image unchanged.").toStdString();
}

bool
Defocus::isImageKernel(double time,
                       ViewIdx view) const
{
    const KnobReader reader(time, view);

    return shapeOf(reader.value(_kernelType)) == BokehShape::Image;
}

bool
Defocus::resolveKernelChannel(double time,
                              ViewIdx view,
                              ImageLayerDesc* layer,
                              int* channel) const
{
    KnobChannelSelectPtr knob = _kernelChannel.lock();
    if (!knob || knob->isNone() || !getInput(kDefocusInputKernel)) {
        return false;
    }
    std::list<ImageLayerDesc> present;
    const_cast<Defocus*>(this)->getPresentLayers(time, view, kDefocusInputKernel, &present);

    return knob->resolve(present, layer, channel) && (*channel >= 0);
}

PassList
Defocus::opPasses(double time,
                  ViewIdx view,
                  const RenderScale& scale) const
{
    const DefocusParams params = getParams(time, view);
    if ((params.shape.shape == BokehShape::Image) && !getInput(kDefocusInputKernel)) {
        return PassList();
    }
    const PassContext context = getPassContext(time, view, scale);
    PassList list = passes(params, context, std::shared_ptr<const BokehKernel>(), RectI());

#ifdef NATRON_HAVE_GPU
    if (!list.empty() && fitsGpu(params, list)) {
        list.gpu = [params, context](const PassGpuIO& io) -> StatusEnum {
            return recordPassesGpu(io, params, context);
        };
    }
#endif

    return list;
}

bool
Defocus::readsGpuInput(int inputNb,
                       const ImageLayerDesc& /*plane*/,
                       double /*time*/,
                       ViewIdx /*view*/) const
{
    return inputNb != kDefocusInputKernel;
}

std::optional<GpuCostHint>
Defocus::gpuCostHint(double time,
                     ViewIdx view,
                     unsigned int mipmapLevel) const
{
    return passListCostHint(opPasses(time, view, RenderScale::fromMipmapLevel(mipmapLevel)));
}

bool
Defocus::getInputLayersRead(int inputNb,
                            double time,
                            ViewIdx view,
                            std::list<ImageLayerDesc>* layers)
{
    if ((inputNb != kDefocusInputKernel) || !isImageKernel(time, view)) {
        return false;
    }
    ImageLayerDesc layer;
    int channel = -1;
    if (!resolveKernelChannel(time, view, &layer, &channel)) {
        return false;
    }
    layers->push_back(layer);

    return true;
}

bool
Defocus::isOpIdentity(double time,
                      const RenderScale& scale,
                      const RectI& /*roi*/,
                      ViewIdx view)
{
    return opPasses(time, view, scale).empty();
}

StatusEnum
Defocus::buildKernel(const DefocusParams& params,
                     const PassContext& context,
                     double time,
                     ViewIdx view,
                     const RenderScale& scale,
                     std::shared_ptr<const BokehKernel>* kernel,
                     bool* empty,
                     std::shared_ptr<const DefocusKernelImage>* kernelImage)
{
    double rx = 0.;
    double ry = 0.;
    radii(params, context, &rx, &ry);
    *empty = false;

    if (params.shape.shape != BokehShape::Image) {
        ShapeCache& cache = *_shapeCache;
        const std::lock_guard<std::mutex> lock(cache.mutex);
        const bool hit = cache.kernel && (cache.radiusX == rx) && (cache.radiusY == ry) && (cache.shape == params.shape);
        if (!hit) {
            cache.kernel = std::make_shared<const BokehKernel>(BokehKernel::gatherKernel(params.shape, rx, ry));
            cache.shape = params.shape;
            cache.radiusX = rx;
            cache.radiusY = ry;
        }
        *kernel = cache.kernel;

        return eStatusOK;
    }

    EffectInstancePtr input = getInput(kDefocusInputKernel);
    ImageLayerDesc layer;
    int channel = -1;
    ImagePtr image;
    if (input && resolveKernelChannel(time, view, &layer, &channel)) {
        RectI roi;
        // A selected layer is read as it is stored, not in the clip's colour layout.
        image = getImage(kDefocusInputKernel, time, scale, view, NULL, &layer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &roi);
        if (image && !PassExec::isFloatImage(image)) {
            return eStatusFailed;
        }
    }
    if (!image || (channel >= (int)image->getComponentsCount())) {
        *kernel = std::make_shared<const BokehKernel>();
        *empty = (bool)input;

        return eStatusOK;
    }
    // The kernel is the input's region of definition, which getRegionsOfInterest() asked for.
    const RectI bounds = image->getBounds();
    RectI frame = bounds;
    RectD rodD;
    bool isProjectFormat = false;
    if (input->getRegionOfDefinition_public(input->getRenderHash(), time, scale, view, &rodD, &isProjectFormat) == eStatusOK) {
        const double par = getRenderMetadata(time, view)->getPixelAspectRatio(kDefocusInputKernel);
        const RectI rodPixel = PassExec::toPixelEnclosing(rodD, scale, par);
        if (!PassExec::isEmptyRect(rodPixel) && ((std::size_t)rodPixel.width() * (std::size_t)rodPixel.height() <= kMaxKernelImagePixels)) {
            frame = rodPixel;
        }
    }
    if (PassExec::isEmptyRect(frame) || ((std::size_t)frame.width() * (std::size_t)frame.height() > kMaxKernelImagePixels)) {
        *kernel = std::make_shared<const BokehKernel>();
        *empty = true;

        return eStatusOK;
    }
    std::shared_ptr<DefocusKernelImage> read = std::make_shared<DefocusKernelImage>();
    const int width = frame.width();
    const int height = frame.height();
    const int nComps = (int)image->getComponentsCount();
    read->width = width;
    read->height = height;
    read->values.resize((std::size_t)width * height);
    Image::ReadAccess access(image.get());
    for (int y = 0; y < height; ++y) {
        PassExec::readChannelRow(image.get(), &access, bounds, nComps, channel, frame.x1, frame.y1 + y, width, 0.f, &read->values[(std::size_t)y * width]);
    }
    *kernel = std::make_shared<const BokehKernel>(BokehKernel::fromImage(read->values.data(), width, height, rx, ry, empty));
    if (kernelImage) {
        *kernelImage = read;
    }

    return eStatusOK;
} // Defocus::buildKernel

StatusEnum
Defocus::renderOp(const RenderActionArgs& args,
                  const StagePlan& plan)
{
    const PassList full = opPasses(args.time, args.view, args.mappedScale);

    if (full.empty()) {
        return runPassesCpu(*this, plan, args, full);
    }
    const DefocusParams params = getParams(args.time, args.view);
    const PassContext context = getPassContext(args.time, args.view, args.mappedScale);
    std::shared_ptr<const BokehKernel> kernel;
    std::shared_ptr<const DefocusKernelImage> kernelImage;
    bool empty = false;
    // With the lens aberrations an analytic kernel is only built per group and cell.
    if (!params.lens.isActive() || (params.shape.shape == BokehShape::Image)) {
        const StatusEnum status = buildKernel(params, context, args.time, args.view, args.mappedScale, &kernel, &empty, &kernelImage);
        if (status != eStatusOK) {
            return status;
        }
    }
    if (params.shape.shape == BokehShape::Image) {
        setKernelWarning(empty ? kernelEmptyMessage() : std::string());
        if (!kernelImage || empty) {
            return runPassesCpu(*this, plan, args, PassList());
        }
    }
    if (!params.lens.isActive()) {
        if (!kernel || kernel->isIdentity()) {
            return runPassesCpu(*this, plan, args, PassList());
        }

        return runPassesCpu(*this, plan, args, passes(params, context, kernel, args.roi));
    }

    // The lens aberrations: a kernel per channel group and cell.
    NodeMetadataConstPtr metadata = getRenderMetadata(args.time, args.view);
    const RectI format = metadata ? metadata->getOutputFormat() : RectI();
    const OfxPointD rs = args.mappedScale.toOfxPointD();
    const LensAberration lens(params.lens, format, context.pixelAspectRatio, rs.x, rs.y);
    std::vector<std::pair<double, double>> groupRadii(lens.groupCount());
    for (int g = 0; g < lens.groupCount(); ++g) {
        radii(params, context, lens.groupScale(g), &groupRadii[g].first, &groupRadii[g].second);
    }
    std::function<BokehRunCache::Runs(int, const BokehClip&)> build;
    if (params.shape.shape == BokehShape::Image) {
        build = [kernelImage, groupRadii](int group, const BokehClip& clip) -> BokehRunCache::Runs {
            const DefocusKernelImage& image = *kernelImage;
            const BokehKernel k = BokehKernel::fromImage(image.values.data(), image.width, image.height, groupRadii[group].first, groupRadii[group].second, nullptr, clip);

            return std::make_shared<const std::vector<BokehRun>>(k.runs());
        };
    } else {
        const std::shared_ptr<BokehRunCache> cache = _runCache;
        const BokehShapeParams shape = params.shape;
        build = [cache, shape, groupRadii](int group, const BokehClip& clip) -> BokehRunCache::Runs {
            BokehShapeParams clipped = shape;
            clipped.clip = clip;

            return cache->runs(clipped, groupRadii[group].first, groupRadii[group].second);
        };
    }

    return runPassesCpu(*this, plan, args, lensPasses(params, context, lens, build, args.roi));
}

void
Defocus::getRegionsOfInterest(double time,
                              const RenderScale& scale,
                              const RectD& outputRoD,
                              const RectD& renderWindow,
                              ViewIdx view,
                              RoIMap* ret)
{
    StagedImageEffect::getRegionsOfInterest(time, scale, outputRoD, renderWindow, view, ret);

    EffectInstancePtr input = getInput(kDefocusInputKernel);
    if (!input || !isImageKernel(time, view)) {
        return;
    }
    // The whole Kernel image, whatever the window.
    RectD rod;
    bool isProjectFormat = false;
    if (input->getRegionOfDefinition_public(input->getRenderHash(), time, scale, view, &rod, &isProjectFormat) != eStatusOK) {
        return;
    }
    // The Kernel may be the same effect as the source or the mask, which then needs both regions.
    RoIMap::iterator found = ret->find(input);
    if (found != ret->end()) {
        found->second.merge(rod);
    } else {
        ret->insert(std::make_pair(input, rod));
    }
}

void
Defocus::onEffectCreated(bool /*mayCreateFileDialog*/,
                         const CreateNodeArgs& /*args*/)
{
    refreshKernelWarning();
}

void
Defocus::onInputChanged(int /*inputNo*/)
{
    refreshKernelWarning();
}

bool
Defocus::knobChanged(KnobI* k,
                     ValueChangedReasonEnum /*reason*/,
                     ViewSpec /*view*/,
                     double /*time*/,
                     bool /*originatedFromMainThread*/)
{
    KnobChoicePtr type = _kernelType.lock();
    if (!type || (k != type.get())) {
        return false;
    }
    refreshKernelWarning();

    return true;
}

void
Defocus::setKernelWarning(const std::string& message)
{
    const NodePtr node = getNode();

    if (!node) {
        return;
    }
    QString current;
    int type = 0;
    node->getPersistentMessage(&current, &type, false);
    if (!message.empty()) {
        if (current != QString::fromUtf8(message.c_str())) {
            node->setPersistentMessage(eMessageTypeWarning, message);
        }

        return;
    }
    // Only our own warnings are taken back: anything posted since has replaced them.
    if ((current == QString::fromUtf8(kernelNotConnectedMessage().c_str())) || (current == QString::fromUtf8(kernelEmptyMessage().c_str()))) {
        node->clearPersistentMessage(false);
    }
}

void
Defocus::refreshKernelWarning()
{
    KnobChoicePtr type = _kernelType.lock();
    const bool image = type && (shapeOf(type->getValue()) == BokehShape::Image);

    setKernelWarning((image && !getInput(kDefocusInputKernel)) ? kernelNotConnectedMessage() : std::string());
}

NATRON_NAMESPACE_EXIT
