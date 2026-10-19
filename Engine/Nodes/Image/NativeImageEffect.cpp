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

#include "NativeImageEffect.h"

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/GpuContext.h"
#include "Engine/Image.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/StagePlan.h"
#include "Engine/PoolParallelFor.h"
#include "Engine/RectD.h"

#ifdef NATRON_HAVE_GPU
#include <span>

#include "Engine/Gpu/GpuBlur.h"
#include "Engine/Gpu/GpuBuffer.h"
#include "Engine/Gpu/GpuKernel.h"
#include "Engine/Gpu/GpuPipelineCache.h"
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Gpu/GpuRuntime.h"
#include "Engine/Gpu/GpuShaderCompiler.h"
#include "Engine/Gpu/GpuTexture.h"
#include "Engine/Gpu/GpuWrap.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
#ifdef NATRON_HAVE_GPU
// GpuRecordContext::updateBuffer() writes at most this much.
const std::size_t kMaxGpuParamBytes = 65536;

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

uint32_t
channelMaskOf(const GpuQuery& query)
{
    std::bitset<4> channels;
    for (int i = 0; i < 4; ++i) {
        channels[i] = query.processChannels[i];
    }

    return (uint32_t)channels.to_ulong();
}

#endif // NATRON_HAVE_GPU
} // anonymous namespace

NativeImageEffect::NativeImageEffect(NodePtr node,
                                     const NativeImageTraits& traits)
    : NativeEffectBase(node)
    , _traits(traits)
    , _mix()
    , _maskInvert()
{
}

NativeImageEffect::~NativeImageEffect()
{
}

void
NativeImageEffect::addAcceptedComponents(int inputNb,
                                         std::list<ImageLayerDesc>* comps)
{
    if (isInputMask(inputNb)) {
        comps->push_back(ImageLayerDesc::getAlphaComponents());

        return;
    }
    NativeEffectBase::addAcceptedComponents(inputNb, comps);
}

void
NativeImageEffect::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

LayerKnobSpec
NativeImageEffect::getLayerKnobSpec() const
{
    if (_traits.generator) {
        return LayerKnobSpec(LayerKnobSpec::eKindLayerSelect, LayerKnobSpec::eRoleTarget, true);
    }

    return EffectInstance::getLayerKnobSpec();
}

bool
NativeImageEffect::isHostChannelSelectorSupported(bool* defaultR,
                                                  bool* defaultG,
                                                  bool* defaultB,
                                                  bool* defaultA) const
{
    *defaultR = _traits.defaultChannels[0];
    *defaultG = _traits.defaultChannels[1];
    *defaultB = _traits.defaultChannels[2];
    *defaultA = _traits.defaultChannels[3];

    return true;
}

int
NativeImageEffect::getMaskInput() const
{
    return stageMaskInput(*this);
}

PixelKernelPtr
NativeImageEffect::makeKernel(const KernelContext& /*context*/)
{
    return PixelKernelPtr();
}

bool
NativeImageEffect::canRenderOnGpu(const GpuQuery& query) const
{
#ifdef NATRON_HAVE_GPU
    if (!isPointOp() || (query.bitDepth != eImageBitDepthFloat) || (query.nComps < 1) || (query.nComps > 4)) {
        return false;
    }
    if ((query.unPremultChannel >= 0) && !query.unPremultInPlane) {
        return false;
    }
    GpuRuntimeShader shader;
    if (gpuRuntimeShader(query, &shader)) {
        return acceptGpuRuntimeShader(shader);
    }

    return gpuKernelId(query) >= 0;
#else
    Q_UNUSED(query);

    return false;
#endif
}

