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

#include "NetworkNode.h"

#include <algorithm>
#include <cctype>
#include <deque>
#include <mutex>
#include <set>
#include <sstream>
#include <variant>

#include <QCoreApplication>
#include <QDebug>
#include <QString>
#include <QStringList>
#include <QThread>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/LibraryBinary.h"
#include "Engine/Node.h"
#include "Engine/Nodes/MissingPluginNode.h"
#include "Engine/Nodes/Network/PortTypes.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"

#include "Serialization/KnobSerialization.h"
#include "Serialization/NodeSerialization.h"

NATRON_NAMESPACE_ENTER

NATRON_NAMESPACE_ANONYMOUS_ENTER

struct LayoutRegistry {
    std::mutex mutex;
    std::map<std::string, const NetworkNodeLayout*> byId;
    std::map<std::string, NetworkNode::LayoutBuilder> builders;
    std::deque<std::unique_ptr<NetworkNodeLayout>> storage;
    // The targets of every registered plug-in, by plug-in ID, in registration order.
    std::vector<std::pair<std::string, std::vector<std::string>>> registered;
};

NodePtr
throughDots(NodePtr node)
{
    while (node && (node->getPluginID() == PLUGINID_NATRON_DOT)) {
        node = node->getInput(0);
    }

    return node;
}

LayoutRegistry&
layoutRegistry()
{
    static LayoutRegistry registry;

    return registry;
}

const NetworkNodeLayout&
emptyLayout()
{
    static const NetworkNodeLayout layout;

    return layout;
}

std::string
knobScriptName(const std::string& text)
{
    std::string ret;

    for (char c : text) {
        ret += std::isalnum((unsigned char)c) ? c : '_';
    }

    return ret;
}

KnobIPtr
createAttributeKnob(EffectInstance* holder,
                    const NetworkAttribute& attribute)
{
    const int dims = std::max(1, attribute.dimensions);
    KnobIPtr knob;

    switch (attribute.knob) {
    case NetworkKnobKind::None:
        return KnobIPtr();
    case NetworkKnobKind::Double: {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(holder, attribute.label, dims);
        for (int d = 0; d < dims; ++d) {
            if (d < (int)attribute.defaults.size()) {
                k->setDefaultValue(attribute.defaults[d], d);
            }
            if (attribute.hasDisplayMinimum) {
                k->setDisplayMinimum(attribute.displayMinimum, d);
            }
            if (attribute.hasDisplayMaximum) {
                k->setDisplayMaximum(attribute.displayMaximum, d);
            }
        }
        knob = k;
        break;
    }
    case NetworkKnobKind::Int: {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(holder, attribute.label, dims);
        for (int d = 0; d < dims; ++d) {
            if (d < (int)attribute.defaults.size()) {
                k->setDefaultValue((int)attribute.defaults[d], d);
            }
            if (attribute.hasDisplayMinimum) {
                k->setDisplayMinimum((int)attribute.displayMinimum, d);
            }
            if (attribute.hasDisplayMaximum) {
                k->setDisplayMaximum((int)attribute.displayMaximum, d);
            }
        }
        knob = k;
        break;
    }
    case NetworkKnobKind::Bool: {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(holder, attribute.label, 1);
        if (!attribute.defaults.empty()) {
            k->setDefaultValue(attribute.defaults[0] != 0.);
        }
        knob = k;
        break;
    }
    case NetworkKnobKind::Choice: {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(holder, attribute.label, 1);
        std::vector<ChoiceOption> options;
        for (const NetworkChoice& choice : attribute.choices) {
            options.push_back(ChoiceOption(choice.id, choice.label, std::string()));
        }
        (void)k->populateChoices(options);
        k->setDefaultValue(attribute.defaultChoice);
        knob = k;
        break;
    }
    case NetworkKnobKind::Color: {
        KnobColorPtr k = AppManager::createKnob<KnobColor>(holder, attribute.label, dims);
        for (int d = 0; d < dims; ++d) {
            if (d < (int)attribute.defaults.size()) {
                k->setDefaultValue(attribute.defaults[d], d);
            }
            if (attribute.hasDisplayMinimum) {
                k->setDisplayMinimum(attribute.displayMinimum, d);
            }
            if (attribute.hasDisplayMaximum) {
                k->setDisplayMaximum(attribute.displayMaximum, d);
            }
        }
        knob = k;
        break;
    }
    case NetworkKnobKind::String:
    case NetworkKnobKind::Table: {
        KnobStringPtr k = AppManager::createKnob<KnobString>(holder, attribute.label, 1);
        if (attribute.knob == NetworkKnobKind::Table) {
            k->setAsMultiLine();
        }
        k->setDefaultValue(attribute.defaultText);
        k->setAnimationEnabled(false);
        knob = k;
        break;
    }
    case NetworkKnobKind::File: {
        KnobFilePtr k = AppManager::createKnob<KnobFile>(holder, attribute.label, 1);
        k->setDefaultValue(attribute.defaultText);
        k->setAnimationEnabled(false);
        knob = k;
        break;
    }
    case NetworkKnobKind::Parametric: {
        KnobParametricPtr k = AppManager::createKnob<KnobParametric>(holder, attribute.label, dims);
        static const double colours[3][3] = { { 0.71, 0.16, 0.16 }, { 0., 0.55, 0. }, { 0.29, 0.29, 0.84 } };
        k->setParametricRange(0., 1.);
        for (int d = 0; d < dims; ++d) {
            if (dims == 3) {
                k->setCurveColor(d, colours[d][0], colours[d][1], colours[d][2]);
            }
            const bool seeded = (d < (int)attribute.curves.size()) && !attribute.curves[d].empty();
            if (!seeded) {
                StatusEnum first = k->addControlPoint(eValueChangedReasonPluginEdited, d, 0., 0., eKeyframeTypeCubic);
                StatusEnum second = k->addControlPoint(eValueChangedReasonPluginEdited, d, 1., 1., eKeyframeTypeCubic);
                Q_UNUSED(first);
                Q_UNUSED(second);
                continue;
            }
            for (const NetworkCurvePoint& point : attribute.curves[d]) {
                StatusEnum added = k->addControlPoint(eValueChangedReasonPluginEdited, d, point.x, point.y, point.interpolation);
                Q_UNUSED(added);
            }
        }
        k->setDefaultCurvesFromCurves();
        knob = k;
        break;
    }
    }
    for (int d = 0; (d < dims) && (d < (int)attribute.dimensionNames.size()); ++d) {
        knob->setDimensionName(d, attribute.dimensionNames[d]);
    }
    knob->setName(NetworkNode::knobNameOf(attribute.ramp.empty() ? attribute.name : attribute.ramp));
    if (!attribute.hint.empty()) {
        knob->setHintToolTip(attribute.hint);
    }

    return knob;
}

