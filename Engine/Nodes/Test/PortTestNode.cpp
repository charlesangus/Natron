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

#include "PortTestNode.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <variant>

#include <QCoreApplication>
#include <QString>
#include <QStringList>
#include <QThread>

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/KnobTypes.h"
#include "Engine/LibraryBinary.h"
#include "Engine/Node.h"
#include "Engine/Plugin.h"

#include "Serialization/KnobSerialization.h"
#include "Serialization/NodeSerialization.h"

NATRON_NAMESPACE_ENTER

NATRON_NAMESPACE_ANONYMOUS_ENTER

struct AttributeRun {
    const char* type;
    int count;
    bool bindable;
};

const char*
nameStem(const std::string& type)
{
    static const std::map<std::string, const char*> stems = {
        { "float", "float" },
        { "color3", "color" },
        { "vector3", "vector" },
        { "float2", "pair" },
        { "int", "int" },
        { "bool", "toggle" },
        { "token", "choice" },
        { "string", "text" },
        { "object", "object" },
        { "float[]", "floatList" },
        { "color3[]", "colorList" },
        { "int[]", "intList" },
    };
    const std::map<std::string, const char*>::const_iterator it = stems.find(type);

    return (it == stems.end()) ? "attribute" : it->second;
}

std::string
labelFromName(const std::string& name)
{
    std::string ret;

    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        const bool startsWord = (i > 0) && (std::isupper((unsigned char)c) || (std::isdigit((unsigned char)c) && !std::isdigit((unsigned char)name[i - 1])));
        if (startsWord) {
            ret += ' ';
        }
        ret += (i == 0) ? (char)std::toupper((unsigned char)c) : c;
    }

    return ret;
}

PortTestLayout
makeLayout(const std::string& name,
           const std::string& description,
           const std::vector<AttributeRun>& runs,
           const std::vector<PortTestOutput>& outputs,
           const std::vector<std::string>& targets,
           bool allPorts)
{
    PortTestLayout layout;
    std::map<std::string, int> counters;

    layout.pluginId = std::string(kPortTestPluginIDPrefix) + name + (allPorts ? kPortTestAllPortsSuffix : "");
    layout.label = "Port" + name + (allPorts ? "AllPorts" : "");
    layout.description = description;
    layout.outputs = outputs;
    layout.targets = targets;
    for (const AttributeRun& run : runs) {
        for (int i = 0; i < run.count; ++i) {
            const std::string stem = nameStem(run.type);
            PortTestAttribute attribute;
            attribute.name = stem + std::to_string(counters[stem]++);
            attribute.type = run.type;
            attribute.bindable = run.bindable;
            layout.attributes.push_back(attribute);
        }
    }
    // Knob-first kinds promote about ten key attributes, the case the design must carry.
    const std::size_t keyPorts = 10;
    for (const PortTestAttribute& attribute : layout.attributes) {
        if (allPorts || (attribute.bindable && (layout.defaultPorts.size() < keyPorts))) {
            layout.defaultPorts.push_back(attribute.name);
        }
    }

    return layout;
}

