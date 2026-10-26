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

#include "Shuffle.h"

#include <algorithm>
#include <cstddef>
#include <list>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <ofxNatron.h>

#include "Engine/AppInstance.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Image.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobShuffleMap.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"

NATRON_NAMESPACE_ENTER

namespace {

bool
findLayer(const std::list<ImageLayerDesc>& layers,
          const std::string& layerID,
          ImageLayerDesc* found)
{
    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        if (it->getLayerID() == layerID) {
            if (found) {
                *found = *it;
            }

            return true;
        }
    }

    return false;
}

void
appendUnique(const ImageLayerDesc& layer,
             std::list<ImageLayerDesc>* layers)
{
    if (!findLayer(*layers, layer.getLayerID(), NULL)) {
        layers->push_back(layer);
    }
}

// Mirrors ResolvedLayer::channelBit: a single-channel colour plane or view is alpha, on bit 3.
int
colorBitOfIndex(int nComps,
                int index)
{
    return (nComps == 1) ? 3 : index;
}

void
configureLayerSelect(const KnobLayerSelectPtr& knob,
                     const std::string& name,
                     bool allowNone,
                     bool defaultNone)
{
    knob->setName(name);
    knob->setWithChannelButtons(false);
    knob->setAllowNone(allowNone);
    knob->setAnimationEnabled(false);
    knob->setIsMetadataSlave(true);
    knob->setSecretByDefault(true);
    if (defaultNone) {
        knob->setDefaultValue(knob->encode(std::string(), std::vector<std::string>()));
    }
}
} // namespace

Shuffle::Shuffle(NodePtr node)
    : ChannelRouteEffect(node)
    , _in1()
    , _in2()
    , _out1()
    , _out2()
    , _mapping()
    , _bbox()
    , _subLabel()
{
}

Shuffle::~Shuffle()
{
}

void
Shuffle::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

