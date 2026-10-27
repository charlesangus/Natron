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

#include "Blur.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Project.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuBlur.h"
#include "Engine/Gpu/GpuMorphology.h"
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
bool
isGaussianFamily(BlurKernels::Filter filter)
{
    return (filter == BlurKernels::eFilterQuasiGaussian) || (filter == BlurKernels::eFilterGaussian);
}

GpuBlurFilter
gpuBlurFilterOf(BlurKernels::Filter filter)
{
    switch (filter) {
    case BlurKernels::eFilterQuasiGaussian:
        return GpuBlurFilter::eQuasiGaussian;
    case BlurKernels::eFilterGaussian:
        return GpuBlurFilter::eGaussian;
    case BlurKernels::eFilterBox:
        return GpuBlurFilter::eBox;
    case BlurKernels::eFilterTriangle:
        return GpuBlurFilter::eTriangle;
    case BlurKernels::eFilterQuadratic:
        return GpuBlurFilter::eQuadratic;
    case BlurKernels::eFilterFIRGaussian:
        return GpuBlurFilter::eFirGaussian;
    }

    return GpuBlurFilter::eAny;
}

int
boxIterations(BlurKernels::Filter filter)
{
    return (filter == BlurKernels::eFilterBox) ? 1 : ((filter == BlurKernels::eFilterTriangle) ? 2 : 3);
}

/// The FIR Gaussian's support for a size (diameter, in pixels at the render scale).
int
firRadius(double size)
{
    return BlurKernels::LineFilter::firGaussianRadius(size / 2.4);
}

/// The size of each axis in pixels at the render scale.
struct PixelSizes {
    double x;
    double y;
};

PixelSizes
pixelSizes(const BlurParams& params,
           const PassContext& context)
{
    const OfxPointD rs = context.scale.toOfxPointD();
    PixelSizes sizes;

    sizes.x = rs.x * (params.sizeX / context.pixelAspectRatio);
    sizes.y = rs.y * params.sizeY;

    return sizes;
}

bool
isIdentityAt(BlurKernels::Filter filter,
             const PixelSizes& s)
{
    if (isGaussianFamily(filter)) {
        const float sigmax = (float)(s.x / 2.4);
        const float sigmay = (float)(s.y / 2.4);

        return (sigmax < 0.1) && (sigmay < 0.1);
    }
    if (filter == BlurKernels::eFilterFIRGaussian) {
        return !firRadius(s.x) && !firRadius(s.y);
    }

    return (s.x <= 1) && (s.y <= 1);
}

using LineFilterPtr = std::shared_ptr<const BlurKernels::LineFilter>;

/// Filters each line the executor hands over with filter, through a scratch owned by the chunk.
/// A FIR column block always goes through applyColumns(), even a last block of one column.
std::function<PassLineFn()>
lineWorker(const LineFilterPtr& filter,
           bool columns)
{
    if (filter->isIdentity()) {
        return std::function<PassLineFn()>();
    }

    return [filter, columns]() -> PassLineFn {
        std::shared_ptr<BlurKernels::LineScratch> scratch = std::make_shared<BlurKernels::LineScratch>();

        return [filter, columns, scratch](float* line, int length, std::ptrdiff_t stride, int count) {
            if (columns) {
                filter->applyColumns(line, length, stride, count, *scratch);
            } else {
                filter->apply(line, length, stride, *scratch);
            }
        };
    };
}

#ifdef NATRON_HAVE_GPU
/// The source the kernels read. With no source pixels in the extent the executor hands over no
/// buffer, but the kernels still bind one: a placeholder, which the empty valid rect keeps unread.
bool
gpuSourceImage(const PassGpuIO& io,
               gpu::GpuBlurImage* image)
{
    if (io.source.buffer) {
        *image = PassExec::toGpuBlurImage(io.source);

        return true;
    }
    gpu::GpuBuffer* placeholder = nullptr;
    if (!io.context.scratch((VkDeviceSize)std::max(1, io.nComps) * sizeof(float), placeholder) || !placeholder) {
        return false;
    }
    *image = gpu::GpuBlurImage();
    image->buffer = placeholder;

    return true;
}

