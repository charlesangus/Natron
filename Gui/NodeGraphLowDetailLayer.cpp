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

#include "NodeGraphLowDetailLayer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QBrush>
#include <QGraphicsEllipseItem>
#include <QGraphicsPixmapItem>
#include <QGraphicsPolygonItem>
#include <QGraphicsRectItem>
#include <QLineF>
#include <QPainter>
#include <QPen>
#include <QPolygon>
#include <QPolygonF>
#include <QStyleOptionGraphicsItem>
#include <QTransform>
#include <QVector>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Gui/BackdropGui.h"
#include "Gui/Edge.h"
#include "Gui/NodeGraphPrivate.h"
#include "Gui/NodeGraphRectItem.h"
#include "Gui/NodeGraphTextItem.h"
#include "Gui/NodeGui.h"

NATRON_NAMESPACE_ENTER

namespace {

QTransform
transformToParent(const QGraphicsItem* item)
{
    if (item->transform().isIdentity() && (item->rotation() == 0.) && (item->scale() == 1.)) {
        return QTransform::fromTranslate(item->x(), item->y());
    }

    return item->itemTransform(item->parentItem());
}

std::uint64_t
mixHash(std::uint64_t h,
        std::uint64_t v)
{
    return (h ^ v) * 1099511628211ull;
}

std::uint64_t
styleHash(const QPen& pen,
          const QBrush& brush,
          qreal opacity)
{
    std::uint64_t h = 14695981039346656037ull;
    h = mixHash(h, pen.color().rgba());
    h = mixHash(h, (std::uint64_t)pen.style());
    h = mixHash(h, std::hash<qreal>()(pen.widthF()));
    h = mixHash(h, brush.color().rgba());
    h = mixHash(h, (std::uint64_t)brush.style());
    h = mixHash(h, std::hash<qreal>()(opacity));

    return h;
}

struct RoundedRect {
    QRectF rect;
    qreal radius = 0.;
};

struct Pie {
    QRectF rect;
    int startAngle = 0;
    int spanAngle = 0;
};

// Everything a bucket draws with one pen, brush and opacity.
struct ShapeBatch {
    QPen pen;
    QBrush brush;
    qreal opacity = 1.;
    QVector<QRectF> rects;
    std::vector<RoundedRect> roundedRects;
    std::vector<QRectF> ellipses;
    std::vector<Pie> pies;
    std::vector<std::pair<QPolygonF, Qt::FillRule>> polygons;
};

struct PixmapDraw {
    const QGraphicsPixmapItem* item = nullptr;
    QTransform toRoot;
    qreal opacity = 1.;
};

// A child the batches cannot reproduce, painted by its own paint() as the scene would.
struct ItemDraw {
    QGraphicsItem* item = nullptr;
    QTransform toRoot;
    QRectF bounds;
    qreal opacity = 1.;
};

// What every node draws at one place in its children's stacking order.
struct Bucket {
    std::vector<ShapeBatch> batches;
    std::unordered_multimap<std::uint64_t, std::size_t> batchIndex;
    std::vector<PixmapDraw> pixmaps;
    std::vector<ItemDraw> itemDraws;

