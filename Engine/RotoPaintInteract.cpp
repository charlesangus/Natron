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


#include "RotoPaintInteract.h"

#include <limits>

#include <QLineF>

#include "Engine/AppInstance.h"
#include "Engine/Bezier.h"
#include "Engine/KnobTypes.h"
#include "Engine/MergingEnum.h"
#include "Engine/Node.h"
#include "Engine/OverlaySupport.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoLayer.h"
#include "Engine/RotoPaint.h"
#include "Engine/RotoPoint.h"
#include "Engine/RotoStrokeItem.h"
#include "Engine/RotoUndoCommand.h"
#include "Engine/Transform.h"
#include "Engine/ViewerInstance.h"
#include "Global/GLIncludes.h"

NATRON_NAMESPACE_ENTER

RotoPaintPrivate::RotoPaintPrivate(RotoPaint* publicInterface,
                                   bool isPaintByDefault)
    : publicInterface(publicInterface)
    , isPaintByDefault(isPaintByDefault)
    , ui(RotoPaintInteract::create(this))
{
}

RotoPaintInteract::RotoPaintInteract(RotoPaintPrivate* p)
    : RotoCurveInteract(p->publicInterface)
    , p(p)
    , strokeBeingPaint()
    , strokeBeingPaintedTimelineFrame(0)
    , cloneOffset()
    , lastPaintToolAction()
    , lastTabletDownTriggeredEraser(false)
    , mouseCenterOnSizeChange()
{
    cloneOffset.first = cloneOffset.second = 0.;
}

// make_shared enabler (because make_shared needs access to the private constructor)
// see https://stackoverflow.com/a/20961251/2607517
struct RotoPaintInteract::MakeSharedEnabler: public RotoPaintInteract
{
    MakeSharedEnabler(RotoPaintPrivate* p) : RotoPaintInteract(p) {
    }
};


RotoPaintInteractPtr
RotoPaintInteract::create(RotoPaintPrivate* p)
{
    return std::make_shared<RotoPaintInteract::MakeSharedEnabler>(p);
}

RotoContextPtr
RotoPaintInteract::getContext()
{
    return getNode()->getRotoContext();
}

bool
RotoPaintInteract::isMultiStrokeEnabled() const
{
    KnobBoolPtr b =  multiStrokeEnabled.lock();

    if (b) {
        return b->getValue();
    } else {
        return false;
    }
}

bool
RotoPaintInteract::getRoleForGroup(const KnobGroupPtr& k,
                                   RotoRoleEnum* role) const
{
    if (RotoCurveInteract::getRoleForGroup(k, role)) {
        return true;
    }

    bool ret = true;

    if (k == paintBrushToolGroup.lock()) {
        *role = eRotoRolePaintBrush;
    } else if (k == cloneBrushToolGroup.lock()) {
        *role = eRotoRoleCloneBrush;
    } else if (k == effectBrushToolGroup.lock()) {
        *role = eRotoRoleEffectBrush;
    } else if (k == mergeBrushToolGroup.lock()) {
        *role = eRotoRoleMergeBrush;
    } else {
        ret = false;
    }

    return ret;
}

bool
RotoPaintInteract::getToolForAction(const KnobButtonPtr& k,
                                    RotoToolEnum* tool) const
{
    if (RotoCurveInteract::getToolForAction(k, tool)) {
        return true;
    }

    bool ret = true;

    if (k == brushAction.lock()) {
        *tool = eRotoToolSolidBrush;
    } else if (k == pencilAction.lock()) {
        *tool = eRotoToolOpenBezier;
    } else if (k == eraserAction.lock()) {
        *tool = eRotoToolEraserBrush;
    } else if (k == cloneAction.lock()) {
        *tool = eRotoToolClone;
    } else if (k == revealAction.lock()) {
        *tool = eRotoToolReveal;
    } else if (k == blurAction.lock()) {
        *tool = eRotoToolBlur;
    } else if (k == smearAction.lock()) {
        *tool = eRotoToolSmear;
    } else if (k == dodgeAction.lock()) {
        *tool = eRotoToolDodge;
    } else if (k == burnAction.lock()) {
        *tool = eRotoToolBurn;
    } else {
        ret = false;
    }

    return ret;
} // RotoPaintInteract::getToolForAction