void
Shuffle::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));
    const bool copy = isCopy();

    KnobLayerSelectPtr in1 = createKnob<KnobLayerSelect>(tr("In 1"));
    configureLayerSelect(in1, kShuffleParamIn1, true, false);
    in1->setHintToolTip(copy
                            ? tr("The layer the first slot reads from input 1.")
                            : tr("The layer the first slot reads. A layer the input no longer has stays selected "
                                 "and is marked \"(not in input)\"."));
    page->addKnob(in1);
    _in1 = in1;

    KnobLayerSelectPtr in2 = createKnob<KnobLayerSelect>(tr("In 2"));
    configureLayerSelect(in2, kShuffleParamIn2, true, !copy);
    in2->setHintToolTip(copy
                            ? tr("The layer the second slot reads from input 2, or None.")
                            : tr("The layer the second slot reads, or None."));
    page->addKnob(in2);
    _in2 = in2;

    KnobLayerSelectPtr out1 = createKnob<KnobLayerSelect>(tr("Out 1"));
    configureLayerSelect(out1, kShuffleParamOut1, false, false);
    out1->setHintToolTip(tr("The first layer this node writes. Choosing \"New layer...\" creates a project layer."));
    page->addKnob(out1);
    _out1 = out1;

    KnobLayerSelectPtr out2 = createKnob<KnobLayerSelect>(tr("Out 2"));
    configureLayerSelect(out2, kShuffleParamOut2, true, true);
    out2->setHintToolTip(tr("The second layer this node writes, or None. The same layer as Out 1, or a colour layer "
                            "sharing a channel with Out 1's (alpha with rgba), counts as None; rgb and alpha merge."));
    page->addKnob(out2);
    _out2 = out2;

    // Built directly rather than through AppManager::createKnob() so the node does not
    // depend on the knob factory knowing this type.
    std::shared_ptr<KnobShuffleMap> mapping(new KnobShuffleMap(this, tr("Mapping").toStdString(), 1, true));
    mapping->populate();
    addKnob(mapping);
    mapping->setName(kShuffleParamMapping);
    mapping->setAnimationEnabled(false);
    mapping->setHintToolTip(tr("The source of every output channel: a slot channel, 0 or 1. An output channel with "
                               "no source reads the same channel of its own slot, or 0 when that slot is None. "
                               "Between rgba, rgb, alpha and xy the same channel means the same colour channel "
                               "(alpha reads rgba's A), and a colour channel the input does not have reads 0. "
                               "Reading any other layer or channel the input does not have is an error: set that "
                               "output channel to 0 or 1 instead."));
    if (copy) {
        std::vector<ShuffleMapRow> rows;
        for (int c = 0; c < 3; ++c) {
            ShuffleMapRow row;
            row.outSlot = 1;
            row.outIndex = c;
            row.src = ShuffleSource::makeInput(2, c);
            rows.push_back(row);
        }
        mapping->setDefaultValue(mapping->encodeRows(rows));
    }
    page->addKnob(mapping);
    _mapping = mapping;

    if (copy) {
        KnobChoicePtr bbox = createKnob<KnobChoice>(tr("BBox"));
        bbox->setName(kShuffleCopyParamBBox);
        bbox->setAnimationEnabled(false);
        std::vector<ChoiceOption> choices;
        choices.push_back(ChoiceOption("union", tr("Union").toStdString(), tr("The union of both inputs' regions.").toStdString()));
        choices.push_back(ChoiceOption("2", "2", tr("Input 2's region, the main input.").toStdString()));
        choices.push_back(ChoiceOption("1", "1", tr("Input 1's region.").toStdString()));
        choices.push_back(ChoiceOption("intersection", tr("Intersection").toStdString(), tr("Where both inputs' regions overlap; empty when they do not.").toStdString()));
        bbox->populateChoices(choices);
        bbox->setDefaultValue((int)eBBoxUnion);
        bbox->setHintToolTip(tr("The region of definition when both inputs are connected. Union covers both "
                                "inputs, 2 takes input 2's (the main input), 1 takes input 1's, and Intersection "
                                "keeps only where they overlap. With one input connected its region is used "
                                "whatever this is set to. The format always comes from input 2."));
        page->addKnob(bbox);
        _bbox = bbox;
    }

    // Follows PrecompNode's kNatronOfxParamStringSublabelName precedent: Node.cpp wraps
    // this knob's value in parentheses and shows it next to the node's label on its own.
    KnobStringPtr sublabel = createKnob<KnobString>(tr("SubLabel"));
    sublabel->setName(kNatronOfxParamStringSublabelName);
    sublabel->setSecretByDefault(true);
    sublabel->setAnimationEnabled(false);
    page->addKnob(sublabel);
    _subLabel = sublabel;

    NodePtr node = getNode();
    if (node) {
        node->declareLayerKnob(in1, getSlotInput(1), LayerKnobSpec::eRoleInputBound);
        node->declareLayerKnob(in2, getSlotInput(2), LayerKnobSpec::eRoleInputBound);
        node->declareLayerKnob(out1, -1, LayerKnobSpec::eRoleTarget);
        node->declareLayerKnob(out2, -1, LayerKnobSpec::eRoleTarget);
    }

    refreshSubLabel();
} // Shuffle::initializeKnobs

int
Shuffle::getSlotInput(int slot) const
{
    return (isCopy() && (slot == 1)) ? (int)eInputCopy1 : (int)eInputMain;
}

std::string
Shuffle::getSlotLayer(int slot) const
{
    KnobLayerSelectPtr knob = (slot == 1) ? _in1.lock() : _in2.lock();

    return knob ? knob->getLayer() : std::string();
}

std::string
Shuffle::getOutputLayer(int slot) const
{
    KnobLayerSelectPtr out1 = _out1.lock();
    const std::string out1Layer = out1 ? out1->getLayer() : std::string();

    if (slot == 1) {
        return out1Layer;
    }

    KnobLayerSelectPtr out2 = _out2.lock();
    const std::string out2Layer = out2 ? out2->getLayer() : std::string();
    if (out2Layer.empty() || out2Layer == out1Layer) {
        return std::string();
    }
    // Both views write the one colour plane, so a shared bit would have two sources.
    if (ImageLayerDesc::isColorViewID(out1Layer) && ImageLayerDesc::isColorViewID(out2Layer) && (ImageLayerDesc::colorViewMask(out1Layer) & ImageLayerDesc::colorViewMask(out2Layer)).any()) {
        return std::string();
    }

    return out2Layer;
}

std::bitset<4>
Shuffle::getOutputColorBits() const
{
    std::bitset<4> bits;

    for (int slot = 1; slot <= 2; ++slot) {
        const std::string layerID = getOutputLayer(slot);
        if (ImageLayerDesc::isColorViewID(layerID)) {
            bits |= ImageLayerDesc::colorViewMask(layerID);
        }
    }

    return bits;
}

