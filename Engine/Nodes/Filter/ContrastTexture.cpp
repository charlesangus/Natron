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

#include "ContrastTexture.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/PassExecutor.h"
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/Nodes/Image/StagePlan.h"
#include "Engine/Nodes/KnobReader.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Nodes/Filter/EdgeAware/EdgeAwareGpu.h"
#endif

NATRON_NAMESPACE_ENTER

namespace CT = ContrastTextureKernels;

namespace {
#ifdef NATRON_HAVE_GPU
// Mirrors CtParams in Engine/Gpu/Kernels/contrastntexture.slang.
struct ContrastTextureGpuParams {
    uint32_t width;
    uint32_t height;
    int32_t fullX1;
    int32_t fullY2;
    uint32_t srcOffset;
    uint32_t srcStride;
    uint32_t srcComps;
    int32_t srcX1;
    int32_t srcY1;
    int32_t srcX2;
    int32_t srcY2;
    int32_t srcIndex0;
    int32_t srcIndex1;
    int32_t srcIndex2;
    int32_t srcIndex3;
    uint32_t nComps;
    float noiseBias;
    float gain;
    float reduction;
    uint32_t subtract;
    int32_t outX1;
    int32_t outY1;
    uint32_t outWidth;
    uint32_t outHeight;
    uint32_t display;
    float slopeShadows;
    float slopeHighlights;
    float poly0;
    float poly1;
    float poly2;
    float noiseBias0;
    uint32_t padding0;
};
static_assert(sizeof(ContrastTextureGpuParams) == 128);
#endif
} // namespace

ContrastTexture::ContrastTexture(NodePtr node)
    : StagedImageEffect(node)
{
}

ContrastTexture::~ContrastTexture()
{
}

const NodeSpec*
ContrastTexture::nodeSpec() const
{
    return staticNodeSpec<kContrastTextureNodeSpec>();
}

const ImagePipelineSpec&
ContrastTexture::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kContrastTexturePipeline, kContrastTextureNodeSpec>();
}

