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

#include "NodeGraph.h"
#include "NodeGraphPrivate.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <stdexcept>

GCC_DIAG_UNUSED_PRIVATE_FIELD_OFF
CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QApplication>
#include <QCursor>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)
GCC_DIAG_UNUSED_PRIVATE_FIELD_ON

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Network/NetworkBuilder.h"
#include "Engine/Nodes/Network/NetworkNode.h"
#include "Engine/Nodes/Network/PortTypes.h"
#include "Engine/Settings.h"
#include "Engine/TimeLine.h"

#include "Gui/BackdropGui.h"
#include "Gui/Edge.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/GuiMacros.h"
#include "Gui/NodeGraphPickUp.h"
#include "Gui/NodeGraphUndoRedo.h"
#include "Gui/NodeGui.h"
#include "Gui/PortListLayout.h"
#include "Gui/PortNodeGui.h"
#include "Gui/TabMenuBridge.h"
#include "Gui/UndoHistory.h"

#include "Global/QtCompat.h"

#define NATRON_GRAPH_AUTO_SCROLL_OFFSET 20
#define NATRON_AUTO_SCROLL_TIMEOUT_MS 50


NATRON_NAMESPACE_ENTER
static bool
handleConnectionError(const NodeGuiPtr& outputNode,
                      const NodeGuiPtr& inputNode,
                      int inputNb)
{
    NodePtr conflictingNode;
    Node::CanConnectInputReturnValue linkRetCode = outputNode->getNode()->canConnectInput(inputNode->getNode(), inputNb, &conflictingNode);

    if ( (linkRetCode != Node::eCanConnectInput_ok) && (linkRetCode != Node::eCanConnectInput_inputAlreadyConnected) ) {
        // The check judged the current frame, so quote that frame's values.
        const double time = outputNode->getNode()->getApp()->getTimeLine()->currentFrame();
        const NodeMetadataConstPtr outputMetadata = outputNode->getNode()->getEffectInstance()->getMetadata(time, ViewIdx(0));
        const NodeMetadataConstPtr inputMetadata = inputNode->getNode()->getEffectInstance()->getMetadata(time, ViewIdx(0));
        const double outputPAR = outputMetadata ? outputMetadata->getPixelAspectRatio(-1) : 1.;
        const double inputPAR = inputMetadata ? inputMetadata->getPixelAspectRatio(-1) : 1.;
        const double outputFPS = outputMetadata ? outputMetadata->getOutputFrameRate() : 0.;
        const double inputFPS = inputMetadata ? inputMetadata->getOutputFrameRate() : 0.;
        if (linkRetCode == Node::eCanConnectInput_differentPars) {
            QString error = QCoreApplication::translate("NodeGraph", "%1 and %2 do not have the same pixel aspect ratio (%3 vs. %4), and %1 does not support inputs with different pixel aspect ratio.\n"
                                                                     "This might yield unwanted results.")
                                .arg(QString::fromUtf8(outputNode->getNode()->getLabel().c_str()))
                                .arg(QString::fromUtf8(inputNode->getNode()->getLabel().c_str()))
                                .arg(outputPAR)
                                .arg(inputPAR);
            Dialogs::warningDialog( QCoreApplication::translate("NodeGraph", "Different pixel aspect").toStdString(),
                                    error.toStdString() );

            return true;
        } else if (linkRetCode == Node::eCanConnectInput_differentFPS) {
            QString error = QCoreApplication::translate("NodeGraph", "%1 and %2 do not have the same frame rate (%3 vs. %4).\n"
                                                                     "This might yield unwanted results.\n"
                                                                     "Either change the FPS from the Read node parameters or change the settings of the project.")
                                .arg(QString::fromUtf8(outputNode->getNode()->getLabel().c_str()))
                                .arg(QString::fromUtf8(inputNode->getNode()->getLabel().c_str()))
                                .arg(outputFPS)
                                .arg(inputFPS);
            Dialogs::warningDialog( QCoreApplication::translate("NodeGraph", "Different frame rate").toStdString(),
                                    error.toStdString() );

            return true;
        } else if (linkRetCode == Node::eCanConnectInput_groupHasNoOutput) {
            QString error = QCoreApplication::translate("NodeGraph", "You cannot connect %1 to %2, because it is a group which does not have an Output node.")
                            .arg( QString::fromUtf8( outputNode->getNode()->getLabel().c_str() ) )
                            .arg( QString::fromUtf8( inputNode->getNode()->getLabel().c_str() ) );
            Dialogs::errorDialog( QCoreApplication::translate("NodeGraph", "Different frame rate").toStdString(),
                                  error.toStdString() );
        } else if (linkRetCode == Node::eCanConnectInput_multiResNotSupported) {
            QString error = QCoreApplication::translate("NodeGraph", "You cannot connect %1 to %2 because multi-resolution is not supported on %1, "
                                                        "which means that it cannot receive images with a lower left corner different than (0,0) and cannot have "
                                                        "multiple inputs/outputs with different image sizes.\n"
                                                        "To overcome this, use a Resize or Crop node upstream to change the image size. In some cases you might also need to make sure the input image size is the project size.").arg( QString::fromUtf8( outputNode->getNode()->getLabel().c_str() ) )
                            .arg( QString::fromUtf8( inputNode->getNode()->getLabel().c_str() ) );
            Dialogs::errorDialog( QCoreApplication::translate("NodeGraph", "Multi-resolution not supported").toStdString(),
                                  error.toStdString() );;
        } else if (linkRetCode == Node::eCanConnectInput_incompatibleDataKind) {
            QString error = QCoreApplication::translate("NodeGraph", "You cannot connect %1 to %2 because the data produced is incompatible with %3.")
                                .arg(QString::fromUtf8(outputNode->getNode()->getLabel().c_str()))
                                .arg(QString::fromUtf8(inputNode->getNode()->getLabel().c_str()))
                                .arg(conflictingNode ? QString::fromUtf8(conflictingNode->getLabel().c_str()) : QString::fromUtf8(outputNode->getNode()->getLabel().c_str()));
            Dialogs::errorDialog(QCoreApplication::translate("NodeGraph", "Incompatible data kind").toStdString(),
                                 error.toStdString());
        } else if (linkRetCode == Node::eCanConnectInput_incompatiblePorts) {
            std::string reason;
            (void)NetworkNode::checkInputConnection(inputNode->getNode(), *outputNode->getNode(), inputNb, &reason);
            QString error = QCoreApplication::translate("NodeGraph", "You cannot connect %1 to %2: %3.")
                                .arg(QString::fromUtf8(inputNode->getNode()->getLabel().c_str()))
                                .arg(QString::fromUtf8(outputNode->getNode()->getLabel().c_str()))
                                .arg(QString::fromUtf8(reason.c_str()));
            Dialogs::errorDialog(QCoreApplication::translate("NodeGraph", "Incompatible ports").toStdString(),
                                 error.toStdString());
        }

        return false;
    }

    if ( (linkRetCode == Node::eCanConnectInput_ok) && outputNode->getNode()->getEffectInstance()->isReader() &&
         ( inputNode->getNode()->getPluginID() != PLUGINID_OFX_RUNSCRIPT) &&
         !inputNode->getNode()->getEffectInstance()->isWriter() ) {
        Dialogs::warningDialog( QCoreApplication::translate("NodeGraph", "Reader input").toStdString(),
                                QCoreApplication::translate("NodeGraph", "Connecting an input to a Reader node "
                                                            "is only useful when connecting to the output of a Writer "
                                                            "or RunScript node "
                                                            "so that the Reader automatically reads an image "
                                                            "when the render of the Writer or RunScript is done.").toStdString() );
    }

    return true;
} // handleConnectionError

