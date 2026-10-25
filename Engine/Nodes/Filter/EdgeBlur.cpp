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

#include "EdgeBlur.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Filter/BlurKernels.h"
#include "Engine/Nodes/Filter/MapInput.h"
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/Project.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuBlur.h"
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
constexpr InputSpec kEdgeBlurInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
    InputSpec { .label = kEdgeBlurInputMatteLabel, .role = InputRole::Aux, .optional = true },
};

constexpr NodeSpec kEdgeBlurNodeSpec = {
    .id = PLUGINID_NATRON_EDGEBLUR,
    .label = "EdgeBlur",
    .description = "Blur the image where there are edges in the alpha/matte channel.\n"
                   "\n"
                   "The edges are the gradient magnitude of the matte, as EdgeDetect computes it with the Filter, pre-blurred by the Size: a sharp step of the matte from 0 to 1 "
                   "gives an edge strength of 1. The image blurred by a Gaussian of the Size is mixed in by the edge strength raised to 1/Edge Mult, clamped to [0, 1], "
                   "so a larger Edge Mult widens the blurred band. The matte is the alpha of the source, or with External Matte the alpha of the Matte input; "
                   "with External Matte on and no Matte connected the image is left unchanged. The Mask and Mix limit the result as usual.",
    .grouping = PLUGIN_GROUP_FILTER,
    .majorVersion = PLUGIN_MAJOR_NATRON_EDGEBLUR,
    .icon = NATRON_IMAGES_PATH "NativeNodes/net.sf.cimg.CImgBlur.png",
    .inputs = kEdgeBlurInputs,
};

constexpr ImagePipelineSpec kEdgeBlurPipeline = {
    .stages = { Stage::MaskMix, Stage::PassThroughUnprocessed },
    .layers = LayerPolicy::color(true, true, true, true),
    .op = OpKind::Passes,
};

// The slot the blur runs in and the mix writes; slot 0 keeps the source.
const int kSlotSource = 0;
const int kSlotResult = 1;

// The alpha channel, in the bit space of pixelKernelChannelBit().
const int kAlphaBit = 3;

using RowWorker = std::function<PassRowFn(const PassScratch&)>;

/// The source copied over every element of the result slot, which the blur then filters in place.
RowWorker
copyWorker()
{
    return [](const PassScratch& scratch) -> PassRowFn {
        const PassScratch* s = &scratch;

        return [s](int line) {
            const int p = line / s->height;
            const int row = line % s->height;
            const std::size_t offset = (std::size_t)row * s->width;
            const float* source = s->plane(kSlotSource, p) + offset;

            std::copy(source, source + s->width, s->plane(kSlotResult, p) + offset);
        };
    };
}

/// The blurred result mixed with the source by the edge weight, in place; the source exactly
/// where the weight is 0.
RowWorker
mixWorker(const std::shared_ptr<const MapPlane>& edge,
          float exponent)
{
    return [edge, exponent](const PassScratch& scratch) -> PassRowFn {
        const PassScratch* s = &scratch;

        return [s, edge, exponent](int line) {
            const int p = line / s->height;
            const int row = line % s->height;
            const std::size_t offset = (std::size_t)row * s->width;
            const float* source = s->plane(kSlotSource, p) + offset;
            float* out = s->plane(kSlotResult, p) + offset;
            const int y = s->rect.y1 + row;
            for (int i = 0; i < s->width; ++i) {
                const float w = EdgeBlur::edgeWeight(edge->at(s->rect.x1 + i, y), exponent);
                out[i] = (w > 0.f) ? (source[i] + (out[i] - source[i]) * w) : source[i];
            }
        };
    };
}

/**
 * @brief The passes of one render: the source copied into the result slot, the blur there, the
 * reach of the edge detection when the matte is the source (so that the source is read over
 * EdgeDetect's buffer), and the mix by edge. Without edge, the passes have no work and only carry
 * the halo and growth.
 **/
