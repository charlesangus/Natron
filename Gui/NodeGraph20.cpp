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
#include <cmath> // abs
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

GCC_DIAG_UNUSED_PRIVATE_FIELD_OFF
CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QMouseEvent>
#include <QPolygonF>
#include <QToolTip>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)
GCC_DIAG_UNUSED_PRIVATE_FIELD_ON

#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Settings.h"
#include "Engine/Utils.h" // convertFromPlainText

#include "Gui/BackdropGui.h"
#include "Gui/Edge.h"
#include "Gui/EdgeGeometry.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/GuiMacros.h"
#include "Gui/NodeGraphKiss.h"
#include "Gui/NodeGraphSplice.h"
#include "Gui/NodeGui.h"

#include "Global/QtCompat.h"

NATRON_NAMESPACE_ENTER

namespace {

const double kSpliceRectTolerance = 10;

bool
segmentTouchesRect(const QLineF& segment,
                   const QRectF& rect)
{
    if (rect.contains(segment.p1()) || rect.contains(segment.p2())) {
        return true;
    }
    const QLineF sides[4] = {
        QLineF(rect.topLeft(), rect.topRight()),
        QLineF(rect.topRight(), rect.bottomRight()),
        QLineF(rect.bottomRight(), rect.bottomLeft()),
        QLineF(rect.bottomLeft(), rect.topLeft())
    };
    QPointF intersection;
    for (int i = 0; i < 4; ++i) {
        if (segment.intersects(sides[i], &intersection) == QLineF::BoundedIntersection) {
            return true;
        }
    }

    return false;
}

QPointF
closestPointOnSegment(const QPointF& p,
                      const QLineF& segment)
{
    const double dx = segment.dx();
    const double dy = segment.dy();
    const double lengthSq = dx * dx + dy * dy;
    double t = 0.;
    if (lengthSq > 0.) {
        t = ((p.x() - segment.x1()) * dx + (p.y() - segment.y1()) * dy) / lengthSq;
        t = std::max(0., std::min(1., t));
    }

    return QPointF(segment.x1() + t * dx, segment.y1() + t * dy);
}

double
squaredDistanceToSegment(const QPointF& p,
                         const QLineF& segment)
{
    const QPointF closest = closestPointOnSegment(p, segment);
    const double cx = closest.x() - p.x();
    const double cy = closest.y() - p.y();

    return cx * cx + cy * cy;
}

// The pipe is already connected, so canConnectInput() on its destination stops at
// eCanConnectInput_inputAlreadyConnected before checking cycles or data kinds; those are
// checked separately here for the exit that would replace the pipe's source.
bool
canSpliceIntoPipe(const NodePtr& destNode,
                  int pipeInput,
                  const NodePtr& srcNode,
                  const NodePtr& entryNode,
                  int entryInput,
                  const NodePtr& exitNode)
{
    Node::CanConnectInputReturnValue destRet = destNode->canConnectInput(exitNode, pipeInput);
    if ((destRet != Node::eCanConnectInput_ok) && (destRet != Node::eCanConnectInput_inputAlreadyConnected)) {
        return false;
    }
    if (!destNode->checkIfConnectingInputIsOk(exitNode.get())) {
        return false;
    }
    if (destNode->checkDataKindCompatibility(exitNode, pipeInput) != Node::eCanConnectInput_ok) {
        return false;
    }

    return entryNode->canConnectInput(srcNode, entryInput) == Node::eCanConnectInput_ok;
}

} // anonymous namespace

void
NodeGraph::buildSplicePlan()
{
    _imp->_splicePlanBuilt = true;
    _imp->_splicePlanSelection = _imp->_selection;
    _imp->_spliceEntry.reset();
    _imp->_spliceEntryInput = -1;
    _imp->_spliceExit.reset();

    SplicePlan plan = planSpliceForSelection(_imp->_selection);
    if (!plan.valid) {
        return;
    }

    NodeGuiPtr entryGui, exitGui;
    for (NodesGuiList::const_iterator it = _imp->_selection.begin(); it != _imp->_selection.end(); ++it) {
        NodePtr node = (*it)->getNode();
        if (!node) {
            continue;
        }
        SpliceNodeId id = reinterpret_cast<SpliceNodeId>(node.get());
        if (id == plan.entryId) {
            entryGui = *it;
        }
        if (id == plan.exitId) {
            exitGui = *it;
        }
    }
    if (!entryGui || !exitGui || !entryGui->getInputArrow(plan.entryInput)) {
        return;
    }

    _imp->_spliceEntry = entryGui;
    _imp->_spliceEntryInput = plan.entryInput;
    _imp->_spliceExit = exitGui;
}

