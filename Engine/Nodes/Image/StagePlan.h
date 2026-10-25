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

#ifndef Engine_Nodes_Image_StagePlan_h
#define Engine_Nodes_Image_StagePlan_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <bitset>
#include <list>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Engine/EffectInstance.h"
#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/ImagePipelineSpec.h"
#include "Engine/Nodes/Image/Op.h"
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/Nodes/NodeSpec.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The knobs the stages read: mix and maskInvert. A knob the node does not have reads as
 * its default, mix 1 and no inversion.
 **/
struct StageKnobs {
    KnobDoubleWPtr mix;
    KnobBoolWPtr maskInvert;
};

/**
 * @brief One input of a Composite op as a render reads it: inputNb at the render's time, in the
 * layer of layerInput, which is inputNb itself unless its InputLayer pairs it with another input: the plane
 * being rendered as that input resolves it when layerKnob is empty, else the layer the
 * KnobLayerSelect named layerKnob selects among the layers layerInput carries. alpha is its alpha
 * source, read from inputNb.
 **/
struct StageSource {
    int inputNb = -1;
    int layerInput = -1;
    std::string_view layerKnob = {};
    CompositeAlpha alpha = {};
};

/**
 * @brief The layers a StageSource reads at one time and view. read is false when the source's pixels
 * read zero: its input is disconnected or its layer resolves to none. Its alpha resolves apart from
 * its pixels, so an input lacking the layer it reads still has the alpha its source names; only a
 * disconnected input has neither. mapped is whether layer is read in
 * the input's clip layout (the plane), rather than as it is stored (a selection). alphaChannel is the
 * channel of alphaLayer that is the alpha of every channel, or -1 with a non-empty alphaLayer when
 * that layer is read per channel; alphaLayer is empty when the source has no alpha, or an alpha that
 * reads zero.
 **/
struct StageSourceLayers {
    bool read = false;
    ImageLayerDesc layer;
    bool mapped = true;
    ImageLayerDesc alphaLayer;
    int alphaChannel = -1;

    bool hasAlpha() const
    {
        return alphaLayer.getNumComponents() > 0;
    }

    bool alphaPerChannel() const
    {
        return hasAlpha() && (alphaChannel < 0);
    }
};

/**
 * @brief Everything the stages around an op decide for one render, resolved once on the
 * rendering thread before any pixel is touched.
 *
 * sourceInput is the input the stages blend against and copy unprocessed channels from, and the
 * one a Point, Passes or Resample op reads; -1 for none. applyMask is whether the mask input is
 * connected and enabled; maskChannel is the channel of maskLayer it reads, -1 when it resolves to
 * none, in which case the mask reads zero everywhere. divisorChannel is the channel of
 * divisorLayer "(Un)premult by" divides by, -1 for none. composite holds a Composite op's base
 * input followed by its further inputs in fold order, and is empty for any other op.
 * compositeLayers, parallel to composite, holds the layers each of them reads once
 * resolveStageSourcesLayers() has filled it, for inputs that read a selected layer or have an alpha
 * source; it is empty otherwise. compositeResultIsAlpha is CompositeInputs::resultIsAlpha.
 **/
struct StagePlan {
    double time = 0.;
    ViewIdx view = ViewIdx(0);
    std::bitset<4> processChannels;
    StageSet stages;
    int sourceInput = -1;
    double mix = 1.;
    bool maskInvert = false;
    bool applyMask = false;
    int maskInput = -1;
    ImageLayerDesc maskLayer;
    int maskChannel = -1;
    ImageLayerDesc divisorLayer;
    int divisorChannel = -1;
    std::vector<StageSource> composite;
    std::vector<StageSourceLayers> compositeLayers;
    bool compositeResultIsAlpha = false;
};

/**
 * @brief A non-owning reference to the per-row function an executor calls, cheaper than a
 * std::function and never allocating. The callable must outlive every call made through it.
 **/
class StageRowFn {
public:
    template <typename F>
    explicit StageRowFn(const F& f)
        : _context(&f)
        , _call([](const void* context, const RowIO& io) {
            (*static_cast<const F*>(context))(io);
        })
    {
    }

    void operator()(const RowIO& io) const
    {
        _call(_context, io);
    }

private:
    const void* _context;
    void (*_call)(const void*, const RowIO&);
};

/**
 * @brief The input effect reads as its mask: the first input described as a mask, or -1.
 **/
int stageMaskInput(const EffectInstance& effect) WARN_UNUSED_RETURN;

/**
 * @brief Whether effect's mask input is connected and enabled.
 **/
bool isStageMaskApplied(const EffectInstance& effect) WARN_UNUSED_RETURN;

