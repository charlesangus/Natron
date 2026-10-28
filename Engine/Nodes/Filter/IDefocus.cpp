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

#include "IDefocus.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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
constexpr InputSpec kIDefocusInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
    InputSpec { .label = "Kernel", .role = InputRole::Aux, .optional = true },
    kMapInputSpec,
};

constexpr NodeSpec kIDefocusNodeSpec = {
    .id = PLUGINID_NATRON_IDEFOCUS,
    .label = "IDefocus",
    .description = "Defocus the input stream by a size that varies per pixel with the Map input, as Defocus does at one size: every pixel spreads into a "
                   "bokeh of its own size, a disc, an aperture of straight or curved blades, or the image connected to the Kernel input.\n"
                   "\n"
                   "Where the Map is white the bokeh has the Size, where it is black the Size At Black (0 by default, which leaves those pixels unchanged "
                   "unless a larger neighbour spreads over them), and in between a size in between. Map Channel picks the Map's channel in any of its layers, "
                   "Invert Map inverts it, Map Gamma raises it to 1/gamma and Map Response picks a Linear or Smooth (smoothstep) curve from Size At Black to "
                   "Size. Outside the Map image, the Map reads black, before Invert Map and the remap. With no Map connected, the bokeh has the Size everywhere.\n"
                   "\n"
                   "Each output pixel sums the bokeh of every pixel that reaches it and divides by the sum of their weights, so a sharp pixel beside an "
                   "out-of-focus highlight receives the highlight's bloom, where IBlur would leave it sharp.\n"
                   "\n"
                   "Highlight Gain pushes the values above the Highlight Threshold away from it before the defocus and pulls them back after it, so that bright "
                   "out-of-focus highlights bloom into the bokeh's shape while the rest of the image keeps its brightness.\n"
                   "\n"
                   "The defocus is black outside the image, and the Mask and Mix limit the result as usual.",
    .grouping = PLUGIN_GROUP_FILTER,
    .majorVersion = PLUGIN_MAJOR_NATRON_IDEFOCUS,
    .icon = NATRON_IMAGES_PATH "NativeNodes/IDefocus.png",
    .inputs = kIDefocusInputs,
};

constexpr ImagePipelineSpec kIDefocusPipeline = {
    .stages = { Stage::MaskMix, Stage::PassThroughUnprocessed },
    .layers = LayerPolicy::all(),
    .op = OpKind::Passes,
};

const double kMaxSize = 500.;
const int kKernelTypeDisc = 0;
const int kKernelTypeBlades = 1;
const int kKernelTypeImage = 2;
// A Kernel image larger than this many pixels is read over its image only.
const std::size_t kMaxKernelImagePixels = (std::size_t)1 << 24;
// Levels per pixel of radius between the Size At Black and the Size, and their most.
const double kLevelsPerPixel = 4.;
const int kMaxLevels = 1024;

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

/// The Image kernel's values, row 0 at the bottom.
struct KernelImage {
    std::vector<float> values;
    int width = 0;
    int height = 0;
};

/// The bokeh of params at context's scale; an Image kernel without values is the single tap.
BokehKernel
buildBokeh(const DefocusParams& params,
           const PassContext& context,
           const KernelImage* image)
{
    double rx = 0.;
    double ry = 0.;
    Defocus::radii(params, context, &rx, &ry);
    if (params.shape.shape == BokehShape::Image) {
        if (!image || image->values.empty()) {
            return BokehKernel();
        }

        return BokehKernel::fromImage(image->values.data(), image->width, image->height, rx, ry);
    }

    return BokehKernel::shaped(params.shape, rx, ry);
}

/// The halo of the kernel of params at context's scale, 0 for the single tap.
void
haloOf(const DefocusParams& params,
       const PassContext& context,
       int* haloX,
       int* haloY,
       double* radius)
{
    double rx = 0.;
    double ry = 0.;
    Defocus::radii(params, context, &rx, &ry);
    if (BokehKernel::isIdentityRadius(rx, ry)) {
        *haloX = 0;
        *haloY = 0;
        *radius = 0.;

        return;
    }
    *haloX = BokehKernel::extent(rx);
    *haloY = BokehKernel::extent(ry);
    *radius = std::max(rx, ry);
}

/// The largest halo over levels lo .. hi: the size is linear in the level, so it is at one end.
void
levelRangeHalo(const IDefocusParams& params,
               const PassContext& context,
               int lo,
               int hi,
               int count,
               int* haloX,
               int* haloY,
               double* radius)
{
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    double r0 = 0., r1 = 0.;
    haloOf(IDefocus::levelParams(params, lo, count), context, &x0, &y0, &r0);
    haloOf(IDefocus::levelParams(params, hi, count), context, &x1, &y1, &r1);
    *haloX = std::max(x0, x1);
    *haloY = std::max(y0, y1);
    *radius = std::max(r0, r1);
}

/// One level's kernel as the CPU gathers it: its runs, row dy's from rowStart[dy + haloY].
struct LevelKernel {
    int haloX = 0;
    int haloY = 0;
    bool identity = true;
    std::vector<BokehRun> runs;
    std::vector<int> rowStart;
};

LevelKernel
makeLevelKernel(const BokehKernel& kernel)
{
    LevelKernel out;

    out.haloX = kernel.haloX();
    out.haloY = kernel.haloY();
    out.identity = kernel.isIdentity();
    out.runs = kernel.runs();
    out.rowStart.assign((std::size_t)(2 * out.haloY + 2), 0);
    std::size_t i = 0;
    for (int row = 0; row <= 2 * out.haloY; ++row) {
        out.rowStart[(std::size_t)row] = (int)i;
        while ((i < out.runs.size()) && (out.runs[i].dy == row - out.haloY)) {
            ++i;
        }
    }
    out.rowStart[(std::size_t)(2 * out.haloY + 1)] = (int)i;

    return out;
}

/// What makes the levels: the knobs that shape them, the scale and the Image kernel's values.
struct LevelKey {
    BokehShapeParams shape;
    double aspectRatio = 1.;
    double size = 0.;
    double sizeAtBlack = 0.;
    double scaleX = 1.;
    double scaleY = 1.;
    double pixelAspectRatio = 1.;
    int count = 0;
    std::shared_ptr<const KernelImage> image;