PassList
mixPasses(const PassList& blur,
          const PassList& edges,
          bool carryEdges,
          const std::shared_ptr<const MapPlane>& edge,
          float exponent)
{
    PassList list;

    list.boundary = blur.boundary;
    list.scratchSlots = 2;
    list.resultSlot = kSlotResult;

    Pass copy = Pass::point(true, edge ? copyWorker() : RowWorker());
    copy.slot = kSlotResult;
    list.passes.push_back(copy);
    for (Pass pass : blur.passes) {
        pass.slot = kSlotResult;
        list.passes.push_back(pass);
    }
    if (carryEdges) {
        const PassExtent reach = passListExtent(edges);
        Pass carrier = Pass::gather(reach.haloX, reach.haloY, true, RowWorker());
        carrier.growX = 0;
        carrier.growY = 0;
        carrier.slot = kSlotResult;
        list.passes.push_back(carrier);
    }
    Pass mix = Pass::point(true, edge ? mixWorker(edge, exponent) : RowWorker());
    mix.slot = kSlotResult;
    list.passes.push_back(mix);

    return list;
}

/// The format of source, else the project's.
RectI
formatOf(const AppInstancePtr& app,
         const EffectInstancePtr& source,
         double time)
{
    EffectInstancePtr formatSource = source ? source->getNearestNonIdentity(time) : EffectInstancePtr();
    if (formatSource) {
        return formatSource->getOutputFormat();
    }
    Format projectFormat;
    if (app) {
        app->getProject()->getProjectDefaultFormat(&projectFormat);
    }

    return projectFormat;
}

/// Clamps back to format, a canonical rect, each border of rod that srcRoD had inside it.
bool
cropToFormatBorders(const RectI& format,
                    const RenderScale& scale,
                    const RectI& srcRoD,
                    RectI* rod)
{
    const OfxPointD rs = scale.toOfxPointD();
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
    if (PassExec::isEmptyRect(inner)) {
        return false;
    }
    bool changed = false;
    if ((rod->x1 < inner.x1) && (srcRoD.x1 >= enclosing.x1)) {
        rod->x1 = inner.x1;
        changed = true;
    }
    if ((rod->x2 > inner.x2) && (srcRoD.x2 <= enclosing.x2)) {
        rod->x2 = inner.x2;
        changed = true;
    }
    if ((rod->y1 < inner.y1) && (srcRoD.y1 >= enclosing.y1)) {
        rod->y1 = inner.y1;
        changed = true;
    }
    if ((rod->y2 > inner.y2) && (srcRoD.y2 <= enclosing.y2)) {
        rod->y2 = inner.y2;
        changed = true;
    }

    return changed;
}

#ifdef NATRON_HAVE_GPU
// Mirrors EdgeBlurParams in Kernels/edgeblur.slang, which has no padding between members.
struct EdgeBlurGpuParams {
    int32_t outX1;
    int32_t outY1;
    uint32_t outWidth;
    uint32_t outHeight;
    uint32_t dstOffset;
    uint32_t dstStride;
    uint32_t channels;
    uint32_t processed;
    int32_t srcX1;
    int32_t srcY1;
    int32_t srcX2;
    int32_t srcY2;
    uint32_t srcOffset;
    uint32_t srcStride;
    uint32_t edgeChannels;
    uint32_t edgeComp;
    uint32_t hasEdge;
    float exponent;
};
static_assert(sizeof(EdgeBlurGpuParams) == 72);
static_assert(offsetof(EdgeBlurGpuParams, srcX1) == 32);
static_assert(offsetof(EdgeBlurGpuParams, edgeChannels) == 56);

// The float index of pixel (x, y) of view, or false when the view exceeds the kernel's 32-bit
// indices.
bool
gpuFloatIndex(const GpuImageView& view,
              int x,
              int y,
              uint32_t* out)
{
    const gpu::GpuBlurImage image = PassExec::toGpuBlurImage(view);
    const uint64_t index = image.offsetFloats + (uint64_t)(y - image.bounds.y1) * image.strideFloats + (uint64_t)(x - image.bounds.x1) * view.nComps;
    const uint64_t end = image.offsetFloats + (uint64_t)(image.bounds.y2 - image.bounds.y1) * image.strideFloats;
    if ((index > UINT32_MAX) || (end > UINT32_MAX)) {
        return false;
    }
    *out = (uint32_t)index;

    return true;
}

// A tightly packed view of buffer over rect, nComps floats per pixel.
GpuImageView
packedView(gpu::GpuBuffer* buffer,
           const RectI& rect,
           int nComps)
{
    GpuImageView view;

    view.buffer = buffer;
    view.byteOffset = 0;
    view.bounds = rect;
    view.rowStrideFloats = rect.width() * nComps;
    view.nComps = nComps;

    return view;
}
#endif // NATRON_HAVE_GPU
} // anonymous namespace

