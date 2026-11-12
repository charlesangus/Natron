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

#include <algorithm> // min, max
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <locale>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QPaintEvent>
#include <QPolygonF>
#include <QScrollBar>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/Backdrop.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/Dot.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Project.h"
#include "Engine/Settings.h"
#include "Engine/TimeLine.h"

#include "Gui/BackdropGui.h"
#include "Gui/DotGui.h"
#include "Gui/Edge.h"
#include "Gui/GpuOverlay.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/LoopBlockOverlay.h"
#include "Gui/Menu.h"
#include "Gui/NodeGraphLowDetailLayer.h"
#include "Gui/NodeGraphTextItem.h"
#include "Gui/NodeGui.h"
#include "Gui/UndoHistory.h"

#include "Global/QtCompat.h"

#include "Serialization/SerializationFwd.h"

NATRON_NAMESPACE_ENTER
//using std::cout; using std::endl;


void
NodeGraph::makeFullyQualifiedLabel(Node* node,
                                   std::string* ret)
{
    NodeCollectionPtr parent = node->getGroup();
    NodeGroup* isParentGrp = dynamic_cast<NodeGroup*>( parent.get() );
    std::string toPreprend = node->getLabel();

    if (isParentGrp) {
        toPreprend.insert(0, "/");
    }
    ret->insert(0, toPreprend);
    if (isParentGrp) {
        makeFullyQualifiedLabel(isParentGrp->getNode().get(), ret);
    }
}

