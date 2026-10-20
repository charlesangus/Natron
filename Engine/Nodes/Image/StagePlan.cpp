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

ImagePtr
fetchStageSource(EffectInstance& effect,
                 const StageSource& source,
                 const EffectInstance::RenderActionArgs& args)
{
    if (!effect.getInput(source.inputNb)) {
        return ImagePtr();
    }
    RectI roi;
    if (source.mask) {
        if (!source.maskApplied || (source.maskChannel < 0)) {
            return ImagePtr();
        }
        ImagePtr image = effect.getImage(source.inputNb, source.time, args.mappedScale, args.view, NULL, &source.maskLayer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &roi);
        if (image && (source.maskChannel >= (int)image->getComponentsCount())) {
            image.reset();
        }

        return image;
    }

    ImageLayerDesc layer;
    bool mapToClipPrefs = true;
    if (!source.layerKnob.empty()) {
        if (!resolveStageSourceLayer(effect, source, args.view, &layer)) {
            return ImagePtr();
        }
        // A selected layer is read as it is stored, not in the clip's colour layout.
        mapToClipPrefs = false;
    } else if (!effect.resolveInputPlaneForRender(source.layerInput, source.time, args.view, &layer, NULL)) {
        return ImagePtr();
    }

    return effect.getImage(source.inputNb, source.time, args.mappedScale, args.view, NULL, &layer, mapToClipPrefs, false /*dontUpscale*/, eStorageModeRAM, NULL, &roi);
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

// Binding 0 and 1 are the source and destination buffers, and the push constants, if any, a prefix
// of GpuShaderRegion.
bool
followsRuntimeShaderLayout(const gpu::GpuKernel& kernel)
{
    if (kernel.desc().pushConstantBytes > sizeof(GpuShaderRegion)) {
        return false;
    }
    int buffers = 0;
    for (const gpu::GpuKernelBinding& b : kernel.bindings()) {
        if ((b.binding <= 1) && (b.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)) {
            ++buffers;
        }
    }

    return buffers == 2;
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

// plan's Composite sources as args hands them over, read as runStagesCpu() reads them: an input
// not handed over, and a Mask-role input that is not applied or resolves to no channel, reads
// zero.
bool
makeStageGpuSources(const StagePlan& plan,
                    const GpuRenderArgs& args,
                    gpu::GpuWrapSources* sources)
{
    if ((plan.nSources < 0) || (plan.nSources > (int)gpu::kGpuWrapMaxSources)) {
        return false;
    }
    sources->count = (uint32_t)plan.nSources;
    for (int s = 0; s < plan.nSources; ++s) {
        const StageSource& source = plan.sources[s];
        gpu::GpuWrapSource& out = sources->list[s];
        const int inputNb = source.inputNb;
        if ((inputNb < 0) || (inputNb >= (int)args.inputs.size()) || !args.inputs[inputNb].buffer) {
            continue;
        }
        const GpuImageView& view = args.inputs[inputNb];
        if (source.mask) {
            if (!source.maskApplied || (source.maskChannel < 0) || (source.maskChannel >= source.maskLayer.getNumComponents())) {
                continue;
            }
            const int component = stageGpuMaskViewComponent(source.maskLayer, source.maskChannel, view.nComps);
            if ((component < 0) || (component >= view.nComps)) {
                return false;
            }
            out.channel = component;
        }
        if (!toWrapView(view, &out.view)) {
            return false;
        }
    }

    return true;
} // makeStageGpuSources

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

    gpu::GpuWrapSources sources;
    if ((plan.nSources > 0) && !makeStageGpuSources(plan, args, &sources)) {
        return eStatusFailed;
    }

    gpu::GpuRecordContext& ctx = args.context;
    gpu::GpuBuffer* params = nullptr;
    if (!kernel.params.empty()) {
        std::vector<std::byte> block(kernel.params);
        block.resize((block.size() + 3) & ~std::size_t(3), std::byte(0));
        if (!ctx.scratch(block.size(), params) || !params || !ctx.updateBuffer(*params, 0, std::span<const std::byte>(block.data(), block.size()))) {
            return eStatusFailed;
        }
    }

    if (plan.nSources > 0) {
        return gpu::recordWrappedSources(ctx, (gpu::GpuKernelId)kernel.kernel, wrap, sources, params) ? eStatusOK : eStatusFailed;
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
        if (b.binding <= 1) {
            bindings.push_back(gpu::GpuBinding::storage(b.binding, (b.binding == 0) ? *divided : *filtered));
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
            if (!texture || (texture->dimensions < 1) || (texture->width < 1) || (texture->height < 1) || (texture->depth < 1) || (texture->channels < 1)) {
                return eStatusFailed;
            }
            gpu::GpuTextureDesc desc;
            desc.dimensions = (uint32_t)texture->dimensions;
            desc.width = (uint32_t)texture->width;
            desc.height = (uint32_t)texture->height;
            desc.depth = (uint32_t)texture->depth;
            desc.channels = (uint32_t)texture->channels;
            desc.filter = texture->linear ? gpu::GpuTextureFilter::Linear : gpu::GpuTextureFilter::Nearest;
            const gpu::GpuTexture* created = nullptr;
            if (!ctx.texture(desc, std::span<const float>(texture->texels.data(), texture->texels.size()), created) || !created) {
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
    const RectI maskPixels = maskRoD.toPixelEnclosing(scale.toMipmapLevel(), mask->getAspectRatio(-1));

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
    KnobChannelSelectPtr unPremultBy = (stages.has(Stage::Premult) && node) ? node->getUnPremultBySelector() : KnobChannelSelectPtr();
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
resolveStageSources(EffectInstance& effect,
                    const NodeSpec* spec,
                    const CompositeSources& sources,
                    StagePlan* plan)
{
    plan->nSources = 0;
    const int count = std::min(sources.count, (int)kPixelKernelMaxInputs);
    const int nSpecInputs = spec ? spec->inputCount() : 0;
    for (int i = 0; i < count; ++i) {
        StageSource& source = plan->sources[i];
        source = StageSource();
        source.inputNb = sources.list[i].inputNb;
        source.time = sources.list[i].time;
        source.layerInput = source.inputNb;

        const NodeSpec::ResolvedInput resolved = spec ? spec->resolveInput(source.inputNb) : NodeSpec::ResolvedInput { nullptr, 0 };
        if (resolved.spec && (resolved.spec->role == InputRole::Mask)) {
            source.mask = true;
            source.maskApplied = effect.getInput(source.inputNb) && effect.isMaskEnabled(source.inputNb);
            ImageLayerDesc maskLayer;
            int maskChannel = -1;
            if (source.maskApplied && effect.resolveInputPlaneForRender(source.inputNb, source.time, plan->view, &maskLayer, &maskChannel) && (maskChannel >= 0)) {
                source.maskLayer = maskLayer;
                source.maskChannel = maskChannel;
            }
            continue;
        }

        // Every step of a pairing names a distinct input, so a chain longer than the inputs loops.
        const InputSpec* layerSpec = resolved.spec;
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
    }
    plan->nSources = count;
} // resolveStageSources

bool
resolveStageSourceLayer(EffectInstance& effect,
                        const StageSource& source,
                        ViewIdx view,
                        ImageLayerDesc* layer)
{
    KnobLayerSelectPtr knob = effect.getKnobByNameAndType<KnobLayerSelect>(std::string(source.layerKnob));
    std::list<ImageLayerDesc> present;
    effect.getAvailableLayers(source.time, view, source.layerInput, &present);
    ResolvedLayer resolved;
    if (!knob || !knob->resolve(present, &resolved) || (resolved.desc.getNumComponents() == 0)) {
        return false;
    }
    *layer = resolved.desc;

    return true;
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

StatusEnum
runStagesCpu(EffectInstance& effect,
             const StagePlan& plan,
             const EffectInstance::RenderActionArgs& args,
             const StageRowFn& row)
{
    const float mix = (float)plan.mix;
    const bool maskInvert = plan.maskInvert;
    const bool passThrough = plan.stages.has(Stage::PassThroughUnprocessed);
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
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
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

    // A Composite op's inputs, per output plane; a Mask-role input is the same for every plane.
    const int nSources = plan.nSources;
    std::vector<std::vector<SourceImage>> jobSources(nSources > 0 ? jobs.size() : 0, std::vector<SourceImage>(nSources));
    for (int s = 0; s < nSources; ++s) {
        const StageSource& source = plan.sources[s];
        ImagePtr maskImage = source.mask ? fetchStageSource(effect, source, args) : ImagePtr();
        if (!PassExec::isFloatImage(maskImage)) {
            return eStatusFailed;
        }
        for (std::size_t j = 0; j < jobSources.size(); ++j) {
            if (!jobs[j].dst) {
                continue;
            }
            jobSources[j][s].image = source.mask ? maskImage : fetchStageSource(effect, source, args);
            if (!PassExec::isFloatImage(jobSources[j][s].image)) {
                return eStatusFailed;
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
        if (nSources > 0) {
            for (SourceImage& source : jobSources[j]) {
                source.lock();
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
        std::vector<float> sourceRow(rowSize);
        std::vector<float> dividedRow(rowSize);
        std::vector<float> kernelRow(rowSize);
        std::vector<float> maskRow(doMask ? width : 0);
        std::vector<float> divisorRow(job.divisor ? width : 0);
        std::vector<float> sourceRows[kPixelKernelMaxInputs];

        RowIO io;
        io.src[0] = &dividedRow[0];
        io.nSrc = 1;
        if (nSources > 0) {
            for (int s = 0; s < nSources; ++s) {
                sourceRows[s].resize(plan.sources[s].mask ? (std::size_t)width : rowSize);
                io.src[s] = &sourceRows[s][0];
            }
            io.nSrc = nSources;
        }
        io.mask = doMask ? &maskRow[0] : 0;
        io.divisor = job.divisor ? &divisorRow[0] : 0;
        io.divisorSkipChannel = job.divisor ? job.skipChannel : -1;
        io.dst = &kernelRow[0];
        io.x0 = roi.x1;
        io.width = width;
        io.nComps = nComps;
        io.channels = job.channels;

        for (int y = band.y1; y < band.y2; ++y) {
            if ((((y - band.y1) % kAbortCheckRows) == 0) && cancel.check()) {
                return;
            }

            PassExec::readSourceRow(job.src.get(), job.srcAccess.get(), job.srcBounds, job.srcNComps, roi.x1, y, width, nComps, &sourceRow[0]);
            std::copy(sourceRow.begin(), sourceRow.end(), dividedRow.begin());
            if (job.divisor) {
                PassExec::readChannelRow(job.divisor.get(), job.divisorAccess.get(), job.divisorBounds, job.divisorNComps, job.divisorChannel, roi.x1, y, width, 1.f, &divisorRow[0]);
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
            for (int s = 0; s < nSources; ++s) {
                const SourceImage& source = jobSources[band.job][s];
                if (plan.sources[s].mask) {
                    PassExec::readChannelRow(source.image.get(), source.access.get(), source.bounds, source.nComps, plan.sources[s].maskChannel, roi.x1, y, width, 0.f, &sourceRows[s][0]);
                } else {
                    PassExec::readSourceRow(source.image.get(), source.access.get(), source.bounds, source.nComps, roi.x1, y, width, nComps, &sourceRows[s][0]);
                }
            }

            io.y = y;
            row(io);

            float* dstPix = (float*)job.dstAccess->pixelAt(roi.x1, y);
            if (!dstPix) {
                continue;
            }
            for (int i = 0; i < width; ++i, dstPix += nComps) {
                const float* srcPix = &sourceRow[(std::size_t)i * nComps];
                const float* outPix = &kernelRow[(std::size_t)i * nComps];
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
                    if (job.divisor && (c != job.skipChannel)) {
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

bool
canRunStageSourcesGpu(const StagePlan& plan,
                      const GpuQuery& query)
{
    if ((plan.nSources < 1) || (plan.nSources > kPixelKernelMaxInputs) || (plan.divisorChannel >= 0)) {
        return false;
    }
    for (int s = 0; s < plan.nSources; ++s) {
        const StageSource& source = plan.sources[s];
        if (source.time != query.time) {
            return false;
        }
        const bool inPlane = !source.mask && source.layerKnob.empty();
        if (!inPlane && (source.inputNb == plan.sourceInput)) {
            return false;
        }
        if (!source.mask && plan.applyMask && (source.inputNb == plan.maskInput)) {
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

bool
StageGpuShaderGate::accept(const GpuRuntimeShader& shader,
                           const NodePtr& node)
{
#ifdef NATRON_HAVE_GPU
    std::string key = shader.entry;
    key += '\0';
    key += shader.source;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_rejected.count(key)) {
            return false;
        }
    }
    GpuContext* context = appPTR ? appPTR->getGpuContext() : nullptr;
    gpu::GpuRuntime* runtime = (context && context->isAvailable()) ? context->runtime() : nullptr;
    if (!runtime || runtime->isLost()) {
        return false;
    }
    std::shared_ptr<gpu::GpuKernel> kernel;
    gpu::GpuStatus status = runtime->pipelines().get(shader.source, gpu::ShaderStage::Compute, shader.entry, kernel);
    if (status && (!kernel || !followsRuntimeShaderLayout(*kernel))) {
        status = gpu::GpuStatus { VK_ERROR_INITIALIZATION_FAILED, "bindings 0 and 1 must be storage buffers and the push constants at most a GpuShaderRegion" };
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
    switch (op.kind) {
    case GpuOpRef::Kind::Kernel:

        return (op.kernel.kernel >= 0) && (op.kernel.params.size() <= kMaxGpuParamBytes);
    case GpuOpRef::Kind::RuntimeShader:

        return gate.accept(op.shader, node);
    case GpuOpRef::Kind::None:
        break;
    }

    return false;
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
        if (plan.nSources > 0) {
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

NATRON_NAMESPACE_EXIT