std::vector<PortTestLayout>
buildLayouts()
{
    // Type mixes copied from the attribute declarations of the renderers' shaders: MoonRay's
    // DwaBaseMaterial (101 attributes with its included groups), Cycles' Principled BSDF and Image
    // Texture, the median MoonRay map (4 attributes), and a 170-attribute top end with the
    // DwaBaseMaterial mix.
    const std::vector<AttributeRun> dwaBase = {
        { "float", 34, true },
        { "bool", 20, false },
        { "color3", 13, true },
        { "token", 11, false },
        { "float", 10, false },
        { "object", 6, false },
        { "string", 2, false },
        { "int", 1, false },
        { "float2", 1, true },
        { "float[]", 1, false },
        { "color3[]", 1, false },
        { "int[]", 1, false },
    };
    const std::vector<AttributeRun> topEnd = {
        { "float", 57, true },
        { "bool", 34, false },
        { "color3", 22, true },
        { "token", 19, false },
        { "float", 17, false },
        { "object", 10, false },
        { "string", 3, false },
        { "int", 2, false },
        { "float2", 2, true },
        { "float[]", 2, false },
        { "color3[]", 1, false },
        { "int[]", 1, false },
    };
    const std::vector<AttributeRun> principled = {
        { "float", 22, true },
        { "color3", 5, true },
        { "vector3", 4, true },
        { "bool", 1, false },
        { "int", 1, false },
        { "token", 2, false },
    };
    const std::vector<AttributeRun> imageTexture = {
        { "vector3", 1, true },
        { "string", 1, false },
        { "token", 5, false },
        { "float", 1, false },
    };
    const std::vector<AttributeRun> map = {
        { "color3", 1, true },
        { "float", 2, true },
        { "bool", 1, false },
    };
    const std::vector<PortTestOutput> material = { { "surface", "closure" } };
    const std::vector<PortTestOutput> bsdf = { { "BSDF", "closure" } };
    const std::vector<PortTestOutput> colorAlpha = { { "Color", "color3" }, { "Alpha", "float" } };
    const std::vector<PortTestOutput> mapOut = { { "out", "color3" } };
    const std::vector<std::string> moonray = { "moonray" };
    const std::vector<std::string> cycles = { "cycles" };

    std::vector<PortTestLayout> ret;
    for (bool allPorts : { false, true }) {
        ret.push_back(makeLayout("DwaBaseMaterial", "A material with the attribute mix of a large layered surface.", dwaBase, material, moonray, allPorts));
        ret.push_back(makeLayout("TopEndMaterial", "A material with 170 attributes.", topEnd, material, moonray, allPorts));
        ret.push_back(makeLayout("PrincipledBsdf", "A BSDF with the attribute mix of a principled surface.", principled, bsdf, cycles, allPorts));
        ret.push_back(makeLayout("ImageTexture", "A texture with two outputs, colour and alpha.", imageTexture, colorAlpha, cycles, allPorts));
        ret.push_back(makeLayout("Map", "A map with the median attribute count.", map, mapOut, moonray, allPorts));
    }

    return ret;
}

NATRON_NAMESPACE_ANONYMOUS_EXIT

std::string
portTestJoinPorts(const std::vector<std::string>& ports)
{
    std::string ret;

    for (const std::string& port : ports) {
        if (!ret.empty()) {
            ret += ' ';
        }
        ret += port;
    }

    return ret;
}

std::vector<std::string>
portTestSplitPorts(const std::string& text)
{
    std::vector<std::string> ret;
    std::istringstream in(text);
    std::string port;

    while (in >> port) {
        ret.push_back(port);
    }

    return ret;
}

const std::vector<PortTestLayout>&
PortTestNode::layouts()
{
    static const std::vector<PortTestLayout> ret = buildLayouts();

    return ret;
}

const PortTestLayout*
PortTestNode::findLayout(const std::string& pluginId)
{
    for (const PortTestLayout& layout : layouts()) {
        if (layout.pluginId == pluginId) {
            return &layout;
        }
    }

    return nullptr;
}

std::vector<std::string>
PortTestNode::registerPlugins()
{
    std::vector<std::string> ids;

    for (const PortTestLayout& layout : layouts()) {
        std::map<std::string, void (*)()> functions;
        functions.insert(std::make_pair("BuildEffect", (void (*)())&PortTestNode::BuildEffect));
        LibraryBinary* binary = new LibraryBinary(functions);
        const QString label = QString::fromUtf8(layout.label.c_str());
        Plugin* plugin = appPTR->registerPlugin(QString(), QStringList(QString::fromUtf8(PLUGIN_GROUP_OTHER)), QString::fromUtf8(layout.pluginId.c_str()), label,
                                                QString(), QStringList(), false, false, binary, false, 1, 0, false);
        if (plugin) {
            // Registered after onAllPluginsLoaded() in the gtests, which would leave the script
            // names of these nodes empty.
            plugin->setLabelWithoutSuffix(Plugin::makeLabelWithoutSuffix(label));
            ids.push_back(layout.pluginId);
        }
    }

    return ids;
}

