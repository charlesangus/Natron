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

/*
 * The maths are a port of darktable's hotpixels module, DARKTABLE_REF release-5.6.2 (commit
 * 20891f6c6fa7), src/iop/hotpixels.c, C path only, Copyright (C) 2011-2026 darktable
 * developers, distributed under the GNU GPL version 3 or later.
 */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "RawHotPixels.h"

#include <algorithm>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/PassExecutor.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/OverlayDrawList.h"
#include "Engine/OverlayImage.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Nodes/Raw/RawGpuParams.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
// darktable's search order for the nearest photosites of the same colour on an X-Trans sensor,
// as (column, row) offsets with rows counted downwards.
const int kXTransSearch[20][2] = {
    { -1, 0 },
    { 1, 0 },
    { 0, -1 },
    { 0, 1 },
    { -1, -1 },
    { -1, 1 },
    { 1, -1 },
    { 1, 1 },
    { -2, 0 },
    { 2, 0 },
    { 0, -2 },
    { 0, 2 },
    { -2, -1 },
    { -2, 1 },
    { 2, -1 },
    { 2, 1 },
    { -1, -2 },
    { 1, -2 },
    { -1, 2 },
    { 1, 2 },
};

inline int
wrapSix(int v)
{
    const int m = v % 6;

    return (m < 0) ? (m + 6) : m;
}

inline int
xtransCell(const RawLayout& layout,
           int x,
           int y)
{
    return wrapSix(y - layout.originY()) * 6 + wrapSix(x - layout.originX());
}

RawHotPixelsOp::XTransNeighbours
xtransNeighbours(const RawLayout& layout)
{
    RawHotPixelsOp::XTransNeighbours n;

    for (int row = 0; row < 6; ++row) {
        for (int col = 0; col < 6; ++col) {
            const int x = layout.originX() + col;
            const int y = layout.originY() + row;
            const int cell = xtransCell(layout, x, y);
            const RawCfaColorEnum c = layout.colourAt(x, y);
            int found = 0;
            for (int s = 0; (s < 20) && (found < 4); ++s) {
                const int dx = kXTransSearch[s][0];
                const int dy = -kXTransSearch[s][1];
                if (layout.colourAt(x + dx, y + dy) == c) {
                    n.offsets[cell][found] = { dx, dy };
                    ++found;
                }
            }
            n.count[cell] = found;
        }
    }

    return n;
}

// darktable's test of one photosite: read(dx, dy) gives the value at that offset.
template <typename Read>
inline bool
replacementFor(const RawHotPixelsOp::Params& params,
               int period,
               float value,
               int x,
               int y,
               const Read& read,
               float* replacement)
{
    if (!(value > params.threshold)) {
        return false;
    }
    const float mid = value * params.multiplier;
    int count = 0;
    float maxin = 0.0f;
    const auto test = [&](float other) {
        if (mid > other) {
            ++count;
            if (other > maxin) {
                maxin = other;
            }
        }
    };
    if (period == 2) {
        test(read(-2, 0));
        test(read(0, -2));
        test(read(2, 0));
        test(read(0, 2));
    } else if (period == 6) {
        const int cell = xtransCell(params.layout, x, y);
        for (int n = 0; n < params.xtrans.count[cell]; ++n) {
            test(read(params.xtrans.offsets[cell][n][0], params.xtrans.offsets[cell][n][1]));
        }
    } else {
        test(read(-1, 0));
        test(read(0, -1));
        test(read(1, 0));
        test(read(0, 1));
    }
    if (count < params.minNeighbours) {
        return false;
    }
    *replacement = maxin;

    return true;
}