    bool operator==(const LevelKey& o) const
    {
        const bool sameImage = (!image && !o.image)
            || (image && o.image && (image->width == o.image->width) && (image->height == o.image->height) && (image->values == o.image->values));

        return (shape.shape == o.shape.shape) && (shape.blades == o.shape.blades) && (shape.rotation == o.shape.rotation)
            && (shape.roundness == o.shape.roundness) && (aspectRatio == o.aspectRatio) && (size == o.size) && (sizeAtBlack == o.sizeAtBlack)
            && (scaleX == o.scaleX) && (scaleY == o.scaleY) && (pixelAspectRatio == o.pixelAspectRatio) && (count == o.count) && sameImage;
    }
};

LevelKey
makeLevelKey(const IDefocusParams& params,
             const PassContext& context,
             int count,
             const std::shared_ptr<const KernelImage>& image)
{
    LevelKey key;
    const OfxPointD rs = context.scale.toOfxPointD();

    key.shape = params.defocus.shape;
    key.aspectRatio = params.defocus.aspectRatio;
    key.size = params.defocus.size;
    key.sizeAtBlack = params.map.sizeAtBlack[0];
    key.scaleX = rs.x;
    key.scaleY = rs.y;
    key.pixelAspectRatio = context.pixelAspectRatio;
    key.count = count;
    if (params.defocus.shape.shape == BokehShape::Image) {
        key.image = image;
    }

    return key;
}

/**
 * @brief The levels built so far for one key, each on first use, and the GPU's table of every
 * level. A new key drops them; renders already holding levels keep theirs.
 **/
struct LevelStore {
    std::mutex mutex;
    bool valid = false;
    LevelKey key;
    std::vector<std::shared_ptr<const LevelKernel>> levels;
    std::shared_ptr<const std::vector<uint32_t>> gpuTable;

    // Under mutex.
    void select(const LevelKey& k)
    {
        if (valid && (key == k)) {
            return;
        }
        key = k;
        valid = true;
        levels.assign((std::size_t)k.count + 1, std::shared_ptr<const LevelKernel>());
        gpuTable.reset();
    }
};

/**
 * @brief What the passes of one render share: each level's kernel, the level of every pixel over
 * gridRect, the render window grown by the halo, and each grid row's largest vertical halo.
 **/
struct IDefocusState {
    std::vector<std::shared_ptr<const LevelKernel>> kernels;
    std::vector<uint16_t> grid;
    RectI gridRect;
    std::vector<int> rowHaloY;
    int haloX = 0;
    int haloY = 0;
    BokehHighlight highlight;
    RectI window;

    const LevelKernel& levelAt(int x, int y) const
    {
        const std::size_t i = (std::size_t)(y - gridRect.y1) * (std::size_t)gridRect.width() + (std::size_t)(x - gridRect.x1);

        return *kernels[grid[i]];
    }
};

using IDefocusStatePtr = std::shared_ptr<IDefocusState>;
using Worker = std::function<PassRowFn(const PassScratch&)>;

/// Slot 1, every pixel of every plane: slot 0 with its highlights boosted.
Worker
boostWorker(const IDefocusStatePtr& state)
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
            for (int x = 0; x < width; ++x) {
                dst[x] = highlight.boost(src[x]);
            }
        };
    };
}

/// One thread's sums along an output row: weights, contributions and values, as differences.
struct GatherRow {
    std::vector<double> weights;
    std::vector<int> counts;
    std::vector<double> sums;
    std::vector<double> acc;
    std::vector<float> values;
};

/**
 * @brief Slot `to` over the render window, a row of every plane per line: each source pixel of
 * slot `from` within the halo adds its kernel's row at the line's offset, weighted, to the row's
 * sums, which are then divided by the weights. A source outside the scratch rect is black but
 * still adds its weight, as Defocus's fixed weights count the black outside the image. A pixel
 * nothing reaches, or only itself with the single tap, keeps slot 0's value.
 **/
