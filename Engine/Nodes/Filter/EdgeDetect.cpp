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

/*
 * The finite-difference gradient schemes in this file follow CImg's CImg<float>::get_gradient()
 * (CImg 2.9.9, as bundled with openfx-misc), by David Tschumperle and contributors,
 * <http://cimg.eu>. CImg is distributed under the CeCILL-C licence, which is compatible with the
 * GNU GPL.
 */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "EdgeDetect.h"

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <QThread>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/ErodeDilate.h"
#include "Engine/PoolParallelFor.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"

NATRON_NAMESPACE_ENTER

namespace {
// How many lines a worker filters or writes between two abort checks.
const int kAbortCheckLines = 16;
// Below this many pixels a chunk of lines is not worth handing to another thread.
const std::size_t kMinChunkPixels = 16384;
// Chunks per available thread, so a helper that starts late still finds work left.
const int kChunksPerThread = 4;
// Keeps the erosion radius of an absurd erode size a representable int.
const double kMaxErodeRadius = 1e7;

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

bool
isGaussianFamily(BlurKernels::Filter filter)
{
    return (filter == BlurKernels::eFilterQuasiGaussian) || (filter == BlurKernels::eFilterGaussian);
}

int
boxIterations(BlurKernels::Filter filter)
{
    return (filter == BlurKernels::eFilterBox) ? 1 : ((filter == BlurKernels::eFilterTriangle) ? 2 : 3);
}

bool
isFiniteDifference(EdgeDetectFilterEnum filter)
{
    return (filter == eEdgeDetectFilterSimple) || (filter == eEdgeDetectFilterSobel) || (filter == eEdgeDetectFilterRotationInvariant);
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

struct EdgePlaneJob {
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

    EdgePlaneJob()
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

// The parabola through (-1, Ip), (0, Ic), (1, In), evaluated at alpha.
double
parabola(double Ip,
         double Ic,
         double In,
         double alpha)
{
    const double a = ((In - Ic) + (Ip - Ic)) / 2;
    const double b = (In - Ip) / 2;
    const double c = Ic;

    return a * alpha * alpha + b * alpha + c;
}

// The extremum value of that parabola.
double
parabolaMaxValue(double Ip,
                 double Ic,
                 double In)
{
    const double a = ((In - Ic) + (Ip - Ic)) / 2;
    const double b = (In - Ip) / 2;
    const double c = Ic;

    return c - b * b / (4 * a);
}
} // anonymous namespace

EdgeDetect::EdgeDetect(NodePtr node)
    : NativeImageEffect(node, []() {
        NativeImageTraits traits;

        traits.hostUnPremult = true;
        traits.processesAllLayers = false;
        traits.defaultChannels[3] = false;

        return traits;
    }())
    , _filter()
    , _multiChannel()
    , _blurSize()
    , _erodeSize()
    , _nms()
    , _expandRoD()
    , _cropToFormat()
{
}

EdgeDetect::~EdgeDetect()
{
}

void
EdgeDetect::addAcceptedComponents(int inputNb,
                                  std::list<ImageLayerDesc>* comps)
{
    NativeImageEffect::addAcceptedComponents(inputNb, comps);
    if (!isInputMask(inputNb)) {
        comps->push_back(ImageLayerDesc::getXYComponents());
    }
}

NativePluginDescription
EdgeDetect::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_EDGEDETECT;
    desc.label = "EdgeDetect";
    desc.description = tr("Perform edge detection by computing the image gradient magnitude. Optionally, edge detection can be preceded by blurring, and followed by erosion and thresholding. In most cases, EdgeDetect is followed a Grade node to extract the proper edges and generate a mask from these.\n"
                          "\n"
                          "For color or multi-channel images, several edge detection algorithms are proposed to combine the gradients computed in each channel:\n"
                          "- Separate: the gradient magnitude is computed in each channel separately, and the output is a color edge image.\n"
                          "- RMS: the RMS of per-channel gradients magnitudes is computed.\n"
                          "- Max: the maximum per-channel gradient magnitude is computed.\n"
                          "- Tensor: the tensor gradient norm [1].\n"
                          "\n"
                          "References:\n"
                          "- [1] Silvano Di Zenzo, A note on the gradient of a multi-image, CVGIP 33, 116-125 (1986). http://people.csail.mit.edu/tieu/notebook/imageproc/dizenzo86.pdf\n"
                          "\n"
                          "The filters are ports of functions of the CImg library (http://cimg.eu), "
                          "distributed under the CeCILL-C licence.")
                           .toStdString();
    desc.grouping = PLUGIN_GROUP_FILTER;
    desc.majorVersion = PLUGIN_MAJOR_NATRON_EDGEDETECT;
    desc.minorVersion = 0;
    desc.inputs.push_back(NativeInputDescription("Source", false, eDataKindImage));
    desc.inputs.push_back(NativeInputDescription("Mask", true, eDataKindImage, true));
    desc.outputKind = eDataKindImage;

    return desc;
}

void
EdgeDetect::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobChoicePtr filter = createKnob<KnobChoice>(tr("Filter"));
    filter->setName(kEdgeDetectParamFilter);
    filter->setHintToolTip(tr("Edge detection filter. If the blur size is not zero, it is used as the kernel size for quasi-Gaussian, Gaussian, box, triangle and quadratic filters. For the simple, rotation-invariant and Sobel filters, the image is pre-blurred with a Gaussian filter."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kEdgeDetectParamFilterSimple, tr("Simple").toStdString(), tr("Gradient is estimated by centered finite differences.").toStdString()));
        options.push_back(ChoiceOption(kEdgeDetectParamFilterSobel, tr("Sobel").toStdString(), tr("Compute gradient using the Sobel 3x3 filter.").toStdString()));
        options.push_back(ChoiceOption(kEdgeDetectParamFilterRotationInvariant, tr("Rotation Invariant").toStdString(), tr("Compute gradient using a 3x3 rotation-invariant filter.").toStdString()));
        options.push_back(ChoiceOption(kEdgeDetectParamFilterQuasiGaussian, tr("Quasi-Gaussian").toStdString(), tr("Quasi-Gaussian filter (0-order recursive Deriche filter, faster) - IIR (infinite support / impulsional response).").toStdString()));
        options.push_back(ChoiceOption(kEdgeDetectParamFilterGaussian, tr("Gaussian").toStdString(), tr("Gaussian filter (Van Vliet recursive Gaussian filter, more isotropic, slower) - IIR (infinite support / impulsional response).").toStdString()));
        options.push_back(ChoiceOption(kEdgeDetectParamFilterBox, tr("Box").toStdString(), tr("Box filter - FIR (finite support / impulsional response).").toStdString()));
        options.push_back(ChoiceOption(kEdgeDetectParamFilterTriangle, tr("Triangle").toStdString(), tr("Triangle/tent filter - FIR (finite support / impulsional response).").toStdString()));
        options.push_back(ChoiceOption(kEdgeDetectParamFilterQuadratic, tr("Quadratic").toStdString(), tr("Quadratic filter - FIR (finite support / impulsional response).").toStdString()));
        filter->populateChoices(options);
    }
    filter->setDefaultValue((int)eEdgeDetectFilterGaussian);
    page->addKnob(filter);
    _filter = filter;

    KnobChoicePtr multiChannel = createKnob<KnobChoice>(tr("Multi-Channel"));
    multiChannel->setName(kEdgeDetectParamMultiChannel);
    multiChannel->setHintToolTip(tr("Operation used to combine multi-channel (e.g. color) gradients into an edge detector. This parameter has no effect if a single channel (e.g. alpha) is processed."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kEdgeDetectParamMultiChannelSeparate, tr("Separate").toStdString(), tr("The gradient magnitude is computed in each channel separately, and the output is a color edge image.").toStdString()));
        options.push_back(ChoiceOption(kEdgeDetectParamMultiChannelRMS, tr("RMS").toStdString(), tr("The RMS of per-channel gradients magnitudes is computed.").toStdString()));
        options.push_back(ChoiceOption(kEdgeDetectParamMultiChannelMax, tr("Max").toStdString(), tr("The maximum per-channel gradient magnitude is computed.").toStdString()));
        options.push_back(ChoiceOption(kEdgeDetectParamMultiChannelTensor, tr("Tensor").toStdString(), tr("The tensor gradient norm is computed. See Silvano Di Zenzo, A note on the gradient of a multi-image, CVGIP 33, 116-125 (1986).").toStdString()));
        multiChannel->populateChoices(options);
    }
    multiChannel->setDefaultValue((int)eEdgeDetectMultiChannelTensor);
    page->addKnob(multiChannel);
    _multiChannel = multiChannel;

    KnobDoublePtr blurSize = createKnob<KnobDouble>(tr("Blur Size"));
    blurSize->setName(kEdgeDetectParamBlurSize);
    blurSize->setHintToolTip(tr("Size of the blur kernel applied before edge detection."));
    blurSize->setMinimum(0.);
    blurSize->setMaximum(DBL_MAX);
    blurSize->setDisplayMinimum(0.);
    blurSize->setDisplayMaximum(100.);
    blurSize->setIncrement(0.1);
    blurSize->setDecimals(1);
    blurSize->setDefaultValue(0.);
    page->addKnob(blurSize);
    _blurSize = blurSize;

    KnobDoublePtr erodeSize = createKnob<KnobDouble>(tr("Erode Size"));
    erodeSize->setName(kEdgeDetectParamErodeSize);
    erodeSize->setHintToolTip(tr("Size of the erosion performed after edge detection."));
    erodeSize->setMinimum(-DBL_MAX);
    erodeSize->setMaximum(DBL_MAX);
    erodeSize->setDisplayMinimum(-10.);
    erodeSize->setDisplayMaximum(10.);
    erodeSize->setIncrement(0.1);
    erodeSize->setDecimals(1);
    erodeSize->setDefaultValue(0.);
    page->addKnob(erodeSize);
    _erodeSize = erodeSize;

    KnobBoolPtr nms = createKnob<KnobBool>(tr("Non-Maxima Suppression"));
    nms->setName(kEdgeDetectParamNMS);
    nms->setHintToolTip(tr("Perform non-maxima suppression (after edge detection and erosion): only values that are maximal in the direction orthogonal to the contour are kept. For multi-channel images, the contour direction estimation depends on the multi-channel operation."));
    nms->setDefaultValue(false);
    page->addKnob(nms);
    _nms = nms;

    KnobBoolPtr expandRoD = createKnob<KnobBool>(tr("Expand RoD"));
    expandRoD->setName(kEdgeDetectParamExpandRoD);
    expandRoD->setHintToolTip(tr("Expand the source region of definition by 1.5*size (3.6*sigma)."));
    expandRoD->setDefaultValue(true);
    expandRoD->setAddNewLine(false);
    page->addKnob(expandRoD);
    _expandRoD = expandRoD;

    KnobBoolPtr cropToFormat = createKnob<KnobBool>(tr("Crop To Format"));
    cropToFormat->setName(kEdgeDetectParamCropToFormat);
    cropToFormat->setHintToolTip(tr("If the source is inside the format and the effect extends it outside of the format, crop it to avoid unnecessary calculations. To avoid unwanted crops, only the borders that were inside of the format in the source clip will be cropped."));
    cropToFormat->setDefaultValue(true);
    page->addKnob(cropToFormat);
    _cropToFormat = cropToFormat;

    addMaskMixKnobs(page);
} // EdgeDetect::initializeKnobs

