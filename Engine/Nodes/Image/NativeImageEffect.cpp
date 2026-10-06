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

#include "NativeImageEffect.h"

#include <algorithm>
#include <cstddef>
#include <list>
#include <memory>
#include <vector>

#include "Engine/Image.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/RectD.h"

NATRON_NAMESPACE_ENTER

namespace {
// How many rows render() runs between two abort checks.
const int kAbortCheckRows = 16;

// The index in an nComps-channel pixel of the channel on colour bit `bit`, or -1.
int
channelIndexForBit(int nComps,
                   int bit)
{
    if (nComps == 1) {
        return (bit == 3) ? 0 : -1;
    }

    return (bit < nComps) ? bit : -1;
}

// The bits processed in a dstNComps-channel image holding `plane`. A one-channel plane rendered
// into a wider image sits where EffectInstance's copy back reads it: alpha for a colour plane,
// channel 0 otherwise.
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

struct PlaneJob {
    ImagePtr dst;
    std::bitset<4> channels;
    ImagePtr src;
    ImagePtr divisor;
    int divisorChannel;
    int skipChannel;

    PlaneJob()
        : dst()
        , channels()
        , src()
        , divisor()
        , divisorChannel(-1)
        , skipChannel(-1)
    {
    }
};

bool
isFloatImage(const ImagePtr& image)
{
    return !image || (image->getBitDepth() == eImageBitDepthFloat);
}

// Copies `width` pixels of row y starting at x0 into row (nComps floats per pixel), writing zero
// wherever the image has no pixel. An image in another layout is mapped channel by colour bit,
// a channel it lacks reading as zero.
void
readSourceRow(const Image* image,
              const Image::ReadAccess* access,
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
    const RectI& bounds = image->getBounds();
    if ((y < bounds.y1) || (y >= bounds.y2)) {
        return;
    }
    const int xStart = std::max(x0, bounds.x1);
    const int xEnd = std::min(x0 + width, bounds.x2);
    if (xStart >= xEnd) {
        return;
    }
    const int srcNComps = (int)image->getComponentsCount();
    const float* srcPix = (const float*)access->pixelAt(xStart, y);
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
    const RectI& bounds = image->getBounds();
    if ((y < bounds.y1) || (y >= bounds.y2)) {
        return;
    }
    const int xStart = std::max(x0, bounds.x1);
    const int xEnd = std::min(x0 + width, bounds.x2);
    if (xStart >= xEnd) {
        return;
    }
    const int nComps = (int)image->getComponentsCount();
    const float* pix = (const float*)access->pixelAt(xStart, y);
    for (int x = xStart; x < xEnd; ++x, pix += nComps) {
        row[x - x0] = pix[channel];
    }
}
} // anonymous namespace

NativeImageEffect::NativeImageEffect(NodePtr node,
                                     const NativeImageTraits& traits)
    : NativeEffectBase(node)
    , _traits(traits)
    , _mix()
    , _maskInvert()
{
}

NativeImageEffect::~NativeImageEffect()
{
}

void
NativeImageEffect::addAcceptedComponents(int inputNb,
                                         std::list<ImageLayerDesc>* comps)
{
    if (isInputMask(inputNb)) {
        comps->push_back(ImageLayerDesc::getAlphaComponents());

        return;
    }
    NativeEffectBase::addAcceptedComponents(inputNb, comps);
}

void
NativeImageEffect::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

LayerKnobSpec
NativeImageEffect::getLayerKnobSpec() const
{
    if (_traits.generator) {
        return LayerKnobSpec(LayerKnobSpec::eKindLayerSelect, LayerKnobSpec::eRoleTarget, true);
    }

    return EffectInstance::getLayerKnobSpec();
}

bool
NativeImageEffect::isHostChannelSelectorSupported(bool* defaultR,
                                                  bool* defaultG,
                                                  bool* defaultB,
                                                  bool* defaultA) const
{
    *defaultR = _traits.defaultChannels[0];
    *defaultG = _traits.defaultChannels[1];
    *defaultB = _traits.defaultChannels[2];
    *defaultA = _traits.defaultChannels[3];

    return true;
}

