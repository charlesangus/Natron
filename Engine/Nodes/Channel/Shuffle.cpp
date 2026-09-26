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
#include <list>
#include <map>
#include <string>
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
    : NativeEffectBase(node)
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

NativePluginDescription
Shuffle::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_SHUFFLE;
    desc.label = "Shuffle";
    desc.description = tr("Rearrange channels between layers. Two input slots, each a layer of the Source input, "
                          "feed two output layers; every output channel takes a slot channel, 0 or 1, and by "
                          "default the same channel of its own slot. Every layer that is not an output passes "
                          "through unchanged.")
                           .toStdString();
    desc.grouping = PLUGIN_GROUP_CHANNEL;
    desc.majorVersion = 1;
    desc.minorVersion = 0;
    desc.inputs.push_back(NativeInputDescription("Source", true, eDataKindImage));
    desc.outputKind = eDataKindImage;

    return desc;
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
    out2->setHintToolTip(tr("The second layer this node writes, or None. The same layer as Out 1 counts as None."));
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
                               "Reading a layer or channel the input does not have is an error: set that output "
                               "channel to 0 or 1 instead."));
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

    return out2Layer;
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

    // Only a None slot reads 0. A channel the slot's layer lacks stays wired so that
    // checkExtraChannelsPresent() fails the render on it instead of silently writing 0.
    if (getSlotLayer(outSlot).empty()) {
        return ShuffleSource::makeZero();
    }

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

std::string
Shuffle::resolveLayerLabel(const std::string& layerID,
                           int inputNb,
                           double time,
                           ViewIdx view)
{
    if (layerID.empty()) {
        return std::string();
    }
    if (ImageLayerDesc::isColorLayer(layerID)) {
        return ImageLayerDesc::getRGBAComponents().getLayerLabel();
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
        std::list<ImageLayerDesc> present;
        getPresentLayers(time, view, inputNb, &present);
        ImageLayerDesc desc;
        if (findLayer(present, layerID, &desc)) {
            appendUnique(desc, &(*comps)[inputNb]);
        }
    }
} // Shuffle::getComponentsNeededAndProduced

bool
Shuffle::isIdentity(double time,
                    const RenderScale& /*scale*/,
                    const RectI& /*roi*/,
                    ViewIdx view,
                    double* inputTime,
                    ViewIdx* inputView,
                    int* inputNb)
{
    if (!getOutputLayer(2).empty()) {
        return false;
    }

    const std::string out1Layer = getOutputLayer(1);
    const int nChannels = layerChannelCount(out1Layer, (int)eInputMain, time, view);
    if (nChannels < 0) {
        return false;
    }
    for (int c = 0; c < nChannels; ++c) {
        const ShuffleSource src = getEffectiveSource(1, c, time, view);
        if ((src.kind != ShuffleSource::eInput) || (src.index != c)) {
            return false;
        }
        if ((getSlotInput(src.slot) != (int)eInputMain) || (getSlotLayer(src.slot) != out1Layer)) {
            return false;
        }
    }
    // renderRoI() validates the mapping only past its identity shortcut, so a channel the input
    // cannot feed (e.g. A of an RGB-only Color) must keep the render off that shortcut.
    if (!checkExtraChannelsPresent(time, view, NULL)) {
        return false;
    }

    *inputNb = (int)eInputMain;
    *inputTime = time;
    *inputView = view;

    return true;
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

struct ChannelFill {
    ImagePtr image;
    int channel;
    float constant;

    ChannelFill()
        : image()
        , channel(0)
        , constant(0.f)
    {
    }
};

std::string
planeChannelName(const ImageLayerDesc& layer,
                 int index)
{
    // Color indexes name R, G, B, A whatever the input's own Color layout, so a wired A reads an
    // alpha-only Color plane and finds nothing in an RGB one.
    static const char* const colorChannels[4] = { "R", "G", "B", "A" };

    if (layer.isColorLayer()) {
        return ((index >= 0) && (index < 4)) ? std::string(colorChannels[index]) : std::string();
    }
    const std::vector<std::string>& channels = layer.getChannels();

    return ((index >= 0) && (index < (int)channels.size())) ? channels[index] : std::string();
}

bool
findChannelInPlane(const ImagePtr& image,
                   const std::string& channelName,
                   ChannelFill* fill)
{
    if (!image || channelName.empty()) {
        return false;
    }
    const std::vector<std::string>& channels = image->getComponents().getChannels();
    const int nComps = (int)image->getComponentsCount();
    for (int i = 0; (i < (int)channels.size()) && (i < nComps); ++i) {
        if (channels[i] == channelName) {
            fill->image = image;
            fill->channel = i;

            return true;
        }
    }

    return false;
}
} // namespace

ImagePtr
Shuffle::fetchInputPlane(const RenderActionArgs& args,
                         int inputNb,
                         const std::string& layerID,
                         std::vector<FetchedPlane>* fetched)
{
    for (std::vector<FetchedPlane>::const_iterator it = fetched->begin(); it != fetched->end(); ++it) {
        if ((it->inputNb == inputNb) && (it->layerID == layerID)) {
            return it->image;
        }
    }

    FetchedPlane plane;
    plane.inputNb = inputNb;
    plane.layerID = layerID;
    if (getInput(inputNb)) {
        std::list<ImageLayerDesc> present;
        getPresentLayers(args.time, args.view, inputNb, &present);
        ImageLayerDesc desc;
        if (findLayer(present, layerID, &desc)) {
            RectI inputRoI;
            plane.image = getImage(inputNb, args.time, args.mappedScale, args.view, NULL, &desc, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &inputRoI);
            if (plane.image && (plane.image->getBitDepth() != eImageBitDepthFloat)) {
                plane.image.reset();
            }
        }
    }
    fetched->push_back(plane);

    return plane.image;
}