    ShapeBatch& batchFor(const QPen& pen,
                         const QBrush& brush,
                         qreal opacity)
    {
        const std::uint64_t h = styleHash(pen, brush, opacity);
        typedef std::unordered_multimap<std::uint64_t, std::size_t>::const_iterator It;
        std::pair<It, It> range = batchIndex.equal_range(h);
        for (It it = range.first; it != range.second; ++it) {
            ShapeBatch& batch = batches[it->second];
            if ((batch.opacity == opacity) && (batch.pen == pen) && (batch.brush == brush)) {
                return batch;
            }
        }
        batchIndex.emplace(h, batches.size());
        batches.push_back(ShapeBatch());
        ShapeBatch& batch = batches.back();
        batch.pen = pen;
        batch.brush = brush;
        batch.opacity = opacity;

        return batch;
    }
};

// Keyed by the z value and the index of the node's child the shapes come from.
typedef std::map<std::pair<qreal, int>, Bucket> BucketMap;

// A node scaled up, by magnification for instance, strokes its children with scaled pens.
QPen
penInRoot(const QPen& pen,
          const QTransform& toRoot)
{
    if ((toRoot.type() <= QTransform::TxTranslate) || pen.isCosmetic() || (pen.style() == Qt::NoPen)) {
        return pen;
    }
    QPen scaled(pen);
    scaled.setWidthF(pen.widthF() * std::sqrt(std::abs(toRoot.determinant())));

    return scaled;
}

void
addRect(Bucket* bucket,
        const QPen& pen,
        const QBrush& brush,
        qreal opacity,
        const QRectF& rect,
        qreal radius,
        const QTransform& toRoot)
{
    if (toRoot.type() > QTransform::TxScale) {
        bucket->batchFor(penInRoot(pen, toRoot), brush, opacity).polygons.push_back(std::make_pair(toRoot.map(QPolygonF(rect)), Qt::OddEvenFill));

        return;
    }
    const QRectF rootRect = toRoot.mapRect(rect);
    ShapeBatch& batch = bucket->batchFor(penInRoot(pen, toRoot), brush, opacity);
    if (radius > 0.) {
        RoundedRect rounded;
        rounded.rect = rootRect;
        rounded.radius = radius;
        batch.roundedRects.push_back(rounded);
    } else {
        batch.rects.push_back(rootRect);
    }
}

void
collectNodeChild(QGraphicsItem* item,
                 const QTransform& toRoot,
                 qreal opacity,
                 bool navigatorRender,
                 const QRectF& exposed,
                 Bucket* bucket)
{
    switch (item->type()) {
    case QGraphicsRectItem::Type: {
        const QGraphicsRectItem* rectItem = static_cast<const QGraphicsRectItem*>(item);
        const NodeGraphRectItem* nodeRect = dynamic_cast<const NodeGraphRectItem*>(rectItem);
        const qreal radius = nodeRect ? nodeRect->getCornerRadiusPx() : 0;
        addRect(bucket, rectItem->pen(), rectItem->brush(), opacity, rectItem->rect(), radius, toRoot);
        QColor borderColor;
        double borderWidth = 0.;
        if (nodeRect && nodeRect->getInsetBorder(&borderColor, &borderWidth)) {
            const double half = borderWidth / 2.;
            const QRectF strokeRect = rectItem->rect().adjusted(half, half, -half, -half);
            if ((strokeRect.width() > 0.) && (strokeRect.height() > 0.)) {
                QPen borderPen(borderColor, borderWidth);
                borderPen.setJoinStyle(Qt::MiterJoin);
                addRect(bucket, borderPen, Qt::NoBrush, opacity, strokeRect, std::max(0., radius - half), toRoot);
            }
        }
        break;
    }
    case QGraphicsEllipseItem::Type: {
        const QGraphicsEllipseItem* ellipse = static_cast<const QGraphicsEllipseItem*>(item);
        ShapeBatch& batch = bucket->batchFor(penInRoot(ellipse->pen(), toRoot), ellipse->brush(), opacity);
        const QRectF rootRect = toRoot.mapRect(ellipse->rect());
        if ((ellipse->spanAngle() != 0) && (std::abs(ellipse->spanAngle()) % (360 * 16) == 0)) {
            batch.ellipses.push_back(rootRect);
        } else {
            Pie pie;
            pie.rect = rootRect;
            pie.startAngle = ellipse->startAngle();
            pie.spanAngle = ellipse->spanAngle();
            batch.pies.push_back(pie);
        }
        break;
    }
    case QGraphicsPolygonItem::Type: {
        const QGraphicsPolygonItem* polygon = static_cast<const QGraphicsPolygonItem*>(item);
        bucket->batchFor(penInRoot(polygon->pen(), toRoot), polygon->brush(), opacity).polygons.push_back(std::make_pair(toRoot.map(polygon->polygon()), polygon->fillRule()));
        break;
    }
    case QGraphicsPixmapItem::Type: {
        const NodeGraphPixmapItem* nodePixmap = dynamic_cast<const NodeGraphPixmapItem*>(item);
        if (navigatorRender || (nodePixmap && nodePixmap->isHiddenAtLowDetail())) {
            return;
        }
        PixmapDraw draw;
        draw.item = static_cast<const QGraphicsPixmapItem*>(item);
        draw.toRoot = toRoot;
        draw.opacity = opacity;
        bucket->pixmaps.push_back(draw);
        break;
    }
    default: {
        // Labels, badges and the disabled cross each decide in paint() what they show at this
        // level and in the navigator, so they keep exactly the look they had when the scene drew
        // them. The same culling and opacity test as the scene's.
        if ((item->flags() & QGraphicsItem::ItemHasNoContents) || (opacity < 0.001)) {
            break;
        }
        ItemDraw draw;
        draw.item = item;
        draw.toRoot = toRoot;
        draw.bounds = item->boundingRect();
        draw.opacity = opacity;
        if (toRoot.mapRect(draw.bounds).intersects(exposed)) {
            bucket->itemDraws.push_back(draw);
        }
        break;
    }
    }
    const QList<QGraphicsItem*> children = item->childItems();
    for (QList<QGraphicsItem*>::const_iterator it = children.begin(); it != children.end(); ++it) {
        if ((*it)->isVisible()) {
            collectNodeChild(*it, transformToParent(*it) * toRoot, opacity * (*it)->opacity(), navigatorRender, exposed, bucket);
        }
    }
} // collectNodeChild

void
drawItems(QPainter* painter,
          const Bucket& bucket,
          QPainter::RenderHints viewHints,
          QWidget* widget)
{
    if (bucket.itemDraws.empty()) {
        return;
    }
    painter->save();
    // The hints the view gives every item, not the plain ones the batches draw with.
    painter->setRenderHints(~viewHints, false);
    painter->setRenderHints(viewHints, true);
    const QTransform base = painter->worldTransform();
    for (std::vector<ItemDraw>::const_iterator it = bucket.itemDraws.begin(); it != bucket.itemDraws.end(); ++it) {
        QGraphicsItem* item = it->item;
        QStyleOptionGraphicsItem option;
        option.state = QStyle::State_None;
        option.rect = it->bounds.toRect();
        option.exposedRect = it->bounds;
        if (item->isSelected()) {
            option.state |= QStyle::State_Selected;
        }
        if (item->isEnabled()) {
            option.state |= QStyle::State_Enabled;
        }
        if (item->hasFocus()) {
            option.state |= QStyle::State_HasFocus;
        }
        painter->setWorldTransform(it->toRoot * base);
        painter->setOpacity(it->opacity);
        item->paint(painter, &option, widget);
    }
    painter->restore();
}

void
drawBucket(QPainter* painter,
           const Bucket& bucket,
           QPainter::RenderHints viewHints,
           QWidget* widget)
{
    for (std::vector<ShapeBatch>::const_iterator it = bucket.batches.begin(); it != bucket.batches.end(); ++it) {
        painter->setOpacity(it->opacity);
        painter->setPen(it->pen);
        painter->setBrush(it->brush);
        if (!it->rects.isEmpty()) {
            painter->drawRects(it->rects);
        }
        for (std::vector<RoundedRect>::const_iterator r = it->roundedRects.begin(); r != it->roundedRects.end(); ++r) {
            painter->drawRoundedRect(r->rect, r->radius, r->radius);
        }
        for (std::vector<QRectF>::const_iterator e = it->ellipses.begin(); e != it->ellipses.end(); ++e) {
            painter->drawEllipse(*e);
        }
        for (std::vector<Pie>::const_iterator p = it->pies.begin(); p != it->pies.end(); ++p) {
            painter->drawPie(p->rect, p->startAngle, p->spanAngle);
        }
        for (std::vector<std::pair<QPolygonF, Qt::FillRule>>::const_iterator p = it->polygons.begin(); p != it->polygons.end(); ++p) {
            painter->drawPolygon(p->first, p->second);
        }
    }
    if (bucket.pixmaps.empty()) {
        drawItems(painter, bucket, viewHints, widget);

        return;
    }
    const QTransform base = painter->worldTransform();
    for (std::vector<PixmapDraw>::const_iterator it = bucket.pixmaps.begin(); it != bucket.pixmaps.end(); ++it) {
        painter->setWorldTransform(it->toRoot * base);
        // The same visibility threshold NodeGraphPixmapItem::paint() applies.
        const int height = painter->worldTransform().map(QPolygonF(it->item->boundingRect())).toPolygon().boundingRect().height();
        if (height < NodeGraphPixmapItem::minimumPaintedHeightPx()) {
            continue;
        }
        painter->setOpacity(it->opacity);
        painter->setRenderHint(QPainter::SmoothPixmapTransform, it->item->transformationMode() == Qt::SmoothTransformation);
        painter->drawPixmap(it->item->offset(), it->item->pixmap());
    }
    painter->setWorldTransform(base);
    drawItems(painter, bucket, viewHints, widget);
} // drawBucket

} // anon namespace

