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

#include "Merge.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <QString>

#include <ofxNatron.h>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
// Option indices of the bbox choice.
enum BBoxEnum {
    eBBoxUnion = 0,
    eBBoxIntersection,
    eBBoxA,
    eBBoxB
};

// Option indices of the metadata from and frame range from choices.
enum FromEnum {
    eFromA = 0,
    eFromB
};

// Pixels a step merges at a time, so that the gathered alphas fit on the stack.
const int kStepChunk = 256;

#ifdef NATRON_HAVE_GPU
// The parameter block of StepMerge in merge.slang. Operator indices are mergeops' MERGE_*, the
// order of MergeOps::Operator.
struct NativeMergeGpuParams {
    std::uint32_t op;
    std::uint32_t identityForTransparentA;
};

static_assert(sizeof(NativeMergeGpuParams) == 8, "must match StepMerge in merge.slang");
static_assert(MergeOps::eOperatorCount == 39, "must match MERGE_OPERATOR_COUNT in mergeops.slang");
#endif

// The alphas of pixels x0..x0+width-1 of a row, nComps per pixel in the row's layout, zero for a
// row without any.
void
gatherAlpha(const RowIO::Alpha& alpha,
            int x0,
            int width,
            int nComps,
            float* out)
{
    if (!alpha.data) {
        std::fill(out, out + std::size_t(width) * nComps, 0.f);

        return;
    }
    for (int i = 0; i < width; ++i) {
        for (int c = 0; c < nComps; ++c) {
            out[i * nComps + c] = alpha.at(x0 + i, c);
        }
    }
}

bool
allZero(const float* values,
        std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i) {
        if (values[i] != 0.f) {
            return false;
        }
    }

    return true;
}

// The bounding box of a and b, where an empty rectangle adds nothing.
RectD
boundingBox(const RectD& a,
            const RectD& b)
{
    if (a.isNull()) {
        return b;
    }
    if (b.isNull()) {
        return a;
    }
    RectD box = a;
    box.merge(b);

    return box;
}

// The layer ID a layer select names, its empty value standing for rgba.
std::string
selectedLayerID(const KnobLayerSelectPtr& knob)
{
    const std::string layerID = knob ? knob->getLayer() : std::string();

    return layerID.empty() ? std::string(kNatronColorViewRGBA) : layerID;
}

KnobChoicePtr
createFromChoice(KnobHolder* holder,
                 const KnobPagePtr& page,
                 const std::string& label,
                 const std::string& name,
                 const std::string& hint)
{
    KnobChoicePtr from = AppManager::createKnob<KnobChoice>(holder, label);

    from->setName(name);
    from->setHintToolTip(hint);
    std::vector<ChoiceOption> options;
    options.push_back(ChoiceOption(kNativeMergeFromA, "A", EffectInstance::tr("Input A.").toStdString()));
    options.push_back(ChoiceOption(kNativeMergeFromB, "B", EffectInstance::tr("Input B.").toStdString()));
    from->populateChoices(options);
    from->setDefaultValue((int)eFromB);
    from->setAnimationEnabled(false);
    from->setIsMetadataSlave(true);
    page->addKnob(from);

    return from;
}

KnobLayerSelectPtr
createLayerSelect(KnobHolder* holder,
                  const KnobPagePtr& page,
                  const std::string& label,
                  const std::string& name,
                  const std::string& hint)
{
    KnobLayerSelectPtr layer = AppManager::createKnob<KnobLayerSelect>(holder, label, 1, false);

    layer->setName(name);
    layer->setHintToolTip(hint);
    layer->setWithChannelButtons(false);
    layer->setAllowNone(false);
    layer->setAnimationEnabled(false);
    layer->setIsMetadataSlave(true);
    layer->setDefaultValue(layer->encode(kNatronColorViewRGBA, std::vector<std::string>()));
    page->addKnob(layer);

    return layer;
}

KnobChannelSelectPtr
createAlphaSelect(KnobHolder* holder,
                  const KnobPagePtr& page,
                  const std::string& label,
                  const std::string& name,
                  const std::string& hint)
{
    KnobChannelSelectPtr alpha = AppManager::createKnob<KnobChannelSelect>(holder, label, 1, false);

    alpha->setName(name);
    alpha->setHintToolTip(hint);
    alpha->setAnimationEnabled(false);
    alpha->setDefaultValue(alpha->encode(std::string(kNatronColorViewRGBA) + ".A"));
    page->addKnob(alpha);

    return alpha;
}

