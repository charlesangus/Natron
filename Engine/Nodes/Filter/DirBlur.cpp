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

#include "DirBlur.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <list>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/PassExecutor.h"
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Image/StagePlan.h"
#include "Engine/Nodes/KnobReader.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

using TransformMath::Mat3;

namespace {
#ifdef NATRON_HAVE_GPU
// dirblur.slang's parameter block layout.
constexpr std::size_t kDirBlurGpuHeaderWords = 4;
constexpr std::size_t kDirBlurGpuSampleWords = 16;
constexpr uint32_t kDirBlurGpuSourceValid = 1;
#endif

constexpr InputSpec kDirBlurInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

constexpr NodeSpec kDirBlurNodeSpec = {
    .id = PLUGINID_NATRON_DIRBLUR,
    .label = "DirBlur",
    .description = "Blur the image along a direction, around a centre or towards a centre.\n"
                   "\n"
                   "Directional smears each pixel Length pixels along Direction, as a straight motion blur. Radial smears it around Center by an angle of Length degrees, as a spinning motion blur. Zoom smears it along the line through Center, so that the corners of the format move by Length pixels; a positive Length streaks towards Center, a negative one away from it.\n"
                   "\n"
                   "The result is the weighted average of Samples copies of the image, each moved by an even step along the blur and filtered bilinearly. Falloff fades the copies out towards the end of the blur, and From Color and To Color tint them, from the start of the blur to its end, which gives God-rays-like light streaks in Zoom mode. The weights are normalised, so with the default white tints the blur keeps the image's energy.\n"
                   "\n"
                   "Beyond the source's region of definition the image reads black.",
    .grouping = PLUGIN_GROUP_FILTER,
    .majorVersion = PLUGIN_MAJOR_NATRON_DIRBLUR,
    .minorVersion = 0,
    .icon = NATRON_IMAGES_PATH "NativeNodes/DirBlur.png",
    .inputs = kDirBlurInputs,
};

constexpr ImagePipelineSpec kDirBlurPipeline = {
    .stages = { Stage::MaskMix, Stage::PassThroughUnprocessed },
    .layers = LayerPolicy::color(true, true, true, true),
    .op = OpKind::Custom,
};

// The source pixels a sample of a matrix with linear part (a b; d e) reads around its centre, at
// most: the bilinear footprint and the supersampling of a minifying sample.
double
footprint(const Mat3& m,
          double par)
{
    const double spread = std::fabs(m(0, 0)) + std::fabs(m(0, 1)) + std::fabs(m(1, 0)) + std::fabs(m(1, 1));

    return 2. + std::ceil(spread * std::max(par, 1. / par));
}

RectD
nullRect()
{
    return RectD(0., 0., 0., 0.);
}

void
mergeInto(const RectD& r,
          RectD* into)
{
    if (r.isNull()) {
        return;
    }
    if (into->isNull()) {
        *into = r;
    } else {
        into->merge(r);
    }
}
} // namespace

DirBlur::DirBlur(NodePtr node)
    : StagedImageEffect(node)
{
}

DirBlur::~DirBlur()
{
}

const NodeSpec*
DirBlur::nodeSpec() const
{
    return staticNodeSpec<kDirBlurNodeSpec>();
}

const ImagePipelineSpec&
DirBlur::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kDirBlurPipeline, kDirBlurNodeSpec>();
}

std::string
DirBlur::getInputHint(int inputNb) const
{
    switch (inputNb) {
    case kDirBlurInputSource:
        return tr("The image to blur.").toStdString();
    case kDirBlurInputMask:
        return tr("The mask the blurred result is mixed with the source through.").toStdString();
    }

    return std::string();
}