NATRON_NAMESPACE_ANONYMOUS_EXIT

std::string
networkJoinPorts(const std::vector<std::string>& ports)
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
networkSplitPorts(const std::string& text)
{
    std::vector<std::string> ret;
    std::istringstream in(text);
    std::string port;

    while (in >> port) {
        ret.push_back(port);
    }

    return ret;
}

const NetworkNodeLayout*
NetworkNode::addLayout(const NetworkNodeLayout& layout)
{
    LayoutRegistry& registry = layoutRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);

    registry.storage.push_back(std::make_unique<NetworkNodeLayout>(layout));
    const NetworkNodeLayout* ret = registry.storage.back().get();
    registry.byId[layout.pluginId] = ret;

    return ret;
}

const NetworkNodeLayout*
NetworkNode::findLayout(const std::string& pluginId)
{
    LayoutRegistry& registry = layoutRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    const std::map<std::string, const NetworkNodeLayout*>::const_iterator it = registry.byId.find(pluginId);

    if (it != registry.byId.end()) {
        return it->second;
    }
    const std::map<std::string, LayoutBuilder>::iterator pending = registry.builders.find(pluginId);
    if (pending == registry.builders.end()) {
        return nullptr;
    }
    const LayoutBuilder build = pending->second;
    registry.builders.erase(pending);
    std::unique_ptr<NetworkNodeLayout> layout = std::make_unique<NetworkNodeLayout>();
    std::string error;
    if (!build(layout.get(), &error)) {
        qWarning() << "The layout of" << QString::fromUtf8(pluginId.c_str()) << "cannot be built:" << QString::fromUtf8(error.c_str());

        return nullptr;
    }
    registry.storage.push_back(std::move(layout));
    const NetworkNodeLayout* ret = registry.storage.back().get();
    registry.byId[pluginId] = ret;

    return ret;
}

void
NetworkNode::addLayoutBuilder(const std::string& pluginId,
                              const LayoutBuilder& builder)
{
    LayoutRegistry& registry = layoutRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);

    registry.builders[pluginId] = builder;
}

Plugin*
NetworkNode::registerPlugin(const NetworkNodeLayout& layout)
{
    std::map<std::string, void (*)()> functions;

    functions.insert(std::make_pair("BuildEffect", (void (*)())&NetworkNode::BuildEffect));
    LibraryBinary* binary = new LibraryBinary(functions);
    const QString label = QString::fromUtf8(layout.label.c_str());
    const QStringList grouping = QString::fromUtf8(layout.grouping.c_str()).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    Plugin* plugin = appPTR->registerPlugin(QString(), grouping, QString::fromUtf8(layout.pluginId.c_str()), label,
                                            QString(), QStringList(), false, false, binary, false, 1, 0, false);

    if (!plugin) {
        return nullptr;
    }
    plugin->setForInternalUseOnly(true);
    plugin->setFirstInputDataKind(true, eDataKindNetwork);
    plugin->setInputDataKinds({ eDataKindNetwork });
    // Plug-ins registered after AppManager::load() get no script-name label from it.
    plugin->setLabelWithoutSuffix(Plugin::makeLabelWithoutSuffix(label));
    {
        LayoutRegistry& registry = layoutRegistry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        registry.registered.emplace_back(layout.pluginId, layout.targets);
    }

    return plugin;
}

