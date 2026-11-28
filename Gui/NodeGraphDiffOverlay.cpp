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

#include "NodeGraphDiffOverlay.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QBrush>
#include <QColor>
#include <QFont>
#include <QGraphicsRectItem>
#include <QPainter>
#include <QPen>
#include <QtCore/QMetaObject>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/AppManager.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/ProjectMerge.h"

#include "Gui/Gui.h"
#include "Gui/GuiDefines.h"
#include "Gui/NodeGraph.h"
#include "Gui/NodeGui.h"

NATRON_NAMESPACE_ENTER

namespace {

// GpuRegionOverlay tags its scrims under key 0 too, by another name.
const int kTintKey = 0;
const char* const kTintName = "NodeDiffTint";
// Above the edges and below the GPU region outlines.
const double kGhostDepth = 13.;

QColor
tintColor(NodeGraphDiffOverlay::TintEnum tint)
{
    return (tint == NodeGraphDiffOverlay::eTintAdded) ? QColor(70, 200, 90) : QColor(255, 176, 30);
}

bool
onlyMoved(const ProjectMerge::NodeChange& change)
{
    return change.moved && change.knobs.empty() && change.inputs.empty() && !change.renamed && !change.appearance && !change.userKnobs && !change.roto && !change.tracker && !change.recreated;
}

QGraphicsRectItem*
tintItemOf(NodeGui* gui)
{
    for (QGraphicsItem* child : gui->childItems()) {
        if (child->data(kTintKey).toString() == QString::fromUtf8(kTintName)) {
            return static_cast<QGraphicsRectItem*>(child);
        }
    }

    return 0;
}

// Where the segment from inside rect towards outside leaves rect.
QPointF
edgePoint(const QRectF& rect,
          const QPointF& outside)
{
    const QPointF center = rect.center();
    const double dx = outside.x() - center.x();
    const double dy = outside.y() - center.y();
    if ((dx == 0.) && (dy == 0.)) {
        return center;
    }
    const double sx = (dx != 0.) ? (rect.width() / 2.) / std::abs(dx) : 1e300;
    const double sy = (dy != 0.) ? (rect.height() / 2.) / std::abs(dy) : 1e300;
    const double s = std::min(1., std::min(sx, sy));

    return QPointF(center.x() + dx * s, center.y() + dy * s);
}

QLineF
wireBetween(const QRectF& source,
            const QRectF& reader)
{
    return QLineF(edgePoint(source, reader.center()), edgePoint(reader, source.center()));
}

} // namespace

NodeDiffGhostItem::NodeDiffGhostItem(QGraphicsItem* parent)
    : QGraphicsObject(parent)
    , _ghosts()
    , _wires()
    , _bounds()
{
    setZValue(kGhostDepth);
    setAcceptedMouseButtons(Qt::NoButton);
    setAcceptHoverEvents(false);
}

void
NodeDiffGhostItem::setShapes(std::vector<Ghost> ghosts,
                             std::vector<QLineF> wires)
{
    QRectF bounds;
    for (const Ghost& ghost : ghosts) {
        bounds = bounds.united(ghost.rect.adjusted(-2., -2., 2., 2.));
    }
    for (const QLineF& wire : wires) {
        bounds = bounds.united(QRectF(wire.p1(), wire.p2()).normalized().adjusted(-2., -2., 2., 2.));
    }
    update(_bounds);
    if (bounds != _bounds) {
        prepareGeometryChange();
        _bounds = bounds;
    }
    _ghosts = std::move(ghosts);
    _wires = std::move(wires);
    update(_bounds);
}

QRectF
NodeDiffGhostItem::boundingRect() const
{
    return _bounds;
}

void
NodeDiffGhostItem::paint(QPainter* painter,
                         const QStyleOptionGraphicsItem* /*option*/,
                         QWidget* /*widget*/)
{
    const QColor removed(230, 70, 60);
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);

    QPen wirePen(removed);
    wirePen.setWidthF(1.5);
    wirePen.setStyle(Qt::DashLine);
    wirePen.setCosmetic(true);
    painter->setPen(wirePen);
    for (const QLineF& wire : _wires) {
        painter->drawLine(wire);
    }

    QColor fill = removed;
    fill.setAlphaF(0.25);
    QPen outline(removed);
    outline.setWidthF(1.5);
    outline.setStyle(Qt::DashLine);
    outline.setCosmetic(true);
    QFont font;
    font.setPixelSize(11);
    painter->setFont(font);
    for (const Ghost& ghost : _ghosts) {
        painter->setPen(outline);
        painter->setBrush(fill);
        painter->drawRect(ghost.rect);
        painter->setPen(removed.lighter(140));
        painter->drawText(ghost.rect, Qt::AlignCenter, ghost.label);
    }
    painter->restore();
}

