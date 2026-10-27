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

#include "RotoCurveInteract.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QLineF>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Bezier.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/OverlaySupport.h"
#include "Engine/PluginActionShortcut.h"
#include "Engine/RotoCurveContext.h"
#include "Engine/RotoLayer.h"
#include "Engine/RotoStrokeItem.h"
#include "Engine/RotoUndoCommand.h"
#include "Engine/Transform.h"
#include "Global/GLIncludes.h"

NATRON_NAMESPACE_ENTER
RotoCurveInteract::RotoCurveInteract(EffectInstance* host)
    : selectedItems()
    , selectedCps()
    , selectedCpsBbox()
    , showCpsBbox(false)
    , transformMode()
    , builtBezier()
    , bezierBeingDragged()
    , cpBeingDragged()
    , tangentBeingDragged()
    , featherBarBeingDragged()
    , featherBarBeingHovered()
    , click()
    , selectedTool(eRotoToolSelectAll)
    , selectedRole(eRotoRoleSelection)
    , state(eEventStateNone)
    , hoverState(eHoverStateNothing)
    , lastClickPos()
    , lastMousePos()
    , evaluateOnPenUp(false)
    , evaluateOnKeyUp(false)
    , iSelectingwithCtrlA(false)
    , shiftDown(0)
    , ctrlDown(0)
    , altDown(0)
    , _host(host)
{
}

RotoCurveInteract::~RotoCurveInteract()
{
}

// make_shared enabler (because make_shared needs access to the private constructor)
// see https://stackoverflow.com/a/20961251/2607517
struct RotoCurveInteract::MakeSharedEnabler : public RotoCurveInteract {
    MakeSharedEnabler(EffectInstance* host)
        : RotoCurveInteract(host)
    {
    }
};

RotoCurveInteractPtr
RotoCurveInteract::create(EffectInstance* host)
{
    return std::make_shared<RotoCurveInteract::MakeSharedEnabler>(host);
}

NodePtr
RotoCurveInteract::getNode() const
{
    return _host->getNode();
}

RotoCurveContextPtr
RotoCurveInteract::getCurveContext() const
{
    return _host->getNode()->getCurveContext();
}

double
RotoCurveInteract::getCurrentTime() const
{
    return _host->getCurrentTime();
}

std::pair<double, double>
RotoCurveInteract::getPixelScale() const
{
    std::pair<double, double> pixelScale;

    _host->getCurrentViewportForOverlays()->getPixelScale(pixelScale.first, pixelScale.second);

    return pixelScale;
}

void
RotoCurveInteract::pushUndoCommand(UndoCommand* command)
{
    _host->pushUndoCommand(command);
}

void
RotoCurveInteract::evaluate(bool redraw)
{
    if (redraw) {
        _host->redrawOverlayInteract();
    }
    getCurveContext()->evaluateChange();
    _host->getApp()->triggerAutoSave();
}

void
RotoCurveInteract::autoSaveAndRedraw()
{
    _host->redrawOverlayInteract();
    _host->getApp()->triggerAutoSave();
}

void
RotoCurveInteract::redrawOverlays()
{
    _host->redrawOverlayInteract();
}

bool
RotoCurveInteract::isFeatherVisible() const
{
    KnobButtonPtr b = displayFeatherEnabledButton.lock();

    if (b) {
        return b->getValue();
    } else {
        return true;
    }
}

bool
RotoCurveInteract::isStickySelectionEnabled() const
{
    KnobButtonPtr b = stickySelectionEnabledButton.lock();

    if (b) {
        return b->getValue();
    } else {
        return false;
    }
}

bool
RotoCurveInteract::isBboxClickAnywhereEnabled() const
{
    KnobButtonPtr b = bboxClickAnywhereButton.lock();

    return b ? b->getValue() : false;
}

void
RotoCurveInteract::drawSelectedCp(double time,
                                  const BezierCPPtr& cp,
                                  double x,
                                  double y,
                                  const Transform::Matrix3x3& transform)
{
    /// if the tangent is being dragged, color it
    bool colorLeftTangent = false;
    bool colorRightTangent = false;

    if ((cp == tangentBeingDragged) && ((state == eEventStateDraggingLeftTangent) || (state == eEventStateDraggingRightTangent))) {
        colorLeftTangent = state == eEventStateDraggingLeftTangent ? true : false;
        colorRightTangent = !colorLeftTangent;
    }

    Transform::Point3D leftDeriv, rightDeriv;
    leftDeriv.z = rightDeriv.z = 1.;
    cp->getLeftBezierPointAtTime(true, time, ViewIdx(0), &leftDeriv.x, &leftDeriv.y);
    cp->getRightBezierPointAtTime(true, time, ViewIdx(0), &rightDeriv.x, &rightDeriv.y);
    leftDeriv = Transform::matApply(transform, leftDeriv);
    rightDeriv = Transform::matApply(transform, rightDeriv);

    bool drawLeftHandle = leftDeriv.x != x || leftDeriv.y != y;
    bool drawRightHandle = rightDeriv.y != x || rightDeriv.y != y;
    glBegin(GL_POINTS);
    if (drawLeftHandle) {
        if (colorLeftTangent) {
            glColor3f(0.2, 1., 0.);
        }
        glVertex2d(leftDeriv.x, leftDeriv.y);
        if (colorLeftTangent) {
            glColor3d(0.85, 0.67, 0.);
        }
    }
    if (drawRightHandle) {
        if (colorRightTangent) {
            glColor3f(0.2, 1., 0.);
        }
        glVertex2d(rightDeriv.x, rightDeriv.y);
        if (colorRightTangent) {
            glColor3d(0.85, 0.67, 0.);
        }
    }
    glEnd();

    glBegin(GL_LINE_STRIP);
    if (drawLeftHandle) {
        glVertex2d(leftDeriv.x, leftDeriv.y);
    }
    glVertex2d(x, y);
    if (drawRightHandle) {
        glVertex2d(rightDeriv.x, rightDeriv.y);
    }
    glEnd();
} // drawSelectedCp

void
RotoCurveInteract::drawEllipse(double x,
                               double y,
                               double radiusX,
                               double radiusY,
                               int l,
                               double r,
                               double g,
                               double b,
                               double a)
{
    glColor3f(r * l * a, g * l * a, b * l * a);

    glPushMatrix();
    //  center the oval at x_center, y_center
    glTranslatef((float)x, (float)y, 0.f);
    //  draw the oval using line segments
    glBegin(GL_LINE_LOOP);
    // we don't need to be pixel-perfect here, it's just an interact!
    // 40 segments is enough.
    double m = 2 * 3.14159265358979323846264338327950288419717 / 40.;
    for (int i = 0; i < 40; ++i) {
        double theta = i * m;
        glVertex2d(radiusX * std::cos(theta), radiusY * std::sin(theta));
    }
    glEnd();

    glPopMatrix();
}

void
RotoCurveInteract::drawArrow(double centerX,
                             double centerY,
                             double rotate,
                             bool hovered,
                             const std::pair<double, double>& pixelScale)
{
    GLProtectMatrix pm(GL_MODELVIEW);

    if (hovered) {
        glColor3f(0., 1., 0.);
    } else {
        glColor3f(1., 1., 1.);
    }

    double arrowLenght = kTransformArrowLenght * pixelScale.second;
    double arrowWidth = kTransformArrowWidth * pixelScale.second;
    double arrowHeadHeight = 4 * pixelScale.second;

    glTranslatef(centerX, centerY, 0.);
    glRotatef(rotate, 0., 0., 1.);
    QPointF bottom(0., -arrowLenght);
    QPointF top(0, arrowLenght);
    /// the arrow head is 4 pixels long and kTransformArrowWidth * 2 large
    double screenPixelRatio = _host->getCurrentViewportForOverlays()->getScreenPixelRatio();
    glLineWidth(1 * screenPixelRatio);
    glBegin(GL_LINES);
    glVertex2f(top.x(), top.y());
    glVertex2f(bottom.x(), bottom.y());
    glEnd();

    glBegin(GL_POLYGON);
    glVertex2f(bottom.x(), bottom.y());
    glVertex2f(bottom.x() + arrowWidth, bottom.y() + arrowHeadHeight);
    glVertex2f(bottom.x() - arrowWidth, bottom.y() + arrowHeadHeight);
    glEnd();

    glBegin(GL_POLYGON);
    glVertex2f(top.x(), top.y());
    glVertex2f(top.x() - arrowWidth, top.y() - arrowHeadHeight);
    glVertex2f(top.x() + arrowWidth, top.y() - arrowHeadHeight);
    glEnd();
}

void
RotoCurveInteract::drawBendedArrow(double centerX,
                                   double centerY,
                                   double rotate,
                                   bool hovered,
                                   const std::pair<double, double>& pixelScale)
{
    GLProtectMatrix p(GL_MODELVIEW);

    if (hovered) {
        glColor3f(0., 1., 0.);
    } else {
        glColor3f(1., 1., 1.);
    }

    double arrowLenght = kTransformArrowLenght * pixelScale.second;
    double arrowWidth = kTransformArrowWidth * pixelScale.second;
    double arrowHeadHeight = 4 * pixelScale.second;

    glTranslatef(centerX, centerY, 0.);
    glRotatef(rotate, 0., 0., 1.);

    /// by default we draw the top left
    QPointF bottom(0., -arrowLenght / 2.);
    QPointF right(arrowLenght / 2., 0.);
    glBegin(GL_LINE_STRIP);
    glVertex2f(bottom.x(), bottom.y());
    glVertex2f(0., 0.);
    glVertex2f(right.x(), right.y());
    glEnd();

    glBegin(GL_POLYGON);
    glVertex2f(bottom.x(), bottom.y() - arrowHeadHeight);
    glVertex2f(bottom.x() - arrowWidth, bottom.y());
    glVertex2f(bottom.x() + arrowWidth, bottom.y());
    glEnd();

    glBegin(GL_POLYGON);
    glVertex2f(right.x() + arrowHeadHeight, right.y());
    glVertex2f(right.x(), right.y() - arrowWidth);
    glVertex2f(right.x(), right.y() + arrowWidth);
    glEnd();
}

void
RotoCurveInteract::drawSelectedCpsBBOX()
{
    std::pair<double, double> pixelScale;

    _host->getCurrentViewportForOverlays()->getPixelScale(pixelScale.first, pixelScale.second);

    {
        GLProtectAttrib a(GL_HINT_BIT | GL_ENABLE_BIT | GL_LINE_BIT | GL_POINT_BIT | GL_COLOR_BUFFER_BIT | GL_CURRENT_BIT | GL_TRANSFORM_BIT);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_LINE_SMOOTH);
        glHint(GL_LINE_SMOOTH_HINT, GL_DONT_CARE);

        QPointF topLeft = selectedCpsBbox.topLeft();
        QPointF btmRight = selectedCpsBbox.bottomRight();

        double screenPixelRatio = _host->getCurrentViewportForOverlays()->getScreenPixelRatio();

        if (hoverState == eHoverStateBbox) {
            glColor4f(0.9, 0.5, 0, 1.);
        } else {
            glColor4f(0.8, 0.8, 0.8, 1.);
        }
        glLineWidth(1.5 * screenPixelRatio);
        glBegin(GL_LINE_LOOP);
        glVertex2f(topLeft.x(), btmRight.y());
        glVertex2f(topLeft.x(), topLeft.y());
        glVertex2f(btmRight.x(), topLeft.y());
        glVertex2f(btmRight.x(), btmRight.y());
        glEnd();

        double midX = (topLeft.x() + btmRight.x()) / 2.;
        double midY = (btmRight.y() + topLeft.y()) / 2.;
        double xHairMidSizeX = kXHairSelectedCpsBox * pixelScale.first;
        double xHairMidSizeY = kXHairSelectedCpsBox * pixelScale.second;
        QLineF selectedCpsCrossHorizLine;
        selectedCpsCrossHorizLine.setLine(midX - xHairMidSizeX, midY, midX + xHairMidSizeX, midY);
        QLineF selectedCpsCrossVertLine;
        selectedCpsCrossVertLine.setLine(midX, midY - xHairMidSizeY, midX, midY + xHairMidSizeY);

        glLineWidth(1.5 * screenPixelRatio);
        glBegin(GL_LINES);
        glVertex2f(std::max(selectedCpsCrossHorizLine.p1().x(), topLeft.x()), selectedCpsCrossHorizLine.p1().y());
        glVertex2f(std::min(selectedCpsCrossHorizLine.p2().x(), btmRight.x()), selectedCpsCrossHorizLine.p2().y());
        glVertex2f(selectedCpsCrossVertLine.p1().x(), std::max(selectedCpsCrossVertLine.p1().y(), btmRight.y()));
        glVertex2f(selectedCpsCrossVertLine.p2().x(), std::min(selectedCpsCrossVertLine.p2().y(), topLeft.y()));
        glEnd();

        glCheckError();

        QPointF midTop((topLeft.x() + btmRight.x()) / 2., topLeft.y());
        QPointF midRight(btmRight.x(), (topLeft.y() + btmRight.y()) / 2.);
        QPointF midBtm((topLeft.x() + btmRight.x()) / 2., btmRight.y());
        QPointF midLeft(topLeft.x(), (topLeft.y() + btmRight.y()) / 2.);

        /// draw the 4 corners points and the 4 mid points
        glPointSize(5.f * screenPixelRatio);
        glBegin(GL_POINTS);
        glVertex2f(topLeft.x(), topLeft.y());
        glVertex2f(btmRight.x(), topLeft.y());
        glVertex2f(btmRight.x(), btmRight.y());
        glVertex2f(topLeft.x(), btmRight.y());

        glVertex2f(midTop.x(), midTop.y());
        glVertex2f(midRight.x(), midRight.y());
        glVertex2f(midBtm.x(), midBtm.y());
        glVertex2f(midLeft.x(), midLeft.y());
        glEnd();

        /// now draw the handles to indicate the user he/she can transform the selection rectangle
        /// draw it only if it is not dragged
        bool drawHandles = state != eEventStateDraggingBBoxBtmLeft && state != eEventStateDraggingBBoxBtmRight && state != eEventStateDraggingBBoxTopLeft && state != eEventStateDraggingBBoxTopRight && state != eEventStateDraggingBBoxMidTop
            && state != eEventStateDraggingBBoxMidRight && state != eEventStateDraggingBBoxMidLeft && state != eEventStateDraggingBBoxMidBtm;

        if (drawHandles) {
            double offset = kTransformArrowOffsetFromPoint * pixelScale.first;
            double halfOffset = offset / 2.;
            if (transformMode == eSelectedCpsTransformModeTranslateAndScale) {
                /// draw mid top arrow vertical
                drawArrow(midTop.x(), midTop.y() + offset, 0., hoverState == eHoverStateBboxMidTop, pixelScale);
                /// draw mid right arrow horizontal
                drawArrow(midRight.x() + offset, midRight.y(), 90., hoverState == eHoverStateBboxMidRight, pixelScale);
                /// draw mid btm arrow vertical
                drawArrow(midBtm.x(), midBtm.y() - offset, 0., hoverState == eHoverStateBboxMidBtm, pixelScale);
                /// draw mid left arrow horizontal
                drawArrow(midLeft.x() - offset, midLeft.y(), 90., hoverState == eHoverStateBboxMidLeft, pixelScale);
                /// draw top left arrow rotated
                drawArrow(topLeft.x() - offset, topLeft.y() + offset, 45., hoverState == eHoverStateBboxTopLeft, pixelScale);
                /// draw top right arrow rotated
                drawArrow(btmRight.x() + offset, topLeft.y() + offset, -45., hoverState == eHoverStateBboxTopRight, pixelScale);
                /// draw btm right arrow rotated
                drawArrow(btmRight.x() + offset, btmRight.y() - offset, 45., hoverState == eHoverStateBboxBtmRight, pixelScale);
                /// draw btm left arrow rotated
                drawArrow(topLeft.x() - offset, btmRight.y() - offset, -45., hoverState == eHoverStateBboxBtmLeft, pixelScale);
            } else {
                /// draw mid top arrow horizontal
                drawArrow(midTop.x(), midTop.y() + offset, 90., hoverState == eHoverStateBboxMidTop, pixelScale);
                /// draw mid right arrow vertical
                drawArrow(midRight.x() + offset, midRight.y(), 0., hoverState == eHoverStateBboxMidRight, pixelScale);
                /// draw mid btm arrow horizontal
                drawArrow(midBtm.x(), midBtm.y() - offset, 90., hoverState == eHoverStateBboxMidBtm, pixelScale);
                /// draw mid left arrow vertical
                drawArrow(midLeft.x() - offset, midLeft.y(), 0., hoverState == eHoverStateBboxMidLeft, pixelScale);
                /// draw the top left bended
                drawBendedArrow(topLeft.x() - halfOffset, topLeft.y() + halfOffset, 0., hoverState == eHoverStateBboxTopLeft, pixelScale);
                /// draw the top right bended
                drawBendedArrow(btmRight.x() + halfOffset, topLeft.y() + halfOffset, -90, hoverState == eHoverStateBboxTopRight, pixelScale);
                /// draw the btm right bended
                drawBendedArrow(btmRight.x() + halfOffset, btmRight.y() - halfOffset, -180, hoverState == eHoverStateBboxBtmRight, pixelScale);
                /// draw the btm left bended
                drawBendedArrow(topLeft.x() - halfOffset, btmRight.y() - halfOffset, 90, hoverState == eHoverStateBboxBtmLeft, pixelScale);
            }
        }
    } // GLProtectAttrib a(GL_HINT_BIT | GL_ENABLE_BIT | GL_LINE_BIT | GL_POINT_BIT | GL_COLOR_BUFFER_BIT | GL_CURRENT_BIT);
} // drawSelectedCpsBBOX

void
RotoCurveInteract::clearSelection()
{
    clearBeziersSelection();
    clearCPSSelection();
}

bool
RotoCurveInteract::hasSelection() const
{
    return !selectedItems.empty() || !selectedCps.empty();
}

void
RotoCurveInteract::clearCPSSelection()
{
    selectedCps.clear();
    showCpsBbox = false;
    transformMode = eSelectedCpsTransformModeTranslateAndScale;
    selectedCpsBbox.setTopLeft(QPointF(0, 0));
    selectedCpsBbox.setTopRight(QPointF(0, 0));
}

void
RotoCurveInteract::clearBeziersSelection()
{
    RotoCurveContextPtr ctx = getCurveContext();

    assert(ctx);
    ctx->clearSelection(RotoItem::eSelectionReasonOverlayInteract);
    selectedItems.clear();
}

bool
RotoCurveInteract::removeItemFromSelection(const RotoDrawableItemPtr& b)
{
    RotoCurveContextPtr ctx = getCurveContext();

    assert(ctx);
    for (SelectedItems::iterator fb = selectedItems.begin(); fb != selectedItems.end(); ++fb) {
        if (fb->get() == b.get()) {
            ctx->deselect(*fb, RotoItem::eSelectionReasonOverlayInteract);
            selectedItems.erase(fb);

            return true;
        }
    }

    return false;
}

