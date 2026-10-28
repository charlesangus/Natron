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
#include <vector>

GCC_DIAG_UNUSED_PRIVATE_FIELD_OFF
CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QAction>
#include <QMouseEvent>
#include <QPainterPath>
#include <QPolygonF>
#include <QString>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)
GCC_DIAG_UNUSED_PRIVATE_FIELD_ON

#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/CreateNodeArgs.h"

#include "Gui/BackdropGui.h"
#include "Gui/Edge.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/GuiMacros.h"
#include "Gui/NodeGui.h"
#include "Gui/NodeSettingsPanel.h"

#include "Global/QtCompat.h"

NATRON_NAMESPACE_ENTER
static NodeGui*
isNodeGuiChild(QGraphicsItem* item)
{
    NodeGui* n = dynamic_cast<NodeGui*>(item);

    if (n) {
        return n;
    }
    if (!item) {
        return 0;
    }
    QGraphicsItem* parent = item->parentItem();
    if (parent) {
        return isNodeGuiChild(parent);
    } else {
        return 0;
    }
}

static Edge*
isEdgeChild(QGraphicsItem* item)
{
    Edge* n = dynamic_cast<Edge*>(item);

    if (n) {
        return n;
    }
    if (!item) {
        return 0;
    }
    QGraphicsItem* parent = item->parentItem();
    if (parent) {
        return isEdgeChild(parent);
    } else {
        return 0;
    }
}

static void
adjustDegenerateRect(QRectF* rect)
{
    // As QGraphicsScene does, so that a horizontal or vertical line still has an area to meet.
    if (!rect->width()) {
        rect->adjust(qreal(-0.00001), 0, qreal(0.00001), 0);
    }
    if (!rect->height()) {
        rect->adjust(0, qreal(-0.00001), 0, qreal(0.00001));
    }
}

// Appends the items of the subtree rooted at item whose shape meets the query, which is what
// QGraphicsScene::items() with Qt::IntersectsItemShape returns for that subtree.
// queryParent bounds the query in item's parent's coordinates; pathScene is the query itself.
static void
collectItemsMeetingQuery(QGraphicsItem* item,
                         const QRectF& queryParent,
                         const QPainterPath& pathScene,
                         bool skipText,
                         std::vector<QGraphicsItem*>* hits)
{
    if (!item->isVisible()) {
        return;
    }
    QRectF itemRect = item->boundingRect();
    adjustDegenerateRect(&itemRect);
    QRectF subtreeRect = item->mapRectToParent(itemRect.united(item->childrenBoundingRect()));
    adjustDegenerateRect(&subtreeRect);
    if (!subtreeRect.intersects(queryParent)) {
        return;
    }
    QRectF queryLocal = item->mapRectFromParent(queryParent);
    bool isText = (item->type() == QGraphicsTextItem::Type) || (item->type() == QGraphicsSimpleTextItem::Type);
    if ((!skipText || !isText) && itemRect.intersects(queryLocal) && (item->effectiveOpacity() >= 0.001)) {
        QPainterPath pathLocal = item->mapFromScene(pathScene);
        // A node's badge letters are text like any other.
        bool meets = (skipText && item->type() == NodeGuiDecorations::Type) ? static_cast<NodeGuiDecorations*>(item)->collidesWithPathIgnoringText(pathLocal) : item->collidesWithPath(pathLocal, Qt::IntersectsItemShape);
        if (meets) {
            hits->push_back(item);
        }
    }
    const QList<QGraphicsItem*> children = item->childItems();
    for (QList<QGraphicsItem*>::const_iterator it = children.begin(); it != children.end(); ++it) {
        collectItemsMeetingQuery(*it, queryLocal, pathScene, skipText, hits);
    }
}

// Nodes and edges are all children of _nodeRoot. Testing them through their bounds in its
// coordinates, which panning leaves alone, costs a few rectangle tests per node, where a scene
// query without an index visits every item of every node.
void
NodeGraph::getGraphItemsMeetingViewportRect(const QRect& rect,
                                            bool skipText,
                                            std::vector<QGraphicsItem*>* hits) const
{
    QPolygonF polygonScene = mapToScene(rect);
    QPainterPath pathScene;
    pathScene.addPolygon(polygonScene);
    pathScene.closeSubpath();
    if (!_imp->_nodeRoot->isVisible()) {
        return;
    }
    QRectF queryRoot = _imp->_nodeRoot->mapRectFromScene(polygonScene.boundingRect());
    const QList<QGraphicsItem*> children = _imp->_nodeRoot->childItems();
    for (QList<QGraphicsItem*>::const_iterator it = children.begin(); it != children.end(); ++it) {
        collectItemsMeetingQuery(*it, queryRoot, pathScene, skipText, hits);
    }
}