EdgeBlur::EdgeBlur(NodePtr node)
    : StagedImageEffect(node)
{
}

EdgeBlur::~EdgeBlur()
{
}

const NodeSpec*
EdgeBlur::nodeSpec() const
{
    return staticNodeSpec<kEdgeBlurNodeSpec>();
}

const ImagePipelineSpec&
EdgeBlur::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kEdgeBlurPipeline, kEdgeBlurNodeSpec>();
}

std::string
EdgeBlur::getInputHint(int inputNb) const
{
    switch (inputNb) {
    case kEdgeBlurInputSource:
        return tr("The image to blur, whose alpha is the matte unless External Matte is on.").toStdString();
    case kEdgeBlurInputMask:
        return tr("The mask the blurred result is mixed with the source through.").toStdString();
    case kEdgeBlurInputMatte:
        return tr("The matte whose alpha edges are blurred when External Matte is on.").toStdString();
    }

    return std::string();
}

void
EdgeBlur::initializeKnobs()
{
    const KnobPagePtr page = createControlsPage();

    KnobBoolPtr externalMatteKnob = AppManager::createKnob<KnobBool>(this, tr("External Matte"));
    externalMatteKnob->setName(kEdgeBlurParamExternalMatte);
    externalMatteKnob->setHintToolTip(tr("Use the edges from the Matte input instead of the alpha channel of the source image."));
    externalMatteKnob->setDefaultValue(false);
    page->addKnob(externalMatteKnob);
    _externalMatte = externalMatteKnob;

    KnobDoublePtr sizeKnob = AppManager::createKnob<KnobDouble>(this, tr("Size"));
    sizeKnob->setName(kEdgeBlurParamSize);
    sizeKnob->setHintToolTip(tr("Size (diameter) of the blur, and of the edge detection's pre-blur, in pixel units (>=0). The standard deviation of the Gaussian is size/2.4."));
    sizeKnob->setSpatial(true);
    sizeKnob->setMinimum(0.);
    sizeKnob->setDisplayMinimum(0.);
    sizeKnob->setDisplayMaximum(100.);
    sizeKnob->setDefaultValue(3.);
    page->addKnob(sizeKnob);
    _size = sizeKnob;

    KnobChoicePtr filterKnob = AppManager::createKnob<KnobChoice>(this, tr("Filter"));
    filterKnob->setName(kEdgeBlurParamFilter);
    filterKnob->setHintToolTip(tr("The edge detection filter, as EdgeDetect's: the Size is the kernel size of the quasi-Gaussian, Gaussian, box, triangle and quadratic filters, and the size of the Gaussian pre-blur of the simple, rotation-invariant and Sobel filters. The blur itself is always a Gaussian."));
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
        filterKnob->populateChoices(options);
    }
    filterKnob->setDefaultValue((int)eEdgeDetectFilterGaussian);
    filterKnob->setAnimationEnabled(false);
    filterKnob->setAddNewLine(false);
    page->addKnob(filterKnob);
    _filter = filterKnob;

    KnobBoolPtr cropToFormatKnob = AppManager::createKnob<KnobBool>(this, tr("Crop To Format"));
    cropToFormatKnob->setName(kEdgeBlurParamCropToFormat);
    cropToFormatKnob->setHintToolTip(tr("If the source is inside the format and the effect extends it outside of the format, crop it to avoid unnecessary calculations. To avoid unwanted crops, only the borders that were inside of the format in the source clip will be cropped."));
    cropToFormatKnob->setDefaultValue(true);
    cropToFormatKnob->setAnimationEnabled(false);
    page->addKnob(cropToFormatKnob);
    _cropToFormat = cropToFormatKnob;

    KnobDoublePtr edgeMultKnob = AppManager::createKnob<KnobDouble>(this, tr("Edge Mult"));
    edgeMultKnob->setName(kEdgeBlurParamEdgeMult);
    edgeMultKnob->setHintToolTip(tr("Sharpness of the borders of the blur area: the edge strength is raised to 1/Edge Mult before it mixes the blur in, so a larger value widens the blurred band."));
    edgeMultKnob->setMinimum(0.);
    edgeMultKnob->setDisplayMinimum(0.1);
    edgeMultKnob->setDisplayMaximum(10.);
    edgeMultKnob->setDefaultValue(2.);
    page->addKnob(edgeMultKnob);
    _edgeMult = edgeMultKnob;

    addStageKnobs(page);
} // EdgeBlur::initializeKnobs