std::vector<std::pair<std::string, std::vector<std::string>>>
NetworkNode::registeredPlugins()
{
    LayoutRegistry& registry = layoutRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);

    return registry.registered;
}

NetworkNode::NetworkNode(NodePtr node)
    : NativeEffectBase(node)
    , _layout(nullptr)
    , _spec()
    , _current()
    , _applyingPorts(false)
    , _portUpstreams()
    , _attributeKnobs()
    , _portsKnob()
    , _inputOutputsKnob()
{
    const Plugin* plugin = node ? node->getPlugin() : nullptr;

    _layout = plugin ? findLayout(plugin->getPluginID().toStdString()) : nullptr;
    if (!_layout) {
        _layout = &emptyLayout();
    }
    _spec = NodeSpec {
        .id = _layout->pluginId,
        .label = _layout->label,
        .description = _layout->description,
        .grouping = _layout->grouping,
        .majorVersion = 1,
        .minorVersion = 0,
        .internal = true,
        .outputKind = eDataKindNetwork,
    };
    publishSpec(sanitizePorts(_layout->defaultPorts));
}

NetworkNode::~NetworkNode()
{
}

const NetworkAttribute*
NetworkNode::findAttribute(const std::string& name) const
{
    for (const NetworkAttribute& attribute : _layout->attributes) {
        if (attribute.name == name) {
            return &attribute;
        }
    }

    return nullptr;
}

const OutputPortDescriptor*
NetworkNode::findOutput(const std::string& name) const
{
    if (name.empty()) {
        return _layout->outputs.empty() ? nullptr : &_layout->outputs.front();
    }
    for (const OutputPortDescriptor& output : _layout->outputs) {
        if (output.name == name) {
            return &output;
        }
    }

    return nullptr;
}

std::string
NetworkNode::knobNameOf(const std::string& attribute)
{
    std::string ret = knobScriptName(attribute);

    if (!ret.empty() && std::isdigit((unsigned char)ret[0])) {
        ret.insert(ret.begin(), '_');
    }

    return ret;
}

KnobIPtr
NetworkNode::getAttributeKnob(const std::string& attribute) const
{
    const std::map<std::string, KnobIWPtr>::const_iterator it = _attributeKnobs.find(attribute);

    return (it == _attributeKnobs.end()) ? KnobIPtr() : it->second.lock();
}

void
NetworkNode::initializeKnobs()
{
    std::map<std::string, KnobPagePtr> pages;
    std::map<std::pair<std::string, std::string>, KnobGroupPtr> groups;
    KnobPagePtr firstPage;

    for (const std::string& pageLabel : _layout->pages) {
        KnobPagePtr page = createKnob<KnobPage>(pageLabel);
        page->setName(knobScriptName(pageLabel) + "Page");
        pages[pageLabel] = page;
        if (!firstPage) {
            firstPage = page;
        }
    }
    if (!firstPage) {
        firstPage = createKnob<KnobPage>(tr("Controls").toStdString());
    }

    std::map<std::string, KnobIPtr> rampKnobs;
    for (const NetworkAttribute& attribute : _layout->attributes) {
        if (!attribute.ramp.empty()) {
            const std::map<std::string, KnobIPtr>::const_iterator rampIt = rampKnobs.find(attribute.ramp);
            if (rampIt != rampKnobs.end()) {
                _attributeKnobs[attribute.name] = rampIt->second;
                continue;
            }
        }
        KnobIPtr knob = createAttributeKnob(this, attribute);
        if (!knob) {
            continue;
        }
        if (!attribute.ramp.empty()) {
            rampKnobs[attribute.ramp] = knob;
        }
        const std::map<std::string, KnobPagePtr>::const_iterator pageIt = pages.find(attribute.page);
        KnobPagePtr page = (pageIt == pages.end()) ? firstPage : pageIt->second;
        if (attribute.group.empty()) {
            page->addKnob(knob);
        } else {
            const std::pair<std::string, std::string> key(attribute.page, attribute.group);
            KnobGroupPtr& group = groups[key];
            if (!group) {
                group = createKnob<KnobGroup>(attribute.group);
                group->setName(knobScriptName(attribute.page) + "_" + knobScriptName(attribute.group));
                page->addKnob(group);
            }
            group->addKnob(knob);
        }
        _attributeKnobs[attribute.name] = knob;
    }

    KnobStringPtr ports = createKnob<KnobString>(tr("Ports").toStdString());
    ports->setName(kNetworkNodeParamPorts);
    ports->setAnimationEnabled(false);
    ports->setEvaluateOnChange(false);
    ports->setSecret(true);
    firstPage->addKnob(ports);
    _portsKnob = ports;
    // An empty default, so that only an empty port list goes unsaved and a node keeps its ports
    // when the layout's default ports change.
    writePortsKnob(networkJoinPorts(getPorts()));

    KnobStringPtr inputOutputs = createKnob<KnobString>(tr("Input Outputs").toStdString());
    inputOutputs->setName(kNetworkNodeParamInputOutputs);
    inputOutputs->setAnimationEnabled(false);
    inputOutputs->setEvaluateOnChange(false);
    inputOutputs->setSecret(true);
    firstPage->addKnob(inputOutputs);
    _inputOutputsKnob = inputOutputs;
}

