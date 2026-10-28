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
#include <cmath>
#include <stdexcept>

#include <QApplication>
#include <QFontMetrics>

#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Project.h"
#include "Engine/RotoLayer.h"
#include "Engine/Settings.h"

#include "Gui/Edge.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/GuiApplicationManager.h" // appPTR
#include "Gui/NodeClipBoard.h"
#include "Gui/NodeGraph.h"
#include "Gui/NodeGui.h"
#include "Gui/ThemedIcon.h"

#include "Serialization/NodeSerialization.h"

NATRON_NAMESPACE_ENTER

NodeGraphPrivate::NodeGraphPrivate(NodeGraph* p,
                                   const NodeCollectionPtr& group)
    : _publicInterface(p)
    , group(group)
    , _lastMousePos()
    , _lastSelectionStartPointScene()
    , _evtState(eEventStateNone)
    , _magnifiedNode()
    , _nodeSelectedScaleBeforeMagnif(1.)
    , _magnifOn(false)
    , _arrowSelected(NULL)
    , _nodesMutex()
    , _nodes()
    , _nodesTrash()
    , _nodeCreationShortcutEnabled(false)
    , _root(NULL)
    , _nodeRoot(NULL)
    , _cacheSizeText(NULL)
    , cacheSizeHidden(true)
    , _refreshCacheTextTimer()
    , _navigator(NULL)
    , _menu(NULL)
    , _tL(NULL)
    , _tR(NULL)
    , _bR(NULL)
    , _bL(NULL)
    , _refreshOverlays(false)
    , _hintInputEdge(NULL)
    , _hintOutputEdge(NULL)
    , _backdropResized()
    , _selection()
    , cursorSet(false)
    , _nodesWithinBDAtPenDown()
    , _selectionRect()
    , _bendPointsVisible(false)
    , _knobLinksVisible(true)
    , _accumDelta(0)
    , _detailsVisible(false)
    , _deltaSinceMousePress(0, 0)
    , _hasMovedOnce(false)
    , lastSelectedViewer(0)
    , isDoingPreviewRender(false)
    , autoScrollTimer()
    , refreshRenderStateTimer()
    , navigatorContent()
    , navigatorContentRect()
    , navigatorContentDirty(true)
    , navigatorRenderTimer()
    , nodesBoundsInRoot()
    , nodesBoundsDirty(true)
    , antiAliasing(appPTR->getCurrentSettings()->isNodeGraphAntiAliasingEnabled())
    , lowDetail(false)
    , lowDetailFontHeight(QFontMetrics(QApplication::font()).height())
    , stackingBatchDepth(0)
    , loadStackingBatchOpen(false)
    , frameDependentNodes()
    , timelineConnected(false)
    , stackingPending()
    , stackingPendingSet()
    , renderStateDirty()
{
    appPTR->getIcon(NATRON_PIXMAP_LOCKED, &unlockIcon);
    ThemedIcon::connectThemeChanged(_publicInterface, [this]() {
        appPTR->getIcon(NATRON_PIXMAP_LOCKED, &unlockIcon);
    });
}

QPoint
NodeGraphPrivate::getPyPlugUnlockPos() const
{
    return QPoint(_publicInterface->width() - qRound(unlockIcon.deviceIndependentSize().width()) - 10, 10);
}

void
NodeGraphPrivate::resetSelection()
{
    for (NodesGuiList::iterator it = _selection.begin(); it != _selection.end(); ++it) {
        (*it)->setUserSelected(false);
    }

    _selection.clear();
}

void
NodeGraphPrivate::replaceSelection(const NodesGuiList& nodes)
{
    // A node that stays selected is not deselected first: that would refresh it twice to the same state.
    std::unordered_set<NodeGui*> kept;
    kept.reserve(nodes.size());
    for (NodesGuiList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        kept.insert(it->get());
    }
    for (NodesGuiList::iterator it = _selection.begin(); it != _selection.end(); ++it) {
        if (kept.find(it->get()) == kept.end()) {
            (*it)->setUserSelected(false);
        }
    }
    _selection.clear();
    for (NodesGuiList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        if (_selection.push_back(*it)) {
            (*it)->setUserSelected(true);
        }
    }
}

void
NodeGraphPrivate::editSelectionFromSelectionRectangle(bool addToSelection)
{
    const QRectF& selection = _selectionRect;
    NodesGuiList inside;

    for (NodesGuiList::iterator it = _nodes.begin(); it != _nodes.end(); ++it) {
        QRectF bbox = (*it)->mapToScene((*it)->outlineBoundingRect()).boundingRect();
        if ( selection.contains(bbox) ) {
            inside.push_back(*it);
        }
    }

    if (!addToSelection) {
        replaceSelection(inside);

        return;
    }
    for (NodesGuiList::iterator it = inside.begin(); it != inside.end(); ++it) {
        if (_selection.push_back(*it)) {
            (*it)->setUserSelected(true);
        }
    }
}

bool
NodeGraphPrivate::rearrangeSelectedNodes()
{
    if ( !_selection.empty() ) {
        _publicInterface->pushUndoCommand(new RearrangeNodesCommand(_selection), _selection);

        return true;
    }

    return false;
}