void
EdgeDetect::getParams(double time,
                      ViewIdx view,
                      EdgeDetectParams* params) const
{
    KnobChoicePtr filter = _filter.lock();
    KnobChoicePtr multiChannel = _multiChannel.lock();
    KnobDoublePtr blurSize = _blurSize.lock();
    KnobDoublePtr erodeSize = _erodeSize.lock();
    KnobBoolPtr nms = _nms.lock();
    KnobBoolPtr expandRoD = _expandRoD.lock();
    KnobBoolPtr cropToFormat = _cropToFormat.lock();

    *params = EdgeDetectParams();
    if (blurSize) {
        params->sizeX = params->sizeY = blurSize->getValueAtTime(time, 0, view);
    }
    params->par = getInput(0) ? getAspectRatio(0) : 0.;
    if (params->par != 0.) {
        params->sizeX /= params->par;
    }
    params->erodeSize = erodeSize ? erodeSize->getValueAtTime(time, 0, view) : 0.;
    if (filter) {
        const int f = filter->getValueAtTime(time, 0, view);
        if ((f >= (int)eEdgeDetectFilterSimple) && (f <= (int)eEdgeDetectFilterQuadratic)) {
            params->filter = (EdgeDetectFilterEnum)f;
        }
    }
    if (multiChannel) {
        const int m = multiChannel->getValueAtTime(time, 0, view);
        if ((m >= (int)eEdgeDetectMultiChannelSeparate) && (m <= (int)eEdgeDetectMultiChannelTensor)) {
            params->multiChannel = (EdgeDetectMultiChannelEnum)m;
        }
    }
    params->nms = nms ? nms->getValueAtTime(time, 0, view) : false;
    params->expandRoD = expandRoD ? expandRoD->getValueAtTime(time, 0, view) : true;
    params->cropToFormat = cropToFormat ? cropToFormat->getValueAtTime(time, 0, view) : true;
} // EdgeDetect::getParams

