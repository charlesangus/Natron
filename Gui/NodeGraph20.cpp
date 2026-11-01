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
#include <cmath> // abs
#include <stdexcept>
#include <vector>

GCC_DIAG_UNUSED_PRIVATE_FIELD_OFF
CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QMouseEvent>
#include <QToolTip>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)
GCC_DIAG_UNUSED_PRIVATE_FIELD_ON

#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Settings.h"
#include "Engine/Utils.h" // convertFromPlainText

#include "Gui/BackdropGui.h"
#include "Gui/Edge.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/GuiMacros.h"
#include "Gui/NodeGraphSplice.h"
#include "Gui/NodeGui.h"

#include "Global/QtCompat.h"

NATRON_NAMESPACE_ENTER

namespace {

const double kSpliceRectTolerance = 10;

bool
segmentTouchesRect(const QLineF& segment,
                   const QRectF& rect)
{
    if (rect.contains(segment.p1()) || rect.contains(segment.p2())) {
        return true;
    }
    const QLineF sides[4] = {
        QLineF(rect.topLeft(), rect.topRight()),
        QLineF(rect.topRight(), rect.bottomRight()),
        QLineF(rect.bottomRight(), rect.bottomLeft()),
        QLineF(rect.bottomLeft(), rect.topLeft())
    };
    QPointF intersection;
    for (int i = 0; i < 4; ++i) {
        if (segment.intersects(sides[i], &intersection) == QLineF::BoundedIntersection) {
            return true;
        }
    }

    return false;
}

QPointF
closestPointOnSegment(const QPointF& p,
                      const QLineF& segment)
{
    const double dx = segment.dx();
    const double dy = segment.dy();
    const double lengthSq = dx * dx + dy * dy;
    double t = 0.;
    if (lengthSq > 0.) {
        t = ((p.x() - segment.x1()) * dx + (p.y() - segment.y1()) * dy) / lengthSq;
        t = std::max(0., std::min(1., t));
    }

    return QPointF(segment.x1() + t * dx, segment.y1() + t * dy);
}

double
squaredDistanceToSegment(const QPointF& p,
                         const QLineF& segment)
{
    const QPointF closest = closestPointOnSegment(p, segment);
    const double cx = closest.x() - p.x();
    const double cy = closest.y() - p.y();

    return cx * cx + cy * cy;
}

// The pipe is already connected, so canConnectInput() on its destination stops at
// eCanConnectInput_inputAlreadyConnected before checking cycles or data kinds; those are
// checked separately here for the exit that would replace the pipe's source.
bool
canSpliceIntoPipe(const NodePtr& destNode,
                  int pipeInput,
                  const NodePtr& srcNode,
                  const NodePtr& entryNode,
                  int entryInput,
                  const NodePtr& exitNode)
{
    Node::CanConnectInputReturnValue destRet = destNode->canConnectInput(exitNode, pipeInput);
    if ((destRet != Node::eCanConnectInput_ok) && (destRet != Node::eCanConnectInput_inputAlreadyConnected)) {
        return false;
    }
    if (!destNode->checkIfConnectingInputIsOk(exitNode.get())) {
        return false;
    }
    if (destNode->checkDataKindCompatibility(exitNode, pipeInput) != Node::eCanConnectInput_ok) {
        return false;
    }

    return entryNode->canConnectInput(srcNode, entryInput) == Node::eCanConnectInput_ok;
}

} // anonymous namespace

void
NodeGraph::buildSplicePlan()
{
    _imp->_splicePlanBuilt = true;
    _imp->_splicePlanSelection = _imp->_selection;
    _imp->_spliceEntry.reset();
    _imp->_spliceEntryInput = -1;
    _imp->_spliceExit.reset();

    SplicePlan plan = planSpliceForSelection(_imp->_selection);
    if (!plan.valid) {
        return;
    }

    NodeGuiPtr entryGui, exitGui;
    for (NodesGuiList::const_iterator it = _imp->_selection.begin(); it != _imp->_selection.end(); ++it) {
        NodePtr node = (*it)->getNode();
        if (!node) {
            continue;
        }
        SpliceNodeId id = reinterpret_cast<SpliceNodeId>(node.get());
        if (id == plan.entryId) {
            entryGui = *it;
        }
        if (id == plan.exitId) {
            exitGui = *it;
        }
    }
    if (!entryGui || !exitGui || !entryGui->getInputArrow(plan.entryInput)) {
        return;
    }

    _imp->_spliceEntry = entryGui;
    _imp->_spliceEntryInput = plan.entryInput;
    _imp->_spliceExit = exitGui;
}

