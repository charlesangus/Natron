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

#include "RadialLayout.h"

#include <cassert>
#include <cmath>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QGraphicsItem>
#include <QPolygonF>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Global/GlobalDefines.h"

#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/NodeSpec.h"

#include "Gui/Edge.h"
#include "Gui/NodeGui.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846264338327950288 /* pi             */
#define M_PI_2 1.57079632679489661923132169163975144 /* pi/2           */
#endif

NATRON_NAMESPACE_ENTER

RadialLayout::RadialLayout(NodeGui* node)
    : ConnectorLayout(node)
    , _inputAngles()
{
}

RadialLayout::~RadialLayout()
{
}

bool
RadialLayout::unfoldsInputs(const NodePtr& node)
{
    if (!node) {
        return false;
    }
    if (dynamic_cast<InspectorNode*>(node.get())) {
        return true;
    }
    NativeEffectBase* native = dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
    const NodeSpec* spec = native ? native->nodeSpec() : 0;
    if (!spec) {
        return false;
    }
    for (const InputSpec& input : spec->inputs) {
        if (input.role == InputRole::Variadic) {
            return true;
        }
    }

    return false;
}

void
RadialLayout::resetInputs(std::size_t inputCount)
{
    _inputAngles.assign(inputCount, 0.);
}

void
RadialLayout::setInputAngle(std::size_t inputNb,
                            double angle)
{
    if (inputNb >= _inputAngles.size()) {
        _inputAngles.resize(inputNb + 1, 0.);
    }
    _inputAngles[inputNb] = angle;
}

double
RadialLayout::getInputAngle(int inputNb) const
{
    if ((inputNb < 0) || (inputNb >= (int)_inputAngles.size())) {
        return 0.;
    }

    return _inputAngles[inputNb];
}

void
RadialLayout::layoutInputsForInspector()
{
    NodePtr node = _node->getNode();

    assert(node);
    if (!node) {
        return;
    }
    const std::vector<Edge*>& inputEdges = _node->getInputsArrows();
    // Count mask inputs first
    std::vector<bool> masksInputs(inputEdges.size());
    int nMasksInputs = 0;
    for (std::size_t i = 0; i < inputEdges.size(); ++i) {
        masksInputs[i] = node->getEffectInstance()->isInputMask(i);
        if (masksInputs[i]) {
            ++nMasksInputs;
        }
    }

    /// If the node is a viewer, display 1 input and another one aside and hide all others.
    /// If the node is something else (switch, merge) show 2 inputs and another one aside an hide all others.

    bool isViewer = node->isEffectViewer() != 0;
    int maxInitiallyOnTopVisibleInputs = isViewer ? 1 : 2;
    double angleBetweenInputs = M_PI / (maxInitiallyOnTopVisibleInputs + 1);
    double angle = angleBetweenInputs;

    double angleBetweenMasks = 0.;
    double maskAngle = 0.;
    if (nMasksInputs > 1) {
        angleBetweenMasks = M_PI / (double)(nMasksInputs + 1);
        maskAngle = -M_PI_2 + angleBetweenMasks;
    }

    for (std::size_t i = 0; i < inputEdges.size(); ++i) {
        if (masksInputs[i]) {
            setInputAngle(i, maskAngle);
            maskAngle += angleBetweenMasks;
        } else {
            if (((int)i < maxInitiallyOnTopVisibleInputs) && !node->getEffectInstance()->isInputFirstOnSide(i)) {
                setInputAngle(i, angle);
                angle += angleBetweenInputs;
            } else {
                setInputAngle(i, M_PI);
            }
        }

        if (!inputEdges[i]->hasSource()) {
            inputEdges[i]->initLine();
        }
    }
}

