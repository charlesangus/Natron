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

#include "Glow.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Filter/Bloom.h"
#include "Engine/Nodes/Filter/BloomKernels.h"
#include "Engine/Nodes/Image/ColorMath.h"
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
const int kSlotSource = 0;
const int kSlotMap = 1;
const int kSlotGlow = 2;

enum ChannelKind : uint32_t {
    eChannelUnprocessed = 0,
    eChannelColour = 1,
    eChannelAlpha = 2,
};

/**
 * @brief What the map and the combination do with each channel of a pixel, by its bit in the
 * space of pixelKernelChannelBit(): its luma weight, its tint and its kind. The weights are the
 * working space's over the processed colour channels, renormalised to sum to 1, so that a grey
 * of value v has luminance v; with no processed colour channel, a processed alpha is its own
 * luminance.
 **/
struct ChannelSetup {
    float weight[4] = { 0.f, 0.f, 0.f, 0.f };
    float tint[4] = { 1.f, 1.f, 1.f, 1.f };
    ChannelKind kind[4] = { eChannelUnprocessed, eChannelUnprocessed, eChannelUnprocessed, eChannelUnprocessed };
};

/// bits[i] is the bit of channel i, processed[i] whether it is processed.
ChannelSetup
channelSetup(const GlowParams& params,
             const int* bits,
             const bool* processed,
             int n)
{
    ChannelSetup s;
    double sum = 0.;
    int nColour = 0;
    int alpha = -1;

    for (int i = 0; i < n; ++i) {
        if (!processed[i]) {
            continue;
        }
        if (bits[i] < 3) {
            s.kind[i] = eChannelColour;
            s.tint[i] = (float)params.tint[bits[i]];
            sum += params.luma[bits[i]];
            ++nColour;
        } else {
            s.kind[i] = eChannelAlpha;
            alpha = i;
        }
    }
    for (int i = 0; i < n; ++i) {
        if (s.kind[i] == eChannelColour) {
            s.weight[i] = (sum > 0.) ? (float)(params.luma[bits[i]] / sum) : (float)(1. / nColour);
        }
    }
    if (!nColour && (alpha >= 0)) {
        s.weight[alpha] = 1.f;
    }

    return s;
}

ChannelSetup
channelSetupOf(const GlowParams& params,
               const PassScratch& scratch)
{
    int bits[4] = { 0, 0, 0, 0 };
    bool processed[4] = { false, false, false, false };
    const int n = std::min(4, (int)scratch.channelBits.size());

    for (int p = 0; p < n; ++p) {
        bits[p] = scratch.channelBits[p];
        processed[p] = true;
    }

    return channelSetup(params, bits, processed, n);
}

/// 0 up to 0, a parabola of curvature 1 / k up to k, then x - k / 2: continuous with its slope.
inline float
softKnee(float x,
         float k)
{
    if (x <= 0.f) {
        return 0.f;
    }
    if ((k > 0.f) && (x < k)) {
        return x * x / (2.f * k);
    }

    return x - 0.5f * k;
}

/// The luminance l of a pixel and the map's scale c(l) / l, with c its kept luminance.
inline void
mapScale(const ChannelSetup& setup,
         const float* v,
         int n,
         float threshold,
         float knee,
         float* s,
         float* c)
{
    float l = 0.f;

    for (int i = 0; i < n; ++i) {
        l += setup.weight[i] * v[i];
    }
    *c = softKnee(l - threshold, knee);
    *s = ((l > 0.f) && (*c > 0.f)) ? (*c / l) : 0.f;
}

/// The bloom's blurs in pixels at the render scale: size across, size * (1 - stretch) down.
BloomKernels::Settings
settingsFor(const GlowParams& params,
            const PassContext& context)
{
    const OfxPointD rs = context.scale.toOfxPointD();
    BloomKernels::Settings s;

    s.sizeX = rs.x * (params.size / context.pixelAspectRatio);
    s.sizeY = rs.y * params.size * (1. - params.stretch);
    s.ratio = params.ratio;
    s.count = params.count;
    s.filter = params.filter;

    return s;
}

