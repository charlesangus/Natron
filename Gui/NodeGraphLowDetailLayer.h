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

#ifndef Gui_NodeGraphLowDetailLayer_h
#define Gui_NodeGraphLowDetailLayer_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QGraphicsItem>
#include <QRectF>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Gui/GuiFwd.h"

NATRON_NAMESPACE_ENTER

class NodeGraphPrivate;

// While the graph is at its low level of detail, nodes and their edges are faded to opacity 0,
// which makes the scene skip their whole subtrees, and these layers draw what is left of them
// (bodies, frames, state indicators, previews and plain lines) in a few batched calls, culled
// with the graph's item grid. Hit-testing still goes to the faded items themselves.
//
// The cache mode a node uses at each level. A node's own paint() draws nothing, so its
// DeviceCoordinateCache only ever holds a transparent pixmap; a faded node is never drawn at all,
// so at the low level a cache would only keep stale pixmaps in QPixmapCache.
const QGraphicsItem::CacheMode kNodeCacheModeFullDetail = QGraphicsItem::DeviceCoordinateCache;
const QGraphicsItem::CacheMode kNodeCacheModeLowDetail = QGraphicsItem::NoCache;

class NodeGraphLowDetailLayer
    : public QGraphicsItem {
public:
    // Edges stack with the loop overlays and link arrows at z 15 and nodes above them all, so the
    // two cannot share one item.
    enum PassEnum {
        ePassEdges,
        ePassNodes
    };

    NodeGraphLowDetailLayer(NodeGraphPrivate* graph,
                            PassEnum pass,
                            QGraphicsItem* parent);

    virtual ~NodeGraphLowDetailLayer();

    virtual QRectF boundingRect() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) OVERRIDE FINAL;

    // Fades item, a node or an edge a node owns, out at the low level of detail and back in at the
    // full one. Backdrops are left to paint themselves: there are few of them and they stack by
    // creation order, which the batches do not keep.
    static void applyDetailLevel(QGraphicsItem* item,
                                 bool isNode,
                                 bool lowDetail);

    // Whether the layers draw item instead of item itself.
    static bool isFaded(const QGraphicsItem* item)
    {
        return item->opacity() <= 0.;
    }

private:
    void paintEdges(QPainter* painter, const QRectF& exposed);

    void paintNodes(QPainter* painter, const QRectF& exposed);

    NodeGraphPrivate* _graph;
    PassEnum _pass;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_NodeGraphLowDetailLayer_h