void
NodeGraph::clearSpliceHint()
{
    if (!_imp->_splicePipe) {
        return;
    }
    _imp->_splicePipe->setUseHighlight(false);
    _imp->_splicePipe = 0;
    _imp->_hintInputEdge->hide();
    _imp->_hintOutputEdge->hide();
}

void
NodeGraph::resetSplicePlan()
{
    clearSpliceHint();
    _imp->_splicePlanBuilt = false;
    _imp->_splicePlanSelection.clear();
    _imp->_spliceEntry.reset();
    _imp->_spliceEntryInput = -1;
    _imp->_spliceExit.reset();
}

void
NodeGraph::cancelSplice()
{
    resetSplicePlan();
    _imp->_spliceCancelled = true;
}

void
NodeGraph::checkForSpliceHint()
{
    if (_imp->_spliceCancelled) {
        return;
    }

    if (!_imp->_splicePlanBuilt || (_imp->_splicePlanSelection != _imp->_selection)) {
        clearSpliceHint();
        buildSplicePlan();
    }

    if (!_imp->_spliceEntry || !_imp->_spliceExit) {
        clearSpliceHint();

        return;
    }

    NodePtr entryNode = _imp->_spliceEntry->getNode();
    NodePtr exitNode = _imp->_spliceExit->getNode();
    if (!entryNode || !exitNode) {
        clearSpliceHint();

        return;
    }

    std::set<NodeGui*> selectedNodes;
    std::vector<QRectF> selectedRects;
    QRectF selectionBody;
    for (NodesGuiList::const_iterator it = _imp->_selection.begin(); it != _imp->_selection.end(); ++it) {
        selectedNodes.insert(it->get());
        QRectF r = (*it)->boundingRectWithEdges();
        r.adjust(-kSpliceRectTolerance, -kSpliceRectTolerance, kSpliceRectTolerance, kSpliceRectTolerance);
        selectedRects.push_back(r);
        selectionBody = selectionBody.united((*it)->mapToScene((*it)->boundingRect()).boundingRect());
    }
    const QPointF selectionCentre = selectionBody.center();

    std::set<NodeGui*> nodesWithinRect;
    getNodesWithinViewportRect(visibleWidgetRect(), &nodesWithinRect);

    Edge* bestPipe = 0;
    double bestDistSq = 0.;
    for (std::set<NodeGui*>::const_iterator it = nodesWithinRect.begin(); it != nodesWithinRect.end(); ++it) {
        NodeGui* destGui = *it;
        if ((selectedNodes.find(destGui) != selectedNodes.end()) || !destGui->isVisible()) {
            continue;
        }
        NodePtr destNode = destGui->getNode();
        if (!destNode) {
            continue;
        }
        EffectInstancePtr destEffect = destNode->getEffectInstance();

        const std::vector<Edge*>& pipes = destGui->getInputsArrows();
        for (std::size_t i = 0; i < pipes.size(); ++i) {
            Edge* pipe = pipes[i];
            if (!pipe || !pipe->isVisible() || !pipe->hasSource()) {
                continue;
            }
            NodeGuiPtr srcGui = pipe->getSource();
            if (!srcGui || (selectedNodes.find(srcGui.get()) != selectedNodes.end())) {
                continue;
            }
            if (destEffect && destEffect->isInputRotoBrush(pipe->getInputNumber())) {
                continue;
            }

            QLineF segment = pipe->line();
            segment.setP1(pipe->mapToScene(segment.p1()));
            segment.setP2(pipe->mapToScene(segment.p2()));

            bool touches = false;
            for (std::size_t r = 0; r < selectedRects.size(); ++r) {
                if (segmentTouchesRect(segment, selectedRects[r])) {
                    touches = true;
                    break;
                }
            }
            if (!touches) {
                continue;
            }

            const double distSq = squaredDistanceToSegment(selectionCentre, segment);
            if (bestPipe && (distSq >= bestDistSq)) {
                continue;
            }

            NodePtr srcNode = srcGui->getNode();
            if (!srcNode || !canSpliceIntoPipe(destNode, pipe->getInputNumber(), srcNode, entryNode, _imp->_spliceEntryInput, exitNode)) {
                continue;
            }

            bestPipe = pipe;
            bestDistSq = distSq;
        }
    }

    if (!bestPipe) {
        clearSpliceHint();

        return;
    }

    if (bestPipe == _imp->_splicePipe) {
        _imp->_hintInputEdge->initLine();
        _imp->_hintOutputEdge->initLine();

        return;
    }

    clearSpliceHint();
    _imp->_splicePipe = bestPipe;
    bestPipe->setUseHighlight(true);

    _imp->_hintInputEdge->setDefaultColor(Edge::highlightColor(100));
    _imp->_hintOutputEdge->setDefaultColor(Edge::highlightColor(100));
    _imp->_hintInputEdge->setInputNumber(_imp->_spliceEntryInput);
    _imp->_hintInputEdge->setSourceAndDestination(bestPipe->getSource(), _imp->_spliceEntry);
    _imp->_hintInputEdge->setVisible(true);

    _imp->_hintOutputEdge->setInputNumber(bestPipe->getInputNumber());
    _imp->_hintOutputEdge->setSourceAndDestination(_imp->_spliceExit, bestPipe->getDest());
    _imp->_hintOutputEdge->setVisible(true);
} // NodeGraph::checkForSpliceHint