/// The graded map of slot 0 into slot 1, every row of every plane.
std::function<PassRowFn(const PassScratch&)>
mapWorker(const GlowParams& params)
{
    return [params](const PassScratch& scratch) -> PassRowFn {
        const ChannelSetup setup = channelSetupOf(params, scratch);

        return [&scratch, params, setup](int r) {
            const int n = std::min(4, scratch.nPlanes);
            const float threshold = (float)params.threshold;
            const float knee = (float)params.knee;
            const float saturation = (float)params.saturation;
            const float brightness = (float)params.brightness;
            const std::size_t row = (std::size_t)r * scratch.width;
            const float* in[4];
            float* out[4];
            for (int p = 0; p < n; ++p) {
                in[p] = scratch.plane(kSlotSource, p) + row;
                out[p] = scratch.plane(kSlotMap, p) + row;
            }
            for (int x = 0; x < scratch.width; ++x) {
                float v[4];
                for (int p = 0; p < n; ++p) {
                    v[p] = in[p][x];
                }
                float s;
                float c;
                mapScale(setup, v, n, threshold, knee, &s, &c);
                for (int p = 0; p < n; ++p) {
                    const float m = v[p] * s;
                    out[p][x] = (setup.kind[p] == eChannelColour) ? brightness * setup.tint[p] * (c + saturation * (m - c)) : brightness * m;
                }
            }
        };
    };
}

/// The output into slot 2 from the source, the map and, outside the preview, the bloom in slot 2.
std::function<PassRowFn(const PassScratch&)>
combineWorker(const GlowParams& params)
{
    return [params](const PassScratch& scratch) -> PassRowFn {
        const ChannelSetup setup = channelSetupOf(params, scratch);

        return [&scratch, params, setup](int r) {
            const int n = std::min(4, scratch.nPlanes);
            const float threshold = (float)params.threshold;
            const float knee = (float)params.knee;
            const bool gammaOn = (params.gamma != 1.);
            const float invGamma = (float)(1. / params.gamma);
            const std::size_t row = (std::size_t)r * scratch.width;
            const float* src[4];
            const float* map[4];
            float* out[4];
            for (int p = 0; p < n; ++p) {
                src[p] = scratch.plane(kSlotSource, p) + row;
                map[p] = scratch.plane(kSlotMap, p) + row;
                out[p] = scratch.plane(kSlotGlow, p) + row;
            }
            for (int x = 0; x < scratch.width; ++x) {
                if (params.preview) {
                    float v[4];
                    for (int p = 0; p < n; ++p) {
                        v[p] = src[p][x];
                    }
                    float s;
                    float c;
                    mapScale(setup, v, n, threshold, knee, &s, &c);
                    for (int p = 0; p < n; ++p) {
                        out[p][x] = v[p] * s;
                    }
                    continue;
                }
                for (int p = 0; p < n; ++p) {
                    float glow = out[p][x];
                    if (params.addInput) {
                        glow += map[p][x];
                    }
                    if (gammaOn && (glow > 0.f)) {
                        glow = std::pow(glow, invGamma);
                    }
                    const float a = src[p][x];
                    float v;
                    if (params.glowOnly) {
                        v = glow;
                    } else if (params.screen) {
                        v = ((a <= 1.f) || (glow <= 1.f)) ? (a + glow - a * glow) : std::max(a, glow);
                    } else {
                        v = a + glow;
                    }
                    if (setup.kind[p] == eChannelAlpha) {
                        v = std::min(std::max(v, 0.f), 1.f);
                    }
                    out[p][x] = v;
                }
            }
        };
    };
}

#ifdef NATRON_HAVE_GPU
// Mirrors GlowParams in Kernels/glow.slang, which has no padding between members.
struct GlowGpuParams {
    int32_t domX1;
    int32_t domY1;
    uint32_t domWidth;
    uint32_t domHeight;
    int32_t outX1;
    int32_t outY1;
    uint32_t outWidth;
    uint32_t outHeight;
    uint32_t outOffset;
    uint32_t outStride;
    uint32_t accStride;
    uint32_t channels;
    uint32_t flags;
    float threshold;
    float knee;
    float saturation;
    float brightness;
    float invGamma;
    uint32_t pad0;
    uint32_t pad1;
    float weight[4];
    float tint[4];
    uint32_t kind[4];
};
static_assert(sizeof(GlowGpuParams) == 128);
static_assert(offsetof(GlowGpuParams, flags) == 48);
static_assert(offsetof(GlowGpuParams, weight) == 80);
static_assert(offsetof(GlowGpuParams, kind) == 112);

constexpr uint32_t kGlowGpuPreview = 1;
constexpr uint32_t kGlowGpuGlowOnly = 2;
constexpr uint32_t kGlowGpuScreen = 4;
constexpr uint32_t kGlowGpuAddInput = 8;
constexpr uint32_t kGlowGpuGamma = 16;

