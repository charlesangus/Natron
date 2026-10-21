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

#include "ErodeDilate.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <limits>
#include <list>
#include <memory>
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

namespace ErodeDilateKernels {
namespace {
    inline float
    reduce(float a,
           float b,
           bool takeMax)
    {
        return takeMax ? ((b > a) ? b : a) : ((b < a) ? b : a);
    }
} // anonymous namespace

void
filterLine(float* data,
           int length,
           std::ptrdiff_t stride,
           int halfWidth,
           bool takeMax,
           LineScratch& scratch)
{
    if ((halfWidth <= 0) || (length <= 1)) {
        return;
    }
    if (length <= halfWidth + 2) {
        float extremum = data[0];
        for (int i = 1; i < length; ++i) {
            extremum = reduce(extremum, data[(std::ptrdiff_t)i * stride], takeMax);
        }
        for (int i = 0; i < length; ++i) {
            data[(std::ptrdiff_t)i * stride] = extremum;
        }

        return;
    }

    const int width = 2 * halfWidth + 1;
    const int n = length + 2 * halfWidth;
    const float neutral = takeMax ? -std::numeric_limits<float>::infinity() : std::numeric_limits<float>::infinity();
    scratch.padded.resize(n);
    scratch.prefix.resize(n);
    scratch.suffix.resize(n);
    float* padded = &scratch.padded[0];
    float* prefix = &scratch.prefix[0];
    float* suffix = &scratch.suffix[0];

    std::fill(padded, padded + halfWidth, neutral);
    for (int i = 0; i < length; ++i) {
        padded[halfWidth + i] = data[(std::ptrdiff_t)i * stride];
    }
    std::fill(padded + halfWidth + length, padded + n, neutral);

    // Running extremum inside each block of `width` values, from the block start and from its end:
    // a window spans at most two blocks, so it is the combination of one suffix and one prefix.
    for (int j = 0; j < n; ++j) {
        prefix[j] = ((j % width) == 0) ? padded[j] : reduce(prefix[j - 1], padded[j], takeMax);
    }
    for (int j = n - 1; j >= 0; --j) {
        suffix[j] = ((j == n - 1) || (((j + 1) % width) == 0)) ? padded[j] : reduce(suffix[j + 1], padded[j], takeMax);
    }
    for (int i = 0; i < length; ++i) {
        data[(std::ptrdiff_t)i * stride] = reduce(suffix[i], prefix[i + width - 1], takeMax);
    }
}

void
filterColumns(float* data,
              int length,
              std::ptrdiff_t stride,
              int count,
              int halfWidth,
              bool takeMax,
              LineScratch& scratch)
{
    if ((halfWidth <= 0) || (length <= 1) || (count <= 0)) {
        return;
    }
    if (length <= halfWidth + 2) {
        for (int c = 0; c < count; ++c) {
            filterLine(data + c, length, stride, halfWidth, takeMax, scratch);
        }

        return;
    }

    // filterLine() with each value widened to kColumnBlock lanes, one per column, so that the
    // inner loops vectorise. Lanes past `count` carry the neutral value and are not written back.
    const int B = kColumnBlock;
    const int width = 2 * halfWidth + 1;
    const int n = length + 2 * halfWidth;
    const float neutral = takeMax ? -std::numeric_limits<float>::infinity() : std::numeric_limits<float>::infinity();
    const std::size_t size = (std::size_t)n * B;
    scratch.padded.resize(size);
    scratch.prefix.resize(size);
    scratch.suffix.resize(size);
    float* padded = &scratch.padded[0];
    float* prefix = &scratch.prefix[0];
    float* suffix = &scratch.suffix[0];

    for (int c0 = 0; c0 < count; c0 += B) {
        const int nc = std::min(B, count - c0);
        float* block = data + c0;

        std::fill(padded, padded + (std::size_t)halfWidth * B, neutral);
        for (int i = 0; i < length; ++i) {
            const float* src = block + (std::ptrdiff_t)i * stride;
            float* dst = padded + (std::size_t)(halfWidth + i) * B;
            for (int c = 0; c < nc; ++c) {
                dst[c] = src[c];
            }
            for (int c = nc; c < B; ++c) {
                dst[c] = neutral;
            }
        }
        std::fill(padded + (std::size_t)(halfWidth + length) * B, padded + size, neutral);

        for (int j = 0; j < n; ++j) {
            const float* in = padded + (std::size_t)j * B;
            float* out = prefix + (std::size_t)j * B;
            if ((j % width) == 0) {
                for (int c = 0; c < B; ++c) {
                    out[c] = in[c];
                }
            } else {
                const float* prev = out - B;
                for (int c = 0; c < B; ++c) {
                    out[c] = reduce(prev[c], in[c], takeMax);
                }
            }
        }
        for (int j = n - 1; j >= 0; --j) {
            const float* in = padded + (std::size_t)j * B;
            float* out = suffix + (std::size_t)j * B;
            if ((j == n - 1) || (((j + 1) % width) == 0)) {
                for (int c = 0; c < B; ++c) {
                    out[c] = in[c];
                }
            } else {
                const float* next = out + B;
                for (int c = 0; c < B; ++c) {
                    out[c] = reduce(next[c], in[c], takeMax);
                }
            }
        }
        for (int i = 0; i < length; ++i) {
            const float* s = suffix + (std::size_t)i * B;
            const float* e = prefix + (std::size_t)(i + width - 1) * B;
            float* dst = block + (std::ptrdiff_t)i * stride;
            for (int c = 0; c < nc; ++c) {
                dst[c] = reduce(s[c], e[c], takeMax);
            }
        }
    }
} // filterColumns
} // namespace ErodeDilateKernels