bool
NodeGraph::connectInputArrowToNode(Edge* arrow,
                                   const NodeGuiPtr& target)
{
    assert(arrow && !arrow->isOutputEdge());
    NodeGuiPtr nodeHoldingEdge = arrow->getDest();
    assert(nodeHoldingEdge);

    bool ok = handleConnectionError(nodeHoldingEdge, target, arrow->getInputNumber());
    if (ok) {
        pushUndoCommand(new ConnectCommand(this, arrow, arrow->getSource(), target),
                        NodesGuiList { nodeHoldingEdge, arrow->getSource(), target });
    }

    return ok;
}

// Moves the arrowhead end of a connected pipe: the input it fed is disconnected and the target's
// preferred input takes the same source, as one undo step.
bool
NodeGraph::moveInputArrowDestinationToNode(Edge* arrow,
                                           const NodeGuiPtr& target)
{
    assert(arrow && !arrow->isOutputEdge());
    NodeGuiPtr source = arrow->getSource();
    NodeGuiPtr oldDest = arrow->getDest();
    if (!source || !oldDest || !target) {
        return false;
    }
    int preferredInput = target->getNode()->getPreferredInputForConnection();
    if (preferredInput == -1) {
        return false;
    }
    Edge* targetInput = target->getInputArrow(preferredInput);
    if (!targetInput || !handleConnectionError(target, source, preferredInput)) {
        return false;
    }

    QUndoCommand* moveCommand = new QUndoCommand(tr("Move pipe"));
    new ConnectCommand(this, arrow, source, NodeGuiPtr(), moveCommand);
    new ConnectCommand(this, targetInput, targetInput->getSource(), source, moveCommand);
    pushUndoCommand(moveCommand, NodesGuiList { oldDest, source, target, targetInput->getSource() });

    return true;
}

// Connects the node owning an output stub to the target's preferred input, as one undo step.
bool
NodeGraph::connectOutputArrowToNode(Edge* arrow,
                                    const NodeGuiPtr& target)
{
    assert(arrow && arrow->isOutputEdge());
    NodeGuiPtr source = arrow->getSource();
    if (!source || !target || (target == source)) {
        return false;
    }
    int preferredInput = target->getNode()->getPreferredInputForConnection();
    if (preferredInput == -1) {
        return false;
    }
    Edge* targetInput = target->getInputArrow(preferredInput);
    if (!targetInput || !handleConnectionError(target, source, preferredInput)) {
        return false;
    }
    pushUndoCommand(new ConnectCommand(this, targetInput, targetInput->getSource(), source),
                    NodesGuiList { target, targetInput->getSource(), source });

    return true;
}

bool
NodeGraph::hasPickedUpArrow() const
{
    if (_imp->_portPipe && _imp->_portPipe->held) {
        return true;
    }

    return _imp->_pickedUpArrowIsOutput || (_imp->_pickedUpArrowInput >= 0);
}

// An output stub is only drawn while its node feeds nothing, so once the node gains an output the
// stub is no longer held.
Edge*
NodeGraph::getPickedUpArrow() const
{
    if (!hasPickedUpArrow()) {
        return 0;
    }
    NodeGuiPtr node = _imp->_pickedUpArrowNode.lock();
    if (!node || !node->getNode() || !node->getNode()->isActivated()) {
        return 0;
    }
    if (_imp->_pickedUpArrowIsOutput) {
        Edge* stub = node->getOutputArrow();

        return (stub && stub->isVisible()) ? stub : 0;
    }

    return node->getInputArrow(_imp->_pickedUpArrowInput);
}

void
NodeGraph::pickUpArrow(Edge* arrow,
                       PickUpEndEnum end)
{
    assert(arrow);
    arrow->initLine();
    const bool isOutput = arrow->isOutputEdge();
    _imp->_pickedUpArrowNode = isOutput ? arrow->getSource() : arrow->getDest();
    _imp->_pickedUpArrowInput = isOutput ? -1 : arrow->getInputNumber();
    _imp->_pickedUpArrowIsOutput = isOutput;
    _imp->_pickedUpArrowEnd = carriedPickUpEnd(end, isOutput, arrow->hasSource());
    dragPickedUpArrowToCursor();
}

void
NodeGraph::forgetPickedUpArrow()
{
    _imp->_pickedUpArrowNode.reset();
    _imp->_pickedUpArrowInput = -1;
    _imp->_pickedUpArrowIsOutput = false;
    _imp->_pickedUpArrowEnd = ePickUpEndSource;
}

// _lastMousePos rather than QCursor::pos(): every mouse and wheel handler keeps it current, and it
// also follows synthesized events, which never move the system cursor.
void
NodeGraph::dragPickedUpArrowToCursor()
{
    if (_imp->_portPipe) {
        updatePortPipePreview();

        return;
    }
    Edge* arrow = getPickedUpArrow();
    if (!arrow) {
        return;
    }
    QPointF cursorScene = mapToScene(_imp->_lastMousePos.x(), _imp->_lastMousePos.y());
    if (_imp->_pickedUpArrowEnd == ePickUpEndDestination) {
        arrow->dragDest(arrow->mapFromScene(cursorScene));
    } else {
        arrow->dragSource(arrow->mapFromScene(cursorScene));
    }
    update();
}

// The arrow's connection is never changed while it is held, whichever end is carried, only its
// drawn line, so restoring the line restores the original connection and nothing reaches the undo
// stack.
void
NodeGraph::dropPickedUpArrow()
{
    cancelPortPipe();
    Edge* arrow = getPickedUpArrow();
    forgetPickedUpArrow();
    if (arrow) {
        arrow->initLine();
        update();
    }
}

