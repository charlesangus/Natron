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

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QMouseEvent>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/ProjectMerge.h"

#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/GuiMacros.h"
#include "Gui/MergeConflictPreview.h"
#include "Gui/MergePopover.h"
#include "Gui/NodeGraphDiffOverlay.h"
#include "Gui/NodeGraphMergeBar.h"
#include "Gui/NodeGui.h"

NATRON_NAMESPACE_ENTER

namespace {

NodeGuiPtr
liveNodeGui(const ProjectPtr& project,
            const std::string& nodeId)
{
    const NodePtr node = (project && !nodeId.empty()) ? project->findNodeById(nodeId) : NodePtr();

    return node ? std::dynamic_pointer_cast<NodeGui>(node->getNodeGui()) : NodeGuiPtr();
}

// The nodes with merge conflicts, in the order their first conflict is listed.
std::vector<std::string>
conflictedNodeIds(const ProjectPtr& project)
{
    std::vector<std::string> ids;
    if (!project) {
        return ids;
    }
    for (const Serialization::MergeConflict& conflict : project->getMergeConflicts()) {
        if (std::find(ids.begin(), ids.end(), conflict.nodeId) == ids.end() && liveNodeGui(project, conflict.nodeId)) {
            ids.push_back(conflict.nodeId);
        }
    }

    return ids;
}

// The entry step places after cursor, wrapping; the first or last when cursor is not listed.
std::size_t
stepFrom(const std::vector<std::string>& ids,
         const std::string& cursor,
         int step)
{
    const std::vector<std::string>::const_iterator found = std::find(ids.begin(), ids.end(), cursor);
    const int count = (int)ids.size();
    if (found == ids.end()) {
        return (step > 0) ? 0 : (std::size_t)(count - 1);
    }

    return (std::size_t)((((int)(found - ids.begin()) + step) % count + count) % count);
}

} // anon namespace

MergeConflictPreview*
NodeGraph::getMergePreview() const
{
    return _imp->mergePreview;
}

MergePopover*
NodeGraph::getMergePopover() const
{
    return _imp->mergePopover;
}

NodeGraphMergeBar*
NodeGraph::getMergeBar() const
{
    return _imp->mergeBar;
}

bool
NodeGraph::openMergePopover(const NodeGuiPtr& node)
{
    return node && (node->getDagGui() == this) && _imp->mergePopover && _imp->mergePopover->openFor(node);
}

void
NodeGraph::closeMergePopover()
{
    if (_imp->mergePopover) {
        _imp->mergePopover->dismiss();
    }
}

bool
NodeGraph::isMergePopoverOpen() const
{
    return _imp->mergePopover && _imp->mergePopover->isOpen();
}

std::set<std::string>
NodeGraph::getSelectedGhostIds() const
{
    return _imp->_diffOverlay ? _imp->_diffOverlay->selectedGhostIds() : std::set<std::string>();
}

void
NodeGraph::setSelectedGhostIds(const std::set<std::string>& ids)
{
    if (_imp->_diffOverlay) {
        _imp->_diffOverlay->setGhostSelection(ids);
    }
}

void
NodeGraph::layoutMergeBar()
{
    NodeGraphMergeBar* bar = _imp->mergeBar;
    if (!bar) {
        return;
    }
    const bool shown = (bar->mode() != NodeGraphMergeBar::eModeNone);
    const int barHeight = shown ? bar->sizeHint().height() : 0;
    setViewportMargins(0, 0, 0, barHeight);
    if (shown) {
        const QRect area = contentsRect();
        bar->setGeometry(area.left(), area.bottom() + 1 - barHeight, area.width(), barHeight);
        bar->show();
        bar->raise();
    } else {
        bar->hide();
    }
}

bool
NodeGraph::frameFirstMergeConflict(Gui* gui)
{
    const ProjectPtr project = (gui && gui->getApp()) ? gui->getApp()->getProject() : ProjectPtr();
    const std::vector<std::string> ids = conflictedNodeIds(project);
    if (ids.empty()) {
        return false;
    }
    const NodeGuiPtr node = liveNodeGui(project, ids.front());
    NodeGraph* graph = node ? node->getDagGui() : 0;
    if (!graph) {
        return false;
    }
    graph->_imp->mergeCursorId = ids.front();
    graph->centerOnItem(node.get());

    return true;
}

void
NodeGraph::goToNextMergeConflict()
{
    goToMergeConflict(1);
}

void
NodeGraph::goToPreviousMergeConflict()
{
    goToMergeConflict(-1);
}

void
NodeGraph::goToNextComparisonChange()
{
    goToComparisonChange(1);
}

void
NodeGraph::goToPreviousComparisonChange()
{
    goToComparisonChange(-1);
}

