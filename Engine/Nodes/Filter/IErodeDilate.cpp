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

#include "IErodeDilate.h"

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
#include <vector>

#include <QString>

#include "Engine/AppManager.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/KnobReader.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuBlur.h"
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace IErodeDilateKernels {
namespace {
    // As ErodeDilateKernels' reduce(), and in the same argument order everywhere, so that the GPU
    // takes the same value even among NaNs and zeros of either sign.
    inline float
    reduce(float a,
           float b,
           bool takeMax)
    {
        return takeMax ? ((b > a) ? b : a) : ((b < a) ? b : a);
    }

    // a + t (b - a), a itself when the two are equal, so that equal extrema blend to themselves
    // exactly whatever t, infinities included.
    inline float
    blend(float a,
          float b,
          float t)
    {
        if ((a == b) || (t == 0.f)) {
            return a;
        }
        const float d = b - a;
        const float s = t * d;

        return a + s;
    }

    /// The half-widths of the ellipse rows of the last few element sizes a row asked for, by |dy|.
    class HalfWidths {
    public:
        const int* get(int rx,
                       int ry)
        {
            Entry& e = _entries[((unsigned)rx * 31u + (unsigned)ry) % kEntries];
            if (!e.valid || (e.rx != rx) || (e.ry != ry)) {
                e.valid = true;
                e.rx = rx;
                e.ry = ry;
                e.widths.resize((std::size_t)ry + 1);
                for (int dy = 0; dy <= ry; ++dy) {
                    e.widths[dy] = ellipseHalfWidth(rx, ry, dy);
                }
            }

            return e.widths.data();
        }

    private:
        static const unsigned kEntries = 16;

        struct Entry {
            bool valid = false;
            int rx = 0;
            int ry = 0;
            std::vector<int> widths;
        };

        Entry _entries[kEntries];
    };

    /// The extremum over the element of whole radii (rx, ry) at pixel (x, y), clipped to the plane:
    /// row by row from the top, each row's span from the table.
    float
    elementExtremum(const float* plane,
                    const float* tables,
                    int width,
                    int height,
                    int x,
                    int y,
                    int rx,
                    int ry,
                    bool disc,
                    bool takeMax,
                    HalfWidths& halfWidths)
    {
        const std::size_t levelStride = (std::size_t)width * height;
        const int y1 = std::max(0, y - ry);
        const int y2 = std::min(height - 1, y + ry);
        const int* widths = disc ? halfWidths.get(rx, ry) : nullptr;
        float acc = 0.f;

        for (int yy = y1; yy <= y2; ++yy) {
            const int dy = (yy >= y) ? (yy - y) : (y - yy);
            const int hw = widths ? widths[dy] : rx;
            const int xa = std::max(0, x - hw);
            const int xb = std::min(width - 1, x + hw);
            const std::size_t row = (std::size_t)yy * width;
            const float q = rowQuery(plane + row, tables + row, levelStride, xa, xb, takeMax);
            acc = (yy == y1) ? q : reduce(acc, q, takeMax);
        }

        return acc;
    }
} // anonymous namespace

int
levelCount(int maxHalfWidth)
{
    return (int)std::bit_width((unsigned)(2 * std::max(0, maxHalfWidth) + 1));
}

void
buildRowTable(const float* row,
              int width,
              int levels,
              bool takeMax,
              float* tableRow,
              std::size_t levelStride)
{
    const float* prev = row;

    for (int k = 1; k < levels; ++k) {
        const int half = 1 << (k - 1);
        const int span = 1 << k;
        float* const cur = tableRow + (std::size_t)(k - 1) * levelStride;
        for (int j = 0; j + span <= width; ++j) {
            cur[j] = reduce(prev[j], prev[j + half], takeMax);
        }
        prev = cur;
    }
}

float
rowQuery(const float* row,
         const float* tableRow,
         std::size_t levelStride,
         int x1,
         int x2,
         bool takeMax)
{
    const int k = (int)std::bit_width((unsigned)(x2 - x1 + 1)) - 1;

    if (k == 0) {
        return row[x1];
    }
    const float* const level = tableRow + (std::size_t)(k - 1) * levelStride;

    return reduce(level[x1], level[x2 - (1 << k) + 1], takeMax);
}

int
ellipseHalfWidth(int rx,
                 int ry,
                 int dy)
{
    if (dy < 0) {
        dy = -dy;
    }
    if (dy > ry) {
        return -1;
    }
    if ((ry == 0) || (rx == 0)) {
        return rx;
    }
    // The largest d with d^2 ry^2 <= rx^2 (ry^2 - dy^2), from the float root, corrected exactly.
    const int64_t ry2 = (int64_t)ry * ry;
    const int64_t rhs = (int64_t)rx * rx * (ry2 - (int64_t)dy * dy);
    int d = std::min(rx, (int)(std::sqrt((double)rhs) / ry));
    while ((d < rx) && ((int64_t)(d + 1) * (d + 1) * ry2 <= rhs)) {
        ++d;
    }
    while ((d > 0) && ((int64_t)d * d * ry2 > rhs)) {
        --d;
    }

    return d;
}

void
filterRow(const float* plane,
          const float* tables,
          int width,
          int height,
          int y,
          int x1,
          int x2,
          const float* rx,
          const float* ry,
          bool disc,
          bool takeMax,
          float* out)
{
    HalfWidths halfWidths;

    for (int x = x1; x < x2; ++x) {
        const float rxf = rx[x - x1];
        const float ryf = ry[x - x1];
        const float fx = std::floor(rxf);
        const float fy = std::floor(ryf);
        const float tx = rxf - fx;
        const float ty = ryf - fy;
        const int r0x = (int)fx;
        const int r0y = (int)fy;
        const int r1x = (tx > 0.f) ? r0x + 1 : r0x;
        const int r1y = (ty > 0.f) ? r0y + 1 : r0y;

        // The four elements of the radii's floors and ceilings, blended in x then in y.
        const float v00 = elementExtremum(plane, tables, width, height, x, y, r0x, r0y, disc, takeMax, halfWidths);
        const float v10 = (r1x != r0x) ? elementExtremum(plane, tables, width, height, x, y, r1x, r0y, disc, takeMax, halfWidths) : v00;
        const float v01 = (r1y != r0y) ? elementExtremum(plane, tables, width, height, x, y, r0x, r1y, disc, takeMax, halfWidths) : v00;
        float v11;
        if ((r1x != r0x) && (r1y != r0y)) {
            v11 = elementExtremum(plane, tables, width, height, x, y, r1x, r1y, disc, takeMax, halfWidths);
        } else {
            v11 = (r1x != r0x) ? v10 : v01;
        }
        const float a = blend(v00, v10, tx);
        const float b = blend(v01, v11, tx);
        out[x - x1] = blend(a, b, ty);
    }
}

void
filterPlane(const float* src,
            int width,
            int height,
            const float* rx,
            const float* ry,
            bool disc,
            bool takeMax,
            float* dst)
{
    if ((width <= 0) || (height <= 0)) {
        return;
    }
    const std::size_t pixels = (std::size_t)width * height;
    float maxRx = 0.f;
    for (std::size_t i = 0; i < pixels; ++i) {
        maxRx = std::max(maxRx, rx[i]);
    }
    const int levels = levelCount((int)std::ceil(maxRx));
    std::vector<float> tables((std::size_t)std::max(0, levels - 1) * pixels);
    for (int y = 0; y < height; ++y) {
        const std::size_t row = (std::size_t)y * width;
        buildRowTable(src + row, width, levels, takeMax, tables.data() + row, pixels);
    }
    for (int y = 0; y < height; ++y) {
        const std::size_t row = (std::size_t)y * width;
        filterRow(src, tables.data(), width, height, y, 0, width, rx + row, ry + row, disc, takeMax, dst + row);
    }
}
} // namespace IErodeDilateKernels

