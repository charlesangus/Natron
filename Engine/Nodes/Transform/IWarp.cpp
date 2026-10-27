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

#include "IWarp.h"

#include <algorithm>
#include <bitset>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/PassExecutor.h"
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/Nodes/Image/StagePlan.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/Nodes/Transform/WarpGpu.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

using TransformMath::Mat3;
using TransformMath::Point3;

namespace {
// The fewest segments of the matrix curve of one shutter time.
const int kMinTimeCurveSegments = 32;

// The words of iwarp.slang's parameter block before the matrix curve.
const std::size_t kGpuHeaderWords = 12;

const uint32_t kIWarpGpuClamp = 1;
const uint32_t kIWarpGpuBlackOutside = 2;
const uint32_t kIWarpGpuJacobian = 4;
const uint32_t kIWarpGpuSample = 8;
const uint32_t kIWarpGpuMapConnected = 16;
const uint32_t kIWarpGpuMapRead = 32;
const uint32_t kIWarpGpuMapInvert = 64;
const uint32_t kIWarpGpuMapSmooth = 128;

bool
rectIsEmpty(const RectD& r)
{
    return (r.x2 <= r.x1) || (r.y2 <= r.y1);
}

bool
rectIsInfinite(const RectD& r)
{
    return (r.x1 <= kOfxFlagInfiniteMin) || (r.x2 >= kOfxFlagInfiniteMax) || (r.y1 <= kOfxFlagInfiniteMin) || (r.y2 >= kOfxFlagInfiniteMax);
}

RectD
infiniteRect()
{
    return RectD(kOfxFlagInfiniteMin, kOfxFlagInfiniteMin, kOfxFlagInfiniteMax, kOfxFlagInfiniteMax);
}

// The bounding box of a and b; an empty operand gives the other, as in the OFX Transform3x3.
RectD
boundingBox(const RectD& a,
            const RectD& b)
{
    if (rectIsEmpty(a)) {
        return b;
    }
    if (rectIsEmpty(b)) {
        return a;
    }

    return RectD((std::min)(a.x1, b.x1), (std::min)(a.y1, b.y1), (std::max)(a.x2, b.x2), (std::max)(a.y2, b.y2));
}

// The overlap of a and b; all zeros when there is none, as in the OFX Transform3x3.
RectD
intersection(const RectD& a,
             const RectD& b)
{
    if (rectIsEmpty(a) || rectIsEmpty(b) || (a.x1 > b.x2) || (b.x1 > a.x2) || (a.y1 > b.y2) || (b.y1 > a.y2)) {
        return RectD(0., 0., 0., 0.);
    }
    const double x1 = (std::max)(a.x1, b.x1);
    const double y1 = (std::max)(a.y1, b.y1);

    return RectD(x1, y1, (std::max)(x1, (std::min)(a.x2, b.x2)), (std::max)(y1, (std::min)(a.y2, b.y2)));
}

RectD
projectRect(const EffectInstance& effect)
{
    Format format;

    effect.getApp()->getProject()->getProjectDefaultFormat(&format);
    const RectI pixel(format.x1, format.y1, format.x2, format.y2);

    return pixel.toCanonical_noClipping(0, format.getPixelAspectRatio());
}

// The corners of rect through matrix, with whether each lies in front of infinity.
struct Corners {
    double x[4];
    double y[4];
    bool valid[4];
};

Corners
projectCorners(const RectD& rect,
               const Mat3& matrix)
{
    Corners corners;
    const double xs[4] = { rect.x1, rect.x1, rect.x2, rect.x2 };
    const double ys[4] = { rect.y1, rect.y2, rect.y2, rect.y1 };

    for (int k = 0; k < 4; ++k) {
        const Point3 p = matrix * Point3(xs[k], ys[k], 1.);
        corners.valid[k] = p.z > 0.;
        corners.x[k] = corners.valid[k] ? p.x / p.z : 0.;
        corners.y[k] = corners.valid[k] ? p.y / p.z : 0.;
    }

    return corners;
}

// The largest move of a corner valid in both a and b.
double
cornerStep(const Corners& a,
           const Corners& b)
{
    double step = 0.;

    for (int k = 0; k < 4; ++k) {
        if (a.valid[k] && b.valid[k]) {
            step = (std::max)(step, (std::max)(std::fabs(a.x[k] - b.x[k]), std::fabs(a.y[k] - b.y[k])));
        }
    }

    return step;
}

// The samples a variance-to-error quotient asks for, as Resampler's motion blur counts them.
int
samplesForVariance(double q)
{
    return (q >= 0. && q < 2147483648.) ? (int)q : 0;
}

TransformInterp::MatrixFn
toPixelFn(const TransformInterp::MatrixFn& canonical,
          double srcPar,
          double dstPar,
          double sx,
          double sy)
{
    return [canonical, srcPar, dstPar, sx, sy](double m, Mat3* matrix) {
        Mat3 c;
        if (!canonical(m, &c)) {
            return false;
        }
        *matrix = TransformMath::inverseToPixel(c, srcPar, dstPar, sx, sy);

        return true;
    };
}

Mat3
scaled(const Mat3& a,
       double s)
{
    Mat3 out;
    for (int k = 0; k < 9; ++k) {
        out.m[k] = a.m[k] * s;
    }

    return out;
}
} // anonymous namespace

IWarp::IWarp(NodePtr node)
    : StagedImageEffect(node)
{
}