// One row of one plane: slot 0 copied to slot 1, then each hot photosite in slot 1 replaced.
std::function<PassRowFn(const PassScratch&)>
hotPixelsWorker(const RawHotPixelsOp::Params& params)
{
    return [params](const PassScratch& scratch) -> PassRowFn {
        const int width = scratch.width;
        const int height = scratch.height;
        const RectI rect = scratch.rect;
        const std::size_t planeSize = scratch.planeSize;
        const float* const source = scratch.buffers[0];
        float* const result = scratch.buffers[1];
        const int period = params.layout.period();
        const int border = (period > 0) ? 2 : 1;

        return [params, width, height, rect, planeSize, source, result, period, border](int line) {
            const int p = line / height;
            const int row = line % height;
            const float* in = source + (std::size_t)p * planeSize;
            float* out = result + (std::size_t)p * planeSize;
            const std::size_t base = (std::size_t)row * (std::size_t)width;

            std::copy(in + base, in + base + width, out + base);
            if ((row < border) || (row >= height - border)) {
                return;
            }
            const std::size_t stride = (std::size_t)width;
            const int y = rect.y1 + row;
            for (int i = border; i < width - border; ++i) {
                const std::size_t index = base + (std::size_t)i;
                const auto read = [in, index, stride](int dx, int dy) {
                    return in[(std::ptrdiff_t)index + (std::ptrdiff_t)dy * (std::ptrdiff_t)stride + dx];
                };
                float replacement;
                if (replacementFor(params, period, in[index], rect.x1 + i, y, read, &replacement)) {
                    out[index] = replacement;
                }
            }
        };
    };
} // hotPixelsWorker

// How far darktable's marks reach along the row of a fixed pixel.
const int kMarkReach = 10;

// A photosite counts once however many of its channels are fixed.
std::vector<std::vector<int>>
fixedColumnsPerRow(const RawHotPixelsOp::Params& params,
                   const RawNodeSupport::LockedSource& source,
                   const std::vector<int>& channels,
                   const RectI& candidates,
                   const RectI& window,
                   const RawKernels::ParallelFor& parallel)
{
    std::vector<std::vector<int>> rows((std::size_t)window.height());
    const int period = params.layout.period();
    const Image::ReadAccess& access = *source.access;

    parallel(window.height(), [&](int r) {
        const int y = window.y1 + r;
        if ((y < candidates.y1) || (y >= candidates.y2)) {
            return;
        }
        for (int x = candidates.x1; x < candidates.x2; ++x) {
            const float* const pixel = (const float*)access.pixelAt(x, y);
            if (!pixel) {
                continue;
            }
            for (const int c : channels) {
                const auto read = [&access, x, y, c](int dx, int dy) {
                    const float* const other = (const float*)access.pixelAt(x + dx, y + dy);

                    return other ? other[c] : 0.f;
                };
                float replacement;
                if (replacementFor(params, period, pixel[c], x, y, read, &replacement)) {
                    rows[(std::size_t)r].push_back(x);
                    break;
                }
            }
        }
    });

    return rows;
}

// darktable marks a fixed pixel with same-coloured photosites along its row: every second one on
// a Bayer mosaic, those from 2 away on an X-Trans one, and every pixel without a CFA.
inline bool
isMarkOffset(const RawLayout& layout,
             int x,
             int y,
             int offset)
{
    const int period = layout.period();

    if (period == 2) {
        return (offset % 2) == 0;
    }
    if (period == 6) {
        return (std::abs(offset) >= 2) && (layout.colourAt(x + offset, y) == layout.colourAt(x, y));
    }

    return true;
}

#ifdef NATRON_HAVE_GPU
// The float index of pixel (x, y) of image, or false when the image exceeds the kernels' 32-bit
// indices.
bool
gpuFloatIndex(const gpu::GpuBlurImage& image,
              int x,
              int y,
              int nComps,
              std::uint32_t* out)
{
    const std::uint64_t index = image.offsetFloats + (std::uint64_t)(y - image.bounds.y1) * image.strideFloats + (std::uint64_t)(x - image.bounds.x1) * nComps;
    const std::uint64_t end = image.offsetFloats + (std::uint64_t)(image.bounds.y2 - image.bounds.y1) * image.strideFloats;
    if ((index > UINT32_MAX) || (end > UINT32_MAX)) {
        return false;
    }
    *out = (std::uint32_t)index;

    return true;
}

