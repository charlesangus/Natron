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

#include <cmath> // abs
#include <stdexcept>

GCC_DIAG_UNUSED_PRIVATE_FIELD_OFF
CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QWheelEvent>
#include <QToolButton>
#include <QApplication>
#include <QTabBar>
#include <QTreeWidget>
GCC_DIAG_UNUSED_PRIVATE_FIELD_ON
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/Node.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/NodeGroup.h"
#include "Engine/ViewerInstance.h"

#include "Gui/CurveWidget.h"
#include "Gui/Edge.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/GuiDefines.h"
#include "Gui/GuiMacros.h"
#include "Gui/Histogram.h"
#include "Gui/NodeGraphUndoRedo.h"
#include "Gui/NodeGui.h"
#include "Gui/ViewerGL.h"
#include "Gui/ViewerTab.h"

#include "Global/QtCompat.h"

NATRON_NAMESPACE_ENTER
ViewerTab*
NodeGraph::getViewerForInputKeys()
{
    ViewerTab* lastUsedViewer = getLastSelectedViewer();

    if (lastUsedViewer) {
        NodeCollectionPtr collection = lastUsedViewer->getInternalNode()->getNode()->getGroup();
        if ( collection && (collection->getNodeGraph() != this) ) {
            //somehow the group doesn't belong to this nodegraph , pick another one
            const std::list<ViewerTab*>& tabs = getGui()->getViewersList();
            lastUsedViewer = 0;
            for (std::list<ViewerTab*>::const_iterator it = tabs.begin(); it != tabs.end(); ++it) {
                NodeCollectionPtr otherCollection = (*it)->getInternalNode()->getNode()->getGroup();
                if ( otherCollection && (otherCollection->getNodeGraph() == this) ) {
                    lastUsedViewer = *it;
                    break;
                }
            }
        }
    }

    return lastUsedViewer;
}

NodeGuiPtr
NodeGraph::getOrCreateViewerForInputKeys()
{
    ViewerTab* lastUsedViewer = getViewerForInputKeys();
    NodePtr viewerNode;

    if (lastUsedViewer) {
        viewerNode = lastUsedViewer->getInternalNode()->getNode();
    } else {
        CreateNodeArgs args( PLUGINID_NATRON_VIEWER,
                             getGroup() );
        // The key decides which input the selection goes to, not the preferred input.
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
        viewerNode = getGui()->getApp()->createNode(args);
    }

    /// if the node is no longer active (i.e: it was deleted by the user), don't do anything.
    if (!viewerNode || !viewerNode->isActivated()) {
        return NodeGuiPtr();
    }

    return std::dynamic_pointer_cast<NodeGui>(viewerNode->getNodeGui());
}

NodeGuiPtr
NodeGraph::getSelectionForViewerInput() const
{
    if (_imp->_selection.size() != 1) {
        return NodeGuiPtr();
    }
    const NodeGuiPtr& selected = _imp->_selection.front();
    const NodePtr node = selected->getNode();
    if (node->isEffectViewer() || !node->canOthersConnectToThisNode()) {
        return NodeGuiPtr();
    }

    return selected;
}

void
NodeGraph::pipeViewerInput(const NodeGuiPtr& viewerGui,
                           int inputNB,
                           const NodeGuiPtr& source,
                           bool isASide)
{
    InspectorNodePtr inspector = std::dynamic_pointer_cast<InspectorNode>(viewerGui->getNode());
    Edge* input = viewerGui->getInputArrow(inputNB);

    if (!inspector || !input) {
        return;
    }
    int activeA, activeB;
    inspector->getActiveInputs(activeA, activeB);
    if (input->getSource() != source) {
        pushUndoCommand(new ConnectCommand(this, input, input->getSource(), source),
                        NodesGuiList { input->getDest(), input->getSource(), source });
    }
    // A connection always makes the new pipe input A, which the B side must not change.
    if (!isASide && (activeA != -1)) {
        inspector->setInputA(activeA);
    }
    inspector->setActiveInputAndRefresh(inputNB, isASide);
    viewerGui->refreshEdges();
}