PassGpuRecorder
firGpuRecorder(double sx,
               double sy)
{
    return [sx, sy](const PassGpuIO& io) -> StatusEnum {
        gpu::GpuBlurImage source;
        if (!gpuSourceImage(io, &source)) {
            return eStatusFailed;
        }
        const std::vector<float> weightsX = BlurKernels::LineFilter::firGaussianWeights(sx / 2.4);
        const std::vector<float> weightsY = BlurKernels::LineFilter::firGaussianWeights(sy / 2.4);
        gpu::GpuSeparableBlur blur;
        blur.src = source;
        blur.srcValid = PassExec::toGpuBlurRect(io.sourceValid);
        blur.dst = PassExec::toGpuBlurImage(io.filtered);
        blur.output = PassExec::toGpuBlurRect(io.output);
        blur.domain = PassExec::toGpuBlurRect(io.domain);
        blur.channels = (uint32_t)io.nComps;
        blur.weightsX = weightsX;
        blur.weightsY = weightsY;

        return gpu::recordSeparableBlur(io.context, blur) ? eStatusOK : eStatusFailed;
    };
}

PassGpuRecorder
lineGpuRecorder(const LineFilterPtr& filterX,
                const LineFilterPtr& filterY)
{
    return [filterX, filterY](const PassGpuIO& io) -> StatusEnum {
        gpu::GpuBlurImage source;
        if (!gpuSourceImage(io, &source)) {
            return eStatusFailed;
        }
        // A recursive filter depends on whole lines, so the filtered rect must be the CPU's: the
        // window grown by the halo and clipped to the region of definition.
        gpu::GpuBuffer* window = nullptr;
        if (!gpu::recordFilterWindow(io.context, PassExec::toGpuBlurRect(io.output), PassExec::toGpuBlurRect(io.rod), io.haloX, io.haloY, window)) {
            return eStatusFailed;
        }
        gpu::GpuLineBlur blur;
        blur.src = source;
        blur.srcValid = PassExec::toGpuBlurRect(io.sourceValid);
        blur.dst = PassExec::toGpuBlurImage(io.filtered);
        blur.output = PassExec::toGpuBlurRect(io.output);
        blur.domain = PassExec::toGpuBlurRect(io.domain);
        blur.window = window;
        blur.channels = (uint32_t)io.nComps;
        blur.filterX = BlurKernels::toGpuLineFilter(*filterX);
        blur.filterY = BlurKernels::toGpuLineFilter(*filterY);

        return gpu::recordLineBlur(io.context, blur) ? eStatusOK : eStatusFailed;
    };
}
#endif // NATRON_HAVE_GPU

/// The pass list of a blur of pixel sizes s: getSourceRoI()'s halo, the region of definition's
/// spread, and the filters CImgBlurPlugin::blur() runs (sigma = size / 2.4 for the Gaussians, the
/// box width the size).
PassList
blurPasses(BlurKernels::Filter filter,
           const PixelSizes& s)
{
    PassList list;
    LineFilterPtr filterX;
    LineFilterPtr filterY;
    int haloX = 0;
    int haloY = 0;
    int growX = 0;
    int growY = 0;
    std::size_t taps[2] = { 1, 1 };
    const bool fir = (filter == BlurKernels::eFilterFIRGaussian);

    if (isGaussianFamily(filter)) {
        const float sigmax = (float)(s.x / 2.4);
        const float sigmay = (float)(s.y / 2.4);
        if ((sigmax < 0.1) && (sigmay < 0.1)) {
            return list;
        }
        haloX = growX = std::max(3, (int)std::ceil(s.x * 1.5));
        haloY = growY = std::max(3, (int)std::ceil(s.y * 1.5));
        filterX = std::make_shared<const BlurKernels::LineFilter>(BlurKernels::LineFilter::forFilter(filter, sigmax, 0, false));
        filterY = std::make_shared<const BlurKernels::LineFilter>(BlurKernels::LineFilter::forFilter(filter, sigmay, 0, false));
    } else if (fir) {
        const int padX = firRadius(s.x);
        const int padY = firRadius(s.y);
        if (!padX && !padY) {
            return list;
        }
        haloX = growX = padX;
        haloY = growY = padY;
        // A FIR Gaussian sample costs its 2r + 1 taps, so the passes are split by taps rather than
        // by pixels.
        taps[0] = (std::size_t)(2 * padX + 1);
        taps[1] = (std::size_t)(2 * padY + 1);
        filterX = std::make_shared<const BlurKernels::LineFilter>(BlurKernels::LineFilter::firGaussian(s.x / 2.4, 0, false));
        filterY = std::make_shared<const BlurKernels::LineFilter>(BlurKernels::LineFilter::firGaussian(s.y / 2.4, 0, false));
    } else {
        // The region of interest rounds the half width down and adds one, the region of definition
        // rounds it up, as CImgBlurPlugin does.
        const int iter = boxIterations(filter);
        haloX = iter * static_cast<int>(std::floor((s.x - 1) / 2) + 1);
        haloY = iter * static_cast<int>(std::floor((s.y - 1) / 2) + 1);
        growX = iter * (int)std::ceil((s.x - 1) / 2);
        growY = iter * (int)std::ceil((s.y - 1) / 2);
        filterX = std::make_shared<const BlurKernels::LineFilter>(BlurKernels::LineFilter::forFilter(filter, static_cast<float>(s.x), 0, false));
        filterY = std::make_shared<const BlurKernels::LineFilter>(BlurKernels::LineFilter::forFilter(filter, static_cast<float>(s.y), 0, false));
    }

    Pass passX = Pass::lineX(haloX, growX, lineWorker(filterX, false));
    passX.tapsPerSample = taps[0];
    // The FIR Gaussian takes the columns a block of adjacent ones at a time, so it reads the
    // plane row by row.
    Pass passY = Pass::lineY(haloY, growY, lineWorker(filterY, fir));
    passY.tapsPerSample = taps[1];
    passY.columnBlock = fir ? BlurKernels::LineFilter::kColumnBlock : 1;
    list.passes.push_back(passX);
    list.passes.push_back(passY);

#ifdef NATRON_HAVE_GPU
    if (!fir) {
        list.gpu = lineGpuRecorder(filterX, filterY);
    } else if ((firRadius(s.x) <= (int)gpu::kGpuBlurMaxRadius) && (firRadius(s.y) <= (int)gpu::kGpuBlurMaxRadius)) {
        list.gpu = firGpuRecorder(s.x, s.y);
    }
#endif

    return list;
} // blurPasses
} // anonymous namespace