namespace {

bool
kissConnectionIsOk(Node::CanConnectInputReturnValue ret)
{
    return (ret == Node::eCanConnectInput_ok) || (ret == Node::eCanConnectInput_differentPars) || (ret == Node::eCanConnectInput_differentFPS);
}

} // anonymous namespace

void
NodeGraph::setKissAiming(bool aiming)
{
    if (_imp->_evtState != eEventStateDraggingNode) {
        return;
    }
    if (aiming) {
        _imp->_kissAiming = true;
        clearSpliceHint();
        updateKissPreview();
    } else {
        if (_imp->_kissAiming) {
            commitKiss();
        }
        checkForSpliceHint();
    }
}

void
NodeGraph::clearKissPreview()
{
    _imp->_kissDragged.reset();
    _imp->_kissTarget.reset();
    _imp->_kissInput = -1;
    _imp->_kissFromOutput = false;
    if (_imp->_kissPreviewEdge && _imp->_kissPreviewEdge->isVisible()) {
        _imp->_kissPreviewEdge->hide();
    }
}

void
NodeGraph::updateKissPreview()
{
    if (_imp->_kissCancelled || !_imp->_kissAiming || !_imp->_hasMovedOnce || _imp->_selection.empty()) {
        clearKissPreview();

        return;
    }

    std::map<KissNodeId, NodeGuiPtr> nodesById;
    std::set<NodeGui*> selectedNodes;
    std::set<NodeGui*> feedingNodes;
    for (NodesGuiList::const_iterator it = _imp->_selection.begin(); it != _imp->_selection.end(); ++it) {
        selectedNodes.insert(it->get());
        const std::vector<Edge*>& arrows = (*it)->getInputsArrows();
        for (std::size_t i = 0; i < arrows.size(); ++i) {
            if (arrows[i] && arrows[i]->hasSource()) {
                feedingNodes.insert(arrows[i]->getSource().get());
            }
        }
    }

    std::vector<KissInputTip> inputs;
    std::vector<KissOutput> outputs;
    // A QRectF of one point is null, and uniting with a null rect drops it, so the reach area is
    // the bounding rect of the points.
    QPolygonF reachPoints;
    for (NodesGuiList::const_iterator it = _imp->_selection.begin(); it != _imp->_selection.end(); ++it) {
        const NodeGuiPtr& gui = *it;
        NodePtr node = gui->getNode();
        if (!node || dynamic_cast<BackdropGui*>(gui.get())) {
            continue;
        }
        const KissNodeId id = reinterpret_cast<KissNodeId>(gui.get());
        nodesById[id] = gui;
        EffectInstancePtr effect = node->getEffectInstance();

        const std::vector<Edge*>& arrows = gui->getInputsArrows();
        for (std::size_t i = 0; i < arrows.size(); ++i) {
            Edge* arrow = arrows[i];
            if (!arrow || !arrow->isVisible() || arrow->hasSource()) {
                continue;
            }
            if (effect && effect->isInputRotoBrush(arrow->getInputNumber())) {
                continue;
            }
            const QPointF tip = arrow->mapToScene(arrow->line().p2());
            inputs.push_back(KissInputTip(id, arrow->getInputNumber(), tip.x(), tip.y()));
            reachPoints << tip;
        }

        if (feedingNodes.find(gui.get()) == feedingNodes.end()) {
            const QRectF body = gui->mapToScene(gui->boundingRect()).boundingRect();
            const QPointF output(body.center().x(), body.bottom());
            outputs.push_back(KissOutput(id, output.x(), output.y()));
            reachPoints << output;
        }
    }
    if (inputs.empty() && outputs.empty()) {
        clearKissPreview();

        return;
    }

    // Like an unconnected input's grab radius, the reach is kept in viewport pixels.
    const double zoomFactor = transform().mapRect(QRectF(0, 0, 1, 1)).width();
    const double reach = TO_DPIX(18.) / zoomFactor;
    const QRectF reachArea = reachPoints.boundingRect().adjusted(-reach, -reach, reach, reach);

    std::set<NodeGui*> nearbyNodes;
    getNodesWithinViewportRect(mapFromScene(reachArea).boundingRect().adjusted(-1, -1, 1, 1), &nearbyNodes);

    std::vector<KissTarget> targets;
    for (std::set<NodeGui*>::const_iterator it = nearbyNodes.begin(); it != nearbyNodes.end(); ++it) {
        NodeGui* gui = *it;
        if ((selectedNodes.find(gui) != selectedNodes.end()) || !gui->isVisible() || dynamic_cast<BackdropGui*>(gui)) {
            continue;
        }
        NodePtr node = gui->getNode();
        if (!node) {
            continue;
        }
        int freeInput = node->getPreferredInputForConnection();
        if ((freeInput >= 0) && !gui->getInputArrow(freeInput)) {
            freeInput = -1;
        }
        const QRectF body = gui->mapToScene(gui->boundingRect()).boundingRect();
        const KissNodeId id = reinterpret_cast<KissNodeId>(gui);
        nodesById[id] = gui->shared_from_this();
        targets.push_back(KissTarget(id, body.left(), body.top(), body.right(), body.bottom(), freeInput));
    }

    KissPlan plan = planKiss(inputs, outputs, targets, reach, [&nodesById](const KissPlan& p) {
        NodePtr dragged = nodesById[p.draggedId]->getNode();
        NodePtr target = nodesById[p.targetId]->getNode();
        if (!dragged || !target) {
            return false;
        }
        if (p.end == eKissEndInput) {
            return kissConnectionIsOk(dragged->canConnectInput(target, p.inputNb));
        }

        return kissConnectionIsOk(target->canConnectInput(dragged, p.inputNb));
    });
    if (!plan.isValid()) {
        clearKissPreview();

        return;
    }

    NodeGuiPtr dragged = nodesById[plan.draggedId];
    NodeGuiPtr target = nodesById[plan.targetId];
    const bool fromOutput = (plan.end == eKissEndOutput);
    Edge* preview = _imp->_kissPreviewEdge;
    const bool unchanged = (_imp->_kissDragged == dragged) && (_imp->_kissTarget == target) && (_imp->_kissInput == plan.inputNb) && (_imp->_kissFromOutput == fromOutput) && preview->isVisible();
    if (unchanged) {
        preview->initLine();

        return;
    }

    _imp->_kissDragged = dragged;
    _imp->_kissTarget = target;
    _imp->_kissInput = plan.inputNb;
    _imp->_kissFromOutput = fromOutput;
    preview->setInputNumber(plan.inputNb);
    if (fromOutput) {
        preview->setSourceAndDestination(dragged, target);
    } else {
        preview->setSourceAndDestination(target, dragged);
    }
    preview->setVisible(true);
} // NodeGraph::updateKissPreview