BlurKernels::Filter
EdgeDetect::blurFilter(EdgeDetectFilterEnum filter)
{
    switch (filter) {
    case eEdgeDetectFilterQuasiGaussian:
        return BlurKernels::eFilterQuasiGaussian;
    case eEdgeDetectFilterBox:
        return BlurKernels::eFilterBox;
    case eEdgeDetectFilterTriangle:
        return BlurKernels::eFilterTriangle;
    case eEdgeDetectFilterQuadratic:
        return BlurKernels::eFilterQuadratic;
    case eEdgeDetectFilterSimple:
    case eEdgeDetectFilterSobel:
    case eEdgeDetectFilterRotationInvariant:
    case eEdgeDetectFilterGaussian:
    default:
        return BlurKernels::eFilterGaussian;
    }
}

void
EdgeDetect::getErodeRadius(const RenderScale& scale,
                           const EdgeDetectParams& params,
                           int* rx,
                           int* ry)
{
    const OfxPointD rs = scale.toOfxPointD();
    const double par = (params.par > 0.) ? params.par : 1.;
    const double e = std::abs(params.erodeSize);

    *rx = (int)std::min(kMaxErodeRadius, std::floor(std::max(0., e / par) * rs.x));
    *ry = (int)std::min(kMaxErodeRadius, std::floor(std::max(0., e) * rs.y));
}

RectI
EdgeDetect::getSourceRoI(const RectI& rect,
                         const RenderScale& scale,
                         const EdgeDetectParams& params)
{
    const OfxPointD rs = scale.toOfxPointD();
    const double sx = rs.x * params.sizeX;
    const double sy = rs.y * params.sizeY;
    const BlurKernels::Filter filter = blurFilter(params.filter);

    // Both gradient components are first-order derivatives: order 1 on each axis.
    int deltaX;
    int deltaY;
    if (isGaussianFamily(filter)) {
        deltaX = std::max(3, (int)std::ceil(sx * 1.5)) + 1;
        deltaY = std::max(3, (int)std::ceil(sy * 1.5)) + 1;
    } else {
        const int iter = boxIterations(filter);
        deltaX = iter * static_cast<int>(std::floor((sx - 1) / 2) + 1) + 1;
        deltaY = iter * static_cast<int>(std::floor((sy - 1) / 2) + 1) + 1;
    }
    int erodeX = 0;
    int erodeY = 0;
    getErodeRadius(scale, params, &erodeX, &erodeY);
    const int nmsDelta = params.nms ? 1 : 0;
    deltaX += erodeX + nmsDelta;
    deltaY += erodeY + nmsDelta;

    return RectI(rect.x1 - deltaX, rect.y1 - deltaY, rect.x2 + deltaX, rect.y2 + deltaY);
}