PortTestNode::PortTestNode(NodePtr node)
    : NativeEffectBase(node)
    , _layout(nullptr)
    , _specs()
    , _spec(nullptr)
    , _appliedPorts()
    , _portsKnob()
    , _inputOutputsKnob()
{
    const Plugin* plugin = node ? node->getPlugin() : nullptr;

    _layout = plugin ? findLayout(plugin->getPluginID().toStdString()) : nullptr;
    if (!_layout) {
        _layout = &layouts().front();
    }
    publishSpec(_layout->defaultPorts);
}

PortTestNode::~PortTestNode()
{
}

const PortTestAttribute*
PortTestNode::findAttribute(const std::string& name) const
{
    for (const PortTestAttribute& attribute : _layout->attributes) {
        if (attribute.name == name) {
            return &attribute;
        }
    }

    return nullptr;
}

const PortTestOutput*
PortTestNode::findOutput(const std::string& name) const
{
    if (name.empty()) {
        return _layout->outputs.empty() ? nullptr : &_layout->outputs.front();
    }
    for (const PortTestOutput& output : _layout->outputs) {
        if (output.name == name) {
            return &output;
        }
    }

    return nullptr;
}

void
PortTestNode::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    for (const PortTestAttribute& attribute : _layout->attributes) {
        const std::string label = labelFromName(attribute.name);
        KnobIPtr knob;
        if (attribute.type == "float") {
            knob = createKnob<KnobDouble>(label, 1);
        } else if (attribute.type == "color3") {
            knob = createKnob<KnobColor>(label, 3);
        } else if (attribute.type == "vector3") {
            knob = createKnob<KnobDouble>(label, 3);
        } else if (attribute.type == "float2") {
            knob = createKnob<KnobDouble>(label, 2);
        } else if (attribute.type == "int") {
            knob = createKnob<KnobInt>(label, 1);
        } else if (attribute.type == "bool") {
            knob = createKnob<KnobBool>(label, 1);
        } else if (attribute.type == "token") {
            KnobChoicePtr choice = createKnob<KnobChoice>(label, 1);
            std::vector<ChoiceOption> options;
            for (const char* option : { "first", "second", "third", "fourth" }) {
                options.push_back(ChoiceOption(option));
            }
            choice->populateChoices(options);
            knob = choice;
        } else {
            KnobStringPtr text = createKnob<KnobString>(label, 1);
            text->setAnimationEnabled(false);
            knob = text;
        }
        knob->setName(attribute.name);
        page->addKnob(knob);
    }

    KnobStringPtr ports = createKnob<KnobString>(tr("Ports"));
    ports->setName(kPortTestParamPorts);
    ports->setDefaultValue(portTestJoinPorts(_layout->defaultPorts));
    ports->setAnimationEnabled(false);
    ports->setEvaluateOnChange(false);
    ports->setSecret(true);
    page->addKnob(ports);
    _portsKnob = ports;

    KnobStringPtr inputOutputs = createKnob<KnobString>(tr("Input Outputs"));
    inputOutputs->setName(kPortTestParamInputOutputs);
    inputOutputs->setAnimationEnabled(false);
    inputOutputs->setEvaluateOnChange(false);
    inputOutputs->setSecret(true);
    page->addKnob(inputOutputs);
    _inputOutputsKnob = inputOutputs;
}

std::vector<std::string>
PortTestNode::sanitizePorts(const std::vector<std::string>& ports) const
{
    std::vector<std::string> ret;
    std::set<std::string> seen;

    for (const std::string& port : ports) {
        if (findAttribute(port) && seen.insert(port).second) {
            ret.push_back(port);
        }
    }

    return ret;
}