bool
dispatchGlow(gpu::GpuRecordContext& ctx,
             gpu::GpuKernelId kernel,
             std::initializer_list<const gpu::GpuBuffer*> buffers,
             const GlowGpuParams& p,
             uint32_t width,
             uint32_t height)
{
    if (!width || !height) {
        return true;
    }
    if (!ctx.dispatch(kernel, buffers, p, { width, height, 1 })) {
        return false;
    }
    ctx.computeBarrier();

    return true;
}

PassGpuRecorder
glowGpuRecorder(const GlowParams& params,
                const BloomKernels::PlanPtr& plan)
{
    return [params, plan](const PassGpuIO& io) -> StatusEnum {
        gpu::GpuRecordContext& ctx = io.context;
        const int nComps = io.nComps;
        const RectI& domain = io.domain;
        const RectI& output = io.output;
        if ((nComps < 1) || (nComps > 4)) {
            return eStatusFailed;
        }

        int bits[4] = { 0, 0, 0, 0 };
        bool processed[4] = { false, false, false, false };
        for (int c = 0; c < nComps; ++c) {
            bits[c] = pixelKernelChannelBit(nComps, c);
            processed[c] = io.channels[bits[c]];
        }
        const ChannelSetup setup = channelSetup(params, bits, processed, nComps);

        GlowGpuParams p {};
        const bool emptyDomain = PassExec::isEmptyRect(domain);
        p.domX1 = domain.x1;
        p.domY1 = domain.y1;
        p.domWidth = emptyDomain ? 0u : (uint32_t)domain.width();
        p.domHeight = emptyDomain ? 0u : (uint32_t)domain.height();
        p.channels = (uint32_t)nComps;
        p.flags = (params.preview ? kGlowGpuPreview : 0u) | (params.glowOnly ? kGlowGpuGlowOnly : 0u) | (params.screen ? kGlowGpuScreen : 0u) | (params.addInput ? kGlowGpuAddInput : 0u) | ((params.gamma != 1.) ? kGlowGpuGamma : 0u);
        p.threshold = (float)params.threshold;
        p.knee = (float)params.knee;
        p.saturation = (float)params.saturation;
        p.brightness = (float)params.brightness;
        p.invGamma = (float)(1. / params.gamma);
        for (int c = 0; c < 4; ++c) {
            p.weight[c] = setup.weight[c];
            p.tint[c] = setup.tint[c];
            p.kind[c] = (uint32_t)setup.kind[c];
        }

        gpu::GpuBuffer* fill = nullptr;
        if (BloomKernels::recordFill(io, false, &fill) != eStatusOK) {
            return eStatusFailed;
        }
        gpu::GpuBuffer* map = nullptr;
        if (!BloomKernels::gpuScratch(ctx, domain, nComps, &map)) {
            return eStatusFailed;
        }
        if (!dispatchGlow(ctx, gpu::GpuKernelId::glow_map, { fill, map, fill, map }, p, p.domWidth, p.domHeight)) {
            return eStatusFailed;
        }
        if (PassExec::isEmptyRect(output)) {
            return eStatusOK;
        }

        gpu::GpuBuffer* acc = map;
        if (!params.preview) {
            if (!BloomKernels::gpuScratch(ctx, output, nComps, &acc)) {
                return eStatusFailed;
            }
            if (BloomKernels::recordGpu(*plan, io, map, BloomKernels::gpuScratchImage(acc, output, nComps)) != eStatusOK) {
                return eStatusFailed;
            }
        }

        const gpu::GpuBlurImage dst = PassExec::toGpuBlurImage(io.filtered);
        p.outX1 = output.x1;
        p.outY1 = output.y1;
        p.outWidth = (uint32_t)output.width();
        p.outHeight = (uint32_t)output.height();
        p.outStride = dst.strideFloats;
        p.accStride = (uint32_t)(output.width() * nComps);
        if (!BloomKernels::gpuFloatIndex(dst, output.x1, output.y1, nComps, &p.outOffset)) {
            return eStatusFailed;
        }

        return dispatchGlow(ctx, gpu::GpuKernelId::glow_combine, { fill, map, acc, dst.buffer }, p, p.outWidth, p.outHeight) ? eStatusOK : eStatusFailed;
    };
}
#endif // NATRON_HAVE_GPU