/// The pass on the GPU over the CPU's buffer, io.domain: the source is copied into a scratch
/// buffer of that extent, zero outside the pixels that exist, and the detection reads it and
/// writes io.output of io.filtered, zero outside the buffer as the CPU writes it.
PassGpuRecorder
hotPixelsGpuRecorder(const RawHotPixelsOp::Params& params)
{
    RawHotPixelsGpuParams base = {};

    base.layout = toRawLayoutGpu(params.layout);
    base.threshold = params.threshold;
    base.multiplier = params.multiplier;
    base.minNeighbours = (std::uint32_t)params.minNeighbours;

    return [base](const PassGpuIO& io) -> StatusEnum {
        gpu::GpuRecordContext& ctx = io.context;
        const int nComps = io.nComps;
        const RectI& domain = io.domain;
        const bool hasDomain = !PassExec::isEmptyRect(domain);
        const std::uint64_t floats = hasDomain ? (std::uint64_t)domain.width() * domain.height() * nComps : 0;
        if (floats > UINT32_MAX) {
            return eStatusFailed;
        }
        gpu::GpuBuffer* buffer = nullptr;
        gpu::GpuBuffer* unread = nullptr;
        if (!ctx.scratch((VkDeviceSize)std::max<std::uint64_t>(floats, 4) * sizeof(float), buffer) || !buffer || !ctx.scratch(4 * sizeof(float), unread) || !unread) {
            return eStatusFailed;
        }
        RawHotPixelsGpuParams p = base;
        p.channels = (std::uint32_t)nComps;
        if (hasDomain) {
            p.domX1 = domain.x1;
            p.domY1 = domain.y1;
            p.domX2 = domain.x2;
            p.domY2 = domain.y2;

            RawHotPixelsGpuParams fill = p;
            const gpu::GpuBuffer* sourceBuffer = unread;
            if (io.source.buffer && !PassExec::isEmptyRect(io.sourceValid)) {
                const gpu::GpuBlurImage source = PassExec::toGpuBlurImage(io.source);
                sourceBuffer = io.source.buffer;
                fill.rectX1 = io.sourceValid.x1;
                fill.rectY1 = io.sourceValid.y1;
                fill.rectX2 = io.sourceValid.x2;
                fill.rectY2 = io.sourceValid.y2;
                fill.stride = source.strideFloats;
                if (!gpuFloatIndex(source, io.sourceValid.x1, io.sourceValid.y1, nComps, &fill.offset)) {
                    return eStatusFailed;
                }
            }
            if (!ctx.dispatch(gpu::GpuKernelId::rawhotpixels_fill, { sourceBuffer, buffer }, fill, { (std::uint32_t)domain.width(), (std::uint32_t)domain.height(), 1 })) {
                return eStatusFailed;
            }
            ctx.computeBarrier();
        }

        const RectI& output = io.output;
        const gpu::GpuBlurImage filtered = PassExec::toGpuBlurImage(io.filtered);
        p.rectX1 = output.x1;
        p.rectY1 = output.y1;
        p.rectX2 = output.x2;
        p.rectY2 = output.y2;
        p.stride = filtered.strideFloats;
        if (!gpuFloatIndex(filtered, output.x1, output.y1, nComps, &p.offset)) {
            return eStatusFailed;
        }

        return ctx.dispatch(gpu::GpuKernelId::rawhotpixels_detect, { buffer, filtered.buffer }, p, { (std::uint32_t)output.width(), (std::uint32_t)output.height(), 1 }) ? eStatusOK : eStatusFailed;
    };
}
#endif // NATRON_HAVE_GPU
} // namespace