void
BlurOp::createKnobs(KnobHolder* holder,
                    const KnobPagePtr& page)
{
    KnobDoublePtr sizeKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Size"), 2);
    sizeKnob->setName(kBlurParamSize);
    sizeKnob->setHintToolTip(EffectInstance::tr("Size (diameter) of the filter kernel, in pixel units (>=0). The standard deviation of the corresponding Gaussian is size/2.4. No blur is applied if size < 0.24 (the Gaussians and quasi-Gaussian) or <= 1 (box, triangle and quadratic)."));
    sizeKnob->setSpatial(true);
    sizeKnob->setCanAutoFoldDimensions(true);
    for (int d = 0; d < 2; ++d) {
        sizeKnob->setMinimum(0., d);
        sizeKnob->setMaximum(1000., d);
        sizeKnob->setDisplayMinimum(0., d);
        sizeKnob->setDisplayMaximum(100., d);
        sizeKnob->setIncrement(0.1, d);
        sizeKnob->setDecimals(1, d);
        sizeKnob->setDefaultValue(0., d);
    }
    sizeKnob->setAddNewLine(false);
    sizeKnob->setSpacingBetweenItems(1);
    page->addKnob(sizeKnob);
    size = sizeKnob;

    KnobChoicePtr filterKnob = AppManager::createKnob<KnobChoice>(holder, EffectInstance::tr("Filter"));
    filterKnob->setName(kBlurParamFilter);
    filterKnob->setHintToolTip(EffectInstance::tr("Bluring filter. The default, Gaussian (FIR), has a finite support of 3 sigma, so it leaves pixels far from any detail exactly unchanged, which also suits the premult-blur-unpremult process; it is fast for small and moderate sizes. The IIR filters cost the same at any size: the quasi-Gaussian is the faster, the Gaussian is more isotropic (its impulse response has rotational symmetry), but both produce very small non-zero values far from any detail."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kBlurParamFilterQuasiGaussian, EffectInstance::tr("Quasi-Gaussian").toStdString(), EffectInstance::tr("Quasi-Gaussian filter (0-order recursive Deriche filter, faster) - IIR (infinite support / impulsional response).").toStdString()));
        options.push_back(ChoiceOption(kBlurParamFilterGaussian, EffectInstance::tr("Gaussian").toStdString(), EffectInstance::tr("Gaussian filter (Van Vliet recursive Gaussian filter, more isotropic, slower) - IIR (infinite support / impulsional response).").toStdString()));
        options.push_back(ChoiceOption(kBlurParamFilterBox, EffectInstance::tr("Box").toStdString(), EffectInstance::tr("Box filter - FIR (finite support / impulsional response).").toStdString()));
        options.push_back(ChoiceOption(kBlurParamFilterTriangle, EffectInstance::tr("Triangle").toStdString(), EffectInstance::tr("Triangle/tent filter - FIR (finite support / impulsional response).").toStdString()));
        options.push_back(ChoiceOption(kBlurParamFilterQuadratic, EffectInstance::tr("Quadratic").toStdString(), EffectInstance::tr("Quadratic filter - FIR (finite support / impulsional response).").toStdString()));
        options.push_back(ChoiceOption(kBlurParamFilterFIRGaussian, EffectInstance::tr("Gaussian (FIR)").toStdString(), EffectInstance::tr("Gaussian filter cropped at 3 sigma and normalised - FIR (finite support / impulsional response).").toStdString()));
        filterKnob->populateChoices(options);
    }
    filterKnob->setDefaultValue((int)BlurKernels::eFilterFIRGaussian);
    page->addKnob(filterKnob);
    filter = filterKnob;

    KnobBoolPtr cropToFormatKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr("Crop To Format"));
    cropToFormatKnob->setName(kBlurParamCropToFormat);
    cropToFormatKnob->setHintToolTip(EffectInstance::tr("If the source is inside the format and the effect extends it outside of the format, crop it to avoid unnecessary calculations. To avoid unwanted crops, only the borders that were inside of the format in the source clip will be cropped."));
    cropToFormatKnob->setDefaultValue(true);
    page->addKnob(cropToFormatKnob);
    cropToFormat = cropToFormatKnob;
} // BlurOp::createKnobs

