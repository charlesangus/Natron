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

#include <stdexcept>

GCC_DIAG_UNUSED_PRIVATE_FIELD_OFF
CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QMouseEvent>
#include <QCursor>
#include <QApplication>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)
GCC_DIAG_UNUSED_PRIVATE_FIELD_ON

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Settings.h"
#include "Engine/TimeLine.h"

#include "Gui/BackdropGui.h"
#include "Gui/Edge.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/GuiMacros.h"
#include "Gui/NodeGraphPickUp.h"
#include "Gui/NodeGui.h"

#include "Global/QtCompat.h"

#define NATRON_GRAPH_AUTO_SCROLL_OFFSET 20
#define NATRON_AUTO_SCROLL_TIMEOUT_MS 50


NATRON_NAMESPACE_ENTER
static bool
handleConnectionError(const NodeGuiPtr& outputNode,
                      const NodeGuiPtr& inputNode,
                      int inputNb)
{
    NodePtr conflictingNode;
    Node::CanConnectInputReturnValue linkRetCode = outputNode->getNode()->canConnectInput(inputNode->getNode(), inputNb, &conflictingNode);

    if ( (linkRetCode != Node::eCanConnectInput_ok) && (linkRetCode != Node::eCanConnectInput_inputAlreadyConnected) ) {
        // The check judged the current frame, so quote that frame's values.
        const double time = outputNode->getNode()->getApp()->getTimeLine()->currentFrame();
        const NodeMetadataConstPtr outputMetadata = outputNode->getNode()->getEffectInstance()->getMetadata(time, ViewIdx(0));
        const NodeMetadataConstPtr inputMetadata = inputNode->getNode()->getEffectInstance()->getMetadata(time, ViewIdx(0));
        const double outputPAR = outputMetadata ? outputMetadata->getPixelAspectRatio(-1) : 1.;
        const double inputPAR = inputMetadata ? inputMetadata->getPixelAspectRatio(-1) : 1.;
        const double outputFPS = outputMetadata ? outputMetadata->getOutputFrameRate() : 0.;
        const double inputFPS = inputMetadata ? inputMetadata->getOutputFrameRate() : 0.;
        if (linkRetCode == Node::eCanConnectInput_differentPars) {
            QString error = QCoreApplication::translate("NodeGraph", "%1 and %2 do not have the same pixel aspect ratio (%3 vs. %4), and %1 does not support inputs with different pixel aspect ratio.\n"
                                                                     "This might yield unwanted results.")
                                .arg(QString::fromUtf8(outputNode->getNode()->getLabel().c_str()))
                                .arg(QString::fromUtf8(inputNode->getNode()->getLabel().c_str()))
                                .arg(outputPAR)
                                .arg(inputPAR);
            Dialogs::warningDialog( QCoreApplication::translate("NodeGraph", "Different pixel aspect").toStdString(),
                                    error.toStdString() );

            return true;
        } else if (linkRetCode == Node::eCanConnectInput_differentFPS) {
            QString error = QCoreApplication::translate("NodeGraph", "%1 and %2 do not have the same frame rate (%3 vs. %4).\n"
                                                                     "This might yield unwanted results.\n"
                                                                     "Either change the FPS from the Read node parameters or change the settings of the project.")
                                .arg(QString::fromUtf8(outputNode->getNode()->getLabel().c_str()))
                                .arg(QString::fromUtf8(inputNode->getNode()->getLabel().c_str()))
                                .arg(outputFPS)
                                .arg(inputFPS);
            Dialogs::warningDialog( QCoreApplication::translate("NodeGraph", "Different frame rate").toStdString(),
                                    error.toStdString() );

            return true;
        } else if (linkRetCode == Node::eCanConnectInput_groupHasNoOutput) {
            QString error = QCoreApplication::translate("NodeGraph", "You cannot connect %1 to %2, because it is a group which does not have an Output node.")
                            .arg( QString::fromUtf8( outputNode->getNode()->getLabel().c_str() ) )
                            .arg( QString::fromUtf8( inputNode->getNode()->getLabel().c_str() ) );
            Dialogs::errorDialog( QCoreApplication::translate("NodeGraph", "Different frame rate").toStdString(),
                                  error.toStdString() );
        } else if (linkRetCode == Node::eCanConnectInput_multiResNotSupported) {
            QString error = QCoreApplication::translate("NodeGraph", "You cannot connect %1 to %2 because multi-resolution is not supported on %1, "
                                                        "which means that it cannot receive images with a lower left corner different than (0,0) and cannot have "
                                                        "multiple inputs/outputs with different image sizes.\n"
                                                        "To overcome this, use a Resize or Crop node upstream to change the image size. In some cases you might also need to make sure the input image size is the project size.").arg( QString::fromUtf8( outputNode->getNode()->getLabel().c_str() ) )
                            .arg( QString::fromUtf8( inputNode->getNode()->getLabel().c_str() ) );
            Dialogs::errorDialog( QCoreApplication::translate("NodeGraph", "Multi-resolution not supported").toStdString(),
                                  error.toStdString() );;
        } else if (linkRetCode == Node::eCanConnectInput_incompatibleDataKind) {
            QString error = QCoreApplication::translate("NodeGraph", "You cannot connect %1 to %2 because the data produced is incompatible with %3.")
                                .arg(QString::fromUtf8(outputNode->getNode()->getLabel().c_str()))
                                .arg(QString::fromUtf8(inputNode->getNode()->getLabel().c_str()))
                                .arg(conflictingNode ? QString::fromUtf8(conflictingNode->getLabel().c_str()) : QString::fromUtf8(outputNode->getNode()->getLabel().c_str()));
            Dialogs::errorDialog(QCoreApplication::translate("NodeGraph", "Incompatible data kind").toStdString(),
                                 error.toStdString());
        }

        return false;
    }

    if ( (linkRetCode == Node::eCanConnectInput_ok) && outputNode->getNode()->getEffectInstance()->isReader() &&
         ( inputNode->getNode()->getPluginID() != PLUGINID_OFX_RUNSCRIPT) &&
         !inputNode->getNode()->getEffectInstance()->isWriter() ) {
        Dialogs::warningDialog( QCoreApplication::translate("NodeGraph", "Reader input").toStdString(),
                                QCoreApplication::translate("NodeGraph", "Connecting an input to a Reader node "
                                                            "is only useful when connecting to the output of a Writer "
                                                            "or RunScript node "
                                                            "so that the Reader automatically reads an image "
                                                            "when the render of the Writer or RunScript is done.").toStdString() );
    }

    return true;
} // handleConnectionError