void
Shuffle::getColorWriteBits(const ImageLayerDesc& /*storage*/,
                           std::bitset<4>* bits) const
{
    *bits = getOutputColorBits();
}

int
Shuffle::layerChannelCount(const std::string& layerID,
                           int inputNb,
                           double time,
                           ViewIdx view) const
{
    if (layerID.empty()) {
        return 0;
    }
    if (ImageLayerDesc::isColorViewID(layerID)) {
        return ImageLayerDesc::getColorView(layerID).getNumComponents();
    }
    if (ImageLayerDesc::isColorLayer(layerID)) {
        return 4;
    }

    AppInstancePtr app = getApp();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    ImageLayerDesc desc;
    if (project && project->findLayer(layerID, &desc)) {
        return (int)desc.getChannels().size();
    }

    std::list<ImageLayerDesc> present;
    // getPresentLayers() only reads, but is not declared const.
    const_cast<Shuffle*>(this)->getPresentLayers(time, view, inputNb, &present);
    if (findLayer(present, layerID, &desc)) {
        return (int)desc.getChannels().size();
    }

    return -1;
}

ShuffleSource
Shuffle::getEffectiveSource(int outSlot,
                            int outIndex,
                            double /*time*/,
                            ViewIdx /*view*/) const
{
    std::shared_ptr<KnobShuffleMap> mapping = _mapping.lock();

    if (mapping && mapping->hasExplicitSource(outSlot, outIndex)) {
        return mapping->getSource(outSlot, outIndex);
    }

    const std::string slotLayer = getSlotLayer(outSlot);
    if (slotLayer.empty()) {
        return ShuffleSource::makeZero();
    }

    // Colour views alias one plane by bit, so wiring by bit is what lets alpha read rgba's A
    // and xy's X read rgba's R.
    const std::string outLayer = getOutputLayer(outSlot);
    if (ImageLayerDesc::isColorViewID(outLayer) && ImageLayerDesc::isColorViewID(slotLayer)) {
        const int bit = ImageLayerDesc::colorViewChannelBit(outLayer, outIndex);
        const int slotIndex = (bit < 0) ? -1 : ImageLayerDesc::colorViewChannelIndex(slotLayer, bit);

        return (slotIndex < 0) ? ShuffleSource::makeZero() : ShuffleSource::makeInput(outSlot, slotIndex);
    }

    // A channel a non-colour slot's layer lacks stays wired so that checkExtraChannelsPresent()
    // fails the render on it instead of silently writing 0.
    return ShuffleSource::makeInput(outSlot, outIndex);
}

bool
Shuffle::knobChanged(KnobI* k,
                     ValueChangedReasonEnum /*reason*/,
                     ViewSpec /*view*/,
                     double /*time*/,
                     bool /*originatedFromMainThread*/)
{
    KnobLayerSelectPtr in1 = _in1.lock();
    KnobLayerSelectPtr in2 = _in2.lock();
    KnobLayerSelectPtr out1 = _out1.lock();
    KnobLayerSelectPtr out2 = _out2.lock();
    std::shared_ptr<KnobShuffleMap> mapping = _mapping.lock();

    if ((in1 && k == in1.get()) || (in2 && k == in2.get()) || (out1 && k == out1.get()) || (out2 && k == out2.get())) {
        refreshSubLabel();

        return true;
    }
    if (mapping && k == mapping.get()) {
        refreshSubLabel();
        // Only a render knows the time and view to revalidate at, so the edit just retires
        // this node's own missing-channel error and the next render posts it again if the
        // mapping is still unreadable.
        NodePtr node = getNode();
        if (node) {
            node->clearChannelSelectorMessage();
        }
    }

    return false;
}

void
Shuffle::onKnobsLoaded()
{
    refreshSubLabel();
}

bool
Shuffle::slotIsRead(int slot,
                    double time,
                    ViewIdx view) const
{
    for (int outSlot = 1; outSlot <= 2; ++outSlot) {
        const std::string outLayer = getOutputLayer(outSlot);
        if (outLayer.empty()) {
            continue;
        }
        const int nChannels = layerChannelCount(outLayer, (int)eInputMain, time, view);
        for (int c = 0; c < ((nChannels < 0) ? 4 : nChannels); ++c) {
            const ShuffleSource src = getEffectiveSource(outSlot, c, time, view);
            if ((src.kind == ShuffleSource::eInput) && (src.slot == slot)) {
                return true;
            }
        }
    }

    return false;
}