/// The map of slot 0 into slot 1, its bloom into slot 2 outside the preview, and the output over
/// it, with black outside the image.
PassList
glowPasses(const GlowParams& params,
           const PassContext& context)
{
    const BloomKernels::PlanPtr plan = BloomKernels::makePlan(settingsFor(params, context), false);
    PassList list;

    list.boundary = PassExec::Boundary::Zero;
    list.scratchSlots = 3;
    list.resultSlot = kSlotGlow;
    list.passes.push_back(Pass::point(false, mapWorker(params)));
    if (!params.preview) {
        const int growX = params.expandRoD ? BloomKernels::haloX(*plan) : 0;
        const int growY = params.expandRoD ? BloomKernels::haloY(*plan) : 0;
        BloomKernels::appendPasses(plan, kSlotMap, kSlotGlow, growX, growY, &list.passes);
    }
    list.passes.push_back(Pass::point(false, combineWorker(params)));

#ifdef NATRON_HAVE_GPU
    list.gpu = glowGpuRecorder(params, plan);
#endif

    return list;
}

KnobDoublePtr
addDoubleKnob(KnobHolder* holder,
              const KnobPagePtr& page,
              const char* name,
              const QString& label,
              const QString& hint,
              double defaultValue,
              double minimum,
              double displayMinimum,
              double displayMaximum)
{
    KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(holder, label);

    knob->setName(name);
    knob->setHintToolTip(hint);
    knob->setMinimum(minimum);
    knob->setDisplayMinimum(displayMinimum);
    knob->setDisplayMaximum(displayMaximum);
    knob->setDefaultValue(defaultValue);
    page->addKnob(knob);

    return knob;
}

KnobBoolPtr
addBoolKnob(KnobHolder* holder,
            const KnobPagePtr& page,
            const char* name,
            const QString& label,
            const QString& hint,
            bool newLine)
{
    KnobBoolPtr knob = AppManager::createKnob<KnobBool>(holder, label);

    knob->setName(name);
    knob->setHintToolTip(hint);
    knob->setDefaultValue(false);
    knob->setAddNewLine(newLine);
    page->addKnob(knob);

    return knob;
}
} // anonymous namespace

void
GlowOp::createKnobs(KnobHolder* holderArg,
                    const KnobPagePtr& page)
{
    holder = holderArg;

    threshold = addDoubleKnob(holder, page, kGlowParamThreshold, EffectInstance::tr("Threshold"),
                              EffectInstance::tr("The luminance above which the image glows, in the working space's scene-linear units. Higher values keep only the brightest areas."), 0.25, 0., 0., 1.);
    knee = addDoubleKnob(holder, page, kGlowParamKnee, EffectInstance::tr("Knee"),
                         EffectInstance::tr("The width of the soft knee above the threshold, over which the glow fades in instead of starting abruptly. 0 is a hard threshold."), 0.5, 0., 0., 1.);
    preview = addBoolKnob(holder, page, kGlowParamPreview, EffectInstance::tr("Glow Map Preview"),
                          EffectInstance::tr("Output the glow map, the highlights that glow, to adjust the threshold and the knee."), true);

    KnobDoublePtr sizeKnob = addDoubleKnob(holder, page, kGlowParamSize, EffectInstance::tr("Size"),
                                           EffectInstance::tr("Size (diameter) of the smallest blur of the glow, in pixel units. Each next blur is ratio times larger."), 3., 0., 0., 100.);
    sizeKnob->setMaximum(1000.);
    sizeKnob->setSpatial(true);
    size = sizeKnob;
    stretch = addDoubleKnob(holder, page, kGlowParamStretch, EffectInstance::tr("Stretch"),
                            EffectInstance::tr("Stretch the glow horizontally: 0 keeps it round, 1 blurs horizontally only."), 0., 0., 0., 1.);
    stretch.lock()->setMaximum(1.);
    ratio = addDoubleKnob(holder, page, kGlowParamRatio, EffectInstance::tr("Ratio"),
                          EffectInstance::tr("How many times larger each blur is than the previous one. At 1 or less, there is a single blur."), 2., 1., 1., 4.);

    KnobIntPtr countKnob = AppManager::createKnob<KnobInt>(holder, EffectInstance::tr("Count"));
    countKnob->setName(kGlowParamCount);
    countKnob->setHintToolTip(EffectInstance::tr("The number of blurs averaged into the glow. More give a wider, heavier tail."));
    countKnob->setMinimum(1);
    countKnob->setDisplayMinimum(1);
    countKnob->setDisplayMaximum(10);
    countKnob->setDefaultValue(7);
    page->addKnob(countKnob);
    count = countKnob;

    filter = Bloom::createFilterKnob(holder, page, kGlowParamFilter);

    KnobColorPtr tintKnob = AppManager::createKnob<KnobColor>(holder, EffectInstance::tr("Tint"), 3);
    tintKnob->setName(kGlowParamTint);
    tintKnob->setHintToolTip(EffectInstance::tr("The colour the glow is multiplied by."));
    for (int d = 0; d < 3; ++d) {
        tintKnob->setDefaultValue(1., d);
        tintKnob->setMinimum(-DBL_MAX, d);
        tintKnob->setMaximum(DBL_MAX, d);
        tintKnob->setDisplayMinimum(0., d);
        tintKnob->setDisplayMaximum(4., d);
    }
    tintKnob->setColorRole(eColorKnobRoleGain);
    page->addKnob(tintKnob);
    tint = tintKnob;

    brightness = addDoubleKnob(holder, page, kGlowParamBrightness, EffectInstance::tr("Brightness"),
                               EffectInstance::tr("The gain of the glow, on top of the tint."), 1., -DBL_MAX, 0., 4.);
    saturation = addDoubleKnob(holder, page, kGlowParamSaturation, EffectInstance::tr("Saturation"),
                               EffectInstance::tr("The saturation of the glow, about its luminance: 0 makes it grey, 1 keeps the source's colours."), 1., -DBL_MAX, 0., 4.);
    gamma = addDoubleKnob(holder, page, kGlowParamGamma, EffectInstance::tr("Gamma"),
                          EffectInstance::tr("The gamma of the glow, applied after the bloom. Values at or below 0 are not affected. A gamma below 0.01 is treated as 0.01."), 1., 0., 0.2, 5.);

    screen = addBoolKnob(holder, page, kGlowParamScreen, EffectInstance::tr("Screen"),
                         EffectInstance::tr("Screen the glow over the input, which preserves its highlights, instead of adding it."), false);
    addInput = addBoolKnob(holder, page, kGlowParamAddInput, EffectInstance::tr("Add Input"),
                           EffectInstance::tr("Add the glow map itself to its bloom, which sharpens the core of the glow."), false);
    glowOnly = addBoolKnob(holder, page, kGlowParamGlowOnly, EffectInstance::tr("Glow Only"),
                           EffectInstance::tr("Output the glow alone, to add or screen it over the source later."), false);
    expandRoD = addBoolKnob(holder, page, kGlowParamExpandRoD, EffectInstance::tr("Expand RoD"),
                            EffectInstance::tr("Grow the region of definition by the bloom's reach, so the glow spills past the input's edges instead of being cropped. Large sizes and counts make it very large."), true);
} // GlowOp::createKnobs

