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

#include "NodeGraph.h"
#include "NodeGraphPrivate.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>

#include <QApplication>
#include <QClipboard>
#include <QDebug>
#include <QMimeData>
#include <QStatusBar>
#include <QStringList>
#include <QThread>

#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Project.h"
#include "Engine/RotoLayer.h"
#include "Engine/ViewerInstance.h"

#include "Gui/BackdropGui.h"
#include "Gui/CurveEditor.h"
#include "Gui/Edge.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/NodeClipBoard.h"
#include "Gui/NodeGraphConnect.h"
#include "Gui/NodeGraphSplice.h"
#include "Gui/NodeGui.h"

#include "Global/QtCompat.h"

#include "Serialization/NodeSerialization.h"
#include "Serialization/Yaml/ClipboardYaml.h"

NATRON_NAMESPACE_ENTER
void
NodeGraph::togglePreviewsForSelectedNodes()
{
    bool empty = false;
    {
        QMutexLocker l(&_imp->_nodesMutex);
        empty = _imp->_selection.empty();
        for (NodesGuiList::iterator it = _imp->_selection.begin();
             it != _imp->_selection.end();
             ++it) {
            (*it)->togglePreview();
        }
    }

    if (empty) {
        Dialogs::warningDialog( tr("Toggle Preview").toStdString(), tr("You must select a node first").toStdString() );
    }
}

void
NodeGraph::showSelectedNodeSettingsPanel()
{
    if (_imp->_selection.size() == 1) {
        showNodePanel(false, false, _imp->_selection.front().get());
    }
}

void
NodeGraph::switchInputs1and2ForSelectedNodes()
{
    QMutexLocker l(&_imp->_nodesMutex);

    for (NodesGuiList::iterator it = _imp->_selection.begin();
         it != _imp->_selection.end();
         ++it) {
        (*it)->onSwitchInputActionTriggered();
    }
}

void
NodeGraph::centerOnItem(QGraphicsItem* item)
{
    _imp->_refreshOverlays = true;
    centerOn(item);
}

void
NodeGraph::copyNodes(const NodesGuiList& nodes,
                     NodeClipBoard& clipboard)
{
    _imp->copyNodesInternal(nodes, clipboard);
}

// Puts the nodes of clipboard on the system clipboard as a clipboard document, so that another
// Natron or a text editor can take them.
static bool
putOnSystemClipboard(const NodeClipBoard& cb)
{
    std::string text;
    try {
        Serialization::ClipboardSerialization serialization;
        serialization.nodes = cb.nodes;
        text = Serialization::encodeClipboardYaml(serialization);
    } catch (const std::exception& e) {
        Dialogs::errorDialog(NodeGraph::tr("Copy").toStdString(), NodeGraph::tr("The selection cannot be copied: %1").arg(QString::fromUtf8(e.what())).toStdString());

        return false;
    }

    QMimeData* mimedata = new QMimeData;
    const QByteArray data = QByteArray::fromStdString(text);
    mimedata->setData(QLatin1String("text/plain"), data);
    mimedata->setData(QString::fromLatin1(Serialization::kClipboardYamlMimeType.data(), (int)Serialization::kClipboardYamlMimeType.size()), data);

    // ownership is transferred to the clipboard
    QApplication::clipboard()->setMimeData(mimedata);

    return true;
}

void
NodeGraph::copySelectedNodes()
{
    if ( _imp->_selection.empty() ) {
        Dialogs::warningDialog( tr("Copy").toStdString(), tr("You must select at least a node to copy first.").toStdString() );

        return;
    }

    NodeClipBoard& cb = appPTR->getNodeClipBoard();
    _imp->copyNodesInternal(_imp->_selection, cb);
    putOnSystemClipboard(cb);
}

void
NodeGraph::cutSelectedNodes()
{
    if ( _imp->_selection.empty() ) {
        Dialogs::warningDialog( tr("Cut").toStdString(), tr("You must select at least a node to cut first.").toStdString() );

        return;
    }
    NodeClipBoard& cb = appPTR->getNodeClipBoard();
    _imp->copyNodesInternal(_imp->_selection, cb);
    if (!putOnSystemClipboard(cb)) {
        return;
    }
    deleteSelection();
}

