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

#include "ErodeDilate.h"

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <limits>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include <QThread>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/PoolParallelFor.h"
#include "Engine/RectD.h"

NATRON_NAMESPACE_ENTER

namespace {
// How many lines a worker filters or writes between two abort checks.
const int kAbortCheckLines = 16;
// Below this many pixels a chunk of lines is not worth handing to another thread.
const std::size_t kMinChunkPixels = 16384;
// Chunks per available thread, so a helper that starts late still finds work left.
const int kChunksPerThread = 4;

int
channelIndexForBit(int nComps,
                   int bit)
{
    if (nComps == 1) {
        return (bit == 3) ? 0 : -1;
    }

    return (bit < nComps) ? bit : -1;
}

// A one-channel plane rendered into a wider image sits where EffectInstance's copy back reads
// it: alpha for a colour plane, channel 0 otherwise.
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

// OFX::Coords::toPixelEnclosing().
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

// OFX::Coords::toCanonical().
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

// OFX::Coords::rectIntersection(): rectangles that merely touch still intersect, and a failed
// intersection leaves an empty rectangle at the origin.
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

// Copies `width` pixels of row y starting at x0 into row (nComps floats per pixel), writing zero
// wherever the image has no pixel. An image in another layout is mapped channel by colour bit,
// a channel it lacks reading as zero.
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

// One value per pixel of `channel` of row y, `fill` wherever the image has no pixel.
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

// The mask channel at (x, y), or null outside the mask image.
const float*
maskValueAt(const Image::ReadAccess& access,
            const RectI& bounds,
            int nComps,
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

// CImgFilterPluginHelperBase::maskLineIsZero(), including its treatment of the last column of
// the mask image as outside it when the mask is inverted, which decides the extent of the lines
// the filters run over and so must match.
bool
maskRowIsZero(const Image::ReadAccess& access,
              const RectI& bounds,
              int nComps,
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
            const float* p = maskValueAt(access, bounds, nComps, channel, x, y);
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
            const float* p = maskValueAt(access, bounds, nComps, channel, x, y);
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
                 int nComps,
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
            const float* p = maskValueAt(access, bounds, nComps, channel, x, y);
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
            const float* p = maskValueAt(access, bounds, nComps, channel, x, y);
            if (p && (*p != 0.)) {
                return false;
            }
        }
    }

    return true;
}

struct ErodeDilatePlaneJob {
    ImagePtr dst;
    std::bitset<4> channels;
    ImagePtr src;
    ImagePtr divisor;
    int divisorChannel;
    int skipChannel;
    int nComps;
    int srcNComps;
    int divisorNComps;
    // Image::getBounds() takes the image's lock, which a pool thread must never ask for: behind a
    // writer waiting on an image this render holds for reading, it blocks forever. The bounds are
    // read once on the calling thread instead.
    RectI srcBounds;
    RectI divisorBounds;
    std::shared_ptr<Image::ReadAccess> srcAccess;
    std::shared_ptr<Image::ReadAccess> divisorAccess;
    std::shared_ptr<Image::WriteAccess> dstAccess;

    ErodeDilatePlaneJob()
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
};

// Shares the render's abort state between the calling thread, which alone may ask the effect,
// and the pool threads.
struct AbortState {
    const EffectInstance* effect;
    QThread* callingThread;
    std::atomic<bool> flag;

    explicit AbortState(const EffectInstance* effect_)
        : effect(effect_)
        , callingThread(QThread::currentThread())
        , flag(false)
    {
    }

    bool check()
    {
        if (flag.load(std::memory_order_relaxed)) {
            return true;
        }
        if ((QThread::currentThread() == callingThread) && effect->aborted()) {
            flag = true;

            return true;
        }

        return false;
    }
};