Worker
gatherWorker(const IDefocusStatePtr& state,
             int from,
             int to)
{
    return [state, from, to](const PassScratch& scratch) -> PassRowFn {
        const IDefocusState* s = state.get();
        const RectI rect = scratch.rect;
        const int wx1 = std::max(s->window.x1, rect.x1);
        const int wx2 = std::max(wx1, std::min(s->window.x2, rect.x2));
        const int length = wx2 - wx1;
        const int nPlanes = scratch.nPlanes;
        std::shared_ptr<GatherRow> row = std::make_shared<GatherRow>();
        row->weights.resize((std::size_t)length + 1);
        row->counts.resize((std::size_t)length + 1);
        row->sums.resize(((std::size_t)length + 1) * (std::size_t)nPlanes);
        row->acc.resize((std::size_t)nPlanes);
        row->values.resize((std::size_t)nPlanes);
        const PassScratch* buffers = &scratch;

        return [s, buffers, rect, wx1, wx2, length, nPlanes, row, from, to](int line) {
            const int y = rect.y1 + line;
            const int width = buffers->width;
            // The slot is written whole: slot 0 outside the window.
            for (int p = 0; p < nPlanes; ++p) {
                const float* const src = buffers->plane(0, p) + (std::size_t)line * width;
                std::copy(src, src + width, buffers->plane(to, p) + (std::size_t)line * width);
            }
            if ((y < s->window.y1) || (y >= s->window.y2) || (length <= 0)) {
                return;
            }
            double* const weights = row->weights.data();
            int* const counts = row->counts.data();
            double* const sums = row->sums.data();
            float* const values = row->values.data();
            std::fill(row->weights.begin(), row->weights.end(), 0.);
            std::fill(row->counts.begin(), row->counts.end(), 0);
            std::fill(row->sums.begin(), row->sums.end(), 0.);

            const RectI& g = s->gridRect;
            const int gridWidth = g.width();
            const int sy1 = std::max(y - s->haloY, g.y1);
            const int sy2 = std::min(y + s->haloY + 1, g.y2);
            const int sx1 = std::max(wx1 - s->haloX, g.x1);
            const int sx2 = std::min(wx2 + s->haloX, g.x2);
            for (int sy = sy1; sy < sy2; ++sy) {
                const int dy = y - sy;
                const int ady = std::abs(dy);
                if (s->rowHaloY[(std::size_t)(sy - g.y1)] < ady) {
                    continue;
                }
                const bool rowInside = (sy >= rect.y1) && (sy < rect.y2);
                const uint16_t* const levels = &s->grid[(std::size_t)(sy - g.y1) * gridWidth];
                for (int sx = sx1; sx < sx2; ++sx) {
                    const LevelKernel& k = *s->kernels[levels[sx - g.x1]];
                    if ((k.haloY < ady) || (sx + k.haloX < wx1) || (sx - k.haloX >= wx2)) {
                        continue;
                    }
                    const bool inside = rowInside && (sx >= rect.x1) && (sx < rect.x2);
                    if (inside) {
                        const std::size_t at = (std::size_t)(sy - rect.y1) * width + (std::size_t)(sx - rect.x1);
                        for (int p = 0; p < nPlanes; ++p) {
                            const float v = buffers->plane(from, p)[at];
                            values[p] = std::isfinite(v) ? v : 0.f;
                        }
                    }
                    const BokehRun* r = k.runs.data() + k.rowStart[(std::size_t)(dy + k.haloY)];
                    const BokehRun* const end = k.runs.data() + k.rowStart[(std::size_t)(dy + k.haloY + 1)];
                    for (; r != end; ++r) {
                        const int a = std::max(0, sx + r->dx0 - wx1);
                        const int b = std::min(length, sx + r->dx0 + r->length - wx1);
                        if (a >= b) {
                            continue;
                        }
                        const double w = r->weight;
                        weights[a] += w;
                        weights[b] -= w;
                        ++counts[a];
                        --counts[b];
                        if (inside) {
                            double* const sa = sums + (std::size_t)a * nPlanes;
                            double* const sb = sums + (std::size_t)b * nPlanes;
                            for (int p = 0; p < nPlanes; ++p) {
                                const double wv = w * values[p];
                                sa[p] += wv;
                                sb[p] -= wv;
                            }
                        }
                    }
                }
            }

            double* const acc = row->acc.data();
            std::fill(row->acc.begin(), row->acc.end(), 0.);
            double weight = 0.;
            int count = 0;
            const bool unboost = s->highlight.isActive();
            const std::size_t rowAt = (std::size_t)(y - rect.y1) * width;
            for (int i = 0; i < length; ++i) {
                weight += weights[i];
                count += counts[i];
                const double* const si = sums + (std::size_t)i * nPlanes;
                for (int p = 0; p < nPlanes; ++p) {
                    acc[p] += si[p];
                }
                const int x = wx1 + i;
                const std::size_t at = rowAt + (std::size_t)(x - rect.x1);
                const bool keep = (count <= 0) || !(weight > 0.) || ((count == 1) && s->levelAt(x, y).identity);
                for (int p = 0; p < nPlanes; ++p) {
                    float out;
                    if (keep) {
                        out = buffers->plane(0, p)[at];
                    } else {
                        out = (float)(acc[p] / weight);
                        if (unboost) {
                            out = s->highlight.unboost(out);
                        }
                    }
                    buffers->plane(to, p)[at] = out;
                }
            }
        };
    };
} // gatherWorker

/**
 * @brief The passes gathering with a halo of haloX x haloY: the highlight boost into slot 1 when
 * it is on, then the gather into the result slot. With no state, the passes have no work and only
 * carry the halo and growth. No pass at all for a zero halo.
 **/
PassList
makePasses(const BokehHighlight& highlight,
           int haloX,
           int haloY,
           double radius,
           const IDefocusStatePtr& state)
{
    PassList list;

    if ((haloX <= 0) && (haloY <= 0)) {
        return list;
    }
    const bool boost = highlight.isActive();
    list.scratchSlots = boost ? 3 : 2;
    list.resultSlot = boost ? 2 : 1;

    if (boost) {
        Pass pass = Pass::point(true, state ? boostWorker(state) : Worker());
        pass.slot = 1;
        list.passes.push_back(pass);
    }
    Pass gather = Pass::gather(haloX, haloY, false, state ? gatherWorker(state, boost ? 1 : 0, list.resultSlot) : Worker());
    gather.slot = list.resultSlot;
    // About two runs per kernel row from every source row within the halo.
    gather.tapsPerSample = (std::size_t)(2 * (2 * haloY + 1));
    gather.sigma = 2. * radius / 2.4;
    list.passes.push_back(gather);

    return list;
}

#ifdef NATRON_HAVE_GPU
// The largest halo, in pixels, the GPU records: every output pixel visits its whole apron, so a
// larger defocus stays on the CPU.
const int kGpuMaxHalo = 64;

// Mirror kHeader, kBoost and kMap* in Kernels/idefocus.slang.
const uint32_t kGpuHeader = 16;
const uint32_t kGpuBoost = 1;
const uint32_t kGpuMapConnected = 1;
const uint32_t kGpuMapRead = 2;
const uint32_t kGpuMapInvert = 4;
const uint32_t kGpuMapSmooth = 8;

// Mirrors IDefocusParams in Kernels/idefocus.slang, which has no padding between members.
struct IDefocusGpuParams {
    int32_t lvX1;
    int32_t lvY1;
    uint32_t lvWidth;
    uint32_t lvHeight;
    uint32_t channels;
    uint32_t flags;
    float threshold;
    float gain;
    int32_t haloX;
    int32_t haloY;
    int32_t outX1;
    int32_t outY1;
    uint32_t outWidth;
    uint32_t outHeight;
    uint32_t dstOffset;
    uint32_t dstStride;
    int32_t srcX1;
    int32_t srcY1;
    int32_t srcX2;
    int32_t srcY2;
    uint32_t srcOffset;
    uint32_t srcStride;
    uint32_t pad0;
    uint32_t pad1;
};
static_assert(sizeof(IDefocusGpuParams) == 96);
static_assert(offsetof(IDefocusGpuParams, haloX) == 32);
static_assert(offsetof(IDefocusGpuParams, srcX1) == 64);