namespace {
/// Runs filterLine() over each line the executor hands over, or filterColumns() over a block of
/// adjacent columns, through a scratch owned by the chunk.
std::function<PassLineFn()>
lineWorker(int halfWidth,
           bool takeMax)
{
    if (halfWidth <= 0) {
        return std::function<PassLineFn()>();
    }

    return [halfWidth, takeMax]() -> PassLineFn {
        std::shared_ptr<ErodeDilateKernels::LineScratch> scratch = std::make_shared<ErodeDilateKernels::LineScratch>();

        return [halfWidth, takeMax, scratch](float* line, int length, std::ptrdiff_t stride, int count) {
            if (count == 1) {
                ErodeDilateKernels::filterLine(line, length, stride, halfWidth, takeMax, *scratch);
            } else {
                ErodeDilateKernels::filterColumns(line, length, stride, count, halfWidth, takeMax, *scratch);
            }
        };
    };
}

#ifdef NATRON_HAVE_GPU
/// The source the kernels read. With no source pixels in the extent the executor hands over no
/// buffer, but the line kernels still bind one: a placeholder, which the empty valid rect keeps
/// unread.
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

/// The window of the source as the CPU buffer holds it, for a list whose passes all have a half
/// width of zero: recordMorphology() takes at least one pass, and the line kernel's identity
/// filter copies the window, zero where the buffer has no pixel.
StatusEnum
recordUnfiltered(const PassGpuIO& io)
{
    gpu::GpuBlurImage source;
    if (!gpuSourceImage(io, &source)) {
        return eStatusFailed;
    }
    gpu::GpuBuffer* window = nullptr;
    if (!gpu::recordFilterWindow(io.context, PassExec::toGpuBlurRect(io.output), PassExec::toGpuBlurRect(io.rod), io.haloX, io.haloY, window)) {
        return eStatusFailed;
    }
    gpu::GpuLineBlur copy;
    copy.src = source;
    copy.srcValid = PassExec::toGpuBlurRect(io.sourceValid);
    copy.dst = PassExec::toGpuBlurImage(io.filtered);
    copy.output = PassExec::toGpuBlurRect(io.output);
    copy.domain = PassExec::toGpuBlurRect(io.domain);
    copy.window = window;
    copy.channels = (uint32_t)io.nComps;

    return gpu::recordLineBlur(io.context, copy) ? eStatusOK : eStatusFailed;
}

PassGpuRecorder
morphologyGpuRecorder(std::vector<gpu::GpuMorphPass> passes)
{
    return [passes](const PassGpuIO& io) -> StatusEnum {
        if (passes.empty()) {
            return recordUnfiltered(io);
        }
        gpu::GpuMorphology morph;
        morph.src = PassExec::toGpuBlurImage(io.source);
        morph.dst = PassExec::toGpuBlurImage(io.filtered);
        morph.output = PassExec::toGpuBlurRect(io.output);
        morph.rod = PassExec::toGpuBlurRect(io.rod);
        morph.haloX = io.haloX;
        morph.haloY = io.haloY;
        morph.passes = passes;
        morph.channels = (uint32_t)io.nComps;

        return gpu::recordMorphology(io.context, morph) ? eStatusOK : eStatusFailed;
    };
}
#endif // NATRON_HAVE_GPU

bool
isIdentityAt(const RenderScale& scale,
             const ErodeDilateParams& params)
{
    const OfxPointD rs = scale.toOfxPointD();

    return (std::floor(params.sizeX * rs.x) == 0) && (std::floor(params.sizeY * rs.y) == 0);
}

/// The halo of an axis, which the one pass running on that axis carries.
int
axisHalo(int size,
         double scale)
{
    return (int)std::ceil(std::abs(size) * scale);
}

PassList
erodeDilatePasses(const ErodeDilateParams& params,
                  const RenderScale& scale,
                  bool dilate)
{
    const OfxPointD rs = scale.toOfxPointD();
    PassList list;
#ifdef NATRON_HAVE_GPU
    std::vector<gpu::GpuMorphPass> gpuPasses;
    bool gpuFits = true;
#endif

    // The operation of the positive axes first, then the opposite one on the negative axes.
    for (int ph = 0; ph < 2; ++ph) {
        const int sign = (ph == 0) ? 1 : -1;
        const bool takeMax = (ph == 0) ? dilate : !dilate;
        const bool onX = (sign * params.sizeX) > 0;
        const bool onY = (sign * params.sizeY) > 0;
        const int halfX = (int)std::floor(std::max(0, sign * params.sizeX) * rs.x);
        const int halfY = (int)std::floor(std::max(0, sign * params.sizeY) * rs.y);

        if (onX) {
            const int halo = axisHalo(params.sizeX, rs.x);
            list.passes.push_back(Pass::lineX(halo, takeMax ? halo : 0, lineWorker(halfX, takeMax)));
        }
        if (onY) {
            const int halo = axisHalo(params.sizeY, rs.y);
            Pass pass = Pass::lineY(halo, takeMax ? halo : 0, lineWorker(halfY, takeMax));
            pass.columnBlock = ErodeDilateKernels::kColumnBlock;
            list.passes.push_back(pass);
        }
#ifdef NATRON_HAVE_GPU
        if (halfX > 0) {
            gpuPasses.push_back(gpu::GpuMorphPass { (uint32_t)halfX, false, takeMax });
        }
        if (halfY > 0) {
            gpuPasses.push_back(gpu::GpuMorphPass { (uint32_t)halfY, true, takeMax });
        }
        gpuFits = gpuFits && (halfX <= (int)gpu::kGpuMorphMaxRadius) && (halfY <= (int)gpu::kGpuMorphMaxRadius);
#endif
    }

#ifdef NATRON_HAVE_GPU
    if (gpuFits) {
        list.gpu = morphologyGpuRecorder(std::move(gpuPasses));
    }
#endif

    return list;
} // erodeDilatePasses
} // anonymous namespace