std::vector<std::string>
NetworkNode::sanitizePorts(const std::vector<std::string>& ports) const
{
    std::vector<std::string> ret;
    std::set<std::string> seen;

    for (const std::string& port : ports) {
        const NetworkAttribute* attribute = findAttribute(port);
        if (attribute && canShowPort(*attribute) && seen.insert(port).second) {
            ret.push_back(port);
        }
    }
    for (const NetworkAttribute& attribute : _layout->attributes) {
        if (attribute.portOnly && seen.insert(attribute.name).second) {
            ret.push_back(attribute.name);
        }
    }

    return ret;
}

void
NetworkNode::publishSpec(const std::vector<std::string>& ports)
{
    std::shared_ptr<SpecStorage> storage = std::make_shared<SpecStorage>();

    for (const std::string& port : ports) {
        storage->labels.push_back(port);
        const NetworkAttribute* attribute = findAttribute(port);
        PortDescriptor descriptor = attribute ? attribute->port : PortDescriptor();
        descriptor.shown = true;
        storage->ports.push_back(descriptor);
    }
    _current.store(std::move(storage), std::memory_order_release);
}

std::vector<std::string>
NetworkNode::getPorts() const
{
    return currentSpec()->labels;
}

int
NetworkNode::getNInputs() const
{
    return (int)currentSpec()->labels.size();
}

std::string
NetworkNode::getInputLabel(int inputNb) const
{
    const std::shared_ptr<const SpecStorage> current = currentSpec();

    if ((inputNb < 0) || (inputNb >= (int)current->labels.size())) {
        return EffectInstance::getInputLabel(inputNb);
    }

    return current->labels[inputNb];
}

bool
NetworkNode::isInputOptional(int inputNb) const
{
    return (inputNb >= 0) && (inputNb < getNInputs());
}

bool
NetworkNode::isInputMask(int /*inputNb*/) const
{
    return false;
}

DataKindEnum
NetworkNode::getInputDataKind(int inputNb) const
{
    return ((inputNb >= 0) && (inputNb < getNInputs())) ? eDataKindNetwork : eDataKindImage;
}

std::optional<PortDescriptor>
NetworkNode::getInputPortDescriptor(int inputNb) const
{
    const std::shared_ptr<const SpecStorage> current = currentSpec();

    if ((inputNb < 0) || (inputNb >= (int)current->ports.size())) {
        return std::nullopt;
    }

    return current->ports[inputNb];
}

std::vector<PortDescriptor>
NetworkNode::getPortDescriptors() const
{
    const std::vector<std::string> ports = getPorts();
    std::vector<PortDescriptor> ret;

    for (const NetworkAttribute& attribute : _layout->attributes) {
        if (!attribute.linkable) {
            continue;
        }
        PortDescriptor descriptor = attribute.port;
        descriptor.shown = std::find(ports.begin(), ports.end(), attribute.name) != ports.end();
        ret.push_back(descriptor);
    }

    return ret;
}

void
NetworkNode::applyPorts(const std::vector<std::string>& requested)
{
    const std::vector<std::string> ports = sanitizePorts(requested);
    const std::vector<std::string> old = getPorts();

    if (ports == old) {
        writePortsKnob(networkJoinPorts(ports));

        return;
    }
    NodePtr node = getNode();
    std::map<std::string, NodePtr> upstream;

    for (std::size_t i = 0; i < old.size(); ++i) {
        NodePtr input = node->getInput((int)i);
        if (input) {
            upstream[old[i]] = input;
        }
    }
    // initializeInputs() keeps connections by input number, so every connection whose port moves
    // or goes away is undone first, which also clears it from the upstream node's outputs.
    _applyingPorts = true;
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
    _applyingPorts = false;
    writePortsKnob(networkJoinPorts(ports));
    pruneInputOutputs();
    refreshKnobsEnabled();
}