void
DirBlur::initializeKnobs()
{
    const KnobPagePtr page = createControlsPage();

    KnobChoicePtr modeKnob = AppManager::createKnob<KnobChoice>(this, tr("Mode"));
    modeKnob->setName(kDirBlurParamMode);
    modeKnob->setHintToolTip(tr("How the image is smeared."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kDirBlurParamModeDirectional, tr("Directional").toStdString(), tr("Along Direction, by Length pixels: a straight motion blur. Center is not used.").toStdString()));
        options.push_back(ChoiceOption(kDirBlurParamModeRadial, tr("Radial").toStdString(), tr("Around Center, by an angle of Length degrees, counter-clockwise for a positive Length: a spinning motion blur.").toStdString()));
        options.push_back(ChoiceOption(kDirBlurParamModeZoom, tr("Zoom").toStdString(), tr("Along the line through Center, the corners of the format moving by Length pixels: towards Center for a positive Length, away from it for a negative one.").toStdString()));
        modeKnob->populateChoices(options);
    }
    modeKnob->setDefaultValue((int)eDirBlurModeZoom);
    page->addKnob(modeKnob);
    _mode = modeKnob;

    KnobDoublePtr lengthKnob = AppManager::createKnob<KnobDouble>(this, tr("Length"));
    lengthKnob->setName(kDirBlurParamLength);
    lengthKnob->setHintToolTip(tr("The extent of the blur: in pixels in Directional and Zoom modes, in degrees in Radial mode. With Centered it extends this far on each side. 0 leaves the input unchanged."));
    lengthKnob->setDisplayMinimum(-100.);
    lengthKnob->setDisplayMaximum(100.);
    lengthKnob->setDefaultValue(20.);
    page->addKnob(lengthKnob);
    _length = lengthKnob;

    KnobDoublePtr centerKnob = AppManager::createKnob<KnobDouble>(this, tr("Center"), 2);
    centerKnob->setName(kDirBlurParamCenter);
    centerKnob->setHintToolTip(tr("The centre of the Radial and Zoom blurs. Can also be moved by dragging its handle in the Viewer."));
    centerKnob->setSpatial(true);
    centerKnob->setDefaultValuesAreNormalized(true);
    centerKnob->disableSlider();
    for (int d = 0; d < 2; ++d) {
        centerKnob->setMinimum(-DBL_MAX, d);
        centerKnob->setMaximum(DBL_MAX, d);
        centerKnob->setDisplayMinimum(-10000., d);
        centerKnob->setDisplayMaximum(10000., d);
        centerKnob->setIncrement(1., d);
        centerKnob->setDefaultValue(0.5, d);
    }
    page->addKnob(centerKnob);
    centerKnob->setHasHostOverlayHandle(true);
    _center = centerKnob;

    KnobDoublePtr directionKnob = AppManager::createKnob<KnobDouble>(this, tr("Direction"));
    directionKnob->setName(kDirBlurParamDirection);
    directionKnob->setHintToolTip(tr("The direction of the Directional blur, in degrees counter-clockwise from the x axis."));
    directionKnob->setDisplayMinimum(-180.);
    directionKnob->setDisplayMaximum(180.);
    directionKnob->setDefaultValue(45.);
    page->addKnob(directionKnob);
    _direction = directionKnob;

    KnobIntPtr samplesKnob = AppManager::createKnob<KnobInt>(this, tr("Samples"));
    samplesKnob->setName(kDirBlurParamSamples);
    samplesKnob->setHintToolTip(tr("How many copies of the image are averaged along the blur. More give a smoother blur and cost proportionally more; about one per pixel of blur avoids visible steps."));
    samplesKnob->setMinimum(1);
    samplesKnob->setMaximum(kMaxSamples);
    samplesKnob->setDisplayMinimum(1);
    samplesKnob->setDisplayMaximum(128);
    samplesKnob->setDefaultValue(32);
    page->addKnob(samplesKnob);
    _samples = samplesKnob;

    KnobBoolPtr centeredKnob = AppManager::createKnob<KnobBool>(this, tr("Centered"));
    centeredKnob->setName(kDirBlurParamCentered);
    centeredKnob->setHintToolTip(tr("When checked the blur extends Length both ways from each pixel instead of one way."));
    centeredKnob->setDefaultValue(false);
    page->addKnob(centeredKnob);
    _centered = centeredKnob;

    KnobDoublePtr falloffKnob = AppManager::createKnob<KnobDouble>(this, tr("Falloff"));
    falloffKnob->setName(kDirBlurParamFalloff);
    falloffKnob->setHintToolTip(tr("How the copies fade towards the end of the blur: a copy at a fraction t of the blur weighs (1 - t)^Falloff. 0 weighs every copy the same."));
    falloffKnob->setMinimum(0.);
    falloffKnob->setDisplayMinimum(0.);
    falloffKnob->setDisplayMaximum(4.);
    falloffKnob->setDefaultValue(0.);
    page->addKnob(falloffKnob);
    _falloff = falloffKnob;

    KnobColorPtr fromColorKnob = AppManager::createKnob<KnobColor>(this, tr("From Color"), 4);
    fromColorKnob->setName(kDirBlurParamFromColor);
    fromColorKnob->setHintToolTip(tr("The tint of the copies at the start of the blur. The tint goes linearly to To Color at its end."));
    for (int d = 0; d < 4; ++d) {
        fromColorKnob->setDefaultValue(1., d);
    }
    page->addKnob(fromColorKnob);
    _fromColor = fromColorKnob;

    KnobColorPtr toColorKnob = AppManager::createKnob<KnobColor>(this, tr("To Color"), 4);
    toColorKnob->setName(kDirBlurParamToColor);
    toColorKnob->setHintToolTip(tr("The tint of the copies at the end of the blur."));
    for (int d = 0; d < 4; ++d) {
        toColorKnob->setDefaultValue(1., d);
    }
    page->addKnob(toColorKnob);
    _toColor = toColorKnob;

    addStageKnobs(page);
} // DirBlur::initializeKnobs