KnobChoicePtr
createAlphaSourceChoice(KnobHolder* holder,
                        const KnobPagePtr& page,
                        const std::string& label,
                        const std::string& name,
                        const std::string& hint)
{
    KnobChoicePtr source = AppManager::createKnob<KnobChoice>(holder, label);

    source->setName(name);
    source->setHintToolTip(hint);
    std::vector<ChoiceOption> options;
    options.push_back(ChoiceOption(kNativeMergeAlphaUniversal, EffectInstance::tr("Universal").toStdString(),
                                   EffectInstance::tr("One channel is the alpha of every merged channel.").toStdString()));
    options.push_back(ChoiceOption(kNativeMergeAlphaPerLayer, EffectInstance::tr("Per-layer").toStdString(),
                                   EffectInstance::tr("The merged layer's own channel named A or alpha, in any case, is its alpha; "
                                                      "a layer without one takes the universal channel.")
                                       .toStdString()));
    options.push_back(ChoiceOption(kNativeMergeAlphaPerChannel, EffectInstance::tr("Per-channel").toStdString(),
                                   EffectInstance::tr("An alpha layer's channel i is the alpha of merged channel i. It must have as "
                                                      "many channels as the merged layer.")
                                       .toStdString()));
    source->populateChoices(options);
    source->setDefaultValue((int)NativeMergeOp::AlphaSource::Universal);
    source->setAnimationEnabled(false);
    source->setIsMetadataSlave(true);
    page->addKnob(source);

    return source;
}

KnobLayerSelectPtr
createAlphaLayerSelect(KnobHolder* holder,
                       const KnobPagePtr& page,
                       const std::string& label,
                       const std::string& name,
                       const std::string& hint)
{
    KnobLayerSelectPtr layer = AppManager::createKnob<KnobLayerSelect>(holder, label, 1, false);

    layer->setName(name);
    layer->setHintToolTip(hint);
    layer->setWithChannelButtons(false);
    layer->setAllowNone(true);
    layer->setAnimationEnabled(false);
    layer->setIsMetadataSlave(true);
    page->addKnob(layer);

    return layer;
}

NativeMergeOp::AlphaSource
alphaSourceValue(const KnobReader& reader,
                 const KnobChoiceWPtr& knob)
{
    const int value = reader.value(knob);

    return ((value >= 0) && (value <= (int)NativeMergeOp::AlphaSource::PerChannel)) ? (NativeMergeOp::AlphaSource)value : NativeMergeOp::AlphaSource::Universal;
}

// The framework's alpha source for one side, from its universal channel and alpha layer knobs.
CompositeAlpha
compositeAlpha(NativeMergeOp::AlphaSource source,
               std::string_view channelKnob,
               std::string_view layerKnob)
{
    switch (source) {
    case NativeMergeOp::AlphaSource::PerLayer:

        return CompositeAlpha::readLayer(channelKnob);
    case NativeMergeOp::AlphaSource::PerChannel:

        return CompositeAlpha::layer(layerKnob);
    case NativeMergeOp::AlphaSource::Universal:
        break;
    }

    return CompositeAlpha::channel(channelKnob);
}

// Whether layers holds a layer with layer's ID, any colour layer standing for any other.
bool
hasLayerWithID(const std::list<ImageLayerDesc>& layers,
               const ImageLayerDesc& layer)
{
    for (const ImageLayerDesc& other : layers) {
        if ((other.isColorLayer() && layer.isColorLayer()) || (other.getLayerID() == layer.getLayerID())) {
            return true;
        }
    }

    return false;
}

void
appendPlane(const ImageLayerDesc& plane,
            std::list<ImageLayerDesc>* planes)
{
    if ((plane.getNumComponents() > 0) && !hasLayerWithID(*planes, plane)) {
        planes->push_back(plane);
    }
}

// Shows a side's universal channel unless it reads a per-channel alpha layer, and its alpha layer
// only then.
void
refreshSideAlphaKnobs(const KnobChoiceWPtr& source,
                      const KnobChannelSelectWPtr& channel,
                      const KnobLayerSelectWPtr& layer)
{
    KnobChoicePtr sourceKnob = source.lock();
    KnobChannelSelectPtr channelKnob = channel.lock();
    KnobLayerSelectPtr layerKnob = layer.lock();

    if (!sourceKnob || !channelKnob || !layerKnob) {
        return;
    }
    const bool perChannel = (sourceKnob->getValue() == (int)NativeMergeOp::AlphaSource::PerChannel);
    channelKnob->setSecret(perChannel);
    layerKnob->setSecret(!perChannel);
}
} // anonymous namespace