void
NetworkNode::writePortsKnob(const std::string& text)
{
    KnobStringPtr knob = _portsKnob.lock();

    if (!knob || (knob->getValue() == text)) {
        return;
    }
    knob->blockValueChanges();
    knob->setValue(text);
    knob->unblockValueChanges();
}

void
NetworkNode::setPorts(const std::vector<std::string>& ports)
{
    applyPorts(ports);
}

bool
NetworkNode::showPort(const std::string& name)
{
    const NetworkAttribute* attribute = findAttribute(name);

    if (!attribute || !canShowPort(*attribute)) {
        return false;
    }
    std::vector<std::string> ports = getPorts();
    if (std::find(ports.begin(), ports.end(), name) == ports.end()) {
        ports.push_back(name);
        setPorts(ports);
    }

    return true;
}

bool
NetworkNode::hidePort(const std::string& name)
{
    const NetworkAttribute* attribute = findAttribute(name);

    if (!attribute || !canHidePort(*attribute)) {
        return false;
    }
    std::vector<std::string> ports = getPorts();
    const std::vector<std::string>::iterator it = std::find(ports.begin(), ports.end(), name);
    if (it != ports.end()) {
        ports.erase(it);
        setPorts(ports);
    }

    return true;
}

void
NetworkNode::refreshKnobsEnabled()
{
    NodePtr node = getNode();
    const std::vector<std::string> ports = getPorts();
    std::set<std::string> connected;

    for (std::size_t i = 0; node && (i < ports.size()); ++i) {
        if (node->getInput((int)i)) {
            connected.insert(ports[i]);
        }
    }
    std::map<KnobI*, KnobIPtr> knobs;
    std::set<KnobI*> driven;
    for (const auto& entry : _attributeKnobs) {
        KnobIPtr knob = entry.second.lock();
        if (!knob) {
            continue;
        }
        knobs[knob.get()] = knob;
        if (connected.count(entry.first)) {
            driven.insert(knob.get());
        }
    }
    for (const auto& entry : knobs) {
        KnobI* knob = entry.first;
        const bool disabledHere = _disabledKnobs.count(knob) != 0;
        if (driven.count(knob)) {
            if (!disabledHere && knob->isEnabled(0)) {
                knob->setAllDimensionsEnabled(false);
                _disabledKnobs.insert(knob);
            }
        } else if (disabledHere) {
            _disabledKnobs.erase(knob);
            enableUnlinkedDimensions(knob);
        }
    }
}

void
NetworkNode::enableUnlinkedDimensions(KnobI* knob)
{
    // A link disables the dimensions it drives too, and a port can be connected before a link
    // is made or, in a project load, before links are restored.
    for (int i = 0; i < knob->getDimension(); ++i) {
        if (!knob->isEnabled(i) && !knob->getMaster(i).second) {
            knob->setEnabled(i, true);
        }
    }
}

void
NetworkNode::onKnobsAboutToBeLoaded(const Serialization::NodeSerializationPtr& serialization)
{
    if (!serialization) {
        return;
    }
    // The inputs must exist before the loader connects them, and it connects them by label. The
    // ports knob is saved unless it is empty, its default.
    std::vector<std::string> savedPorts;
    for (const Serialization::KnobSerialization& knob : serialization->knobs) {
        if ((knob.name != kNetworkNodeParamPorts) || knob.dimensions.empty() || !knob.dimensions[0].value) {
            continue;
        }
        if (const std::string* text = std::get_if<std::string>(&*knob.dimensions[0].value)) {
            savedPorts = networkSplitPorts(*text);
        }
    }
    applyPorts(savedPorts);
    // A renderer upgrade can remove an attribute: its saved value has no knob to go to.
    AppInstancePtr app = getApp();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    if (!project) {
        return;
    }
    const QString nodeName = QString::fromUtf8(serialization->scriptName.c_str());
    for (const Serialization::KnobSerialization& knob : serialization->knobs) {
        if (getKnobByName(NATRON_PYTHON_NAMESPACE::makeNameScriptFriendly(knob.name))) {
            continue;
        }
        project->addLoadWarning(tr("%1.%2: %3 has no such parameter; its value was dropped")
                                    .arg(nodeName)
                                    .arg(QString::fromUtf8(knob.name.c_str()))
                                    .arg(QString::fromUtf8(_layout->pluginId.c_str())));
    }
}

void
NetworkNode::onKnobsLoaded()
{
    // The saved text may name ports the layout no longer has.
    writePortsKnob(networkJoinPorts(getPorts()));
    pruneInputOutputs();
    // The loader restores the saved enabled states, which only record which ports were connected
    // then; refreshKnobsEnabled() disables the knobs of the ports connected now.
    for (const auto& entry : _attributeKnobs) {
        if (KnobIPtr knob = entry.second.lock()) {
            enableUnlinkedDimensions(knob.get());
        }
    }
    _disabledKnobs.clear();
    refreshKnobsEnabled();
}