bool
NodeGraph::handlePickUpPress(QMouseEvent* e)
{
    if (_imp->_portPipe) {
        return handlePortPipePress(e);
    }
    Edge* arrow = getPickedUpArrow();
    if (!arrow) {
        dropPickedUpArrow();

        return false;
    }

    PickUpButtonEnum button = ePickUpButtonOther;
    if (buttonDownIsLeft(e)) {
        button = ePickUpButtonLeft;
    } else if (buttonDownIsRight(e)) {
        button = ePickUpButtonRight;
    } else if (buttonDownIsMiddle(e)) {
        button = ePickUpButtonMiddle;
    }

    const bool carriesOutputStub = arrow->isOutputEdge();
    NodeGuiPtr nodeHoldingEdge = carriesOutputStub ? arrow->getSource() : arrow->getDest();
    NodeGuiPtr arrowSource = arrow->getSource();
    const bool carriesDestination = (_imp->_pickedUpArrowEnd == ePickUpEndDestination) && arrowSource;
    NodeGui* nearbyNode = 0;
    Edge* nearbyEdge = 0;
    PickUpClickTargetEnum target = ePickUpClickTargetOther;
    QPointF navigatorScenePos;
    if (isNearbyNavigator(e->pos(), navigatorScenePos)) {
        target = ePickUpClickTargetNavigator;
    } else if ((hasItemNearbyMouse(e->pos(), &nearbyNode, &nearbyEdge) == eNearbyItemNode) && nearbyNode) {
        // With the arrowhead carried, the pipe hangs from its source: clicking either node it
        // already joins leaves the connection as it was.
        const bool isHoldingNode = (nearbyNode == nodeHoldingEdge.get()) || (carriesDestination && (nearbyNode == arrowSource.get()));
        target = isHoldingNode ? ePickUpClickTargetHoldingNode : ePickUpClickTargetNode;
    }

    switch (decidePickUpPress(button, target, carriesOutputStub)) {
    case ePickUpPressPassThrough:
        return false;
    case ePickUpPressIgnore:
        return true;
    case ePickUpPressCancel:
        dropPickedUpArrow();

        return true;
    case ePickUpPressDisconnect: {
        forgetPickedUpArrow();
        if (!carriesOutputStub && arrow->getSource()) {
            pushUndoCommand(new ConnectCommand(this, arrow, arrow->getSource(), NodeGuiPtr()),
                            NodesGuiList { arrow->getDest(), arrow->getSource() });
        }
        if (nodeHoldingEdge) {
            nodeHoldingEdge->refreshEdges();
        }
        arrow->initLine();
        scene()->update();
        update();

        return true;
    }
    case ePickUpPressConnect: {
        NodeGuiPtr targetNode = nearbyNode->shared_from_this();
        // Let go of the arrow before connecting: a rejected connection opens a dialog, and the
        // focus loss that causes must not find the arrow still held.
        forgetPickedUpArrow();
        if (carriesOutputStub) {
            connectOutputArrowToNode(arrow, targetNode);
            targetNode->refreshEdges();
        } else if (carriesDestination) {
            moveInputArrowDestinationToNode(arrow, targetNode);
            targetNode->refreshEdges();
        } else {
            connectInputArrowToNode(arrow, targetNode);
        }
        if (nodeHoldingEdge) {
            nodeHoldingEdge->refreshEdges();
        }
        arrow->initLine();
        scene()->update();
        update();

        return true;
    }
    }

    return true;
}

NATRON_NAMESPACE_ANONYMOUS_ENTER

NetworkNode*
networkOf(const NodeGuiPtr& gui)
{
    NodePtr node = gui ? gui->getNode() : NodePtr();

    return node ? dynamic_cast<NetworkNode*>(node->getEffectInstance().get()) : NULL;
}

PortListLayout*
portLayoutOf(const NodeGuiPtr& gui)
{
    return gui ? dynamic_cast<PortListLayout*>(gui->getConnectorLayout()) : NULL;
}

std::string
outputNamedForInput(const NodeGuiPtr& dst,
                    int inputNb)
{
    NetworkNode* network = networkOf(dst);
    NodePtr node = dst ? dst->getNode() : NodePtr();

    if (!network || (inputNb < 0) || (inputNb >= node->getNInputs())) {
        return std::string();
    }

    return network->getOutputForInput(node->getInputLabel(inputNb));
}

// The output a connected input reads: the one it names, or the source's first when the source
// lacks it, as the engine resolves it.
std::string
outputReadByInput(const NodeGuiPtr& dst,
                  int inputNb,
                  const NodeGuiPtr& src)
{
    NetworkNode* source = networkOf(src);
    const std::string named = outputNamedForInput(dst, inputNb);

    if (!source || source->findOutput(named) || source->getOutputPorts().empty()) {
        return named;
    }

    return source->getOutputPorts().front().name;
}

// The attributes a new node of layout has as its inputs, in input order, as NetworkNode makes them.
std::vector<const NetworkAttribute*>
initialPortsOf(const NetworkNodeLayout& layout)
{
    std::vector<const NetworkAttribute*> ports;
    std::set<std::string> seen;
    const auto find = [&layout](const std::string& name) -> const NetworkAttribute* {
        for (const NetworkAttribute& attribute : layout.attributes) {
            if (attribute.name == name) {
                return &attribute;
            }
        }

        return NULL;
    };

    for (const std::string& name : layout.defaultPorts) {
        const NetworkAttribute* attribute = find(name);
        if (attribute && NetworkNode::canPromote(*attribute) && seen.insert(name).second) {
            ports.push_back(attribute);
        }
    }
    for (const NetworkAttribute& attribute : layout.attributes) {
        if (attribute.portOnly && seen.insert(attribute.name).second) {
            ports.push_back(&attribute);
        }
    }

    return ports;
}

// Whether a new node of layout has a port the fixed end of a pipe can connect to: an input for the
// output fixedOutput of fixed, when fromOutput, else an output for the input fixedInput of fixed.
bool
layoutTakesPipe(const NetworkNodeLayout& layout,
                const NetworkNode& fixed,
                bool fromOutput,
                const std::string& fixedOutput,
                int fixedInput)
{
    const std::vector<std::string>& fixedTargets = fixed.layout().targets;

    if (!portTargetsShared(layout.targets, fixedTargets)) {
        return false;
    }
    if (fromOutput) {
        const OutputPortDescriptor* output = fixed.findOutput(fixedOutput);
        if (!output) {
            return false;
        }
        for (const NetworkAttribute* attribute : initialPortsOf(layout)) {
            if (canConnectPorts(fixedTargets, *output, layout.targets, attribute->port)) {
                return true;
            }
        }

        return false;
    }
    const std::optional<PortDescriptor> input = fixed.getInputPortDescriptor(fixedInput);
    if (!input) {
        return false;
    }
    for (const OutputPortDescriptor& output : layout.outputs) {
        if (canConnectPorts(layout.targets, output, fixedTargets, *input)) {
            return true;
        }
    }

    return false;
}