static NodeGuiPtr
findNodeGui(const NodesGuiList& nodes,
            SpliceNodeId id)
{
    for (NodesGuiList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        if (reinterpret_cast<SpliceNodeId>((*it)->getNode().get()) == id) {
            return *it;
        }
    }

    return NodeGuiPtr();
}

static bool
pasteOntoNode(NodeGraph* graph,
              const NodesGuiList& selection,
              const NodesGuiList& pasted)
{
    PasteSplicePlan plan = planPasteSpliceForSelection(selection, pasted);

    if (!plan.valid) {
        return false;
    }

    NodeGuiPtr targetGui = findNodeGui(selection, plan.targetId);
    NodeGuiPtr entryGui = findNodeGui(pasted, plan.splice.entryId);
    NodeGuiPtr exitGui = findNodeGui(pasted, plan.splice.exitId);
    if (!targetGui || !entryGui || !exitGui) {
        return false;
    }

    NodePtr targetNode = targetGui->getNode();
    NodePtr entryNode = entryGui->getNode();
    NodePtr exitNode = exitGui->getNode();

    std::map<NodePtr, int> targetOutputs;
    targetNode->getOutputsConnectedToThisNode(&targetOutputs);

    std::vector<NodePtr> watchedNodes;
    watchedNodes.push_back(entryNode);
    for (std::map<NodePtr, int>::const_iterator it = targetOutputs.begin(); it != targetOutputs.end(); ++it) {
        watchedNodes.push_back(it->first);
    }
    std::vector<std::vector<NodeWPtr>> inputsBefore;
    for (std::size_t i = 0; i < watchedNodes.size(); ++i) {
        inputsBefore.push_back(watchedNodes[i]->getGuiInputs());
    }

    // The outputs map holds one input per downstream node, so a node the target feeds through
    // several inputs, such as both A and B of a Merge, needs every one of them found here.
    std::map<NodePtr, std::vector<int>> targetUses;
    for (std::size_t i = 1; i < watchedNodes.size(); ++i) {
        const std::vector<NodeWPtr>& inputs = inputsBefore[i];
        for (std::size_t j = 0; j < inputs.size(); ++j) {
            if (inputs[j].lock() == targetNode) {
                targetUses[watchedNodes[i]].push_back((int)j);
            }
        }
    }

    const QPointF entryPosBefore = entryGui->getPos_mt_safe();
    graph->moveNodesForIdealPosition(entryGui, targetGui, true);
    const QPointF delta = entryGui->getPos_mt_safe() - entryPosBefore;
    for (NodesGuiList::const_iterator it = pasted.begin(); it != pasted.end(); ++it) {
        if (*it == entryGui) {
            continue;
        }
        const QPointF pos = (*it)->getPos_mt_safe();
        (*it)->setPosition(pos.x() + delta.x(), pos.y() + delta.y());
    }

    // Node creation re-routes one input of each of the target's outputs to the node it placed,
    // the entry, and leaves alone the outputs it chose not to splice; a node it re-routed has
    // every input that took the target fed from the exit, where the pasted nodes' result comes out.
    for (std::map<NodePtr, std::vector<int>>::const_iterator it = targetUses.begin(); it != targetUses.end(); ++it) {
        const std::vector<NodeWPtr> inputs = it->first->getGuiInputs();
        bool spliced = false;
        for (std::size_t k = 0; k < it->second.size(); ++k) {
            const int inputNb = it->second[k];
            if ((inputNb < (int)inputs.size()) && (inputs[inputNb].lock() == entryNode)) {
                spliced = true;
            }
        }
        if (!spliced) {
            continue;
        }
        for (std::size_t k = 0; k < it->second.size(); ++k) {
            const int inputNb = it->second[k];
            if (inputNb >= (int)inputs.size()) {
                continue;
            }
            const NodePtr input = inputs[inputNb].lock();
            if ((input == targetNode) || ((input == entryNode) && (entryNode != exitNode))) {
                ignore_result(it->first->replaceInput(exitNode, inputNb));
            }
        }
    }

    if (entryGui != exitGui) {
        QRectF pastedRect;
        for (NodesGuiList::const_iterator it = pasted.begin(); it != pasted.end(); ++it) {
            pastedRect |= (*it)->mapToScene((*it)->boundingRect()).boundingRect();
        }
        for (std::map<NodePtr, int>::const_iterator it = targetOutputs.begin(); it != targetOutputs.end(); ++it) {
            NodeGuiIPtr outputGuiI = it->first->getNodeGui();
            NodeGui* outputGui = dynamic_cast<NodeGui*>(outputGuiI.get());
            if (outputGui) {
                outputGui->moveBelowPositionRecursively(pastedRect);
            }
        }
    }

    std::vector<ReconnectInputsCommand::InputChange> changes;
    for (std::size_t i = 0; i < watchedNodes.size(); ++i) {
        const std::vector<NodeWPtr>& before = inputsBefore[i];
        const std::vector<NodeWPtr>& after = watchedNodes[i]->getGuiInputs();
        const std::size_t nInputs = std::max(before.size(), after.size());
        for (std::size_t j = 0; j < nInputs; ++j) {
            NodePtr beforeInput = (j < before.size()) ? before[j].lock() : NodePtr();
            NodePtr afterInput = (j < after.size()) ? after[j].lock() : NodePtr();
            if (beforeInput != afterInput) {
                ReconnectInputsCommand::InputChange change;
                change.node = watchedNodes[i];
                change.inputNb = (int)j;
                change.before = beforeInput;
                change.after = afterInput;
                changes.push_back(change);
            }
        }
    }

    QUndoCommand* pasteCommand = new QUndoCommand(NodeGraph::tr("Paste node(s)"));
    new AddMultipleNodesCommand(graph, pasted, pasteCommand);
    new ReconnectInputsCommand(graph, changes, pasteCommand);
    graph->pushUndoCommand(pasteCommand);

    return true;
} // pasteOntoNode

