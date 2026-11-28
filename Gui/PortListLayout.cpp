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

#include "PortListLayout.h"

#include <algorithm>
#include <string>
#include <variant>
#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QGraphicsItem>
#include <QPainterPath>
#include <QPolygonF>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/Nodes/MissingPluginNode.h"
#include "Engine/Nodes/Network/NetworkNode.h"

#include "Gui/Edge.h"
#include "Gui/NodeGui.h"
#include "Gui/PortNodeGui.h"

#include "Serialization/KnobSerialization.h"
#include "Serialization/NodeSerialization.h"

NATRON_NAMESPACE_ENTER

PortListLayout::PortListLayout(NodeGui* node)
    : ConnectorLayout(node)
    , _inputCount(0)
{
}

PortListLayout::~PortListLayout()
{
}

double
PortListLayout::headerHeight()
{
    return TO_DPIY(24);
}

double
PortListLayout::rowHeight()
{
    return TO_DPIY(18);
}

double
PortListLayout::portRadius()
{
    return TO_DPIX(5);
}

NetworkNode*
PortListLayout::getNetworkNode() const
{
    NodePtr node = _node->getNode();

    return node ? dynamic_cast<NetworkNode*>(node->getEffectInstance().get()) : 0;
}

bool
PortListLayout::isMissingPlugin() const
{
    NodePtr node = _node->getNode();

    return node && dynamic_cast<MissingPluginNode*>(node->getEffectInstance().get());
}

double
PortListLayout::nodeHeaderHeight() const
{
    return headerHeight() + (isMissingPlugin() ? rowHeight() : 0.);
}

std::string
PortListLayout::outputReadBy(const NodePtr& node,
                             int inputNb)
{
    if (!node || (inputNb < 0) || (inputNb >= node->getNInputs())) {
        return std::string();
    }
    const std::string label = node->getInputLabel(inputNb);
    if (NetworkNode* network = dynamic_cast<NetworkNode*>(node->getEffectInstance().get())) {
        return network->getOutputForInput(label);
    }
    MissingPluginNode* placeholder = dynamic_cast<MissingPluginNode*>(node->getEffectInstance().get());
    const Serialization::NodeSerializationPtr stored = placeholder ? placeholder->getStoredSerialization() : Serialization::NodeSerializationPtr();
    if (!stored) {
        return std::string();
    }
    for (const Serialization::KnobSerialization& knob : stored->knobs) {
        if ((knob.name != kNetworkNodeParamInputOutputs) || knob.dimensions.empty() || !knob.dimensions[0].value) {
            continue;
        }
        const std::string* text = std::get_if<std::string>(&*knob.dimensions[0].value);
        for (const std::string& entry : text ? networkSplitPorts(*text) : std::vector<std::string>()) {
            const std::size_t eq = entry.find('=');
            if ((eq != std::string::npos) && (entry.compare(0, eq, label) == 0) && (eq == label.size())) {
                return entry.substr(eq + 1);
            }
        }
    }

    return std::string();
}

QPointF
PortListLayout::inputPortPosition(int inputNb) const
{
    const QRectF body = _node->bodyRect();
    const PortNodeGui* portNode = dynamic_cast<const PortNodeGui*>(_node);
    const int row = portNode ? portNode->getInputRow(inputNb) : std::max(0, inputNb);

    return QPointF(body.left() + PortNodeGui::portInset(), body.top() + nodeHeaderHeight() + rowHeight() * (row + 0.5));
}

QPointF
PortListLayout::outputPortPosition(int outputIndex) const
{
    const QRectF body = _node->bodyRect();

    return QPointF(body.right() - PortNodeGui::portInset(), body.top() + nodeHeaderHeight() + rowHeight() * (std::max(0, outputIndex) + 0.5));
}

int
PortListLayout::outputIndexOf(const std::string& outputName) const
{
    if (outputName.empty()) {
        return 0;
    }
    if (NetworkNode* network = getNetworkNode()) {
        const std::vector<OutputPortDescriptor>& outputs = network->getOutputPorts();
        for (std::size_t i = 0; i < outputs.size(); ++i) {
            if (outputs[i].name == outputName) {
                return (int)i;
            }
        }

        return -1;
    }
    const PortNodeGui* portNode = dynamic_cast<const PortNodeGui*>(_node);
    if (portNode) {
        const std::vector<PortNodeRow>& outputs = portNode->getOutputRows();
        for (std::size_t i = 0; i < outputs.size(); ++i) {
            if (outputs[i].name == outputName) {
                return (int)i;
            }
        }
    }

    return -1;
}

std::string
PortListLayout::outputNameForInput(int inputNb) const
{
    return outputReadBy(_node->getNode(), inputNb);
}

void
PortListLayout::resetInputs(std::size_t inputCount)
{
    _inputCount = inputCount;
}

void
PortListLayout::layoutInputs()
{
    PortNodeGui* portNode = dynamic_cast<PortNodeGui*>(_node);
    if (portNode) {
        portNode->refreshPorts();
    }
    const std::vector<Edge*>& inputEdges = _node->getInputsArrows();
    for (std::size_t i = 0; i < inputEdges.size(); ++i) {
        inputEdges[i]->initLine();
    }
}