float
IErodeDilateAxis::at(float map) const
{
    if (map >= 1.f) {
        return white;
    }
    if (!(map > 0.f)) {
        return black;
    }
    // Separate statements, as the GPU's precise ones, so that neither contracts them.
    const float d = white - black;
    const float s = map * d;
    const float r = black + s;

    return std::min(std::max(r, std::min(black, white)), std::max(black, white));
}

float
IErodeDilateAxis::largest(const MapRange& range) const
{
    // at() is monotonic, so its largest value over the range is at one end.
    return std::max(at(range.min), at(range.max));
}

void
IErodeDilateParams::axes(const RenderScale& scale,
                         IErodeDilateAxis* x,
                         IErodeDilateAxis* y) const
{
    const OfxPointD rs = scale.toOfxPointD();

    // Like Erode's, the radii are not divided by the pixel aspect ratio.
    x->black = (float)std::max(0., map.sizeAtBlack[0] * rs.x);
    x->white = (float)std::max(0., sizeX * rs.x);
    y->black = (float)std::max(0., map.sizeAtBlack[1] * rs.y);
    y->white = (float)std::max(0., sizeY * rs.y);
}

namespace {
constexpr InputSpec kIErodeDilateInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
    kMapInputSpec,
};

template <ErodeDilateMode MODE>
struct IErodeDilateSpec {
    static constexpr bool kDilate = (MODE == ErodeDilateMode::Dilate);

