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

#include "GpuOverlay.h"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QFont>
#include <QFontMetricsF>
#include <QGraphicsRectItem>
#include <QPainter>
#include <QPainterPathStroker>
#include <QStringList>
#include <QStyleOptionGraphicsItem>
#include <QtCore/QMetaObject>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/AppManager.h"
#include "Engine/GroupInput.h"
#include "Engine/Knob.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Settings.h"

#include "Gui/Edge.h"
#include "Gui/Gui.h"
#include "Gui/NodeGraph.h"
#include "Gui/NodeGui.h"
#include "Gui/ViewerTab.h"

NATRON_NAMESPACE_ENTER

namespace {
// The outlines sit above backdrops (10) and below the pipes (15) so the pipes stay readable through the fill; the markers
// sit above the pipes they mark and below the nodes (20).
const double kOutlineDepth = 14.;
const double kMarkerDepth = 17.;
const double kOutlinePadding = 12.;
const double kMarkerHalfSize = 6.;
const double kMarkerDistance = 26.;
const double kLabelGap = 4.;
// Fading paints the graph's background over the node rather than lowering the node's opacity, which would show the
// pipes running under it.
const double kFadeAmount = 0.6;
const int kScrimKey = 0;
const char* const kScrimName = "gpuFadeScrim";

// Bounds a walk through nested groups, in case a corrupt graph loops.
const int kMaxGroupDepth = 256;

const QColor kRegionColor(80, 200, 150);
const QColor kFallbackColor(240, 165, 40);
const QColor kExitColor(235, 60, 55);
const QColor kEntryColor(200, 200, 200);
const QColor kLabelBackground(20, 20, 20, 190);

NodeGroup*
groupOf(const NodeCollectionPtr& collection)
{
    return dynamic_cast<NodeGroup*>(collection.get());
}

bool
isUnder(NodeCollectionPtr collection,
        const NodeCollectionPtr& ancestor)
{
    for (int depth = 0; collection && (depth < kMaxGroupDepth); ++depth) {
        if (collection == ancestor) {
            return true;
        }
        NodeGroup* group = groupOf(collection);
        const NodePtr groupNode = group ? group->getNode() : NodePtr();
        collection = groupNode ? groupNode->getGroup() : NodeCollectionPtr();
    }

    return false;
}

// The input of the Group node that a GroupInput node of the group feeds from, or -1.
int
groupInputIndex(const NodePtr& node,
                const NodeCollectionPtr& collection)
{
    if (!node || !dynamic_cast<GroupInput*>(node->getEffectInstance().get()) || (node->getGroup() != collection)) {
        return -1;
    }
    NodeGroup* group = groupOf(collection);
    const NodePtr groupNode = group ? group->getNode() : NodePtr();
    if (!groupNode) {
        return -1;
    }
    for (int i = 0; i < groupNode->getNInputs(); ++i) {
        if (group->getGroupInputNode(i) == node) {
            return i;
        }
    }

    return -1;
}

NodeGuiPtr
guiOf(const NodePtr& node)
{
    return node ? std::dynamic_pointer_cast<NodeGui>(node->getNodeGui()) : NodeGuiPtr();
}

QRectF
sceneRectOf(const NodeGuiPtr& gui)
{
    return gui->mapToScene(gui->boundingRect()).boundingRect();
}

QGraphicsRectItem*
scrimOf(NodeGui* gui)
{
    for (QGraphicsItem* child : gui->childItems()) {
        if (child->data(kScrimKey).toString() == QString::fromUtf8(kScrimName)) {
            return static_cast<QGraphicsRectItem*>(child);
        }
    }

    return 0;
}

QFont
labelFont()
{
    QFont font;
    font.setPixelSize(11);

    return font;
}

QRectF
labelRect(const QString& text,
          const QPointF& anchor,
          bool above)
{
    const QFontMetricsF metrics(labelFont());
    const QSizeF size(metrics.horizontalAdvance(text) + 2 * kLabelGap, metrics.height() + kLabelGap);

    return above ? QRectF(QPointF(anchor.x(), anchor.y() - size.height()), size)
                 : QRectF(QPointF(anchor.x(), anchor.y() - size.height() / 2), size);
}

void
drawLabel(QPainter* painter,
          const QRectF& rect,
          const QString& text,
          const QColor& color)
{
    painter->setPen(Qt::NoPen);
    painter->setBrush(kLabelBackground);
    painter->drawRoundedRect(rect, 3., 3.);
    painter->setPen(color);
    painter->drawText(rect, Qt::AlignCenter, text);
}
} // anon namespace