bool
RotoPaintInteract::onKnobChanged(KnobI* k,
                                 double time,
                                 bool* ret)
{
    if (RotoCurveInteract::onKnobChanged(k, time, ret)) {
        return true;
    }

    *ret = true;
    if ((k == colorWheelButton.lock().get()) || (k == pressureOpacityButton.lock().get()) || (k == pressureSizeButton.lock().get()) || (k == hardnessSpinbox.lock().get()) || (k == buildUpButton.lock().get())) {
        onBreakMultiStrokeTriggered();
    } else if (k == resetCloneOffsetButton.lock().get()) {
        onBreakMultiStrokeTriggered();
        cloneOffset.first = cloneOffset.second = 0;
    } else {
        return false;
    }

    return true;
}

void
RotoPaintInteract::onRoleSelected(RotoRoleEnum role)
{
    bool isPaintRole = (role == eRotoRolePaintBrush) || (role == eRotoRoleCloneBrush) || (role == eRotoRoleMergeBrush) ||
                       (role == eRotoRoleEffectBrush);

    // RotoPaint action bar
    const KnobIWPtr paintKnobs[] = {
        colorWheelButton, compositingOperatorChoice, opacitySpinbox, pressureOpacityButton, sizeSpinbox, pressureSizeButton,
        hardnessSpinbox, pressureHardnessButton, buildUpButton, autoConnectViewerButton, effectSpinBox, timeOffsetSpinBox,
        timeOffsetModeChoice, sourceTypeChoice, resetCloneOffsetButton, multiStrokeEnabled
    };
    for (const KnobIWPtr& knob : paintKnobs) {
        if (KnobIPtr k = knob.lock()) {
            k->setInViewerContextSecret(!isPaintRole);
        }
    }
}

void
RotoPaintInteract::onToolSelected(const KnobButtonPtr& actionButton,
                                  RotoToolEnum tool)
{
    bool isPaintRole = (selectedRole == eRotoRolePaintBrush) || (selectedRole == eRotoRoleCloneBrush) || (selectedRole == eRotoRoleMergeBrush) || (selectedRole == eRotoRoleEffectBrush);
    if (!isPaintRole) {
        return;
    }

    const KnobIWPtr cloneKnobs[] = {
        timeOffsetModeChoice, timeOffsetSpinBox, sourceTypeChoice, resetCloneOffsetButton
    };
    for (const KnobIWPtr& knob : cloneKnobs) {
        if (KnobIPtr k = knob.lock()) {
            k->setInViewerContextSecret(selectedRole != eRotoRoleCloneBrush);
        }
    }
    if (KnobDoublePtr effect = effectSpinBox.lock()) {
        effect->setInViewerContextSecret(tool != eRotoToolBlur);
    }
    if (KnobChoicePtr sourceType = sourceTypeChoice.lock()) {
        if (tool == eRotoToolClone) {
            sourceType->setValue(1);
        } else if (tool == eRotoToolReveal) {
            sourceType->setValue(2);
        }
    }
    if (KnobChoicePtr compositingOperator = compositingOperatorChoice.lock()) {
        if ( (tool == eRotoToolSolidBrush ) || ( tool == eRotoToolOpenBezier ) ) {
            compositingOperator->setValue((int)eMergeOver);
        } else if (tool == eRotoToolBurn) {
            compositingOperator->setValue((int)eMergeColorBurn);
        } else if (tool == eRotoToolDodge) {
            compositingOperator->setValue((int)eMergeColorDodge);
        } else {
            compositingOperator->setValue((int)eMergeCopy);
        }
    }

    if (tool != eRotoToolEraserBrush) {
        lastPaintToolAction = actionButton;
    }

    if ( (selectedTool == eRotoToolBlur) ||
         ( selectedTool == eRotoToolBurn) ||
         ( selectedTool == eRotoToolDodge) ||
         ( selectedTool == eRotoToolClone) ||
         ( selectedTool == eRotoToolEraserBrush) ||
         ( selectedTool == eRotoToolSolidBrush) ||
         ( selectedTool == eRotoToolReveal) ||
         ( selectedTool == eRotoToolSmear) ||
         ( selectedTool == eRotoToolSharpen) ) {
        makeStroke( true, RotoPoint() );
    }
} // RotoPaintInteract::onToolSelected