void
NodeGraph::pasteCliboard(const NodeClipBoard& clipboard,
                         std::list<std::pair<std::string, NodeGuiPtr> >* newNodes)
{
    QPointF position = _imp->_root->mapFromScene( mapToScene( mapFromGlobal( QCursor::pos() ) ) );

    _imp->pasteNodesInternal(clipboard, position, false, newNodes);
}

// Text that is not a Natron clipboard document, an old XML one included, is not pasted.
static bool
decodeClipboardNodes(const std::string& text,
                     NodeClipBoard* clipboard)
{
    Serialization::ClipboardSerialization serialization;
    try {
        serialization = Serialization::decodeClipboardYaml(text);
    } catch (const std::exception&) {
        return false;
    }
    if (serialization.nodes.empty()) {
        return false;
    }
    clipboard->nodes = serialization.nodes;
    clipboard->sourceNodes.assign(serialization.nodes.size(), NodePtr());

    return true;
}

bool
NodeGraph::pasteNodeClipBoards(const QPointF& pos)
{
    QClipboard* clipboard = QApplication::clipboard();
    const QMimeData* mimedata = clipboard->mimeData();

    const QString yamlMimeType = QString::fromLatin1(Serialization::kClipboardYamlMimeType.data(), (int)Serialization::kClipboardYamlMimeType.size());
    QByteArray data;
    if (mimedata->hasFormat(yamlMimeType)) {
        data = mimedata->data(yamlMimeType);
    } else if (mimedata->hasFormat(QLatin1String("text/plain"))) {
        data = mimedata->data(QLatin1String("text/plain"));
    } else {
        return false;
    }
    std::list<std::pair<std::string, NodeGuiPtr> > newNodes;
    NodeClipBoard& cb = appPTR->getNodeClipBoard();
    if (!decodeClipboardNodes(data.toStdString(), &cb)) {
        return false;
    }

    const NodesGuiList selectionBeforePaste = _imp->_selection;
    if (selectionBeforePaste.empty()) {
        _imp->pasteNodesInternal(cb, _imp->_root->mapFromScene(pos), true, &newNodes);

        return true;
    }

    _imp->pasteNodesInternal(cb, _imp->_root->mapFromScene(pos), false, &newNodes);
    if (newNodes.empty() || (newNodes.size() != cb.nodes.size())) {
        return true;
    }

    NodesGuiList pastedNodes;
    for (std::list<std::pair<std::string, NodeGuiPtr>>::const_iterator it = newNodes.begin(); it != newNodes.end(); ++it) {
        pastedNodes.push_back(it->second);
    }
    if (!pasteOntoNode(this, selectionBeforePaste, pastedNodes)) {
        pushUndoCommand(new AddMultipleNodesCommand(this, pastedNodes));
    }

    return true;
} // pasteNodeClipBoards

