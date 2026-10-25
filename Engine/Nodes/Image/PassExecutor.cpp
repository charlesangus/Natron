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

#include "PassExecutor.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <list>
#include <memory>
#include <utility>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/HostImageMemory.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/Nodes/Image/StagePlan.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuBuffer.h"
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace PassExec {
namespace {
    // Below this many pixels a chunk of lines is not worth handing to another thread.
    const std::size_t kMinChunkPixels = 16384;
    // Chunks per available thread, so a helper that starts late, on a pool busy with other tasks,
    // still finds work left.
    const int kChunksPerThread = 4;
} // anonymous namespace

int
channelIndexForBit(int nComps,
                   int bit)
{
    if (nComps == 1) {
        return (bit == 3) ? 0 : -1;
    }

    return (bit < nComps) ? bit : -1;
}

std::bitset<4>
processedBitsForImage(const ImageLayerDesc& plane,
                      int dstNComps,
                      const std::bitset<4>& planeBits)
{
    if ((plane.getNumComponents() != 1) || (dstNComps == 1)) {
        return planeBits;
    }
    std::bitset<4> bits;
    bits[plane.isColorLayer() ? 3 : 0] = planeBits[3];

    return bits;
}

bool
isFloatImage(const ImagePtr& image)
{
    return !image || (image->getBitDepth() == eImageBitDepthFloat);
}

bool
isEmptyRect(const RectI& r)
{
    return (r.x2 <= r.x1) || (r.y2 <= r.y1);
}

bool
isEmptyRect(const RectD& r)
{
    return (r.x2 <= r.x1) || (r.y2 <= r.y1);
}

RectI
toPixelEnclosing(const RectD& r,
                 const RenderScale& scale,
                 double par)
{
    if (isEmptyRect(r)) {
        return RectI(0, 0, 0, 0);
    }
    const OfxPointD s = scale.toOfxPointD();

    return RectI((int)std::floor(r.x1 * s.x / par),
                 (int)std::floor(r.y1 * s.y),
                 (int)std::ceil(r.x2 * s.x / par),
                 (int)std::ceil(r.y2 * s.y));
}

RectD
toCanonical(const RectI& r,
            const RenderScale& scale,
            double par)
{
    if (isEmptyRect(r)) {
        return RectD(0., 0., 0., 0.);
    }
    const OfxPointD s = scale.toOfxPointD();

    return RectD(r.x1 * par / s.x, r.y1 / s.y, r.x2 * par / s.x, r.y2 / s.y);
}

bool
intersectRects(const RectI& a,
               const RectI& b,
               RectI* out)
{
    if (isEmptyRect(a) || isEmptyRect(b) || (a.x1 > b.x2) || (b.x1 > a.x2) || (a.y1 > b.y2) || (b.y1 > a.y2)) {
        *out = RectI(0, 0, 0, 0);

        return false;
    }
    RectI r;
    r.x1 = std::max(a.x1, b.x1);
    r.x2 = std::max(r.x1, std::min(a.x2, b.x2));
    r.y1 = std::max(a.y1, b.y1);
    r.y2 = std::max(r.y1, std::min(a.y2, b.y2));
    *out = r;

    return true;
}

void
readSourceRow(const Image* image,
              const Image::ReadAccess* access,
              const RectI& bounds,
              int srcNComps,
              int x0,
              int y,
              int width,
              int nComps,
              float* row)
{
    std::fill(row, row + (std::size_t)width * nComps, 0.f);
    if (!image) {
        return;
    }
    if ((y < bounds.y1) || (y >= bounds.y2)) {
        return;
    }
    const int xStart = std::max(x0, bounds.x1);
    const int xEnd = std::min(x0 + width, bounds.x2);
    if (xStart >= xEnd) {
        return;
    }
    const float* srcPix = (const float*)access->pixelAt(xStart, y);
    if (!srcPix) {
        return;
    }
    float* dstPix = row + (std::size_t)(xStart - x0) * nComps;
    if (srcNComps == nComps) {
        std::copy(srcPix, srcPix + (std::size_t)(xEnd - xStart) * nComps, dstPix);

        return;
    }
    int srcIndex[4] = { -1, -1, -1, -1 };
    for (int c = 0; (c < nComps) && (c < 4); ++c) {
        srcIndex[c] = channelIndexForBit(srcNComps, pixelKernelChannelBit(nComps, c));
    }
    for (int x = xStart; x < xEnd; ++x, srcPix += srcNComps, dstPix += nComps) {
        for (int c = 0; (c < nComps) && (c < 4); ++c) {
            if (srcIndex[c] >= 0) {
                dstPix[c] = srcPix[srcIndex[c]];
            }
        }
    }
}

void
readChannelRow(const Image* image,
               const Image::ReadAccess* access,
               const RectI& bounds,
               int imageNComps,
               int channel,
               int x0,
               int y,
               int width,
               float fill,
               float* row)
{
    std::fill(row, row + width, fill);
    if (!image) {
        return;
    }
    if ((y < bounds.y1) || (y >= bounds.y2)) {
        return;
    }
    const int xStart = std::max(x0, bounds.x1);
    const int xEnd = std::min(x0 + width, bounds.x2);
    if (xStart >= xEnd) {
        return;
    }
    const float* pix = (const float*)access->pixelAt(xStart, y);
    if (!pix) {
        return;
    }
    for (int x = xStart; x < xEnd; ++x, pix += imageNComps) {
        row[x - x0] = pix[channel];
    }
}

PlaneJob::PlaneJob()
    : dst()
    , channels()
    , src()
    , divisor()
    , divisorChannel(-1)
    , skipChannel(-1)
    , nComps(0)
    , srcNComps(0)
    , divisorNComps(0)
{
}

void
PlaneJob::lock()
{
    if (src) {
        srcBounds = src->getBounds();
        srcNComps = (int)src->getComponentsCount();
        srcAccess = std::make_shared<Image::ReadAccess>(src.get());
    }
    if (divisor) {
        divisorBounds = divisor->getBounds();
        divisorNComps = (int)divisor->getComponentsCount();
        divisorAccess = std::make_shared<Image::ReadAccess>(divisor.get());
    }
    dstAccess = std::make_shared<Image::WriteAccess>(dst.get());
}

MaskInput::MaskInput()
    : applied(false)
    , invert(false)
    , image()
    , channel(-1)
    , nComps(0)
{
}

void
MaskInput::lock()
{
    if (image) {
        bounds = image->getBounds();
        nComps = (int)image->getComponentsCount();
        access = std::make_shared<Image::ReadAccess>(image.get());
    }
}

ProcessedPlanes::ProcessedPlanes(const PlaneJob& job)
    : channel()
{
    for (int c = 0; c < 4; ++c) {
        planeOfChannel[c] = -1;
    }
    for (int c = 0; (c < job.nComps) && (c < 4); ++c) {
        const int bit = pixelKernelChannelBit(job.nComps, c);
        if (job.channels[bit]) {
            planeOfChannel[c] = (int)channel.size();
            channel.push_back(c);
        }
    }
}

LineBands
splitLines(int nLines,
           std::size_t pixelsPerLine,
           int nThreads)
{
    LineBands bands;

    if (nLines <= 0) {
        return bands;
    }
    std::size_t nChunks = 1;
    if (nThreads > 1) {
        nChunks = std::min((std::size_t)nThreads * kChunksPerThread, ((std::size_t)nLines * pixelsPerLine) / kMinChunkPixels);
        nChunks = std::max((std::size_t)1, std::min(nChunks, (std::size_t)nLines));
    }
    bands.linesPerBand = (int)(((std::size_t)nLines + nChunks - 1) / nChunks);
    bands.count = (nLines + bands.linesPerBand - 1) / bands.linesPerBand;

    return bands;
}

void
forEachLineChunk(int nLines,
                 std::size_t pixelsPerLine,
                 int nThreads,
                 RenderCancellation& cancel,
                 const std::function<void(int, int)>& body)
{
    const LineBands bands = splitLines(nLines, pixelsPerLine, nThreads);

    if (bands.count <= 0) {
        return;
    }
    const std::function<void(int)> chunk = [&](int i) {
        const int first = i * bands.linesPerBand;
        body(first, std::min(nLines, first + bands.linesPerBand));
    };
    parallelForCancellable(bands.count, nThreads, cancel, chunk);
}

void
fillBuffer(const PlaneJob& job,
           const ProcessedPlanes& planes,
           const RectI& bufferRect,
           Boundary boundary,
           int nThreads,
           RenderCancellation& cancel,
           float* buffer)
{
    const int bufferWidth = std::max(0, bufferRect.width());
    const int bufferHeight = std::max(0, bufferRect.height());
    const std::size_t planeSize = (std::size_t)bufferWidth * bufferHeight;
    const int nPlanes = planes.count();
    if ((planeSize == 0) || (nPlanes == 0)) {
        return;
    }

    // Per plane, the source channel it reads (-1 for one the source lacks, which reads as zero
    // and is still divided, so that a NaN divisor gives NaN there too) and whether it is divided.
    std::vector<int> srcIndex(nPlanes);
    std::vector<char> divided(nPlanes);
    for (int p = 0; p < nPlanes; ++p) {
        const int c = planes.channel[p];
        srcIndex[p] = job.src ? channelIndexForBit(job.srcNComps, pixelKernelChannelBit(job.nComps, c)) : -1;
        divided[p] = (c != job.skipChannel);
    }
    const bool nearest = (boundary == Boundary::Nearest);
    const RectI& b = job.srcBounds;
    const RectI& db = job.divisorBounds;
    const bool hasSource = job.src && !isEmptyRect(b);

    const std::function<void(int, int)> fillRows = [&](int firstRow, int endRow) {
        for (int row = firstRow; row < endRow; ++row) {
            if ((((row - firstRow) % kAbortCheckLines) == 0) && cancel.check()) {
                return;
            }
            float* out = buffer + (std::size_t)row * bufferWidth;
            const int y = bufferRect.y1 + row;
            int cy = y;
            bool rowInside = hasSource;
            if (hasSource && ((y < b.y1) || (y >= b.y2))) {
                if (nearest) {
                    cy = (y < b.y1) ? b.y1 : (b.y2 - 1);
                } else {
                    rowInside = false;
                }
            }
            const float* srcRow = rowInside ? (const float*)job.srcAccess->pixelAt(b.x1, cy) : NULL;
            if (!srcRow) {
                for (int p = 0; p < nPlanes; ++p) {
                    float* planeRow = out + (std::size_t)p * planeSize;
                    std::fill(planeRow, planeRow + bufferWidth, 0.f);
                }
                continue;
            }
            const float* divisorRow = NULL;
            if (job.divisor && (cy >= db.y1) && (cy < db.y2)) {
                divisorRow = (const float*)job.divisorAccess->pixelAt(db.x1, cy);
            }
            for (int i = 0; i < bufferWidth; ++i) {
                const int x = bufferRect.x1 + i;
                int cx = x;
                if ((x < b.x1) || (x >= b.x2)) {
                    if (!nearest) {
                        for (int p = 0; p < nPlanes; ++p) {
                            out[(std::size_t)p * planeSize + i] = 0.f;
                        }
                        continue;
                    }
                    cx = (x < b.x1) ? b.x1 : (b.x2 - 1);
                }
                const float* srcPix = srcRow + (std::size_t)(cx - b.x1) * job.srcNComps;
                float d = 1.f;
                bool divide = false;
                if (divisorRow && (cx >= db.x1) && (cx < db.x2)) {
                    d = divisorRow[(std::size_t)(cx - db.x1) * job.divisorNComps + job.divisorChannel];
                    divide = Image::unPremultDivisorIsUsable(d);
                }
                for (int p = 0; p < nPlanes; ++p) {
                    float v = (srcIndex[p] >= 0) ? srcPix[srcIndex[p]] : 0.f;
                    if (divide && divided[p]) {
                        v = Image::unPremultiplyValue(v, d);
                    }
                    out[(std::size_t)p * planeSize + i] = v;
                }
            }
        }
    };
    forEachLineChunk(bufferHeight, (std::size_t)bufferWidth * nPlanes, nThreads, cancel, fillRows);
} // fillBuffer

void
writeWindow(const PlaneJob& job,
            const ProcessedPlanes& planes,
            const MaskInput& mask,
            const WindowLayout& layout,
            int nThreads,
            RenderCancellation& cancel)
{
    const RectI& roi = layout.roi;
    const RectI& processWindow = layout.processWindow;
    const RectI& bufferRect = layout.bufferRect;
    const int width = roi.width();
    const int nComps = job.nComps;
    const int bufferWidth = std::max(0, bufferRect.width());
    const std::size_t planeSize = (std::size_t)bufferWidth * std::max(0, bufferRect.height());
    const bool doMask = mask.applied;
    const float mix = layout.mix;

    if ((width <= 0) || (roi.height() <= 0)) {
        return;
    }
    const LineBands bands = splitLines(roi.height(), (std::size_t)width, nThreads);
    const std::function<void(int)> writeBand = [&](int bandIndex) {
        const int bandY1 = roi.y1 + bandIndex * bands.linesPerBand;
        const int bandY2 = std::min(roi.y2, bandY1 + bands.linesPerBand);
        const std::size_t rowSize = (std::size_t)width * nComps;
        std::vector<float> sourceRow(rowSize);
        std::vector<float> maskRow(doMask ? width : 0);
        std::vector<float> divisorRow(job.divisor ? width : 0);

        std::vector<int> planeOfComp(nComps, -1);
        bool allProcessed = true;
        for (int c = 0; c < nComps; ++c) {
            planeOfComp[c] = (c < 4) ? planes.planeOfChannel[c] : -1;
            allProcessed = allProcessed && (planeOfComp[c] >= 0);
        }
        // A window row with no divisor, no mask and mix 1 has alpha 1 at every pixel, so a processed channel is
        // just its buffer value.
        const bool plainBlend = !job.divisor && !doMask && (mix == 1.f);
        const int fastX1 = std::max(roi.x1, processWindow.x1);
        const int fastX2 = std::min(roi.x2, processWindow.x2);
        const bool fastColumns = plainBlend && (fastX1 < fastX2) && (fastX1 >= bufferRect.x1) && (fastX2 <= bufferRect.x2);

        for (int y = bandY1; y < bandY2; ++y) {
            if ((((y - bandY1) % kAbortCheckLines) == 0) && cancel.check()) {
                return;
            }
            float* dstPix = (float*)job.dstAccess->pixelAt(roi.x1, y);
            if (!dstPix) {
                continue;
            }
            const bool rowProcessed = layout.processing && (y >= processWindow.y1) && (y < processWindow.y2);
            const bool rowInBuffer = (y >= bufferRect.y1) && (y < bufferRect.y2);
            if (rowProcessed && fastColumns && rowInBuffer) {
                const int left = fastX1 - roi.x1;
                const int right = roi.x2 - fastX2;
                if ((left > 0) || (right > 0) || !allProcessed) {
                    readSourceRow(job.src.get(), job.srcAccess.get(), job.srcBounds, job.srcNComps, roi.x1, y, width, nComps, &sourceRow[0]);
                }
                if (left > 0) {
                    std::copy(sourceRow.begin(), sourceRow.begin() + (std::size_t)left * nComps, dstPix);
                }
                const std::size_t offset = (std::size_t)(y - bufferRect.y1) * bufferWidth + (std::size_t)(fastX1 - bufferRect.x1);
                float* midPix = dstPix + (std::size_t)left * nComps;
                const int midWidth = fastX2 - fastX1;
                for (int c = 0; c < nComps; ++c) {
                    const int p = planeOfComp[c];
                    float* out = midPix + c;
                    if (p < 0) {
                        const float* in = &sourceRow[(std::size_t)left * nComps + c];
                        for (int i = 0; i < midWidth; ++i, out += nComps, in += nComps) {
                            *out = *in;
                        }
                        continue;
                    }
                    const float* in = layout.buffer + (std::size_t)p * planeSize + offset;
                    for (int i = 0; i < midWidth; ++i, out += nComps) {
                        *out = in[i];
                    }
                }
                if (right > 0) {
                    std::copy(sourceRow.begin() + (std::size_t)(width - right) * nComps, sourceRow.end(), midPix + (std::size_t)midWidth * nComps);
                }
                continue;
            }
            readSourceRow(job.src.get(), job.srcAccess.get(), job.srcBounds, job.srcNComps, roi.x1, y, width, nComps, &sourceRow[0]);
            if (!rowProcessed) {
                std::copy(sourceRow.begin(), sourceRow.end(), dstPix);
                continue;
            }
            if (job.divisor) {
                readChannelRow(job.divisor.get(), job.divisorAccess.get(), job.divisorBounds, job.divisorNComps, job.divisorChannel, roi.x1, y, width, 1.f, &divisorRow[0]);
            }
            if (doMask) {
                readChannelRow(mask.image.get(), mask.access.get(), mask.bounds, mask.nComps, mask.channel, roi.x1, y, width, 0.f, &maskRow[0]);
            }
            const std::size_t bufferRow = rowInBuffer ? (std::size_t)(y - bufferRect.y1) * bufferWidth : 0;
            for (int i = 0; i < width; ++i, dstPix += nComps) {
                const int x = roi.x1 + i;
                const float* srcPix = &sourceRow[(std::size_t)i * nComps];
                if ((x < processWindow.x1) || (x >= processWindow.x2)) {
                    std::copy(srcPix, srcPix + nComps, dstPix);
                    continue;
                }
                const bool inBuffer = rowInBuffer && (x >= bufferRect.x1) && (x < bufferRect.x2);
                const std::size_t offset = inBuffer ? bufferRow + (std::size_t)(x - bufferRect.x1) : 0;
                float alpha = mix;
                if (doMask) {
                    const float maskScale = mask.invert ? (1.f - maskRow[i]) : maskRow[i];
                    alpha = maskScale * mix;
                }
                for (int c = 0; c < nComps; ++c) {
                    const int p = planeOfComp[c];
                    if (p < 0) {
                        dstPix[c] = srcPix[c];
                        continue;
                    }
                    float v = inBuffer ? layout.buffer[(std::size_t)p * planeSize + offset] : 0.f;
                    if (job.divisor && (c != job.skipChannel)) {
                        v = Image::premultiplyValue(v, divisorRow[i]);
                    }
                    if (alpha == 0.f) {
                        v = srcPix[c];
                    } else if (alpha != 1.f) {
                        v = v * alpha + (1.f - alpha) * srcPix[c];
                    }
                    dstPix[c] = v;
                }
            }
        }
    };
    parallelForCancellable(bands.count, nThreads, cancel, writeBand);
} // writeWindow

bool
isUsableGpuView(const GpuImageView& view,
                int nComps)
{
    return view.buffer && (view.nComps == nComps) && (view.rowStrideFloats >= view.bounds.width() * nComps) && ((view.byteOffset % sizeof(float)) == 0);
}

GpuImageView
cropGpuView(const GpuImageView& view,
            const RectI& r)
{
    GpuImageView out;
    if ((r.x2 <= r.x1) || (r.y2 <= r.y1)) {
        return out;
    }
    out = view;
    out.bounds = r;
    out.byteOffset += ((uint64_t)(r.y1 - view.bounds.y1) * view.rowStrideFloats + (uint64_t)(r.x1 - view.bounds.x1) * view.nComps) * sizeof(float);

    return out;
}

#ifdef NATRON_HAVE_GPU
gpu::GpuBlurRect
toGpuBlurRect(const RectI& r)
{
    gpu::GpuBlurRect g;
    g.x1 = r.x1;
    g.y1 = r.y1;
    g.x2 = r.x2;
    g.y2 = r.y2;

    return g;
}

gpu::GpuBlurImage
toGpuBlurImage(const GpuImageView& view)
{
    gpu::GpuBlurImage g;
    g.buffer = view.buffer;
    g.offsetFloats = view.byteOffset / sizeof(float);
    g.bounds = toGpuBlurRect(view.bounds);
    g.strideFloats = (uint32_t)view.rowStrideFloats;

    return g;
}
#endif // NATRON_HAVE_GPU
} // namespace PassExec