EdgeBlurParams
EdgeBlur::getParams(double time,
                    ViewIdx view) const
{
    const KnobReader reader(time, view);
    EdgeBlurParams params;

    params.externalMatte = reader.value(_externalMatte);
    params.size = reader.value(_size);
    const int f = reader.value(_filter);
    if ((f >= (int)eEdgeDetectFilterSimple) && (f <= (int)eEdgeDetectFilterQuadratic)) {
        params.filter = (EdgeDetectFilterEnum)f;
    }
    params.cropToFormat = reader.value(_cropToFormat);
    params.edgeMult = reader.value(_edgeMult);

    return params;
}

PassList
EdgeBlur::blurPasses(const EdgeBlurParams& params,
                     const PassContext& context)
{
    BlurParams blur;

    blur.sizeX = params.size;
    blur.sizeY = params.size;
    blur.filter = BlurKernels::eFilterGaussian;
    blur.cropToFormat = params.cropToFormat;
    const BlurOp op;
    if (op.isIdentity(blur, context)) {
        return PassList();
    }

    return op.passes(blur, context);
}

PassList
EdgeBlur::edgePasses(const EdgeBlurParams& params,
                     const PassContext& context)
{
    EdgeDetectParams edges;

    edges.sizeX = params.size;
    edges.sizeY = params.size;
    edges.erodeSize = 0.;
    edges.filter = params.filter;
    edges.multiChannel = eEdgeDetectMultiChannelSeparate;
    edges.nms = false;
    edges.expandRoD = true;
    edges.cropToFormat = true;

    return EdgeDetectOp().passes(edges, context);
}

float
EdgeBlur::edgeWeight(float edge,
                     float exponent)
{
    if (!(edge > 0.f)) {
        return 0.f;
    }

    return std::min(1.f, std::pow(edge, exponent));
}

float
EdgeBlur::edgeExponent(double edgeMult)
{
    return (float)(1. / std::max(1e-8, edgeMult));
}

int
EdgeBlur::matteInput(const EdgeBlurParams& params) const
{
    return params.externalMatte ? kEdgeBlurInputMatte : kEdgeBlurInputSource;
}

PassList
EdgeBlur::opPasses(double time,
                   ViewIdx view,
                   const RenderScale& scale) const
{
    const EdgeBlurParams params = getParams(time, view);
    const PassContext context = getPassContext(scale);
    const PassList blur = blurPasses(params, context);

    if (blur.empty() || !getInput(matteInput(params))) {
        return PassList();
    }
    const PassList edges = edgePasses(params, context);
    PassList list = mixPasses(blur, edges, !params.externalMatte, std::shared_ptr<const MapPlane>(), 0.f);

#ifdef NATRON_HAVE_GPU
    if (blur.gpu && edges.gpu) {
        const EdgeBlur* self = this;
        list.gpu = [self, time, view, scale, params, blur, edges](const PassGpuIO& io) -> StatusEnum {
            return self->recordGpu(io, time, view, scale, params, blur, edges);
        };
    }
#endif

    return list;
}

bool
EdgeBlur::edgeRects(const EdgeBlurParams& params,
                    const PassList& edges,
                    double time,
                    ViewIdx view,
                    const RenderScale& scale,
                    const RectI& output,
                    RectI* matteRoD,
                    RectI* edgeRoD,
                    RectI* buffer) const
{
    const int input = matteInput(params);
    EffectInstancePtr matte = getInput(input);
    if (!matte) {
        return false;
    }
    RectD rod;
    bool isProjectFormat = false;
    if (matte->getRegionOfDefinition_public(matte->getRenderHash(), time, scale, view, &rod, &isProjectFormat) == eStatusFailed) {
        return false;
    }
    *matteRoD = PassExec::toPixelEnclosing(rod, scale, getAspectRatio(input));
    if (PassExec::isEmptyRect(*matteRoD)) {
        return false;
    }
    // EdgeDetect's own region of definition, cropped to the format as its Crop To Format does.
    *edgeRoD = passListRoD(*matteRoD, edges);
    cropToFormatBorders(formatOf(getApp(), matte, time), scale, *matteRoD, edgeRoD);

    return PassExec::intersectRects(passListSourceRoI(output, edges), *edgeRoD, buffer);
}

