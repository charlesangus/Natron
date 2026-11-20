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

#ifndef Tests_Scene_ShaderNodeTestUtils_h
#define Tests_Scene_ShaderNodeTestUtils_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <QString>

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Test/PortTestNode.h"
#include "Engine/Project.h"

#include "Serialization/NodeSerialization.h"

NATRON_NAMESPACE_ENTER

inline std::string
portTestPluginId(const std::string& kind,
                 bool allPorts = false)
{
    return std::string(kPortTestPluginIDPrefix) + kind + (allPorts ? kPortTestAllPortsSuffix : "");
}

inline PortTestNode*
portTestEffect(const NodePtr& node)
{
    return node ? dynamic_cast<PortTestNode*>(node->getEffectInstance().get()) : nullptr;
}

inline NodePtr
createPortTestNode(const AppInstancePtr& app,
                   const std::string& pluginId,
                   const NodeCollectionPtr& collection)
{
    CreateNodeArgs args(pluginId, collection);
    args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
    args.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, false);
    args.setProperty<bool>(kCreateNodeArgsPropSettingsOpened, false);

    return app->createNode(args);
}

inline NodePtr
createEmptyGroup(const AppInstancePtr& app)
{
    CreateNodeArgs args(PLUGINID_NATRON_GROUP, app->getProject());
    args.setProperty<bool>(kCreateNodeArgsPropNodeGroupDisableCreateInitialNodes, true);
    args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
    args.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, false);
    args.setProperty<bool>(kCreateNodeArgsPropSettingsOpened, false);

    return app->createNode(args);
}

inline NodeCollectionPtr
groupCollection(const NodePtr& group)
{
    return group ? std::dynamic_pointer_cast<NodeCollection>(group->getEffectInstance()) : NodeCollectionPtr();
}

// What the node graph's paste does with a copied node, minus the reconnection it does itself.
inline NodePtr
pasteSerializedNode(const AppInstancePtr& app,
                    const Serialization::NodeSerializationPtr& serialization,
                    const NodeCollectionPtr& collection)
{
    CreateNodeArgs args(serialization->pluginID, collection);
    args.setProperty<Serialization::NodeSerializationPtr>(kCreateNodeArgsPropNodeSerialization, serialization);
    args.setProperty<int>(kCreateNodeArgsPropPluginVersion, serialization->pluginMajorVersion, 0);
    args.setProperty<int>(kCreateNodeArgsPropPluginVersion, serialization->pluginMinorVersion, 1);
    args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
    args.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, false);
    args.setProperty<bool>(kCreateNodeArgsPropSettingsOpened, false);
    args.setProperty<bool>(kCreateNodeArgsPropAllowNonUserCreatablePlugins, true);

    return app->createNode(args);
}

// Connects input inputLabel of downstream to upstream, as the loader does: by label.
inline bool
connectPort(const NodePtr& upstream,
            const NodePtr& downstream,
            const std::string& inputLabel)
{
    const int index = downstream->getInputNumberFromLabel(inputLabel);

    return (index >= 0) && downstream->connectInput(upstream, index);
}

inline NodePtr
portInput(const NodePtr& downstream,
          const std::string& inputLabel)
{
    const int index = downstream->getInputNumberFromLabel(inputLabel);

    return (index >= 0) ? downstream->getInput(index) : NodePtr();
}

// Input label to upstream script name for every connected input of node.
inline std::map<std::string, std::string>
portConnections(const NodePtr& node)
{
    std::map<std::string, std::string> ret;
    PortTestNode* effect = portTestEffect(node);

    if (!effect) {
        return ret;
    }
    for (const std::string& port : effect->getPorts()) {
        NodePtr input = portInput(node, port);
        if (input) {
            ret[port] = input->getScriptName();
        }
    }

    return ret;
}

inline bool
isOutputOf(const NodePtr& upstream,
           const NodePtr& downstream)
{
    for (const NodeWPtr& output : upstream->getOutputs()) {
        if (output.lock() == downstream) {
            return true;
        }
    }

    return false;
}

NATRON_NAMESPACE_EXIT

#endif // Tests_Scene_ShaderNodeTestUtils_h
