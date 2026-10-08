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

#include "Crop.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include <ofxNatron.h>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/NativeGenerator.h"
#include "Engine/PoolParallelFor.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

namespace {
// How many rows render() runs between two abort checks.
const int kAbortCheckRows = 16;

bool
isEmptyRect(const RectD& r)
{
    return (r.x2 <= r.x1) || (r.y2 <= r.y1);
}

// The OpenFX support library's rectIntersection(): rectangles that merely touch still
// intersect, and an empty or disjoint pair gives an empty rectangle at the origin.
RectD
intersectRects(const RectD& a,
               const RectD& b)
{
    RectD empty(0., 0., 0., 0.);

    if (isEmptyRect(a) || isEmptyRect(b)) {
        return empty;
    }
    if ((a.x1 > b.x2) || (b.x1 > a.x2) || (a.y1 > b.y2) || (b.y1 > a.y2)) {
        return empty;
    }
    RectD ret;
    ret.x1 = std::max(a.x1, b.x1);
    ret.x2 = std::max(ret.x1, std::min(a.x2, b.x2));
    ret.y1 = std::max(a.y1, b.y1);
    ret.y2 = std::max(ret.y1, std::min(a.y2, b.y2));

    return ret;
}

RectI
toPixelEnclosing(const RectD& r,
                 double scale,
                 double par)
{
    if (isEmptyRect(r)) {
        return RectI(0, 0, 0, 0);
    }

    return RectI((int)std::floor(r.x1 * scale / par),
                 (int)std::floor(r.y1 * scale),
                 (int)std::ceil(r.x2 * scale / par),
                 (int)std::ceil(r.y2 * scale));
}

RectI
toPixelNearest(const RectD& r,
               double scale,
               double par)
{
    if (isEmptyRect(r)) {
        return RectI(0, 0, 0, 0);
    }

    return RectI((int)std::floor(r.x1 * scale / par + 0.5),
                 (int)std::floor(r.y1 * scale + 0.5),
                 (int)std::ceil(r.x2 * scale / par - 0.5),
                 (int)std::ceil(r.y2 * scale - 0.5));
}

RectD
toCanonical(const RectI& r,
            double scale,
            double par)
{
    if ((r.x2 <= r.x1) || (r.y2 <= r.y1)) {
        return RectD(0., 0., 0., 0.);
    }

    return RectD(r.x1 * par / scale, r.y1 / scale, r.x2 * par / scale, r.y2 / scale);
}

double
rampSmooth(double t)
{
    t *= 2.;
    if (t < 1) {
        return t * t / 2.;
    }
    t -= 1.;

    return -0.5 * (t * (t - 2) - 1);
}

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

// Copies `width` pixels of row y starting at x0 into row (nComps floats per pixel). Pixels outside
// `bounds` repeat the nearest edge pixel, as OFX Image::getPixelAddressNearest() does in the OFX
// Crop, so a rectangle larger than the source extends its edges. An image in another layout is
// mapped channel by colour bit, a channel it lacks reading as zero.
void
readSourceRow(const Image* image,
              const Image::ReadAccess* access,
              const RectI& bounds,
              int x0,
              int y,
              int width,
              int nComps,
              float* row)
{
    std::fill(row, row + (std::size_t)width * nComps, 0.f);
    if (!image || (bounds.x2 <= bounds.x1) || (bounds.y2 <= bounds.y1)) {
        return;
    }
    const int sy = std::max(bounds.y1, std::min(y, bounds.y2 - 1));
    const int srcNComps = (int)image->getComponentsCount();
    int srcIndex[4] = { -1, -1, -1, -1 };
    for (int c = 0; (c < nComps) && (c < 4); ++c) {
        srcIndex[c] = (srcNComps == nComps) ? c : channelIndexForBit(srcNComps, pixelKernelChannelBit(nComps, c));
    }
    const float* rowStart = (const float*)access->pixelAt(bounds.x1, sy);
    if (!rowStart) {
        return;
    }
    float* dstPix = row;
    for (int x = x0; x < x0 + width; ++x, dstPix += nComps) {
        const int sx = std::max(bounds.x1, std::min(x, bounds.x2 - 1));
        const float* srcPix = rowStart + (std::size_t)(sx - bounds.x1) * srcNComps;
        for (int c = 0; (c < nComps) && (c < 4); ++c) {
            if (srcIndex[c] >= 0) {
                dstPix[c] = srcPix[srcIndex[c]];
            }
        }
    }
}

struct CropPlaneJob {
    ImagePtr dst;
    ImagePtr src;
    std::shared_ptr<Image::ReadAccess> srcAccess;
    std::shared_ptr<Image::WriteAccess> dstAccess;
    // Image::getBounds() takes the image's lock, which a band thread must never ask for: behind
    // a writer waiting on an image this render holds for reading, it blocks forever. The bounds
    // are read once on the calling thread instead.
    RectI srcBounds;
    // The OFX host shows a plug-in only the part of an input image inside the RoI it asked for.
    RectI srcRoI;
};

struct CropBand {
    std::size_t job;
    int y1;
    int y2;

