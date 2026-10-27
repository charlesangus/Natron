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

#ifndef NATRON_GUI_EDGE_H
#define NATRON_GUI_EDGE_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QGraphicsLineItem>
#include <QLineF>
#include <QPointF>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Gui/GuiFwd.h"

NATRON_NAMESPACE_ENTER

struct EdgePrivate;
class Edge
    : public QGraphicsLineItem
{
public:

    ///Used to make an input edge
    Edge(int inputNb,
         double angle,
         const NodeGuiPtr & dest,
         QGraphicsItem *parent = 0);

    ///Used to make an output edge
    Edge(const NodeGuiPtr & src,
         QGraphicsItem *parent = 0);

    virtual ~Edge() OVERRIDE;

    QRectF boundingRect() const OVERRIDE WARN_UNUSED_RETURN;

    QPainterPath shape() const OVERRIDE WARN_UNUSED_RETURN;

    bool contains(const QPointF &point) const OVERRIDE WARN_UNUSED_RETURN;

    /**
     * @brief Squared distance from point to the closest point on the segment (clamped to its
     * endpoints, not the infinite line through it). Both must be in the same coordinate space.
     **/
    static double dist2ToSegment(const QLineF& line, const QPointF& point);

    void setSource(const NodeGuiPtr & src);

    void setSourceAndDestination(const NodeGuiPtr & src, const NodeGuiPtr & dst);

    int getInputNumber() const;

    void setInputNumber(int i);

    NodeGuiPtr getDest() const;
    NodeGuiPtr getSource() const;

    bool hasSource() const;

    void dragSource(const QPointF & src);

    void dragDest(const QPointF & dst);

    void initLine();

    void setAngle(double a);

    void turnOnRenderingColor();

    void turnOffRenderingColor();

    void setUseHighlight(bool highlight);

    void setUseSelected(bool val);

    bool isOutputEdge() const;

    void setDefaultColor(const QColor & color);

    /**
     * @brief The Preferences Selection colour, read live, with the given alpha.
     **/
    static QColor highlightColor(int alpha = 255);

    bool isBendPointVisible() const;

    void setOptional(bool optional);

    void setDashed(bool dashed);

    void setBendPointVisible(bool visible);

    bool isRotoEdge() const;

    bool isMask() const;

    bool isNearbyBendPoint(const QPointF & scenePoint);

    void setLabel(const QString &label);

    /**
     * @brief Refresh the Edge properties such as dashed state, optional state, visibility etc...
     **/
    void refreshState();

    bool computeVisibility() const;

    /**
     * @brief Re-reads the source node's resolved data kind and caches the colour that kind is drawn
     * in; @p force recomputes the colour even when the kind is unchanged.
     * Public because a kind resolves from the whole connected component: an edit anywhere in it can
     * retype nodes whose own edges were not otherwise touched, and NodeGui refreshes those from
     * Node::dataKindChanged().
     **/
    void refreshDataKindPen(bool force = false);

private:

    virtual void paint(QPainter *painter, const QStyleOptionGraphicsItem *options, QWidget *parent = 0) OVERRIDE FINAL;

    std::unique_ptr<EdgePrivate> _imp;
};

// One edge under consideration by pickNearestEdgeCandidate(), with its line already mapped to
// the coordinate space scenePos is in (scene space, for NodeGraph::hasItemNearbyMouse).
struct EdgeNearbyCandidate {
    QLineF line;
    bool isUnconnectedInput; // an input edge with no source: grabbed with unconnectedInputGrabRadius, not grabRadius
};

/**
 * @brief Returns the index into candidates of the one whose line is closest to scenePos, among
 * those within their own grab radius (unconnectedInputGrabRadius for an unconnected input stub,
 * grabRadius for everything else), or -1 if none qualify.
 **/
int pickNearestEdgeCandidate(const QPointF& scenePos,
                             const std::vector<EdgeNearbyCandidate>& candidates,
                             double grabRadius,
                             double unconnectedInputGrabRadius);

/**
 * @brief An arrow in the graph representing an expression between 2 nodes or that one node is a clone of another.
 **/
class LinkArrow
    : public QObject, public QGraphicsLineItem
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    LinkArrow(const NodeGuiPtr& master,
              const NodeGuiPtr& slave,
              QGraphicsItem* parent);

    virtual ~LinkArrow();

    void setColor(const QColor & color);

    void setArrowHeadColor(const QColor & headColor);

    void setWidth(int lineWidth);

    /**
     * @brief An undirected link is drawn without an arrow head.
     **/
    void setUndirected(bool undirected);

    void setDashed(bool dashed);

public Q_SLOTS:

    /**
     * @brief Called when one of the 2 nodes is moved
     **/
    void refreshPosition();

private:

    virtual void paint(QPainter *painter, const QStyleOptionGraphicsItem *options, QWidget *parent = 0) OVERRIDE FINAL;
    NodeGuiWPtr _master, _slave;
    QPolygonF _arrowHead;
    QColor _renderColor;
    QColor _headColor;
    int _lineWidth;
    bool _undirected;
    bool _dashed;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_EDGE_H
