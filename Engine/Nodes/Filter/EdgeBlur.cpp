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
#include "Engine/NodeMetadata.h"
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
                   "gives an edge strength of 1. Each pixel is blurred by the Size times its edge weight, the edge strength raised to 1/Edge Mult and clamped to [0, 1], "
                   "as IBlur blurs by its Map: the blur is widest on the edge and narrows smoothly to nothing away from it, with no second, sharp copy of the edge, "
                   "and a larger Edge Mult widens the blurred band. A pixel with no edge is left exactly as it is. "
                   "The blur is IBlur's box applied three times, an approximation of the Gaussian of standard deviation size/2.4. "
                   "The matte is the alpha of the source, or with External Matte the alpha of the Matte input; "
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

// The alpha channel, in the bit space of pixelKernelChannelBit().
const int kAlphaBit = 3;

// The whole range of the edge weight, for the passes the region actions and opPasses() see.
MapRange
fullWeightRange()
{
    MapRange range;

    range.min = 0.f;
    range.max = 1.f;

    return range;
}

/**
 * @brief A pass without work that reads the reach of the edge detection about each pixel, so
 * that the source is read over EdgeDetect's buffer when the matte is the source's alpha.
 **/
Pass
edgeReachPass(const PassList& edges)
{
    const PassExtent reach = passListExtent(edges);
    Pass carrier = Pass::gather(reach.haloX, reach.haloY, true, std::function<PassRowFn(const PassScratch&)>());

    carrier.growX = 0;
    carrier.growY = 0;

    return carrier;
}

// The Matte's pixels rect needs: rect grown by the blur's halo, then by the edge detection's reach.
RectI
matteRoIOf(const EdgeBlurParams& params,
           const PassContext& context,
           const RectI& rect)
{
    return passListSourceRoI(passListSourceRoI(rect, EdgeBlur::blurPasses(params, context)), EdgeBlur::edgePasses(params, context));
}