    CropBand(std::size_t jobIndex,
             int firstRow,
             int endRow)
        : job(jobIndex)
        , y1(firstRow)
        , y2(endRow)
    {
    }
};

NativeImageTraits
cropTraits()
{
    NativeImageTraits traits;

    traits.processesAllLayers = true;

    return traits;
}

ExtentPolicy
cropExtentPolicy()
{
    ExtentPolicy policy;

    policy.defaultExtent = ExtentKnobs::eExtentSize;
    policy.reformatToggle = false;
    policy.overlayFollowsExtent = false;
    policy.recenterOnSource = true;

    return policy;
}
} // anonymous namespace

Crop::Crop(NodePtr node)
    : NativeImageEffect(node, cropTraits())
    , _extentKnobs(this, cropExtentPolicy())
{
}

Crop::~Crop()
{
}

void
Crop::addAcceptedComponents(int inputNb,
                            std::list<ImageLayerDesc>* comps)
{
    NativeImageEffect::addAcceptedComponents(inputNb, comps);
    comps->push_back(ImageLayerDesc::getXYComponents());
}

NativePluginDescription
Crop::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_CROP;
    desc.label = "Crop";
    desc.description = tr("Removes everything outside the defined rectangle and optionally adds black edges so everything outside is black.\n"
                          "If the 'Extent' parameter is set to 'Format', and 'Reformat' is checked, the output pixel aspect ratio is also set to this of the format.\n"
                          "This plugin does not concatenate transforms.")
                           .toStdString();
    desc.grouping = PLUGIN_GROUP_TRANSFORM;
    desc.majorVersion = PLUGIN_MAJOR_NATRON_CROP;
    desc.minorVersion = 0;
    desc.inputs.push_back(NativeInputDescription(kNativeGeneratorSourceInputLabel, false, eDataKindImage));
    desc.outputKind = eDataKindImage;

    return desc;
}

