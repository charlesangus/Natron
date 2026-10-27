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

#include "NodeGraphPrivate.h"
#include "NodeGraph.h"

#include <algorithm>
#include <limits>
#include <sstream> // stringstream
#include <stdexcept>

#include <QDebug>

#include "Engine/CreateNodeArgs.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/MissingPluginNode.h"
#include "Engine/Project.h"
#include "Engine/RotoLayer.h"

#include "Gui/BackdropGui.h"
#include "Gui/DotGui.h"
#include "Gui/Edge.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/GuiApplicationManager.h" // appPTR
#include "Gui/NodeClipBoard.h"
#include "Gui/NodeGui.h"

#include "Serialization/NodeSerialization.h"

NATRON_NAMESPACE_ENTER


void
NodeGraphPrivate::pasteNodesInternal(const NodeClipBoard & clipboard,
                                     const QPointF& scenePos,
                                     bool useUndoCommand,
                                     std::list<std::pair<std::string, NodeGuiPtr> > *newNodes)
{
    if ( !clipboard.isEmpty() ) {
        double xmax = std::numeric_limits<double>::lowest();
        double xmin = std::numeric_limits<double>::max();
        double ymin = std::numeric_limits<double>::max();
        double ymax = std::numeric_limits<double>::lowest();
        bool anyPositioned = false;

        // A node with no stored size counts as a point at its position.
        for (std::list<Serialization::NodeSerializationPtr>::const_iterator it = clipboard.nodes.begin();
             it != clipboard.nodes.end(); ++it) {
            const Serialization::NodeLayoutSerialization& layout = (*it)->layout;
            if (!layout.hasPosition) {
                continue;
            }
            anyPositioned = true;
            const double w = layout.hasSize ? layout.width : 0.;
            const double h = layout.hasSize ? layout.height : 0.;
            xmin = std::min(xmin, layout.x);
            ymin = std::min(ymin, layout.y);
            xmax = std::max(xmax, layout.x + w);
            ymax = std::max(ymax, layout.y + h);
        }

        QPointF offset(0., 0.);
        if (anyPositioned) {
            offset = QPointF(scenePos.x() - ((xmin + xmax) / 2.), scenePos.y() - ((ymin + ymax) / 2.));
        }

        NodesGuiList newNodesList;
        std::list<std::pair<Serialization::NodeSerializationPtr, NodePtr>> newNodesMap;

        std::set<std::string> missingNodePlugins;

        ///The script-name of the copy node is different than the one of the original one
        ///We store the mapping so we can restore node links correctly
        std::map<std::string, std::string> oldNewScriptNamesMapping;
        {
            CreatingNodeTreeFlag_RAII createNodeTree( _publicInterface->getGui()->getApp() );
            const std::list<Serialization::NodeSerializationPtr>& internalNodesClipBoard = clipboard.nodes;
            std::list<Serialization::NodeSerializationPtr>::const_iterator itOther = internalNodesClipBoard.begin();
            std::list<NodePtr>::const_iterator itSource = clipboard.sourceNodes.begin();
            for (; itOther != internalNodesClipBoard.end(); ++itOther) {
                NodePtr sourceNode;
                if (itSource != clipboard.sourceNodes.end()) {
                    sourceNode = *itSource;
                    ++itSource;
                }
                const std::string& oldScriptName = (*itOther)->scriptName;
                NodeGuiPtr node = pasteNode(*itOther, sourceNode, offset, group.lock(), std::string(), 0, &oldNewScriptNamesMapping);

                if (!node) {
                    missingNodePlugins.insert((*itOther)->pluginID);
                    continue;
                }
                newNodes->push_back( std::make_pair(oldScriptName, node) );
                newNodesList.push_back(node);
                newNodesMap.push_back( std::make_pair( *itOther, node->getNode() ) );

                const std::string& newScriptName = node->getNode()->getScriptName();
                oldNewScriptNamesMapping[oldScriptName] = newScriptName;
            }
            if (internalNodesClipBoard.size() != newNodes->size()) {
                std::ostringstream message;
                if (!missingNodePlugins.empty()) {
                    message << QObject::tr("Following plugins are missing:").toStdString() << std::endl << std::endl << "<ul>";
                    for (std::set<std::string>::const_iterator it = missingNodePlugins.begin(); it != missingNodePlugins.end(); ++it) {
                        message << "<li>" << *it << "</li>";
                    }
                    message << "</ul>";
                } else {
                    message << QObject::tr("Unknown error").toStdString() << std::endl;
                }
                _publicInterface->getGui()->informationDialog(QObject::tr("Paste error").toStdString(), message.str(), true);
                return;
            }

            ///Now that all nodes have been duplicated, try to restore nodes connections
            restoreConnections(internalNodesClipBoard, *newNodes, oldNewScriptNamesMapping);

            NodesList allNodes;
            _publicInterface->getGui()->getApp()->getProject()->getActiveNodesExpandGroups(&allNodes);

            //Restore links once all children are created for alias knobs/expressions
            NodesList pastedNodes;
            for (std::list<std::pair<Serialization::NodeSerializationPtr, NodePtr>>::iterator it = newNodesMap.begin(); it != newNodesMap.end(); ++it) {
                it->second->storeKnobsLinks(*(it->first), oldNewScriptNamesMapping);
                it->second->restoreKnobsLinks(allNodes, oldNewScriptNamesMapping, false); // may fail
                pastedNodes.push_back(it->second);
            }
            // In the project they were copied from, the pasted copies of linked knobs join the groups of the
            // knobs they were copied from, or link to each other when those are gone. In another project a
            // node of the same name is another node, so there they link only to each other.
            const bool sameProject = _publicInterface->getGui()->getApp()->getProject()->isProjectId(clipboard.sourceProject);
            KnobHelper::restoreLinkGroups(pastedNodes, sameProject);
        }

        if (newNodesList.size() > 1) {
            ///Only compute datas if we're pasting more than 1 node
            _publicInterface->getGui()->getApp()->getProject()->forceComputeInputDependentDataOnAllTrees();
        } else if (newNodesList.size() == 1) {
            newNodesList.front()->getNode()->getEffectInstance()->refreshMetadataSideEffects();
        }

        // Now that meta-datas are refreshed, compute preview
        for (NodesGuiList::const_iterator it = newNodesList.begin(); it!=newNodesList.end(); ++it) {
            (*it)->forceComputePreview(_publicInterface->getGui()->getApp()->getTimeLine()->currentFrame());
        }

        if (useUndoCommand) {
            _publicInterface->pushUndoCommand(new AddMultipleNodesCommand(_publicInterface, newNodesList), newNodesList);
        }
    }
} // pasteNodesInternal