bool
NodeGraph::pasteNodeClipBoards()
{
    QPointF position = mapToScene( mapFromGlobal( QCursor::pos() ) );

    return pasteNodeClipBoards(position);
}

void
NodeGraph::duplicateSelectedNodes(const QPointF& pos)
{
    if ( _imp->_selection.empty() && _imp->_selection.empty() ) {
        Dialogs::warningDialog( tr("Duplicate").toStdString(), tr("You must select at least a node to duplicate first.").toStdString() );

        return;
    }

    ///Don't use the member clipboard as the user might have something copied
    NodeClipBoard tmpClipboard;
    _imp->copyNodesInternal(_imp->_selection, tmpClipboard);
    std::list<std::pair<std::string, NodeGuiPtr> > newNodes;
    _imp->pasteNodesInternal(tmpClipboard, _imp->_root->mapFromScene(pos), true, &newNodes);
}

void
NodeGraph::duplicateSelectedNodes()
{
    QPointF scenePos = mapToScene( mapFromGlobal( QCursor::pos() ) );

    duplicateSelectedNodes(scenePos);
}

void
NodeGraph::cloneSelectedNodes(const QPointF& scenePos)
{
    if ( _imp->_selection.empty() ) {
        Dialogs::warningDialog( tr("Clone").toStdString(), tr("You must select at least a node to clone first.").toStdString() );

        return;
    }

    double xmax = std::numeric_limits<int>::min();
    double xmin = std::numeric_limits<int>::max();
    double ymin = std::numeric_limits<int>::max();
    double ymax = std::numeric_limits<int>::min();
    NodesGuiList nodesToCopy = _imp->_selection;
    for (NodesGuiList::iterator it = _imp->_selection.begin(); it != _imp->_selection.end(); ++it) {
        if ( (*it)->getNode()->getMasterNode() ) {
            Dialogs::errorDialog( tr("Clone").toStdString(), tr("You cannot clone a node which is already a clone.").toStdString() );

            return;
        }
        QRectF bbox = (*it)->mapToScene( (*it)->boundingRect() ).boundingRect();
        if ( ( bbox.x() + bbox.width() ) > xmax ) {
            xmax = ( bbox.x() + bbox.width() );
        }
        if (bbox.x() < xmin) {
            xmin = bbox.x();
        }

        if ( ( bbox.y() + bbox.height() ) > ymax ) {
            ymax = ( bbox.y() + bbox.height() );
        }
        if (bbox.y() < ymin) {
            ymin = bbox.y();
        }

        ///Also copy all nodes within the backdrop
        BackdropGui* isBd = dynamic_cast<BackdropGui*>( it->get() );
        if (isBd) {
            NodesGuiList nodesWithinBD = getNodesWithinBackdrop(*it);
            for (NodesGuiList::iterator it2 = nodesWithinBD.begin(); it2 != nodesWithinBD.end(); ++it2) {
                NodesGuiList::iterator found = std::find(nodesToCopy.begin(), nodesToCopy.end(), *it2);
                if ( found == nodesToCopy.end() ) {
                    nodesToCopy.push_back(*it2);
                }
            }
        }
    }

    for (NodesGuiList::iterator it = nodesToCopy.begin(); it != nodesToCopy.end(); ++it) {
        if ( (*it)->getNode()->getEffectInstance()->isSlave() ) {
            Dialogs::errorDialog( tr("Clone").toStdString(), tr("You cannot clone a node which is already a clone.").toStdString() );

            return;
        }
        ViewerInstance* isViewer = (*it)->getNode()->isEffectViewer();
        if (isViewer) {
            Dialogs::errorDialog( tr("Clone").toStdString(), tr("Cloning a viewer is not a valid operation.").toStdString() );

            return;
        }
        // A missing-plugin placeholder is saved back unchanged, so a clone of it would lose its link on save.
        if ((*it)->getNode()->isMultiInstance() || !(*it)->getNode()->getEffectInstance()->getIsEditable()) {
            QString err = QString::fromUtf8("%1 cannot be cloned.").arg( QString::fromUtf8( (*it)->getNode()->getLabel().c_str() ) );
            Dialogs::errorDialog( tr("Clone").toStdString(),
                                  tr( err.toStdString().c_str() ).toStdString() );

            return;
        }
    }

    QPointF offset( scenePos.x() - ( (xmax + xmin) / 2. ), scenePos.y() -  ( (ymax + ymin) / 2. ) );
    std::list<std::pair<std::string, NodeGuiPtr> > newNodes;
    std::list<Serialization::NodeSerializationPtr> serializations;
    std::list<NodeGuiPtr> newNodesList;
    std::map<std::string, std::string> oldNewScriptNamesMapping;
    for (NodesGuiList::iterator it = nodesToCopy.begin(); it != nodesToCopy.end(); ++it) {
        NodePtr sourceNode = (*it)->getNode();
        Serialization::NodeSerializationPtr internalSerialization = std::make_shared<Serialization::NodeSerialization>();
        sourceNode->toSerialization(internalSerialization.get());
        NodeGuiPtr clone = _imp->pasteNode(internalSerialization, sourceNode, offset,
                                           _imp->group.lock(), std::string(), true, &oldNewScriptNamesMapping);

        newNodes.push_back(std::make_pair(internalSerialization->scriptName, clone));
        newNodesList.push_back(clone);
        serializations.push_back(internalSerialization);

        oldNewScriptNamesMapping[internalSerialization->scriptName] = clone->getNode()->getScriptName();
    }


    assert( serializations.size() == newNodes.size() );
    ///restore connections
    _imp->restoreConnections(serializations, newNodes, oldNewScriptNamesMapping);


    NodesList allNodes;
    getGui()->getApp()->getProject()->getActiveNodesExpandGroups(&allNodes);

    //Restore links once all children are created for alias knobs/expressions
    std::list<Serialization::NodeSerializationPtr>::iterator itS = serializations.begin();
    for (std::list<NodeGuiPtr> ::iterator it = newNodesList.begin(); it != newNodesList.end(); ++it, ++itS) {
        (*it)->getNode()->storeKnobsLinks(**itS, oldNewScriptNamesMapping);
        (*it)->getNode()->restoreKnobsLinks(allNodes, oldNewScriptNamesMapping, true); // clone should never fail
    }


    pushUndoCommand( new AddMultipleNodesCommand(this, newNodesList) );
} // NodeGraph::cloneSelectedNodes