    static constexpr NodeSpec kNodeSpec = {
        .id = kDilate ? PLUGINID_NATRON_IDILATE : PLUGINID_NATRON_IERODE,
        .label = kDilate ? "IDilate" : "IErode",
        .description = kDilate ? "Dilate the input stream by a structuring element whose size varies per pixel with the Map input: each pixel takes the maximum of the pixels around it.\n"
                                 "\n"
                                 "Where the Map is white the element has the Size, where it is black the Size At Black (0 by default, which leaves those pixels exactly unchanged), "
                                 "and in between a size in between; a fractional size blends the results of the sizes either side of it, so the result changes smoothly with the Map. "
                                 "Map Channel picks the Map's channel in any of its layers, Invert Map inverts it, and Map Black and Map White set the Map values that give Size At Black and Size. "
                                 "Outside the Map image, the Map reads black. With no Map connected, the element has the Size everywhere.\n"
                                 "\n"
                                 "The element is a rectangle of 2*size+1 pixels, as Dilate's, or a disc (an ellipse when the two sizes differ). At a whole Size where the Map is white, "
                                 "the rectangle gives Dilate's result. The Mask and Mix limit the result as usual."
                               : "Erode the input stream by a structuring element whose size varies per pixel with the Map input: each pixel takes the minimum of the pixels around it.\n"
                                 "\n"
                                 "Where the Map is white the element has the Size, where it is black the Size At Black (0 by default, which leaves those pixels exactly unchanged), "
                                 "and in between a size in between; a fractional size blends the results of the sizes either side of it, so the result changes smoothly with the Map. "
                                 "Map Channel picks the Map's channel in any of its layers, Invert Map inverts it, and Map Black and Map White set the Map values that give Size At Black and Size. "
                                 "Outside the Map image, the Map reads black. With no Map connected, the element has the Size everywhere.\n"
                                 "\n"
                                 "The element is a rectangle of 2*size+1 pixels, as Erode's, or a disc (an ellipse when the two sizes differ). At a whole Size where the Map is white, "
                                 "the rectangle gives Erode's result. The Mask and Mix limit the result as usual.",
        .grouping = PLUGIN_GROUP_FILTER,
        .majorVersion = kDilate ? PLUGIN_MAJOR_NATRON_IDILATE : PLUGIN_MAJOR_NATRON_IERODE,
        .icon = kDilate ? NATRON_IMAGES_PATH "NativeNodes/IDilate.png" : NATRON_IMAGES_PATH "NativeNodes/IErode.png",
        .inputs = kIErodeDilateInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::MaskMix, Stage::PassThroughUnprocessed },
        .layers = LayerPolicy::all(),
        .op = OpKind::Passes,
    };
};

/**
 * @brief What the passes of one render share: the Map and the radius functions, the render window
 * the Map covers, and the row tables of every plane. The first pass sizes the tables for the plane
 * job it runs in, as IBlur's state does; they are written whole before they are read, so they
 * start uninitialised.
 **/
struct MorphologyState {
    std::shared_ptr<const MapPlane> map;
    IErodeDilateAxis axisX;
    IErodeDilateAxis axisY;
    bool disc = false;
    bool takeMax = false;
    RectI roi;
    int levels = 1;

    std::mutex mutex;
    std::unique_ptr<float[]> tables;
    std::size_t capacity = 0;

    // The tables of one plane: levels - 1 levels of width x height.
    std::size_t planeTables(const PassScratch& scratch) const
    {
        return (std::size_t)(levels - 1) * (std::size_t)scratch.width * (std::size_t)scratch.height;
    }

    void reserve(const PassScratch& scratch)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        const std::size_t size = planeTables(scratch) * (std::size_t)scratch.nPlanes;
        if (capacity < size) {
            tables.reset(new float[size]);
            capacity = size;
        }
    }
};

using MorphologyStatePtr = std::shared_ptr<MorphologyState>;

/// The table of row y of plane p of slot 0.
std::function<PassRowFn(const PassScratch&)>
tableWorker(const MorphologyStatePtr& state)
{
    return [state](const PassScratch& scratch) -> PassRowFn {
        state->reserve(scratch);
        float* const tables = state->tables.get();
        const std::size_t planeTables = state->planeTables(scratch);
        const std::size_t levelStride = (std::size_t)scratch.width * scratch.height;
        const int width = scratch.width;
        const int height = scratch.height;
        const int levels = state->levels;
        const bool takeMax = state->takeMax;
        const PassScratch* source = &scratch;

        return [tables, planeTables, levelStride, width, height, levels, takeMax, source](int line) {
            const int p = line / height;
            const int y = line % height;
            const std::size_t row = (std::size_t)y * width;
            IErodeDilateKernels::buildRowTable(source->plane(0, p) + row, width, levels, takeMax, tables + (std::size_t)p * planeTables + row, levelStride);
        };
    };
}