void
RadialLayout::layoutInputs()
{
    NodePtr node = _node->getNode();

    if (unfoldsInputs(node)) {
        layoutInputsForInspector();

        return;
    }

    const std::vector<Edge*>& inputEdges = _node->getInputsArrows();
    int inputsCount = 0;
    for (U32 i = 0; i < inputEdges.size(); ++i) {
        if (node && !node->getEffectInstance()->isInputMask(i) && !node->getEffectInstance()->isInputRotoBrush(i) && !node->getEffectInstance()->isInputFirstOnSide(i)) {
            ++inputsCount;
        }
    }

    double piDividedbyX = M_PI / (inputsCount + 1);
    double angle = piDividedbyX;
    int maskIndex = 0;
    for (U32 i = 0; i < inputEdges.size(); ++i) {
        if (node && !node->getEffectInstance()->isInputRotoBrush(i)) {
            double edgeAngle;
            bool incrAngle = true;
            if (node->getEffectInstance()->isInputFirstOnSide(i)) {
                edgeAngle = M_PI;
                incrAngle = false;
            } else if (node->getEffectInstance()->isInputMask(i)) {
                if (maskIndex == 0) {
                    edgeAngle = 0;
                    incrAngle = false;
                    ++maskIndex;
                } else if (maskIndex == 1) {
                    edgeAngle = M_PI;
                    incrAngle = false;
                    ++maskIndex;
                } else {
                    edgeAngle = angle;
                }
            } else {
                edgeAngle = angle;
            }
            setInputAngle(i, edgeAngle);
            if (incrAngle) {
                angle += piDividedbyX;
            }
            if (!inputEdges[i]->hasSource()) {
                inputEdges[i]->initLine();
            }
        }
    }
} // layoutInputs

ConnectorAnchor
RadialLayout::anchorAtCenter(const QGraphicsItem* space) const
{
    ConnectorAnchor anchor;

    anchor.outline = space->mapFromItem(_node, _node->outlineBoundingRect()).boundingRect();
    anchor.point = anchor.outline.center();

    return anchor;
}

ConnectorAnchor
RadialLayout::inputAnchor(int inputNb,
                          const QGraphicsItem* space) const
{
    ConnectorAnchor anchor = anchorAtCenter(space);
    double angle = getInputAngle(inputNb);

    // y points down, hence the negated sine. Negation is exact, so adding tangent.y() gives the
    // same bits as subtracting the sine.
    anchor.tangent = QPointF(std::cos(angle), -std::sin(angle));

    return anchor;
}

ConnectorAnchor
RadialLayout::outputAnchor(const std::string& /*outputName*/,
                           const QGraphicsItem* space) const
{
    ConnectorAnchor anchor = anchorAtCenter(space);

    anchor.tangent = QPointF(0., 1.);

    return anchor;
}

ConnectorHit
RadialLayout::hitTest(const QPointF& scenePos,
                      double grabRadius,
                      double unconnectedInputGrabRadius) const
{
    ConnectorHit hit;
    const std::vector<Edge*>& inputEdges = _node->getInputsArrows();

    for (std::size_t i = 0; i < inputEdges.size(); ++i) {
        const Edge* edge = inputEdges[i];
        double d2 = 0.;
        if (!edge || !edge->isVisible() || !hitEdge(edge, scenePos, grabRadius, unconnectedInputGrabRadius, &d2)) {
            continue;
        }
        if ((hit.kind == eConnectorKindNone) || (d2 < hit.dist2)) {
            hit.kind = eConnectorKindInput;
            hit.inputNb = (int)i;
            hit.dist2 = d2;
        }
    }
    const Edge* output = _node->getOutputArrow();
    double d2 = 0.;
    if (output && output->isVisible() && hitEdge(output, scenePos, grabRadius, unconnectedInputGrabRadius, &d2)) {
        if ((hit.kind == eConnectorKindNone) || (d2 < hit.dist2)) {
            hit.kind = eConnectorKindOutput;
            hit.inputNb = -1;
            hit.dist2 = d2;
        }
    }

    return hit;
}

NATRON_NAMESPACE_EXIT
