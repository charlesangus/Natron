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
#include <cassert>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <unordered_map>
#include <vector>

GCC_DIAG_UNUSED_PRIVATE_FIELD_OFF
CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QAction>
#include <QFileSystemModel>
#include <QMenu>
GCC_DIAG_UNUSED_PRIVATE_FIELD_ON
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/FileSystemModel.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/Settings.h"
#include "Engine/TimeLine.h"

#include "Gui/ActionShortcuts.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/Menu.h"
#include "Gui/NodeGui.h"
#include "Gui/SequenceFileDialog.h"

#include "Global/QtCompat.h"

NATRON_NAMESPACE_ENTER
const NodesGuiList &
NodeGraph::getAllActiveNodes() const
{
    return _imp->_nodes;
}

NodesGuiList
NodeGraph::getAllActiveNodes_mt_safe() const
{
    QMutexLocker l(&_imp->_nodesMutex);

    return _imp->_nodes;
}

void
NodeGraph::moveToTrash(NodeGui* node)
{
    assert(node);
    NodeGuiPtr deselected = _imp->_selection.remove(node);
    if (deselected) {
        deselected->setUserSelected(false);
    }

    {
        QMutexLocker l(&_imp->_nodesMutex);
        NodeGuiPtr removed = _imp->_nodes.remove(node);
        if (removed) {
            _imp->_nodesTrash.push_back(removed);
        }
    }
    invalidateNavigatorContent();
}

void
NodeGraph::restoreFromTrash(NodeGui* node)
{
    assert(node);
    {
        QMutexLocker l(&_imp->_nodesMutex);
        NodeGuiPtr restored = _imp->_nodesTrash.remove(node);
        if (restored) {
            _imp->_nodes.push_back(restored);
        }
    }
    invalidateNavigatorContent();
}

// grabbed from QDirModelPrivate::size() in qtbase/src/widgets/itemviews/qdirmodel.cpp
static
QString
QDirModelPrivate_size(quint64 bytes)
{
    // According to the Si standard KB is 1000 bytes, KiB is 1024
    // but on windows sizes are calculated by dividing by 1024 so we do what they do.
    const quint64 kb = 1024;
    const quint64 mb = 1024 * kb;
    const quint64 gb = 1024 * mb;
    const quint64 tb = 1024 * gb;

    if (bytes >= tb) {
        return QFileSystemModel::tr("%1 TB").arg( QLocale().toString(qreal(bytes) / tb, 'f', 3) );
    }
    if (bytes >= gb) {
        return QFileSystemModel::tr("%1 GB").arg( QLocale().toString(qreal(bytes) / gb, 'f', 2) );
    }
    if (bytes >= mb) {
        return QFileSystemModel::tr("%1 MB").arg( QLocale().toString(qreal(bytes) / mb, 'f', 1) );
    }
    if (bytes >= kb) {
        return QFileSystemModel::tr("%1 KB").arg( QLocale().toString(bytes / kb) );
    }

    return QFileSystemModel::tr("%1 byte(s)").arg( QLocale().toString(bytes) );
}

void
NodeGraph::updateCacheSizeText()
{
    if ( !getGui() ) {
        return;
    }
    if ( getGui()->isGUIFrozen() ) {
        if ( _imp->_cacheSizeText->isVisible() ) {
            _imp->_cacheSizeText->hide();
        }

        return;
    } else {
        if ( !_imp->cacheSizeHidden && !_imp->_cacheSizeText->isVisible() ) {
            _imp->_cacheSizeText->show();
        } else if ( _imp->cacheSizeHidden && _imp->_cacheSizeText->isVisible() ) {
            _imp->_cacheSizeText->hide();

            return;
        }
    }

    QString oldText = _imp->_cacheSizeText->text();
    quint64 cacheSize = appPTR->getCachesTotalMemorySize();
    QString cacheSizeStr = QDirModelPrivate_size(cacheSize);
    quint64 diskSize = appPTR->getCachesTotalDiskSize();
    QString diskCacheSizeStr = QDirModelPrivate_size(diskSize);
    QString newText = tr("Memory cache: %1 / Disk cache: %2").arg(cacheSizeStr).arg(diskCacheSizeStr);
    if (newText != oldText) {
        _imp->_cacheSizeText->setText(newText);
    }
}