void
Crop::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    // The frame range knob is kept for parity only: the node has no time domain of its own.
    _extentKnobs.createKnobs(page);

    KnobDoublePtr softness = createKnob<KnobDouble>(tr("Softness"));
    softness->setName(kCropParamSoftness);
    softness->setHintToolTip(tr("Size of the fade to black around edges to apply."));
    softness->setDefaultValue(0.);
    softness->setMinimum(0.);
    softness->setMaximum(1000.);
    softness->setDisplayMinimum(0.);
    softness->setDisplayMaximum(100.);
    softness->setIncrement(1.);
    page->addKnob(softness);
    _softness = softness;

    KnobBoolPtr reformat = createKnob<KnobBool>(tr("Reformat"));
    reformat->setName(kCropParamReformat);
    reformat->setHintToolTip(tr("Translates the bottom left corner of the crop rectangle to be in (0,0)."
                                " This sets the output format only if 'Format' or 'Project' is selected as the output Extend. "
                                "In order to actually change the format of this image stream for other Extent choices, feed the output of this node to a either a NoOp node which sets the proper format, "
                                "or a Reformat node with the same extent and with 'Resize Type' set to None and 'Center' unchecked. "
                                "The reason is that the Crop size may be animated, but the output format can not be animated."));
    reformat->setDefaultValue(false);
    reformat->setAnimationEnabled(false);
    reformat->setAddNewLine(false);
    reformat->setSpacingBetweenItems(1);
    reformat->setIsMetadataSlave(true);
    page->addKnob(reformat);
    _reformat = reformat;

    KnobBoolPtr intersect = createKnob<KnobBool>(tr("Intersect"));
    intersect->setName(kCropParamIntersect);
    intersect->setHintToolTip(tr("Intersects the crop rectangle with the input region of definition instead of extending it."));
    intersect->setDefaultValue(false);
    intersect->setAddNewLine(false);
    intersect->setSpacingBetweenItems(1);
    page->addKnob(intersect);
    _intersect = intersect;

    KnobBoolPtr blackOutside = createKnob<KnobBool>(tr("Black Outside"));
    blackOutside->setName(kCropParamBlackOutside);
    blackOutside->setHintToolTip(tr("Add 1 black and transparent pixel to the region of definition so that all the area outside the crop rectangle is black."));
    blackOutside->setDefaultValue(false);
    page->addKnob(blackOutside);
    _blackOutside = blackOutside;

    _extentKnobs.finishKnobs();
} // Crop::initializeKnobs

Crop::ExtentEnum
Crop::getExtent() const
{
    return _extentKnobs.getExtent();
}

void
Crop::updateRectangleEnable()
{
    KnobBoolPtr reformat = _reformat.lock();
    KnobBoolPtr rectangleEnable = _extentKnobs.getRectangleEnableKnob();

    if (!reformat || !rectangleEnable) {
        return;
    }
    const bool enable = !reformat->getValue();
    if (rectangleEnable->getValue() != enable) {
        rectangleEnable->setValue(enable);
    }
}

void
Crop::onKnobsLoaded()
{
    _extentKnobs.updateVisibility();
    updateRectangleEnable();
}

bool
Crop::knobChanged(KnobI* k,
                  ValueChangedReasonEnum reason,
                  ViewSpec /*view*/,
                  double time,
                  bool /*originatedFromMainThread*/)
{
    if (_extentKnobs.onKnobChanged(k, reason, time)) {
        return true;
    }
    KnobBoolPtr reformat = _reformat.lock();
    if (reformat && (k == reformat.get())) {
        updateRectangleEnable();
        if (reason == eValueChangedReasonUserEdited) {
            KnobBoolPtr blackOutside = _blackOutside.lock();
            if (blackOutside) {
                blackOutside->setValue(!reformat->getValue());
            }
        }

        return true;
    }

    return false;
}