static void
handleControlPointMaximum(double time,
                          const BezierCP& p,
                          double* l,
                          double* b,
                          double* r,
                          double* t)
{
    double x, y, xLeft, yLeft, xRight, yRight;

    p.getPositionAtTime(true, time, ViewIdx(0), &x, &y);
    p.getLeftBezierPointAtTime(true, time, ViewIdx(0), &xLeft, &yLeft);
    p.getRightBezierPointAtTime(true, time, ViewIdx(0), &xRight, &yRight);

    *r = std::max(x, *r);
    *l = std::min(x, *l);

    *r = std::max(xLeft, *r);
    *l = std::min(xLeft, *l);

    *r = std::max(xRight, *r);
    *l = std::min(xRight, *l);

    *t = std::max(y, *t);
    *b = std::min(y, *b);

    *t = std::max(yLeft, *t);
    *b = std::min(yLeft, *b);

    *t = std::max(yRight, *t);
    *b = std::min(yRight, *b);
}

bool
RotoCurveInteract::getRoleForGroup(const KnobGroupPtr& k,
                                   RotoRoleEnum* role) const
{
    bool ret = true;

    if (k == selectToolGroup.lock()) {
        *role = eRotoRoleSelection;
    } else if (k == pointsEditionToolGroup.lock()) {
        *role = eRotoRolePointsEdition;
    } else if (k == bezierEditionToolGroup.lock()) {
        *role = eRotoRoleBezierEdition;
    } else {
        ret = false;
    }

    return ret;
}

bool
RotoCurveInteract::getToolForAction(const KnobButtonPtr& k,
                                    RotoToolEnum* tool) const
{
    bool ret = true;

    if (k == selectAllAction.lock()) {
        *tool = eRotoToolSelectAll;
    } else if (k == selectPointsAction.lock()) {
        *tool = eRotoToolSelectPoints;
    } else if (k == selectCurvesAction.lock()) {
        *tool = eRotoToolSelectCurves;
    } else if (k == selectFeatherPointsAction.lock()) {
        *tool = eRotoToolSelectFeatherPoints;
    } else if (k == addPointsAction.lock()) {
        *tool = eRotoToolAddPoints;
    } else if (k == removePointsAction.lock()) {
        *tool = eRotoToolRemovePoints;
    } else if (k == cuspPointsAction.lock()) {
        *tool = eRotoToolCuspPoints;
    } else if (k == smoothPointsAction.lock()) {
        *tool = eRotoToolSmoothPoints;
    } else if (k == openCloseCurveAction.lock()) {
        *tool = eRotoToolOpenCloseCurve;
    } else if (k == removeFeatherAction.lock()) {
        *tool = eRotoToolRemoveFeatherPoints;
    } else if (k == drawBezierAction.lock()) {
        *tool = eRotoToolDrawBezier;
    } else if (k == drawEllipseAction.lock()) {
        *tool = eRotoToolDrawEllipse;
    } else if (k == drawRectangleAction.lock()) {
        *tool = eRotoToolDrawRectangle;
    } else {
        ret = false;
    }

    return ret;
} // RotoCurveInteract::getToolForAction

bool
RotoCurveInteract::onRoleChangedInternal(const KnobGroupPtr& roleGroup)
{
    RotoRoleEnum role;

    if (!getRoleForGroup(roleGroup, &role)) {
        return false;
    }

    // The gui just deactivated this action
    if (!roleGroup->getValue()) {
        return true;
    }

    // Reset the selected control points
    selectedCps.clear();
    showCpsBbox = false;
    transformMode = eSelectedCpsTransformModeTranslateAndScale;
    selectedCpsBbox.setTopLeft(QPointF(0, 0));
    selectedCpsBbox.setTopRight(QPointF(0, 0));

    // Roto action bar
    bool isPaintRole = (role == eRotoRolePaintBrush) || (role == eRotoRoleCloneBrush) || (role == eRotoRoleMergeBrush) || (role == eRotoRoleEffectBrush);
    const KnobButtonWPtr rotoButtons[] = {
        autoKeyingEnabledButton, featherLinkEnabledButton, displayFeatherEnabledButton, stickySelectionEnabledButton,
        bboxClickAnywhereButton, rippleEditEnabledButton, addKeyframeButton, removeKeyframeButton, showTransformHandle
    };
    for (const KnobButtonWPtr& button : rotoButtons) {
        if (KnobButtonPtr b = button.lock()) {
            b->setInViewerContextSecret(isPaintRole);
        }
    }

    selectedRole = role;
    onRoleSelected(role);

    return true;
} // RotoCurveInteract::onRoleChangedInternal

bool
RotoCurveInteract::onToolChangedInternal(const KnobButtonPtr& actionButton)
{
    RotoToolEnum tool;

    if (!getToolForAction(actionButton, &tool)) {
        return false;
    }

    // The gui just deactivated this action
    if (!actionButton->getValue()) {
        return true;
    }

    // Clear all selection if we were building a new bezier
    if ((selectedRole == eRotoRoleBezierEdition) && ((selectedTool == eRotoToolDrawBezier) || (selectedTool == eRotoToolOpenBezier)) && builtBezier && (tool != selectedTool)) {
        builtBezier->setCurveFinished(true);
        clearSelection();
    }
    selectedToolAction = actionButton;
    {
        KnobIPtr parentKnob = actionButton->getParentKnob();
        KnobGroupPtr parentGroup = std::dynamic_pointer_cast<KnobGroup>(parentKnob);
        assert(parentGroup);
        selectedToolRole = parentGroup;
    }
    selectedTool = tool;
    onToolSelected(actionButton, tool);

    return true;
} // RotoCurveInteract::onToolChangedInternal

KnobPagePtr
RotoCurveInteract::createToolbarKnobs(int toolSet)
{
    KnobPagePtr toolbar = AppManager::createKnob<KnobPage>(_host, std::string(kRotoUIParamToolbar));
    toolbar->setAsToolBar(true);
    toolbar->setEvaluateOnChange(false);
    toolbar->setSecretByDefault(true);
    toolbarPage = toolbar;

    if (toolSet & eRotoCurveToolSetSelection) {
        KnobGroupPtr selectionToolButton = AppManager::createKnob<KnobGroup>(_host, tr(kRotoUIParamSelectionToolButtonLabel));
        selectionToolButton->setName(kRotoUIParamSelectionToolButton);
        selectionToolButton->setAsToolButton(true);
        selectionToolButton->setEvaluateOnChange(false);
        selectionToolButton->setSecretByDefault(true);
        selectionToolButton->setInViewerContextCanHaveShortcut(true);
        selectionToolButton->setIsPersistent(false);
        toolbar->addKnob(selectionToolButton);
        selectToolGroup = selectionToolButton;

        {
            KnobButtonPtr tool = AppManager::createKnob<KnobButton>(_host, tr(kRotoUIParamSelectAllToolButtonActionLabel));
            tool->setName(kRotoUIParamSelectAllToolButtonAction);
            tool->setHintToolTip(tr(kRotoUIParamSelectAllToolButtonActionHint));
            tool->setCheckable(true);
            tool->setDefaultValue(true);
            tool->setSecretByDefault(true);
            tool->setEvaluateOnChange(false);
            tool->setIconLabel(NATRON_ICONS_PATH "lucide/mouse-pointer-2.svg");
            tool->setIsPersistent(false);
            selectionToolButton->addKnob(tool);
            selectAllAction = tool;
        }
        {
            KnobButtonPtr tool = AppManager::createKnob<KnobButton>(_host, tr(kRotoUIParamSelectPointsToolButtonActionLabel));
            tool->setName(kRotoUIParamSelectPointsToolButtonAction);
            tool->setHintToolTip(tr(kRotoUIParamSelectPointsToolButtonActionHint));
            tool->setCheckable(true);
            tool->setDefaultValue(false);
            tool->setSecretByDefault(true);
            tool->setEvaluateOnChange(false);
            tool->setIconLabel(NATRON_ICONS_PATH "natron/select-points.svg");
            tool->setIsPersistent(false);
            selectionToolButton->addKnob(tool);
            selectPointsAction = tool;
        }
        {
            KnobButtonPtr tool = AppManager::createKnob<KnobButton>(_host, tr(kRotoUIParamSelectShapesToolButtonActionLabel));
            tool->setName(kRotoUIParamSelectShapesToolButtonAction);
            tool->setHintToolTip(tr(kRotoUIParamSelectShapesToolButtonActionHint));
            tool->setCheckable(true);
            tool->setDefaultValue(false);
            tool->setSecretByDefault(true);
            tool->setEvaluateOnChange(false);
            tool->setIconLabel(NATRON_ICONS_PATH "natron/select-curves.svg");
            tool->setIsPersistent(false);
            selectionToolButton->addKnob(tool);
            selectCurvesAction = tool;
        }
        {
            KnobButtonPtr tool = AppManager::createKnob<KnobButton>(_host, tr(kRotoUIParamSelectFeatherPointsToolButtonActionLabel));
            tool->setName(kRotoUIParamSelectFeatherPointsToolButtonAction);
            tool->setHintToolTip(tr(kRotoUIParamSelectFeatherPointsToolButtonActionHint));
            tool->setCheckable(true);
            tool->setDefaultValue(false);
            tool->setSecretByDefault(true);
            tool->setEvaluateOnChange(false);
            tool->setIconLabel(NATRON_ICONS_PATH "natron/select-feather.svg");
            tool->setIsPersistent(false);
            selectionToolButton->addKnob(tool);
            selectFeatherPointsAction = tool;
        }
    }

    if (toolSet & eRotoCurveToolSetPointsEdition) {
        KnobGroupPtr editPointsToolButton = AppManager::createKnob<KnobGroup>(_host, tr(kRotoUIParamEditPointsToolButtonLabel));
        editPointsToolButton->setName(kRotoUIParamEditPointsToolButton);
        editPointsToolButton->setAsToolButton(true);
        editPointsToolButton->setEvaluateOnChange(false);
        editPointsToolButton->setSecretByDefault(true);
        editPointsToolButton->setInViewerContextCanHaveShortcut(true);
        editPointsToolButton->setIsPersistent(false);
        toolbar->addKnob(editPointsToolButton);
        pointsEditionToolGroup = editPointsToolButton;

        {
            KnobButtonPtr tool = AppManager::createKnob<KnobButton>(_host, tr(kRotoUIParamAddPointsToolButtonActionLabel));
            tool->setName(kRotoUIParamAddPointsToolButtonAction);
            tool->setHintToolTip(tr(kRotoUIParamAddPointsToolButtonActionHint));
            tool->setCheckable(true);
            tool->setDefaultValue(true);
            tool->setSecretByDefault(true);
            tool->setEvaluateOnChange(false);
            tool->setIconLabel(NATRON_ICONS_PATH "natron/add-points.svg");
            tool->setIsPersistent(false);
            editPointsToolButton->addKnob(tool);
            addPointsAction = tool;
        }
        {
            KnobButtonPtr tool = AppManager::createKnob<KnobButton>(_host, tr(kRotoUIParamRemovePointsToolButtonActionLabel));
            tool->setName(kRotoUIParamRemovePointsToolButtonAction);
            tool->setHintToolTip(tr(kRotoUIParamRemovePointsToolButtonAction));
            tool->setCheckable(true);
            tool->setDefaultValue(false);
            tool->setSecretByDefault(true);
            tool->setEvaluateOnChange(false);
            tool->setIconLabel(NATRON_ICONS_PATH "natron/remove-points.svg");
            tool->setIsPersistent(false);
            editPointsToolButton->addKnob(tool);
            removePointsAction = tool;
        }
        {
            KnobButtonPtr tool = AppManager::createKnob<KnobButton>(_host, tr(kRotoUIParamCuspPointsToolButtonActionLabel));
            tool->setName(kRotoUIParamCuspPointsToolButtonAction);
            tool->setHintToolTip(tr(kRotoUIParamCuspPointsToolButtonActionHint));
            tool->setCheckable(true);
            tool->setDefaultValue(false);
            tool->setSecretByDefault(true);
            tool->setEvaluateOnChange(false);
            tool->setIconLabel(NATRON_ICONS_PATH "natron/cusp-point.svg");
            tool->setIsPersistent(false);
            editPointsToolButton->addKnob(tool);
            cuspPointsAction = tool;
        }
        {
            KnobButtonPtr tool = AppManager::createKnob<KnobButton>(_host, tr(kRotoUIParamSmoothPointsToolButtonActionLabel));
            tool->setName(kRotoUIParamSmoothPointsToolButtonAction);
            tool->setHintToolTip(tr(kRotoUIParamSmoothPointsToolButtonActionHint));
            tool->setCheckable(true);
            tool->setDefaultValue(false);
            tool->setSecretByDefault(true);
            tool->setEvaluateOnChange(false);
            tool->setIconLabel(NATRON_ICONS_PATH "lucide/tangent.svg");
            tool->setIsPersistent(false);
            editPointsToolButton->addKnob(tool);
            smoothPointsAction = tool;
        }
        {
            KnobButtonPtr tool = AppManager::createKnob<KnobButton>(_host, tr(kRotoUIParamOpenCloseCurveToolButtonActionLabel));
            tool->setName(kRotoUIParamOpenCloseCurveToolButtonAction);
            tool->setHintToolTip(tr(kRotoUIParamOpenCloseCurveToolButtonActionHint));
            tool->setCheckable(true);
            tool->setDefaultValue(false);
            tool->setSecretByDefault(true);
            tool->setEvaluateOnChange(false);
            tool->setIconLabel(NATRON_ICONS_PATH "natron/open-close-curve.svg");
            tool->setIsPersistent(false);
            editPointsToolButton->addKnob(tool);
            openCloseCurveAction = tool;
        }

        {
            KnobButtonPtr tool = AppManager::createKnob<KnobButton>(_host, tr(kRotoUIParamRemoveFeatherToolButtonAction));
            tool->setName(kRotoUIParamRemoveFeatherToolButtonAction);
            tool->setHintToolTip(tr(kRotoUIParamRemoveFeatherToolButtonActionHint));
            tool->setCheckable(true);
            tool->setDefaultValue(false);
            tool->setSecretByDefault(true);
            tool->setEvaluateOnChange(false);
            tool->setIconLabel(NATRON_ICONS_PATH "natron/remove-feather.svg");
            tool->setIsPersistent(false);
            editPointsToolButton->addKnob(tool);
            removeFeatherAction = tool;
        }
    }

    if (toolSet & eRotoCurveToolSetShapesDrawing) {
        KnobGroupPtr editBezierToolButton = AppManager::createKnob<KnobGroup>(_host, tr(kRotoUIParamBezierEditionToolButtonLabel));
        editBezierToolButton->setName(kRotoUIParamBezierEditionToolButton);
        editBezierToolButton->setAsToolButton(true);
        editBezierToolButton->setEvaluateOnChange(false);
        editBezierToolButton->setSecretByDefault(true);
        editBezierToolButton->setInViewerContextCanHaveShortcut(true);
        editBezierToolButton->setIsPersistent(false);
        toolbar->addKnob(editBezierToolButton);
        bezierEditionToolGroup = editBezierToolButton;

        {
            KnobButtonPtr tool = AppManager::createKnob<KnobButton>(_host, tr(kRotoUIParamDrawBezierToolButtonActionLabel));
            tool->setName(kRotoUIParamDrawBezierToolButtonAction);
            tool->setHintToolTip(tr(kRotoUIParamDrawBezierToolButtonActionHint));
            tool->setCheckable(true);
            tool->setDefaultValue(true);
            tool->setSecretByDefault(true);
            tool->setEvaluateOnChange(false);
            tool->setIconLabel(NATRON_ICONS_PATH "lucide/pen-tool.svg");
            tool->setIsPersistent(false);
            editBezierToolButton->addKnob(tool);
            drawBezierAction = tool;
        }

        {
            KnobButtonPtr tool = AppManager::createKnob<KnobButton>(_host, tr(kRotoUIParamDrawEllipseToolButtonActionLabel));
            tool->setName(kRotoUIParamDrawEllipseToolButtonAction);
            tool->setHintToolTip(tr(kRotoUIParamDrawEllipseToolButtonActionHint));
            tool->setCheckable(true);
            tool->setDefaultValue(false);
            tool->setSecretByDefault(true);
            tool->setEvaluateOnChange(false);
            tool->setIconLabel(NATRON_ICONS_PATH "natron/ellipse-handles.svg");
            tool->setIsPersistent(false);
            editBezierToolButton->addKnob(tool);
            drawEllipseAction = tool;
        }

        {
            KnobButtonPtr tool = AppManager::createKnob<KnobButton>(_host, tr(kRotoUIParamDrawRectangleToolButtonActionLabel));
            tool->setName(kRotoUIParamDrawRectangleToolButtonAction);
            tool->setHintToolTip(tr(kRotoUIParamDrawRectangleToolButtonActionHint));
            tool->setCheckable(true);
            tool->setDefaultValue(false);
            tool->setSecretByDefault(true);
            tool->setEvaluateOnChange(false);
            tool->setIconLabel(NATRON_ICONS_PATH "natron/rectangle-handles.svg");
            tool->setIsPersistent(false);
            editBezierToolButton->addKnob(tool);
            drawRectangleAction = tool;
        }
    }

    return toolbar;
} // RotoCurveInteract::createToolbarKnobs