// Mirrors the header of the table in Kernels/idefocus.slang.
struct IDefocusGpuHeader {
    uint32_t mapFlags;
    uint32_t mapChannel;
    uint32_t mapChannels;
    uint32_t mapOffset;
    uint32_t mapStride;
    int32_t mapX1;
    int32_t mapY1;
    int32_t mapX2;
    int32_t mapY2;
    float exponent;
    uint32_t count;
    uint32_t pad[5];
};
static_assert(sizeof(IDefocusGpuHeader) == kGpuHeader * sizeof(uint32_t));

/**
 * @brief The table after the header: levels 0 .. count, three uints each (the index of its
 * weights in the whole table, its haloX and haloY), then each level's weights row by row.
 **/
std::vector<uint32_t>
buildGpuLevels(const IDefocusParams& params,
               const PassContext& context,
               int count)
{
    std::vector<uint32_t> info;
    std::vector<uint32_t> weights;
    const uint32_t base = kGpuHeader + 3u * (uint32_t)(count + 1);

    info.reserve((std::size_t)3 * (count + 1));
    for (int level = 0; level <= count; ++level) {
        const BokehKernel kernel = buildBokeh(IDefocus::levelParams(params, level, count), context, nullptr);
        info.push_back(base + (uint32_t)weights.size());
        info.push_back((uint32_t)kernel.haloX());
        info.push_back((uint32_t)kernel.haloY());
        for (float w : kernel.weights()) {
            weights.push_back(std::bit_cast<uint32_t>(w));
        }
    }
    info.insert(info.end(), weights.begin(), weights.end());

    return info;
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

bool
uploadWords(gpu::GpuRecordContext& ctx,
            gpu::GpuBuffer& buffer,
            std::size_t byteOffset,
            std::span<const std::byte> bytes)
{
    const std::size_t kChunk = 65536;
    for (std::size_t offset = 0; offset < bytes.size(); offset += kChunk) {
        if (!ctx.updateBuffer(buffer, byteOffset + offset, bytes.subspan(offset, std::min(kChunk, bytes.size() - offset)))) {
            return false;
        }
    }

    return true;
}

/**
 * @brief The gather as the GPU records it: the level and the boosted source of every pixel of the
 * output grown by the halo into the work buffer, then every output pixel's gather over its apron
 * into io.filtered. The Map's channel is mapChannel of io.inputs[Map], or none when that is
 * negative. Fails, so that the CPU renders, when a buffer exceeds the kernels' 32-bit indices.
 **/
StatusEnum
recordIDefocusGpu(const PassGpuIO& io,
                  const IDefocusParams& params,
                  const std::vector<uint32_t>& levels,
                  int count,
                  int haloX,
                  int haloY,
                  bool mapConnected,
                  int mapChannel)
{
    gpu::GpuRecordContext& ctx = io.context;
    const int nComps = io.nComps;
    if (PassExec::isEmptyRect(io.output) || (nComps < 1) || (nComps > 4) || !io.filtered.buffer || levels.empty()) {
        return eStatusFailed;
    }
    const uint64_t lvWidth = (uint64_t)io.output.width() + 2 * (uint64_t)haloX;
    const uint64_t lvHeight = (uint64_t)io.output.height() + 2 * (uint64_t)haloY;
    const uint64_t workFloats = lvWidth * lvHeight * (uint64_t)(nComps + 1);
    if (workFloats > UINT32_MAX) {
        return eStatusFailed;
    }

    IDefocusGpuHeader header {};
    header.mapFlags = (mapConnected ? kGpuMapConnected : 0u) | (params.map.invert ? kGpuMapInvert : 0u) | (params.map.smooth ? kGpuMapSmooth : 0u);
    header.exponent = params.map.exponent;
    header.count = (uint32_t)count;

    gpu::GpuBuffer* table = nullptr;
    gpu::GpuBuffer* work = nullptr;
    const std::size_t tableBytes = sizeof(header) + levels.size() * sizeof(uint32_t);
    if (!ctx.scratch(tableBytes, table) || !table || !ctx.scratch(workFloats * sizeof(float), work) || !work) {
        return eStatusFailed;
    }

    const gpu::GpuBuffer* source = table;
    const gpu::GpuBuffer* map = table;
    const GpuImageView mapView = (kIDefocusInputMap < (int)io.inputs.size()) ? io.inputs[kIDefocusInputMap] : GpuImageView();
    if (mapConnected && mapView.buffer && (mapChannel >= 0) && (mapChannel < mapView.nComps) && !PassExec::isEmptyRect(mapView.bounds)) {
        const gpu::GpuBlurImage image = PassExec::toGpuBlurImage(mapView);
        if (!gpuFloatIndex(image, mapView.bounds.x1, mapView.bounds.y1, mapView.nComps, &header.mapOffset)) {
            return eStatusFailed;
        }
        header.mapFlags |= kGpuMapRead;
        header.mapChannel = (uint32_t)mapChannel;
        header.mapChannels = (uint32_t)mapView.nComps;
        header.mapStride = image.strideFloats;
        header.mapX1 = mapView.bounds.x1;
        header.mapY1 = mapView.bounds.y1;
        header.mapX2 = mapView.bounds.x2;
        header.mapY2 = mapView.bounds.y2;
        map = mapView.buffer;
    }
    if (!uploadWords(ctx, *table, 0, std::as_bytes(std::span<const IDefocusGpuHeader>(&header, 1)))
        || !uploadWords(ctx, *table, sizeof(header), std::as_bytes(std::span<const uint32_t>(levels)))) {
        return eStatusFailed;
    }

    IDefocusGpuParams p {};
    p.lvX1 = io.output.x1 - haloX;
    p.lvY1 = io.output.y1 - haloY;
    p.lvWidth = (uint32_t)lvWidth;
    p.lvHeight = (uint32_t)lvHeight;
    p.channels = (uint32_t)nComps;
    p.flags = params.defocus.highlight.isActive() ? kGpuBoost : 0u;
    p.threshold = params.defocus.highlight.threshold;
    p.gain = params.defocus.highlight.gain;
    p.haloX = haloX;
    p.haloY = haloY;
    p.outX1 = io.output.x1;
    p.outY1 = io.output.y1;
    p.outWidth = (uint32_t)io.output.width();
    p.outHeight = (uint32_t)io.output.height();
    const gpu::GpuBlurImage filtered = PassExec::toGpuBlurImage(io.filtered);
    p.dstStride = filtered.strideFloats;
    if (!gpuFloatIndex(filtered, io.output.x1, io.output.y1, nComps, &p.dstOffset)) {
        return eStatusFailed;
    }
    if (io.source.buffer && !PassExec::isEmptyRect(io.sourceValid)) {
        const gpu::GpuBlurImage image = PassExec::toGpuBlurImage(io.source);
        p.srcX1 = io.sourceValid.x1;
        p.srcY1 = io.sourceValid.y1;
        p.srcX2 = io.sourceValid.x2;
        p.srcY2 = io.sourceValid.y2;
        p.srcStride = image.strideFloats;
        if (!gpuFloatIndex(image, io.sourceValid.x1, io.sourceValid.y1, nComps, &p.srcOffset)) {
            return eStatusFailed;
        }
        source = io.source.buffer;
    }

    if (!ctx.dispatch(gpu::GpuKernelId::idefocus_load, { source, map, table, work, io.filtered.buffer }, p, { p.lvWidth, p.lvHeight, 1 })) {
        return eStatusFailed;
    }
    ctx.computeBarrier();
    if (!ctx.dispatch(gpu::GpuKernelId::idefocus_gather, { source, map, table, work, io.filtered.buffer }, p, { p.outWidth, p.outHeight, 1 })) {
        return eStatusFailed;
    }

    return eStatusOK;
} // recordIDefocusGpu
#endif // NATRON_HAVE_GPU
} // anonymous namespace