/// The format of source, else the project's.
RectI
formatOf(const AppInstancePtr& app,
         const EffectInstancePtr& source,
         double time,
         ViewIdx view)
{
    EffectInstancePtr formatSource = source ? source->getNearestNonIdentity(time) : EffectInstancePtr();
    if (formatSource) {
        return formatSource->getRenderMetadata(time, view)->getOutputFormat();
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
    sizeKnob->setHintToolTip(tr("Size (diameter) of the blur where the edge weight is 1, and of the edge detection's pre-blur, in pixel units (0 to 1000). Each pixel's blur is this Size times its edge weight, three boxes approximating the Gaussian of standard deviation size/2.4."));
    sizeKnob->setSpatial(true);
    sizeKnob->setMinimum(0.);
    sizeKnob->setMaximum(1000.);
    sizeKnob->setDisplayMinimum(0.);
    sizeKnob->setDisplayMaximum(100.);
    sizeKnob->setDefaultValue(3.);
    page->addKnob(sizeKnob);
    _size = sizeKnob;

    KnobChoicePtr filterKnob = AppManager::createKnob<KnobChoice>(this, tr("Filter"));
    filterKnob->setName(kEdgeBlurParamFilter);
    filterKnob->setHintToolTip(tr("The edge detection filter, as EdgeDetect's: the Size is the kernel size of the quasi-Gaussian, Gaussian, box, triangle and quadratic filters, and the size of the Gaussian pre-blur of the simple, rotation-invariant and Sobel filters. The blur itself is always IBlur's three boxes."));
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
    edgeMultKnob->setHintToolTip(tr("Sharpness of the borders of the blur area: the edge strength is raised to 1/Edge Mult, clamped to [0, 1], and scales the Size of each pixel's blur, so a larger value widens the blurred band."));
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
    params.size = MapInput::clampSize(reader.value(_size));
    const int f = reader.value(_filter);
    if ((f >= (int)eEdgeDetectFilterSimple) && (f <= (int)eEdgeDetectFilterQuadratic)) {
        params.filter = (EdgeDetectFilterEnum)f;
    }
    params.cropToFormat = reader.value(_cropToFormat);
    params.edgeMult = reader.value(_edgeMult);

    return params;
}

IBlurParams
EdgeBlur::blurParams(const EdgeBlurParams& params)
{
    IBlurParams blur;

    blur.sizeX = params.size;
    blur.sizeY = params.size;
    blur.cropToFormat = params.cropToFormat;
    blur.map.exponent = edgeExponent(params.edgeMult);

    return blur;
}

PassList
EdgeBlur::blurPasses(const EdgeBlurParams& params,
                     const PassContext& context)
{
    return IBlur::passes(blurParams(params), context, fullWeightRange(), std::shared_ptr<const MapPlane>());
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
    MapInputParams map;

    map.exponent = exponent;

    return map.remap(edge);
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
    const PassContext context = getPassContext(time, view, scale);
    const PassList blur = blurPasses(params, context);

    if (blur.empty() || !getInput(matteInput(params))) {
        return PassList();
    }
    const PassList edges = edgePasses(params, context);
    PassList list = blur;
    if (!params.externalMatte) {
        list.passes.insert(list.passes.begin(), edgeReachPass(edges));
    }

#ifdef NATRON_HAVE_GPU
    if (edges.gpu && IBlur::fitsGpu(blur)) {
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
    *matteRoD = PassExec::toPixelEnclosing(rod, scale, getRenderMetadata(time, view)->getPixelAspectRatio(input));
    if (PassExec::isEmptyRect(*matteRoD)) {
        return false;
    }
    // EdgeDetect's own region of definition, cropped to the format as its Crop To Format does.
    *edgeRoD = passListRoD(*matteRoD, edges);
    cropToFormatBorders(formatOf(getApp(), matte, time, view), scale, *matteRoD, edgeRoD);

    return PassExec::intersectRects(passListSourceRoI(output, edges), *edgeRoD, buffer);
}

StatusEnum
EdgeBlur::computeEdges(const RenderActionArgs& args,
                       const EdgeBlurParams& params,
                       const PassList& edges,
                       const RectI& window,
                       MapPlane* plane)
{
    plane->reset(window, 0.f);
    RectI matteRoD;
    RectI edgeRoD;
    RectI buffer;
    if (!edgeRects(params, edges, args.time, args.view, args.mappedScale, window, &matteRoD, &edgeRoD, &buffer)) {
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

    // The weight IBlur's radii read, remapped as its Map is.
    const IBlurParams blur = blurParams(params);
    RectI inside;
    if (PassExec::intersectRects(window, buffer, &inside)) {
        const float* result = storage[(std::size_t)edges.resultSlot].data();
        for (int y = inside.y1; y < inside.y2; ++y) {
            const float* from = result + (std::size_t)(y - buffer.y1) * width + (inside.x1 - buffer.x1);
            float* to = plane->row(y) + (inside.x1 - window.x1);
            for (int i = 0; i < inside.width(); ++i) {
                to[i] = blur.map.remap(from[i]);
            }
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
    const PassContext context = getPassContext(args.time, args.view, args.mappedScale);
    const PassList edges = edgePasses(params, context);
    // The weight over every pixel IBlur's passes read; the radii beyond it, over the reach the
    // first pass carries, are 0 and reach no pixel of args.roi.
    const RectI window = passListSourceRoI(args.roi, blurPasses(params, context));
    std::shared_ptr<MapPlane> weight = std::make_shared<MapPlane>();
    const StatusEnum status = computeEdges(args, params, edges, window, weight.get());
    if (status != eStatusOK) {
        return status;
    }
    PassList list = IBlur::passes(blurParams(params), context, weight->tileRange(window), weight);
    if (list.empty()) {
        // No edge over the window: the image as it is.
        return runPassesCpu(*this, plan, args, list);
    }
    if (!params.externalMatte) {
        list.passes.insert(list.passes.begin(), edgeReachPass(edges));
    }

    return runPassesCpu(*this, plan, args, list);
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
    if ((nComps < 1) || (nComps > 4) || !io.filtered.buffer || !edges.gpu) {
        return eStatusFailed;
    }
    // IBlur's passes over the pixels they read, as on the CPU, and no further into the reach
    // the first pass carries.
    const RectI window = passListSourceRoI(output, blur);
    PassGpuIO blurIO(io);
    if (!PassExec::intersectRects(io.domain, window, &blurIO.domain)) {
        return eStatusFailed;
    }

    // The edges of the matte's alpha over EdgeDetect's buffer, into a buffer over the window
    // within EdgeDetect's region of definition, outside which IBlur's Map reads 0.
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
    RectI edgeOutput;
    const bool hasEdge = (edgeComp >= 0) && edgeRects(params, edges, time, view, scale, window, &matteRoD, &edgeRoD, &edgeBuffer)
        && PassExec::intersectRects(window, edgeRoD, &edgeOutput);
    GpuImageView edgeView;
    if (hasEdge) {
        const uint64_t edgeFloats = (uint64_t)edgeOutput.width() * edgeOutput.height() * matte.nComps;
        gpu::GpuBuffer* edgeBuf = nullptr;
        if ((edgeFloats > UINT32_MAX) || !ctx.scratch((VkDeviceSize)edgeFloats * sizeof(float), edgeBuf) || !edgeBuf) {
            return eStatusFailed;
        }
        edgeView = packedView(edgeBuf, edgeOutput, matte.nComps);
        PassGpuIO edgeIO(ctx);
        edgeIO.source = matte;
        PassExec::intersectRects(matteValid, matteRoD, &edgeIO.sourceValid);
        edgeIO.filtered = edgeView;
        edgeIO.output = edgeOutput;
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
        ctx.computeBarrier();
    }

    // The edges are IBlur's Map, connected even when there are none, so that they read 0.
    if ((int)blurIO.inputs.size() <= kIBlurInputMap) {
        blurIO.inputs.resize(kIBlurInputMap + 1);
    }
    blurIO.inputs[kIBlurInputMap] = edgeView;

    return IBlur::recordPassesGpu(blurIO, blurParams(params), getPassContext(time, view, scale), true, hasEdge ? edgeComp : -1);
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

    return matteRoIOf(getParams(getCurrentTime(), getCurrentView()), getPassContext(getCurrentTime(), getCurrentView(), scale), outputRect);
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
    // The Matte is read over the render window grown by the blur's halo and the edge detection's
    // reach.
    const double par = getRenderMetadata(time, view)->getPixelAspectRatio(kEdgeBlurInputSource);
    const RectI rectPixel = PassExec::toPixelEnclosing(renderWindow, scale, par);
    const RectD matteRoI = PassExec::toCanonical(matteRoIOf(params, getPassContext(time, view, scale), rectPixel), scale, par);
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

    return cropToFormatBorders(formatOf(getApp(), source, time, view), scale, srcRoD, rod);
}

NATRON_NAMESPACE_EXIT