IWarp::~IWarp()
{
}

std::string
IWarp::getInputHint(int inputNb) const
{
    if (inputNb == kIWarpInputSource) {
        return tr("The image to transform.").toStdString();
    }
    if (inputNb == kIWarpInputMap) {
        return tr("How much of the transform each output pixel gets: none where the Map is black, all of it where it is white. "
                  "Read at the output pixel. With no Map connected, the whole transform applies everywhere.")
            .toStdString();
    }

    return EffectInstance::getInputHint(inputNb);
}

void
IWarp::addAcceptedComponents(int inputNb,
                             std::list<ImageLayerDesc>* comps)
{
    StagedImageEffect::addAcceptedComponents(inputNb, comps);
    if (!isInputMask(inputNb)) {
        comps->push_back(ImageLayerDesc::getXYComponents());
    }
}

void
IWarp::createMapKnobs(const KnobPagePtr& page)
{
    _map.createKnobs(this, page, kMapWarpKnobHints);
}

void
IWarp::declareMapLayerKnob()
{
    _map.declareLayerKnob(getNode(), kIWarpInputMap);
}

bool
IWarp::hasTimeBlur(const WarpSettings& settings)
{
    return (settings.blur.motionBlur != 0.) && (settings.blur.shutter != 0.) && !settings.blur.directionalBlur;
}

bool
IWarp::hasDirectionalBlur(const WarpSettings& settings)
{
    return settings.blur.directionalBlur;
}

TransformInterp::MatrixFn
IWarp::pixelMatrixFn(double time,
                     ViewIdx view,
                     bool invert,
                     const RenderScale& scale) const
{
    const OfxPointD s = scale.toOfxPointD();
    const NodeMetadataConstPtr metadata = getRenderMetadata(time, view);

    return toPixelFn(canonicalMatrixFn(time, view, invert), metadata->getPixelAspectRatio(kIWarpInputSource), metadata->getPixelAspectRatio(-1), s.x, s.y);
}

void
IWarp::warpRegion(const RectD& from,
                  double time,
                  ViewIdx view,
                  bool forward,
                  RectD* to) const
{
    WarpSettings settings;
    readWarpSettings(time, view, &settings);
    const bool invert = forward ? !settings.invert : settings.invert;
    const bool timeBlur = hasTimeBlur(settings);

    if (!timeBlur && isTransformIdentity(time, view)) {
        *to = from;

        return;
    }

    // Every Map value with a Map connected or a directional blur (whose amounts span [0, 1] under
    // any Map), else only the full transform.
    const bool spread = (bool)getInput(kIWarpInputMap) || hasDirectionalBlur(settings);

    // The shutter at every quarter frame, as Resampler::transformRegion() samples it.
    std::vector<double> times;
    if (timeBlur) {
        double tMin = time;
        double tMax = time;
        Resampler::shutterRange(time, settings.blur.shutter, settings.blur.shutterOffset, settings.blur.shutterCustomOffset, &tMin, &tMax);
        double t = tMin;
        for (;;) {
            times.push_back(t);
            if (t >= tMax) {
                break;
            }
            t = std::floor(t * 4 + 1) / 4;
            if (t >= tMax) {
                t = tMax;
            }
        }
    } else {
        times.push_back(time);
    }

    // The segments of the render's curves: renderOp()'s at time, and its shutter times' under a
    // time blur.
    const int segments = curveSegments(time, view);
    const int timeSegments = timeBlur ? (std::max)(kMinTimeCurveSegments, segments / 4) : segments;

    RectD box(0., 0., 0., 0.);
    // Everything the corners of from reach over every Map value along the curve of fn, which is
    // the render's matrix curve in canonical coordinates; with knots, the corners at each knot.
    const auto addCurve = [&](const TransformInterp::MatrixFn& fn,
                              int curveSegmentCount,
                              std::vector<Corners>* knots) {
        TransformInterp::MatrixCurve curve;
        if (!fn || !curve.build(fn, curveSegmentCount)) {
            return false;
        }
        const double xs[4] = { from.x1, from.x1, from.x2, from.x2 };
        const double ys[4] = { from.y1, from.y2, from.y2, from.y1 };
        RectD region;
        for (int k = 0; k < 4; ++k) {
            RectD reach;
            if (!curve.pointRange(xs[k], ys[k], &reach.x1, &reach.y1, &reach.x2, &reach.y2)) {
                return false;
            }
            if (k == 0) {
                region = reach;
            } else {
                region.x1 = (std::min)(region.x1, reach.x1);
                region.y1 = (std::min)(region.y1, reach.y1);
                region.x2 = (std::max)(region.x2, reach.x2);
                region.y2 = (std::max)(region.y2, reach.y2);
            }
        }
        box = boundingBox(box, region);
        if (knots) {
            knots->resize((std::size_t)curve.segments() + 1);
            for (int k = 0; k <= curve.segments(); ++k) {
                (*knots)[k] = projectCorners(from, curve.knot(k));
            }
        }

        return true;
    };

    double expand = 0.;
    std::vector<Corners> previousTime;
    for (std::size_t ti = 0; ti < times.size(); ++ti) {
        const TransformInterp::MatrixFn fn = canonicalMatrixFn(times[ti], view, invert);
        std::vector<Corners> corners;
        if (spread) {
            if (!addCurve(fn, timeSegments, &corners)) {
                *to = infiniteRect();

                return;
            }
        } else {
            Mat3 matrix;
            if (!fn || !fn(1., &matrix)) {
                *to = infiniteRect();

                return;
            }
            RectD region;
            Resampler::transformRegionFromRect(from, matrix, &region);
            box = boundingBox(box, region);
            corners.push_back(projectCorners(from, matrix));
        }
        if (previousTime.size() == corners.size()) {
            for (std::size_t a = 0; a < corners.size(); ++a) {
                expand = (std::max)(expand, cornerStep(previousTime[a], corners[a]));
            }
        }
        previousTime.swap(corners);
    }
    // A time blur whose shutter times share one transform renders the curve at time.
    if (spread && timeBlur && !addCurve(canonicalMatrixFn(time, view, invert), segments, NULL)) {
        *to = infiniteRect();

        return;
    }

    // The corners move between the shutter samples: grow by the largest step between them.
    if (box.x1 > kOfxFlagInfiniteMin) {
        box.x1 -= expand;
    }
    if (box.x2 < kOfxFlagInfiniteMax) {
        box.x2 += expand;
    }
    if (box.y1 > kOfxFlagInfiniteMin) {
        box.y1 -= expand;
    }
    if (box.y2 < kOfxFlagInfiniteMax) {
        box.y2 += expand;
    }
    *to = box;
} // IWarp::warpRegion

