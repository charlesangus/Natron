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

#ifndef Engine_Nodes_Filter_SpatialFilterSupport_h
#define Engine_Nodes_Filter_SpatialFilterSupport_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <algorithm>
#include <bitset>
#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/Image/NativeImageEffect.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The render plumbing Blur, ErodeDilate and EdgeDetect share, ported from openfx-misc's
 * CImgFilterPluginHelper: the source over a planar buffer, the filtered extent shrunk to the
 * mask, whole-line parallelism, and the write back with premultiply, mask and mix.
 **/
namespace SpatialFilter {
// How many lines a worker filters or writes between two abort checks.
const int kAbortCheckLines = 16;

/**
 * @brief The index in an nComps-channel pixel of the channel on colour bit `bit`, or -1.
 **/
int channelIndexForBit(int nComps, int bit) WARN_UNUSED_RETURN;

/**
 * @brief The bits processed in a dstNComps-channel image holding `plane`. A one-channel plane
 * rendered into a wider image sits where EffectInstance's copy back reads it: alpha for a colour
 * plane, channel 0 otherwise.
 **/
std::bitset<4> processedBitsForImage(const ImageLayerDesc& plane, int dstNComps, const std::bitset<4>& planeBits) WARN_UNUSED_RETURN;

bool isFloatImage(const ImagePtr& image) WARN_UNUSED_RETURN;

bool isEmptyRect(const RectI& r) WARN_UNUSED_RETURN;

bool isEmptyRect(const RectD& r) WARN_UNUSED_RETURN;

/**
 * @brief OFX::Coords::toPixelEnclosing().
 **/
RectI toPixelEnclosing(const RectD& r, const RenderScale& scale, double par) WARN_UNUSED_RETURN;

/**
 * @brief OFX::Coords::toCanonical().
 **/
RectD toCanonical(const RectI& r, const RenderScale& scale, double par) WARN_UNUSED_RETURN;

/**
 * @brief OFX::Coords::rectIntersection(): rectangles that merely touch still intersect, and a
 * failed intersection leaves an empty rectangle at the origin.
 **/
bool intersectRects(const RectI& a, const RectI& b, RectI* out);

/**
 * @brief Copies `width` pixels of row y starting at x0 into row (nComps floats per pixel),
 * writing zero wherever the image has no pixel. An image in another layout is mapped channel by
 * colour bit, a channel it lacks reading as zero.
 **/
void readSourceRow(const Image* image,
                   const Image::ReadAccess* access,
                   const RectI& bounds,
                   int srcNComps,
                   int x0,
                   int y,
                   int width,
                   int nComps,
                   float* row);

/**
 * @brief One value per pixel of `channel` of row y, `fill` wherever the image has no pixel.
 **/
void readChannelRow(const Image* image,
                    const Image::ReadAccess* access,
                    const RectI& bounds,
                    int imageNComps,
                    int channel,
                    int x0,
                    int y,
                    int width,
                    float fill,
                    float* row);

/**
 * @brief One output plane of a render with the images it reads. Image::getBounds() takes the
 * image's lock, which a pool thread must never ask for: behind a writer waiting on an image this
 * render holds for reading, it blocks forever. lock() reads the bounds and takes the accesses on
 * the calling thread, and the pool threads only compute pixel addresses through them.
 **/
struct PlaneJob {
    ImagePtr dst;
    std::bitset<4> channels;
    ImagePtr src;
    ImagePtr divisor;
    int divisorChannel;
    int skipChannel;
    int nComps;
    int srcNComps;
    int divisorNComps;
    RectI srcBounds;
    RectI divisorBounds;
    std::shared_ptr<Image::ReadAccess> srcAccess;
    std::shared_ptr<Image::ReadAccess> divisorAccess;
    std::shared_ptr<Image::WriteAccess> dstAccess;

    PlaneJob();

    void lock();
};

/**
 * @brief The mask a render reads, locked on the calling thread like PlaneJob. `applied` is
 * NativeImageEffect::isMaskApplied(): with no image, an applied mask reads as zero everywhere.
 **/
struct MaskInput {
    bool applied;
    bool invert;
    ImagePtr image;
    int channel;
    int nComps;
    RectI bounds;
    std::shared_ptr<Image::ReadAccess> access;

    MaskInput();

