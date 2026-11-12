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

#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>

GCC_DIAG_UNUSED_PRIVATE_FIELD_OFF
CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QAction>
#include <QMouseEvent>
#include <QPainterPath>
#include <QPolygonF>
#include <QRegion>
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
#include "Gui/NodeGraphLowDetailLayer.h"
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
// parentOpacity is the opacity item's parent passes on. A candidate, a node or an edge, that the
// low level of detail fades out counts as opaque, since a layer draws it in its place.
static void
collectItemsMeetingQuery(QGraphicsItem* item,
                         const QRectF& queryParent,
                         const QPainterPath& pathScene,
                         bool skipText,
                         bool isCandidate,
                         qreal parentOpacity,
                         std::vector<QGraphicsItem*>* hits)
{
    if (!item->isVisible()) {
        return;
    }
    const qreal opacity = parentOpacity * ((isCandidate && NodeGraphLowDetailLayer::isFaded(item)) ? 1. : item->opacity());
    QRectF itemRect = item->boundingRect();
    adjustDegenerateRect(&itemRect);
    QRectF subtreeRect = item->mapRectToParent(itemRect.united(item->childrenBoundingRect()));
    adjustDegenerateRect(&subtreeRect);
    if (!subtreeRect.intersects(queryParent)) {
        return;
    }
    QRectF queryLocal = item->mapRectFromParent(queryParent);
    bool isText = (item->type() == QGraphicsTextItem::Type) || (item->type() == QGraphicsSimpleTextItem::Type);
    if ((!skipText || !isText) && itemRect.intersects(queryLocal) && (opacity >= 0.001)) {
        QPainterPath pathLocal = item->mapFromScene(pathScene);
        // A node's badge letters are text like any other.
        bool meets = (skipText && item->type() == NodeGuiDecorations::Type) ? static_cast<NodeGuiDecorations*>(item)->collidesWithPathIgnoringText(pathLocal) : item->collidesWithPath(pathLocal, Qt::IntersectsItemShape);
        if (meets) {
            hits->push_back(item);
        }
    }
    const QList<QGraphicsItem*> children = item->childItems();
    for (QList<QGraphicsItem*>::const_iterator it = children.begin(); it != children.end(); ++it) {
        collectItemsMeetingQuery(*it, queryLocal, pathScene, skipText, false, opacity, hits);
    }
}

// The rectangle the grid files an item under, in its parent's coordinates. The margin absorbs
// small changes to a node's children, a label growing for instance, that nothing reports.
static QRectF
gridRectOf(QGraphicsItem* item)
{
    const qreal margin = 8.;
    QRectF itemRect = item->boundingRect();
    adjustDegenerateRect(&itemRect);
    QRectF subtreeRect = item->mapRectToParent(itemRect.united(item->childrenBoundingRect()));
    adjustDegenerateRect(&subtreeRect);

    return subtreeRect.adjusted(-margin, -margin, margin, margin);
}

bool
NodeGraphPrivate::isGridItemValid(const GridItem& gridItem) const
{
    if (!gridItem.owner || !gridItem.item || !_nodes.contains(gridItem.owner)) {
        return false;
    }
    QGraphicsItem* expected = (gridItem.slot == kGridNodeSlot) ? static_cast<QGraphicsItem*>(gridItem.owner) : static_cast<QGraphicsItem*>(gridItem.owner->getInputArrow(gridItem.slot));

    return expected && (expected == gridItem.item) && isInGraph(expected);
}