bool
Shuffle::resolveOutputLayerDesc(const std::string& layerID,
                                double time,
                                ViewIdx view,
                                ImageLayerDesc* desc)
{
    if (ImageLayerDesc::isColorViewID(layerID)) {
        *desc = getOutputColorStorage(time, view);

        return true;
    }
    if (ImageLayerDesc::isColorLayer(layerID)) {
        *desc = ImageLayerDesc::getRGBAComponents();

        return true;
    }

    AppInstancePtr app = getApp();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    if (project && project->findLayer(layerID, desc)) {
        return true;
    }

    std::list<ImageLayerDesc> mainLayers;
    getPresentLayers(time, view, (int)eInputMain, &mainLayers);

    return findLayer(mainLayers, layerID, desc);
}

bool
Shuffle::getInputColorStorage(int inputNb,
                              double time,
                              ViewIdx view,
                              ImageLayerDesc* storage)
{
    if (!getInput(inputNb)) {
        return false;
    }

    std::list<ImageLayerDesc> present;
    getPresentLayers(time, view, inputNb, &present);
    for (std::list<ImageLayerDesc>::const_iterator it = present.begin(); it != present.end(); ++it) {
        if (it->isColorLayer() && (it->getNumComponents() > 0)) {
            *storage = ImageLayerDesc::mapNCompsToColorLayer(it->getNumComponents());

            return true;
        }
    }

    return false;
}

ImageLayerDesc
Shuffle::getOutputColorStorage(double time,
                               ViewIdx view)
{
    ImageLayerDesc mainColor;

    if (getInputColorStorage((int)eInputMain, time, view, &mainColor) && (getOutputColorBits() & ~ImageLayerDesc::colorStorageBits(mainColor)).none()) {
        return mainColor;
    }

    return ImageLayerDesc::getRGBAComponents();
}

std::string
Shuffle::resolveLayerLabel(const std::string& layerID,
                           int inputNb,
                           double time,
                           ViewIdx view)
{
    if (layerID.empty()) {
        return std::string();
    }
    if (ImageLayerDesc::isColorViewID(layerID)) {
        return layerID;
    }
    if (layerID == kNatronColorLayerID) {
        return kNatronColorViewRGBA;
    }

    AppInstancePtr app = getApp();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    ImageLayerDesc desc;
    if (project && project->findLayer(layerID, &desc)) {
        return desc.getLayerLabel();
    }

    std::list<ImageLayerDesc> present;
    getPresentLayers(time, view, inputNb, &present);
    if (findLayer(present, layerID, &desc)) {
        return desc.getLayerLabel();
    }

    return layerID;
}

std::string
Shuffle::buildSubLabel()
{
    // Escaped rather than a literal glyph so the meaning survives regardless of this file's editor encoding.
    static const char* const kArrow = " \xE2\x86\x92 ";

    AppInstancePtr app = getApp();
    const double time = app ? app->getTimeLine()->currentFrame() : 0.;
    const ViewIdx view(0);

    std::string result;
    for (int outSlot = 1; outSlot <= 2; ++outSlot) {
        const std::string outLayer = getOutputLayer(outSlot);
        if (outLayer.empty()) {
            continue;
        }
        const std::string outLabel = resolveLayerLabel(outLayer, (int)eInputMain, time, view);

        // Walk every channel this output actually produces and note which input slots feed
        // it, so a slot the mapping never reads (or that is only ever zero/one) is left out.
        bool usesSlot1 = false;
        bool usesSlot2 = false;
        const int nChannels = layerChannelCount(outLayer, (int)eInputMain, time, view);
        for (int c = 0, n = (nChannels < 0) ? 4 : nChannels; c < n; ++c) {
            const ShuffleSource src = getEffectiveSource(outSlot, c, time, view);
            if (src.kind != ShuffleSource::eInput) {
                continue;
            }
            if (src.slot == 1) {
                usesSlot1 = true;
            } else if (src.slot == 2) {
                usesSlot2 = true;
            }
        }

        std::vector<std::string> feedingLabels;
        if (usesSlot1) {
            feedingLabels.push_back(resolveLayerLabel(getSlotLayer(1), getSlotInput(1), time, view));
        }
        if (usesSlot2) {
            const std::string label2 = resolveLayerLabel(getSlotLayer(2), getSlotInput(2), time, view);
            if (feedingLabels.empty() || feedingLabels.front() != label2) {
                feedingLabels.push_back(label2);
            }
        }

        // A single feeding layer with the same name as the output is a plain passthrough:
        // nothing to call out, so this output contributes nothing to the sub-label.
        const bool isIdentity = (feedingLabels.size() == 1) && (feedingLabels.front() == outLabel);
        if (feedingLabels.empty() || isIdentity) {
            continue;
        }

        std::string block;
        for (std::size_t i = 0; i < feedingLabels.size(); ++i) {
            if (i > 0) {
                block += ", ";
            }
            block += feedingLabels[i];
        }
        block += kArrow;
        block += outLabel;

        // A plain "\n" (not "<br>") is correct here: Node::replaceCustomDataInlabel wraps
        // this whole string in one pair of parentheses and NodeGui::refreshNodeText later
        // turns embedded "\n" into "<br />" before handing the result to the HTML label item.
        if (!result.empty()) {
            result += "\n";
        }
        result += block;
    }

    return result;
} // Shuffle::buildSubLabel

