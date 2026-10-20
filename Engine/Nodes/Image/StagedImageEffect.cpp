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

#include "StagedImageEffect.h"

#include <bitset>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/RectD.h"

NATRON_NAMESPACE_ENTER

StagedImageEffect::StagedImageEffect(NodePtr node)
    : NativeEffectBase(node)
    , _stageKnobs()
{
}

StagedImageEffect::~StagedImageEffect()
{
}

void
StagedImageEffect::addAcceptedComponents(int inputNb,
                                         std::list<ImageLayerDesc>* comps)
{
    if (isInputMask(inputNb)) {
        comps->push_back(ImageLayerDesc::getAlphaComponents());

        return;
    }
    NativeEffectBase::addAcceptedComponents(inputNb, comps);
}

void
StagedImageEffect::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
StagedImageEffect::wantsHostUnPremultSelector() const
{
    return imagePipelineSpec().stages.has(Stage::Premult);
}

bool
StagedImageEffect::rendersUnprocessedChannels() const
{
    return imagePipelineSpec().stages.has(Stage::PassThroughUnprocessed);
}

LayerKnobSpec
StagedImageEffect::getLayerKnobSpec() const
{
    switch (imagePipelineSpec().layers.kind) {
    case LayerPolicy::Kind::TargetSelect:

        return LayerKnobSpec(LayerKnobSpec::eKindLayerSelect, LayerKnobSpec::eRoleTarget, true);
    case LayerPolicy::Kind::None:

        return LayerKnobSpec();
    case LayerPolicy::Kind::Color:
    case LayerPolicy::Kind::All:
        break;
    }

    return EffectInstance::getLayerKnobSpec();
}

bool
StagedImageEffect::defaultProcessesAllLayers() const
{
    return imagePipelineSpec().layers.kind == LayerPolicy::Kind::All;
}

bool
StagedImageEffect::isHostChannelSelectorSupported(bool* defaultR,
                                                  bool* defaultG,
                                                  bool* defaultB,
                                                  bool* defaultA) const
{
    const LayerPolicy& layers = imagePipelineSpec().layers;

    *defaultR = layers.defaults[0];
    *defaultG = layers.defaults[1];
    *defaultB = layers.defaults[2];
    *defaultA = layers.defaults[3];

    return layers.kind != LayerPolicy::Kind::None;
}

std::optional<OpKind>
StagedImageEffect::opKind() const
{
    return imagePipelineSpec().op;
}

StagePlan
StagedImageEffect::resolveGpuPlan(const GpuQuery& query) const
{
    // Resolving only reads knobs, the node's selectors and its inputs' layers.
    StagedImageEffect* self = const_cast<StagedImageEffect*>(this);

    if (imagePipelineSpec().op != OpKind::Composite) {
        return resolveGpuStagePlan(*self, imagePipelineSpec().stages, _stageKnobs, getSourceInput(), query);
    }
    const CompositeInputs inputs = opInputs(query.time, query.view);
    StagePlan plan = resolveGpuStagePlan(*self, imagePipelineSpec().stages, _stageKnobs, inputs.blend(), query);
    resolveStageSources(nodeSpec(), inputs, &plan);

    return plan;
}

bool
StagedImageEffect::canRenderOnGpu(const GpuQuery& query) const
{
    const OpKind kind = imagePipelineSpec().op;

    if (kind == OpKind::Passes) {
        return canRecordPassesGpu(resolveGpuPlan(query), query, opPasses(query.time, query.view, query.mappedScale));
    }
    if (kind == OpKind::Resample) {
        return opResamplesOnGpu() && canRecordResampleGpu(*this, resolveGpuPlan(query), query, opResample(query.time, query.view, query.mappedScale));
    }
    if (kind == OpKind::Custom) {
        return canRenderOpOnGpu(query);
    }
    if ((kind != OpKind::Point) && (kind != OpKind::Composite)) {
        return false;
    }
    const StagePlan plan = resolveGpuPlan(query);
    if (!canRunStagesGpu(plan, query) || ((kind == OpKind::Composite) && !canRunStageSourcesGpu(plan, query))) {
        return false;
    }

    const GpuOpRef op = gpuOp(query.time, query.view, query.mappedScale);
    if ((kind == OpKind::Composite) && (op.kind != GpuOpRef::Kind::Kernel)) {
        return false;
    }

    return op.acceptsComponents(query.nComps) && canRecordGpuOp(op, _gpuShaderGate, getNode());
}

