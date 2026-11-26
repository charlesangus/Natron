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

#ifndef NATRON_GUI_CONNECTORLAYOUT_H
#define NATRON_GUI_CONNECTORLAYOUT_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <string>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QPointF>
#include <QRectF>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Gui/GuiFwd.h"

class QGraphicsItem;

NATRON_NAMESPACE_ENTER

/**
 * @brief Where an edge meets one connector of a node, in the coordinates of the item that asked
 * (the edge itself, so that both ends of a line are in the same space).
 **/
struct ConnectorAnchor {
    QPointF point;
    // Unit direction from point towards the far end of a connection, y pointing down.
    QPointF tangent;
    // The node outline a straight edge is clipped against. Null when the edge ends at point.
    QRectF outline;
};

enum ConnectorKindEnum {
    eConnectorKindNone,
    eConnectorKindInput,
    eConnectorKindOutput
};

struct ConnectorHit {
    ConnectorKindEnum kind = eConnectorKindNone;
    int inputNb = -1;
    std::string outputName;
    // Squared scene distance from the tested point to the connector.
    double dist2 = 0.;
};

/**
 * @brief How one node places its connectors: the anchor and direction of each input and named
 * output, and which of them lies under a point. Each NodeGui owns one; Edge takes both of its ends
 * from the layouts of the nodes it joins.
 **/
class ConnectorLayout {
public:
    explicit ConnectorLayout(NodeGui* node);

    virtual ~ConnectorLayout();

    NodeGui* getNodeGui() const;

    /**
     * @brief Forgets the placement of the inputs, before the node replaces its input edges with
     * inputCount new ones. A new edge draws itself as soon as it is made, so an input it asks for
     * must already have its unplaced default.
     **/
    virtual void resetInputs(std::size_t inputCount) = 0;

    /**
     * @brief Places the node's inputs after they, or their connections, changed, and redraws the
     * unconnected ones.
     **/
    virtual void layoutInputs() = 0;

    virtual ConnectorAnchor inputAnchor(int inputNb, const QGraphicsItem* space) const = 0;

    /**
     * @brief An empty outputName is the node's main output, the only one a classic node has.
     **/
    virtual ConnectorAnchor outputAnchor(const std::string& outputName, const QGraphicsItem* space) const = 0;

    /**
     * @brief The node's connector nearest scenePos among those within reach of it, or a hit of
     * kind eConnectorKindNone. An unconnected input is reached from unconnectedInputGrabRadius,
     * anything else from grabRadius.
     **/
    virtual ConnectorHit hitTest(const QPointF& scenePos,
                                 double grabRadius,
                                 double unconnectedInputGrabRadius) const
        = 0;

    /**
     * @brief Whether edge, drawn for one of this node's connectors, is within reach of scenePos;
     * *dist2 receives the squared scene distance to it either way. Radii as for hitTest().
     **/
    virtual bool hitEdge(const Edge* edge,
                         const QPointF& scenePos,
                         double grabRadius,
                         double unconnectedInputGrabRadius,
                         double* dist2) const;

    /**
     * @brief hitEdge() for an edge drawn as its straight line, which is how an edge that belongs to
     * no layout is reached too.
     **/
    static bool hitEdgeLine(const Edge* edge,
                            const QPointF& scenePos,
                            double grabRadius,
                            double unconnectedInputGrabRadius,
                            double* dist2);

protected:
    NodeGui* const _node;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_CONNECTORLAYOUT_H