void
NodeGraph::getNodesWithinViewportRect(const QRect& rect,
                                      std::set<NodeGui*>* nodes) const
{
    std::vector<QGraphicsItem*> hits;
    getGraphItemsMeetingViewportRect(rect, false, &hits);
    for (std::vector<QGraphicsItem*>::iterator it = hits.begin(); it != hits.end(); ++it) {
        NodeGui* n = isNodeGuiChild(*it);
        if (n) {
            nodes->insert(n);
        }
    }
}

NodeGraph::NearbyItemEnum
NodeGraph::hasItemNearbyMouse(const QPoint& mousePosViewport,
                              NodeGui** node,
                              Edge** edge)
{
    assert(node && edge);
    *node = 0;
    *edge = 0;
    // if mouse is exactly on node, select it
    // do not select text that may go beyond the Node box
    // see https://github.com/MrKepzie/Natron/issues/1604
    std::vector<QGraphicsItem*> hits;
    getGraphItemsMeetingViewportRect(QRect(mousePosViewport, QSize(1, 1)), true, &hits);
    std::set<NodeGui*> nodes;
    for (std::vector<QGraphicsItem*>::iterator it = hits.begin(); it != hits.end(); ++it) {
        NodeGui* n = isNodeGuiChild(*it);
        if (n) {
            nodes.insert(n);
        }
    }
    // The broad-phase square reaches as far as the largest grab radius below (an unconnected
    // input stub's) on every side of the cursor. It still runs against each edge's drawn shape;
    // the radii that decide which one actually wins are applied afterwards, against the nearest
    // point on its line.
    double tolerance = TO_DPIX(18.);
    QRect toleranceRect(mousePosViewport.x() - tolerance,
                        mousePosViewport.y() - tolerance,
                        2. * tolerance,
                        2. * tolerance);
    hits.clear();
    // text is decorative only, see https://github.com/MrKepzie/Natron/issues/1604
    getGraphItemsMeetingViewportRect(toleranceRect, true, &hits);
    std::set<Edge*> edges;
    for (std::vector<QGraphicsItem*>::iterator it = hits.begin(); it != hits.end(); ++it) {
        Edge* isEdge = isEdgeChild(*it);
        if (isEdge) {
            edges.insert(isEdge);
        }
    }

    // first test normal nodes, then backdrops.
    // reason is https://github.com/MrKepzie/Natron/issues/1689
    for (std::set<NodeGui*>::iterator it = nodes.begin(); it != nodes.end(); ++it) {
        if ( (*it)->isVisible() && (*it)->isActive() ) {
            BackdropGui* isBd = dynamic_cast<BackdropGui*>(*it);
            if (!isBd) {
                *node = *it;

                return eNearbyItemNode;
            }
        }
    }
   for (std::set<NodeGui*>::iterator it = nodes.begin(); it != nodes.end(); ++it) {
        if ( (*it)->isVisible() && (*it)->isActive() ) {
            QPointF localPoint = (*it)->mapFromScene( mapToScene(mousePosViewport) );
            BackdropGui* isBd = dynamic_cast<BackdropGui*>(*it);
            if (isBd) {
                if ( isBd->isNearbyNameFrame(localPoint) ) {
                    *node = *it;

                    return eNearbyItemBackdropFrame;
                } else if ( isBd->isNearbyResizeHandle(localPoint) ) {
                    *node = *it;

                    return eNearbyItemBackdropResizeHandle;
                }
            }
        }
    }
    if (!edges.empty()) {
        QPointF scenePos = mapToScene(mousePosViewport);

        // Grab radii are kept in viewport pixels (not scene units) so they don't shrink when the
        // graph is zoomed out. An unconnected input has no pipe to click along, only its stub and
        // free tail, so it gets a bigger one than a connected pipe or an output edge.
        double zoomFactor = transform().mapRect(QRectF(0, 0, 1, 1)).width();
        assert(zoomFactor != 0);
        double grabRadius = TO_DPIX(10.) / zoomFactor;
        double unconnectedInputGrabRadius = TO_DPIX(18.) / zoomFactor;

        std::vector<Edge*> edgeList(edges.begin(), edges.end());
        std::vector<EdgeNearbyCandidate> candidates;
        candidates.reserve(edgeList.size());
        for (std::vector<Edge*>::const_iterator it = edgeList.begin(); it != edgeList.end(); ++it) {
            EdgeNearbyCandidate candidate;
            candidate.line = QLineF((*it)->mapToScene((*it)->line().p1()), (*it)->mapToScene((*it)->line().p2()));
            candidate.isUnconnectedInput = !(*it)->isOutputEdge() && !(*it)->hasSource();
            candidates.push_back(candidate);
        }

        int nearestIndex = pickNearestEdgeCandidate(scenePos, candidates, grabRadius, unconnectedInputGrabRadius);
        if (nearestIndex == -1) {
            return eNearbyItemNone;
        }
        *edge = edgeList[nearestIndex];

        if ( (*edge)->hasSource() && (*edge)->isBendPointVisible() && (*edge)->isNearbyBendPoint(scenePos) ) {
            return eNearbyItemEdgeBendPoint;
        }
        return eNearbyItemNodeEdge;
    }

    return eNearbyItemNone;
} // NodeGraph::hasItemNearbyMouse