StatusEnum
IWarp::getRegionOfDefinition(U64 hash,
                             double time,
                             const RenderScale& scale,
                             ViewIdx view,
                             RectD* rod)
{
    EffectInstancePtr input = getInput(kIWarpInputSource);
    if (!input) {
        return StagedImageEffect::getRegionOfDefinition(hash, time, scale, view, rod);
    }
    const RenderScale inputScale = input->supportsRenderScale() ? scale : RenderScale::identity;
    RectD srcRoD;
    bool isProjectFormat = false;
    if (input->getRegionOfDefinition_public(input->getRenderHash(), time, inputScale, view, &srcRoD, &isProjectFormat) == eStatusFailed) {
        return eStatusFailed;
    }

    if (rectIsInfinite(srcRoD)) {
        *rod = infiniteRect();

        return eStatusOK;
    }
    if (rectIsEmpty(srcRoD)) {
        *rod = RectD(0., 0., 0., 0.);

        return eStatusOK;
    }
    const bool doMasking = isMaskApplied();
    const double mix = getMixValue(time, view);
    if (doMasking && (mix == 0.)) {
        *rod = srcRoD;

        return eStatusOK;
    }

    WarpSettings settings;
    readWarpSettings(time, view, &settings);
    warpRegion(srcRoD, time, view, true, rod);
    if (hasTimeBlur(settings) || !isTransformIdentity(time, view)) {
        const OfxPointD s = scale.toOfxPointD();
        Resampler::expandRoD(getRenderMetadata(time, view)->getPixelAspectRatio(-1), s.x, s.y, settings.blackOutside, rod);
    }
    if (doMasking || (mix != 1.)) {
        *rod = boundingBox(*rod, srcRoD);
    }

    return eStatusOK;
} // IWarp::getRegionOfDefinition

void
IWarp::getRegionsOfInterest(double time,
                            const RenderScale& scale,
                            const RectD& outputRoD,
                            const RectD& renderWindow,
                            ViewIdx view,
                            RoIMap* ret)
{
    StagedImageEffect::getRegionsOfInterest(time, scale, outputRoD, renderWindow, view, ret);

    EffectInstancePtr source = getInput(kIWarpInputSource);
    if (source) {
        const RenderScale inputScale = source->supportsRenderScale() ? scale : RenderScale::identity;
        RectD srcRoD;
        bool isProjectFormat = false;
        if (source->getRegionOfDefinition_public(source->getRenderHash(), time, inputScale, view, &srcRoD, &isProjectFormat) == eStatusFailed) {
            srcRoD = RectD();
        }
        const bool doMasking = isMaskApplied();
        const double mix = getMixValue(time, view);
        RectD srcRoI = renderWindow;
        if (!(doMasking && (mix == 0.))) {
            WarpSettings settings;
            readWarpSettings(time, view, &settings);
            const OfxPointD s = scale.toOfxPointD();
            warpRegion(renderWindow, time, view, false, &srcRoI);
            Resampler::expandRoI(renderWindow, getRenderMetadata(time, view)->getPixelAspectRatio(kIWarpInputSource), s.x, s.y, settings.filter, doMasking, mix, &srcRoI);
            if (rectIsInfinite(srcRoI)) {
                // An RoI cannot be infinite: fall back to what the source and the project can show.
                srcRoI = boundingBox(intersection(srcRoI, srcRoD), projectRect(*this));
            }
        }
        // A mask that is the source's own effect shares its entry, and is read under the output pixel.
        EffectInstancePtr mask = getInput(kIWarpInputMask);
        if (mask && (mask == source)) {
            srcRoI.merge(renderWindow);
        }
        (*ret)[source] = srcRoI;
    }

    // The Map's gradient reads the neighbouring pixels.
    EffectInstancePtr map = getInput(kIWarpInputMap);
    if (map) {
        const double par = getRenderMetadata(time, view)->getPixelAspectRatio(-1);
        RectI pixels = PassExec::toPixelEnclosing(renderWindow, scale, par);
        pixels.x1 -= 1;
        pixels.y1 -= 1;
        pixels.x2 += 1;
        pixels.y2 += 1;
        const RectD region = PassExec::toCanonical(pixels, scale, par);
        RoIMap::iterator found = ret->find(map);
        if (found == ret->end()) {
            ret->insert(std::make_pair(map, region));
        } else if (map == source) {
            found->second.merge(region);
        } else {
            found->second = region;
        }
    }
} // IWarp::getRegionsOfInterest