void
RotoPaintInteract::onBreakMultiStrokeTriggered()
{
    makeStroke( true, RotoPoint() );
}

static bool
isBranchConnectedToRotoNodeRecursive(Node* node,
                                     const Node* rotoNode,
                                     int* recursion,
                                     std::list<Node*>& markedNodes)
{
    assert(recursion);
    if (!node) {
        return false;
    }
    if (rotoNode == node) {
        return true;
    }
    markedNodes.push_back(node);
    int maxInputs = node->getNInputs();
    *recursion = *recursion + 1;
    for (int i = 0; i < maxInputs; ++i) {
        NodePtr inp = node->getInput(i);
        if (inp) {
            if ( isBranchConnectedToRotoNodeRecursive(inp.get(), rotoNode, recursion, markedNodes) ) {
                return true;
            }
        }
    }

    return false;
}

void
RotoPaintInteract::checkViewersAreDirectlyConnected()
{
    if (!autoConnectViewerButton.lock()->getValue()) {
        return;
    }
    NodePtr rotoNode = getNode();
    std::list<ViewerInstance*> viewers;

    rotoNode->hasViewersConnected(&viewers);
    for (std::list<ViewerInstance*>::iterator it = viewers.begin(); it != viewers.end(); ++it) {
        NodePtr viewerNode = (*it)->getNode();
        int maxInputs = viewerNode->getNInputs();
        int hasBranchConnectedToRoto = -1;
        for (int i = 0; i < maxInputs; ++i) {
            NodePtr input = viewerNode->getInput(i);
            if (input) {
                std::list<Node*> markedNodes;
                int recursion = 0;
                if ( isBranchConnectedToRotoNodeRecursive(input.get(), rotoNode.get(), &recursion, markedNodes) ) {
                    if (recursion == 0) {
                        //This viewer is already connected to the Roto node directly.
                        break;
                    }
                    viewerNode->disconnectInput(i);
                    if (hasBranchConnectedToRoto == -1) {
                        viewerNode->connectInput(rotoNode, i);
                        hasBranchConnectedToRoto = i;
                    }
                }
            }
        }
    }
}