void
RotoCurveInteract::createViewerButtonKnobs(const KnobPagePtr& page)
{
    KnobButtonPtr autoKeyingEnabled = AppManager::createKnob<KnobButton>(_host, tr(kRotoUIParamAutoKeyingEnabledLabel));
    autoKeyingEnabled->setName(kRotoUIParamAutoKeyingEnabled);
    autoKeyingEnabled->setHintToolTip(tr(kRotoUIParamAutoKeyingEnabledHint));
    autoKeyingEnabled->setEvaluateOnChange(false);
    autoKeyingEnabled->setCheckable(true);
    autoKeyingEnabled->setDefaultValue(true);
    autoKeyingEnabled->setSecretByDefault(true);
    autoKeyingEnabled->setInViewerContextCanHaveShortcut(true);
    autoKeyingEnabled->setIconLabel(NATRON_ICONS_PATH "lucide/timer.svg");
    page->addKnob(autoKeyingEnabled);
    autoKeyingEnabledButton = autoKeyingEnabled;

    KnobButtonPtr featherLinkEnabled = AppManager::createKnob<KnobButton>(_host, tr(kRotoUIParamFeatherLinkEnabledLabel));
    featherLinkEnabled->setName(kRotoUIParamFeatherLinkEnabled);
    featherLinkEnabled->setCheckable(true);
    featherLinkEnabled->setEvaluateOnChange(false);
    featherLinkEnabled->setDefaultValue(true);
    featherLinkEnabled->setSecretByDefault(true);
    featherLinkEnabled->setInViewerContextCanHaveShortcut(true);
    featherLinkEnabled->setHintToolTip(tr(kRotoUIParamFeatherLinkEnabledHint));
    featherLinkEnabled->setIconLabel(NATRON_ICONS_PATH "lucide/link.svg");
    page->addKnob(featherLinkEnabled);
    featherLinkEnabledButton = featherLinkEnabled;

    KnobButtonPtr displayFeatherEnabled = AppManager::createKnob<KnobButton>(_host, tr(kRotoUIParamDisplayFeatherLabel));
    displayFeatherEnabled->setName(kRotoUIParamDisplayFeather);
    displayFeatherEnabled->setHintToolTip(tr(kRotoUIParamDisplayFeatherHint));
    displayFeatherEnabled->setEvaluateOnChange(false);
    displayFeatherEnabled->setCheckable(true);
    displayFeatherEnabled->setDefaultValue(true);
    displayFeatherEnabled->setSecretByDefault(true);
    displayFeatherEnabled->setInViewerContextCanHaveShortcut(true);
    _host->addOverlaySlaveParam(displayFeatherEnabled);
    displayFeatherEnabled->setIconLabel(NATRON_ICONS_PATH "lucide/feather.svg");
    page->addKnob(displayFeatherEnabled);
    displayFeatherEnabledButton = displayFeatherEnabled;

    KnobButtonPtr stickySelection = AppManager::createKnob<KnobButton>(_host, tr(kRotoUIParamStickySelectionEnabledLabel));
    stickySelection->setName(kRotoUIParamStickySelectionEnabled);
    stickySelection->setHintToolTip(tr(kRotoUIParamStickySelectionEnabledHint));
    stickySelection->setEvaluateOnChange(false);
    stickySelection->setCheckable(true);
    stickySelection->setDefaultValue(false);
    stickySelection->setSecretByDefault(true);
    stickySelection->setInViewerContextCanHaveShortcut(true);
    stickySelection->setIconLabel(NATRON_ICONS_PATH "lucide/pin.svg");
    page->addKnob(stickySelection);
    stickySelectionEnabledButton = stickySelection;

    KnobButtonPtr bboxClickAnywhere = AppManager::createKnob<KnobButton>(_host, tr(kRotoUIParamStickyBboxLabel));
    bboxClickAnywhere->setName(kRotoUIParamStickyBbox);
    bboxClickAnywhere->setHintToolTip(tr(kRotoUIParamStickyBboxHint));
    bboxClickAnywhere->setEvaluateOnChange(false);
    bboxClickAnywhere->setCheckable(true);
    bboxClickAnywhere->setDefaultValue(false);
    bboxClickAnywhere->setSecretByDefault(true);
    bboxClickAnywhere->setInViewerContextCanHaveShortcut(true);
    bboxClickAnywhere->setIconLabel(NATRON_ICONS_PATH "lucide/square-mouse-pointer.svg");
    page->addKnob(bboxClickAnywhere);
    bboxClickAnywhereButton = bboxClickAnywhere;

    KnobButtonPtr rippleEditEnabled = AppManager::createKnob<KnobButton>(_host, tr(kRotoUIParamRippleEditLabel));
    rippleEditEnabled->setName(kRotoUIParamRippleEdit);
    rippleEditEnabled->setHintToolTip(tr(kRotoUIParamRippleEditLabelHint));
    rippleEditEnabled->setEvaluateOnChange(false);
    rippleEditEnabled->setCheckable(true);
    rippleEditEnabled->setDefaultValue(false);
    rippleEditEnabled->setSecretByDefault(true);
    rippleEditEnabled->setInViewerContextCanHaveShortcut(true);
    rippleEditEnabled->setIconLabel(NATRON_ICONS_PATH "lucide/waves-horizontal.svg");
    page->addKnob(rippleEditEnabled);
    rippleEditEnabledButton = rippleEditEnabled;

    KnobButtonPtr addKeyframe = AppManager::createKnob<KnobButton>(_host, tr(kRotoUIParamAddKeyFrameLabel));
    addKeyframe->setName(kRotoUIParamAddKeyFrame);
    addKeyframe->setEvaluateOnChange(false);
    addKeyframe->setHintToolTip(tr(kRotoUIParamAddKeyFrameHint));
    addKeyframe->setSecretByDefault(true);
    addKeyframe->setInViewerContextCanHaveShortcut(true);
    addKeyframe->setIconLabel(NATRON_ICONS_PATH "lucide/diamond-plus.svg");
    page->addKnob(addKeyframe);
    addKeyframeButton = addKeyframe;

    KnobButtonPtr removeKeyframe = AppManager::createKnob<KnobButton>(_host, tr(kRotoUIParamRemoveKeyframeLabel));
    removeKeyframe->setName(kRotoUIParamRemoveKeyframe);
    removeKeyframe->setHintToolTip(tr(kRotoUIParamRemoveKeyframeHint));
    removeKeyframe->setEvaluateOnChange(false);
    removeKeyframe->setSecretByDefault(true);
    removeKeyframe->setInViewerContextCanHaveShortcut(true);
    removeKeyframe->setIconLabel(NATRON_ICONS_PATH "lucide/diamond-minus.svg");
    page->addKnob(removeKeyframe);
    removeKeyframeButton = removeKeyframe;

    KnobButtonPtr showTransform = AppManager::createKnob<KnobButton>(_host, tr(kRotoUIParamShowTransformLabel));
    showTransform->setName(kRotoUIParamShowTransform);
    showTransform->setHintToolTip(tr(kRotoUIParamShowTransformHint));
    showTransform->setEvaluateOnChange(false);
    showTransform->setSecretByDefault(true);
    showTransform->setCheckable(true);
    showTransform->setDefaultValue(true);
    showTransform->setInViewerContextCanHaveShortcut(true);
    showTransform->setInViewerContextNewLineActivated(true);
    showTransform->setIconLabel(NATRON_ICONS_PATH "lucide/move.svg");
    page->addKnob(showTransform);
    _host->addOverlaySlaveParam(showTransform);
    showTransformHandle = showTransform;

    _host->addKnobToViewerUI(autoKeyingEnabled);
    _host->addKnobToViewerUI(featherLinkEnabled);
    _host->addKnobToViewerUI(displayFeatherEnabled);
    _host->addKnobToViewerUI(stickySelection);
    _host->addKnobToViewerUI(bboxClickAnywhere);
    _host->addKnobToViewerUI(rippleEditEnabled);
    _host->addKnobToViewerUI(addKeyframe);
    _host->addKnobToViewerUI(removeKeyframe);
    _host->addKnobToViewerUI(showTransform);
} // RotoCurveInteract::createViewerButtonKnobs

void
RotoCurveInteract::createRightClickMenuKnobs(const KnobPagePtr& page)
{
    KnobChoicePtr rightClickMenu = AppManager::createKnob<KnobChoice>(_host, std::string(kRotoUIParamRightClickMenu));
    rightClickMenu->setSecretByDefault(true);
    rightClickMenu->setEvaluateOnChange(false);
    page->addKnob(rightClickMenu);
    rightClickMenuKnob = rightClickMenu;
    {
        KnobButtonPtr action = AppManager::createKnob<KnobButton>(_host, tr(kRotoUIParamRightClickMenuActionRemoveItemsLabel));
        action->setName(kRotoUIParamRightClickMenuActionRemoveItems);
        action->setSecretByDefault(true);
        action->setEvaluateOnChange(false);
        action->setInViewerContextCanHaveShortcut(true);
        _host->addOverlaySlaveParam(action);
        page->addKnob(action);
        removeItemsMenuAction = action;
    }
    {
        KnobButtonPtr action = AppManager::createKnob<KnobButton>(_host, tr(kRotoUIParamRightClickMenuActionCuspItemsLabel));
        action->setName(kRotoUIParamRightClickMenuActionCuspItems);
        action->setEvaluateOnChange(false);
        action->setSecretByDefault(true);
        action->setInViewerContextCanHaveShortcut(true);
        _host->addOverlaySlaveParam(action);
        page->addKnob(action);
        cuspItemMenuAction = action;
    }
    {
        KnobButtonPtr action = AppManager::createKnob<KnobButton>(_host, tr(kRotoUIParamRightClickMenuActionSmoothItemsLabel));
        action->setName(kRotoUIParamRightClickMenuActionSmoothItems);
        action->setSecretByDefault(true);
        action->setEvaluateOnChange(false);
        action->setInViewerContextCanHaveShortcut(true);
        _host->addOverlaySlaveParam(action);
        page->addKnob(action);
        smoothItemMenuAction = action;
    }
    {
        KnobButtonPtr action = AppManager::createKnob<KnobButton>(_host, tr(kRotoUIParamRightClickMenuActionRemoveItemsFeatherLabel));
        action->setName(kRotoUIParamRightClickMenuActionRemoveItemsFeather);
        action->setSecretByDefault(true);
        action->setEvaluateOnChange(false);
        action->setInViewerContextCanHaveShortcut(true);
        _host->addOverlaySlaveParam(action);
        page->addKnob(action);
        removeItemFeatherMenuAction = action;
    }
    {
        KnobButtonPtr action = AppManager::createKnob<KnobButton>(_host, tr(kRotoUIParamRightClickMenuActionNudgeBottomLabel));
        action->setName(kRotoUIParamRightClickMenuActionNudgeBottom);
        action->setSecretByDefault(true);
        action->setEvaluateOnChange(false);
        action->setInViewerContextCanHaveShortcut(true);
        _host->addOverlaySlaveParam(action);
        page->addKnob(action);
        nudgeBottomMenuAction = action;
    }
    {
        KnobButtonPtr action = AppManager::createKnob<KnobButton>(_host, tr(kRotoUIParamRightClickMenuActionNudgeLeftLabel));
        action->setName(kRotoUIParamRightClickMenuActionNudgeLeft);
        action->setSecretByDefault(true);
        action->setEvaluateOnChange(false);
        action->setInViewerContextCanHaveShortcut(true);
        _host->addOverlaySlaveParam(action);
        page->addKnob(action);
        nudgeLeftMenuAction = action;
    }
    {
        KnobButtonPtr action = AppManager::createKnob<KnobButton>(_host, tr(kRotoUIParamRightClickMenuActionNudgeTopLabel));
        action->setName(kRotoUIParamRightClickMenuActionNudgeTop);
        action->setSecretByDefault(true);
        action->setEvaluateOnChange(false);
        action->setInViewerContextCanHaveShortcut(true);
        _host->addOverlaySlaveParam(action);
        page->addKnob(action);
        nudgeTopMenuAction = action;
    }
    {
        KnobButtonPtr action = AppManager::createKnob<KnobButton>(_host, tr(kRotoUIParamRightClickMenuActionNudgeRightLabel));
        action->setName(kRotoUIParamRightClickMenuActionNudgeRight);
        action->setSecretByDefault(true);
        action->setEvaluateOnChange(false);
        action->setInViewerContextCanHaveShortcut(true);
        _host->addOverlaySlaveParam(action);
        page->addKnob(action);
        nudgeRightMenuAction = action;
    }
    {
        KnobButtonPtr action = AppManager::createKnob<KnobButton>(_host, tr(kRotoUIParamRightClickMenuActionSelectAllLabel));
        action->setName(kRotoUIParamRightClickMenuActionSelectAll);
        action->setSecretByDefault(true);
        action->setEvaluateOnChange(false);
        action->setInViewerContextCanHaveShortcut(true);
        page->addKnob(action);
        selectAllMenuAction = action;
    }
    {
        KnobButtonPtr action = AppManager::createKnob<KnobButton>(_host, tr(kRotoUIParamRightClickMenuActionOpenCloseLabel));
        action->setName(kRotoUIParamRightClickMenuActionOpenClose);
        action->setSecretByDefault(true);
        action->setEvaluateOnChange(false);
        action->setInViewerContextCanHaveShortcut(true);
        _host->addOverlaySlaveParam(action);
        page->addKnob(action);
        openCloseMenuAction = action;
    }
    {
        KnobButtonPtr action = AppManager::createKnob<KnobButton>(_host, tr(kRotoUIParamRightClickMenuActionLockShapesLabel));
        action->setName(kRotoUIParamRightClickMenuActionLockShapes);
        action->setSecretByDefault(true);
        action->setEvaluateOnChange(false);
        action->setInViewerContextCanHaveShortcut(true);
        _host->addOverlaySlaveParam(action);
        page->addKnob(action);
        lockShapeMenuAction = action;
    }
} // RotoCurveInteract::createRightClickMenuKnobs

void
RotoCurveInteract::connectToCurveContext()
{
    RotoCurveContextPtr ctx = getCurveContext();
    assert(ctx);
    std::weak_ptr<RotoCurveInteract> weakThis = shared_from_this();

    QObject::connect(ctx.get(), &RotoCurveContext::refreshViewerOverlays, _host, [weakThis]() {
        if (RotoCurveInteractPtr ui = weakThis.lock()) {
            ui->redrawOverlays();
        }
    });
    QObject::connect(ctx.get(), &RotoCurveContext::selectionChanged, _host, [weakThis](int reason) {
        if (RotoCurveInteractPtr ui = weakThis.lock()) {
            ui->onCurveSelectionChanged(reason);
        }
    });
    QObject::connect(ctx.get(), &RotoCurveContext::itemLockedChanged, _host, [weakThis](int reason) {
        if (RotoCurveInteractPtr ui = weakThis.lock()) {
            ui->onCurveLockedChanged(reason);
        }
    });
}

void
RotoCurveInteract::onCurveSelectionChanged(int reason)
{
    if ((RotoItem::SelectionReasonEnum)reason != RotoItem::eSelectionReasonOverlayInteract) {
        selectedItems = getCurveContext()->getSelectedCurves();
        redrawOverlays();
    }
}

void
RotoCurveInteract::onCurveLockedChanged(int reason)
{
    RotoItemPtr item = getCurveContext()->getLastItemLocked();

    if (item && ((RotoItem::SelectionReasonEnum)reason != RotoItem::eSelectionReasonOverlayInteract)) {
        bool changed = false;
        onCurveLockedChangedRecursive(item, &changed);
        if (changed) {
            redrawOverlays();
        }
    }
}

void
RotoCurveInteract::refreshAfterTimeChanged(double /*time*/)
{
    computeSelectedCpsBBOX();
}

void
RotoCurveInteract::getViewerButtonShortcuts(std::list<PluginActionShortcut>* shortcuts)
{
    shortcuts->push_back(PluginActionShortcut(kRotoUIParamAutoKeyingEnabled, kRotoUIParamAutoKeyingEnabledLabel));
    shortcuts->push_back(PluginActionShortcut(kRotoUIParamFeatherLinkEnabled, kRotoUIParamFeatherLinkEnabledLabel));
    shortcuts->push_back(PluginActionShortcut(kRotoUIParamDisplayFeather, kRotoUIParamDisplayFeatherLabel));
    shortcuts->push_back(PluginActionShortcut(kRotoUIParamStickySelectionEnabled, kRotoUIParamStickySelectionEnabledLabel));
    shortcuts->push_back(PluginActionShortcut(kRotoUIParamStickyBbox, kRotoUIParamStickyBboxLabel));
    shortcuts->push_back(PluginActionShortcut(kRotoUIParamRippleEdit, kRotoUIParamRippleEditLabel));
    shortcuts->push_back(PluginActionShortcut(kRotoUIParamAddKeyFrame, kRotoUIParamAddKeyFrameLabel));
    shortcuts->push_back(PluginActionShortcut(kRotoUIParamRemoveKeyframe, kRotoUIParamRemoveKeyframeLabel));
    shortcuts->push_back(PluginActionShortcut(kRotoUIParamShowTransform, kRotoUIParamShowTransformLabel, Key_T));
}

void
RotoCurveInteract::getToolbarShortcuts(int toolSet,
                                       std::list<PluginActionShortcut>* shortcuts)
{
    if (toolSet & eRotoCurveToolSetSelection) {
        shortcuts->push_back(PluginActionShortcut(kRotoUIParamSelectionToolButton, kRotoUIParamSelectionToolButtonLabel, Key_Q));
    }
    if (toolSet & eRotoCurveToolSetPointsEdition) {
        shortcuts->push_back(PluginActionShortcut(kRotoUIParamEditPointsToolButton, kRotoUIParamEditPointsToolButtonLabel, Key_D));
    }
    if (toolSet & eRotoCurveToolSetShapesDrawing) {
        shortcuts->push_back(PluginActionShortcut(kRotoUIParamBezierEditionToolButton, kRotoUIParamBezierEditionToolButtonLabel, Key_V));
    }
}

void
RotoCurveInteract::getRightClickMenuShortcuts(std::list<PluginActionShortcut>* shortcuts)
{
    shortcuts->push_back(PluginActionShortcut(kRotoUIParamRightClickMenuActionRemoveItems, kRotoUIParamRightClickMenuActionRemoveItemsLabel, Key_BackSpace));
    shortcuts->push_back(PluginActionShortcut(kRotoUIParamRightClickMenuActionCuspItems, kRotoUIParamRightClickMenuActionCuspItemsLabel, Key_Z, eKeyboardModifierShift));
    shortcuts->push_back(PluginActionShortcut(kRotoUIParamRightClickMenuActionSmoothItems, kRotoUIParamRightClickMenuActionSmoothItemsLabel, Key_Z));
    shortcuts->push_back(PluginActionShortcut(kRotoUIParamRightClickMenuActionRemoveItemsFeather, kRotoUIParamRightClickMenuActionRemoveItemsFeatherLabel, Key_E, eKeyboardModifierShift));
    shortcuts->push_back(PluginActionShortcut(kRotoUIParamRightClickMenuActionNudgeLeft, kRotoUIParamRightClickMenuActionNudgeLeftLabel, Key_4, eKeyboardModifierKeypad));
    shortcuts->push_back(PluginActionShortcut(kRotoUIParamRightClickMenuActionNudgeBottom, kRotoUIParamRightClickMenuActionNudgeBottomLabel, Key_2, eKeyboardModifierKeypad));
    shortcuts->push_back(PluginActionShortcut(kRotoUIParamRightClickMenuActionNudgeTop, kRotoUIParamRightClickMenuActionNudgeTopLabel, Key_8, eKeyboardModifierKeypad));
    shortcuts->push_back(PluginActionShortcut(kRotoUIParamRightClickMenuActionNudgeRight, kRotoUIParamRightClickMenuActionNudgeRightLabel, Key_6, eKeyboardModifierKeypad));
    shortcuts->push_back(PluginActionShortcut(kRotoUIParamRightClickMenuActionSelectAll, kRotoUIParamRightClickMenuActionSelectAllLabel, Key_A, eKeyboardModifierControl));
    shortcuts->push_back(PluginActionShortcut(kRotoUIParamRightClickMenuActionOpenClose, kRotoUIParamRightClickMenuActionOpenCloseLabel, Key_Return));
    shortcuts->push_back(PluginActionShortcut(kRotoUIParamRightClickMenuActionLockShapes, kRotoUIParamRightClickMenuActionLockShapesLabel, Key_L, eKeyboardModifierShift));
}