void
NativeMergeOp::createKnobs(KnobHolder* holder,
                           const KnobPagePtr& page)
{
    // Node.cpp shows this knob's value in parentheses next to the node's label, and NodeGui
    // draws the operator icon from it.
    KnobStringPtr subLabelKnob = AppManager::createKnob<KnobString>(holder, std::string(kNatronOfxParamStringSublabelName));
    subLabelKnob->setName(kNatronOfxParamStringSublabelName);
    subLabelKnob->setSecretByDefault(true);
    subLabelKnob->setDefaultAllDimensionsEnabled(false);
    subLabelKnob->setIsPersistent(false);
    subLabelKnob->setEvaluateOnChange(false);
    subLabelKnob->setAnimationEnabled(false);
    subLabelKnob->setDefaultValue(std::string(MergeOps::operatorName(MergeOps::eOver)));
    page->addKnob(subLabelKnob);
    subLabel = subLabelKnob;

    KnobChoicePtr operationKnob = AppManager::createKnob<KnobChoice>(holder, EffectInstance::tr("Operation").toStdString());
    operationKnob->setName(kNativeMergeParamOperation);
    operationKnob->setHintToolTip(EffectInstance::tr("The operator merging each A input over the result so far, starting "
                                                     "from B. A and B are the channel values, a and b their alphas.")
                                      .toStdString());
    {
        std::vector<ChoiceOption> options;
        for (int op = 0; op < MergeOps::eOperatorCount; ++op) {
            const MergeOps::Operator o = (MergeOps::Operator)op;
            options.push_back(ChoiceOption(MergeOps::operatorName(o), MergeOps::operatorName(o), MergeOps::operatorHint(o)));
        }
        operationKnob->populateChoices(options);
    }
    operationKnob->setDefaultValue((int)MergeOps::eOver);
    page->addKnob(operationKnob);
    operation = operationKnob;

    KnobChoicePtr bboxKnob = AppManager::createKnob<KnobChoice>(holder, EffectInstance::tr("Set BBox To").toStdString());
    bboxKnob->setName(kNativeMergeParamBBox);
    bboxKnob->setHintToolTip(EffectInstance::tr("The output's region of definition.").toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kNativeMergeBBoxUnion, EffectInstance::tr("union").toStdString(), EffectInstance::tr("The union of B and every connected A input.").toStdString()));
        options.push_back(ChoiceOption(kNativeMergeBBoxIntersection, EffectInstance::tr("intersection").toStdString(), EffectInstance::tr("The intersection of B and every connected A input.").toStdString()));
        options.push_back(ChoiceOption(kNativeMergeBBoxA, "A", EffectInstance::tr("The union of every connected A input.").toStdString()));
        options.push_back(ChoiceOption(kNativeMergeBBoxB, "B", EffectInstance::tr("Input B's.").toStdString()));
        bboxKnob->populateChoices(options);
    }
    bboxKnob->setDefaultValue((int)eBBoxUnion);
    page->addKnob(bboxKnob);
    bbox = bboxKnob;

    metadataFrom = createFromChoice(holder, page, EffectInstance::tr("Metadata From").toStdString(), kNativeMergeParamMetadataFrom,
                                    EffectInstance::tr("The input whose metadata the output carries: A is input A, not A2 onwards. "
                                                       "When it is not connected, the other one's.")
                                        .toStdString());
    rangeFrom = createFromChoice(holder, page, EffectInstance::tr("Range From").toStdString(), kNativeMergeParamRangeFrom,
                                 EffectInstance::tr("The input whose frame range the output has: A is input A, not A2 onwards. "
                                                    "When it is not connected, the other one's.")
                                     .toStdString());

    aLayer = createLayerSelect(holder, page, EffectInstance::tr("A Layer").toStdString(), kNativeMergeParamALayer,
                               EffectInstance::tr("The layer every A input is read in. A layer an input lacks reads as zero, the input keeping its alpha.").toStdString());
    aAlphaSource = createAlphaSourceChoice(holder, page, EffectInstance::tr("A Alpha Source").toStdString(), kNativeMergeParamAAlphaSource,
                                           EffectInstance::tr("Where each A input's alpha, a, comes from.").toStdString());
    aAlpha = createAlphaSelect(holder, page, EffectInstance::tr("A Alpha").toStdString(), kNativeMergeParamAAlpha,
                               EffectInstance::tr("The channel of each A input that is its alpha, a, for every merged channel, "
                                                  "and for a layer without its own alpha under Per-layer. None gives an alpha of 0.")
                                   .toStdString());
    aAlphaLayer = createAlphaLayerSelect(holder, page, EffectInstance::tr("A Alpha Layer").toStdString(), kNativeMergeParamAAlphaLayer,
                                         EffectInstance::tr("The layer of each A input whose channel i is the alpha of merged channel i. "
                                                            "None, or a layer an input lacks, gives an alpha of 0.")
                                             .toStdString());
    bLayer = createLayerSelect(holder, page, EffectInstance::tr("B Layer").toStdString(), kNativeMergeParamBLayer,
                               EffectInstance::tr("The layer B is read in. Choosing it also sets the output layer.").toStdString());
    bAlphaSource = createAlphaSourceChoice(holder, page, EffectInstance::tr("B Alpha Source").toStdString(), kNativeMergeParamBAlphaSource,
                                           EffectInstance::tr("Where B's alpha, b, comes from.").toStdString());
    bAlpha = createAlphaSelect(holder, page, EffectInstance::tr("B Alpha").toStdString(), kNativeMergeParamBAlpha,
                               EffectInstance::tr("The channel of B that is its alpha, b, for every merged channel, and for a "
                                                  "layer without its own alpha under Per-layer. None gives an alpha of 0.")
                                   .toStdString());
    bAlphaLayer = createAlphaLayerSelect(holder, page, EffectInstance::tr("B Alpha Layer").toStdString(), kNativeMergeParamBAlphaLayer,
                                         EffectInstance::tr("The layer of B whose channel i is the alpha of merged channel i. None, or "
                                                            "a layer B lacks, gives an alpha of 0. The merged alphas are written to it.")
                                             .toStdString());

    KnobChoicePtr layersKnob = AppManager::createKnob<KnobChoice>(holder, EffectInstance::tr("Layers").toStdString());
    layersKnob->setName(kNativeMergeParamLayers);
    layersKnob->setHintToolTip(EffectInstance::tr("What is merged beside the A layer into the output layer. Every other layer "
                                                  "of B passes through.")
                                   .toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kNativeMergeLayersSingle, EffectInstance::tr("Single").toStdString(),
                                       EffectInstance::tr("Only the A layer into the output layer.").toStdString()));
        options.push_back(ChoiceOption(kNativeMergeLayersAllMatching, EffectInstance::tr("All matching").toStdString(),
                                       EffectInstance::tr("Also every layer present on B and on an A input, each read and "
                                                          "written in itself.")
                                           .toStdString()));
        options.push_back(ChoiceOption(kNativeMergeLayersList, EffectInstance::tr("List").toStdString(),
                                       EffectInstance::tr("Also the layers of the Also Merge list, each read and written in "
                                                          "itself. A layer a side lacks reads as zero there.")
                                           .toStdString()));
        layersKnob->populateChoices(options);
    }
    layersKnob->setDefaultValue((int)LayersMode::Single);
    layersKnob->setAnimationEnabled(false);
    layersKnob->setIsMetadataSlave(true);
    page->addKnob(layersKnob);
    layers = layersKnob;

    KnobChannelSetPtr alsoMergeKnob = AppManager::createKnob<KnobChannelSet>(holder, EffectInstance::tr("Also Merge").toStdString());
    alsoMergeKnob->setName(kNativeMergeParamAlsoMerge);
    alsoMergeKnob->setHintToolTip(EffectInstance::tr("The layers of B merged, each in itself, beside the output layer in List "
                                                     "mode.")
                                      .toStdString());
    alsoMergeKnob->setWithChannelButtons(false);
    alsoMergeKnob->setAnimationEnabled(false);
    alsoMergeKnob->setIsMetadataSlave(true);
    {
        std::vector<ChannelSetRow> none(1);
        none[0].mode = ChannelSetRow::eModeNone;
        alsoMergeKnob->setDefaultValue(alsoMergeKnob->encodeRows(none));
    }
    page->addKnob(alsoMergeKnob);
    alsoMerge = alsoMergeKnob;
} // NativeMergeOp::createKnobs