int
NativeImageEffect::getMaskInput() const
{
    const int nInputs = getNInputs();

    for (int i = 0; i < nInputs; ++i) {
        if (isInputMask(i)) {
            return i;
        }
    }

    return -1;
}

PixelKernelPtr
NativeImageEffect::makeKernel(const KernelContext& /*context*/)
{
    return PixelKernelPtr();
}

void
NativeImageEffect::addMaskMixKnobs(const KnobPagePtr& page)
{
    KnobBoolPtr maskInvert = createKnob<KnobBool>(std::string("Invert Mask"));

    maskInvert->setName(kOfxMaskInvertParamName);
    maskInvert->setHintToolTip(tr("When checked, the effect is fully applied where the mask is 0."));
    maskInvert->setDefaultValue(false);
    if (page) {
        page->addKnob(maskInvert);
    }
    _maskInvert = maskInvert;

    KnobDoublePtr mix = createKnob<KnobDouble>(std::string("Mix"));
    mix->setName(kOfxMixParamName);
    mix->setHintToolTip(tr("Mix factor between the original and the transformed image."));
    mix->setDefaultValue(1.);
    mix->setIncrement(0.01);
    mix->setMinimum(0.);
    mix->setMaximum(1.);
    mix->setDisplayMinimum(0.);
    mix->setDisplayMaximum(1.);
    if (page) {
        page->addKnob(mix);
    }
    _mix = mix;
}

double
NativeImageEffect::getMixValue(double time,
                               ViewIdx view) const
{
    KnobDoublePtr mix = _mix.lock();

    return mix ? mix->getValueAtTime(time, 0, view) : 1.;
}

bool
NativeImageEffect::getMaskInvertValue(double time,
                                      ViewIdx view) const
{
    KnobBoolPtr maskInvert = _maskInvert.lock();

    return maskInvert ? maskInvert->getValueAtTime(time, 0, view) : false;
}

bool
NativeImageEffect::isMaskApplied() const
{
    const int maskInput = getMaskInput();

    return (maskInput >= 0) && getInput(maskInput) && isMaskEnabled(maskInput);
}

bool
NativeImageEffect::isIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view,
                              double* inputTime,
                              ViewIdx* inputView,
                              int* inputNb)
{
    *inputTime = time;
    *inputView = view;
    *inputNb = 0;

    if (getMixValue(time, view) == 0.) {
        return true;
    }
    if (isIdentityOp(time, scale, roi, view)) {
        return true;
    }

    // A mask that does not reach the window leaves the source unchanged there; an inverted
    // one applies the effect in full instead.
    const int maskInput = getMaskInput();
    if (isMaskApplied() && !getMaskInvertValue(time, view)) {
        EffectInstancePtr mask = getInput(maskInput);
        const RenderScale maskScale = mask->supportsRenderScale() ? scale : RenderScale::identity;
        RectD maskRoD;
        bool isProjectFormat = false;
        if (mask->getRegionOfDefinition_public(mask->getRenderHash(), time, maskScale, view, &maskRoD, &isProjectFormat) != eStatusFailed) {
            const RectI maskPixels = maskRoD.toPixelEnclosing(scale.toMipmapLevel(), mask->getAspectRatio(-1));
            if (!roi.intersects(maskPixels)) {
                return true;
            }
        }
    }

    return false;
} // NativeImageEffect::isIdentity