BlurOp::Params
BlurOp::bake(const KnobReader& reader) const
{
    Params params;

    params.sizeX = reader.value(size, 0);
    params.sizeY = reader.value(size, 1);
    const int f = reader.value(filter);
    if ((f >= (int)BlurKernels::eFilterQuasiGaussian) && (f <= (int)BlurKernels::eFilterFIRGaussian)) {
        params.filter = (BlurKernels::Filter)f;
    }
    params.cropToFormat = reader.value(cropToFormat);

    return params;
}

bool
BlurOp::isIdentity(const Params& params,
                   const PassContext& context) const
{
    return isIdentityAt(params.filter, pixelSizes(params, context));
}

PassList
BlurOp::passes(const Params& params,
               const PassContext& context) const
{
    return blurPasses(params.filter, pixelSizes(params, context));
}

std::optional<GpuWeight>
BlurOp::gpuWeight(const Params& params,
                  const PassContext& context) const
{
    if (isGaussianFamily(params.filter)) {
        return std::nullopt;
    }

    return gpuSizeScaledWeight(std::max(params.sizeX / context.pixelAspectRatio, params.sizeY) * context.scale.toOfxPointD().x);
}

Blur::Blur(NodePtr node)
    : ImageNode<BlurOp>(node)
{
}

Blur::~Blur()
{
}

void
Blur::addAcceptedComponents(int inputNb,
                            std::list<ImageLayerDesc>* comps)
{
    ImageNode<BlurOp>::addAcceptedComponents(inputNb, comps);
    if (!isInputMask(inputNb)) {
        comps->push_back(ImageLayerDesc::getXYComponents());
    }
}

void
Blur::getParams(double time,
                ViewIdx view,
                BlurParams* params) const
{
    *params = bakeParams(op(), time, view);
}

RectI
Blur::getSourceRoI(const RectI& rect,
                   const RenderScale& scale,
                   const BlurParams& params)
{
    PassContext context;

    context.scale = scale;

    return passListSourceRoI(rect, blurPasses(params.filter, pixelSizes(params, context)));
}

bool
Blur::paramsAreIdentity(const RenderScale& scale,
                        const BlurParams& params)
{
    PassContext context;

    context.scale = scale;

    return isIdentityAt(params.filter, pixelSizes(params, context));
}

bool
Blur::clipRegionOfDefinition(double time,
                             const RenderScale& scale,
                             ViewIdx view,
                             const EffectInstancePtr& source,
                             const RectI& srcRoD,
                             RectI* rod)
{
    BlurParams params;

    getParams(time, view, &params);
    if (!params.cropToFormat) {
        return false;
    }
    RectI format;
    EffectInstancePtr formatSource = source ? source->getNearestNonIdentity(time) : EffectInstancePtr();
    if (formatSource) {
        format = formatSource->getRenderMetadata(time, view)->getOutputFormat();
    } else {
        Format projectFormat;
        getApp()->getProject()->getProjectDefaultFormat(&projectFormat);
        format = projectFormat;
    }
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
    // Only the borders the source had inside its format are clamped back to it.
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
} // Blur::clipRegionOfDefinition

NATRON_NAMESPACE_EXIT