bool
NodeGraph::connectInputArrowToNode(Edge* arrow,
                                   const NodeGuiPtr& target)
{
    assert(arrow && !arrow->isOutputEdge());
    NodeGuiPtr nodeHoldingEdge = arrow->getDest();
    assert(nodeHoldingEdge);

    bool ok = handleConnectionError(nodeHoldingEdge, target, arrow->getInputNumber());
    arrow->stackBefore(target.get());
    if (ok) {
        pushUndoCommand(new ConnectCommand(this, arrow, arrow->getSource(), target),
                        NodesGuiList { nodeHoldingEdge, arrow->getSource(), target });
    }

    return ok;
}

Edge*
NodeGraph::getPickedUpArrow() const
{
    if (_imp->_pickedUpArrowInput < 0) {
        return 0;
    }
    NodeGuiPtr dest = _imp->_pickedUpArrowDest.lock();
    if (!dest || !dest->getNode() || !dest->getNode()->isActivated()) {
        return 0;
    }

    return dest->getInputArrow(_imp->_pickedUpArrowInput);
}

void
NodeGraph::pickUpArrow(Edge* arrow)
{
    assert(arrow && !arrow->isOutputEdge());
    _imp->_pickedUpArrowDest = arrow->getDest();
    _imp->_pickedUpArrowInput = arrow->getInputNumber();
    dragPickedUpArrowToCursor();
}

// _lastMousePos rather than QCursor::pos(): every mouse and wheel handler keeps it current, and it
// also follows synthesized events, which never move the system cursor.
void
NodeGraph::dragPickedUpArrowToCursor()
{
    Edge* arrow = getPickedUpArrow();
    if (!arrow) {
        return;
    }
    QPointF cursorScene = mapToScene(_imp->_lastMousePos.x(), _imp->_lastMousePos.y());
    arrow->dragSource(arrow->mapFromScene(cursorScene));
    update();
}

// The arrow's source is never changed while it is held, only its drawn line, so restoring the
// line restores the original connection and nothing reaches the undo stack.
void
NodeGraph::dropPickedUpArrow()
{
    Edge* arrow = getPickedUpArrow();
    _imp->_pickedUpArrowDest.reset();
    _imp->_pickedUpArrowInput = -1;
    if (arrow) {
        arrow->initLine();
        update();
    }
}