NodeGraphLowDetailLayer::NodeGraphLowDetailLayer(NodeGraphPrivate* graph,
                                                 PassEnum pass,
                                                 QGraphicsItem* parent)
    : QGraphicsItem(parent)
    , _graph(graph)
    , _pass(pass)
{
    setFlag(QGraphicsItem::ItemUsesExtendedStyleOption);
    setAcceptedMouseButtons(Qt::NoButton);
    setZValue(pass == ePassEdges ? 15 : 20);
    setVisible(false);
}

NodeGraphLowDetailLayer::~NodeGraphLowDetailLayer()
{
}

QRectF
NodeGraphLowDetailLayer::boundingRect() const
{
    return QRectF(NATRON_SCENE_MIN, NATRON_SCENE_MIN, NATRON_SCENE_MAX - NATRON_SCENE_MIN, NATRON_SCENE_MAX - NATRON_SCENE_MIN);
}

void
NodeGraphLowDetailLayer::applyDetailLevel(QGraphicsItem* item,
                                          bool isNode,
                                          bool lowDetail)
{
    if (!item) {
        return;
    }
    const bool faded = lowDetail && !(isNode && dynamic_cast<BackdropGui*>(item));
    const qreal opacity = faded ? 0. : 1.;
    if (item->opacity() != opacity) {
        item->setOpacity(opacity);
    }
    if (isNode) {
        const QGraphicsItem::CacheMode mode = faded ? kNodeCacheModeLowDetail : kNodeCacheModeFullDetail;
        if (item->cacheMode() != mode) {
            item->setCacheMode(mode);
        }
    }
}