NodeGraphDiffOverlay::NodeGraphDiffOverlay(NodeGraph* graph,
                                           QGraphicsItem* nodeRoot)
    : QObject(graph)
    , _graph(graph)
    , _ghosts(new NodeDiffGhostItem(nodeRoot))
    , _tints()
    , _neighbours()
    , _pending(false)
{
    _ghosts->setObjectName(QString::fromUtf8("NodeDiffGhostItem"));
    _ghosts->hide();
    Gui* gui = graph->getGui();
    if (gui) {
        QObject::connect(gui, SIGNAL(graphComparisonChanged()), this, SLOT(refresh()));
    }
    schedule();
}

NodeGraphDiffOverlay::~NodeGraphDiffOverlay()
{
    // The scene owns the items.
}

std::string
NodeGraphDiffOverlay::groupIdOf(const NodeGraph* graph)
{
    const std::shared_ptr<NodeGroup> group = std::dynamic_pointer_cast<NodeGroup>(graph->getGroup());
    const NodePtr node = group ? group->getNode() : NodePtr();

    return node ? node->getNodeId() : std::string();
}

void
NodeGraphDiffOverlay::schedule()
{
    if (_pending) {
        return;
    }
    _pending = true;
    QMetaObject::invokeMethod(this, "refresh", Qt::QueuedConnection);
}

void
NodeGraphDiffOverlay::noteNodeChanged(NodeGui* node)
{
    if (!node) {
        return;
    }
    if (_tints.count(node)) {
        fitTint(node);
    }
    if (_neighbours.count(node)) {
        schedule();
    }
}

NodeGraphDiffOverlay::TintEnum
NodeGraphDiffOverlay::tintOf(const NodeGui* node) const
{
    std::unordered_map<const NodeGui*, TintEnum>::const_iterator found = _tints.find(node);

    return (found == _tints.end()) ? eTintNone : found->second;
}

int
NodeGraphDiffOverlay::ghostCount() const
{
    return _ghosts->isVisible() ? (int)_ghosts->ghostCount() : 0;
}

QRectF
NodeGraphDiffOverlay::ghostRect(int index) const
{
    if ((index < 0) || (index >= ghostCount())) {
        return QRectF();
    }

    return _ghosts->ghosts()[index].rect;
}

void
NodeGraphDiffOverlay::fitTint(NodeGui* gui)
{
    QGraphicsRectItem* tint = tintItemOf(gui);
    if (!tint) {
        return;
    }
    qreal top = 0.;
    for (QGraphicsItem* child : gui->childItems()) {
        if (child != tint) {
            top = std::max(top, child->zValue());
        }
    }
    const QRectF rect = gui->boundingRect();
    if ((tint->zValue() != top + 1.) || (tint->rect() != rect)) {
        tint->setZValue(top + 1.);
        tint->setRect(rect);
        _graph->repaintLowDetailItem(gui);
    }
}

