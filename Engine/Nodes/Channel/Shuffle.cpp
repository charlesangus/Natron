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

KnobChoicePtr
configureInputChoice(const KnobChoicePtr& choice,
                     const std::string& name,
                     int defaultInput)
{
    choice->setName(name);
    choice->setAnimationEnabled(false);
    choice->setIsMetadataSlave(true);
    std::vector<ChoiceOption> options;
    options.push_back(ChoiceOption("B", "B", std::string()));
    options.push_back(ChoiceOption("A", "A", std::string()));
    choice->populateChoices(options);
    choice->setDefaultValue(defaultInput);

    return choice;
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
    if (defaultNone) {
        knob->setDefaultValue(knob->encode(std::string(), std::vector<std::string>()));
    }
}
} // namespace

Shuffle::Shuffle(NodePtr node)
    : NativeEffectBase(node)
    , _in1Input()
    , _in2Input()
    , _in1()
    , _in2()
    , _out1()
    , _out2()
    , _mapping()
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
    desc.description = tr("Rearrange channels between layers. Two input slots, each a layer read from B or A, "
                          "feed two output layers; every output channel takes a slot channel, 0, 1, or keeps "
                          "B's value. Every layer that is not an output passes through from B unchanged.")
                           .toStdString();
    desc.grouping = PLUGIN_GROUP_CHANNEL;
    desc.majorVersion = 1;
    desc.minorVersion = 0;
    desc.inputs.push_back(NativeInputDescription("B", true, eDataKindImage));
    desc.inputs.push_back(NativeInputDescription("A", true, eDataKindImage));
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

    KnobChoicePtr in1Input = configureInputChoice(createKnob<KnobChoice>(tr("In 1 Input")), kShuffleParamIn1Input, (int)eInputB);
    in1Input->setHintToolTip(tr("The input the first slot reads its layer from."));
    in1Input->setAddNewLine(false);
    page->addKnob(in1Input);
    _in1Input = in1Input;

    KnobLayerSelectPtr in1 = createKnob<KnobLayerSelect>(tr("In 1"));
    configureLayerSelect(in1, kShuffleParamIn1, true, false);
    in1->setHintToolTip(tr("The layer the first slot reads. A layer the input no longer has is kept and marked "
                           "\"(not in input)\"."));
    page->addKnob(in1);
    _in1 = in1;

    KnobChoicePtr in2Input = configureInputChoice(createKnob<KnobChoice>(tr("In 2 Input")), kShuffleParamIn2Input, (int)eInputA);
    in2Input->setHintToolTip(tr("The input the second slot reads its layer from."));
    in2Input->setAddNewLine(false);
    page->addKnob(in2Input);
    _in2Input = in2Input;

    KnobLayerSelectPtr in2 = createKnob<KnobLayerSelect>(tr("In 2"));
    configureLayerSelect(in2, kShuffleParamIn2, true, true);
    in2->setHintToolTip(tr("The layer the second slot reads, or None."));
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
    mapping->setHintToolTip(tr("The source of every output channel: a slot channel, 0, 1, or keep (B's same channel "
                               "of the same layer, 0 if B lacks it). An output channel with no source keeps."));
    page->addKnob(mapping);
    _mapping = mapping;

    NodePtr node = getNode();
    if (node) {
        node->declareLayerKnob(in1, getSlotInput(1), LayerKnobSpec::eRoleInputBound);
        node->declareLayerKnob(in2, getSlotInput(2), LayerKnobSpec::eRoleInputBound);
        node->declareLayerKnob(out1, -1, LayerKnobSpec::eRoleTarget);
        node->declareLayerKnob(out2, -1, LayerKnobSpec::eRoleTarget);
    }
} // Shuffle::initializeKnobs