void
NodeGraph::scheduleNodeRenderStateRefresh(NodeGui* node)
{
    if (!node) {
        return;
    }
    _imp->renderStateDirty[node] = node->weak_from_this();
    if (!_imp->refreshRenderStateTimer.isActive()) {
        _imp->refreshRenderStateTimer.start(NATRON_NODES_RENDER_STATE_REFRESH_INTERVAL_MS);
    }
}

void
NodeGraph::onRefreshNodesRenderStateTimerTimeout()
{
    std::map<NodeGui*, NodeGuiWPtr> dirty;
    dirty.swap(_imp->renderStateDirty);
    for (std::map<NodeGui*, NodeGuiWPtr>::const_iterator it = dirty.begin(); it != dirty.end(); ++it) {
        NodeGuiPtr node = it->second.lock();
        if (node) {
            node->refreshRenderingIndicator();
        }
    }
}

void
NodeGraph::beginNodeStackingBatch()
{
    ++_imp->stackingBatchDepth;
}

void
NodeGraph::endNodeStackingBatch()
{
    assert(_imp->stackingBatchDepth > 0);
    if (--_imp->stackingBatchDepth > 0) {
        return;
    }
    std::vector<NodeGuiWPtr> pending;
    pending.swap(_imp->stackingPending);
    _imp->stackingPendingSet.clear();

    std::vector<NodeGuiPtr> alive;
    std::vector<NodeGui*> nodes;
    alive.reserve(pending.size());
    nodes.reserve(pending.size());
    for (std::vector<NodeGuiWPtr>::const_iterator it = pending.begin(); it != pending.end(); ++it) {
        NodeGuiPtr node = it->lock();
        if (node) {
            alive.push_back(node);
            nodes.push_back(node.get());
        }
    }
    stackNodesAboveOverlapping(nodes);
}

bool
NodeGraph::isLoadingProjectNodes() const
{
    Gui* gui = getGui();
    GuiAppInstancePtr app = gui ? gui->getApp() : GuiAppInstancePtr();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();

    return project && project->isLoadingProjectInternal();
}

void
NodeGraph::onProjectNodesRestored()
{
    if (_imp->loadStackingBatchOpen) {
        _imp->loadStackingBatchOpen = false;
        endNodeStackingBatch();
    }
    // Expressions and links are restored after the node GUIs are created.
    for (NodesGuiList::const_iterator it = _imp->_nodes.begin(); it != _imp->_nodes.end(); ++it) {
        (*it)->refreshFrameDependency();
    }
    invalidateNavigatorContent();
}

void
NodeGraph::registerFrameDependentNode(NodeGui* node)
{
    NodeGuiWPtr weak = node->weak_from_this();
    if (weak.expired()) {
        return;
    }
    NodeGuiWPtr& entry = _imp->frameDependentNodes[node];
    // A destroyed node's address can be reused by a new one.
    if (entry.lock().get() != node) {
        entry = weak;
    }
    if (!_imp->timelineConnected) {
        NodePtr internalNode = node->getNode();
        if (internalNode && internalNode->getApp()) {
            _imp->timelineConnected = true;
            QObject::connect(internalNode->getApp()->getTimeLine().get(), SIGNAL(frameChanged(SequenceTime, int)),
                             this, SLOT(onTimelineFrameChanged(SequenceTime, int)));
        }
    }
}

void
NodeGraph::onTimelineFrameChanged(SequenceTime time,
                                  int reason)
{
    std::vector<NodeGuiPtr> nodes;
    nodes.reserve(_imp->frameDependentNodes.size());
    for (std::unordered_map<NodeGui*, NodeGuiWPtr>::iterator it = _imp->frameDependentNodes.begin(); it != _imp->frameDependentNodes.end();) {
        NodeGuiPtr node = it->second.lock();
        if (node) {
            nodes.push_back(node);
            ++it;
        } else {
            it = _imp->frameDependentNodes.erase(it);
        }
    }
    for (std::vector<NodeGuiPtr>::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        (*it)->onTimelineTimeChanged(time, reason);
    }
}