bool
NodeGraph::recallLockedViewerInput(const NodeGuiPtr& viewerGui,
                                   int inputNB,
                                   bool isASide)
{
    const NodePtr viewerNode = viewerGui->getNode();
    ViewerInstance* viewer = viewerNode->isEffectViewer();

    if (!viewer) {
        return false;
    }
    const std::string lockedName = viewer->getLockedInput(inputNB);
    if (lockedName.empty()) {
        return false;
    }
    NodeCollectionPtr collection = viewerNode->getGroup();
    NodePtr locked = collection ? collection->getNodeByName(lockedName) : NodePtr();
    NodeGuiPtr lockedGui = locked ? std::dynamic_pointer_cast<NodeGui>(locked->getNodeGui()) : NodeGuiPtr();
    if (!lockedGui || !locked->isActivated() || !locked->canOthersConnectToThisNode()) {
        viewer->setLockedInput(inputNB, std::string());

        return false;
    }
    pipeViewerInput(viewerGui, inputNB, lockedGui, isASide);

    return true;
}

void
NodeGraph::connectCurrentViewerToSelection(int inputNB,
                                           bool isASide)
{
    // Creating a viewer selects it, so the selection has to be read first.
    const NodeGuiPtr selected = getSelectionForViewerInput();
    const NodeGuiPtr viewerGui = getOrCreateViewerForInputKeys();

    if (!viewerGui) {
        return;
    }
    if (recallLockedViewerInput(viewerGui, inputNB, isASide)) {
        return;
    }
    if (selected) {
        pipeViewerInput(viewerGui, inputNB, selected, isASide);

        return;
    }
    InspectorNodePtr inspector = std::dynamic_pointer_cast<InspectorNode>(viewerGui->getNode());
    if (inspector) {
        inspector->setActiveInputAndRefresh(inputNB, isASide);
    }
    viewerGui->refreshEdges();
} // connectCurrentViewerToSelection

void
NodeGraph::lockViewerInputToSelection(int inputNB)
{
    const NodeGuiPtr selected = getSelectionForViewerInput();
    if (!selected) {
        return;
    }
    const NodeGuiPtr viewerGui = getOrCreateViewerForInputKeys();
    if (!viewerGui) {
        return;
    }
    Edge* input = viewerGui->getInputArrow(inputNB);
    if (!input) {
        return;
    }
    pushUndoCommand(new ViewerInputLockCommand(this, viewerGui, input, selected),
                    NodesGuiList { viewerGui, input->getSource(), selected });
}

void
NodeGraph::unlockViewerInput(int inputNB)
{
    ViewerTab* viewerTab = getViewerForInputKeys();
    if (!viewerTab) {
        return;
    }
    const NodePtr viewerNode = viewerTab->getInternalNode()->getNode();
    if (viewerTab->getInternalNode()->getLockedInput(inputNB).empty()) {
        return;
    }
    const NodeGuiPtr viewerGui = std::dynamic_pointer_cast<NodeGui>(viewerNode->getNodeGui());
    Edge* input = viewerGui ? viewerGui->getInputArrow(inputNB) : 0;
    if (!input) {
        return;
    }
    pushUndoCommand(new ViewerInputLockCommand(this, viewerGui, input, NodeGuiPtr()),
                    NodesGuiList(1, viewerGui));
}

void
NodeGraph::enterEvent(QEnterEvent* e)
{
    enterEventBase();
    QGraphicsView::enterEvent(e);
    _imp->_nodeCreationShortcutEnabled = true;
}

void
NodeGraph::leaveEvent(QEvent* e)
{
    leaveEventBase();
    QGraphicsView::leaveEvent(e);

    _imp->_nodeCreationShortcutEnabled = false;
    // setFocus();
}