NativeMergeOp::Params
NativeMergeOp::bake(const KnobReader& reader) const
{
    Params params;
    const int value = reader.value(operation);

    params.op = ((value >= 0) && (value < (int)MergeOps::eOperatorCount)) ? (MergeOps::Operator)value : MergeOps::eOver;
    params.identityForTransparentA = isIdentityForTransparentA(params.op);
    params.aAlpha = alphaSourceValue(reader, aAlphaSource);
    params.bAlpha = alphaSourceValue(reader, bAlphaSource);

    return params;
}

std::optional<std::string>
NativeMergeOp::error(const Params& params) const
{
    if (MergeOps::isSeparable(params.op) || ((params.aAlpha != AlphaSource::PerChannel) && (params.bAlpha != AlphaSource::PerChannel))) {
        return std::nullopt;
    }

    return EffectInstance::tr("The %1 operator blends whole colours, so it cannot take a per-channel alpha layer. Choose a "
                              "universal or per-layer alpha source.")
        .arg(QString::fromUtf8(MergeOps::operatorName(params.op)))
        .toStdString();
}

GpuOpRef
NativeMergeOp::gpu(const Params& params) const
{
#ifdef NATRON_HAVE_GPU
    NativeMergeGpuParams block = {};
    block.op = (std::uint32_t)params.op;
    block.identityForTransparentA = params.identityForTransparentA ? 1u : 0u;
    GpuOpRef ref = GpuOpRef::compiled((int)gpu::GpuKernelId::merge, block);
    if (!MergeOps::isSeparable(params.op)) {
        ref.acceptedComponents = GpuOpRef::componentsBit(3) | GpuOpRef::componentsBit(4);
    }

    return ref;
#else
    Q_UNUSED(params);

    return GpuOpRef::none();
#endif
}