// The connectors of node that can take the fixed end of a pipe, as for layoutTakesPipe().
std::vector<ConnectorHit>
connectorsTakingPipe(const NodeGuiPtr& node,
                     const NodeGuiPtr& fixed,
                     bool fromOutput,
                     const std::string& fixedOutput,
                     int fixedInput)
{
    std::vector<ConnectorHit> found;
    NetworkNode* network = networkOf(node);
    NetworkNode* fixedNetwork = networkOf(fixed);

    if (!network || !fixedNetwork) {
        return found;
    }
    if (fromOutput) {
        const int inputCount = node->getNode()->getNInputs();
        for (int i = 0; i < inputCount; ++i) {
            if (network->canConnectPort(i, fixed->getNode(), fixedOutput)) {
                ConnectorHit hit;
                hit.kind = eConnectorKindInput;
                hit.inputNb = i;
                found.push_back(hit);
            }
        }
    } else {
        for (const OutputPortDescriptor& output : network->getOutputPorts()) {
            if (fixedNetwork->canConnectPort(fixedInput, node->getNode(), output.name)) {
                ConnectorHit hit;
                hit.kind = eConnectorKindOutput;
                hit.outputName = output.name;
                found.push_back(hit);
            }
        }
    }

    return found;
}

// Whether the connection hit names, between node and the fixed end of a pipe, is made.
bool
isWired(const NodeGuiPtr& node,
        const NodeGuiPtr& fixed,
        bool fromOutput,
        int fixedInput,
        const ConnectorHit& hit)
{
    if (fromOutput) {
        return node->getNode()->getInput(hit.inputNb) == fixed->getNode();
    }

    return (fixed->getNode()->getInput(fixedInput) == node->getNode()) && (outputReadByInput(fixed, fixedInput, node) == hit.outputName);
}

QPointF
inputPortScenePos(const NodeGuiPtr& gui,
                  int inputNb)
{
    PortListLayout* layout = portLayoutOf(gui);

    return layout ? gui->mapToScene(layout->inputPortPosition(inputNb)) : QPointF();
}

QPointF
outputPortScenePos(const NodeGuiPtr& gui,
                   const std::string& outputName)
{
    PortListLayout* layout = portLayoutOf(gui);

    return layout ? gui->mapToScene(layout->outputPortPosition(std::max(0, layout->outputIndexOf(outputName)))) : QPointF();
}

NATRON_NAMESPACE_ANONYMOUS_EXIT

bool
NodeGraph::isPortGraph() const
{
    return dynamic_cast<NetworkBuilder*>(getGroup().get()) != NULL;
}

bool
NodeGraph::beginPortPipe(QMouseEvent* e)
{
    NodeGraphPrivate::PortPipe pipe;
    NodeGuiPtr node;
    int inputNb = -1;
    std::string outputName;

    if (portNearbyMouse(e->pos(), &node, &inputNb, &outputName)) {
        Edge* edge = (inputNb >= 0) ? node->getInputArrow(inputNb) : NULL;
        NodeGuiPtr source = edge ? edge->getSource() : NodeGuiPtr();
        if (inputNb < 0) {
            pipe.anchorNode = node;
            pipe.anchorIsOutput = true;
            pipe.anchorOutput = outputName;
        } else if (source && portLayoutOf(source)) {
            pipe.anchorNode = source;
            pipe.anchorIsOutput = true;
            pipe.anchorOutput = outputReadByInput(node, inputNb, source);
            pipe.liftedNode = node;
            pipe.liftedInput = inputNb;
        } else if (source) {
            return false;
        } else {
            pipe.anchorNode = node;
            pipe.anchorInput = inputNb;
        }
    } else {
        NodeGui* nearbyNode = NULL;
        Edge* nearbyEdge = NULL;
        if ((hasItemNearbyMouse(e->pos(), &nearbyNode, &nearbyEdge) != eNearbyItemNodeEdge) || !nearbyEdge || nearbyEdge->isOutputEdge()) {
            return false;
        }
        NodeGuiPtr dst = nearbyEdge->getDest();
        NodeGuiPtr src = nearbyEdge->getSource();
        if (!src || !portLayoutOf(dst) || !portLayoutOf(src)) {
            return false;
        }
        const int dstInput = nearbyEdge->getInputNumber();
        const std::string output = outputReadByInput(dst, dstInput, src);
        const PickUpEndEnum end = pickedUpEnd(mapToScene(e->pos()), inputPortScenePos(dst, dstInput), outputPortScenePos(src, output));
        if (end == ePickUpEndDestination) {
            pipe.anchorNode = src;
            pipe.anchorIsOutput = true;
            pipe.anchorOutput = output;
        } else {
            pipe.anchorNode = dst;
            pipe.anchorInput = dstInput;
        }
        pipe.liftedNode = dst;
        pipe.liftedInput = dstInput;
    }

    _imp->_portPipe = pipe;
    NodeGuiPtr lifted = pipe.liftedNode.lock();
    Edge* liftedEdge = lifted ? lifted->getInputArrow(pipe.liftedInput) : NULL;
    if (liftedEdge) {
        liftedEdge->setVisible(false);
    }
    setPortPipeHighlights(true);
    updatePortPipePreview();

    return true;
} // NodeGraph::beginPortPipe

void
NodeGraph::releasePortPipe(QMouseEvent* e,
                           bool dragged)
{
    if (!_imp->_portPipe) {
        return;
    }
    if (dragged) {
        dropPortPipe(e->pos(), modCASIsShift(e));
    } else {
        _imp->_portPipe->held = true;
    }
}

bool
NodeGraph::handlePortPipePress(QMouseEvent* e)
{
    if (!_imp->_portPipe || !_imp->_portPipe->held) {
        return false;
    }
    if (buttonDownIsLeft(e)) {
        dropPortPipe(e->pos(), modCASIsShift(e));
    } else {
        cancelPortPipe();
    }

    return true;
}

