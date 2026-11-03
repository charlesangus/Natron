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

#include "StagePlan.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QString>

#include "Engine/AppManager.h"
#include "Engine/GpuContext.h"
#include "Engine/Image.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/PassExecutor.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/RectD.h"

#ifdef NATRON_HAVE_GPU
#include <span>

#include "Engine/Gpu/GpuBuffer.h"
#include "Engine/Gpu/GpuKernel.h"
#include "Engine/Gpu/GpuPipelineCache.h"
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Gpu/GpuRuntime.h"
#include "Engine/Gpu/GpuTexture.h"
#include "Engine/Gpu/GpuWrap.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
// How many rows runStagesCpu() runs between two abort checks.
const int kAbortCheckRows = 16;

struct RowBand {
    std::size_t job;
    int y1;
    int y2;

    RowBand(std::size_t jobIndex,
            int firstRow,
            int endRow)
        : job(jobIndex)
        , y1(firstRow)
        , y2(endRow)
    {
    }
};

void
resolvePlanMask(EffectInstance& effect,
                bool applyMask,
                StagePlan* plan)
{
    plan->applyMask = false;
    plan->maskInput = -1;
    plan->maskLayer = ImageLayerDesc();
    plan->maskChannel = -1;
    const int maskInput = stageMaskInput(effect);
    if (!applyMask || (maskInput < 0)) {
        return;
    }
    plan->applyMask = true;
    plan->maskInput = maskInput;
    ImageLayerDesc maskLayer;
    int maskChannel = -1;
    if (effect.resolveInputPlaneForRender(maskInput, plan->time, plan->view, &maskLayer, &maskChannel) && (maskChannel >= 0)) {
        plan->maskLayer = maskLayer;
        plan->maskChannel = maskChannel;
    }
}

// Whether a source's per-channel alpha pairs with the channels of plane a render writes: its
// layer is read in plane's layout. A layer read as stored lands its channels in the row's first
// ones, and its alpha pairs with them channel by channel.
bool
stageAlphaPairsWrittenChannels(const StageSourceLayers& layers,
                               const ImageLayerDesc& plane)
{
    return layers.mapped && (plane.getNumComponents() > 0);
}

// One input image a Composite op reads, locked on the calling thread like PassExec::PlaneJob.
struct SourceImage {
    ImagePtr image;
    int nComps = 0;
    RectI bounds;
    std::shared_ptr<Image::ReadAccess> access;

    void lock()
    {
        if (image) {
            bounds = image->getBounds();
            nComps = (int)image->getComponentsCount();
            access = std::make_shared<Image::ReadAccess>(image.get());
        }
    }
};

// The layer the KnobLayerSelect named knobName selects among those input carries at time. False
// when the knob is missing or resolves to no layer, in which case what it selects reads zero.
bool
resolveStageLayerKnob(EffectInstance& effect,
                      std::string_view knobName,
                      int input,
                      double time,
                      ViewIdx view,
                      ImageLayerDesc* layer)
{
    KnobLayerSelectPtr knob = effect.getKnobByNameAndType<KnobLayerSelect>(std::string(knobName));
    std::list<ImageLayerDesc> present;
    effect.getAvailableLayers(time, view, input, &present);
    ResolvedLayer resolved;
    if (!knob || !knob->resolve(present, &resolved) || (resolved.desc.getNumComponents() == 0)) {
        return false;
    }
    *layer = resolved.desc;

    return true;
}

// The channel the KnobChannelSelect named knobName selects among the layers input carries at time,
// as layers' broadcast alpha. Left without alpha when the knob is missing or selects none.
void
resolveStageAlphaChannel(EffectInstance& effect,
                         std::string_view knobName,
                         int input,
                         double time,
                         ViewIdx view,
                         StageSourceLayers* layers)
{
    KnobChannelSelectPtr knob = effect.getKnobByNameAndType<KnobChannelSelect>(std::string(knobName));
    std::list<ImageLayerDesc> present;
    effect.getAvailableLayers(time, view, input, &present);
    ImageLayerDesc layer;
    int channel = -1;
    if (knob && knob->resolve(present, &layer, &channel) && (channel >= 0) && (layer.getNumComponents() > 0)) {
        layers->alphaLayer = layer;
        layers->alphaChannel = channel;
    }
}

// The index of layer's channel named "A" or "alpha", in any case, or -1.
int
stageLayerAlphaChannel(const ImageLayerDesc& layer)
{
    const std::vector<std::string>& channels = layer.getChannels();
    for (std::size_t c = 0; c < channels.size(); ++c) {
        const QString name = QString::fromUtf8(channels[c].c_str());
        if ((name.compare(QString::fromUtf8("A"), Qt::CaseInsensitive) == 0) || (name.compare(QString::fromUtf8("alpha"), Qt::CaseInsensitive) == 0)) {
            return (int)c;
        }
    }

    return -1;
}

ImagePtr
fetchStageSource(EffectInstance& effect,
                 const StageSource& source,
                 const StageSourceLayers& layers,
                 const EffectInstance::RenderActionArgs& args)
{
    if (!layers.read) {
        return ImagePtr();
    }
    RectI roi;
    // A selected layer is read as it is stored, not in the clip's colour layout.
    return effect.getImage(source.inputNb, args.time, args.mappedScale, args.view, NULL, &layers.layer, layers.mapped, false /*dontUpscale*/, eStorageModeRAM, NULL, &roi);
}

// The image a source's alpha is read from, as stored; null when it has none or its channel lies
// outside the image, in which case the alpha reads zero.
ImagePtr
fetchStageSourceAlpha(EffectInstance& effect,
                      const StageSource& source,
                      const StageSourceLayers& layers,
                      const EffectInstance::RenderActionArgs& args)
{
    if (!layers.hasAlpha()) {
        return ImagePtr();
    }
    RectI roi;
    ImagePtr image = effect.getImage(source.inputNb, args.time, args.mappedScale, args.view, NULL, &layers.alphaLayer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &roi);
    if (image && (layers.alphaChannel >= (int)image->getComponentsCount())) {
        image.reset();
    }

    return image;
}

// GpuRecordContext::updateBuffer() writes at most this much.
const std::size_t kMaxGpuParamBytes = 65536;

#ifdef NATRON_HAVE_GPU
bool
toWrapView(const GpuImageView& view,
           gpu::GpuWrapView* out)
{
    if ((view.byteOffset % sizeof(float)) != 0) {
        return false;
    }
    out->buffer = view.buffer;
    out->offsetFloats = view.byteOffset / sizeof(float);
    out->bounds = gpu::GpuWrapRect { view.bounds.x1, view.bounds.y1, view.bounds.x2, view.bounds.y2 };
    out->strideFloats = (uint32_t)view.rowStrideFloats;
    out->nComps = (uint32_t)view.nComps;

    return true;
}

// Whether b is the storage buffer named name, the source or destination of a runtime shader.
bool
isRuntimeShaderBuffer(const gpu::GpuKernelBinding& b,
                      const char* name)
{
    return (b.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) && (b.name == name);
}

// The source and destination buffers, at whatever binding, are its only storage buffers, and the
// push constants, if any, a prefix of GpuShaderRegion.
bool
followsRuntimeShaderLayout(const gpu::GpuKernel& kernel)
{
    if (kernel.desc().pushConstantBytes > sizeof(GpuShaderRegion)) {
        return false;
    }
    int sources = 0;
    int destinations = 0;
    for (const gpu::GpuKernelBinding& b : kernel.bindings()) {
        if (isRuntimeShaderBuffer(b, kGpuShaderSourceBuffer)) {
            ++sources;
        } else if (isRuntimeShaderBuffer(b, kGpuShaderDestinationBuffer)) {
            ++destinations;
        } else if (b.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            return false;
        }
    }

    return (sources == 1) && (destinations == 1);
}

// Failures another request may not meet.
bool
isTransientGpuFailure(const gpu::GpuStatus& status)
{
    return (status.result == VK_ERROR_DEVICE_LOST) || (status.result == VK_ERROR_OUT_OF_DEVICE_MEMORY) || (status.result == VK_ERROR_OUT_OF_HOST_MEMORY);
}