// Runs body(first, end) over [0, nLines) split into contiguous chunks on the global pool. Each
// line is processed whole by one call, so the result does not depend on the split.
void
forEachLineChunk(int nLines,
                 std::size_t pixelsPerLine,
                 int nThreads,
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
    parallelForOnGlobalPool(realChunks, nThreads, chunk);
}
} // anonymous namespace

namespace ErodeDilateKernels {
namespace {
    inline float
    reduce(float a,
           float b,
           bool takeMax)
    {
        return takeMax ? ((b > a) ? b : a) : ((b < a) ? b : a);
    }
} // anonymous namespace

void
filterLine(float* data,
           int length,
           std::ptrdiff_t stride,
           int halfWidth,
           bool takeMax,
           LineScratch& scratch)
{
    if ((halfWidth <= 0) || (length <= 1)) {
        return;
    }
    if (length <= halfWidth + 2) {
        float extremum = data[0];
        for (int i = 1; i < length; ++i) {
            extremum = reduce(extremum, data[(std::ptrdiff_t)i * stride], takeMax);
        }
        for (int i = 0; i < length; ++i) {
            data[(std::ptrdiff_t)i * stride] = extremum;
        }

        return;
    }

    const int width = 2 * halfWidth + 1;
    const int n = length + 2 * halfWidth;
    const float neutral = takeMax ? -std::numeric_limits<float>::infinity() : std::numeric_limits<float>::infinity();
    scratch.padded.resize(n);
    scratch.prefix.resize(n);
    scratch.suffix.resize(n);
    float* padded = &scratch.padded[0];
    float* prefix = &scratch.prefix[0];
    float* suffix = &scratch.suffix[0];

    std::fill(padded, padded + halfWidth, neutral);
    for (int i = 0; i < length; ++i) {
        padded[halfWidth + i] = data[(std::ptrdiff_t)i * stride];
    }
    std::fill(padded + halfWidth + length, padded + n, neutral);

    // Running extremum inside each block of `width` values, from the block start and from its end:
    // a window spans at most two blocks, so it is the combination of one suffix and one prefix.
    for (int j = 0; j < n; ++j) {
        prefix[j] = ((j % width) == 0) ? padded[j] : reduce(prefix[j - 1], padded[j], takeMax);
    }
    for (int j = n - 1; j >= 0; --j) {
        suffix[j] = ((j == n - 1) || (((j + 1) % width) == 0)) ? padded[j] : reduce(suffix[j + 1], padded[j], takeMax);
    }
    for (int i = 0; i < length; ++i) {
        data[(std::ptrdiff_t)i * stride] = reduce(suffix[i], prefix[i + width - 1], takeMax);
    }
}
} // namespace ErodeDilateKernels

ErodeDilate::ErodeDilate(NodePtr node)
    : ErodeDilate(node, false)
{
}

ErodeDilate::ErodeDilate(NodePtr node,
                         bool dilate)
    : NativeImageEffect(node, []() {
        NativeImageTraits traits;

        traits.hostUnPremult = true;
        traits.processesAllLayers = true;

        return traits;
    }())
    , _dilate(dilate)
    , _size()
    , _expandRoD()
{
}

ErodeDilate::~ErodeDilate()
{
}

void
ErodeDilate::addAcceptedComponents(int inputNb,
                                   std::list<ImageLayerDesc>* comps)
{
    NativeImageEffect::addAcceptedComponents(inputNb, comps);
    if (!isInputMask(inputNb)) {
        comps->push_back(ImageLayerDesc::getXYComponents());
    }
}