bool
NodeGraph::handlePickUpPress(QMouseEvent* e)
{
    Edge* arrow = getPickedUpArrow();
    if (!arrow) {
        dropPickedUpArrow();

        return false;
    }

    PickUpButtonEnum button = ePickUpButtonOther;
    if (buttonDownIsLeft(e)) {
        button = ePickUpButtonLeft;
    } else if (buttonDownIsRight(e)) {
        button = ePickUpButtonRight;
    } else if (buttonDownIsMiddle(e)) {
        button = ePickUpButtonMiddle;
    }

    NodeGuiPtr nodeHoldingEdge = arrow->getDest();
    NodeGui* nearbyNode = 0;
    Edge* nearbyEdge = 0;
    PickUpClickTargetEnum target = ePickUpClickTargetOther;
    QPointF navigatorScenePos;
    if (isNearbyNavigator(e->pos(), navigatorScenePos)) {
        target = ePickUpClickTargetNavigator;
    } else if ((hasItemNearbyMouse(e->pos(), &nearbyNode, &nearbyEdge) == eNearbyItemNode) && nearbyNode) {
        target = (nearbyNode == nodeHoldingEdge.get()) ? ePickUpClickTargetHoldingNode : ePickUpClickTargetNode;
    }

    switch (decidePickUpPress(button, target)) {
    case ePickUpPressPassThrough:
        return false;
    case ePickUpPressIgnore:
        return true;
    case ePickUpPressCancel:
        dropPickedUpArrow();

        return true;
    case ePickUpPressDisconnect: {
        _imp->_pickedUpArrowDest.reset();
        _imp->_pickedUpArrowInput = -1;
        if (arrow->getSource()) {
            pushUndoCommand(new ConnectCommand(this, arrow, arrow->getSource(), NodeGuiPtr()),
                            NodesGuiList { arrow->getDest(), arrow->getSource() });
        }
        if (nodeHoldingEdge) {
            nodeHoldingEdge->refreshEdges();
        }
        arrow->initLine();
        scene()->update();
        update();

        return true;
    }
    case ePickUpPressConnect: {
        NodeGuiPtr targetNode = nearbyNode->shared_from_this();
        // Let go of the arrow before connecting: a rejected connection opens a dialog, and the
        // focus loss that causes must not find the arrow still held.
        _imp->_pickedUpArrowDest.reset();
        _imp->_pickedUpArrowInput = -1;
        connectInputArrowToNode(arrow, targetNode);
        if (nodeHoldingEdge) {
            nodeHoldingEdge->refreshEdges();
        }
        arrow->initLine();
        scene()->update();
        update();

        return true;
    }
    }

    return true;
}