void
ContrastTexture::initializeKnobs()
{
    KnobPagePtr page = createControlsPage();

    KnobDoublePtr detailLevel = createKnob<KnobDouble>(tr("Base Detail Level"));
    detailLevel->setName(kContrastTextureParamDetailLevel);
    detailLevel->setHintToolTip(tr("The size of the coarsest detail, as a power of two below the image size: the coarsest blur's radius is "
                                   "the image's larger side over 2^(level + 1). It sets the size the shadows, highlights and coarse "
                                   "details work at.\nHigher puts more of the contrast boost in finer details, lower in coarser ones."));
    detailLevel->setDefaultValue(5.);
    detailLevel->setMinimum(1.);
    detailLevel->setMaximum(15.);
    detailLevel->setDisplayMinimum(1.);
    detailLevel->setDisplayMaximum(10.);
    page->addKnob(detailLevel);
    _detailLevel = detailLevel;

    KnobDoublePtr highlights = createKnob<KnobDouble>(tr("Highlights"));
    highlights->setName(kContrastTextureParamHighlights);
    highlights->setHintToolTip(tr("Brightens (above 0) or darkens (below 0) the highlights, measured on the coarsest blur: the log slope "
                                  "above scene grey is 2^highlights."));
    highlights->setDefaultValue(0.);
    highlights->setMinimum(-5.);
    highlights->setMaximum(5.);
    highlights->setDisplayMinimum(-2.);
    highlights->setDisplayMaximum(2.);
    page->addKnob(highlights);
    _highlights = highlights;

    KnobDoublePtr shadows = createKnob<KnobDouble>(tr("Shadows"));
    shadows->setName(kContrastTextureParamShadows);
    shadows->setHintToolTip(tr("Brightens (above 0) or darkens (below 0) the shadows, measured on the coarsest blur: the log slope "
                               "below scene grey is 2^-shadows."));
    shadows->setDefaultValue(0.);
    shadows->setMinimum(-5.);
    shadows->setMaximum(5.);
    shadows->setDisplayMinimum(-2.);
    shadows->setDisplayMaximum(2.);
    page->addKnob(shadows);
    _shadows = shadows;

    KnobGroupPtr localContrast = createKnob<KnobGroup>(tr("Local Contrast"));
    localContrast->setName(kContrastTextureParamLocalContrast);
    localContrast->setHintToolTip(tr("How much the detail of each size is boosted: 0 leaves it, 1 doubles it in log luminance, -1 removes it."));
    localContrast->setDefaultValue(true);
    page->addKnob(localContrast);

    const char* const bandNames[CT::eBandCount] = { kContrastTextureParamCoarse, kContrastTextureParamMedium, kContrastTextureParamFine };
    const QString bandLabels[CT::eBandCount] = { tr("Coarse Details"), tr("Medium Details"), tr("Fine Details") };
    const QString bandHints[CT::eBandCount] = {
        tr("Boosts the coarse, low frequency detail, around the base detail level."),
        tr("Boosts the medium detail, between the coarse and the fine."),
        tr("Boosts the fine, high frequency detail, down to a few pixels."),
    };
    KnobDoubleWPtr* bandKnobs[CT::eBandCount] = { &_coarse, &_medium, &_fine };
    for (int band = 0; band < CT::eBandCount; ++band) {
        KnobDoublePtr knob = createKnob<KnobDouble>(bandLabels[band]);
        knob->setName(bandNames[band]);
        knob->setHintToolTip(bandHints[band] + QLatin1Char('\n') + tr("Mask Display shows which detail it acts on."));
        knob->setDefaultValue(0.);
        knob->setMinimum(-1.);
        knob->setMaximum(5.);
        knob->setDisplayMinimum(-1.);
        knob->setDisplayMaximum(1.5);
        localContrast->addKnob(knob);
        *bandKnobs[band] = knob;
    }

    KnobGroupPtr filterSettings = createKnob<KnobGroup>(tr("Filter Settings"));
    filterSettings->setName(kContrastTextureParamFilterSettings);
    filterSettings->setHintToolTip(tr("The edge-aware filter the details are measured with."));
    filterSettings->setDefaultValue(true);
    page->addKnob(filterSettings);

    KnobDoublePtr haloControl = createKnob<KnobDouble>(tr("Halo Control"));
    haloControl->setName(kContrastTextureParamHaloControl);
    haloControl->setHintToolTip(tr("How strictly the filter keeps to edges.\nHigher suppresses halos at the expense of detail around "
                                   "edges, lower allows more halos for more local contrast and detail."));
    haloControl->setDefaultValue(0.);
    haloControl->setMinimum(-10.);
    haloControl->setMaximum(10.);
    haloControl->setDisplayMinimum(-5.);
    haloControl->setDisplayMaximum(5.);
    filterSettings->addKnob(haloControl);
    _haloControl = haloControl;

    KnobDoublePtr noiseBias = createKnob<KnobDouble>(tr("Noise Bias"));
    noiseBias->setName(kContrastTextureParamNoiseBias);
    noiseBias->setHintToolTip(tr("A luminance offset that keeps noise in the shadows from being amplified. Only affects the dark parts "
                                 "of the image."));
    noiseBias->setDefaultValue(0.001);
    noiseBias->setMinimum(0.);
    noiseBias->setMaximum(1.);
    noiseBias->setDisplayMinimum(0.);
    noiseBias->setDisplayMaximum(0.2);
    filterSettings->addKnob(noiseBias);
    _noiseBias = noiseBias;

    KnobChoicePtr maskDisplay = createKnob<KnobChoice>(tr("Mask Display"));
    maskDisplay->setName(kContrastTextureParamMaskDisplay);
    maskDisplay->setHintToolTip(tr("Outputs a preview instead of the image, to set up the controls. Turn it off before rendering."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kContrastTextureMaskDisplayOff, tr("Off").toStdString(), tr("The image.").toStdString()));
        options.push_back(ChoiceOption(kContrastTextureMaskDisplayCoarse, tr("Coarse Details").toStdString(),
                                       tr("The detail Coarse Details acts on, at full strength: grey where there is none, brighter "
                                          "where a pixel is brighter than its surroundings, darker where it is darker.")
                                           .toStdString()));
        options.push_back(ChoiceOption(kContrastTextureMaskDisplayMedium, tr("Medium Details").toStdString(),
                                       tr("The detail Medium Details acts on, shown as for Coarse Details.").toStdString()));
        options.push_back(ChoiceOption(kContrastTextureMaskDisplayFine, tr("Fine Details").toStdString(),
                                       tr("The detail Fine Details acts on, shown as for Coarse Details.").toStdString()));
        options.push_back(ChoiceOption(kContrastTextureMaskDisplayLowpass, tr("Low-Pass").toStdString(),
                                       tr("The coarsest blur Shadows and Highlights read, tinted blue in the shadows and yellow in the "
                                          "highlights.")
                                           .toStdString()));
        maskDisplay->populateChoices(options);
    }
    maskDisplay->setDefaultValue((int)CT::eDisplayOff);
    page->addKnob(maskDisplay);
    _maskDisplay = maskDisplay;
} // ContrastTexture::initializeKnobs