void
Crop::getCropRectangle(double time,
                       ViewIdx view,
                       const RenderScale& scale,
                       bool useIntersect,
                       bool forceIntersect,
                       bool useBlackOutside,
                       bool useReformat,
                       RectD* cropRect,
                       double* par) const
{
    KnobBoolPtr intersectKnob = _intersect.lock();
    KnobBoolPtr blackOutsideKnob = _blackOutside.lock();
    KnobBoolPtr reformatKnob = _reformat.lock();

    bool intersect = false;
    if (useIntersect) {
        intersect = forceIntersect || (intersectKnob && intersectKnob->getValueAtTime(time, 0, view));
    }
    const bool blackOutside = useBlackOutside && blackOutsideKnob && blackOutsideKnob->getValueAtTime(time, 0, view);
    const bool reformat = useReformat && reformatKnob && reformatKnob->getValueAtTime(time, 0, view);

    EffectInstancePtr source = getInput(0);
    RectD sourceRoD;
    bool hasSourceRoD = false;
    if (source) {
        const RenderScale sourceScale = source->supportsRenderScale() ? scale : RenderScale::identity;
        bool isProjectFormat = false;
        hasSourceRoD = (source->getRegionOfDefinition_public(source->getRenderHash(), time, sourceScale, view, &sourceRoD, &isProjectFormat) != eStatusFailed);
    }

    RectD rod;
    double rodPar = 1.;
    switch (getExtent()) {
    case eExtentFormat: {
        KnobIntPtr formatSize = _extentKnobs.getFormatSizeKnob();
        KnobDoublePtr formatPar = _extentKnobs.getFormatParKnob();
        const int w = formatSize ? formatSize->getValue(0) : 0;
        const int h = formatSize ? formatSize->getValue(1) : 0;
        rodPar = formatPar ? formatPar->getValue() : 1.;
        rod = toCanonical(RectI(0, 0, w, h), 1., rodPar);
        break;
    }
    case eExtentSize: {
        KnobDoublePtr size = _extentKnobs.getSizeKnob();
        KnobDoublePtr bottomLeft = _extentKnobs.getBottomLeftKnob();
        if (source) {
            rodPar = getAspectRatio(0);
        } else {
            _extentKnobs.getProjectExtentRect(&rodPar);
        }
        if (size && bottomLeft) {
            rod.x1 = bottomLeft->getValueAtTime(time, 0, view);
            rod.y1 = bottomLeft->getValueAtTime(time, 1, view);
            rod.x2 = size->getValueAtTime(time, 0, view) + rod.x1;
            rod.y2 = size->getValueAtTime(time, 1, view) + rod.y1;
        }
        break;
    }
    case eExtentProject:
        rod = _extentKnobs.getProjectExtentRect(&rodPar);
        break;
    case eExtentDefault:
        if (source && hasSourceRoD) {
            rod = sourceRoD;
            rodPar = getAspectRatio(0);
        } else {
            rod = _extentKnobs.getProjectExtentRect(&rodPar);
        }
        break;
    }

    if (reformat) {
        rod.x2 -= rod.x1;
        rod.y2 -= rod.y1;
        rod.x1 = 0.;
        rod.y1 = 0.;
    }
    if (intersect && hasSourceRoD) {
        rod = intersectRects(rod, sourceRoD);
    }
    if (blackOutside) {
        const double s = scale.toOfxPointD().x;
        RectI rodPixel = toPixelEnclosing(rod, s, rodPar);
        rodPixel.x1 -= 1;
        rodPixel.y1 -= 1;
        rodPixel.x2 += 1;
        rodPixel.y2 += 1;
        rod = toCanonical(rodPixel, s, rodPar);
    }

    if (cropRect) {
        *cropRect = rod;
    }
    if (par) {
        *par = rodPar;
    }
} // Crop::getCropRectangle

StatusEnum
Crop::getRegionOfDefinition(U64 /*hash*/,
                            double time,
                            const RenderScale& scale,
                            ViewIdx view,
                            RectD* rod)
{
    getCropRectangle(time, view, scale, true, false, true, true, rod, NULL);

    return eStatusOK;
}

void
Crop::getRegionsOfInterest(double time,
                           const RenderScale& scale,
                           const RectD& /*outputRoD*/,
                           const RectD& renderWindow,
                           ViewIdx view,
                           RoIMap* ret)
{
    EffectInstancePtr source = getInput(0);

    if (!source) {
        return;
    }
    RectD cropRect;
    getCropRectangle(time, view, scale, true, true, false, false, &cropRect, NULL);

    RectD roi = renderWindow;
    KnobBoolPtr reformat = _reformat.lock();
    if (reformat && reformat->getValueAtTime(time, 0, view)) {
        // The crop rectangle is rendered at the origin, so the window maps back by its offset.
        roi.x1 += cropRect.x1;
        roi.y1 += cropRect.y1;
        roi.x2 += cropRect.x1;
        roi.y2 += cropRect.y1;
    }
    ret->insert(std::make_pair(source, intersectRects(cropRect, roi)));
}