void
Shuffle::refreshSubLabel()
{
    KnobStringPtr sublabel = _subLabel.lock();

    if (sublabel) {
        sublabel->setValue(buildSubLabel());
    }
}

void
Shuffle::getComponentsNeededAndProduced(double time,
                                        ViewIdx view,
                                        EffectInstance::ComponentsNeededMap* comps,
                                        double* passThroughTime,
                                        int* passThroughView,
                                        int* passThroughInputNb)
{
    *passThroughTime = time;
    *passThroughView = view;
    *passThroughInputNb = (int)eInputMain;

    std::list<ImageLayerDesc>& produced = (*comps)[-1];
    produced.clear();
    for (int slot = 1; slot <= 2; ++slot) {
        const std::string layerID = getOutputLayer(slot);
        if (layerID.empty()) {
            continue;
        }
        ImageLayerDesc desc;
        if (resolveOutputLayerDesc(layerID, time, view, &desc)) {
            appendUnique(desc, &produced);
        }
    }

    const int nInputs = getNInputs();
    for (int inputNb = 0; inputNb < nInputs; ++inputNb) {
        (*comps)[inputNb].clear();
    }

    for (int slot = 1; slot <= 2; ++slot) {
        const std::string layerID = getSlotLayer(slot);
        if (layerID.empty() || !slotIsRead(slot, time, view)) {
            continue;
        }
        const int inputNb = getSlotInput(slot);
        ImageLayerDesc desc;
        if (ImageLayerDesc::isColorViewID(layerID)) {
            if (getInputColorStorage(inputNb, time, view, &desc)) {
                appendUnique(desc, &(*comps)[inputNb]);
            }
            continue;
        }
        std::list<ImageLayerDesc> present;
        getPresentLayers(time, view, inputNb, &present);
        if (findLayer(present, layerID, &desc)) {
            appendUnique(desc, &(*comps)[inputNb]);
        }
    }

    // The colour bits no output view writes are copied from the main input's colour plane.
    const std::bitset<4> writtenBits = getOutputColorBits();
    if (writtenBits.any() && (ImageLayerDesc::colorStorageBits(getOutputColorStorage(time, view)) & ~writtenBits).any()) {
        ImageLayerDesc mainColor;
        if (getInputColorStorage((int)eInputMain, time, view, &mainColor)) {
            appendUnique(mainColor, &(*comps)[(int)eInputMain]);
        }
    }
} // Shuffle::getComponentsNeededAndProduced