void
NodeGraph::goToMergeConflict(int step)
{
    Gui* gui = getGui();
    // A comparison hides the conflicts until it is cleared.
    if (!gui || gui->getGraphComparison()) {
        return;
    }
    const ProjectPtr project = gui->getApp() ? gui->getApp()->getProject() : ProjectPtr();
    const std::vector<std::string> ids = conflictedNodeIds(project);
    if (ids.empty()) {
        return;
    }
    const std::string id = ids[stepFrom(ids, _imp->mergeCursorId, step)];
    const NodeGuiPtr node = liveNodeGui(project, id);
    // Conflicts span the whole project: the node may be in another group's graph.
    NodeGraph* graph = node ? node->getDagGui() : 0;
    if (!graph) {
        return;
    }
    _imp->mergeCursorId = id;
    graph->_imp->mergeCursorId = id;
    if (graph != this) {
        closeMergePopover();
    }
    graph->centerOnItem(node.get());
    graph->openMergePopover(node);
}

void
NodeGraph::goToComparisonChange(int step)
{
    Gui* gui = getGui();
    const ProjectMerge::ChangeSummary* summary = gui ? gui->getGraphComparison() : 0;
    if (!summary) {
        return;
    }
    const ProjectPtr project = gui->getApp()->getProject();
    std::vector<std::string> ids;
    for (const ProjectMerge::NodeChange& change : summary->nodes) {
        const NodeGuiPtr node = liveNodeGui(project, change.nodeId);
        const bool here = node ? (node->getDagGui() == this)
                               : (_imp->_diffOverlay && !_imp->_diffOverlay->ghostRectOf(change.nodeId).isEmpty());
        if (here) {
            ids.push_back(change.nodeId);
        }
    }
    if (ids.empty()) {
        return;
    }
    const std::string id = ids[stepFrom(ids, _imp->mergeCursorId, step)];
    _imp->mergeCursorId = id;
    const NodeGuiPtr node = liveNodeGui(project, id);
    if (node) {
        _imp->_diffOverlay->clearGhostSelection();
        selectNode(node, false);
        centerOnItem(node.get());
        if (!openMergePopover(node)) {
            closeMergePopover();
        }
    } else {
        closeMergePopover();
        deselect();
        _imp->_diffOverlay->setGhostSelection(std::set<std::string> { id });
        centerOn(_imp->_diffOverlay->ghostRectOf(id).center());
    }
}

bool
NodeGraph::handleMergePress(QMouseEvent* e,
                            const QPointF& scenePos)
{
    QString error;
    if (buttonDownIsLeft(e) && _imp->mergePreview && _imp->mergePreview->pickAt(scenePos, &error)) {
        if (!error.isEmpty()) {
            getGui()->getApp()->appendToScriptEditor(error.toStdString());
        }
        _imp->_evtState = eEventStateNone;

        return true;
    }

    NodeGui* nearbyNode = 0;
    Edge* nearbyEdge = 0;
    const NearbyItemEnum nearby = hasItemNearbyMouse(e->pos(), &nearbyNode, &nearbyEdge);
    if (buttonDownIsLeft(e)) {
        for (const NodeGuiPtr& node : getAllActiveNodes()) {
            if (node->isMergeConflictBadgeAt(scenePos) || node->isChangeMarkerAt(scenePos)) {
                openMergePopover(node);
                _imp->_evtState = eEventStateNone;

                return true;
            }
        }
    }

    const std::string ghost = ((nearby == eNearbyItemNone) && _imp->_diffOverlay) ? _imp->_diffOverlay->ghostIdAt(scenePos) : std::string();
    if (!ghost.empty()) {
        closeMergePopover();
        _imp->_evtState = eEventStateNone;
        if (buttonDownIsLeft(e)) {
            if (modCASIsShift(e) || modCASIsControl(e)) {
                _imp->_diffOverlay->toggleGhostSelected(ghost);
            } else {
                deselect();
                _imp->_diffOverlay->setGhostSelection(std::set<std::string> { ghost });
            }

            return true;
        }
        if (buttonDownIsRight(e)) {
            if (!_imp->_diffOverlay->isGhostSelected(ghost)) {
                deselect();
                _imp->_diffOverlay->setGhostSelection(std::set<std::string> { ghost });
            }
            showMenu(mapToGlobal(e->pos()));

            return true;
        }

        return false;
    }

    if (isMergePopoverOpen()) {
        closeMergePopover();
    }
    if (buttonDownIsLeft(e) && !modCASIsShift(e) && !modCASIsControl(e) && _imp->_diffOverlay) {
        _imp->_diffOverlay->clearGhostSelection();
    }

    return false;
} // NodeGraph::handleMergePress

NATRON_NAMESPACE_EXIT
