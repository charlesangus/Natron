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

#include "AdjustBBox.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/NodeMetadata.h"
#include "Engine/RectD.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
#ifdef NATRON_HAVE_GPU
// AdjustBBoxParams in adjustbbox.slang.
struct AdjustBBoxGpuParams {
    // The source pixels that exist, the first at srcOffset; empty when there are none.
    std::int32_t srcX1;
    std::int32_t srcY1;
    std::int32_t srcX2;
    std::int32_t srcY2;
    // The rect written, the first pixel at dstOffset.
    std::int32_t dstX1;
    std::int32_t dstY1;
    std::int32_t dstX2;
    std::int32_t dstY2;
    std::uint32_t srcOffset;
    std::uint32_t srcStride;
    std::uint32_t dstOffset;
    std::uint32_t dstStride;
    std::uint32_t channels;
    std::uint32_t padding0;
    std::uint32_t padding1;
    std::uint32_t padding2;
};
static_assert(sizeof(AdjustBBoxGpuParams) == 64, "must match AdjustBBoxParams in adjustbbox.slang");

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

// Writes io.output of io.filtered from the source clamped to the pixels that exist, zero when
// there are none: the CPU's nearest fill, which the pass list's halo keeps region-independent.
StatusEnum
recordNearestCopy(const PassGpuIO& io)
{
    gpu::GpuRecordContext& ctx = io.context;
    const RectI& output = io.output;

    if (PassExec::isEmptyRect(output)) {
        return eStatusOK;
    }
    AdjustBBoxGpuParams p = {};
    p.channels = (std::uint32_t)io.nComps;
    p.dstX1 = output.x1;
    p.dstY1 = output.y1;
    p.dstX2 = output.x2;
    p.dstY2 = output.y2;
    const gpu::GpuBlurImage filtered = PassExec::toGpuBlurImage(io.filtered);
    p.dstStride = filtered.strideFloats;
    if (!gpuFloatIndex(filtered, output.x1, output.y1, io.nComps, &p.dstOffset)) {
        return eStatusFailed;
    }

    const gpu::GpuBuffer* sourceBuffer = nullptr;
    if (io.source.buffer && !PassExec::isEmptyRect(io.sourceValid)) {
        const gpu::GpuBlurImage source = PassExec::toGpuBlurImage(io.source);
        sourceBuffer = io.source.buffer;
        p.srcX1 = io.sourceValid.x1;
        p.srcY1 = io.sourceValid.y1;
        p.srcX2 = io.sourceValid.x2;
        p.srcY2 = io.sourceValid.y2;
        p.srcStride = source.strideFloats;
        if (!gpuFloatIndex(source, io.sourceValid.x1, io.sourceValid.y1, io.nComps, &p.srcOffset)) {
            return eStatusFailed;
        }
    } else {
        gpu::GpuBuffer* unread = nullptr;
        if (!ctx.scratch(4 * sizeof(float), unread) || !unread) {
            return eStatusFailed;
        }
        sourceBuffer = unread;
    }

    return ctx.dispatch(gpu::GpuKernelId::adjustbbox_nearest, { sourceBuffer, filtered.buffer }, p, { (std::uint32_t)output.width(), (std::uint32_t)output.height(), 1 }) ? eStatusOK : eStatusFailed;
}
#endif // NATRON_HAVE_GPU
} // anonymous namespace