void
NodeGraphPrivate::setNodesBendPointsVisible(bool visible)
{
    _bendPointsVisible = visible;

    for (NodesGuiList::iterator it = _nodes.begin(); it != _nodes.end(); ++it) {
        const std::vector<Edge*> & edges = (*it)->getInputsArrows();
        for (std::vector<Edge*>::const_iterator it2 = edges.begin(); it2 != edges.end(); ++it2) {
            if (visible) {
                if ( !(*it2)->isOutputEdge() && (*it2)->hasSource() && ( (*it2)->line().length() > 50 ) ) {
                    (*it2)->setBendPointVisible(visible);
                }
            } else {
                if ( (*it2) && !(*it2)->isOutputEdge() ) {
                    (*it2)->setBendPointVisible(visible);
                }
            }
        }
    }
}

QRectF
NodeGraphPrivate::calcNodesBoundingRect()
{
    if (nodesBoundsDirty) {
        QRectF ret;
        {
            QMutexLocker l(&_nodesMutex);

            for (NodesGuiList::iterator it = _nodes.begin(); it != _nodes.end(); ++it) {
                if ((*it)->isVisible()) {
                    ret = ret.united((*it)->boundingRectWithEdges());
                }
            }
        }
        nodesBoundsInRoot = ret.isNull() ? QRectF() : _root->mapRectFromScene(ret);
        nodesBoundsDirty = false;
    }

    return nodesBoundsInRoot.isNull() ? QRectF() : _root->mapRectToScene(nodesBoundsInRoot);
}

void
NodeGraphPrivate::getNavigatorMapping(QRectF* sceneRect,
                                      double* scale,
                                      QPointF* offset) const
{
    NodeGraphPrivate* self = const_cast<NodeGraphPrivate*>(this);
    QRectF sceneR = self->calcNodesBoundingRect().united(_publicInterface->visibleSceneRect());
    int navWidth = std::ceil(_publicInterface->width() * NATRON_NAVIGATOR_BASE_WIDTH);
    int navHeight = std::ceil(_publicInterface->height() * NATRON_NAVIGATOR_BASE_HEIGHT);
    double xScale = navWidth / sceneR.width();
    double yScale = navHeight / sceneR.height();
    double scaleFactor = std::max(0.001, std::min(xScale, yScale));
    int renderW = std::floor(sceneR.width() * scaleFactor);
    int renderH = std::floor(sceneR.height() * scaleFactor);

    *sceneRect = sceneR;
    *scale = scaleFactor;
    *offset = QPointF((navWidth - renderW) / 2, (navHeight - renderH) / 2);
}

void
NodeGraphPrivate::renderNavigatorContent()
{
    QRectF nodesScene = calcNodesBoundingRect();

    navigatorContentDirty = false;
    if (nodesScene.isEmpty()) {
        navigatorContent = QImage();
        navigatorContentRect = QRectF();

        return;
    }

    // Rendered at the navigator's own size: panning or zooming out only ever shrinks the nodes
    // inside the navigator, so the thumbnail never needs more pixels than this.
    int navWidth = std::ceil(_publicInterface->width() * NATRON_NAVIGATOR_BASE_WIDTH);
    int navHeight = std::ceil(_publicInterface->height() * NATRON_NAVIGATOR_BASE_HEIGHT);
    double scaleFactor = std::min(navWidth / nodesScene.width(), navHeight / nodesScene.height());
    int w = std::max(1, (int)std::floor(nodesScene.width() * scaleFactor));
    int h = std::max(1, (int)std::floor(nodesScene.height() * scaleFactor));

    navigatorContent = QImage(w, h, QImage::Format_ARGB32_Premultiplied);
    navigatorContent.fill(QColor(71, 71, 71, 255));
    {
        QPainter painter(&navigatorContent);
        isDoingPreviewRender = true;
        _publicInterface->scene()->render(&painter, navigatorContent.rect(), nodesScene, Qt::KeepAspectRatio);
        isDoingPreviewRender = false;
    }
    navigatorContentRect = _root->mapRectFromScene(nodesScene);
}

void
NodeGraphPrivate::resetAllClipboards()
{
    appPTR->clearNodeClipBoard();
}

void
NodeGraphPrivate::copyNodesInternal(const NodesGuiList& selection,
                                    NodeClipBoard & clipboard)
{
    ///Clear clipboard
    clipboard.nodes.clear();
    clipboard.sourceNodes.clear();
    clipboard.sourceProject = _publicInterface->getGui()->getApp()->getProject()->getProjectId();

    NodesGuiList nodesToCopy = selection;
    for (NodesGuiList::iterator it = nodesToCopy.begin(); it != nodesToCopy.end(); ++it) {
        ///Also copy all nodes within the backdrop
        NodesGuiList nodesWithinBD = _publicInterface->getNodesWithinBackdrop(*it);
        for (NodesGuiList::iterator it2 = nodesWithinBD.begin(); it2 != nodesWithinBD.end(); ++it2) {
            NodesGuiList::iterator found = std::find(nodesToCopy.begin(), nodesToCopy.end(), *it2);
            if ( found == nodesToCopy.end() ) {
                nodesToCopy.push_back(*it2);
            }
        }
    }

    for (NodesGuiList::iterator it = nodesToCopy.begin(); it != nodesToCopy.end(); ++it) {
        if ( (*it)->isVisible() ) {
            Serialization::NodeSerializationPtr ns = std::make_shared<Serialization::NodeSerialization>();
            NodePtr node = (*it)->getNode();
            if (node) {
                node->toSerialization(ns.get(), true);
            }
            clipboard.nodes.push_back(ns);
            clipboard.sourceNodes.push_back(node);
        }
    }
}

NATRON_NAMESPACE_EXIT