ConnectorAnchor
PortListLayout::anchorAt(const QPointF& nodePoint,
                         const QPointF& tangent,
                         const QGraphicsItem* space) const
{
    ConnectorAnchor anchor;
    const double r = portRadius();

    anchor.point = space->mapFromItem(_node, nodePoint);
    anchor.tangent = tangent;
    // A straight edge from a classic node stops at the port rather than at the node's side.
    anchor.outline = space->mapFromItem(_node, QRectF(nodePoint.x() - r, nodePoint.y() - r, 2 * r, 2 * r)).boundingRect();

    return anchor;
}

ConnectorAnchor
PortListLayout::inputAnchor(int inputNb,
                            const QGraphicsItem* space) const
{
    return anchorAt(inputPortPosition(inputNb), QPointF(-1., 0.), space);
}

ConnectorAnchor
PortListLayout::outputAnchor(const std::string& outputName,
                             const QGraphicsItem* space) const
{
    return anchorAt(outputPortPosition(std::max(0, outputIndexOf(outputName))), QPointF(1., 0.), space);
}

static double
sceneDist2(const QPointF& a,
           const QPointF& b)
{
    const QPointF d = a - b;

    return d.x() * d.x() + d.y() * d.y();
}

ConnectorHit
PortListLayout::hitTest(const QPointF& scenePos,
                        double grabRadius,
                        double unconnectedInputGrabRadius) const
{
    ConnectorHit hit;
    const std::vector<Edge*>& inputEdges = _node->getInputsArrows();
    for (std::size_t i = 0; i < inputEdges.size(); ++i) {
        const bool connected = inputEdges[i] && inputEdges[i]->hasSource();
        const double radius = std::max(connected ? grabRadius : unconnectedInputGrabRadius, portRadius());
        const double d2 = sceneDist2(_node->mapToScene(inputPortPosition((int)i)), scenePos);
        if ((d2 > radius * radius) || ((hit.kind != eConnectorKindNone) && (d2 >= hit.dist2))) {
            continue;
        }
        hit.kind = eConnectorKindInput;
        hit.inputNb = (int)i;
        hit.outputName.clear();
        hit.dist2 = d2;
    }
    std::vector<std::string> outputNames;
    if (NetworkNode* network = getNetworkNode()) {
        for (const OutputPortDescriptor& output : network->getOutputPorts()) {
            outputNames.push_back(output.name);
        }
    } else if (const PortNodeGui* portNode = dynamic_cast<const PortNodeGui*>(_node)) {
        for (const PortNodeRow& output : portNode->getOutputRows()) {
            outputNames.push_back(output.name);
        }
    }
    const double outputRadius = std::max(grabRadius, portRadius());
    for (std::size_t i = 0; i < outputNames.size(); ++i) {
        const double d2 = sceneDist2(_node->mapToScene(outputPortPosition((int)i)), scenePos);
        if ((d2 > outputRadius * outputRadius) || ((hit.kind != eConnectorKindNone) && (d2 >= hit.dist2))) {
            continue;
        }
        hit.kind = eConnectorKindOutput;
        hit.inputNb = -1;
        hit.outputName = outputNames[i];
        hit.dist2 = d2;
    }

    return hit;
}

bool
PortListLayout::hitEdge(const Edge* edge,
                        const QPointF& scenePos,
                        double grabRadius,
                        double unconnectedInputGrabRadius,
                        double* dist2) const
{
    if (!edge->isPipe()) {
        return hitEdgeLine(edge, scenePos, grabRadius, unconnectedInputGrabRadius, dist2);
    }
    *dist2 = Edge::dist2ToPath(edge->mapToScene(edge->getPipePath()), scenePos);

    return !(*dist2 > grabRadius * grabRadius);
}

void
PortListLayout::highlightConnectors(const std::vector<ConnectorHit>& connectors)
{
    PortNodeGui* ports = dynamic_cast<PortNodeGui*>(_node);

    if (!ports) {
        return;
    }
    const std::size_t inputCount = ports->getInputRows().size();
    for (std::size_t i = 0; i < inputCount; ++i) {
        const bool offered = std::any_of(connectors.begin(), connectors.end(), [i](const ConnectorHit& hit) {
            return (hit.kind == eConnectorKindInput) && (hit.inputNb == (int)i);
        });
        ports->setInputPortHighlight((int)i, offered ? ePortHighlightHalo : ePortHighlightDimmed);
    }
    const std::size_t outputCount = ports->getOutputRows().size();
    for (std::size_t i = 0; i < outputCount; ++i) {
        const bool offered = std::any_of(connectors.begin(), connectors.end(), [this, i](const ConnectorHit& hit) {
            return (hit.kind == eConnectorKindOutput) && (outputIndexOf(hit.outputName) == (int)i);
        });
        ports->setOutputPortHighlight((int)i, offered ? ePortHighlightHalo : ePortHighlightDimmed);
    }
}

void
PortListLayout::clearConnectorHighlights()
{
    PortNodeGui* ports = dynamic_cast<PortNodeGui*>(_node);

    if (ports) {
        ports->clearPortHighlights();
    }
}

NATRON_NAMESPACE_EXIT
