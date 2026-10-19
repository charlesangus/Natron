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
#include "Engine/Image.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/StagePlan.h"
#include "Engine/PoolParallelFor.h"
#include "Engine/RectD.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuBlur.h"
#endif

NATRON_NAMESPACE_ENTER

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

GpuStageSummary
NativeImageEffect::getGpuStageSummary(double time,
                                      ViewIdx view)
{
    GpuStageSummary summary;

    const int maskInput = getMaskInput();
    summary.applyMask = (maskInput >= 0) && getInput(maskInput) && isMaskEnabled(maskInput);
    if (summary.applyMask) {
        ImageLayerDesc maskLayer;
        int maskChannel = -1;
        if (resolveInputPlaneForRender(maskInput, time, view, &maskLayer, &maskChannel) && (maskChannel >= 0)) {
            summary.maskLayer = maskLayer;
            summary.maskChannel = maskChannel;
        }
    }

    KnobDoublePtr mix = std::dynamic_pointer_cast<KnobDouble>(getKnobByName(kOfxMixParamName));
    summary.mix = mix ? mix->getValueAtTime(time, 0, view) : 1.;

    NodePtr node = getNode();
    KnobChannelSelectPtr unPremultBy = (wantsHostUnPremultSelector() && node) ? node->getUnPremultBySelector() : KnobChannelSelectPtr();
    if (unPremultBy && !unPremultBy->isNone() && getInput(0)) {
        std::list<ImageLayerDesc> availableLayers;
        getAvailableLayers(time, view, 0, &availableLayers);
        ImageLayerDesc divisorLayer;
        const int divisorChannel = node->getUnPremultChannel(availableLayers, &divisorLayer);
        if (divisorLayer.getNumComponents() > 0) {
            summary.divisorLayer = divisorLayer;
            summary.divisorChannel = divisorChannel;
        }
    }

    return summary;
} // NativeImageEffect::getGpuStageSummary

PixelKernelPtr
NativeImageEffect::makeKernel(const KernelContext& /*context*/)
{
    return PixelKernelPtr();
}

StageSet
NativeImageEffect::stageSet() const
{
    const StageSet stages { Stage::MaskMix, Stage::PassThroughUnprocessed };

    return _traits.hostUnPremult ? stages.with(Stage::Premult) : stages;
}

StageKnobs
NativeImageEffect::stageKnobs() const
{
    StageKnobs knobs;

    knobs.mix = _mix;
    knobs.maskInvert = _maskInvert;

    return knobs;
}

StagePlan
NativeImageEffect::resolveGpuPlan(const GpuQuery& query) const
{
    // Resolving only reads knobs, the node's selectors and its inputs' layers.
    NativeImageEffect* self = const_cast<NativeImageEffect*>(this);

    return resolveGpuStagePlan(*self, stageSet(), stageKnobs(), 0, query);
}

bool
NativeImageEffect::canRenderOnGpu(const GpuQuery& query) const
{
#ifdef NATRON_HAVE_GPU
    if (!isPointOp() || !canRunStagesGpu(resolveGpuPlan(query), query)) {
        return false;
    }
    GpuRuntimeShader shader;
    if (gpuRuntimeShader(query, &shader)) {
        return _gpuShaderGate.accept(shader, getNode());
    }

    return gpuKernelId(query) >= 0;
#else
    Q_UNUSED(query);

    return false;
#endif
}

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
    return stageGpuMaskViewComponent(maskLayer, maskChannel, viewNComps);
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
    stageGpuInputLayer(*this, inputNb, plane, time, view, layer);

    return true;
}

bool
NativeImageEffect::getGpuUnPremultComponent(const GpuQuery& query,
                                            int* component)
{
    return stageGpuDivisorComponent(resolveGpuPlan(query), query, component);
}

StatusEnum
NativeImageEffect::renderGpu(const GpuRenderArgs& args)
{
#ifdef NATRON_HAVE_GPU
    const GpuQuery& query = args.query;
    if (!canRenderOnGpu(query) || args.outputRect.isNull()) {
        return eStatusFailed;
    }

    KernelContext context;
    context.time = query.time;
    context.view = query.view;
    context.mappedScale = query.mappedScale;
    for (int i = 0; i < 4; ++i) {
        context.processChannels[i] = query.processChannels[i];
    }

    GpuOpRef op;
    GpuRuntimeShader shader;
    if (gpuRuntimeShader(query, &shader)) {
        GpuShaderInputs inputs;
        if (!makeGpuShaderInputs(context, &inputs)) {
            return eStatusFailed;
        }
        op = GpuOpRef::runtime(shader, inputs);
    } else {
        op.kind = GpuOpRef::Kind::Kernel;
        if (!makeGpuKernel(context, &op.kernel)) {
            return eStatusFailed;
        }
    }

    return recordStagesGpu(resolveGpuPlan(query), args, op);
#else
    Q_UNUSED(args);

    return eStatusFailed;
#endif
} // NativeImageEffect::renderGpu

StatusEnum
NativeImageEffect::recordGpuDivide(const GpuRenderArgs& args,
                                   const GpuImageView& scratch)
{
    return recordStagesGpuDivide(resolveGpuPlan(args.query), args, scratch);
}

StatusEnum
NativeImageEffect::recordGpuFinish(const GpuRenderArgs& args,
                                   const GpuImageView& filtered)
{
    return recordStagesGpuFinish(resolveGpuPlan(args.query), args, filtered);
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

    const StagePlan plan = resolveStagePlan(*this, stageSet(), stageKnobs(), 0, args.time, args.view, args.processChannels);

    const PixelKernel* rowKernel = kernel.get();
    const auto processRow = [rowKernel](const RowIO& io) {
        rowKernel->processRow(io);
    };

    return runStagesCpu(*this, plan, args, StageRowFn(processRow));
} // NativeImageEffect::render

NATRON_NAMESPACE_EXIT