void
NodeGraphPrivate::refreshItemGrid()
{
    if (navigatorDeferredByDrag && (_evtState != eEventStateDraggingNode)) {
        endNavigatorDragDeferral();
    }
    if (itemGridDirty) {
        itemGrid.clear();
        itemGridStale.clear();
        QMutexLocker l(&_nodesMutex);
        for (NodesGuiList::const_iterator it = _nodes.begin(); it != _nodes.end(); ++it) {
            NodeGui* node = it->get();
            GridItem gridItem;
            gridItem.owner = node;
            gridItem.slot = kGridNodeSlot;
            gridItem.item = node;
            if (isInGraph(node)) {
                itemGrid.insertOrUpdate(gridItem.item, gridItem, gridRectOf(gridItem.item));
            }
            const std::vector<Edge*>& inputs = node->getInputsArrows();
            for (std::size_t i = 0; i < inputs.size(); ++i) {
                gridItem.slot = (int)i;
                gridItem.item = inputs[i];
                if (gridItem.item && isInGraph(gridItem.item)) {
                    itemGrid.insertOrUpdate(gridItem.item, gridItem, gridRectOf(gridItem.item));
                }
            }
            gridItem.slot = -1;
            gridItem.item = node->getOutputArrow();
            if (gridItem.item && isInGraph(gridItem.item)) {
                itemGrid.insertOrUpdate(gridItem.item, gridItem, gridRectOf(gridItem.item));
            }
        }
        itemGridDirty = false;

        return;
    }
    for (std::unordered_map<const QGraphicsItem*, GridItem>::const_iterator it = itemGridStale.begin(); it != itemGridStale.end(); ++it) {
        if (isGridItemValid(it->second)) {
            itemGrid.insertOrUpdate(it->first, it->second, gridRectOf(it->second.item));
        } else {
            itemGrid.remove(it->first);
        }
    }
    itemGridStale.clear();
}

void
NodeGraphPrivate::getGridItems(const QRectF& queryRoot,
                               std::vector<GridItem>* items,
                               bool refresh,
                               GridKindEnum kind)
{
    if (refresh) {
        refreshItemGrid();
    }
    std::vector<GridItem> found;
    itemGrid.query(queryRoot, &found);
    items->reserve(items->size() + found.size());
    for (std::vector<GridItem>::const_iterator it = found.begin(); it != found.end(); ++it) {
        const bool isNode = (it->slot == kGridNodeSlot);
        if (((kind == eGridKindNodes) && !isNode) || ((kind == eGridKindEdges) && isNode)) {
            continue;
        }
        if (isGridItemValid(*it)) {
            items->push_back(*it);
        } else {
            itemGrid.remove(it->item);
        }
    }
}

void
NodeGraphPrivate::getGridCandidates(const QRectF& queryRoot,
                                    std::vector<QGraphicsItem*>* candidates)
{
    std::vector<GridItem> found;
    getGridItems(queryRoot, &found);
    for (std::vector<GridItem>::const_iterator it = found.begin(); it != found.end(); ++it) {
        candidates->push_back(it->item);
    }
    // The graph's own preview pipes belong to no node; there are only ever these three.
    Edge* const graphEdges[] = { _hintInputEdge, _hintOutputEdge, _kissPreviewEdge };
    for (std::size_t i = 0; i < sizeof(graphEdges) / sizeof(graphEdges[0]); ++i) {
        if (graphEdges[i] && (graphEdges[i]->parentItem() == _nodeRoot)) {
            candidates->push_back(graphEdges[i]);
        }
    }
}

void
NodeGraphPrivate::getGridCandidatesForViewportRect(const QRect& rect,
                                                   std::vector<QGraphicsItem*>* candidates)
{
    if (!_nodeRoot->isVisible()) {
        return;
    }
    getGridCandidates(_nodeRoot->mapRectFromScene(_publicInterface->mapToScene(rect).boundingRect()), candidates);
}

void
NodeGraphPrivate::collectGraphItemsMeetingViewportRect(const QRect& rect,
                                                       bool skipText,
                                                       const std::vector<QGraphicsItem*>& candidates,
                                                       std::vector<QGraphicsItem*>* hits) const
{
    if (!_nodeRoot->isVisible()) {
        return;
    }
    QPolygonF polygonScene = _publicInterface->mapToScene(rect);
    QPainterPath pathScene;
    pathScene.addPolygon(polygonScene);
    pathScene.closeSubpath();
    QRectF queryRoot = _nodeRoot->mapRectFromScene(polygonScene.boundingRect());
    const qreal rootOpacity = _nodeRoot->effectiveOpacity();
    for (std::vector<QGraphicsItem*>::const_iterator it = candidates.begin(); it != candidates.end(); ++it) {
        collectItemsMeetingQuery(*it, queryRoot, pathScene, skipText, true, rootOpacity, hits);
    }
}

