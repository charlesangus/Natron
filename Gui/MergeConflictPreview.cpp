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

#include "MergeConflictPreview.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QBrush>
#include <QGraphicsRectItem>
#include <QPen>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/Node.h"
#include "Engine/Project.h"

#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/GuiDefines.h"
#include "Gui/MergeConflictResolve.h"
#include "Gui/NodeGraph.h"
#include "Gui/NodeGraphDiffOverlay.h"
#include "Gui/NodeGui.h"

#include "Serialization/NodeSerialization.h"

NATRON_NAMESPACE_ENTER

namespace {

// Above the comparison ghosts, below the GPU region outlines.
const double kPreviewDepth = 13.5;
const double kFaintAlpha = 0.3;
const double kWireEndRadius = 6.;

const Serialization::MergeConflictValue&
valueOf(const Serialization::MergeConflict& conflict,
        ProjectMerge::ConflictSide side)
{
    switch (side) {
    case ProjectMerge::ConflictSide::eMine:
        return conflict.mine;
    case ProjectMerge::ConflictSide::eTheirs:
        return conflict.theirs;
    case ProjectMerge::ConflictSide::eBase:
        break;
    }

    return conflict.base;
}

std::vector<ProjectMerge::ConflictSide>
sidesOf(const ProjectPtr& project,
        const Serialization::MergeConflict& conflict)
{
    std::vector<ProjectMerge::ConflictSide> sides;
    for (const ProjectMerge::ConflictChoice& choice : ProjectMerge::availableChoices(project, conflict)) {
        if (choice.option.empty()) {
            sides.push_back(choice.side);
        }
    }

    return sides;
}

QColor
withAlpha(QColor color,
          double alpha)
{
    color.setAlphaF(alpha);

    return color;
}

} // anon namespace

MergeConflictPreview::MergeConflictPreview(NodeGraph* graph,
                                           QGraphicsItem* nodeRoot)
    : QObject(graph)
    , _graph(graph)
    , _nodeRoot(nodeRoot)
    , _shown()
    , _hovered()
    , _wires(new NodeDiffWireItem(nodeRoot))
    , _ghosts()
    , _marks()
    , _picks()
{
    _wires->setObjectName(QString::fromUtf8("MergeConflictPreviewWires"));
    _wires->setZValue(kPreviewDepth);
    _wires->hide();
}

MergeConflictPreview::~MergeConflictPreview()
{
    // The scene owns the items.
}

bool
MergeConflictPreview::hasPreview(Serialization::MergeConflictKind kind)
{
    switch (kind) {
    case Serialization::MergeConflictKind::eInput:
    case Serialization::MergeConflictKind::eStructuralInsert:
    case Serialization::MergeConflictKind::eReparent:
    case Serialization::MergeConflictKind::eDeleteModify:
    case Serialization::MergeConflictKind::eSpliceChoice:
        return true;
    default:
        break;
    }

    return false;
}

QColor
MergeConflictPreview::sideColor(ProjectMerge::ConflictSide side)
{
    switch (side) {
    case ProjectMerge::ConflictSide::eMine:
        return QColor(80, 160, 255);
    case ProjectMerge::ConflictSide::eTheirs:
        return QColor(225, 90, 225);
    case ProjectMerge::ConflictSide::eBase:
        break;
    }

    return QColor(170, 170, 170);
}

void
MergeConflictPreview::setShownConflicts(const std::vector<std::size_t>& indices)
{
    if (_shown != indices) {
        _shown = indices;
        redraw();
    }
}

void
MergeConflictPreview::hover(std::size_t index,
                            ProjectMerge::ConflictSide side)
{
    const std::pair<std::size_t, ProjectMerge::ConflictSide> hovered(index, side);
    if (!_hovered || (*_hovered != hovered)) {
        _hovered = hovered;
        redraw();
    }
}