// Trim underscore followed by digits at the end of baseName (see #732).
static
void trimNumber(std::string &baseName)
{
    std::size_t found_underscore = baseName.rfind('_');
    if (found_underscore != std::string::npos && found_underscore != (baseName.size() - 1)) {
        std::size_t found_nondigit = baseName.find_last_not_of("0123456789");
        if (found_nondigit == found_underscore) {
            baseName.erase(found_underscore);
        }
    }
}

NodeGuiPtr
NodeGraphPrivate::pasteNode(const Serialization::NodeSerializationPtr& internalSerialization,
                            const NodePtr& sourceNode,
                            const QPointF& offset,
                            const NodeCollectionPtr& grp,
                            const std::string& parentName,
                            std::list<std::pair<NodePtr, NodePtr>>* linkedNodes,
                            std::map<std::string, std::string>* oldNewScriptNamesMapping)
{
    CreateNodeArgs args(internalSerialization->pluginID, grp);
    args.setProperty<Serialization::NodeSerializationPtr>(kCreateNodeArgsPropNodeSerialization, internalSerialization);
    if (!parentName.empty()) {
        args.setProperty<std::string>(kCreateNodeArgsPropMultiInstanceParentName, parentName);
    }
    args.setProperty<int>(kCreateNodeArgsPropPluginVersion, internalSerialization->pluginMajorVersion, 0);
    args.setProperty<int>(kCreateNodeArgsPropPluginVersion, internalSerialization->pluginMinorVersion, 1);
    args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
    args.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, false);
    args.setProperty<bool>(kCreateNodeArgsPropSettingsOpened, false);
    args.setProperty<bool>(kCreateNodeArgsPropAllowNonUserCreatablePlugins, true);

    bool isPlaceholder = false;
    NodePtr n = _publicInterface->getGui()->getApp()->createNodeOrMissingPluginPlaceholder(args, &isPlaceholder);

    if (!n) {
        return NodeGuiPtr();
    }
    NodeGuiIPtr gui_i = n->getNodeGui();
    NodeGuiPtr gui = std::dynamic_pointer_cast<NodeGui>(gui_i);
    assert(gui);

    if ((grp == group.lock()) && (!sourceNode || (sourceNode->getGroup() == group.lock()))) {
        // We pasted the node in the same group, give it another label.
        // If the label is available, use it as is.
        // Else look for a similar name by adding a (different) number suffix.
        std::string name = internalSerialization->label;
        if ( grp->checkIfNodeLabelExists( name, n.get() ) ) {
            std::string baseName = name;
            trimNumber(baseName);
            int no = 1;
            do {
                name = baseName;
                if (no > 1) {
                    name += '_';
                    std::stringstream ss;
                    ss << no;
                    name += ss.str();
                }
                ++no;
            } while ( grp->checkIfNodeLabelExists( name, n.get() ) );
        }
        n->setLabel(name);
    } else {
        //If we paste the node in a different graph, it can keep its scriptname/label
    }

    //All nodes that are reachable via expressions
    NodesList allNodes;
    _publicInterface->getGui()->getApp()->getProject()->getActiveNodesExpandGroups(&allNodes);

    if (internalSerialization->layout.hasPosition && !offset.isNull()) {
        double x, y;
        n->getPosition(&x, &y);
        n->setPosition(x + offset.x(), y + offset.y());
    }

    if (linkedNodes && sourceNode) {
        DotGui* isDot = dynamic_cast<DotGui*>( gui.get() );
        /// Dots cannot be linked, just copy them
        if (!isDot) {
            linkedNodes->push_back(std::make_pair(n, sourceNode));
        }
    }

    ///Recurse if this is a group or multi-instance
    NodeGroupPtr isGrp =
        std::dynamic_pointer_cast<NodeGroup>( n->getEffectInstance()->shared_from_this() );
    const std::list<Serialization::NodeSerializationPtr>& nodes = internalSerialization->children;

    if (internalSerialization->scriptName != n->getScriptName()) {
        (*oldNewScriptNamesMapping)[internalSerialization->scriptName] = n->getScriptName();
    }

    // For PyPlugs, don't recurse otherwise we would recreate all nodes on top of the ones created by the python script
    if (!nodes.empty() && n->getPluginPythonModule().empty() && !isPlaceholder) {
        std::string parentName;
        NodeCollectionPtr collection;
        if (isGrp) {
            collection = isGrp;
        } else {
            assert( n->isMultiInstance() );
            collection = n->getGroup();
            parentName = n->getScriptName_mt_safe();
        }
        // The nodes of a group serialization are named within the source group, the instances
        // of a multi-instance serialization within the collection holding the source node.
        NodeCollectionPtr sourceCollection;
        if (sourceNode) {
            NodeGroupPtr sourceGroup = std::dynamic_pointer_cast<NodeGroup>(sourceNode->getEffectInstance());
            if (sourceGroup) {
                sourceCollection = sourceGroup;
            } else {
                sourceCollection = sourceNode->getGroup();
            }
        }
        std::list<std::pair<Serialization::NodeSerializationPtr, NodePtr>> newNodesMap;
        std::list<std::pair<std::string, NodeGuiPtr>> newNodes;
        for (std::list<Serialization::NodeSerializationPtr>::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
            NodePtr childSourceNode;
            if (sourceCollection) {
                childSourceNode = sourceCollection->getNodeByName((*it)->scriptName);
            }
            NodeGuiPtr newChild = pasteNode(*it, childSourceNode, QPointF(0, 0), collection, parentName, linkedNodes, oldNewScriptNamesMapping);
            if (newChild) {
                newNodes.push_back(std::make_pair((*it)->scriptName, newChild));
                if ((*it)->scriptName != newChild->getNode()->getScriptName()) {
                    (*oldNewScriptNamesMapping)[(*it)->scriptName] = newChild->getNode()->getScriptName();
                }
                allNodes.push_back( newChild->getNode() );
                newNodesMap.push_back(std::make_pair(*it,newChild->getNode()));
            }
        }
        restoreConnections(nodes, newNodes, *oldNewScriptNamesMapping);

        //Restore links once all children are created for alias knobs/expressions
        for (std::list<std::pair<Serialization::NodeSerializationPtr, NodePtr>>::iterator it = newNodesMap.begin(); it != newNodesMap.end(); ++it) {
            it->second->storeKnobsLinks(*(it->first), *oldNewScriptNamesMapping);
            it->second->restoreKnobsLinks(allNodes, *oldNewScriptNamesMapping, false); // may fail
        }
    }

    return gui;
} // NodeGraphPrivate::pasteNode

