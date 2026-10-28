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

#include "NodeGraphTextItem.h"

#include <cmath>
#include <stdexcept>

#include <QDebug>
#include <QFontMetrics>
#include <QPainter>
#include <QPolygonF>
#include <QStyleOption>

#include "Engine/Settings.h"

#include "Gui/NodeGraph.h"
#include "Gui/GuiApplicationManager.h"

#define NODEGRAPH_TEXT_ITEM_MIN_HEIGHT_PX 4
#define NODEGRAPH_SIMPLE_TEXT_ITEM_MIN_HEIGHT_PX 6
#define NODEGRAPH_PIXMAP_ITEM_MIN_HEIGHT_PX 10

NATRON_NAMESPACE_ENTER

// The height in device pixels of a line of text, measured with the painter's transform, which is
// the item-to-viewport transform when the view paints. Measuring the font is cached because the
// font rarely changes and QFontMetrics is costly to build on every paint of every item.
static int
paintedTextHeight(const QPainter* painter,
                  const QFont& font,
                  NodeGraphFontHeightCache* cache)
{
    if (!cache->valid || (cache->font != font)) {
        cache->font = font;
        cache->height = QFontMetrics(font).height();
        cache->valid = true;
    }
    const QTransform& t = painter->worldTransform();
    QPoint topLeft = t.map(QPointF(0, 0)).toPoint();
    QPoint btmLeft = t.map(QPointF(0, cache->height)).toPoint();

    return std::abs(btmLeft.y() - topLeft.y());
}

NodeGraphTextItem::NodeGraphTextItem(NodeGraph* graph,
                                     QGraphicsItem* parent,
                                     bool alwaysDrawText)
    : QGraphicsTextItem(parent)
    , _graph(graph)
    , _alwaysDrawText(alwaysDrawText)
{
    QFont f = font();
    bool antialias = appPTR->getCurrentSettings()->isNodeGraphAntiAliasingEnabled();

    if (!antialias) {
        f.setStyleStrategy(QFont::NoAntialias);
    }
    setFont(f);
}

NodeGraphTextItem::~NodeGraphTextItem()
{
}

void
NodeGraphTextItem::paint(QPainter *painter,
                         const QStyleOptionGraphicsItem *option,
                         QWidget *widget)
{
    bool isTooSmall = false;

    if (_graph->isDoingNavigatorRender()) {
        // The navigator thumbnail is too small for any text to be legible.
        isTooSmall = true;
    } else if (!_alwaysDrawText) {
        isTooSmall = paintedTextHeight(painter, font(), &_fontHeightCache) < NODEGRAPH_TEXT_ITEM_MIN_HEIGHT_PX;
    }
    if (isTooSmall) {
        return;
    }
    QGraphicsTextItem::paint(painter, option, widget);
}

NodeGraphSimpleTextItem::NodeGraphSimpleTextItem(NodeGraph* graph,
                                                 QGraphicsItem* parent,
                                                 bool alwaysDrawText)
    : QGraphicsSimpleTextItem(parent)
    , _graph(graph)
    , _alwaysDrawText(alwaysDrawText)
    , _hiddenAtLowDetail(false)
{
    QFont f = font();
    bool antialias = appPTR->getCurrentSettings()->isNodeGraphAntiAliasingEnabled();

    if (!antialias) {
        f.setStyleStrategy(QFont::NoAntialias);
    }
    setFont(f);
}

NodeGraphSimpleTextItem::~NodeGraphSimpleTextItem()
{
}

void
NodeGraphSimpleTextItem::paint(QPainter *painter,
                               const QStyleOptionGraphicsItem *option,
                               QWidget *widget)
{
    bool isTooSmall = false;

    if (_graph->isDoingNavigatorRender()) {
        // The navigator thumbnail is too small for any text to be legible.
        isTooSmall = true;
    } else if (_hiddenAtLowDetail && _graph->isLowDetail()) {
        isTooSmall = true;
    } else if (!_alwaysDrawText) {
        isTooSmall = paintedTextHeight(painter, font(), &_fontHeightCache) < NODEGRAPH_SIMPLE_TEXT_ITEM_MIN_HEIGHT_PX;
    }
    if (isTooSmall) {
        return;
    }
    QGraphicsSimpleTextItem::paint(painter, option, widget);
}

NodeGraphPixmapItem::NodeGraphPixmapItem(NodeGraph* graph,
                                         QGraphicsItem* parent)
    : QGraphicsPixmapItem(parent)
    , _graph(graph)
    , _hiddenAtLowDetail(false)
{
}

NodeGraphPixmapItem::~NodeGraphPixmapItem()
{
}

void
NodeGraphPixmapItem::paint(QPainter *painter,
                           const QStyleOptionGraphicsItem *option,
                           QWidget *widget)
{
    if (_graph->isDoingNavigatorRender() || (_hiddenAtLowDetail && _graph->isLowDetail())) {
        return;
    }
    int height = painter->worldTransform().map(QPolygonF(boundingRect())).toPolygon().boundingRect().height();
    if (height < NODEGRAPH_PIXMAP_ITEM_MIN_HEIGHT_PX) {
        return;
    }
    QGraphicsPixmapItem::paint(painter, option, widget);
}

NATRON_NAMESPACE_EXIT