RectI
StagedImageEffect::getGpuInputRoI(int inputNb,
                                  const RectI& outputRect,
                                  const RenderScale& scale) const
{
    const OpKind kind = imagePipelineSpec().op;
    const int source = getSourceInput();

    if ((inputNb == source) && (kind == OpKind::Passes)) {
        return passListSourceRoI(outputRect, opPasses(getCurrentTime(), getCurrentView(), scale));
    }
    if ((inputNb == source) && (kind == OpKind::Resample) && opResamplesOnGpu()) {
        const double time = getCurrentTime();
        const ViewIdx view = getCurrentView();

        return resampleGpuSourceRoI(*this, source, opResample(time, view, scale), getResampleRegionStages(time, view), time, view, outputRect, scale);
    }

    return NativeEffectBase::getGpuInputRoI(inputNb, outputRect, scale);
}

bool
StagedImageEffect::getGpuInputLayer(int inputNb,
                                    const ImageLayerDesc& plane,
                                    double time,
                                    ViewIdx view,
                                    ImageLayerDesc* layer) const
{
    const OpKind kind = imagePipelineSpec().op;

    if (kind == OpKind::Custom) {
        return getOpGpuInputLayer(inputNb, plane, time, view, layer);
    }
    if ((kind != OpKind::Point) && (kind != OpKind::Composite) && (kind != OpKind::Passes) && ((kind != OpKind::Resample) || !opResamplesOnGpu())) {
        return false;
    }
    stageGpuInputLayer(*this, inputNb, plane, time, view, layer);

    return true;
}

StatusEnum
StagedImageEffect::renderGpu(const GpuRenderArgs& args)
{
    const GpuQuery& query = args.query;
    const OpKind kind = imagePipelineSpec().op;

    if (kind == OpKind::Passes) {
        return recordPassesGpu(*this, resolveGpuPlan(query), args, opPasses(query.time, query.view, query.mappedScale));
    }
    if (kind == OpKind::Resample) {
        if (!opResamplesOnGpu()) {
            return eStatusFailed;
        }

        return recordResampleGpu(*this, resolveGpuPlan(query), args, opResample(query.time, query.view, query.mappedScale), getResampleRegionStages(query.time, query.view));
    }
    if (kind == OpKind::Custom) {
        return renderOpGpu(args);
    }
    if ((kind != OpKind::Point) && (kind != OpKind::Composite)) {
        return eStatusFailed;
    }
    const StagePlan plan = resolveGpuPlan(query);
    const GpuOpRef op = gpuOp(query.time, query.view, query.mappedScale);
    if (!canRunStagesGpu(plan, query) || !op.acceptsComponents(query.nComps) || !canRecordGpuOp(op, _gpuShaderGate, getNode())) {
        return eStatusFailed;
    }
    if ((kind == OpKind::Composite) && (!canRunStageSourcesGpu(plan, query) || (op.kind != GpuOpRef::Kind::Kernel))) {
        return eStatusFailed;
    }

    return recordStagesGpu(plan, args, op);
}

GpuStageSummary
StagedImageEffect::getGpuStageSummary(double time,
                                      ViewIdx view)
{
    const StagePlan plan = resolveStagePlan(*this, imagePipelineSpec().stages, _stageKnobs, getBlendInput(time, view), time, view, std::bitset<4>());
    GpuStageSummary summary;

    summary.applyMask = plan.applyMask;
    summary.maskLayer = plan.maskLayer;
    summary.maskChannel = plan.maskChannel;
    summary.mix = plan.mix;
    summary.divisorLayer = plan.divisorLayer;
    summary.divisorChannel = plan.divisorChannel;

    return summary;
}

