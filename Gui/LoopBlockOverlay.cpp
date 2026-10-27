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

#include "LoopBlockOverlay.h"

#include <algorithm>
#include <cmath>
#include <map>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QPainter>
#include <QtCore/QMetaObject>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/Node.h"
#include "Engine/Nodes/Loop/LoopBegin.h"
#include "Engine/Nodes/Loop/LoopEnd.h"

#include "Gui/NodeGraph.h"
#include "Gui/NodeGui.h"

NATRON_NAMESPACE_ENTER

namespace {
// Below the nodes (20) and above backdrops (10).
const double kLoopBlockDepth = 15.;
const double kHullPadding = 18.;
const double kWedgeHeight = 14.;

double
cross(const QPointF& o,
      const QPointF& a,
      const QPointF& b)
{
    return (a.x() - o.x()) * (b.y() - o.y()) - (a.y() - o.y()) * (b.x() - o.x());
}

// Andrew's monotone chain.
QPolygonF
convexHull(std::vector<QPointF> points)
{
    std::sort(points.begin(), points.end(), [](const QPointF& a, const QPointF& b) {
        return (a.x() < b.x()) || ((a.x() == b.x()) && (a.y() < b.y()));
    });
    if (points.size() < 3) {
        return QPolygonF(QVector<QPointF>(points.begin(), points.end()));
    }
    std::vector<QPointF> hull(2 * points.size());
    std::size_t k = 0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        while ((k >= 2) && (cross(hull[k - 2], hull[k - 1], points[i]) <= 0.)) {
            --k;
        }
        hull[k++] = points[i];
    }
    for (std::size_t i = points.size() - 1, t = k + 1; i > 0; --i) {
        while ((k >= t) && (cross(hull[k - 2], hull[k - 1], points[i - 1]) <= 0.)) {
            --k;
        }
        hull[k++] = points[i - 1];
    }
    hull.resize(k - 1);

    return QPolygonF(QVector<QPointF>(hull.begin(), hull.end()));
}

NodeGui*
guiOf(const NodePtr& node)
{
    return node ? dynamic_cast<NodeGui*>(node->getNodeGui().get()) : 0;
}

QRectF
sceneRectOf(NodeGui* gui)
{
    return gui->mapToScene(gui->boundingRect()).boundingRect();
}
} // anon namespace

//////////////////////////////// LoopBlockItem

LoopBlockItem::LoopBlockItem(QGraphicsItem* parent)
    : QGraphicsObject(parent)
    , _blocks()
    , _bounds()
{
    setZValue(kLoopBlockDepth);
    setAcceptedMouseButtons(Qt::NoButton);
    setAcceptHoverEvents(false);
}

void
LoopBlockItem::setBlocks(const std::vector<Block>& blocks)
{
    QRectF bounds;

    for (const Block& block : blocks) {
        bounds = bounds.united(block.hull.boundingRect());
        bounds = bounds.united(block.beginWedge.boundingRect());
        bounds = bounds.united(block.endWedge.boundingRect());
    }
    // Room for the outline and rings drawn outside the shapes.
    bounds.adjust(-8., -8., 8., 8.);
    prepareGeometryChange();
    _blocks = blocks;
    _bounds = mapRectFromScene(bounds);
    update();
}

QRectF
LoopBlockItem::boundingRect() const
{
    return _bounds;
}

void
LoopBlockItem::paint(QPainter* painter,
                     const QStyleOptionGraphicsItem* /*option*/,
                     QWidget* /*widget*/)
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    for (const Block& block : _blocks) {
        QColor fill = block.color;
        fill.setAlpha(block.highlighted ? 60 : 36);
        QColor line = block.color;
        line.setAlpha(block.highlighted ? 255 : 110);
        painter->setPen(QPen(line, block.highlighted ? 2.5 : 1., block.highlighted ? Qt::SolidLine : Qt::DashLine));
        painter->setBrush(fill);
        painter->drawPath(mapFromScene(block.hull));

        painter->setPen(QPen(block.color.darker(160), 1.));
        painter->setBrush(block.color);
        painter->drawPolygon(mapFromScene(block.beginWedge));
        painter->drawPolygon(mapFromScene(block.endWedge));

        if (block.highlighted) {
            painter->setBrush(Qt::NoBrush);
            painter->setPen(QPen(block.color.lighter(130), 3.));
            painter->drawRect(mapRectFromScene(block.beginRect.adjusted(-4., -4., 4., 4.)));
            painter->drawRect(mapRectFromScene(block.endRect.adjusted(-4., -4., 4., 4.)));
        }
    }
    painter->restore();
}

//////////////////////////////// LoopBlockOverlay

LoopBlockOverlay::LoopBlockOverlay(NodeGraph* graph,
                                   QGraphicsItem* nodeRoot)
    : QObject(graph)
    , _graph(graph)
    , _item(new LoopBlockItem(nodeRoot))
    , _blocks()
    , _pending(false)
    , _topologyDirty(true)
{
    _item->setObjectName(QString::fromUtf8("LoopBlockItem"));
}

LoopBlockOverlay::~LoopBlockOverlay()
{
    // The scene owns the item.
}

void
LoopBlockOverlay::schedule(bool topology)
{
    _topologyDirty = _topologyDirty || topology;
    if (_pending) {
        return;
    }
    _pending = true;
    QMetaObject::invokeMethod(this, "refresh", Qt::QueuedConnection);
}