// The wrap of args' source, mask, mix, processed channels and divisor as plan resolves them, for
// the caller to give an output and a destination.
bool
makeStageGpuWrap(const StagePlan& plan,
                 const GpuRenderArgs& args,
                 gpu::GpuWrap* wrap)
{
    const GpuQuery& query = args.query;
    const int nComps = query.nComps;

    wrap->nComps = (uint32_t)nComps;
    wrap->channelMask = (uint32_t)plan.processChannels.to_ulong();
    wrap->mix = (float)plan.mix;

    // An input that is not connected reads zero, as runStagesCpu() reads it.
    const int sourceInput = plan.sourceInput;
    const bool hasSource = (sourceInput >= 0) && (sourceInput < (int)args.inputs.size()) && args.inputs[sourceInput].buffer;
    wrap->src.nComps = (uint32_t)nComps;
    if (hasSource && !toWrapView(args.inputs[sourceInput], &wrap->src)) {
        return false;
    }

    if (hasSource) {
        int component = -1;
        if (!stageGpuDivisorComponent(plan, query, &component)) {
            return false;
        }
        wrap->divisorChannel = component;
        wrap->skipChannel = component;
    }

    if (plan.applyMask) {
        wrap->applyMask = true;
        wrap->maskInvert = plan.maskInvert;
        // A mask that is not handed over reads zero everywhere.
        wrap->mask.nComps = 1;
        const int maskInput = plan.maskInput;
        const bool hasMask = (maskInput >= 0) && (maskInput < (int)args.inputs.size()) && args.inputs[maskInput].buffer;
        if (hasMask && (plan.maskChannel >= 0) && (plan.maskChannel < plan.maskLayer.getNumComponents())) {
            const GpuImageView& maskView = args.inputs[maskInput];
            const int component = stageGpuMaskViewComponent(plan.maskLayer, plan.maskChannel, maskView.nComps);
            if ((component < 0) || (component >= maskView.nComps) || !toWrapView(maskView, &wrap->mask)) {
                return false;
            }
            wrap->maskChannel = (uint32_t)component;
        }
    }

    return true;
} // makeStageGpuWrap

// The view args hands input inputNb over in for layer, read whole or only its channel `channel`
// when that is not -1: the input's own for the layer plan hands it over in
// (stageGpuSourceInputLayer()), else the layer input naming it. Null when args hands none over.
const GpuImageView*
findStageGpuView(const StagePlan& plan,
                 const GpuRenderArgs& args,
                 int inputNb,
                 const ImageLayerDesc& layer,
                 int channel)
{
    const GpuImageView* view = nullptr;

    if (stageGpuLayerRead(layer, channel, stageGpuSourceInputLayer(plan, inputNb, args.query.plane)) == StageGpuLayerRead::Plane) {
        if ((inputNb >= 0) && (inputNb < (int)args.inputs.size())) {
            view = &args.inputs[inputNb];
        }
    } else {
        for (const GpuLayerInput& layerInput : args.layerInputs) {
            if ((layerInput.inputNb == inputNb) && (layerInput.layer == layer)) {
                view = &layerInput.view;
                break;
            }
        }
    }

    return (view && view->buffer) ? view : nullptr;
}

// A Composite input as args hands it over, in the layer `layers` names for it when given; one not
// handed over reads zero, as runStagesCpu() reads it.
bool
makeStageGpuInput(const StagePlan& plan,
                  const StageSource& source,
                  const StageSourceLayers* layers,
                  const GpuRenderArgs& args,
                  gpu::GpuWrapView* view)
{
    *view = gpu::GpuWrapView();
    view->nComps = (uint32_t)args.query.nComps;
    const int inputNb = source.inputNb;
    if (layers && !layers->mapped) {
        const GpuImageView* layerView = layers->read ? findStageGpuView(plan, args, inputNb, layers->layer, -1) : nullptr;

        return !layerView || toWrapView(*layerView, view);
    }
    if (layers && !layers->read) {
        return true;
    }
    if ((inputNb < 0) || (inputNb >= (int)args.inputs.size()) || !args.inputs[inputNb].buffer) {
        return true;
    }

    return toWrapView(args.inputs[inputNb], view);
}

// kernel's parameter block in a scratch buffer, its size rounded up to whole words; no buffer when
// it has none.
bool
uploadKernelParams(gpu::GpuRecordContext& ctx,
                   const PixelKernelGpu& kernel,
                   gpu::GpuBuffer** params)
{
    *params = nullptr;
    if (kernel.params.empty()) {
        return true;
    }
    std::vector<std::byte> block(kernel.params);
    block.resize((block.size() + 3) & ~std::size_t(3), std::byte(0));

    return ctx.scratch(block.size(), *params) && *params && ctx.updateBuffer(**params, 0, std::span<const std::byte>(block.data(), block.size()));
}

// A Composite input's alpha as args hands it over; one with no alpha, or not handed over, reads
// zero, as runStagesCpu() reads it.
bool
makeStageGpuAlpha(const StagePlan& plan,
                  const StageSource& source,
                  const StageSourceLayers& layers,
                  const GpuRenderArgs& args,
                  gpu::GpuWrapAlpha* alpha)
{
    *alpha = gpu::GpuWrapAlpha();
    if (!layers.hasAlpha()) {
        return true;
    }
    const GpuImageView* view = findStageGpuView(plan, args, source.inputNb, layers.alphaLayer, layers.alphaChannel);
    if (!view) {
        return true;
    }
    if (!toWrapView(*view, &alpha->view)) {
        return false;
    }
    if (layers.alphaPerChannel()) {
        alpha->channel = stageAlphaPairsWrittenChannels(layers, plan.plane) ? gpu::kGpuWrapAlphaChannelPerWrittenChannel : gpu::kGpuWrapAlphaChannelPerChannel;

        return true;
    }
    // The plane's view may hold a colour alpha in another colour layout.
    const int component = stageGpuMaskViewComponent(layers.alphaLayer, layers.alphaChannel, view->nComps);
    if ((component < 0) || (component >= view->nComps)) {
        alpha->view = gpu::GpuWrapView();
    } else {
        alpha->channel = component;
    }

    return true;
}

StatusEnum
recordKernelOp(const StagePlan& plan,
               const GpuRenderArgs& args,
               const PixelKernelGpu& kernel)
{
    if ((kernel.kernel < 0) || (kernel.params.size() > kMaxGpuParamBytes)) {
        return eStatusFailed;
    }

    gpu::GpuWrap wrap;
    if (!makeStageGpuWrap(plan, args, &wrap)) {
        return eStatusFailed;
    }
    wrap.output = gpu::GpuWrapRect { args.outputRect.x1, args.outputRect.y1, args.outputRect.x2, args.outputRect.y2 };
    if (!toWrapView(args.output, &wrap.dst)) {
        return eStatusFailed;
    }

    gpu::GpuWrapView base;
    std::vector<gpu::GpuWrapView> further;
    std::vector<gpu::GpuWrapAlpha> alphas;
    if (!plan.composite.empty()) {
        const bool hasLayers = plan.compositeLayers.size() == plan.composite.size();
        if (!hasLayers && !plan.compositeLayers.empty()) {
            return eStatusFailed;
        }
        const auto layersOf = [&](std::size_t k) {
            return hasLayers ? &plan.compositeLayers[k] : nullptr;
        };
        if (!makeStageGpuInput(plan, plan.composite[0], layersOf(0), args, &base)) {
            return eStatusFailed;
        }
        further.resize(plan.composite.size() - 1);
        for (std::size_t k = 1; k < plan.composite.size(); ++k) {
            if (!makeStageGpuInput(plan, plan.composite[k], layersOf(k), args, &further[k - 1])) {
                return eStatusFailed;
            }
        }
        if (stageSourcesHaveAlpha(plan)) {
            if (!hasLayers) {
                return eStatusFailed;
            }
            alphas.resize(plan.composite.size());
            for (std::size_t k = 0; k < plan.composite.size(); ++k) {
                if (!makeStageGpuAlpha(plan, plan.composite[k], plan.compositeLayers[k], args, &alphas[k])) {
                    return eStatusFailed;
                }
            }
        }
    }

    gpu::GpuRecordContext& ctx = args.context;
    gpu::GpuBuffer* params = nullptr;
    if (!uploadKernelParams(ctx, kernel, &params)) {
        return eStatusFailed;
    }

    if (!plan.composite.empty()) {
        return gpu::recordWrapFold(ctx, (gpu::GpuKernelId)kernel.kernel, wrap, base, std::span<const gpu::GpuWrapView>(further.data(), further.size()), params,
                                   std::span<const gpu::GpuWrapAlpha>(alphas.data(), alphas.size()), plan.compositeResultIsAlpha)
            ? eStatusOK
            : eStatusFailed;
    }

    return gpu::recordWrapped(ctx, (gpu::GpuKernelId)kernel.kernel, wrap, params) ? eStatusOK : eStatusFailed;
} // recordKernelOp