bool
NetworkNode::knobChanged(KnobI* k,
                         ValueChangedReasonEnum reason,
                         ViewSpec /*view*/,
                         double /*time*/,
                         bool /*originatedFromMainThread*/)
{
    KnobStringPtr ports = _portsKnob.lock();

    if (!ports || (k != ports.get()) || (QThread::currentThread() != qApp->thread())) {
        return false;
    }
    // The knob's own default is empty; the node's default is the layout's.
    if (reason == eValueChangedReasonRestoreDefault) {
        applyPorts(_layout->defaultPorts);
    } else {
        applyPorts(networkSplitPorts(ports->getValue()));
    }

    return true;
}

std::map<std::string, std::string>
NetworkNode::getInputOutputs() const
{
    std::map<std::string, std::string> ret;
    KnobStringPtr knob = _inputOutputsKnob.lock();

    if (!knob) {
        return ret;
    }
    for (const std::string& entry : networkSplitPorts(knob->getValue())) {
        const std::size_t eq = entry.find('=');
        if ((eq != std::string::npos) && (eq > 0)) {
            ret[entry.substr(0, eq)] = entry.substr(eq + 1);
        }
    }

    return ret;
}

std::string
NetworkNode::getOutputForInput(const std::string& inputLabel) const
{
    const std::map<std::string, std::string> outputs = getInputOutputs();
    const std::map<std::string, std::string>::const_iterator it = outputs.find(inputLabel);

    return (it == outputs.end()) ? std::string() : it->second;
}

void
NetworkNode::setInputOutputs(const std::map<std::string, std::string>& outputs)
{
    std::vector<std::string> entries;

    for (const auto& entry : outputs) {
        entries.push_back(entry.first + "=" + entry.second);
    }
    KnobStringPtr knob = _inputOutputsKnob.lock();
    const std::string text = networkJoinPorts(entries);
    if (knob && (knob->getValue() != text)) {
        knob->setValue(text);
    }
}

void
NetworkNode::setOutputForInput(const std::string& inputLabel,
                               const std::string& outputName)
{
    std::map<std::string, std::string> outputs = getInputOutputs();

    if (outputName.empty()) {
        outputs.erase(inputLabel);
    } else {
        outputs[inputLabel] = outputName;
    }
    setInputOutputs(outputs);
}

void
NetworkNode::pruneInputOutputs()
{
    const std::vector<std::string> ports = getPorts();
    const std::map<std::string, std::string> outputs = getInputOutputs();
    std::map<std::string, std::string> kept;

    for (const auto& entry : outputs) {
        if (std::find(ports.begin(), ports.end(), entry.first) != ports.end()) {
            kept.insert(entry);
        }
    }
    for (std::map<std::string, NodeWPtr>::iterator it = _portUpstreams.begin(); it != _portUpstreams.end();) {
        if (std::find(ports.begin(), ports.end(), it->first) == ports.end()) {
            it = _portUpstreams.erase(it);
        } else {
            ++it;
        }
    }
    if (kept.size() != outputs.size()) {
        setInputOutputs(kept);
    }
}

void
NetworkNode::onInputConnectionChanged(int inputNb)
{
    if (_applyingPorts) {
        return;
    }
    // A load or a paste connects inputs without calling onInputChanged(), so the upstream node is
    // recorded here too: a later connection to another node must then drop the output name.
    const std::vector<std::string> ports = getPorts();
    NodePtr node = getNode();
    NodePtr input = (node && (inputNb >= 0) && (inputNb < (int)ports.size())) ? throughDots(node->getInput(inputNb)) : NodePtr();
    if (input && !_portUpstreams.count(ports[inputNb])) {
        _portUpstreams[ports[inputNb]] = input;
    }
    refreshKnobsEnabled();
}

void
NetworkNode::onInputChanged(int inputNb)
{
    if (_applyingPorts) {
        return;
    }
    const std::vector<std::string> ports = getPorts();
    if ((inputNb < 0) || (inputNb >= (int)ports.size())) {
        return;
    }
    NodePtr node = getNode();
    NodePtr input = node ? throughDots(node->getInput(inputNb)) : NodePtr();
    // A disconnect keeps the name: deleting the upstream node disconnects the input, and undoing
    // the delete reconnects the same node, which must read the same output again.
    if (!input) {
        return;
    }
    const std::string& label = ports[inputNb];
    const std::map<std::string, NodeWPtr>::const_iterator previous = _portUpstreams.find(label);
    const bool otherUpstream = (previous != _portUpstreams.end()) && (previous->second.lock() != input);
    _portUpstreams[label] = input;
    const std::string output = getOutputForInput(label);
    if (output.empty()) {
        return;
    }
    const NetworkNode* upstream = dynamic_cast<const NetworkNode*>(input->getEffectInstance().get());
    // A missing shader's outputs are unknown, so the name its reader saved is kept for the day
    // the shader is back.
    const bool missingUpstream = dynamic_cast<const MissingPluginNode*>(input->getEffectInstance().get()) != NULL;
    // A loader or a paste connects inputs one at a time after all their names are set, with no
    // previous upstream, so only the changed input's entry is checked.
    if (otherUpstream || (!upstream && !missingUpstream) || (upstream && !upstream->findOutput(output))) {
        setOutputForInput(label, std::string());
    }
}