CT::Params
ContrastTexture::readParams(double time,
                            ViewIdx view) const
{
    const KnobReader reader(time, view);
    CT::Params p;

    p.gainCoarse = (float)reader.value(_coarse);
    p.gainMedium = (float)reader.value(_medium);
    p.gainFine = (float)reader.value(_fine);
    p.detailLevel = (float)reader.value(_detailLevel);
    p.haloControl = (float)reader.value(_haloControl);
    p.noiseBias = (float)reader.value(_noiseBias);
    p.gainShadows = (float)reader.value(_shadows);
    p.gainHighlights = (float)reader.value(_highlights);
    const int display = reader.value(_maskDisplay);
    p.display = ((display >= (int)CT::eDisplayOff) && (display <= (int)CT::eDisplayLowpass)) ? (CT::DisplayEnum)display : CT::eDisplayOff;

    return p;
}

bool
ContrastTexture::sourceRoD(double time,
                           ViewIdx view,
                           RectD* rod) const
{
    const int sourceInput = getSourceInput();
    EffectInstancePtr input = getInput(sourceInput);

    if (!input) {
        return false;
    }
    bool isProjectFormat = false;
    if (input->getRegionOfDefinition_public(input->getRenderHash(), time, RenderScale::identity, view, rod, &isProjectFormat) == eStatusFailed) {
        return false;
    }
    if (rod->isInfinite()) {
        const NodeMetadataConstPtr metadata = getRenderMetadata(time, view);
        if (!metadata) {
            return false;
        }
        *rod = rod->intersect(PassExec::toCanonical(metadata->getOutputFormat(), RenderScale::identity, metadata->getPixelAspectRatio(sourceInput)));
    }

    return !rod->isNull();
}

bool
ContrastTexture::makePlan(double time,
                          ViewIdx view,
                          const RenderScale& scale,
                          RectI* sourcePixels,
                          CT::Plan* plan) const
{
    RectD rod;

    if (!sourceRoD(time, view, &rod)) {
        return false;
    }
    const NodeMetadataConstPtr metadata = getRenderMetadata(time, view);
    const double par = metadata ? metadata->getPixelAspectRatio(getSourceInput()) : 1.;
    const RectI full = PassExec::toPixelEnclosing(rod, RenderScale::identity, par);
    *sourcePixels = PassExec::toPixelEnclosing(rod, scale, par);
    if (full.isNull() || sourcePixels->isNull()) {
        return false;
    }
    const float fullSize = (float)std::max(full.width(), full.height());
    *plan = CT::makePlan(readParams(time, view), fullSize, (float)scale.toOfxPointD().x);

    return true;
}

void
ContrastTexture::getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret)
{
    StagedImageEffect::getRegionsOfInterest(time, scale, outputRoD, renderWindow, view, ret);

    EffectInstancePtr input = getInput(getSourceInput());
    RectD rod;
    if (input && sourceRoD(time, view, &rod)) {
        (*ret)[input] = rod;
    }
}