StatusEnum
Crop::getPreferredMetadata(NodeMetadata& metadata)
{
    KnobBoolPtr reformat = _reformat.lock();

    if (!reformat || !reformat->getValue()) {
        return eStatusOK;
    }

    RectI pixelFormat(0, 0, 0, 0);
    double par = 0.;
    const ExtentEnum extent = getExtent();
    if (extent == eExtentFormat) {
        KnobIntPtr formatSize = _extentKnobs.getFormatSizeKnob();
        KnobDoublePtr formatPar = _extentKnobs.getFormatParKnob();
        if (formatSize && formatPar) {
            par = formatPar->getValue();
            pixelFormat = RectI(0, 0, formatSize->getValue(0), formatSize->getValue(1));
        }
    } else if (extent == eExtentProject) {
        const RectD project = _extentKnobs.getProjectExtentRect(&par);
        pixelFormat = toPixelNearest(project, 1., par);
    }
    if (par != 0.) {
        metadata.setPixelAspectRatio(-1, par);
    }
    if ((pixelFormat.x2 > pixelFormat.x1) && (pixelFormat.y2 > pixelFormat.y1)) {
        metadata.setOutputFormat(pixelFormat);
    }

    return eStatusOK;
}

StatusEnum
Crop::render(const RenderActionArgs& args)
{
    KnobBoolPtr reformatKnob = _reformat.lock();
    KnobBoolPtr blackOutsideKnob = _blackOutside.lock();
    KnobDoublePtr softnessKnob = _softness.lock();
    if (!reformatKnob || !blackOutsideKnob || !softnessKnob) {
        return eStatusFailed;
    }

    const bool reformat = reformatKnob->getValueAtTime(args.time, 0, args.view);
    const bool blackOutside = blackOutsideKnob->getValueAtTime(args.time, 0, args.view);
    // Softness is in canonical units, so the render scale does not apply to it.
    const double softness = softnessKnob->getValueAtTime(args.time, 0, args.view);
    const double scale = args.mappedScale.toOfxPointD().x;
    const double dstPar = getAspectRatio(-1);

    RectD cropFull;
    double par = 1.;
    getCropRectangle(args.time, args.view, args.mappedScale, false, false, false, false, &cropFull, &par);
    const RectI fullPixel = toPixelNearest(cropFull, scale, par);
    const int tx = reformat ? -fullPixel.x1 : 0;
    const int ty = reformat ? -fullPixel.y1 : 0;

    // Every source image is fetched before any is locked: fetching renders upstream, which may
    // write into a cached image this render would otherwise already hold a read lock on.
    std::vector<CropPlaneJob> jobs;
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        CropPlaneJob job;
        job.dst = it->second;
        if (job.dst) {
            if (job.dst->getBitDepth() != eImageBitDepthFloat) {
                return eStatusFailed;
            }
            ImageLayerDesc sourceLayer;
            if (getInput(0) && resolveInputPlaneForRender(0, args.time, args.view, &sourceLayer, NULL)) {
                // Mapped to the clip's components, which is the layout the output plane is rendered in.
                job.src = getImage(0, args.time, args.mappedScale, args.view, NULL, &sourceLayer, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &job.srcRoI);
                if (job.src && (job.src->getBitDepth() != eImageBitDepthFloat)) {
                    return eStatusFailed;
                }
            }
        }
        jobs.push_back(job);
    }

    const RectI& roi = args.roi;
    const int width = roi.width();
    if ((width <= 0) || (roi.height() <= 0)) {
        return eStatusOK;
    }

    // Images are locked here, on the calling thread, for the whole render; the band threads only
    // compute pixel addresses through these accesses.
    const int nThreads = appPTR->getNCPUsAvailableForEffect();
    std::vector<RectI> bandRects;
    makeRowBands(roi, nThreads, &bandRects);
    std::vector<CropBand> bands;
    for (std::size_t j = 0; j < jobs.size(); ++j) {
        CropPlaneJob& job = jobs[j];
        if (!job.dst) {
            continue;
        }
        if (job.src) {
            job.srcBounds = job.src->getBounds().intersect(job.srcRoI);
            job.srcAccess = std::make_shared<Image::ReadAccess>(job.src.get());
        }
        job.dstAccess = std::make_shared<Image::WriteAccess>(job.dst.get());
        for (std::size_t b = 0; b < bandRects.size(); ++b) {
            bands.push_back(CropBand(j, bandRects[b].y1, bandRects[b].y2));
        }
    }

    RenderCancellation cancel(this);

    const std::function<void(int)> renderBand = [&](int bandIndex) {
        const CropBand& band = bands[bandIndex];
        const CropPlaneJob& job = jobs[band.job];
        const int nComps = (int)job.dst->getComponentsCount();
        std::vector<float> srcRow(job.src ? (std::size_t)width * nComps : 0);

        for (int y = band.y1; y < band.y2; ++y) {
            if ((((y - band.y1) % kAbortCheckRows) == 0) && cancel.check()) {
                return;
            }

            float* dstPix = (float*)job.dstAccess->pixelAt(roi.x1, y);
            if (!dstPix) {
                continue;
            }
            const bool yBlack = blackOutside && ((y == (fullPixel.y1 + ty)) || (y == (fullPixel.y2 - 1 + ty)));
            if (yBlack || !job.src) {
                std::fill(dstPix, dstPix + (std::size_t)width * nComps, 0.f);
                continue;
            }

            const int srcY = y - ty;
            readSourceRow(job.src.get(), job.srcAccess.get(), job.srcBounds, roi.x1 - tx, srcY, width, nComps, &srcRow[0]);
            const double canonicalY = (srcY + 0.5) / scale;
            const double dy = std::min(canonicalY - cropFull.y1, cropFull.y2 - canonicalY);

            for (int i = 0; i < width; ++i, dstPix += nComps) {
                const int x = roi.x1 + i;
                const bool xBlack = blackOutside && ((x == (fullPixel.x1 + tx)) || (x == (fullPixel.x2 - 1 + tx)));
                if (xBlack) {
                    std::fill(dstPix, dstPix + nComps, 0.f);
                    continue;
                }
                const double canonicalX = ((x - tx) + 0.5) * dstPar / scale;
                const double dx = std::min(canonicalX - cropFull.x1, cropFull.x2 - canonicalX);
                if (blackOutside && ((dx <= 0) || (dy <= 0))) {
                    std::fill(dstPix, dstPix + nComps, 0.f);
                    continue;
                }
                const float* srcPix = &srcRow[(std::size_t)i * nComps];
                if ((softness == 0) || ((dx >= softness) && (dy >= softness))) {
                    std::copy(srcPix, srcPix + nComps, dstPix);
                    continue;
                }
                const double tx2 = (dx >= softness) ? 1. : rampSmooth(dx / softness);
                const double ty2 = (dy >= softness) ? 1. : rampSmooth(dy / softness);
                const double t = tx2 * ty2;
                if (t >= 1) {
                    std::copy(srcPix, srcPix + nComps, dstPix);
                } else {
                    for (int c = 0; c < nComps; ++c) {
                        dstPix[c] = (float)(srcPix[c] * t);
                    }
                }
            }
        }
    };
    parallelForCancellable((int)bands.size(), nThreads, cancel, renderBand);

    return eStatusOK;
} // Crop::render

NATRON_NAMESPACE_EXIT