NativePluginDescription
ErodeDilate::getNativePluginDescription() const
{
    NativePluginDescription desc;

    if (_dilate) {
        desc.id = PLUGINID_NATRON_DILATE;
        desc.label = "Dilate";
        desc.description = tr("Dilate (or erode) input stream by a rectangular structuring element of specified size and Neumann boundary conditions (pixels out of the image get the value of the nearest pixel).\n"
                              "A negative size will perform an erosion instead of a dilation.\n"
                              "Different sizes can be given for the x and y axis.\n"
                              "The operation is the 'dilate' and 'erode' behaviour of the CImg library (http://cimg.eu), "
                              "distributed under the CeCILL-C licence.")
                               .toStdString();
        desc.majorVersion = PLUGIN_MAJOR_NATRON_DILATE;
    } else {
        desc.id = PLUGINID_NATRON_ERODE;
        desc.label = "Erode";
        desc.description = tr("Erode (or dilate) input stream by a rectangular structuring element of specified size and Neumann boundary conditions (pixels out of the image get the value of the nearest pixel).\n"
                              "A negative size will perform a dilation instead of an erosion.\n"
                              "Different sizes can be given for the x and y axis.\n"
                              "The operation is the 'erode' and 'dilate' behaviour of the CImg library (http://cimg.eu), "
                              "distributed under the CeCILL-C licence.")
                               .toStdString();
        desc.majorVersion = PLUGIN_MAJOR_NATRON_ERODE;
    }
    desc.grouping = PLUGIN_GROUP_FILTER;
    desc.minorVersion = 0;
    desc.inputs.push_back(NativeInputDescription("Source", false, eDataKindImage));
    desc.inputs.push_back(NativeInputDescription("Mask", true, eDataKindImage, true));
    desc.outputKind = eDataKindImage;

    return desc;
}

void
ErodeDilate::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobIntPtr size = createKnob<KnobInt>(tr("Size"), 2);
    size->setName(kErodeDilateParamSize);
    size->setHintToolTip(tr("Width/height of the rectangular structuring element is 2*size+1, in pixel units (>=0)."));
    for (int d = 0; d < 2; ++d) {
        size->setMinimum(-1000, d);
        size->setMaximum(1000, d);
        size->setDisplayMinimum(-100, d);
        size->setDisplayMaximum(100, d);
        size->setDefaultValue(1, d);
    }
    page->addKnob(size);
    _size = size;

    KnobBoolPtr expandRoD = createKnob<KnobBool>(tr("Expand RoD"));
    expandRoD->setName(kErodeDilateParamExpandRoD);
    if (_dilate) {
        expandRoD->setHintToolTip(tr("Expand the source region of definition by 2*size pixels if size is positive"));
    } else {
        expandRoD->setHintToolTip(tr("Expand the source region of definition by 2*size pixels if size is negative"));
    }
    expandRoD->setDefaultValue(true);
    page->addKnob(expandRoD);
    _expandRoD = expandRoD;

    addMaskMixKnobs(page);
}

void
ErodeDilate::getParams(double time,
                       ViewIdx view,
                       ErodeDilateParams* params) const
{
    KnobIntPtr size = _size.lock();
    KnobBoolPtr expandRoD = _expandRoD.lock();

    *params = ErodeDilateParams();
    if (size) {
        params->sizeX = size->getValueAtTime(time, 0, view);
        params->sizeY = size->getValueAtTime(time, 1, view);
    }
    params->expandRoD = expandRoD ? expandRoD->getValueAtTime(time, 0, view) : true;
}

RectI
ErodeDilate::getSourceRoI(const RectI& rect,
                          const RenderScale& scale,
                          const ErodeDilateParams& params)
{
    const OfxPointD rs = scale.toOfxPointD();
    const int deltaX = (int)std::ceil(std::abs(params.sizeX) * rs.x);
    const int deltaY = (int)std::ceil(std::abs(params.sizeY) * rs.y);

    return RectI(rect.x1 - deltaX, rect.y1 - deltaY, rect.x2 + deltaX, rect.y2 + deltaY);
}

bool
ErodeDilate::paramsAreIdentity(const RenderScale& scale,
                               const ErodeDilateParams& params)
{
    const OfxPointD rs = scale.toOfxPointD();

    return (std::floor(params.sizeX * rs.x) == 0) && (std::floor(params.sizeY * rs.y) == 0);
}