DirBlurParams
DirBlur::getParams(double time,
                   ViewIdx view) const
{
    const KnobReader reader(time, view);
    DirBlurParams params;

    const int mode = reader.value(_mode);
    params.mode = (mode == (int)eDirBlurModeDirectional) ? eDirBlurModeDirectional : (mode == (int)eDirBlurModeRadial) ? eDirBlurModeRadial
                                                                                                                       : eDirBlurModeZoom;
    params.length = reader.value(_length);
    params.centerX = reader.value(_center, 0);
    params.centerY = reader.value(_center, 1);
    params.direction = reader.value(_direction);
    params.samples = std::min(kMaxSamples, std::max(1, (int)reader.value(_samples)));
    params.centered = reader.value(_centered);
    params.falloff = std::max(0., (double)reader.value(_falloff));
    for (int c = 0; c < 4; ++c) {
        params.fromColor[c] = reader.value(_fromColor, c);
        params.toColor[c] = reader.value(_toColor, c);
    }

    return params;
}

double
DirBlur::outputPar(double time,
                   ViewIdx view) const
{
    const NodeMetadataConstPtr metadata = getRenderMetadata(time, view);

    return metadata ? metadata->getPixelAspectRatio(-1) : 1.;
}

double
DirBlur::zoomReference(double time,
                       ViewIdx view) const
{
    const NodeMetadataConstPtr metadata = getRenderMetadata(time, view);
    if (!metadata) {
        return 0.;
    }
    const RectI format = metadata->getOutputFormat();
    if (format.isNull()) {
        return 0.;
    }
    const double par = metadata->getPixelAspectRatio(-1);

    return 0.5 * std::hypot(format.width() * par, (double)format.height());
}

RectD
DirBlur::sourceRoD(double time,
                   ViewIdx view,
                   const RenderScale& scale) const
{
    EffectInstancePtr input = getInput(kDirBlurInputSource);
    if (!input) {
        return nullRect();
    }
    const RenderScale inputScale = input->supportsRenderScale() ? scale : RenderScale::identity;
    RectD rod;
    bool isProjectFormat = false;
    if ((input->getRegionOfDefinition_public(input->getRenderHash(), time, inputScale, view, &rod, &isProjectFormat) == eStatusFailed) || rod.isNull()) {
        return nullRect();
    }

    return rod;
}