void
RotoPaintInteract::makeStroke(bool prepareForLater,
                              const RotoPoint& point)
{
    RotoStrokeType strokeType;
    std::string itemName;

    switch (selectedTool) {
    case eRotoToolSolidBrush:
        strokeType = eRotoStrokeTypeSolid;
        itemName = kRotoPaintBrushBaseName;
        break;
    case eRotoToolEraserBrush:
        strokeType = eRotoStrokeTypeEraser;
        itemName = kRotoPaintEraserBaseName;
        break;
    case eRotoToolClone:
        strokeType = eRotoStrokeTypeClone;
        itemName = kRotoPaintCloneBaseName;
        break;
    case eRotoToolReveal:
        strokeType = eRotoStrokeTypeReveal;
        itemName = kRotoPaintRevealBaseName;
        break;
    case eRotoToolBlur:
        strokeType = eRotoStrokeTypeBlur;
        itemName = kRotoPaintBlurBaseName;
        break;
    case eRotoToolSharpen:
        strokeType = eRotoStrokeTypeSharpen;
        itemName = kRotoPaintSharpenBaseName;
        break;
    case eRotoToolSmear:
        strokeType = eRotoStrokeTypeSmear;
        itemName = kRotoPaintSmearBaseName;
        break;
    case eRotoToolDodge:
        strokeType = eRotoStrokeTypeDodge;
        itemName = kRotoPaintDodgeBaseName;
        break;
    case eRotoToolBurn:
        strokeType = eRotoStrokeTypeBurn;
        itemName = kRotoPaintBurnBaseName;
        break;
    default:

        return;
    }

    RotoContextPtr context = getContext();

    if (prepareForLater || !strokeBeingPaint) {
        if ( strokeBeingPaint &&
             ( strokeBeingPaint->getBrushType() == strokeType) &&
             strokeBeingPaint->isEmpty() ) {
            ///We already have a fresh stroke prepared for that type
            return;
        }
        std::string name = context->generateUniqueName(itemName);
        strokeBeingPaint.reset( new RotoStrokeItem( strokeType, context, name, RotoLayerPtr() ) );
        strokeBeingPaint->createNodes(false);
    }


    assert(strokeBeingPaint);
    KnobColorPtr colorKnob = strokeBeingPaint->getColorKnob();
    KnobChoicePtr operatorKnob = strokeBeingPaint->getOperatorKnob();
    KnobDoublePtr opacityKnob = strokeBeingPaint->getOpacityKnob();
    KnobDoublePtr sizeKnob = strokeBeingPaint->getBrushSizeKnob();
    KnobDoublePtr hardnessKnob = strokeBeingPaint->getBrushHardnessKnob();
    KnobBoolPtr pressureOpaKnob = strokeBeingPaint->getPressureOpacityKnob();
    KnobBoolPtr pressureSizeKnob = strokeBeingPaint->getPressureSizeKnob();
    KnobBoolPtr pressureHardnessKnob = strokeBeingPaint->getPressureHardnessKnob();
    KnobBoolPtr buildUpKnob = strokeBeingPaint->getBuildupKnob();
    KnobChoicePtr timeOffsetModeKnob = strokeBeingPaint->getTimeOffsetModeKnob();
    KnobChoicePtr sourceTypeKnob = strokeBeingPaint->getBrushSourceTypeKnob();
    KnobIntPtr timeOffsetKnob = strokeBeingPaint->getTimeOffsetKnob();
    KnobDoublePtr translateKnob = strokeBeingPaint->getBrushCloneTranslateKnob();
    KnobDoublePtr effectKnob = strokeBeingPaint->getBrushEffectKnob();
    KnobColorPtr colorWheel = colorWheelButton.lock();
    double color[4];
    for (int i = 0; i < 3; ++i) {
        color[i] = colorWheel->getValue(i);
    }

    MergingFunctionEnum compOp = (MergingFunctionEnum)compositingOperatorChoice.lock()->getValue();
    double opacity = opacitySpinbox.lock()->getValue();
    double size = sizeSpinbox.lock()->getValue();
    double hardness = hardnessSpinbox.lock()->getValue();
    bool pressOpa = pressureOpacityButton.lock()->getValue();
    bool pressSize = pressureSizeButton.lock()->getValue();
    bool pressHarness = pressureHardnessButton.lock()->getValue();
    bool buildUp = buildUpButton.lock()->getValue();
    double timeOffset = timeOffsetSpinBox.lock()->getValue();
    double timeOffsetMode_i = timeOffsetModeChoice.lock()->getValue();
    int sourceType_i = sourceTypeChoice.lock()->getValue();
    double effectValue = effectSpinBox.lock()->getValue();

    int time = context->getTimelineCurrentTime();
    strokeBeingPaintedTimelineFrame = time;

    colorKnob->setValues(color[0], color[1], color[2], ViewSpec::all(), eValueChangedReasonNatronGuiEdited);
    operatorKnob->setValueFromID(Merge::getOperatorString(compOp), 0);
    opacityKnob->setValue(opacity);
    sizeKnob->setValue(size);
    hardnessKnob->setValue(hardness);
    pressureOpaKnob->setValue(pressOpa);
    pressureSizeKnob->setValue(pressSize);
    pressureHardnessKnob->setValue(pressHarness);
    buildUpKnob->setValue(buildUp);
    effectKnob->setValue(effectValue);
    if (!prepareForLater) {
        KnobIntPtr lifeTimeFrameKnob = strokeBeingPaint->getLifeTimeFrameKnob();
        lifeTimeFrameKnob->setValue( time );
    }
    if ( (strokeType == eRotoStrokeTypeClone) || (strokeType == eRotoStrokeTypeReveal) ) {
        timeOffsetKnob->setValue(timeOffset);
        timeOffsetModeKnob->setValue(timeOffsetMode_i);
        sourceTypeKnob->setValue(sourceType_i);
        translateKnob->setValues(-cloneOffset.first, -cloneOffset.second, ViewSpec::all(), eValueChangedReasonNatronGuiEdited);
    }
    if (!prepareForLater) {
        RotoLayerPtr layer = context->findDeepestSelectedLayer();
        if (!layer) {
            layer = context->getOrCreateBaseLayer();
        }
        assert(layer);
        context->addItem(layer, 0, strokeBeingPaint, RotoItem::eSelectionReasonOther);
        context->getNode()->getApp()->setUserIsPainting(context->getNode(), strokeBeingPaint, true);
        strokeBeingPaint->appendPoint(true, point);
    }
} // RotoGui::RotoGuiPrivate::makeStroke