//////////////////////////////// GpuRegionItem

GpuRegionItem::GpuRegionItem(NodeGraph* graph,
                             LayerEnum layer,
                             QGraphicsItem* parent)
    : QGraphicsObject(parent)
    , _graph(graph)
    , _layer(layer)
    , _outlines()
    , _markers()
    , _tags()
    , _bounds()
{
    setZValue(layer == eLayerOutlines ? kOutlineDepth : kMarkerDepth);
    setAcceptedMouseButtons(Qt::NoButton);
    setAcceptHoverEvents(false);
    setFlag(QGraphicsItem::ItemUsesExtendedStyleOption);
}

void
GpuRegionItem::setShapes(const std::vector<Outline>& outlines,
                         const std::vector<Marker>& markers,
                         const std::vector<Tag>& tags)
{
    // Panning moves the parent, so the shapes are kept in the item's own coordinates, which move with the nodes.
    QRectF bounds;

    _outlines = outlines;
    for (Outline& outline : _outlines) {
        outline.path = mapFromScene(outline.path);
        outline.bounds = outline.path.boundingRect().adjusted(-2., -2., 2., 2.);
        if (!outline.label.isEmpty()) {
            outline.bounds = outline.bounds.united(labelRect(outline.label, outline.bounds.topLeft(), true));
        }
        bounds = bounds.united(outline.bounds);
    }
    _markers = markers;
    for (Marker& marker : _markers) {
        marker.pipe = QLineF(mapFromScene(marker.pipe.p1()), mapFromScene(marker.pipe.p2()));
        marker.at = mapFromScene(marker.at);
        const double reach = kMarkerHalfSize + 2.;
        marker.bounds = QRectF(marker.at.x() - reach, marker.at.y() - reach, 2 * reach, 2 * reach);
        if (!marker.label.isEmpty()) {
            marker.bounds = marker.bounds.united(labelRect(marker.label, QPointF(marker.at.x() + reach + kLabelGap, marker.at.y()), false));
        }
        bounds = bounds.united(marker.bounds);
    }
    _tags = tags;
    for (Tag& tag : _tags) {
        tag.at = mapFromScene(tag.at);
        tag.bounds = labelRect(tag.text, tag.at, true);
        bounds = bounds.united(tag.bounds);
    }
    prepareGeometryChange();
    _bounds = bounds;
    update();
}

QRectF
GpuRegionItem::boundingRect() const
{
    return _bounds;
}

