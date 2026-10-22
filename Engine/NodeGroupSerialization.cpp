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

#include "NodeGroup.h"

#include <cassert>
#include <list>
#include <map>
#include <stdexcept>
#include <string>

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/RotoLayer.h"
#include "Engine/Settings.h"
#include "Engine/ViewerInstance.h"

#include "Serialization/NodeSerialization.h"

#include <SequenceParsing.h> // for removePath

NATRON_NAMESPACE_ENTER

NATRON_NAMESPACE_ANONYMOUS_ENTER

// Connects node's inputs to the siblings serialization names. An input that cannot be connected
// is left disconnected; when report is set, that is a load warning naming the input and target.
void
connectSerializedInputs(const NodeCollectionPtr& group,
                        const NodePtr& node,
                        const Serialization::NodeSerialization& serialization,
                        bool report)
{
    ProjectPtr project = node->getApp() ? node->getApp()->getProject() : ProjectPtr();
    const QString nodeName = QString::fromUtf8(serialization.scriptName.c_str());

    for (std::map<std::string, std::string>::const_iterator it = serialization.inputs.begin(); it != serialization.inputs.end(); ++it) {
        if (it->second.empty()) {
            continue;
        }
        const QString inputLabel = QString::fromUtf8(it->first.c_str());
        const QString targetName = QString::fromUtf8(it->second.c_str());
        QString problem;
        const int index = node->getInputNumberFromLabel(it->first);
        if (index == -1) {
            problem = NodeCollection::tr("the node has no such input; left disconnected");
        } else if (!group->getNodeByName(it->second)) {
            problem = NodeCollection::tr("node not found; input left disconnected");
        } else if (!group->connectNodes(index, it->second, node)) {
            problem = NodeCollection::tr("the connection was refused; input left disconnected");
        } else {
            continue;
        }
        if (!report) {
            qDebug() << nodeName << inputLabel << targetName << problem;
            continue;
        }
        const QString warning = NodeCollection::tr("%1.%2 \u2192 %3: %4").arg(nodeName).arg(inputLabel).arg(targetName).arg(problem);
        if (project) {
            project->addLoadWarning(warning);
        } else {
            appPTR->writeToErrorLog_mt_safe(NodeCollection::tr("Project"), QDateTime::currentDateTime(), warning);
        }
    }
}

NATRON_NAMESPACE_ANONYMOUS_EXIT

static QString lookForFileRecursively(const QString& dirPath, const QString& filenameUnPathed)
{
    QDir d(dirPath);
    if (!d.exists()) {
        return QString();
    }
    if (d.exists(filenameUnPathed)) {
        return d.absoluteFilePath(filenameUnPathed);
    }
    QStringList subDirs = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (QStringList::const_iterator it = subDirs.begin(); it != subDirs.end(); ++it) {
        QString ret = lookForFileRecursively(d.absoluteFilePath(*it), filenameUnPathed);
        if (!ret.isEmpty()) {
            return ret;
        }
    }
    return QString();

}