void
NodeGraph::dropPortPipe(const QPoint& viewportPos,
                        bool shiftDrop)
{
    if (!_imp->_portPipe) {
        return;
    }
    if (shiftDrop) {
        NodeGuiPtr port;
        int portInput = -1;
        std::string portOutput;
        NodeGui* nearbyNode = NULL;
        Edge* nearbyEdge = NULL;
        if (!portNearbyMouse(viewportPos, &port, &portInput, &portOutput) && (hasItemNearbyMouse(viewportPos, &nearbyNode, &nearbyEdge) != eNearbyItemNode) && launchShiftDropMenu()) {
            return;
        }
    }
    const NodeGraphPrivate::PortPipe pipe = *_imp->_portPipe;
    cancelPortPipe();

    NodeGuiPtr anchor = pipe.anchorNode.lock();
    NodeGuiPtr lifted = pipe.liftedNode.lock();
    if (!anchor) {
        return;
    }
    NodeGuiPtr target;
    int targetInput = -1;
    std::string targetOutput;
    if (!portNearbyMouse(viewportPos, &target, &targetInput, &targetOutput)) {
        if (lifted && lifted->getNode()->getInput(pipe.liftedInput)) {
            pushUndoCommand(new PortConnectCommand(this, lifted, pipe.liftedInput, NodeGuiPtr(), outputNamedForInput(lifted, pipe.liftedInput)),
                            NodesGuiList { lifted });
        }

        return;
    }

    // An output is dropped on an input, an input on an output; the wrong side is no target.
    NodeGuiPtr dst, src;
    int inputNb = -1;
    std::string output;
    if (pipe.anchorIsOutput) {
        if (targetInput < 0) {
            return;
        }
        dst = target;
        inputNb = targetInput;
        src = anchor;
        output = pipe.anchorOutput;
    } else {
        if (targetInput >= 0) {
            return;
        }
        dst = anchor;
        inputNb = pipe.anchorInput;
        src = target;
        output = targetOutput;
    }
    NetworkNode* network = networkOf(dst);
    if (!network) {
        return;
    }
    NodePtr current = dst->getNode()->getInput(inputNb);
    const bool liftedElsewhere = lifted && ((lifted != dst) || (pipe.liftedInput != inputNb));
    if (!liftedElsewhere && (current == src->getNode()) && (outputReadByInput(dst, inputNb, src) == output)) {
        return;
    }
    std::string reason;
    if (!network->canConnectPort(inputNb, src->getNode(), output, &reason)) {
        getGui()->getApp()->showStatusMessage(tr("Cannot connect %1 to %2: %3.")
                                                  .arg(QString::fromUtf8(src->getNode()->getLabel().c_str()))
                                                  .arg(QString::fromUtf8(dst->getNode()->getLabel().c_str()))
                                                  .arg(QString::fromUtf8(reason.c_str())));

        return;
    }

    NodesGuiList touched { dst, src };
    if (liftedElsewhere && lifted->getNode()->getInput(pipe.liftedInput)) {
        QUndoCommand* move = new QUndoCommand(tr("Move connection"));
        new PortConnectCommand(this, lifted, pipe.liftedInput, NodeGuiPtr(), outputNamedForInput(lifted, pipe.liftedInput), move);
        new PortConnectCommand(this, dst, inputNb, src, output, move);
        touched.push_back(lifted);
        pushUndoCommand(move, touched);
    } else {
        pushUndoCommand(new PortConnectCommand(this, dst, inputNb, src, output), touched);
    }
} // NodeGraph::dropPortPipe

void
NodeGraph::cancelPortPipe()
{
    if (!_imp->_portPipe) {
        return;
    }
    NodeGuiPtr lifted = _imp->_portPipe->liftedNode.lock();
    setPortPipeHighlights(false);
    _imp->_portPipe.reset();
    if (_imp->_portPipePreview) {
        _imp->_portPipePreview->hide();
    }
    if (lifted) {
        lifted->refreshEdgesVisility();
    }
    update();
}

void
NodeGraph::updatePortPipePreview()
{
    if (!_imp->_portPipe) {
        return;
    }
    NodeGuiPtr anchor = _imp->_portPipe->anchorNode.lock();
    PortNodeGui* anchorGui = dynamic_cast<PortNodeGui*>(anchor.get());
    PortListLayout* layout = portLayoutOf(anchor);
    if (!anchorGui || !layout) {
        cancelPortPipe();

        return;
    }
    const bool fromOutput = _imp->_portPipe->anchorIsOutput;
    PortColor color;
    QPointF anchorPos;
    if (fromOutput) {
        const int index = layout->outputIndexOf(_imp->_portPipe->anchorOutput);
        const std::vector<PortNodeRow>& rows = anchorGui->getOutputRows();
        if ((index >= 0) && (index < (int)rows.size())) {
            color = rows[index].look.fill;
        }
        anchorPos = outputPortScenePos(anchor, _imp->_portPipe->anchorOutput);
    } else {
        const int index = _imp->_portPipe->anchorInput;
        const std::vector<PortNodeRow>& rows = anchorGui->getInputRows();
        if ((index >= 0) && (index < (int)rows.size())) {
            color = rows[index].look.fill;
        }
        anchorPos = inputPortScenePos(anchor, index);
    }
    if (!_imp->_portPipePreview) {
        _imp->_portPipePreview = new QGraphicsPathItem(_imp->_nodeRoot);
        // Above the pipes, under the nodes, as the kiss preview.
        _imp->_portPipePreview->setZValue(16);
    }
    QGraphicsPathItem* preview = _imp->_portPipePreview;
    const QPointF cursor = preview->mapFromScene(mapToScene(_imp->_lastMousePos));
    const QPointF anchorPoint = preview->mapFromScene(anchorPos);
    const QPointF start = fromOutput ? anchorPoint : cursor;
    const QPointF end = fromOutput ? cursor : anchorPoint;
    const double reach = std::max(std::abs(end.x() - start.x()) / 2., (double)TO_DPIX(40));
    QPainterPath path(start);
    path.cubicTo(start + QPointF(reach, 0.), end - QPointF(reach, 0.), end);
    QPen pen(QColor(color.r, color.g, color.b));
    pen.setWidthF(2.);
    preview->setPen(pen);
    preview->setPath(path);
    preview->show();
    update();
} // NodeGraph::updatePortPipePreview