StatusEnum
EdgeDetect::getRegionOfDefinition(U64 hash,
                                  double time,
                                  const RenderScale& scale,
                                  ViewIdx view,
                                  RectD* rod)
{
    EffectInstancePtr input = getInput(0);

    if (!input) {
        return EffectInstance::getRegionOfDefinition(hash, time, scale, view, rod);
    }
    const RenderScale inputScale = input->supportsRenderScale() ? scale : RenderScale::identity;
    RectD srcRoD;
    bool isProjectFormat = false;
    if (input->getRegionOfDefinition_public(input->getRenderHash(), time, inputScale, view, &srcRoD, &isProjectFormat) == eStatusFailed) {
        return eStatusFailed;
    }

    EdgeDetectParams params;
    getParams(time, view, &params);
    const OfxPointD rs = scale.toOfxPointD();
    const double sx = rs.x * params.sizeX;
    const double sy = rs.y * params.sizeY;
    const RectI srcRoDPixel = toPixelEnclosing(srcRoD, scale, getAspectRatio(0));
    const BlurKernels::Filter filter = blurFilter(params.filter);

    // CImgBlurPlugin::getRegionOfDefinition() with first-order derivatives on both axes, which
    // are never an identity: the erosion does not expand the region.
    RectI rodPixel = srcRoDPixel;
    bool changed = false;
    if (params.expandRoD && !isEmptyRect(srcRoDPixel)) {
        int deltaX;
        int deltaY;
        if (isGaussianFamily(filter)) {
            deltaX = std::max(3, (int)std::ceil(sx * 1.5)) + 1;
            deltaY = std::max(3, (int)std::ceil(sy * 1.5)) + 1;
        } else {
            const int iter = boxIterations(filter);
            deltaX = iter * (int)std::ceil((sx - 1) / 2) + 1;
            deltaY = iter * (int)std::ceil((sy - 1) / 2) + 1;
        }
        rodPixel.x1 -= deltaX;
        rodPixel.x2 += deltaX;
        rodPixel.y1 -= deltaY;
        rodPixel.y2 += deltaY;
        changed = true;
    }
    if (params.cropToFormat) {
        // Only the borders the source had inside its format are clamped back to it.
        RectI format;
        EffectInstancePtr formatSource = input->getNearestNonIdentity(time);
        if (formatSource) {
            format = formatSource->getOutputFormat();
        } else {
            Format projectFormat;
            getApp()->getProject()->getProjectDefaultFormat(&projectFormat);
            format = projectFormat;
        }
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
        if (!isEmptyRect(inner)) {
            if ((rodPixel.x1 < inner.x1) && (srcRoDPixel.x1 >= enclosing.x1)) {
                rodPixel.x1 = inner.x1;
                changed = true;
            }
            if ((rodPixel.x2 > inner.x2) && (srcRoDPixel.x2 <= enclosing.x2)) {
                rodPixel.x2 = inner.x2;
                changed = true;
            }
            if ((rodPixel.y1 < inner.y1) && (srcRoDPixel.y1 >= enclosing.y1)) {
                rodPixel.y1 = inner.y1;
                changed = true;
            }
            if ((rodPixel.y2 > inner.y2) && (srcRoDPixel.y2 <= enclosing.y2)) {
                rodPixel.y2 = inner.y2;
                changed = true;
            }
        }
    }
    if (!changed) {
        return EffectInstance::getRegionOfDefinition(hash, time, scale, view, rod);
    }
    *rod = toCanonical(rodPixel, scale, getAspectRatio(-1));

    return eStatusOK;
} // EdgeDetect::getRegionOfDefinition

void
EdgeDetect::getRegionsOfInterest(double time,
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

    EdgeDetectParams params;
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
} // EdgeDetect::getRegionsOfInterest