StatusEnum
recordRuntimeShaderOp(const StagePlan& plan,
                      const GpuRenderArgs& args,
                      const GpuRuntimeShader& shader,
                      const GpuShaderInputs& inputs)
{
    const RectI& rect = args.outputRect;
    const int nComps = args.query.nComps;
    gpu::GpuRecordContext& ctx = args.context;

    std::shared_ptr<gpu::GpuKernel> kernel;
    if (!ctx.runtimeKernel(shader.source, gpu::ShaderStage::Compute, shader.entry, kernel) || !kernel || !followsRuntimeShaderLayout(*kernel)) {
        return eStatusFailed;
    }

    const std::size_t bytes = (std::size_t)rect.width() * rect.height() * nComps * sizeof(float);
    gpu::GpuBuffer* divided = nullptr;
    gpu::GpuBuffer* filtered = nullptr;
    if (!ctx.scratch(bytes, divided) || !divided || !ctx.scratch(bytes, filtered) || !filtered) {
        return eStatusFailed;
    }
    GpuImageView dividedView;
    dividedView.buffer = divided;
    dividedView.bounds = rect;
    dividedView.rowStrideFloats = rect.width() * nComps;
    dividedView.nComps = nComps;
    GpuImageView filteredView = dividedView;
    filteredView.buffer = filtered;

    if (recordStagesGpuDivide(plan, args, dividedView) != eStatusOK) {
        return eStatusFailed;
    }
    ctx.computeBarrier();

    std::map<std::string, const gpu::GpuTexture*> textures;
    std::vector<gpu::GpuBinding> bindings;
    for (const gpu::GpuKernelBinding& b : kernel->bindings()) {
        if (b.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            bindings.push_back(gpu::GpuBinding::storage(b.binding, isRuntimeShaderBuffer(b, kGpuShaderSourceBuffer) ? *divided : *filtered));
            continue;
        }
        if (b.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            const GpuShaderUniform* uniform = nullptr;
            for (const GpuShaderUniform& u : inputs.uniforms) {
                if (u.binding == b.name) {
                    uniform = &u;
                }
            }
            if (!uniform) {
                return eStatusFailed;
            }
            std::vector<std::byte> block(uniform->data);
            block.resize(std::max<std::size_t>({ block.size(), (std::size_t)b.blockBytes, 1 }), std::byte(0));
            const gpu::GpuUniformBuffer* buffer = nullptr;
            if (!ctx.uniform(std::span<const std::byte>(block.data(), block.size()), buffer) || !buffer) {
                return eStatusFailed;
            }
            bindings.push_back(gpu::GpuBinding::uniform(b.binding, *buffer));
            continue;
        }
        if ((b.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) && (b.type != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) && (b.type != VK_DESCRIPTOR_TYPE_SAMPLER)) {
            return eStatusFailed;
        }
        std::map<std::string, const gpu::GpuTexture*>::const_iterator found = textures.find(b.name);
        if (found == textures.end()) {
            const GpuShaderTexture* texture = nullptr;
            for (const GpuShaderTexture& t : inputs.textures) {
                if (t.binding == b.name) {
                    texture = &t;
                }
            }
            if (!texture || !texture->texels || (texture->dimensions < 1) || (texture->width < 1) || (texture->height < 1) || (texture->depth < 1) || (texture->channels < 1)) {
                return eStatusFailed;
            }
            gpu::GpuTextureDesc desc;
            desc.dimensions = (uint32_t)texture->dimensions;
            desc.width = (uint32_t)texture->width;
            desc.height = (uint32_t)texture->height;
            desc.depth = (uint32_t)texture->depth;
            desc.channels = (uint32_t)texture->channels;
            desc.filter = texture->linear ? gpu::GpuTextureFilter::Linear : gpu::GpuTextureFilter::Nearest;
            const std::span<const float> texels(texture->texels->data(), texture->texels->size());
            const gpu::GpuTexture* created = nullptr;
            const gpu::GpuStatus status = texture->cacheKey.empty() ? ctx.texture(desc, texels, created) : ctx.cachedTexture(texture->cacheKey, desc, texels, created);
            if (!status || !created) {
                return eStatusFailed;
            }
            found = textures.insert(std::make_pair(b.name, created)).first;
        }
        bindings.push_back(gpu::GpuBinding::texture(b.binding, *found->second));
    }

    GpuShaderRegion region;
    region.x1 = rect.x1;
    region.y1 = rect.y1;
    region.width = (uint32_t)rect.width();
    region.height = (uint32_t)rect.height();
    region.nComps = (uint32_t)nComps;
    region.channelMask = (uint32_t)plan.processChannels.to_ulong();
    const std::span<const std::byte> push = std::as_bytes(std::span<const GpuShaderRegion>(&region, 1)).first(kernel->desc().pushConstantBytes);
    if (!ctx.dispatchBindings(kernel, bindings, push, { region.width, region.height, 1u })) {
        return eStatusFailed;
    }
    ctx.computeBarrier();

    return recordStagesGpuFinish(plan, args, filteredView);
} // recordRuntimeShaderOp
#endif // NATRON_HAVE_GPU
} // anonymous namespace

int
stageMaskInput(const EffectInstance& effect)
{
    const int nInputs = effect.getNInputs();

    for (int i = 0; i < nInputs; ++i) {
        if (effect.isInputMask(i)) {
            return i;
        }
    }

    return -1;
}

bool
isStageMaskApplied(const EffectInstance& effect)
{
    const int maskInput = stageMaskInput(effect);

    return (maskInput >= 0) && effect.getInput(maskInput) && effect.isMaskEnabled(maskInput);
}

void
resolveStageOpMask(EffectInstance& effect,
                   StagePlan* plan)
{
    resolvePlanMask(effect, isStageMaskApplied(effect), plan);
}

bool
stageMaskMissesWindow(const EffectInstance& effect,
                      int maskInput,
                      double time,
                      const RenderScale& scale,
                      const RectI& roi,
                      ViewIdx view)
{
    EffectInstancePtr mask = effect.getInput(maskInput);

    if (!mask) {
        return false;
    }
    const RenderScale maskScale = mask->supportsRenderScale() ? scale : RenderScale::identity;
    RectD maskRoD;
    bool isProjectFormat = false;
    if (mask->getRegionOfDefinition_public(mask->getRenderHash(), time, maskScale, view, &maskRoD, &isProjectFormat) == eStatusFailed) {
        return false;
    }
    const RectI maskPixels = maskRoD.toPixelEnclosing(scale.toMipmapLevel(), mask->getRenderMetadata(time, view)->getPixelAspectRatio(-1));

    return !roi.intersects(maskPixels);
}

StagePlan
resolveStagePlan(EffectInstance& effect,
                 const StageSet& stages,
                 const StageKnobs& knobs,
                 int sourceInput,
                 double time,
                 ViewIdx view,
                 const std::bitset<4>& processChannels)
{
    StagePlan plan;

    plan.time = time;
    plan.view = view;
    plan.processChannels = processChannels;
    plan.stages = stages;
    plan.sourceInput = sourceInput;

    if (stages.has(Stage::MaskMix)) {
        const KnobReader reader(time, view);
        plan.mix = reader.valueOr(knobs.mix, 1.);
        plan.maskInvert = reader.valueOr(knobs.maskInvert, false);
        resolvePlanMask(effect, isStageMaskApplied(effect), &plan);
    }

    NodePtr node = effect.getNode();
    const bool hasDivisor = stages.has(Stage::Premult) || stages.has(Stage::Divisor);
    KnobChannelSelectPtr unPremultBy = (hasDivisor && node) ? node->getUnPremultBySelector() : KnobChannelSelectPtr();
    if (unPremultBy && !unPremultBy->isNone() && (sourceInput >= 0) && effect.getInput(sourceInput)) {
        std::list<ImageLayerDesc> availableLayers;
        effect.getAvailableLayers(time, view, sourceInput, &availableLayers);
        plan.divisorChannel = node->getUnPremultChannel(availableLayers, &plan.divisorLayer);
        if (plan.divisorLayer.getNumComponents() == 0) {
            plan.divisorChannel = -1;
        }
    }

    return plan;
} // resolveStagePlan

void
resolveStageSources(const NodeSpec* spec,
                    const CompositeInputs& inputs,
                    StagePlan* plan)
{
    const int nSpecInputs = spec ? spec->inputCount() : 0;
    const auto resolve = [spec, nSpecInputs](const CompositeInput& input) {
        StageSource source;
        source.inputNb = input.inputNb;
        source.layerInput = input.inputNb;

        // Every step of a pairing names a distinct input, so a chain longer than the inputs loops.
        const InputSpec* layerSpec = spec ? spec->resolveInput(source.inputNb).spec : nullptr;
        int layerInput = source.inputNb;
        for (int step = 0; layerSpec && (layerSpec->layer.kind == InputLayer::Kind::PairWith); ++step) {
            if (step >= nSpecInputs) {
                layerSpec = nullptr;
                layerInput = source.inputNb;
                break;
            }
            layerInput = layerSpec->layer.pairedInput;
            layerSpec = spec->resolveInput(layerInput).spec;
        }
        source.layerInput = layerInput;
        if (layerSpec && (layerSpec->layer.kind == InputLayer::Kind::Select)) {
            source.layerKnob = layerSpec->layer.knob;
        }
        // A read layer the op names replaces whatever the spec resolves to.
        if (!input.layerKnob.empty()) {
            source.layerInput = source.inputNb;
            source.layerKnob = input.layerKnob;
        }
        source.alpha = input.alpha;

        return source;
    };

    plan->composite.clear();
    plan->composite.reserve(inputs.further.size() + 1);
    plan->composite.push_back(resolve(inputs.base));
    for (const CompositeInput& input : inputs.further) {
        plan->composite.push_back(resolve(input));
    }
    plan->compositeResultIsAlpha = inputs.resultIsAlpha;
} // resolveStageSources