void
NodeGraph::mouseReleaseEvent(QMouseEvent* e)
{
    EventStateEnum state = _imp->_evtState;

    _imp->_evtState = eEventStateNone;
    _imp->_dragGesture.end();

    _imp->autoScrollTimer.stop();

    bool hasMovedOnce = modCASIsControl(e) || _imp->_hasMovedOnce;
    if ( (state == eEventStateDraggingArrow) && hasMovedOnce ) {
        bool foundSrc = false;
        assert(_imp->_arrowSelected);
        NodeGuiPtr nodeHoldingEdge = _imp->_arrowSelected->isOutputEdge() ?
                                     _imp->_arrowSelected->getSource() : _imp->_arrowSelected->getDest();
        assert(nodeHoldingEdge);


        NodeGui* nearbyNode;
        Edge* nearbyEdge;
        NearbyItemEnum nearbyItemCode = hasItemNearbyMouse(e->pos(), &nearbyNode, &nearbyEdge);

        if (nearbyItemCode == eNearbyItemNode) {
            NodeGuiPtr targetNode = nearbyNode->shared_from_this();
            if (targetNode != nodeHoldingEdge) {
                if ( !_imp->_arrowSelected->isOutputEdge() ) {
                    foundSrc = connectInputArrowToNode(_imp->_arrowSelected, targetNode);
                } else {
                    // Find the input edge of the node we just released the mouse over,
                    // and use that edge to connect to the source of the selected edge.
                    int preferredInput = targetNode->getNode()->getPreferredInputForConnection();
                    if (preferredInput != -1) {
                        bool ok = handleConnectionError(targetNode, nodeHoldingEdge, preferredInput);
                        if (ok) {
                            Edge* foundInput = targetNode->getInputArrow(preferredInput);
                            assert(foundInput);
                            foundSrc = true;
                            pushUndoCommand(new ConnectCommand(this, foundInput,
                                                               foundInput->getSource(), _imp->_arrowSelected->getSource()),
                                            NodesGuiList { targetNode, foundInput->getSource(), _imp->_arrowSelected->getSource() });
                        }
                    }
                }
            }
        }

        // If we disconnected the input edge, use the undo/redo stack.
        // Output edges can never be really connected, they're just there
        // So the user understands some nodes can have output
        if ( !foundSrc && !_imp->_arrowSelected->isOutputEdge() && _imp->_arrowSelected->getSource() ) {
            pushUndoCommand(new ConnectCommand(this, _imp->_arrowSelected, _imp->_arrowSelected->getSource(),
                                               NodeGuiPtr()),
                            NodesGuiList { nodeHoldingEdge, _imp->_arrowSelected->getSource() });
        }


        nodeHoldingEdge->refreshEdges();
        scene()->update();
    } else if ((state == eEventStateDraggingArrow) && _imp->_arrowSelected && shouldStartPickUp(triggerButtonIsLeft(e) ? ePickUpButtonLeft : ePickUpButtonOther, hasMovedOnce, _imp->_arrowSelected->isOutputEdge())) {
        pickUpArrow(_imp->_arrowSelected);
    } else if (state == eEventStateDraggingNode) {
        if ( !_imp->_hasMovedOnce ) {
            if ( modCASIsControl(e) ) { // control-click is the same as double-click, see NodeGraph::mouseDoubleClickEvent
                NodeGui* nearbyNode;
                Edge* nearbyEdge;
                NearbyItemEnum nearbyItemCode = hasItemNearbyMouse(e->pos(), &nearbyNode, &nearbyEdge);

                if (nearbyItemCode == eNearbyItemNode || nearbyItemCode == eNearbyItemBackdropFrame) {
                    showNodePanel(false, modCASIsShift(e), nearbyNode);
                }
            }
        } else if ( !_imp->_selection.empty() ) {
            NodesGuiList nodesToMove;
            for (NodesGuiList::iterator it = _imp->_selection.begin();
                 it != _imp->_selection.end(); ++it) {
                const NodeGuiPtr& node = *it;
                nodesToMove.push_back(node);

                std::map<NodeGuiPtr, NodesGuiList>::iterator foundBd = _imp->_nodesWithinBDAtPenDown.find(*it);
                if ( !modCASIsControl(e) && ( foundBd != _imp->_nodesWithinBDAtPenDown.end() ) ) {
                    for (NodesGuiList::iterator it2 = foundBd->second.begin();
                         it2 != foundBd->second.end(); ++it2) {
                        ///add it only if it's not already in the list
                        bool found = false;
                        for (NodesGuiList::iterator it3 = nodesToMove.begin();
                             it3 != nodesToMove.end(); ++it3) {
                            if (*it3 == *it2) {
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            nodesToMove.push_back(*it2);
                        }
                    }
                }
            }
            const bool hasMoved = (_imp->_deltaSinceMousePress.x() != 0) || (_imp->_deltaSinceMousePress.y() != 0);

            if (_imp->_splicePipe) {
                Edge* pipe = _imp->_splicePipe;
                NodeGuiPtr entryGui = _imp->_spliceEntry;
                NodeGuiPtr exitGui = _imp->_spliceExit;
                int entryInput = _imp->_spliceEntryInput;
                resetSplicePlan();

                QUndoCommand* spliceCommand = new QUndoCommand(tr("Insert nodes"));
                NodesGuiList touched = nodesToMove;
                touched.push_back(pipe->getDest());
                touched.push_back(pipe->getSource());
                touched.push_back(exitGui);
                touched.push_back(entryGui);
                if (hasMoved) {
                    new MoveMultipleNodesCommand(nodesToMove, _imp->_deltaSinceMousePress.x(), _imp->_deltaSinceMousePress.y(), spliceCommand);
                }
                new InsertNodeCommand(this, pipe, exitGui, entryGui, entryInput, spliceCommand);
                pushUndoCommand(spliceCommand, touched);
            } else {
                if (hasMoved) {
                    pushUndoCommand(new MoveMultipleNodesCommand(nodesToMove, _imp->_deltaSinceMousePress.x(), _imp->_deltaSinceMousePress.y()), nodesToMove);
                }
            }
        }
    } else if (state == eEventStateSelectionRect) {
        _imp->editSelectionFromSelectionRectangle(modCASIsShift(e));
    }
    resetSplicePlan();
    _imp->_nodesWithinBDAtPenDown.clear();

    _imp->cursorSet = false;
    unsetCursor();
    update();
} // mouseReleaseEvent

void
NodeGraph::scrollViewIfNeeded(const QPointF& scenePos)
{
    QPoint widgetPos = mapFromScene(scenePos);
    QRectF visibleRect = visibleWidgetRect();
    int scrollDeltaZone = 50;
    int dx = 0, dy = 0;

    if (widgetPos.x() <= visibleRect.x() + scrollDeltaZone) {
        dx = -NATRON_GRAPH_AUTO_SCROLL_OFFSET;
    } else if (widgetPos.x() >= ( visibleRect.x() + visibleRect.width() ) - scrollDeltaZone) {
        dx = NATRON_GRAPH_AUTO_SCROLL_OFFSET;
    }
    if (widgetPos.y() <= visibleRect.y() + scrollDeltaZone) {
        dy = -NATRON_GRAPH_AUTO_SCROLL_OFFSET;
    } else if (widgetPos.y() >= ( visibleRect.y() + visibleRect.height() ) - scrollDeltaZone) {
        dy = NATRON_GRAPH_AUTO_SCROLL_OFFSET;
    }


    if ( (dx != 0) || (dy != 0) ) {
        moveRootInternal(-dx, -dy);
        /*if (_imp->_evtState == eEventStateDraggingArrow && _imp->_arrowSelected) {

           } else if (_imp->_evtState == eEventStateDraggingNode && !_imp->_selection.empty()) {
            Qt::KeyboardModifiers mods = qApp->keyboardModifiers();
            QPointF lastMousePosScene = mapToScene(_imp->_lastMousePos);
            QPoint newPos = _imp->_lastMousePos + QPoint(dx,dy);
            QPointF newPosScene = mapToScene(newPos);
            moveSelectedNodesBy(mods.testFlag(Qt::ShiftModifier), mods.testFlag(Qt::ControlModifier), lastMousePosScene, newPosScene, visibleSceneRect(), false);
            _imp->_lastMousePos = newPos;
           }*/

        _imp->_refreshOverlays = true;
        update();
    } else {
        _imp->autoScrollTimer.stop();
    }
}

void
NodeGraph::moveRootInternal(double dx,
                            double dy)
{
    _imp->_lastSelectionStartPointScene.rx() += dx;
    _imp->_lastSelectionStartPointScene.ry() += dy;

    _imp->_root->moveBy(dx, dy);
}

void
NodeGraph::checkAndStartAutoScrollTimer(const QPointF& scenePos)
{
    if ( _imp->autoScrollTimer.isActive() ) {
        return;
    }
    QPoint widgetPos = mapFromScene(scenePos);
    QRectF visibleRect = visibleWidgetRect();
    int scrollDeltaZone = 50;
    bool startScrolling = false;
    if (widgetPos.x() <= visibleRect.x() + scrollDeltaZone) {
        startScrolling = true;
    } else if (widgetPos.x() >= ( visibleRect.x() + visibleRect.width() ) - scrollDeltaZone) {
        startScrolling = true;
    }
    if (widgetPos.y() <= visibleRect.y() + scrollDeltaZone) {
        startScrolling = true;
    } else if (widgetPos.y() >= ( visibleRect.y() + visibleRect.height() ) - scrollDeltaZone) {
        startScrolling = true;
    }

    if (startScrolling) {
        _imp->autoScrollTimer.start(NATRON_AUTO_SCROLL_TIMEOUT_MS);
    }
}

void
NodeGraph::onAutoScrollTimerTriggered()
{
    bool isAutoScrollEnabled = appPTR->getCurrentSettings()->isAutoScrollEnabled();

    if (isAutoScrollEnabled) {
        QPointF cursorPos = mapToScene( mapFromGlobal( QCursor::pos() ) );
        scrollViewIfNeeded(cursorPos);
    }
}

NATRON_NAMESPACE_EXIT