void
NodeGraph::wheelEventInternal(bool ctrlDown,
                              double delta)
{
    double scaleFactor = pow( NATRON_WHEEL_ZOOM_PER_DELTA, delta);
    QTransform transfo = transform();
    double currentZoomFactor = transfo.mapRect( QRectF(0, 0, 1, 1) ).width();
    double newZoomfactor = currentZoomFactor * scaleFactor;

    if ( ( (newZoomfactor < 0.01) && (scaleFactor < 1.) ) || ( (newZoomfactor > 50) && (scaleFactor > 1.) ) ) {
        return;
    }

    if (ctrlDown && _imp->_magnifiedNode) {
        if (!_imp->_magnifOn) {
            _imp->_magnifOn = true;
            _imp->_nodeSelectedScaleBeforeMagnif = _imp->_magnifiedNode->scale();
        }
        _imp->_magnifiedNode->setScale_natron(_imp->_magnifiedNode->scale() * scaleFactor);
    } else {
        _imp->_accumDelta += delta;
        if (std::abs(_imp->_accumDelta) > 60) {
            scaleFactor = pow( NATRON_WHEEL_ZOOM_PER_DELTA, _imp->_accumDelta );
            scale(scaleFactor, scaleFactor);
            _imp->_accumDelta = 0;
        }
        _imp->_refreshOverlays = true;
    }
}

void
NodeGraph::wheelEvent(QWheelEvent* e)
{
    if (e->angleDelta().x() != 0) {
        return;
    }
    wheelEventInternal( modCASIsControl(e), e->angleDelta().y() );
    _imp->_lastMousePos = QPoint(e->position().x(), e->position().y());
    dragPickedUpArrowToCursor();
    update();
}

void
NodeGraph::keyReleaseEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Control) {
        if (_imp->_magnifOn) {
            _imp->_magnifOn = false;
            _imp->_magnifiedNode->setScale_natron(_imp->_nodeSelectedScaleBeforeMagnif);
        }
        if (_imp->_bendPointsVisible) {
            _imp->setNodesBendPointsVisible(false);
        }
        if (!e->isAutoRepeat()) {
            setKissAiming(false);
            update();
        }
    }

    handleUnCaughtKeyUpEvent(e);
    QGraphicsView::keyReleaseEvent(e);
}

void
NodeGraph::removeNode(const NodeGuiPtr & node)
{
    NodeGroup* isGrp = node->getNode()->isEffectGroup();
    const KnobsVec & knobs = node->getNode()->getKnobs();


    for (U32 i = 0; i < knobs.size(); ++i) {
        KnobI::ListenerDimsMap listeners;
        knobs[i]->getListeners(listeners);
        ///For all listeners make sure they belong to a node
        std::string foundLinkedErrorMessage;
        for (KnobI::ListenerDimsMap::iterator it2 = listeners.begin(); it2 != listeners.end(); ++it2) {
            KnobIPtr listener = it2->first.lock();
            if (!listener) {
                continue;
            }
            EffectInstance* isEffect = dynamic_cast<EffectInstance*>( listener->getHolder() );
            if (!isEffect) {
                continue;
            }
            if ( isGrp && (isEffect->getNode()->getGroup().get() == isGrp) ) {
                continue;
            }

            if ( isEffect && ( isEffect != node->getNode()->getEffectInstance().get() ) ) {
                std::string masterNodeName = node->getNode()->getFullyQualifiedName();
                std::string master = masterNodeName + "." + knobs[i]->getName();
                std::string slave = isEffect->getNode()->getFullyQualifiedName() + "." + listener->getName();
                foundLinkedErrorMessage = tr("%1 is linked to %2 through a link or expression. Deleting %3 may break this link.\nContinue anyway?").arg(QString::fromUtf8(slave.c_str())).arg(QString::fromUtf8(master.c_str())).arg(QString::fromUtf8(masterNodeName.c_str())).toStdString();
                break;
            }

            
            
            
        }
        if (!foundLinkedErrorMessage.empty()) {
            StandardButtonEnum reply = Dialogs::questionDialog( tr("Delete").toStdString(), foundLinkedErrorMessage, false );
            if (reply == eStandardButtonNo) {
                return;
            }
            break;
        }
    }

    node->setUserSelected(false);
    NodesGuiList nodesToRemove;
    nodesToRemove.push_back(node);
    pushUndoCommand(new RemoveMultipleNodesCommand(this, nodesToRemove), nodesToRemove);
} // NodeGraph::removeNode