bool
RotoCurveInteract::onKnobChanged(KnobI* k,
                                 double time,
                                 bool* ret)
{
    RotoCurveContextPtr ctx = getCurveContext();

    *ret = true;
    KnobIPtr kShared = k->shared_from_this();
    KnobButtonPtr isBtn = std::dynamic_pointer_cast<KnobButton>(kShared);
    KnobGroupPtr isGrp = std::dynamic_pointer_cast<KnobGroup>(kShared);
    if (isBtn && onToolChangedInternal(isBtn)) {
        return true;
    } else if (isGrp && onRoleChangedInternal(isGrp)) {
        return true;
    } else if (k == featherLinkEnabledButton.lock().get()) {
        ctx->onFeatherLinkChanged(featherLinkEnabledButton.lock()->getValue());
    } else if (k == autoKeyingEnabledButton.lock().get()) {
        ctx->onAutoKeyingChanged(autoKeyingEnabledButton.lock()->getValue());
    } else if (k == rippleEditEnabledButton.lock().get()) {
        ctx->onRippleEditChanged(rippleEditEnabledButton.lock()->getValue());
    } else if (k == addKeyframeButton.lock().get()) {
        for (SelectedItems::iterator it = selectedItems.begin(); it != selectedItems.end(); ++it) {
            Bezier* isBezier = dynamic_cast<Bezier*>(it->get());
            if (!isBezier) {
                continue;
            }
            isBezier->setKeyframe(time);
        }
        getCurveContext()->evaluateChange();
    } else if (k == removeKeyframeButton.lock().get()) {
        for (SelectedItems::iterator it = selectedItems.begin(); it != selectedItems.end(); ++it) {
            Bezier* isBezier = dynamic_cast<Bezier*>(it->get());
            if (!isBezier) {
                continue;
            }
            isBezier->removeKeyframe(time);
        }
        getCurveContext()->evaluateChange();
    } else if (k == removeItemsMenuAction.lock().get()) {
        /// if control points are selected, delete them, otherwise delete the selected beziers
        if (!selectedCps.empty()) {
            pushUndoCommand(new RemovePointUndoCommand(shared_from_this(), selectedCps));
        } else if (!selectedItems.empty()) {
            pushUndoCommand(new RemoveCurveUndoCommand(shared_from_this(), selectedItems));
        }
    } else if (k == smoothItemMenuAction.lock().get()) {
        if (!smoothSelectedCurve()) {
            *ret = false;
        }
    } else if (k == cuspItemMenuAction.lock().get()) {
        if (!cuspSelectedCurve()) {
            *ret = false;
        }
    } else if (k == removeItemFeatherMenuAction.lock().get()) {
        if (!removeFeatherForSelectedCurve()) {
            *ret = false;
        }
    } else if (k == nudgeLeftMenuAction.lock().get()) {
        if (!moveSelectedCpsWithKeyArrows(-1, 0)) {
            *ret = false;
        }
    } else if (k == nudgeRightMenuAction.lock().get()) {
        if (!moveSelectedCpsWithKeyArrows(1, 0)) {
            *ret = false;
        }
    } else if (k == nudgeBottomMenuAction.lock().get()) {
        if (!moveSelectedCpsWithKeyArrows(0, -1)) {
            *ret = false;
        }
    } else if (k == nudgeTopMenuAction.lock().get()) {
        if (!moveSelectedCpsWithKeyArrows(0, 1)) {
            *ret = false;
        }
    } else if (k == selectAllMenuAction.lock().get()) {
        iSelectingwithCtrlA = true;
        /// if no bezier are selected, select all beziers
        if (selectedItems.empty()) {
            std::list<RotoDrawableItemPtr> bez = ctx->getCurvesByRenderOrder();
            for (std::list<RotoDrawableItemPtr>::const_iterator it = bez.begin(); it != bez.end(); ++it) {
                ctx->select(*it, RotoItem::eSelectionReasonOverlayInteract);
                selectedItems.push_back(*it);
            }
        } else {
            /// select all the control points of all selected beziers
            selectedCps.clear();
            for (SelectedItems::iterator it = selectedItems.begin(); it != selectedItems.end(); ++it) {
                Bezier* isBezier = dynamic_cast<Bezier*>(it->get());
                if (!isBezier) {
                    continue;
                }
                const std::list<BezierCPPtr>& cps = isBezier->getControlPoints();
                const std::list<BezierCPPtr>& fps = isBezier->getFeatherPoints();
                assert(cps.size() == fps.size());

                std::list<BezierCPPtr>::const_iterator cpIT = cps.begin();
                for (std::list<BezierCPPtr>::const_iterator fpIT = fps.begin(); fpIT != fps.end(); ++fpIT, ++cpIT) {
                    selectedCps.push_back(std::make_pair(*cpIT, *fpIT));
                }
            }
            computeSelectedCpsBBOX();
        }
    } else if (k == openCloseMenuAction.lock().get()) {
        if (((selectedTool == eRotoToolDrawBezier) || (selectedTool == eRotoToolOpenBezier)) && builtBezier && !builtBezier->isCurveFinished()) {
            pushUndoCommand(new OpenCloseUndoCommand(shared_from_this(), builtBezier));

            builtBezier.reset();
            selectedCps.clear();
            setCurrentTool(selectAllAction.lock());
            getCurveContext()->evaluateChange();
        }
    } else if (k == lockShapeMenuAction.lock().get()) {
        lockSelectedCurves();
    } else {
        return false;
    }

    return true;
} // RotoCurveInteract::onKnobChanged

void
RotoCurveInteract::setCurrentTool(const KnobButtonPtr& tool)
{
    if (!tool) {
        return;
    }
    KnobIPtr parentKnob = tool->getParentKnob();
    KnobGroupPtr parentGroup = std::dynamic_pointer_cast<KnobGroup>(parentKnob);
    assert(parentGroup);
    if (!parentGroup) {
        return;
    }

    RotoToolEnum toolEnum;
    getToolForAction(tool, &toolEnum);

    RotoRoleEnum roleEnum;
    getRoleForGroup(parentGroup, &roleEnum);

    KnobGroupPtr curGroup = selectedToolRole.lock();
    KnobButtonPtr curToolButton = selectedToolAction.lock();
    if (curGroup && (curGroup != parentGroup)) {
        curGroup->setValue(false);
    }

    // If we changed group, just keep this action on
    if (curToolButton && (curGroup == parentGroup)) {
        curToolButton->setValue(false);
    }
    selectedToolAction = tool;
    selectedToolRole = parentGroup;
    if (parentGroup != curGroup) {
        if (parentGroup->getValue() != true) {
            parentGroup->setValue(true);
        } else {
            onRoleChangedInternal(parentGroup);
        }
    }
    if (tool->getValue() != true) {
        tool->setValue(true);
    } else {
        // We must notify of the change
        onToolChangedInternal(tool);
    }
}

void
RotoCurveInteract::computeSelectedCpsBBOX()
{
    NodePtr n = _host->getNode();

    if (!n || !n->isActivated()) {
        return;
    }

    double time = _host->getCurrentTime();

    double l = std::numeric_limits<int>::max(), r = std::numeric_limits<int>::min();
    double b = std::numeric_limits<int>::max(), t = std::numeric_limits<int>::min();
    for (SelectedCPs::iterator it = selectedCps.begin(); it != selectedCps.end(); ++it) {
        handleControlPointMaximum(time, *(it->first), &l, &b, &r, &t);
        if (it->second) {
            handleControlPointMaximum(time, *(it->second), &l, &b, &r, &t);
        }
    }
    selectedCpsBbox.setCoords(l, t, r, b);
    if (selectedCps.size() > 1) {
        showCpsBbox = true;
    } else {
        showCpsBbox = false;
    }
}

QPointF
RotoCurveInteract::getSelectedCpsBBOXCenter()
{
    return selectedCpsBbox.center();
}

void
RotoCurveInteract::handleBezierSelection(const BezierPtr& curve)
{
    /// find out if the bezier is already selected.
    bool found = false;

    for (SelectedItems::iterator it = selectedItems.begin(); it != selectedItems.end(); ++it) {
        if (it->get() == curve.get()) {
            found = true;
            break;
        }
    }

    if (!found) {
        /// clear previous selection if the SHIFT modifier isn't held
        if (!shiftDown) {
            clearBeziersSelection();
        }
        selectedItems.push_back(curve);

        RotoCurveContextPtr ctx = getCurveContext();
        assert(ctx);
        ctx->select(curve, RotoItem::eSelectionReasonOverlayInteract);
    }
}

void
RotoCurveInteract::handleControlPointSelection(const std::pair<BezierCPPtr,
                                                               BezierCPPtr>& p)
{
    /// find out if the cp is already selected.
    SelectedCPs::iterator foundCP = selectedCps.end();

    for (SelectedCPs::iterator it = selectedCps.begin(); it != selectedCps.end(); ++it) {
        if (p.first == it->first) {
            foundCP = it;
            break;
        }
    }

    if (foundCP == selectedCps.end()) {
        /// clear previous selection if the SHIFT modifier isn't held
        if (!shiftDown) {
            selectedCps.clear();
        }
        selectedCps.push_back(p);
        computeSelectedCpsBBOX();
    } else {
        /// Erase the point from the selection to allow the user to toggle the selection
        if (shiftDown) {
            selectedCps.erase(foundCP);
            computeSelectedCpsBBOX();
        }
    }

    cpBeingDragged = p;
    state = eEventStateDraggingControlPoint;
    evaluateOnPenUp = true; // so that color is restored to normal on pen up, even if there's no motion
}

void
RotoCurveInteract::showMenuForControlPoint(const BezierCPPtr& /*cp*/)
{
    KnobChoicePtr menu = rightClickMenuKnob.lock();

    if (!menu) {
        return;
    }
    std::vector<KnobButtonPtr> menuKnobs;

    menuKnobs.push_back(removeItemsMenuAction.lock());
    menuKnobs.push_back(smoothItemMenuAction.lock());
    menuKnobs.push_back(cuspItemMenuAction.lock());
    menuKnobs.push_back(removeItemFeatherMenuAction.lock());
    menuKnobs.push_back(nudgeLeftMenuAction.lock());
    menuKnobs.push_back(nudgeBottomMenuAction.lock());
    menuKnobs.push_back(nudgeRightMenuAction.lock());
    menuKnobs.push_back(nudgeTopMenuAction.lock());

    std::vector<ChoiceOption> choices(menuKnobs.size());
    for (std::size_t i = 0; i < menuKnobs.size(); ++i) {
        choices[i].id = menuKnobs[i]->getName();
        choices[i].tooltip = menuKnobs[i]->getHintToolTip();
    }
    menu->populateChoices(choices);

} // showMenuForControlPoint

void
RotoCurveInteract::showMenuForCurve(const BezierPtr& curve)
{
    KnobChoicePtr menu = rightClickMenuKnob.lock();

    if (!menu) {
        return;
    }
    std::vector<KnobButtonPtr> menuKnobs;

    menuKnobs.push_back(selectAllMenuAction.lock());
    menuKnobs.push_back(removeItemsMenuAction.lock());
    if (!curve->isOpenBezier()) {
        menuKnobs.push_back(openCloseCurveAction.lock());
    }

    menuKnobs.push_back(smoothItemMenuAction.lock());
    menuKnobs.push_back(cuspItemMenuAction.lock());
    if (!curve->isOpenBezier()) {
        menuKnobs.push_back(removeItemFeatherMenuAction.lock());
    }
    menuKnobs.push_back(lockShapeMenuAction.lock());

    std::vector<ChoiceOption> choices(menuKnobs.size());
    for (std::size_t i = 0; i < menuKnobs.size(); ++i) {
        choices[i].id = menuKnobs[i]->getName();
        choices[i].tooltip = menuKnobs[i]->getHintToolTip();
    }

    menu->populateChoices(choices);
} // showMenuForCurve

bool
RotoCurveInteract::isNearbySelectedCpsCrossHair(const QPointF& pos) const
{
    std::pair<double, double> pixelScale;

    _host->getCurrentViewportForOverlays()->getPixelScale(pixelScale.first, pixelScale.second);

    double xHairMidSizeX = kXHairSelectedCpsBox * pixelScale.first;
    double xHairMidSizeY = kXHairSelectedCpsBox * pixelScale.second;
    double l = selectedCpsBbox.topLeft().x();
    double r = selectedCpsBbox.bottomRight().x();
    double b = selectedCpsBbox.bottomRight().y();
    double t = selectedCpsBbox.topLeft().y();
    double toleranceX = kXHairSelectedCpsTolerance * pixelScale.first;
    double toleranceY = kXHairSelectedCpsTolerance * pixelScale.second;
    double midX = (l + r) / 2.;
    double midY = (b + t) / 2.;
    double lCross = midX - xHairMidSizeX;
    double rCross = midX + xHairMidSizeX;
    double bCross = midY - xHairMidSizeY;
    double tCross = midY + xHairMidSizeY;

    if ((pos.x() >= (lCross - toleranceX)) && (pos.x() <= (rCross + toleranceX)) && (pos.y() <= (tCross + toleranceY)) && (pos.y() >= (bCross - toleranceY))) {
        return true;
    } else {
        return false;
    }
}

bool
RotoCurveInteract::isWithinSelectedCpsBBox(const QPointF& pos) const
{
    //   std::pair<double, double> pixelScale;
    //    viewer->getPixelScale(pixelScale.first,pixelScale.second);

    double l = selectedCpsBbox.topLeft().x();
    double r = selectedCpsBbox.bottomRight().x();
    double b = selectedCpsBbox.bottomRight().y();
    double t = selectedCpsBbox.topLeft().y();
    double toleranceX = 0; // kXHairSelectedCpsTolerance * pixelScale.first;
    double toleranceY = 0; // kXHairSelectedCpsTolerance * pixelScale.second;

    return pos.x() > (l - toleranceX) && pos.x() < (r + toleranceX) && pos.y() > (b - toleranceY) && pos.y() < (t + toleranceY);
}

bool
RotoCurveInteract::isNearbyBBoxTopLeft(const QPointF& p,
                                       double tolerance,
                                       const std::pair<double, double>& pixelScale) const
{
    QPointF corner = selectedCpsBbox.topLeft();

    if ((p.x() >= (corner.x() - tolerance)) && (p.x() <= (corner.x() + tolerance)) && (p.y() >= (corner.y() - tolerance)) && (p.y() <= (corner.y() + tolerance))) {
        return true;
    } else {
        double halfOffset = kTransformArrowOffsetFromPoint * pixelScale.first / 2.;
        double length = kTransformArrowLenght * pixelScale.first;
        double halfLength = length / 2.;
        ;
        /// test if pos is within the arrow bounding box
        QPointF center(corner.x() - halfOffset, corner.y() + halfOffset);
        RectD arrowBbox(center.x() - halfLength, center.y() - halfLength, center.x() + halfLength, center.y() + halfLength);

        return arrowBbox.contains(p.x(), p.y());
    }
}

bool
RotoCurveInteract::isNearbyBBoxTopRight(const QPointF& p,
                                        double tolerance,
                                        const std::pair<double, double>& pixelScale) const
{
    QPointF topLeft = selectedCpsBbox.topLeft();
    QPointF btmRight = selectedCpsBbox.bottomRight();
    QPointF corner(btmRight.x(), topLeft.y());

    if ((p.x() >= (corner.x() - tolerance)) && (p.x() <= (corner.x() + tolerance)) && (p.y() >= (corner.y() - tolerance)) && (p.y() <= (corner.y() + tolerance))) {
        return true;
    } else {
        double halfOffset = kTransformArrowOffsetFromPoint * pixelScale.first / 2.;
        double length = kTransformArrowLenght * pixelScale.first;
        double halfLength = length / 2.;
        ;
        /// test if pos is within the arrow bounding box
        QPointF center(corner.x() + halfOffset, corner.y() + halfOffset);
        RectD arrowBbox(center.x() - halfLength, center.y() - halfLength, center.x() + halfLength, center.y() + halfLength);

        return arrowBbox.contains(p.x(), p.y());
    }
}

bool
RotoCurveInteract::isNearbyBBoxBtmLeft(const QPointF& p,
                                       double tolerance,
                                       const std::pair<double, double>& pixelScale) const
{
    QPointF topLeft = selectedCpsBbox.topLeft();
    QPointF btmRight = selectedCpsBbox.bottomRight();
    QPointF corner(topLeft.x(), btmRight.y());

    if ((p.x() >= (corner.x() - tolerance)) && (p.x() <= (corner.x() + tolerance)) && (p.y() >= (corner.y() - tolerance)) && (p.y() <= (corner.y() + tolerance))) {
        return true;
    } else {
        double halfOffset = kTransformArrowOffsetFromPoint * pixelScale.first / 2.;
        double length = kTransformArrowLenght * pixelScale.first;
        double halfLength = length / 2.;
        ;
        /// test if pos is within the arrow bounding box
        QPointF center(corner.x() - halfOffset, corner.y() - halfOffset);
        RectD arrowBbox(center.x() - halfLength, center.y() - halfLength, center.x() + halfLength, center.y() + halfLength);

        return arrowBbox.contains(p.x(), p.y());
    }
}

bool
RotoCurveInteract::isNearbyBBoxBtmRight(const QPointF& p,
                                        double tolerance,
                                        const std::pair<double, double>& pixelScale) const
{
    QPointF corner = selectedCpsBbox.bottomRight();

    if ((p.x() >= (corner.x() - tolerance)) && (p.x() <= (corner.x() + tolerance)) && (p.y() >= (corner.y() - tolerance)) && (p.y() <= (corner.y() + tolerance))) {
        return true;
    } else {
        double halfOffset = kTransformArrowOffsetFromPoint * pixelScale.first / 2.;
        double length = kTransformArrowLenght * pixelScale.first;
        double halfLength = length / 2.;
        ;
        /// test if pos is within the arrow bounding box
        QPointF center(corner.x() + halfOffset, corner.y() - halfOffset);
        RectD arrowBbox(center.x() - halfLength, center.y() - halfLength, center.x() + halfLength, center.y() + halfLength);

        return arrowBbox.contains(p.x(), p.y());
    }
}

bool
RotoCurveInteract::isNearbyBBoxMidTop(const QPointF& p,
                                      double tolerance,
                                      const std::pair<double, double>& pixelScale) const
{
    QPointF topLeft = selectedCpsBbox.topLeft();
    QPointF btmRight = selectedCpsBbox.bottomRight();
    QPointF topRight(btmRight.x(), topLeft.y());
    QPointF mid = (topLeft + topRight) / 2.;

    if ((p.x() >= (mid.x() - tolerance)) && (p.x() <= (mid.x() + tolerance)) && (p.y() >= (mid.y() - tolerance)) && (p.y() <= (mid.y() + tolerance))) {
        return true;
    } else {
        double offset = kTransformArrowOffsetFromPoint * pixelScale.first;
        double length = kTransformArrowLenght * pixelScale.first;
        double halfLength = length / 2.;
        /// test if pos is within the arrow bounding box
        QPointF center(mid.x(), mid.y() + offset);
        RectD arrowBbox(center.x() - halfLength, center.y() - halfLength, center.x() + halfLength, center.y() + halfLength);

        return arrowBbox.contains(p.x(), p.y());
    }
}