// Only nodes and edges, all children of _nodeRoot, are looked for. The grid narrows them down to
// those near the rectangle, whose subtrees are then tested item by item.
void
NodeGraph::getGraphItemsMeetingViewportRect(const QRect& rect,
                                            bool skipText,
                                            std::vector<QGraphicsItem*>* hits) const
{
    std::vector<QGraphicsItem*> candidates;
    _imp->getGridCandidatesForViewportRect(rect, &candidates);
    _imp->collectGraphItemsMeetingViewportRect(rect, skipText, candidates, hits);
}

void
NodeGraphPrivate::applyDetailLevel()
{
    lowDetailEdgeLayer->setVisible(lowDetail);
    lowDetailNodeLayer->setVisible(lowDetail);
    // Showing or hiding the layers already repaints them.
    invalidateLowDetailLayers(false);
    // The trash too: a node restored from it must not come back faded at the full level.
    const IndexedNodeGuiList* lists[] = { &_nodes, &_nodesTrash };
    QMutexLocker l(&_nodesMutex);
    for (std::size_t i = 0; i < sizeof(lists) / sizeof(lists[0]); ++i) {
        for (NodesGuiList::const_iterator it = lists[i]->begin(); it != lists[i]->end(); ++it) {
            NodeGui* node = it->get();
            NodeGraphLowDetailLayer::applyDetailLevel(node, true, lowDetail);
            const std::vector<Edge*>& inputs = node->getInputsArrows();
            for (std::size_t j = 0; j < inputs.size(); ++j) {
                NodeGraphLowDetailLayer::applyDetailLevel(inputs[j], false, lowDetail);
            }
            NodeGraphLowDetailLayer::applyDetailLevel(node->getOutputArrow(), false, lowDetail);
        }
    }
}

void
NodeGraph::refreshDetailLevel()
{
    const bool lowDetail = std::abs(transform().m22()) * _imp->lowDetailFontHeight < NATRON_NODEGRAPH_LOW_DETAIL_TEXT_HEIGHT_PX;
    if (lowDetail == _imp->lowDetail) {
        return;
    }
    _imp->lowDetail = lowDetail;
    _imp->applyDetailLevel();
}

void
NodeGraph::repaintLowDetailItem(QGraphicsItem* item)
{
    if (!_imp->lowDetail || !NodeGraphLowDetailLayer::isFaded(item) || !_imp->isInGraph(item)) {
        return;
    }
    _imp->addLowDetailDirty(gridRectOf(item));
    // Where it was filed, a separate rect: a node created at one place and placed at another, far
    // away, would otherwise repaint everything between the two.
    QRectF filed;
    if (_imp->itemGrid.rectOf(item, &filed)) {
        _imp->addLowDetailDirty(filed);
    }
}

void
NodeGraphPrivate::addLowDetailDirty(const QRectF& rect)
{
    if (!lowDetail || rect.isEmpty()) {
        return;
    }
    NodeGraphLowDetailLayer::addDirtyRect(&lowDetailDirty, rect);
    if (!lowDetailFlushPosted) {
        lowDetailFlushPosted = true;
        // A posted call rather than a zero timer: it runs before a repaint the change itself
        // queues, so that repaint already includes the layers.
        QMetaObject::invokeMethod(_publicInterface, [this]() {
            lowDetailFlushPosted = false;
            flushLowDetailUpdates(); }, Qt::QueuedConnection);
    }
}

void
NodeGraphPrivate::invalidateLowDetailLayers(bool repaint)
{
    lowDetailDirty.clear();
    lowDetailEdgeLayer->invalidateCache();
    lowDetailNodeLayer->invalidateCache();
    if (repaint && lowDetail) {
        lowDetailEdgeLayer->update();
        lowDetailNodeLayer->update();
    }
}