bool
ContrastTexture::isOpIdentity(double time,
                              const RenderScale& /*scale*/,
                              const RectI& /*roi*/,
                              ViewIdx view)
{
    return CT::isIdentity(readParams(time, view));
}

StatusEnum
ContrastTexture::renderOp(const RenderActionArgs& args,
                          const StagePlan& plan)
{
    const int sourceInput = getSourceInput();
    CT::Plan ctPlan;
    RectI full;

    if (!makePlan(args.time, args.view, args.mappedScale, &full, &ctPlan)) {
        return eStatusFailed;
    }

    ImagePtr src;
    ImageLayerDesc sourceLayer;
    if (resolveInputPlaneForRender(sourceInput, args.time, args.view, &sourceLayer, NULL)) {
        RectI sourceRoI;
        // Mapped to the clip's components, the layout the output plane is rendered in.
        src = getImage(sourceInput, args.time, args.mappedScale, args.view, NULL, &sourceLayer, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &sourceRoI);
    }
    if (src && !PassExec::isFloatImage(src)) {
        return eStatusFailed;
    }
    std::shared_ptr<Image::ReadAccess> srcAccess;
    RectI srcBounds;
    int srcNComps = 0;
    if (src) {
        srcAccess = std::make_shared<Image::ReadAccess>(src.get());
        srcBounds = src->getBounds();
        srcNComps = (int)src->getComponentsCount();
    }

    RenderCancellation cancel(this);
    const int nThreads = appPTR->getNCPUsAvailableForEffect();
    const EdgeAware::ParallelFor parallel = [&cancel, nThreads](int count, const std::function<void(int)>& body) {
        parallelForCancellable(count, nThreads, cancel, body);
    };
    const int width = full.width();
    const int height = full.height();

    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        if (!it->second) {
            continue;
        }
        const int nComps = (int)it->second->getComponentsCount();

        // The whole source, top-down, in the output plane's layout.
        std::vector<float> image((std::size_t)width * height * nComps, 0.f);
        parallel(height, [&](int r) {
            const int y = full.y2 - 1 - r;
            PassExec::readSourceRow(src.get(), srcAccess.get(), srcBounds, srcNComps, full.x1, y, width, nComps, &image[(std::size_t)r * width * nComps]);
        });
        std::vector<float> map((std::size_t)width * height);
        CT::computeMap(image.data(), width, height, nComps, ctPlan, map.data(), parallel);
        image.clear();
        image.shrink_to_fit();
        if (cancel.check()) {
            return eStatusFailed;
        }

        const auto row = [&](const RowIO& io) {
            float pixel[4] = { 0.f, 0.f, 0.f, 0.f };
            const bool insideRow = (io.y >= full.y1) && (io.y < full.y2);
            const std::size_t rowStart = insideRow ? (std::size_t)(full.y2 - 1 - io.y) * width : 0;
            for (int i = 0; i < io.width; ++i) {
                const int x = io.x0 + i;
                const float* in = io.src[0] ? &io.src[0][(std::size_t)i * io.nComps] : pixel;
                float out[4] = { 0.f, 0.f, 0.f, 0.f };
                if (insideRow && (x >= full.x1) && (x < full.x2)) {
                    CT::writePixel(in, map[rowStart + (x - full.x1)], ctPlan, io.nComps, out);
                } else {
                    std::copy(in, in + io.nComps, out);
                }
                for (int c = 0; c < io.nComps; ++c) {
                    if (io.channels[pixelKernelChannelBit(io.nComps, c)]) {
                        io.dst[(std::size_t)i * io.nComps + c] = out[c];
                    }
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

    return cancel.check() ? eStatusFailed : eStatusOK;
} // ContrastTexture::renderOp

bool
ContrastTexture::canRenderOpOnGpu(const GpuQuery& query) const
{
#ifdef NATRON_HAVE_GPU
    if ((query.bitDepth != eImageBitDepthFloat) || (query.nComps < 1) || (query.nComps > 4)) {
        return false;
    }

    return canRunStagesGpu(resolveGpuPlan(query), query);
#else
    Q_UNUSED(query);

    return false;
#endif
}

bool
ContrastTexture::getOpGpuInputLayer(int inputNb,
                                    const ImageLayerDesc& plane,
                                    double time,
                                    ViewIdx view,
                                    ImageLayerDesc* layer) const
{
    if (inputNb != getSourceInput()) {
        stageGpuInputLayer(*this, inputNb, plane, time, view, layer);

        return true;
    }

    return const_cast<ContrastTexture*>(this)->resolveInputPlaneForRender(inputNb, time, view, layer, NULL);
}

RectI
ContrastTexture::getOpGpuInputRoI(int inputNb,
                                  double time,
                                  ViewIdx view,
                                  const RectI& outputRect,
                                  const RenderScale& scale) const
{
    if (inputNb != getSourceInput()) {
        return outputRect;
    }
    CT::Plan plan;
    RectI full;
    if (!makePlan(time, view, scale, &full, &plan)) {
        return outputRect;
    }

    return full;
}

StatusEnum
ContrastTexture::renderOpGpu(const GpuRenderArgs& args)
{
#ifdef NATRON_HAVE_GPU
    const GpuQuery& query = args.query;
    const RectI& rect = args.outputRect;
    const int nComps = query.nComps;
    if (!canRenderOpOnGpu(query) || rect.isNull() || !PassExec::isUsableGpuView(args.output, nComps) || !args.output.bounds.contains(rect)) {
        return eStatusFailed;
    }
    CT::Plan ctPlan;
    RectI full;
    if (!makePlan(query.time, query.view, query.mappedScale, &full, &ctPlan)) {
        return eStatusFailed;
    }
    const int sourceInput = getSourceInput();
    GpuImageView source;
    if (!stageGpuInputView(*this, args, sourceInput, &source) || (source.buffer && ((source.byteOffset % sizeof(float)) != 0))) {
        return eStatusFailed;
    }
    const StagePlan plan = resolveGpuPlan(query);

    const uint64_t pixels = (uint64_t)full.width() * (uint64_t)full.height();
    const uint64_t filteredFloats = (uint64_t)rect.area() * (uint64_t)nComps;
    const uint64_t srcEnd = source.buffer ? (source.byteOffset / sizeof(float) + (uint64_t)source.bounds.height() * (uint64_t)source.rowStrideFloats) : 0;
    if ((pixels > UINT32_MAX) || (filteredFloats > UINT32_MAX) || (srcEnd > UINT32_MAX)) {
        return eStatusFailed;
    }

    gpu::GpuRecordContext& ctx = args.context;
    gpu::GpuBuffer* lowpass = nullptr;
    gpu::GpuBuffer* highpass = nullptr;
    gpu::GpuBuffer* corrections = nullptr;
    gpu::GpuBuffer* filteredBuffer = nullptr;
    gpu::GpuBuffer* standIn = nullptr;
    if (!ctx.scratch(pixels * sizeof(float), lowpass) || !lowpass || !ctx.scratch(pixels * sizeof(float), highpass) || !highpass
        || !ctx.scratch(pixels * sizeof(float), corrections) || !corrections || !ctx.scratch(filteredFloats * sizeof(float), filteredBuffer) || !filteredBuffer) {
        return eStatusFailed;
    }
    const gpu::GpuBuffer* srcBuffer = source.buffer;
    if (!srcBuffer) {
        if (!ctx.scratch(4 * sizeof(float), standIn) || !standIn) {
            return eStatusFailed;
        }
        srcBuffer = standIn;
    }

    ContrastTextureGpuParams p = {};
    p.width = (uint32_t)full.width();
    p.height = (uint32_t)full.height();
    p.fullX1 = full.x1;
    p.fullY2 = full.y2;
    if (source.buffer) {
        p.srcOffset = (uint32_t)(source.byteOffset / sizeof(float));
        p.srcStride = (uint32_t)source.rowStrideFloats;
        p.srcComps = (uint32_t)source.nComps;
        p.srcX1 = source.bounds.x1;
        p.srcY1 = source.bounds.y1;
        p.srcX2 = source.bounds.x2;
        p.srcY2 = source.bounds.y2;
    }
    // The source read in the plane's layout, as readSourceRow() reads it on the CPU.
    int srcIndex[4] = { -1, -1, -1, -1 };
    for (int c = 0; c < nComps; ++c) {
        srcIndex[c] = source.buffer ? PassExec::channelIndexForBit(source.nComps, pixelKernelChannelBit(nComps, c)) : -1;
    }
    p.srcIndex0 = srcIndex[0];
    p.srcIndex1 = srcIndex[1];
    p.srcIndex2 = srcIndex[2];
    p.srcIndex3 = srcIndex[3];
    p.nComps = (uint32_t)nComps;
    p.outX1 = rect.x1;
    p.outY1 = rect.y1;
    p.outWidth = (uint32_t)rect.width();
    p.outHeight = (uint32_t)rect.height();
    p.display = (uint32_t)ctPlan.display;
    p.slopeShadows = ctPlan.slopeShadows;
    p.slopeHighlights = ctPlan.slopeHighlights;
    p.poly0 = ctPlan.polynomial[0];
    p.poly1 = ctPlan.polynomial[1];
    p.poly2 = ctPlan.polynomial[2];
    p.noiseBias0 = ctPlan.noiseBias[0];

    const auto run = [&](gpu::GpuKernelId kernel, const ContrastTextureGpuParams& params, uint32_t x, uint32_t y) {
        if (!ctx.dispatch(kernel, { srcBuffer, lowpass, highpass, corrections, filteredBuffer }, params, { std::max<uint32_t>(x, 1), std::max<uint32_t>(y, 1), 1 })) {
            return false;
        }
        ctx.computeBarrier();

        return true;
    };

    p.noiseBias = ctPlan.noiseBias[ctPlan.maxUsedLevel];
    if (!run(gpu::GpuKernelId::contrastntexture_luminance, p, p.width, p.height)) {
        return eStatusFailed;
    }
    for (int level = ctPlan.maxUsedLevel; level >= 0; --level) {
        if (!EdgeAware::recordEigfSurfaceBlur(ctx, *lowpass, full.width(), full.height(), ctPlan.radius[level], ctPlan.feathering, 1, EdgeAware::eGuidedBlendingLinear, 0.0f, CT::kNormMin, 4.0f)) {
            return eStatusFailed;
        }
        ContrastTextureGpuParams lp = p;
        lp.gain = ctPlan.gainPerLevel[level];
        lp.noiseBias = ctPlan.noiseBias[level];
        lp.subtract = (level > 0) ? 1u : 0u;
        lp.reduction = (level > 0) ? (ctPlan.noiseBias[level] - ctPlan.noiseBias[level - 1]) : 0.f;
        if (!run(gpu::GpuKernelId::contrastntexture_details, lp, p.width, p.height)) {
            return eStatusFailed;
        }
    }
    if (!run(gpu::GpuKernelId::contrastntexture_apply, p, p.outWidth, p.outHeight)) {
        return eStatusFailed;
    }

    GpuImageView filtered;
    filtered.buffer = filteredBuffer;
    filtered.bounds = rect;
    filtered.nComps = nComps;
    filtered.rowStrideFloats = rect.width() * nComps;
    // The stages blend with the source as the CPU fetches it, cut to its region of definition.
    GpuRenderArgs finishArgs(args);
    if ((sourceInput >= 0) && (sourceInput < (int)finishArgs.inputs.size())) {
        finishArgs.inputs[sourceInput] = source;
    }

    return recordStagesGpuFinish(plan, finishArgs, filtered);
#else
    Q_UNUSED(args);

    return eStatusFailed;
#endif
} // ContrastTexture::renderOpGpu

NATRON_NAMESPACE_EXIT