NodeGraph::NodeGraph(Gui* gui,
                     const NodeCollectionPtr& group,
                     QGraphicsScene* scene,
                     QWidget *parent)
    : QGraphicsView(scene, parent)
    , NodeGraphI()
    , PanelWidget(this, gui)
    , _imp( new NodeGraphPrivate(this, group) )
{
    group->setNodeGraphPointer(this);

    setAttribute(Qt::WA_MacShowFocusRect, 0);

    NodeGroup* isGrp = dynamic_cast<NodeGroup*>( group.get() );
    if (isGrp) {
        std::string newName = isGrp->getNode()->getFullyQualifiedName();
        for (std::size_t i = 0; i < newName.size(); ++i) {
            if (newName[i] == '.') {
                newName[i] = '_';
            }
        }
        setScriptName(newName);
        std::string label;
        makeFullyQualifiedLabel(isGrp->getNode().get(), &label);
        setLabel(label);
        QObject::connect( isGrp->getNode().get(), SIGNAL(labelChanged(QString)), this, SLOT(onGroupNameChanged(QString)) );
        QObject::connect( isGrp->getNode().get(), SIGNAL(scriptNameChanged(QString)), this, SLOT(onGroupScriptNameChanged(QString)) );
    } else {
        setScriptName(kNodeGraphObjectName);
        setLabel( tr("Node Graph").toStdString() );
    }

    QObject::connect( &_imp->autoScrollTimer, SIGNAL(timeout()), this, SLOT(onAutoScrollTimerTriggered()) );

    _imp->overlayRefreshTimer.setSingleShot(true);
    QObject::connect(&_imp->overlayRefreshTimer, &QTimer::timeout, this, [this]() {
        refreshOverlays();
    });

    setMouseTracking(true);
    setCacheMode(CacheBackground);
    // Any mode but FullViewportUpdate lets a scroll blit the viewport and paint only the exposed
    // strip; the pan relies on that.
    setViewportUpdateMode(QGraphicsView::BoundingRectViewportUpdate);
    setRenderHint(QPainter::Antialiasing);
    // Not AnchorUnderMouse: hover moves skip QGraphicsView::mouseMoveEvent, so Qt's last mouse
    // scene point goes stale; the wheel zoom pivots on the cursor by hand instead.
    setTransformationAnchor(QGraphicsView::AnchorViewCenter);
    // Fixed, so that items never grow or shrink the scrollable area and panning is a scroll.
    setSceneRect(QRectF(NATRON_SCENE_MIN, NATRON_SCENE_MIN, NATRON_SCENE_MAX - NATRON_SCENE_MIN, NATRON_SCENE_MAX - NATRON_SCENE_MIN));
    scale(0.8, 0.8);

    _imp->_root = new NodeGraphTextItem(this, 0, false);
    _imp->_nodeRoot = new NodeGraphTextItem(this, _imp->_root, false);
    scene->addItem(_imp->_root);
    _imp->_loopBlocks = new LoopBlockOverlay(this, _imp->_nodeRoot);
    _imp->_gpuRegions = new GpuRegionOverlay(this, _imp->_nodeRoot);

    _imp->_navigator = new Navigator(&_imp->isDoingPreviewRender);
    scene->addItem(_imp->_navigator);
    _imp->_navigator->setFlag(QGraphicsItem::ItemIgnoresTransformations);
    _imp->_navigator->hide();


    _imp->_cacheSizeText = new NodeGraphSimpleTextItem(this, 0, true);
    scene->addItem(_imp->_cacheSizeText);
    _imp->_cacheSizeText->setFlag(QGraphicsItem::ItemIgnoresTransformations);
    _imp->_cacheSizeText->setBrush( QColor(200, 200, 200) );
    _imp->_cacheSizeText->setVisible(false);

    _imp->navigatorRenderTimer.setSingleShot(true);
    QObject::connect(&_imp->navigatorRenderTimer, &QTimer::timeout, this, [this]() {
        onNavigatorRenderTimerTimeout();
    });

    _imp->refreshRenderStateTimer.setSingleShot(true);
    QObject::connect(&_imp->refreshRenderStateTimer, SIGNAL(timeout()), this, SLOT(onRefreshNodesRenderStateTimerTimeout()));

    QObject::connect( &_imp->_refreshCacheTextTimer, SIGNAL(timeout()), this, SLOT(updateCacheSizeText()) );
    _imp->_refreshCacheTextTimer.start(NATRON_CACHE_SIZE_TEXT_REFRESH_INTERVAL_MS);

    QObject::connect(appPTR->getCurrentSettings().get(), SIGNAL(settingsSaved()), this, SLOT(onSettingsSaved()));

    _imp->_hintInputEdge = new Edge(0, 0, NodeGuiPtr(), _imp->_nodeRoot);
    _imp->_hintInputEdge->setIsSpliceHint(true);
    _imp->_hintInputEdge->hide();

    _imp->_hintOutputEdge = new Edge(0, 0, NodeGuiPtr(), _imp->_nodeRoot);
    _imp->_hintOutputEdge->setIsSpliceHint(true);
    _imp->_hintOutputEdge->hide();

    _imp->_kissPreviewEdge = new Edge(0, 0, NodeGuiPtr(), _imp->_nodeRoot);
    _imp->_kissPreviewEdge->setUseHighlight(true);
    // Above the other pipes, which share z 15 and would otherwise paint over it in creation order
    // (the dragged node's own free input arrow lies along the preview), but still under the nodes.
    _imp->_kissPreviewEdge->setZValue(16);
    _imp->_kissPreviewEdge->hide();

    // Created after the graph's own pipes, so that at equal z the nodes' pipes still draw above
    // them, as they do when they draw themselves.
    _imp->lowDetailEdgeLayer = new NodeGraphLowDetailLayer(_imp.get(), NodeGraphLowDetailLayer::ePassEdges, _imp->_nodeRoot);
    _imp->lowDetailNodeLayer = new NodeGraphLowDetailLayer(_imp.get(), NodeGraphLowDetailLayer::ePassNodes, _imp->_nodeRoot);
    // After the edge layer, so that at equal z the nodes' edges draw above the graph's own pipes
    // and the layer, and the link arrows above the edges.
    _imp->backdropLinkArrowLayer = new NodeGraphItemLayer(_imp->_nodeRoot, 5);
    _imp->edgeLayer = new NodeGraphItemLayer(_imp->_nodeRoot, 15);
    _imp->linkArrowLayer = new NodeGraphItemLayer(_imp->_nodeRoot, 15);
    refreshDetailLevel();

    centerOn(0, 0);

    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    setAcceptDrops(false);

    _imp->_menu = new Menu(this);
}