StatusEnum
EdgeBlur::computeEdges(const RenderActionArgs& args,
                       const EdgeBlurParams& params,
                       const PassList& edges,
                       MapPlane* plane)
{
    plane->reset(args.roi, 0.f);
    RectI matteRoD;
    RectI edgeRoD;
    RectI buffer;
    if (!edgeRects(params, edges, args.time, args.view, args.mappedScale, args.roi, &matteRoD, &edgeRoD, &buffer)) {
        return eStatusOK;
    }
    if (!isRunnablePassList(edges)) {
        return eStatusFailed;
    }
    const int input = matteInput(params);
    ImageLayerDesc layer;
    if (!resolveInputPlaneForRender(input, args.time, args.view, &layer, NULL)) {
        return eStatusOK;
    }
    RectI roiPixel;
    // Read in the layout of the plane being rendered, as the source is.
    const ImagePtr image = getImage(input, args.time, args.mappedScale, args.view, NULL, &layer, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &roiPixel);
    if (!image) {
        return eStatusOK;
    }
    if (!PassExec::isFloatImage(image)) {
        return eStatusFailed;
    }
    const int nComps = (int)image->getComponentsCount();
    const int channel = PassExec::channelIndexForBit(nComps, kAlphaBit);
    if (channel < 0) {
        return eStatusOK;
    }

    const int width = buffer.width();
    const int height = buffer.height();
    const std::size_t planeSize = (std::size_t)width * height;
    std::vector<std::vector<float>> storage((std::size_t)edges.scratchSlots, std::vector<float>(planeSize, 0.f));
    PassScratch scratch;
    scratch.rect = buffer;
    scratch.width = width;
    scratch.height = height;
    scratch.nPlanes = 1;
    scratch.planeSize = planeSize;
    for (std::vector<float>& slot : storage) {
        scratch.buffers.push_back(slot.data());
    }
    scratch.nComps = 1;
    scratch.channelBits.push_back(kAlphaBit);

    // Slot 0 is the matte's alpha over the buffer, beyond its image by the passes' boundary, as
    // fillBuffer() reads EdgeDetect's source. Read on this thread, which holds the image's lock.
    RectI valid;
    if (PassExec::intersectRects(image->getBounds(), matteRoD, &valid)) {
        const bool nearest = (edges.boundary == PassExec::Boundary::Nearest);
        Image::ReadAccess access(image.get());
        float* const slot0 = storage[0].data();
        for (int row = 0; row < height; ++row) {
            float* out = slot0 + (std::size_t)row * width;
            int y = buffer.y1 + row;
            if ((y < valid.y1) || (y >= valid.y2)) {
                if (!nearest) {
                    continue;
                }
                y = std::min(std::max(y, valid.y1), valid.y2 - 1);
            }
            const float* srcRow = (const float*)access.pixelAt(valid.x1, y);
            if (!srcRow) {
                continue;
            }
            for (int i = 0; i < width; ++i) {
                int x = buffer.x1 + i;
                if ((x < valid.x1) || (x >= valid.x2)) {
                    if (!nearest) {
                        continue;
                    }
                    x = std::min(std::max(x, valid.x1), valid.x2 - 1);
                }
                out[i] = srcRow[(std::size_t)(x - valid.x1) * nComps + channel];
            }
        }
    }

    RenderCancellation cancel(this);
    runPassListOnScratch(edges, scratch, appPTR->getNCPUsAvailableForEffect(), cancel);

    RectI inside;
    if (PassExec::intersectRects(args.roi, buffer, &inside)) {
        const float* result = storage[(std::size_t)edges.resultSlot].data();
        for (int y = inside.y1; y < inside.y2; ++y) {
            const float* from = result + (std::size_t)(y - buffer.y1) * width + (inside.x1 - buffer.x1);
            std::copy(from, from + inside.width(), plane->row(y) + (inside.x1 - args.roi.x1));
        }
    }

    return eStatusOK;
} // EdgeBlur::computeEdges