bool
NativeMergeOp::isIdentityForTransparentA(MergeOps::Operator op)
{
    // With A = 0 and a = 0 each of these reduces to B for every B and b (see mergeops.slang).
    // conjoint-over and disjoint-over do not where b <= 0 or b > 1, and the non-separable
    // operators are left out rather than relied on through their colour maths.
    switch (op) {
    case MergeOps::eATop:
    case MergeOps::eColorDodge:
    case MergeOps::eExclusion:
    case MergeOps::eFrom:
    case MergeOps::eMatte:
    case MergeOps::eOver:
    case MergeOps::ePlus:
    case MergeOps::eScreen:
    case MergeOps::eStencil:
    case MergeOps::eUnder:
    case MergeOps::eXor:

        return true;
    default:

        return false;
    }
}

bool
NativeMergeOp::isIdentity(const Params& /*params*/) const
{
    // Whether an A input is connected is not in the Params: route() answers that identity.
    return false;
}

Route
NativeMergeOp::route(const Params& params,
                     const RouteContext& context) const
{
    if (!params.identityForTransparentA) {
        return Route::none();
    }
    for (int k = 0; k < kNativeMergeMaxAInputs; ++k) {
        if (context.isConnected(nativeMergeAInput(k))) {
            return Route::none();
        }
    }

    return context.isConnected(kNativeMergeInputB) ? Route::input(kNativeMergeInputB, context.time()) : Route::black();
}

CompositeInputs
NativeMergeOp::inputs(const Params& params,
                      const RouteContext& context) const
{
    CompositeInputs folded;
    const CompositeAlpha aAlphaOf = compositeAlpha(params.aAlpha, kNativeMergeParamAAlpha, kNativeMergeParamAAlphaLayer);

    folded.base = CompositeInput { kNativeMergeInputB, kNativeMergeParamBLayer, compositeAlpha(params.bAlpha, kNativeMergeParamBAlpha, kNativeMergeParamBAlphaLayer) };
    for (int k = 0; k < kNativeMergeMaxAInputs; ++k) {
        const int inputNb = nativeMergeAInput(k);
        if (context.isConnected(inputNb)) {
            folded.add(CompositeInput { inputNb, kNativeMergeParamALayer, aAlphaOf });
        }
    }
    // With no A connected, an operator that is not an identity then (route() answers the others)
    // merges a transparent A: input A, which reads zero.
    if (folded.further.empty()) {
        folded.add(CompositeInput { kNativeMergeInputA, kNativeMergeParamALayer, aAlphaOf });
    }

    return folded;
}

void
NativeMergeOp::step(const Params& params,
                    const RowIO& io) const
{
    const int nComps = io.nComps;
    const MergeOps::RowFunction merge = MergeOps::rowFunction(params.op, nComps);

    if (!merge) {
        return;
    }
    // The result's alpha follows the operator, as a separable operator merges channels; a
    // non-separable one has no meaning on alphas alone, which then merge "over", as its fourth
    // channel does.
    const MergeOps::RowFunction mergeAlpha = io.dstAlpha ? MergeOps::rowFunction(MergeOps::isSeparable(params.op) ? params.op : MergeOps::eOver, nComps) : 0;

    // The alphas in the row's layout, per channel, whatever their source's layout.
    float aAlphas[kStepChunk * 4];
    float bAlphas[kStepChunk * 4];
    for (int x = 0; x < io.width; x += kStepChunk) {
        const int width = std::min(kStepChunk, io.width - x);
        const std::size_t count = std::size_t(width) * nComps;
        const float* A = io.src[1] + std::size_t(x) * nComps;
        const float* B = io.src[0] + std::size_t(x) * nComps;
        float* dst = io.dst + std::size_t(x) * nComps;
        float* dstAlpha = io.dstAlpha ? io.dstAlpha + std::size_t(x) * nComps : 0;
        gatherAlpha(io.alpha[1], x, width, nComps, aAlphas);
        gatherAlpha(io.alpha[0], x, width, nComps, bAlphas);

        // A stretch with nothing in A, such as one outside its region of definition, keeps B
        // without an operator call when a transparent A leaves B unchanged.
        if (params.identityForTransparentA && allZero(A, count) && allZero(aAlphas, count)) {
            std::memcpy(dst, B, count * sizeof(float));
            if (dstAlpha) {
                std::memcpy(dstAlpha, bAlphas, count * sizeof(float));
            }
            continue;
        }
        const MergeOps::AlphaSpan aSpan = { aAlphas, nComps, 1 };
        const MergeOps::AlphaSpan bSpan = { bAlphas, nComps, 1 };
        merge(A, aSpan, B, bSpan, width, dst);
        if (mergeAlpha && dstAlpha) {
            mergeAlpha(aAlphas, aSpan, bAlphas, bSpan, width, dstAlpha);
        }
    }
} // NativeMergeOp::step

NativeMerge::NativeMerge(NodePtr node)
    : ImageNode<NativeMergeOp>(node)
{
}

NativeMerge::~NativeMerge()
{
}