void
NodeGraphPrivate::flushLowDetailUpdates(const QRegion* painting)
{
    if (!lowDetail) {
        lowDetailDirty.clear();

        return;
    }
    refreshItemGrid();
    if (lowDetailDirty.empty()) {
        return;
    }
    std::vector<QRectF> dirtyRects;
    dirtyRects.swap(lowDetailDirty);
    const QRect viewportRect = _publicInterface->viewport()->rect();
    for (std::vector<QRectF>::const_iterator it = dirtyRects.begin(); it != dirtyRects.end(); ++it) {
        const QRectF& dirty = *it;
        lowDetailEdgeLayer->invalidateCache(dirty);
        lowDetailNodeLayer->invalidateCache(dirty);
        if (painting) {
            const QRect dirtyViewport = _publicInterface->mapFromScene(_nodeRoot->mapRectToScene(dirty)).boundingRect().adjusted(-1, -1, 1, 1);
            if (QRegion(dirtyViewport & viewportRect).subtracted(*painting).isEmpty()) {
                continue;
            }
        }
        lowDetailEdgeLayer->update(dirty);
        lowDetailNodeLayer->update(dirty);
    }
}

void
NodeGraph::noteNodeGeometryChanged(NodeGui* node)
{
    if (!node) {
        return;
    }
    NodeGraphLowDetailLayer::applyDetailLevel(node, true, _imp->lowDetail);
    repaintLowDetailItem(node);
    _imp->nodesBoundsPending.insert(node);
    if (_imp->itemGridDirty) {
        return;
    }
    NodeGraphPrivate::GridItem gridItem;
    gridItem.item = node;
    gridItem.owner = node;
    gridItem.slot = NodeGraphPrivate::kGridNodeSlot;
    _imp->itemGridStale[gridItem.item] = gridItem;
}

void
NodeGraph::noteEdgeGeometryChanged(NodeGui* owner,
                                   int slot,
                                   Edge* edge)
{
    if (!owner || !edge) {
        return;
    }
    NodeGraphLowDetailLayer::applyDetailLevel(edge, false, _imp->lowDetail);
    repaintLowDetailItem(edge);
    // An input without a source counts in its owner's bounds.
    _imp->nodesBoundsPending.insert(owner);
    if (_imp->itemGridDirty) {
        return;
    }
    NodeGraphPrivate::GridItem gridItem;
    gridItem.item = edge;
    gridItem.owner = owner;
    gridItem.slot = slot;
    _imp->itemGridStale[gridItem.item] = gridItem;
}

void
NodeGraph::noteNodeAdded(NodeGui* node)
{
    if (!node) {
        return;
    }
    noteNodeGeometryChanged(node);
    const std::vector<Edge*>& inputs = node->getInputsArrows();
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        noteEdgeGeometryChanged(node, (int)i, inputs[i]);
    }
    noteEdgeGeometryChanged(node, -1, node->getOutputArrow());
    invalidateNavigatorImage();
}