void
NodeGraphLowDetailLayer::paint(QPainter* painter,
                               const QStyleOptionGraphicsItem* option,
                               QWidget* widget)
{
    if (!_graph->lowDetail) {
        return;
    }
    const QPainter::RenderHints viewHints = painter->renderHints();
    painter->setRenderHint(QPainter::Antialiasing, false);
    if (_pass == ePassEdges) {
        paintEdges(painter, option->exposedRect);
    } else {
        paintNodes(painter, option->exposedRect, viewHints, widget);
    }
    painter->setOpacity(1.);
}

void
NodeGraphLowDetailLayer::paintEdges(QPainter* painter,
                                    const QRectF& exposed)
{
    // As Edge::paint() does, so that the thumbnail looks the same at every level.
    if (_graph->isDoingPreviewRender) {
        return;
    }
    std::vector<NodeGraphPrivate::GridItem> found;
    _graph->getGridItems(exposed, &found);

    std::vector<std::pair<QPen, QVector<QLineF>>> groups;
    std::unordered_multimap<std::uint64_t, std::size_t> groupIndex;
    for (std::vector<NodeGraphPrivate::GridItem>::const_iterator it = found.begin(); it != found.end(); ++it) {
        if (it->slot == NodeGraphPrivate::kGridNodeSlot) {
            continue;
        }
        const Edge* edge = static_cast<const Edge*>(it->item);
        if (!edge->isVisible() || !isFaded(edge)) {
            continue;
        }
        const QTransform toRoot = transformToParent(edge);
        const QLineF line = (toRoot.type() == QTransform::TxNone) ? edge->line() : toRoot.map(edge->line());
        const QPen pen = penInRoot(edge->getDrawnPen(), toRoot);
        const std::uint64_t h = styleHash(pen, QBrush(), 1.);
        std::size_t index = groups.size();
        typedef std::unordered_multimap<std::uint64_t, std::size_t>::const_iterator It;
        std::pair<It, It> range = groupIndex.equal_range(h);
        for (It g = range.first; g != range.second; ++g) {
            if (groups[g->second].first == pen) {
                index = g->second;
                break;
            }
        }
        if (index == groups.size()) {
            groupIndex.emplace(h, index);
            groups.push_back(std::make_pair(pen, QVector<QLineF>()));
        }
        groups[index].second.push_back(line);
    }
    for (std::vector<std::pair<QPen, QVector<QLineF>>>::const_iterator it = groups.begin(); it != groups.end(); ++it) {
        painter->setPen(it->first);
        painter->drawLines(it->second);
    }
} // NodeGraphLowDetailLayer::paintEdges

void
NodeGraphLowDetailLayer::paintNodes(QPainter* painter,
                                    const QRectF& exposed,
                                    QPainter::RenderHints viewHints,
                                    QWidget* widget)
{
    std::vector<NodeGraphPrivate::GridItem> found;
    _graph->getGridItems(exposed, &found);

    const bool navigatorRender = _graph->isDoingPreviewRender;
    BucketMap buckets;
    for (std::vector<NodeGraphPrivate::GridItem>::const_iterator it = found.begin(); it != found.end(); ++it) {
        if (it->slot != NodeGraphPrivate::kGridNodeSlot) {
            continue;
        }
        const QGraphicsItem* node = it->item;
        if (!node->isVisible() || !isFaded(node)) {
            continue;
        }
        const QTransform nodeToRoot = transformToParent(node);
        // Sorted by stacking order, so the index breaks ties between children of equal z as the
        // scene does.
        const QList<QGraphicsItem*> children = node->childItems();
        for (int i = 0; i < children.size(); ++i) {
            QGraphicsItem* child = children[i];
            if (!child->isVisible()) {
                continue;
            }
            Bucket& bucket = buckets[std::make_pair(child->zValue(), i)];
            collectNodeChild(child, transformToParent(child) * nodeToRoot, child->opacity(), navigatorRender, exposed, &bucket);
        }
    }
    for (BucketMap::const_iterator it = buckets.begin(); it != buckets.end(); ++it) {
        drawBucket(painter, it->second, viewHints, widget);
    }
}

NATRON_NAMESPACE_EXIT