StageSourceLayers
resolveStageSourceLayers(EffectInstance& effect,
                         const StageSource& source,
                         const ImageLayerDesc& plane,
                         double time,
                         ViewIdx view)
{
    StageSourceLayers layers;
    if (!effect.getInput(source.inputNb)) {
        return layers;
    }
    // A connected input lacking the layer it reads has zero pixels but still its alpha.
    if (!source.layerKnob.empty()) {
        layers.read = resolveStageLayerKnob(effect, source.layerKnob, source.layerInput, time, view, &layers.layer);
        layers.mapped = !layers.read;
    } else if ((plane.getNumComponents() > 0) && !plane.isColorLayer()) {
        // The plane itself, which only a render's planned input planes would otherwise name: the
        // GPU query and the request pass have none, and the node may have no layer knob naming it.
        std::list<ImageLayerDesc> present;
        effect.getAvailableLayers(time, view, source.layerInput, &present);
        layers.read = ImageLayerDesc::findEquivalentLayer(plane, present.begin(), present.end()) != present.end();
        layers.layer = plane;
    } else {
        layers.read = effect.resolveInputPlaneForRender(source.layerInput, time, view, &layers.layer, NULL);
    }
    if (!layers.read) {
        layers.layer = ImageLayerDesc();
    }

    switch (source.alpha.kind) {
    case CompositeAlpha::Kind::None:
        break;
    case CompositeAlpha::Kind::Channel:
        resolveStageAlphaChannel(effect, source.alpha.knob, source.inputNb, time, view, &layers);
        break;
    case CompositeAlpha::Kind::Layer: {
        ImageLayerDesc alphaLayer;
        if (resolveStageLayerKnob(effect, source.alpha.knob, source.inputNb, time, view, &alphaLayer)) {
            layers.alphaLayer = alphaLayer;
            layers.alphaChannel = -1;
        }
        break;
    }
    case CompositeAlpha::Kind::ReadLayer: {
        const int channel = layers.read ? stageLayerAlphaChannel(layers.layer) : -1;
        if (channel >= 0) {
            layers.alphaLayer = layers.layer;
            layers.alphaChannel = channel;
        } else {
            resolveStageAlphaChannel(effect, source.alpha.knob, source.inputNb, time, view, &layers);
        }
        break;
    }
    }

    return layers;
} // resolveStageSourceLayers

bool
stageSourcesHaveAlpha(const StagePlan& plan)
{
    for (const StageSource& source : plan.composite) {
        if (source.alpha.kind != CompositeAlpha::Kind::None) {
            return true;
        }
    }

    return false;
}

void
resolveStageSourcesLayers(EffectInstance& effect,
                          StagePlan* plan)
{
    plan->compositeLayers.clear();
    bool needed = false;
    for (const StageSource& source : plan->composite) {
        needed = needed || !source.layerKnob.empty() || (source.alpha.kind != CompositeAlpha::Kind::None);
    }
    if (!needed) {
        return;
    }
    plan->compositeLayers.reserve(plan->composite.size());
    for (const StageSource& source : plan->composite) {
        plan->compositeLayers.push_back(resolveStageSourceLayers(effect, source, plan->plane, plan->time, plan->view));
    }
}

namespace {
// How many of an nComps-channel layout's channels processChannels writes.
int
writtenChannelCount(int nComps,
                    const std::bitset<4>& processChannels)
{
    int count = 0;
    for (int c = 0; (c < nComps) && (c < 4); ++c) {
        if (processChannels[pixelKernelChannelBit(nComps, c)]) {
            ++count;
        }
    }

    return count;
}

// The error a source whose alpha layer is read per channel raises when it has not as many
// channels as the channels it is the alpha of (stageAlphaPairsWrittenChannels()), or nullopt.
// Channels a render leaves unwritten pass through without an alpha.
std::optional<std::string>
stageSourceLayersError(const StageSourceLayers& layers,
                       const ImageLayerDesc& plane,
                       const std::bitset<4>& processChannels)
{
    const bool inPlane = stageAlphaPairsWrittenChannels(layers, plane);
    const ImageLayerDesc& readIn = inPlane ? plane : layers.layer;
    const int paired = inPlane ? writtenChannelCount(plane.getNumComponents(), processChannels) : readIn.getNumComponents();

    if (!layers.read || !layers.alphaPerChannel() || (layers.alphaLayer.getNumComponents() == paired)) {
        return std::nullopt;
    }
    if (paired == readIn.getNumComponents()) {
        return QCoreApplication::translate("StagePlan", "The alpha layer %1 has %2 channels, but the layer %3 it is the alpha of has %4.")
            .arg(QString::fromUtf8(layers.alphaLayer.getUserFacingLabel().c_str()))
            .arg(layers.alphaLayer.getNumComponents())
            .arg(QString::fromUtf8(readIn.getUserFacingLabel().c_str()))
            .arg(paired)
            .toStdString();
    }

    return QCoreApplication::translate("StagePlan", "The alpha layer %1 has %2 channels, but %3 channels of the layer %4 it is the alpha of are written.")
        .arg(QString::fromUtf8(layers.alphaLayer.getUserFacingLabel().c_str()))
        .arg(layers.alphaLayer.getNumComponents())
        .arg(paired)
        .arg(QString::fromUtf8(readIn.getUserFacingLabel().c_str()))
        .toStdString();
}

// Row x0..x0+width of a per-channel alpha image into row, nComps floats per pixel: channel i of
// the alpha is the alpha of the i-th channel channels writes, and the others, which pass through
// unprocessed, read zero. scratch holds the alpha's own row.
void
readStagePerChannelAlphaRow(const SourceImage& alpha,
                            const std::bitset<4>& channels,
                            int x0,
                            int y,
                            int width,
                            int nComps,
                            std::vector<float>* scratch,
                            float* row)
{
    const int alphaNComps = alpha.nComps;
    scratch->resize((std::size_t)width * alphaNComps);
    PassExec::readSourceRow(alpha.image.get(), alpha.access.get(), alpha.bounds, alphaNComps, x0, y, width, alphaNComps, scratch->data());
    std::fill(row, row + (std::size_t)width * nComps, 0.f);
    int alphaOf[4] = { -1, -1, -1, -1 };
    const int nMapped = std::min(nComps, 4);
    int next = 0;
    for (int c = 0; c < nMapped; ++c) {
        if (channels[pixelKernelChannelBit(nComps, c)] && (next < alphaNComps)) {
            alphaOf[c] = next++;
        }
    }
    const float* src = scratch->data();
    for (int x = 0; x < width; ++x, src += alphaNComps, row += nComps) {
        for (int c = 0; c < nMapped; ++c) {
            if (alphaOf[c] >= 0) {
                row[c] = src[alphaOf[c]];
            }
        }
    }
}

// One row of a Composite input, x0 onwards, in the nComps-channel layout plane is rendered in. A
// one-channel layer other than the colour one rendered wider sits in the row's first channel,
// where processedBitsForImage() guards it and the host reads it back from; readSourceRow() would
// put the input's one channel in the alpha slot, the colour layer's place for it.
void
readStageSourceRow(const ImageLayerDesc& plane,
                   const SourceImage& source,
                   int x0,
                   int y,
                   int width,
                   int nComps,
                   float* row)
{
    if (!source.image || (source.nComps != 1) || (nComps == 1) || (plane.getNumComponents() != 1) || plane.isColorLayer()) {
        PassExec::readSourceRow(source.image.get(), source.access.get(), source.bounds, source.nComps, x0, y, width, nComps, row);

        return;
    }
    std::fill(row, row + (std::size_t)width * nComps, 0.f);
    if ((y < source.bounds.y1) || (y >= source.bounds.y2)) {
        return;
    }
    const int xStart = std::max(x0, source.bounds.x1);
    const int xEnd = std::min(x0 + width, source.bounds.x2);
    if (xStart >= xEnd) {
        return;
    }
    const float* srcPix = (const float*)source.access->pixelAt(xStart, y);
    if (!srcPix) {
        return;
    }
    float* dstPix = row + (std::size_t)(xStart - x0) * nComps;
    for (int x = xStart; x < xEnd; ++x, ++srcPix, dstPix += nComps) {
        dstPix[0] = *srcPix;
    }
}
} // anonymous namespace