void
NodeGraph::cloneSelectedNodes()
{
    QPointF scenePos = mapToScene( mapFromGlobal( QCursor::pos() ) );

    cloneSelectedNodes(scenePos);
} // cloneSelectedNodes

void
NodeGraph::decloneSelectedNodes()
{
    if ( _imp->_selection.empty() ) {
        Dialogs::warningDialog( tr("Declone").toStdString(), tr("You must select at least a node to declone first.").toStdString() );

        return;
    }
    NodesGuiList nodesToDeclone;


    for (NodesGuiList::iterator it = _imp->_selection.begin(); it != _imp->_selection.end(); ++it) {
        BackdropGui* isBd = dynamic_cast<BackdropGui*>( it->get() );
        if (isBd) {
            ///Also copy all nodes within the backdrop
            NodesGuiList nodesWithinBD = getNodesWithinBackdrop(*it);
            for (NodesGuiList::iterator it2 = nodesWithinBD.begin(); it2 != nodesWithinBD.end(); ++it2) {
                NodesGuiList::iterator found = std::find(nodesToDeclone.begin(), nodesToDeclone.end(), *it2);
                if ( found == nodesToDeclone.end() ) {
                    nodesToDeclone.push_back(*it2);
                }
            }
        }
        if ( (*it)->getNode()->getEffectInstance()->isSlave() ) {
            nodesToDeclone.push_back(*it);
        }
    }

    pushUndoCommand( new DecloneMultipleNodesCommand(this, nodesToDeclone) );
}

void
NodeGraph::setUndoRedoStackLimit(int limit)
{
    _imp->_undoStack->clear();
    _imp->_undoStack->setUndoLimit(limit);
}