void
RotoPaintInteract::removeCurve(const RotoDrawableItemPtr& curve)
{
    if (curve == strokeBeingPaint) {
        strokeBeingPaint.reset();
    }
    RotoCurveInteract::removeCurve(curve);
}

void
RotoPaintInteract::drawPaintToolOverlay(double screenPixelRatio,
                                        const std::pair<double, double>& viewportSize)
{
    if (p->isPaintByDefault && ((selectedRole == eRotoRoleMergeBrush) || (selectedRole == eRotoRolePaintBrush) || (selectedRole == eRotoRoleEffectBrush) || (selectedRole == eRotoRoleCloneBrush)) && (selectedTool != eRotoToolOpenBezier)) {
        Point cursorPos;
        getHost()->getCurrentViewportForOverlays()->getCursorPosition(cursorPos.x, cursorPos.y);
        RectD viewportRect = getHost()->getCurrentViewportForOverlays()->getViewportRect();

        if (viewportRect.contains(cursorPos.x, cursorPos.y)) {
            // Draw a circle  around the cursor
            double brushSize = sizeSpinbox.lock()->getValue();
            GLdouble projection[16];
            glGetDoublev(GL_PROJECTION_MATRIX, projection);
            Point shadow; // how much to translate GL_PROJECTION to get exactly one pixel on screen
            shadow.x = 2. / (projection[0] * viewportSize.first);
            shadow.y = 2. / (projection[5] * viewportSize.second);

            double halfBrush = brushSize / 2.;
            QPointF ellipsePos;
            if ((state == eEventStateDraggingBrushSize) || (state == eEventStateDraggingBrushOpacity)) {
                ellipsePos = mouseCenterOnSizeChange;
            } else {
                ellipsePos = lastMousePos;
            }
            double opacity = opacitySpinbox.lock()->getValue();

            for (int l = 0; l < 2; ++l) {
                glMatrixMode(GL_PROJECTION);
                int direction = (l == 0) ? 1 : -1;
                // translate (1,-1) pixels
                glTranslated(direction * shadow.x, -direction * shadow.y, 0);
                glMatrixMode(GL_MODELVIEW);
                drawEllipse(ellipsePos.x(), ellipsePos.y(), halfBrush, halfBrush, l, 1.f, 1.f, 1.f, opacity);

                glColor3f(.5f * l * opacity, .5f * l * opacity, .5f * l * opacity);

                if (((selectedTool == eRotoToolClone) || (selectedTool == eRotoToolReveal)) && ((cloneOffset.first != 0) || (cloneOffset.second != 0))) {
                    glLineWidth(1.5 * screenPixelRatio);
                    glBegin(GL_LINES);

                    if (state == eEventStateDraggingCloneOffset) {
                        // draw a line between the center of the 2 ellipses
                        glVertex2d(ellipsePos.x(), ellipsePos.y());
                        glVertex2d(ellipsePos.x() + cloneOffset.first, ellipsePos.y() + cloneOffset.second);
                    }
                    // draw a cross in the center of the source ellipse
                    glVertex2d(ellipsePos.x() + cloneOffset.first, ellipsePos.y() + cloneOffset.second - halfBrush);
                    glVertex2d(ellipsePos.x() + cloneOffset.first, ellipsePos.y() + cloneOffset.second + halfBrush);
                    glVertex2d(ellipsePos.x() + cloneOffset.first - halfBrush, ellipsePos.y() + cloneOffset.second);
                    glVertex2d(ellipsePos.x() + cloneOffset.first + halfBrush, ellipsePos.y() + cloneOffset.second);
                    glEnd();

                    // draw the source ellipse
                    drawEllipse(ellipsePos.x() + cloneOffset.first, ellipsePos.y() + cloneOffset.second, halfBrush, halfBrush, l, 1.f, 1.f, 1.f, opacity / 2.);
                }
            }
        }
    }
} // RotoPaintInteract::drawPaintToolOverlay

void
RotoPaintInteract::onPenDownStarted(PenType pen)
{
    lastTabletDownTriggeredEraser = false;
    if (p->isPaintByDefault && ((pen == ePenTypeEraser) || (pen == ePenTypePen) || (pen == ePenTypeCursor))) {
        if ((pen == ePenTypeEraser) && (selectedTool != eRotoToolEraserBrush)) {
            setCurrentTool(eraserAction.lock());
            lastTabletDownTriggeredEraser = true;
        }
    }
}