std::optional<std::string>
stageSourcesError(EffectInstance& effect,
                  const StagePlan& plan,
                  double time,
                  ViewIdx view)
{
    for (const StageSource& source : plan.composite) {
        if (source.alpha.kind != CompositeAlpha::Kind::Layer) {
            continue;
        }
        const std::optional<std::string> error = stageSourceLayersError(resolveStageSourceLayers(effect, source, plan.plane, time, view), plan.plane, plan.processChannels);
        if (error) {
            return error;
        }
    }

    return std::nullopt;
}

void
makeStageRowBands(const RectI& roi,
                  int nThreads,
                  std::vector<RectI>* bands)
{
    bands->clear();
    const int height = roi.height();
    if ((height <= 0) || (roi.width() <= 0)) {
        return;
    }
    const PassExec::LineBands split = PassExec::splitLines(height, (std::size_t)roi.width(), nThreads);
    for (int y = roi.y1; y < roi.y2; y += split.linesPerBand) {
        bands->push_back(RectI(roi.x1, y, roi.x2, std::min(y + split.linesPerBand, roi.y2)));
    }
}

bool
stageFinishIsCopy(bool applyMask,
                  float mix,
                  bool hasDivisor,
                  int nComps,
                  const std::bitset<4>& channels)
{
    if (applyMask || (mix != 1.f) || hasDivisor || (nComps > 4)) {
        return false;
    }
    for (int c = 0; c < nComps; ++c) {
        if (!channels[pixelKernelChannelBit(nComps, c)]) {
            return false;
        }
    }

    return true;
}

StatusEnum
runStagesCpu(EffectInstance& effect,
             const StagePlan& plan,
             const EffectInstance::RenderActionArgs& args,
             const StageRowFn& row)
{
    const float mix = (float)plan.mix;
    const bool maskInvert = plan.maskInvert;
    const bool passThrough = plan.stages.has(Stage::PassThroughUnprocessed);
    // A Divisor stage hands the op the divisor row and neither divides nor multiplies back.
    const bool divisorToOp = plan.stages.has(Stage::Divisor);
    const int sourceInput = plan.sourceInput;

    // Every image is fetched before any is locked: fetching renders upstream, which may write
    // into a cached image this render would otherwise already hold a read lock on.
    const bool doMask = plan.applyMask;
    ImagePtr mask;
    const int maskChannel = plan.maskChannel;
    if (doMask) {
        if (maskChannel >= 0) {
            RectI maskRoI;
            mask = effect.getImage(plan.maskInput, args.time, args.mappedScale, args.view, NULL, &plan.maskLayer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &maskRoI);
        }
        if (mask && ((maskChannel >= (int)mask->getComponentsCount()))) {
            mask.reset();
        }
    }

    const ImageLayerDesc& divisorLayer = plan.divisorLayer;
    const int divisorChannel = plan.divisorChannel;

    std::vector<PassExec::PlaneJob> jobs;
    std::vector<ImageLayerDesc> jobPlanes;
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        jobPlanes.push_back(it->first);
        PassExec::PlaneJob job;
        job.dst = it->second;
        if (!job.dst) {
            jobs.push_back(job);
            continue;
        }
        job.nComps = (int)job.dst->getComponentsCount();
        job.channels = PassExec::processedBitsForImage(it->first, job.nComps, args.processChannels);

        ImageLayerDesc sourceLayer;
        if ((sourceInput >= 0) && effect.getInput(sourceInput) && effect.resolveInputPlaneForRender(sourceInput, args.time, args.view, &sourceLayer, NULL)) {
            RectI sourceRoI;
            // Mapped to the clip's components, which is the layout the output plane is rendered in.
            job.src = effect.getImage(sourceInput, args.time, args.mappedScale, args.view, NULL, &sourceLayer, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &sourceRoI);
        }
        if (job.src && (divisorChannel >= 0)) {
            RectI divisorRoI;
            job.divisor = effect.getImage(sourceInput, args.time, args.mappedScale, args.view, NULL, &divisorLayer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &divisorRoI);
            if (job.divisor && (divisorChannel < (int)job.divisor->getComponentsCount())) {
                job.divisorChannel = divisorChannel;
                job.skipChannel = Node::getUnPremultSkipChannel(it->first.isColorLayer() ? job.dst->getComponents() : it->first, divisorLayer, divisorChannel);
            } else {
                job.divisor.reset();
            }
        }
        if (!PassExec::isFloatImage(job.dst) || !PassExec::isFloatImage(job.src) || !PassExec::isFloatImage(job.divisor)) {
            return eStatusFailed;
        }
        jobs.push_back(job);
    }
    if (!PassExec::isFloatImage(mask)) {
        return eStatusFailed;
    }

    // A Composite op's inputs, per output plane, base first, and their alphas when the fold
    // carries one. A per-channel alpha layer that does not match its layer fails the render, which
    // the caller reports (stageSourcesError()).
    const std::size_t nFolded = plan.composite.size();
    const bool foldAlpha = (nFolded > 1) && stageSourcesHaveAlpha(plan);
    if (plan.compositeResultIsAlpha && !foldAlpha) {
        return eStatusFailed;
    }
    std::vector<StageSourceLayers> sourceLayers(nFolded);
    for (std::size_t s = 0; s < nFolded; ++s) {
        sourceLayers[s] = resolveStageSourceLayers(effect, plan.composite[s], plan.plane, args.time, args.view);
        if (stageSourceLayersError(sourceLayers[s], plan.plane, plan.processChannels)) {
            return eStatusFailed;
        }
    }
    std::vector<std::vector<SourceImage>> jobSources(nFolded > 0 ? jobs.size() : 0, std::vector<SourceImage>(nFolded));
    std::vector<std::vector<SourceImage>> jobAlphas(foldAlpha ? jobs.size() : 0, std::vector<SourceImage>(nFolded));
    for (std::size_t j = 0; j < jobSources.size(); ++j) {
        if (!jobs[j].dst) {
            continue;
        }
        for (std::size_t s = 0; s < nFolded; ++s) {
            jobSources[j][s].image = fetchStageSource(effect, plan.composite[s], sourceLayers[s], args);
            if (!PassExec::isFloatImage(jobSources[j][s].image)) {
                return eStatusFailed;
            }
            if (foldAlpha) {
                jobAlphas[j][s].image = fetchStageSourceAlpha(effect, plan.composite[s], sourceLayers[s], args);
                if (!PassExec::isFloatImage(jobAlphas[j][s].image)) {
                    return eStatusFailed;
                }
            }
        }
    }

    const RectI& roi = args.roi;
    const int width = roi.width();
    if ((width <= 0) || (roi.height() <= 0)) {
        return eStatusOK;
    }

    // Images are locked here, on the calling thread, for the whole render; the band threads only
    // compute pixel addresses through these accesses.
    PassExec::MaskInput maskSource;
    maskSource.image = mask;
    maskSource.lock();
    std::vector<RowBand> bands;
    const int nThreads = appPTR->getNCPUsAvailableForEffect();
    std::vector<RectI> bandRects;
    makeStageRowBands(roi, nThreads, &bandRects);
    for (std::size_t j = 0; j < jobs.size(); ++j) {
        if (!jobs[j].dst) {
            continue;
        }
        jobs[j].lock();
        if (nFolded > 0) {
            for (SourceImage& source : jobSources[j]) {
                source.lock();
            }
        }
        if (foldAlpha) {
            for (SourceImage& alpha : jobAlphas[j]) {
                alpha.lock();
            }
        }
        for (std::size_t b = 0; b < bandRects.size(); ++b) {
            bands.push_back(RowBand(j, bandRects[b].y1, bandRects[b].y2));
        }
    }

    RenderCancellation cancel(&effect);

    const std::function<void(int)> renderBand = [&](int bandIndex) {
        const RowBand& band = bands[bandIndex];
        const PassExec::PlaneJob& job = jobs[band.job];
        const int nComps = job.nComps;
        const std::size_t rowSize = (std::size_t)width * nComps;
        const bool divides = job.divisor && !divisorToOp;
        const bool direct = stageFinishIsCopy(doMask, mix, divides, nComps, job.channels);
        // The same image fetched twice reads the same row, so the source row serves as the base.
        const bool baseIsSource = (nFolded > 0) && job.src && (jobSources[band.job][0].image == job.src);
        const bool readsSource = (nFolded == 0) || !direct || baseIsSource;
        std::vector<float> sourceRow(readsSource ? rowSize : 0);
        std::vector<float> dividedRow(divides ? rowSize : 0);
        std::vector<float> kernelRow(direct ? 0 : rowSize);
        std::vector<float> maskRow(doMask ? width : 0);
        std::vector<float> divisorRow(job.divisor ? width : 0);
        // A fold keeps its result in two rows it alternates between, and reads one input row.
        std::vector<float> foldRows[2];
        std::vector<float> inputRow;
        if (nFolded > 0) {
            foldRows[0].resize(rowSize);
            foldRows[1].resize(nFolded > 1 ? rowSize : 0);
            inputRow.resize(nFolded > 1 ? rowSize : 0);
        }
        // With alphas, the base's and the input's are read into a row each, nComps floats per pixel
        // at most, the result's alternates between two rows, and a source without one reads zero.
        std::vector<float> baseAlphaRow(foldAlpha ? rowSize : 0);
        std::vector<float> inputAlphaRow(foldAlpha ? rowSize : 0);
        std::vector<float> alphaRows[2];
        std::vector<float> zeroAlphaRow(foldAlpha ? width : 0, 0.f);
        std::vector<float> alphaScratch;
        if (foldAlpha) {
            alphaRows[0].resize(rowSize);
            alphaRows[1].resize(rowSize);
        }
        const auto readAlpha = [&](std::size_t s, int y, float* buffer) {
            const SourceImage& alpha = jobAlphas[band.job][s];
            RowIO::Alpha span;
            if (!alpha.image) {
                span.data = zeroAlphaRow.data();
                span.pixelStride = 1;
            } else if (sourceLayers[s].alphaPerChannel()) {
                if (stageAlphaPairsWrittenChannels(sourceLayers[s], jobPlanes[band.job])) {
                    readStagePerChannelAlphaRow(alpha, job.channels, roi.x1, y, width, nComps, &alphaScratch, buffer);
                } else {
                    readStageSourceRow(jobPlanes[band.job], alpha, roi.x1, y, width, nComps, buffer);
                }
                span.data = buffer;
                span.pixelStride = nComps;
                span.channelStride = 1;
            } else {
                PassExec::readChannelRow(alpha.image.get(), alpha.access.get(), alpha.bounds, alpha.nComps, sourceLayers[s].alphaChannel, roi.x1, y, width, 0.f, buffer);
                span.data = buffer;
                span.pixelStride = 1;
            }

            return span;
        };
        const float* resultRow = kernelRow.data();

        RowIO io;
        io.src[0] = divides ? dividedRow.data() : sourceRow.data();
        io.nSrc = 1;
        io.mask = doMask ? &maskRow[0] : 0;
        io.divisor = job.divisor ? &divisorRow[0] : 0;
        io.divisorSkipChannel = job.divisor ? job.skipChannel : -1;
        io.x0 = roi.x1;
        io.width = width;
        io.nComps = nComps;
        io.channels = job.channels;

        for (int y = band.y1; y < band.y2; ++y) {
            if ((((y - band.y1) % kAbortCheckRows) == 0) && cancel.check()) {
                return;
            }

            float* dstPix = (float*)job.dstAccess->pixelAt(roi.x1, y);
            if (!dstPix) {
                continue;
            }

            if (readsSource) {
                PassExec::readSourceRow(job.src.get(), job.srcAccess.get(), job.srcBounds, job.srcNComps, roi.x1, y, width, nComps, sourceRow.data());
            }
            if (job.divisor) {
                PassExec::readChannelRow(job.divisor.get(), job.divisorAccess.get(), job.divisorBounds, job.divisorNComps, job.divisorChannel, roi.x1, y, width, 1.f, &divisorRow[0]);
            }
            if (divides) {
                std::copy(sourceRow.begin(), sourceRow.end(), dividedRow.begin());
                for (int i = 0; i < width; ++i) {
                    const float d = divisorRow[i];
                    if (!Image::unPremultDivisorIsUsable(d)) {
                        continue;
                    }
                    float* pix = &dividedRow[(std::size_t)i * nComps];
                    for (int c = 0; (c < nComps) && (c < 4); ++c) {
                        if (c != job.skipChannel) {
                            pix[c] = Image::unPremultiplyValue(pix[c], d);
                        }
                    }
                }
            }
            if (doMask) {
                PassExec::readChannelRow(mask.get(), maskSource.access.get(), maskSource.bounds, maskSource.nComps, maskChannel, roi.x1, y, width, 0.f, &maskRow[0]);
            }
            io.y = y;
            if (nFolded > 0) {
                const std::vector<SourceImage>& folded = jobSources[band.job];
                const float* result = sourceRow.data();
                if (!baseIsSource) {
                    const SourceImage& base = folded[0];
                    float* baseRow = (direct && (nFolded == 1)) ? dstPix : foldRows[0].data();
                    readStageSourceRow(jobPlanes[band.job], base, roi.x1, y, width, nComps, baseRow);
                    result = baseRow;
                }
                // Steps write only to the fold rows or the output, never to the source row the
                // finish still blends with.
                int next = 1;
                if (foldAlpha) {
                    io.alpha[0] = readAlpha(0, y, baseAlphaRow.data());
                }
                for (std::size_t s = 1; s < nFolded; ++s) {
                    const SourceImage& input = folded[s];
                    readStageSourceRow(jobPlanes[band.job], input, roi.x1, y, width, nComps, &inputRow[0]);
                    float* stepRow = (direct && ((s + 1) == nFolded)) ? dstPix : foldRows[next].data();
                    io.src[0] = result;
                    io.src[1] = &inputRow[0];
                    io.nSrc = 2;
                    io.dst = stepRow;
                    if (foldAlpha) {
                        io.alpha[1] = readAlpha(s, y, inputAlphaRow.data());
                        io.dstAlpha = alphaRows[s & 1].data();
                    }
                    row(io);
                    result = stepRow;
                    next = 1 - next;
                    if (foldAlpha) {
                        io.alpha[0].data = io.dstAlpha;
                        io.alpha[0].pixelStride = nComps;
                        io.alpha[0].channelStride = 1;
                    }
                }
                resultRow = plan.compositeResultIsAlpha ? io.dstAlpha : result;
            } else {
                io.dst = direct ? dstPix : kernelRow.data();
                row(io);
                resultRow = io.dst;
            }

            if (direct) {
                if (resultRow != dstPix) {
                    std::copy(resultRow, resultRow + rowSize, dstPix);
                }
                continue;
            }
            for (int i = 0; i < width; ++i, dstPix += nComps) {
                const float* srcPix = &sourceRow[(std::size_t)i * nComps];
                const float* outPix = &resultRow[(std::size_t)i * nComps];
                float alpha = mix;
                if (doMask) {
                    const float maskScale = maskInvert ? (1.f - maskRow[i]) : maskRow[i];
                    alpha = maskScale * mix;
                }
                for (int c = 0; c < nComps; ++c) {
                    if ((c >= 4) || !job.channels[pixelKernelChannelBit(nComps, c)]) {
                        if (passThrough) {
                            dstPix[c] = srcPix[c];
                        }
                        continue;
                    }
                    float v = outPix[c];
                    if (divides && (c != job.skipChannel)) {
                        v = Image::premultiplyValue(v, divisorRow[i]);
                    }
                    if (alpha == 0.f) {
                        v = srcPix[c];
                    } else if (alpha != 1.f) {
                        v = v * alpha + (1.f - alpha) * srcPix[c];
                    }
                    dstPix[c] = v;
                }
            }
        }
    };
    parallelForCancellable((int)bands.size(), nThreads, cancel, renderBand);

    return eStatusOK;
} // runStagesCpu