std::vector<double>
DirBlur::samplePositions(const DirBlurParams& params)
{
    const int n = std::max(1, params.samples);
    std::vector<double> t(n);
    for (int i = 0; i < n; ++i) {
        t[i] = params.centered ? (-1. + (2. * i + 1.) / n) : ((i + 0.5) / n);
    }

    return t;
}

std::vector<DirBlurSample>
DirBlur::canonicalSamples(const DirBlurParams& params,
                          double zoomReference)
{
    const std::vector<double> positions = samplePositions(params);
    std::vector<double> weights(positions.size());
    double sum = 0.;
    for (std::size_t i = 0; i < positions.size(); ++i) {
        weights[i] = (params.falloff > 0.) ? std::pow(1. - std::fabs(positions[i]), params.falloff) : 1.;
        sum += weights[i];
    }

    const Mat3 toCenter = TransformMath::translation(params.centerX, params.centerY);
    const Mat3 fromCenter = TransformMath::translation(-params.centerX, -params.centerY);
    const double direction = TransformMath::toRadians(params.direction);
    std::vector<DirBlurSample> samples(positions.size());
    for (std::size_t i = 0; i < positions.size(); ++i) {
        const double t = positions[i];
        DirBlurSample& s = samples[i];
        // Destination to source: the output pixel reads the source where the sample came from.
        switch (params.mode) {
        case eDirBlurModeDirectional: {
            const double offset = t * params.length;
            s.inverse = TransformMath::translation(-offset * std::cos(direction), -offset * std::sin(direction));
            break;
        }
        case eDirBlurModeRadial:
            s.inverse = toCenter * TransformMath::rotation(TransformMath::toRadians(t * params.length)) * fromCenter;
            break;
        case eDirBlurModeZoom: {
            const double k = (zoomReference > 0.) ? std::max(kMinZoomScale, 1. + t * params.length / zoomReference) : 1.;
            s.inverse = toCenter * TransformMath::scale(k, k) * fromCenter;
            break;
        }
        }
        const double a = std::fabs(t);
        for (int c = 0; c < 4; ++c) {
            const double tint = params.fromColor[c] + (params.toColor[c] - params.fromColor[c]) * a;
            s.factor[c] = (float)(weights[i] * tint / sum);
        }
    }

    return samples;
}

std::vector<DirBlurSample>
DirBlur::pixelSamples(const std::vector<DirBlurSample>& canonical,
                      double time,
                      const RenderScale& scale,
                      double par)
{
    const double sx = scale.toOfxPointD().x;
    const double sy = scale.toOfxPointD().y;
    std::vector<DirBlurSample> pixel(canonical.size());
    for (std::size_t i = 0; i < canonical.size(); ++i) {
        const Mat3& m = canonical[i].inverse;
        const Resampler::CanonicalTransformFn fn = [&m](double, double, bool, Mat3* out) {
            *out = m;

            return true;
        };
        Resampler::SamplingTransforms transforms;
        Resampler::buildSamplingTransforms(fn, time, false, Resampler::BlurSettings(), sx, sy, false, par, par, &transforms);
        pixel[i] = canonical[i];
        pixel[i].inverse = transforms.invTransforms.front();
    }

    return pixel;
}

RectD
DirBlur::sourceRegion(const std::vector<DirBlurSample>& canonical,
                      const RectD& window,
                      const RenderScale& scale,
                      double par,
                      const RectD& srcRoD)
{
    RectD region = nullRect();
    if (window.isNull()) {
        return region;
    }
    const double sx = scale.toOfxPointD().x;
    const double sy = scale.toOfxPointD().y;
    for (const DirBlurSample& s : canonical) {
        RectD r;
        Resampler::transformRegionFromRect(window, s.inverse, &r);
        if (r.isNull()) {
            continue;
        }
        const double grow = footprint(s.inverse, par);
        r.x1 -= grow * par / sx;
        r.x2 += grow * par / sx;
        r.y1 -= grow / sy;
        r.y2 += grow / sy;
        mergeInto(r, &region);
    }
    if (!srcRoD.isNull() && !region.isNull()) {
        if (!region.intersects(srcRoD)) {
            return nullRect();
        }
        region = region.intersect(srcRoD);
    }

    return region;
}