NetworkNode::ConnectionCheckEnum
NetworkNode::checkConnection(const NetworkNode& upstream,
                             const std::string& outputName,
                             const NetworkNode& downstream,
                             const std::string& inputLabel,
                             std::string* reason)
{
    ConnectionCheckEnum check = eConnectionOk;
    std::string why;

    const OutputPortDescriptor* output = upstream.findOutput(outputName);
    const std::vector<std::string> ports = downstream.getPorts();
    const NetworkAttribute* attribute = downstream.findAttribute(inputLabel);
    if (!portTargetsShared(upstream.layout().targets, downstream.layout().targets)) {
        check = eConnectionNoCommonTarget;
    } else if (!output) {
        check = eConnectionNoSuchOutput;
    } else if (!attribute || (std::find(ports.begin(), ports.end(), inputLabel) == ports.end())) {
        check = eConnectionNoSuchInput;
    } else if (!canConnectPorts(upstream.layout().targets, *output, downstream.layout().targets, attribute->port, &why)) {
        check = eConnectionTypeMismatch;
    }
    if (reason) {
        *reason = (check == eConnectionTypeMismatch) ? why : connectionCheckReason(check);
    }

    return check;
}

NetworkNode::ConnectionCheckEnum
NetworkNode::checkInputConnection(const NodePtr& upstream,
                                  const Node& downstream,
                                  int inputNb,
                                  std::string* reason)
{
    const NodePtr source = throughDots(upstream);
    const NetworkNode* from = source ? dynamic_cast<const NetworkNode*>(source->getEffectInstance().get()) : nullptr;
    const NetworkNode* to = dynamic_cast<const NetworkNode*>(downstream.getEffectInstance().get());

    if (!from || !to) {
        return eConnectionOk;
    }
    const std::vector<std::string> ports = to->getPorts();
    if ((inputNb < 0) || (inputNb >= (int)ports.size())) {
        if (reason) {
            *reason = connectionCheckReason(eConnectionNoSuchInput);
        }

        return eConnectionNoSuchInput;
    }
    // The output onInputChanged() leaves in place once the connection is made.
    const std::string& label = ports[inputNb];
    std::string output = to->getOutputForInput(label);
    const std::map<std::string, NodeWPtr>::const_iterator previous = to->_portUpstreams.find(label);
    const bool otherUpstream = (previous != to->_portUpstreams.end()) && (previous->second.lock() != source);
    if (!output.empty() && (otherUpstream || !from->findOutput(output))) {
        output.clear();
    }

    return checkConnection(*from, output, *to, ports[inputNb], reason);
}

bool
NetworkNode::canConnectPort(int inputNb,
                            const NodePtr& upstream,
                            const std::string& outputName,
                            std::string* reason) const
{
    NodePtr node = getNode();
    const std::vector<std::string> ports = getPorts();

    if (!node || (inputNb < 0) || (inputNb >= (int)ports.size())) {
        if (reason) {
            *reason = connectionCheckReason(eConnectionNoSuchInput);
        }

        return false;
    }
    if (!upstream) {
        return true;
    }
    const NodePtr source = throughDots(upstream);
    const NetworkNode* from = source ? dynamic_cast<const NetworkNode*>(source->getEffectInstance().get()) : nullptr;
    if (from && (checkConnection(*from, outputName, *this, ports[inputNb], reason) != eConnectionOk)) {
        return false;
    }
    if (!node->checkIfConnectingInputIsOk(upstream.get())) {
        if (reason) {
            *reason = tr("it would make a cycle").toStdString();
        }

        return false;
    }

    return true;
}