void
MergeConflictPreview::clearHover()
{
    if (_hovered) {
        _hovered.reset();
        redraw();
    }
}

void
MergeConflictPreview::clear()
{
    _shown.clear();
    _hovered.reset();
    redraw();
}

int
MergeConflictPreview::pickCount() const
{
    return (int)_picks.size();
}

const MergeConflictPreview::Pick*
MergeConflictPreview::pickUnder(const QPointF& scenePos) const
{
    const QPointF local = _nodeRoot->mapFromScene(scenePos);
    // Wire ends sit over ghosts, so they are tried first, last drawn first.
    for (int round = 1; round >= 0; --round) {
        for (std::vector<Pick>::const_reverse_iterator it = _picks.rbegin(); it != _picks.rend(); ++it) {
            if (it->round != (round == 1)) {
                continue;
            }
            if (it->round) {
                const QPointF d = local - it->area.center();
                const double r = it->area.width() / 2.;
                if (d.x() * d.x() + d.y() * d.y() <= r * r) {
                    return &*it;
                }
            } else if (it->area.contains(local)) {
                return &*it;
            }
        }
    }

    return 0;
}

bool
MergeConflictPreview::isPickAt(const QPointF& scenePos) const
{
    return pickUnder(scenePos) != 0;
}

bool
MergeConflictPreview::pickAt(const QPointF& scenePos,
                             QString* error)
{
    const Pick* pick = pickUnder(scenePos);
    if (!pick) {
        return false;
    }
    const std::size_t index = pick->index;
    const ProjectMerge::ConflictSide side = pick->side;
    _hovered.reset();
    error->clear();
    ignore_result(MergeConflictResolve::resolve(_graph->getGui(), std::vector<std::size_t>(1, index), side, error));

    return true;
}