int
StagedImageEffect::getSourceInput() const
{
    const NodeSpec* spec = nodeSpec();

    return spec ? nodeSpecSourceInput(*spec) : 0;
}

int
StagedImageEffect::getBlendInput(double time,
                                 ViewIdx view) const
{
    if (imagePipelineSpec().op != OpKind::Composite) {
        return getSourceInput();
    }

    return opInputs(time, view).blend();
}

int
StagedImageEffect::getMaskInput() const
{
    return stageMaskInput(*this);
}

void
StagedImageEffect::addMaskMixKnobs(const KnobPagePtr& page)
{
    KnobBoolPtr maskInvert = createKnob<KnobBool>(std::string("Invert Mask"));

    maskInvert->setName(kOfxMaskInvertParamName);
    maskInvert->setHintToolTip(tr("When checked, the effect is fully applied where the mask is 0."));
    maskInvert->setDefaultValue(false);
    if (page) {
        page->addKnob(maskInvert);
    }
    _stageKnobs.maskInvert = maskInvert;

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
    _stageKnobs.mix = mix;
}

KnobPagePtr
StagedImageEffect::createControlsPage()
{
    return createKnob<KnobPage>(tr("Controls"));
}

void
StagedImageEffect::addStageKnobs(const KnobPagePtr& page)
{
    if (imagePipelineSpec().stages.has(Stage::MaskMix)) {
        addMaskMixKnobs(page);
    }
}

double
StagedImageEffect::getMixValue(double time,
                               ViewIdx view) const
{
    const KnobReader reader(time, view);

    return reader.valueOr(_stageKnobs.mix, 1.);
}

bool
StagedImageEffect::getMaskInvertValue(double time,
                                      ViewIdx view) const
{
    const KnobReader reader(time, view);

    return reader.valueOr(_stageKnobs.maskInvert, false);
}

bool
StagedImageEffect::isMaskApplied() const
{
    return isStageMaskApplied(*this);
}

PassContext
StagedImageEffect::getPassContext(const RenderScale& scale) const
{
    PassContext context;
    const int source = getSourceInput();
    const double par = ((source >= 0) && getInput(source)) ? getAspectRatio(source) : 0.;

    context.scale = scale;
    if (par != 0.) {
        context.pixelAspectRatio = par;
    }

    return context;
}

StatusEnum
StagedImageEffect::getRegionOfDefinition(U64 hash,
                                         double time,
                                         const RenderScale& scale,
                                         ViewIdx view,
                                         RectD* rod)
{
    if (imagePipelineSpec().op == OpKind::Route) {
        const Route answer = route(time, view);
        EffectInstancePtr routed = (answer.kind == Route::Kind::Input) ? getInput(answer.inputNb) : EffectInstancePtr();

        if (answer.kind == Route::Kind::None) {
            return NativeEffectBase::getRegionOfDefinition(hash, time, scale, view, rod);
        }
        if (!routed) {
            *rod = RectD();

            return eStatusOK;
        }
        const RenderScale inputScale = routed->supportsRenderScale() ? scale : RenderScale::identity;
        bool isProjectFormat = false;
        const StatusEnum status = routed->getRegionOfDefinition_public(routed->getRenderHash(), answer.time, inputScale, view, rod, &isProjectFormat);
        // An input reports its empty region as a failure; it is still the region it routes to.
        if ((status == eStatusFailed) && rod->isNull()) {
            *rod = RectD();

            return eStatusOK;
        }

        return status;
    }

    const int source = getSourceInput();
    EffectInstancePtr input = (source >= 0) ? getInput(source) : EffectInstancePtr();

    if ((imagePipelineSpec().op == OpKind::Resample) && input) {
        return resampleRegionOfDefinition(*this, source, opResample(time, view, scale), getResampleRegionStages(time, view), time, scale, view, rod);
    }
    if ((imagePipelineSpec().op != OpKind::Passes) || !input) {
        return NativeEffectBase::getRegionOfDefinition(hash, time, scale, view, rod);
    }
    const RenderScale inputScale = input->supportsRenderScale() ? scale : RenderScale::identity;
    RectD srcRoD;
    bool isProjectFormat = false;
    if (input->getRegionOfDefinition_public(input->getRenderHash(), time, inputScale, view, &srcRoD, &isProjectFormat) == eStatusFailed) {
        return eStatusFailed;
    }
    const RectI srcRoDPixel = PassExec::toPixelEnclosing(srcRoD, scale, getAspectRatio(source));

    RectI rodPixel = srcRoDPixel;
    bool changed = false;
    if (!PassExec::isEmptyRect(srcRoDPixel)) {
        // An identity op keeps the default region, whatever clipRegionOfDefinition() would do.
        if (isOpIdentity(time, scale, RectI(), view)) {
            return NativeEffectBase::getRegionOfDefinition(hash, time, scale, view, rod);
        }
        rodPixel = passListRoD(srcRoDPixel, opPasses(time, view, scale));
        changed = true;
    }
    if (clipRegionOfDefinition(time, scale, view, input, srcRoDPixel, &rodPixel)) {
        changed = true;
    }
    if (!changed) {
        return NativeEffectBase::getRegionOfDefinition(hash, time, scale, view, rod);
    }
    *rod = PassExec::toCanonical(rodPixel, scale, getAspectRatio(-1));

    return eStatusOK;
} // StagedImageEffect::getRegionOfDefinition