StatusEnum
Shuffle::render(const RenderActionArgs& args)
{
    const std::string outputLayers[2] = { getOutputLayer(1), getOutputLayer(2) };
    std::vector<FetchedPlane> fetched;

    // Every input plane is fetched before any image is locked: fetching renders upstream, which
    // may write into a cached image this render would otherwise already hold a read lock on.
    std::vector<std::vector<ChannelFill>> fills;
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        const ImageLayerDesc& plane = it->first;
        int outSlot = 0;
        for (int slot = 1; slot <= 2; ++slot) {
            if (!outputLayers[slot - 1].empty() && (outputLayers[slot - 1] == plane.getLayerID())) {
                outSlot = slot;
                break;
            }
        }

        std::vector<ChannelFill> planeFills((std::size_t)plane.getNumComponents());
        for (int c = 0; outSlot && (c < plane.getNumComponents()); ++c) {
            const ShuffleSource src = getEffectiveSource(outSlot, c, args.time, args.view);
            ChannelFill& fill = planeFills[c];
            if (src.kind == ShuffleSource::eOne) {
                fill.constant = 1.f;
                continue;
            }
            if ((src.kind != ShuffleSource::eInput) || ((src.slot != 1) && (src.slot != 2))) {
                continue;
            }
            const std::string slotLayer = getSlotLayer(src.slot);
            if (slotLayer.empty()) {
                continue;
            }
            ImagePtr slotImage = fetchInputPlane(args, getSlotInput(src.slot), slotLayer, &fetched);
            if (slotImage) {
                findChannelInPlane(slotImage, planeChannelName(slotImage->getComponents(), src.index), &fill);
            }
        }
        fills.push_back(planeFills);
    }

    std::map<const Image*, Image::ReadAccessPtr> readAccesses;
    for (std::vector<FetchedPlane>::const_iterator it = fetched.begin(); it != fetched.end(); ++it) {
        if (it->image && (readAccesses.find(it->image.get()) == readAccesses.end())) {
            readAccesses[it->image.get()] = std::make_shared<Image::ReadAccess>(it->image.get());
        }
    }

    std::vector<std::vector<ChannelFill>>::const_iterator planeFills = fills.begin();
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it, ++planeFills) {
        const ImagePtr& outImage = it->second;
        if (!outImage) {
            continue;
        }
        if (outImage->getBitDepth() != eImageBitDepthFloat) {
            return eStatusFailed;
        }

        // The image can be wider than the plane (a plane with no same-sized supported layout);
        // the host reads channel c of the plane back from channel c of the image.
        const int dstComps = (int)outImage->getComponentsCount();
        const int planeComps = std::min(dstComps, (int)planeFills->size());
        std::vector<const Image::ReadAccess*> sources((std::size_t)planeComps, nullptr);
        for (int c = 0; c < planeComps; ++c) {
            const ChannelFill& fill = (*planeFills)[c];
            if (fill.image) {
                sources[c] = readAccesses[fill.image.get()].get();
            }
        }

        Image::WriteAccess dst(outImage.get());
        for (int y = args.roi.y1; y < args.roi.y2; ++y) {
            if (aborted()) {
                return eStatusOK;
            }
            for (int x = args.roi.x1; x < args.roi.x2; ++x) {
                float* out = (float*)dst.pixelAt(x, y);
                if (!out) {
                    continue;
                }
                for (int c = 0; c < dstComps; ++c) {
                    float value = 0.f;
                    if (c < planeComps) {
                        const ChannelFill& fill = (*planeFills)[c];
                        if (sources[c]) {
                            const float* in = (const float*)sources[c]->pixelAt(x, y);
                            value = in ? in[fill.channel] : 0.f;
                        } else {
                            value = fill.constant;
                        }
                    }
                    out[c] = value;
                }
            }
        }
    }

    return eStatusOK;
} // Shuffle::render

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
        if (outLayer.empty() || !resolveOutputLayerDesc(outLayer, time, view, &outDesc)) {
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
                // Resolved by name as render() does, so a Color plane's missing R/G/B/A is caught
                // whatever its channel count.
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

NativePluginDescription
ShuffleCopy::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_SHUFFLECOPY;
    desc.label = "ShuffleCopy";
    desc.description = tr("Rearrange channels between layers of two inputs. The second slot reads input 2, the "
                          "main input, and the first slot reads input 1; each feeds an output layer, and every "
                          "output channel takes a slot channel, 0 or 1. By default the color comes from input 2 "
                          "and the alpha from input 1. Every layer that is not an output passes through from "
                          "input 2 unchanged.")
                           .toStdString();
    desc.grouping = PLUGIN_GROUP_CHANNEL;
    desc.majorVersion = 1;
    desc.minorVersion = 0;
    desc.inputs.push_back(NativeInputDescription("2", true, eDataKindImage));
    desc.inputs.push_back(NativeInputDescription("1", true, eDataKindImage));
    desc.outputKind = eDataKindImage;

    return desc;
}

NATRON_NAMESPACE_EXIT