/**
 * @brief Whether the region of definition of the effect connected to maskInput misses roi at
 * time, scale and view, so that a non-inverted mask leaves the source unchanged over roi.
 * False when the region cannot be computed.
 **/
bool stageMaskMissesWindow(const EffectInstance& effect,
                           int maskInput,
                           double time,
                           const RenderScale& scale,
                           const RectI& roi,
                           ViewIdx view) WARN_UNUSED_RETURN;

/**
 * @brief Resolves the stages of effect for a render at time and view: mix and mask inversion
 * from knobs, the mask input and its channel, and the "(Un)premult by" divisor of sourceInput.
 * Stages left out of stages resolve to their neutral values.
 **/
StagePlan resolveStagePlan(EffectInstance& effect,
                           const StageSet& stages,
                           const StageKnobs& knobs,
                           int sourceInput,
                           double time,
                           ViewIdx view,
                           const std::bitset<4>& processChannels) WARN_UNUSED_RETURN;

/**
 * @brief Fills plan's composite list from a Composite op's inputs, base first: the input and knob
 * each one's InputLayer resolves to. A pairing that never ends in a plane or a selection reads the
 * input's own plane. With a null spec, every input reads its own plane.
 **/
void resolveStageSources(const NodeSpec* spec,
                         const CompositeInputs& inputs,
                         StagePlan* plan);

/**
 * @brief The layers source reads in a render of effect at time and view.
 **/
StageSourceLayers resolveStageSourceLayers(EffectInstance& effect,
                                           const StageSource& source,
                                           double time,
                                           ViewIdx view) WARN_UNUSED_RETURN;

/**
 * @brief Fills plan's compositeLayers with the layers each of its Composite inputs reads in a render
 * of effect at plan's time and view, when one of them reads a selected layer or has an alpha source.
 **/
void resolveStageSourcesLayers(EffectInstance& effect,
                               StagePlan* plan);

/**
 * @brief Whether any of plan's Composite inputs has an alpha source, so that the fold carries the
 * result's alpha.
 **/
bool stageSourcesHaveAlpha(const StagePlan& plan) WARN_UNUSED_RETURN;

/**
 * @brief The render error plan's Composite inputs raise at time and view, or nullopt: a layer read
 * per channel as another layer's alpha with a different number of channels from it.
 **/
std::optional<std::string> stageSourcesError(EffectInstance& effect,
                                             const StagePlan& plan,
                                             double time,
                                             ViewIdx view) WARN_UNUSED_RETURN;

/**
 * @brief Renders args through plan with row, the op's per-row function, on the CPU.
 *
 * Every image is fetched first, then locked on this thread for the whole render; the window
 * is split into row bands run over the global pool within the thread budget the host grants,
 * and row must therefore be safe to call concurrently. Each row of the source is read (zero
 * outside the input), divided by the divisor where the Premult stage has one, and handed to
 * row; the processed channels it wrote are multiplied back by the divisor and blended with the
 * undivided source by mask x mix, and with PassThroughUnprocessed the other channels are
 * copied from the source.
 *
 * With a non-empty plan.composite, row is a Composite op's step: the result starts as the base
 * input's row and row folds each further input's row into it, as CompositeOp describes, and that
 * result takes the place of row's output; the source rows only serve the blend and the
 * pass-through. When an input has an alpha source, row is handed the alphas and writes the
 * result's, as CompositeOp describes; a render stageSourcesError() objects to fails. With
 * plan.compositeResultIsAlpha the result's alpha takes the place of the folded pixels.
 **/
StatusEnum runStagesCpu(EffectInstance& effect,
                        const StagePlan& plan,
                        const EffectInstance::RenderActionArgs& args,
                        const StageRowFn& row) WARN_UNUSED_RETURN;

/**
 * @brief Splits roi into the horizontal row bands runStagesCpu() runs in parallel for nThreads
 * threads: one band when nThreads is 1 or the window is too small to be worth sharing.
 **/
void makeStageRowBands(const RectI& roi,
                       int nThreads,
                       std::vector<RectI>* bands);

/**
 * @brief Whether the CPU executors' finish over an nComps-channel plane leaves the op's output
 * row unchanged: no mask applied, a mix of 1, no divisor, at most 4 channels and every one of
 * them processed. The op may then write straight into the output image.
 **/
bool stageFinishIsCopy(bool applyMask,
                       float mix,
                       bool hasDivisor,
                       int nComps,
                       const std::bitset<4>& channels) WARN_UNUSED_RETURN;