bool
RotoCurveInteract::isNearbyBBoxMidRight(const QPointF& p,
                                        double tolerance,
                                        const std::pair<double, double>& pixelScale) const
{
    QPointF topLeft = selectedCpsBbox.topLeft();
    QPointF btmRight = selectedCpsBbox.bottomRight();
    QPointF topRight(btmRight.x(), topLeft.y());
    QPointF mid = (btmRight + topRight) / 2.;

    if ((p.x() >= (mid.x() - tolerance)) && (p.x() <= (mid.x() + tolerance)) && (p.y() >= (mid.y() - tolerance)) && (p.y() <= (mid.y() + tolerance))) {
        return true;
    } else {
        double offset = kTransformArrowOffsetFromPoint * pixelScale.first;
        double length = kTransformArrowLenght * pixelScale.first;
        double halfLength = length / 2.;
        ;
        /// test if pos is within the arrow bounding box
        QPointF center(mid.x() + offset, mid.y());
        RectD arrowBbox(center.x() - halfLength, center.y() - halfLength, center.x() + halfLength, center.y() + halfLength);

        return arrowBbox.contains(p.x(), p.y());
    }
}

bool
RotoCurveInteract::isNearbyBBoxMidBtm(const QPointF& p,
                                      double tolerance,
                                      const std::pair<double, double>& pixelScale) const
{
    QPointF topLeft = selectedCpsBbox.topLeft();
    QPointF btmRight = selectedCpsBbox.bottomRight();
    QPointF btmLeft(topLeft.x(), btmRight.y());
    QPointF mid = (btmRight + btmLeft) / 2.;

    if ((p.x() >= (mid.x() - tolerance)) && (p.x() <= (mid.x() + tolerance)) && (p.y() >= (mid.y() - tolerance)) && (p.y() <= (mid.y() + tolerance))) {
        return true;
    } else {
        double offset = kTransformArrowOffsetFromPoint * pixelScale.first;
        double length = kTransformArrowLenght * pixelScale.first;
        double halfLength = length / 2.;
        ;
        /// test if pos is within the arrow bounding box
        QPointF center(mid.x(), mid.y() - offset);
        RectD arrowBbox(center.x() - halfLength, center.y() - halfLength, center.x() + halfLength, center.y() + halfLength);

        return arrowBbox.contains(p.x(), p.y());
    }
}

EventStateEnum
RotoCurveInteract::isMouseInteractingWithCPSBbox(const QPointF& pos,
                                                 double cpSelectionTolerance,
                                                 const std::pair<double, double>& pixelScale) const
{
    bool clickAnywhere = isBboxClickAnywhereEnabled();
    EventStateEnum state = eEventStateNone;

    if (showCpsBbox && isNearbyBBoxTopLeft(pos, cpSelectionTolerance, pixelScale)) {
        state = eEventStateDraggingBBoxTopLeft;
    } else if (showCpsBbox && isNearbyBBoxTopRight(pos, cpSelectionTolerance, pixelScale)) {
        state = eEventStateDraggingBBoxTopRight;
    } else if (showCpsBbox && isNearbyBBoxBtmLeft(pos, cpSelectionTolerance, pixelScale)) {
        state = eEventStateDraggingBBoxBtmLeft;
    } else if (showCpsBbox && isNearbyBBoxBtmRight(pos, cpSelectionTolerance, pixelScale)) {
        state = eEventStateDraggingBBoxBtmRight;
    } else if (showCpsBbox && isNearbyBBoxMidTop(pos, cpSelectionTolerance, pixelScale)) {
        state = eEventStateDraggingBBoxMidTop;
    } else if (showCpsBbox && isNearbyBBoxMidRight(pos, cpSelectionTolerance, pixelScale)) {
        state = eEventStateDraggingBBoxMidRight;
    } else if (showCpsBbox && isNearbyBBoxMidBtm(pos, cpSelectionTolerance, pixelScale)) {
        state = eEventStateDraggingBBoxMidBtm;
    } else if (showCpsBbox && isNearbyBBoxMidLeft(pos, cpSelectionTolerance, pixelScale)) {
        state = eEventStateDraggingBBoxMidLeft;
    } else if (clickAnywhere && showCpsBbox && isWithinSelectedCpsBBox(pos)) {
        state = eEventStateDraggingSelectedControlPoints;
    } else if (!clickAnywhere && showCpsBbox && isNearbySelectedCpsCrossHair(pos)) {
        state = eEventStateDraggingSelectedControlPoints;
    }

    return state;
}

bool
RotoCurveInteract::isNearbyBBoxMidLeft(const QPointF& p,
                                       double tolerance,
                                       const std::pair<double, double>& pixelScale) const
{
    QPointF topLeft = selectedCpsBbox.topLeft();
    QPointF btmRight = selectedCpsBbox.bottomRight();
    QPointF btmLeft(topLeft.x(), btmRight.y());
    QPointF mid = (topLeft + btmLeft) / 2.;

    if ((p.x() >= (mid.x() - tolerance)) && (p.x() <= (mid.x() + tolerance)) && (p.y() >= (mid.y() - tolerance)) && (p.y() <= (mid.y() + tolerance))) {
        return true;
    } else {
        double offset = kTransformArrowOffsetFromPoint * pixelScale.first;
        double length = kTransformArrowLenght * pixelScale.first;
        double halfLength = length / 2.;
        ;
        /// test if pos is within the arrow bounding box
        QPointF center(mid.x() - offset, mid.y());
        RectD arrowBbox(center.x() - halfLength, center.y() - halfLength, center.x() + halfLength, center.y() + halfLength);

        return arrowBbox.contains(p.x(), p.y());
    }
}

bool
RotoCurveInteract::isNearbySelectedCpsBoundingBox(const QPointF& pos,
                                                  double tolerance) const
{
    QPointF topLeft = selectedCpsBbox.topLeft();
    QPointF btmRight = selectedCpsBbox.bottomRight();
    QPointF btmLeft(topLeft.x(), btmRight.y());
    QPointF topRight(btmRight.x(), topLeft.y());

    /// check if it is nearby top edge
    if ((pos.x() >= (topLeft.x() - tolerance)) && (pos.x() <= (topRight.x() + tolerance)) && (pos.y() >= (topLeft.y() - tolerance)) && (pos.y() <= (topLeft.y() + tolerance))) {
        return true;
    }

    /// right edge
    if ((pos.x() >= (topRight.x() - tolerance)) && (pos.x() <= (topRight.x() + tolerance)) && (pos.y() >= (btmRight.y() - tolerance)) && (pos.y() <= (topRight.y() + tolerance))) {
        return true;
    }

    /// btm edge
    if ((pos.x() >= (btmLeft.x() - tolerance)) && (pos.x() <= (btmRight.x() + tolerance)) && (pos.y() >= (btmLeft.y() - tolerance)) && (pos.y() <= (btmLeft.y() + tolerance))) {
        return true;
    }

    /// left edge
    if ((pos.x() >= (btmLeft.x() - tolerance)) && (pos.x() <= (btmLeft.x() + tolerance)) && (pos.y() >= (btmLeft.y() - tolerance)) && (pos.y() <= (topLeft.y() + tolerance))) {
        return true;
    }

    return false;
}

std::pair<BezierCPPtr, BezierCPPtr>
RotoCurveInteract::isNearbyFeatherBar(double time,
                                      const std::pair<double, double>& pixelScale,
                                      const QPointF& pos) const
{
    double distFeatherX = 20. * pixelScale.first;
    double acceptance = 10 * pixelScale.second;

    for (SelectedItems::const_iterator it = selectedItems.begin(); it != selectedItems.end(); ++it) {
        Bezier* isBezier = dynamic_cast<Bezier*>(it->get());
        RotoStrokeItem* isStroke = dynamic_cast<RotoStrokeItem*>(it->get());
        assert(isStroke || isBezier);
        if (isStroke || !isBezier || (isBezier && isBezier->isOpenBezier())) {
            continue;
        }

        /*
           For each selected bezier, we compute the extent of the feather bars and check if the mouse would be nearby one of these bars.
           The feather bar of a control point is only displayed is the feather point is equal to the bezier control point.
           In order to give it the  correc direction we use the derivative of the bezier curve at the control point and then use
           the pointInPolygon function to make sure the feather bar is always oriented on the outer part of the polygon.
           The pointInPolygon function needs the polygon of the bezier to test whether the point is inside or outside the polygon
           hence in this loop we compute the polygon for each bezier.
         */

        Transform::Matrix3x3 transform;
        isBezier->getTransformAtTime(time, &transform);

        const std::list<BezierCPPtr>& fps = isBezier->getFeatherPoints();
        const std::list<BezierCPPtr>& cps = isBezier->getControlPoints();
        assert(cps.size() == fps.size());

        int cpCount = (int)cps.size();
        if (cpCount <= 1) {
            continue;
        }

        std::list<BezierCPPtr>::const_iterator itF = fps.begin();
        std::list<BezierCPPtr>::const_iterator nextF = itF;
        if (nextF != fps.end()) {
            ++nextF;
        }
        std::list<BezierCPPtr>::const_iterator prevF = fps.end();
        if (prevF != fps.begin()) {
            --prevF;
        }
        bool isClockWiseOriented = isBezier->isFeatherPolygonClockwiseOriented(true, time);

        for (std::list<BezierCPPtr>::const_iterator itCp = cps.begin();
             itCp != cps.end();
             ++itCp) {
            if (prevF == fps.end()) {
                prevF = fps.begin();
            }
            if (nextF == fps.end()) {
                nextF = fps.begin();
            }
            assert(itF != fps.end()); // because cps.size() == fps.size()
            if (itF == fps.end()) {
                itF = fps.begin();
            }

            Transform::Point3D controlPoint, featherPoint;
            controlPoint.z = featherPoint.z = 1;
            (*itCp)->getPositionAtTime(true, time, ViewIdx(0), &controlPoint.x, &controlPoint.y);
            (*itF)->getPositionAtTime(true, time, ViewIdx(0), &featherPoint.x, &featherPoint.y);

            controlPoint = Transform::matApply(transform, controlPoint);
            featherPoint = Transform::matApply(transform, featherPoint);
            {
                Point cp, fp;
                cp.x = controlPoint.x;
                cp.y = controlPoint.y;
                fp.x = featherPoint.x;
                fp.y = featherPoint.y;
                Bezier::expandToFeatherDistance(true, cp, &fp, distFeatherX, time, isClockWiseOriented, transform, prevF, itF, nextF);
                featherPoint.x = fp.x;
                featherPoint.y = fp.y;
            }
            assert(featherPoint.x != controlPoint.x || featherPoint.y != controlPoint.y);

            /// Now test if the user mouse click is on the line using bounding box and cross product.
            if ((((pos.y() >= (controlPoint.y - acceptance)) && (pos.y() <= (featherPoint.y + acceptance))) || ((pos.y() >= (featherPoint.y - acceptance)) && (pos.y() <= (controlPoint.y + acceptance)))) && (((pos.x() >= (controlPoint.x - acceptance)) && (pos.x() <= (featherPoint.x + acceptance))) || ((pos.x() >= (featherPoint.x - acceptance)) && (pos.x() <= (controlPoint.x + acceptance))))) {
                Point a;
                a.x = (featherPoint.x - controlPoint.x);
                a.y = (featherPoint.y - controlPoint.y);
                double norm = sqrt(a.x * a.x + a.y * a.y);

                /// The point is in the bounding box of the segment, if it is vertical it must be on the segment anyway
                if (norm == 0) {
                    return std::make_pair(*itCp, *itF);
                }

                a.x /= norm;
                a.y /= norm;
                Point b;
                b.x = (pos.x() - controlPoint.x);
                b.y = (pos.y() - controlPoint.y);
                norm = sqrt(b.x * b.x + b.y * b.y);

                /// This vector is not vertical
                if (norm != 0) {
                    b.x /= norm;
                    b.y /= norm;

                    double crossProduct = b.y * a.x - b.x * a.y;
                    if (std::abs(crossProduct) < 0.3) {
                        return std::make_pair(*itCp, *itF);
                    }
                }
            }

            // increment for next iteration
            // ++itF, ++nextF, ++prevF
            if (itF != fps.end()) {
                ++itF;
            }
            if (nextF != fps.end()) {
                ++nextF;
            }
            if (prevF != fps.end()) {
                ++prevF;
            }
        } // for(itCp)
    }

    return std::make_pair(BezierCPPtr(), BezierCPPtr());
} // isNearbyFeatherBar

void
RotoCurveInteract::setSelection(const std::list<RotoDrawableItemPtr>& drawables,
                                const std::list<std::pair<BezierCPPtr, BezierCPPtr>>& points)
{
    selectedItems.clear();
    for (std::list<RotoDrawableItemPtr>::const_iterator it = drawables.begin(); it != drawables.end(); ++it) {
        if (*it) {
            selectedItems.push_back(*it);
        }
    }
    selectedCps.clear();
    for (SelectedCPs::const_iterator it = points.begin(); it != points.end(); ++it) {
        if (it->first && it->second) {
            selectedCps.push_back(*it);
        }
    }
    getCurveContext()->select(selectedItems, RotoItem::eSelectionReasonOverlayInteract);
    computeSelectedCpsBBOX();
}

void
RotoCurveInteract::setSelection(const BezierPtr& curve,
                                const std::pair<BezierCPPtr, BezierCPPtr>& point)
{
    selectedItems.clear();
    if (curve) {
        selectedItems.push_back(curve);
    }
    selectedCps.clear();
    if (point.first && point.second) {
        selectedCps.push_back(point);
    }
    if (curve) {
        getCurveContext()->select(curve, RotoItem::eSelectionReasonOverlayInteract);
    }
    computeSelectedCpsBBOX();
}

void
RotoCurveInteract::getSelection(std::list<RotoDrawableItemPtr>* beziers,
                                std::list<std::pair<BezierCPPtr, BezierCPPtr>>* points)
{
    *beziers = selectedItems;
    *points = selectedCps;
}

void
RotoCurveInteract::setBuiltBezier(const BezierPtr& curve)
{
    assert(curve);
    builtBezier = curve;
}

BezierPtr
RotoCurveInteract::getBezierBeingBuild() const
{
    return builtBezier;
}

bool
RotoCurveInteract::smoothSelectedCurve()
{
    std::pair<double, double> pixelScale;

    _host->getCurrentViewportForOverlays()->getPixelScale(pixelScale.first, pixelScale.second);
    RotoCurveContextPtr context = getCurveContext();
    double time = context->getTimelineCurrentTime();
    std::list<SmoothCuspUndoCommand::SmoothCuspCurveData> datas;

    if (!selectedCps.empty()) {
        for (SelectedCPs::const_iterator it = selectedCps.begin(); it != selectedCps.end(); ++it) {
            SmoothCuspUndoCommand::SmoothCuspCurveData data;
            data.curve = it->first->getBezier();
            data.newPoints.push_back(*it);
            datas.push_back(data);
        }
    } else {
        for (SelectedItems::const_iterator it = selectedItems.begin(); it != selectedItems.end(); ++it) {
            BezierPtr bezier = std::dynamic_pointer_cast<Bezier>(*it);
            if (bezier) {
                SmoothCuspUndoCommand::SmoothCuspCurveData data;
                data.curve = bezier;
                const std::list<BezierCPPtr>& cps = bezier->getControlPoints();
                const std::list<BezierCPPtr>& fps = bezier->getFeatherPoints();
                std::list<BezierCPPtr>::const_iterator itFp = fps.begin();
                for (std::list<BezierCPPtr>::const_iterator it = cps.begin(); it != cps.end(); ++it, ++itFp) {
                    data.newPoints.push_back(std::make_pair(*it, *itFp));
                }
                datas.push_back(data);
            }
        }
    }
    if (!datas.empty()) {
        SmoothCuspUndoCommand* cmd = new SmoothCuspUndoCommand(shared_from_this(), datas, time, false, pixelScale);
        // A shortcut or menu action is one undo step per trigger: the knob action that runs this
        // cannot tell a held key's auto-repeat from a separate press.
        cmd->setGestureKey(0);
        _host->pushUndoCommand(cmd);

        return true;
    }

    return false;
}

bool
RotoCurveInteract::cuspSelectedCurve()
{
    std::pair<double, double> pixelScale;

    _host->getCurrentViewportForOverlays()->getPixelScale(pixelScale.first, pixelScale.second);
    RotoCurveContextPtr context = getCurveContext();
    double time = context->getTimelineCurrentTime();
    std::list<SmoothCuspUndoCommand::SmoothCuspCurveData> datas;

    if (!selectedCps.empty()) {
        for (SelectedCPs::const_iterator it = selectedCps.begin(); it != selectedCps.end(); ++it) {
            SmoothCuspUndoCommand::SmoothCuspCurveData data;
            data.curve = it->first->getBezier();
            data.newPoints.push_back(*it);
            datas.push_back(data);
        }
    } else {
        for (SelectedItems::const_iterator it = selectedItems.begin(); it != selectedItems.end(); ++it) {
            BezierPtr bezier = std::dynamic_pointer_cast<Bezier>(*it);
            if (bezier) {
                SmoothCuspUndoCommand::SmoothCuspCurveData data;
                data.curve = bezier;
                const std::list<BezierCPPtr>& cps = bezier->getControlPoints();
                const std::list<BezierCPPtr>& fps = bezier->getFeatherPoints();
                std::list<BezierCPPtr>::const_iterator itFp = fps.begin();
                for (std::list<BezierCPPtr>::const_iterator it = cps.begin(); it != cps.end(); ++it, ++itFp) {
                    data.newPoints.push_back(std::make_pair(*it, *itFp));
                }
                datas.push_back(data);
            }
        }
    }
    if (!datas.empty()) {
        SmoothCuspUndoCommand* cmd = new SmoothCuspUndoCommand(shared_from_this(), datas, time, true, pixelScale);
        // A shortcut or menu action is one undo step per trigger: the knob action that runs this
        // cannot tell a held key's auto-repeat from a separate press.
        cmd->setGestureKey(0);
        _host->pushUndoCommand(cmd);

        return true;
    }

    return false;
}

bool
RotoCurveInteract::removeFeatherForSelectedCurve()
{
    std::list<RemoveFeatherUndoCommand::RemoveFeatherData> datas;

    if (!selectedCps.empty()) {
        for (SelectedCPs::const_iterator it = selectedCps.begin(); it != selectedCps.end(); ++it) {
            RemoveFeatherUndoCommand::RemoveFeatherData data;
            data.curve = it->first->getBezier();
            data.newPoints = data.curve->getFeatherPoints();
            datas.push_back(data);
        }
    } else {
        for (SelectedItems::const_iterator it = selectedItems.begin(); it != selectedItems.end(); ++it) {
            BezierPtr bezier = std::dynamic_pointer_cast<Bezier>(*it);
            if (bezier) {
                RemoveFeatherUndoCommand::RemoveFeatherData data;
                data.curve = bezier;
                data.newPoints = bezier->getFeatherPoints();
                datas.push_back(data);
            }
        }
    }
    if (!datas.empty()) {
        _host->pushUndoCommand(new RemoveFeatherUndoCommand(shared_from_this(), datas));

        return true;
    }

    return false;
}

bool
RotoCurveInteract::lockSelectedCurves()
{
    /// Make a copy because setLocked will change the selection internally and invalidate the iterator
    SelectedItems selection = selectedItems;

    if (selection.empty()) {
        return false;
    }
    for (SelectedItems::const_iterator it = selection.begin(); it != selection.end(); ++it) {
        (*it)->setLocked(true, false, RotoItem::eSelectionReasonOverlayInteract);
    }
    clearSelection();

    return true;
}