using namespace PassExec;

namespace {
// The effect's own region of definition in pixels at scale: the extent its passes run over.
bool
effectRoDPixels(EffectInstance& effect,
                double time,
                const RenderScale& scale,
                ViewIdx view,
                RectI* out)
{
    RectD rod;
    bool isProjectFormat = false;

    if (effect.getRegionOfDefinition_public(effect.getRenderHash(), time, scale, view, &rod, &isProjectFormat) == eStatusFailed) {
        return false;
    }
    *out = toPixelEnclosing(rod, scale, effect.getAspectRatio(-1));

    return true;
}

void
runLinePass(const Pass& pass,
            const PassScratch& scratch,
            int nThreads,
            RenderCancellation& cancel)
{
    float* const data = scratch.buffers[pass.slot];
    const int width = scratch.width;
    const int height = scratch.height;
    const int nPlanes = scratch.nPlanes;
    const std::size_t planeSize = scratch.planeSize;
    const std::size_t taps = std::max((std::size_t)1, pass.tapsPerSample);

    if (pass.kind == PassKind::LineX) {
        const std::function<void(int, int)> rows = [&](int first, int end) {
            const PassLineFn fn = pass.lineWorker();
            for (int line = first; line < end; ++line) {
                if ((((line - first) % kAbortCheckLines) == 0) && cancel.check()) {
                    return;
                }
                const int p = line / height;
                const int row = line % height;
                fn(data + (std::size_t)p * planeSize + (std::size_t)row * width, width, 1, 1);
            }
        };
        forEachLineChunk(nPlanes * height, (std::size_t)width * taps, nThreads, cancel, rows);

        return;
    }

    const int block = std::max(1, pass.columnBlock);
    if (block > 1) {
        const int blocksPerPlane = (width + block - 1) / block;
        const std::function<void(int, int)> columnBlocks = [&](int first, int end) {
            const PassLineFn fn = pass.lineWorker();
            for (int line = first; line < end; ++line) {
                if (cancel.check()) {
                    return;
                }
                const int p = line / blocksPerPlane;
                const int column = (line % blocksPerPlane) * block;
                fn(data + (std::size_t)p * planeSize + column, height, width, std::min(block, width - column));
            }
        };
        forEachLineChunk(nPlanes * blocksPerPlane, (std::size_t)height * block * taps, nThreads, cancel, columnBlocks);

        return;
    }

    // A worker that filters one column at a time gets each column as a contiguous copy, taken with
    // its neighbours a cache line at a time. Walked in place, a column reads one line per row and
    // relies on the cache keeping them for the next column; at a power-of-two width on huge pages
    // all those lines fall in the same cache sets and are evicted before they are reused.
    const int kTransposeColumns = 16;
    const int groupsPerPlane = (width + kTransposeColumns - 1) / kTransposeColumns;
    const std::function<void(int, int)> columnGroups = [&](int first, int end) {
        const PassLineFn fn = pass.lineWorker();
        std::vector<float> columns((std::size_t)kTransposeColumns * height);
        for (int group = first; group < end; ++group) {
            if (cancel.check()) {
                return;
            }
            const int p = group / groupsPerPlane;
            const int column = (group % groupsPerPlane) * kTransposeColumns;
            const int count = std::min(kTransposeColumns, width - column);
            float* const origin = data + (std::size_t)p * planeSize + column;
            for (int y = 0; y < height; ++y) {
                const float* row = origin + (std::size_t)y * width;
                for (int c = 0; c < count; ++c) {
                    columns[(std::size_t)c * height + y] = row[c];
                }
            }
            for (int c = 0; c < count; ++c) {
                fn(&columns[(std::size_t)c * height], height, 1, 1);
            }
            for (int y = 0; y < height; ++y) {
                float* row = origin + (std::size_t)y * width;
                for (int c = 0; c < count; ++c) {
                    row[c] = columns[(std::size_t)c * height + y];
                }
            }
        }
    };
    forEachLineChunk(nPlanes * groupsPerPlane, (std::size_t)height * kTransposeColumns * taps, nThreads, cancel, columnGroups);
} // runLinePass

void
runRowPass(const Pass& pass,
           const PassScratch& scratch,
           int nThreads,
           RenderCancellation& cancel)
{
    const int lines = pass.columns ? scratch.width : scratch.height;
    const int length = pass.columns ? scratch.height : scratch.width;
    const int nLines = pass.perPlane ? scratch.nPlanes * lines : lines;
    const std::size_t pixelsPerLine = (std::size_t)length * (pass.perPlane ? 1 : scratch.nPlanes);
    const std::function<void(int, int)> rows = [&](int first, int end) {
        const PassRowFn fn = pass.rowWorker(scratch);
        for (int line = first; line < end; ++line) {
            if ((((line - first) % kAbortCheckLines) == 0) && cancel.check()) {
                return;
            }
            fn(line);
        }
    };
    forEachLineChunk(nLines, pixelsPerLine, nThreads, cancel, rows);
}

void
runPass(const Pass& pass,
        const PassScratch& scratch,
        int nThreads,
        RenderCancellation& cancel)
{
    switch (pass.kind) {
    case PassKind::LineX:
    case PassKind::LineY:
        if (pass.lineWorker) {
            runLinePass(pass, scratch, nThreads, cancel);
        }
        break;
    case PassKind::Gather:
    case PassKind::Point:
        if (pass.rowWorker) {
            runRowPass(pass, scratch, nThreads, cancel);
        }
        break;
    case PassKind::Resample:
    case PassKind::Downsample:
        break;
    }
}
} // anonymous namespace