StagePlan
resolveGpuStagePlan(EffectInstance& effect,
                    const StageSet& stages,
                    const StageKnobs& knobs,
                    int sourceInput,
                    const GpuQuery& query)
{
    std::bitset<4> channels;
    for (int i = 0; i < 4; ++i) {
        channels[i] = query.processChannels[i];
    }
    StagePlan plan = resolveStagePlan(effect, stages, knobs, sourceInput, query.time, query.view, channels);
    if (stages.has(Stage::MaskMix) && (plan.applyMask != query.hasMask)) {
        resolvePlanMask(effect, query.hasMask, &plan);
    }

    return plan;
}

bool
stageGpuDivisorComponent(const StagePlan& plan,
                         const GpuQuery& query,
                         int* component)
{
    *component = -1;
    if (plan.divisorChannel < 0) {
        return true;
    }
    if (!query.unPremultInPlane) {
        return false;
    }
    // A colour divisor lies in a colour plane of nComps channels; any other lies in its own layer.
    const ImageLayerDesc plane = plan.divisorLayer.isColorLayer() ? ImageLayerDesc::mapNCompsToColorLayer(query.nComps) : plan.divisorLayer;
    const int c = Node::getUnPremultSkipChannel(plane, plan.divisorLayer, plan.divisorChannel);
    if ((c < 0) || (c >= query.nComps)) {
        return false;
    }
    *component = c;

    return true;
}

bool
canRunStagesGpu(const StagePlan& plan,
                const GpuQuery& query)
{
    if ((query.bitDepth != eImageBitDepthFloat) || (query.nComps < 1) || (query.nComps > 4)) {
        return false;
    }
    if ((query.unPremultChannel >= 0) && !query.unPremultInPlane) {
        return false;
    }
    int component = -1;

    return stageGpuDivisorComponent(plan, query, &component);
}