void
GpuRegionItem::paint(QPainter* painter,
                     const QStyleOptionGraphicsItem* option,
                     QWidget* /*widget*/)
{
    if (_graph->isDoingNavigatorRender()) {
        return;
    }
    const bool lowDetail = _graph->isLowDetail();
    const QRectF exposed = option ? option->exposedRect : _bounds;

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, _graph->isAntiAliasingEnabled());
    painter->setFont(labelFont());
    for (const Outline& outline : _outlines) {
        if (!outline.bounds.intersects(exposed)) {
            continue;
        }
        const QColor color = outline.fellBack ? kFallbackColor : kRegionColor;
        QColor fill = color;
        fill.setAlpha(outline.fellBack ? 30 : 24);
        QPen pen(color, outline.fellBack ? 2. : 1.5, outline.fellBack ? Qt::DashLine : Qt::SolidLine);
        // Cosmetic, so the outline stays visible when the graph is zoomed far out.
        pen.setCosmetic(true);
        painter->setPen(pen);
        painter->setBrush(fill);
        painter->drawPath(outline.path);
        if (!lowDetail && !outline.label.isEmpty()) {
            drawLabel(painter, labelRect(outline.label, outline.path.boundingRect().adjusted(-2., -2., 2., 2.).topLeft(), true),
                      outline.label, color);
        }
    }
    for (const Marker& marker : _markers) {
        if (!marker.bounds.intersects(exposed)) {
            continue;
        }
        if (marker.exit) {
            QPen pen(kExitColor, 2.5);
            pen.setCapStyle(Qt::RoundCap);
            painter->setPen(pen);
            const QPointF& c = marker.at;
            painter->drawLine(QPointF(c.x() - kMarkerHalfSize, c.y() - kMarkerHalfSize), QPointF(c.x() + kMarkerHalfSize, c.y() + kMarkerHalfSize));
            painter->drawLine(QPointF(c.x() - kMarkerHalfSize, c.y() + kMarkerHalfSize), QPointF(c.x() + kMarkerHalfSize, c.y() - kMarkerHalfSize));
            if (!lowDetail && !marker.label.isEmpty()) {
                const double reach = kMarkerHalfSize + 2.;
                drawLabel(painter, labelRect(marker.label, QPointF(c.x() + reach + kLabelGap, c.y()), false), marker.label, kExitColor.lighter(130));
            }
        } else {
            QLineF normal = marker.pipe.normalVector().unitVector();
            const QPointF offset = (normal.p2() - normal.p1()) * kMarkerHalfSize;
            QPen pen(kEntryColor, 2.);
            pen.setCapStyle(Qt::RoundCap);
            painter->setPen(pen);
            painter->drawLine(marker.at - offset, marker.at + offset);
        }
    }
    if (!lowDetail) {
        for (const Tag& tag : _tags) {
            if (tag.bounds.intersects(exposed)) {
                drawLabel(painter, tag.bounds, tag.text, kRegionColor.lighter(120));
            }
        }
    }
    painter->restore();
} // GpuRegionItem::paint

//////////////////////////////// GpuRegionOverlay

GpuRegionOverlay::GpuRegionOverlay(NodeGraph* graph,
                                   QGraphicsItem* nodeRoot)
    : QObject(graph)
    , _graph(graph)
    , _outlines(new GpuRegionItem(graph, GpuRegionItem::eLayerOutlines, nodeRoot))
    , _markers(new GpuRegionItem(graph, GpuRegionItem::eLayerMarkers, nodeRoot))
    , _watchedViewer()
    , _snapshot()
    , _regions()
    , _faded()
    , _pending(false)
    , _remapPending(true)
{
    _outlines->setObjectName(QString::fromUtf8("GpuRegionOutlineItem"));
    _markers->setObjectName(QString::fromUtf8("GpuRegionMarkerItem"));
    _outlines->hide();
    _markers->hide();

    QObject::connect(appPTR->getCurrentSettings().get(), SIGNAL(settingChanged(KnobI*)), this, SLOT(onSettingChanged(KnobI*)));
    Gui* gui = graph->getGui();
    if (gui) {
        QObject::connect(gui, SIGNAL(activeViewerChanged()), this, SLOT(onViewersChanged()));
        QObject::connect(gui, SIGNAL(viewersChanged()), this, SLOT(onViewersChanged()));
    }
    schedule(true);
}

GpuRegionOverlay::~GpuRegionOverlay()
{
    // The scene owns the items.
}

bool
GpuRegionOverlay::isEnabled() const
{
    return appPTR->getCurrentSettings()->isGpuRegionOverlayEnabled();
}

void
GpuRegionOverlay::schedule(bool topology)
{
    _remapPending = _remapPending || topology;
    if (_pending || !isEnabled()) {
        return;
    }
    _pending = true;
    QMetaObject::invokeMethod(this, "refresh", Qt::QueuedConnection);
}

void
GpuRegionOverlay::onSettingChanged(KnobI* knob)
{
    if (!knob || (knob->getName() != "showGpuRegions") || _pending) {
        return;
    }
    // Turning the overlay off must still reach refresh(), which schedule() skips while off.
    _pending = true;
    QMetaObject::invokeMethod(this, "refresh", Qt::QueuedConnection);
}