QColor
LoopBlockOverlay::pairColor(const std::string& loopName)
{
    // FNV-1a, spread over the hue circle by the golden ratio so neighbouring names differ.
    quint32 hash = 2166136261u;
    for (unsigned char c : loopName) {
        hash = (hash ^ c) * 16777619u;
    }
    const double hue = std::fmod((double)(hash % 1000) * 0.618033988749895, 1.);

    return QColor::fromHsvF(hue, 0.55, 0.85);
}

std::vector<NodePtr>
LoopBlockOverlay::pathNodes(const NodePtr& begin,
                            const NodePtr& end)
{
    std::vector<NodePtr> nodes;
    // Whether a node reaches begin going up; absent until settled, false while on the stack.
    std::map<Node*, bool> reaches;
    std::vector<std::pair<NodePtr, int>> stack;

    if (!begin || !end) {
        return nodes;
    }
    reaches[begin.get()] = true;
    reaches[end.get()] = false;
    stack.push_back(std::make_pair(end, 0));
    while (!stack.empty()) {
        const NodePtr node = stack.back().first;
        const int input = stack.back().second;
        if (input < node->getNInputs()) {
            ++stack.back().second;
            const NodePtr up = node->getInput(input);
            if (up && (reaches.find(up.get()) == reaches.end())) {
                reaches[up.get()] = false;
                stack.push_back(std::make_pair(up, 0));
            }
            continue;
        }
        bool found = false;
        for (int i = 0; i < node->getNInputs(); ++i) {
            const NodePtr up = node->getInput(i);
            found = found || (up && reaches[up.get()]);
        }
        reaches[node.get()] = found;
        if (found && (node != end)) {
            nodes.push_back(node);
        }
        stack.pop_back();
    }

    return nodes;
}

void
LoopBlockOverlay::recomputeMembership()
{
    _blocks.clear();
    const NodesGuiList& guis = _graph->getAllActiveNodes();
    for (NodesGuiList::const_iterator it = guis.begin(); it != guis.end(); ++it) {
        const NodePtr end = (*it)->getNode();
        if (!LoopEnd::isLoopEnd(end)) {
            continue;
        }
        LoopEnd* effect = dynamic_cast<LoopEnd*>(end->getEffectInstance().get());
        const NodePtr begin = effect ? effect->getLoopBegin() : NodePtr();
        if (!begin) {
            continue;
        }
        Membership block;
        block.begin = begin;
        block.end = end;
        LoopBegin* beginEffect = dynamic_cast<LoopBegin*>(begin->getEffectInstance().get());
        block.name = beginEffect ? beginEffect->getLoopName() : begin->getScriptName_mt_safe();
        const std::vector<NodePtr> nodes = pathNodes(begin, end);
        for (const NodePtr& node : nodes) {
            block.nodes.push_back(node);
        }
        _blocks.push_back(block);
    }
    // Outer blocks first, so nested hulls paint over them.
    std::stable_sort(_blocks.begin(), _blocks.end(), [](const Membership& a, const Membership& b) {
        return a.nodes.size() > b.nodes.size();
    });
}

void
LoopBlockOverlay::refresh()
{
    _pending = false;
    if (!_item) {
        return;
    }
    if (_topologyDirty) {
        _topologyDirty = false;
        recomputeMembership();
    }

    std::vector<LoopBlockItem::Block> shapes;
    for (const Membership& block : _blocks) {
        NodeGui* beginGui = guiOf(block.begin.lock());
        NodeGui* endGui = guiOf(block.end.lock());
        if (!beginGui || !endGui || !beginGui->isVisible() || !endGui->isVisible()) {
            continue;
        }
        LoopBlockItem::Block shape;
        shape.color = pairColor(block.name);
        shape.beginRect = sceneRectOf(beginGui);
        shape.endRect = sceneRectOf(endGui);
        shape.highlighted = beginGui->getIsSelected() || endGui->getIsSelected();

        std::vector<QRectF> rects;
        rects.push_back(shape.beginRect);
        rects.push_back(shape.endRect);
        for (const NodeWPtr& weak : block.nodes) {
            NodeGui* gui = guiOf(weak.lock());
            if (gui && gui->isVisible()) {
                rects.push_back(sceneRectOf(gui));
            }
        }
        std::vector<QPointF> corners;
        for (const QRectF& r : rects) {
            const QRectF padded = r.adjusted(-kHullPadding, -kHullPadding, kHullPadding, kHullPadding);
            corners.push_back(padded.topLeft());
            corners.push_back(padded.topRight());
            corners.push_back(padded.bottomLeft());
            corners.push_back(padded.bottomRight());
        }
        shape.hull.addPolygon(convexHull(corners));
        shape.hull.closeSubpath();

        // The begin opens the block downwards and the end closes it from above, like a bracket pair.
        const QRectF& b = shape.beginRect;
        shape.beginWedge << b.bottomLeft() << b.bottomRight() << QPointF(b.center().x(), b.bottom() + kWedgeHeight);
        const QRectF& e = shape.endRect;
        shape.endWedge << e.topLeft() << e.topRight() << QPointF(e.center().x(), e.top() - kWedgeHeight);
        shapes.push_back(shape);
    }
    _item->setBlocks(shapes);
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_LoopBlockOverlay.cpp"