void
NodeGraph::deleteNodePermanantly(const NodeGuiPtr& n)
{
    assert(n);
    NodesGuiList::iterator it = std::find(_imp->_nodesTrash.begin(), _imp->_nodesTrash.end(), n);

    if ( it != _imp->_nodesTrash.end() ) {
        _imp->_nodesTrash.erase(it);
    }

    {
        QMutexLocker l(&_imp->_nodesMutex);
        NodesGuiList::iterator it = std::find(_imp->_nodes.begin(), _imp->_nodes.end(), n);
        if ( it != _imp->_nodes.end() ) {
            _imp->_nodes.erase(it);
        }
    }

    NodesGuiList::iterator found = std::find(_imp->_selection.begin(), _imp->_selection.end(), n);
    if ( found != _imp->_selection.end() ) {
        n->setUserSelected(false);
        _imp->_selection.erase(found);
    }
} // deleteNodePermanantly

void
NodeGraph::invalidateAllNodesParenting()
{
    for (NodesGuiList::iterator it = _imp->_nodes.begin(); it != _imp->_nodes.end(); ++it) {
        (*it)->setParentItem(NULL);
        if ( (*it)->scene() ) {
            (*it)->scene()->removeItem( it->get() );
        }
    }
    for (NodesGuiList::iterator it = _imp->_nodesTrash.begin(); it != _imp->_nodesTrash.end(); ++it) {
        (*it)->setParentItem(NULL);
        if ( (*it)->scene() ) {
            (*it)->scene()->removeItem( it->get() );
        }
    }
}

void
NodeGraph::centerOnAllNodes()
{
    assert( QThread::currentThread() == qApp->thread() );
    double xmin = std::numeric_limits<int>::max();
    double xmax = std::numeric_limits<int>::min();
    double ymin = std::numeric_limits<int>::max();
    double ymax = std::numeric_limits<int>::min();
    //_imp->_root->setPos(0,0);

    if ( _imp->_selection.empty() ) {
        QMutexLocker l(&_imp->_nodesMutex);

        for (NodesGuiList::iterator it = _imp->_nodes.begin(); it != _imp->_nodes.end(); ++it) {
            if ( /*(*it)->isActive() &&*/ (*it)->isVisible() ) {
                QSize size = (*it)->getSize();
                QPointF pos = (*it)->mapToScene( (*it)->mapFromParent( (*it)->getPos_mt_safe() ) );
                xmin = std::min( xmin, pos.x() );
                xmax = std::max( xmax, pos.x() + size.width() );
                ymin = std::min( ymin, pos.y() );
                ymax = std::max( ymax, pos.y() + size.height() );
            }
        }
    } else {
        for (NodesGuiList::iterator it = _imp->_selection.begin(); it != _imp->_selection.end(); ++it) {
            if ( /*(*it)->isActive() && */ (*it)->isVisible() ) {
                QSize size = (*it)->getSize();
                QPointF pos = (*it)->mapToScene( (*it)->mapFromParent( (*it)->getPos_mt_safe() ) );
                xmin = std::min( xmin, pos.x() );
                xmax = std::max( xmax, pos.x() + size.width() );
                ymin = std::min( ymin, pos.y() );
                ymax = std::max( ymax, pos.y() + size.height() );
            }
        }
    }
    // Move the scene so that topleft of the viewing area is at 0,0, and avoid issues
    // when topleft has negative coords.
    moveRootInternal(-xmin, -ymin);
    QRectF bbox( 0, 0, (xmax - xmin), (ymax - ymin) );
    setAlignment(Qt::AlignRight|Qt::AlignVCenter);
    fitInView(bbox, Qt::KeepAspectRatio);

    double currentZoomFactor = transform().mapRect( QRectF(0, 0, 1, 1) ).width();
    assert(currentZoomFactor != 0);
    //we want to fit at scale 1 at most
    if (currentZoomFactor > 1.) {
        double scaleFactor = 1. / currentZoomFactor;
        setTransformationAnchor(QGraphicsView::AnchorViewCenter);
        scale(scaleFactor, scaleFactor);
        setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    }

    _imp->_refreshOverlays = true;
    update();
} // NodeGraph::centerOnAllNodes