bool
RotoPaintInteract::onPaintToolPenDown(double /*time*/,
                                      const QPointF& pos,
                                      double pressure,
                                      double timestamp)
{
    RotoContextPtr context = getContext();
    bool didSomething = false;

    switch (selectedTool) {
    case eRotoToolSolidBrush:
    case eRotoToolEraserBrush:
    case eRotoToolClone:
    case eRotoToolReveal:
    case eRotoToolBlur:
    case eRotoToolSharpen:
    case eRotoToolSmear:
    case eRotoToolDodge:
    case eRotoToolBurn: {
        if (((selectedTool == eRotoToolClone) || (selectedTool == eRotoToolReveal)) && ctrlDown && !shiftDown && !altDown) {
            state = eEventStateDraggingCloneOffset;
        } else if (shiftDown && !ctrlDown && !altDown) {
            state = eEventStateDraggingBrushSize;
            mouseCenterOnSizeChange = pos;
        } else if (ctrlDown && shiftDown) {
            state = eEventStateDraggingBrushOpacity;
            mouseCenterOnSizeChange = pos;
        } else {
            /*
               Check that all viewers downstream are connected directly to the RotoPaint to avoid glitches and bugs
             */
            checkViewersAreDirectlyConnected();
            bool multiStrokeEnabled = isMultiStrokeEnabled();
            if (strokeBeingPaint && strokeBeingPaint->getParentLayer() && multiStrokeEnabled) {
                RotoLayerPtr layer = strokeBeingPaint->getParentLayer();
                if (!layer) {
                    layer = context->findDeepestSelectedLayer();
                    if (!layer) {
                        layer = context->getOrCreateBaseLayer();
                    }
                    assert(layer);
                    context->addItem(layer, 0, strokeBeingPaint, RotoItem::eSelectionReasonOther);
                }

                context->getNode()->getApp()->setUserIsPainting(context->getNode(), strokeBeingPaint, true);

                KnobIntPtr lifeTimeFrameKnob = strokeBeingPaint->getLifeTimeFrameKnob();
                lifeTimeFrameKnob->setValue(context->getTimelineCurrentTime());

                strokeBeingPaint->appendPoint(true, RotoPoint(pos.x(), pos.y(), pressure, timestamp));
            } else {
                if (strokeBeingPaint && !strokeBeingPaint->getParentLayer() && multiStrokeEnabled) {
                    strokeBeingPaint.reset();
                }
                makeStroke(false, RotoPoint(pos.x(), pos.y(), pressure, timestamp));
            }
            context->evaluateChange();
            state = eEventStateBuildingStroke;
            getHost()->setCurrentCursor(eCursorBlank);
        }
        didSomething = true;
        break;
    }
    default:
        assert(false);
        break;
    } // switch

    return didSomething;
} // RotoPaintInteract::onPaintToolPenDown

bool
RotoPaintInteract::setPaintToolCursor(bool* redraw)
{
    RotoContextPtr context = getContext();
    bool cursorSet = false;

    if (context->isRotoPaint() && ((selectedRole == eRotoRoleMergeBrush) || (selectedRole == eRotoRoleCloneBrush) || (selectedRole == eRotoRolePaintBrush) || (selectedRole == eRotoRoleEffectBrush))) {
        if (state != eEventStateBuildingStroke) {
            getHost()->setCurrentCursor(eCursorCross);
        } else {
            getHost()->setCurrentCursor(eCursorBlank);
        }
        cursorSet = true;
    }

    switch (selectedTool) {
    case eRotoToolSolidBrush:
    case eRotoToolEraserBrush:
    case eRotoToolClone:
    case eRotoToolReveal:
    case eRotoToolBlur:
    case eRotoToolSharpen:
    case eRotoToolSmear:
    case eRotoToolDodge:
    case eRotoToolBurn: {
        *redraw = true; // Those tools use the overlay to draw the tool
    }
    default:
        break;
    } // switch

    return cursorSet;
}