void
ErodeDilate::getExpansion(const RenderScale& scale,
                          const ErodeDilateParams& params,
                          bool dilate,
                          int* deltaX,
                          int* deltaY)
{
    const OfxPointD rs = scale.toOfxPointD();
    const bool growsX = dilate ? (params.sizeX > 0) : (params.sizeX < 0);
    const bool growsY = dilate ? (params.sizeY > 0) : (params.sizeY < 0);

    *deltaX = growsX ? (int)std::ceil(std::abs(params.sizeX) * rs.x) : 0;
    *deltaY = growsY ? (int)std::ceil(std::abs(params.sizeY) * rs.y) : 0;
}

bool
ErodeDilate::isIdentityOp(double time,
                          const RenderScale& scale,
                          const RectI& /*roi*/,
                          ViewIdx view)
{
    ErodeDilateParams params;

    getParams(time, view, &params);

    return paramsAreIdentity(scale, params);
}

StatusEnum
ErodeDilate::getRegionOfDefinition(U64 hash,
                                   double time,
                                   const RenderScale& scale,
                                   ViewIdx view,
                                   RectD* rod)
{
    EffectInstancePtr input = getInput(0);

    if (!input) {
        return EffectInstance::getRegionOfDefinition(hash, time, scale, view, rod);
    }
    ErodeDilateParams params;
    getParams(time, view, &params);
    if (!params.expandRoD) {
        return EffectInstance::getRegionOfDefinition(hash, time, scale, view, rod);
    }

    const RenderScale inputScale = input->supportsRenderScale() ? scale : RenderScale::identity;
    RectD srcRoD;
    bool isProjectFormat = false;
    if (input->getRegionOfDefinition_public(input->getRenderHash(), time, inputScale, view, &srcRoD, &isProjectFormat) == eStatusFailed) {
        return eStatusFailed;
    }
    RectI rodPixel = toPixelEnclosing(srcRoD, scale, getAspectRatio(0));
    if (isEmptyRect(rodPixel)) {
        return EffectInstance::getRegionOfDefinition(hash, time, scale, view, rod);
    }
    int deltaX = 0;
    int deltaY = 0;
    getExpansion(scale, params, _dilate, &deltaX, &deltaY);
    rodPixel.x1 -= deltaX;
    rodPixel.x2 += deltaX;
    rodPixel.y1 -= deltaY;
    rodPixel.y2 += deltaY;
    *rod = toCanonical(rodPixel, scale, getAspectRatio(-1));

    return eStatusOK;
}

void
ErodeDilate::getRegionsOfInterest(double time,
                                  const RenderScale& scale,
                                  const RectD& outputRoD,
                                  const RectD& renderWindow,
                                  ViewIdx view,
                                  RoIMap* ret)
{
    EffectInstance::getRegionsOfInterest(time, scale, outputRoD, renderWindow, view, ret);

    EffectInstancePtr input = getInput(0);
    if (!input) {
        return;
    }
    const bool doMasking = isMaskApplied();
    double mix = 1.;
    if (doMasking) {
        mix = getMixValue(time, view);
        if (mix == 0.) {
            return;
        }
    }

    ErodeDilateParams params;
    getParams(time, view, &params);
    const double par = getAspectRatio(0);
    const RectI rectPixel = toPixelEnclosing(renderWindow, scale, par);
    RectD srcRoI = toCanonical(getSourceRoI(rectPixel, scale, params), scale, par);
    if (doMasking && (mix != 1.)) {
        srcRoI.merge(renderWindow);
    }
    // The mask may be the same effect as the source, in which case both needs share one entry.
    RoIMap::iterator found = ret->find(input);
    if (found != ret->end()) {
        if (isMaskApplied() && (getInput(getMaskInput()) == input)) {
            srcRoI.merge(found->second);
        }
        found->second = srcRoI;
    } else {
        ret->insert(std::make_pair(input, srcRoI));
    }
}