/// Row y of plane p of slot 1, whole: the filter over the render window, from the Map's radii,
/// and slot 0's pixels elsewhere, which only the halo holds and nothing reads.
std::function<PassRowFn(const PassScratch&)>
filterWorker(const MorphologyStatePtr& state)
{
    return [state](const PassScratch& scratch) -> PassRowFn {
        state->reserve(scratch);
        const MorphologyState* s = state.get();
        const float* const tables = state->tables.get();
        const std::size_t planeTables = state->planeTables(scratch);
        const int width = scratch.width;
        const int height = scratch.height;
        const RectI rect = scratch.rect;
        const int x1 = std::max(s->roi.x1, rect.x1) - rect.x1;
        const int x2 = std::max(x1, std::min(s->roi.x2, rect.x2) - rect.x1);
        const PassScratch* buffers = &scratch;
        // The radii of one row, reused by every line of the chunk.
        std::shared_ptr<std::vector<float>> radii = std::make_shared<std::vector<float>>(2 * (std::size_t)(x2 - x1));

        return [s, tables, planeTables, width, height, rect, x1, x2, buffers, radii](int line) {
            const int p = line / height;
            const int y = line % height;
            const std::size_t row = (std::size_t)y * width;
            const float* const src = buffers->plane(0, p);
            float* const dst = buffers->plane(1, p) + row;
            const int yAbs = rect.y1 + y;
            if ((yAbs < s->roi.y1) || (yAbs >= s->roi.y2) || (x1 == x2)) {
                std::copy(src + row, src + row + width, dst);

                return;
            }
            std::copy(src + row, src + row + x1, dst);
            std::copy(src + row + x2, src + row + width, dst + x2);
            float* const rx = radii->data();
            float* const ry = rx + (x2 - x1);
            float last = s->map->at(rect.x1 + x1, yAbs);
            float lastX = s->axisX.at(last);
            float lastY = s->axisY.at(last);
            for (int x = x1; x < x2; ++x) {
                const float m = s->map->at(rect.x1 + x, yAbs);
                if (m != last) {
                    last = m;
                    lastX = s->axisX.at(m);
                    lastY = s->axisY.at(m);
                }
                rx[x - x1] = lastX;
                ry[x - x1] = lastY;
            }
            IErodeDilateKernels::filterRow(src, tables + (std::size_t)p * planeTables, width, height, y, x1, x2, rx, ry, s->disc, s->takeMax, dst + x1);
        };
    };
}
#ifdef NATRON_HAVE_GPU
// The largest radius, in pixels, the GPU records: its ellipse arithmetic is in 32-bit integers.
// A larger element stays on the CPU.
const int kGpuMaxRadius = 255;

// Mirror kDisc, kTakeMax and kMap* in Kernels/ierodedilate.slang.
const uint32_t kGpuDisc = 1;
const uint32_t kGpuTakeMax = 2;
const uint32_t kGpuMapConnected = 1;
const uint32_t kGpuMapRead = 2;
const uint32_t kGpuMapInvert = 4;

