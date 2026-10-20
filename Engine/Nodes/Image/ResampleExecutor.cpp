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

#include "ResampleExecutor.h"

#include <algorithm>
#include <bitset>
#include <cstddef>
#include <functional>
#include <list>
#include <memory>
#include <utility>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/Image/PassExecutor.h"
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/Nodes/Image/StagePlan.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

using TransformMath::Mat3;

namespace {
// How many rows runResampleCpu() runs between two abort checks.
const int kAbortCheckRows = 16;

// One output plane of a render. The matrices are built and the images locked on the calling
// thread; the band threads only read through source, params and the accesses. params and
// referenceParams point into transforms and referenceTransforms, so a plane is never copied.
struct ResamplePlane {
    ImagePtr dst;
    ImagePtr src;
    Transform::Matrix3x3Ptr inputTransform;
    std::bitset<4> channels;
    int nComps = 0;
    int srcNComps = 0;
    RectI srcBounds;
    std::shared_ptr<Image::ReadAccess> srcAccess;
    std::shared_ptr<Image::WriteAccess> dstAccess;
    std::vector<float> converted;
    Resampler::SourceImage source;
    Resampler::SamplingTransforms transforms;
    Resampler::ResampleParams params;
    bool resampleReference = false;
    Resampler::SamplingTransforms referenceTransforms;
    Resampler::ResampleParams referenceParams;

    ResamplePlane() = default;
    ResamplePlane(const ResamplePlane&) = delete;
    ResamplePlane& operator=(const ResamplePlane&) = delete;
};

struct ResampleBand {
    std::size_t plane;
    int y1;
    int y2;

    ResampleBand(std::size_t planeIndex,
                 int firstRow,
                 int endRow)
        : plane(planeIndex)
        , y1(firstRow)
        , y2(endRow)
    {
    }
};

// Copies the source pixels into out in an nComps layout, mapping channels by colour bit and
// reading zero for a channel the source lacks.
void
convertSource(const Image::ReadAccess& access,
              const RectI& bounds,
              int srcNComps,
              int nComps,
              std::vector<float>* out)
{
    const std::size_t width = (std::size_t)bounds.width();
    const std::size_t height = (std::size_t)bounds.height();

    out->assign(width * height * (std::size_t)nComps, 0.f);
    int srcIndex[4] = { -1, -1, -1, -1 };
    for (int c = 0; (c < nComps) && (c < 4); ++c) {
        srcIndex[c] = PassExec::channelIndexForBit(srcNComps, pixelKernelChannelBit(nComps, c));
    }
    for (int y = bounds.y1; y < bounds.y2; ++y) {
        const float* srcPix = (const float*)access.pixelAt(bounds.x1, y);
        if (!srcPix) {
            continue;
        }
        float* dstPix = &(*out)[(std::size_t)(y - bounds.y1) * width * nComps];
        for (std::size_t x = 0; x < width; ++x, srcPix += srcNComps, dstPix += nComps) {
            for (int c = 0; (c < nComps) && (c < 4); ++c) {
                if (srcIndex[c] >= 0) {
                    dstPix[c] = srcPix[srcIndex[c]];
                }
            }
        }
    }
}

// Pixels [x0, x0 + width) of row y of src, src.nComps floats each, zero outside its bounds.
void
readSourceImageRow(const Resampler::SourceImage& src,
                   int x0,
                   int y,
                   int width,
                   float* row)
{
    const int nComps = src.nComps;

    std::fill(row, row + (std::size_t)width * nComps, 0.f);
    if (!src.isValid() || (y < src.bounds.y1) || (y >= src.bounds.y2)) {
        return;
    }
    const int xStart = std::max(x0, src.bounds.x1);
    const int xEnd = std::min(x0 + width, src.bounds.x2);
    if (xStart >= xEnd) {
        return;
    }
    const float* pix = src.pixel(xStart, y);
    std::copy(pix, pix + (std::size_t)(xEnd - xStart) * nComps, row + (std::size_t)(xStart - x0) * nComps);
}

RectD
projectRect(const EffectInstance& effect)
{
    Format format;

    effect.getApp()->getProject()->getProjectDefaultFormat(&format);
    const RectI pixel(format.x1, format.y1, format.x2, format.y2);

    return pixel.toCanonical_noClipping(0, format.getPixelAspectRatio());
}

Resampler::RegionParams
regionParams(const ResampleRequest& request,
             const ResampleRegionStages& stages)
{
    Resampler::RegionParams params;
    const ResampleSettings& settings = request.settings;

    params.invert = settings.invert;
    params.blur = settings.blur;
    params.filter = settings.filter;
    params.blackOutside = settings.blackOutside;
    params.isIdentity = settings.transformIsIdentity;
    params.doMasking = stages.doMasking;
    params.mix = stages.doMasking ? stages.mix : 1.;

    return params;
}

// The source's region of definition at scale; false when the source fails to give one.
bool
sourceRegionOfDefinition(const EffectInstancePtr& input,
                         double time,
                         const RenderScale& scale,
                         ViewIdx view,
                         RectD* rod)
{
    const RenderScale inputScale = input->supportsRenderScale() ? scale : RenderScale::identity;
    bool isProjectFormat = false;

    return input->getRegionOfDefinition_public(input->getRenderHash(), time, inputScale, view, rod, &isProjectFormat) != eStatusFailed;
}

// The offset's translation in canonical units: x through the output's pixel aspect ratio, and
// both axes through the horizontal render scale.
void
offsetToCanonical(const EffectInstance& effect,
                  const PixelOffset& offset,
                  const RenderScale& scale,
                  double* dx,
                  double* dy)
{
    const double s = scale.toOfxPointD().x;

    *dx = offset.x * effect.getAspectRatio(-1) / s;
    *dy = offset.y / s;
}
} // anonymous namespace

