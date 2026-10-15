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

#include <QMutexLocker>

#include "Engine/Node.h"

#include "Serialization/NodeSerialization.h"

NATRON_NAMESPACE_ENTER

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

NativePluginDescription
MissingPluginNode::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_MISSING_PLUGIN;
    desc.label = "MissingPlugin";
    desc.description = tr("Placeholder for a node whose plug-in is not installed. "
                          "It keeps the node's settings and connections so that saving the project does not lose them.")
                           .toStdString();
    desc.grouping = PLUGIN_GROUP_OTHER;
    desc.majorVersion = 1;
    desc.minorVersion = 0;
    desc.outputKind = eDataKindPolymorphic;
    {
        QMutexLocker k(&_storedMutex);
        for (const std::string& label : _inputLabels) {
            desc.inputs.push_back(NativeInputDescription(label, true, eDataKindPolymorphic));
        }
    }

    return desc;
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
    }
    getNode()->initializeInputs();
}

void
MissingPluginNode::onEffectCreated(bool /*mayCreateFileDialog*/,
                                   const CreateNodeArgs& /*args*/)
{
    const Serialization::NodeSerializationPtr stored = getStoredSerialization();

    if (!stored) {
        return;
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