void
NodeGraph::clearSpliceHint()
{
    if (!_imp->_splicePipe) {
        return;
    }
    _imp->_splicePipe->setUseHighlight(false);
    _imp->_splicePipe = 0;
    _imp->_hintInputEdge->hide();
    _imp->_hintOutputEdge->hide();
}

void
NodeGraph::resetSplicePlan()
{
    clearSpliceHint();
    _imp->_splicePlanBuilt = false;
    _imp->_splicePlanSelection.clear();
    _imp->_spliceEntry.reset();
    _imp->_spliceEntryInput = -1;
    _imp->_spliceExit.reset();
}

void
NodeGraph::cancelSplice()
{
    resetSplicePlan();
    _imp->_spliceCancelled = true;
}

void
NodeGraph::checkForSpliceHint()
{
    if (_imp->_spliceCancelled) {
        return;
    }

    if (!_imp->_splicePlanBuilt || (_imp->_splicePlanSelection != _imp->_selection)) {
        clearSpliceHint();
        buildSplicePlan();
    }

    if (!_imp->_spliceEntry || !_imp->_spliceExit) {
        clearSpliceHint();

        return;
    }

    NodePtr entryNode = _imp->_spliceEntry->getNode();
    NodePtr exitNode = _imp->_spliceExit->getNode();
    if (!entryNode || !exitNode) {
        clearSpliceHint();

        return;
    }

    std::set<NodeGui*> selectedNodes;
    std::vector<QRectF> selectedRects;
    QRectF selectionBody;
    for (NodesGuiList::const_iterator it = _imp->_selection.begin(); it != _imp->_selection.end(); ++it) {
        selectedNodes.insert(it->get());
        QRectF r = (*it)->boundingRectWithEdges();
        r.adjust(-kSpliceRectTolerance, -kSpliceRectTolerance, kSpliceRectTolerance, kSpliceRectTolerance);
        selectedRects.push_back(r);
        selectionBody = selectionBody.united((*it)->mapToScene((*it)->boundingRect()).boundingRect());
    }
    const QPointF selectionCentre = selectionBody.center();

    std::set<NodeGui*> nodesWithinRect;
    getNodesWithinViewportRect(visibleWidgetRect(), &nodesWithinRect);

    Edge* bestPipe = 0;
    double bestDistSq = 0.;
    for (std::set<NodeGui*>::const_iterator it = nodesWithinRect.begin(); it != nodesWithinRect.end(); ++it) {
        NodeGui* destGui = *it;
        if ((selectedNodes.find(destGui) != selectedNodes.end()) || !destGui->isVisible()) {
            continue;
        }
        NodePtr destNode = destGui->getNode();
        if (!destNode) {
            continue;
        }
        EffectInstancePtr destEffect = destNode->getEffectInstance();

        const std::vector<Edge*>& pipes = destGui->getInputsArrows();
        for (std::size_t i = 0; i < pipes.size(); ++i) {
            Edge* pipe = pipes[i];
            if (!pipe || !pipe->isVisible() || !pipe->hasSource()) {
                continue;
            }
            NodeGuiPtr srcGui = pipe->getSource();
            if (!srcGui || (selectedNodes.find(srcGui.get()) != selectedNodes.end())) {
                continue;
            }
            if (destEffect && destEffect->isInputRotoBrush(pipe->getInputNumber())) {
                continue;
            }

            QLineF segment = pipe->line();
            segment.setP1(pipe->mapToScene(segment.p1()));
            segment.setP2(pipe->mapToScene(segment.p2()));

            bool touches = false;
            for (std::size_t r = 0; r < selectedRects.size(); ++r) {
                if (segmentTouchesRect(segment, selectedRects[r])) {
                    touches = true;
                    break;
                }
            }
            if (!touches) {
                continue;
            }

            const double distSq = squaredDistanceToSegment(selectionCentre, segment);
            if (bestPipe && (distSq >= bestDistSq)) {
                continue;
            }

            NodePtr srcNode = srcGui->getNode();
            if (!srcNode || !canSpliceIntoPipe(destNode, pipe->getInputNumber(), srcNode, entryNode, _imp->_spliceEntryInput, exitNode)) {
                continue;
            }

            bestPipe = pipe;
            bestDistSq = distSq;
        }
    }

    if (!bestPipe) {
        clearSpliceHint();

        return;
    }

    if (bestPipe == _imp->_splicePipe) {
        _imp->_hintInputEdge->initLine();
        _imp->_hintOutputEdge->initLine();

        return;
    }

    clearSpliceHint();
    _imp->_splicePipe = bestPipe;
    bestPipe->setUseHighlight(true);

    _imp->_hintInputEdge->setInputNumber(_imp->_spliceEntryInput);
    _imp->_hintInputEdge->setSourceAndDestination(bestPipe->getSource(), _imp->_spliceEntry);
    _imp->_hintInputEdge->setVisible(true);

    _imp->_hintOutputEdge->setInputNumber(bestPipe->getInputNumber());
    _imp->_hintOutputEdge->setSourceAndDestination(_imp->_spliceExit, bestPipe->getDest());
    _imp->_hintOutputEdge->setVisible(true);
} // NodeGraph::checkForSpliceHint

