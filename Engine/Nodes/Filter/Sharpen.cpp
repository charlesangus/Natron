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

#include "Sharpen.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuBlur.h"
#include "Engine/Gpu/GpuMorphology.h"
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
// The source's copy, which the blur passes filter in place; the result is written over the source.
const int kSlotSource = 0;
const int kSlotBlur = 1;

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
pixelSizes(const SharpenParams& params,
           const PassContext& context)
{
    const OfxPointD rs = context.scale.toOfxPointD();
    PixelSizes sizes;

    sizes.x = rs.x * (params.sizeX / context.pixelAspectRatio);
    sizes.y = rs.y * params.sizeY;

    return sizes;
}

/// Whether the blur leaves every line unchanged: Blur's identity rule.
bool
blurIsIdentity(BlurKernels::Filter filter,
               const PixelSizes& s)
{
    if (isGaussianFamily(filter)) {
        return ((float)(s.x / 2.4) < 0.1) && ((float)(s.y / 2.4) < 0.1);
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

// The source copied into the buffer the blur passes filter.
std::function<PassRowFn(const PassScratch&)>
copyWorker()
{
    return [](const PassScratch& scratch) -> PassRowFn {
        return [&scratch](int line) {
            const int p = line / scratch.height;
            const int row = line % scratch.height;
            const std::size_t offset = (std::size_t)row * scratch.width;
            const float* source = scratch.plane(kSlotSource, p) + offset;

            std::copy(source, source + scratch.width, scratch.plane(kSlotBlur, p) + offset);
        };
    };
}

/// The value of one sample of the result: what sharpen.slang's combine computes too.
inline float
combineSample(SharpenModeEnum mode,
              float amount,
              float threshold,
              float in,
              float blurred)
{
    const float d = in - blurred;

    if (mode == eSharpenModeLaplacian) {
        return amount * d;
    }
    const float w = Sharpen::thresholdWeight(d, blurred, threshold);

    return (mode == eSharpenModeSoften) ? (in - amount * w * d) : (in + amount * w * d);
}

// The source and its blur combined into the source's buffer.
std::function<PassRowFn(const PassScratch&)>
combineWorker(SharpenModeEnum mode,
              float amount,
              float threshold)
{
    return [mode, amount, threshold](const PassScratch& scratch) -> PassRowFn {
        return [&scratch, mode, amount, threshold](int line) {
            const int p = line / scratch.height;
            const int row = line % scratch.height;
            const std::size_t offset = (std::size_t)row * scratch.width;
            float* in = scratch.plane(kSlotSource, p) + offset;
            const float* blurred = scratch.plane(kSlotBlur, p) + offset;

            for (int i = 0; i < scratch.width; ++i) {
                in[i] = combineSample(mode, amount, threshold, in[i], blurred[i]);
            }
        };
    };
}

Pass
atSlot(Pass pass,
       int slot)
{
    pass.slot = slot;

    return pass;
}

#ifdef NATRON_HAVE_GPU
// Mirrors SharpenParams in Kernels/sharpen.slang, which has no padding between members.
struct SharpenGpuParams {
    int32_t bufX1;
    int32_t bufY1;
    uint32_t bufWidth;
    uint32_t bufHeight;
    int32_t blurX1;
    int32_t blurY1;
    uint32_t blurWidth;
    uint32_t channels;
    int32_t domX1;
    int32_t domY1;
    int32_t domX2;
    int32_t domY2;
    int32_t outX1;
    int32_t outY1;
    uint32_t outWidth;
    uint32_t outHeight;
    uint32_t dstOffset;
    uint32_t dstStride;
    int32_t rodX1;
    int32_t rodY1;
    int32_t rodX2;
    int32_t rodY2;
    int32_t srcX1;
    int32_t srcY1;
    int32_t srcX2;
    int32_t srcY2;
    uint32_t srcOffset;
    uint32_t srcStride;
    uint32_t mode;
    float amount;
    float threshold;
    float epsilon;
};
static_assert(sizeof(SharpenGpuParams) == 128);
static_assert(offsetof(SharpenGpuParams, domX1) == 32);
static_assert(offsetof(SharpenGpuParams, rodX1) == 72);
static_assert(offsetof(SharpenGpuParams, mode) == 112);

// The blur of sharpenPasses() as the GPU recorder runs it.
struct SharpenGpuPlan {
    SharpenModeEnum mode = eSharpenModeSharpen;
    float amount = 1.f;
    float threshold = 0.f;
    // No blur pass: the blurred buffer is the source's.
    bool identity = false;
    bool fir = false;
    // fir: the half kernels and their radii.
    std::vector<float> weightsX;
    std::vector<float> weightsY;
    int radiusX = 0;
    int radiusY = 0;
    // The other filters, with the nearest pixel repeated past the ends of their lines.
    gpu::GpuLineFilter filterX;
    gpu::GpuLineFilter filterY;
};

// A tightly packed scratch buffer of nComps floats per pixel of rect.
gpu::GpuBlurImage
scratchImage(gpu::GpuBuffer* buffer,
             const RectI& rect,
             int nComps)
{
    gpu::GpuBlurImage image;

    image.buffer = buffer;
    image.bounds = PassExec::toGpuBlurRect(rect);
    image.strideFloats = (uint32_t)(rect.width() * nComps);

    return image;
}

// The float index of pixel (x, y) of image, or false when the image exceeds the kernels' 32-bit
// indices.
bool
gpuFloatIndex(const gpu::GpuBlurImage& image,
              int x,
              int y,
              int nComps,
              uint32_t* out)
{
    const uint64_t index = image.offsetFloats + (uint64_t)(y - image.bounds.y1) * image.strideFloats + (uint64_t)(x - image.bounds.x1) * nComps;
    const uint64_t end = image.offsetFloats + (uint64_t)(image.bounds.y2 - image.bounds.y1) * image.strideFloats;
    if ((index > UINT32_MAX) || (end > UINT32_MAX)) {
        return false;
    }
    *out = (uint32_t)index;

    return true;
}

// A scratch buffer for nComps floats per pixel of rect, at least one float.
bool
scratchFor(gpu::GpuRecordContext& ctx,
           const RectI& rect,
           int nComps,
           gpu::GpuBuffer** buffer)
{
    const uint64_t floats = PassExec::isEmptyRect(rect) ? 0 : (uint64_t)rect.width() * rect.height() * nComps;
    if (floats > UINT32_MAX) {
        return false;
    }
    gpu::GpuBuffer* out = nullptr;
    if (!ctx.scratch((VkDeviceSize)std::max<uint64_t>(floats, 1) * sizeof(float), out) || !out) {
        return false;
    }
    *buffer = out;

    return true;
}

bool
dispatchSharpen(gpu::GpuRecordContext& ctx,
                gpu::GpuKernelId kernel,
                const gpu::GpuBuffer* in,
                const gpu::GpuBuffer* blurred,
                const gpu::GpuBuffer* out,
                const SharpenGpuParams& p,
                uint32_t width,
                uint32_t height)
{
    if (!width || !height) {
        return true;
    }

    return (bool)ctx.dispatch(kernel, { in, blurred, out }, p, { width, height, 1 });
}

/// The pass list on the GPU. The source is filled into a buffer with the CPU's nearest-pixel
/// boundary: over the CPU's buffer, io.domain, for the recursive and box filters, whose result
/// depends on the extent of their lines. The FIR Gaussian reads a fixed reach, so its buffer is
/// io.output grown by the radius with the region of definition's edges repeated, which gives the
/// CPU's Neumann ends to a blur that reads black past its domain. The combine then writes
/// io.filtered over io.output, zero outside the CPU's buffer as the CPU writes it.
PassGpuRecorder
sharpenGpuRecorder(const std::shared_ptr<const SharpenGpuPlan>& plan)
{
    return [plan](const PassGpuIO& io) -> StatusEnum {
        gpu::GpuRecordContext& ctx = io.context;
        const int nComps = io.nComps;
        const gpu::GpuBlurImage filtered = PassExec::toGpuBlurImage(io.filtered);

        SharpenGpuParams base {};
        base.channels = (uint32_t)nComps;
        base.mode = (uint32_t)plan->mode;
        base.amount = plan->amount;
        base.threshold = plan->threshold;
        base.epsilon = Sharpen::kRelativeEpsilon;
        base.domX1 = io.domain.x1;
        base.domY1 = io.domain.y1;
        base.domX2 = io.domain.x2;
        base.domY2 = io.domain.y2;
        base.outX1 = io.output.x1;
        base.outY1 = io.output.y1;
        base.outWidth = (uint32_t)std::max(0, io.output.width());
        base.outHeight = (uint32_t)std::max(0, io.output.height());
        base.dstStride = filtered.strideFloats;
        if (!PassExec::isEmptyRect(io.output) && !gpuFloatIndex(filtered, io.output.x1, io.output.y1, nComps, &base.dstOffset)) {
            return eStatusFailed;
        }

        RectI fillRect = io.domain;
        if (plan->fir && !plan->identity && !PassExec::isEmptyRect(io.domain)) {
            fillRect = RectI(io.output.x1 - plan->radiusX, io.output.y1 - plan->radiusY, io.output.x2 + plan->radiusX, io.output.y2 + plan->radiusY);
        }
        gpu::GpuBuffer* fill = nullptr;
        if (!scratchFor(ctx, fillRect, nComps, &fill)) {
            return eStatusFailed;
        }
        if (PassExec::isEmptyRect(io.domain)) {
            // Every pixel of the window lies outside the empty buffer, so the combine writes zero.
            return dispatchSharpen(ctx, gpu::GpuKernelId::sharpen_combine, fill, fill, filtered.buffer, base, base.outWidth, base.outHeight) ? eStatusOK : eStatusFailed;
        }
        const gpu::GpuBlurImage fillImage = scratchImage(fill, fillRect, nComps);

        SharpenGpuParams fillParams = base;
        fillParams.bufX1 = fillRect.x1;
        fillParams.bufY1 = fillRect.y1;
        fillParams.bufWidth = (uint32_t)fillRect.width();
        fillParams.bufHeight = (uint32_t)fillRect.height();
        fillParams.rodX1 = io.rod.x1;
        fillParams.rodY1 = io.rod.y1;
        fillParams.rodX2 = io.rod.x2;
        fillParams.rodY2 = io.rod.y2;
        const gpu::GpuBuffer* sourceBuffer = io.source.buffer;
        if (!sourceBuffer || PassExec::isEmptyRect(io.sourceValid)) {
            // The kernel still binds a source, which the empty source rect keeps unread.
            sourceBuffer = fill;
        } else {
            const gpu::GpuBlurImage source = PassExec::toGpuBlurImage(io.source);
            fillParams.srcX1 = io.sourceValid.x1;
            fillParams.srcY1 = io.sourceValid.y1;
            fillParams.srcX2 = io.sourceValid.x2;
            fillParams.srcY2 = io.sourceValid.y2;
            fillParams.srcStride = source.strideFloats;
            if (!gpuFloatIndex(source, io.sourceValid.x1, io.sourceValid.y1, nComps, &fillParams.srcOffset)) {
                return eStatusFailed;
            }
        }
        if (!dispatchSharpen(ctx, gpu::GpuKernelId::sharpen_fill, sourceBuffer, sourceBuffer, fill, fillParams, fillParams.bufWidth, fillParams.bufHeight)) {
            return eStatusFailed;
        }
        ctx.computeBarrier();

        // The blurred buffer: the source's own without a blur, else the window for the FIR
        // Gaussian, the CPU's buffer for the others.
        gpu::GpuBuffer* blurred = fill;
        RectI blurRect = fillRect;
        if (!plan->identity) {
            blurRect = plan->fir ? io.output : io.domain;
            if (!scratchFor(ctx, blurRect, nComps, &blurred)) {
                return eStatusFailed;
            }
            const gpu::GpuBlurImage blurImage = scratchImage(blurred, blurRect, nComps);
            if (plan->fir) {
                gpu::GpuSeparableBlur blur;
                blur.src = fillImage;
                blur.srcValid = fillImage.bounds;
                blur.dst = blurImage;
                blur.output = blurImage.bounds;
                blur.domain = fillImage.bounds;
                blur.channels = (uint32_t)nComps;
                blur.weightsX = plan->weightsX;
                blur.weightsY = plan->weightsY;
                if (!gpu::recordSeparableBlur(ctx, blur)) {
                    return eStatusFailed;
                }
            } else {
                gpu::GpuBuffer* window = nullptr;
                if (!gpu::recordFilterWindow(ctx, PassExec::toGpuBlurRect(io.output), PassExec::toGpuBlurRect(io.rod), io.haloX, io.haloY, window)) {
                    return eStatusFailed;
                }
                gpu::GpuLineBlur blur;
                blur.src = fillImage;
                blur.srcValid = fillImage.bounds;
                blur.dst = blurImage;
                blur.output = blurImage.bounds;
                blur.domain = blurImage.bounds;
                blur.window = window;
                blur.channels = (uint32_t)nComps;
                blur.filterX = plan->filterX;
                blur.filterY = plan->filterY;
                if (!gpu::recordLineBlur(ctx, blur)) {
                    return eStatusFailed;
                }
            }
            ctx.computeBarrier();
        }

        SharpenGpuParams combine = base;
        combine.bufX1 = fillRect.x1;
        combine.bufY1 = fillRect.y1;
        combine.bufWidth = (uint32_t)fillRect.width();
        combine.bufHeight = (uint32_t)fillRect.height();
        combine.blurX1 = blurRect.x1;
        combine.blurY1 = blurRect.y1;
        combine.blurWidth = (uint32_t)blurRect.width();

        return dispatchSharpen(ctx, gpu::GpuKernelId::sharpen_combine, fill, blurred, filtered.buffer, combine, combine.outWidth, combine.outHeight) ? eStatusOK : eStatusFailed;
    };
} // sharpenGpuRecorder
#endif // NATRON_HAVE_GPU

/// The passes of one sharpening at pixel sizes s. The blur's halo is Blur's (getSourceRoI()); the
/// nearest-pixel boundary spreads nothing, so the region of definition is the source's.
PassList
sharpenPasses(const SharpenParams& params,
              const PixelSizes& s)
{
    const BlurKernels::Filter filter = params.filter;
    const bool fir = (filter == BlurKernels::eFilterFIRGaussian);
    const bool neumann = true;
    LineFilterPtr filterX;
    LineFilterPtr filterY;
    int haloX = 0;
    int haloY = 0;
    std::size_t taps[2] = { 1, 1 };

    if (blurIsIdentity(filter, s)) {
        filterX = filterY = std::make_shared<const BlurKernels::LineFilter>();
    } else if (isGaussianFamily(filter)) {
        haloX = std::max(3, (int)std::ceil(s.x * 1.5));
        haloY = std::max(3, (int)std::ceil(s.y * 1.5));
        filterX = std::make_shared<const BlurKernels::LineFilter>(BlurKernels::LineFilter::forFilter(filter, (float)(s.x / 2.4), 0, neumann));
        filterY = std::make_shared<const BlurKernels::LineFilter>(BlurKernels::LineFilter::forFilter(filter, (float)(s.y / 2.4), 0, neumann));
    } else if (fir) {
        haloX = firRadius(s.x);
        haloY = firRadius(s.y);
        // A FIR Gaussian sample costs its 2r + 1 taps, so the passes are split by taps rather than
        // by pixels.
        taps[0] = (std::size_t)(2 * haloX + 1);
        taps[1] = (std::size_t)(2 * haloY + 1);
        filterX = std::make_shared<const BlurKernels::LineFilter>(BlurKernels::LineFilter::firGaussian(s.x / 2.4, 0, neumann));
        filterY = std::make_shared<const BlurKernels::LineFilter>(BlurKernels::LineFilter::firGaussian(s.y / 2.4, 0, neumann));
    } else {
        const int iter = boxIterations(filter);
        haloX = iter * static_cast<int>(std::floor((s.x - 1) / 2) + 1);
        haloY = iter * static_cast<int>(std::floor((s.y - 1) / 2) + 1);
        filterX = std::make_shared<const BlurKernels::LineFilter>(BlurKernels::LineFilter::forFilter(filter, static_cast<float>(s.x), 0, neumann));
        filterY = std::make_shared<const BlurKernels::LineFilter>(BlurKernels::LineFilter::forFilter(filter, static_cast<float>(s.y), 0, neumann));
    }

    PassList list;
    list.boundary = PassExec::Boundary::Nearest;
    list.scratchSlots = 2;
    list.resultSlot = kSlotSource;

    Pass passX = Pass::lineX(haloX, 0, lineWorker(filterX, false));
    passX.tapsPerSample = taps[0];
    // The FIR Gaussian takes the columns a block of adjacent ones at a time, so it reads the
    // plane row by row.
    Pass passY = Pass::lineY(haloY, 0, lineWorker(filterY, fir));
    passY.tapsPerSample = taps[1];
    passY.columnBlock = fir ? BlurKernels::LineFilter::kColumnBlock : 1;
    list.passes.push_back(Pass::point(true, copyWorker()));
    list.passes.push_back(atSlot(passX, kSlotBlur));
    list.passes.push_back(atSlot(passY, kSlotBlur));
    list.passes.push_back(Pass::point(true, combineWorker(params.mode, knobValueAsFloat(params.amount), knobValueAsFloat(params.threshold))));

#ifdef NATRON_HAVE_GPU
    std::shared_ptr<SharpenGpuPlan> plan = std::make_shared<SharpenGpuPlan>();
    plan->mode = params.mode;
    plan->amount = knobValueAsFloat(params.amount);
    plan->threshold = knobValueAsFloat(params.threshold);
    plan->identity = filterX->isIdentity() && filterY->isIdentity();
    plan->fir = fir;
    if (fir) {
        plan->radiusX = haloX;
        plan->radiusY = haloY;
        plan->weightsX = BlurKernels::LineFilter::firGaussianWeights(s.x / 2.4);
        plan->weightsY = BlurKernels::LineFilter::firGaussianWeights(s.y / 2.4);
    } else {
        plan->filterX = BlurKernels::toGpuLineFilter(*filterX);
        plan->filterY = BlurKernels::toGpuLineFilter(*filterY);
    }
    if (!fir || ((haloX <= (int)gpu::kGpuBlurMaxRadius) && (haloY <= (int)gpu::kGpuBlurMaxRadius))) {
        list.gpu = sharpenGpuRecorder(plan);
    }
#endif

    return list;
} // sharpenPasses
} // anonymous namespace

void
SharpenOp::createKnobs(KnobHolder* holder,
                       const KnobPagePtr& page)
{
    KnobChoicePtr modeKnob = AppManager::createKnob<KnobChoice>(holder, EffectInstance::tr("Mode"));
    modeKnob->setName(kSharpenParamMode);
    modeKnob->setHintToolTip(EffectInstance::tr("What is done with the detail, the input minus its blur."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kSharpenParamModeSharpen, EffectInstance::tr("Sharpen").toStdString(), EffectInstance::tr("The input plus amount times the detail.").toStdString()));
        options.push_back(ChoiceOption(kSharpenParamModeSoften, EffectInstance::tr("Soften").toStdString(), EffectInstance::tr("The input minus amount times the detail.").toStdString()));
        options.push_back(ChoiceOption(kSharpenParamModeLaplacian, EffectInstance::tr("Laplacian").toStdString(), EffectInstance::tr("Amount times the detail alone.").toStdString()));
        modeKnob->populateChoices(options);
    }
    modeKnob->setDefaultValue((int)eSharpenModeSharpen);
    page->addKnob(modeKnob);
    mode = modeKnob;

    KnobDoublePtr sizeKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Size"), 2);
    sizeKnob->setName(kSharpenParamSize);
    sizeKnob->setHintToolTip(EffectInstance::tr("Size (diameter) of the blur that separates the detail, in pixel units (>=0). The standard deviation of the corresponding Gaussian is size/2.4."));
    sizeKnob->setSpatial(true);
    sizeKnob->setCanAutoFoldDimensions(true);
    for (int d = 0; d < 2; ++d) {
        sizeKnob->setMinimum(0., d);
        sizeKnob->setMaximum(1000., d);
        sizeKnob->setDisplayMinimum(0., d);
        sizeKnob->setDisplayMaximum(100., d);
        sizeKnob->setIncrement(0.1, d);
        sizeKnob->setDecimals(1, d);
        sizeKnob->setDefaultValue(3., d);
    }
    page->addKnob(sizeKnob);
    size = sizeKnob;

    KnobChoicePtr filterKnob = AppManager::createKnob<KnobChoice>(holder, EffectInstance::tr("Filter"));
    filterKnob->setName(kSharpenParamFilter);
    filterKnob->setHintToolTip(EffectInstance::tr("The blur's filter, as Blur's. The default, Gaussian (FIR), reads a finite neighbourhood of 3 sigma."));
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

    KnobDoublePtr amountKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Amount"));
    amountKnob->setName(kSharpenParamAmount);
    amountKnob->setHintToolTip(EffectInstance::tr("How much of the detail is added (Sharpen), removed (Soften) or output (Laplacian). 0 leaves the input unchanged in Sharpen and Soften."));
    amountKnob->setDisplayMinimum(0.);
    amountKnob->setDisplayMaximum(5.);
    amountKnob->setDefaultValue(1.);
    page->addKnob(amountKnob);
    amount = amountKnob;

    KnobDoublePtr thresholdKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Threshold"));
    thresholdKnob->setName(kSharpenParamThreshold);
    thresholdKnob->setHintToolTip(EffectInstance::tr("Detail smaller than threshold times the local blurred value is left out, detail larger than twice that is taken whole, with a linear ramp between. Relative, so a dark and a bright plate are treated alike. 0 turns it off. Ignored in Laplacian mode."));
    thresholdKnob->setMinimum(0.);
    thresholdKnob->setDisplayMinimum(0.);
    thresholdKnob->setDisplayMaximum(1.);
    thresholdKnob->setDefaultValue(0.);
    page->addKnob(thresholdKnob);
    threshold = thresholdKnob;
} // SharpenOp::createKnobs