void
NodeGraph::setPortPipeHighlights(bool highlighted)
{
    const NodeGraphPrivate::PortPipe* pipe = _imp->_portPipe ? &*_imp->_portPipe : NULL;
    NodeGuiPtr anchor = pipe ? pipe->anchorNode.lock() : NodeGuiPtr();
    NetworkNode* anchorNetwork = networkOf(anchor);

    for (const NodeGuiPtr& gui : getAllActiveNodes()) {
        PortNodeGui* ports = dynamic_cast<PortNodeGui*>(gui.get());
        if (!ports) {
            continue;
        }
        if (!highlighted || !anchorNetwork) {
            ports->clearPortHighlights();
            continue;
        }
        NetworkNode* network = networkOf(gui);
        const std::size_t inputCount = ports->getInputRows().size();
        for (std::size_t i = 0; i < inputCount; ++i) {
            PortHighlightEnum highlight = ePortHighlightDimmed;
            if (pipe->anchorIsOutput) {
                highlight = (network && network->canConnectPort((int)i, anchor->getNode(), pipe->anchorOutput)) ? ePortHighlightHalo : ePortHighlightDimmed;
            } else if ((gui == anchor) && ((int)i == pipe->anchorInput)) {
                highlight = ePortHighlightNone;
            }
            ports->setInputPortHighlight((int)i, highlight);
        }
        const std::vector<PortNodeRow>& outputs = ports->getOutputRows();
        for (std::size_t i = 0; i < outputs.size(); ++i) {
            PortHighlightEnum highlight = ePortHighlightDimmed;
            if (!pipe->anchorIsOutput) {
                highlight = anchorNetwork->canConnectPort(pipe->anchorInput, gui->getNode(), outputs[i].name) ? ePortHighlightHalo : ePortHighlightDimmed;
            } else if ((gui == anchor) && (outputs[i].name == pipe->anchorOutput)) {
                highlight = ePortHighlightNone;
            }
            ports->setOutputPortHighlight((int)i, highlight);
        }
    }
} // NodeGraph::setPortPipeHighlights

bool
NodeGraph::launchShiftDropMenu()
{
    if (!_imp->_portPipe) {
        return false;
    }
    const NodeGraphPrivate::PortPipe pipe = *_imp->_portPipe;
    NodeGuiPtr anchor = pipe.anchorNode.lock();
    NetworkNode* fixed = networkOf(anchor);
    std::optional<TabMenuBridge::NetworkFilter> filter = TabMenuBridge::networkFilterOf(this);
    if (!fixed || !filter) {
        return false;
    }
    const std::vector<std::string>& fixedTargets = fixed->layout().targets;
    std::vector<std::string> targets;
    if (filter->targets.empty()) {
        targets = fixedTargets;
    } else {
        for (const std::string& target : filter->targets) {
            if (std::find(fixedTargets.begin(), fixedTargets.end(), target) != fixedTargets.end()) {
                targets.push_back(target);
            }
        }
    }
    std::vector<std::string> pluginIds;
    for (const std::pair<std::string, std::vector<std::string>>& registered : NetworkNode::registeredPlugins()) {
        if (!portTargetsShared(registered.second, targets)) {
            continue;
        }
        const NetworkNodeLayout* layout = NetworkNode::findLayout(registered.first);
        if (layout && layoutTakesPipe(*layout, *fixed, pipe.anchorIsOutput, pipe.anchorOutput, pipe.anchorInput)) {
            pluginIds.push_back(registered.first);
        }
    }
    cancelPortPipe();
    if (pluginIds.empty()) {
        getGui()->getApp()->showStatusMessage(tr("No node type can connect to %1.").arg(QString::fromUtf8(anchor->getNode()->getLabel().c_str())));

        return true;
    }
    filter->targets = targets;
    filter->pluginIds = pluginIds;
    filter->withBackdrop = false;

    TabMenuBridge::LaunchContext context;
    context.network = filter;
    context.created = [this, pipe](const NodePtr& node) {
        NodeGuiPtr gui = std::dynamic_pointer_cast<NodeGui>(node->getNodeGui());
        NodeGuiPtr fixedGui = pipe.anchorNode.lock();
        NodeGuiPtr lifted = pipe.liftedNode.lock();
        _imp->_portCycle.reset();
        if (!gui || !fixedGui) {
            return;
        }
        const std::vector<ConnectorHit> ports = connectorsTakingPipe(gui, fixedGui, pipe.anchorIsOutput, pipe.anchorOutput, pipe.anchorInput);
        if (ports.empty()) {
            return;
        }
        // The lifted input of a pipe held by its input end is that end, which the wire reconnects.
        if (lifted && pipe.anchorIsOutput && lifted->getNode()->getInput(pipe.liftedInput)) {
            pushUndoCommand(new PortConnectCommand(this, lifted, pipe.liftedInput, NodeGuiPtr(), outputNamedForInput(lifted, pipe.liftedInput)),
                            NodesGuiList { lifted });
        }
        ConnectorHit fixedEnd;
        fixedEnd.kind = pipe.anchorIsOutput ? eConnectorKindOutput : eConnectorKindInput;
        fixedEnd.inputNb = pipe.anchorInput;
        fixedEnd.outputName = pipe.anchorOutput;
        PortWireCommand* wire = new PortWireCommand(this, fixedGui, fixedEnd, gui, ports.front());
        pushUndoCommand(wire, NodesGuiList { gui, fixedGui });
        // A wire that failed is deleted by the history instead of joining the step.
        if (!isWired(gui, fixedGui, pipe.anchorIsOutput, pipe.anchorInput, ports.front())) {
            return;
        }
        NodeGraphPrivate::PortCycle cycle;
        cycle.created = gui;
        cycle.ports = ports;
        cycle.wire = wire;
        cycle.liftedNode = lifted;
        cycle.liftedInput = pipe.liftedInput;
        _imp->_portCycle = cycle;
    };
    context.stepClosed = [this](const NodePtr& node) {
        beginPortCycle(std::dynamic_pointer_cast<NodeGui>(node->getNodeGui()));
    };
    TabMenuBridge::launch(this, context);

    return true;
} // NodeGraph::launchShiftDropMenu

void
NodeGraph::beginPortCycle(const NodeGuiPtr& created)
{
    if (!_imp->_portCycle) {
        return;
    }
    UndoHistory* history = getGui() ? getGui()->getUndoHistory() : NULL;
    const QUndoCommand* entry = history ? history->getLastCommand() : NULL;
    if (!created || (_imp->_portCycle->created.lock() != created) || !entry) {
        _imp->_portCycle.reset();

        return;
    }
    _imp->_portCycle->entry = entry;
    ConnectorLayout* layout = created->getConnectorLayout();
    if (layout) {
        layout->highlightConnectors(_imp->_portCycle->ports);
    }
    setFocus(Qt::OtherFocusReason);
    update();
}