void
NodeGraph::moveSelectedNodesBy(bool /*shiftdown*/,
                               bool controlDown,
                               const QPointF& lastMousePosScene,
                               const QPointF& newPos,
                               const QRectF& /*visibleSceneR*/,
                               bool userEdit)
{
    if ( _imp->_selection.empty() ) {
        return;
    }


    //Get the nodes to move, taking into account the backdrops
    bool ignoreMagnet = false;
    std::set<NodeGuiPtr> nodesToMove;
    for (NodesGuiList::iterator it = _imp->_selection.begin();
         it != _imp->_selection.end(); ++it) {
        const NodeGuiPtr& node = *it;
        nodesToMove.insert(node);

        std::map<NodeGuiPtr, NodesGuiList>::iterator foundBd = _imp->_nodesWithinBDAtPenDown.find(*it);
        if ( !controlDown && ( foundBd != _imp->_nodesWithinBDAtPenDown.end() ) ) {
            ignoreMagnet = true; // we move a backdrop, ignore magnet
            for (NodesGuiList::iterator it2 = foundBd->second.begin();
                 it2 != foundBd->second.end(); ++it2) {
                ///add it only if it's not already in the list
                nodesToMove.insert(*it2);

            }
        }
    }

    if (!ignoreMagnet && nodesToMove.size() > 1) {
        ignoreMagnet = true;
    }

    //The delta
    double dxScene = newPos.x() - lastMousePosScene.x();
    double dyScene = newPos.y() - lastMousePosScene.y();


    //Move all nodes
    bool deltaSet = false;
    for (std::set<NodeGuiPtr>::iterator it = nodesToMove.begin();
         it != nodesToMove.end(); ++it) {
        //The current position
        QPointF pos = (*it)->getPos_mt_safe();

        //if ignoreMagnet == true, we do not snap nodes to horizontal/vertical positions
        (*it)->refreshPosition(pos.x() + dxScene, pos.y() + dyScene, ignoreMagnet, newPos);

        //The new position
        QPointF newNodePos = (*it)->getPos_mt_safe();
        if (!ignoreMagnet) {
            //Magnet only works when selection is only for a single node
            //Adjust the delta since mouse press by the new position after snapping
            assert(nodesToMove.size() == 1);
            _imp->_deltaSinceMousePress.rx() += newNodePos.x() - pos.x();
            _imp->_deltaSinceMousePress.ry() += newNodePos.y() - pos.y();
            deltaSet = true;
        }
    }

    if (!deltaSet) {
        _imp->_deltaSinceMousePress.rx() += dxScene;
        _imp->_deltaSinceMousePress.ry() += dyScene;
    }

    if (!userEdit) {
        return;
    }

    //Start auto-scolling if nearby the edges
    checkAndStartAutoScrollTimer(newPos);

    checkForSpliceHint();
} // NodeGraph::moveSelectedNodesBy