void
NodeGraphDiffOverlay::refresh()
{
    _pending = false;
    Gui* gui = _graph->getGui();
    const ProjectMerge::ChangeSummary* summary = gui ? gui->getGraphComparison() : 0;

    std::map<std::string, const ProjectMerge::NodeChange*> changes;
    if (summary) {
        for (const ProjectMerge::NodeChange& change : summary->nodes) {
            changes[change.nodeId] = &change;
        }
    }

    std::map<std::string, NodeGuiPtr> live;
    std::unordered_map<const NodeGui*, TintEnum> tints;
    bool repainted = false;
    for (const NodeGuiPtr& node : _graph->getAllActiveNodes()) {
        const NodePtr internal = node ? node->getNode() : NodePtr();
        if (!internal) {
            continue;
        }
        const std::string id = internal->getNodeId();
        live[id] = node;
        TintEnum tint = eTintNone;
        std::map<std::string, const ProjectMerge::NodeChange*>::const_iterator found = changes.find(id);
        if (found != changes.end()) {
            if (found->second->kind == ProjectMerge::NodeChange::eAdded) {
                tint = eTintAdded;
            } else if (found->second->kind == ProjectMerge::NodeChange::eChanged) {
                tint = onlyMoved(*found->second) ? eTintMoved : eTintChanged;
            }
        }
        QGraphicsRectItem* item = tintItemOf(node.get());
        if (tint == eTintNone) {
            if (item) {
                delete item;
                _graph->repaintLowDetailItem(node.get());
                repainted = true;
            }
            continue;
        }
        tints[node.get()] = tint;
        const QColor color = tintColor(tint);
        QColor fill = color;
        fill.setAlphaF((tint == eTintMoved) ? 0. : 0.35);
        QPen pen(color);
        pen.setWidthF(2.);
        pen.setCosmetic(true);
        if (!item) {
            item = new QGraphicsRectItem(node.get());
            item->setData(kTintKey, QString::fromUtf8(kTintName));
            item->setAcceptedMouseButtons(Qt::NoButton);
            item->setAcceptHoverEvents(false);
        }
        if ((item->brush().color() != fill) || (item->pen().color() != color)) {
            item->setBrush(fill);
            item->setPen(pen);
            _graph->repaintLowDetailItem(node.get());
            repainted = true;
        }
    }
    _tints = std::move(tints);
    for (const std::pair<const NodeGui* const, TintEnum>& entry : _tints) {
        fitTint(const_cast<NodeGui*>(entry.first));
    }

    const std::string groupId = groupIdOf(this->_graph);
    const QSizeF size(TO_DPIX(NODE_WIDTH), TO_DPIY(NODE_HEIGHT));
    std::map<std::string, QRectF> ghostRects;
    std::vector<NodeDiffGhostItem::Ghost> ghosts;
    if (summary) {
        for (const ProjectMerge::NodeChange& change : summary->nodes) {
            if ((change.kind != ProjectMerge::NodeChange::eRemoved) || (change.parentId != groupId) || !change.hasPosition) {
                continue;
            }
            const QRectF rect(QPointF(change.placedX, change.placedY), size);
            ghostRects[change.nodeId] = rect;
            ghosts.push_back(NodeDiffGhostItem::Ghost { rect, QString::fromStdString(change.label.empty() ? change.scriptName : change.label) });
        }
    }
    std::vector<QLineF> wires;
    std::unordered_set<const NodeGui*> neighbours;
    const auto rectOf = [&](const std::string& id, QRectF* rect) {
        std::map<std::string, QRectF>::const_iterator ghost = ghostRects.find(id);
        if (ghost != ghostRects.end()) {
            *rect = ghost->second;

            return true;
        }
        std::map<std::string, NodeGuiPtr>::const_iterator node = live.find(id);
        if ((node == live.end()) || !node->second->isVisible()) {
            return false;
        }
        neighbours.insert(node->second.get());
        *rect = node->second->mapToItem(_ghosts, node->second->boundingRect()).boundingRect();

        return true;
    };
    if (summary) {
        for (const ProjectMerge::NodeChange& change : summary->nodes) {
            std::map<std::string, QRectF>::const_iterator self = ghostRects.find(change.nodeId);
            if ((change.kind != ProjectMerge::NodeChange::eRemoved) || (self == ghostRects.end())) {
                continue;
            }
            QRectF other;
            for (const ProjectMerge::NodeChange::Link& source : change.sources) {
                if (rectOf(source.nodeId, &other)) {
                    wires.push_back(wireBetween(other, self->second));
                }
            }
            // A wire between two ghosts is drawn once, from the reader's sources.
            for (const ProjectMerge::NodeChange::Link& reader : change.readers) {
                if (!ghostRects.count(reader.nodeId) && rectOf(reader.nodeId, &other)) {
                    wires.push_back(wireBetween(self->second, other));
                }
            }
        }
    }
    _neighbours = std::move(neighbours);
    const bool hadGhosts = _ghosts->isVisible();
    _ghosts->setVisible(!ghosts.empty());
    _ghosts->setShapes(std::move(ghosts), std::move(wires));
    if (repainted || hadGhosts || _ghosts->isVisible()) {
        _graph->invalidateNavigatorImage();
    }
} // NodeGraphDiffOverlay::refresh

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING
#include "moc_NodeGraphDiffOverlay.cpp"
