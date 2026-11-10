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

#include <cmath> // abs, ceil, floor
#include <stdexcept>
#include <unordered_set>
#include <vector>

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
#include "Gui/NodeGraphDeleteLinks.h"
#include "Gui/NodeGui.h"
#include "Gui/ViewerGL.h"
#include "Gui/ViewerTab.h"

#include "Global/QtCompat.h"

NATRON_NAMESPACE_ENTER
void
NodeGraph::connectCurrentViewerToSelection(int inputNB,
                                           bool isASide)
{
    ViewerTab* lastUsedViewer =  getLastSelectedViewer();

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


    InspectorNodePtr v;
    if (lastUsedViewer) {
        v = std::dynamic_pointer_cast<InspectorNode>( lastUsedViewer->
                                                        getInternalNode()->getNode() );
    } else {
        CreateNodeArgs args( PLUGINID_NATRON_VIEWER,
                             getGroup() );
        NodePtr viewerNode = getGui()->getApp()->createNode(args);

        if (!viewerNode) {
            return;
        }
        v = std::dynamic_pointer_cast<InspectorNode>(viewerNode);
    }

    if (!v) {
        return;
    }

    ///if the node is no longer active (i.e: it was deleted by the user), don't do anything.
    if ( !v->isActivated() ) {
        return;
    }

    ///get a ptr to the NodeGui
    NodeGuiIPtr gui_i = v->getNodeGui();
    NodeGuiPtr gui = std::dynamic_pointer_cast<NodeGui>(gui_i);
    assert(gui);
    ///if there's no selected node or the viewer is selected, then try refreshing that input nb if it is connected.
    bool viewerAlreadySelected = _imp->_selection.contains(gui);
    if (_imp->_selection.empty() || (_imp->_selection.size() > 1) || viewerAlreadySelected) {
        v->setActiveInputAndRefresh(inputNB, isASide);
        gui->refreshEdges();

        return;
    }

    NodeGuiPtr selected = _imp->_selection.front();


    if ( !selected->getNode()->canOthersConnectToThisNode() ) {
        return;
    }

    ///if the node doesn't have the input 'inputNb' created yet, populate enough input
    ///so it can be created.
    Edge* foundInput = gui->getInputArrow(inputNB);
    assert(foundInput);

    ///and push a connect command to the selected node.
    pushUndoCommand(new ConnectCommand(this, foundInput, foundInput->getSource(), selected),
                    NodesGuiList { foundInput->getDest(), foundInput->getSource(), selected });

} // connectCurrentViewerToSelection

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
            refreshOverlays();
        }
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

namespace {
// Links from nodes that stay to knobs of the nodes in the list; a node reading a node that goes
// away with it, or living inside a group that goes away, breaks nothing.
std::vector<BrokenLink>
collectBrokenLinks(const NodesGuiList& nodesToRemove)
{
    std::unordered_set<const EffectInstance*> effectsToRemove;
    std::unordered_set<const NodeGroup*> groupsToRemove;
    for (NodesGuiList::const_iterator it = nodesToRemove.begin(); it != nodesToRemove.end(); ++it) {
        EffectInstance* effect = (*it)->getNode()->getEffectInstance().get();
        effectsToRemove.insert(effect);
        if (const NodeGroup* grp = (*it)->getNode()->isEffectGroup()) {
            groupsToRemove.insert(grp);
        }
    }

    std::vector<BrokenLink> links;
    for (NodesGuiList::const_iterator it = nodesToRemove.begin(); it != nodesToRemove.end(); ++it) {
        const NodePtr master = (*it)->getNode();
        const KnobsVec& knobs = master->getKnobs();
        const std::string masterName = master->getFullyQualifiedName();
        for (U32 i = 0; i < knobs.size(); ++i) {
            KnobI::ListenerDimsMap listeners;
            knobs[i]->getListeners(listeners);
            for (KnobI::ListenerDimsMap::iterator it2 = listeners.begin(); it2 != listeners.end(); ++it2) {
                KnobIPtr listener = it2->first.lock();
                if (!listener) {
                    continue;
                }
                EffectInstance* isEffect = dynamic_cast<EffectInstance*>(listener->getHolder());
                if (!isEffect || effectsToRemove.count(isEffect)) {
                    continue;
                }
                const NodeCollectionPtr slaveGroup = isEffect->getNode()->getGroup();
                if (!slaveGroup || groupsToRemove.count(dynamic_cast<NodeGroup*>(slaveGroup.get()))) {
                    continue;
                }
                BrokenLink link;
                link.masterNode = masterName;
                link.masterKnob = knobs[i]->getName();
                link.slaveNode = isEffect->getNode()->getFullyQualifiedName();
                link.slaveKnob = listener->getName();
                links.push_back(link);
            }
        }
    }

    return links;
}

bool
confirmBrokenLinks(const NodesGuiList& nodesToRemove)
{
    const QString text = brokenLinksPrompt(collectBrokenLinks(nodesToRemove));
    if (text.isEmpty()) {
        return true;
    }

    return Dialogs::questionDialog(QCoreApplication::translate("NodeGraph", "Delete").toStdString(),
                                   text.toStdString(), false)
        != eStandardButtonNo;
}
} // anonymous namespace

void
NodeGraph::removeNode(const NodeGuiPtr& node)
{
    NodesGuiList nodesToRemove;
    nodesToRemove.push_back(node);
    if (!confirmBrokenLinks(nodesToRemove)) {
        return;
    }

    node->setUserSelected(false);
    pushUndoCommand(new RemoveMultipleNodesCommand(this, nodesToRemove), nodesToRemove);
}