bool
NetworkNode::connectPort(int inputNb,
                         const NodePtr& upstream,
                         const std::string& outputName,
                         std::string* reason)
{
    if (!canConnectPort(inputNb, upstream, outputName, reason)) {
        return false;
    }
    NodePtr node = getNode();
    const std::string label = getPorts()[inputNb];
    const NodePtr current = node->getInput(inputNb);

    if (!upstream) {
        if (current) {
            node->disconnectInput(inputNb);
        }
        setOutputForInput(label, outputName);

        return true;
    }
    // Recorded first so that neither the check connecting makes nor onInputChanged() takes
    // upstream for a new node and drops the name.
    const std::map<std::string, NodeWPtr>::const_iterator previous = _portUpstreams.find(label);
    const bool hadPrevious = (previous != _portUpstreams.end());
    const NodeWPtr previousUpstream = hadPrevious ? previous->second : NodeWPtr();
    const std::string previousOutput = getOutputForInput(label);
    _portUpstreams[label] = throughDots(upstream);
    setOutputForInput(label, outputName);
    const bool connected = current ? node->replaceInput(upstream, inputNb) : node->connectInput(upstream, inputNb);
    if (!connected) {
        if (hadPrevious) {
            _portUpstreams[label] = previousUpstream;
        } else {
            _portUpstreams.erase(label);
        }
        setOutputForInput(label, previousOutput);
        if (reason) {
            *reason = tr("the input refused the connection").toStdString();
        }

        return false;
    }

    return true;
}

std::string
NetworkNode::connectionCheckReason(ConnectionCheckEnum check)
{
    switch (check) {
    case eConnectionOk:
        break;
    case eConnectionNoCommonTarget:
        return portNoSharedTargetReason();
    case eConnectionNoSuchOutput:
        return tr("the upstream node has no such output").toStdString();
    case eConnectionNoSuchInput:
        return tr("the input is not a port").toStdString();
    case eConnectionTypeMismatch:
        return tr("the output's type does not convert to the port's").toStdString();
    }

    return std::string();
}

StatusEnum
NetworkNode::render(const RenderActionArgs& /*args*/)
{
    return eStatusFailed;
}

NetworkEditPortsCommand::NetworkEditPortsCommand(const NodePtr& node,
                                                 const std::vector<std::string>& ports,
                                                 const std::string& text)
    : UndoCommand(text)
    , _node(node)
    , _before()
    , _after(ports)
    , _connections()
    , _outputs()
{
    NetworkNode* effect = node ? dynamic_cast<NetworkNode*>(node->getEffectInstance().get()) : nullptr;

    if (!effect) {
        return;
    }
    _before = effect->getPorts();
    for (std::size_t i = 0; i < _before.size(); ++i) {
        NodePtr input = node->getInput((int)i);
        if (input) {
            _connections[_before[i]] = input;
        }
        const std::string output = effect->getOutputForInput(_before[i]);
        if (!output.empty()) {
            _outputs[_before[i]] = output;
        }
    }
}

void
NetworkEditPortsCommand::redo()
{
    NodePtr node = _node.lock();
    NetworkNode* effect = node ? dynamic_cast<NetworkNode*>(node->getEffectInstance().get()) : nullptr;

    if (effect) {
        effect->setPorts(_after);
    }
}

void
NetworkEditPortsCommand::undo()
{
    NodePtr node = _node.lock();
    NetworkNode* effect = node ? dynamic_cast<NetworkNode*>(node->getEffectInstance().get()) : nullptr;

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
    for (const auto& output : _outputs) {
        effect->setOutputForInput(output.first, output.second);
    }
}

bool
makeNetworkPortCommand(const NodePtr& node,
                       const std::string& attribute,
                       bool show,
                       UndoCommandPtr* command)
{
    NetworkNode* effect = node ? dynamic_cast<NetworkNode*>(node->getEffectInstance().get()) : nullptr;
    const NetworkAttribute* found = effect ? effect->findAttribute(attribute) : nullptr;

    command->reset();
    if (!found || !(show ? NetworkNode::canShowPort(*found) : NetworkNode::canHidePort(*found))) {
        return false;
    }
    std::vector<std::string> ports = effect->getPorts();
    const std::vector<std::string>::iterator it = std::find(ports.begin(), ports.end(), attribute);
    if (show == (it != ports.end())) {
        return true;
    }
    if (show) {
        ports.push_back(attribute);
    } else {
        ports.erase(it);
    }
    *command = std::make_shared<NetworkEditPortsCommand>(node, ports, (show ? "Show Port " : "Hide Port ") + found->label);

    return true;
}

bool
showNetworkPort(const NodePtr& node,
                const std::string& attribute)
{
    UndoCommandPtr command;

    if (!makeNetworkPortCommand(node, attribute, true, &command)) {
        return false;
    }
    if (command) {
        node->pushUndoCommand(command);
    }

    return true;
}

bool
hideNetworkPort(const NodePtr& node,
                const std::string& attribute)
{
    UndoCommandPtr command;

    if (!makeNetworkPortCommand(node, attribute, false, &command)) {
        return false;
    }
    if (command) {
        node->pushUndoCommand(command);
    }

    return true;
}

NATRON_NAMESPACE_EXIT