void
NodeGraph::commitKiss()
{
    _imp->_kissAiming = false;
    NodeGuiPtr dragged = _imp->_kissDragged;
    NodeGuiPtr target = _imp->_kissTarget;
    const int inputNb = _imp->_kissInput;
    const bool fromOutput = _imp->_kissFromOutput;
    clearKissPreview();
    if (!dragged || !target || _imp->_kissCancelled) {
        return;
    }

    NodeGuiPtr dest = fromOutput ? target : dragged;
    NodeGuiPtr source = fromOutput ? dragged : target;
    Edge* arrow = dest->getInputArrow(inputNb);
    if (!arrow || arrow->hasSource()) {
        return;
    }

    if (!_imp->_kissCommand) {
        _imp->_kissCommand = new QUndoCommand(tr("Connect nodes"));
    }
    KissConnectCommand* connect = new KissConnectCommand(this, arrow, NodeGuiPtr(), source, _imp->_kissCommand);
    connect->applyNow();
    _imp->_kissTouched.push_back(dest);
    _imp->_kissTouched.push_back(source);

    // The splice plan is built once per drag on the assumption that the selection's connections
    // don't change during it.
    resetSplicePlan();
    scene()->update();
    update();
}

void
NodeGraph::rollBackKisses()
{
    _imp->_kissAiming = false;
    clearKissPreview();
    if (!_imp->_kissCommand) {
        return;
    }
    _imp->_kissCommand->undo();
    delete _imp->_kissCommand;
    _imp->_kissCommand = nullptr;
    _imp->_kissTouched.clear();
    resetSplicePlan();
    scene()->update();
    update();
}