void
StagedImageEffect::getRegionsOfInterest(double time,
                                        const RenderScale& scale,
                                        const RectD& outputRoD,
                                        const RectD& renderWindow,
                                        ViewIdx view,
                                        RoIMap* ret)
{
    NativeEffectBase::getRegionsOfInterest(time, scale, outputRoD, renderWindow, view, ret);

    const int source = getSourceInput();
    EffectInstancePtr input = (source >= 0) ? getInput(source) : EffectInstancePtr();
    if ((imagePipelineSpec().op == OpKind::Resample) && input) {
        resampleRegionsOfInterest(*this, source, opResample(time, view, scale), getResampleRegionStages(time, view), time, scale, renderWindow, view, ret);

        return;
    }
    if ((imagePipelineSpec().op != OpKind::Passes) || !input) {
        return;
    }
    const bool doMasking = isMaskApplied();
    double mix = 1.;
    if (doMasking) {
        mix = getMixValue(time, view);
        if (mix == 0.) {
            return;
        }
    }

    const double par = getAspectRatio(source);
    const RectI rectPixel = PassExec::toPixelEnclosing(renderWindow, scale, par);
    RectD srcRoI = PassExec::toCanonical(passListSourceRoI(rectPixel, opPasses(time, view, scale)), scale, par);
    if (doMasking && (mix != 1.)) {
        srcRoI.merge(renderWindow);
    }
    // The mask may be the same effect as the source, in which case both needs share one entry.
    RoIMap::iterator found = ret->find(input);
    if (found != ret->end()) {
        if (doMasking && (getInput(getMaskInput()) == input)) {
            srcRoI.merge(found->second);
        }
        found->second = srcRoI;
    } else {
        ret->insert(std::make_pair(input, srcRoI));
    }
} // StagedImageEffect::getRegionsOfInterest

ResampleRegionStages
StagedImageEffect::getResampleRegionStages(double time,
                                           ViewIdx view) const
{
    ResampleRegionStages stages;

    stages.maskMix = imagePipelineSpec().stages.has(Stage::MaskMix);
    if (stages.maskMix) {
        stages.doMasking = isMaskApplied();
        stages.mix = getMixValue(time, view);
    }
    stages.maskInput = getMaskInput();

    return stages;
}

bool
StagedImageEffect::getInputsHoldingTransform(std::list<int>* inputs) const
{
    const int source = getSourceInput();

    if (!inputs || !opHasMatrix() || (source < 0)) {
        return false;
    }
    inputs->push_back(source);

    return true;
}