void
NodeGraphPrivate::restoreConnections(const std::list<Serialization::NodeSerializationPtr>& serializations,
                                     const std::list<std::pair<std::string, NodeGuiPtr>>& newNodes,
                                     const std::map<std::string, std::string>& oldNewScriptNamesMap)
{
    ///For all nodes restore its connections
    std::list<Serialization::NodeSerializationPtr>::const_iterator itSer = serializations.begin();

    assert( serializations.size() == newNodes.size() );
    for (std::list<std::pair<std::string, NodeGuiPtr> >::const_iterator it = newNodes.begin();
         it != newNodes.end(); ++it, ++itSer) {
        const std::map<std::string, std::string>& inputNames = (*itSer)->inputs;
        ///Restore each input
        for (std::map<std::string, std::string>::const_iterator it2 = inputNames.begin(); it2 != inputNames.end(); ++it2) {
            std::string inputScriptName = it2->second;
            std::map<std::string, std::string>::const_iterator foundMapping = oldNewScriptNamesMap.find(it2->second);
            if ( foundMapping != oldNewScriptNamesMap.end() ) {
                inputScriptName = foundMapping->second;
            }

            if ( inputScriptName.empty() ) {
                continue;
            }

            int index = it->second->getNode()->getInputNumberFromLabel(it2->first);
            if (index == -1) {
                qDebug() << "Could not find an input named " << it2->first.c_str();
                continue;
            }


            ///find a node  containing the same name. It should not match exactly because there's already
            /// the "-copy" that was added to its name
            for (std::list<std::pair<std::string, NodeGuiPtr> >::const_iterator it3 = newNodes.begin();
                 it3 != newNodes.end(); ++it3) {
                if (it3->second->getNode()->getScriptName() == inputScriptName) {
                    NodeCollection::connectNodes( index, it3->second->getNode(), it->second->getNode() );
                    break;
                }
            }
        }
    }
}