StatusEnum
EdgeDetect::render(const RenderActionArgs& args)
{
    EdgeDetectParams params;
    getParams(args.time, args.view, &params);

    const OfxPointD rs = args.mappedScale.toOfxPointD();
    const double sx = rs.x * params.sizeX;
    const double sy = rs.y * params.sizeY;
    const BlurKernels::Filter filter = blurFilter(params.filter);
    const bool finiteDifferences = isFiniteDifference(params.filter);
    // EdgeDetect always uses CImg's Neumann (nearest) boundary condition.
    const bool neumann = true;

    // CImgBlurPlugin::renderEdgeDetect(): either a Gaussian pre-blur followed by finite
    // differences, or the x and y gradient components as derivative blurs, each run as a
    // horizontal pass then a vertical one.
    BlurKernels::LineFilter preBlurX;
    BlurKernels::LineFilter preBlurY;
    BlurKernels::LineFilter gxRows;
    BlurKernels::LineFilter gxColumns;
    BlurKernels::LineFilter gyRows;
    BlurKernels::LineFilter gyColumns;
    if (finiteDifferences) {
        const float sigmax = (float)(sx / 2.4);
        const float sigmay = (float)(sy / 2.4);
        if (!((sigmax < 0.1) && (sigmay < 0.1))) {
            preBlurX = BlurKernels::LineFilter::forFilter(filter, sigmax, 0, neumann);
            preBlurY = BlurKernels::LineFilter::forFilter(filter, sigmay, 0, neumann);
        }
    } else if (isGaussianFamily(filter)) {
        const float sigmax = (float)(sx / 2.4);
        const float sigmay = (float)(sy / 2.4);
        gxRows = BlurKernels::LineFilter::forFilter(filter, sigmax, 1, neumann);
        gxColumns = BlurKernels::LineFilter::forFilter(filter, sigmay, 0, neumann);
        gyRows = BlurKernels::LineFilter::forFilter(filter, sigmax, 0, neumann);
        gyColumns = BlurKernels::LineFilter::forFilter(filter, sigmay, 1, neumann);
    } else {
        // CImg's box filter is the identity at size 0, but the derivative is still wanted there.
        const double boxX = (sx <= 0.) ? 1e-8 : sx;
        const double boxY = (sy <= 0.) ? 1e-8 : sy;
        gxRows = BlurKernels::LineFilter::forFilter(filter, static_cast<float>(boxX), 1, neumann);
        gxColumns = BlurKernels::LineFilter::forFilter(filter, static_cast<float>(sy), 0, neumann);
        gyRows = BlurKernels::LineFilter::forFilter(filter, static_cast<float>(sx), 0, neumann);
        gyColumns = BlurKernels::LineFilter::forFilter(filter, static_cast<float>(boxY), 1, neumann);
    }

    // Normalisation so that a blurred unit step edge responds with exactly 1, applied to the
    // finite-difference schemes too when their pre-blur is the Gaussian one.
    bool normalizeX = false;
    bool normalizeY = false;
    double normX = 1.;
    double normY = 1.;
    switch (filter) {
    case BlurKernels::eFilterBox:
        normalizeX = (sx > 1.);
        normalizeY = (sy > 1.);
        normX = sx;
        normY = sy;
        break;
    case BlurKernels::eFilterTriangle:
        normalizeX = (sx > 1.);
        normalizeY = (sy > 1.);
        normX = 2 * sx * sx / (2 * sx + 1);
        normY = 2 * sy * sy / (2 * sy + 1);
        break;
    case BlurKernels::eFilterQuadratic:
        normalizeX = (sx > 1.);
        normalizeY = (sy > 1.);
        normX = (12 * sx * sx * sx) / (9 * sx * sx - 4);
        normY = (12 * sy * sy * sy) / (9 * sy * sy - 4);
        break;
    case BlurKernels::eFilterGaussian:
    case BlurKernels::eFilterQuasiGaussian:
        // The second factor is 1 / 2.4 where sigma would be expected, as in the OpenFX plug-in.
        normalizeX = (sx / 2.4 >= 0.1);
        normalizeY = (sy / 2.4 >= 0.1);
        normX = 2. * M_PI * (sx / 2.4) * (1. / 2.4);
        normY = 2. * M_PI * (sy / 2.4) * (1. / 2.4);
        break;
    }

    int erodeX = 0;
    int erodeY = 0;
    getErodeRadius(args.mappedScale, params, &erodeX, &erodeY);
    const bool erodeOrDilate = (params.erodeSize != 0.) && ((erodeX > 0) || (erodeY > 0));
    const bool takeMax = (params.erodeSize < 0.);

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

    std::vector<EdgePlaneJob> jobs;
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        EdgePlaneJob job;
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
        EdgePlaneJob& job = jobs[j];
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
        while ((processWindow.y2 > processWindow.y1) && maskRowIsZero(*maskAccess, maskBounds, maskChannel, processWindow.x1, processWindow.x2, processWindow.y2 - 1, maskInvert)) {
            --processWindow.y2;
        }
        while ((processWindow.y2 > processWindow.y1) && maskRowIsZero(*maskAccess, maskBounds, maskChannel, processWindow.x1, processWindow.x2, processWindow.y1, maskInvert)) {
            ++processWindow.y1;
        }
        while ((processWindow.x2 > processWindow.x1) && maskColumnIsZero(*maskAccess, maskBounds, maskChannel, processWindow.x1, processWindow.y1, processWindow.y2, maskInvert)) {
            ++processWindow.x1;
        }
        while ((processWindow.x2 > processWindow.x1) && maskColumnIsZero(*maskAccess, maskBounds, maskChannel, processWindow.x2 - 1, processWindow.y1, processWindow.y2, maskInvert)) {
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
        const EdgePlaneJob& job = jobs[j];
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
        const bool separate = (nPlanes == 1) || (params.multiChannel == eEdgeDetectMultiChannelSeparate);
        std::vector<float> buffer(processing ? planeSize * nPlanes : 0);

        if (!buffer.empty()) {
            // 1. The source over the buffer, outside its image by the nearest pixel, divided by
            // the "(Un)premult by" channel where that is usable.
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
                    int cy = y;
                    if (hasSource && ((y < b.y1) || (y >= b.y2))) {
                        cy = (y < b.y1) ? b.y1 : (b.y2 - 1);
                    }
                    const float* srcRow = hasSource ? (const float*)job.srcAccess->pixelAt(b.x1, cy) : NULL;
                    const float* divisorRow = NULL;
                    const RectI& db = job.divisorBounds;
                    if (srcRow && job.divisor && (cy >= db.y1) && (cy < db.y2)) {
                        divisorRow = (const float*)job.divisorAccess->pixelAt(db.x1, cy);
                    }
                    for (int i = 0; i < bufferWidth; ++i) {
                        const int x = bufferRect.x1 + i;
                        std::fill(pixel.begin(), pixel.end(), 0.f);
                        if (srcRow) {
                            const int cx = (x < b.x1) ? b.x1 : ((x >= b.x2) ? (b.x2 - 1) : x);
                            const float* srcPix = srcRow + (std::size_t)(cx - b.x1) * job.srcNComps;
                            for (int c = 0; (c < nComps) && (c < 4); ++c) {
                                if (srcIndex[c] >= 0) {
                                    pixel[c] = srcPix[srcIndex[c]];
                                }
                            }
                            if (divisorRow && (cx >= db.x1) && (cx < db.x2)) {
                                const float d = divisorRow[(std::size_t)(cx - db.x1) * job.divisorNComps + job.divisorChannel];
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

            // Runs a line filter over whole rows, then another over whole columns, of every plane
            // of `data`.
            const auto separableFilter = [&](std::vector<float>& data,
                                             const BlurKernels::LineFilter& rowFilter,
                                             const BlurKernels::LineFilter& columnFilter) {
                if (!rowFilter.isIdentity()) {
                    const std::function<void(int, int)> rows = [&](int first, int end) {
                        BlurKernels::LineScratch scratch;
                        for (int line = first; line < end; ++line) {
                            if ((((line - first) % kAbortCheckLines) == 0) && abortState.check()) {
                                return;
                            }
                            const int p = line / bufferHeight;
                            const int row = line % bufferHeight;
                            rowFilter.apply(&data[(std::size_t)p * planeSize + (std::size_t)row * bufferWidth], bufferWidth, 1, scratch);
                        }
                    };
                    forEachLineChunk(nPlanes * bufferHeight, (std::size_t)bufferWidth, nThreads, rows);
                    if (abortState.check()) {
                        return;
                    }
                }
                if (!columnFilter.isIdentity()) {
                    const std::function<void(int, int)> columns = [&](int first, int end) {
                        BlurKernels::LineScratch scratch;
                        for (int line = first; line < end; ++line) {
                            if ((((line - first) % kAbortCheckLines) == 0) && abortState.check()) {
                                return;
                            }
                            const int p = line / bufferWidth;
                            const int column = line % bufferWidth;
                            columnFilter.apply(&data[(std::size_t)p * planeSize + column], bufferHeight, bufferWidth, scratch);
                        }
                    };
                    forEachLineChunk(nPlanes * bufferWidth, (std::size_t)bufferHeight, nThreads, columns);
                }
            };

            // 2. The two gradient components.
            std::vector<float> gx;
            std::vector<float> gy;
            if (finiteDifferences) {
                separableFilter(buffer, preBlurX, preBlurY);
                if (abortState.check()) {
                    return eStatusOK;
                }
                gx.resize(buffer.size());
                gy.resize(buffer.size());
                // CImg<float>::get_gradient(0, scheme): centred differences (one-sided and halved
                // at the ends), Sobel or rotation-invariant 3x3 kernels with repeated edges, all
                // in float. A one-pixel-wide axis has a zero derivative.
                const float ra = (float)(0.25f * (2 - std::sqrt(2.f)));
                const float rb = (float)(0.5f * (std::sqrt(2.f) - 1));
                const EdgeDetectFilterEnum scheme = params.filter;
                const std::function<void(int, int)> stencilRows = [&](int first, int end) {
                    for (int line = first; line < end; ++line) {
                        if ((((line - first) % kAbortCheckLines) == 0) && abortState.check()) {
                            return;
                        }
                        const int p = line / bufferHeight;
                        const int row = line % bufferHeight;
                        const float* blurred = &buffer[(std::size_t)p * planeSize];
                        float* gxRow = &gx[(std::size_t)p * planeSize + (std::size_t)row * bufferWidth];
                        float* gyRow = &gy[(std::size_t)p * planeSize + (std::size_t)row * bufferWidth];
                        const int rowP = std::max(0, row - 1);
                        const int rowN = std::min(bufferHeight - 1, row + 1);
                        const float* lineP = blurred + (std::size_t)rowP * bufferWidth;
                        const float* lineC = blurred + (std::size_t)row * bufferWidth;
                        const float* lineN = blurred + (std::size_t)rowN * bufferWidth;
                        for (int i = 0; i < bufferWidth; ++i) {
                            const int iP = std::max(0, i - 1);
                            const int iN = std::min(bufferWidth - 1, i + 1);
                            const float Ipp = lineP[iP];
                            const float Icp = lineP[i];
                            const float Inp = lineP[iN];
                            const float Ipc = lineC[iP];
                            const float Inc = lineC[iN];
                            const float Ipn = lineN[iP];
                            const float Icn = lineN[i];
                            const float Inn = lineN[iN];
                            float vx;
                            float vy;
                            if (scheme == eEdgeDetectFilterSobel) {
                                vx = -Ipp + Inp - 2 * Ipc + 2 * Inc - Ipn + Inn;
                                vy = -Ipp - 2 * Icp - Inp + Ipn + 2 * Icn + Inn;
                                vx = vx / 8;
                                vy = vy / 8;
                            } else if (scheme == eEdgeDetectFilterRotationInvariant) {
                                vx = -ra * Ipp - rb * Ipc - ra * Ipn + ra * Inp + rb * Inc + ra * Inn;
                                vy = -ra * Ipp - rb * Icp - ra * Inp + ra * Ipn + rb * Icn + ra * Inn;
                            } else {
                                vx = (Inc - Ipc) / 2;
                                vy = (Icn - Icp) / 2;
                            }
                            gxRow[i] = (bufferWidth > 1) ? vx : 0.f;
                            gyRow[i] = (bufferHeight > 1) ? vy : 0.f;
                        }
                    }
                };
                forEachLineChunk(nPlanes * bufferHeight, (std::size_t)bufferWidth, nThreads, stencilRows);
            } else {
                gx = buffer;
                gy.swap(buffer);
                separableFilter(gx, gxRows, gxColumns);
                if (abortState.check()) {
                    return eStatusOK;
                }
                separableFilter(gy, gyRows, gyColumns);
                buffer.resize(gx.size());
            }
            if (abortState.check()) {
                return eStatusOK;
            }

            // 3. Normalisation and the channel combination: the edge magnitude goes to the
            // buffer, and gx, gy keep the direction non-maxima suppression follows.
            const std::function<void(int, int)> combineRows = [&](int first, int end) {
                for (int row = first; row < end; ++row) {
                    if ((((row - first) % kAbortCheckLines) == 0) && abortState.check()) {
                        return;
                    }
                    for (int i = 0; i < bufferWidth; ++i) {
                        const std::size_t o = (std::size_t)row * bufferWidth + i;
                        for (int p = 0; p < nPlanes; ++p) {
                            const std::size_t k = (std::size_t)p * planeSize + o;
                            if (normalizeX) {
                                gx[k] = (float)(gx[k] * normX);
                            }
                            if (normalizeY) {
                                gy[k] = (float)(gy[k] * normY);
                            }
                        }
                        if (separate) {
                            for (int p = 0; p < nPlanes; ++p) {
                                const std::size_t k = (std::size_t)p * planeSize + o;
                                const double gxd = gx[k];
                                const double gyd = gy[k];
                                const double sqe = gxd * gxd + gyd * gyd;
                                buffer[k] = static_cast<float>(std::sqrt(std::max(sqe, 0.)));
                            }
                        } else if (params.multiChannel == eEdgeDetectMultiChannelRMS) {
                            // The direction is the plain sum of the channel gradients: the OpenFX
                            // plug-in's sign alignment never triggers, and its result is kept.
                            double sumsq = 0.;
                            double gradx = 0.;
                            double grady = 0.;
                            for (int p = 0; p < nPlanes; ++p) {
                                const std::size_t k = (std::size_t)p * planeSize + o;
                                const double gxd = gx[k];
                                const double gyd = gy[k];
                                sumsq += gxd * gxd + gyd * gyd;
                                gradx += gxd;
                                grady += gyd;
                            }
                            const double sqe = sumsq / nPlanes;
                            const double e = std::sqrt(std::max(sqe, 0.));
                            for (int p = 0; p < nPlanes; ++p) {
                                const std::size_t k = (std::size_t)p * planeSize + o;
                                buffer[k] = static_cast<float>(e);
                                gx[k] = static_cast<float>(gradx);
                                gy[k] = static_cast<float>(grady);
                            }
                        } else if (params.multiChannel == eEdgeDetectMultiChannelMax) {
                            double maxsq = 0.;
                            double gradx = 0.;
                            double grady = 0.;
                            for (int p = 0; p < nPlanes; ++p) {
                                const std::size_t k = (std::size_t)p * planeSize + o;
                                const double gxd = gx[k];
                                const double gyd = gy[k];
                                const double sq = gxd * gxd + gyd * gyd;
                                if (sq > maxsq) {
                                    maxsq = sq;
                                    gradx = gxd;
                                    grady = gyd;
                                }
                            }
                            const double e = std::sqrt(std::max(maxsq, 0.));
                            for (int p = 0; p < nPlanes; ++p) {
                                const std::size_t k = (std::size_t)p * planeSize + o;
                                buffer[k] = static_cast<float>(e);
                                gx[k] = static_cast<float>(gradx);
                                gy[k] = static_cast<float>(grady);
                            }
                        } else {
                            // Di Zenzo: the square root of the largest eigenvalue of the
                            // structure tensor, whose eigenvector (Jxy, e1 - Jx) is the direction.
                            double Jx = 0.;
                            double Jy = 0.;
                            double Jxy = 0.;
                            for (int p = 0; p < nPlanes; ++p) {
                                const std::size_t k = (std::size_t)p * planeSize + o;
                                const double gxd = gx[k];
                                const double gyd = gy[k];
                                Jx += gxd * gxd;
                                Jy += gyd * gyd;
                                Jxy += gxd * gyd;
                            }
                            const double sqD = Jx * Jx - 2 * Jx * Jy + Jy * Jy + 4 * Jxy * Jxy;
                            const double D = std::sqrt(std::max(sqD, 0.));
                            const double e1 = (Jx + Jy + D) / 2;
                            const double e = std::sqrt(std::max(e1, 0.));
                            for (int p = 0; p < nPlanes; ++p) {
                                const std::size_t k = (std::size_t)p * planeSize + o;
                                buffer[k] = static_cast<float>(e);
                                gx[k] = static_cast<float>(Jxy);
                                gy[k] = static_cast<float>(e1 - Jx);
                            }
                        }
                    }
                }
            };
            forEachLineChunk(bufferHeight, (std::size_t)bufferWidth * nPlanes, nThreads, combineRows);
            if (abortState.check()) {
                return eStatusOK;
            }

            // 4. Erosion (positive size) or dilation (negative size) by a (2 rx + 1) x (2 ry + 1)
            // rectangle, rows then columns.
            if (erodeOrDilate) {
                if ((erodeX > 0) && (bufferWidth > 1)) {
                    const std::function<void(int, int)> rows = [&](int first, int end) {
                        ErodeDilateKernels::LineScratch scratch;
                        for (int line = first; line < end; ++line) {
                            if ((((line - first) % kAbortCheckLines) == 0) && abortState.check()) {
                                return;
                            }
                            const int p = line / bufferHeight;
                            const int row = line % bufferHeight;
                            ErodeDilateKernels::filterLine(&buffer[(std::size_t)p * planeSize + (std::size_t)row * bufferWidth], bufferWidth, 1, erodeX, takeMax, scratch);
                        }
                    };
                    forEachLineChunk(nPlanes * bufferHeight, (std::size_t)bufferWidth, nThreads, rows);
                    if (abortState.check()) {
                        return eStatusOK;
                    }
                }
                if ((erodeY > 0) && (bufferHeight > 1)) {
                    const std::function<void(int, int)> columns = [&](int first, int end) {
                        ErodeDilateKernels::LineScratch scratch;
                        for (int line = first; line < end; ++line) {
                            if ((((line - first) % kAbortCheckLines) == 0) && abortState.check()) {
                                return;
                            }
                            const int p = line / bufferWidth;
                            const int column = line % bufferWidth;
                            ErodeDilateKernels::filterLine(&buffer[(std::size_t)p * planeSize + column], bufferHeight, (std::ptrdiff_t)bufferWidth, erodeY, takeMax, scratch);
                        }
                    };
                    forEachLineChunk(nPlanes * bufferWidth, (std::size_t)bufferHeight, nThreads, columns);
                    if (abortState.check()) {
                        return eStatusOK;
                    }
                }
            }

            // 5. Non-maxima suppression along the gradient direction, with the parabolic peak
            // value at the maxima; the buffer's border is cleared.
            if (params.nms) {
                const std::vector<float> eroded(buffer);
                const int cmax = separate ? nPlanes : 1;
                const std::function<void(int, int)> nmsRows = [&](int first, int end) {
                    for (int y = first; y < end; ++y) {
                        if ((((y - first) % kAbortCheckLines) == 0) && abortState.check()) {
                            return;
                        }
                        for (int x = 0; x < bufferWidth; ++x) {
                            const std::size_t o = (std::size_t)y * bufferWidth + x;
                            if ((x == 0) || (x == bufferWidth - 1) || (y == 0) || (y == bufferHeight - 1)) {
                                for (int p = 0; p < nPlanes; ++p) {
                                    buffer[(std::size_t)p * planeSize + o] = 0.f;
                                }
                                continue;
                            }
                            for (int c = 0; c < cmax; ++c) {
                                const std::size_t base = (std::size_t)c * planeSize;
                                const float* mag = &eroded[base];
                                float value = 0.f;
                                float gradx = gx[base + o];
                                float grady = gy[base + o];
                                if ((gradx != 0.f) || (grady != 0.f)) {
                                    const bool horiz = std::abs(gradx) >= std::abs(grady);
                                    const std::size_t rowP = (std::size_t)(y - 1) * bufferWidth;
                                    const std::size_t rowC = (std::size_t)y * bufferWidth;
                                    const std::size_t rowN = (std::size_t)(y + 1) * bufferWidth;
                                    float Ipp;
                                    float Ipc;
                                    float Ipn;
                                    float Inp;
                                    float Inc;
                                    float Inn;
                                    if (horiz) {
                                        Ipp = mag[rowP + x - 1];
                                        Ipc = mag[rowC + x - 1];
                                        Ipn = mag[rowN + x - 1];
                                        Inp = mag[rowP + x + 1];
                                        Inc = mag[rowC + x + 1];
                                        Inn = mag[rowN + x + 1];
                                    } else {
                                        std::swap(gradx, grady);
                                        Ipp = mag[rowP + x - 1];
                                        Ipc = mag[rowP + x];
                                        Ipn = mag[rowP + x + 1];
                                        Inp = mag[rowN + x - 1];
                                        Inc = mag[rowN + x];
                                        Inn = mag[rowN + x + 1];
                                    }
                                    const double alpha = grady / gradx;
                                    const double Ip = parabola(Ipp, Ipc, Ipn, -alpha);
                                    const double In = parabola(Inp, Inc, Inn, +alpha);
                                    const double Icv = mag[rowC + x];
                                    if ((Ip < Icv) && (Icv >= In)) {
                                        value = static_cast<float>(parabolaMaxValue(Ip, Icv, In));
                                    }
                                }
                                if (separate) {
                                    buffer[base + o] = value;
                                } else {
                                    for (int p = 0; p < nPlanes; ++p) {
                                        buffer[(std::size_t)p * planeSize + o] = value;
                                    }
                                }
                            }
                        }
                    }
                };
                forEachLineChunk(bufferHeight, (std::size_t)bufferWidth * nPlanes, nThreads, nmsRows);
                if (abortState.check()) {
                    return eStatusOK;
                }
            }
        }

        // 6. The window: multiply back, mask and mix against the undivided source; unprocessed
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
                        float v = (inBuffer && !buffer.empty()) ? buffer[(std::size_t)p * planeSize + offset] : 0.f;
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
} // EdgeDetect::render

NATRON_NAMESPACE_EXIT