bool
StagedImageEffect::getCanTransform() const
{
    // Mask and mix are no matrix, so a node with them only applies the transforms upstream.
    return opHasMatrix() && !imagePipelineSpec().stages.has(Stage::MaskMix) && opCanConcatenate();
}

StatusEnum
StagedImageEffect::getTransform(double time,
                                const RenderScale& renderScale,
                                bool draftRender,
                                ViewIdx view,
                                EffectInstancePtr* inputToTransform,
                                Transform::Matrix3x3* transform)
{
    if (imagePipelineSpec().op != OpKind::Resample) {
        return NativeEffectBase::getTransform(time, renderScale, draftRender, view, inputToTransform, transform);
    }

    return resampleTransform(*this, getSourceInput(), opResample(time, view, renderScale), time, renderScale, draftRender, inputToTransform, transform);
}

FramesNeededMap
StagedImageEffect::getFramesNeeded(double time,
                                   ViewIdx view)
{
    if (imagePipelineSpec().op != OpKind::Route) {
        return NativeEffectBase::getFramesNeeded(time, view);
    }
    const Route answer = route(time, view);
    if (answer.kind == Route::Kind::None) {
        return NativeEffectBase::getFramesNeeded(time, view);
    }
    FramesNeededMap ret;
    if ((answer.kind == Route::Kind::Input) && getInput(answer.inputNb)) {
        RangeD range;
        range.min = range.max = answer.time;
        FrameRangesMap viewRanges;
        viewRanges.insert(std::make_pair(view, std::vector<RangeD>(1, range)));
        ret.insert(std::make_pair(answer.inputNb, viewRanges));
    }

    return ret;
}

ImageMetadata
StagedImageEffect::deriveOutputMetadata(double time,
                                        ViewIdx view)
{
    if (imagePipelineSpec().op != OpKind::Route) {
        return NativeEffectBase::deriveOutputMetadata(time, view);
    }
    const Route answer = route(time, view);
    switch (answer.kind) {
    case Route::Kind::Input:

        return getInputMetadata(answer.inputNb, answer.time, view);
    case Route::Kind::Black:

        return ImageMetadata();
    case Route::Kind::None:
        break;
    }

    return NativeEffectBase::deriveOutputMetadata(time, view);
}

bool
StagedImageEffect::isIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view,
                              double* inputTime,
                              ViewIdx* inputView,
                              int* inputNb)
{
    if (NativeEffectBase::isIdentity(time, scale, roi, view, inputTime, inputView, inputNb)) {
        return true;
    }

    *inputTime = time;
    *inputView = view;
    *inputNb = getBlendInput(time, view);

    const bool maskMix = imagePipelineSpec().stages.has(Stage::MaskMix);
    if (maskMix && (getMixValue(time, view) == 0.)) {
        return true;
    }
    if (isOpIdentity(time, scale, roi, view)) {
        return true;
    }

    // A mask that does not reach the window leaves the source unchanged there; an inverted
    // one applies the effect in full instead.
    if (maskMix && isMaskApplied() && !getMaskInvertValue(time, view) && stageMaskMissesWindow(*this, getMaskInput(), time, scale, roi, view)) {
        return true;
    }

    return false;
} // StagedImageEffect::isIdentity

StatusEnum
StagedImageEffect::render(const RenderActionArgs& args)
{
    if (imagePipelineSpec().op != OpKind::Composite) {
        return renderOp(args, resolveStagePlan(*this, imagePipelineSpec().stages, _stageKnobs, getSourceInput(), args.time, args.view, args.processChannels));
    }
    const CompositeInputs inputs = opInputs(args.time, args.view);
    StagePlan plan = resolveStagePlan(*this, imagePipelineSpec().stages, _stageKnobs, inputs.blend(), args.time, args.view, args.processChannels);
    resolveStageSources(nodeSpec(), inputs, &plan);

    return renderOp(args, plan);
}

NATRON_NAMESPACE_EXIT