// Mirrors MorphParams in Kernels/ierodedilate.slang, which has no padding between members.
struct IErodeDilateGpuParams {
    int32_t domX1;
    int32_t domY1;
    uint32_t domWidth;
    uint32_t domHeight;
    uint32_t channels;
    uint32_t comp;
    uint32_t level;
    uint32_t flags;
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
static_assert(sizeof(IErodeDilateGpuParams) == 80);
static_assert(offsetof(IErodeDilateGpuParams, srcX1) == 32);
static_assert(offsetof(IErodeDilateGpuParams, outX1) == 56);

// Mirrors MorphConstants in Kernels/ierodedilate.slang, which has no padding between members.
struct IErodeDilateGpuConstants {
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
    float outside;
    float blackRx;
    float whiteRx;
    float blackRy;
    float whiteRy;
};
static_assert(sizeof(IErodeDilateGpuConstants) == 64);
static_assert(offsetof(IErodeDilateGpuConstants, black) == 36);
static_assert(offsetof(IErodeDilateGpuConstants, blackRx) == 48);

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

// Every kernel of Kernels/ierodedilate.slang binds these, in this order.
struct IErodeDilateGpuBuffers {
    const gpu::GpuBuffer* source = nullptr;
    const gpu::GpuBuffer* map = nullptr;
    const gpu::GpuBuffer* constants = nullptr;
    const gpu::GpuBuffer* work = nullptr;
    const gpu::GpuBuffer* out = nullptr;
};

bool
dispatchIErodeDilate(gpu::GpuRecordContext& ctx,
                     gpu::GpuKernelId kernel,
                     const IErodeDilateGpuBuffers& b,
                     const IErodeDilateGpuParams& p,
                     uint32_t width,
                     uint32_t height)
{
    if (!width || !height) {
        return true;
    }

    return (bool)ctx.dispatch(kernel, { b.source, b.map, b.constants, b.work, b.out }, p, { width, height, 1 });
}

/**
 * @brief The passes of IErodeDilateNode::passes() as the GPU records them over io.domain, the
 * CPU's scratch rect, one component at a time: the component into level 0 of the work buffer,
 * the table's levels one dispatch each, and the filter of every output pixel into io.filtered.
 * The Map's channel is mapChannel of io.inputs[Map], or none when that is negative. Fails, so that
 * the CPU renders, when a buffer exceeds the kernels' 32-bit indices.
 **/
StatusEnum
recordIErodeDilateGpu(const PassGpuIO& io,
                      const IErodeDilateParams& params,
                      const RenderScale& scale,
                      bool takeMax,
                      bool mapConnected,
                      int mapChannel)
{
    gpu::GpuRecordContext& ctx = io.context;
    const RectI& domain = io.domain;
    const int nComps = io.nComps;
    if (PassExec::isEmptyRect(domain) || (nComps < 1) || (nComps > 4) || !io.filtered.buffer) {
        return eStatusFailed;
    }
    IErodeDilateAxis axisX;
    IErodeDilateAxis axisY;
    params.axes(scale, &axisX, &axisY);
    MapRange full;
    full.min = 0.f;
    full.max = 1.f;
    const int levels = IErodeDilateKernels::levelCount((int)std::ceil(axisX.largest(full)));
    const uint64_t width = (uint64_t)domain.width();
    const uint64_t height = (uint64_t)domain.height();
    const uint64_t workFloats = (uint64_t)levels * width * height;
    if (workFloats > UINT32_MAX) {
        return eStatusFailed;
    }

    IErodeDilateGpuConstants k {};
    k.mapFlags = (mapConnected ? kGpuMapConnected : 0u) | (params.map.invert ? kGpuMapInvert : 0u);
    k.black = (float)params.map.black;
    k.white = (float)params.map.white;
    // A disconnected Map reads 1 everywhere, a connected one raw 0 outside its image.
    k.outside = mapConnected ? params.map.remap(0.f) : 1.f;
    k.blackRx = axisX.black;
    k.whiteRx = axisX.white;
    k.blackRy = axisY.black;
    k.whiteRy = axisY.white;

    gpu::GpuBuffer* constants = nullptr;
    gpu::GpuBuffer* work = nullptr;
    if (!ctx.scratch(sizeof(k), constants) || !constants || !ctx.scratch(workFloats * sizeof(float), work) || !work) {
        return eStatusFailed;
    }

    IErodeDilateGpuBuffers buffers;
    // Unread placeholders for a missing source or Map, which the kernels leave alone.
    buffers.source = constants;
    buffers.map = constants;
    buffers.constants = constants;
    buffers.work = work;
    buffers.out = io.filtered.buffer;

    const GpuImageView map = (kIErodeDilateInputMap < (int)io.inputs.size()) ? io.inputs[kIErodeDilateInputMap] : GpuImageView();
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
    if (!ctx.updateBuffer(*constants, 0, std::as_bytes(std::span<const IErodeDilateGpuConstants>(&k, 1)))) {
        return eStatusFailed;
    }

    IErodeDilateGpuParams base {};
    base.domX1 = domain.x1;
    base.domY1 = domain.y1;
    base.domWidth = (uint32_t)width;
    base.domHeight = (uint32_t)height;
    base.channels = (uint32_t)nComps;
    base.flags = (params.disc ? kGpuDisc : 0u) | (takeMax ? kGpuTakeMax : 0u);
    const gpu::GpuBlurImage filtered = PassExec::toGpuBlurImage(io.filtered);
    base.outX1 = io.output.x1;
    base.outY1 = io.output.y1;
    base.outWidth = (uint32_t)std::max(0, io.output.width());
    base.outHeight = (uint32_t)std::max(0, io.output.height());
    base.dstStride = filtered.strideFloats;
    if (!PassExec::isEmptyRect(io.output) && !gpuFloatIndex(filtered, io.output.x1, io.output.y1, nComps, &base.dstOffset)) {
        return eStatusFailed;
    }
    if (io.source.buffer && !PassExec::isEmptyRect(io.sourceValid)) {
        const gpu::GpuBlurImage source = PassExec::toGpuBlurImage(io.source);
        base.srcX1 = io.sourceValid.x1;
        base.srcY1 = io.sourceValid.y1;
        base.srcX2 = io.sourceValid.x2;
        base.srcY2 = io.sourceValid.y2;
        base.srcStride = source.strideFloats;
        if (!gpuFloatIndex(source, io.sourceValid.x1, io.sourceValid.y1, nComps, &base.srcOffset)) {
            return eStatusFailed;
        }
        buffers.source = io.source.buffer;
    }

    for (int c = 0; c < nComps; ++c) {
        IErodeDilateGpuParams p = base;
        p.comp = (uint32_t)c;
        if (!dispatchIErodeDilate(ctx, gpu::GpuKernelId::ierodedilate_load, buffers, p, base.domWidth, base.domHeight)) {
            return eStatusFailed;
        }
        ctx.computeBarrier();
        for (int level = 1; level < levels; ++level) {
            p.level = (uint32_t)level;
            if (!dispatchIErodeDilate(ctx, gpu::GpuKernelId::ierodedilate_level, buffers, p, base.domWidth, base.domHeight)) {
                return eStatusFailed;
            }
            ctx.computeBarrier();
        }
        if (!dispatchIErodeDilate(ctx, gpu::GpuKernelId::ierodedilate_filter, buffers, p, base.outWidth, base.outHeight)) {
            return eStatusFailed;
        }
        // The next component overwrites the table the filter reads.
        if (c < nComps - 1) {
            ctx.computeBarrier();
        }
    }

    return eStatusOK;
} // recordIErodeDilateGpu
#endif // NATRON_HAVE_GPU
} // anonymous namespace