void
GpuRegionOverlay::onViewersChanged()
{
    schedule(false);
}

void
GpuRegionOverlay::onSnapshotChanged()
{
    schedule(false);
}

ViewerInstance*
GpuRegionOverlay::currentViewer() const
{
    Gui* gui = _graph->getGui();
    if (!gui) {
        return 0;
    }
    ViewerTab* tab = gui->getActiveViewer();
    if (!tab) {
        const std::list<ViewerTab*>& tabs = gui->getViewersList();
        tab = tabs.empty() ? 0 : tabs.front();
    }

    return tab ? tab->getInternalNode() : 0;
}

void
GpuRegionOverlay::watchViewer(ViewerInstance* viewer)
{
    QObject* object = viewer;
    if (_watchedViewer.data() == object) {
        return;
    }
    if (_watchedViewer) {
        QObject::disconnect(_watchedViewer.data(), SIGNAL(gpuPlacementSnapshotChanged()), this, SLOT(onSnapshotChanged()));
    }
    _watchedViewer = object;
    if (object) {
        // The viewer publishes from a render or planner thread.
        QObject::connect(object, SIGNAL(gpuPlacementSnapshotChanged()), this, SLOT(onSnapshotChanged()), Qt::QueuedConnection);
    }
}

NodePtr
GpuRegionOverlay::mapNodeToCollection(const NodePtr& node,
                                      const NodeCollectionPtr& collection)
{
    NodePtr current = node;

    for (int depth = 0; current && collection && (depth < kMaxGroupDepth); ++depth) {
        const NodeCollectionPtr owner = current->getGroup();
        if (owner == collection) {
            return current;
        }
        NodeGroup* group = groupOf(owner);
        current = group ? group->getNode() : NodePtr();
    }

    return NodePtr();
}

bool
GpuRegionOverlay::mapPipeToCollection(const NodePtr& dest,
                                      int inputNb,
                                      const NodeCollectionPtr& collection,
                                      NodePtr* mappedDest,
                                      int* mappedInputNb)
{
    NodePtr node = dest;
    int input = inputNb;

    for (int depth = 0; node && collection && (input >= 0) && (depth < kMaxGroupDepth); ++depth) {
        const NodeCollectionPtr owner = node->getGroup();
        if (owner == collection) {
            *mappedDest = node;
            *mappedInputNb = input;

            return true;
        }
        const NodePtr real = node->getRealInput(input);
        if (!real) {
            return false;
        }
        const int groupInput = groupInputIndex(real, owner);
        if (groupInput >= 0) {
            // The pipe enters node's group from outside: in the parent graph it is that input of the Group node.
            node = groupOf(owner)->getNode();
            input = groupInput;
            continue;
        }
        if (isUnder(owner, collection)) {
            // Both ends lie inside a group shown in collection as a single node.
            return false;
        }
        NodeGroup* sourceGroup = real->isEffectGroup();
        if (!sourceGroup) {
            return false;
        }
        // The pipe leaves a group: inside it, it is the input of that group's Output node.
        node = sourceGroup->getOutputNode(false);
        input = 0;
    }

    return false;
}

QString
GpuRegionOverlay::exitLabel(FrameGraph::PlacementReason reason,
                            const std::string& detail,
                            bool mixed)
{
    QString text;

    switch (reason) {
    case FrameGraph::PlacementReason::None:
        text = tr("region split");
        break;
    case FrameGraph::PlacementReason::InvalidGraph:
        text = tr("graph has a cycle");
        break;
    case FrameGraph::PlacementReason::NoGpuPath:
        text = tr("no GPU path");
        break;
    case FrameGraph::PlacementReason::DownscaleUnsupported:
        text = tr("no downscale");
        break;
    case FrameGraph::PlacementReason::Cached:
        text = tr("cached");
        break;
    case FrameGraph::PlacementReason::Identity:
        text = tr("identity");
        break;
    case FrameGraph::PlacementReason::RawRunAllOrNothing:
        text = tr("raw run on the CPU");
        break;
    case FrameGraph::PlacementReason::WouldFormCycle:
        text = tr("would form a cycle");
        break;
    case FrameGraph::PlacementReason::CostModel:
        text = tr("cost model");
        break;
    }
    if (!detail.empty()) {
        text += QString::fromUtf8(": ") + QString::fromUtf8(detail.c_str());
    }
    if (mixed) {
        text += tr(" (mixed)");
    }

    return text;
} // GpuRegionOverlay::exitLabel