NodeGraph::~NodeGraph()
{
    for (NodesGuiList::iterator it = _imp->_nodes.begin();
         it != _imp->_nodes.end();
         ++it) {
        (*it)->discardGraphPointer();
    }
    for (NodesGuiList::iterator it = _imp->_nodesTrash.begin();
         it != _imp->_nodesTrash.end();
         ++it) {
        (*it)->discardGraphPointer();
    }

    if ( getGui() ) {
        QGraphicsScene* scene = _imp->_hintInputEdge->scene();
        if (scene) {
            scene->removeItem(_imp->_hintInputEdge);
        }
        _imp->_hintInputEdge->setParentItem(NULL);
        delete _imp->_hintInputEdge;

        scene = _imp->_hintOutputEdge->scene();
        if (scene) {
            scene->removeItem(_imp->_hintOutputEdge);
        }
        _imp->_hintOutputEdge->setParentItem(NULL);
        delete _imp->_hintOutputEdge;

        scene = _imp->_kissPreviewEdge->scene();
        if (scene) {
            scene->removeItem(_imp->_kissPreviewEdge);
        }
        _imp->_kissPreviewEdge->setParentItem(NULL);
        delete _imp->_kissPreviewEdge;
    }

    delete _imp->_kissCommand;
    _imp->_kissCommand = nullptr;

    QObject::disconnect( &_imp->_refreshCacheTextTimer, SIGNAL(timeout()), this, SLOT(updateCacheSizeText()) );
    _imp->_nodeCreationShortcutEnabled = false;
}

void
NodeGraph::onSettingsSaved()
{
    // Recolouring on save rather than on settingChanged() keeps a discarded Preferences edit off the graph.
    _imp->antiAliasing = appPTR->getCurrentSettings()->isNodeGraphAntiAliasingEnabled();
    for (NodesGuiList::const_iterator it = _imp->_nodes.begin(); it != _imp->_nodes.end(); ++it) {
        (*it)->refreshCategoryColor();
        (*it)->refreshEdgesDataKindPen(true);
        (*it)->refreshLinkLinesAppearance();
    }
    for (NodesGuiList::const_iterator it = _imp->_nodesTrash.begin(); it != _imp->_nodesTrash.end(); ++it) {
        (*it)->refreshCategoryColor();
        (*it)->refreshEdgesDataKindPen(true);
        (*it)->refreshLinkLinesAppearance();
    }
    // scene()->update() below repaints everything.
    _imp->invalidateLowDetailLayers(false);
    if (scene()) {
        scene()->update();
    }
}

bool
NodeGraph::isDoingNavigatorRender() const
{
    return _imp->isDoingPreviewRender;
}

bool
NodeGraph::isAntiAliasingEnabled() const
{
    return _imp->antiAliasing;
}

bool
NodeGraph::isLowDetail() const
{
    return _imp->lowDetail;
}

const std::list<NodeGuiPtr> &
NodeGraph::getSelectedNodes() const
{
    return _imp->_selection;
}

NodeCollectionPtr
NodeGraph::getGroup() const
{
    return _imp->group.lock();
}

QGraphicsItem*
NodeGraph::getRootItem() const
{
    return _imp->_root;
}

QGraphicsItem*
NodeGraph::getEdgeLayer() const
{
    return _imp->edgeLayer;
}

QGraphicsItem*
NodeGraph::getLinkArrowLayer(bool fromBackdrop) const
{
    return fromBackdrop ? _imp->backdropLinkArrowLayer : _imp->linkArrowLayer;
}