template <ErodeDilateMode MODE>
IErodeDilateNode<MODE>::IErodeDilateNode(NodePtr node)
    : StagedImageEffect(node)
    , _map()
    , _size()
    , _shape()
    , _expandRoD()
{
}

template <ErodeDilateMode MODE>
IErodeDilateNode<MODE>::~IErodeDilateNode()
{
}

template <ErodeDilateMode MODE>
const NodeSpec*
IErodeDilateNode<MODE>::nodeSpec() const
{
    return staticNodeSpec<IErodeDilateSpec<MODE>::kNodeSpec>();
}

template <ErodeDilateMode MODE>
const ImagePipelineSpec&
IErodeDilateNode<MODE>::imagePipelineSpec() const
{
    return staticImagePipelineSpec<IErodeDilateSpec<MODE>::kPipeline, IErodeDilateSpec<MODE>::kNodeSpec>();
}

template <ErodeDilateMode MODE>
std::string
IErodeDilateNode<MODE>::getInputHint(int inputNb) const
{
    switch (inputNb) {
    case kIErodeDilateInputSource:
        return kDilate ? tr("The image to dilate.").toStdString() : tr("The image to erode.").toStdString();
    case kIErodeDilateInputMask:
        return tr("The mask the result is mixed with the source through.").toStdString();
    case kIErodeDilateInputMap:
        return tr("The Map: the element has the Size where it is white and the Size At Black where it is black.").toStdString();
    }

    return std::string();
}

template <ErodeDilateMode MODE>
void
IErodeDilateNode<MODE>::addAcceptedComponents(int inputNb,
                                              std::list<ImageLayerDesc>* comps)
{
    StagedImageEffect::addAcceptedComponents(inputNb, comps);
    if (!isInputMask(inputNb)) {
        comps->push_back(ImageLayerDesc::getXYComponents());
    }
}

template <ErodeDilateMode MODE>
void
IErodeDilateNode<MODE>::initializeKnobs()
{
    const KnobPagePtr page = createControlsPage();

    KnobDoublePtr sizeKnob = AppManager::createKnob<KnobDouble>(this, tr("Size"), 2);
    sizeKnob->setName(kIErodeDilateParamSize);
    sizeKnob->setHintToolTip(tr("Half the width and height of the structuring element where the Map is white, in pixel units (>=0): the rectangle is 2*size+1 pixels wide, the disc's radius is size. A fractional size blends the results of the whole sizes either side of it."));
    sizeKnob->setSpatial(true);
    sizeKnob->setCanAutoFoldDimensions(true);
    for (int d = 0; d < 2; ++d) {
        sizeKnob->setMinimum(0., d);
        sizeKnob->setMaximum(1000., d);
        sizeKnob->setDisplayMinimum(0., d);
        sizeKnob->setDisplayMaximum(100., d);
        sizeKnob->setIncrement(0.1, d);
        sizeKnob->setDecimals(1, d);
        sizeKnob->setDefaultValue(1., d);
    }
    page->addKnob(sizeKnob);
    _size = sizeKnob;

    KnobChoicePtr shapeKnob = AppManager::createKnob<KnobChoice>(this, tr("Shape"));
    shapeKnob->setName(kIErodeDilateParamShape);
    shapeKnob->setHintToolTip(tr("The shape of the structuring element."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kIErodeDilateParamShapeSquare, tr("Square").toStdString(), tr("A rectangle of 2*size+1 pixels, as Erode's and Dilate's.").toStdString()));
        options.push_back(ChoiceOption(kIErodeDilateParamShapeDisc, tr("Disc").toStdString(), tr("The pixels within the ellipse of half-axes the two sizes: a disc when they are equal.").toStdString()));
        shapeKnob->populateChoices(options);
    }
    shapeKnob->setDefaultValue(0);
    page->addKnob(shapeKnob);
    _shape = shapeKnob;

    _map.createKnobs(this, page, 2);

    KnobBoolPtr expandRoDKnob = AppManager::createKnob<KnobBool>(this, tr("Expand RoD"));
    expandRoDKnob->setName(kIErodeDilateParamExpandRoD);
    if (kDilate) {
        expandRoDKnob->setHintToolTip(tr("Expand the source region of definition by the larger of the two sizes."));
    } else {
        expandRoDKnob->setHintToolTip(tr("Erosion never expands the source region of definition; kept for symmetry with IDilate."));
    }
    expandRoDKnob->setDefaultValue(true);
    page->addKnob(expandRoDKnob);
    _expandRoD = expandRoDKnob;

    addStageKnobs(page);

    _map.declareLayerKnob(getNode(), kIErodeDilateInputMap);
} // IErodeDilateNode::initializeKnobs