ScratchSetRegistry&
ScratchSetRegistry::global()
{
    // Never destroyed: render threads may still end their renders during static teardown.
    static ScratchSetRegistry* registry = new ScratchSetRegistry;

    return *registry;
}

ScratchSetHolder::ScratchSetHolder(HostScratchFloats& source,
                                   std::vector<HostScratchFloats>& extra,
                                   ScratchSetRegistry& registry)
    : _source(source)
    , _extra(extra)
    , _registry(registry)
{
    const std::lock_guard<std::mutex> lock(_registry.mutex);
    ++_registry.active;
}

ScratchSetHolder::~ScratchSetHolder()
{
    std::unique_lock<std::mutex> lock(_registry.mutex);
    if (_registry.active > 1) {
        // Uncounted before the slow unmapping, outside the lock: another holder ending meanwhile
        // must see itself as the last and pool its set, and nothing this one frees is pooled.
        --_registry.active;
        lock.unlock();
        if (_registry.beforeRelease) {
            _registry.beforeRelease();
        }
        _source.discard();
        for (HostScratchFloats& slot : _extra) {
            slot.discard();
        }

        return;
    }
    // Pooled under the lock: a holder registering meanwhile would allocate a second set, and
    // pool it in turn when it ends.
    if (_registry.beforeRelease) {
        _registry.beforeRelease();
    }
    _source.recycle();
    for (HostScratchFloats& slot : _extra) {
        slot.recycle();
    }
    --_registry.active;
}