void
AdjustBBoxOp::createKnobs(KnobHolder* holder,
                          const KnobPagePtr& page)
{
    KnobDoublePtr numPixelsKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Num Pixels"), 2);
    numPixelsKnob->setName(kAdjustBBoxParamNumPixels);
    numPixelsKnob->setHintToolTip(EffectInstance::tr("How far to move each side of the region of definition outwards, horizontally and vertically, in pixels at full resolution. Negative values crop."));
    numPixelsKnob->setSpatial(true);
    numPixelsKnob->setCanAutoFoldDimensions(true);
    for (int d = 0; d < 2; ++d) {
        numPixelsKnob->setDisplayMinimum(-100., d);
        numPixelsKnob->setDisplayMaximum(100., d);
        numPixelsKnob->setIncrement(1., d);
        numPixelsKnob->setDefaultValue(0., d);
    }
    numPixelsKnob->setDimensionName(0, "w");
    numPixelsKnob->setDimensionName(1, "h");
    page->addKnob(numPixelsKnob);
    numPixels = numPixelsKnob;

    KnobChoicePtr boundaryKnob = AppManager::createKnob<KnobChoice>(holder, EffectInstance::tr("Boundary"));
    boundaryKnob->setName(kAdjustBBoxParamBoundary);
    boundaryKnob->setHintToolTip(EffectInstance::tr("What fills the border added around the input."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kAdjustBBoxBoundaryBlack, EffectInstance::tr("Black").toStdString(), EffectInstance::tr("Transparent black.").toStdString()));
        options.push_back(ChoiceOption(kAdjustBBoxBoundaryNearest, EffectInstance::tr("Nearest").toStdString(), EffectInstance::tr("The nearest pixel of the input's edge.").toStdString()));
        boundaryKnob->populateChoices(options);
    }
    boundaryKnob->setDefaultValue((int)eBoundaryBlack);
    page->addKnob(boundaryKnob);
    boundary = boundaryKnob;
}

AdjustBBoxOp::Params
AdjustBBoxOp::bake(const KnobReader& reader) const
{
    Params params;

    params.padX = reader.valueOr(numPixels, 0., 0);
    params.padY = reader.valueOr(numPixels, 0., 1);
    params.nearest = (reader.valueOr(boundary, (int)eBoundaryBlack) == (int)eBoundaryNearest);

    return params;
}

bool
AdjustBBoxOp::isIdentity(const Params& params,
                         const PassContext& /*context*/) const
{
    return (params.padX == 0.) && (params.padY == 0.);
}

PassList
AdjustBBoxOp::passes(const Params& params,
                     const PassContext& context) const
{
    PassList list;

    if (!params.nearest) {
        return list;
    }
    const double par = (context.pixelAspectRatio > 0.) ? context.pixelAspectRatio : 1.;
    const OfxPointD scale = context.scale.toOfxPointD();
    const int haloX = (int)std::ceil(std::max(0., params.padX) * scale.x / par);
    const int haloY = (int)std::ceil(std::max(0., params.padY) * scale.y);
    if ((haloX == 0) && (haloY == 0)) {
        return list;
    }
    list.boundary = PassExec::Boundary::Nearest;
    // No work: it only widens what is read, so that every border pixel's nearest source pixel is
    // read whatever the window.
    Pass reach;
    reach.kind = PassKind::Gather;
    reach.haloX = haloX;
    reach.haloY = haloY;
    list.passes.push_back(reach);
#ifdef NATRON_HAVE_GPU
    list.gpu = recordNearestCopy;
#endif

    return list;
}

AdjustBBox::AdjustBBox(NodePtr node)
    : ImageNode<AdjustBBoxOp>(node)
{
}

AdjustBBox::~AdjustBBox()
{
}

bool
AdjustBBox::clipRegionOfDefinition(double time,
                                   const RenderScale& scale,
                                   ViewIdx view,
                                   const EffectInstancePtr& source,
                                   const RectI& srcRoD,
                                   RectI* rod)
{
    if (!source || PassExec::isEmptyRect(srcRoD)) {
        return false;
    }
    KnobDoublePtr numPixelsKnob = op().numPixels.lock();
    const double padX = numPixelsKnob ? numPixelsKnob->getValueAtTime(time, 0, view) : 0.;
    const double padY = numPixelsKnob ? numPixelsKnob->getValueAtTime(time, 1, view) : 0.;
    const RenderScale inputScale = source->supportsRenderScale() ? scale : RenderScale::identity;
    RectD canonical;
    bool isProjectFormat = false;
    if (source->getRegionOfDefinition_public(source->getRenderHash(), time, inputScale, view, &canonical, &isProjectFormat) == eStatusFailed) {
        return false;
    }
    canonical.x1 -= padX;
    canonical.x2 += padX;
    canonical.y1 -= padY;
    canonical.y2 += padY;
    if ((canonical.x2 <= canonical.x1) || (canonical.y2 <= canonical.y1)) {
        *rod = RectI();

        return true;
    }
    *rod = PassExec::toPixelEnclosing(canonical, scale, getRenderMetadata(time, view)->getPixelAspectRatio(-1));

    return true;
}

NATRON_NAMESPACE_EXIT