Route
Shuffle::route(double time,
               ViewIdx view)
{
    if (!getOutputLayer(2).empty()) {
        return Route::none();
    }

    const std::string out1Layer = getOutputLayer(1);
    const bool outIsView = ImageLayerDesc::isColorViewID(out1Layer);
    const int nChannels = layerChannelCount(out1Layer, (int)eInputMain, time, view);
    if (nChannels < 0) {
        return Route::none();
    }
    for (int c = 0; c < nChannels; ++c) {
        const ShuffleSource src = getEffectiveSource(1, c, time, view);
        if ((src.kind != ShuffleSource::eInput) || (getSlotInput(src.slot) != (int)eInputMain)) {
            return Route::none();
        }
        const std::string slotLayer = getSlotLayer(src.slot);
        if (outIsView) {
            if (!ImageLayerDesc::isColorViewID(slotLayer) || (ImageLayerDesc::colorViewChannelBit(slotLayer, src.index) != ImageLayerDesc::colorViewChannelBit(out1Layer, c))) {
                return Route::none();
            }
        } else if ((src.index != c) || (slotLayer != out1Layer)) {
            return Route::none();
        }
    }
    // A colour channel the main input lacks is written as 0 into a wider plane, which passing
    // the input through would not do.
    if (outIsView) {
        ImageLayerDesc mainColor;
        if (getInputColorStorage((int)eInputMain, time, view, &mainColor) && (ImageLayerDesc::colorViewMask(out1Layer) & ~ImageLayerDesc::colorStorageBits(mainColor)).any()) {
            return Route::none();
        }
    }
    // renderRoI() validates the mapping only past its identity shortcut, so a channel the input
    // cannot feed (e.g. a layer it does not carry) must keep the render off that shortcut.
    if (!checkExtraChannelsPresent(time, view, NULL)) {
        return Route::none();
    }

    return Route::input((int)eInputMain, time);
}

StatusEnum
Shuffle::getRegionOfDefinition(U64 /*hash*/,
                               double time,
                               const RenderScale& scale,
                               ViewIdx view,
                               RectD* rod)
{
    RectD inputRods[2];
    bool connected[2] = { false, false };
    const int nInputs = std::min(getNInputs(), 2);

    for (int inputNb = 0; inputNb < nInputs; ++inputNb) {
        EffectInstancePtr input = getInput(inputNb);
        if (!input) {
            continue;
        }
        bool isProjectFormat = false;
        StatusEnum st = input->getRegionOfDefinition_public(input->getRenderHash(), time, scale, view, &inputRods[inputNb], &isProjectFormat);
        if (st == eStatusFailed) {
            return st;
        }
        connected[inputNb] = true;
    }

    if (!connected[0] && !connected[1]) {
        return eStatusReplyDefault;
    }
    if (!connected[0] || !connected[1]) {
        *rod = connected[0] ? inputRods[0] : inputRods[1];

        return eStatusOK;
    }

    switch (getBBox()) {
    case eBBoxMain:
        *rod = inputRods[eInputMain];
        break;
    case eBBoxInput1:
        *rod = inputRods[eInputCopy1];
        break;
    case eBBoxIntersection:
        *rod = inputRods[0].intersect(inputRods[1]);
        break;
    case eBBoxUnion:
    default:
        *rod = inputRods[0];
        rod->merge(inputRods[1]);
        break;
    }

    return eStatusOK;
}

Shuffle::BBoxEnum
Shuffle::getBBox() const
{
    KnobChoicePtr bbox = _bbox.lock();
    const int value = bbox ? bbox->getValue() : (int)eBBoxUnion;

    return ((value >= (int)eBBoxUnion) && (value <= (int)eBBoxIntersection)) ? (BBoxEnum)value : eBBoxUnion;
}

void
Shuffle::getFrameRange(double* first,
                       double* last)
{
    const int nInputs = getNInputs();

    for (int inputNb = 0; inputNb < nInputs; ++inputNb) {
        EffectInstancePtr input = getInput(inputNb);
        if (input) {
            input->getFrameRange_public(input->getRenderHash(), first, last);

            return;
        }
    }
    NativeEffectBase::getFrameRange(first, last);
}

namespace {

std::string
planeChannelName(const ImageLayerDesc& layer,
                 int index)
{
    const std::vector<std::string>& channels = layer.getChannels();

    return ((index >= 0) && (index < (int)channels.size())) ? channels[index] : std::string();
}
} // namespace