GlowOp::Params
GlowOp::bake(const KnobReader& reader) const
{
    Params params;

    params.threshold = std::max(0., reader.value(threshold));
    params.knee = std::max(0., reader.value(knee));
    params.preview = reader.value(preview);
    params.size = reader.value(size);
    params.stretch = std::min(std::max(reader.value(stretch), 0.), 1.);
    params.ratio = reader.value(ratio);
    params.count = reader.value(count);
    const int f = reader.value(filter);
    if ((f >= (int)BlurKernels::eFilterQuasiGaussian) && (f <= (int)BlurKernels::eFilterFIRGaussian)) {
        params.filter = (BlurKernels::Filter)f;
    }
    for (int d = 0; d < 3; ++d) {
        params.tint[d] = reader.value(tint, d);
    }
    params.brightness = reader.value(brightness);
    params.saturation = reader.value(saturation);
    params.gamma = std::max(0.01, reader.value(gamma));
    params.screen = reader.value(screen);
    params.addInput = reader.value(addInput);
    params.glowOnly = reader.value(glowOnly);
    params.expandRoD = reader.value(expandRoD);

    const double* fallback = ColorMath::acesCgLumaWeights();
    for (int d = 0; d < 3; ++d) {
        params.luma[d] = fallback[d];
    }
    AppInstancePtr app = holder ? holder->getApp() : AppInstancePtr();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    ProjectColorManagementPtr cm = project ? project->getColorManagement() : ProjectColorManagementPtr();
    if (cm) {
        cm->getWorkingLuma(params.luma);
    }

    return params;
} // GlowOp::bake

bool
GlowOp::isIdentity(const Params& /*params*/,
                   const PassContext& /*context*/) const
{
    return false;
}

PassList
GlowOp::passes(const Params& params,
               const PassContext& context) const
{
    return glowPasses(params, context);
}

Glow::Glow(NodePtr node)
    : ImageNode<GlowOp>(node)
{
}

Glow::~Glow()
{
}

RectI
Glow::getSourceRoI(const RectI& rect,
                   const RenderScale& scale,
                   const GlowParams& params)
{
    PassContext context;

    context.scale = scale;

    return passListSourceRoI(rect, glowPasses(params, context));
}

NATRON_NAMESPACE_EXIT