StatusEnum
ErodeDilate::render(const RenderActionArgs& args)
{
    ErodeDilateParams params;
    getParams(args.time, args.view, &params);

    const OfxPointD rs = args.mappedScale.toOfxPointD();
    // The operation of the positive axes first, then the opposite one on the negative axes.
    const int positiveHalfX = (int)std::floor(std::max(0, params.sizeX) * rs.x);
    const int positiveHalfY = (int)std::floor(std::max(0, params.sizeY) * rs.y);
    const int negativeHalfX = (int)std::floor(std::max(0, -params.sizeX) * rs.x);
    const int negativeHalfY = (int)std::floor(std::max(0, -params.sizeY) * rs.y);
    struct Phase {
        int halfX;
        int halfY;
        bool takeMax;
    };
    const Phase phases[2] = {
        { positiveHalfX, positiveHalfY, _dilate },
        { negativeHalfX, negativeHalfY, !_dilate }
    };

    const double mixValue = getMixValue(args.time, args.view);
    const float mix = (float)mixValue;
    const bool maskInvert = getMaskInvertValue(args.time, args.view);
    NodePtr node = getNode();

    // Every image is fetched before any is locked: fetching renders upstream, which may write
    // into a cached image this render would otherwise already hold a read lock on.
    const bool doMask = isMaskApplied();
    ImagePtr mask;
    int maskChannel = -1;
    if (doMask) {
        const int maskInput = getMaskInput();
        ImageLayerDesc maskLayer;
        if (resolveInputPlaneForRender(maskInput, args.time, args.view, &maskLayer, &maskChannel) && (maskChannel >= 0)) {
            RectI maskRoI;
            mask = getImage(maskInput, args.time, args.mappedScale, args.view, NULL, &maskLayer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &maskRoI);
        }
        if (mask && ((maskChannel >= (int)mask->getComponentsCount()))) {
            mask.reset();
        }
    }

    KnobChannelSelectPtr unPremultBy = node ? node->getUnPremultBySelector() : KnobChannelSelectPtr();
    ImageLayerDesc divisorLayer;
    int divisorChannel = -1;
    if (unPremultBy && !unPremultBy->isNone() && getInput(0)) {
        std::list<ImageLayerDesc> availableLayers;
        getAvailableLayers(args.time, args.view, 0, &availableLayers);
        divisorChannel = node->getUnPremultChannel(availableLayers, &divisorLayer);
        if (divisorLayer.getNumComponents() == 0) {
            divisorChannel = -1;
        }
    }

    std::vector<ErodeDilatePlaneJob> jobs;
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        ErodeDilatePlaneJob job;
        job.dst = it->second;
        if (!job.dst) {
            continue;
        }
        job.nComps = (int)job.dst->getComponentsCount();
        job.channels = processedBitsForImage(it->first, job.nComps, args.processChannels);

        ImageLayerDesc sourceLayer;
        if (getInput(0) && resolveInputPlaneForRender(0, args.time, args.view, &sourceLayer, NULL)) {
            RectI sourceRoI;
            // Mapped to the clip's components, which is the layout the output plane is rendered in.
            job.src = getImage(0, args.time, args.mappedScale, args.view, NULL, &sourceLayer, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &sourceRoI);
        }
        if (job.src && (divisorChannel >= 0)) {
            RectI divisorRoI;
            job.divisor = getImage(0, args.time, args.mappedScale, args.view, NULL, &divisorLayer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &divisorRoI);
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
    const int width = roi.width();
    if ((width <= 0) || (roi.height() <= 0)) {
        return eStatusOK;
    }

    // The filters run over the window plus the halo, clipped to this effect's own region of
    // definition, as CImgFilterPluginHelper::render() clips its source region of interest.
    RectD ownRoD;
    bool isProjectFormat = false;
    if (getRegionOfDefinition_public(getRenderHash(), args.time, args.mappedScale, args.view, &ownRoD, &isProjectFormat) == eStatusFailed) {
        return eStatusFailed;
    }
    const RectI dstRoDPixel = toPixelEnclosing(ownRoD, args.mappedScale, getAspectRatio(-1));

    // Images are locked here, on the calling thread, for the whole render; the pool threads only
    // compute pixel addresses through these accesses.
    std::shared_ptr<Image::ReadAccess> maskAccess;
    RectI maskBounds;
    int maskNComps = 0;
    if (mask) {
        maskBounds = mask->getBounds();
        maskNComps = (int)mask->getComponentsCount();
        maskAccess = std::make_shared<Image::ReadAccess>(mask.get());
    }
    for (std::size_t j = 0; j < jobs.size(); ++j) {
        ErodeDilatePlaneJob& job = jobs[j];
        if (job.src) {
            job.srcBounds = job.src->getBounds();
            job.srcNComps = (int)job.src->getComponentsCount();
            job.srcAccess = std::make_shared<Image::ReadAccess>(job.src.get());
        }
        if (job.divisor) {
            job.divisorBounds = job.divisor->getBounds();
            job.divisorNComps = (int)job.divisor->getComponentsCount();
            job.divisorAccess = std::make_shared<Image::ReadAccess>(job.divisor.get());
        }
        job.dstAccess = std::make_shared<Image::WriteAccess>(job.dst.get());
    }

    // The processed window shrinks past the rows and columns where the mask is zero, exactly as
    // CImgFilterPluginHelper::render() does: the shrunk window decides the filtered extent.
    RectI processWindow = roi;
    if (mixValue == 0.) {
        processWindow.x2 = processWindow.x1;
        processWindow.y2 = processWindow.y1;
    }
    if (maskAccess) {
        while ((processWindow.y2 > processWindow.y1) && maskRowIsZero(*maskAccess, maskBounds, maskNComps, maskChannel, processWindow.x1, processWindow.x2, processWindow.y2 - 1, maskInvert)) {
            --processWindow.y2;
        }
        while ((processWindow.y2 > processWindow.y1) && maskRowIsZero(*maskAccess, maskBounds, maskNComps, maskChannel, processWindow.x1, processWindow.x2, processWindow.y1, maskInvert)) {
            ++processWindow.y1;
        }
        while ((processWindow.x2 > processWindow.x1) && maskColumnIsZero(*maskAccess, maskBounds, maskNComps, maskChannel, processWindow.x1, processWindow.y1, processWindow.y2, maskInvert)) {
            ++processWindow.x1;
        }
        while ((processWindow.x2 > processWindow.x1) && maskColumnIsZero(*maskAccess, maskBounds, maskNComps, maskChannel, processWindow.x2 - 1, processWindow.y1, processWindow.y2, maskInvert)) {
            --processWindow.x2;
        }
    }
    const bool processing = !isEmptyRect(processWindow);

    RectI bufferRect(0, 0, 0, 0);
    if (processing) {
        intersectRects(getSourceRoI(processWindow, args.mappedScale, params), dstRoDPixel, &bufferRect);
    }
    const int bufferWidth = std::max(0, bufferRect.width());
    const int bufferHeight = std::max(0, bufferRect.height());
    const std::size_t planeSize = (std::size_t)bufferWidth * bufferHeight;

    const int nThreads = appPTR->getNCPUsAvailableForEffect();
    AbortState abortState(this);

    for (std::size_t j = 0; j < jobs.size(); ++j) {
        const ErodeDilatePlaneJob& job = jobs[j];
        const int nComps = job.nComps;

        // The processed channels, one plane each, the way CImgFilterPluginHelper extracts them.
        std::vector<int> planeChannel;
        int planeOfChannel[4] = { -1, -1, -1, -1 };
        for (int c = 0; (c < nComps) && (c < 4); ++c) {
            const int bit = pixelKernelChannelBit(nComps, c);
            if (job.channels[bit]) {
                planeOfChannel[c] = (int)planeChannel.size();
                planeChannel.push_back(c);
            }
        }
        const int nPlanes = (int)planeChannel.size();
        std::vector<float> buffer((processing && (nPlanes > 0)) ? planeSize * nPlanes : 0);

        if (!buffer.empty()) {
            // 1. The source over the buffer, zero outside its image, divided by the
            // "(Un)premult by" channel where that is usable.
            const std::function<void(int, int)> fillRows = [&](int firstRow, int endRow) {
                std::vector<float> pixel(nComps);
                int srcIndex[4] = { -1, -1, -1, -1 };
                for (int c = 0; (c < nComps) && (c < 4); ++c) {
                    srcIndex[c] = job.src ? channelIndexForBit(job.srcNComps, pixelKernelChannelBit(nComps, c)) : -1;
                }
                const RectI& b = job.srcBounds;
                const bool hasSource = job.src && !isEmptyRect(b);
                for (int row = firstRow; row < endRow; ++row) {
                    if ((((row - firstRow) % kAbortCheckLines) == 0) && abortState.check()) {
                        return;
                    }
                    const int y = bufferRect.y1 + row;
                    const bool rowInside = hasSource && (y >= b.y1) && (y < b.y2);
                    const float* srcRow = rowInside ? (const float*)job.srcAccess->pixelAt(b.x1, y) : NULL;
                    const float* divisorRow = NULL;
                    const RectI& db = job.divisorBounds;
                    if (srcRow && job.divisor && (y >= db.y1) && (y < db.y2)) {
                        divisorRow = (const float*)job.divisorAccess->pixelAt(db.x1, y);
                    }
                    for (int i = 0; i < bufferWidth; ++i) {
                        const int x = bufferRect.x1 + i;
                        std::fill(pixel.begin(), pixel.end(), 0.f);
                        if (srcRow && (x >= b.x1) && (x < b.x2)) {
                            const float* srcPix = srcRow + (std::size_t)(x - b.x1) * job.srcNComps;
                            for (int c = 0; (c < nComps) && (c < 4); ++c) {
                                if (srcIndex[c] >= 0) {
                                    pixel[c] = srcPix[srcIndex[c]];
                                }
                            }
                            if (divisorRow && (x >= db.x1) && (x < db.x2)) {
                                const float d = divisorRow[(std::size_t)(x - db.x1) * job.divisorNComps + job.divisorChannel];
                                if (Image::unPremultDivisorIsUsable(d)) {
                                    for (int c = 0; (c < nComps) && (c < 4); ++c) {
                                        if (c != job.skipChannel) {
                                            pixel[c] = Image::unPremultiplyValue(pixel[c], d);
                                        }
                                    }
                                }
                            }
                        }
                        const std::size_t offset = (std::size_t)row * bufferWidth + i;
                        for (int p = 0; p < nPlanes; ++p) {
                            buffer[(std::size_t)p * planeSize + offset] = pixel[planeChannel[p]];
                        }
                    }
                }
            };
            forEachLineChunk(bufferHeight, (std::size_t)bufferWidth * nPlanes, nThreads, fillRows);
            if (abortState.check()) {
                return eStatusOK;
            }

            // 2. Each phase runs its horizontal pass over whole rows and its vertical pass over
            // whole columns of every plane.
            for (int ph = 0; ph < 2; ++ph) {
                const Phase& phase = phases[ph];
                if (phase.halfX > 0) {
                    const std::function<void(int, int)> rows = [&](int first, int end) {
                        ErodeDilateKernels::LineScratch scratch;
                        for (int line = first; line < end; ++line) {
                            if ((((line - first) % kAbortCheckLines) == 0) && abortState.check()) {
                                return;
                            }
                            const int p = line / bufferHeight;
                            const int row = line % bufferHeight;
                            ErodeDilateKernels::filterLine(&buffer[(std::size_t)p * planeSize + (std::size_t)row * bufferWidth], bufferWidth, 1, phase.halfX, phase.takeMax, scratch);
                        }
                    };
                    forEachLineChunk(nPlanes * bufferHeight, (std::size_t)bufferWidth, nThreads, rows);
                    if (abortState.check()) {
                        return eStatusOK;
                    }
                }
                if (phase.halfY > 0) {
                    const std::function<void(int, int)> columns = [&](int first, int end) {
                        ErodeDilateKernels::LineScratch scratch;
                        for (int line = first; line < end; ++line) {
                            if ((((line - first) % kAbortCheckLines) == 0) && abortState.check()) {
                                return;
                            }
                            const int p = line / bufferWidth;
                            const int column = line % bufferWidth;
                            ErodeDilateKernels::filterLine(&buffer[(std::size_t)p * planeSize + column], bufferHeight, (std::ptrdiff_t)bufferWidth, phase.halfY, phase.takeMax, scratch);
                        }
                    };
                    forEachLineChunk(nPlanes * bufferWidth, (std::size_t)bufferHeight, nThreads, columns);
                    if (abortState.check()) {
                        return eStatusOK;
                    }
                }
            }
        }

        // 3. The window: multiply back, mask and mix against the undivided source; unprocessed
        // channels and the pixels outside the processed window pass through.
        std::vector<RectI> bandRects;
        NativeImageEffect::makeRowBands(roi, nThreads, &bandRects);
        const std::function<void(int)> writeBand = [&](int bandIndex) {
            const RectI& band = bandRects[bandIndex];
            const std::size_t rowSize = (std::size_t)width * nComps;
            std::vector<float> sourceRow(rowSize);
            std::vector<float> maskRow(doMask ? width : 0);
            std::vector<float> divisorRow(job.divisor ? width : 0);

            for (int y = band.y1; y < band.y2; ++y) {
                if ((((y - band.y1) % kAbortCheckLines) == 0) && abortState.check()) {
                    return;
                }
                float* dstPix = (float*)job.dstAccess->pixelAt(roi.x1, y);
                if (!dstPix) {
                    continue;
                }
                readSourceRow(job.src.get(), job.srcAccess.get(), job.srcBounds, job.srcNComps, roi.x1, y, width, nComps, &sourceRow[0]);
                const bool rowProcessed = processing && (y >= processWindow.y1) && (y < processWindow.y2);
                if (!rowProcessed) {
                    std::copy(sourceRow.begin(), sourceRow.end(), dstPix);
                    continue;
                }
                if (job.divisor) {
                    readChannelRow(job.divisor.get(), job.divisorAccess.get(), job.divisorBounds, job.divisorNComps, job.divisorChannel, roi.x1, y, width, 1.f, &divisorRow[0]);
                }
                if (doMask) {
                    readChannelRow(mask.get(), maskAccess.get(), maskBounds, maskNComps, maskChannel, roi.x1, y, width, 0.f, &maskRow[0]);
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
                        const float maskScale = maskInvert ? (1.f - maskRow[i]) : maskRow[i];
                        alpha = maskScale * mix;
                    }
                    for (int c = 0; c < nComps; ++c) {
                        const int p = (c < 4) ? planeOfChannel[c] : -1;
                        if (p < 0) {
                            dstPix[c] = srcPix[c];
                            continue;
                        }
                        float v = inBuffer ? buffer[(std::size_t)p * planeSize + offset] : 0.f;
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
        parallelForOnGlobalPool((int)bandRects.size(), nThreads, writeBand);
        if (abortState.check()) {
            return eStatusOK;
        }
    }

    return eStatusOK;
} // ErodeDilate::render

NATRON_NAMESPACE_EXIT