void
RawHotPixelsOp::createKnobs(KnobHolder* holder,
                            const KnobPagePtr& page)
{
    KnobDoublePtr thresholdKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Threshold").toStdString());
    thresholdKnob->setName(kRawHotPixelsParamThreshold);
    thresholdKnob->setHintToolTip(EffectInstance::tr("Only photosites above this value can be hot, 1 being the white level.").toStdString());
    thresholdKnob->setDefaultValue(0.05);
    thresholdKnob->setMinimum(0.);
    thresholdKnob->setMaximum(1.);
    thresholdKnob->setDisplayMinimum(0.);
    thresholdKnob->setDisplayMaximum(1.);
    page->addKnob(thresholdKnob);
    threshold = thresholdKnob;

    KnobDoublePtr strengthKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Strength").toStdString());
    strengthKnob->setName(kRawHotPixelsParamStrength);
    strengthKnob->setHintToolTip(EffectInstance::tr("How readily a photosite counts as hot: it is hot when strength / 2 times its value is above its "
                                                    "neighbours. 0 turns the node off.")
                                     .toStdString());
    strengthKnob->setDefaultValue(0.25);
    strengthKnob->setMinimum(0.);
    strengthKnob->setMaximum(1.);
    strengthKnob->setDisplayMinimum(0.);
    strengthKnob->setDisplayMaximum(1.);
    page->addKnob(strengthKnob);
    strength = strengthKnob;

    KnobBoolPtr permissiveKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr("Detect By 3 Neighbours").toStdString());
    permissiveKnob->setName(kRawHotPixelsParamPermissive);
    permissiveKnob->setHintToolTip(EffectInstance::tr("A photosite is hot when it is above three of its four neighbours instead of all four, "
                                                      "which also catches pairs of adjacent hot pixels.")
                                       .toStdString());
    permissiveKnob->setDefaultValue(false);
    permissiveKnob->setAnimationEnabled(false);
    page->addKnob(permissiveKnob);
    permissive = permissiveKnob;

    KnobBoolPtr markFixedKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr("Mark Fixed").toStdString());
    markFixedKnob->setName(kRawHotPixelsParamMarkFixed);
    markFixedKnob->setHintToolTip(EffectInstance::tr("While the node's panel is open, marks each fixed pixel on the viewer with a short "
                                                     "horizontal line and shows how many pixels the view holds. The marks are drawn over the "
                                                     "image and never enter the render. The setting is not saved.")
                                      .toStdString());
    markFixedKnob->setDefaultValue(false);
    markFixedKnob->setAnimationEnabled(false);
    markFixedKnob->setHashExempt(true);
    markFixedKnob->setEvaluateOnChange(false);
    markFixedKnob->setIsPersistent(false);
    page->addKnob(markFixedKnob);
    markFixed = markFixedKnob;
}

RawHotPixelsOp::Params
RawHotPixelsOp::bake(const KnobReader& reader,
                     const ImageMetadata& source) const
{
    Params params;

    params.layout = RawLayout::fromMetadata(source).value_or(RawLayout());
    params.threshold = (float)reader.value(threshold);
    params.multiplier = (float)((double)(float)reader.value(strength) / 2.0);
    params.minNeighbours = reader.value(permissive) ? 3 : 4;
    if (params.layout.period() == 6) {
        params.xtrans = xtransNeighbours(params.layout);
    }

    return params;
}

bool
RawHotPixelsOp::isIdentity(const Params& params,
                           const PassContext& /*context*/) const
{
    return params.multiplier == 0.f;
}

PassList
RawHotPixelsOp::passes(const Params& params,
                       const PassContext& /*context*/) const
{
    PassList list;
    const int halo = params.layout.hasCfa() ? 2 : 1;

    list.boundary = PassExec::Boundary::Zero;
    list.scratchSlots = 2;
    list.resultSlot = 1;
    Pass pass = Pass::gather(halo, halo, true, hotPixelsWorker(params));
    pass.growX = 0;
    pass.growY = 0;
    pass.tapsPerSample = 5;
    list.passes.push_back(pass);
#ifdef NATRON_HAVE_GPU
    list.gpu = hotPixelsGpuRecorder(params);
#endif

    return list;
}