StatusEnum
runResampleCpu(EffectInstance& effect,
               const StagePlan& plan,
               const EffectInstance::RenderActionArgs& args,
               const ResampleRequest& request)
{
    const bool matrix = request.form == ResampleRequest::Form::Matrix;
    if (!matrix && (request.form != ResampleRequest::Form::Offset)) {
        return eStatusFailed;
    }
    const float mix = (float)plan.mix;
    const bool maskInvert = plan.maskInvert;
    const bool passThrough = plan.stages.has(Stage::PassThroughUnprocessed);
    const bool readsReference = plan.stages.has(Stage::MaskMix) || passThrough;
    const int sourceInput = plan.sourceInput;

    // Every image is fetched before any is locked: fetching renders upstream, which may write
    // into a cached image this render would otherwise already hold a read lock on.
    const bool doMask = plan.applyMask;
    ImagePtr mask;
    const int maskChannel = plan.maskChannel;
    if (doMask) {
        if (maskChannel >= 0) {
            RectI maskRoI;
            mask = effect.getImage(plan.maskInput, args.time, args.mappedScale, args.view, NULL, &plan.maskLayer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &maskRoI);
        }
        if (mask && ((maskChannel >= (int)mask->getComponentsCount()))) {
            mask.reset();
        }
    }
    if (!PassExec::isFloatImage(mask)) {
        return eStatusFailed;
    }

    std::vector<std::unique_ptr<ResamplePlane>> planes;
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        std::unique_ptr<ResamplePlane> plane(new ResamplePlane());
        plane->dst = it->second;
        if (plane->dst) {
            plane->nComps = (int)plane->dst->getComponentsCount();
            plane->channels = PassExec::processedBitsForImage(it->first, plane->nComps, args.processChannels);
            ImageLayerDesc sourceLayer;
            if ((sourceInput >= 0) && effect.getInput(sourceInput) && effect.resolveInputPlaneForRender(sourceInput, args.time, args.view, &sourceLayer, NULL)) {
                RectI sourceRoI;
                // Mapped to the clip's components, which is the layout the output plane is rendered in.
                plane->src = effect.getImage(sourceInput, args.time, args.mappedScale, args.view, NULL, &sourceLayer, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &sourceRoI, matrix ? &plane->inputTransform : NULL);
            }
            if (!PassExec::isFloatImage(plane->dst) || !PassExec::isFloatImage(plane->src)) {
                return eStatusFailed;
            }
        }
        planes.push_back(std::move(plane));
    }

    const RectI& roi = args.roi;
    const int width = roi.width();
    if ((width <= 0) || (roi.height() <= 0)) {
        return eStatusOK;
    }

    Resampler::FilterEnum filter = request.settings.filter;
    if (args.draftMode) {
        filter = Resampler::eFilterImpulse;
    }
    Resampler::BlurSettings blur = request.settings.blur;
    // A directional blur ignores the shutter.
    if (blur.directionalBlur) {
        blur.shutter = 0.;
    }
    const OfxPointD s = args.mappedScale.toOfxPointD();
    const double dstPar = effect.getAspectRatio(-1);
    const PixelOffset offset = request.offset;

    // Images are locked, their bounds read and the matrices built here, on the calling thread;
    // the band threads only read through these.
    PassExec::MaskInput maskSource;
    maskSource.image = mask;
    maskSource.lock();
    const int nThreads = appPTR->getNCPUsAvailableForEffect();
    std::vector<RectI> bandRects;
    makeStageRowBands(roi, nThreads, &bandRects);
    std::vector<ResampleBand> bands;
    for (std::size_t p = 0; p < planes.size(); ++p) {
        ResamplePlane& plane = *planes[p];
        if (!plane.dst) {
            continue;
        }
        const int nComps = plane.nComps;
        if (plane.src) {
            plane.srcBounds = plane.src->getBounds();
            plane.srcNComps = (int)plane.src->getComponentsCount();
            plane.srcAccess = std::make_shared<Image::ReadAccess>(plane.src.get());
            if (plane.srcNComps == nComps) {
                plane.source = Resampler::SourceImage((const float*)plane.srcAccess->pixelAt(plane.srcBounds.x1, plane.srcBounds.y1), plane.srcBounds, nComps);
            } else {
                convertSource(*plane.srcAccess, plane.srcBounds, plane.srcNComps, nComps, &plane.converted);
                plane.source = Resampler::SourceImage(plane.converted.empty() ? (const float*)0 : &plane.converted[0], plane.srcBounds, nComps);
            }
            if (matrix) {
                const double srcPar = plane.src->getPixelAspectRatio();
                Resampler::buildSamplingTransforms(request.transform, args.time, request.settings.invert, blur, s.x, s.y, false, srcPar, dstPar, &plane.transforms);
                if (plane.inputTransform) {
                    const Mat3 inputTransform = TransformMath::fromEngineMatrix(*plane.inputTransform);
                    Resampler::concatenateInputTransform(inputTransform, &plane.transforms);
                    if (readsReference) {
                        // The source fetched is upstream of the concatenated transforms, but the
                        // reference must show this node's immediate input: resample it through
                        // the input transform alone. That input is black outside its own region,
                        // whatever the op's black-outside setting says.
                        plane.referenceTransforms.invTransforms.push_back(Mat3::identity());
                        Resampler::concatenateInputTransform(inputTransform, &plane.referenceTransforms);
                        plane.referenceParams = Resampler::makeResampleParams(plane.referenceTransforms, filter, request.settings.clamp, true);
                        plane.resampleReference = true;
                    }
                }
                plane.params = Resampler::makeResampleParams(plane.transforms, filter, request.settings.clamp, request.settings.blackOutside);
            }
        } else {
            plane.source = Resampler::SourceImage((const float*)0, RectI(), nComps);
        }
        plane.dstAccess = std::make_shared<Image::WriteAccess>(plane.dst.get());
        for (std::size_t b = 0; b < bandRects.size(); ++b) {
            bands.push_back(ResampleBand(p, bandRects[b].y1, bandRects[b].y2));
        }
    }

    RenderCancellation cancel(&effect);

    const std::function<void(int)> renderBand = [&](int bandIndex) {
        const ResampleBand& band = bands[bandIndex];
        const ResamplePlane& plane = *planes[band.plane];
        const int nComps = plane.nComps;
        const bool hasSource = plane.source.isValid();
        const std::size_t rowSize = (std::size_t)width * nComps;
        std::vector<float> resampledRow(rowSize);
        std::vector<float> referenceRow(rowSize);
        std::vector<float> maskRow(doMask ? width : 0);

        for (int y = band.y1; y < band.y2; ++y) {
            if ((((y - band.y1) % kAbortCheckRows) == 0) && cancel.check()) {
                return;
            }

            float* dstPix = (float*)plane.dstAccess->pixelAt(roi.x1, y);
            if (!dstPix) {
                continue;
            }
            if (!hasSource) {
                std::fill(resampledRow.begin(), resampledRow.end(), 0.f);
            } else if (matrix) {
                Resampler::resampleRow(plane.params, plane.source, y, roi.x1, roi.x2, &resampledRow[0]);
            } else {
                readSourceImageRow(plane.source, roi.x1 - offset.x, y - offset.y, width, &resampledRow[0]);
            }
            if (readsReference) {
                if (plane.resampleReference) {
                    Resampler::resampleRow(plane.referenceParams, plane.source, y, roi.x1, roi.x2, &referenceRow[0]);
                } else {
                    readSourceImageRow(plane.source, roi.x1, y, width, &referenceRow[0]);
                }
            }
            if (doMask) {
                PassExec::readChannelRow(mask.get(), maskSource.access.get(), maskSource.bounds, maskSource.nComps, maskChannel, roi.x1, y, width, 0.f, &maskRow[0]);
            }

            for (int i = 0; i < width; ++i, dstPix += nComps) {
                const float* srcPix = &referenceRow[(std::size_t)i * nComps];
                const float* outPix = &resampledRow[(std::size_t)i * nComps];
                float alpha = mix;
                if (doMask) {
                    const float maskScale = maskInvert ? (1.f - maskRow[i]) : maskRow[i];
                    alpha = maskScale * mix;
                }
                for (int c = 0; c < nComps; ++c) {
                    if ((c >= 4) || !plane.channels[pixelKernelChannelBit(nComps, c)]) {
                        if (passThrough) {
                            dstPix[c] = srcPix[c];
                        }
                        continue;
                    }
                    float v = outPix[c];
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
    parallelForCancellable((int)bands.size(), nThreads, cancel, renderBand);

    return eStatusOK;
} // runResampleCpu

StatusEnum
resampleRegionOfDefinition(EffectInstance& effect,
                           int sourceInput,
                           const ResampleRequest& request,
                           const ResampleRegionStages& stages,
                           double time,
                           const RenderScale& scale,
                           ViewIdx view,
                           RectD* rod)
{
    EffectInstancePtr input = (sourceInput >= 0) ? effect.getInput(sourceInput) : EffectInstancePtr();
    if (!input || (request.form == ResampleRequest::Form::None)) {
        return eStatusFailed;
    }
    RectD srcRoD;
    if (!sourceRegionOfDefinition(input, time, scale, view, &srcRoD)) {
        return eStatusFailed;
    }

    if (request.form == ResampleRequest::Form::Matrix) {
        const OfxPointD s = scale.toOfxPointD();
        Resampler::getRegionOfDefinition(request.transform, srcRoD, time, effect.getAspectRatio(-1), s.x, s.y, regionParams(request, stages), rod);

        return eStatusOK;
    }

    if (srcRoD.isNull()) {
        return eStatusReplyDefault;
    }
    if (stages.doMasking && (stages.mix == 0.)) {
        *rod = srcRoD;

        return eStatusOK;
    }
    double dx = 0.;
    double dy = 0.;
    offsetToCanonical(effect, request.offset, scale, &dx, &dy);
    rod->x1 = srcRoD.x1 + dx;
    rod->x2 = srcRoD.x2 + dx;
    rod->y1 = srcRoD.y1 + dy;
    rod->y2 = srcRoD.y2 + dy;
    if (stages.doMasking) {
        rod->merge(srcRoD);
    }

    return eStatusOK;
} // resampleRegionOfDefinition

void
resampleRegionsOfInterest(EffectInstance& effect,
                          int sourceInput,
                          const ResampleRequest& request,
                          const ResampleRegionStages& stages,
                          double time,
                          const RenderScale& scale,
                          const RectD& renderWindow,
                          ViewIdx view,
                          RoIMap* ret)
{
    EffectInstancePtr input = (sourceInput >= 0) ? effect.getInput(sourceInput) : EffectInstancePtr();
    if (!input || (request.form == ResampleRequest::Form::None)) {
        return;
    }
    RectD srcRoD;
    const bool hasSrcRoD = sourceRegionOfDefinition(input, time, scale, view, &srcRoD);
    const OfxPointD s = scale.toOfxPointD();
    const double srcPar = effect.getAspectRatio(sourceInput);
    const bool readsReference = stages.maskMix && (stages.doMasking || (stages.mix != 1.)) && !renderWindow.isNull();

    RectD srcRoI = renderWindow;
    if (request.form == ResampleRequest::Form::Matrix) {
        if (!hasSrcRoD) {
            srcRoD = RectD();
        }
        const Resampler::RegionParams params = regionParams(request, stages);
        Resampler::getRegionOfInterest(request.transform, renderWindow, srcRoD, projectRect(effect), time, srcPar, s.x, s.y, params, &srcRoI);
        // Under concatenation the reference row is resampled with the op's filter, and the engine
        // maps this region through the concatenated transform.
        if (readsReference) {
            RectD reference = renderWindow;
            Resampler::expandRoI(renderWindow, srcPar, s.x, s.y, params.filter, false, 1., &reference);
            if (srcRoI.isNull()) {
                srcRoI = reference;
            } else {
                srcRoI.merge(reference);
            }
        }
    } else {
        const PixelOffset& offset = request.offset;
        if (((offset.x != 0) || (offset.y != 0)) && hasSrcRoD && !srcRoD.isNull()) {
            double dx = 0.;
            double dy = 0.;
            offsetToCanonical(effect, offset, scale, &dx, &dy);
            const RectD shifted(renderWindow.x1 - dx, renderWindow.y1 - dy, renderWindow.x2 - dx, renderWindow.y2 - dy);
            srcRoI = shifted.intersect(srcRoD);
        }
        if (stages.doMasking && (stages.mix == 0.)) {
            srcRoI = renderWindow;
        } else if (readsReference) {
            if (srcRoI.isNull()) {
                srcRoI = renderWindow;
            } else {
                srcRoI.merge(renderWindow);
            }
        }
    }

    // A mask that is the source's own effect shares its entry, and is read under the output pixel.
    EffectInstancePtr mask = (stages.maskInput >= 0) ? effect.getInput(stages.maskInput) : EffectInstancePtr();
    if (mask && (mask == input)) {
        srcRoI.merge(renderWindow);
    }
    (*ret)[input] = srcRoI;
} // resampleRegionsOfInterest

StatusEnum
resampleTransform(EffectInstance& effect,
                  int sourceInput,
                  const ResampleRequest& request,
                  double time,
                  const RenderScale& renderScale,
                  bool draftRender,
                  EffectInstancePtr* inputToTransform,
                  Transform::Matrix3x3* transform)
{
    if ((request.form != ResampleRequest::Form::Matrix) || !request.transform) {
        return eStatusReplyDefault;
    }
    if (!draftRender) {
        const Resampler::BlurSettings& blur = request.settings.blur;
        const double shutter = blur.directionalBlur ? 0. : blur.shutter;
        if (((shutter != 0.) && (blur.motionBlur != 0.)) || blur.directionalBlur) {
            return eStatusReplyDefault;
        }
    }

    Mat3 invCanonical;
    if (!request.transform(time, 1., request.settings.invert, &invCanonical)) {
        return eStatusReplyDefault;
    }
    Mat3 canonical;
    if (!invCanonical.inverse(&canonical)) {
        return eStatusReplyDefault;
    }

    EffectInstancePtr input = (sourceInput >= 0) ? effect.getInput(sourceInput) : EffectInstancePtr();
    if (!input) {
        return eStatusFailed;
    }
    const OfxPointD s = renderScale.toOfxPointD();
    *transform = TransformMath::toEngineMatrix(TransformMath::forwardToPixel(canonical, effect.getAspectRatio(sourceInput), effect.getAspectRatio(-1), s.x, s.y));
    *inputToTransform = input;

    return eStatusOK;
} // resampleTransform

NATRON_NAMESPACE_EXIT