    void lock();
};

/**
 * @brief window shrunk past its outer rows and columns where the mask is zero, as
 * CImgFilterPluginHelper::render() does with maskLineIsZero() and maskColumnIsZero(); that
 * includes their treatment of the last column of the mask image as outside it when the mask is
 * inverted, which decides the extent the filters run over and so must match. window itself
 * without a locked mask image.
 **/
RectI shrinkToMask(const RectI& window, const MaskInput& mask) WARN_UNUSED_RETURN;

/**
 * @brief The processed channels of job, one buffer plane each in channel order, the way
 * CImgFilterPluginHelper extracts them. planeOfChannel maps a channel to its plane or -1, and
 * alphaPlane is the plane of colour bit 3, or -1.
 **/
struct ProcessedPlanes {
    std::vector<int> channel;
    int planeOfChannel[4];
    int alphaPlane;

    explicit ProcessedPlanes(const PlaneJob& job);

    int count() const WARN_UNUSED_RETURN
    {
        return (int)channel.size();
    }
};

/**
 * @brief Runs body(first, end) over [0, nLines) split into contiguous chunks on the global
 * pool, stopping once cancel is set. Each line is processed whole by one call, so the result
 * does not depend on the split.
 **/
void forEachLineChunk(int nLines,
                      std::size_t pixelsPerLine,
                      int nThreads,
                      RenderCancellation& cancel,
                      const std::function<void(int, int)>& body);

/**
 * @brief How the source reads outside its image when filling the buffer.
 **/
enum class Boundary {
    // Zero (Dirichlet).
    Zero,
    // The nearest pixel of the image (Neumann).
    Nearest
};

/**
 * @brief Fills buffer, one bufferRect-sized plane per processed channel, with the source of job,
 * outside its image by boundary, divided by the "(Un)premult by" channel where that is usable.
 **/
void fillBuffer(const PlaneJob& job,
                const ProcessedPlanes& planes,
                const RectI& bufferRect,
                Boundary boundary,
                int nThreads,
                RenderCancellation& cancel,
                float* buffer);

/**
 * @brief What writeWindow() writes: roi, of which processWindow (when processing) is taken from
 * the planar buffer over bufferRect and the rest passed through from the source.
 **/
struct WindowLayout {
    RectI roi;
    RectI processWindow;
    bool processing;
    RectI bufferRect;
    const float* buffer;
    float mix;
};

/**
 * @brief Writes job's window in row bands: each processed channel of the processed window is
 * output(buffer value, plane) multiplied back by the divisor, then blended with the undivided
 * source by mask x mix; unprocessed channels and the pixels outside the processed window pass
 * through. A buffer value outside bufferRect reads as zero.
 **/
template <class Output>
void
writeWindow(const PlaneJob& job,
            const ProcessedPlanes& planes,
            const MaskInput& mask,
            const WindowLayout& layout,
            int nThreads,
            RenderCancellation& cancel,
            const Output& output)
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

    std::vector<RectI> bandRects;
    NativeImageEffect::makeRowBands(roi, nThreads, &bandRects);
    const std::function<void(int)> writeBand = [&](int bandIndex) {
        const RectI& band = bandRects[bandIndex];
        const std::size_t rowSize = (std::size_t)width * nComps;
        std::vector<float> sourceRow(rowSize);
        std::vector<float> maskRow(doMask ? width : 0);
        std::vector<float> divisorRow(job.divisor ? width : 0);

        for (int y = band.y1; y < band.y2; ++y) {
            if ((((y - band.y1) % kAbortCheckLines) == 0) && cancel.check()) {
                return;
            }
            float* dstPix = (float*)job.dstAccess->pixelAt(roi.x1, y);
            if (!dstPix) {
                continue;
            }
            readSourceRow(job.src.get(), job.srcAccess.get(), job.srcBounds, job.srcNComps, roi.x1, y, width, nComps, &sourceRow[0]);
            const bool rowProcessed = layout.processing && (y >= processWindow.y1) && (y < processWindow.y2);
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
            const bool rowInBuffer = (y >= bufferRect.y1) && (y < bufferRect.y2);
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
                    const int p = (c < 4) ? planes.planeOfChannel[c] : -1;
                    if (p < 0) {
                        dstPix[c] = srcPix[c];
                        continue;
                    }
                    float v = inBuffer ? output(layout.buffer[(std::size_t)p * planeSize + offset], p) : output(0.f, p);
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
    parallelForCancellable((int)bandRects.size(), nThreads, cancel, writeBand);
} // writeWindow
} // namespace SpatialFilter

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_SpatialFilterSupport_h