bool
NodeCollection::restoreFromSerialization(const std::list<Serialization::NodeSerializationPtr>& serializedNodes,
                                         const NodeCollectionPtr& group,
                                         bool createNodes,
                                         std::map<std::string, bool>* moduleUpdatesProcessed)
{
    bool mustShowErrorsLog = false;
    NodeGroup* isNodeGroup = dynamic_cast<NodeGroup*>( group.get() );
    QString groupName;

    if (isNodeGroup) {
        groupName = QString::fromUtf8( isNodeGroup->getNode()->getLabel().c_str() );
    } else {
        groupName = tr("top-level");
    }
    AppInstancePtr appInst = group->getApplication();
    if (!appInst) {
        return !mustShowErrorsLog;
    }
    appInst->updateProjectLoadStatus( tr("Creating nodes in group: %1").arg(groupName) );

    ///If a parent of a multi-instance node doesn't exist anymore but the children do, we must recreate the parent.
    ///Problem: we have lost the nodes connections. To do so we restore them using the serialization of a child.
    ///This map contains all the parents that must be reconnected and an iterator to the child serialization
    std::map<NodePtr, std::list<Serialization::NodeSerializationPtr>::const_iterator> parentsToReconnect;
    std::list<Serialization::NodeSerializationPtr> multiInstancesToRecurse;
    std::map<NodePtr, Serialization::NodeSerializationPtr> createdNodes;
    for (std::list<Serialization::NodeSerializationPtr>::const_iterator it = serializedNodes.begin(); it != serializedNodes.end(); ++it) {
        std::string pluginID = (*it)->pluginID;

        if ( appPTR->isBackground() && ( (pluginID == PLUGINID_NATRON_VIEWER) || (pluginID == "Viewer") ) ) {
            //if the node is a viewer, don't try to load it in background mode
            continue;
        }

        ///If the node is a multiinstance child find in all the serialized nodes if the parent exists.
        ///If not, create it

        if (!(*it)->multiInstanceParentName.empty()) {
            bool foundParent = false;
            for (std::list<Serialization::NodeSerializationPtr>::const_iterator it2 = serializedNodes.begin();
                 it2 != serializedNodes.end(); ++it2) {
                if ((*it2)->scriptName == (*it)->multiInstanceParentName) {
                    foundParent = true;
                    break;
                }
            }
            if (!foundParent) {
                ///Maybe it was created so far by another child who created it so look into the nodes

                NodePtr parent = group->getNodeByName((*it)->multiInstanceParentName);
                if (parent) {
                    foundParent = true;
                }
                ///Create the parent
                if (!foundParent) {
                    CreateNodeArgs args(pluginID, group);
                    args.setProperty<bool>(kCreateNodeArgsPropSilent, true);
                    args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
                    args.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, false);
                    args.setProperty<bool>(kCreateNodeArgsPropAllowNonUserCreatablePlugins, true);
                    
                    NodePtr parent = appInst->createNode(args);
                    try {
                        parent->setScriptName((*it)->multiInstanceParentName.c_str());
                    } catch (...) {
                    }

                    parentsToReconnect.insert( std::make_pair(parent, it) );
                }
            }
        } // if ( !(*it)->multiInstanceParentName.empty() ) {

        const std::string& pythonModuleAbsolutePath = (*it)->pythonModule;
        NodePtr n;
        bool usingPythonModule = false;
        bool pythonModuleIsMissing = false;
        if ( !pythonModuleAbsolutePath.empty() ) {
            unsigned int savedPythonModuleVersion = (*it)->pythonModuleVersion;
            QString qPyModulePath = QString::fromUtf8( pythonModuleAbsolutePath.c_str() );
            //Workaround a bug introduced in Natron where we were not saving the .py extension
            if ( !qPyModulePath.endsWith( QString::fromUtf8(".py") ) ) {
                qPyModulePath.append( QString::fromUtf8(".py") );
            }
            // The path that has been saved in the project might not be corresponding on this computer.
            // First try with the saved path
            // Then search through all PyPlug search paths recursively for a match
            if (!QFile::exists(qPyModulePath)) {
                std::string pythonModuleUnPathed = qPyModulePath.toStdString();
                std::string s = SequenceParsing::removePath(pythonModuleUnPathed);
                Q_UNUSED(s);

                qPyModulePath.clear();
                QStringList natronPaths = appPTR->getAllNonOFXPluginsPaths();
                for (int i = 0; i < natronPaths.size(); ++i) {
                    qPyModulePath = lookForFileRecursively(natronPaths[i], QString::fromUtf8(pythonModuleUnPathed.c_str()));
                    if (!qPyModulePath.isEmpty()) {
                        break;
                    }
                }
            }


            //This is a python group plug-in, try to find the corresponding .py file, maybe a more recent version of the plug-in exists.
            QFileInfo pythonModuleInfo(qPyModulePath);
            if (!pythonModuleInfo.exists()) {
                pythonModuleIsMissing = true;
            } else if (!appPTR->getCurrentSettings()->isLoadFromPyPlugsEnabled()) {
                pluginID = PLUGINID_NATRON_GROUP;
                usingPythonModule = false;
            } else {
                std::string pythonPluginID, pythonPluginLabel, pythonIcFilePath, pythonGrouping, pythonDesc;
                unsigned int pyVersion;
                QString pythonModuleName = pythonModuleInfo.fileName();
                if ( pythonModuleName.endsWith( QString::fromUtf8(".py") ) ) {
                    pythonModuleName = pythonModuleName.remove(pythonModuleName.size() - 3, 3);
                }

                std::string stdModuleName = pythonModuleName.toStdString();
                bool istoolset;
                if ( NATRON_PYTHON_NAMESPACE::getGroupInfos(pythonModuleInfo.path().toStdString() + '/', stdModuleName, &pythonPluginID, &pythonPluginLabel, &pythonIcFilePath, &pythonGrouping, &pythonDesc, &istoolset, &pyVersion) ) {
                    if (pyVersion != savedPythonModuleVersion) {
                        std::map<std::string, bool>::iterator found = moduleUpdatesProcessed->find(stdModuleName);
                        if ( found != moduleUpdatesProcessed->end() ) {
                            if (found->second) {
                                pluginID = pythonPluginID;
                                usingPythonModule = true;
                            }
                        } else {
                            StandardButtonEnum rep = Dialogs::questionDialog( ( (pyVersion > savedPythonModuleVersion) ? tr("New PyPlug version") : tr("Old PyPlug version") ).toStdString(),
                                                                              ( tr("Version %1 of PyPlug \"%2\" was found in the PyPlug search path.").arg(pyVersion).arg( QString::fromUtf8( stdModuleName.c_str() ) ).toStdString() + '\n' +
                                                                                tr("This file is using version %1.").arg(savedPythonModuleVersion).toStdString() + '\n' +
                                                                               ( (pyVersion > savedPythonModuleVersion) ? tr("Would you like to update this file to use the newer version?") : tr("Would you like to update this file to use the older version?") ).toStdString() + '\n' +
                                                                               tr("If you click 'No', the PyPlyg found in this file will be used instead.").toStdString() ),
                                                                              false,
                                                                              StandardButtons(eStandardButtonYes | eStandardButtonNo) );
                            if (rep == eStandardButtonYes) {
                                pluginID = pythonPluginID;
                                usingPythonModule = true;
                            } else {
                                pluginID = PLUGINID_NATRON_GROUP;
                                usingPythonModule = false;
                            }
                            moduleUpdatesProcessed->insert( std::make_pair(stdModuleName, rep == eStandardButtonYes) );
                        }
                    } else {
                        pluginID = pythonPluginID;
                        usingPythonModule = true;
                    }
                }
            }
        } // if (!pythonModuleAbsolutePath.empty()) {

        if (!createNodes) {
            ///We are in the case where we loaded a PyPlug: it probably created all the nodes in the group already but didn't
            ///load their serialization
            n = group->getNodeByName((*it)->scriptName);
        }

        int majorVersion, minorVersion;
        if (usingPythonModule) {
            //We already asked the user whether he/she wanted to load a newer version of the PyPlug, let the loadNode function accept it
            majorVersion = -1;
            minorVersion = -1;
        } else {
            majorVersion = (*it)->pluginMajorVersion;
            minorVersion = (*it)->pluginMinorVersion;
        }

        bool isPlaceholder = false;
        if (!n) {
            CreateNodeArgs args(pluginID, group);
            args.setProperty<int>(kCreateNodeArgsPropPluginVersion, majorVersion, 0);
            args.setProperty<int>(kCreateNodeArgsPropPluginVersion, minorVersion, 1);
            args.setProperty<Serialization::NodeSerializationPtr>(kCreateNodeArgsPropNodeSerialization, *it);
            args.setProperty<bool>(kCreateNodeArgsPropSilent, true);
            args.setProperty<bool>(kCreateNodeArgsPropAllowNonUserCreatablePlugins, true); // also load deprecated plugins
            if (!(*it)->multiInstanceParentName.empty()) {
                args.setProperty<std::string>(kCreateNodeArgsPropMultiInstanceParentName, (*it)->multiInstanceParentName);
            }
            args.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, false);

            if (pythonModuleIsMissing) {
                appPTR->writeToErrorLog_mt_safe(tr("Project"), QDateTime::currentDateTime(),
                                                tr("ERROR: The node %1 uses the PyPlug %2 (%3), which is not installed. "
                                                   "It is kept as a placeholder and saved unchanged.")
                                                    .arg(QString::fromUtf8((*it)->scriptName.c_str()))
                                                    .arg(QString::fromUtf8(pluginID.c_str()))
                                                    .arg(QString::fromUtf8(pythonModuleAbsolutePath.c_str())));
                n = appInst->createMissingPluginPlaceholder(args);
                isPlaceholder = bool(n);
            } else {
                n = appInst->createNodeOrMissingPluginPlaceholder(args, &isPlaceholder);
            }
            if (isPlaceholder) {
                mustShowErrorsLog = true;
            }
        }
        if (!n) {
            appPTR->writeToErrorLog_mt_safe(tr("Project"), QDateTime::currentDateTime(),
                                            tr("ERROR: The node %1 (%2) could not be created.")
                                                .arg(QString::fromUtf8((*it)->scriptName.c_str()))
                                                .arg(QString::fromUtf8(pluginID.c_str())));
            mustShowErrorsLog = true;
            continue;
        } else {
            if (!usingPythonModule && n->getPlugin() && (n->getPlugin()->getMajorVersion() != (int)majorVersion) && (n->getPluginID() == pluginID)) {
                QString text(tr("WARNING: The node %1 (%2) version %3.%4 "
                                "was found in the script but was loaded "
                                "with version %5.%6 instead.")
                                 .arg(QString::fromUtf8((*it)->scriptName.c_str()))
                                 .arg(QString::fromUtf8(pluginID.c_str()))
                                 .arg(majorVersion)
                                 .arg(minorVersion)
                                 .arg(n->getPlugin()->getMajorVersion())
                                 .arg(n->getPlugin()->getMinorVersion()));
                appPTR->writeToErrorLog_mt_safe(tr("Project"), QDateTime::currentDateTime(), text);
                mustShowErrorsLog = true;
            }
        }
        if (!createNodes && n) {
            // If we created the node using a PyPlug, deserialize the project too to override any modification made by the user.
            n->fromSerialization(**it);
        }
        assert(n);

        createdNodes[n] = *it;

        const std::list<Serialization::NodeSerializationPtr>& children = (*it)->children;
        // A placeholder keeps its children inside its stored serialization only.
        if (!children.empty() && !usingPythonModule && !isPlaceholder) {
            NodeGroup* isGrp = n->isEffectGroup();
            if (isGrp) {
                EffectInstancePtr sharedEffect = isGrp->shared_from_this();
                NodeGroupPtr sharedGrp = std::dynamic_pointer_cast<NodeGroup>(sharedEffect);
                NodeCollection::restoreFromSerialization(children, sharedGrp, !usingPythonModule, moduleUpdatesProcessed);
            } else {
                ///For multi-instances, wait for the group to be entirely created then load the sub-tracks in a separate loop.
                assert( n->isMultiInstance() );
                multiInstancesToRecurse.push_back(*it);
            }
        }
    } // for (std::list<Serialization::NodeSerializationPtr>::const_iterator it = serializedNodes.begin(); it != serializedNodes.end(); ++it) {

    for (std::list<Serialization::NodeSerializationPtr>::const_iterator it = multiInstancesToRecurse.begin(); it != multiInstancesToRecurse.end(); ++it) {
        NodeCollection::restoreFromSerialization((*it)->children, group, true, moduleUpdatesProcessed);
    }

    appInst->updateProjectLoadStatus( tr("Restoring graph links in group: %1").arg(groupName) );


    /// Connect the nodes together
    for (std::map<NodePtr, Serialization::NodeSerializationPtr>::const_iterator it = createdNodes.begin(); it != createdNodes.end(); ++it) {
        if ( appPTR->isBackground() && ( it->first->isEffectViewer() ) ) {
            //ignore viewers on background mode
            continue;
        }

        ///for all nodes that are part of a multi-instance, fetch the main instance node pointer
        const std::string& parentName = it->second->multiInstanceParentName;
        if ( !parentName.empty() ) {
            it->first->fetchParentMultiInstancePointer();
            //Do not restore connections as we just use the ones of the parent anyway
            continue;
        }

        ///restore slave/master link if any
        const std::string& masterNodeName = it->second->masterNodeName;
        if ( !masterNodeName.empty() ) {
            ///find such a node
            NodePtr masterNode = it->first->getApp()->getNodeByFullySpecifiedName(masterNodeName);

            if (!masterNode) {
                appInst->getProject()->addLoadWarning(tr("%1 \u2192 %2: node not found; its parameters were left unlinked")
                                                          .arg(QString::fromUtf8(it->second->scriptName.c_str()))
                                                          .arg(QString::fromUtf8(masterNodeName.c_str())));
            } else {
                it->first->getEffectInstance()->slaveAllKnobs( masterNode->getEffectInstance().get(), true );
            }
        }

        if ( !parentName.empty() ) {
            //The parent will have connection mades for its children
            continue;
        }

        connectSerializedInputs(group, it->first, *it->second, createNodes);
    } // for (std::list<Serialization::NodeSerializationPtr>::const_iterator it = serializedNodes.begin(); it != serializedNodes.end(); ++it) {

    ///Now that the graph is setup, restore expressions
    {
        std::map<std::string, std::string> oldNewScriptNamesMapping;
        for (std::map<NodePtr, Serialization::NodeSerializationPtr>::const_iterator it = createdNodes.begin(); it != createdNodes.end(); ++it) {
            if ( appPTR->isBackground() && ( it->first->isEffectViewer() ) ) {
                //ignore viewers on background mode
                continue;
            }
            it->first->storeKnobsLinks(*it->second, oldNewScriptNamesMapping);
            // restoreKnobsLinks() is called on all nodes at the end of ProjectPrivate::restoreFromSerialization()
        }
    }

    ///Also reconnect parents of multiinstance nodes that were created on the fly
    for (std::map<NodePtr, std::list<Serialization::NodeSerializationPtr>::const_iterator>::const_iterator
             it
         = parentsToReconnect.begin();
         it != parentsToReconnect.end(); ++it) {
        connectSerializedInputs(group, it->first, **it->second, false);
    }

    return !mustShowErrorsLog;
} // NodeCollection::restoreFromSerialization

NATRON_NAMESPACE_EXIT