StatusEnum
DirBlur::getRegionOfDefinition(U64 /*hash*/,
                               double time,
                               const RenderScale& scale,
                               ViewIdx view,
                               RectD* rod)
{
    const RectD srcRoD = sourceRoD(time, view, scale);
    if (srcRoD.isNull()) {
        rod->clear();

        return eStatusOK;
    }
    const DirBlurParams params = getParams(time, view);
    if (srcRoD.isInfinite() || (params.length == 0.)) {
        *rod = srcRoD;

        return eStatusOK;
    }

    RectD region = nullRect();
    for (const DirBlurSample& s : canonicalSamples(params, zoomReference(time, view))) {
        Mat3 forward;
        if (!s.inverse.inverse(&forward)) {
            continue;
        }
        RectD r;
        Resampler::transformRegionFromRect(srcRoD, forward, &r);
        mergeInto(r, &region);
    }
    if (!region.isNull()) {
        Resampler::expandRoD(outputPar(time, view), scale.toOfxPointD().x, scale.toOfxPointD().y, true, &region);
    }
    if (isMaskApplied() || (getMixValue(time, view) != 1.)) {
        mergeInto(srcRoD, &region);
    }
    *rod = region;

    return eStatusOK;
}

void
DirBlur::getRegionsOfInterest(double time,
                              const RenderScale& scale,
                              const RectD& outputRoD,
                              const RectD& renderWindow,
                              ViewIdx view,
                              RoIMap* ret)
{
    StagedImageEffect::getRegionsOfInterest(time, scale, outputRoD, renderWindow, view, ret);

    EffectInstancePtr source = getInput(kDirBlurInputSource);
    if (!source) {
        return;
    }
    // The window itself too, which the mask and mix blend with.
    RectD roi = renderWindow;
    const DirBlurParams params = getParams(time, view);
    if (params.length != 0.) {
        const RectD read = sourceRegion(canonicalSamples(params, zoomReference(time, view)), renderWindow, scale, outputPar(time, view), sourceRoD(time, view, scale));
        mergeInto(read, &roi);
    }
    (*ret)[source] = roi;
}

bool
DirBlur::isOpIdentity(double time,
                      const RenderScale& /*scale*/,
                      const RectI& /*roi*/,
                      ViewIdx view)
{
    return getParams(time, view).length == 0.;
}

