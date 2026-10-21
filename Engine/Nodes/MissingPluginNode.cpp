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

#include "MissingPluginNode.h"

#include <QDateTime>
#include <QMutexLocker>

#include "Engine/AppManager.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"

#include "Serialization/NodeSerialization.h"

NATRON_NAMESPACE_ENTER

MissingPluginNode::MissingPluginNode(NodePtr node)
    : NativeEffectBase(node)
    , _info()
    , _storedMutex()
    , _stored()
    , _inputLabels()
    , _description(tr("Placeholder for a node whose plug-in is not installed. "
                      "It keeps the node's settings and connections so that saving the project does not lose them.")
                       .toStdString())
    , _inputSpecs()
    , _spec()
{
    rebuildSpec();
}

Serialization::NodeSerializationPtr
MissingPluginNode::getStoredSerialization() const
{
    QMutexLocker k(&_storedMutex);

    return _stored;
}

void
MissingPluginNode::setStoredGuiChildren(const std::string& nodeName,
                                        const std::list<Serialization::NodeGuiSerializationPtr>& children)
{
    QMutexLocker k(&_storedMutex);

    _guiChildrenNodeName = nodeName;
    _guiChildren = children;
}

std::list<Serialization::NodeGuiSerializationPtr>
MissingPluginNode::getStoredGuiChildren(std::string* nodeName) const
{
    QMutexLocker k(&_storedMutex);

    *nodeName = _guiChildrenNodeName;

    return _guiChildren;
}

std::string
MissingPluginNode::getShortPluginName(const std::string& pluginID)
{
    static const std::string suffix = "Plugin";

    std::string name = pluginID.substr(pluginID.rfind('.') == std::string::npos ? 0 : pluginID.rfind('.') + 1);

    if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
        name.erase(name.size() - suffix.size());
    }

    return name.empty() ? pluginID : name;
}

QString
MissingPluginNode::getMissingSummary() const
{
    const Serialization::NodeSerializationPtr stored = getStoredSerialization();

    if (!stored) {
        return QString();
    }

    return tr("missing: %1 %2.%3")
        .arg(QString::fromStdString(getShortPluginName(stored->pluginID)))
        .arg(stored->pluginMajorVersion)
        .arg(stored->pluginMinorVersion);
}

void
MissingPluginNode::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));
    KnobStringPtr info = createKnob<KnobString>(tr("Missing plug-in"));

    info->setAnimationEnabled(false);
    info->setAsLabel();
    info->setEvaluateOnChange(false);
    info->setIsPersistent(false);
    page->addKnob(info);
    _info = info;
}

void
MissingPluginNode::rebuildSpec()
{
    _inputSpecs.clear();
    _inputSpecs.reserve(_inputLabels.size());
    for (const std::string& label : _inputLabels) {
        _inputSpecs.push_back(InputSpec { .label = label, .kind = eDataKindPolymorphic, .optional = true });
    }

    _spec = NodeSpec {
        .id = PLUGINID_NATRON_MISSING_PLUGIN,
        .label = "MissingPlugin",
        .description = _description,
        .grouping = PLUGIN_GROUP_OTHER,
        .majorVersion = 1,
        .minorVersion = 0,
        .internal = true,
        .outputKind = eDataKindPolymorphic,
        .inputs = _inputSpecs,
    };

    if (!isValidNodeSpec(_spec)) {
        appPTR->writeToErrorLog_mt_safe(QString::fromUtf8("MissingPluginNode"),
                                        QDateTime::currentDateTime(),
                                        tr("The inputs of the missing plug-in do not form a valid node description and are dropped."));
        _inputSpecs.clear();
        _spec.inputs = _inputSpecs;
    }
}

void
MissingPluginNode::onKnobsAboutToBeLoaded(const Serialization::NodeSerializationPtr& serialization)
{
    if (!serialization) {
        return;
    }
    {
        QMutexLocker k(&_storedMutex);
        _stored = serialization;
        _inputLabels.clear();
        if (!serialization->inputs.empty()) {
            for (const auto& input : serialization->inputs) {
                _inputLabels.push_back(input.first);
            }
        } else {
            for (std::size_t i = 0; i < serialization->oldInputs.size(); ++i) {
                _inputLabels.push_back(std::to_string(i));
            }
        }
        rebuildSpec();
    }
    getNode()->initializeInputs();
}

void
MissingPluginNode::onKnobsLoaded()
{
    // A save writes the stored serialization back, so an edit to any knob would be lost.
    const std::vector<KnobIPtr>& knobs = getKnobs();

    for (const KnobIPtr& knob : knobs) {
        knob->setAllDimensionsEnabled(false);
    }
}

void
MissingPluginNode::onEffectCreated(bool /*mayCreateFileDialog*/,
                                   const CreateNodeArgs& /*args*/)
{
    const Serialization::NodeSerializationPtr stored = getStoredSerialization();

    if (!stored) {
        return;
    }
    KnobStringPtr info = _info.lock();

    if (info) {
        info->setValue(tr("Plug-in: %1\nVersion: %2.%3\n"
                          "This plug-in is not installed. The settings and connections of this node are preserved "
                          "and will be restored when the plug-in is installed.")
                           .arg(QString::fromStdString(stored->pluginID))
                           .arg(stored->pluginMajorVersion)
                           .arg(stored->pluginMinorVersion)
                           .toStdString());
    }
    setPersistentMessage(eMessageTypeError,
                         tr("The plug-in %1 (version %2.%3) is not installed. This node is kept as a placeholder and is saved back unchanged.")
                             .arg(QString::fromStdString(stored->pluginID))
                             .arg(stored->pluginMajorVersion)
                             .arg(stored->pluginMinorVersion)
                             .toStdString());
}

StatusEnum
MissingPluginNode::render(const RenderActionArgs& /*args*/)
{
    return eStatusFailed;
}

NATRON_NAMESPACE_EXIT