template <ErodeDilateMode MODE>
IErodeDilateParams
IErodeDilateNode<MODE>::getParams(double time,
                                  ViewIdx view) const
{
    const KnobReader reader(time, view);
    IErodeDilateParams params;

    params.sizeX = reader.value(_size, 0);
    params.sizeY = reader.value(_size, 1);
    params.disc = (reader.value(_shape) == 1);
    params.expandRoD = reader.value(_expandRoD);
    params.map = _map.bake(reader);

    return params;
}

template <ErodeDilateMode MODE>
PassList
IErodeDilateNode<MODE>::passes(const IErodeDilateParams& params,
                               const RenderScale& scale,
                               const MapRange& range,
                               const std::shared_ptr<const MapPlane>& map,
                               const RectI& roi)
{
    PassList list;
    IErodeDilateAxis axisX;
    IErodeDilateAxis axisY;
    params.axes(scale, &axisX, &axisY);
    const float rx = axisX.largest(range);
    const float ry = axisY.largest(range);

    if ((rx <= 0.f) && (ry <= 0.f)) {
        return list;
    }
    const int haloX = (int)std::ceil(rx);
    const int haloY = (int)std::ceil(ry);

    MorphologyStatePtr state;
    if (map) {
        state = std::make_shared<MorphologyState>();
        state->map = map;
        state->axisX = axisX;
        state->axisY = axisY;
        state->disc = params.disc;
        state->takeMax = kDilate;
        state->roi = roi;
        state->levels = IErodeDilateKernels::levelCount(haloX);
    }

    list.scratchSlots = 2;
    list.resultSlot = 1;

    list.passes.push_back(Pass::point(true, state ? tableWorker(state) : std::function<PassRowFn(const PassScratch&)>()));

    Pass filter = Pass::gather(haloX, haloY, true, state ? filterWorker(state) : std::function<PassRowFn(const PassScratch&)>());
    filter.slot = 1;
    // An erosion never spreads the region of definition.
    if (!kDilate) {
        filter.growX = 0;
        filter.growY = 0;
    }
    // Up to four elements of two lookups per row.
    filter.tapsPerSample = 8 * (2 * (std::size_t)haloY + 1);
    list.passes.push_back(filter);

    return list;
} // IErodeDilateNode::passes

template <ErodeDilateMode MODE>
std::string
IErodeDilateNode<MODE>::mapNotConnectedMessage()
{
    return kDilate ? tr("The Map input is not connected: the dilation has the Size everywhere.").toStdString()
                   : tr("The Map input is not connected: the erosion has the Size everywhere.").toStdString();
}

template <ErodeDilateMode MODE>
PassList
IErodeDilateNode<MODE>::opPasses(double time,
                                 ViewIdx view,
                                 const RenderScale& scale) const
{
    // A disconnected Map reads 1 everywhere; a connected one may hold any value.
    MapRange range;
    range.min = getInput(kIErodeDilateInputMap) ? 0.f : 1.f;
    range.max = 1.f;
    const IErodeDilateParams params = getParams(time, view);
    PassList list = passes(params, scale, range, std::shared_ptr<const MapPlane>(), RectI());

#ifdef NATRON_HAVE_GPU
    bool capped = false;
    for (const Pass& pass : list.passes) {
        capped = capped || (pass.haloX > kGpuMaxRadius) || (pass.haloY > kGpuMaxRadius);
    }
    if (!list.empty() && !capped) {
        const IErodeDilateNode* self = this;
        list.gpu = [self, time, view, params, scale](const PassGpuIO& io) -> StatusEnum {
            return self->recordGpu(io, time, view, params, scale);
        };
    }
#endif

    return list;
}