QUndoCommand*
NodeGraph::takeKissCommand(NodesGuiList* touched)
{
    _imp->_kissAiming = false;
    clearKissPreview();
    QUndoCommand* command = _imp->_kissCommand;
    _imp->_kissCommand = nullptr;
    if (touched) {
        touched->insert(touched->end(), _imp->_kissTouched.begin(), _imp->_kissTouched.end());
    }
    _imp->_kissTouched.clear();

    return command;
}

void
NodeGraph::disconnectShakenNodes(const std::set<NodeGuiPtr>& shaken)
{
    clearKissPreview();

    // Each pipe between a shaken node and one left behind, by its destination's input.
    std::set<std::pair<NodeGuiPtr, int>> cuts;
    for (std::set<NodeGuiPtr>::const_iterator it = shaken.begin(); it != shaken.end(); ++it) {
        const NodeGuiPtr& gui = *it;
        NodePtr node = gui->getNode();
        if (!node || dynamic_cast<BackdropGui*>(gui.get())) {
            continue;
        }
        const std::vector<Edge*>& arrows = gui->getInputsArrows();
        for (std::size_t i = 0; i < arrows.size(); ++i) {
            if (arrows[i] && arrows[i]->hasSource() && (shaken.find(arrows[i]->getSource()) == shaken.end())) {
                cuts.insert(std::make_pair(gui, arrows[i]->getInputNumber()));
            }
        }
        const NodesWList outputs = node->getGuiOutputs();
        for (NodesWList::const_iterator it2 = outputs.begin(); it2 != outputs.end(); ++it2) {
            NodePtr output = it2->lock();
            NodeGuiPtr outputGui = output ? std::dynamic_pointer_cast<NodeGui>(output->getNodeGui()) : NodeGuiPtr();
            if (!outputGui || (outputGui->getDagGui() != this) || (shaken.find(outputGui) != shaken.end())) {
                continue;
            }
            const std::vector<Edge*>& outputArrows = outputGui->getInputsArrows();
            for (std::size_t i = 0; i < outputArrows.size(); ++i) {
                if (outputArrows[i] && (outputArrows[i]->getSource() == gui)) {
                    cuts.insert(std::make_pair(outputGui, outputArrows[i]->getInputNumber()));
                }
            }
        }
    }
    if (cuts.empty()) {
        return;
    }

    if (!_imp->_kissCommand) {
        _imp->_kissCommand = new QUndoCommand(tr("Disconnect nodes"));
    }
    for (std::set<std::pair<NodeGuiPtr, int>>::const_iterator it = cuts.begin(); it != cuts.end(); ++it) {
        Edge* arrow = it->first->getInputArrow(it->second);
        if (!arrow || !arrow->hasSource()) {
            continue;
        }
        NodeGuiPtr source = arrow->getSource();
        KissConnectCommand* disconnect = new KissConnectCommand(this, arrow, source, NodeGuiPtr(), _imp->_kissCommand);
        disconnect->applyNow();
        _imp->_kissTouched.push_back(it->first);
        _imp->_kissTouched.push_back(source);
    }

    resetSplicePlan();
    scene()->update();
    update();
} // NodeGraph::disconnectShakenNodes

void
NodeGraph::extendCutLine(const QPointF& scenePos)
{
    const QPointF point = _imp->_root->mapFromScene(scenePos);
    if (_imp->_cutLine.empty()) {
        _imp->_cutLine.push_back(point);

        return;
    }
    const QLineF step(_imp->_root->mapToScene(_imp->_cutLine.back()), scenePos);
    _imp->_cutLine.push_back(point);

    // The pipes keep still for the whole cut, so only the newest step of the line can cross more.
    for (NodesGuiList::const_iterator it = _imp->_nodes.begin(); it != _imp->_nodes.end(); ++it) {
        const NodeGuiPtr& destGui = *it;
        if (!destGui->isVisible()) {
            continue;
        }
        NodePtr destNode = destGui->getNode();
        if (!destNode) {
            continue;
        }
        EffectInstancePtr destEffect = destNode->getEffectInstance();
        const std::vector<Edge*>& pipes = destGui->getInputsArrows();
        for (std::size_t i = 0; i < pipes.size(); ++i) {
            Edge* pipe = pipes[i];
            if (!pipe || !pipe->isVisible() || !pipe->hasSource() || pipe->isOutputEdge()) {
                continue;
            }
            if (destEffect && destEffect->isInputRotoBrush(pipe->getInputNumber())) {
                continue;
            }
            QLineF segment = pipe->line();
            segment.setP1(pipe->mapToScene(segment.p1()));
            segment.setP2(pipe->mapToScene(segment.p2()));
            if (!segmentsIntersect(step, segment)) {
                continue;
            }
            bool known = false;
            for (std::size_t c = 0; c < _imp->_cutPipes.size(); ++c) {
                if ((_imp->_cutPipes[c].first.lock() == destGui) && (_imp->_cutPipes[c].second == pipe->getInputNumber())) {
                    known = true;
                    break;
                }
            }
            if (!known) {
                _imp->_cutPipes.push_back(std::make_pair(NodeGuiWPtr(destGui), pipe->getInputNumber()));
                pipe->setUseHighlight(true);
            }
        }
    }
} // NodeGraph::extendCutLine