void
NodeGraph::mouseMoveEvent(QMouseEvent* e)
{
    QPointF newPos = mapToScene( e->pos() );
    QPointF lastMousePosScene = mapToScene( _imp->_lastMousePos.x(), _imp->_lastMousePos.y() );
    double dx, dy;
    {
        QPointF newPosRoot = _imp->_root->mapFromScene(newPos);
        QPointF lastMousePosRoot = _imp->_root->mapFromScene(lastMousePosScene);
        dx = newPosRoot.x() - lastMousePosRoot.x();
        dy = newPosRoot.y() - lastMousePosRoot.y();
    }

    _imp->_hasMovedOnce = true;

    bool mustUpdate = true;
    NodeCollectionPtr collection = getGroup();
    NodeGroup* isGroup = dynamic_cast<NodeGroup*>( collection.get() );
    bool isGroupEditable = true;
    bool groupEdited = true;
    if (isGroup) {
        isGroupEditable = isGroup->isSubGraphEditable();
        groupEdited = isGroup->getNode()->hasPyPlugBeenEdited();
    }
    if (!groupEdited && isGroupEditable) {
        ///check if user is nearby unlock
        // see NodeGraph::paintEvent()
        QPoint pixPos = _imp->getPyPlugUnlockPos();
        int pixW = qRound(_imp->unlockIcon.deviceIndependentSize().width());
        int pixH = qRound(_imp->unlockIcon.deviceIndependentSize().height());
        QRect pixRect(pixPos.x(), pixPos.y(), pixW, pixH);
        pixRect.adjust(-2, -2, 2, 2);
        QRect selRect = pixRect;
        selRect.adjust(-3, -3, 3, 3);
        if ( selRect.contains( e->pos() ) ) {
            assert(isGroup);
            QPoint pos = mapToGlobal( e->pos() );
            // Unfortunately, the timeout delay for the tooltip is hardcoded in Qt 4, and the last parameter to showText doesn't seem to influence anything
            // Can not fix https://github.com/MrKepzie/Natron/issues/1151 (at least in Qt4)
            QToolTip::showText( pos, NATRON_NAMESPACE::convertFromPlainText(QCoreApplication::translate("NodeGraph", "Clicking the unlock button will convert the PyPlug to a regular group saved in the project and detach it from the script.\n"
                                                                                                "Any modification will not be written to the Python script. Subsequent loading of the project will no longer load this group from the python script."), NATRON_NAMESPACE::WhiteSpaceNormal),
                               this, selRect);
        }
    }

    QRectF sceneR = visibleSceneRect();

    bool mustUpdateNavigator = false;
    ///Apply actions
    switch (_imp->_evtState) {
    case eEventStateDraggingArrow: {
        QPointF np = _imp->_arrowSelected->mapFromScene(newPos);
        if ( _imp->_arrowSelected->isOutputEdge() ) {
            _imp->_arrowSelected->dragDest(np);
        } else {
            _imp->_arrowSelected->dragSource(np);
        }
        checkAndStartAutoScrollTimer(newPos);
        mustUpdate = true;
        if (_imp->cursorSet) {
            _imp->cursorSet = false;
            unsetCursor();
        }
        break;
    }
    case eEventStateDraggingNode: {
        mustUpdate = true;
        mustUpdateNavigator = true;
        bool controlDown = modifierHasControl(e);
        bool shiftdown = modifierHasShift(e);
        moveSelectedNodesBy(shiftdown, controlDown, lastMousePosScene, newPos, sceneR, true);
        _imp->cursorSet = true;
        setCursor( QCursor(Qt::ClosedHandCursor) );
        break;
    }
    case eEventStateMovingArea: {
        mustUpdateNavigator = true;
        moveRootInternal(dx, dy);
        mustUpdate = true;
        _imp->cursorSet = true;
        setCursor( QCursor(Qt::SizeAllCursor) );
        break;
    }
    case eEventStateResizingBackdrop: {
        mustUpdateNavigator = true;
        assert(_imp->_backdropResized);
        QPointF p = _imp->_backdropResized->scenePos();
        int w = newPos.x() - p.x();
        int h = newPos.y() - p.y();
        checkAndStartAutoScrollTimer(newPos);
        mustUpdate = true;
        ResizeBackdropCommand* cmd = new ResizeBackdropCommand(_imp->_backdropResized, w, h);
        cmd->setGestureKey(_imp->_dragGesture.key());
        pushUndoCommand(cmd, NodesGuiList(1, _imp->_backdropResized));
        _imp->cursorSet = true;
        setCursor( QCursor(Qt::SizeFDiagCursor) );
        break;
    }
    case eEventStateSelectionRect: {
        QPointF startDrag = _imp->_lastSelectionStartPointScene;
        QPointF cur = newPos;
        double xmin = std::min( cur.x(), startDrag.x() );
        double xmax = std::max( cur.x(), startDrag.x() );
        double ymin = std::min( cur.y(), startDrag.y() );
        double ymax = std::max( cur.y(), startDrag.y() );
        checkAndStartAutoScrollTimer(newPos);
        QRectF selRect(xmin, ymin, xmax - xmin, ymax - ymin);
        _imp->_selectionRect = selRect;
        mustUpdate = true;
        _imp->cursorSet = true;
        setCursor( QCursor(Qt::CrossCursor) );
        break;
    }
    case eEventStateDraggingNavigator: {
        _imp->cursorSet = true;
        setCursor( QCursor(Qt::ClosedHandCursor) );
        QPointF mousePosSceneCoordinates;
        bool insideNavigator = isNearbyNavigator(e->pos(), mousePosSceneCoordinates);
        if (insideNavigator) {
            _imp->_refreshOverlays = true;
            centerOn(mousePosSceneCoordinates);
            _imp->_lastMousePos = e->pos();
            dragPickedUpArrowToCursor();
            update();

            return;
        }
        break;
    }
    case eEventStateZoomingArea: {
        int delta = 2 * ( ( e->position().x() - _imp->_lastMousePos.x() ) - ( e->position().y() - _imp->_lastMousePos.y() ) );
        setTransformationAnchor(QGraphicsView::AnchorViewCenter);
        wheelEventInternal(modCASIsControl(e), delta);
        setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
        mustUpdate = true;
        _imp->cursorSet = true;
        setCursor( QCursor(Qt::SizeAllCursor) );
        break;
    }
    case eEventStateNone:
    default: {
        mustUpdate = false;
        // Test if mouse is inside the navigator
        QPointF mousePosSceneCoordinates;
        bool insideNavigator = isNearbyNavigator(e->pos(), mousePosSceneCoordinates);
        if (insideNavigator) {
            _imp->cursorSet = true;
            setCursor( QCursor(Qt::OpenHandCursor) );
        } else if (!groupEdited) {
            if (_imp->cursorSet) {
                _imp->cursorSet = false;
                unsetCursor();
            }
        } else {
            // Set cursor
            // The cursor should clearly indicate when will happen if mouse is pressed
            NodeGui* nearbyNode = NULL;
            Edge* nearbyEdge = NULL;
            NearbyItemEnum nearbyItemCode = hasItemNearbyMouse(e->pos(), &nearbyNode, &nearbyEdge);

            switch (nearbyItemCode) {
            case eNearbyItemNode:
            case eNearbyItemBackdropFrame:
            case eNearbyItemEdgeBendPoint: {
                _imp->cursorSet = true;
                setCursor( QCursor(Qt::OpenHandCursor) );
                break;
            }
            case eNearbyItemBackdropResizeHandle: {
                _imp->cursorSet = true;
                setCursor( QCursor(Qt::SizeFDiagCursor) );
                break;
            }
            case eNearbyItemNone: {
                _imp->cursorSet = true;
                setCursor( QCursor(Qt::CrossCursor) );
                break;
            }
            case eNearbyItemNodeEdge:
            default: {
                if (_imp->cursorSet) {
                    _imp->cursorSet = false;
                    unsetCursor();
                }
                break;
            }
            }
        }
        break;
    }
    } // switch


    _imp->_lastMousePos = e->pos();

    // Runs after the switch so that a pan or zoom done by this same move is already applied.
    dragPickedUpArrowToCursor();

    if (mustUpdateNavigator) {
        _imp->_refreshOverlays = true;
        mustUpdate = true;
    }

    if (mustUpdate) {
        update();
    }
    QGraphicsView::mouseMoveEvent(e);
} // mouseMoveEvent

NATRON_NAMESPACE_EXIT