template <ErodeDilateMode MODE>
void
ErodeDilateOp<MODE>::createKnobs(KnobHolder* holder,
                                 const KnobPagePtr& page)
{
    KnobIntPtr sizeKnob = AppManager::createKnob<KnobInt>(holder, EffectInstance::tr("Size"), 2);
    sizeKnob->setName(kErodeDilateParamSize);
    sizeKnob->setHintToolTip(EffectInstance::tr("Width/height of the rectangular structuring element is 2*size+1, in pixel units (>=0)."));
    for (int d = 0; d < 2; ++d) {
        sizeKnob->setMinimum(-1000, d);
        sizeKnob->setMaximum(1000, d);
        sizeKnob->setDisplayMinimum(-100, d);
        sizeKnob->setDisplayMaximum(100, d);
        sizeKnob->setDefaultValue(1, d);
    }
    page->addKnob(sizeKnob);
    size = sizeKnob;

    KnobBoolPtr expandRoDKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr("Expand RoD"));
    expandRoDKnob->setName(kErodeDilateParamExpandRoD);
    if (kDilate) {
        expandRoDKnob->setHintToolTip(EffectInstance::tr("Expand the source region of definition by 2*size pixels if size is positive"));
    } else {
        expandRoDKnob->setHintToolTip(EffectInstance::tr("Expand the source region of definition by 2*size pixels if size is negative"));
    }
    expandRoDKnob->setDefaultValue(true);
    page->addKnob(expandRoDKnob);
    expandRoD = expandRoDKnob;
}

