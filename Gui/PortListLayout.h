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

#ifndef NATRON_GUI_PORTLISTLAYOUT_H
#define NATRON_GUI_PORTLISTLAYOUT_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <string>

#include "Engine/EngineFwd.h"

#include "Gui/ConnectorLayout.h"

NATRON_NAMESPACE_ENTER

class NetworkNode;

/**
 * @brief The layout of a network node: one row per port under a header, input ports down the
 * left side in the order of the node's attributes and named outputs down the right side in the order the node lists
 * them, PortNodeGui::portInset() inside the body's sides. Edges leave and reach a port
 * horizontally, and end on the port itself.
 **/
class PortListLayout
    : public ConnectorLayout {
public:
    explicit PortListLayout(NodeGui* node);

    virtual ~PortListLayout() OVERRIDE;

    static double headerHeight();
    static double rowHeight();
    static double portRadius();

    // Null when the node is not a network node.
    NetworkNode* getNetworkNode() const;

    // Whether the node stands in for a network node whose plug-in is missing.
    bool isMissingPlugin() const;

    // The height of the node's header, which also holds a missing plug-in's caption.
    double nodeHeaderHeight() const;

    /**
     * @brief The name of the output of its upstream node that input inputNb of node reads, empty
     * for the default output: what a network node names, or what a missing one saved.
     **/
    static std::string outputReadBy(const NodePtr& node, int inputNb);

    // Node coordinates of the centre of the port of input inputNb.
    QPointF inputPortPosition(int inputNb) const;

    // Node coordinates of the centre of the port of the output at outputIndex.
    QPointF outputPortPosition(int outputIndex) const;

    /**
     * @brief The index of the output named outputName, 0 for an empty name; -1 for a name the node
     * has no output of.
     **/
    int outputIndexOf(const std::string& outputName) const;

    /**
     * @brief The name of the upstream output input inputNb reads, empty for that node's first.
     **/
    std::string outputNameForInput(int inputNb) const;

    virtual void resetInputs(std::size_t inputCount) OVERRIDE;
    virtual void layoutInputs() OVERRIDE;
    virtual ConnectorAnchor inputAnchor(int inputNb, const QGraphicsItem* space) const OVERRIDE;
    virtual ConnectorAnchor outputAnchor(const std::string& outputName, const QGraphicsItem* space) const OVERRIDE;

    // Outputs are picked up from their ports.
    virtual bool hasOutputEdge() const OVERRIDE
    {
        return false;
    }

    virtual ConnectorHit hitTest(const QPointF& scenePos,
                                 double grabRadius,
                                 double unconnectedInputGrabRadius) const OVERRIDE;

    virtual bool hitEdge(const Edge* edge,
                         const QPointF& scenePos,
                         double grabRadius,
                         double unconnectedInputGrabRadius,
                         double* dist2) const OVERRIDE;

    // Gives the connectors a halo and dims the node's other ports.
    virtual void highlightConnectors(const std::vector<ConnectorHit>& connectors) OVERRIDE;

    virtual void clearConnectorHighlights() OVERRIDE;

private:
    ConnectorAnchor anchorAt(const QPointF& nodePoint,
                             const QPointF& tangent,
                             const QGraphicsItem* space) const;

    std::size_t _inputCount;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_PORTLISTLAYOUT_H