void
PortTestNode::publishSpec(const std::vector<std::string>& ports)
{
    std::unique_ptr<SpecStorage> storage(new SpecStorage);

    for (const std::string& port : ports) {
        storage->labels.push_back(port);
    }
    storage->inputs.reserve(storage->labels.size());
    for (const std::string& label : storage->labels) {
        storage->inputs.push_back(InputSpec { .label = label, .kind = eDataKindPolymorphic, .role = InputRole::Aux, .optional = true });
    }
    storage->spec = NodeSpec {
        .id = _layout->pluginId,
        .label = _layout->label,
        .description = _layout->description,
        .grouping = PLUGIN_GROUP_OTHER,
        .majorVersion = 1,
        .minorVersion = 0,
        .outputKind = eDataKindPolymorphic,
        .inputs = storage->inputs,
    };
    _spec.store(&storage->spec, std::memory_order_release);
    _specs.push_back(std::move(storage));
    _appliedPorts = ports;
}

std::vector<std::string>
PortTestNode::getPorts() const
{
    return _appliedPorts;
}

void
PortTestNode::applyPorts(const std::vector<std::string>& requested)
{
    const std::vector<std::string> ports = sanitizePorts(requested);

    if (ports == _appliedPorts) {
        return;
    }
    NodePtr node = getNode();
    const std::vector<std::string> old = _appliedPorts;
    std::map<std::string, NodePtr> upstream;

    for (std::size_t i = 0; i < old.size(); ++i) {
        NodePtr input = node->getInput((int)i);
        if (input) {
            upstream[old[i]] = input;
        }
    }
    // initializeInputs() keeps connections by input number, so every connection whose port moves
    // or goes away is undone first, which also clears it from the upstream node's outputs.
    for (std::size_t i = 0; i < old.size(); ++i) {
        if (upstream.count(old[i]) && ((i >= ports.size()) || (ports[i] != old[i]))) {
            node->disconnectInput((int)i);
        }
    }
    publishSpec(ports);
    node->initializeInputs();
    for (std::size_t i = 0; i < ports.size(); ++i) {
        const std::map<std::string, NodePtr>::const_iterator it = upstream.find(ports[i]);
        if ((it != upstream.end()) && !node->getInput((int)i)) {
            node->connectInput(it->second, (int)i);
        }
    }
}

void
PortTestNode::setPorts(const std::vector<std::string>& ports)
{
    applyPorts(ports);
    KnobStringPtr knob = _portsKnob.lock();
    if (knob) {
        knob->setValue(portTestJoinPorts(_appliedPorts));
    }
}

void
PortTestNode::onKnobsAboutToBeLoaded(const Serialization::NodeSerializationPtr& serialization)
{
    if (!serialization) {
        return;
    }
    // The inputs must exist before the loader connects them, and it connects them by label.
    for (const Serialization::KnobSerialization& knob : serialization->knobs) {
        if ((knob.name != kPortTestParamPorts) || knob.dimensions.empty() || !knob.dimensions[0].value) {
            continue;
        }
        if (const std::string* text = std::get_if<std::string>(&*knob.dimensions[0].value)) {
            applyPorts(portTestSplitPorts(*text));
        }
    }
}

bool
PortTestNode::knobChanged(KnobI* k,
                          ValueChangedReasonEnum /*reason*/,
                          ViewSpec /*view*/,
                          double /*time*/,
                          bool /*originatedFromMainThread*/)
{
    KnobStringPtr ports = _portsKnob.lock();

    if (!ports || (k != ports.get()) || (QThread::currentThread() != qApp->thread())) {
        return false;
    }
    applyPorts(portTestSplitPorts(ports->getValue()));

    return true;
}

std::map<std::string, std::string>
PortTestNode::getInputOutputs() const
{
    std::map<std::string, std::string> ret;
    KnobStringPtr knob = _inputOutputsKnob.lock();

    if (!knob) {
        return ret;
    }
    for (const std::string& entry : portTestSplitPorts(knob->getValue())) {
        const std::size_t eq = entry.find('=');
        if ((eq != std::string::npos) && (eq > 0)) {
            ret[entry.substr(0, eq)] = entry.substr(eq + 1);
        }
    }

    return ret;
}

std::string
PortTestNode::getOutputForInput(const std::string& inputLabel) const
{
    const std::map<std::string, std::string> outputs = getInputOutputs();
    const std::map<std::string, std::string>::const_iterator it = outputs.find(inputLabel);

    return (it == outputs.end()) ? std::string() : it->second;
}