void
NodeGraph::clearCutLine()
{
    for (std::size_t i = 0; i < _imp->_cutPipes.size(); ++i) {
        NodeGuiPtr dest = _imp->_cutPipes[i].first.lock();
        Edge* pipe = dest ? dest->getInputArrow(_imp->_cutPipes[i].second) : 0;
        if (pipe) {
            pipe->setUseHighlight(false);
        }
    }
    _imp->_cutPipes.clear();
    _imp->_cutLine.clear();
}

void
NodeGraph::commitCut()
{
    const std::vector<std::pair<NodeGuiWPtr, int>> cutPipes = _imp->_cutPipes;
    clearCutLine();

    QUndoCommand* cut = 0;
    NodesGuiList touched;
    for (std::size_t i = 0; i < cutPipes.size(); ++i) {
        NodeGuiPtr dest = cutPipes[i].first.lock();
        Edge* pipe = dest ? dest->getInputArrow(cutPipes[i].second) : 0;
        if (!pipe || !pipe->hasSource()) {
            continue;
        }
        if (!cut) {
            cut = new QUndoCommand(tr("Cut pipes"));
        }
        NodeGuiPtr source = pipe->getSource();
        new ConnectCommand(this, pipe, source, NodeGuiPtr(), cut);
        touched.push_back(dest);
        touched.push_back(source);
    }
    if (cut) {
        pushUndoCommand(cut, touched);
    }
    scene()->update();
    update();
}

void
NodeGraph::moveSelectedNodesBy(bool /*shiftdown*/,
                               bool controlDown,
                               const QPointF& lastMousePosScene,
                               const QPointF& newPos,
                               const QRectF& /*visibleSceneR*/,
                               bool userEdit)
{
    if ( _imp->_selection.empty() ) {
        return;
    }


    //Get the nodes to move, taking into account the backdrops
    bool ignoreMagnet = false;
    std::set<NodeGuiPtr> nodesToMove;
    for (NodesGuiList::iterator it = _imp->_selection.begin();
         it != _imp->_selection.end(); ++it) {
        const NodeGuiPtr& node = *it;
        nodesToMove.insert(node);

        std::map<NodeGuiPtr, NodesGuiList>::iterator foundBd = _imp->_nodesWithinBDAtPenDown.find(*it);
        if ( !controlDown && ( foundBd != _imp->_nodesWithinBDAtPenDown.end() ) ) {
            ignoreMagnet = true; // we move a backdrop, ignore magnet
            for (NodesGuiList::iterator it2 = foundBd->second.begin();
                 it2 != foundBd->second.end(); ++it2) {
                ///add it only if it's not already in the list
                nodesToMove.insert(*it2);

            }
        }
    }

    if (!ignoreMagnet && nodesToMove.size() > 1) {
        ignoreMagnet = true;
    }

    //The delta
    double dxScene = newPos.x() - lastMousePosScene.x();
    double dyScene = newPos.y() - lastMousePosScene.y();


    //Move all nodes
    bool deltaSet = false;
    for (std::set<NodeGuiPtr>::iterator it = nodesToMove.begin();
         it != nodesToMove.end(); ++it) {
        //The current position
        QPointF pos = (*it)->getPos_mt_safe();

        //if ignoreMagnet == true, we do not snap nodes to horizontal/vertical positions
        (*it)->refreshPosition(pos.x() + dxScene, pos.y() + dyScene, ignoreMagnet, newPos);

        //The new position
        QPointF newNodePos = (*it)->getPos_mt_safe();
        if (!ignoreMagnet) {
            //Magnet only works when selection is only for a single node
            //Adjust the delta since mouse press by the new position after snapping
            assert(nodesToMove.size() == 1);
            _imp->_deltaSinceMousePress.rx() += newNodePos.x() - pos.x();
            _imp->_deltaSinceMousePress.ry() += newNodePos.y() - pos.y();
            deltaSet = true;
        }
    }

    if (!deltaSet) {
        _imp->_deltaSinceMousePress.rx() += dxScene;
        _imp->_deltaSinceMousePress.ry() += dyScene;
    }

    if (!userEdit) {
        return;
    }

    //Start auto-scolling if nearby the edges
    checkAndStartAutoScrollTimer(newPos);

    // A move without Ctrl after one with it means Ctrl was let go, even if its key release went
    // to another widget.
    setKissAiming(controlDown);

    if (_imp->_shakeDetector.addSample(_imp->_dragClock.elapsed(), newPos.x(), newPos.y()) && !_imp->_kissCancelled) {
        disconnectShakenNodes(nodesToMove);
    }
} // NodeGraph::moveSelectedNodesBy