bool
NodeGraph::deferNodeStacking(NodeGui* node)
{
    // Nodes restored by a project load are stacked once, when the load ends.
    if (!_imp->loadStackingBatchOpen && isLoadingProjectNodes()) {
        _imp->loadStackingBatchOpen = true;
        beginNodeStackingBatch();
    }
    if (_imp->stackingBatchDepth <= 0) {
        return false;
    }
    NodeGuiWPtr weak = node->weak_from_this();
    if (weak.expired()) {
        return false;
    }
    if (_imp->stackingPendingSet.insert(node).second) {
        _imp->stackingPending.push_back(weak);
    }

    return true;
}

void
NodeGraph::stackNodesAboveOverlapping(const std::vector<NodeGui*>& nodes)
{
    if (nodes.empty()) {
        return;
    }
    std::vector<QRectF> bboxes;
    bboxes.reserve(nodes.size());
    QRectF all;
    for (std::vector<NodeGui*>::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        QRectF bbox = (*it)->mapRectToScene((*it)->boundingRect());
        bboxes.push_back(bbox);
        all = all.united(bbox);
    }
    // One walk over the graph finds the few nodes near any of them; the margin keeps a node whose
    // edge only grazes the union, as rounded in another item's coordinates, a candidate.
    all.adjust(-1, -1, 1, 1);
    std::vector<NodeGui*> candidates;
    for (NodesGuiList::const_iterator it = _imp->_nodes.begin(); it != _imp->_nodes.end(); ++it) {
        if ((*it)->isVisible() && all.intersects((*it)->mapRectToScene((*it)->boundingRect()))) {
            candidates.push_back(it->get());
        }
    }
    if (nodes.size() * candidates.size() <= 4096) {
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            for (std::vector<NodeGui*>::const_iterator it = candidates.begin(); it != candidates.end(); ++it) {
                if ((*it != nodes[i]) && (*it)->intersects(bboxes[i])) {
                    nodes[i]->setAboveItem(*it);
                }
            }
        }

        return;
    }

    // Large batches (a project load, a big paste) bin the candidates in a uniform grid so each node
    // only tests its neighbours. The grid is a conservative prefilter: matches are visited in graph
    // order and still go through intersects(), so the stacking is the same as the full scan.
    std::vector<QRectF> candRects;
    candRects.reserve(candidates.size());
    std::vector<double> sizes;
    sizes.reserve(candidates.size());
    for (std::vector<NodeGui*>::const_iterator it = candidates.begin(); it != candidates.end(); ++it) {
        QRectF r = (*it)->mapRectToScene((*it)->boundingRect()).adjusted(-1, -1, 1, 1);
        candRects.push_back(r);
        sizes.push_back(std::max(r.width(), r.height()));
    }
    std::nth_element(sizes.begin(), sizes.begin() + sizes.size() / 2, sizes.end());
    const double cell = std::max(1., sizes[sizes.size() / 2]);
    const double maxCell = (double)std::numeric_limits<int>::max() / 2;
    struct CellRange {
        int x0, y0, x1, y1;
    };
    const auto cellRange = [cell, maxCell](const QRectF& r) {
        CellRange c;
        c.x0 = (int)qBound(-maxCell, std::floor(r.left() / cell), maxCell);
        c.y0 = (int)qBound(-maxCell, std::floor(r.top() / cell), maxCell);
        c.x1 = (int)qBound(-maxCell, std::floor(r.right() / cell), maxCell);
        c.y1 = (int)qBound(-maxCell, std::floor(r.bottom() / cell), maxCell);

        return c;
    };
    const auto cellKey = [](int x, int y) {
        return ((quint64)(quint32)x << 32) | (quint64)(quint32)y;
    };
    const qint64 kMaxCellsPerRect = 64;

    std::unordered_map<quint64, std::vector<int>> grid;
    std::vector<int> oversize;
    for (int c = 0; c < (int)candidates.size(); ++c) {
        const CellRange cr = cellRange(candRects[c]);
        if ((qint64)(cr.x1 - cr.x0 + 1) * (cr.y1 - cr.y0 + 1) > kMaxCellsPerRect) {
            oversize.push_back(c);
            continue;
        }
        for (int x = cr.x0; x <= cr.x1; ++x) {
            for (int y = cr.y0; y <= cr.y1; ++y) {
                grid[cellKey(x, y)].push_back(c);
            }
        }
    }

    std::vector<std::size_t> stamp(candidates.size(), 0);
    std::vector<int> matches;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        matches.clear();
        const CellRange cr = cellRange(bboxes[i]);
        if ((qint64)(cr.x1 - cr.x0 + 1) * (cr.y1 - cr.y0 + 1) > kMaxCellsPerRect) {
            for (int c = 0; c < (int)candidates.size(); ++c) {
                matches.push_back(c);
            }
        } else {
            for (int x = cr.x0; x <= cr.x1; ++x) {
                for (int y = cr.y0; y <= cr.y1; ++y) {
                    std::unordered_map<quint64, std::vector<int>>::const_iterator found = grid.find(cellKey(x, y));
                    if (found == grid.end()) {
                        continue;
                    }
                    for (std::vector<int>::const_iterator c = found->second.begin(); c != found->second.end(); ++c) {
                        if (stamp[*c] != i + 1) {
                            stamp[*c] = i + 1;
                            matches.push_back(*c);
                        }
                    }
                }
            }
            matches.insert(matches.end(), oversize.begin(), oversize.end());
            std::sort(matches.begin(), matches.end());
        }
        for (std::vector<int>::const_iterator c = matches.begin(); c != matches.end(); ++c) {
            NodeGui* cand = candidates[*c];
            if ((cand != nodes[i]) && candRects[*c].intersects(bboxes[i]) && cand->intersects(bboxes[i])) {
                nodes[i]->setAboveItem(cand);
            }
        }
    }
} // stackNodesAboveOverlapping