void
NodeGraph::deleteSelection()
{
    if ( !_imp->_selection.empty() ) {
        NodesGuiList nodesToRemove = _imp->_selection;


        ///For all backdrops also move all the nodes contained within it
        for (NodesGuiList::iterator it = nodesToRemove.begin(); it != nodesToRemove.end(); ++it) {
            NodesGuiList nodesWithinBD = getNodesWithinBackdrop(*it);
            for (NodesGuiList::iterator it2 = nodesWithinBD.begin(); it2 != nodesWithinBD.end(); ++it2) {
                NodesGuiList::iterator found = std::find(nodesToRemove.begin(), nodesToRemove.end(), *it2);
                if ( found == nodesToRemove.end() ) {
                    nodesToRemove.push_back(*it2);
                }
            }
        }


        for (NodesGuiList::iterator it = nodesToRemove.begin(); it != nodesToRemove.end(); ++it) {
            const KnobsVec & knobs = (*it)->getNode()->getKnobs();
            bool mustBreak = false;
            NodeGroup* isGrp = (*it)->getNode()->isEffectGroup();

            for (U32 i = 0; i < knobs.size(); ++i) {
                KnobI::ListenerDimsMap listeners;
                knobs[i]->getListeners(listeners);

                ///For all listeners make sure they belong to a node
                std::string foundLinkedErrorMessage;
                for (KnobI::ListenerDimsMap::iterator it2 = listeners.begin(); it2 != listeners.end(); ++it2) {
                    KnobIPtr listener = it2->first.lock();
                    if (!listener) {
                        continue;
                    }
                    EffectInstance* isEffect = dynamic_cast<EffectInstance*>( listener->getHolder() );

                    if (!isEffect) {
                        continue;
                    }
                    if ( isGrp && (isEffect->getNode()->getGroup().get() == isGrp) ) {
                        continue;
                    }

                    if ( !isEffect->getNode()->getGroup() ) {
                        continue;
                    }

                    if ( isEffect && ( isEffect != (*it)->getNode()->getEffectInstance().get() ) ) {
                        bool isInSelection = false;
                        for (NodesGuiList::iterator it3 = nodesToRemove.begin(); it3 != nodesToRemove.end(); ++it3) {
                            if ((*it3)->getNode()->getEffectInstance().get() == isEffect) {
                                isInSelection = true;
                                break;
                            }
                        }
                        if (!isInSelection) {
                            std::string masterNodeName = (*it)->getNode()->getFullyQualifiedName();
                            std::string master = masterNodeName + "." + knobs[i]->getName();
                            std::string slave = isEffect->getNode()->getFullyQualifiedName() + "." + listener->getName();
                            foundLinkedErrorMessage = tr("%1 is linked to %2 through a link or expression. Deleting %3 may break this link.\nContinue anyway?").arg(QString::fromUtf8(slave.c_str())).arg(QString::fromUtf8(master.c_str())).arg(QString::fromUtf8(masterNodeName.c_str())).toStdString();
                            break;
                        }
                    }
                }
                if (!foundLinkedErrorMessage.empty()) {
                    StandardButtonEnum reply = Dialogs::questionDialog( tr("Delete").toStdString(),
                                                                       foundLinkedErrorMessage, false);
                    if (reply == eStandardButtonNo) {
                        return;
                    }
                    mustBreak = true;
                    break;
                }
            }
            if (mustBreak) {
                break;
            }
        }


        for (NodesGuiList::iterator it = nodesToRemove.begin(); it != nodesToRemove.end(); ++it) {
            (*it)->setUserSelected(false);
        }

        pushUndoCommand(new RemoveMultipleNodesCommand(this, nodesToRemove), nodesToRemove);
        _imp->_selection.clear();
    }
} // deleteSelection