bool
Shuffle::channelRoute(double time,
                      ViewIdx view,
                      const ImageLayerDesc& plane,
                      ChannelRoute* route)
{
    *route = ChannelRoute();

    const std::string outputLayers[2] = { getOutputLayer(1), getOutputLayer(2) };
    std::map<int, std::list<ImageLayerDesc>> presentByInput;

    // The plane of layerID that inputNb carries at (time, view), as the CPU fetches it.
    const auto findInputPlane = [&](int inputNb,
                                    const std::string& layerID,
                                    ImageLayerDesc* desc) {
        if (!getInput(inputNb)) {
            return false;
        }
        std::map<int, std::list<ImageLayerDesc>>::iterator present = presentByInput.find(inputNb);
        if (present == presentByInput.end()) {
            present = presentByInput.insert(std::make_pair(inputNb, std::list<ImageLayerDesc>())).first;
            getPresentLayers(time, view, inputNb, &present->second);
        }

        return findLayer(present->second, layerID, desc);
    };

    const auto sourceIndex = [&](int inputNb,
                                 const ImageLayerDesc& layer) {
        for (std::size_t i = 0; i < route->sources.size(); ++i) {
            const ChannelRouteSource& source = route->sources[i];
            if ((source.inputNb == inputNb) && (source.layer == layer) && (source.layer.getChannels() == layer.getChannels())) {
                return (int)i;
            }
        }
        route->sources.push_back(ChannelRouteSource(inputNb, layer, ChannelRouteSource::eNonFloatReadsZero));

        return (int)route->sources.size() - 1;
    };

    // Colour views alias the one colour plane, which is fetched whatever view reads it.
    const auto colorComponent = [&](int inputNb,
                                    int bit) {
        ImageLayerDesc desc;
        if ((bit < 0) || !findInputPlane(inputNb, kNatronColorLayerID, &desc)) {
            return ChannelRouteComponent::zero();
        }

        return ChannelRouteComponent::colorBit(sourceIndex(inputNb, desc), bit);
    };

    const auto sourceComponent = [&](const ShuffleSource& src) {
        if (src.kind == ShuffleSource::eOne) {
            return ChannelRouteComponent::one();
        }
        if ((src.kind != ShuffleSource::eInput) || ((src.slot != 1) && (src.slot != 2))) {
            return ChannelRouteComponent::zero();
        }
        const std::string slotLayer = getSlotLayer(src.slot);
        if (slotLayer.empty()) {
            return ChannelRouteComponent::zero();
        }
        const int inputNb = getSlotInput(src.slot);
        if (ImageLayerDesc::isColorViewID(slotLayer)) {
            return colorComponent(inputNb, ImageLayerDesc::colorViewChannelBit(slotLayer, src.index));
        }
        ImageLayerDesc desc;
        if (!findInputPlane(inputNb, slotLayer, &desc)) {
            return ChannelRouteComponent::zero();
        }
        // Looked up by name, so a layer listing a channel name twice reads its first channel of
        // that name.
        const std::string channelName = planeChannelName(desc, src.index);
        const std::vector<std::string>& channels = desc.getChannels();
        const std::vector<std::string>::const_iterator found = channelName.empty() ? channels.end() : std::find(channels.begin(), channels.end(), channelName);
        if (found == channels.end()) {
            return ChannelRouteComponent::zero();
        }

        return ChannelRouteComponent::channel(sourceIndex(inputNb, desc), (int)(found - channels.begin()));
    };

    const int nPlaneComps = plane.getNumComponents();
    route->components.assign((std::size_t)nPlaneComps, ChannelRouteComponent::zero());
    if (plane.isColorLayer()) {
        // The output views share this one plane by bit: a bit a view covers takes that view's
        // source, every other bit is the main input's.
        for (int c = 0; (c < nPlaneComps) && (c < 4); ++c) {
            const int bit = colorBitOfIndex(nPlaneComps, c);
            int outSlot = 0;
            int outIndex = -1;
            for (int slot = 1; slot <= 2; ++slot) {
                if (ImageLayerDesc::isColorViewID(outputLayers[slot - 1])) {
                    outIndex = ImageLayerDesc::colorViewChannelIndex(outputLayers[slot - 1], bit);
                    if (outIndex >= 0) {
                        outSlot = slot;
                        break;
                    }
                }
            }
            route->components[c] = outSlot ? sourceComponent(getEffectiveSource(outSlot, outIndex, time, view)) : colorComponent((int)eInputMain, bit);
        }
    } else {
        int outSlot = 0;
        for (int slot = 1; slot <= 2; ++slot) {
            if (!outputLayers[slot - 1].empty() && (outputLayers[slot - 1] == plane.getLayerID())) {
                outSlot = slot;
                break;
            }
        }
        for (int c = 0; outSlot && (c < nPlaneComps); ++c) {
            route->components[c] = sourceComponent(getEffectiveSource(outSlot, c, time, view));
        }
    }

    // A channel the engine's pre-render check fails on keeps the GPU off, so that the CPU reports
    // it. Region slots are float, so a source the CPU reads as 0 for not being float would reach
    // the GPU converted.
    route->cpuOnly = !checkExtraChannelsPresent(time, view, NULL);
    const NodeMetadataConstPtr metadata = getRenderMetadata(time, view);
    for (const ChannelRouteSource& source : route->sources) {
        EffectInstancePtr input = getInput(source.inputNb);
        if (input && ((metadata->getBitDepth(source.inputNb) != eImageBitDepthFloat) || (input->getRenderMetadata(time, view)->getBitDepth(-1) != eImageBitDepthFloat))) {
            route->cpuOnly = true;
        }
    }

    return true;
} // Shuffle::channelRoute