void
NodeGraph::scheduleLoopBlockRefresh(bool topology)
{
    if (_imp->_loopBlocks) {
        _imp->_loopBlocks->schedule(topology);
    }
    if (topology) {
        // Moves and resizes reach the GPU overlay through the geometry notes, region by region.
        if (_imp->_gpuRegions) {
            _imp->_gpuRegions->schedule(true);
        }
        Q_EMIT topologyChanged();
    }
}

void
NodeGraph::notifyGuiClosing()
{
    NodeCollectionPtr group = getGroup();

    if (group) {
        group->discardNodeGraphPointer();
    }
}

void
NodeGraph::onNodesCleared()
{
    {
        QMutexLocker l(&_imp->_nodesMutex);
        _imp->_nodes.clear();
        _imp->_nodesTrash.clear();
    }
    invalidateNavigatorContent();

    _imp->_selection.clear();
    _imp->_magnifiedNode.reset();
}

void
NodeGraph::addNodesToCreationOf(const NodePtr& created,
                                const NodesList& added)
{
    const NodeGuiPtr owner = created ? std::dynamic_pointer_cast<NodeGui>(created->getNodeGui()) : NodeGuiPtr();
    NodesGuiList guis;

    for (NodesList::const_iterator it = added.begin(); it != added.end(); ++it) {
        if (const NodeGuiPtr gui = *it ? std::dynamic_pointer_cast<NodeGui>((*it)->getNodeGui()) : NodeGuiPtr()) {
            guis.push_back(gui);
        }
    }
    if (!owner || guis.empty()) {
        return;
    }
    AddMultipleNodesCommand* command = new AddMultipleNodesCommand(this, guis);
    command->setCreatedFor(owner);
    pushUndoCommand(command, guis);
}

void
NodeGraph::resizeEvent(QResizeEvent* e)
{
    // The thumbnail is rendered at the navigator's size, which follows the view's.
    invalidateNavigatorContent();
    QGraphicsView::resizeEvent(e);
    refreshOverlays();
}

void
NodeGraph::scrollContentsBy(int dx,
                            int dy)
{
    QGraphicsView::scrollContentsBy(dx, dy);
    if (isDrawingLockedVeil()) {
        // The unlock button is painted at a fixed place in the viewport, so a blit would move it.
        viewport()->update();
    }
    if (!_imp->overlayRefreshSuspended) {
        refreshOverlays();
    }
}

void
NodeGraph::panContentBy(const QPoint& delta)
{
    if (delta.isNull()) {
        return;
    }
    _imp->overlayRefreshSuspended = true;
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
    verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
    _imp->overlayRefreshSuspended = false;
    refreshOverlays();
}

void
NodeGraph::scheduleOverlayRefresh()
{
    if (!_imp->overlayRefreshTimer.isActive()) {
        _imp->overlayRefreshTimer.start(0);
    }
}

void
NodeGraph::refreshOverlays()
{
    // QGraphicsView scrolls while the constructor sets the view up, before the overlays exist.
    if (!_imp->_navigator || !_imp->_cacheSizeText) {
        return;
    }
    _imp->overlayRefreshTimer.stop();

    QRectF visibleScene = visibleSceneRect();
    QRect visibleWidget = visibleWidgetRect();

    _imp->_cacheSizeText->setPos(visibleScene.topLeft());

    double navWidth = NATRON_NAVIGATOR_BASE_WIDTH * width();
    double navHeight = NATRON_NAVIGATOR_BASE_HEIGHT * height();
    QPoint btmRightWidget = visibleWidget.bottomRight();
    QPoint navTopLeftWidget = btmRightWidget - QPoint(navWidth, navHeight);
    QPointF navTopLeftScene = mapToScene(navTopLeftWidget);

    _imp->_navigator->refreshPosition(navTopLeftScene, navWidth, navHeight);
    updateNavigator();
}

bool
NodeGraph::isDrawingLockedVeil() const
{
    NodeCollectionPtr collection = getGroup();
    NodeGroup* isGroup = dynamic_cast<NodeGroup*>( collection.get() );
    if (!isGroup || !isGroup->getNode()->isPyPlug()) {
        return false;
    }

    return !isGroup->isSubGraphEditable() || !isGroup->getNode()->hasPyPlugBeenEdited();
}