struct IDefocus::LevelCache {
    LevelStore store;
};

IDefocus::IDefocus(NodePtr node)
    : StagedImageEffect(node)
    , _map()
    , _size()
    , _aspectRatio()
    , _kernelType()
    , _blades()
    , _rotation()
    , _roundness()
    , _kernelChannel()
    , _highlightThreshold()
    , _highlightGain()
    , _levelCache(new LevelCache)
{
}

IDefocus::~IDefocus()
{
}

const NodeSpec*
IDefocus::nodeSpec() const
{
    return staticNodeSpec<kIDefocusNodeSpec>();
}

const ImagePipelineSpec&
IDefocus::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kIDefocusPipeline, kIDefocusNodeSpec>();
}

std::string
IDefocus::getInputHint(int inputNb) const
{
    switch (inputNb) {
    case kIDefocusInputSource:
        return tr("The image to defocus.").toStdString();
    case kIDefocusInputMask:
        return tr("The mask the defocused result is mixed with the source through.").toStdString();
    case kIDefocusInputKernel:
        return tr("The Kernel: with the Image kernel type, the bokeh's shape, its Kernel Channel over its region of definition.").toStdString();
    case kIDefocusInputMap:
        return tr("The Map: the bokeh has the Size where it is white and the Size At Black where it is black.").toStdString();
    }

    return std::string();
}

void
IDefocus::addAcceptedComponents(int inputNb,
                                std::list<ImageLayerDesc>* comps)
{
    StagedImageEffect::addAcceptedComponents(inputNb, comps);
    if (inputNb == kIDefocusInputMap) {
        comps->push_back(ImageLayerDesc::getXYComponents());
    }
}

void
IDefocus::initializeKnobs()
{
    const KnobPagePtr page = createControlsPage();

    _map.createSizeAtBlackKnob(this, page, 1);
    if (KnobDoublePtr sizeAtBlack = std::dynamic_pointer_cast<KnobDouble>(getKnobByName(kMapInputParamSizeAtBlack))) {
        sizeAtBlack->setMaximum(kMaxSize);
    }

    KnobDoublePtr sizeKnob = AppManager::createKnob<KnobDouble>(this, tr("Size"));
    sizeKnob->setName(kDefocusParamSize);
    sizeKnob->setHintToolTip(tr("Diameter of the bokeh where the Map is white, in pixel units (>=0). Below one pixel a pixel does not spread."));
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
    aspectKnob->setHintToolTip(tr("Width of the bokeh over its height: the width is the size times the aspect ratio, the height the size."));
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
        options.push_back(ChoiceOption(kDefocusParamKernelTypeImage, tr("Image").toStdString(), tr("The Kernel input's Kernel Channel, stretched over each pixel's size.").toStdString()));
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

    KnobChannelSelectPtr channelKnob = AppManager::createKnob<KnobChannelSelect>(this, tr("Kernel Channel"));
    channelKnob->setName(kDefocusParamKernelChannel);
    channelKnob->setAnimationEnabled(false);
    channelKnob->setDefaultValue(channelKnob->encode(std::string(kNatronColorViewRGBA) + ".A"));
    channelKnob->setHintToolTip(tr("Image kernel only: the channel of the Kernel input, in any of its layers, that holds the bokeh. A channel the input does not have, or None, reads 0."));
    page->addKnob(channelKnob);
    _kernelChannel = channelKnob;

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

    _map.createKnobs(this, page);

    addStageKnobs(page);

    getNode()->declareLayerKnob(channelKnob, kIDefocusInputKernel, LayerKnobSpec::eRoleInputBound);
    _map.declareLayerKnob(getNode(), kIDefocusInputMap);
} // IDefocus::initializeKnobs