void
NativeMerge::initializeKnobs()
{
    ImageNode<NativeMergeOp>::initializeKnobs();

    NodePtr node = getNode();
    if (node) {
        node->declareLayerKnob(op().aLayer.lock(), kNativeMergeInputA, LayerKnobSpec::eRoleInputBound);
        node->declareLayerKnob(op().aAlpha.lock(), kNativeMergeInputA, LayerKnobSpec::eRoleInputBound);
        node->declareLayerKnob(op().bLayer.lock(), kNativeMergeInputB, LayerKnobSpec::eRoleInputBound);
        node->declareLayerKnob(op().bAlpha.lock(), kNativeMergeInputB, LayerKnobSpec::eRoleInputBound);
        node->declareLayerKnob(op().aAlphaLayer.lock(), kNativeMergeInputA, LayerKnobSpec::eRoleInputBound);
        node->declareLayerKnob(op().bAlphaLayer.lock(), kNativeMergeInputB, LayerKnobSpec::eRoleInputBound);
        node->declareLayerKnob(op().alsoMerge.lock(), kNativeMergeInputB, LayerKnobSpec::eRoleInputBound);
    }
    refreshSubLabel();
    refreshAlphaKnobs();
    refreshLayersKnobs();
}

void
NativeMerge::refreshLayersKnobs()
{
    KnobChoicePtr layers = op().layers.lock();
    KnobChannelSetPtr alsoMerge = op().alsoMerge.lock();

    if (layers && alsoMerge) {
        alsoMerge->setSecret(layers->getValue() != (int)NativeMergeOp::LayersMode::List);
    }
}

void
NativeMerge::refreshAlphaKnobs()
{
    refreshSideAlphaKnobs(op().aAlphaSource, op().aAlpha, op().aAlphaLayer);
    refreshSideAlphaKnobs(op().bAlphaSource, op().bAlpha, op().bAlphaLayer);
}

void
NativeMerge::refreshSubLabel()
{
    KnobChoicePtr operation = op().operation.lock();
    KnobStringPtr subLabel = op().subLabel.lock();

    if (!operation || !subLabel) {
        return;
    }
    const int value = operation->getValue();
    if ((value >= 0) && (value < (int)MergeOps::eOperatorCount)) {
        subLabel->setValue(std::string(MergeOps::operatorName((MergeOps::Operator)value)));
    }
}

bool
NativeMerge::knobChanged(KnobI* k,
                         ValueChangedReasonEnum reason,
                         ViewSpec /*view*/,
                         double /*time*/,
                         bool /*originatedFromMainThread*/)
{
    KnobChoicePtr operation = op().operation.lock();

    if (operation && (k == operation.get())) {
        refreshSubLabel();

        return true;
    }

    KnobChoicePtr aAlphaSource = op().aAlphaSource.lock();
    KnobChoicePtr bAlphaSource = op().bAlphaSource.lock();
    if ((aAlphaSource && (k == aAlphaSource.get())) || (bAlphaSource && (k == bAlphaSource.get()))) {
        refreshAlphaKnobs();

        return true;
    }

    KnobChoicePtr layers = op().layers.lock();
    if (layers && (k == layers.get())) {
        refreshLayersKnobs();

        return true;
    }

    // The output defaults to the B layer: a user's choice of B's layer moves the output with it.
    KnobLayerSelectPtr bLayer = op().bLayer.lock();
    if (bLayer && (k == bLayer.get()) && (reason == eValueChangedReasonUserEdited)) {
        KnobLayerSelectPtr target = getKnobByNameAndType<KnobLayerSelect>(kNodeParamLayerSelect);
        if (target) {
            target->setLayer(selectedLayerID(bLayer));
        }

        return true;
    }

    return false;
}

void
NativeMerge::followContainerLayer(const NodePtr& node,
                                  const std::string& layerID)
{
    NativeMerge* merge = node ? dynamic_cast<NativeMerge*>(node->getEffectInstance().get()) : 0;

    if (!merge) {
        return;
    }
    const std::string readLayer = layerID.empty() ? std::string(kNatronColorViewRGBA) : layerID;
    const NativeMergeOp& mergeOp = merge->op();
    // The container is the one reference to its layer: none of this Merge's layer knobs
    // counts as a separate user of it.
    const KnobIPtr declared[] = {
        mergeOp.aLayer.lock(),
        mergeOp.aAlpha.lock(),
        mergeOp.aAlphaLayer.lock(),
        mergeOp.bLayer.lock(),
        mergeOp.bAlpha.lock(),
        mergeOp.bAlphaLayer.lock(),
        mergeOp.alsoMerge.lock(),
    };
    for (const KnobIPtr& knob : declared) {
        node->setLayerKnobDrivenByContainer(knob);
    }
    for (const KnobLayerSelectWPtr& side : { mergeOp.aLayer, mergeOp.bLayer }) {
        KnobLayerSelectPtr knob = side.lock();
        if (knob && (knob->getLayer() != readLayer)) {
            knob->setLayer(readLayer);
        }
    }
    for (const KnobChoiceWPtr& side : { mergeOp.aAlphaSource, mergeOp.bAlphaSource }) {
        KnobChoicePtr knob = side.lock();
        if (knob && (knob->getValue() != (int)NativeMergeOp::AlphaSource::PerLayer)) {
            knob->setValue((int)NativeMergeOp::AlphaSource::PerLayer);
        }
    }
    for (const KnobChannelSelectWPtr& side : { mergeOp.aAlpha, mergeOp.bAlpha }) {
        KnobChannelSelectPtr knob = side.lock();
        if (knob && !knob->isNone()) {
            knob->setNone();
        }
    }
}