Pass
Pass::lineX(int halo,
            int grow,
            std::function<PassLineFn()> worker)
{
    Pass pass;

    pass.kind = PassKind::LineX;
    pass.haloX = halo;
    pass.growX = grow;
    pass.lineWorker = std::move(worker);

    return pass;
}

Pass
Pass::lineY(int halo,
            int grow,
            std::function<PassLineFn()> worker)
{
    Pass pass;

    pass.kind = PassKind::LineY;
    pass.haloY = halo;
    pass.growY = grow;
    pass.lineWorker = std::move(worker);

    return pass;
}

Pass
Pass::gather(int haloX,
             int haloY,
             bool perPlane,
             std::function<PassRowFn(const PassScratch&)> worker)
{
    Pass pass;

    pass.kind = PassKind::Gather;
    pass.haloX = haloX;
    pass.haloY = haloY;
    pass.growX = haloX;
    pass.growY = haloY;
    pass.perPlane = perPlane;
    pass.rowWorker = std::move(worker);

    return pass;
}

Pass
Pass::point(bool perPlane,
            std::function<PassRowFn(const PassScratch&)> worker)
{
    Pass pass;

    pass.kind = PassKind::Point;
    pass.perPlane = perPlane;
    pass.rowWorker = std::move(worker);

    return pass;
}