QString
GpuRegionOverlay::fallbackLabel(const GpuPlacementSnapshot::Region& region)
{
    if (!region.fellBack) {
        return QString();
    }
    QString text = tr("CPU fallback");
    if (!region.fallbackReason.empty()) {
        text += QString::fromUtf8(": ") + QString::fromUtf8(region.fallbackReason.c_str());
    }
    if (region.deviceLost) {
        text += tr(", device lost");
    }
    if (region.retries > 0) {
        text += tr(", %1 retries").arg(region.retries);
    }
    if (region.fallbackMixed) {
        text += tr(" (mixed)");
    }

    return text;
}

void
GpuRegionOverlay::remap()
{
    _regions.clear();
    _runs.clear();
    const NodeCollectionPtr collection = _graph->getGroup();
    if (!_snapshot || !collection) {
        return;
    }
    std::map<NodeGui*, std::size_t> runIndex;
    for (const GpuPlacementSnapshot::NodeRun& run : _snapshot->nodeRuns) {
        const NodeGuiPtr gui = guiOf(mapNodeToCollection(run.node.lock(), collection));
        if (!gui) {
            continue;
        }
        std::map<NodeGui*, std::size_t>::iterator found = runIndex.find(gui.get());
        if (found == runIndex.end()) {
            runIndex[gui.get()] = _runs.size();
            MappedRun entry;
            entry.gui = gui;
            entry.ran = run.ran;
            _runs.push_back(entry);
        } else {
            // A Group node stays coloured when anything it stands for ran.
            _runs[found->second].ran = _runs[found->second].ran || run.ran;
        }
    }
    for (const GpuPlacementSnapshot::Region& region : _snapshot->regions) {
        MappedRegion mapped;
        std::map<NodeGui*, std::size_t> memberIndex;
        for (const GpuPlacementSnapshot::Member& member : region.members) {
            const NodeGuiPtr gui = guiOf(mapNodeToCollection(member.node.lock(), collection));
            if (!gui) {
                continue;
            }
            std::map<NodeGui*, std::size_t>::iterator found = memberIndex.find(gui.get());
            if (found == memberIndex.end()) {
                memberIndex[gui.get()] = mapped.members.size();
                MappedMember entry;
                entry.gui = gui;
                entry.mixed = member.mixed;
                mapped.members.push_back(entry);
            } else {
                MappedMember& entry = mapped.members[found->second];
                entry.mixed = entry.mixed || member.mixed;
            }
        }
        std::set<std::pair<NodeGui*, int>> seenExits;
        for (const GpuPlacementSnapshot::Exit& exit : region.exits) {
            NodePtr dest;
            int input = -1;
            if (!mapPipeToCollection(exit.pipe.destNode.lock(), exit.pipe.inputNb, collection, &dest, &input)) {
                continue;
            }
            const NodeGuiPtr gui = guiOf(dest);
            if (!gui || !seenExits.insert(std::make_pair(gui.get(), input)).second) {
                continue;
            }
            MappedPipe pipe;
            pipe.dest = gui;
            pipe.inputNb = input;
            pipe.label = exitLabel(exit.reason, exit.detail, exit.mixed);
            mapped.exits.push_back(pipe);
        }
        std::set<std::pair<NodeGui*, int>> seenEntries;
        for (const GpuPlacementSnapshot::Pipe& entry : region.entries) {
            NodePtr dest;
            int input = -1;
            if (!mapPipeToCollection(entry.destNode.lock(), entry.inputNb, collection, &dest, &input)) {
                continue;
            }
            const NodeGuiPtr gui = guiOf(dest);
            if (!gui || !seenEntries.insert(std::make_pair(gui.get(), input)).second) {
                continue;
            }
            MappedPipe pipe;
            pipe.dest = gui;
            pipe.inputNb = input;
            mapped.entries.push_back(pipe);
        }
        if (mapped.members.empty() && mapped.exits.empty() && mapped.entries.empty()) {
            continue;
        }
        mapped.fellBack = region.fellBack;
        mapped.fallbackLabel = fallbackLabel(region);
        _regions.push_back(mapped);
    }
} // GpuRegionOverlay::remap