void
NativeMerge::onKnobsLoaded()
{
    refreshSubLabel();
    refreshAlphaKnobs();
    refreshLayersKnobs();
}

bool
NativeMerge::isOutputPlane(const ImageLayerDesc& plane,
                           double time,
                           ViewIdx view) const
{
    NodePtr node = getNode();
    std::vector<ResolvedLayer> selected;

    if (!node || !node->resolveLayerKnob(time, view, &selected) || selected.empty()) {
        // getComponentsNeededDefault() then outputs the metadata colour plane.
        return plane.isColorLayer();
    }
    for (const ResolvedLayer& layer : selected) {
        if ((layer.desc.isColorLayer() && plane.isColorLayer()) || (layer.desc == plane)) {
            return true;
        }
    }

    return false;
}

bool
NativeMerge::bAlphaPlane(double time,
                         ViewIdx view,
                         ImageLayerDesc* layer) const
{
    KnobChoicePtr source = op().bAlphaSource.lock();
    KnobLayerSelectPtr alphaLayer = op().bAlphaLayer.lock();

    if (!source || !alphaLayer || (source->getValue() != (int)NativeMergeOp::AlphaSource::PerChannel)) {
        return false;
    }
    // Listing an input's layers only reads its metadata and layer knobs.
    NativeMerge* self = const_cast<NativeMerge*>(this);
    std::list<ImageLayerDesc> present;
    self->getAvailableLayers(time, view, kNativeMergeInputB, &present);
    ResolvedLayer resolved;
    if (!alphaLayer->resolve(present, &resolved) || (resolved.desc.getNumComponents() == 0)) {
        return false;
    }
    *layer = resolved.desc;

    return true;
}

void
NativeMerge::addProducedPlanes(double time,
                               ViewIdx view,
                               std::list<ImageLayerDesc>* planes)
{
    KnobChoicePtr layersKnob = op().layers.lock();
    const int mode = layersKnob ? layersKnob->getValue() : (int)NativeMergeOp::LayersMode::Single;

    if (mode == (int)NativeMergeOp::LayersMode::AllMatching) {
        std::list<ImageLayerDesc> aLayers;
        for (int k = 0; k < kNativeMergeMaxAInputs; ++k) {
            if (!getInput(aInput(k))) {
                continue;
            }
            std::list<ImageLayerDesc> present;
            getAvailableLayers(time, view, aInput(k), &present);
            for (const ImageLayerDesc& layer : present) {
                appendPlane(layer, &aLayers);
            }
        }
        std::list<ImageLayerDesc> bLayers;
        if (getInput(kNativeMergeInputB)) {
            getAvailableLayers(time, view, kNativeMergeInputB, &bLayers);
        }
        for (const ImageLayerDesc& layer : bLayers) {
            if (hasLayerWithID(aLayers, layer)) {
                appendPlane(layer, planes);
            }
        }
    } else if (mode == (int)NativeMergeOp::LayersMode::List) {
        KnobChannelSetPtr alsoMergeKnob = op().alsoMerge.lock();
        NodePtr node = getNode();
        if (alsoMergeKnob && node) {
            std::list<ImageLayerDesc> listed;
            node->listLayersForKnob(alsoMergeKnob, time, view, &listed);
            for (const ResolvedLayer& layer : alsoMergeKnob->resolve(listed)) {
                appendPlane(layer.desc, planes);
            }
        }
    }

    ImageLayerDesc alphaLayer;
    if (bAlphaPlane(time, view, &alphaLayer)) {
        appendPlane(alphaLayer, planes);
    }
}

void
NativeMerge::adjustInputsForPlane(const ImageLayerDesc& plane,
                                  double time,
                                  ViewIdx view,
                                  CompositeInputs* inputs) const
{
    if ((plane.getNumComponents() == 0) || isOutputPlane(plane, time, view)) {
        return;
    }
    // B's per-channel alpha layer is written the merged alpha: the fold the output plane runs,
    // whose carried alpha takes the place of its pixels, so that each side keeps the alpha it
    // has there.
    ImageLayerDesc alphaLayer;
    if (bAlphaPlane(time, view, &alphaLayer) && (alphaLayer.getLayerID() == plane.getLayerID())) {
        inputs->resultIsAlpha = true;

        return;
    }
    // Any other plane merges in itself on both sides.
    inputs->base.layerKnob = std::string_view();
    for (CompositeInput& input : inputs->further) {
        input.layerKnob = std::string_view();
    }
}