PassExtent
passListExtent(const PassList& list)
{
    PassExtent extent;

    for (const Pass& pass : list.passes) {
        extent.haloX += pass.haloX;
        extent.haloY += pass.haloY;
        extent.growX += pass.growX;
        extent.growY += pass.growY;
    }

    return extent;
}

RectI
passListSourceRoI(const RectI& rect,
                  const PassList& list)
{
    const PassExtent extent = passListExtent(list);

    return RectI(rect.x1 - extent.haloX, rect.y1 - extent.haloY, rect.x2 + extent.haloX, rect.y2 + extent.haloY);
}

RectI
passListRoD(const RectI& srcRoD,
            const PassList& list)
{
    if (isEmptyRect(srcRoD)) {
        return srcRoD;
    }
    const PassExtent extent = passListExtent(list);

    return RectI(srcRoD.x1 - extent.growX, srcRoD.y1 - extent.growY, srcRoD.x2 + extent.growX, srcRoD.y2 + extent.growY);
}

std::optional<GpuCostHint>
passListCostHint(const PassList& list)
{
    std::optional<GpuCostHint> hint;

    for (const Pass& pass : list.passes) {
        if ((pass.sigma <= 0.) && !pass.recursive) {
            continue;
        }
        if (!hint) {
            hint = GpuCostHint();
        }
        hint->sigma = std::max(hint->sigma, pass.sigma);
        hint->iir = hint->iir || pass.recursive;
    }

    return hint;
}