StatusEnum
DirBlur::renderOp(const RenderActionArgs& args,
                  const StagePlan& plan)
{
    const DirBlurParams params = getParams(args.time, args.view);
    const std::vector<DirBlurSample> samples = pixelSamples(canonicalSamples(params, zoomReference(args.time, args.view)), args.time, args.mappedScale, outputPar(args.time, args.view));
    const Resampler::FilterEnum filter = args.draftMode ? Resampler::eFilterImpulse : Resampler::eFilterBilinear;

    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        if (!it->second) {
            continue;
        }
        const int nComps = (int)it->second->getComponentsCount();

        ImagePtr src;
        ImageLayerDesc sourceLayer;
        if (resolveInputPlaneForRender(kDirBlurInputSource, args.time, args.view, &sourceLayer, NULL)) {
            RectI sourceRoI;
            src = getImage(kDirBlurInputSource, args.time, args.mappedScale, args.view, NULL, &sourceLayer, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &sourceRoI);
        }
        if (src && !PassExec::isFloatImage(src)) {
            return eStatusFailed;
        }

        // The source in the output plane's layout, locked here; the band threads only read it.
        RectI srcBounds;
        std::shared_ptr<Image::ReadAccess> srcAccess;
        std::vector<float> converted;
        Resampler::SourceImage source((const float*)0, RectI(), nComps);
        if (src) {
            srcBounds = src->getBounds();
            const int srcNComps = (int)src->getComponentsCount();
            srcAccess = std::make_shared<Image::ReadAccess>(src.get());
            if (srcBounds.isNull()) {
                src.reset();
            } else if (srcNComps == nComps) {
                source = Resampler::SourceImage((const float*)srcAccess->pixelAt(srcBounds.x1, srcBounds.y1), srcBounds, nComps);
            } else {
                const int width = srcBounds.width();
                converted.resize((std::size_t)width * srcBounds.height() * nComps);
                for (int y = srcBounds.y1; y < srcBounds.y2; ++y) {
                    PassExec::readSourceRow(src.get(), srcAccess.get(), srcBounds, srcNComps, srcBounds.x1, y, width, nComps, &converted[(std::size_t)(y - srcBounds.y1) * width * nComps]);
                }
                source = Resampler::SourceImage(&converted[0], srcBounds, nComps);
            }
        }
        const bool sampled = (bool)src;

        const auto row = [&](const RowIO& io) {
            const std::size_t n = (std::size_t)io.width * io.nComps;
            std::vector<float> sum(n, 0.f);
            if (sampled) {
                std::vector<float> tmp(n);
                int bits[4];
                for (int c = 0; c < io.nComps; ++c) {
                    bits[c] = pixelKernelChannelBit(io.nComps, c);
                }
                // Each sample is one matrix resampled on its own, summed in sample order, so a
                // pixel does not depend on the window it is rendered in.
                for (const DirBlurSample& s : samples) {
                    Resampler::ResampleParams rp;
                    rp.invTransforms = &s.inverse;
                    rp.alphas = 0;
                    rp.count = 1;
                    rp.filter = filter;
                    rp.clamp = false;
                    rp.blackOutside = true;
                    rp.motionBlur = 0.;
                    Resampler::resampleRow(rp, source, io.y, io.x0, io.x0 + io.width, &tmp[0]);
                    for (int i = 0; i < io.width; ++i) {
                        for (int c = 0; c < io.nComps; ++c) {
                            const std::size_t k = (std::size_t)i * io.nComps + c;
                            sum[k] += s.factor[bits[c]] * tmp[k];
                        }
                    }
                }
            }
            for (int c = 0; c < io.nComps; ++c) {
                if (!io.channels[pixelKernelChannelBit(io.nComps, c)]) {
                    continue;
                }
                for (int i = 0; i < io.width; ++i) {
                    io.dst[(std::size_t)i * io.nComps + c] = sum[(std::size_t)i * io.nComps + c];
                }
            }
        };

        EffectInstance::RenderActionArgs planeArgs = args;
        planeArgs.outputLayers.clear();
        planeArgs.outputLayers.push_back(*it);
        const StatusEnum status = runStagesCpu(*this, plan, planeArgs, StageRowFn(row));
        if (status != eStatusOK) {
            return status;
        }
    }

    return eStatusOK;
} // DirBlur::renderOp

bool
DirBlur::canRenderOpOnGpu(const GpuQuery& query) const
{
#ifdef NATRON_HAVE_GPU
    return (query.bitDepth == eImageBitDepthFloat) && canRunStagesGpu(resolveGpuPlan(query), query);
#else
    Q_UNUSED(query);

    return false;
#endif
}

bool
DirBlur::getOpGpuInputLayer(int inputNb,
                            const ImageLayerDesc& plane,
                            double time,
                            ViewIdx view,
                            ImageLayerDesc* layer) const
{
    stageGpuInputLayer(*this, inputNb, plane, time, view, layer);

    return true;
}