void
NodeGraph::paintEvent(QPaintEvent* e)
{
    AppInstancePtr app;

    if (getGui()) {
        app = getGui()->getApp();
    }
    if (app && app->getProject()->isLoadingProjectInternal()) {
        return;
    }

    NodeCollectionPtr collection = getGroup();
    NodeGroup* isGroup = dynamic_cast<NodeGroup*>(collection.get());
    const bool drawLockedMode = isDrawingLockedVeil();
    const bool isGroupEditable = !isGroup || !isGroup->getNode()->isPyPlug() || isGroup->isSubGraphEditable();

    // The zooms refresh it as they happen; this catches any other change to the transform.
    refreshDetailLevel();
    _imp->flushLowDetailUpdates(&e->region());

    QGraphicsView::paintEvent(e);

    if (drawLockedMode) {
        /// Show a semi-transparent foreground indicating the PyPlug has not been edited
        QPainter p( viewport() );
        p.setBrush( QColor(120, 120, 120) );
        p.setOpacity(0.7);
        p.drawRect( rect() );

        if (isGroupEditable) {
            ///Draw the unlock icon
            QPoint pixPos = _imp->getPyPlugUnlockPos();
            int pixW = qRound(_imp->unlockIcon.deviceIndependentSize().width());
            int pixH = qRound(_imp->unlockIcon.deviceIndependentSize().height());
            QRect pixRect(pixPos.x(), pixPos.y(), pixW, pixH);
            pixRect.adjust(-2, -2, 2, 2);
            QRect selRect = pixRect;
            selRect.adjust(-3, -3, 3, 3);
            p.setBrush( QColor(243, 137, 0) );
            p.setOpacity(1.);
            p.setPen(Qt::NoPen);
            p.drawRoundedRect(selRect, 5, 5);
            p.setBrush( QColor(100, 100, 100) );
            p.drawRoundedRect(pixRect, 5, 5);
            p.drawPixmap(pixPos.x(), pixPos.y(), pixW, pixH, _imp->unlockIcon, 0, 0, _imp->unlockIcon.width(), _imp->unlockIcon.height());
        }
    }

    if ( (_imp->_evtState == eEventStateSelectionRect) && !isDoingNavigatorRender() ) {
        QPainter p( viewport() );
        const QRect r = mapFromScene(_imp->_selectionRect).boundingRect();
        QColor color(16, 84, 200, 20);
        p.setBrush(color);
        p.drawRect(r);
        double w = p.pen().widthF();
        p.fillRect(QRect(r.x() + w, r.y() + w, r.width() - w, r.height() - w), color);
    }

    if ((_imp->_evtState == eEventStateCutting) && (_imp->_cutLine.size() > 1) && !isDoingNavigatorRender()) {
        QPainter p(viewport());
        p.setRenderHint(QPainter::Antialiasing);
        QPen pen(Edge::selectionColorWithAlpha());
        pen.setWidthF(TO_DPIX(2.));
        pen.setStyle(Qt::DashLine);
        p.setPen(pen);
        QPolygonF line;
        for (std::size_t i = 0; i < _imp->_cutLine.size(); ++i) {
            line << mapFromScene(_imp->_cutLine[i]);
        }
        p.drawPolyline(line);
    }
} // NodeGraph::paintEvent

QRectF
NodeGraph::visibleSceneRect() const
{
    return mapToScene( visibleWidgetRect() ).boundingRect();
}

QRect
NodeGraph::visibleWidgetRect() const
{
    return viewport()->rect();
}