void
NodeGraph::mouseMoveEvent(QMouseEvent* e)
{
    QPointF newPos = mapToScene( e->pos() );
    QPointF lastMousePosScene = mapToScene( _imp->_lastMousePos.x(), _imp->_lastMousePos.y() );
    double dx, dy;
    {
        QPointF newPosRoot = _imp->_root->mapFromScene(newPos);
        QPointF lastMousePosRoot = _imp->_root->mapFromScene(lastMousePosScene);
        dx = newPosRoot.x() - lastMousePosRoot.x();
        dy = newPosRoot.y() - lastMousePosRoot.y();
    }

    _imp->_hasMovedOnce = true;

    bool mustUpdate = true;
    NodeCollectionPtr collection = getGroup();
    NodeGroup* isGroup = dynamic_cast<NodeGroup*>( collection.get() );
    bool isGroupEditable = true;
    bool groupEdited = true;
    if (isGroup) {
        isGroupEditable = isGroup->isSubGraphEditable();
        groupEdited = isGroup->getNode()->hasPyPlugBeenEdited();
    }
    if (!groupEdited && isGroupEditable) {
        ///check if user is nearby unlock
        // see NodeGraph::paintEvent()
        QPoint pixPos = _imp->getPyPlugUnlockPos();
        int pixW = qRound(_imp->unlockIcon.deviceIndependentSize().width());
        int pixH = qRound(_imp->unlockIcon.deviceIndependentSize().height());
        QRect pixRect(pixPos.x(), pixPos.y(), pixW, pixH);
        pixRect.adjust(-2, -2, 2, 2);
        QRect selRect = pixRect;
        selRect.adjust(-3, -3, 3, 3);
        if ( selRect.contains( e->pos() ) ) {
            assert(isGroup);
            QPoint pos = mapToGlobal( e->pos() );
            // Unfortunately, the timeout delay for the tooltip is hardcoded in Qt 4, and the last parameter to showText doesn't seem to influence anything
            // Can not fix https://github.com/MrKepzie/Natron/issues/1151 (at least in Qt4)
            QToolTip::showText( pos, NATRON_NAMESPACE::convertFromPlainText(QCoreApplication::translate("NodeGraph", "Clicking the unlock button will convert the PyPlug to a regular group saved in the project and detach it from the script.\n"
                                                                                                "Any modification will not be written to the Python script. Subsequent loading of the project will no longer load this group from the python script."), NATRON_NAMESPACE::WhiteSpaceNormal),
                               this, selRect);
        }
    }

    QRectF sceneR = visibleSceneRect();

    bool mustUpdateNavigator = false;
    ///Apply actions
    switch (_imp->_evtState) {
    case eEventStateDraggingArrow: {
        QPointF np = _imp->_arrowSelected->mapFromScene(newPos);
        if ( _imp->_arrowSelected->isOutputEdge() ) {
            _imp->_arrowSelected->dragDest(np);
        } else {
            _imp->_arrowSelected->dragSource(np);
        }
        checkAndStartAutoScrollTimer(newPos);
        mustUpdate = true;
        if (_imp->cursorSet) {
            _imp->cursorSet = false;
            unsetCursor();
        }
        break;
    }
    case eEventStateDraggingNode: {
        mustUpdate = true;
        mustUpdateNavigator = true;
        bool controlDown = modifierHasControl(e);
        bool shiftdown = modifierHasShift(e);
        moveSelectedNodesBy(shiftdown, controlDown, lastMousePosScene, newPos, sceneR, true);
        _imp->cursorSet = true;
        setCursor( QCursor(Qt::ClosedHandCursor) );
        break;
    }
    case eEventStateMovingArea: {
        mustUpdateNavigator = true;
        moveRootInternal(dx, dy);
        mustUpdate = true;
        _imp->cursorSet = true;
        setCursor( QCursor(Qt::SizeAllCursor) );
        break;
    }
    case eEventStateResizingBackdrop: {
        mustUpdateNavigator = true;
        assert(_imp->_backdropResized);
        QPointF p = _imp->_backdropResized->scenePos();
        int w = newPos.x() - p.x();
        int h = newPos.y() - p.y();
        checkAndStartAutoScrollTimer(newPos);
        mustUpdate = true;
        ResizeBackdropCommand* cmd = new ResizeBackdropCommand(_imp->_backdropResized, w, h);
        cmd->setGestureKey(_imp->_dragGesture.key());
        pushUndoCommand(cmd, NodesGuiList(1, _imp->_backdropResized));
        _imp->cursorSet = true;
        setCursor( QCursor(Qt::SizeFDiagCursor) );
        break;
    }
    case eEventStateCutting: {
        extendCutLine(newPos);
        mustUpdate = true;
        break;
    }
    case eEventStateSelectionRect: {
        QPointF startDrag = _imp->_lastSelectionStartPointScene;
        QPointF cur = newPos;
        double xmin = std::min( cur.x(), startDrag.x() );
        double xmax = std::max( cur.x(), startDrag.x() );
        double ymin = std::min( cur.y(), startDrag.y() );
        double ymax = std::max( cur.y(), startDrag.y() );
        checkAndStartAutoScrollTimer(newPos);
        QRectF selRect(xmin, ymin, xmax - xmin, ymax - ymin);
        _imp->_selectionRect = selRect;
        mustUpdate = true;
        _imp->cursorSet = true;
        setCursor( QCursor(Qt::CrossCursor) );
        break;
    }
    case eEventStateDraggingNavigator: {
        _imp->cursorSet = true;
        setCursor( QCursor(Qt::ClosedHandCursor) );
        QPointF mousePosSceneCoordinates;
        bool insideNavigator = isNearbyNavigator(e->pos(), mousePosSceneCoordinates);
        if (insideNavigator) {
            _imp->_refreshOverlays = true;
            centerOn(mousePosSceneCoordinates);
            _imp->_lastMousePos = e->pos();
            dragPickedUpArrowToCursor();
            update();

            return;
        }
        break;
    }
    case eEventStateZoomingArea: {
        int delta = 2 * ( ( e->position().x() - _imp->_lastMousePos.x() ) - ( e->position().y() - _imp->_lastMousePos.y() ) );
        setTransformationAnchor(QGraphicsView::AnchorViewCenter);
        wheelEventInternal(modCASIsControl(e), delta);
        setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
        mustUpdate = true;
        _imp->cursorSet = true;
        setCursor( QCursor(Qt::SizeAllCursor) );
        break;
    }
    case eEventStateNone:
    default: {
        mustUpdate = false;
        // Test if mouse is inside the navigator
        QPointF mousePosSceneCoordinates;
        bool insideNavigator = isNearbyNavigator(e->pos(), mousePosSceneCoordinates);
        if (insideNavigator) {
            _imp->cursorSet = true;
            setCursor( QCursor(Qt::OpenHandCursor) );
        } else if (!groupEdited) {
            if (_imp->cursorSet) {
                _imp->cursorSet = false;
                unsetCursor();
            }
        } else {
            // Set cursor
            // The cursor should clearly indicate when will happen if mouse is pressed
            NodeGui* nearbyNode = NULL;
            Edge* nearbyEdge = NULL;
            NearbyItemEnum nearbyItemCode = hasItemNearbyMouse(e->pos(), &nearbyNode, &nearbyEdge);

            switch (nearbyItemCode) {
            case eNearbyItemNode:
            case eNearbyItemBackdropFrame:
            case eNearbyItemEdgeBendPoint: {
                _imp->cursorSet = true;
                setCursor( QCursor(Qt::OpenHandCursor) );
                break;
            }
            case eNearbyItemBackdropResizeHandle: {
                _imp->cursorSet = true;
                setCursor( QCursor(Qt::SizeFDiagCursor) );
                break;
            }
            case eNearbyItemNone: {
                _imp->cursorSet = true;
                setCursor( QCursor(Qt::CrossCursor) );
                break;
            }
            case eNearbyItemNodeEdge:
            default: {
                if (_imp->cursorSet) {
                    _imp->cursorSet = false;
                    unsetCursor();
                }
                break;
            }
            }
        }
        break;
    }
    } // switch


    _imp->_lastMousePos = e->pos();

    // Runs after the switch so that a pan or zoom done by this same move is already applied.
    dragPickedUpArrowToCursor();

    if (mustUpdateNavigator) {
        _imp->_refreshOverlays = true;
        mustUpdate = true;
    }

    if (mustUpdate) {
        update();
    }
    QGraphicsView::mouseMoveEvent(e);
} // mouseMoveEvent

NATRON_NAMESPACE_EXIT