Route
NativeMerge::route(double time,
                   ViewIdx view)
{
    const Route answer = ImageNode<NativeMergeOp>::route(time, view);

    if (answer.kind != Route::Kind::Input) {
        return answer;
    }
    KnobLayerSelectPtr target = getKnobByNameAndType<KnobLayerSelect>(kNodeParamLayerSelect);
    if (selectedLayerID(op().bLayer.lock()) != selectedLayerID(target)) {
        return Route::none();
    }

    return answer;
}

bool
NativeMerge::getInputRoD(int inputNb,
                         double time,
                         const RenderScale& scale,
                         ViewIdx view,
                         RectD* rod) const
{
    EffectInstancePtr input = getInput(inputNb);

    if (!input) {
        return false;
    }
    const RenderScale inputScale = input->supportsRenderScale() ? scale : RenderScale::identity;
    bool isProjectFormat = false;
    if (input->getRegionOfDefinition_public(input->getRenderHash(), time, inputScale, view, rod, &isProjectFormat) == eStatusFailed) {
        // A failed region is how an input with nothing to show answers: an empty one.
        *rod = RectD();
    }

    return true;
}

StatusEnum
NativeMerge::getRegionOfDefinition(U64 /*hash*/,
                                   double time,
                                   const RenderScale& scale,
                                   ViewIdx view,
                                   RectD* rod)
{
    bool found = false;
    const Route answer = route(time, view);

    if ((answer.kind == Route::Kind::Input) || (getMixValue(time, view) == 0.)) {
        found = getInputRoD(kNativeMergeInputB, time, scale, view, rod);
    } else {
        KnobChoicePtr bboxKnob = op().bbox.lock();
        const int bbox = bboxKnob ? bboxKnob->getValueAtTime(time, 0, view) : (int)eBBoxUnion;
        RectD inputRoD;
        if (bbox == eBBoxB) {
            found = getInputRoD(kNativeMergeInputB, time, scale, view, rod);
        } else {
            const bool withB = (bbox != eBBoxA);
            if (withB && getInputRoD(kNativeMergeInputB, time, scale, view, &inputRoD)) {
                *rod = inputRoD;
                found = true;
            }
            for (int k = 0; k < kNativeMergeMaxAInputs; ++k) {
                if (!getInputRoD(aInput(k), time, scale, view, &inputRoD)) {
                    continue;
                }
                if (!found) {
                    *rod = inputRoD;
                } else if (bbox == eBBoxIntersection) {
                    *rod = rod->intersect(inputRoD);
                } else {
                    *rod = boundingBox(*rod, inputRoD);
                }
                found = true;
            }
        }
    }

    if (!found) {
        // OpenFX's default region in the general context when every clip is optional: the
        // project extent, from the origin.
        Format format;
        getApp()->getProject()->getProjectDefaultFormat(&format);
        const RectD canonical = format.toCanonical_noClipping(0, format.getPixelAspectRatio());
        *rod = RectD(0., 0., canonical.right(), canonical.top());
    }

    return eStatusOK;
} // NativeMerge::getRegionOfDefinition

int
NativeMerge::fromInput(const KnobChoiceWPtr& from,
                       double time,
                       ViewIdx view) const
{
    KnobChoicePtr knob = from.lock();
    const int value = knob ? knob->getValueAtTime(time, 0, view) : (int)eFromB;
    const int preferred = (value == eFromA) ? kNativeMergeInputA : kNativeMergeInputB;
    const int other = (value == eFromA) ? kNativeMergeInputB : kNativeMergeInputA;

    if (getInput(preferred)) {
        return preferred;
    }

    return getInput(other) ? other : -1;
}

ImageMetadata
NativeMerge::deriveOutputMetadata(double time,
                                  ViewIdx view)
{
    const int inputNb = fromInput(op().metadataFrom, time, view);

    if (inputNb >= 0) {
        return getInputMetadata(inputNb, time, view);
    }

    return ImageNode<NativeMergeOp>::deriveOutputMetadata(time, view);
}

void
NativeMerge::getFrameRange(double* first,
                           double* last)
{
    const int inputNb = fromInput(op().rangeFrom, getCurrentTime(), ViewIdx(0));
    EffectInstancePtr input = (inputNb >= 0) ? getInput(inputNb) : EffectInstancePtr();

    if (input) {
        input->getFrameRange_public(input->getRenderHash(), first, last);

        return;
    }
    ImageNode<NativeMergeOp>::getFrameRange(first, last);
}

NATRON_NAMESPACE_EXIT
