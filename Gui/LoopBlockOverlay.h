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

#ifndef Gui_LoopBlockOverlay_h
#define Gui_LoopBlockOverlay_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QColor>
#include <QGraphicsObject>
#include <QPainterPath>
#include <QPolygonF>
#include <QtCore/QObject>
#include <QtCore/QPointer>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/EngineFwd.h"

#include "Gui/GuiFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Paints the loop blocks of a node graph under its nodes: a hull around each block's
 * nodes in the colour the pair shares, a downward wedge under the LoopBegin and an upward one
 * over the LoopEnd, and, while either is selected, the hull's outline and a ring round both.
 **/
class LoopBlockItem
    : public QGraphicsObject {
public:
    struct Block {
        QColor color;
        QPainterPath hull;
        QPolygonF beginWedge;
        QPolygonF endWedge;
        QRectF beginRect;
        QRectF endRect;
        bool highlighted;
    };

    explicit LoopBlockItem(QGraphicsItem* parent);

    void setBlocks(const std::vector<Block>& blocks);

    virtual QRectF boundingRect() const OVERRIDE FINAL;
    virtual void paint(QPainter* painter,
                       const QStyleOptionGraphicsItem* option,
                       QWidget* widget) OVERRIDE FINAL;

private:
    std::vector<Block> _blocks;
    QRectF _bounds;
};

/**
 * @brief Keeps a graph's LoopBlockItem up to date: which nodes form each block when the graph's
 * connections change, their shapes when nodes move or the selection changes. Both are coalesced
 * into one refresh per event loop pass.
 **/
class LoopBlockOverlay
    : public QObject {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    LoopBlockOverlay(NodeGraph* graph,
                     QGraphicsItem* nodeRoot);

    virtual ~LoopBlockOverlay() OVERRIDE;

    /**
     * @brief Schedules a refresh; topology also recomputes which nodes form each block.
     **/
    void schedule(bool topology);

    /**
     * @brief The colour of the loop block a LoopBegin opens, the same for the same loop name.
     **/
    static QColor pairColor(const std::string& loopName);

    /**
     * @brief The nodes on the paths from begin down to end, both excluded.
     **/
    static std::vector<NodePtr> pathNodes(const NodePtr& begin,
                                          const NodePtr& end);

public Q_SLOTS:

    void refresh();

private:
    struct Membership {
        NodeWPtr begin;
        NodeWPtr end;
        std::vector<NodeWPtr> nodes;
        std::string name;
    };

    void recomputeMembership();

    NodeGraph* _graph;
    QPointer<LoopBlockItem> _item;
    std::vector<Membership> _blocks;
    bool _pending;
    bool _topologyDirty;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_LoopBlockOverlay_h