void
GpuRegionOverlay::setFaded(const std::vector<NodeGuiWPtr>& faded)
{
    std::set<NodeGui*> keep;
    for (const NodeGuiWPtr& weak : faded) {
        const NodeGuiPtr gui = weak.lock();
        if (!gui) {
            continue;
        }
        keep.insert(gui.get());
        QGraphicsRectItem* scrim = scrimOf(gui.get());
        if (!scrim) {
            scrim = new QGraphicsRectItem(gui.get());
            scrim->setData(kScrimKey, QString::fromUtf8(kScrimName));
            scrim->setPen(Qt::NoPen);
            scrim->setAcceptedMouseButtons(Qt::NoButton);
            scrim->setAcceptHoverEvents(false);
        }
        qreal top = 0.;
        for (QGraphicsItem* child : gui->childItems()) {
            if (child != scrim) {
                top = std::max(top, child->zValue());
            }
        }
        scrim->setZValue(top + 1.);
        scrim->setRect(gui->boundingRect());
        QColor shade = _graph->viewport()->palette().color(_graph->viewport()->backgroundRole());
        shade.setAlphaF(kFadeAmount);
        scrim->setBrush(shade);
    }
    for (const NodeGuiWPtr& weak : _faded) {
        const NodeGuiPtr gui = weak.lock();
        if (gui && !keep.count(gui.get())) {
            delete scrimOf(gui.get());
        }
    }
    _faded = faded;
}

void
GpuRegionOverlay::clear()
{
    watchViewer(0);
    _snapshot.reset();
    _regions.clear();
    _runs.clear();
    _remapPending = true;
    setFaded(std::vector<NodeGuiWPtr>());
    _outlines->setShapes(std::vector<GpuRegionItem::Outline>(), std::vector<GpuRegionItem::Marker>(), std::vector<GpuRegionItem::Tag>());
    _markers->setShapes(std::vector<GpuRegionItem::Outline>(), std::vector<GpuRegionItem::Marker>(), std::vector<GpuRegionItem::Tag>());
    _outlines->hide();
    _markers->hide();
}