int
Shuffle::getSlotInput(int slot) const
{
    KnobChoicePtr choice = (slot == 1) ? _in1Input.lock() : _in2Input.lock();

    if (!choice) {
        return (slot == 1) ? (int)eInputB : (int)eInputA;
    }

    return (choice->getValue() == (int)eInputA) ? (int)eInputA : (int)eInputB;
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

void
Shuffle::syncSlotInputs()
{
    NodePtr node = getNode();

    if (!node) {
        return;
    }
    KnobLayerSelectPtr in1 = _in1.lock();
    if (in1) {
        node->setLayerKnobInput(in1, getSlotInput(1));
    }
    KnobLayerSelectPtr in2 = _in2.lock();
    if (in2) {
        node->setLayerKnobInput(in2, getSlotInput(2));
    }
}

bool
Shuffle::knobChanged(KnobI* k,
                     ValueChangedReasonEnum /*reason*/,
                     ViewSpec /*view*/,
                     double /*time*/,
                     bool /*originatedFromMainThread*/)
{
    KnobChoicePtr in1Input = _in1Input.lock();
    KnobChoicePtr in2Input = _in2Input.lock();

    if ((in1Input && k == in1Input.get()) || (in2Input && k == in2Input.get())) {
        syncSlotInputs();

        return true;
    }

    return false;
}

void
Shuffle::onKnobsLoaded()
{
    syncSlotInputs();
}

bool
Shuffle::mappingReadsInput(int inputNb) const
{
    std::shared_ptr<KnobShuffleMap> mapping = _mapping.lock();

    if (!mapping) {
        return false;
    }
    const std::vector<ShuffleMapRow> rows = mapping->getRows();
    for (std::vector<ShuffleMapRow>::const_iterator it = rows.begin(); it != rows.end(); ++it) {
        if (it->src.kind != ShuffleSource::eInput) {
            continue;
        }
        if (getSlotLayer(it->src.slot).empty()) {
            continue;
        }
        if (getSlotInput(it->src.slot) == inputNb) {
            return true;
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

    std::list<ImageLayerDesc> bLayers;
    getPresentLayers(time, view, (int)eInputB, &bLayers);

    return findLayer(bLayers, layerID, desc);
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
    // Keep reads B even when only A is connected, so pass-through never falls back to A.
    *passThroughInputNb = (int)eInputB;

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

    std::list<ImageLayerDesc> present[2];
    for (int inputNb = 0; inputNb < 2; ++inputNb) {
        (*comps)[inputNb].clear();
        getPresentLayers(time, view, inputNb, &present[inputNb]);
    }

    for (int slot = 1; slot <= 2; ++slot) {
        const std::string layerID = getSlotLayer(slot);
        if (layerID.empty()) {
            continue;
        }
        const int inputNb = getSlotInput(slot);
        ImageLayerDesc desc;
        if (findLayer(present[inputNb], layerID, &desc)) {
            appendUnique(desc, &(*comps)[inputNb]);
        }
    }

    for (std::list<ImageLayerDesc>::const_iterator it = produced.begin(); it != produced.end(); ++it) {
        ImageLayerDesc desc;
        if (findLayer(present[eInputB], it->getLayerID(), &desc)) {
            appendUnique(desc, &(*comps)[eInputB]);
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

    std::shared_ptr<KnobShuffleMap> mapping = _mapping.lock();
    if (mapping) {
        const std::string out1Layer = getOutputLayer(1);
        const bool in1IsOut1OnB = getSlotInput(1) == (int)eInputB && !getSlotLayer(1).empty() && getSlotLayer(1) == out1Layer;
        const std::vector<ShuffleMapRow> rows = mapping->getRows();
        for (std::vector<ShuffleMapRow>::const_iterator it = rows.begin(); it != rows.end(); ++it) {
            if (it->outSlot != 1) {
                continue;
            }
            const bool straight = in1IsOut1OnB && it->src.kind == ShuffleSource::eInput && it->src.slot == 1 && it->src.index == it->outIndex;
            if (!straight) {
                return false;
            }
        }
    }

    *inputNb = (int)eInputB;
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
    bool rodSet = false;

    for (int inputNb = 0; inputNb < 2; ++inputNb) {
        if ((inputNb == (int)eInputA) && rodSet && !mappingReadsInput(inputNb)) {
            continue;
        }
        EffectInstancePtr input = getInput(inputNb);
        if (!input) {
            continue;
        }
        RectD inputRod;
        bool isProjectFormat = false;
        StatusEnum st = input->getRegionOfDefinition_public(input->getRenderHash(), time, scale, view, &inputRod, &isProjectFormat);
        if (st == eStatusFailed) {
            return st;
        }
        if (rodSet) {
            rod->merge(inputRod);
        } else {
            *rod = inputRod;
            rodSet = true;
        }
    }

    return rodSet ? eStatusOK : eStatusReplyDefault;
}

void
Shuffle::getFrameRange(double* first,
                       double* last)
{
    for (int inputNb = 0; inputNb < 2; ++inputNb) {
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
    std::shared_ptr<KnobShuffleMap> mapping = _mapping.lock();
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
        for (int c = 0; c < plane.getNumComponents(); ++c) {
            const ShuffleSource src = (mapping && outSlot) ? mapping->getSource(outSlot, c) : ShuffleSource();
            ChannelFill& fill = planeFills[c];
            if (src.kind == ShuffleSource::eZero) {
                continue;
            }
            if (src.kind == ShuffleSource::eOne) {
                fill.constant = 1.f;
                continue;
            }
            if ((src.kind == ShuffleSource::eInput) && ((src.slot == 1) || (src.slot == 2))) {
                const std::string slotLayer = getSlotLayer(src.slot);
                if (!slotLayer.empty()) {
                    const int inputNb = getSlotInput(src.slot);
                    ImagePtr slotImage = fetchInputPlane(args, inputNb, slotLayer, &fetched);
                    if (slotImage && findChannelInPlane(slotImage, planeChannelName(slotImage->getComponents(), src.index), &fill)) {
                        continue;
                    }
                }
            }
            ImagePtr bImage = fetchInputPlane(args, (int)eInputB, plane.getLayerID(), &fetched);
            findChannelInPlane(bImage, planeChannelName(plane, c), &fill);
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
Shuffle::checkExtraChannelsPresent(std::string* message)
{
    std::shared_ptr<KnobShuffleMap> mapping = _mapping.lock();

    if (!mapping) {
        return true;
    }

    AppInstancePtr app = getApp();
    const double time = app ? app->getTimeLine()->currentFrame() : 0.;
    const ViewIdx view(0);

    const std::vector<ShuffleMapRow> rows = mapping->getRows();
    for (std::vector<ShuffleMapRow>::const_iterator it = rows.begin(); it != rows.end(); ++it) {
        if (it->src.kind != ShuffleSource::eInput) {
            continue;
        }
        const std::string layerID = getSlotLayer(it->src.slot);
        if (layerID.empty()) {
            continue; // A None slot is silent: the row renders as keep.
        }
        const int inputNb = getSlotInput(it->src.slot);
        if (!getInput(inputNb)) {
            continue; // A disconnected input is silent: the row renders as keep.
        }

        std::list<ImageLayerDesc> present;
        getPresentLayers(time, view, inputNb, &present);
        ImageLayerDesc desc;
        const bool found = findLayer(present, layerID, &desc);
        const int nChannels = found ? (desc.isColorLayer() ? 4 : (int)desc.getChannels().size()) : 0;
        if (found && (it->src.index < nChannels)) {
            continue; // Readable: nothing to report.
        }

        if (message) {
            // Best-effort channel name for the message; falls back to the bare layer name when
            // even the registry has no descriptor for it (e.g. an unknown/mistyped layer).
            std::string channelName;
            ImageLayerDesc named;
            if (resolveOutputLayerDesc(layerID, time, view, &named)) {
                channelName = planeChannelName(named, it->src.index);
            }
            *message = std::string(kExtraChannelMissingMessagePrefix) + layerID
                + (channelName.empty() ? std::string() : ("." + channelName))
                + " is not in the " + getInputLabel(inputNb) + " input";
        }

        return false;
    }

    return true;
} // Shuffle::checkExtraChannelsPresent

NATRON_NAMESPACE_EXIT