bool
NativeImageEffect::acceptGpuRuntimeShader(const GpuRuntimeShader& shader) const
{
#ifdef NATRON_HAVE_GPU
    std::string key = shader.entry;
    key += '\0';
    key += shader.source;
    {
        std::lock_guard<std::mutex> lock(_gpuShaderMutex);
        if (_rejectedGpuShaders.count(key)) {
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
    NodePtr node = getNode();
    if (status) {
        std::string posted;
        {
            std::lock_guard<std::mutex> lock(_gpuShaderMutex);
            posted.swap(_gpuShaderMessage);
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
    const std::string message = tr("The GPU shader cannot be used, so this node renders on the CPU.\n%1").arg(QString::fromUtf8(status.message.c_str())).toStdString();
    {
        std::lock_guard<std::mutex> lock(_gpuShaderMutex);
        if (!_rejectedGpuShaders.insert(key).second) {
            return false;
        }
        _gpuShaderMessage = message;
    }
    if (node) {
        node->setPersistentMessage(eMessageTypeWarning, message);
    }

    return false;
#else
    Q_UNUSED(shader);

    return false;
#endif
} // NativeImageEffect::acceptGpuRuntimeShader

StatusEnum
NativeImageEffect::renderGpuRuntimeShader(const GpuRenderArgs& args,
                                          const GpuRuntimeShader& shader)
{
#ifdef NATRON_HAVE_GPU
    const GpuQuery& query = args.query;
    const RectI& rect = args.outputRect;
    const int nComps = query.nComps;
    gpu::GpuRecordContext& ctx = args.context;

    std::shared_ptr<gpu::GpuKernel> kernel;
    if (!ctx.runtimeKernel(shader.source, gpu::ShaderStage::Compute, shader.entry, kernel) || !kernel || !followsRuntimeShaderLayout(*kernel)) {
        return eStatusFailed;
    }

    KernelContext context;
    context.time = query.time;
    context.view = query.view;
    context.mappedScale = query.mappedScale;
    for (int i = 0; i < 4; ++i) {
        context.processChannels[i] = query.processChannels[i];
    }
    GpuShaderInputs inputs;
    if (!makeGpuShaderInputs(context, &inputs)) {
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

    if (recordGpuDivide(args, dividedView) != eStatusOK) {
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
    region.channelMask = channelMaskOf(query);
    const std::span<const std::byte> push = std::as_bytes(std::span<const GpuShaderRegion>(&region, 1)).first(kernel->desc().pushConstantBytes);
    if (!ctx.dispatchBindings(kernel, bindings, push, { region.width, region.height, 1u })) {
        return eStatusFailed;
    }
    ctx.computeBarrier();

    return recordGpuFinish(args, filteredView);
#else
    Q_UNUSED(args);
    Q_UNUSED(shader);

    return eStatusFailed;
#endif
} // NativeImageEffect::renderGpuRuntimeShader

bool
NativeImageEffect::isUsableGpuView(const GpuImageView& view,
                                   int nComps)
{
    return view.buffer && (view.nComps == nComps) && (view.rowStrideFloats >= view.bounds.width() * nComps) && ((view.byteOffset % sizeof(float)) == 0);
}

GpuImageView
NativeImageEffect::cropGpuView(const GpuImageView& view,
                               const RectI& r)
{
    GpuImageView out;
    if ((r.x2 <= r.x1) || (r.y2 <= r.y1)) {
        return out;
    }
    out = view;
    out.bounds = r;
    out.byteOffset += ((uint64_t)(r.y1 - view.bounds.y1) * view.rowStrideFloats + (uint64_t)(r.x1 - view.bounds.x1) * view.nComps) * sizeof(float);

    return out;
}

int
NativeImageEffect::getGpuMaskViewComponent(const ImageLayerDesc& maskLayer,
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

#ifdef NATRON_HAVE_GPU
gpu::GpuBlurRect
NativeImageEffect::toGpuBlurRect(const RectI& r)
{
    gpu::GpuBlurRect g;
    g.x1 = r.x1;
    g.y1 = r.y1;
    g.x2 = r.x2;
    g.y2 = r.y2;

    return g;
}

gpu::GpuBlurImage
NativeImageEffect::toGpuBlurImage(const GpuImageView& view)
{
    gpu::GpuBlurImage g;
    g.buffer = view.buffer;
    g.offsetFloats = view.byteOffset / sizeof(float);
    g.bounds = toGpuBlurRect(view.bounds);
    g.strideFloats = (uint32_t)view.rowStrideFloats;

    return g;
}
#endif // NATRON_HAVE_GPU

bool
NativeImageEffect::getGpuInputLayer(int inputNb,
                                    const ImageLayerDesc& plane,
                                    double time,
                                    ViewIdx view,
                                    ImageLayerDesc* layer) const
{
    *layer = plane;
    if (!isInputMask(inputNb)) {
        return true;
    }
    ImageLayerDesc maskLayer;
    int maskChannel = -1;
    // Resolving only reads the node's selectors and its inputs' layers.
    NativeImageEffect* self = const_cast<NativeImageEffect*>(this);
    if (self->resolveInputPlaneForRender(inputNb, time, view, &maskLayer, &maskChannel)) {
        *layer = maskLayer;
    }

    return true;
}

bool
NativeImageEffect::getGpuUnPremultComponent(const GpuQuery& query,
                                            int* component)
{
    *component = -1;
    NodePtr node = getNode();
    KnobChannelSelectPtr unPremultBy = (_traits.hostUnPremult && node) ? node->getUnPremultBySelector() : KnobChannelSelectPtr();
    if (!unPremultBy || unPremultBy->isNone() || !getInput(0)) {
        return true;
    }
    std::list<ImageLayerDesc> availableLayers;
    getAvailableLayers(query.time, query.view, 0, &availableLayers);
    ImageLayerDesc divisorLayer;
    const int divisorChannel = node->getUnPremultChannel(availableLayers, &divisorLayer);
    if ((divisorChannel < 0) || (divisorLayer.getNumComponents() == 0)) {
        return true;
    }
    if (!query.unPremultInPlane) {
        return false;
    }
    // A colour divisor lies in a colour plane of nComps channels; any other lies in its own layer.
    const ImageLayerDesc plane = divisorLayer.isColorLayer() ? ImageLayerDesc::mapNCompsToColorLayer(query.nComps) : divisorLayer;
    const int c = Node::getUnPremultSkipChannel(plane, divisorLayer, divisorChannel);
    if ((c < 0) || (c >= query.nComps)) {
        return false;
    }
    *component = c;

    return true;
}

bool
NativeImageEffect::makeGpuWrap(const GpuRenderArgs& args,
                               gpu::GpuWrap* wrap)
{
#ifdef NATRON_HAVE_GPU
    const GpuQuery& query = args.query;
    const double time = query.time;
    const ViewIdx view = query.view;
    const int nComps = query.nComps;

    wrap->nComps = (uint32_t)nComps;
    wrap->channelMask = channelMaskOf(query);
    wrap->mix = (float)getMixValue(time, view);

    // An input that is not connected reads zero, as render() reads it.
    const bool hasSource = !args.inputs.empty() && args.inputs[0].buffer;
    wrap->src.nComps = (uint32_t)nComps;
    if (hasSource && !toWrapView(args.inputs[0], &wrap->src)) {
        return false;
    }

    if (hasSource) {
        int component = -1;
        if (!getGpuUnPremultComponent(query, &component)) {
            return false;
        }
        wrap->divisorChannel = component;
        wrap->skipChannel = component;
    }

    const int maskInput = getMaskInput();
    if (query.hasMask && (maskInput >= 0)) {
        wrap->applyMask = true;
        wrap->maskInvert = getMaskInvertValue(time, view);
        // A mask render() cannot fetch reads zero everywhere.
        wrap->mask.nComps = 1;
        const bool hasMask = (maskInput < (int)args.inputs.size()) && args.inputs[maskInput].buffer;
        ImageLayerDesc maskLayer;
        int maskChannel = -1;
        if (hasMask && resolveInputPlaneForRender(maskInput, time, view, &maskLayer, &maskChannel) && (maskChannel >= 0) && (maskChannel < maskLayer.getNumComponents())) {
            const GpuImageView& maskView = args.inputs[maskInput];
            const int component = getGpuMaskViewComponent(maskLayer, maskChannel, maskView.nComps);
            if ((component < 0) || (component >= maskView.nComps) || !toWrapView(maskView, &wrap->mask)) {
                return false;
            }
            wrap->maskChannel = (uint32_t)component;
        }
    }

    return true;
#else
    Q_UNUSED(args);
    Q_UNUSED(wrap);

    return false;
#endif
}

StatusEnum
NativeImageEffect::renderGpu(const GpuRenderArgs& args)
{
#ifdef NATRON_HAVE_GPU
    const GpuQuery& query = args.query;
    if (!canRenderOnGpu(query) || args.outputRect.isNull()) {
        return eStatusFailed;
    }

    GpuRuntimeShader shader;
    if (gpuRuntimeShader(query, &shader)) {
        return renderGpuRuntimeShader(args, shader);
    }

    KernelContext context;
    context.time = query.time;
    context.view = query.view;
    context.mappedScale = query.mappedScale;
    for (int i = 0; i < 4; ++i) {
        context.processChannels[i] = query.processChannels[i];
    }
    PixelKernelGpu kernel;
    if (!makeGpuKernel(context, &kernel) || (kernel.kernel < 0) || (kernel.params.size() > kMaxGpuParamBytes)) {
        return eStatusFailed;
    }

    gpu::GpuWrap wrap;
    if (!makeGpuWrap(args, &wrap)) {
        return eStatusFailed;
    }
    wrap.output = gpu::GpuWrapRect { args.outputRect.x1, args.outputRect.y1, args.outputRect.x2, args.outputRect.y2 };
    if (!toWrapView(args.output, &wrap.dst)) {
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

    return gpu::recordWrapped(ctx, (gpu::GpuKernelId)kernel.kernel, wrap, params) ? eStatusOK : eStatusFailed;
#else
    Q_UNUSED(args);

    return eStatusFailed;
#endif
} // NativeImageEffect::renderGpu

StatusEnum
NativeImageEffect::recordGpuDivide(const GpuRenderArgs& args,
                                   const GpuImageView& scratch)
{
#ifdef NATRON_HAVE_GPU
    gpu::GpuWrap wrap;
    if (!makeGpuWrap(args, &wrap) || !toWrapView(scratch, &wrap.dst)) {
        return eStatusFailed;
    }
    wrap.output = wrap.dst.bounds;
    wrap.applyMask = false;
    wrap.mix = 1.f;

    return gpu::recordWrapDivide(args.context, wrap) ? eStatusOK : eStatusFailed;
#else
    Q_UNUSED(args);
    Q_UNUSED(scratch);

    return eStatusFailed;
#endif
}

StatusEnum
NativeImageEffect::recordGpuFinish(const GpuRenderArgs& args,
                                   const GpuImageView& filtered)
{
#ifdef NATRON_HAVE_GPU
    gpu::GpuWrap wrap;
    gpu::GpuWrapView filteredView;
    if (args.outputRect.isNull() || !makeGpuWrap(args, &wrap) || !toWrapView(args.output, &wrap.dst) || !toWrapView(filtered, &filteredView)) {
        return eStatusFailed;
    }
    wrap.output = gpu::GpuWrapRect { args.outputRect.x1, args.outputRect.y1, args.outputRect.x2, args.outputRect.y2 };

    return gpu::recordWrapFinish(args.context, wrap, filteredView) ? eStatusOK : eStatusFailed;
#else
    Q_UNUSED(args);
    Q_UNUSED(filtered);

    return eStatusFailed;
#endif
}

void
NativeImageEffect::addMaskMixKnobs(const KnobPagePtr& page)
{
    KnobBoolPtr maskInvert = createKnob<KnobBool>(std::string("Invert Mask"));

    maskInvert->setName(kOfxMaskInvertParamName);
    maskInvert->setHintToolTip(tr("When checked, the effect is fully applied where the mask is 0."));
    maskInvert->setDefaultValue(false);
    if (page) {
        page->addKnob(maskInvert);
    }
    _maskInvert = maskInvert;

    KnobDoublePtr mix = createKnob<KnobDouble>(std::string("Mix"));
    mix->setName(kOfxMixParamName);
    mix->setHintToolTip(tr("Mix factor between the original and the transformed image."));
    mix->setDefaultValue(1.);
    mix->setIncrement(0.01);
    mix->setMinimum(0.);
    mix->setMaximum(1.);
    mix->setDisplayMinimum(0.);
    mix->setDisplayMaximum(1.);
    if (page) {
        page->addKnob(mix);
    }
    _mix = mix;
}

double
NativeImageEffect::getMixValue(double time,
                               ViewIdx view) const
{
    KnobDoublePtr mix = _mix.lock();

    return mix ? mix->getValueAtTime(time, 0, view) : 1.;
}

bool
NativeImageEffect::getMaskInvertValue(double time,
                                      ViewIdx view) const
{
    KnobBoolPtr maskInvert = _maskInvert.lock();

    return maskInvert ? maskInvert->getValueAtTime(time, 0, view) : false;
}

bool
NativeImageEffect::isMaskApplied() const
{
    return isStageMaskApplied(*this);
}

bool
NativeImageEffect::isIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view,
                              double* inputTime,
                              ViewIdx* inputView,
                              int* inputNb)
{
    *inputTime = time;
    *inputView = view;
    *inputNb = 0;

    if (getMixValue(time, view) == 0.) {
        return true;
    }
    if (isIdentityOp(time, scale, roi, view)) {
        return true;
    }

    // A mask that does not reach the window leaves the source unchanged there; an inverted
    // one applies the effect in full instead.
    if (isMaskApplied() && !getMaskInvertValue(time, view) && stageMaskMissesWindow(*this, getMaskInput(), time, scale, roi, view)) {
        return true;
    }

    return false;
} // NativeImageEffect::isIdentity

void
NativeImageEffect::makeRowBands(const RectI& roi,
                                int nThreads,
                                std::vector<RectI>* bands)
{
    makeStageRowBands(roi, nThreads, bands);
}

StatusEnum
NativeImageEffect::render(const RenderActionArgs& args)
{
    if (!isPointOp()) {
        return eStatusFailed;
    }

    KernelContext context;
    context.time = args.time;
    context.view = args.view;
    context.mappedScale = args.mappedScale;
    context.processChannels = args.processChannels;
    const PixelKernelPtr kernel = makeKernel(context);
    if (!kernel) {
        return eStatusFailed;
    }

    StageSet stages { Stage::MaskMix, Stage::PassThroughUnprocessed };
    if (_traits.hostUnPremult) {
        stages = stages.with(Stage::Premult);
    }
    StageKnobs knobs;
    knobs.mix = _mix;
    knobs.maskInvert = _maskInvert;
    const StagePlan plan = resolveStagePlan(*this, stages, knobs, 0, args.time, args.view, args.processChannels);

    const PixelKernel* rowKernel = kernel.get();
    const auto processRow = [rowKernel](const RowIO& io) {
        rowKernel->processRow(io);
    };

    return runStagesCpu(*this, plan, args, StageRowFn(processRow));
} // NativeImageEffect::render

NATRON_NAMESPACE_EXIT