bool
NodeGraph::isPortCycleLive()
{
    if (!_imp->_portCycle) {
        return false;
    }
    const NodeGraphPrivate::PortCycle& cycle = *_imp->_portCycle;
    UndoHistory* history = getGui() ? getGui()->getUndoHistory() : NULL;
    NodeGuiPtr created = cycle.created.lock();
    if (history && created && cycle.wire && cycle.entry && !history->isReplaying() && (history->getLastCommand() == cycle.entry)) {
        return true;
    }
    ConnectorLayout* layout = created ? created->getConnectorLayout() : NULL;
    if (layout) {
        layout->clearConnectorHighlights();
    }
    _imp->_portCycle.reset();
    update();

    return false;
}

void
NodeGraph::stepPortCycle(int step)
{
    if (!isPortCycleLive()) {
        return;
    }
    NodeGraphPrivate::PortCycle& cycle = *_imp->_portCycle;
    const int count = (int)cycle.ports.size();
    if (count < 2) {
        return;
    }
    int next = ((int)cycle.current + step) % count;
    if (next < 0) {
        next += count;
    }
    cycle.current = (std::size_t)next;
    cycle.wire->setPort(cycle.ports[cycle.current]);
    NodeGuiPtr created = cycle.created.lock();
    ConnectorLayout* layout = created ? created->getConnectorLayout() : NULL;
    if (layout) {
        layout->highlightConnectors(cycle.ports);
    }
    update();
}

void
NodeGraph::endPortCycle(bool keep)
{
    if (!_imp->_portCycle) {
        return;
    }
    const NodeGraphPrivate::PortCycle cycle = *_imp->_portCycle;
    const bool live = isPortCycleLive();
    NodeGuiPtr created = cycle.created.lock();
    ConnectorLayout* layout = created ? created->getConnectorLayout() : NULL;
    if (layout) {
        layout->clearConnectorHighlights();
    }
    created.reset();
    _imp->_portCycle.reset();
    if (keep || !live) {
        update();

        return;
    }
    // Undone and then discarded, the node is destroyed, and no redo brings it back.
    UndoHistory* history = getGui()->getUndoHistory();
    history->undo();
    history->discardUndone();
    NodeGuiPtr lifted = cycle.liftedNode.lock();
    if (lifted && lifted->getNode()->getInput(cycle.liftedInput)) {
        pushUndoCommand(new PortConnectCommand(this, lifted, cycle.liftedInput, NodeGuiPtr(), outputNamedForInput(lifted, cycle.liftedInput)),
                        NodesGuiList { lifted });
    }
    update();
} // NodeGraph::endPortCycle

bool
NodeGraph::handlePortCycleKey(QKeyEvent* e)
{
    if (!isPortCycleLive()) {
        return false;
    }
    switch (e->key()) {
    case Qt::Key_Down:
        stepPortCycle(1);

        return true;
    case Qt::Key_Up:
        stepPortCycle(-1);

        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        endPortCycle(true);

        return true;
    case Qt::Key_Escape:
        endPortCycle(false);

        return true;
    case Qt::Key_Shift:
    case Qt::Key_Control:
    case Qt::Key_Alt:
    case Qt::Key_Meta:

        return false;
    default:
        endPortCycle(true);

        return false;
    }
} // NodeGraph::handlePortCycleKey

void
NodeGraph::mouseReleaseEvent(QMouseEvent* e)
{
    EventStateEnum state = _imp->_evtState;

    _imp->_evtState = eEventStateNone;
    _imp->_dragGesture.end();
    _imp->endNavigatorDragDeferral();

    _imp->autoScrollTimer.stop();

    // A pipe press is a drag only once it travels the drag distance, however many move events
    // came in between and whatever modifiers are held.
    const double arrowDragDistance = TO_DPIX(QApplication::startDragDistance());
    const double arrowPressToRelease = (e->pos() - _imp->_pressPos).manhattanLength();
    const bool arrowDragged = isDragDistance(arrowPressToRelease, arrowDragDistance);
    if ((state == eEventStateDraggingArrow) && arrowDragged) {
        bool foundSrc = false;
        assert(_imp->_arrowSelected);
        NodeGuiPtr nodeHoldingEdge = _imp->_arrowSelected->isOutputEdge() ?
                                     _imp->_arrowSelected->getSource() : _imp->_arrowSelected->getDest();
        assert(nodeHoldingEdge);


        NodeGui* nearbyNode;
        Edge* nearbyEdge;
        NearbyItemEnum nearbyItemCode = hasItemNearbyMouse(e->pos(), &nearbyNode, &nearbyEdge);

        if (nearbyItemCode == eNearbyItemNode) {
            NodeGuiPtr targetNode = nearbyNode->shared_from_this();
            if (targetNode != nodeHoldingEdge) {
                if ( !_imp->_arrowSelected->isOutputEdge() ) {
                    foundSrc = connectInputArrowToNode(_imp->_arrowSelected, targetNode);
                } else {
                    foundSrc = connectOutputArrowToNode(_imp->_arrowSelected, targetNode);
                }
            }
        }

        // A dragged input that found no new source is disconnected through the undo stack. An
        // output stub feeds nothing, so a drag of one that found no input leaves nothing to undo.
        if ( !foundSrc && !_imp->_arrowSelected->isOutputEdge() && _imp->_arrowSelected->getSource() ) {
            pushUndoCommand(new ConnectCommand(this, _imp->_arrowSelected, _imp->_arrowSelected->getSource(),
                                               NodeGuiPtr()),
                            NodesGuiList { nodeHoldingEdge, _imp->_arrowSelected->getSource() });
        }


        nodeHoldingEdge->refreshEdges();
        scene()->update();
    } else if (state == eEventStateDraggingPortPipe) {
        releasePortPipe(e, arrowDragged);
    } else if ((state == eEventStateDraggingArrow) && _imp->_arrowSelected) {
        Edge* arrow = _imp->_arrowSelected;
        // Moves short of the drag distance still dragged the drawn line; put it back first.
        arrow->initLine();
        PickUpButtonEnum button = triggerButtonIsLeft(e) ? ePickUpButtonLeft : ePickUpButtonOther;
        if (shouldStartPickUp(button, arrowPressToRelease, arrowDragDistance)) {
            QPointF pressScene = mapToScene(_imp->_pressPos);
            PickUpEndEnum end = pickedUpEnd(pressScene, arrow->mapToScene(arrow->line().p1()), arrow->mapToScene(arrow->line().p2()));
            pickUpArrow(arrow, end);
        }
    } else if (state == eEventStateDraggingNode) {
        if ( !_imp->_hasMovedOnce ) {
            if ( modCASIsControl(e) ) { // control-click is the same as double-click, see NodeGraph::mouseDoubleClickEvent
                NodeGui* nearbyNode;
                Edge* nearbyEdge;
                NearbyItemEnum nearbyItemCode = hasItemNearbyMouse(e->pos(), &nearbyNode, &nearbyEdge);

                if (nearbyItemCode == eNearbyItemNode || nearbyItemCode == eNearbyItemBackdropFrame) {
                    showNodePanel(false, modCASIsShift(e), nearbyNode);
                }
            }
        } else if (!_imp->_selection.empty()) {
            NodesGuiList kissTouched;
            QUndoCommand* kissCommand = takeKissCommand(&kissTouched);
            // Alt returns the nodes to where they were picked up, with the pipes the drag kissed.
            if (kissCommand && modifierHasAlt(e)) {
                for (std::map<NodeGuiPtr, QPointF>::const_iterator it = _imp->_dragPickupPositions.begin(); it != _imp->_dragPickupPositions.end(); ++it) {
                    it->first->setPosition(it->second.x(), it->second.y());
                }
                _imp->_deltaSinceMousePress = QPointF(0, 0);
                resetSplicePlan();
            }

            if (_imp->_splicePipe) {
                Edge* pipe = _imp->_splicePipe;
                NodeGuiPtr entryGui = _imp->_spliceEntry;
                NodeGuiPtr exitGui = _imp->_spliceExit;
                int entryInput = _imp->_spliceEntryInput;
                resetSplicePlan();

                QUndoCommand* spliceCommand = kissCommand ? kissCommand : new QUndoCommand(tr("Insert nodes"));
                NodesGuiList touched;
                takeDragMoveCommand(spliceCommand, &touched);
                touched.insert(touched.end(), kissTouched.begin(), kissTouched.end());
                touched.push_back(pipe->getDest());
                touched.push_back(pipe->getSource());
                touched.push_back(exitGui);
                touched.push_back(entryGui);
                new InsertNodeCommand(this, pipe, exitGui, entryGui, entryInput, spliceCommand);
                pushUndoCommand(spliceCommand, touched);
            } else if (kissCommand) {
                NodesGuiList touched;
                takeDragMoveCommand(kissCommand, &touched);
                touched.insert(touched.end(), kissTouched.begin(), kissTouched.end());
                pushUndoCommand(kissCommand, touched);
            } else {
                NodesGuiList moved;
                QUndoCommand* moveCommand = takeDragMoveCommand(nullptr, &moved);
                if (moveCommand) {
                    pushUndoCommand(moveCommand, moved);
                }
            }
        }
    } else if (state == eEventStateSelectionRect) {
        _imp->editSelectionFromSelectionRectangle(modCASIsShift(e));
    } else if (state == eEventStateCutting) {
        commitCut();
    }
    // Kisses still held here were made by a drag that did not end in the move branch above.
    NodesGuiList kissTouched;
    QUndoCommand* kissCommand = takeKissCommand(&kissTouched);
    if (kissCommand) {
        pushUndoCommand(kissCommand, kissTouched);
    }
    resetSplicePlan();
    _imp->_nodesWithinBDAtPenDown.clear();
    _imp->_dragPickupPositions.clear();

    _imp->cursorSet = false;
    unsetCursor();
    update();
} // mouseReleaseEvent

