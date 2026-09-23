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

#include <list>
#include <string>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/ChoiceOption.h"
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

StatusEnum
Shuffle::render(const RenderActionArgs& /*args*/)
{
    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