bool
isRunnablePassList(const PassList& list)
{
    if ((list.scratchSlots < 1) || (list.resultSlot < 0) || (list.resultSlot >= list.scratchSlots)) {
        return false;
    }
    for (const Pass& pass : list.passes) {
        if ((pass.kind == PassKind::Resample) || (pass.kind == PassKind::Downsample)) {
            return false;
        }
        if ((pass.slot < 0) || (pass.slot >= list.scratchSlots)) {
            return false;
        }
    }

    return true;
}

StatusEnum
runPassesCpu(EffectInstance& effect,
             const StagePlan& plan,
             const EffectInstance::RenderActionArgs& args,
             const PassList& list)
{
    if (!isRunnablePassList(list)) {
        return eStatusFailed;
    }
    const int sourceInput = plan.sourceInput;

    // Every image is fetched before any is locked: fetching renders upstream, which may write
    // into a cached image this render would otherwise already hold a read lock on.
    const bool doMask = plan.applyMask;
    ImagePtr mask;
    if (doMask) {
        if (plan.maskChannel >= 0) {
            RectI maskRoI;
            mask = effect.getImage(plan.maskInput, args.time, args.mappedScale, args.view, NULL, &plan.maskLayer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &maskRoI);
        }
        if (mask && ((plan.maskChannel >= (int)mask->getComponentsCount()))) {
            mask.reset();
        }
    }

    const ImageLayerDesc& divisorLayer = plan.divisorLayer;
    const int divisorChannel = plan.divisorChannel;
    std::vector<PlaneJob> jobs;
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        PlaneJob job;
        job.dst = it->second;
        if (!job.dst) {
            continue;
        }
        job.nComps = (int)job.dst->getComponentsCount();
        job.channels = processedBitsForImage(it->first, job.nComps, args.processChannels);

        ImageLayerDesc sourceLayer;
        if ((sourceInput >= 0) && effect.getInput(sourceInput) && effect.resolveInputPlaneForRender(sourceInput, args.time, args.view, &sourceLayer, NULL)) {
            RectI sourceRoI;
            // Mapped to the clip's components, which is the layout the output plane is rendered in.
            job.src = effect.getImage(sourceInput, args.time, args.mappedScale, args.view, NULL, &sourceLayer, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &sourceRoI);
        }
        if (job.src && (divisorChannel >= 0)) {
            RectI divisorRoI;
            job.divisor = effect.getImage(sourceInput, args.time, args.mappedScale, args.view, NULL, &divisorLayer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &divisorRoI);
            if (job.divisor && (divisorChannel < (int)job.divisor->getComponentsCount())) {
                job.divisorChannel = divisorChannel;
                job.skipChannel = Node::getUnPremultSkipChannel(it->first.isColorLayer() ? job.dst->getComponents() : it->first, divisorLayer, divisorChannel);
            } else {
                job.divisor.reset();
            }
        }
        if (!isFloatImage(job.dst) || !isFloatImage(job.src) || !isFloatImage(job.divisor)) {
            return eStatusFailed;
        }
        jobs.push_back(job);
    }
    if (!isFloatImage(mask)) {
        return eStatusFailed;
    }

    const RectI& roi = args.roi;
    if ((roi.width() <= 0) || (roi.height() <= 0)) {
        return eStatusOK;
    }

    // The passes run over the window plus the halo, clipped to this effect's own region of
    // definition, as CImgFilterPluginHelper::render() clips its source region of interest.
    RectI dstRoDPixel;
    if (!effectRoDPixels(effect, args.time, args.mappedScale, args.view, &dstRoDPixel)) {
        return eStatusFailed;
    }

    MaskInput maskSource;
    maskSource.applied = doMask;
    maskSource.invert = plan.maskInvert;
    maskSource.image = mask;
    maskSource.channel = plan.maskChannel;
    maskSource.lock();
    for (std::size_t j = 0; j < jobs.size(); ++j) {
        jobs[j].lock();
    }

    // The whole window is processed whatever the mask, so that a pixel's value does not depend on
    // where the mask is zero; the mask only blends in writeWindow().
    RectI processWindow = roi;
    if (plan.mix == 0.) {
        processWindow.x2 = processWindow.x1;
        processWindow.y2 = processWindow.y1;
    }
    const bool processing = !isEmptyRect(processWindow);

    RectI bufferRect(0, 0, 0, 0);
    if (processing) {
        intersectRects(passListSourceRoI(processWindow, list), dstRoDPixel, &bufferRect);
    }
    const int bufferWidth = std::max(0, bufferRect.width());
    const int bufferHeight = std::max(0, bufferRect.height());
    const std::size_t planeSize = (std::size_t)bufferWidth * bufferHeight;

    const int nThreads = appPTR->getNCPUsAvailableForEffect();
    RenderCancellation cancel(&effect);

    // All scratch comes uninitialised from the pooled huge-page allocator, and is released when the
    // function returns. Zeroing it here would fault every page in on this thread alone; instead
    // the threads that first write a page fault it in, and a re-render takes pooled pages that are
    // already resident. Kept across jobs so that a later plane reuses the pages the first one touched.
    //
    // Slot 0 is written whole by fillBuffer(): the window, the halo and the boundary alike.
    // The other slots are each written whole by the first pass that touches them, before any pass
    // reads them; a pass that does not write every element must not be added without clearing the
    // rest here. See the pass lists with more than one slot, EdgeDetect's:
    //   slots 1 and 2: copyWorker() copies slot 0 over every row of every plane, or stencilWorker()
    //                  writes every element of both, and combineWorker() reads only after that;
    //   slot 3:        suppressionWorker() writes every pixel of every plane, border included;
    // and IBlur's, whose box passes alternate between slots 0 and 1, each writing every pixel of
    // every plane of the slot the next one reads.
    HostScratchFloats sourceStorage;
    std::vector<HostScratchFloats> extraStorage(list.scratchSlots - 1);
    // Playback overlaps two frames, and pooling both sets would keep both resident.
    const ScratchSetHolder scratchHolder(sourceStorage, extraStorage, ScratchSetRegistry::global());
    for (std::size_t j = 0; j < jobs.size(); ++j) {
        const PlaneJob& job = jobs[j];
        const ProcessedPlanes planes(job);
        const int nPlanes = planes.count();
        const std::size_t bufferSize = processing ? planeSize * nPlanes : 0;
        float* buffer = NULL;
        if (bufferSize) {
            sourceStorage.reserve(bufferSize);
            buffer = sourceStorage.data();
        }

        PassScratch scratch;
        if (buffer) {
            scratch.rect = bufferRect;
            scratch.width = bufferWidth;
            scratch.height = bufferHeight;
            scratch.nPlanes = nPlanes;
            scratch.planeSize = planeSize;
            scratch.buffers.push_back(buffer);
            for (HostScratchFloats& slot : extraStorage) {
                slot.reserve(bufferSize);
                scratch.buffers.push_back(slot.data());
            }

            fillBuffer(job, planes, bufferRect, list.boundary, nThreads, cancel, buffer);
            if (cancel.check()) {
                return eStatusOK;
            }
            for (const Pass& pass : list.passes) {
                if (!pass.hasWork()) {
                    continue;
                }
                runPass(pass, scratch, nThreads, cancel);
                if (cancel.check()) {
                    return eStatusOK;
                }
            }
        }

        WindowLayout layout;
        layout.roi = roi;
        layout.processWindow = processWindow;
        layout.processing = processing;
        layout.bufferRect = bufferRect;
        layout.buffer = buffer ? scratch.buffers[list.resultSlot] : NULL;
        layout.mix = (float)plan.mix;
        writeWindow(job, planes, maskSource, layout, nThreads, cancel);
        if (cancel.check()) {
            return eStatusOK;
        }
    }

    return eStatusOK;
} // runPassesCpu

