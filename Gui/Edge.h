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
#include <QPen>
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

    /**
     * @brief Squared distance from point to the closest point on the segment (clamped to its
     * endpoints, not the infinite line through it). Both must be in the same coordinate space.
     **/
    static double dist2ToSegment(const QLineF& line, const QPointF& point);

    /**
     * @brief The distance in viewport pixels from which an edge can be grabbed, grown with the
     * Pipe Width preference. A dangling edge (an unconnected input or an output stub) has no pipe
     * to click along, only its short stub, so it gets the larger radius.
     **/
    static double grabRadiusPixels(bool dangling);

    // The width across the base of a pipe's arrowhead in pixels, at node scale 1, from the Pipe
    // Width and Arrowhead Width preferences.
    static double arrowheadWidthPixels();

    // An input with no source, or an output stub.
    bool isDangling() const;

    // Whether scenePoint is on the drawn output nub; false for every edge not drawn as one. For a
    // Dot's ring (isDotRing()) it is whether scenePoint is outside the Dot's disk and at most
    // ringReach, in scene units, outside the ring.
    bool isScenePointOnNub(const QPointF& scenePoint, double ringReach = 0.) const;

    // The output of a Dot in the Nub style: a ring around the disk, drawn only while hovered.
    bool isDotRing() const;

    // Re-reads the Pipe Width preference into the pen. The caller re-lays the line, whose arrow
    // head scales with the width.
    void refreshPipeWidth();

    void setSource(const NodeGuiPtr & src);

    void setSourceAndDestination(const NodeGuiPtr & src, const NodeGuiPtr & dst);

    int getInputNumber() const;

    void setInputNumber(int i);

    NodeGuiPtr getDest() const;
    NodeGuiPtr getSource() const;

    bool hasSource() const;

    void dragSource(const QPointF & src);

    void dragDest(const QPointF & dst);

    // Left to the end of an open node refresh batch on the graph of the node owning the edge.
    void initLine();

    void initLineNow();

    void setAngle(double a);

    void turnOnRenderingColor();

    void turnOffRenderingColor();

    void setUseHighlight(bool highlight);

    void setUseSelected(bool val);

    // Drawn in the highlight colour while a click at the cursor would pick this edge up. Scripts
    // read the state back through QGraphicsItem::data(kHoverHighlightDataKey).
    void setHoverHighlight(bool hover);

    bool isHoverHighlighted() const;

    static const int kHoverHighlightDataKey = 0;

    bool isOutputEdge() const;

    void setIsSpliceHint(bool hint);

    static QColor selectionColorWithAlpha(int alpha = 255);

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
     * Like initLine(), left to the end of an open node refresh batch.
     **/
    void refreshDataKindPen(bool force = false);

    void refreshDataKindPenNow(bool force = false);

    // The pen the line is stroked with in the current state, dashes included.
    QPen getDrawnPen() const;

private:

    virtual void paint(QPainter *painter, const QStyleOptionGraphicsItem *options, QWidget *parent = 0) OVERRIDE FINAL;

    virtual QVariant itemChange(GraphicsItemChange change, const QVariant& value) OVERRIDE;

    void invalidateGraphNavigator();

    void appearanceChanged();

    std::unique_ptr<EdgePrivate> _imp;
};

// One edge under consideration by pickNearestEdgeCandidate(), with its line already mapped to
// the coordinate space scenePos is in (scene space, for NodeGraph::hasItemNearbyMouse).
struct EdgeNearbyCandidate {
    QLineF line;
    bool isDangling; // grabbed with danglingGrabRadius, not grabRadius
    // scenePos is on the candidate's drawn output nub. The pipes a connected output feeds leave from
    // under its nub, so a press there must pull a new pipe rather than pick one of them up.
    bool onNub = false;
    // The candidate is a Dot's ring: it is picked only when scenePos is in its hit area (onNub),
    // and then only when no other candidate is within its grab radius.
    bool isDotRing = false;
};

/**
 * @brief Returns the index into candidates of the one whose line is closest to scenePos, among
 * those within their own grab radius (danglingGrabRadius for an unconnected input or output stub,
 * grabRadius for everything else), or -1 if none qualify. A candidate with onNub set wins over
 * every one without it, except that a Dot's ring loses to every other qualifying candidate.
 **/
int pickNearestEdgeCandidate(const QPointF& scenePos,
                             const std::vector<EdgeNearbyCandidate>& candidates,
                             double grabRadius,
                             double danglingGrabRadius);

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

    QRectF boundingRect() const OVERRIDE WARN_UNUSED_RETURN;

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
