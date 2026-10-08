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

#include "SpatialFilterSupport.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/RenderScale.h"

NATRON_NAMESPACE_ENTER

namespace SpatialFilter {
namespace {
    // Below this many pixels a chunk of lines is not worth handing to another thread.
    const std::size_t kMinChunkPixels = 16384;
    // Chunks per available thread, so a helper that starts late still finds work left.
    const int kChunksPerThread = 4;

    // The mask channel at (x, y), or null outside the mask image.
    const float*
    maskValueAt(const Image::ReadAccess& access,
                const RectI& bounds,
                int channel,
                int x,
                int y)
    {
        if ((x < bounds.x1) || (x >= bounds.x2) || (y < bounds.y1) || (y >= bounds.y2)) {
            return NULL;
        }
        const float* pix = (const float*)access.pixelAt(x, y);

        return pix ? (pix + channel) : NULL;
    }

    // CImgFilterPluginHelperBase::maskLineIsZero().
    bool
    maskRowIsZero(const Image::ReadAccess& access,
                  const RectI& bounds,
                  int channel,
                  int x1,
                  int x2,
                  int y,
                  bool maskInvert)
    {
        if (maskInvert) {
            if ((y < bounds.y1) || (bounds.y2 <= y) || (x1 < bounds.x1) || (bounds.x2 <= x2)) {
                return false;
            }
            for (int x = x1; x < x2; ++x) {
                const float* p = maskValueAt(access, bounds, channel, x, y);
                if (!p || (*p != 1.)) {
                    return false;
                }
            }
        } else {
            if ((y < bounds.y1) || (bounds.y2 <= y)) {
                return true;
            }
            x1 = std::max(x1, bounds.x1);
            x2 = std::min(x2, bounds.x2);
            for (int x = x1; x < x2; ++x) {
                const float* p = maskValueAt(access, bounds, channel, x, y);
                if (p && (*p != 0.)) {
                    return false;
                }
            }
        }

        return true;
    }

    // CImgFilterPluginHelperBase::maskColumnIsZero().
    bool
    maskColumnIsZero(const Image::ReadAccess& access,
                     const RectI& bounds,
                     int channel,
                     int x,
                     int y1,
                     int y2,
                     bool maskInvert)
    {
        if (maskInvert) {
            if ((x < bounds.x1) || (bounds.x2 <= x) || (y1 < bounds.y1) || (bounds.y2 <= y2)) {
                return false;
            }
            for (int y = y1; y < y2; ++y) {
                const float* p = maskValueAt(access, bounds, channel, x, y);
                if (!p || (*p != 1.)) {
                    return false;
                }
            }
        } else {
            if ((x < bounds.x1) || (bounds.x2 <= x)) {
                return true;
            }
            y1 = std::max(y1, bounds.y1);
            y2 = std::min(y2, bounds.y2);
            for (int y = y1; y < y2; ++y) {
                const float* p = maskValueAt(access, bounds, channel, x, y);
                if (p && (*p != 0.)) {
                    return false;
                }
            }
        }

        return true;
    }
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

RectI
shrinkToMask(const RectI& window,
             const MaskInput& mask)
{
    RectI r = window;
    if (!mask.access) {
        return r;
    }
    const Image::ReadAccess& access = *mask.access;
    while ((r.y2 > r.y1) && maskRowIsZero(access, mask.bounds, mask.channel, r.x1, r.x2, r.y2 - 1, mask.invert)) {
        --r.y2;
    }
    while ((r.y2 > r.y1) && maskRowIsZero(access, mask.bounds, mask.channel, r.x1, r.x2, r.y1, mask.invert)) {
        ++r.y1;
    }
    while ((r.x2 > r.x1) && maskColumnIsZero(access, mask.bounds, mask.channel, r.x1, r.y1, r.y2, mask.invert)) {
        ++r.x1;
    }
    while ((r.x2 > r.x1) && maskColumnIsZero(access, mask.bounds, mask.channel, r.x2 - 1, r.y1, r.y2, mask.invert)) {
        --r.x2;
    }

    return r;
}

ProcessedPlanes::ProcessedPlanes(const PlaneJob& job)
    : channel()
    , alphaPlane(-1)
{
    for (int c = 0; c < 4; ++c) {
        planeOfChannel[c] = -1;
    }
    for (int c = 0; (c < job.nComps) && (c < 4); ++c) {
        const int bit = pixelKernelChannelBit(job.nComps, c);
        if (job.channels[bit]) {
            planeOfChannel[c] = (int)channel.size();
            if (bit == 3) {
                alphaPlane = (int)channel.size();
            }
            channel.push_back(c);
        }
    }
}

void
forEachLineChunk(int nLines,
                 std::size_t pixelsPerLine,
                 int nThreads,
                 RenderCancellation& cancel,
                 const std::function<void(int, int)>& body)
{
    if (nLines <= 0) {
        return;
    }
    std::size_t nChunks = 1;
    if (nThreads > 1) {
        nChunks = std::min((std::size_t)nThreads * kChunksPerThread, ((std::size_t)nLines * pixelsPerLine) / kMinChunkPixels);
        nChunks = std::max((std::size_t)1, std::min(nChunks, (std::size_t)nLines));
    }
    const int linesPerChunk = (int)(((std::size_t)nLines + nChunks - 1) / nChunks);
    const int realChunks = (nLines + linesPerChunk - 1) / linesPerChunk;
    const std::function<void(int)> chunk = [&](int i) {
        const int first = i * linesPerChunk;
        body(first, std::min(nLines, first + linesPerChunk));
    };
    parallelForCancellable(realChunks, nThreads, cancel, chunk);
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
} // namespace SpatialFilter

NATRON_NAMESPACE_EXIT