void
NodeGraphPrivate::getNodeSet(const NodesGuiList& nodeList, std::set<NodeGuiPtr>& nodeSet)
{
    for (NodesGuiList::const_iterator it = nodeList.begin(); it != nodeList.end(); ++it) {
        if (nodeSet.find(*it) == nodeSet.end()) {
            nodeSet.insert(*it);
            BackdropGui* isBd = dynamic_cast<BackdropGui*>( it->get() );
            if (isBd) {
                NodesGuiList nodesWithin = _publicInterface->getNodesWithinBackdrop(*it);
                getNodeSet(nodesWithin, nodeSet);
            }
        }
    }
}

void
NodeGraphPrivate::toggleSelectedNodesEnabled()
{
    std::set<NodeGuiPtr> nodeSet;

    // first, put all selected nodes, including those within backdrop, in nodeSet
    getNodeSet(_selection, nodeSet);

    NodesGuiList disabledNodes;
    NodesGuiList allNodes;
    for (std::set<NodeGuiPtr>::const_iterator it = nodeSet.begin(); it != nodeSet.end(); ++it) {
        KnobBoolPtr k = (*it)->getNode()->getDisabledKnob();
        if (!k) {
            continue;
        }
        allNodes.push_back(*it);
        if ( k->getValue() ) {
            disabledNodes.push_back(*it);
        }
    }
    ///if some nodes are disabled , enable them before

    if ( disabledNodes.size() == allNodes.size() ) {
        _publicInterface->pushUndoCommand(new EnableNodesCommand(allNodes), allNodes);
    } else if (disabledNodes.size() > 0) {
        _publicInterface->pushUndoCommand(new EnableNodesCommand(disabledNodes), disabledNodes);
    } else {
        _publicInterface->pushUndoCommand(new DisableNodesCommand(allNodes), allNodes);
    }
}

NATRON_NAMESPACE_EXIT