void
NodeGraph::deleteSelection()
{
    if ( !_imp->_selection.empty() ) {
        NodesGuiList nodesToRemove = _imp->_selection;
        std::unordered_set<NodeGui*> nodesToRemoveSet;
        for (NodesGuiList::const_iterator it = nodesToRemove.begin(); it != nodesToRemove.end(); ++it) {
            nodesToRemoveSet.insert(it->get());
        }

        ///For all backdrops also move all the nodes contained within it
        for (NodesGuiList::iterator it = nodesToRemove.begin(); it != nodesToRemove.end(); ++it) {
            NodesGuiList nodesWithinBD = getNodesWithinBackdrop(*it);
            for (NodesGuiList::iterator it2 = nodesWithinBD.begin(); it2 != nodesWithinBD.end(); ++it2) {
                if (nodesToRemoveSet.insert(it2->get()).second) {
                    nodesToRemove.push_back(*it2);
                }
            }
        }

        if (!confirmBrokenLinks(nodesToRemove)) {
            return;
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
        _imp->_selection.remove(n.get());
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
    bool alreadyInSelection = _imp->_selection.contains(n);

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
        magnifiedNodeSelected = _imp->_selection.contains(_imp->_magnifiedNode);
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
    _imp->beginNavigatorBatch();
    {
        QMutexLocker l(&_imp->_nodesMutex);
        for (NodesGuiList::iterator it = _imp->_selection.begin(); it != _imp->_selection.end(); ++it) {
            (*it)->setUserSelected(false);
        }
    }

    _imp->_selection.clear();
    _imp->endNavigatorBatch();
}

void
NodeGraph::updateNavigator()
{
    if (areAllNodesVisible()) {
        _imp->_navigator->hide();

        return;
    }

    if (_imp->navigatorDeferredByDrag && (_imp->_evtState != eEventStateDraggingNode)) {
        _imp->endNavigatorDragDeferral();
    }
    if (_imp->navigatorContentDirty && !_imp->navigatorDeferredByDrag && !_imp->navigatorRenderTimer.isActive()) {
        // Render at once the first time so the navigator never appears empty; later changes are
        // coalesced so that a drag or a batch edit renders the scene once, not once per event.
        _imp->navigatorRenderTimer.start(_imp->navigatorContent.isNull() ? 0 : NATRON_NAVIGATOR_RENDER_DELAY_MS);
    }

    QRectF sceneR;
    double scaleFactor;
    QPointF offset;
    _imp->getNavigatorMapping(&sceneR, &scaleFactor, &offset);

    int navWidth = std::ceil(width() * NATRON_NAVIGATOR_BASE_WIDTH);
    int navHeight = std::ceil(height() * NATRON_NAVIGATOR_BASE_HEIGHT);
    QImage img(navWidth, navHeight, QImage::Format_ARGB32_Premultiplied);
    img.fill(QColor(71, 71, 71, 255));

    {
        QPainter painter(&img);
        QRectF renderArea(offset, QSizeF(std::floor(sceneR.width() * scaleFactor), std::floor(sceneR.height() * scaleFactor)));
        painter.setClipRect(renderArea);
        painter.translate(offset);
        painter.scale(scaleFactor, scaleFactor);
        painter.translate(-sceneR.topLeft());

        if (!_imp->navigatorContent.isNull()) {
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.drawImage(_imp->navigatorContentRect, _imp->navigatorContent);
        }

        QRectF viewRect = visibleSceneRect();
        painter.resetTransform();
        QRectF viewRectNav(offset.x() + (viewRect.x() - sceneR.x()) * scaleFactor,
                           offset.y() + (viewRect.y() - sceneR.y()) * scaleFactor,
                           viewRect.width() * scaleFactor,
                           viewRect.height() * scaleFactor);
        painter.fillRect(viewRectNav, QColor(200, 200, 200, 100));

        QPen p;
        p.setWidth(2);
        p.setBrush(Qt::yellow);
        painter.setPen(p);
        viewRectNav.adjust(2, 2, -2, -2);
        painter.drawRect(viewRectNav);
    }

    _imp->_navigator->setPixmap(QPixmap::fromImage(img));
    _imp->_navigator->show();
} // updateNavigator

void
NodeGraph::invalidateNavigatorContent()
{
    _imp->navigatorContentDirty = true;
    if (_imp->_evtState == eEventStateDraggingNode) {
        // Positions change on every move of the drag; growNodesBoundsForDrag() and the grid's
        // stale items keep up with them until the release ends the deferral.
        _imp->navigatorDeferredByDrag = true;

        return;
    }
    if (_imp->navigatorBatchDepth > 0) {
        _imp->navigatorBatchPending = true;

        return;
    }
    _imp->navigatorDeferredByDrag = false;
    _imp->nodesBoundsDirty = true;
    _imp->itemGridDirty = true;
    if (isLoadingProjectNodes()) {
        // onProjectNodesRestored() invalidates once more.
        return;
    }
    scheduleOverlayRefresh();
    if (!_imp->navigatorRenderTimer.isActive()) {
        _imp->navigatorRenderTimer.start(NATRON_NAVIGATOR_RENDER_DELAY_MS);
    }
    viewport()->update();
}

void
NodeGraph::onNavigatorRenderTimerTimeout()
{
    if (_imp->navigatorDeferredByDrag && (_imp->_evtState != eEventStateDraggingNode)) {
        _imp->endNavigatorDragDeferral();
    }
    if (!_imp->navigatorContentDirty || _imp->navigatorDeferredByDrag || areAllNodesVisible()) {
        return;
    }
    _imp->renderNavigatorContent();
    refreshOverlays();
}

bool
NodeGraph::areAllNodesVisible()
{
    QRectF nodes = _imp->calcNodesBoundingRect();

    return nodes.isNull() || visibleSceneRect().contains(nodes);
}

NATRON_NAMESPACE_EXIT