bool
Shuffle::checkExtraChannelsPresent(double time,
                                   ViewIdx view,
                                   std::string* message)
{
    std::shared_ptr<KnobShuffleMap> mapping = _mapping.lock();
    // Every output channel is checked, so an input's layers are listed once rather than per channel.
    std::map<int, std::list<ImageLayerDesc>> presentByInput;

    for (int outSlot = 1; outSlot <= 2; ++outSlot) {
        const std::string outLayer = getOutputLayer(outSlot);
        ImageLayerDesc outDesc;
        if (outLayer.empty()) {
            continue;
        }
        if (ImageLayerDesc::isColorViewID(outLayer)) {
            outDesc = ImageLayerDesc::getColorView(outLayer);
        } else if (!resolveOutputLayerDesc(outLayer, time, view, &outDesc)) {
            continue; // The render produces no channel of this output, so nothing is read for it.
        }

        for (int c = 0; c < outDesc.getNumComponents(); ++c) {
            const ShuffleSource src = getEffectiveSource(outSlot, c, time, view);
            if (src.kind != ShuffleSource::eInput) {
                continue;
            }
            const std::string layerID = getSlotLayer(src.slot);
            if (layerID.empty()) {
                continue; // A None slot is silent: an explicit row to it renders 0.
            }
            if (ImageLayerDesc::isColorViewID(layerID)) {
                continue; // A colour channel the input lacks reads 0.
            }
            const int inputNb = getSlotInput(src.slot);
            if (!getInput(inputNb)) {
                continue; // A disconnected input is silent: the channel renders 0.
            }

            std::map<int, std::list<ImageLayerDesc>>::iterator present = presentByInput.find(inputNb);
            if (present == presentByInput.end()) {
                present = presentByInput.insert(std::make_pair(inputNb, std::list<ImageLayerDesc>())).first;
                getPresentLayers(time, view, inputNb, &present->second);
            }
            ImageLayerDesc desc;
            if (findLayer(present->second, layerID, &desc)) {
                // Resolved by name as render() does.
                const std::string wanted = planeChannelName(desc, src.index);
                const std::vector<std::string>& channels = desc.getChannels();
                if (!wanted.empty() && (std::find(channels.begin(), channels.end(), wanted) != channels.end())) {
                    continue;
                }
            }

            if (message) {
                std::string channelName;
                ImageLayerDesc named;
                if (resolveOutputLayerDesc(layerID, time, view, &named)) {
                    channelName = planeChannelName(named, src.index);
                }
                // An implicit source reads the output channel's own index, so when the slot's
                // layer is unknown or narrower, that channel's name is the one the user expects.
                const bool isImplicit = !mapping || !mapping->hasExplicitSource(outSlot, c);
                if (channelName.empty() && isImplicit) {
                    channelName = planeChannelName(outDesc, c);
                }
                *message = "Channel " + layerID
                    + (channelName.empty() ? std::string() : ("." + channelName))
                    + " is not in the " + getInputLabel(inputNb) + " input";
            }

            return false;
        }
    }

    return true;
} // Shuffle::checkExtraChannelsPresent

ShuffleCopy::ShuffleCopy(NodePtr node)
    : Shuffle(node)
{
}

ShuffleCopy::~ShuffleCopy()
{
}

NATRON_NAMESPACE_EXIT