NodeGuiPtr
NodeGraph::createNodeGUI(const NodePtr & node,
                         const CreateNodeArgs& args)
{
    NodeGuiPtr node_ui;
    Dot* isDot = dynamic_cast<Dot*>( node->getEffectInstance().get() );
    Backdrop* isBd = dynamic_cast<Backdrop*>( node->getEffectInstance().get() );
    NodeGroup* isGrp = dynamic_cast<NodeGroup*>( node->getEffectInstance().get() );


    if (isDot) {
        node_ui.reset( new DotGui(_imp->_nodeRoot) );
    } else if (isBd) {
        node_ui.reset( new BackdropGui(_imp->_nodeRoot) );
    } else {
        node_ui.reset( new NodeGui(_imp->_nodeRoot) );
    }
    assert(node_ui);
    node_ui->initialize(this, node);

    // A backdrop from a serialization already has the layout it was saved with.
    if (isBd && !args.getProperty<Serialization::NodeSerializationPtr>(kCreateNodeArgsPropNodeSerialization)) {
        BackdropGui* bd = dynamic_cast<BackdropGui*>( node_ui.get() );
        assert(bd);
        NodesGuiList selectedNodes = _imp->_selection;
        if ( bd && !selectedNodes.empty() ) {
            ///make the backdrop large enough to contain the selected nodes and position it correctly
            QRectF bbox;
            for (NodesGuiList::iterator it = selectedNodes.begin(); it != selectedNodes.end(); ++it) {
                QRectF nodeBbox = (*it)->mapToScene((*it)->bodyRect()).boundingRect();
                bbox = bbox.united(nodeBbox);
            }

            double border50 = mapToScene( QPoint(50, 0) ).x();
            double border0 = mapToScene( QPoint(0, 0) ).x();
            double border = border50 - border0;
            double headerHeight = bd->getFrameNameHeight();
            QPointF scenePos(bbox.x() - border, bbox.y() - border);

            bd->setPos( bd->mapToParent( bd->mapFromScene(scenePos) ) );
            bd->resizeByUser(bbox.width() + 2 * border, bbox.height() + 2 * border - headerHeight);
        }
    }

    {
        QMutexLocker l(&_imp->_nodesMutex);
        _imp->_nodes.push_back(node_ui);
    }
    invalidateNavigatorContent();
    scheduleLoopBlockRefresh(true);

    //NodeGroup* parentIsGroup = dynamic_cast<NodeGroup*>(node->getGroup().get());;
    const std::list<NodePtr>& nodesBeingCreated = getGui()->getApp()->getNodesBeingCreated();
    bool isTopLevelNodeBeingCreated = false;
    if ( !nodesBeingCreated.empty() && (nodesBeingCreated.front() == node) ) {
        isTopLevelNodeBeingCreated = true;
    }

    Serialization::NodeSerializationPtr serialization = args.getProperty<Serialization::NodeSerializationPtr>(kCreateNodeArgsPropNodeSerialization);

    // A Dot is a routing aid created in passing, and every project starts with a Viewer, so their
    // panels only open when asked for.
    bool isViewer = node->isEffectViewer() != 0;
    bool panelOpened = args.getProperty<bool>(kCreateNodeArgsPropSettingsOpened) && !isDot && !isViewer;
    if ( !serialization && panelOpened && isTopLevelNodeBeingCreated ) {
        node_ui->ensurePanelCreated();
    }

    bool addUndoRedo = args.getProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand);
    if (addUndoRedo) {
        pushUndoCommand(new AddMultipleNodesCommand(this, node_ui), NodesGuiList(1, node_ui));
    } else if ( !serialization && !isGrp ) {
        selectNode(node_ui, false);
    }

    _imp->_evtState = eEventStateNone;

    return node_ui;
} // NodeGraph::createNodeGUI

void
NodeGraph::pushUndoCommand(QUndoCommand* command,
                           const NodesGuiList& touchedNodes)
{
    UndoScope scope;

    for (NodesGuiList::const_iterator it = touchedNodes.begin(); it != touchedNodes.end(); ++it) {
        if (*it) {
            scope.touchNode((*it)->getNode());
        }
    }
    pushUndoCommand(command, scope);
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_NodeGraph.cpp"