bool
RotoCurveInteract::moveSelectedCpsWithKeyArrows(int x,
                                                int y)
{
    std::list<std::pair<BezierCPPtr, BezierCPPtr>> points;

    if (!selectedCps.empty()) {
        points = selectedCps;
    } else {
        for (SelectedItems::const_iterator it = selectedItems.begin(); it != selectedItems.end(); ++it) {
            BezierPtr bezier = std::dynamic_pointer_cast<Bezier>(*it);
            if (bezier) {
                const std::list<BezierCPPtr>& cps = bezier->getControlPoints();
                const std::list<BezierCPPtr>& fps = bezier->getFeatherPoints();
                std::list<BezierCPPtr>::const_iterator fpIt = fps.begin();
                assert(fps.empty() || fps.size() == cps.size());
                for (std::list<BezierCPPtr>::const_iterator it = cps.begin(); it != cps.end(); ++it) {
                    points.push_back(std::make_pair(*it, *fpIt));
                    if (!fps.empty()) {
                        ++fpIt;
                    }
                }
            }
        }
    }

    if (!points.empty()) {
        std::pair<double, double> pixelScale;
        _host->getCurrentViewportForOverlays()->getPixelScale(pixelScale.first, pixelScale.second);
        double time = _host->getCurrentTime();

        MoveControlPointsUndoCommand* cmd = new MoveControlPointsUndoCommand(shared_from_this(), points, (double)x * pixelScale.first,
                                                                             (double)y * pixelScale.second, time);
        // Each nudge is its own undo step: the knob action that runs this cannot tell a held
        // arrow's auto-repeat from a separate press, so it never folds into the previous nudge.
        cmd->setGestureKey(0);
        _host->pushUndoCommand(cmd);
        computeSelectedCpsBBOX();
        getCurveContext()->evaluateChange();

        return true;
    }

    return false;
}

void
RotoCurveInteract::onCurveLockedChangedRecursive(const RotoItemPtr& item,
                                                 bool* ret)
{
    BezierPtr b = std::dynamic_pointer_cast<Bezier>(item);
    RotoLayerPtr layer = std::dynamic_pointer_cast<RotoLayer>(item);

    if (b) {
        if (item->isLockedRecursive()) {
            for (SelectedItems::iterator fb = selectedItems.begin(); fb != selectedItems.end(); ++fb) {
                if (fb->get() == b.get()) {
                    /// if the curve was selected, wipe the selection CP bbox
                    clearCPSSelection();
                    selectedItems.erase(fb);
                    *ret = true;
                    break;
                }
            }
        } else {
            /// Explanation: This change has been made in result to a user click on the settings panel.
            /// We have to reselect the bezier overlay hence put a reason different of eSelectionReasonOverlayInteract
            SelectedItems::iterator found = std::find(selectedItems.begin(), selectedItems.end(), b);
            if (found == selectedItems.end()) {
                selectedItems.push_back(b);
                getCurveContext()->select(b, RotoItem::eSelectionReasonSettingsPanel);
                *ret = true;
            }
        }
    } else if (layer) {
        const std::list<RotoItemPtr>& items = layer->getItems();
        for (std::list<RotoItemPtr>::const_iterator it = items.begin(); it != items.end(); ++it) {
            onCurveLockedChangedRecursive(*it, ret);
        }
    }
}

void
RotoCurveInteract::removeCurve(const RotoDrawableItemPtr& curve)
{
    if (curve == builtBezier) {
        builtBezier.reset();
    }
    getCurveContext()->removeItem(curve);
}

void
RotoCurveInteract::drawOverlay(double time)
{
    std::list<RotoDrawableItemPtr> drawables = getCurveContext()->getCurvesByRenderOrder();
    std::pair<double, double> pixelScale;
    std::pair<double, double> viewportSize;
    double screenPixelRatio = _host->getCurrentViewportForOverlays()->getScreenPixelRatio();

    _host->getCurrentViewportForOverlays()->getPixelScale(pixelScale.first, pixelScale.second);
    _host->getCurrentViewportForOverlays()->getViewportSize(viewportSize.first, viewportSize.second);

    bool featherVisible = isFeatherVisible();

    {
        GLProtectAttrib a(GL_HINT_BIT | GL_ENABLE_BIT | GL_LINE_BIT | GL_COLOR_BUFFER_BIT | GL_POINT_BIT | GL_CURRENT_BIT);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_LINE_SMOOTH);
        glHint(GL_LINE_SMOOTH_HINT, GL_DONT_CARE);

        double cpWidth = kControlPointMidSize * 2;
        for (std::list<RotoDrawableItemPtr>::const_iterator it = drawables.begin(); it != drawables.end(); ++it) {
            if (!(*it)->isGloballyActivated()) {
                continue;
            }

            Bezier* isBezier = dynamic_cast<Bezier*>(it->get());
            RotoStrokeItem* isStroke = dynamic_cast<RotoStrokeItem*>(it->get());
            if (isStroke) {
                if (selectedTool != eRotoToolSelectAll) {
                    continue;
                }

                bool selected = false;
                for (SelectedItems::const_iterator it2 = selectedItems.begin(); it2 != selectedItems.end(); ++it2) {
                    if (it2->get() == isStroke) {
                        selected = true;
                        break;
                    }
                }
                if (!selected) {
                    continue;
                }

                std::list<std::list<std::pair<Point, double>>> strokes;
                isStroke->evaluateStroke(0, time, &strokes);
                bool locked = (*it)->isLockedRecursive();
                double curveColor[4];
                if (!locked) {
                    (*it)->getOverlayColor(curveColor);
                } else {
                    curveColor[0] = 0.8;
                    curveColor[1] = 0.8;
                    curveColor[2] = 0.8;
                    curveColor[3] = 1.;
                }
                glColor4dv(curveColor);

                for (std::list<std::list<std::pair<Point, double>>>::iterator itStroke = strokes.begin(); itStroke != strokes.end(); ++itStroke) {
                    glLineWidth(1.5 * screenPixelRatio);
                    glBegin(GL_LINE_STRIP);
                    for (std::list<std::pair<Point, double>>::const_iterator it2 = itStroke->begin(); it2 != itStroke->end(); ++it2) {
                        glVertex2f(it2->first.x, it2->first.y);
                    }
                    glEnd();
                }
            } else if (isBezier) {
                /// draw the bezier
                // check if the bbox is visible
                // if the bbox is visible, compute the polygon and draw it.

                RectD bbox = isBezier->getBoundingBox(time);

                // Is the Bezier a closed curve? Only if not an open Bezier, and it's finished.
                const bool isLoop = !isBezier->isOpenBezier() && isBezier->isCurveFinished();

                // To decomment you must transform the viewport by the OpenGL transform first
                // if ( !_host->getCurrentViewportForOverlays()->isVisibleInViewport(bbox) ) {
                //                  continue;
                //            }

                std::list<ParametricPoint> points;

                isBezier->evaluateAtTime_DeCasteljau(true, time, 0,
#ifdef ROTO_BEZIER_EVAL_ITERATIVE
                                                     100,
#else
                                                     1,
#endif
                                                     &points, NULL);

                bool locked = (*it)->isLockedRecursive();
                double curveColor[4];
                if (!locked) {
                    (*it)->getOverlayColor(curveColor);
                } else {
                    curveColor[0] = 0.8;
                    curveColor[1] = 0.8;
                    curveColor[2] = 0.8;
                    curveColor[3] = 1.;
                }
                glColor4dv(curveColor);
                glLineWidth(1.5 * screenPixelRatio);
                glBegin(isLoop ? GL_LINE_LOOP : GL_LINE_STRIP);
                for (std::list<ParametricPoint>::const_iterator it2 = points.begin(); it2 != points.end(); ++it2) {
                    glVertex2f(it2->x, it2->y);
                }
                glEnd();

                /// draw the feather points
                std::list<ParametricPoint> featherPoints;
                RectD featherBBox(std::numeric_limits<double>::infinity(),
                                  std::numeric_limits<double>::infinity(),
                                  -std::numeric_limits<double>::infinity(),
                                  -std::numeric_limits<double>::infinity());
                bool clockWise = isBezier->isFeatherPolygonClockwiseOriented(true, time);

                if (featherVisible) {
                    /// Draw feather only if visible (button is toggled in the user interface)
                    isBezier->evaluateFeatherPointsAtTime_DeCasteljau(true, time, 0,
#ifdef ROTO_BEZIER_EVAL_ITERATIVE
                                                                      100,
#else
                                                                      1,
#endif
                                                                      true, &featherPoints, &featherBBox);

                    if (!featherPoints.empty()) {
                        glLineStipple(2, 0xAAAA);
                        glEnable(GL_LINE_STIPPLE);
                        glLineWidth(1.5 * screenPixelRatio);
                        glBegin(isLoop ? GL_LINE_LOOP : GL_LINE_STRIP);
                        for (std::list<ParametricPoint>::const_iterator it2 = featherPoints.begin(); it2 != featherPoints.end(); ++it2) {
                            glVertex2f(it2->x, it2->y);
                        }
                        glEnd();
                        glDisable(GL_LINE_STIPPLE);
                    }
                }

                /// draw the control points if the bezier is selected
                bool selected = false;
                for (SelectedItems::const_iterator it2 = selectedItems.begin(); it2 != selectedItems.end(); ++it2) {
                    if (it2->get() == isBezier) {
                        selected = true;
                        break;
                    }
                }

                if (selected && !locked) {
                    Transform::Matrix3x3 transform;
                    isBezier->getTransformAtTime(time, &transform);

                    const std::list<BezierCPPtr>& cps = isBezier->getControlPoints();
                    const std::list<BezierCPPtr>& featherPts = isBezier->getFeatherPoints();
                    assert(cps.size() == featherPts.size());

                    if (cps.empty()) {
                        continue;
                    }

                    glColor3d(0.85, 0.67, 0.);

                    std::list<BezierCPPtr>::const_iterator itF = featherPts.begin();
                    int index = 0;
                    std::list<BezierCPPtr>::const_iterator prevCp = cps.end();
                    if (prevCp != cps.begin()) {
                        --prevCp;
                    }
                    std::list<BezierCPPtr>::const_iterator nextCp = cps.begin();
                    if (nextCp != cps.end()) {
                        ++nextCp;
                    }
                    for (std::list<BezierCPPtr>::const_iterator it2 = cps.begin(); it2 != cps.end();
                         ++it2) {
                        if (nextCp == cps.end()) {
                            nextCp = cps.begin();
                        }
                        if (prevCp == cps.end()) {
                            prevCp = cps.begin();
                        }
                        assert(itF != featherPts.end()); // because cps.size() == featherPts.size()
                        if (itF == featherPts.end()) {
                            break;
                        }
                        double x, y;
                        Transform::Point3D p, pF;
                        (*it2)->getPositionAtTime(true, time, ViewIdx(0), &p.x, &p.y);
                        p.z = 1.;

                        double xF, yF;
                        (*itF)->getPositionAtTime(true, time, ViewIdx(0), &pF.x, &pF.y);
                        pF.z = 1.;

                        p = Transform::matApply(transform, p);
                        pF = Transform::matApply(transform, pF);

                        x = p.x;
                        y = p.y;
                        xF = pF.x;
                        yF = pF.y;

                        /// draw the feather point only if it is distinct from the associated point
                        bool drawFeather = featherVisible;
                        if (featherVisible) {
                            drawFeather = !(*it2)->equalsAtTime(true, time, ViewIdx(0), **itF);
                        }

                        /// if the control point is the only control point being dragged, color it to identify it to the user
                        bool colorChanged = false;
                        SelectedCPs::const_iterator firstSelectedCP = selectedCps.begin();
                        if ((firstSelectedCP != selectedCps.end()) && ((firstSelectedCP->first == *it2) || (firstSelectedCP->second == *it2)) && (selectedCps.size() == 1) && ((state == eEventStateDraggingSelectedControlPoints) || (state == eEventStateDraggingControlPoint))) {
                            glColor3f(0., 1., 1.);
                            colorChanged = true;
                        }

                        glEnable(GL_POINT_SMOOTH);
                        glPointSize(cpWidth * screenPixelRatio);
                        // code below draws GL_POINTS
                        for (SelectedCPs::const_iterator cpIt = selectedCps.begin();
                             cpIt != selectedCps.end();
                             ++cpIt) {
                            /// if the control point is selected, draw its tangent handles
                            if (cpIt->first == *it2) {
                                drawSelectedCp(time, cpIt->first, x, y, transform);
                                if (drawFeather) {
                                    drawSelectedCp(time, cpIt->second, xF, yF, transform);
                                }
                                glColor3f(0.2, 1., 0.);
                                colorChanged = true;
                                break;
                            } else if (cpIt->second == *it2) {
                                drawSelectedCp(time, cpIt->second, x, y, transform);
                                if (drawFeather) {
                                    drawSelectedCp(time, cpIt->first, xF, yF, transform);
                                }
                                glColor3f(0.2, 1., 0.);
                                colorChanged = true;
                                break;
                            }
                        } // for(cpIt)

                        glBegin(GL_POINTS);
                        glVertex2f(x, y);
                        glEnd();

                        if (colorChanged) {
                            glColor3d(0.85, 0.67, 0.);
                        }

                        if ((firstSelectedCP->first == *itF)
                            && (selectedCps.size() == 1) && ((state == eEventStateDraggingSelectedControlPoints) || (state == eEventStateDraggingControlPoint))
                            && !colorChanged) {
                            glColor3f(0.2, 1., 0.);
                            colorChanged = true;
                        }

                        double distFeatherX = 20. * pixelScale.first;
                        double distFeatherY = 20. * pixelScale.second;
                        bool isHovered = false;
                        if (featherBarBeingHovered.first) {
                            assert(featherBarBeingHovered.second);
                            if (featherBarBeingHovered.first->isFeatherPoint()) {
                                isHovered = featherBarBeingHovered.first == *itF;
                            } else if (featherBarBeingHovered.second->isFeatherPoint()) {
                                isHovered = featherBarBeingHovered.second == *itF;
                            }
                        }

                        if (drawFeather) {
                            glEnable(GL_POINT_SMOOTH);
                            glPointSize(cpWidth * screenPixelRatio);
                            glBegin(GL_POINTS);
                            glVertex2f(xF, yF);
                            glEnd();

                            if (((state == eEventStateDraggingFeatherBar) && ((*itF == featherBarBeingDragged.first) || (*itF == featherBarBeingDragged.second))) || isHovered) {
                                glColor3f(0.2, 1., 0.);
                                colorChanged = true;
                            } else {
                                glColor4dv(curveColor);
                            }

                            double beyondX, beyondY;
                            double dx = (xF - x);
                            double dy = (yF - y);
                            double dist = sqrt(dx * dx + dy * dy);
                            beyondX = (dx * (dist + distFeatherX)) / dist + x;
                            beyondY = (dy * (dist + distFeatherY)) / dist + y;

                            /// draw a link between the feather point and the control point.
                            /// Also extend that link of 20 pixels beyond the feather point.

                            glLineWidth(1.5 * screenPixelRatio);
                            glBegin(GL_LINE_STRIP);
                            glVertex2f(x, y);
                            glVertex2f(xF, yF);
                            glVertex2f(beyondX, beyondY);
                            glEnd();

                            glColor3d(0.85, 0.67, 0.);
                        } else if (featherVisible) {
                            /// if the feather point is identical to the control point
                            /// draw a small hint line that the user can drag to move the feather point
                            if (!isBezier->isOpenBezier() && ((selectedTool == eRotoToolSelectAll) || (selectedTool == eRotoToolSelectFeatherPoints))) {
                                int cpCount = (*it2)->getBezier()->getControlPointsCount();
                                if (cpCount > 1) {
                                    Point controlPoint;
                                    controlPoint.x = x;
                                    controlPoint.y = y;
                                    Point featherPoint;
                                    featherPoint.x = xF;
                                    featherPoint.y = yF;

                                    Bezier::expandToFeatherDistance(true, controlPoint, &featherPoint, distFeatherX, time, clockWise, transform, prevCp, it2, nextCp);

                                    if (((state == eEventStateDraggingFeatherBar) && ((*itF == featherBarBeingDragged.first) || (*itF == featherBarBeingDragged.second))) || isHovered) {
                                        glColor3f(0.2, 1., 0.);
                                        colorChanged = true;
                                    } else {
                                        glColor4dv(curveColor);
                                    }

                                    glLineWidth(1.5 * screenPixelRatio);
                                    glBegin(GL_LINES);
                                    glVertex2f(x, y);
                                    glVertex2f(featherPoint.x, featherPoint.y);
                                    glEnd();

                                    glColor3d(0.85, 0.67, 0.);
                                }
                            }
                        } // isFeatherVisible()

                        if (colorChanged) {
                            glColor3d(0.85, 0.67, 0.);
                        }

                        // increment for next iteration
                        if (itF != featherPts.end()) {
                            ++itF;
                        }
                        if (nextCp != cps.end()) {
                            ++nextCp;
                        }
                        if (prevCp != cps.end()) {
                            ++prevCp;
                        }
                        ++index;
                    } // for(it2)
                } // if ( ( selected != selectedBeziers.end() ) && !locked ) {
            } // if (isBezier)
            glCheckError();
        } // for (std::list<RotoDrawableItemPtr>::const_iterator it = drawables.begin(); it != drawables.end(); ++it) {

        if (!_host->isDrawingPassiveOverlay()) {
            drawPaintToolOverlay(screenPixelRatio, viewportSize);
        }
    } // GLProtectAttrib a(GL_HINT_BIT | GL_ENABLE_BIT | GL_LINE_BIT | GL_COLOR_BUFFER_BIT | GL_POINT_BIT | GL_CURRENT_BIT);

    if (showCpsBbox) {
        drawSelectedCpsBBOX();
    }
    glCheckError();
} // drawOverlay

void
RotoCurveInteract::onViewportSelectionCleared()
{
    if (!isStickySelectionEnabled() && !shiftDown) {
        clearSelection();
    }

    if ((selectedTool == eRotoToolDrawBezier) || (selectedTool == eRotoToolOpenBezier)) {
        if (((selectedTool == eRotoToolDrawBezier) || (selectedTool == eRotoToolOpenBezier)) && builtBezier && !builtBezier->isCurveFinished()) {
            pushUndoCommand(new OpenCloseUndoCommand(shared_from_this(), builtBezier));

            builtBezier.reset();
            selectedCps.clear();
            setCurrentTool(selectAllAction.lock());
            getCurveContext()->evaluateChange();
        }
    }
}