static QString
briefConnectFailureReason(Node::CanConnectInputReturnValue ret)
{
    switch (ret) {
    case Node::eCanConnectInput_graphCycles:
        return QCoreApplication::translate("NodeGraph", "would create a cycle");
    case Node::eCanConnectInput_groupHasNoOutput:
        return QCoreApplication::translate("NodeGraph", "the group has no output");
    case Node::eCanConnectInput_differentPars:
        return QCoreApplication::translate("NodeGraph", "different pixel aspect ratios");
    case Node::eCanConnectInput_differentFPS:
        return QCoreApplication::translate("NodeGraph", "different frame rates");
    case Node::eCanConnectInput_multiResNotSupported:
        return QCoreApplication::translate("NodeGraph", "multi-resolution isn't supported");
    case Node::eCanConnectInput_incompatibleDataKind:
        return QCoreApplication::translate("NodeGraph", "incompatible data kind");
    case Node::eCanConnectInput_givenNodeNotConnectable:
        return QCoreApplication::translate("NodeGraph", "it can't be connected to");
    case Node::eCanConnectInput_indexOutOfRange:
    case Node::eCanConnectInput_inputAlreadyConnected:
    case Node::eCanConnectInput_ok:
        break;
    }

    return QCoreApplication::translate("NodeGraph", "the connection was rejected");
}

void
NodeGraph::applyConnectPlan(const std::vector<ConnectPlanConnection>& plan,
                            const QString& undoText)
{
    QStringList skipped;
    QUndoCommand* parentCommand = new QUndoCommand(undoText);

    for (std::size_t i = 0; i < plan.size(); ++i) {
        const ConnectPlanConnection& entry = plan[i];
        NodePtr srcNode = entry.source ? entry.source->getNode() : NodePtr();
        NodePtr dstNode = entry.dest ? entry.dest->getNode() : NodePtr();
        if (!srcNode || !dstNode) {
            continue;
        }

        Node::CanConnectInputReturnValue ret = dstNode->canConnectInput(srcNode, entry.destInput);
        if (ret != Node::eCanConnectInput_ok) {
            skipped << tr("%1 to %2's input %3 (%4)")
                           .arg(QString::fromUtf8(srcNode->getLabel().c_str()))
                           .arg(QString::fromUtf8(dstNode->getLabel().c_str()))
                           .arg(entry.destInput + 1)
                           .arg(briefConnectFailureReason(ret));
            continue;
        }

        Edge* edge = entry.dest->getInputArrow(entry.destInput);
        assert(edge);
        if (!edge) {
            continue;
        }
        new ConnectCommand(this, edge, edge->getSource(), entry.source, parentCommand);
    }

    if (parentCommand->childCount() > 0) {
        pushUndoCommand(parentCommand);
    } else {
        delete parentCommand;
    }

    if (!skipped.isEmpty()) {
        getGui()->statusBar()->showMessage(tr("Skipped: %1").arg(skipped.join(QLatin1String("; "))), 5000);
    }
} // NodeGraph::applyConnectPlan

void
NodeGraph::connectLaterSelectedNodesIntoFirstSelected()
{
    std::vector<ConnectPlanConnection> plan = planFeedFirstSelectedForSelection(_imp->_selection);

    applyConnectPlan(plan, tr("Connect Later-Selected Nodes into First Selected"));
}

void
NodeGraph::connectEarlierSelectedNodesIntoLastSelected()
{
    std::vector<ConnectPlanConnection> plan = planFeedLastSelectedForSelection(_imp->_selection);

    applyConnectPlan(plan, tr("Connect Earlier-Selected Nodes into Last Selected"));
}

void
NodeGraph::connectFirstSelectedIntoLaterSelectedNodes()
{
    std::vector<ConnectPlanConnection> plan = planFeedFirstSelectedIntoLaterForSelection(_imp->_selection);

    applyConnectPlan(plan, tr("Connect First Selected into Later-Selected Nodes"));
}

void
NodeGraph::connectLastSelectedIntoOtherSelectedNodes()
{
    std::vector<ConnectPlanConnection> plan = planFeedLastSelectedIntoOthersForSelection(_imp->_selection);

    applyConnectPlan(plan, tr("Connect Last Selected into Other Selected Nodes"));
}

NATRON_NAMESPACE_EXIT