ImageLayerDesc
stageGpuSourceInputLayer(const StagePlan& plan,
                         int inputNb,
                         const ImageLayerDesc& plane)
{
    if ((plan.composite.size() < 2) || (plan.compositeLayers.size() != plan.composite.size()) || (inputNb == plan.composite[0].inputNb) || (inputNb == plan.sourceInput) || (plan.applyMask && (inputNb == plan.maskInput))) {
        return plane;
    }
    if (!plane.isColorLayer()) {
        // An input lacking the plane reads zero pixels there, so it is handed over in the layer
        // its alpha is read from, which it carries, rather than in a plane it cannot render.
        ImageLayerDesc alphaLayer;
        for (std::size_t s = 1; s < plan.composite.size(); ++s) {
            const StageSourceLayers& layers = plan.compositeLayers[s];
            if (plan.composite[s].inputNb != inputNb) {
                continue;
            }
            if (layers.read || !layers.mapped || !layers.hasAlpha() || ((alphaLayer.getNumComponents() > 0) && (alphaLayer != layers.alphaLayer))) {
                return plane;
            }
            alphaLayer = layers.alphaLayer;
        }

        return (alphaLayer.getNumComponents() > 0) ? alphaLayer : plane;
    }
    ImageLayerDesc selected;
    for (std::size_t s = 1; s < plan.composite.size(); ++s) {
        const StageSourceLayers& layers = plan.compositeLayers[s];
        if ((plan.composite[s].inputNb != inputNb) || !layers.read || !layers.layer.isColorLayer()) {
            continue;
        }
        if (layers.mapped || ((selected.getNumComponents() > 0) && (selected != layers.layer))) {
            return plane;
        }
        selected = layers.layer;
    }

    return (selected.getNumComponents() > 0) ? selected : plane;
}

bool
canRunStageSourcesGpu(const StagePlan& plan,
                      const GpuQuery& query)
{
    if (plan.composite.empty() || (plan.divisorChannel >= 0)) {
        return false;
    }
    if (plan.compositeResultIsAlpha && ((plan.composite.size() < 2) || !stageSourcesHaveAlpha(plan))) {
        return false;
    }
    for (std::size_t s = 0; s < plan.composite.size(); ++s) {
        const StageSource& source = plan.composite[s];
        if (plan.applyMask && (source.inputNb == plan.maskInput)) {
            return false;
        }
        if (source.layerKnob.empty() && (source.alpha.kind == CompositeAlpha::Kind::None)) {
            continue;
        }
        if (plan.compositeLayers.size() != plan.composite.size()) {
            return false;
        }
        const StageSourceLayers& layers = plan.compositeLayers[s];
        if (stageSourceLayersError(layers, query.plane, plan.processChannels)) {
            return false;
        }
        const ImageLayerDesc inputLayer = stageGpuSourceInputLayer(plan, source.inputNb, query.plane);
        if (layers.read && !layers.mapped) {
            const StageGpuLayerRead read = stageGpuLayerRead(layers.layer, -1, inputLayer);
            if ((read == StageGpuLayerRead::Unreadable) || ((s == 0) && (read == StageGpuLayerRead::Extra) && (layers.layer.getNumComponents() != query.nComps))) {
                return false;
            }
        }
        if (layers.hasAlpha() && (stageGpuLayerRead(layers.alphaLayer, layers.alphaChannel, inputLayer) == StageGpuLayerRead::Unreadable)) {
            return false;
        }
    }

    return true;
}

int
stageGpuMaskViewComponent(const ImageLayerDesc& maskLayer,
                          int maskChannel,
                          int viewNComps)
{
    const int layerNComps = maskLayer.getNumComponents();

    if (viewNComps == layerNComps) {
        return maskChannel;
    }
    if (!maskLayer.isColorLayer()) {
        return -1;
    }
    const int bit = ImageLayerDesc::colorViewChannelBit(ImageLayerDesc::colorViewForNComps(layerNComps).getLayerID(), maskChannel);

    return ImageLayerDesc::colorViewChannelIndex(ImageLayerDesc::colorViewForNComps(viewNComps).getLayerID(), bit);
}

void
stageGpuInputLayer(const EffectInstance& effect,
                   int inputNb,
                   const ImageLayerDesc& plane,
                   double time,
                   ViewIdx view,
                   ImageLayerDesc* layer)
{
    *layer = plane;
    if (!effect.isInputMask(inputNb)) {
        return;
    }
    ImageLayerDesc maskLayer;
    int maskChannel = -1;
    // Resolving only reads the node's selectors and its inputs' layers.
    EffectInstance& self = const_cast<EffectInstance&>(effect);
    if (self.resolveInputPlaneForRender(inputNb, time, view, &maskLayer, &maskChannel)) {
        *layer = maskLayer;
    }
}

StageGpuLayerRead
stageGpuLayerRead(const ImageLayerDesc& layer,
                  int channel,
                  const ImageLayerDesc& plane)
{
    if (layer == plane) {
        return StageGpuLayerRead::Plane;
    }
    if (!layer.isColorLayer() || !plane.isColorLayer()) {
        return StageGpuLayerRead::Extra;
    }
    if ((channel >= 0) && (stageGpuMaskViewComponent(layer, channel, plane.getNumComponents()) >= 0)) {
        return StageGpuLayerRead::Plane;
    }

    return StageGpuLayerRead::Unreadable;
}

namespace {
// Appends layer to layers unless it is there; false when the GPU executor cannot read it.
bool
appendStageGpuLayer(const ImageLayerDesc& layer,
                    int channel,
                    const ImageLayerDesc& plane,
                    std::vector<ImageLayerDesc>* layers)
{
    const StageGpuLayerRead read = stageGpuLayerRead(layer, channel, plane);

    if (read == StageGpuLayerRead::Unreadable) {
        return false;
    }
    if ((read == StageGpuLayerRead::Extra) && (std::find(layers->begin(), layers->end(), layer) == layers->end())) {
        layers->push_back(layer);
    }

    return true;
}

// Appends layer to layers unless an equivalent layer is there, as the request pass plans layers.
void
appendStageNeededLayer(const ImageLayerDesc& layer,
                       std::list<ImageLayerDesc>* layers)
{
    if ((layer.getNumComponents() > 0) && (ImageLayerDesc::findEquivalentLayer(layer, layers->begin(), layers->end()) == layers->end())) {
        layers->push_back(layer);
    }
}
} // anonymous namespace

bool
stageSourcesGpuLayers(EffectInstance& effect,
                      const StagePlan& plan,
                      int inputNb,
                      const ImageLayerDesc& plane,
                      double time,
                      ViewIdx view,
                      std::vector<ImageLayerDesc>* layers)
{
    StagePlan resolved = plan;
    resolved.time = time;
    resolved.view = view;
    resolveStageSourcesLayers(effect, &resolved);
    if (resolved.compositeLayers.size() != resolved.composite.size()) {
        return true;
    }
    const ImageLayerDesc inputLayer = stageGpuSourceInputLayer(resolved, inputNb, plane);
    for (std::size_t s = 0; s < resolved.composite.size(); ++s) {
        const StageSource& source = resolved.composite[s];
        if ((source.inputNb != inputNb) || (source.layerKnob.empty() && (source.alpha.kind == CompositeAlpha::Kind::None))) {
            continue;
        }
        const StageSourceLayers& read = resolved.compositeLayers[s];
        // A plane read in the clip layout is the input's own view.
        if (read.read && !read.mapped && !appendStageGpuLayer(read.layer, -1, inputLayer, layers)) {
            return false;
        }
        if (read.hasAlpha() && !appendStageGpuLayer(read.alphaLayer, read.alphaChannel, inputLayer, layers)) {
            return false;
        }
    }

    return true;
}

void
stageSourcesNeededLayers(EffectInstance& effect,
                         const StagePlan& plan,
                         int inputNb,
                         double time,
                         ViewIdx view,
                         std::list<ImageLayerDesc>* layers)
{
    for (const StageSource& source : plan.composite) {
        if ((source.inputNb != inputNb) || (source.layerKnob.empty() && (source.alpha.kind == CompositeAlpha::Kind::None))) {
            continue;
        }
        const StageSourceLayers read = resolveStageSourceLayers(effect, source, plan.plane, time, view);
        if (read.read && !read.mapped) {
            appendStageNeededLayer(read.layer, layers);
        }
        if (read.hasAlpha()) {
            appendStageNeededLayer(read.alphaLayer, layers);
        }
    }
}

std::string
StageGpuShaderGate::rejectionKey(const std::string& deviceTag,
                                 const GpuRuntimeShader& shader)
{
    std::string key = deviceTag;
    key += '\0';
    key += shader.entry;
    key += '\0';
    key += shader.source;

    return key;
}

