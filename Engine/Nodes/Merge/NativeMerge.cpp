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

#include "NativeMerge.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include <ofxNatron.h>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"

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
                               EffectInstance::tr("The layer every A input is read in. A layer an input lacks reads as zero.").toStdString());
    aAlpha = createAlphaSelect(holder, page, EffectInstance::tr("A Alpha").toStdString(), kNativeMergeParamAAlpha,
                               EffectInstance::tr("The channel of each A input that is its alpha, a, for every merged channel. "
                                                  "None gives an alpha of 0.")
                                   .toStdString());
    bLayer = createLayerSelect(holder, page, EffectInstance::tr("B Layer").toStdString(), kNativeMergeParamBLayer,
                               EffectInstance::tr("The layer B is read in. Choosing it also sets the output layer.").toStdString());
    bAlpha = createAlphaSelect(holder, page, EffectInstance::tr("B Alpha").toStdString(), kNativeMergeParamBAlpha,
                               EffectInstance::tr("The channel of B that is its alpha, b, for every merged channel. None gives "
                                                  "an alpha of 0.")
                                   .toStdString());
} // NativeMergeOp::createKnobs

NativeMergeOp::Params
NativeMergeOp::bake(const KnobReader& reader) const
{
    Params params;
    const int value = reader.value(operation);

    params.op = ((value >= 0) && (value < (int)MergeOps::eOperatorCount)) ? (MergeOps::Operator)value : MergeOps::eOver;
    params.identityForTransparentA = isIdentityForTransparentA(params.op);

    return params;
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
NativeMergeOp::inputs(const Params& /*params*/,
                      const RouteContext& context) const
{
    CompositeInputs folded;

    folded.base = CompositeInput { kNativeMergeInputB, kNativeMergeParamBLayer, CompositeAlpha::channel(kNativeMergeParamBAlpha) };
    for (int k = 0; k < kNativeMergeMaxAInputs; ++k) {
        const int inputNb = nativeMergeAInput(k);
        if (context.isConnected(inputNb)) {
            folded.add(CompositeInput { inputNb, kNativeMergeParamALayer, CompositeAlpha::channel(kNativeMergeParamAAlpha) });
        }
    }
    // With no A connected, an operator that is not an identity then (route() answers the others)
    // merges a transparent A: input A, which reads zero.
    if (folded.further.empty()) {
        folded.add(CompositeInput { kNativeMergeInputA, kNativeMergeParamALayer, CompositeAlpha::channel(kNativeMergeParamAAlpha) });
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
    }
    refreshSubLabel();
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
NativeMerge::onKnobsLoaded()
{
    refreshSubLabel();
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