RawHotPixels::RawHotPixels(NodePtr node)
    : ImageNode<RawHotPixelsOp>(node)
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

RawHotPixels::~RawHotPixels()
{
}

std::size_t
RawHotPixels::fixedPixelCount(const OverlayImageFrame& frame)
{
    std::size_t count = 0;

    for (const OverlayImagePlane& plane : frame.planes) {
        if (plane.name != kRawHotPixelsMarkPlaneFixed) {
            continue;
        }
        for (const std::shared_ptr<const OverlayImageTile>& tile : plane.tiles) {
            count += (std::size_t)std::count_if(tile->values.begin(), tile->values.end(), [](float v) { return v > 0.f; });
        }
    }

    return count;
}

std::string
RawHotPixels::fixedPixelsLabel(std::size_t count)
{
    return (count == 1) ? std::string("fixed 1 pixel") : ("fixed " + std::to_string(count) + " pixels");
}

void
RawHotPixels::getRegionsOfInterest(double time,
                                   const RenderScale& scale,
                                   const RectD& outputRoD,
                                   const RectD& renderWindow,
                                   ViewIdx view,
                                   RoIMap* ret)
{
    ImageNode<RawHotPixelsOp>::getRegionsOfInterest(time, scale, outputRoD, renderWindow, view, ret);

    if (!isOverlayImageRender() || !isOverlayImageViewOn()) {
        return;
    }
    const EffectInstancePtr input = getInput(getSourceInput());
    if (!input) {
        return;
    }
    const double par = getRenderMetadata(time, view)->getPixelAspectRatio(getSourceInput());
    RectI need = PassExec::toPixelEnclosing(renderWindow, scale, par);
    const int halo = 2;
    need.x1 -= kMarkReach + halo;
    need.x2 += kMarkReach + halo;
    need.y1 -= halo;
    need.y2 += halo;
    (*ret)[input] = PassExec::toCanonical(need, scale, par);
}

bool
RawHotPixels::isOverlayImageViewOn() const
{
    const KnobBoolPtr knob = op().markFixed.lock();

    return knob && knob->getValue();
}

bool
RawHotPixels::hasOverlayDrawList() const
{
    return true;
}

void
RawHotPixels::buildOverlay(double time,
                           const RenderScale& /*renderScale*/,
                           ViewIdx view,
                           OverlayDrawList* list)
{
    const OverlayImageFrameConstPtr frame = getOverlayImages(time, view);

    if (!frame) {
        return;
    }
    if (frame != _countedFrame) {
        _countedFrame = frame;
        _fixedCount = fixedPixelCount(*frame);
    }
    const RectI& visible = frame->request.visibleCanonical;
    list->addText(visible.x1, visible.y2, fixedPixelsLabel(_fixedCount), OverlayColor::fromRGBA(1.f, 1.f, 0.f), 10., -24.);
}

bool
RawHotPixels::knobChanged(KnobI* k,
                          ValueChangedReasonEnum /*reason*/,
                          ViewSpec /*view*/,
                          double /*time*/,
                          bool /*originatedFromMainThread*/)
{
    if (!k || (k != op().markFixed.lock().get())) {
        return false;
    }
    refreshOverlayImages();

    return true;
}

StatusEnum
RawHotPixels::renderOp(const RenderActionArgs& args,
                       const StagePlan& plan)
{
    const StatusEnum status = ImageNode<RawHotPixelsOp>::renderOp(args, plan);

    if ((status == eStatusOK) && isOverlayImageRender() && isOverlayImageViewOn()) {
        publishMarks(args, plan);
    }

    return status;
}