bool
IWarp::getInputLayersRead(int inputNb,
                          double time,
                          ViewIdx view,
                          std::list<ImageLayerDesc>* layers)
{
    if (inputNb != kIWarpInputMap) {
        return false;
    }

    return _map.getInputLayersRead(this, kIWarpInputMap, time, view, layers);
}

bool
IWarp::isOpIdentity(double time,
                    const RenderScale& /*scale*/,
                    const RectI& /*roi*/,
                    ViewIdx view)
{
    WarpSettings settings;
    readWarpSettings(time, view, &settings);
    if (hasTimeBlur(settings)) {
        return false;
    }

    return isTransformIdentity(time, view);
}

StatusEnum
IWarp::renderOp(const RenderActionArgs& args,
                const StagePlan& plan)
{
    const float mix = (float)plan.mix;
    const bool maskInvert = plan.maskInvert;
    const bool passThrough = plan.stages.has(Stage::PassThroughUnprocessed);
    const int sourceInput = plan.sourceInput;

    WarpSettings settings;
    readWarpSettings(args.time, args.view, &settings);
    if (args.draftMode) {
        settings.filter = Resampler::eFilterImpulse;
    }
    const bool impulse = settings.filter == Resampler::eFilterImpulse;
    const Resampler::BlurSettings& blur = settings.blur;

    // The matrix curves: one per shutter time for a time blur (one when they are all equal),
    // else the curve at time, which a directional blur samples at amounts below the Map's.
    const int segments = curveSegments(args.time, args.view);
    std::vector<TransformInterp::MatrixCurve> curves;
    if (hasTimeBlur(settings)) {
        double tStart = args.time;
        double tEnd = args.time;
        Resampler::shutterRange(args.time, blur.shutter, blur.shutterOffset, blur.shutterCustomOffset, &tStart, &tEnd);
        const int count = Resampler::kMotionBlurTransformCount;
        const int timeSegments = (std::max)(kMinTimeCurveSegments, segments / 4);
        curves.resize(count);
        bool allEqual = true;
        for (int i = 0; i < count; ++i) {
            const double t = (i == 0) ? tStart : (tStart + i * (tEnd - tStart) / (double)(count - 1));
            if (!curves[i].build(pixelMatrixFn(t, args.view, settings.invert, args.mappedScale), timeSegments)) {
                // A time without a matrix samples nothing, as Resampler's null sampling matrix.
                curves[i] = TransformInterp::MatrixCurve::constant(Mat3());
            }
            allEqual = allEqual && (curves[i] == curves[0]);
        }
        if (allEqual) {
            curves.resize(1);
            // The single curve renders at full resolution of the Map, as without a blur.
            if (!curves[0].build(pixelMatrixFn(args.time, args.view, settings.invert, args.mappedScale), segments)) {
                curves[0] = TransformInterp::MatrixCurve::constant(Mat3());
            }
        }
    } else {
        curves.resize(1);
        if (!curves[0].build(pixelMatrixFn(args.time, args.view, settings.invert, args.mappedScale), segments)) {
            curves[0] = TransformInterp::MatrixCurve::constant(Mat3());
        }
    }

    // A directional blur over the amounts amountFrom + (amountTo - amountFrom) * (1 - (i + 1) / count),
    // times the pixel's Map value, weighted as Resampler::buildSamplingTransforms() weighs them.
    // Without motion blur (or when the amount does not move the image) it renders the first.
    std::vector<double> dirAmounts;
    std::vector<double> dirWeights;
    if (hasDirectionalBlur(settings)) {
        const int count = Resampler::kMotionBlurTransformCount;
        for (int i = 0; i < count; ++i) {
            const double a = 1. - (i + 1) / (double)count;
            const double amount = blur.amountFrom + (blur.amountTo - blur.amountFrom) * a;
            dirAmounts.push_back(amount);
            dirWeights.push_back((blur.fading <= 0.) ? 1. : std::pow(1. - std::abs(amount) / blur.amountTo, blur.fading));
        }
        if ((blur.motionBlur == 0.) || curves[0].isConstant()) {
            dirAmounts.resize(1);
            dirWeights.clear();
        }
    }
    const bool timeSampled = curves.size() > 1;
    const bool dirSampled = dirAmounts.size() > 1;
    const bool sampled = timeSampled || dirSampled;
    // A single amount scales the Map: 1 without a directional blur.
    const double amountScale = dirAmounts.empty() ? 1. : dirAmounts[0];

    // Every image is fetched before any is locked: fetching renders upstream, which may write
    // into a cached image this render would otherwise already hold a read lock on.
    const RectI& roi = args.roi;
    const int width = roi.width();
    const bool mapped = (bool)getInput(kIWarpInputMap);
    MapPlane mapPlane;
    {
        const MapInputParams mapParams = [&]() {
            const KnobReader reader(args.time, args.view);

            return _map.bake(reader);
        }();
        const RectI window(roi.x1 - 1, roi.y1 - 1, roi.x2 + 1, roi.y2 + 1);
        const StatusEnum status = _map.read(this, kIWarpInputMap, mapParams, args.time, args.view, args.mappedScale, window, &mapPlane);
        if (status != eStatusOK) {
            return status;
        }
    }

    const bool doMask = plan.applyMask;
    ImagePtr mask;
    const int maskChannel = plan.maskChannel;
    if (doMask) {
        if (maskChannel >= 0) {
            RectI maskRoI;
            mask = getImage(plan.maskInput, args.time, args.mappedScale, args.view, NULL, &plan.maskLayer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &maskRoI);
        }
        if (mask && (maskChannel >= (int)mask->getComponentsCount())) {
            mask.reset();
        }
    }
    if (!PassExec::isFloatImage(mask)) {
        return eStatusFailed;
    }

    struct Plane {
        ImagePtr dst;
        ImagePtr src;
        int nComps = 0;
        int srcNComps = 0;
        std::bitset<4> channels;
        RectI srcBounds;
        std::shared_ptr<Image::ReadAccess> srcAccess;
        std::shared_ptr<Image::WriteAccess> dstAccess;
        std::vector<float> converted;
        Resampler::SourceImage source;
    };
    std::vector<std::unique_ptr<Plane>> planes;
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        std::unique_ptr<Plane> plane(new Plane());
        plane->dst = it->second;
        if (plane->dst) {
            plane->nComps = (int)plane->dst->getComponentsCount();
            plane->channels = PassExec::processedBitsForImage(it->first, plane->nComps, args.processChannels);
            ImageLayerDesc sourceLayer;
            if ((sourceInput >= 0) && getInput(sourceInput) && resolveInputPlaneForRender(sourceInput, args.time, args.view, &sourceLayer, NULL)) {
                // Mapped to the clip's components, which is the layout the output plane is rendered in.
                RectI srcRoI;
                plane->src = getImage(sourceInput, args.time, args.mappedScale, args.view, NULL, &sourceLayer, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &srcRoI);
            }
            if (!PassExec::isFloatImage(plane->dst) || !PassExec::isFloatImage(plane->src)) {
                return eStatusFailed;
            }
        }
        planes.push_back(std::move(plane));
    }

    if ((width <= 0) || (roi.height() <= 0)) {
        return eStatusOK;
    }

    // Images are locked and their bounds read here, on the calling thread; the band threads only
    // read through these.
    PassExec::MaskInput maskSource;
    maskSource.image = mask;
    maskSource.lock();
    for (std::size_t p = 0; p < planes.size(); ++p) {
        Plane& plane = *planes[p];
        if (!plane.dst) {
            continue;
        }
        const int nComps = plane.nComps;
        if (plane.src) {
            plane.srcBounds = plane.src->getBounds();
            const int srcNComps = (int)plane.src->getComponentsCount();
            plane.srcNComps = srcNComps;
            plane.srcAccess = std::make_shared<Image::ReadAccess>(plane.src.get());
            if (srcNComps == nComps) {
                plane.source = Resampler::SourceImage((const float*)plane.srcAccess->pixelAt(plane.srcBounds.x1, plane.srcBounds.y1), plane.srcBounds, nComps);
            } else {
                const int srcWidth = plane.srcBounds.width();
                plane.converted.assign((std::size_t)srcWidth * plane.srcBounds.height() * nComps, 0.f);
                for (int y = plane.srcBounds.y1; y < plane.srcBounds.y2; ++y) {
                    PassExec::readSourceRow(plane.src.get(), plane.srcAccess.get(), plane.srcBounds, srcNComps, plane.srcBounds.x1, y, srcWidth, nComps,
                                            &plane.converted[(std::size_t)(y - plane.srcBounds.y1) * srcWidth * nComps]);
                }
                plane.source = Resampler::SourceImage(plane.converted.empty() ? (const float*)0 : &plane.converted[0], plane.srcBounds, nComps);
            }
        } else {
            plane.source = Resampler::SourceImage((const float*)0, RectI(), nComps);
        }
        plane.dstAccess = std::make_shared<Image::WriteAccess>(plane.dst.get());
    }

    const int nThreads = appPTR->getNCPUsAvailableForEffect();
    std::vector<RectI> bandRects;
    makeStageRowBands(roi, nThreads, &bandRects);
    std::vector<std::pair<std::size_t, RectI>> bands;
    for (std::size_t p = 0; p < planes.size(); ++p) {
        if (!planes[p]->dst) {
            continue;
        }
        for (std::size_t b = 0; b < bandRects.size(); ++b) {
            bands.push_back(std::make_pair(p, bandRects[b]));
        }
    }

    const float nan = std::numeric_limits<float>::quiet_NaN();

    // Where pixel (x, y) samples the source through curve at Map value m and gradient (dmdx, dmdy),
    // the Map scaled by amount; false when it samples nothing.
    const auto sampleAt = [&](const TransformInterp::MatrixCurve& curve,
                              double amount,
                              int x,
                              int y,
                              double m,
                              double dmdx,
                              double dmdy,
                              TransformInterp::SourceSample* sample) {
        Mat3 H;
        Mat3 dH;
        curve.evaluate(m * amount, &H, &dH);
        // d H(amount m) / dm = amount H'(amount m).
        return TransformInterp::sourceSample(H, scaled(dH, amount), (double)x + 0.5, (double)y + 0.5, dmdx * (impulse ? 0. : 1.), dmdy * (impulse ? 0. : 1.), sample);
    };

    // The Map value at (x, y) and its gradient, by central differences over the window grown by
    // one pixel.
    const auto mapAt = [&](int x,
                           int y,
                           double* m,
                           double* dmdx,
                           double* dmdy) {
        *m = mapPlane.at(x, y);
        if (impulse) {
            *dmdx = 0.;
            *dmdy = 0.;

            return;
        }
        *dmdx = 0.5 * ((double)mapPlane.at(x + 1, y) - (double)mapPlane.at(x - 1, y));
        *dmdy = 0.5 * ((double)mapPlane.at(x, y + 1) - (double)mapPlane.at(x, y - 1));
    };

    RenderCancellation cancel(this);

    const std::function<void(int)> renderBand = [&](int bandIndex) {
        const Plane& plane = *planes[bands[bandIndex].first];
        const RectI& band = bands[bandIndex].second;
        const int nComps = plane.nComps;
        const bool hasSource = plane.source.isValid();
        const std::size_t rowSize = (std::size_t)width * nComps;
        const bool direct = stageFinishIsCopy(doMask, mix, false, nComps, plane.channels);
        std::vector<float> warpedRow(direct ? 0 : rowSize);
        std::vector<float> referenceRow(direct ? 0 : rowSize);
        std::vector<float> maskRow(doMask ? width : 0);
        std::vector<float> sx(width);
        std::vector<float> sy(width);
        std::vector<float> jacobian(impulse ? 0 : (std::size_t)width * 4);
        std::vector<float> one(nComps);
        std::vector<double> accPix(nComps);
        std::vector<double> accPix2(nComps);
        std::vector<double> mean(nComps);

        // One motion-blurred pixel, as Resampler's motionBlurPixel averages it, each sample at the
        // pixel's Map value through one shutter time's curve or one directional amount.
        const auto blurredPixel = [&](int x,
                                      int y,
                                      double m,
                                      double dmdx,
                                      double dmdy,
                                      float* out) {
            const double motionBlur = blur.motionBlur;
            const double maxErr = Resampler::motionBlurMaxError(motionBlur);
            const double maxErr2 = maxErr * maxErr;
            const int maxIt = Resampler::motionBlurMaxIterations(motionBlur);
            const int minsamples = Resampler::motionBlurMinSamples(motionBlur);
            const std::size_t count = timeSampled ? curves.size() : dirAmounts.size();
            const bool weighted = !timeSampled;
            double acc = 0.;
            for (int c = 0; c < nComps; ++c) {
                accPix[c] = 0.;
                accPix2[c] = 0.;
                mean[c] = 0.;
            }
            unsigned int seed = Resampler::motionBlurHash(Resampler::motionBlurHash(x + (unsigned int)(0x10000 * motionBlur)) + y);
            int sample = 0;
            int maxsamples = minsamples;
            while (sample < maxsamples) {
                for (; sample < maxsamples; ++sample, ++seed) {
                    int t;
                    if (sample < minsamples) {
                        t = (int)((sample + Resampler::vanDerCorput2(seed)) * count / (double)minsamples);
                    } else {
                        t = (int)(Resampler::vanDerCorput2(seed) * count);
                    }
                    const TransformInterp::MatrixCurve& curve = timeSampled ? curves[t] : curves[0];
                    const double amount = timeSampled ? 1. : dirAmounts[t];
                    TransformInterp::SourceSample s;
                    float fx = nan;
                    float fy = nan;
                    float J[4] = { 0.f, 0.f, 0.f, 0.f };
                    if (sampleAt(curve, amount, x, y, m, dmdx, dmdy, &s)) {
                        fx = (float)s.x;
                        fy = (float)s.y;
                        for (int k = 0; k < 4; ++k) {
                            J[k] = (float)s.jacobian[k];
                        }
                    }
                    Resampler::resampleRowFromCoords(plane.source, &fx, &fy, impulse ? NULL : J, 1, settings.filter, settings.clamp, settings.blackOutside,
                                                     Resampler::eWrapNone, Resampler::eWrapNone, one.data(), nComps);
                    if (!weighted) {
                        for (int c = 0; c < nComps; ++c) {
                            accPix[c] += one[c];
                            accPix2[c] += (double)one[c] * one[c];
                        }
                    } else {
                        const double w = dirWeights[t];
                        acc += w;
                        for (int c = 0; c < nComps; ++c) {
                            accPix[c] += one[c] * w;
                            accPix2[c] += (double)one[c] * one[c] * w;
                        }
                    }
                }
                if (!weighted) {
                    for (int c = 0; c < nComps; ++c) {
                        mean[c] = accPix[c] / sample;
                        if ((sample > 1) && (maxsamples < maxIt)) {
                            const double var = (accPix2[c] - mean[c] * mean[c] * sample) / (sample - 1);
                            maxsamples = (std::max)(maxsamples, (std::min)(samplesForVariance(var / maxErr2), maxIt));
                        }
                    }
                } else if (acc > 0.) {
                    for (int c = 0; c < nComps; ++c) {
                        mean[c] = accPix[c] / acc;
                        if ((sample > 1) && (maxsamples < maxIt)) {
                            const double var = accPix2[c] / acc - mean[c] * mean[c];
                            maxsamples = (std::max)(maxsamples, (std::min)(samplesForVariance(var / maxErr2), maxIt));
                        }
                    }
                }
            }
            for (int c = 0; c < nComps; ++c) {
                out[c] = (float)mean[c];
            }
        };

        for (int y = band.y1; y < band.y2; ++y) {
            if ((((y - band.y1) % PassExec::kAbortCheckLines) == 0) && cancel.check()) {
                return;
            }

            float* dstPix = (float*)plane.dstAccess->pixelAt(roi.x1, y);
            if (!dstPix) {
                continue;
            }
            float* const outRow = direct ? dstPix : warpedRow.data();
            if (!hasSource) {
                std::fill(outRow, outRow + rowSize, 0.f);
            } else if (sampled) {
                for (int i = 0; i < width; ++i) {
                    double m = 0.;
                    double dmdx = 0.;
                    double dmdy = 0.;
                    mapAt(roi.x1 + i, y, &m, &dmdx, &dmdy);
                    blurredPixel(roi.x1 + i, y, m, dmdx, dmdy, outRow + (std::size_t)i * nComps);
                }
            } else {
                for (int i = 0; i < width; ++i) {
                    double m = 0.;
                    double dmdx = 0.;
                    double dmdy = 0.;
                    mapAt(roi.x1 + i, y, &m, &dmdx, &dmdy);
                    TransformInterp::SourceSample s;
                    if (sampleAt(curves[0], amountScale, roi.x1 + i, y, m, dmdx, dmdy, &s)) {
                        sx[i] = (float)s.x;
                        sy[i] = (float)s.y;
                        if (!impulse) {
                            for (int k = 0; k < 4; ++k) {
                                jacobian[(std::size_t)i * 4 + k] = (float)s.jacobian[k];
                            }
                        }
                    } else {
                        sx[i] = nan;
                        sy[i] = nan;
                        if (!impulse) {
                            std::fill(&jacobian[(std::size_t)i * 4], &jacobian[(std::size_t)i * 4] + 4, 0.f);
                        }
                    }
                }
                Resampler::resampleRowFromCoords(plane.source, sx.data(), sy.data(), impulse ? NULL : jacobian.data(), width, settings.filter, settings.clamp, settings.blackOutside,
                                                 Resampler::eWrapNone, Resampler::eWrapNone, outRow, nComps);
            }
            if (mapped && hasSource) {
                // Where the Map is 0 the output is the Source pixel itself, never filtered, which
                // would soften a held pixel with its displaced neighbours.
                for (int i = 0; i < width; ++i) {
                    if (mapPlane.at(roi.x1 + i, y) != 0.f) {
                        continue;
                    }
                    float* dst = &outRow[(std::size_t)i * nComps];
                    const float* pix = plane.source.pixel(roi.x1 + i, y);
                    if (pix) {
                        std::copy(pix, pix + nComps, dst);
                    } else {
                        std::fill(dst, dst + nComps, 0.f);
                    }
                }
            }
            if (direct) {
                continue;
            }
            PassExec::readSourceRow(plane.src.get(), plane.srcAccess.get(), plane.srcBounds, plane.src ? plane.srcNComps : nComps, roi.x1, y, width, nComps, &referenceRow[0]);
            if (doMask) {
                PassExec::readChannelRow(mask.get(), maskSource.access.get(), maskSource.bounds, maskSource.nComps, maskChannel, roi.x1, y, width, 0.f, &maskRow[0]);
            }

            for (int i = 0; i < width; ++i, dstPix += nComps) {
                const float* srcPix = &referenceRow[(std::size_t)i * nComps];
                const float* outPix = &warpedRow[(std::size_t)i * nComps];
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
} // IWarp::renderOp

bool
IWarp::resolveGpuMapChannel(double time,
                            ViewIdx view,
                            ImageLayerDesc* layer,
                            int* channel) const
{
    return resolveWarpGpuMapChannel(*this, _map, kIWarpInputMap, time, view, layer, channel);
}

bool
IWarp::canRenderOpOnGpu(const GpuQuery& query) const
{
#ifdef NATRON_HAVE_GPU
    if ((query.bitDepth != eImageBitDepthFloat) || !canRunStagesGpu(resolveGpuPlan(query), query)) {
        return false;
    }
    WarpSettings settings;
    readWarpSettings(query.time, query.view, &settings);
    // Motion blur averages hundreds of warps per pixel, which only the CPU does.
    if (hasTimeBlur(settings) || hasDirectionalBlur(settings)) {
        return false;
    }
    ImageLayerDesc layer;
    int channel = -1;
    if (!resolveGpuMapChannel(query.time, query.view, &layer, &channel)) {
        return false;
    }
    TransformInterp::MatrixCurve curve;

    return curve.build(pixelMatrixFn(query.time, query.view, settings.invert, query.mappedScale), curveSegments(query.time, query.view));
#else
    Q_UNUSED(query);

    return false;
#endif
}

bool
IWarp::readsGpuInput(int inputNb,
                     const ImageLayerDesc& /*plane*/,
                     double time,
                     ViewIdx view) const
{
    if (inputNb != kIWarpInputMap) {
        return true;
    }
    ImageLayerDesc layer;
    int channel = -1;

    return resolveGpuMapChannel(time, view, &layer, &channel) && (channel >= 0);
}

bool
IWarp::getOpGpuInputLayer(int inputNb,
                          const ImageLayerDesc& plane,
                          double time,
                          ViewIdx view,
                          ImageLayerDesc* layer) const
{
    if (inputNb == kIWarpInputMap) {
        int channel = -1;
        if (resolveGpuMapChannel(time, view, layer, &channel) && (layer->getNumComponents() > 0)) {
            return true;
        }
    }
    stageGpuInputLayer(*this, inputNb, plane, time, view, layer);

    return true;
}

RectI
IWarp::getOpGpuInputRoI(int inputNb,
                        double time,
                        ViewIdx view,
                        const RectI& outputRect,
                        const RenderScale& scale) const
{
    if ((inputNb == kIWarpInputSource) || (inputNb == kIWarpInputMap)) {
        return warpGpuInputRoI(*this, inputNb, kIWarpInputSource, kIWarpInputMap, time, view, outputRect, scale);
    }

    return StagedImageEffect::getOpGpuInputRoI(inputNb, time, view, outputRect, scale);
}

StatusEnum
IWarp::renderOpGpu(const GpuRenderArgs& args)
{
#ifdef NATRON_HAVE_GPU
    const GpuQuery& query = args.query;
    ImageLayerDesc mapLayer;
    int mapChannel = -1;
    if (!canRenderOpOnGpu(query) || !resolveGpuMapChannel(query.time, query.view, &mapLayer, &mapChannel)) {
        return eStatusFailed;
    }
    WarpSettings settings;
    readWarpSettings(query.time, query.view, &settings);
    if (query.draftMode) {
        settings.filter = Resampler::eFilterImpulse;
    }
    TransformInterp::MatrixCurve curve;
    if (!curve.build(pixelMatrixFn(query.time, query.view, settings.invert, query.mappedScale), curveSegments(query.time, query.view))) {
        return eStatusFailed;
    }
    const MapInputParams mapParams = [&]() {
        const KnobReader reader(query.time, query.view);

        return _map.bake(reader);
    }();

    const StagePlan plan = resolveGpuPlan(query);
    GpuRenderArgs cropped = args;
    const int sourceInput = plan.sourceInput;
    if ((sourceInput >= 0) && (sourceInput < (int)cropped.inputs.size())) {
        if (!stageGpuInputView(*this, args, sourceInput, &cropped.inputs[sourceInput])) {
            return eStatusFailed;
        }
    }
    const bool hasSource = (sourceInput >= 0) && (sourceInput < (int)cropped.inputs.size()) && cropped.inputs[sourceInput].buffer;

    GpuImageView mapView;
    if (!stageGpuInputView(*this, args, kIWarpInputMap, &mapView)) {
        return eStatusFailed;
    }

    // The header, word for word as iwarp.slang loads it, then the curve.
    uint32_t header[kGpuHeaderWords] = {};
    uint32_t flags = (settings.clamp ? kIWarpGpuClamp : 0u) | (settings.blackOutside ? kIWarpGpuBlackOutside : 0u) | ((settings.filter != Resampler::eFilterImpulse) ? kIWarpGpuJacobian : 0u) | (hasSource ? kIWarpGpuSample : 0u) | (mapParams.invert ? kIWarpGpuMapInvert : 0u) | (mapParams.smooth ? kIWarpGpuMapSmooth : 0u);
    int32_t mapComponent = -1;
    if (getInput(kIWarpInputMap)) {
        flags |= kIWarpGpuMapConnected;
        if (mapView.buffer && (mapChannel >= 0) && (mapChannel < mapLayer.getNumComponents())) {
            const int component = stageGpuMaskViewComponent(mapLayer, mapChannel, mapView.nComps);
            if ((component >= 0) && (component < mapView.nComps)) {
                mapComponent = component;
                flags |= kIWarpGpuMapRead;
                header[2] = (uint32_t)(mapView.byteOffset / sizeof(float));
                header[3] = (uint32_t)mapView.rowStrideFloats;
                header[4] = (uint32_t)mapView.bounds.x1;
                header[5] = (uint32_t)mapView.bounds.y1;
                header[6] = (uint32_t)mapView.bounds.x2;
                header[7] = (uint32_t)mapView.bounds.y2;
                header[8] = (uint32_t)mapView.nComps;
            }
        }
    }
    header[0] = (uint32_t)settings.filter;
    header[1] = flags;
    header[9] = (uint32_t)mapComponent;
    std::memcpy(&header[10], &mapParams.exponent, sizeof(float));
    header[11] = (uint32_t)curve.segments();

    std::vector<float> lut;
    curve.pack(&lut);

    PixelKernelGpu op;
    op.kernel = (int)gpu::GpuKernelId::iwarp;
    op.params.resize(sizeof(header) + lut.size() * sizeof(float));
    std::memcpy(op.params.data(), header, sizeof(header));
    std::memcpy(op.params.data() + sizeof(header), lut.data(), lut.size() * sizeof(float));

    return recordStagesGpuKernelInputs(plan, cropped, op, std::span<const GpuImageView>(&mapView, 1));
#else
    Q_UNUSED(args);

    return eStatusFailed;
#endif
} // IWarp::renderOpGpu

void
IWarp::onEffectCreated(bool /*mayCreateFileDialog*/,
                       const CreateNodeArgs& /*args*/)
{
    refreshMapWarning();
}

void
IWarp::onInputChanged(int /*inputNo*/)
{
    refreshMapWarning();
}

void
IWarp::refreshMapWarning()
{
    const NodePtr node = getNode();

    if (!node) {
        return;
    }
    const std::string message = mapNotConnectedMessage();
    if (!getInput(kIWarpInputMap)) {
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

NATRON_NAMESPACE_EXIT