void
RotoCurveInteract::onViewportSelectionUpdated(const RectD& rectangle,
                                              bool onRelease)
{
    if (!onRelease || !getNode()->isSettingsPanelVisible()) {
        return;
    }

    bool stickySel = isStickySelectionEnabled();
    if (!stickySel && !shiftDown) {
        clearCPSSelection();
        selectedItems.clear();
    }

    int selectionMode = -1;
    if (selectedTool == eRotoToolSelectAll) {
        selectionMode = 0;
    } else if (selectedTool == eRotoToolSelectPoints) {
        selectionMode = 1;
    } else if ((selectedTool == eRotoToolSelectFeatherPoints) || (selectedTool == eRotoToolSelectCurves)) {
        selectionMode = 2;
    }

    bool featherVisible = isFeatherVisible();
    RotoCurveContextPtr ctx = getCurveContext();
    assert(ctx);
    std::list<RotoDrawableItemPtr> curves = ctx->getCurvesByRenderOrder();
    for (std::list<RotoDrawableItemPtr>::const_iterator it = curves.begin(); it != curves.end(); ++it) {
        BezierPtr isBezier = std::dynamic_pointer_cast<Bezier>(*it);
        if ((*it)->isLockedRecursive()) {
            continue;
        }

        if (isBezier) {
            SelectedCPs points = isBezier->controlPointsWithinRect(rectangle.x1, rectangle.x2, rectangle.y1, rectangle.y2, 0, selectionMode);
            if (selectedTool != eRotoToolSelectCurves) {
                for (SelectedCPs::iterator ptIt = points.begin(); ptIt != points.end(); ++ptIt) {
                    if (!featherVisible && ptIt->first->isFeatherPoint()) {
                        continue;
                    }
                    SelectedCPs::iterator foundCP = std::find(selectedCps.begin(), selectedCps.end(), *ptIt);
                    if (foundCP == selectedCps.end()) {
                        if (!shiftDown || !ctrlDown) {
                            selectedCps.push_back(*ptIt);
                        }
                    } else {
                        if (shiftDown && ctrlDown) {
                            selectedCps.erase(foundCP);
                        }
                    }
                }
            }
            if (!points.empty()) {
                selectedItems.push_back(isBezier);
            }
        }
    }

    if (!selectedItems.empty()) {
        ctx->select(selectedItems, RotoItem::eSelectionReasonOverlayInteract);
    } else if (!stickySel && !shiftDown) {
        ctx->clearSelection(RotoItem::eSelectionReasonOverlayInteract);
    }

    computeSelectedCpsBBOX();
} // RotoCurveInteract::onViewportSelectionUpdated

bool
RotoCurveInteract::onOverlayPenDoubleClicked(const QPointF& pos)
{
    bool didSomething = false;
    std::pair<double, double> pixelScale;

    _host->getCurrentViewportForOverlays()->getPixelScale(pixelScale.first, pixelScale.second);

    if (selectedTool == eRotoToolSelectAll) {
        double bezierSelectionTolerance = kBezierSelectionTolerance * pixelScale.first;
        double nearbyBezierT;
        int nearbyBezierCPIndex;
        bool isFeather;
        BezierPtr nearbyBezier = getCurveContext()->isNearbyBezier(pos.x(), pos.y(), bezierSelectionTolerance, &nearbyBezierCPIndex, &nearbyBezierT, &isFeather);

        if (nearbyBezier) {
            /// If the bezier is already selected and we re-click on it, change the transform mode
            handleBezierSelection(nearbyBezier);
            clearCPSSelection();
            const std::list<BezierCPPtr>& cps = nearbyBezier->getControlPoints();
            const std::list<BezierCPPtr>& fps = nearbyBezier->getFeatherPoints();
            assert(cps.size() == fps.size());
            std::list<BezierCPPtr>::const_iterator itCp = cps.begin();
            std::list<BezierCPPtr>::const_iterator itFp = fps.begin();
            for (; itCp != cps.end(); ++itCp, ++itFp) {
                selectedCps.push_back(std::make_pair(*itCp, *itFp));
            }
            if (selectedCps.size() > 1) {
                computeSelectedCpsBBOX();
            }
            didSomething = true;
        }
    }

    return didSomething;
} // onOverlayPenDoubleClicked

bool
RotoCurveInteract::onOverlayPenDown(double time,
                                    const QPointF& pos,
                                    double pressure,
                                    double timestamp,
                                    PenType pen)
{
    // Every command this press and the drag after it push is one undo step, closed at pen up.
    undoGesture.begin();

    std::pair<double, double> pixelScale;
    _host->getCurrentViewportForOverlays()->getPixelScale(pixelScale.first, pixelScale.second);

    bool didSomething = false;
    double tangentSelectionTol = kTangentHandleSelectionTolerance * pixelScale.first;
    double cpSelectionTolerance = kControlPointSelectionTolerance * pixelScale.first;

    onPenDownStarted(pen);

    RotoCurveContextPtr context = getCurveContext();
    assert(context);

    const bool featherVisible = isFeatherVisible();

    //////////////////BEZIER SELECTION
    /////Check if the point is nearby a bezier
    /// tolerance for bezier selection
    double bezierSelectionTolerance = kBezierSelectionTolerance * pixelScale.first;
    double nearbyBezierT;
    int nearbyBezierCPIndex;
    bool isFeather;
    BezierPtr nearbyBezier = context->isNearbyBezier(pos.x(), pos.y(), bezierSelectionTolerance, &nearbyBezierCPIndex, &nearbyBezierT, &isFeather);
    std::pair<BezierCPPtr, BezierCPPtr> nearbyCP;
    int nearbyCpIndex = -1;
    if (nearbyBezier) {
        /////////////////CONTROL POINT SELECTION
        //////Check if the point is nearby a control point of a selected bezier
        /// Find out if the user selected a control point

        Bezier::ControlPointSelectionPrefEnum pref = Bezier::eControlPointSelectionPrefWhateverFirst;
        if ((selectedTool == eRotoToolSelectFeatherPoints) && featherVisible) {
            pref = Bezier::eControlPointSelectionPrefFeatherFirst;
        }

        nearbyCP = nearbyBezier->isNearbyControlPoint(pos.x(), pos.y(), cpSelectionTolerance, pref, &nearbyCpIndex);
    }

    ////////////////// TANGENT SELECTION
    /// in all cases except cusp/smooth if a control point is selected, check if the user clicked on a tangent handle
    /// in which case we go into eEventStateDraggingTangent mode
    if (!nearbyCP.first && (selectedTool != eRotoToolCuspPoints) && (selectedTool != eRotoToolSmoothPoints) && (selectedTool != eRotoToolSelectCurves)) {
        for (SelectedCPs::iterator it = selectedCps.begin(); it != selectedCps.end(); ++it) {
            if ((selectedTool == eRotoToolSelectAll) || (selectedTool == eRotoToolDrawBezier)) {
                int ret = it->first->isNearbyTangent(true, time, ViewIdx(0), pos.x(), pos.y(), tangentSelectionTol);
                if (ret >= 0) {
                    tangentBeingDragged = it->first;
                    state = ret == 0 ? eEventStateDraggingLeftTangent : eEventStateDraggingRightTangent;
                    didSomething = true;
                } else {
                    /// try with the counter part point
                    if (it->second) {
                        ret = it->second->isNearbyTangent(true, time, ViewIdx(0), pos.x(), pos.y(), tangentSelectionTol);
                    }
                    if (ret >= 0) {
                        tangentBeingDragged = it->second;
                        state = ret == 0 ? eEventStateDraggingLeftTangent : eEventStateDraggingRightTangent;
                        didSomething = true;
                    }
                }
            } else if (selectedTool == eRotoToolSelectFeatherPoints) {
                const BezierCPPtr& fp = it->first->isFeatherPoint() ? it->first : it->second;
                int ret = fp->isNearbyTangent(true, time, ViewIdx(0), pos.x(), pos.y(), tangentSelectionTol);
                if (ret >= 0) {
                    tangentBeingDragged = fp;
                    state = ret == 0 ? eEventStateDraggingLeftTangent : eEventStateDraggingRightTangent;
                    didSomething = true;
                }
            } else if (selectedTool == eRotoToolSelectPoints) {
                const BezierCPPtr& cp = it->first->isFeatherPoint() ? it->second : it->first;
                int ret = cp->isNearbyTangent(true, time, ViewIdx(0), pos.x(), pos.y(), tangentSelectionTol);
                if (ret >= 0) {
                    tangentBeingDragged = cp;
                    state = ret == 0 ? eEventStateDraggingLeftTangent : eEventStateDraggingRightTangent;
                    didSomething = true;
                }
            }

            /// check in case this is a feather tangent
            if (tangentBeingDragged && tangentBeingDragged->isFeatherPoint() && !featherVisible) {
                tangentBeingDragged.reset();
                state = eEventStateNone;
                didSomething = false;
            }

            if (didSomething) {
                return didSomething;
            }
        }
    }

    switch (selectedTool) {
    case eRotoToolSelectAll:
    case eRotoToolSelectPoints:
    case eRotoToolSelectFeatherPoints: {
        if ((selectedTool == eRotoToolSelectFeatherPoints) && !featherVisible) {
            /// nothing to do
            break;
        }
        std::pair<BezierCPPtr, BezierCPPtr> featherBarSel;
        if (((selectedTool == eRotoToolSelectAll) || (selectedTool == eRotoToolSelectFeatherPoints))) {
            featherBarSel = isNearbyFeatherBar(time, pixelScale, pos);
            if (featherBarSel.first && !featherVisible) {
                featherBarSel.first.reset();
                featherBarSel.second.reset();
            }
        }
        // Grabbing the bar of an already selected point keeps the selection, so the drag moves
        // every selected feather; handleControlPointSelection would drop it on Shift.
        bool featherBarIsSelected = false;
        if (featherBarSel.first) {
            for (SelectedCPs::const_iterator it = selectedCps.begin(); it != selectedCps.end(); ++it) {
                if ((it->first == featherBarSel.first) || (it->second == featherBarSel.first)) {
                    featherBarIsSelected = true;
                    break;
                }
            }
        }

        if (nearbyBezier) {
            /// check if the user clicked nearby the cross hair of the selection rectangle in which case
            /// we drag all the control points selected
            if (nearbyCP.first) {
                handleControlPointSelection(nearbyCP);
                handleBezierSelection(nearbyBezier);
                if (pen == ePenTypeRMB) {
                    state = eEventStateNone;
                    showMenuForControlPoint(nearbyCP.first);
                }
                didSomething = true;
            } else if (featherBarSel.first) {
                if (!featherBarIsSelected) {
                    clearCPSSelection();
                }
                featherBarBeingDragged = featherBarSel;

                /// Also select the point only if the curve is the same!
                if (featherBarSel.first->getBezier() == nearbyBezier) {
                    if (!featherBarIsSelected) {
                        handleControlPointSelection(featherBarBeingDragged);
                    }
                    handleBezierSelection(nearbyBezier);
                }
                state = eEventStateDraggingFeatherBar;
                evaluateOnPenUp = true;
                didSomething = true;
            } else {
                bool found = false;
                for (SelectedItems::iterator it = selectedItems.begin(); it != selectedItems.end(); ++it) {
                    if (it->get() == nearbyBezier.get()) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    handleBezierSelection(nearbyBezier);
                }
                if (pen == ePenTypeLMB || pen == ePenTypeMMB) {
                    if (ctrlDown && altDown && !shiftDown) {
                        pushUndoCommand(new AddPointUndoCommand(shared_from_this(), nearbyBezier, nearbyBezierCPIndex, nearbyBezierT));
                        evaluateOnPenUp = true;
                    } else {
                        state = eEventStateDraggingSelectedControlPoints;
                        bezierBeingDragged = nearbyBezier;
                    }
                } else if (pen == ePenTypeRMB) {
                    showMenuForCurve(nearbyBezier);
                }
                didSomething = true;
            }
        } else {
            if (featherBarSel.first) {
                featherBarBeingDragged = featherBarSel;
                if (!featherBarIsSelected) {
                    clearCPSSelection();
                    handleControlPointSelection(featherBarBeingDragged);
                }
                state = eEventStateDraggingFeatherBar;
                evaluateOnPenUp = true;
                didSomething = true;
            }
            if (state == eEventStateNone) {
                state = isMouseInteractingWithCPSBbox(pos, cpSelectionTolerance, pixelScale);
                if (state != eEventStateNone) {
                    didSomething = true;
                }
            }
        }
        break;
    }
    case eRotoToolSelectCurves:

        if (nearbyBezier) {
            /// If the bezier is already selected and we re-click on it, change the transform mode
            bool found = false;
            for (SelectedItems::iterator it = selectedItems.begin(); it != selectedItems.end(); ++it) {
                if (it->get() == nearbyBezier.get()) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                handleBezierSelection(nearbyBezier);
            }
            if (pen == ePenTypeRMB) {
                showMenuForCurve(nearbyBezier);
            } else {
                if (ctrlDown && altDown && !shiftDown) {
                    pushUndoCommand(new AddPointUndoCommand(shared_from_this(), nearbyBezier, nearbyBezierCPIndex, nearbyBezierT));
                    evaluateOnPenUp = true;
                }
            }
            didSomething = true;
        } else {
            if (state == eEventStateNone) {
                state = isMouseInteractingWithCPSBbox(pos, cpSelectionTolerance, pixelScale);
                if (state != eEventStateNone) {
                    didSomething = true;
                }
            }
        }
        break;
    case eRotoToolAddPoints:
        /// If the user clicked on a bezier and this bezier is selected add a control point by
        /// splitting up the targeted segment
        if (nearbyBezier) {
            bool found = false;
            for (SelectedItems::iterator it = selectedItems.begin(); it != selectedItems.end(); ++it) {
                if (it->get() == nearbyBezier.get()) {
                    found = true;
                    break;
                }
            }
            if (found) {
                /// check that the point is not too close to an existing point
                if (nearbyCP.first) {
                    handleControlPointSelection(nearbyCP);
                } else {
                    pushUndoCommand(new AddPointUndoCommand(shared_from_this(), nearbyBezier, nearbyBezierCPIndex, nearbyBezierT));
                    evaluateOnPenUp = true;
                }
                didSomething = true;
            }
        }
        break;
    case eRotoToolRemovePoints:
        if (nearbyCP.first) {
            assert(nearbyBezier && nearbyBezier == nearbyCP.first->getBezier());
            if (nearbyCP.first->isFeatherPoint()) {
                pushUndoCommand(new RemovePointUndoCommand(shared_from_this(), nearbyBezier, nearbyCP.second));
            } else {
                pushUndoCommand(new RemovePointUndoCommand(shared_from_this(), nearbyBezier, nearbyCP.first));
            }
            didSomething = true;
        }
        break;
    case eRotoToolRemoveFeatherPoints:
        if (nearbyCP.first) {
            assert(nearbyBezier);
            std::list<RemoveFeatherUndoCommand::RemoveFeatherData> datas;
            RemoveFeatherUndoCommand::RemoveFeatherData data;
            data.curve = nearbyBezier;
            data.newPoints.push_back(nearbyCP.first->isFeatherPoint() ? nearbyCP.first : nearbyCP.second);
            datas.push_back(data);
            pushUndoCommand(new RemoveFeatherUndoCommand(shared_from_this(), datas));
            didSomething = true;
        }
        break;
    case eRotoToolOpenCloseCurve:
        if (nearbyBezier) {
            pushUndoCommand(new OpenCloseUndoCommand(shared_from_this(), nearbyBezier));
            didSomething = true;
        }
        break;
    case eRotoToolSmoothPoints:

        if (nearbyCP.first) {
            std::list<SmoothCuspUndoCommand::SmoothCuspCurveData> datas;
            SmoothCuspUndoCommand::SmoothCuspCurveData data;
            data.curve = nearbyBezier;
            data.newPoints.push_back(nearbyCP);
            datas.push_back(data);
            pushUndoCommand(new SmoothCuspUndoCommand(shared_from_this(), datas, time, false, pixelScale));
            didSomething = true;
        }
        break;
    case eRotoToolCuspPoints:
        if (nearbyCP.first && getCurveContext()->isAutoKeyingEnabled()) {
            std::list<SmoothCuspUndoCommand::SmoothCuspCurveData> datas;
            SmoothCuspUndoCommand::SmoothCuspCurveData data;
            data.curve = nearbyBezier;
            data.newPoints.push_back(nearbyCP);
            datas.push_back(data);
            pushUndoCommand(new SmoothCuspUndoCommand(shared_from_this(), datas, time, true, pixelScale));
            didSomething = true;
        }
        break;
    case eRotoToolDrawBezier:
    case eRotoToolOpenBezier: {
        if (builtBezier && builtBezier->isCurveFinished()) {
            builtBezier.reset();
            clearSelection();
            setCurrentTool(selectAllAction.lock());

            return true;
        }
        if (builtBezier) {
            /// if the user clicked on a control point of the bezier, select the point instead.
            /// if that point is the starting point of the curve, close the curve
            const std::list<BezierCPPtr>& cps = builtBezier->getControlPoints();
            int i = 0;
            for (std::list<BezierCPPtr>::const_iterator it = cps.begin(); it != cps.end(); ++it, ++i) {
                double x, y;
                (*it)->getPositionAtTime(true, time, ViewIdx(0), &x, &y);
                if ((x >= (pos.x() - cpSelectionTolerance)) && (x <= (pos.x() + cpSelectionTolerance)) && (y >= (pos.y() - cpSelectionTolerance)) && (y <= (pos.y() + cpSelectionTolerance))) {
                    if (it == cps.begin()) {
                        pushUndoCommand(new OpenCloseUndoCommand(shared_from_this(), builtBezier));

                        builtBezier.reset();

                        selectedCps.clear();
                        setCurrentTool(selectAllAction.lock());

                        // continue editing the tangent

                        //->ui->tangentBeingDragged = *it;
                        // state = eEventStateDraggingRightTangent;
                    } else {
                        BezierCPPtr fp = builtBezier->getFeatherPointAtIndex(i);
                        assert(fp);
                        handleControlPointSelection(std::make_pair(*it, fp));
                    }

                    return true;
                }
            }
        }

        bool isOpenBezier = selectedTool == eRotoToolOpenBezier;
        MakeBezierUndoCommand* cmd = new MakeBezierUndoCommand(shared_from_this(), builtBezier, isOpenBezier, true, pos.x(), pos.y(), time);
        pushUndoCommand(cmd);
        builtBezier = cmd->getCurve();
        assert(builtBezier);
        state = eEventStateBuildingBezierControlPointTangent;
        didSomething = true;
        break;
    }
    case eRotoToolDrawBSpline:

        break;
    case eRotoToolDrawEllipse: {
        click = pos;
        pushUndoCommand(new MakeEllipseUndoCommand(shared_from_this(), true, false, false, pos.x(), pos.y(), pos.x(), pos.y(), time));
        state = eEventStateBuildingEllipse;
        didSomething = true;
        break;
    }
    case eRotoToolDrawRectangle: {
        click = pos;
        pushUndoCommand(new MakeRectangleUndoCommand(shared_from_this(), true, false, false, pos.x(), pos.y(), pos.x(), pos.y(), time));
        evaluateOnPenUp = true;
        state = eEventStateBuildingRectangle;
        didSomething = true;
        break;
    }
    default:
        didSomething = onPaintToolPenDown(time, pos, pressure, timestamp);
        break;
    } // switch

    lastClickPos = pos;
    lastMousePos = pos;

    return didSomething;
} // penDown