void
RawHotPixels::publishMarks(const RenderActionArgs& args,
                           const StagePlan& plan)
{
    if (args.outputLayers.empty() || !args.outputLayers.front().second) {
        return;
    }
    RectD rodCanonical;
    bool isProjectFormat = false;
    if (getRegionOfDefinition_public(getRenderHash(), args.time, args.mappedScale, args.view, &rodCanonical, &isProjectFormat) == eStatusFailed) {
        return;
    }
    const RectI rod = PassExec::toPixelEnclosing(rodCanonical, args.mappedScale, getRenderMetadata(args.time, args.view)->getPixelAspectRatio(-1));
    RectI window;
    if (!PassExec::intersectRects(args.roi, rod, &window)) {
        return;
    }

    RawNodeSupport::LockedSource source;
    ImageLayerDesc sourceLayer;
    if ((plan.sourceInput < 0) || !getInput(plan.sourceInput) || !resolveInputPlaneForRender(plan.sourceInput, args.time, args.view, &sourceLayer, NULL)) {
        return;
    }
    RectI sourceRoI;
    source.image = getImage(plan.sourceInput, args.time, args.mappedScale, args.view, NULL, &sourceLayer, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &sourceRoI);
    if (!source.image || !PassExec::isFloatImage(source.image)) {
        return;
    }
    source.lock();

    const auto& [plane, dst] = args.outputLayers.front();
    const std::bitset<4> bits = PassExec::processedBitsForImage(plane, (int)dst->getComponentsCount(), args.processChannels);
    std::vector<int> channels;
    for (int bit = 0; bit < 4; ++bit) {
        const int channel = PassExec::channelIndexForBit(source.nComps, bit);
        if (bits[bit] && (channel >= 0)) {
            channels.push_back(channel);
        }
    }

    const RawHotPixelsOp::Params params = bakeParams(op(), args.time, args.view, getSourceMetadata(args.time, args.view));
    const int border = (params.layout.period() > 0) ? 2 : 1;
    const RectI candidates(std::max(window.x1 - kMarkReach, rod.x1 + border),
                           rod.y1 + border,
                           std::min(window.x2 + kMarkReach, rod.x2 - border),
                           rod.y2 - border);
    if ((candidates.x1 >= candidates.x2) || (candidates.y1 >= candidates.y2)) {
        return;
    }

    RenderCancellation cancel(this);
    const RawKernels::ParallelFor parallel = RawNodeSupport::makeParallelFor(&cancel, appPTR->getNCPUsAvailableForEffect());
    const std::vector<std::vector<int>> rows = fixedColumnsPerRow(params, source, channels, candidates, window, parallel);
    if (cancel.check()) {
        return;
    }

    const std::size_t width = (std::size_t)window.width();
    const std::size_t pixelCount = width * (std::size_t)window.height();
    std::vector<float> fixed;
    std::vector<float> arms;
    for (std::size_t r = 0; r < rows.size(); ++r) {
        const int y = window.y1 + (int)r;
        for (const int x : rows[r]) {
            if ((x >= window.x1) && (x < window.x2)) {
                fixed.resize(pixelCount, 0.f);
                fixed[r * width + (std::size_t)(x - window.x1)] = 1.f;
            }
            for (int offset = -kMarkReach; offset <= kMarkReach; ++offset) {
                const int markX = x + offset;
                if ((offset != 0) && (markX >= window.x1) && (markX < window.x2) && isMarkOffset(params.layout, x, y, offset)) {
                    arms.resize(pixelCount, 0.f);
                    arms[r * width + (std::size_t)(markX - window.x1)] = 1.f;
                }
            }
        }
    }

    const OverlayColor yellow = OverlayColor::fromRGBA(1.f, 1.f, 0.f);
    const unsigned int mipmapLevel = args.mappedScale.toMipmapLevel();
    if (!fixed.empty()) {
        addOverlayImageTile(kRawHotPixelsMarkPlaneFixed, yellow, window, mipmapLevel, std::move(fixed));
    }
    if (!arms.empty()) {
        addOverlayImageTile(kRawHotPixelsMarkPlaneArms, yellow, window, mipmapLevel, std::move(arms));
    }
} // RawHotPixels::publishMarks

NATRON_NAMESPACE_EXIT