void
NodeGraph::mousePressEvent(QMouseEvent* e)
{
    assert(e);

    takeClickFocus();

    _imp->_dragGesture.begin();
    _imp->_hasMovedOnce = false;
    _imp->_pressPos = e->pos();
    _imp->_spliceCancelled = false;
    _imp->_deltaSinceMousePress = QPointF(0, 0);
    if ( buttonDownIsMiddle(e) ) {
        _imp->_evtState = eEventStateMovingArea;

        return;
    }

    bool didSomething = false;

    _imp->_lastMousePos = e->pos();
    const QPointF lastMousePosScene = mapToScene( _imp->_lastMousePos.x(), _imp->_lastMousePos.y() );

    if ( ( (e->buttons() & Qt::MiddleButton) &&
           ( ( buttonMetaAlt(e) == Qt::AltModifier) || (e->buttons() & Qt::LeftButton) ) ) ||
         ( (e->buttons() & Qt::LeftButton) &&
           ( buttonMetaAlt(e) == (Qt::AltModifier | Qt::MetaModifier) ) ) ) {
        // Alt + middle or Left + middle or Crtl + Alt + Left = zoom
        _imp->_evtState = eEventStateZoomingArea;

        return;
    }

    if (hasPickedUpArrow() && handlePickUpPress(e)) {
        _imp->_evtState = eEventStateNone;

        return;
    }

    NodeCollectionPtr collection = getGroup();
    NodeGroup* isGroup = dynamic_cast<NodeGroup*>( collection.get() );
    bool isGroupEditable = true;
    bool groupEdited = true;
    if (isGroup) {
        isGroupEditable = isGroup->isSubGraphEditable();
        groupEdited = isGroup->getNode()->hasPyPlugBeenEdited();
    }


    if (!groupEdited) {
        if (isGroupEditable) {
            ///check if user clicked unlock
            int iw = qRound(_imp->unlockIcon.deviceIndependentSize().width());
            int ih = qRound(_imp->unlockIcon.deviceIndependentSize().height());
            int w = width();
            int offset = 20;
            if ( ( e->position().x() >= (w - iw - 10 - offset) ) && ( e->position().x() <= (w - 10 + offset) ) &&
                 ( e->position().y() >= (10 - offset) ) && ( e->position().y() <= (10 + ih + offset) ) ) {
                assert(isGroup);
                isGroup->getNode()->setPyPlugEdited(true);
                NodesList nodes = isGroup->getNodes();
                for (NodesList::iterator it = nodes.begin(); it != nodes.end(); ++it) {
                    NodeGuiPtr nodeUi = std::dynamic_pointer_cast<NodeGui>( (*it)->getNodeGui() );
                    if (nodeUi) {
                        NodeSettingsPanel* panel = nodeUi->getSettingPanel();
                        if (panel) {
                            panel->setEnabled(true);
                        }
                    }
                }
            }
            getGui()->getApp()->triggerAutoSave();
            update();
        }
    }

    NodeGui* nearbyNode = NULL;
    Edge* nearbyEdge = NULL;
    NearbyItemEnum nearbyItemCode = hasItemNearbyMouse(e->pos(), &nearbyNode, &nearbyEdge);
    if (nearbyItemCode == eNearbyItemBackdropResizeHandle) {
        assert(nearbyNode);
        didSomething = true;
        _imp->_backdropResized = nearbyNode->shared_from_this();
        _imp->_evtState = eEventStateResizingBackdrop;
    }


    if ( ( (nearbyItemCode == eNearbyItemNode) || (nearbyItemCode == eNearbyItemBackdropFrame) ) && groupEdited ) {
        assert(nearbyNode);
//#pragma message WARN("TODO: if control is held, select all the nodes above, see https://github.com/NatronGitHub/Natron/issues/97")
        NodeGuiPtr selectedNode = nearbyNode->shared_from_this();

        didSomething = true;
        if ( buttonDownIsLeft(e) ) {
            BackdropGui* isBd = dynamic_cast<BackdropGui*>( selectedNode.get() );
            if (!isBd) {
                _imp->_magnifiedNode = selectedNode;
            }

            if ( !selectedNode->getIsSelected() ) {
                selectNode( selectedNode, modCASIsShift(e) );
            } else if ( modCASIsShift(e) ) {
                if (_imp->_selection.remove(selectedNode.get())) {
                    selectedNode->setUserSelected(false);
                }
            }
            if ( groupEdited && (_imp->_evtState != eEventStateResizingBackdrop) ) {
                _imp->_evtState = eEventStateDraggingNode;
            }
            ///build the _nodesWithinBDAtPenDown map
            _imp->_nodesWithinBDAtPenDown.clear();
            for (NodesGuiList::iterator it = _imp->_selection.begin(); it != _imp->_selection.end(); ++it) {
                BackdropGui* isBd = dynamic_cast<BackdropGui*>( it->get() );
                if (isBd) {
                    NodesGuiList nodesWithin = getNodesWithinBackdrop(*it);
                    _imp->_nodesWithinBDAtPenDown.insert( std::make_pair(*it, nodesWithin) );
                }
            }
        } else if ( buttonDownIsRight(e) ) {
            if ( !selectedNode->getIsSelected() ) {
                selectNode(selectedNode, true); //< don't wipe the selection
            }
        }
    }

    if ( (nearbyItemCode == eNearbyItemEdgeBendPoint) && groupEdited ) {
        _imp->setNodesBendPointsVisible(false);
        assert(nearbyEdge);
        ///Now connect the node to the edge input
        NodePtr inputNode = nearbyEdge->getSource()->getNode();
        assert(inputNode);
        ///disconnect previous connection
        NodePtr outputNode = nearbyEdge->getDest()->getNode();
        assert(outputNode);
        int inputNb = outputNode->inputIndex(inputNode);
        if (inputNb == -1) {
            return;
        }

        CreateNodeArgs args( PLUGINID_NATRON_DOT, _imp->group.lock() );
        args.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, false);
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
        args.setProperty<bool>(kCreateNodeArgsPropSettingsOpened, false);

        NodePtr dotNode = getGui()->getApp()->createNode(args);
        assert(dotNode);
        NodeGuiIPtr dotNodeGui_i = dotNode->getNodeGui();
        NodeGuiPtr dotNodeGui = std::dynamic_pointer_cast<NodeGui>(dotNodeGui_i);
        assert(dotNodeGui);

        NodesGuiList nodesList;
        nodesList.push_back(dotNodeGui);


        assert( getGui() );
        GuiAppInstancePtr guiApp = getGui()->getApp();
        assert(guiApp);
        ProjectPtr project = guiApp->getProject();
        assert(project);
        bool ok = project->disconnectNodes(inputNode, outputNode);
        if (!ok) {
            return;
        }

        ok = project->connectNodes(0, inputNode, dotNode);
        if (!ok) {
            return;
        }

        ok = project->connectNodes(inputNb, dotNode, outputNode);
        if (!ok) {
            return;
        }

        QPointF pos = dotNodeGui->mapToParent( dotNodeGui->mapFromScene(lastMousePosScene) );

        dotNodeGui->refreshPosition( pos.x(), pos.y() );
        if ( !dotNodeGui->getIsSelected() ) {
            selectNode( dotNodeGui, modCASIsShift(e) );
        }
        pushUndoCommand(new AddMultipleNodesCommand(this, nodesList), nodesList);

        _imp->_evtState = eEventStateDraggingNode;
        didSomething = true;
    } else if ( (nearbyItemCode == eNearbyItemNodeEdge) && groupEdited ) {
        assert(nearbyEdge);
        _imp->_arrowSelected = nearbyEdge;
        didSomething = true;
        _imp->_evtState = eEventStateDraggingArrow;
    }

    ///Test if mouse is inside the navigator
    {
        QPointF mousePosSceneCoordinates;
        bool insideNavigator = isNearbyNavigator(e->pos(), mousePosSceneCoordinates);
        if (insideNavigator) {
            _imp->_refreshOverlays = true;
            centerOn(mousePosSceneCoordinates);
            _imp->_evtState = eEventStateDraggingNavigator;
            didSomething = true;
        }
    }

    ///Don't forget to reset back to null the _backdropResized pointer
    if (_imp->_evtState != eEventStateResizingBackdrop) {
        _imp->_backdropResized.reset();
    }

    if (buttonDownIsRight(e) && groupEdited) {
        showMenu( mapToGlobal( e->pos() ) );
        didSomething = true;
    }
    if (!didSomething) {
        if ( buttonDownIsLeft(e) ) {
            if ( !modCASIsShift(e) ) {
                deselect();
            }
            _imp->_evtState = eEventStateSelectionRect;
            _imp->_lastSelectionStartPointScene = lastMousePosScene;
            _imp->_selectionRect = QRectF(lastMousePosScene.x(), lastMousePosScene.y(), 0, 0);
        } else if ( buttonDownIsMiddle(e) ) {
            _imp->_evtState = eEventStateMovingArea;
            QGraphicsView::mousePressEvent(e);
        }
    }
} // mousePressEvent

NATRON_NAMESPACE_EXIT