StatusEnum
NativeImageEffect::render(const RenderActionArgs& args)
{
    if (!isPointOp()) {
        return eStatusFailed;
    }

    KernelContext context;
    context.time = args.time;
    context.view = args.view;
    context.mappedScale = args.mappedScale;
    context.processChannels = args.processChannels;
    const PixelKernelPtr kernel = makeKernel(context);
    if (!kernel) {
        return eStatusFailed;
    }

    const float mix = (float)getMixValue(args.time, args.view);
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

    KnobChannelSelectPtr unPremultBy = (_traits.hostUnPremult && node) ? node->getUnPremultBySelector() : KnobChannelSelectPtr();
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

    std::vector<PlaneJob> jobs;
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        PlaneJob job;
        job.dst = it->second;
        if (!job.dst) {
            jobs.push_back(job);
            continue;
        }
        const int dstNComps = (int)job.dst->getComponentsCount();
        job.channels = processedBitsForImage(it->first, dstNComps, args.processChannels);

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

    std::shared_ptr<Image::ReadAccess> maskAccess;
    if (mask) {
        maskAccess = std::make_shared<Image::ReadAccess>(mask.get());
    }
    std::vector<float> maskRow(doMask ? width : 0);
    std::vector<float> divisorRow;

    for (std::vector<PlaneJob>::const_iterator job = jobs.begin(); job != jobs.end(); ++job) {
        if (!job->dst) {
            continue;
        }
        const int nComps = (int)job->dst->getComponentsCount();
        const std::size_t rowSize = (std::size_t)width * nComps;
        std::vector<float> sourceRow(rowSize);
        std::vector<float> dividedRow(rowSize);
        std::vector<float> kernelRow(rowSize);
        if (job->divisor) {
            divisorRow.resize(width);
        }

        std::shared_ptr<Image::ReadAccess> srcAccess;
        if (job->src) {
            srcAccess = std::make_shared<Image::ReadAccess>(job->src.get());
        }
        std::shared_ptr<Image::ReadAccess> divisorAccess;
        if (job->divisor) {
            divisorAccess = std::make_shared<Image::ReadAccess>(job->divisor.get());
        }
        Image::WriteAccess dstAccess(job->dst.get());

        RowIO io;
        io.src[0] = &dividedRow[0];
        io.nSrc = 1;
        io.mask = doMask ? &maskRow[0] : 0;
        io.divisor = job->divisor ? &divisorRow[0] : 0;
        io.divisorSkipChannel = job->divisor ? job->skipChannel : -1;
        io.dst = &kernelRow[0];
        io.x0 = roi.x1;
        io.width = width;
        io.nComps = nComps;
        io.channels = job->channels;

        for (int y = roi.y1; y < roi.y2; ++y) {
            if ((((y - roi.y1) % kAbortCheckRows) == 0) && aborted()) {
                return eStatusOK;
            }

            readSourceRow(job->src.get(), srcAccess.get(), roi.x1, y, width, nComps, &sourceRow[0]);
            std::copy(sourceRow.begin(), sourceRow.end(), dividedRow.begin());
            if (job->divisor) {
                readChannelRow(job->divisor.get(), divisorAccess.get(), job->divisorChannel, roi.x1, y, width, 1.f, &divisorRow[0]);
                for (int i = 0; i < width; ++i) {
                    const float d = divisorRow[i];
                    if (!Image::unPremultDivisorIsUsable(d)) {
                        continue;
                    }
                    float* pix = &dividedRow[(std::size_t)i * nComps];
                    for (int c = 0; (c < nComps) && (c < 4); ++c) {
                        if (c != job->skipChannel) {
                            pix[c] = Image::unPremultiplyValue(pix[c], d);
                        }
                    }
                }
            }
            if (doMask) {
                readChannelRow(mask.get(), maskAccess.get(), maskChannel, roi.x1, y, width, 0.f, &maskRow[0]);
            }

            io.y = y;
            kernel->processRow(io);

            float* dstPix = (float*)dstAccess.pixelAt(roi.x1, y);
            if (!dstPix) {
                continue;
            }
            for (int i = 0; i < width; ++i, dstPix += nComps) {
                const float* srcPix = &sourceRow[(std::size_t)i * nComps];
                const float* outPix = &kernelRow[(std::size_t)i * nComps];
                float alpha = mix;
                if (doMask) {
                    const float maskScale = maskInvert ? (1.f - maskRow[i]) : maskRow[i];
                    alpha = maskScale * mix;
                }
                for (int c = 0; c < nComps; ++c) {
                    if ((c >= 4) || !job->channels[pixelKernelChannelBit(nComps, c)]) {
                        dstPix[c] = srcPix[c];
                        continue;
                    }
                    float v = outPix[c];
                    if (job->divisor && (c != job->skipChannel)) {
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
    }

    return eStatusOK;
} // NativeImageEffect::render

NATRON_NAMESPACE_EXIT