bool
canRecordPassesGpu(const StagePlan& plan,
                   const GpuQuery& query,
                   const PassList& list)
{
#ifdef NATRON_HAVE_GPU
    if (!canRunStagesGpu(plan, query) || !isRunnablePassList(list)) {
        return false;
    }

    return list.empty() || (bool)list.gpu;
#else
    Q_UNUSED(plan);
    Q_UNUSED(query);
    Q_UNUSED(list);

    return false;
#endif
}

StatusEnum
recordPassesGpu(EffectInstance& effect,
                const StagePlan& plan,
                const GpuRenderArgs& args,
                const PassList& list)
{
#ifdef NATRON_HAVE_GPU
    const GpuQuery& query = args.query;
    const int sourceInput = plan.sourceInput;
    if (!canRecordPassesGpu(plan, query, list) || (sourceInput < 0) || (sourceInput >= (int)args.inputs.size())) {
        return eStatusFailed;
    }
    const GpuImageView& src = args.inputs[sourceInput];
    const GpuImageView& dst = args.output;
    const int nComps = query.nComps;
    const RectI& rect = args.outputRect;
    EffectInstancePtr input = effect.getInput(sourceInput);
    if (!input || !isUsableGpuView(src, nComps) || !isUsableGpuView(dst, nComps) || isEmptyRect(rect) || !dst.bounds.contains(rect)) {
        return eStatusFailed;
    }

    const double time = query.time;
    const ViewIdx view = query.view;
    const RenderScale& scale = query.mappedScale;

    // runPassesCpu() runs the passes over the window plus its halo clipped to this effect's region
    // of definition, and fillBuffer() resolves the source against the input image.
    RectI rodPixel;
    if (!effectRoDPixels(effect, time, scale, view, &rodPixel)) {
        return eStatusFailed;
    }
    const RectI sourceRoI = passListSourceRoI(rect, list);
    RectI domain;
    intersectRects(sourceRoI, rodPixel, &domain);

    const RenderScale inputScale = input->supportsRenderScale() ? scale : RenderScale::identity;
    RectD srcRoD;
    bool isProjectFormat = false;
    if (input->getRegionOfDefinition_public(input->getRenderHash(), time, inputScale, view, &srcRoD, &isProjectFormat) == eStatusFailed) {
        return eStatusFailed;
    }
    RectI valid;
    intersectRects(src.bounds, toPixelEnclosing(srcRoD, scale, effect.getAspectRatio(sourceInput)), &valid);

    int divisor = -1;
    if (!stageGpuDivisorComponent(plan, query, &divisor)) {
        return eStatusFailed;
    }
    // With a mix of 0 runPassesCpu() processes nothing, and the finish passes the source through.
    const bool filtering = !list.empty() && (plan.mix != 0.);
    const bool finish = !filtering || plan.applyMask || (plan.mix != 1.) || !plan.processChannels.all() || (divisor >= 0);

    // The source view may extend past the input's region of definition; runPassesCpu() reads the
    // divisor there as 1, as the wrap does outside its source view.
    GpuRenderArgs wrapArgs(args);
    wrapArgs.inputs[sourceInput] = cropGpuView(src, valid);

    gpu::GpuRecordContext& ctx = args.context;
    GpuImageView source = wrapArgs.inputs[sourceInput];
    RectI sourceValid = valid;
    if ((plan.stages.has(Stage::Premult) && (divisor >= 0)) || !filtering) {
        // The source divided where the divisor is usable and zero outside the input, over the
        // extent the passes read, or over the window when nothing filters it.
        const RectI dividedRect = filtering ? domain : rect;
        source = GpuImageView();
        sourceValid = RectI(0, 0, 0, 0);
        if (!isEmptyRect(dividedRect)) {
            source.bounds = dividedRect;
            source.nComps = nComps;
            source.rowStrideFloats = dividedRect.width() * nComps;
            if (!ctx.scratch((VkDeviceSize)dividedRect.area() * nComps * sizeof(float), source.buffer) || !source.buffer || (recordStagesGpuDivide(plan, wrapArgs, source) != eStatusOK)) {
                return eStatusFailed;
            }
            ctx.computeBarrier();
            sourceValid = dividedRect;
        }
    }
    if (!filtering) {
        return recordStagesGpuFinish(plan, wrapArgs, source);
    }

    GpuImageView filtered = dst;
    if (finish) {
        filtered = GpuImageView();
        filtered.bounds = rect;
        filtered.nComps = nComps;
        filtered.rowStrideFloats = rect.width() * nComps;
        if (!ctx.scratch((VkDeviceSize)rect.area() * nComps * sizeof(float), filtered.buffer) || !filtered.buffer) {
            return eStatusFailed;
        }
    }

    PassGpuIO io(ctx);
    io.source = source;
    io.sourceValid = sourceValid;
    io.filtered = filtered;
    io.output = rect;
    io.domain = domain;
    io.rod = rodPixel;
    io.haloX = rect.x1 - sourceRoI.x1;
    io.haloY = rect.y1 - sourceRoI.y1;
    io.nComps = nComps;
    io.channels = plan.processChannels;
    // The other inputs as the CPU passes read them through getImage(): over their own region of
    // definition only.
    io.inputs.resize(args.inputs.size());
    for (int i = 0; i < (int)args.inputs.size(); ++i) {
        const GpuImageView& in = args.inputs[i];
        EffectInstancePtr other = (i == sourceInput) ? EffectInstancePtr() : effect.getInput(i);
        if (!other || !in.buffer || (in.nComps <= 0)) {
            continue;
        }
        const RenderScale otherScale = other->supportsRenderScale() ? scale : RenderScale::identity;
        RectD otherRoD;
        bool otherIsProjectFormat = false;
        if (other->getRegionOfDefinition_public(other->getRenderHash(), time, otherScale, view, &otherRoD, &otherIsProjectFormat) == eStatusFailed) {
            return eStatusFailed;
        }
        RectI otherValid;
        intersectRects(in.bounds, toPixelEnclosing(otherRoD, scale, effect.getAspectRatio(i)), &otherValid);
        io.inputs[i] = cropGpuView(in, otherValid);
    }
    if (list.gpu(io) != eStatusOK) {
        return eStatusFailed;
    }
    if (!finish) {
        return eStatusOK;
    }
    ctx.computeBarrier();

    return recordStagesGpuFinish(plan, wrapArgs, filtered);
#else
    Q_UNUSED(effect);
    Q_UNUSED(plan);
    Q_UNUSED(args);
    Q_UNUSED(list);

    return eStatusFailed;
#endif
} // recordPassesGpu

NATRON_NAMESPACE_EXIT