IDefocusParams
IDefocus::getParams(double time,
                    ViewIdx view) const
{
    const KnobReader reader(time, view);
    IDefocusParams params;
    DefocusParams& d = params.defocus;
    const double size = reader.value(_size);
    const double aspect = reader.value(_aspectRatio);

    d.size = std::isfinite(size) ? std::min(kMaxSize, std::max(0., size)) : 0.;
    d.aspectRatio = std::isfinite(aspect) ? std::min(10., std::max(0.1, aspect)) : 1.;
    d.shape.shape = shapeOf(reader.value(_kernelType));
    d.shape.blades = std::max(3, (int)reader.value(_blades));
    d.shape.rotation = reader.value(_rotation);
    d.shape.roundness = reader.value(_roundness);
    d.highlight.threshold = (float)std::max(0., reader.value(_highlightThreshold));
    d.highlight.gain = (float)std::max(1., reader.value(_highlightGain));
    if (!std::isfinite(d.highlight.threshold) || !std::isfinite(d.highlight.gain)) {
        d.highlight = BokehHighlight();
    }
    params.map = _map.bake(reader);
    params.map.sizeAtBlack[0] = std::min(kMaxSize, params.map.sizeAtBlack[0]);
    params.map.sizeAtBlack[1] = params.map.sizeAtBlack[0];

    return params;
}

int
IDefocus::levelCount(const IDefocusParams& params,
                     const PassContext& context)
{
    DefocusParams black = params.defocus;
    black.size = params.map.sizeAtBlack[0];
    double bx = 0., by = 0., wx = 0., wy = 0.;
    Defocus::radii(black, context, &bx, &by);
    Defocus::radii(params.defocus, context, &wx, &wy);
    const double steps = std::ceil(std::max(std::fabs(wx - bx), std::fabs(wy - by)) * kLevelsPerPixel);

    if (!(steps >= 1.)) {
        return 1;
    }

    return (int)std::min((double)kMaxLevels, steps);
}

int
IDefocus::levelOf(float m,
                  int count)
{
    // Two roundings, as Kernels/idefocus.slang does them.
    const float scaled = m * (float)count;
    const float rounded = scaled + 0.5f;

    if (!(rounded > 0.f)) {
        return 0;
    }

    return std::min(count, (int)rounded);
}

DefocusParams
IDefocus::levelParams(const IDefocusParams& params,
                      int level,
                      int count)
{
    DefocusParams out = params.defocus;
    const float m = (float)level / (float)std::max(1, count);
    const double size = params.map.sizeAt(m, 0, params.defocus.size);

    out.size = std::isfinite(size) ? std::min(kMaxSize, std::max(0., size)) : 0.;

    return out;
}

std::string
IDefocus::mapNotConnectedMessage()
{
    return tr("The Map input is not connected: the bokeh has the Size everywhere.").toStdString();
}

bool
IDefocus::isImageKernel(double time,
                        ViewIdx view) const
{
    const KnobReader reader(time, view);

    return shapeOf(reader.value(_kernelType)) == BokehShape::Image;
}

bool
IDefocus::resolveKernelChannel(double time,
                               ViewIdx view,
                               ImageLayerDesc* layer,
                               int* channel) const
{
    KnobChannelSelectPtr knob = _kernelChannel.lock();
    if (!knob || knob->isNone() || !getInput(kIDefocusInputKernel)) {
        return false;
    }
    std::list<ImageLayerDesc> present;
    const_cast<IDefocus*>(this)->getPresentLayers(time, view, kIDefocusInputKernel, &present);

    return knob->resolve(present, layer, channel) && (*channel >= 0);
}

PassList
IDefocus::opPasses(double time,
                   ViewIdx view,
                   const RenderScale& scale) const
{
    const IDefocusParams params = getParams(time, view);
    const bool image = (params.defocus.shape.shape == BokehShape::Image);
    if (image && !getInput(kIDefocusInputKernel)) {
        return PassList();
    }
    const PassContext context = getPassContext(time, view, scale);
    const int count = levelCount(params, context);
    // A disconnected Map reads 1 everywhere; a connected one may take any level.
    const int lo = getInput(kIDefocusInputMap) ? 0 : count;
    int haloX = 0, haloY = 0;
    double radius = 0.;
    levelRangeHalo(params, context, lo, count, count, &haloX, &haloY, &radius);
    PassList list = makePasses(params.defocus.highlight, haloX, haloY, radius, IDefocusStatePtr());

#ifdef NATRON_HAVE_GPU
    if (!list.empty() && !image && (haloX <= kGpuMaxHalo) && (haloY <= kGpuMaxHalo)) {
        const IDefocus* self = this;
        list.gpu = [self, time, view, params, context](const PassGpuIO& io) -> StatusEnum {
            return self->recordGpu(io, time, view, params, context);
        };
    }
#endif

    return list;
}