RectI
DirBlur::getOpGpuInputRoI(int inputNb,
                          double time,
                          ViewIdx view,
                          const RectI& outputRect,
                          const RenderScale& scale) const
{
    if (inputNb != kDirBlurInputSource) {
        return StagedImageEffect::getOpGpuInputRoI(inputNb, time, view, outputRect, scale);
    }
    const DirBlurParams params = getParams(time, view);
    if (params.length == 0.) {
        return outputRect;
    }
    // The same canonical region the CPU asks for, in pixels.
    const double par = outputPar(time, view);
    const double sx = scale.toOfxPointD().x;
    const double sy = scale.toOfxPointD().y;
    const RectD window(outputRect.x1 * par / sx, outputRect.y1 / sy, outputRect.x2 * par / sx, outputRect.y2 / sy);
    const RectD read = sourceRegion(canonicalSamples(params, zoomReference(time, view)), window, scale, par, sourceRoD(time, view, scale));
    RectI roi = outputRect;
    if (!read.isNull()) {
        roi.merge(read.toPixelEnclosing(scale, par));
    }

    return roi;
}

StatusEnum
DirBlur::renderOpGpu(const GpuRenderArgs& args)
{
#ifdef NATRON_HAVE_GPU
    const GpuQuery& query = args.query;
    if (!canRenderOpOnGpu(query)) {
        return eStatusFailed;
    }
    const StagePlan plan = resolveGpuPlan(query);
    const DirBlurParams params = getParams(query.time, query.view);
    const std::vector<DirBlurSample> samples = pixelSamples(canonicalSamples(params, zoomReference(query.time, query.view)), query.time, query.mappedScale, outputPar(query.time, query.view));
    const Resampler::FilterEnum filter = query.draftMode ? Resampler::eFilterImpulse : Resampler::eFilterBilinear;

    // The source cropped to its region of definition, as the CPU fetches it, is both sampled and
    // blended under the mask and mix.
    GpuRenderArgs cropped = args;
    const int sourceInput = plan.sourceInput;
    const bool sourceSlot = (sourceInput >= 0) && (sourceInput < (int)cropped.inputs.size());
    if (sourceSlot && !stageGpuInputView(*this, args, sourceInput, &cropped.inputs[sourceInput])) {
        return eStatusFailed;
    }
    const bool hasSource = sourceSlot && cropped.inputs[sourceInput].buffer;

    // dirblur.slang's parameter block: filter, flags and count, then per sample the matrix's two
    // rows as (hi, lo) float pairs and the four factors.
    std::vector<uint32_t> words(kDirBlurGpuHeaderWords + samples.size() * kDirBlurGpuSampleWords, 0u);
    words[0] = (uint32_t)filter;
    words[1] = hasSource ? kDirBlurGpuSourceValid : 0u;
    words[2] = (uint32_t)samples.size();
    for (std::size_t i = 0; i < samples.size(); ++i) {
        uint32_t* w = &words[kDirBlurGpuHeaderWords + i * kDirBlurGpuSampleWords];
        const Mat3& m = samples[i].inverse;
        const double entries[6] = { m(0, 0), m(0, 1), m(0, 2), m(1, 0), m(1, 1), m(1, 2) };
        for (int e = 0; e < 6; ++e) {
            const float hi = (float)entries[e];
            const float lo = (float)(entries[e] - (double)hi);
            std::memcpy(&w[2 * e], &hi, sizeof(float));
            std::memcpy(&w[2 * e + 1], &lo, sizeof(float));
        }
        std::memcpy(&w[12], samples[i].factor, 4 * sizeof(float));
    }

    PixelKernelGpu op;
    op.kernel = (int)gpu::GpuKernelId::dirblur;
    op.params.resize(words.size() * sizeof(uint32_t));
    std::memcpy(op.params.data(), words.data(), op.params.size());

    return recordStagesGpuKernelInputs(plan, cropped, op, std::span<const GpuImageView>());
#else
    Q_UNUSED(args);

    return eStatusFailed;
#endif
}

NATRON_NAMESPACE_EXIT