void
NodeGraph::noteNodeRemoved(NodeGui* node)
{
    if (!node) {
        return;
    }
    std::vector<QGraphicsItem*> items;
    items.push_back(node);
    const std::vector<Edge*>& inputs = node->getInputsArrows();
    items.insert(items.end(), inputs.begin(), inputs.end());
    items.push_back(node->getOutputArrow());
    for (std::vector<QGraphicsItem*>::const_iterator it = items.begin(); it != items.end(); ++it) {
        if (!*it) {
            continue;
        }
        QRectF filed;
        if (_imp->itemGrid.rectOf(*it, &filed)) {
            _imp->addLowDetailDirty(filed);
        }
        // Dropped now rather than left to the next refresh: a node being destroyed takes its
        // edges with it, and the grid would then hold their addresses.
        _imp->itemGrid.remove(*it);
        _imp->itemGridStale.erase(*it);
    }
    _imp->nodesBoundsPending.insert(node);
    invalidateNavigatorImage();
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
    // The broad-phase square reaches as far as the largest grab radius below (an unconnected
    // input stub's) on every side of the cursor. It still runs against each edge's drawn shape;
    // the radii that decide which one actually wins are applied afterwards, against the nearest
    // point on its line.
    double tolerance = TO_DPIX(18.);
    QRect toleranceRect(mousePosViewport.x() - tolerance,
                        mousePosViewport.y() - tolerance,
                        2. * tolerance,
                        2. * tolerance);
    // One grid query serves both the point and the square around it.
    std::vector<QGraphicsItem*> candidates;
    _imp->getGridCandidatesForViewportRect(toleranceRect.united(QRect(mousePosViewport, QSize(1, 1))), &candidates);

    std::vector<QGraphicsItem*> hits;
    _imp->collectGraphItemsMeetingViewportRect(QRect(mousePosViewport, QSize(1, 1)), true, candidates, &hits);
    std::set<NodeGui*> nodes;
    for (std::vector<QGraphicsItem*>::iterator it = hits.begin(); it != hits.end(); ++it) {
        NodeGui* n = isNodeGuiChild(*it);
        if (n) {
            nodes.insert(n);
        }
    }
    hits.clear();
    // text is decorative only, see https://github.com/MrKepzie/Natron/issues/1604
    _imp->collectGraphItemsMeetingViewportRect(toleranceRect, true, candidates, &hits);
    std::set<Edge*> edges;
    for (std::vector<QGraphicsItem*>::iterator it = hits.begin(); it != hits.end(); ++it) {
        Edge* isEdge = isEdgeChild(*it);
        if (isEdge) {
            edges.insert(isEdge);
        }
    }

    // first test normal nodes, then backdrops.
    // reason is https://github.com/MrKepzie/Natron/issues/1689
    // isActive() would be the scene's activation, which resetting a project leaves off for good.
    for (std::set<NodeGui*>::iterator it = nodes.begin(); it != nodes.end(); ++it) {
        if ((*it)->isVisible()) {
            BackdropGui* isBd = dynamic_cast<BackdropGui*>(*it);
            if (!isBd) {
                *node = *it;

                return eNearbyItemNode;
            }
        }
    }
   for (std::set<NodeGui*>::iterator it = nodes.begin(); it != nodes.end(); ++it) {
       if ((*it)->isVisible()) {
           QPointF localPoint = (*it)->mapFromScene(mapToScene(mousePosViewport));
           BackdropGui* isBd = dynamic_cast<BackdropGui*>(*it);
           if (isBd) {
               if (isBd->isNearbyNameFrame(localPoint)) {
                   *node = *it;

                   return eNearbyItemBackdropFrame;
               } else if (isBd->isNearbyResizeHandle(localPoint)) {
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
    _imp->_kissCancelled = false;
    _imp->_kissAiming = false;
    _imp->_shakeDetector.reset();
    _imp->_dragClock.start();
    _imp->_deltaSinceMousePress = QPointF(0, 0);
    _imp->_dragPickupPositions.clear();
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
        if (buttonDownIsLeft(e) && modCASIsControl(e) && groupEdited) {
            clearCutLine();
            _imp->_evtState = eEventStateCutting;
            extendCutLine(lastMousePosScene);
        } else if (buttonDownIsLeft(e)) {
            if ( !modCASIsShift(e) ) {
                deselect();
            }
            _imp->_evtState = eEventStateSelectionRect;
            _imp->_lastSelectionStartPointScene = lastMousePosScene;
            _imp->_selectionRect = QRectF(lastMousePosScene.x(), lastMousePosScene.y(), 0, 0);
        } else if (buttonDownIsMiddle(e)) {
            _imp->_evtState = eEventStateMovingArea;
            QGraphicsView::mousePressEvent(e);
        }
    }
} // mousePressEvent

NATRON_NAMESPACE_EXIT