void
MergeConflictPreview::redraw()
{
    for (NodeDiffGhostItem* ghost : _ghosts) {
        delete ghost;
    }
    _ghosts.clear();
    for (QGraphicsRectItem* mark : _marks) {
        delete mark;
    }
    _marks.clear();
    _picks.clear();

    Gui* gui = _graph->getGui();
    const ProjectPtr project = gui ? gui->getApp()->getProject() : ProjectPtr();
    std::vector<NodeDiffWireItem::Wire> wires;
    if (!project) {
        _wires->setWires(wires);
        _wires->hide();

        return;
    }
    const std::vector<Serialization::MergeConflict>& conflicts = project->getMergeConflicts();

    std::map<std::string, NodeGuiPtr> live;
    for (const NodeGuiPtr& node : _graph->getAllActiveNodes()) {
        const NodePtr internal = node ? node->getNode() : NodePtr();
        if (internal && node->isVisible()) {
            live[internal->getNodeId()] = node;
        }
    }
    const auto liveRect = [&](const std::string& id, QRectF* rect) {
        std::map<std::string, NodeGuiPtr>::const_iterator found = live.find(id);
        if (found == live.end()) {
            return false;
        }
        *rect = found->second->mapToItem(_nodeRoot, found->second->boundingRect()).boundingRect();

        return true;
    };
    const auto mark = [&](const std::string& id, const QColor& pen, const QColor& fill) {
        QRectF rect;
        if (!liveRect(id, &rect)) {
            return;
        }
        QGraphicsRectItem* item = new QGraphicsRectItem(rect.adjusted(-3., -3., 3., 3.), _nodeRoot);
        QPen outline(pen);
        outline.setWidthF(2.);
        outline.setCosmetic(true);
        item->setPen((pen.alpha() == 0) ? QPen(Qt::NoPen) : outline);
        item->setBrush(fill);
        item->setZValue(kPreviewDepth);
        item->setAcceptedMouseButtons(Qt::NoButton);
        item->setAcceptHoverEvents(false);
        _marks.push_back(item);
    };

    std::vector<std::size_t> drawn = _shown;
    if (_hovered && (std::find(drawn.begin(), drawn.end(), _hovered->first) == drawn.end())) {
        drawn.push_back(_hovered->first);
    }
    const QSizeF ghostSize(TO_DPIX(NODE_WIDTH), TO_DPIY(NODE_HEIGHT));
    for (std::size_t index : drawn) {
        if ((index >= conflicts.size()) || !hasPreview(conflicts[index].kind)) {
            continue;
        }
        const Serialization::MergeConflict& conflict = conflicts[index];
        for (ProjectMerge::ConflictSide side : sidesOf(project, conflict)) {
            const bool full = _hovered && (_hovered->first == index) && (_hovered->second == side);
            const QColor color = withAlpha(sideColor(side), full ? 1. : kFaintAlpha);
            const Serialization::MergeConflictValue& value = valueOf(conflict, side);

            std::map<std::string, QRectF> ghosts;
            if (value.node && !live.count(value.node->id)) {
                const QRectF rect(QPointF(value.node->layout.x, value.node->layout.y), ghostSize);
                const QString label = QString::fromStdString(value.node->label.empty() ? value.node->scriptName : value.node->label);
                NodeDiffGhostItem* ghost = new NodeDiffGhostItem(_nodeRoot, value.node->id, rect, label, color);
                ghost->setObjectName(QString::fromUtf8("MergeConflictPreviewGhost"));
                ghost->setZValue(kPreviewDepth);
                _ghosts.push_back(ghost);
                ghosts[value.node->id] = rect;
                _picks.push_back(Pick { rect, false, index, side });
            }
            const auto rectOf = [&](const std::string& id, QRectF* rect) {
                std::map<std::string, QRectF>::const_iterator ghost = ghosts.find(id);
                if (ghost != ghosts.end()) {
                    *rect = ghost->second;

                    return true;
                }

                return liveRect(id, rect);
            };
            for (const Serialization::MergeConflictConnection& connection : value.connections) {
                QRectF source;
                QRectF reader;
                if (connection.sourceId.empty() || !rectOf(connection.sourceId, &source) || !rectOf(connection.nodeId, &reader)) {
                    continue;
                }
                const QLineF line = NodeGraphDiffOverlay::wireBetween(source, reader);
                wires.push_back(NodeDiffWireItem::Wire { line, color });
                const QPointF end = line.p2();
                _picks.push_back(Pick { QRectF(end.x() - kWireEndRadius, end.y() - kWireEndRadius, 2. * kWireEndRadius, 2. * kWireEndRadius), true, index, side });
            }
            if (!full) {
                continue;
            }
            if (conflict.kind == Serialization::MergeConflictKind::eStructuralInsert) {
                // A side's removedNodes are the other side's inserts, which choosing it deletes.
                const Serialization::MergeConflictValue& other = valueOf(conflict, (side == ProjectMerge::ConflictSide::eMine) ? ProjectMerge::ConflictSide::eTheirs : ProjectMerge::ConflictSide::eMine);
                for (const std::string& id : other.removedNodes) {
                    mark(id, sideColor(side), Qt::NoBrush);
                }
                for (const std::string& id : value.removedNodes) {
                    mark(id, QColor(0, 0, 0, 0), QColor(0, 0, 0, 150));
                }
            } else if (conflict.kind == Serialization::MergeConflictKind::eDeleteModify) {
                const bool deletes = (side == ProjectMerge::ConflictSide::eMine) == (conflict.side == Serialization::MergeSide::eMine);
                if (deletes && (side != ProjectMerge::ConflictSide::eBase) && conflict.knob.empty() && conflict.item.empty()) {
                    mark(conflict.nodeId, QColor(0, 0, 0, 0), QColor(0, 0, 0, 150));
                }
            }
        }
    }
    _wires->setVisible(!wires.empty());
    _wires->setWires(std::move(wires));
} // MergeConflictPreview::redraw

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING
#include "moc_MergeConflictPreview.cpp"