void
NodeGraph::toggleCacheInfo()
{
    _imp->cacheSizeHidden = !_imp->cacheSizeHidden;
    if (_imp->cacheSizeHidden) {
        _imp->_cacheSizeText->hide();
    } else {
        _imp->_cacheSizeText->show();
    }
}

void
NodeGraph::toggleKnobLinksVisible()
{
    _imp->_knobLinksVisible = !_imp->_knobLinksVisible;
    {
        QMutexLocker l(&_imp->_nodesMutex);
        // Each kind of line still follows its own preference while the switch is on.
        for (NodesGuiList::iterator it = _imp->_nodes.begin(); it != _imp->_nodes.end(); ++it) {
            (*it)->refreshLinkLinesAppearance();
        }
        for (NodesGuiList::iterator it = _imp->_nodesTrash.begin(); it != _imp->_nodesTrash.end(); ++it) {
            (*it)->refreshLinkLinesAppearance();
        }
    }
}

void
NodeGraph::toggleAutoPreview()
{
    getGui()->getApp()->getProject()->toggleAutoPreview();
}

void
NodeGraph::forceRefreshAllPreviews()
{
    getGui()->forceRefreshAllPreviews();
}

void
NodeGraph::showMenu(const QPoint & pos)
{
    _imp->_menu->clear();

    QAction* createNode = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphCreateNode,
                                                  kShortcutDescActionGraphCreateNode, _imp->_menu);
    _imp->_menu->addAction(createNode);


    //QFont font(appFont,appFontSize);
    Menu* editMenu = new Menu(tr("Edit"), _imp->_menu);
    //editMenu->setFont(font);
    _imp->_menu->addAction( editMenu->menuAction() );

    QAction* copyAction = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphCopy,
                                                 kShortcutDescActionGraphCopy, editMenu);
    QObject::connect( copyAction, SIGNAL(triggered()), this, SLOT(copySelectedNodes()) );
    editMenu->addAction(copyAction);

    QAction* cutAction = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphCut,
                                                kShortcutDescActionGraphCut, editMenu);
    QObject::connect( cutAction, SIGNAL(triggered()), this, SLOT(cutSelectedNodes()) );
    editMenu->addAction(cutAction);


    QAction* pasteAction = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphPaste,
                                                  kShortcutDescActionGraphPaste, editMenu);
    pasteAction->setEnabled( !appPTR->isNodeClipBoardEmpty() );
    editMenu->addAction(pasteAction);

    QAction* deleteAction = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphRemoveNodes,
                                                   kShortcutDescActionGraphRemoveNodes, editMenu);
    QObject::connect( deleteAction, SIGNAL(triggered()), this, SLOT(deleteSelection()) );
    editMenu->addAction(deleteAction);

    QAction* renameAction = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphRenameNode,
                                                   kShortcutDescActionGraphRenameNode, editMenu);
    QObject::connect( renameAction, SIGNAL(triggered()), this, SLOT(renameNode()) );
    editMenu->addAction(renameAction);

    QAction* duplicateAction = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphDuplicate,
                                                      kShortcutDescActionGraphDuplicate, editMenu);
    editMenu->addAction(duplicateAction);

    QAction* createLinkedAction = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphCreateLinkedNode,
                                                         kShortcutDescActionGraphCreateLinkedNode, editMenu);
    editMenu->addAction(createLinkedAction);

    QAction* unlinkAction = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphUnlinkNode,
                                                   kShortcutDescActionGraphUnlinkNode, editMenu);
    QObject::connect(unlinkAction, SIGNAL(triggered()), this, SLOT(unlinkSelectedNodes()));
    editMenu->addAction(unlinkAction);

    QAction* switchInputs = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphExtractNode,
                                                   kShortcutDescActionGraphExtractNode, editMenu);
    QObject::connect( switchInputs, SIGNAL(triggered()), this, SLOT(extractSelectedNode()) );
    editMenu->addAction(switchInputs);

    QAction* extractNode = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphSwitchInputs,
                                                  kShortcutDescActionGraphSwitchInputs, editMenu);
    QObject::connect( extractNode, SIGNAL(triggered()), this, SLOT(switchInputs1and2ForSelectedNodes()) );
    editMenu->addAction(extractNode);

    QAction* disableNodes = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphDisableNodes,
                                                   kShortcutDescActionGraphDisableNodes, editMenu);
    QObject::connect( disableNodes, SIGNAL(triggered()), this, SLOT(toggleSelectedNodesEnabled()) );
    editMenu->addAction(disableNodes);

    QAction* groupFromSel = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphMakeGroup,
                                                   kShortcutDescActionGraphMakeGroup, editMenu);
    QObject::connect( groupFromSel, SIGNAL(triggered()), this, SLOT(createGroupFromSelection()) );
    editMenu->addAction(groupFromSel);

    QAction* expandGroup = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphExpandGroup,
                                                  kShortcutDescActionGraphExpandGroup, editMenu);
    QObject::connect( expandGroup, SIGNAL(triggered()), this, SLOT(expandSelectedGroups()) );
    editMenu->addAction(expandGroup);

    QAction* findAction = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphFindNode,
                                                 kShortcutDescActionGraphFindNode, editMenu);
    editMenu->addAction(findAction);


    //QFont font(appFont,appFontSize);
    Menu* viewMenu = new Menu(tr("View"), _imp->_menu);
    //editMenu->setFont(font);
    _imp->_menu->addAction( viewMenu->menuAction() );

    QAction* displayCacheInfoAction = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphShowCacheSize,
                                                             kShortcutDescActionGraphShowCacheSize, viewMenu);
    displayCacheInfoAction->setCheckable(true);
    displayCacheInfoAction->setChecked( _imp->_cacheSizeText->isVisible() );
    QObject::connect( displayCacheInfoAction, SIGNAL(triggered()), this, SLOT(toggleCacheInfo()) );
    viewMenu->addAction(displayCacheInfoAction);

    const NodesGuiList& selectedNodes = getSelectedNodes();
    if ( !selectedNodes.empty() ) {
        QAction* turnOffPreviewAction = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphTogglePreview,
                                                               kShortcutDescActionGraphTogglePreview, viewMenu);
        turnOffPreviewAction->setCheckable(true);
        turnOffPreviewAction->setChecked(false);
        QObject::connect( turnOffPreviewAction, SIGNAL(triggered()), this, SLOT(togglePreviewsForSelectedNodes()) );
        viewMenu->addAction(turnOffPreviewAction);

        if (selectedNodes.size() == 1) {
            QAction* openNodePanelAction = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphOpenNodePanel,
                                                                  kShortcutDescActionGraphOpenNodePanel, viewMenu);
            openNodePanelAction->setCheckable(true);
            openNodePanelAction->setChecked(false);
            QObject::connect( openNodePanelAction, SIGNAL(triggered()), this, SLOT(showSelectedNodeSettingsPanel()) );
            viewMenu->addAction(openNodePanelAction);
        }
    }

    QAction* hideInputs = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphHideInputs,
                                                 kShortcutDescActionGraphHideInputs, viewMenu);
    hideInputs->setCheckable(true);
    bool hideInputsVal = false;
    if ( !selectedNodes.empty() ) {
        hideInputsVal = selectedNodes.front()->getNode()->getHideInputsKnobValue();
    }
    hideInputs->setChecked(hideInputsVal);
    QObject::connect( hideInputs, SIGNAL(triggered()), this, SLOT(toggleHideInputs()) );
    viewMenu->addAction(hideInputs);

    QAction* knobLinks = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphShowLinkLines,
                                                kShortcutDescActionGraphShowLinkLines, viewMenu);
    knobLinks->setCheckable(true);
    knobLinks->setChecked( areKnobLinksVisible() );
    QObject::connect( knobLinks, SIGNAL(triggered()), this, SLOT(toggleKnobLinksVisible()) );
    viewMenu->addAction(knobLinks);

    QAction* autoPreview = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphToggleAutoPreview,
                                                  kShortcutDescActionGraphToggleAutoPreview, viewMenu);
    autoPreview->setCheckable(true);
    autoPreview->setChecked( getGui()->getApp()->getProject()->isAutoPreviewEnabled() );
    QObject::connect( autoPreview, SIGNAL(triggered()), this, SLOT(toggleAutoPreview()) );
    QObject::connect( getGui()->getApp()->getProject().get(), SIGNAL(autoPreviewChanged(bool)), autoPreview, SLOT(setChecked(bool)) );
    viewMenu->addAction(autoPreview);

    QAction* autoTurbo = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphToggleAutoTurbo,
                                                kShortcutDescActionGraphToggleAutoTurbo, viewMenu);
    autoTurbo->setCheckable(true);
    autoTurbo->setChecked( appPTR->getCurrentSettings()->isAutoTurboEnabled() );
    QObject::connect( autoTurbo, SIGNAL(triggered()), this, SLOT(toggleAutoTurbo()) );
    viewMenu->addAction(autoTurbo);


    QAction* forceRefreshPreviews = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphForcePreview,
                                                           kShortcutDescActionGraphForcePreview, viewMenu);
    QObject::connect( forceRefreshPreviews, SIGNAL(triggered()), this, SLOT(forceRefreshAllPreviews()) );
    viewMenu->addAction(forceRefreshPreviews);

    QAction* frameAllNodes = new ActionWithShortcut(kShortcutGroupNodegraph, kShortcutIDActionGraphFrameNodes,
                                                    kShortcutDescActionGraphFrameNodes, viewMenu);
    QObject::connect( frameAllNodes, SIGNAL(triggered()), this, SLOT(centerOnAllNodes()) );
    viewMenu->addAction(frameAllNodes);


    _imp->_menu->addSeparator();

    QMenu* nodesMenu = appPTR->getNodesMenu();
    if (nodesMenu) {
        _imp->_menu->addActions(nodesMenu->actions());
    }
    appPTR->setNodesMenuTarget(getGui(), _imp->_menu);

    QAction* ret = _imp->_menu->exec(pos);
    if (ret == findAction) {
        popFindDialog();
    } else if (ret == createNode) {
            popCreateNodeDialog();
    } else if (ret == duplicateAction) {
        // Duplicate at mouse click position:
        QPointF scenePos = mapToScene( mapFromGlobal(pos) );
        duplicateSelectedNodes(scenePos);
    } else if (ret == createLinkedAction) {
        // Create the Linked Nodes at mouse click position:
        QPointF scenePos = mapToScene( mapFromGlobal(pos) );
        createLinkedNodes(scenePos);
    } else if (ret == pasteAction) {
        // Paste at mouse click position:
        QPointF scenePos = mapToScene( mapFromGlobal(pos) );
        pasteNodeClipBoards(scenePos);
    }
} // NodeGraph::showMenu

NATRON_NAMESPACE_EXIT