bool
StageGpuShaderGate::isRejected(const std::string& deviceTag,
                               const GpuRuntimeShader& shader)
{
    std::lock_guard<std::mutex> lock(_mutex);

    return _rejected.count(rejectionKey(deviceTag, shader)) != 0;
}

bool
StageGpuShaderGate::accept(const GpuRuntimeShader& shader,
                           const NodePtr& node)
{
#ifdef NATRON_HAVE_GPU
    GpuContext* context = appPTR ? appPTR->getGpuContext() : nullptr;
    gpu::GpuRuntime* runtime = (context && context->isAvailable()) ? context->runtime() : nullptr;
    if (!runtime || runtime->isLost()) {
        return false;
    }
    // Another device, picked in the preferences, may run what this one rejected.
    const std::string key = rejectionKey(runtime->pipelines().deviceTag(), shader);
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_rejected.count(key)) {
            return false;
        }
    }
    std::shared_ptr<gpu::GpuKernel> kernel;
    gpu::GpuStatus status = runtime->pipelines().get(shader.source, gpu::ShaderStage::Compute, shader.entry, kernel);
    if (status && (!kernel || !followsRuntimeShaderLayout(*kernel))) {
        status = gpu::GpuStatus { VK_ERROR_INITIALIZATION_FAILED, "the storage buffers must be exactly one named Src and one named Dst, and the push constants at most a GpuShaderRegion" };
    }
    if (status) {
        std::string posted;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            posted.swap(_message);
        }
        if (!posted.empty() && node) {
            QString current;
            int type = 0;
            node->getPersistentMessage(&current, &type, false);
            if (current == QString::fromUtf8(posted.c_str())) {
                node->clearPersistentMessage(false);
            }
        }

        return true;
    }
    if (isTransientGpuFailure(status) || runtime->isLost()) {
        return false;
    }
    const std::string message = QCoreApplication::translate("StageGpuShaderGate", "The GPU shader cannot be used, so this node renders on the CPU.\n%1").arg(QString::fromUtf8(status.message.c_str())).toStdString();
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_rejected.insert(key).second) {
            return false;
        }
        _message = message;
    }
    if (node) {
        node->setPersistentMessage(eMessageTypeWarning, message);
    }

    return false;
#else
    Q_UNUSED(shader);
    Q_UNUSED(node);

    return false;
#endif
} // StageGpuShaderGate::accept

bool
canRecordGpuOp(const GpuOpRef& op,
               StageGpuShaderGate& gate,
               const NodePtr& node)
{
#ifdef NATRON_HAVE_GPU
    switch (op.kind) {
    case GpuOpRef::Kind::Kernel:

        return (op.kernel.kernel >= 0) && (op.kernel.params.size() <= kMaxGpuParamBytes);
    case GpuOpRef::Kind::RuntimeShader:

        return gate.accept(op.shader, node);
    case GpuOpRef::Kind::None:
        break;
    }

    return false;
#else
    Q_UNUSED(op);
    Q_UNUSED(gate);
    Q_UNUSED(node);

    return false;
#endif
}

StatusEnum
recordStagesGpu(const StagePlan& plan,
                const GpuRenderArgs& args,
                const GpuOpRef& op)
{
#ifdef NATRON_HAVE_GPU
    if (args.outputRect.isNull()) {
        return eStatusFailed;
    }
    switch (op.kind) {
    case GpuOpRef::Kind::Kernel:

        return recordKernelOp(plan, args, op.kernel);
    case GpuOpRef::Kind::RuntimeShader:
        if (!plan.composite.empty()) {
            return eStatusFailed;
        }

        return recordRuntimeShaderOp(plan, args, op.shader, op.shaderInputs);
    case GpuOpRef::Kind::None:
        break;
    }

    return eStatusFailed;
#else
    Q_UNUSED(plan);
    Q_UNUSED(args);
    Q_UNUSED(op);

    return eStatusFailed;
#endif
}

StatusEnum
recordStagesGpuDivide(const StagePlan& plan,
                      const GpuRenderArgs& args,
                      const GpuImageView& scratch)
{
#ifdef NATRON_HAVE_GPU
    gpu::GpuWrap wrap;
    if (!makeStageGpuWrap(plan, args, &wrap) || !toWrapView(scratch, &wrap.dst)) {
        return eStatusFailed;
    }
    wrap.output = wrap.dst.bounds;
    wrap.applyMask = false;
    wrap.mix = 1.f;

    return gpu::recordWrapDivide(args.context, wrap) ? eStatusOK : eStatusFailed;
#else
    Q_UNUSED(plan);
    Q_UNUSED(args);
    Q_UNUSED(scratch);

    return eStatusFailed;
#endif
}

StatusEnum
recordStagesGpuFinish(const StagePlan& plan,
                      const GpuRenderArgs& args,
                      const GpuImageView& filtered)
{
#ifdef NATRON_HAVE_GPU
    gpu::GpuWrap wrap;
    gpu::GpuWrapView filteredView;
    if (args.outputRect.isNull() || !makeStageGpuWrap(plan, args, &wrap) || !toWrapView(args.output, &wrap.dst) || !toWrapView(filtered, &filteredView)) {
        return eStatusFailed;
    }
    wrap.output = gpu::GpuWrapRect { args.outputRect.x1, args.outputRect.y1, args.outputRect.x2, args.outputRect.y2 };

    return gpu::recordWrapFinish(args.context, wrap, filteredView) ? eStatusOK : eStatusFailed;
#else
    Q_UNUSED(plan);
    Q_UNUSED(args);
    Q_UNUSED(filtered);

    return eStatusFailed;
#endif
}

StatusEnum
recordStagesGpuKernelInputs(const StagePlan& plan,
                            const GpuRenderArgs& args,
                            const PixelKernelGpu& kernel,
                            std::span<const GpuImageView> inputs)
{
#ifdef NATRON_HAVE_GPU
    if (args.outputRect.isNull() || (kernel.kernel < 0) || (kernel.params.size() > kMaxGpuParamBytes)) {
        return eStatusFailed;
    }
    gpu::GpuWrap wrap;
    if (!makeStageGpuWrap(plan, args, &wrap) || !toWrapView(args.output, &wrap.dst)) {
        return eStatusFailed;
    }
    wrap.output = gpu::GpuWrapRect { args.outputRect.x1, args.outputRect.y1, args.outputRect.x2, args.outputRect.y2 };

    std::vector<gpu::GpuWrapView> views(inputs.size());
    for (std::size_t k = 0; k < inputs.size(); ++k) {
        views[k].nComps = 1;
        if (inputs[k].buffer && !inputs[k].bounds.isNull() && !toWrapView(inputs[k], &views[k])) {
            return eStatusFailed;
        }
    }

    gpu::GpuRecordContext& ctx = args.context;
    gpu::GpuBuffer* params = nullptr;
    if (!uploadKernelParams(ctx, kernel, &params)) {
        return eStatusFailed;
    }

    return gpu::recordWrappedInputs(ctx, (gpu::GpuKernelId)kernel.kernel, wrap, params, std::span<const gpu::GpuWrapView>(views.data(), views.size())) ? eStatusOK : eStatusFailed;
#else
    Q_UNUSED(plan);
    Q_UNUSED(args);
    Q_UNUSED(kernel);
    Q_UNUSED(inputs);

    return eStatusFailed;
#endif
}

bool
stageGpuInputView(EffectInstance& effect,
                  const GpuRenderArgs& args,
                  int inputNb,
                  GpuImageView* view)
{
    *view = GpuImageView();
    if ((inputNb < 0) || (inputNb >= (int)args.inputs.size())) {
        return true;
    }
    const GpuImageView& in = args.inputs[inputNb];
    EffectInstancePtr input = effect.getInput(inputNb);
    if (!input || !in.buffer) {
        return true;
    }
    if ((in.nComps < 1) || (in.nComps > 4) || !PassExec::isUsableGpuView(in, in.nComps)) {
        return false;
    }
    const RenderScale& scale = args.query.mappedScale;
    const RenderScale inputScale = input->supportsRenderScale() ? scale : RenderScale::identity;
    RectD rod;
    bool isProjectFormat = false;
    if (input->getRegionOfDefinition_public(input->getRenderHash(), args.query.time, inputScale, args.query.view, &rod, &isProjectFormat) == eStatusFailed) {
        return false;
    }
    const RectI crop = in.bounds.intersect(rod.toPixelEnclosing(scale, effect.getRenderMetadata(args.query.time, args.query.view)->getPixelAspectRatio(inputNb)));
    if (crop.isNull()) {
        return true;
    }
    *view = PassExec::cropGpuView(in, crop);

    return true;
}

NATRON_NAMESPACE_EXIT