SharpenOp::Params
SharpenOp::bake(const KnobReader& reader) const
{
    Params params;

    const int m = reader.value(mode);
    if ((m >= (int)eSharpenModeSharpen) && (m <= (int)eSharpenModeLaplacian)) {
        params.mode = (SharpenModeEnum)m;
    }
    params.sizeX = reader.value(size, 0);
    params.sizeY = reader.value(size, 1);
    const int f = reader.value(filter);
    if ((f >= (int)BlurKernels::eFilterQuasiGaussian) && (f <= (int)BlurKernels::eFilterFIRGaussian)) {
        params.filter = (BlurKernels::Filter)f;
    }
    params.amount = reader.value(amount);
    params.threshold = std::max(0., (double)reader.value(threshold));

    return params;
}

bool
SharpenOp::isIdentity(const Params& params,
                      const PassContext& context) const
{
    if (params.mode == eSharpenModeLaplacian) {
        return false;
    }

    return (params.amount == 0.) || blurIsIdentity(params.filter, pixelSizes(params, context));
}

PassList
SharpenOp::passes(const Params& params,
                  const PassContext& context) const
{
    return sharpenPasses(params, pixelSizes(params, context));
}

std::optional<GpuWeight>
SharpenOp::gpuWeight(const Params& params,
                     const PassContext& context) const
{
    if (isGaussianFamily(params.filter)) {
        return std::nullopt;
    }
    const PixelSizes sizes = pixelSizes(params, context);

    return gpuSizeScaledWeight(std::max(sizes.x, sizes.y));
}

Sharpen::Sharpen(NodePtr node)
    : ImageNode<SharpenOp>(node)
{
}

Sharpen::~Sharpen()
{
}

void
Sharpen::addAcceptedComponents(int inputNb,
                               std::list<ImageLayerDesc>* comps)
{
    ImageNode<SharpenOp>::addAcceptedComponents(inputNb, comps);
    if (!isInputMask(inputNb)) {
        comps->push_back(ImageLayerDesc::getXYComponents());
    }
}

RectI
Sharpen::getSourceRoI(const RectI& rect,
                      const RenderScale& scale,
                      const SharpenParams& params)
{
    PassContext context;

    context.scale = scale;

    return passListSourceRoI(rect, sharpenPasses(params, pixelSizes(params, context)));
}

float
Sharpen::thresholdWeight(float d,
                         float b,
                         float threshold)
{
    if (threshold <= 0.f) {
        return 1.f;
    }
    const float r = std::fabs(d) / std::max(std::fabs(b), kRelativeEpsilon);

    return std::min(1.f, std::max(0.f, (r - threshold) / threshold));
}

NATRON_NAMESPACE_EXIT