void
GpuRegionOverlay::refresh()
{
    _pending = false;
    if (!_outlines || !_markers) {
        return;
    }

    int regionCount = 0;
    int exitCount = 0;
    int entryCount = 0;
    int fallbackCount = 0;
    int mixedCount = 0;
    QStringList exitLabels;
    QStringList fadedNames;
    std::vector<NodeGuiWPtr> faded;

    if (!isEnabled()) {
        clear();
    } else {
        // Connect before reading, so a snapshot published in between still schedules another refresh.
        ViewerInstance* viewer = currentViewer();
        watchViewer(viewer);
        const GpuPlacementSnapshotPtr snapshot = viewer ? viewer->getGpuPlacementSnapshot() : GpuPlacementSnapshotPtr();
        if ((snapshot != _snapshot) || _remapPending) {
            _snapshot = snapshot;
            _remapPending = false;
            remap();
        }

        for (const MappedRun& run : _runs) {
            const NodeGuiPtr gui = run.gui.lock();
            const NodePtr node = gui ? gui->getNode() : NodePtr();
            if (node && !run.ran) {
                faded.push_back(gui);
                fadedNames.push_back(QString::fromUtf8(node->getScriptName_mt_safe().c_str()));
            }
        }

        std::vector<GpuRegionItem::Outline> outlines;
        std::vector<GpuRegionItem::Marker> markers;
        std::vector<GpuRegionItem::Tag> tags;
        QPainterPathStroker stroker;
        stroker.setWidth(2 * kOutlinePadding);
        stroker.setCapStyle(Qt::RoundCap);
        stroker.setJoinStyle(Qt::RoundJoin);

        for (const MappedRegion& region : _regions) {
            std::set<NodeGui*> members;
            QPainterPath path;
            path.setFillRule(Qt::WindingFill);
            for (const MappedMember& member : region.members) {
                const NodeGuiPtr gui = member.gui.lock();
                if (!gui || !gui->isVisible()) {
                    continue;
                }
                members.insert(gui.get());
                const QRectF rect = sceneRectOf(gui);
                path.addRoundedRect(rect.adjusted(-kOutlinePadding, -kOutlinePadding, kOutlinePadding, kOutlinePadding),
                                    kOutlinePadding, kOutlinePadding);
                if (member.mixed) {
                    GpuRegionItem::Tag tag;
                    tag.at = rect.topRight() + QPointF(-kOutlinePadding, -kLabelGap);
                    tag.text = tr("mixed");
                    tags.push_back(tag);
                    ++mixedCount;
                }
            }
            if (!members.empty()) {
                for (NodeGui* gui : members) {
                    for (Edge* edge : gui->getInputsArrows()) {
                        if (!edge || !edge->isVisible() || !edge->hasSource() || !members.count(edge->getSource().get())) {
                            continue;
                        }
                        QPainterPath pipe;
                        pipe.moveTo(edge->mapToScene(edge->line().p1()));
                        pipe.lineTo(edge->mapToScene(edge->line().p2()));
                        path.addPath(stroker.createStroke(pipe));
                    }
                }
                GpuRegionItem::Outline outline;
                outline.path = path.simplified();
                outline.fellBack = region.fellBack;
                outline.label = region.fallbackLabel;
                outlines.push_back(outline);
                ++regionCount;
                if (region.fellBack) {
                    ++fallbackCount;
                }
            }

            for (int kind = 0; kind < 2; ++kind) {
                const bool exit = (kind == 0);
                for (const MappedPipe& pipe : exit ? region.exits : region.entries) {
                    const NodeGuiPtr gui = pipe.dest.lock();
                    Edge* edge = gui && gui->isVisible() ? gui->getInputArrow(pipe.inputNb) : 0;
                    if (!edge || !edge->isVisible() || !edge->hasSource()) {
                        continue;
                    }
                    // p1 is the end at the destination node.
                    const QLineF line(edge->mapToScene(edge->line().p1()), edge->mapToScene(edge->line().p2()));
                    const double length = line.length();
                    if (length < 1.) {
                        continue;
                    }
                    GpuRegionItem::Marker marker;
                    marker.pipe = line;
                    marker.at = line.pointAt(std::min(kMarkerDistance, length * 0.4) / length);
                    marker.exit = exit;
                    marker.label = pipe.label;
                    markers.push_back(marker);
                    if (exit) {
                        ++exitCount;
                        exitLabels.push_back(pipe.label);
                    } else {
                        ++entryCount;
                    }
                }
            }
        }
        _outlines->setShapes(outlines, std::vector<GpuRegionItem::Marker>(), std::vector<GpuRegionItem::Tag>());
        _markers->setShapes(std::vector<GpuRegionItem::Outline>(), markers, tags);
        _outlines->show();
        _markers->show();
        setFaded(faded);
    }

    _outlines->setProperty("regionCount", regionCount);
    _outlines->setProperty("exitCount", exitCount);
    _outlines->setProperty("entryCount", entryCount);
    _outlines->setProperty("fallbackCount", fallbackCount);
    _outlines->setProperty("fadedCount", (int)_faded.size());
    _outlines->setProperty("fadedNodes", fadedNames);
    _outlines->setProperty("mixedCount", mixedCount);
    _outlines->setProperty("exitLabels", exitLabels);
} // GpuRegionOverlay::refresh

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_GpuOverlay.cpp"