void
PortTestNode::setOutputForInput(const std::string& inputLabel,
                                const std::string& outputName)
{
    std::map<std::string, std::string> outputs = getInputOutputs();

    if (outputName.empty()) {
        outputs.erase(inputLabel);
    } else {
        outputs[inputLabel] = outputName;
    }
    std::vector<std::string> entries;
    for (const auto& entry : outputs) {
        entries.push_back(entry.first + "=" + entry.second);
    }
    KnobStringPtr knob = _inputOutputsKnob.lock();
    if (knob) {
        knob->setValue(portTestJoinPorts(entries));
    }
}

bool
PortTestNode::portTypesConvert(const std::string& from,
                               const std::string& to)
{
    if (from == to) {
        return true;
    }
    static const std::set<std::pair<std::string, std::string>> conversions = {
        { "float", "color3" },
        { "float", "vector3" },
        { "color3", "float" },
        { "vector3", "float" },
        { "color3", "vector3" },
        { "vector3", "color3" },
        { "int", "float" },
    };

    return conversions.count(std::make_pair(from, to)) > 0;
}

PortTestNode::ConnectionCheckEnum
PortTestNode::checkConnection(const PortTestNode& upstream,
                              const std::string& outputName,
                              const PortTestNode& downstream,
                              const std::string& inputLabel)
{
    bool sharesTarget = false;

    for (const std::string& target : upstream.layout().targets) {
        const std::vector<std::string>& theirs = downstream.layout().targets;
        if (std::find(theirs.begin(), theirs.end(), target) != theirs.end()) {
            sharesTarget = true;
            break;
        }
    }
    if (!sharesTarget) {
        return eConnectionNoCommonTarget;
    }
    const PortTestOutput* output = upstream.findOutput(outputName);
    if (!output) {
        return eConnectionNoSuchOutput;
    }
    const std::vector<std::string>& ports = downstream._appliedPorts;
    const PortTestAttribute* attribute = downstream.findAttribute(inputLabel);
    if (!attribute || (std::find(ports.begin(), ports.end(), inputLabel) == ports.end())) {
        return eConnectionNoSuchInput;
    }

    return portTypesConvert(output->type, attribute->type) ? eConnectionOk : eConnectionTypeMismatch;
}

StatusEnum
PortTestNode::render(const RenderActionArgs& /*args*/)
{
    return eStatusFailed;
}

PortTestEditPortsCommand::PortTestEditPortsCommand(const NodePtr& node,
                                                   const std::vector<std::string>& ports)
    : UndoCommand("Edit Ports")
    , _node(node)
    , _before()
    , _after(ports)
    , _connections()
{
    PortTestNode* effect = node ? dynamic_cast<PortTestNode*>(node->getEffectInstance().get()) : nullptr;

    if (!effect) {
        return;
    }
    _before = effect->getPorts();
    for (std::size_t i = 0; i < _before.size(); ++i) {
        NodePtr input = node->getInput((int)i);
        if (input) {
            _connections[_before[i]] = input;
        }
    }
}

void
PortTestEditPortsCommand::redo()
{
    NodePtr node = _node.lock();
    PortTestNode* effect = node ? dynamic_cast<PortTestNode*>(node->getEffectInstance().get()) : nullptr;

    if (effect) {
        effect->setPorts(_after);
    }
}

void
PortTestEditPortsCommand::undo()
{
    NodePtr node = _node.lock();
    PortTestNode* effect = node ? dynamic_cast<PortTestNode*>(node->getEffectInstance().get()) : nullptr;

    if (!effect) {
        return;
    }
    effect->setPorts(_before);
    for (std::size_t i = 0; i < _before.size(); ++i) {
        const std::map<std::string, NodeWPtr>::const_iterator it = _connections.find(_before[i]);
        NodePtr input = (it == _connections.end()) ? NodePtr() : it->second.lock();
        if (input && !node->getInput((int)i)) {
            node->connectInput(input, (int)i);
        }
    }
}

NATRON_NAMESPACE_EXIT