/**
 * @brief resolveStagePlan() for a GPU render of query: the processed channels are the query's,
 * and the MaskMix stage applies a mask exactly when query.hasMask says so. Placement sets
 * hasMask from this same plan, so the two only differ for a caller that hands over a query of
 * its own, which then gets the mask it asked for.
 **/
StagePlan resolveGpuStagePlan(EffectInstance& effect,
                              const StageSet& stages,
                              const StageKnobs& knobs,
                              int sourceInput,
                              const GpuQuery& query) WARN_UNUSED_RETURN;

/**
 * @brief The component of the query's plane that plan's divisor is, -1 when there is none.
 * False when the divisor lies outside that plane, which the GPU executor cannot read.
 **/
bool stageGpuDivisorComponent(const StagePlan& plan,
                              const GpuQuery& query,
                              int* component) WARN_UNUSED_RETURN;

/**
 * @brief Whether the GPU executor can run plan for query, whatever the op: a float plane of 1
 * to 4 channels whose "(Un)premult by" divisor, if any, lies in the plane.
 **/
bool canRunStagesGpu(const StagePlan& plan,
                     const GpuQuery& query) WARN_UNUSED_RETURN;

/**
 * @brief Whether the GPU executor can fold plan's Composite inputs for query: a base input and no
 * divisor. An input that reads a selected layer or has an alpha source needs plan's
 * compositeLayers, no stageSourcesError(), and its layers readable beside the query's plane
 * (stageGpuLayerRead()) beside the layer the input is handed over in (stageGpuSourceInputLayer());
 * a selected base layer must have the plane's channel count, since the fold starts from it. The region engine hands the mask input over in its mask layer alone, so no folded
 * input can be the input the MaskMix stage reads its mask from.
 **/
bool canRunStageSourcesGpu(const StagePlan& plan,
                           const GpuQuery& query) WARN_UNUSED_RETURN;

/**
 * @brief The component of a mask view of viewNComps components that holds maskChannel of
 * maskLayer, or -1 when the view does not hold it. The region engine hands a mask over in
 * maskLayer, but a caller may hand a colour mask over in another colour layout.
 **/
int stageGpuMaskViewComponent(const ImageLayerDesc& maskLayer,
                              int maskChannel,
                              int viewNComps) WARN_UNUSED_RETURN;

/**
 * @brief The layer the GPU executor reads input inputNb of effect in when it renders plane at
 * time and view: plane, except for a mask input, which is read in the layer its mask channel
 * resolves in, so that the channel indexes the mask view's own layout.
 **/
void stageGpuInputLayer(const EffectInstance& effect,
                        int inputNb,
                        const ImageLayerDesc& plane,
                        double time,
                        ViewIdx view,
                        ImageLayerDesc* layer);

/**
 * @brief The layer the GPU executor hands Composite input inputNb over in when plan renders plane:
 * the selected colour layer every further input reading inputNb reads it in, in the layout
 * runStagesCpu() reads it in, when plane is a colour layer of another colour layout; plane
 * otherwise, and always for the base, blend and mask inputs, whose views the fold and the stages
 * read in plane's layout. Needs plan's compositeLayers; plane without them.
 **/
ImageLayerDesc stageGpuSourceInputLayer(const StagePlan& plan,
                                        int inputNb,
                                        const ImageLayerDesc& plane) WARN_UNUSED_RETURN;

/**
 * @brief Where the GPU executor finds a layer a Composite input reads, whole or only its channel
 * `channel` when that is not -1, from an input handed over in plane (stageGpuSourceInputLayer()):
 * in the input's own view (Plane), in a layer handed over beside it (Extra, see
 * getGpuExtraInputLayers()), or nowhere (Unreadable): the request pass plans one colour layout per
 * input, so a colour layer in another colour layout than plane is only read through plane's view,
 * and only one channel of it.
 **/
enum class StageGpuLayerRead {
    Plane,
    Extra,
    Unreadable
};

StageGpuLayerRead stageGpuLayerRead(const ImageLayerDesc& layer,
                                    int channel,
                                    const ImageLayerDesc& plane) WARN_UNUSED_RETURN;

/**
 * @brief Appends to layers the layers beyond plane that the GPU executor reads input inputNb of
 * effect in for plan's Composite inputs at time and view: a selected layer and an alpha layer
 * each, unless the input's own view, in stageGpuSourceInputLayer(), holds it. False when one of them is Unreadable.
 **/
bool stageSourcesGpuLayers(EffectInstance& effect,
                           const StagePlan& plan,
                           int inputNb,
                           const ImageLayerDesc& plane,
                           double time,
                           ViewIdx view,
                           std::vector<ImageLayerDesc>* layers) WARN_UNUSED_RETURN;