StatusEnum
EdgeBlur::renderOp(const RenderActionArgs& args,
                   const StagePlan& plan)
{
    const PassList full = opPasses(args.time, args.view, args.mappedScale);

    if (full.empty()) {
        return runPassesCpu(*this, plan, args, full);
    }
    const EdgeBlurParams params = getParams(args.time, args.view);
    const PassContext context = getPassContext(args.mappedScale);
    const PassList edges = edgePasses(params, context);
    std::shared_ptr<MapPlane> edge = std::make_shared<MapPlane>();
    const StatusEnum status = computeEdges(args, params, edges, edge.get());
    if (status != eStatusOK) {
        return status;
    }

    return runPassesCpu(*this, plan, args, mixPasses(blurPasses(params, context), edges, !params.externalMatte, edge, edgeExponent(params.edgeMult)));
}

StatusEnum
EdgeBlur::recordGpu(const PassGpuIO& io,
                    double time,
                    ViewIdx view,
                    const RenderScale& scale,
                    const EdgeBlurParams& params,
                    const PassList& blur,
                    const PassList& edges) const
{
#ifdef NATRON_HAVE_GPU
    gpu::GpuRecordContext& ctx = io.context;
    const RectI& output = io.output;
    const int nComps = io.nComps;
    if (PassExec::isEmptyRect(output)) {
        return eStatusOK;
    }
    if ((nComps < 1) || (nComps > 4) || !io.filtered.buffer || !blur.gpu || !edges.gpu) {
        return eStatusFailed;
    }
    const uint64_t pixels = (uint64_t)output.width() * output.height();
    if (pixels * 4 > UINT32_MAX) {
        return eStatusFailed;
    }

    // The blur over the CPU's buffer, the same rects as Blur's, into a buffer over the output.
    gpu::GpuBuffer* blurred = nullptr;
    if (!ctx.scratch((VkDeviceSize)(pixels * nComps) * sizeof(float), blurred) || !blurred) {
        return eStatusFailed;
    }
    {
        PassGpuIO blurIO(io);
        blurIO.filtered = packedView(blurred, output, nComps);
        const StatusEnum status = blur.gpu(blurIO);
        if (status != eStatusOK) {
            return status;
        }
    }

    // The edges of the matte's alpha over EdgeDetect's buffer, into a buffer over the output.
    GpuImageView matte;
    RectI matteValid;
    if (params.externalMatte) {
        if (kEdgeBlurInputMatte < (int)io.inputs.size()) {
            matte = io.inputs[kEdgeBlurInputMatte];
            matteValid = matte.bounds;
        }
    } else {
        matte = io.source;
        matteValid = io.sourceValid;
    }
    const int edgeComp = (matte.buffer && (matte.nComps >= 1) && (matte.nComps <= 4)) ? PassExec::channelIndexForBit(matte.nComps, kAlphaBit) : -1;
    RectI matteRoD;
    RectI edgeRoD;
    RectI edgeBuffer;
    const bool hasEdge = (edgeComp >= 0) && edgeRects(params, edges, time, view, scale, output, &matteRoD, &edgeRoD, &edgeBuffer);
    gpu::GpuBuffer* edgeBuf = nullptr;
    if (hasEdge) {
        if (!ctx.scratch((VkDeviceSize)(pixels * matte.nComps) * sizeof(float), edgeBuf) || !edgeBuf) {
            return eStatusFailed;
        }
        PassGpuIO edgeIO(ctx);
        edgeIO.source = matte;
        PassExec::intersectRects(matteValid, matteRoD, &edgeIO.sourceValid);
        edgeIO.filtered = packedView(edgeBuf, output, matte.nComps);
        edgeIO.output = output;
        edgeIO.domain = edgeBuffer;
        edgeIO.rod = edgeRoD;
        const PassExtent reach = passListExtent(edges);
        edgeIO.haloX = reach.haloX;
        edgeIO.haloY = reach.haloY;
        edgeIO.nComps = matte.nComps;
        edgeIO.channels.set(kAlphaBit);
        const StatusEnum status = edges.gpu(edgeIO);
        if (status != eStatusOK) {
            return status;
        }
    }
    ctx.computeBarrier();

    EdgeBlurGpuParams p {};
    p.outX1 = output.x1;
    p.outY1 = output.y1;
    p.outWidth = (uint32_t)output.width();
    p.outHeight = (uint32_t)output.height();
    if (!gpuFloatIndex(io.filtered, output.x1, output.y1, &p.dstOffset)) {
        return eStatusFailed;
    }
    p.dstStride = (uint32_t)io.filtered.rowStrideFloats;
    p.channels = (uint32_t)nComps;
    for (int c = 0; c < nComps; ++c) {
        if (io.channels[pixelKernelChannelBit(nComps, c)]) {
            p.processed |= 1u << c;
        }
    }
    // Without source pixels the kernel binds a placeholder, which the empty rect keeps unread.
    const gpu::GpuBuffer* source = blurred;
    if (io.source.buffer && !PassExec::isEmptyRect(io.sourceValid)) {
        p.srcX1 = io.sourceValid.x1;
        p.srcY1 = io.sourceValid.y1;
        p.srcX2 = io.sourceValid.x2;
        p.srcY2 = io.sourceValid.y2;
        if (!gpuFloatIndex(io.source, io.sourceValid.x1, io.sourceValid.y1, &p.srcOffset)) {
            return eStatusFailed;
        }
        p.srcStride = (uint32_t)io.source.rowStrideFloats;
        source = io.source.buffer;
    }
    p.edgeChannels = hasEdge ? (uint32_t)matte.nComps : 1u;
    p.edgeComp = hasEdge ? (uint32_t)edgeComp : 0u;
    p.hasEdge = hasEdge ? 1u : 0u;
    p.exponent = edgeExponent(params.edgeMult);

    return ctx.dispatch(gpu::GpuKernelId::edgeblur_mix, { source, blurred, hasEdge ? edgeBuf : blurred, io.filtered.buffer }, p, { p.outWidth, p.outHeight, 1 }) ? eStatusOK : eStatusFailed;
#else
    Q_UNUSED(io);
    Q_UNUSED(time);
    Q_UNUSED(view);
    Q_UNUSED(scale);
    Q_UNUSED(params);
    Q_UNUSED(blur);
    Q_UNUSED(edges);

    return eStatusFailed;
#endif
} // EdgeBlur::recordGpu