void
NodeGraph::scrollViewIfNeeded(const QPointF& scenePos)
{
    QPoint widgetPos = mapFromScene(scenePos);
    QRectF visibleRect = visibleWidgetRect();
    int scrollDeltaZone = 50;
    int dx = 0, dy = 0;

    if (widgetPos.x() <= visibleRect.x() + scrollDeltaZone) {
        dx = -NATRON_GRAPH_AUTO_SCROLL_OFFSET;
    } else if (widgetPos.x() >= ( visibleRect.x() + visibleRect.width() ) - scrollDeltaZone) {
        dx = NATRON_GRAPH_AUTO_SCROLL_OFFSET;
    }
    if (widgetPos.y() <= visibleRect.y() + scrollDeltaZone) {
        dy = -NATRON_GRAPH_AUTO_SCROLL_OFFSET;
    } else if (widgetPos.y() >= ( visibleRect.y() + visibleRect.height() ) - scrollDeltaZone) {
        dy = NATRON_GRAPH_AUTO_SCROLL_OFFSET;
    }


    if ( (dx != 0) || (dy != 0) ) {
        // The step is in scene units, so it is at least one pixel however far the view is zoomed out.
        const QTransform& t = transform();
        int px = (dx == 0) ? 0 : (dx > 0 ? 1 : -1) * std::max(1, qRound(std::abs(dx * t.m11())));
        int py = (dy == 0) ? 0 : (dy > 0 ? 1 : -1) * std::max(1, qRound(std::abs(dy * t.m22())));
        panContentBy(QPoint(-px, -py));
        if (_imp->_evtState == eEventStateSelectionRect) {
            // The band's far corner stays under the cursor, which the scroll moved over the scene.
            const QPointF cur = mapToScene(mapFromGlobal(QCursor::pos()));
            _imp->_selectionRect = QRectF(_imp->_lastSelectionStartPointScene, cur).normalized();
            viewport()->update();
        }
    } else {
        _imp->autoScrollTimer.stop();
    }
}

void
NodeGraph::checkAndStartAutoScrollTimer(const QPointF& scenePos)
{
    if ( _imp->autoScrollTimer.isActive() ) {
        return;
    }
    QPoint widgetPos = mapFromScene(scenePos);
    QRectF visibleRect = visibleWidgetRect();
    int scrollDeltaZone = 50;
    bool startScrolling = false;
    if (widgetPos.x() <= visibleRect.x() + scrollDeltaZone) {
        startScrolling = true;
    } else if (widgetPos.x() >= ( visibleRect.x() + visibleRect.width() ) - scrollDeltaZone) {
        startScrolling = true;
    }
    if (widgetPos.y() <= visibleRect.y() + scrollDeltaZone) {
        startScrolling = true;
    } else if (widgetPos.y() >= ( visibleRect.y() + visibleRect.height() ) - scrollDeltaZone) {
        startScrolling = true;
    }

    if (startScrolling) {
        _imp->autoScrollTimer.start(NATRON_AUTO_SCROLL_TIMEOUT_MS);
    }
}

void
NodeGraph::onAutoScrollTimerTriggered()
{
    bool isAutoScrollEnabled = appPTR->getCurrentSettings()->isAutoScrollEnabled();

    if (isAutoScrollEnabled) {
        QPointF cursorPos = mapToScene( mapFromGlobal( QCursor::pos() ) );
        scrollViewIfNeeded(cursorPos);
    }
}

NATRON_NAMESPACE_EXIT