/**
 * @brief Appends to layers, the layers the request pass plans for input inputNb of effect, the
 * selected layers and alpha layers plan's Composite inputs read it in at time and view, each
 * unless an equivalent layer is listed already.
 **/
void stageSourcesNeededLayers(EffectInstance& effect,
                              const StagePlan& plan,
                              int inputNb,
                              double time,
                              ViewIdx view,
                              std::list<ImageLayerDesc>* layers);

/**
 * @brief The runtime shaders one node's device cannot run. Each is reported once in the node's
 * persistent message and never built again for that node; the message is withdrawn once a shader
 * is accepted. Safe to call from several threads.
 **/
class StageGpuShaderGate {
public:
    /**
     * @brief Whether the GPU context's device runs shader, building it the first time.
     **/
    bool accept(const GpuRuntimeShader& shader,
                const NodePtr& node) WARN_UNUSED_RETURN;

private:
    std::mutex _mutex;
    std::set<std::string> _rejected;
    std::string _message;
};

/**
 * @brief Whether op names GPU work the device can run: a registry kernel whose parameter block
 * fits the 64 KB the recorder uploads, or a runtime shader gate accepts for node. A build
 * without GPU support accepts nothing.
 **/
bool canRecordGpuOp(const GpuOpRef& op,
                    StageGpuShaderGate& gate,
                    const NodePtr& node) WARN_UNUSED_RETURN;

/**
 * @brief Records a Point op over args.outputRect of args.output through plan, as runStagesCpu()
 * renders it on the CPU. A Kernel op is one dispatch of its pixelwrap entry. A RuntimeShader op
 * runs between the divide and finish passes of recordStagesGpuDivide() and
 * recordStagesGpuFinish(), with its uniforms and textures, as GpuRuntimeShader describes.
 * An input that is not connected reads zero, and a mask that is not handed over reads zero.
 *
 * With a non-empty plan.composite the op is a Composite op, and must be a Kernel whose entry is
 * built with runCompositeStep: it is dispatched once per further input, each dispatch folding one
 * input into the result so far, alternating between two scratch buffers, and recordStagesGpuFinish()
 * then blends the result against plan.sourceInput. With no further input the result is the base.
 * An input reads the layer plan.compositeLayers names for it from args.layerInputs where it is not
 * the plane. When an input has an alpha source (stageSourcesHaveAlpha()) the entry is built with
 * runCompositeAlphaStep instead, and the fold carries the result's alpha as runStagesCpu() does.
 **/
StatusEnum recordStagesGpu(const StagePlan& plan,
                           const GpuRenderArgs& args,
                           const GpuOpRef& op) WARN_UNUSED_RETURN;

/**
 * @brief Records plan's source over scratch.bounds into scratch, divided by the divisor where it is
 * usable, as a spatial filter's input. Every channel is written; the source reads zero outside its
 * view. scratch has the plane's channel count.
 **/
StatusEnum recordStagesGpuDivide(const StagePlan& plan,
                                 const GpuRenderArgs& args,
                                 const GpuImageView& scratch) WARN_UNUSED_RETURN;

/**
 * @brief Writes args.output over args.outputRect from filtered, a result over a rect containing
 * outputRect: processed channels multiplied back by the divisor and blended with the undivided
 * source by mask x mix, unprocessed channels passed through. Recorded work that wrote filtered
 * must be ordered before it with a compute barrier.
 **/
StatusEnum recordStagesGpuFinish(const StagePlan& plan,
                                 const GpuRenderArgs& args,
                                 const GpuImageView& filtered) WARN_UNUSED_RETURN;

/**
 * @brief Records kernel, a pixelwrap entry built with runWrapped, over args.outputRect of
 * args.output through plan, as recordStagesGpu() records a Kernel op, the entry also reading
 * inputs at the bindings after its parameter block, in order. An input without a buffer is bound
 * to a stand-in, so the entry must read none of its pixels.
 **/
StatusEnum recordStagesGpuKernelInputs(const StagePlan& plan,
                                       const GpuRenderArgs& args,
                                       const PixelKernelGpu& kernel,
                                       std::span<const GpuImageView> inputs) WARN_UNUSED_RETURN;

/**
 * @brief args' view of input inputNb of effect cropped to that input's region of definition, as a
 * CPU render fetches it: an image of the input holds no pixel outside its region, while the view
 * may. An empty view when the input is not connected, not handed over or the crop is empty. False
 * when the view is not 1 to 4 floats per pixel or the region cannot be read.
 **/
bool stageGpuInputView(EffectInstance& effect,
                       const GpuRenderArgs& args,
                       int inputNb,
                       GpuImageView* view) WARN_UNUSED_RETURN;

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Image_StagePlan_h