bool
RotoCurveInteract::onOverlayPenMotion(double time,
                                      const QPointF& pos,
                                      double pressure,
                                      double timestamp)
{
    std::pair<double, double> pixelScale;

    _host->getCurrentViewportForOverlays()->getPixelScale(pixelScale.first, pixelScale.second);

    RotoCurveContextPtr context = getCurveContext();
    assert(context);
    if (!context) {
        return false;
    }
    bool didSomething = false; // Set if an actual action was performed based on mouse motion.
    bool redraw = false; // Set if we just need a redraw (e.g., hover state changed).
    HoverStateEnum lastHoverState = hoverState;
    /// Set the cursor to the appropriate case
    bool cursorSet = setPaintToolCursor(&redraw);
    double cpTol = kControlPointSelectionTolerance * pixelScale.first;

    if (!cursorSet && showCpsBbox && (state != eEventStateDraggingControlPoint) && (state != eEventStateDraggingSelectedControlPoints)
        && (state != eEventStateDraggingLeftTangent) && (state != eEventStateDraggingRightTangent)) {
        double bboxTol = cpTol;
        HoverStateEnum newState = hoverState;
        if (isNearbyBBoxBtmLeft(pos, bboxTol, pixelScale)) {
            newState = eHoverStateBboxBtmLeft;
        } else if (isNearbyBBoxBtmRight(pos, bboxTol, pixelScale)) {
            newState = eHoverStateBboxBtmRight;
        } else if (isNearbyBBoxTopRight(pos, bboxTol, pixelScale)) {
            newState = eHoverStateBboxTopRight;
        } else if (isNearbyBBoxTopLeft(pos, bboxTol, pixelScale)) {
            newState = eHoverStateBboxTopLeft;
        } else if (isNearbyBBoxMidTop(pos, bboxTol, pixelScale)) {
            newState = eHoverStateBboxMidTop;
        } else if (isNearbyBBoxMidRight(pos, bboxTol, pixelScale)) {
            newState = eHoverStateBboxMidRight;
        } else if (isNearbyBBoxMidBtm(pos, bboxTol, pixelScale)) {
            newState = eHoverStateBboxMidBtm;
        } else if (isNearbyBBoxMidLeft(pos, bboxTol, pixelScale)) {
            newState = eHoverStateBboxMidLeft;
        } else if (lastHoverState != eHoverStateNothing) {
            newState = eHoverStateNothing;
        }
        if (hoverState != newState) {
            redraw = true;
            hoverState = newState;
        }
    }
    const bool featherVisible = isFeatherVisible();

    if ((state == eEventStateNone) && (hoverState == eHoverStateNothing)) {
        if ((state != eEventStateDraggingControlPoint) && (state != eEventStateDraggingSelectedControlPoints)) {
            for (SelectedItems::const_iterator it = selectedItems.begin(); it != selectedItems.end(); ++it) {
                int index = -1;
                Bezier* isBezier = dynamic_cast<Bezier*>(it->get());
                if (isBezier) {
                    std::pair<BezierCPPtr, BezierCPPtr> nb = isBezier->isNearbyControlPoint(pos.x(), pos.y(), cpTol, Bezier::eControlPointSelectionPrefWhateverFirst, &index);
                    if ((index != -1) && ((!nb.first->isFeatherPoint() && !featherVisible) || featherVisible)) {
                        _host->setCurrentCursor(eCursorCross);
                        cursorSet = true;
                        break;
                    }
                }
            }
        }
        if (!cursorSet && (state != eEventStateDraggingLeftTangent) && (state != eEventStateDraggingRightTangent)) {
            /// find a nearby tangent
            for (SelectedCPs::const_iterator it = selectedCps.begin(); it != selectedCps.end(); ++it) {
                if (it->first->isNearbyTangent(true, time, ViewIdx(0), pos.x(), pos.y(), cpTol) != -1) {
                    _host->setCurrentCursor(eCursorCross);
                    cursorSet = true;
                    break;
                }
            }
        }
        if (!cursorSet && (state != eEventStateDraggingControlPoint) && (state != eEventStateDraggingSelectedControlPoints) && (state != eEventStateDraggingLeftTangent) && (state != eEventStateDraggingRightTangent)) {
            double bezierSelectionTolerance = kBezierSelectionTolerance * pixelScale.first;
            double nearbyBezierT;
            int nearbyBezierCPIndex;
            bool isFeather;
            BezierPtr nearbyBezier = context->isNearbyBezier(pos.x(), pos.y(), bezierSelectionTolerance, &nearbyBezierCPIndex, &nearbyBezierT, &isFeather);
            if (isFeather && !featherVisible) {
                nearbyBezier.reset();
            }
            if (nearbyBezier) {
                _host->setCurrentCursor(eCursorPointingHand);
                cursorSet = true;
            }
        }

        bool clickAnywhere = isBboxClickAnywhereEnabled();

        if (!cursorSet && (selectedCps.size() > 1)) {
            if ((clickAnywhere && isWithinSelectedCpsBBox(pos)) || (!clickAnywhere && isNearbySelectedCpsCrossHair(pos))) {
                _host->setCurrentCursor(eCursorSizeAll);
                cursorSet = true;
            }
        }

        SelectedCP nearbyFeatherBar;
        if (!cursorSet && featherVisible) {
            nearbyFeatherBar = isNearbyFeatherBar(time, pixelScale, pos);
            if (nearbyFeatherBar.first && nearbyFeatherBar.second) {
                featherBarBeingHovered = nearbyFeatherBar;
                redraw = true;
            }
        }
        if (!nearbyFeatherBar.first || !nearbyFeatherBar.second) {
            if (featherBarBeingHovered.first || featherBarBeingHovered.second) {
                featherBarBeingHovered.first.reset();
                featherBarBeingHovered.second.reset();
                redraw = true;
            }
        }

        if ((state != eEventStateNone) || featherBarBeingHovered.first || (lastHoverState != hoverState)) {
            redraw = true;
        }
        if (!cursorSet) {
            _host->setCurrentCursor(eCursorDefault);
        }
    }

    double dx = pos.x() - lastMousePos.x();
    double dy = pos.y() - lastMousePos.y();
    switch (state) {
    case eEventStateDraggingSelectedControlPoints: {
        if (bezierBeingDragged) {
            SelectedCPs cps;
            const std::list<BezierCPPtr>& c = bezierBeingDragged->getControlPoints();
            const std::list<BezierCPPtr>& f = bezierBeingDragged->getFeatherPoints();
            assert(c.size() == f.size() || !bezierBeingDragged->useFeatherPoints());
            bool useFeather = bezierBeingDragged->useFeatherPoints();
            std::list<BezierCPPtr>::const_iterator itFp = f.begin();
            for (std::list<BezierCPPtr>::const_iterator itCp = c.begin(); itCp != c.end(); ++itCp) {
                if (useFeather) {
                    cps.push_back(std::make_pair(*itCp, *itFp));
                    ++itFp;
                } else {
                    cps.push_back(std::make_pair(*itCp, BezierCPPtr()));
                }
            }
            pushUndoCommand(new MoveControlPointsUndoCommand(shared_from_this(), cps, dx, dy, time));
        } else {
            pushUndoCommand(new MoveControlPointsUndoCommand(shared_from_this(), selectedCps, dx, dy, time));
        }
        evaluateOnPenUp = true;
        computeSelectedCpsBBOX();
        didSomething = true;
        break;
    }
    case eEventStateDraggingControlPoint: {
        assert(cpBeingDragged.first);
        std::list<SelectedCP> toDrag;
        toDrag.push_back(cpBeingDragged);
        pushUndoCommand(new MoveControlPointsUndoCommand(shared_from_this(), toDrag, dx, dy, time));
        evaluateOnPenUp = true;
        computeSelectedCpsBBOX();
        didSomething = true;
    }; break;
    case eEventStateBuildingBezierControlPointTangent: {
        assert(builtBezier);
        bool isOpenBezier = selectedTool == eRotoToolOpenBezier;
        pushUndoCommand(new MakeBezierUndoCommand(shared_from_this(), builtBezier, isOpenBezier, false, dx, dy, time));
        didSomething = true;
        break;
    }
    case eEventStateBuildingEllipse: {
        bool fromCenter = ctrlDown > 0;
        bool constrained = shiftDown > 0;
        pushUndoCommand(new MakeEllipseUndoCommand(shared_from_this(), false, fromCenter, constrained, click.x(), click.y(), pos.x(), pos.y(), time));

        didSomething = true;
        evaluateOnPenUp = true;
        break;
    }
    case eEventStateBuildingRectangle: {
        bool fromCenter = ctrlDown > 0;
        bool constrained = shiftDown > 0;
        pushUndoCommand(new MakeRectangleUndoCommand(shared_from_this(), false, fromCenter, constrained, click.x(), click.y(), pos.x(), pos.y(), time));
        didSomething = true;
        evaluateOnPenUp = true;
        break;
    }
    case eEventStateDraggingLeftTangent: {
        assert(tangentBeingDragged);
        pushUndoCommand(new MoveTangentUndoCommand(shared_from_this(), dx, dy, time, tangentBeingDragged, true,
                                                   ctrlDown && !shiftDown && !altDown));
        evaluateOnPenUp = true;
        didSomething = true;
        break;
    }
    case eEventStateDraggingRightTangent: {
        assert(tangentBeingDragged);
        pushUndoCommand(new MoveTangentUndoCommand(shared_from_this(), dx, dy, time, tangentBeingDragged, false,
                                                   ctrlDown && !shiftDown && !altDown));
        evaluateOnPenUp = true;
        didSomething = true;
        break;
    }
    case eEventStateDraggingFeatherBar: {
        pushUndoCommand(new MoveFeatherBarUndoCommand(shared_from_this(), dx, dy, featherBarBeingDragged, selectedCps, time));
        evaluateOnPenUp = true;
        didSomething = true;
        break;
    }
    case eEventStateDraggingBBoxTopLeft:
    case eEventStateDraggingBBoxTopRight:
    case eEventStateDraggingBBoxBtmRight:
    case eEventStateDraggingBBoxBtmLeft: {
        QPointF center = getSelectedCpsBBOXCenter();
        double rot = 0;
        double sx = 1., sy = 1.;

        if (transformMode == eSelectedCpsTransformModeRotateAndSkew) {
            double angle = std::atan2(pos.y() - center.y(), pos.x() - center.x());
            double prevAngle = std::atan2(lastMousePos.y() - center.y(), lastMousePos.x() - center.x());
            rot = angle - prevAngle;
        } else {
            // the scale ratio is the ratio of distances to the center
            double prevDist = (lastMousePos.x() - center.x()) * (lastMousePos.x() - center.x()) + (lastMousePos.y() - center.y()) * (lastMousePos.y() - center.y());
            if (prevDist != 0) {
                double dist = (pos.x() - center.x()) * (pos.x() - center.x()) + (pos.y() - center.y()) * (pos.y() - center.y());
                double ratio = std::sqrt(dist / prevDist);
                sx *= ratio;
                sy *= ratio;
            }
        }

        double tx = 0., ty = 0.;
        double skewX = 0., skewY = 0.;
        pushUndoCommand(new TransformUndoCommand(shared_from_this(), center.x(), center.y(), rot, skewX, skewY, tx, ty, sx, sy, time));
        evaluateOnPenUp = true;
        didSomething = true;
        break;
    }
    case eEventStateDraggingBBoxMidTop:
    case eEventStateDraggingBBoxMidBtm:
    case eEventStateDraggingBBoxMidLeft:
    case eEventStateDraggingBBoxMidRight: {
        QPointF center = getSelectedCpsBBOXCenter();
        double rot = 0;
        double sx = 1., sy = 1.;
        double skewX = 0., skewY = 0.;
        double tx = 0., ty = 0.;
        TransformUndoCommand::TransformPointsSelectionEnum type = TransformUndoCommand::eTransformAllPoints;
        if (!shiftDown) {
            type = TransformUndoCommand::eTransformAllPoints;
        } else {
            if (state == eEventStateDraggingBBoxMidTop) {
                type = TransformUndoCommand::eTransformMidTop;
            } else if (state == eEventStateDraggingBBoxMidBtm) {
                type = TransformUndoCommand::eTransformMidBottom;
            } else if (state == eEventStateDraggingBBoxMidLeft) {
                type = TransformUndoCommand::eTransformMidLeft;
            } else if (state == eEventStateDraggingBBoxMidRight) {
                type = TransformUndoCommand::eTransformMidRight;
            }
        }

        const QRectF& bbox = selectedCpsBbox;

        switch (type) {
        case TransformUndoCommand::eTransformMidBottom:
            center.rx() = bbox.center().x();
            center.ry() = bbox.top();
            break;
        case TransformUndoCommand::eTransformMidTop:
            center.rx() = bbox.center().x();
            center.ry() = bbox.bottom();
            break;
        case TransformUndoCommand::eTransformMidRight:
            center.rx() = bbox.left();
            center.ry() = bbox.center().y();
            break;
        case TransformUndoCommand::eTransformMidLeft:
            center.rx() = bbox.right();
            center.ry() = bbox.center().y();
            break;
        default:
            break;
        }

        bool processX = state == eEventStateDraggingBBoxMidRight || state == eEventStateDraggingBBoxMidLeft;

        if (transformMode == eSelectedCpsTransformModeRotateAndSkew) {
            if (!processX) {
                const double addSkew = (pos.x() - lastMousePos.x()) / (pos.y() - center.y());
                skewX += addSkew;
            } else {
                const double addSkew = (pos.y() - lastMousePos.y()) / (pos.x() - center.x());
                skewY += addSkew;
            }
        } else {
            // the scale ratio is the ratio of distances to the center
            double prevDist = (lastMousePos.x() - center.x()) * (lastMousePos.x() - center.x()) + (lastMousePos.y() - center.y()) * (lastMousePos.y() - center.y());
            if (prevDist != 0) {
                double dist = (pos.x() - center.x()) * (pos.x() - center.x()) + (pos.y() - center.y()) * (pos.y() - center.y());
                double ratio = std::sqrt(dist / prevDist);
                if (processX) {
                    sx *= ratio;
                } else {
                    sy *= ratio;
                }
            }
        }

        pushUndoCommand(new TransformUndoCommand(shared_from_this(), center.x(), center.y(), rot, skewX, skewY, tx, ty, sx, sy, time));
        evaluateOnPenUp = true;
        didSomething = true;
        break;
    }
    case eEventStateNone:
        break;
    default:
        if (onPaintStatePenMotion(pos, pressure, timestamp, dx, dy)) {
            didSomething = true;
        }
        break;
    } // switch
    lastMousePos = pos;

    if (redraw) {
        redrawOverlays();
    }

    return didSomething;
} // onOverlayPenMotion

bool
RotoCurveInteract::onOverlayPenUp()
{
    undoGesture.end();

    RotoCurveContextPtr context = getCurveContext();

    assert(context);
    if (!context) {
        return false;
    }

    if (evaluateOnPenUp) {
        context->evaluateChange();
        _host->getApp()->triggerAutoSave();

        // sync other viewers linked to this roto
        redrawOverlays();
        evaluateOnPenUp = false;
    }

    bool ret = false;
    tangentBeingDragged.reset();
    bezierBeingDragged.reset();
    cpBeingDragged.first.reset();
    cpBeingDragged.second.reset();
    featherBarBeingDragged.first.reset();
    featherBarBeingDragged.second.reset();

    if ((state == eEventStateDraggingBBoxMidLeft) || (state == eEventStateDraggingBBoxMidLeft) || (state == eEventStateDraggingBBoxMidTop) || (state == eEventStateDraggingBBoxMidBtm) || (state == eEventStateDraggingBBoxTopLeft) || (state == eEventStateDraggingBBoxTopRight) || (state == eEventStateDraggingBBoxBtmRight) || (state == eEventStateDraggingBBoxBtmLeft)) {
        computeSelectedCpsBBOX();
    }

    if (onPaintStatePenUp()) {
        ret = true;
    }

    state = eEventStateNone;

    if ((selectedTool == eRotoToolDrawEllipse) || (selectedTool == eRotoToolDrawRectangle)) {
        selectedCps.clear();
        setCurrentTool(selectAllAction.lock());
        ret = true;
    }

    if (onPenUpFinished()) {
        ret = true;
    }

    return ret;
} // onOverlayPenUp

bool
RotoCurveInteract::onOverlayKeyDown(Key key)
{
    bool didSomething = false;

    if ((key == Key_Shift_L) || (key == Key_Shift_R)) {
        ++shiftDown;
    } else if ((key == Key_Control_L) || (key == Key_Control_R)) {
        ++ctrlDown;
    } else if ((key == Key_Alt_L) || (key == Key_Alt_R)) {
        ++altDown;
    }

    if (ctrlDown && !shiftDown && !altDown) {
        if (!iSelectingwithCtrlA && showCpsBbox && ((key == Key_Control_L) || (key == Key_Control_R))) {
            transformMode = transformMode == eSelectedCpsTransformModeTranslateAndScale ? eSelectedCpsTransformModeRotateAndSkew : eSelectedCpsTransformModeTranslateAndScale;
            didSomething = true;
        }
    }

    if (((key == Key_Escape) && ((selectedTool == eRotoToolDrawBezier) || (selectedTool == eRotoToolOpenBezier)))) {
        if (((selectedTool == eRotoToolDrawBezier) || (selectedTool == eRotoToolOpenBezier)) && builtBezier && !builtBezier->isCurveFinished()) {
            pushUndoCommand(new OpenCloseUndoCommand(shared_from_this(), builtBezier));

            builtBezier.reset();
            selectedCps.clear();
            setCurrentTool(selectAllAction.lock());
            getCurveContext()->evaluateChange();
            didSomething = true;
        }
    }

    return didSomething;
} // onOverlayKeyDown

bool
RotoCurveInteract::onOverlayKeyUp(Key key)
{
    bool didSomething = false;

    if ((key == Key_Shift_L) || (key == Key_Shift_R)) {
        if (shiftDown) {
            --shiftDown;
        }
    } else if ((key == Key_Control_L) || (key == Key_Control_R)) {
        if (ctrlDown) {
            --ctrlDown;
        }
    } else if ((key == Key_Alt_L) || (key == Key_Alt_R)) {
        if (altDown) {
            --altDown;
        }
    }

    if (!ctrlDown) {
        if (!iSelectingwithCtrlA && showCpsBbox && ((key == Key_Control_L) || (key == Key_Control_R))) {
            transformMode = (transformMode == eSelectedCpsTransformModeTranslateAndScale ? eSelectedCpsTransformModeRotateAndSkew : eSelectedCpsTransformModeTranslateAndScale);
            didSomething = true;
        }
    }

    if (((key == Key_Control_L) || (key == Key_Control_R)) && iSelectingwithCtrlA) {
        iSelectingwithCtrlA = false;
    }

    if (evaluateOnKeyUp) {
        getCurveContext()->evaluateChange();
        _host->getApp()->triggerAutoSave();
        redrawOverlays();
        evaluateOnKeyUp = false;
    }

    return didSomething;
} // onOverlayKeyUp

bool
RotoCurveInteract::onOverlayFocusLost()
{
    shiftDown = 0;
    ctrlDown = 0;
    altDown = 0;

    return true;
} // onOverlayFocusLost

NATRON_NAMESPACE_EXIT