template <ErodeDilateMode MODE>
typename ErodeDilateOp<MODE>::Params
ErodeDilateOp<MODE>::bake(const KnobReader& reader) const
{
    Params params;

    params.sizeX = reader.value(size, 0);
    params.sizeY = reader.value(size, 1);
    params.expandRoD = reader.value(expandRoD);

    return params;
}

template <ErodeDilateMode MODE>
bool
ErodeDilateOp<MODE>::isIdentity(const Params& params,
                                const PassContext& context) const
{
    return isIdentityAt(context.scale, params);
}

template <ErodeDilateMode MODE>
PassList
ErodeDilateOp<MODE>::passes(const Params& params,
                            const PassContext& context) const
{
    return erodeDilatePasses(params, context.scale, kDilate);
}

template <ErodeDilateMode MODE>
std::optional<GpuCostHint>
ErodeDilateOp<MODE>::costHint(const Params& params,
                              const PassContext& context) const
{
    GpuCostHint hint;

    hint.size = (int)std::floor(std::max(std::abs(params.sizeX), std::abs(params.sizeY)) * context.scale.toOfxPointD().x);

    return hint;
}

template struct ErodeDilateOp<ErodeDilateMode::Erode>;
template struct ErodeDilateOp<ErodeDilateMode::Dilate>;

template <ErodeDilateMode MODE>
ErodeDilateNode<MODE>::ErodeDilateNode(NodePtr node)
    : ImageNode<ErodeDilateOp<MODE>>(node)
{
}

template <ErodeDilateMode MODE>
ErodeDilateNode<MODE>::~ErodeDilateNode()
{
}

template <ErodeDilateMode MODE>
void
ErodeDilateNode<MODE>::addAcceptedComponents(int inputNb,
                                             std::list<ImageLayerDesc>* comps)
{
    ImageNode<ErodeDilateOp<MODE>>::addAcceptedComponents(inputNb, comps);
    if (!this->isInputMask(inputNb)) {
        comps->push_back(ImageLayerDesc::getXYComponents());
    }
}

template <ErodeDilateMode MODE>
void
ErodeDilateNode<MODE>::getParams(double time,
                                 ViewIdx view,
                                 ErodeDilateParams* params) const
{
    *params = bakeParams(this->op(), time, view);
}

template <ErodeDilateMode MODE>
RectI
ErodeDilateNode<MODE>::getSourceRoI(const RectI& rect,
                                    const RenderScale& scale,
                                    const ErodeDilateParams& params)
{
    return passListSourceRoI(rect, erodeDilatePasses(params, scale, ErodeDilateOp<MODE>::kDilate));
}

template <ErodeDilateMode MODE>
bool
ErodeDilateNode<MODE>::paramsAreIdentity(const RenderScale& scale,
                                         const ErodeDilateParams& params)
{
    return isIdentityAt(scale, params);
}

template <ErodeDilateMode MODE>
void
ErodeDilateNode<MODE>::getExpansion(const RenderScale& scale,
                                    const ErodeDilateParams& params,
                                    bool dilate,
                                    int* deltaX,
                                    int* deltaY)
{
    const PassExtent extent = passListExtent(erodeDilatePasses(params, scale, dilate));

    *deltaX = extent.growX;
    *deltaY = extent.growY;
}

template <ErodeDilateMode MODE>
bool
ErodeDilateNode<MODE>::clipRegionOfDefinition(double time,
                                              const RenderScale& /*scale*/,
                                              ViewIdx view,
                                              const EffectInstancePtr& /*source*/,
                                              const RectI& srcRoD,
                                              RectI* rod)
{
    ErodeDilateParams params;

    getParams(time, view, &params);
    if (params.expandRoD || (*rod == srcRoD)) {
        return false;
    }
    *rod = srcRoD;

    return true;
}

template class ErodeDilateNode<ErodeDilateMode::Erode>;
template class ErodeDilateNode<ErodeDilateMode::Dilate>;

NATRON_NAMESPACE_EXIT