StatusEnum
IDefocus::recordGpu(const PassGpuIO& io,
                    double time,
                    ViewIdx view,
                    const IDefocusParams& params,
                    const PassContext& context) const
{
#ifdef NATRON_HAVE_GPU
    if (params.defocus.shape.shape == BokehShape::Image) {
        return eStatusFailed;
    }
    const bool connected = (bool)getInput(kIDefocusInputMap);
    ImageLayerDesc layer;
    int channel = -1;
    if (!connected || !_map.resolveChannel(const_cast<IDefocus*>(this), kIDefocusInputMap, time, view, &layer, &channel)) {
        channel = -1;
    }
    const int count = levelCount(params, context);
    int haloX = 0, haloY = 0;
    double radius = 0.;
    levelRangeHalo(params, context, connected ? 0 : count, count, count, &haloX, &haloY, &radius);
    if ((haloX > kGpuMaxHalo) || (haloY > kGpuMaxHalo)) {
        return eStatusFailed;
    }

    std::shared_ptr<const std::vector<uint32_t>> levels;
    {
        LevelStore& store = _levelCache->store;
        const std::lock_guard<std::mutex> lock(store.mutex);
        store.select(makeLevelKey(params, context, count, std::shared_ptr<const KernelImage>()));
        if (!store.gpuTable) {
            store.gpuTable = std::make_shared<const std::vector<uint32_t>>(buildGpuLevels(params, context, count));
        }
        levels = store.gpuTable;
    }

    return recordIDefocusGpu(io, params, *levels, count, haloX, haloY, connected, channel);
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
IDefocus::readsGpuInput(int inputNb,
                        const ImageLayerDesc& /*plane*/,
                        double time,
                        ViewIdx view) const
{
    if (inputNb == kIDefocusInputKernel) {
        return false;
    }
    if (inputNb != kIDefocusInputMap) {
        return true;
    }
    ImageLayerDesc layer;
    int channel = -1;

    return _map.resolveChannel(const_cast<IDefocus*>(this), kIDefocusInputMap, time, view, &layer, &channel);
}

bool
IDefocus::opPassesInputLayer(int inputNb,
                             const ImageLayerDesc& plane,
                             double time,
                             ViewIdx view,
                             ImageLayerDesc* layer) const
{
    if (inputNb != kIDefocusInputMap) {
        *layer = plane;

        return true;
    }
    int channel = -1;

    return _map.resolveChannel(const_cast<IDefocus*>(this), kIDefocusInputMap, time, view, layer, &channel);
}

RectI
IDefocus::opPassesInputRoI(int inputNb,
                           const RectI& outputRect,
                           const RenderScale& scale) const
{
    if (inputNb != kIDefocusInputMap) {
        return outputRect;
    }

    return passListSourceRoI(outputRect, opPasses(getCurrentTime(), getCurrentView(), scale));
}

std::optional<GpuCostHint>
IDefocus::gpuCostHint(double time,
                      ViewIdx view,
                      unsigned int mipmapLevel) const
{
    return passListCostHint(opPasses(time, view, RenderScale::fromMipmapLevel(mipmapLevel)));
}

bool
IDefocus::getInputLayersRead(int inputNb,
                             double time,
                             ViewIdx view,
                             std::list<ImageLayerDesc>* layers)
{
    if (inputNb == kIDefocusInputMap) {
        return _map.getInputLayersRead(this, kIDefocusInputMap, time, view, layers);
    }
    if ((inputNb != kIDefocusInputKernel) || !isImageKernel(time, view)) {
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
IDefocus::isOpIdentity(double time,
                       const RenderScale& scale,
                       const RectI& /*roi*/,
                       ViewIdx view)
{
    return opPasses(time, view, scale).empty();
}

StatusEnum
IDefocus::readKernelImage(double time,
                          ViewIdx view,
                          const RenderScale& scale,
                          std::vector<float>* values,
                          int* width,
                          int* height,
                          bool* empty)
{
    values->clear();
    *width = 0;
    *height = 0;
    *empty = false;

    EffectInstancePtr input = getInput(kIDefocusInputKernel);
    ImageLayerDesc layer;
    int channel = -1;
    ImagePtr image;
    if (input && resolveKernelChannel(time, view, &layer, &channel)) {
        RectI roi;
        // A selected layer is read as it is stored, not in the clip's colour layout.
        image = getImage(kIDefocusInputKernel, time, scale, view, NULL, &layer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &roi);
        if (image && !PassExec::isFloatImage(image)) {
            return eStatusFailed;
        }
    }
    if (!image || (channel >= (int)image->getComponentsCount())) {
        *empty = (bool)input;

        return eStatusOK;
    }
    // The kernel is the input's region of definition, which getRegionsOfInterest() asked for.
    const RectI bounds = image->getBounds();
    RectI frame = bounds;
    RectD rodD;
    bool isProjectFormat = false;
    if (input->getRegionOfDefinition_public(input->getRenderHash(), time, scale, view, &rodD, &isProjectFormat) == eStatusOK) {
        const double par = getRenderMetadata(time, view)->getPixelAspectRatio(kIDefocusInputKernel);
        const RectI rodPixel = PassExec::toPixelEnclosing(rodD, scale, par);
        if (!PassExec::isEmptyRect(rodPixel) && ((std::size_t)rodPixel.width() * (std::size_t)rodPixel.height() <= kMaxKernelImagePixels)) {
            frame = rodPixel;
        }
    }
    if (PassExec::isEmptyRect(frame) || ((std::size_t)frame.width() * (std::size_t)frame.height() > kMaxKernelImagePixels)) {
        *empty = true;

        return eStatusOK;
    }
    const int w = frame.width();
    const int h = frame.height();
    const int nComps = (int)image->getComponentsCount();
    values->resize((std::size_t)w * h);
    Image::ReadAccess access(image.get());
    for (int y = 0; y < h; ++y) {
        PassExec::readChannelRow(image.get(), &access, bounds, nComps, channel, frame.x1, frame.y1 + y, w, 0.f, &(*values)[(std::size_t)y * w]);
    }
    // The same test fromImage() makes: an image with nothing positive gives the single tap.
    bool any = false;
    for (float v : *values) {
        if (std::isfinite(v) && (v > 0.f)) {
            any = true;
            break;
        }
    }
    if (!any) {
        values->clear();
        *empty = true;

        return eStatusOK;
    }
    *width = w;
    *height = h;

    return eStatusOK;
} // IDefocus::readKernelImage

StatusEnum
IDefocus::renderOp(const RenderActionArgs& args,
                   const StagePlan& plan)
{
    const PassList full = opPasses(args.time, args.view, args.mappedScale);

    if (full.empty()) {
        return runPassesCpu(*this, plan, args, full);
    }
    const IDefocusParams params = getParams(args.time, args.view);
    const PassContext context = getPassContext(args.time, args.view, args.mappedScale);

    std::shared_ptr<KernelImage> image;
    if (params.defocus.shape.shape == BokehShape::Image) {
        image = std::make_shared<KernelImage>();
        bool empty = false;
        const StatusEnum status = readKernelImage(args.time, args.view, args.mappedScale, &image->values, &image->width, &image->height, &empty);
        if (status != eStatusOK) {
            return status;
        }
        setWarning(empty ? Defocus::kernelEmptyMessage() : idleWarning());
        if (image->values.empty()) {
            return runPassesCpu(*this, plan, args, PassList());
        }
    }

    // The Map over every pixel the passes may read; the tile's sources reach no further, their
    // halo being at most the full one.
    const RectI window = passListSourceRoI(args.roi, full);
    std::shared_ptr<MapPlane> map = std::make_shared<MapPlane>();
    StatusEnum status = _map.read(this, kIDefocusInputMap, params.map, args.time, args.view, args.mappedScale, window, map.get());
    if (status != eStatusOK) {
        return status;
    }
    const MapRange range = map->tileRange(window);
    const int count = levelCount(params, context);
    int haloX = 0, haloY = 0;
    double radius = 0.;
    levelRangeHalo(params, context, levelOf(range.min, count), levelOf(range.max, count), count, &haloX, &haloY, &radius);
    if ((haloX <= 0) && (haloY <= 0)) {
        // Every source within reach has the single tap.
        return runPassesCpu(*this, plan, args, PassList());
    }

    IDefocusStatePtr state = std::make_shared<IDefocusState>();
    state->haloX = haloX;
    state->haloY = haloY;
    state->highlight = params.defocus.highlight;
    state->window = args.roi;
    state->gridRect = RectI(args.roi.x1 - haloX, args.roi.y1 - haloY, args.roi.x2 + haloX, args.roi.y2 + haloY);
    const RectI& g = state->gridRect;
    const int gridWidth = g.width();
    const int gridHeight = g.height();
    state->grid.resize((std::size_t)gridWidth * gridHeight);
    std::vector<char> used((std::size_t)count + 1, 0);
    for (int y = 0; y < gridHeight; ++y) {
        uint16_t* const row = &state->grid[(std::size_t)y * gridWidth];
        for (int x = 0; x < gridWidth; ++x) {
            const int level = levelOf(map->at(g.x1 + x, g.y1 + y), count);
            row[x] = (uint16_t)level;
            used[(std::size_t)level] = 1;
        }
    }

    state->kernels.assign((std::size_t)count + 1, std::shared_ptr<const LevelKernel>());
    {
        LevelStore& store = _levelCache->store;
        const std::lock_guard<std::mutex> lock(store.mutex);
        store.select(makeLevelKey(params, context, count, image));
        for (int level = 0; level <= count; ++level) {
            if (!used[(std::size_t)level]) {
                continue;
            }
            std::shared_ptr<const LevelKernel>& kernel = store.levels[(std::size_t)level];
            if (!kernel) {
                kernel = std::make_shared<const LevelKernel>(makeLevelKernel(buildBokeh(levelParams(params, level, count), context, image.get())));
            }
            state->kernels[(std::size_t)level] = kernel;
        }
    }
    state->rowHaloY.assign((std::size_t)gridHeight, 0);
    for (int y = 0; y < gridHeight; ++y) {
        const uint16_t* const row = &state->grid[(std::size_t)y * gridWidth];
        int most = 0;
        for (int x = 0; x < gridWidth; ++x) {
            most = std::max(most, state->kernels[row[x]]->haloY);
        }
        state->rowHaloY[(std::size_t)y] = most;
    }

    return runPassesCpu(*this, plan, args, makePasses(params.defocus.highlight, haloX, haloY, radius, state));
} // IDefocus::renderOp

void
IDefocus::getRegionsOfInterest(double time,
                               const RenderScale& scale,
                               const RectD& outputRoD,
                               const RectD& renderWindow,
                               ViewIdx view,
                               RoIMap* ret)
{
    StagedImageEffect::getRegionsOfInterest(time, scale, outputRoD, renderWindow, view, ret);

    // Either extra input may be the same effect as the source, the mask or the other, which then
    // needs both regions.
    const auto add = [ret](const EffectInstancePtr& input, const RectD& roi) {
        RoIMap::iterator found = ret->find(input);
        if (found != ret->end()) {
            found->second.merge(roi);
        } else {
            ret->insert(std::make_pair(input, roi));
        }
    };

    EffectInstancePtr kernel = getInput(kIDefocusInputKernel);
    if (kernel && isImageKernel(time, view)) {
        // The whole Kernel image, whatever the window.
        RectD rod;
        bool isProjectFormat = false;
        if (kernel->getRegionOfDefinition_public(kernel->getRenderHash(), time, scale, view, &rod, &isProjectFormat) == eStatusOK) {
            add(kernel, rod);
        }
    }

    EffectInstancePtr map = getInput(kIDefocusInputMap);
    if (map) {
        // The Map is read over the render window grown by the full halo, in the source's pixels.
        const double par = getRenderMetadata(time, view)->getPixelAspectRatio(kIDefocusInputSource);
        const RectI rectPixel = PassExec::toPixelEnclosing(renderWindow, scale, par);
        add(map, PassExec::toCanonical(passListSourceRoI(rectPixel, opPasses(time, view, scale)), scale, par));
    }
}

void
IDefocus::onEffectCreated(bool /*mayCreateFileDialog*/,
                          const CreateNodeArgs& /*args*/)
{
    refreshWarning();
}

void
IDefocus::onInputChanged(int /*inputNo*/)
{
    refreshWarning();
}

bool
IDefocus::knobChanged(KnobI* k,
                      ValueChangedReasonEnum /*reason*/,
                      ViewSpec /*view*/,
                      double /*time*/,
                      bool /*originatedFromMainThread*/)
{
    KnobChoicePtr type = _kernelType.lock();
    if (!type || (k != type.get())) {
        return false;
    }
    refreshWarning();

    return true;
}

void
IDefocus::setWarning(const std::string& message)
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
    if ((current == QString::fromUtf8(Defocus::kernelNotConnectedMessage().c_str())) || (current == QString::fromUtf8(Defocus::kernelEmptyMessage().c_str()))
        || (current == QString::fromUtf8(mapNotConnectedMessage().c_str()))) {
        node->clearPersistentMessage(false);
    }
}

std::string
IDefocus::idleWarning() const
{
    KnobChoicePtr type = _kernelType.lock();
    const bool image = type && (shapeOf(type->getValue()) == BokehShape::Image);

    if (image && !getInput(kIDefocusInputKernel)) {
        return Defocus::kernelNotConnectedMessage();
    }
    if (!getInput(kIDefocusInputMap)) {
        return mapNotConnectedMessage();
    }

    return std::string();
}

void
IDefocus::refreshWarning()
{
    setWarning(idleWarning());
}

NATRON_NAMESPACE_EXIT