template <ErodeDilateMode MODE>
StatusEnum
IErodeDilateNode<MODE>::recordGpu(const PassGpuIO& io,
                                  double time,
                                  ViewIdx view,
                                  const IErodeDilateParams& params,
                                  const RenderScale& scale) const
{
#ifdef NATRON_HAVE_GPU
    const bool connected = (bool)getInput(kIErodeDilateInputMap);
    ImageLayerDesc layer;
    int channel = -1;
    // Resolving only reads the Map knob and the Map's layers.
    if (!connected || !_map.resolveChannel(const_cast<IErodeDilateNode*>(this), kIErodeDilateInputMap, time, view, &layer, &channel)) {
        channel = -1;
    }

    return recordIErodeDilateGpu(io, params, scale, kDilate, connected, channel);
#else
    Q_UNUSED(io);
    Q_UNUSED(time);
    Q_UNUSED(view);
    Q_UNUSED(params);
    Q_UNUSED(scale);

    return eStatusFailed;
#endif
}

template <ErodeDilateMode MODE>
bool
IErodeDilateNode<MODE>::readsGpuInput(int inputNb,
                                      const ImageLayerDesc& /*plane*/,
                                      double time,
                                      ViewIdx view) const
{
    if (inputNb != kIErodeDilateInputMap) {
        return true;
    }
    ImageLayerDesc layer;
    int channel = -1;

    return _map.resolveChannel(const_cast<IErodeDilateNode*>(this), kIErodeDilateInputMap, time, view, &layer, &channel);
}

template <ErodeDilateMode MODE>
bool
IErodeDilateNode<MODE>::opPassesInputLayer(int inputNb,
                                           const ImageLayerDesc& plane,
                                           double time,
                                           ViewIdx view,
                                           ImageLayerDesc* layer) const
{
    if (inputNb != kIErodeDilateInputMap) {
        *layer = plane;

        return true;
    }
    int channel = -1;

    return _map.resolveChannel(const_cast<IErodeDilateNode*>(this), kIErodeDilateInputMap, time, view, layer, &channel);
}

template <ErodeDilateMode MODE>
bool
IErodeDilateNode<MODE>::isOpIdentity(double time,
                                     const RenderScale& scale,
                                     const RectI& /*roi*/,
                                     ViewIdx view)
{
    return opPasses(time, view, scale).empty();
}

template <ErodeDilateMode MODE>
StatusEnum
IErodeDilateNode<MODE>::renderOp(const RenderActionArgs& args,
                                 const StagePlan& plan)
{
    const PassList full = opPasses(args.time, args.view, args.mappedScale);

    if (full.empty()) {
        return runPassesCpu(*this, plan, args, full);
    }
    const IErodeDilateParams params = getParams(args.time, args.view);
    // Only the output pixels' radii matter, so the Map is read over the render window alone, and
    // the tile's halo is that of its largest radius there.
    std::shared_ptr<MapPlane> map = std::make_shared<MapPlane>();
    const StatusEnum status = _map.read(this, kIErodeDilateInputMap, params.map, args.time, args.view, args.mappedScale, args.roi, map.get());
    if (status != eStatusOK) {
        return status;
    }
    const MapRange range = map->tileRange(args.roi);

    return runPassesCpu(*this, plan, args, passes(params, args.mappedScale, range, map, args.roi));
}

template <ErodeDilateMode MODE>
bool
IErodeDilateNode<MODE>::clipRegionOfDefinition(double time,
                                               const RenderScale& /*scale*/,
                                               ViewIdx view,
                                               const EffectInstancePtr& /*source*/,
                                               const RectI& srcRoD,
                                               RectI* rod)
{
    const KnobReader reader(time, view);

    if (reader.value(_expandRoD) || (*rod == srcRoD)) {
        return false;
    }
    *rod = srcRoD;

    return true;
}

template <ErodeDilateMode MODE>
void
IErodeDilateNode<MODE>::onEffectCreated(bool /*mayCreateFileDialog*/,
                                        const CreateNodeArgs& /*args*/)
{
    refreshMapWarning();
}

template <ErodeDilateMode MODE>
void
IErodeDilateNode<MODE>::onInputChanged(int /*inputNo*/)
{
    refreshMapWarning();
}

template <ErodeDilateMode MODE>
void
IErodeDilateNode<MODE>::refreshMapWarning()
{
    const NodePtr node = getNode();

    if (!node) {
        return;
    }
    const std::string message = mapNotConnectedMessage();
    if (!getInput(kIErodeDilateInputMap)) {
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

template class IErodeDilateNode<ErodeDilateMode::Erode>;
template class IErodeDilateNode<ErodeDilateMode::Dilate>;

NATRON_NAMESPACE_EXIT