bool
RotoPaintInteract::onPaintStatePenMotion(const QPointF& pos,
                                         double pressure,
                                         double timestamp,
                                         double dx,
                                         double dy)
{
    RotoContextPtr context = getContext();
    bool didSomething = false;

    switch (state) {
    case eEventStateBuildingStroke: {
        if (strokeBeingPaint) {
            // disable draft during painting
            if (getHost()->getApp()->isDraftRenderEnabled()) {
                getHost()->getApp()->setDraftRenderEnabled(false);
            }

            RotoPoint p(pos.x(), pos.y(), pressure, timestamp);
            if (strokeBeingPaint->appendPoint(false, p)) {
                lastMousePos = pos;
                context->evaluateChange_noIncrement();
                didSomething = true;
            }
        }
        break;
    }
    case eEventStateDraggingCloneOffset: {
        cloneOffset.first -= dx;
        cloneOffset.second -= dy;
        onBreakMultiStrokeTriggered();
        break;
    }
    case eEventStateDraggingBrushSize: {
        KnobDoublePtr sizeSb = sizeSpinbox.lock();
        double size = 0;
        if (sizeSb) {
            size = sizeSb->getValue();
        }
        size += ((dx + dy) / 2.);
        const double scale = 0.01; // i.e. round to nearest one-hundreth
        size = std::floor(size / scale + 0.5) * scale;
        if (sizeSb) {
            sizeSb->setValue(std::max(1., size));
        }
        onBreakMultiStrokeTriggered();
        didSomething = true;
        break;
    }
    case eEventStateDraggingBrushOpacity: {
        KnobDoublePtr opaSb = sizeSpinbox.lock();
        double opa = 0;
        if (opaSb) {
            opa = opaSb->getValue();
        }
        double newOpa = opa + ((dx + dy) / 2.);
        if (opa != 0) {
            newOpa = std::max(0., std::min(1., newOpa / opa));
            newOpa = newOpa > 0 ? std::min(1., opa + 0.05) : std::max(0., opa - 0.05);
        } else {
            newOpa = newOpa < 0 ? .0 : 0.05;
        }
        const double scale = 0.01; // i.e. round to nearest one-hundreth
        newOpa = std::floor(newOpa / scale + 0.5) * scale;
        if (opaSb) {
            opaSb->setValue(newOpa);
        }
        onBreakMultiStrokeTriggered();
        didSomething = true;
        break;
    }
    default:
        break;
    } // switch

    return didSomething;
} // RotoPaintInteract::onPaintStatePenMotion

bool
RotoPaintInteract::onPaintStatePenUp()
{
    RotoContextPtr context = getContext();
    bool ret = false;

    if (state == eEventStateBuildingStroke) {
        assert(strokeBeingPaint);
        context->getNode()->getApp()->setUserIsPainting(context->getNode(), strokeBeingPaint, false);
        assert(strokeBeingPaint->getParentLayer());

        bool multiStrokeEnabled = isMultiStrokeEnabled();
        if (!multiStrokeEnabled) {
            pushUndoCommand(new AddStrokeUndoCommand(shared_from_this_paint(), strokeBeingPaint));
            makeStroke(true, RotoPoint());
        } else {
            pushUndoCommand(new AddMultiStrokeUndoCommand(shared_from_this_paint(), strokeBeingPaint));
        }

        /**
         * Do a neat render for the stroke (better interpolation). This call is blocking otherwise the user might
         * attempt to make a new stroke while the previous stroke is not finished... this would yield artifacts.
         **/
        getHost()->setCurrentCursor(eCursorBusy);
        context->evaluateNeatStrokeRender();
        if (context->isRotoPaint() && ((selectedRole == eRotoRoleMergeBrush) || (selectedRole == eRotoRoleCloneBrush) || (selectedRole == eRotoRolePaintBrush) || (selectedRole == eRotoRoleEffectBrush))) {
            getHost()->setCurrentCursor(eCursorCross);
        } else {
            getHost()->setCurrentCursor(eCursorDefault);
        }
        strokeBeingPaint->setStrokeFinished();
        ret = true;
    }

    return ret;
} // RotoPaintInteract::onPaintStatePenUp

bool
RotoPaintInteract::onPenUpFinished()
{
    if (lastTabletDownTriggeredEraser) {
        setCurrentTool(lastPaintToolAction.lock());

        return true;
    }

    return false;
}

NATRON_NAMESPACE_EXIT