bool
EdgeBlur::readsGpuInput(int inputNb,
                        const ImageLayerDesc& /*plane*/,
                        double time,
                        ViewIdx view) const
{
    if (inputNb != kEdgeBlurInputMatte) {
        return true;
    }

    return getParams(time, view).externalMatte && (bool)getInput(kEdgeBlurInputMatte);
}

RectI
EdgeBlur::opPassesInputRoI(int inputNb,
                           const RectI& outputRect,
                           const RenderScale& scale) const
{
    if (inputNb != kEdgeBlurInputMatte) {
        return outputRect;
    }

    return passListSourceRoI(outputRect, edgePasses(getParams(getCurrentTime(), getCurrentView()), getPassContext(scale)));
}

bool
EdgeBlur::isOpIdentity(double time,
                       const RenderScale& scale,
                       const RectI& /*roi*/,
                       ViewIdx view)
{
    return opPasses(time, view, scale).empty();
}

void
EdgeBlur::getRegionsOfInterest(double time,
                               const RenderScale& scale,
                               const RectD& outputRoD,
                               const RectD& renderWindow,
                               ViewIdx view,
                               RoIMap* ret)
{
    StagedImageEffect::getRegionsOfInterest(time, scale, outputRoD, renderWindow, view, ret);

    const EdgeBlurParams params = getParams(time, view);
    EffectInstancePtr matte = getInput(kEdgeBlurInputMatte);
    if (!params.externalMatte || !matte) {
        return;
    }
    // The Matte is read over the render window grown by the edge detection's reach.
    const double par = getAspectRatio(kEdgeBlurInputSource);
    const RectI rectPixel = PassExec::toPixelEnclosing(renderWindow, scale, par);
    const PassList edges = edgePasses(params, getPassContext(scale));
    const RectD matteRoI = PassExec::toCanonical(passListSourceRoI(rectPixel, edges), scale, par);
    // The Matte may be the same effect as the source or the mask, which then needs both regions.
    RoIMap::iterator found = ret->find(matte);
    if (found != ret->end()) {
        found->second.merge(matteRoI);
    } else {
        ret->insert(std::make_pair(matte, matteRoI));
    }
}

bool
EdgeBlur::clipRegionOfDefinition(double time,
                                 const RenderScale& scale,
                                 ViewIdx view,
                                 const EffectInstancePtr& source,
                                 const RectI& srcRoD,
                                 RectI* rod)
{
    const KnobReader reader(time, view);

    if (!reader.value(_cropToFormat)) {
        return false;
    }

    return cropToFormatBorders(formatOf(getApp(), source, time), scale, srcRoD, rod);
}

NATRON_NAMESPACE_EXIT
