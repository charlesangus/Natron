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
#include <set>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QPainter>
#include <QtCore/QMetaObject>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/AppInstance.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Loop/LoopBlock.h"
#include "Engine/Project.h"
#include "Engine/TemplateNode.h"

#include "Serialization/ProjectSerialization.h"

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

TemplateNode*
templateEffectOf(const NodePtr& node)
{
    return node ? dynamic_cast<TemplateNode*>(node->getEffectInstance().get()) : 0;
}

QPainterPath
paddedHull(const std::vector<QRectF>& rects)
{
    std::vector<QPointF> corners;
    for (const QRectF& r : rects) {
        const QRectF padded = r.adjusted(-kHullPadding, -kHullPadding, kHullPadding, kHullPadding);
        corners.push_back(padded.topLeft());
        corners.push_back(padded.topRight());
        corners.push_back(padded.bottomLeft());
        corners.push_back(padded.bottomRight());
    }
    QPainterPath hull;
    hull.addPolygon(convexHull(corners));
    hull.closeSubpath();

    return hull;
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
LoopBlockItem::setBlocks(const std::vector<Block>& sceneBlocks)
{
    // Panning moves the parent, so the shapes are kept in the item's own coordinates, which move with the nodes.
    std::vector<Block> blocks = sceneBlocks;
    QRectF bounds;

    for (Block& block : blocks) {
        block.hull = mapFromScene(block.hull);
        for (QPolygonF& wedge : block.beginWedges) {
            wedge = mapFromScene(wedge);
        }
        block.endWedge = mapFromScene(block.endWedge);
        for (QRectF& rect : block.beginRects) {
            rect = mapRectFromScene(rect);
        }
        block.endRect = mapRectFromScene(block.endRect);
        bounds = bounds.united(block.hull.boundingRect());
        for (const QPolygonF& wedge : block.beginWedges) {
            bounds = bounds.united(wedge.boundingRect());
        }
        bounds = bounds.united(block.endWedge.boundingRect());
    }
    // Room for the outline and rings drawn outside the shapes.
    bounds.adjust(-8., -8., 8., 8.);
    prepareGeometryChange();
    _blocks = blocks;
    _bounds = bounds;
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
        painter->drawPath(block.hull);
        if (block.hullOnly) {
            continue;
        }

        painter->setPen(QPen(block.color.darker(160), 1.));
        painter->setBrush(block.color);
        for (const QPolygonF& wedge : block.beginWedges) {
            painter->drawPolygon(wedge);
        }
        painter->drawPolygon(block.endWedge);

        if (block.highlighted) {
            painter->setBrush(Qt::NoBrush);
            painter->setPen(QPen(block.color.lighter(130), 3.));
            for (const QRectF& rect : block.beginRects) {
                painter->drawRect(rect.adjusted(-4., -4., 4., 4.));
            }
            painter->drawRect(block.endRect.adjusted(-4., -4., 4., 4.));
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
    , _watchedNames()
    , _pending(false)
    , _topologyDirty(true)
{
    _item->setObjectName(QString::fromUtf8("LoopBlockItem"));
    NodeCollectionPtr group = graph->getGroup();
    AppInstancePtr app = group ? group->getApplication() : AppInstancePtr();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    if (project) {
        QObject::connect(project.get(), &Project::templateImportsChanged, this, &LoopBlockOverlay::onTemplateMembershipChanged);
        // Origins may change off the main thread; the queued connection brings the refresh back to it.
        QObject::connect(project.get(), &Project::nodeOriginsChanged, this, &LoopBlockOverlay::onTemplateMembershipChanged, Qt::QueuedConnection);
    }
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
LoopBlockOverlay::bodyNodes(const NodePtr& end)
{
    std::vector<NodePtr> nodes;
    std::set<Node*> members;
    // Each node with the count of block ends less block begins between it and end. The pairing
    // walk goes down from every node; walking up instead visits each node once per nesting level.
    // An end's body is its first input: the others, a for-each's Piece, are outside its block.
    std::set<std::pair<Node*, int>> visited;
    std::vector<std::pair<NodePtr, int>> stack;

    if (!end) {
        return nodes;
    }
    if (end->getNInputs() > 0) {
        stack.push_back(std::make_pair(end->getInput(0), 0));
    }
    while (!stack.empty()) {
        const NodePtr node = stack.back().first;
        int open = stack.back().second;
        stack.pop_back();
        if (!node || (node == end) || !visited.insert(std::make_pair(node.get(), open)).second) {
            continue;
        }
        if (members.insert(node.get()).second) {
            nodes.push_back(node);
        }
        if (LoopBlocks::isEnd(node)) {
            for (int i = 1; i < node->getNInputs(); ++i) {
                stack.push_back(std::make_pair(node->getInput(i), open));
            }
            if (node->getNInputs() > 0) {
                stack.push_back(std::make_pair(node->getInput(0), open + 1));
            }
            continue;
        }
        if (LoopBlocks::isBegin(node)) {
            // Nodes above a begin with no end open below it feed that begin's block from
            // outside, so they are not in end's body.
            if (open == 0) {
                continue;
            }
            --open;
        }
        for (int i = 0; i < node->getNInputs(); ++i) {
            stack.push_back(std::make_pair(node->getInput(i), open));
        }
    }

    return nodes;
}

void
LoopBlockOverlay::disconnectNames()
{
    for (const KnobIWPtr& weak : _watchedNames) {
        KnobIPtr knob = weak.lock();
        KnobHelper* helper = dynamic_cast<KnobHelper*>(knob.get());
        if (helper && helper->getSignalSlotHandler()) {
            QObject::disconnect(helper->getSignalSlotHandler().get(), 0, this, 0);
        }
    }
    _watchedNames.clear();
}

void
LoopBlockOverlay::onLoopNameChanged()
{
    schedule(true);
}

void
LoopBlockOverlay::onTemplateMembershipChanged()
{
    schedule(true);
}

std::vector<NodePtr>
LoopBlockOverlay::templateMembers(const NodesList& graphNodes,
                                  const std::string& key,
                                  const std::set<std::string>& excluded)
{
    std::set<Node*> inGraph;
    std::map<Node*, std::vector<NodePtr>> outputs;
    std::vector<NodePtr> seeds;
    for (const NodePtr& node : graphNodes) {
        inGraph.insert(node.get());
    }
    for (const NodePtr& node : graphNodes) {
        for (int i = 0; i < node->getNInputs(); ++i) {
            const NodePtr input = node->getInput(i);
            if (input && inGraph.count(input.get())) {
                outputs[input.get()].push_back(node);
            }
        }
        if (!templateEffectOf(node) && (node->getOriginTemplate() == key)) {
            seeds.push_back(node);
        }
    }

    // A node lies between members when it is both downstream and upstream of one. The walks
    // cross excluded nodes and template nodes, so a node beyond one is still enclosed.
    std::set<Node*> seedSet;
    for (const NodePtr& seed : seeds) {
        seedSet.insert(seed.get());
    }
    auto walk = [&](bool down) {
        std::set<Node*> reached;
        std::vector<NodePtr> stack(seeds);
        while (!stack.empty()) {
            const NodePtr node = stack.back();
            stack.pop_back();
            std::vector<NodePtr> next;
            if (down) {
                std::map<Node*, std::vector<NodePtr>>::const_iterator found = outputs.find(node.get());
                if (found != outputs.end()) {
                    next = found->second;
                }
            } else {
                for (int i = 0; i < node->getNInputs(); ++i) {
                    const NodePtr input = node->getInput(i);
                    if (input && inGraph.count(input.get())) {
                        next.push_back(input);
                    }
                }
            }
            for (const NodePtr& n : next) {
                if (!seedSet.count(n.get()) && reached.insert(n.get()).second) {
                    stack.push_back(n);
                }
            }
        }

        return reached;
    };
    const std::set<Node*> below = walk(true);
    const std::set<Node*> above = walk(false);

    std::vector<NodePtr> members(seeds);
    for (const NodePtr& node : graphNodes) {
        if (below.count(node.get()) && above.count(node.get()) && !templateEffectOf(node) && !excluded.count(node->getNodeId()) && node->getOriginTemplate().empty()) {
            members.push_back(node);
        }
    }

    return members;
}

std::vector<NodePtr>
LoopBlockOverlay::templateNodesAt(const QPointF& scenePos) const
{
    std::vector<NodePtr> nodes;
    for (const TemplateHit& hit : _templateHits) {
        if (!hit.hull.contains(scenePos)) {
            continue;
        }
        for (const NodeWPtr& weak : hit.nodes) {
            if (NodePtr node = weak.lock()) {
                nodes.push_back(node);
            }
        }

        return nodes;
    }

    return nodes;
}

void
LoopBlockOverlay::recomputeMembership()
{
    _templates.clear();
    {
        NodeCollectionPtr group = _graph->getGroup();
        AppInstancePtr app = group ? group->getApplication() : AppInstancePtr();
        ProjectPtr project = app ? app->getProject() : ProjectPtr();
        NodesList graphNodes;
        const NodesGuiList& all = _graph->getAllActiveNodes();
        for (NodesGuiList::const_iterator it = all.begin(); it != all.end(); ++it) {
            if (NodePtr node = (*it)->getNode()) {
                graphNodes.push_back(node);
            }
        }
        const std::list<Serialization::TemplateImportSerialization> imports = project ? project->getTemplateImports() : std::list<Serialization::TemplateImportSerialization>();
        for (const NodePtr& node : graphNodes) {
            TemplateNode* effect = templateEffectOf(node);
            if (!effect) {
                continue;
            }
            const std::string key = effect->getTemplateKey();
            std::set<std::string> excluded;
            for (const Serialization::TemplateImportSerialization& entry : imports) {
                if (entry.key == key) {
                    excluded = entry.excluded;
                    break;
                }
            }
            TemplateMembership membership;
            membership.templateNode = node;
            if (!key.empty()) {
                for (const NodePtr& member : templateMembers(graphNodes, key, excluded)) {
                    membership.nodes.push_back(member);
                }
            }
            _templates.push_back(membership);
        }
    }

    _blocks.clear();
    disconnectNames();
    const NodesGuiList& guis = _graph->getAllActiveNodes();
    for (NodesGuiList::const_iterator it = guis.begin(); it != guis.end(); ++it) {
        const NodePtr end = (*it)->getNode();
        const LoopBlockEnd* effect = LoopBlockEnd::of(end);
        const std::vector<NodePtr> begins = effect ? effect->getBlockBegins() : std::vector<NodePtr>();
        if (begins.empty()) {
            continue;
        }
        Membership block;
        block.end = end;
        block.name = effect->getBlockLoopName();
        if (block.name.empty()) {
            block.name = end->getScriptName_mt_safe();
        }
        for (const NodePtr& begin : begins) {
            block.begins.push_back(begin);
        }
        const KnobIPtr nameKnob = effect->getBlockNameKnob();
        KnobHelper* helper = dynamic_cast<KnobHelper*>(nameKnob.get());
        if (helper && helper->getSignalSlotHandler()) {
            QObject::connect(helper->getSignalSlotHandler().get(), SIGNAL(valueChanged(ViewSpec, int, int)), this, SLOT(onLoopNameChanged()),
                             Qt::UniqueConnection);
            _watchedNames.push_back(nameKnob);
        }
        const std::vector<NodePtr> nodes = bodyNodes(end);
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
    _templateHits.clear();
    for (const TemplateMembership& membership : _templates) {
        const NodePtr templateNode = membership.templateNode.lock();
        NodeGui* templateGui = guiOf(templateNode);
        if (!templateGui || !templateGui->isVisible()) {
            continue;
        }
        LoopBlockItem::Block shape;
        shape.hullOnly = true;
        shape.color = templateGui->getCurrentColor();
        shape.highlighted = templateGui->getIsSelected();
        std::vector<QRectF> rects(1, sceneRectOf(templateGui));
        TemplateHit hit;
        for (const NodeWPtr& weak : membership.nodes) {
            const NodePtr node = weak.lock();
            NodeGui* gui = guiOf(node);
            if (gui && gui->isVisible()) {
                rects.push_back(sceneRectOf(gui));
                hit.nodes.push_back(node);
            }
        }
        hit.nodes.push_back(templateNode);
        shape.hull = paddedHull(rects);
        hit.hull = shape.hull;
        shapes.push_back(shape);
        _templateHits.push_back(hit);
    }
    std::stable_sort(_templateHits.begin(), _templateHits.end(), [](const TemplateHit& a, const TemplateHit& b) {
        const QRectF ra = a.hull.boundingRect();
        const QRectF rb = b.hull.boundingRect();

        return ra.width() * ra.height() < rb.width() * rb.height();
    });
    for (const Membership& block : _blocks) {
        NodeGui* endGui = guiOf(block.end.lock());
        if (!endGui || !endGui->isVisible()) {
            continue;
        }
        LoopBlockItem::Block shape;
        shape.color = pairColor(block.name);
        shape.endRect = sceneRectOf(endGui);
        shape.highlighted = endGui->getIsSelected();
        for (const NodeWPtr& weak : block.begins) {
            NodeGui* beginGui = guiOf(weak.lock());
            if (beginGui && beginGui->isVisible()) {
                shape.beginRects.push_back(sceneRectOf(beginGui));
                shape.highlighted = shape.highlighted || beginGui->getIsSelected();
            }
        }
        if (shape.beginRects.empty()) {
            continue;
        }

        std::vector<QRectF> rects(shape.beginRects);
        rects.push_back(shape.endRect);
        for (const NodeWPtr& weak : block.nodes) {
            NodeGui* gui = guiOf(weak.lock());
            if (gui && gui->isVisible()) {
                rects.push_back(sceneRectOf(gui));
            }
        }
        shape.hull = paddedHull(rects);

        // The begin opens the block downwards and the end closes it from above, like a bracket pair.
        for (const QRectF& b : shape.beginRects) {
            QPolygonF wedge;
            wedge << b.bottomLeft() << b.bottomRight() << QPointF(b.center().x(), b.bottom() + kWedgeHeight);
            shape.beginWedges.push_back(wedge);
        }
        const QRectF& e = shape.endRect;
        shape.endWedge << e.topLeft() << e.topRight() << QPointF(e.center().x(), e.top() - kWedgeHeight);
        shapes.push_back(shape);
    }
    _item->setBlocks(shapes);
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_LoopBlockOverlay.cpp"