void
NodeGraph::deselectNode(const NodeGuiPtr& n)
{
    {
        QMutexLocker k(&_imp->_nodesMutex);
        NodesGuiList::iterator it = std::find(_imp->_selection.begin(), _imp->_selection.end(), n);
        if ( it != _imp->_selection.end() ) {
            _imp->_selection.erase(it);
        }
    }
    n->setUserSelected(false);

    //Stop magnification if active
    if ( (_imp->_magnifiedNode == n) && _imp->_magnifOn ) {
        _imp->_magnifOn = false;
        _imp->_magnifiedNode->setScale_natron(_imp->_nodeSelectedScaleBeforeMagnif);
    }
}

void
NodeGraph::selectNode(const NodeGuiPtr & n,
                      bool addToSelection)
{
    if ( !n->isVisible() ) {
        return;
    }
    bool alreadyInSelection = std::find(_imp->_selection.begin(), _imp->_selection.end(), n) != _imp->_selection.end();


    assert(n);
    if (addToSelection && !alreadyInSelection) {
        _imp->_selection.push_back(n);
    } else if (!addToSelection) {
        clearSelection();
        _imp->_selection.push_back(n);
    }

    n->setUserSelected(true);

    NodePtr node = n->getNode();
    ViewerInstance* isViewer =  node ? node->isEffectViewer() : NULL;
    if (isViewer) {
        OpenGLViewerI* viewer = isViewer->getUiContext();
        const std::list<ViewerTab*> & viewerTabs = getGui()->getViewersList();
        for (std::list<ViewerTab*>::const_iterator it = viewerTabs.begin(); it != viewerTabs.end(); ++it) {
            if ( (*it)->getViewer() == viewer ) {
                setLastSelectedViewer( (*it) );
            }
        }
    }

    bool magnifiedNodeSelected = false;
    if (_imp->_magnifiedNode) {
        magnifiedNodeSelected = std::find(_imp->_selection.begin(), _imp->_selection.end(), _imp->_magnifiedNode)
                                != _imp->_selection.end();
    }
    if (magnifiedNodeSelected && _imp->_magnifOn) {
        _imp->_magnifOn = false;
        _imp->_magnifiedNode->setScale_natron(_imp->_nodeSelectedScaleBeforeMagnif);
    }
}

void
NodeGraph::setLastSelectedViewer(ViewerTab* tab)
{
    _imp->lastSelectedViewer = tab;
}

ViewerTab*
NodeGraph::getLastSelectedViewer() const
{
    return _imp->lastSelectedViewer;
}

void
NodeGraph::setSelection(const NodesGuiList& nodes)
{
    clearSelection();
    for (NodesGuiList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        selectNode(*it, true);
    }
}

void
NodeGraph::clearSelection()
{
    {
        QMutexLocker l(&_imp->_nodesMutex);
        for (NodesGuiList::iterator it = _imp->_selection.begin(); it != _imp->_selection.end(); ++it) {
            (*it)->setUserSelected(false);
        }
    }

    _imp->_selection.clear();
}

void
NodeGraph::updateNavigator()
{
    if ( !areAllNodesVisible() ) {
        _imp->_navigator->setPixmap( QPixmap::fromImage( getFullSceneScreenShot() ) );
        _imp->_navigator->show();
    } else {
        _imp->_navigator->hide();
    }
}

bool
NodeGraph::areAllNodesVisible()
{
    QRectF rect = visibleSceneRect();
    QMutexLocker l(&_imp->_nodesMutex);

    for (NodesGuiList::iterator it = _imp->_nodes.begin(); it != _imp->_nodes.end(); ++it) {
        if ( (*it)->isVisible() ) {
            if ( !rect.contains( (*it)->boundingRectWithEdges() ) ) {
                return false;
            }
        }
    }

    return true;
}

NATRON_NAMESPACE_EXIT
