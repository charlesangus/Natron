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

#ifndef Engine_RotoCurveInteract_h
#define Engine_RotoCurveInteract_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <memory>
#include <utility>

#include <QPointF>
#include <QRectF>
#include <QtCore/QCoreApplication>

#include <ofxNatron.h>

#include "Engine/Bezier.h"
#include "Engine/BezierCP.h"
#include "Engine/EngineFwd.h"
#include "Engine/RectD.h"
#include "Engine/Transform.h"
#include "Engine/UndoGesture.h"
#include "Global/Enums.h"
#include "Global/KeySymbols.h"

NATRON_NAMESPACE_ENTER

struct PluginActionShortcut;

#define kControlPointMidSize 3
#define kBezierSelectionTolerance 8
#define kControlPointSelectionTolerance 8
#define kXHairSelectedCpsTolerance 8
#define kXHairSelectedCpsBox 8
#define kTangentHandleSelectionTolerance 8
#define kTransformArrowLenght 10
#define kTransformArrowWidth 3
#define kTransformArrowOffsetFromPoint 15

// parameters

// The toolbar
#define kRotoUIParamToolbar "Toolbar"

#define kRotoUIParamSelectionToolButton "SelectionToolButton"
#define kRotoUIParamSelectionToolButtonLabel "Selection Tool"

#define kRotoUIParamSelectAllToolButtonAction "SelectAllTool"
#define kRotoUIParamSelectAllToolButtonActionLabel "Select All Tool"
#define kRotoUIParamSelectAllToolButtonActionHint "Everything can be selected and moved"

#define kRotoUIParamSelectPointsToolButtonAction "SelectPointsTool"
#define kRotoUIParamSelectPointsToolButtonActionLabel "Select Points Tool"
#define kRotoUIParamSelectPointsToolButtonActionHint "Works only for the points of the inner shape," \
                                                     " feather points will not be taken into account."

#define kRotoUIParamSelectShapesToolButtonAction "SelectShapesTool"
#define kRotoUIParamSelectShapesToolButtonActionLabel "Select Shapes Tool"
#define kRotoUIParamSelectShapesToolButtonActionHint "Only Shapes may be selected"

#define kRotoUIParamSelectFeatherPointsToolButtonAction "SelectFeatherTool"
#define kRotoUIParamSelectFeatherPointsToolButtonActionLabel "Select Feather Points Tool"
#define kRotoUIParamSelectFeatherPointsToolButtonActionHint "Only Feather points may be selected"

#define kRotoUIParamEditPointsToolButton "EditPointsToolButton"
#define kRotoUIParamEditPointsToolButtonLabel "Points Edition Tool"

#define kRotoUIParamAddPointsToolButtonAction "AddPointsTool"
#define kRotoUIParamAddPointsToolButtonActionLabel "Add Points Tool"
#define kRotoUIParamAddPointsToolButtonActionHint "Add a control point to the shape"

#define kRotoUIParamRemovePointsToolButtonAction "RemovePointsTool"
#define kRotoUIParamRemovePointsToolButtonActionLabel "Remove Points Tool"
#define kRotoUIParamRemovePointsToolButtonActionHint "Remove a control point from the shape"

#define kRotoUIParamCuspPointsToolButtonAction "CuspPointsTool"
#define kRotoUIParamCuspPointsToolButtonActionLabel "Cusp Points Tool"
#define kRotoUIParamCuspPointsToolButtonActionHint "Cusp points on the shape"

#define kRotoUIParamSmoothPointsToolButtonAction "SmoothPointsTool"
#define kRotoUIParamSmoothPointsToolButtonActionLabel "Smooth Points Tool"
#define kRotoUIParamSmoothPointsToolButtonActionHint "Smooth points on the shape"

#define kRotoUIParamOpenCloseCurveToolButtonAction "OpenCloseShapeTool"
#define kRotoUIParamOpenCloseCurveToolButtonActionLabel "Open/Close Shape Tool"
#define kRotoUIParamOpenCloseCurveToolButtonActionHint "Open or Close shapes"

#define kRotoUIParamRemoveFeatherToolButtonAction "RemoveFeatherPointTool"
#define kRotoUIParamRemoveFeatherToolButtonActionLabel "Remove Feather Tool"
#define kRotoUIParamRemoveFeatherToolButtonActionHint "Remove feather on points"

#define kRotoUIParamBezierEditionToolButton "EditBezierToolButton"
#define kRotoUIParamBezierEditionToolButtonLabel "Shapes Drawing Tool"

#define kRotoUIParamDrawBezierToolButtonAction "DrawBezierTool"
#define kRotoUIParamDrawBezierToolButtonActionLabel "Bezier Tool"
#define kRotoUIParamDrawBezierToolButtonActionHint "Draw Bezier"

#define kRotoUIParamDrawEllipseToolButtonAction "DrawEllipseTool"
#define kRotoUIParamDrawEllipseToolButtonActionLabel "Ellipse Tool"
#define kRotoUIParamDrawEllipseToolButtonActionHint "Draw Ellipse"

#define kRotoUIParamDrawRectangleToolButtonAction "DrawRectangleTool"
#define kRotoUIParamDrawRectangleToolButtonActionLabel "Rectangle Tool"
#define kRotoUIParamDrawRectangleToolButtonActionHint "Draw Rectangle"

#define kRotoUIParamOpenBezierToolButtonAction "OpenBezierTool"
#define kRotoUIParamOpenBezierToolButtonActionLabel "Open Bezier Tool"
#define kRotoUIParamOpenBezierToolButtonActionHint "Draw an open Bezier. Press Escape or pick another tool to end it"

#define kRotoUIParamPointToolButtonAction "PointTool"
#define kRotoUIParamPointToolButtonActionLabel "Point Tool"
#define kRotoUIParamPointToolButtonActionHint "Each click makes a curve of a single point"

// The right click menu
#define kRotoUIParamRightClickMenu kNatronOfxParamRightClickMenu

// For all items
#define kRotoUIParamRightClickMenuActionRemoveItems "removeItemsAction"
#define kRotoUIParamRightClickMenuActionRemoveItemsLabel "Remove Selected Item(s)"

#define kRotoUIParamRightClickMenuActionCuspItems "cuspItemsAction"
#define kRotoUIParamRightClickMenuActionCuspItemsLabel "Cusp Selected Item(s)"

#define kRotoUIParamRightClickMenuActionSmoothItems "SmoothItemsAction"
#define kRotoUIParamRightClickMenuActionSmoothItemsLabel "Smooth Selected Item(s)"

#define kRotoUIParamRightClickMenuActionRemoveItemsFeather "removeItemsFeatherAction"
#define kRotoUIParamRightClickMenuActionRemoveItemsFeatherLabel "Remove Selected item(s) Feather"

#define kRotoUIParamRightClickMenuActionNudgeLeft "nudgeLeftAction"
#define kRotoUIParamRightClickMenuActionNudgeLeftLabel "Nudge Left"

#define kRotoUIParamRightClickMenuActionNudgeRight "nudgeRightAction"
#define kRotoUIParamRightClickMenuActionNudgeRightLabel "Nudge Right"

#define kRotoUIParamRightClickMenuActionNudgeBottom "nudgeBottomAction"
#define kRotoUIParamRightClickMenuActionNudgeBottomLabel "Nudge Bottom"

#define kRotoUIParamRightClickMenuActionNudgeTop "nudgeTopAction"
#define kRotoUIParamRightClickMenuActionNudgeTopLabel "Nudge Top"

// just for shapes
#define kRotoUIParamRightClickMenuActionSelectAll "selectAllAction"
#define kRotoUIParamRightClickMenuActionSelectAllLabel "Select All"

#define kRotoUIParamRightClickMenuActionOpenClose "openCloseAction"
#define kRotoUIParamRightClickMenuActionOpenCloseLabel "Open/Close Shape"

#define kRotoUIParamRightClickMenuActionLockShapes "lockShapesAction"
#define kRotoUIParamRightClickMenuActionLockShapesLabel "Lock Shape(s)"

// Viewer UI buttons

// Roto
#define kRotoUIParamAutoKeyingEnabled "autoKeyingEnabledButton"
#define kRotoUIParamAutoKeyingEnabledLabel "Enable Auto-Keying"
#define kRotoUIParamAutoKeyingEnabledHint "When activated any change made to a control point will set a keyframe at the current time"

#define kRotoUIParamFeatherLinkEnabled "featherLinkEnabledButton"
#define kRotoUIParamFeatherLinkEnabledLabel "Enable Feather-Link"
#define kRotoUIParamFeatherLinkEnabledHint "Feather-link: When activated the feather points will follow the same" \
                                           " movement as their counter-part does"

#define kRotoUIParamDisplayFeather "displayFeatherButton"
#define kRotoUIParamDisplayFeatherLabel "Display Feather"
#define kRotoUIParamDisplayFeatherHint "When checked, the feather curve applied to the shape(s) will be visible and editable"

#define kRotoUIParamStickySelectionEnabled "stickySelectionEnabledButton"
#define kRotoUIParamStickySelectionEnabledLabel "Enable Sticky Selection"
#define kRotoUIParamStickySelectionEnabledHint "Sticky-selection: When activated, " \
                                               " clicking outside of any shape will not clear the current selection"

#define kRotoUIParamStickyBbox "stickyBboxButton"
#define kRotoUIParamStickyBboxLabel "Enable Sticky Bounding Box"
#define kRotoUIParamStickyBboxHint "Easy bounding box manipulation: When activated, "                              \
                                   " clicking inside of the bounding box of selected points will move the points." \
                                   "When deactivated, only clicking on the cross will move the points"

#define kRotoUIParamRippleEdit "rippleEditButton"
#define kRotoUIParamRippleEditLabel "Enable Ripple Edit"
#define kRotoUIParamRippleEditLabelHint "Ripple-edit: When activated, moving a control point"     \
                                        " will move it by the same amount for all the keyframes " \
                                        "it has"

#define kRotoUIParamAddKeyFrame "addKeyframeButton"
#define kRotoUIParamAddKeyFrameLabel "Add Keyframe"
#define kRotoUIParamAddKeyFrameHint "Set a keyframe at the current time for the selected shape(s), if any"

#define kRotoUIParamRemoveKeyframe "removeKeyframeButton"
#define kRotoUIParamRemoveKeyframeLabel "Remove Keyframe"
#define kRotoUIParamRemoveKeyframeHint "Remove a keyframe at the current time for the selected shape(s), if any"

#define kRotoUIParamShowTransform "showTransformButton"
#define kRotoUIParamShowTransformLabel "Show Transform Handle"
#define kRotoUIParamShowTransformHint "When unchecked, even if the Transform tab is visible in the settings panel, the transform handle will be hidden"

/// A list of points and their counter-part, that is: either a control point and its feather point, or
/// the feather point and its associated control point
typedef std::pair<BezierCPPtr, BezierCPPtr> SelectedCP;
typedef std::list<SelectedCP> SelectedCPs;
typedef std::list<RotoDrawableItemPtr> SelectedItems;

enum EventStateEnum {
    eEventStateNone = 0,
    eEventStateDraggingControlPoint,
    eEventStateDraggingSelectedControlPoints,
    eEventStateBuildingBezierControlPointTangent,
    eEventStateBuildingEllipse,
    eEventStateBuildingRectangle,
    eEventStateDraggingLeftTangent,
    eEventStateDraggingRightTangent,
    eEventStateDraggingFeatherBar,
    eEventStateDraggingBBoxTopLeft,
    eEventStateDraggingBBoxTopRight,
    eEventStateDraggingBBoxBtmRight,
    eEventStateDraggingBBoxBtmLeft,
    eEventStateDraggingBBoxMidTop,
    eEventStateDraggingBBoxMidRight,
    eEventStateDraggingBBoxMidBtm,
    eEventStateDraggingBBoxMidLeft,
    eEventStateBuildingStroke,
    eEventStateDraggingCloneOffset,
    eEventStateDraggingBrushSize,
    eEventStateDraggingBrushOpacity,
    eEventStateDraggingCorrespondence,
};

enum HoverStateEnum {
    eHoverStateNothing = 0,
    eHoverStateBboxTopLeft,
    eHoverStateBboxTopRight,
    eHoverStateBboxBtmRight,
    eHoverStateBboxBtmLeft,
    eHoverStateBboxMidTop,
    eHoverStateBboxMidRight,
    eHoverStateBboxMidBtm,
    eHoverStateBboxMidLeft,
    eHoverStateBbox
};

enum SelectedCpsTransformModeEnum {
    eSelectedCpsTransformModeTranslateAndScale = 0,
    eSelectedCpsTransformModeRotateAndSkew = 1
};

enum RotoRoleEnum {
    eRotoRoleSelection = 0,
    eRotoRolePointsEdition,
    eRotoRoleBezierEdition,
    eRotoRolePaintBrush,
    eRotoRoleCloneBrush,
    eRotoRoleEffectBrush,
    eRotoRoleMergeBrush,
    eRotoRoleCorrespondence
};

enum RotoToolEnum {
    eRotoToolSelectAll = 0,
    eRotoToolSelectPoints,
    eRotoToolSelectCurves,
    eRotoToolSelectFeatherPoints,

    eRotoToolAddPoints,
    eRotoToolRemovePoints,
    eRotoToolRemoveFeatherPoints,
    eRotoToolOpenCloseCurve,
    eRotoToolSmoothPoints,
    eRotoToolCuspPoints,

    eRotoToolDrawBezier,
    eRotoToolDrawBSpline,
    eRotoToolDrawEllipse,
    eRotoToolDrawRectangle,

    eRotoToolSolidBrush,
    eRotoToolOpenBezier,
    eRotoToolEraserBrush,

    eRotoToolClone,
    eRotoToolReveal,

    eRotoToolBlur,
    eRotoToolSharpen,
    eRotoToolSmear,

    eRotoToolDodge,
    eRotoToolBurn,

    eRotoToolDrawPoint,
    eRotoToolCorrespondence
};

// The curve tool groups RotoCurveInteract::createToolbarKnobs() can create, as a bitmask.
enum RotoCurveToolSetEnum {
    eRotoCurveToolSetSelection = 0x1,
    eRotoCurveToolSetPointsEdition = 0x2,
    eRotoCurveToolSetShapesDrawing = 0x4,
    eRotoCurveToolSetAll = eRotoCurveToolSetSelection | eRotoCurveToolSetPointsEdition | eRotoCurveToolSetShapesDrawing,
    // Extra tools of the Shapes Drawing group, which eRotoCurveToolSetAll leaves out.
    eRotoCurveToolSetOpenBezier = 0x8,
    eRotoCurveToolSetPoint = 0x10
};

/**
 * @brief The viewer interact that edits the curves of a RotoCurveContext: selection, control point,
 * tangent and feather editing, and drawing Beziers, ellipses and rectangles. Any effect whose
 * getCurveContextKind() is not None can host one and forward its overlay actions to it.
 * RotoPaintInteract derives from it and adds the paint roles and tools through the virtual hooks.
 **/
class RotoCurveInteract
    : public std::enable_shared_from_this<RotoCurveInteract> {
    Q_DECLARE_TR_FUNCTIONS(RotoCurveInteract)

public:
    SelectedItems selectedItems;
    SelectedCPs selectedCps;
    QRectF selectedCpsBbox;
    bool showCpsBbox;

    ////This is by default eSelectedCpsTransformModeTranslateAndScale. When clicking the cross-hair in the center this will toggle the transform mode
    ////like it does in inkscape.
    SelectedCpsTransformModeEnum transformMode;
    BezierPtr builtBezier; //< the bezier currently being built
    BezierPtr bezierBeingDragged;
    SelectedCP cpBeingDragged; //< the cp being dragged
    BezierCPPtr tangentBeingDragged; //< the control point whose tangent is being dragged.
    // only relevant when the state is DRAGGING_X_TANGENT
    SelectedCP featherBarBeingDragged, featherBarBeingHovered;
    QPointF click; // used for drawing ellipses and rectangles, to handle center/constrain. May also be used for the selection bbox.
    RotoToolEnum selectedTool;
    RotoRoleEnum selectedRole;
    EventStateEnum state;
    HoverStateEnum hoverState;
    QPointF lastClickPos;
    QPointF lastMousePos;
    bool evaluateOnPenUp; //< if true the next pen up will call context->evaluateChange()
    bool evaluateOnKeyUp; //< if true the next key up will call context->evaluateChange()
    bool iSelectingwithCtrlA;
    int shiftDown;
    int ctrlDown;
    int altDown;
    // Open from pen down to pen up. The drag commands pushed in between carry its key, so one drag
    // is one undo step and the next drag, even of the same points, is another.
    UndoGesture undoGesture;

    //////// Toolbar
    KnobPageWPtr toolbarPage;
    KnobGroupWPtr selectedToolRole;
    KnobButtonWPtr selectedToolAction;
    KnobGroupWPtr selectToolGroup;
    KnobButtonWPtr selectAllAction;
    KnobButtonWPtr selectPointsAction;
    KnobButtonWPtr selectCurvesAction;
    KnobButtonWPtr selectFeatherPointsAction;
    KnobGroupWPtr pointsEditionToolGroup;
    KnobButtonWPtr addPointsAction;
    KnobButtonWPtr removePointsAction;
    KnobButtonWPtr cuspPointsAction;
    KnobButtonWPtr smoothPointsAction;
    KnobButtonWPtr openCloseCurveAction;
    KnobButtonWPtr removeFeatherAction;
    KnobGroupWPtr bezierEditionToolGroup;
    KnobButtonWPtr drawBezierAction;
    KnobButtonWPtr drawEllipseAction;
    KnobButtonWPtr drawRectangleAction;
    KnobButtonWPtr drawOpenBezierAction;
    KnobButtonWPtr drawPointAction;

    //////Right click menu
    KnobChoiceWPtr rightClickMenuKnob;

    // Right click on point
    KnobButtonWPtr removeItemsMenuAction;
    KnobButtonWPtr cuspItemMenuAction;
    KnobButtonWPtr smoothItemMenuAction;
    KnobButtonWPtr removeItemFeatherMenuAction;
    KnobButtonWPtr nudgeLeftMenuAction, nudgeRightMenuAction, nudgeBottomMenuAction, nudgeTopMenuAction;

    // Right click on curve
    KnobButtonWPtr selectAllMenuAction;
    KnobButtonWPtr openCloseMenuAction;
    KnobButtonWPtr lockShapeMenuAction;

    // Roto buttons
    KnobButtonWPtr autoKeyingEnabledButton;
    KnobButtonWPtr featherLinkEnabledButton;
    KnobButtonWPtr displayFeatherEnabledButton;
    KnobButtonWPtr stickySelectionEnabledButton;
    KnobButtonWPtr bboxClickAnywhereButton;
    KnobButtonWPtr rippleEditEnabledButton;
    KnobButtonWPtr addKeyframeButton;
    KnobButtonWPtr removeKeyframeButton;
    KnobButtonWPtr showTransformHandle;

private:
    EffectInstance* _host;

    struct MakeSharedEnabler;

protected:
    // constructors should be privatized in any class that derives from std::enable_shared_from_this<>
    explicit RotoCurveInteract(EffectInstance* host);

public:
    static RotoCurveInteractPtr create(EffectInstance* host);

    virtual ~RotoCurveInteract();

    EffectInstance* getHost() const
    {
        return _host;
    }

    NodePtr getNode() const;

    RotoCurveContextPtr getCurveContext() const;

    double getCurrentTime() const;

    std::pair<double, double> getPixelScale() const;

    void pushUndoCommand(UndoCommand* command);

    bool isFeatherVisible() const;

    RotoToolEnum getSelectedTool() const
    {
        return selectedTool;
    }

    bool isStickySelectionEnabled() const;

    virtual bool getRoleForGroup(const KnobGroupPtr& group, RotoRoleEnum* role) const;
    virtual bool getToolForAction(const KnobButtonPtr& action, RotoToolEnum* tool) const;

    bool onRoleChangedInternal(const KnobGroupPtr& roleGroup);

    bool onToolChangedInternal(const KnobButtonPtr& actionButton);

    /**
     * @brief Creates the toolbar page and the tool groups toolSet (a RotoCurveToolSetEnum mask)
     * asks for, in the order Selection, Points Edition, Shapes Drawing. The host appends its own
     * tool groups to the returned page and picks the default tool.
     **/
    KnobPagePtr createToolbarKnobs(int toolSet);

    /**
     * @brief Creates the curve viewer buttons (auto-keying, feather link, feather display, sticky
     * selection, bbox click anywhere, ripple edit, add/remove keyframe, show transform) on page and
     * adds them to the viewer UI. The viewer shows its knobs in the order they were added, so a
     * host calls this before adding its own viewer knobs.
     **/
    void createViewerButtonKnobs(const KnobPagePtr& page);

    /**
     * @brief Creates the right-click menu choice and its actions on page.
     **/
    void createRightClickMenuKnobs(const KnobPagePtr& page);

    /**
     * @brief Keeps the selection and lock state in step with the curve context and redraws the
     * host's overlay when they, or the context's overlays, change.
     **/
    void connectToCurveContext();

    void onCurveSelectionChanged(int reason);

    void onCurveLockedChanged(int reason);

    // The host's refreshExtraStateAfterTimeChanged() forwards to this.
    virtual void refreshAfterTimeChanged(double time);

    // The shortcuts of the knobs createViewerButtonKnobs(), createToolbarKnobs() and
    // createRightClickMenuKnobs() create, for the host's getPluginShortcuts().
    static void getViewerButtonShortcuts(std::list<PluginActionShortcut>* shortcuts);
    static void getToolbarShortcuts(int toolSet, std::list<PluginActionShortcut>* shortcuts);
    static void getRightClickMenuShortcuts(std::list<PluginActionShortcut>* shortcuts);

    /**
     * @brief Handles a change of one of the toolbar, viewer-button or right-click menu knobs.
     * Returns false if k is none of them. Otherwise *ret is the result of the host's knobChanged.
     **/
    virtual bool onKnobChanged(KnobI* k, double time, bool* ret);

    void clearSelection();

    void clearCPSSelection();

    void clearBeziersSelection();

    bool hasSelection() const;

    void onCurveLockedChangedRecursive(const RotoItemPtr& item, bool* ret);

    bool removeItemFromSelection(const RotoDrawableItemPtr& b);

    void computeSelectedCpsBBOX();

    QPointF getSelectedCpsBBOXCenter();

    void drawSelectedCpsBBOX();

    void drawEllipse(double x,
                     double y,
                     double radiusX,
                     double radiusY,
                     int l,
                     double r,
                     double g,
                     double b,
                     double a);

    /// by default draws a vertical arrow, which can be rotated by rotate amount.
    void drawArrow(double centerX, double centerY, double rotate, bool hovered, const std::pair<double, double>& pixelScale);

    /// same as drawArrow but the two ends will make an angle of 90 degrees
    void drawBendedArrow(double centerX, double centerY, double rotate, bool hovered, const std::pair<double, double>& pixelScale);

    void handleBezierSelection(const BezierPtr& curve);

    void handleControlPointSelection(const std::pair<BezierCPPtr, BezierCPPtr>& p);

    void drawSelectedCp(double time,
                        const BezierCPPtr& cp,
                        double x, double y,
                        const Transform::Matrix3x3& transform);

    std::pair<BezierCPPtr, BezierCPPtr> isNearbyFeatherBar(double time, const std::pair<double, double>& pixelScale, const QPointF& pos) const;

    bool isNearbySelectedCpsCrossHair(const QPointF& pos) const;

    bool isWithinSelectedCpsBBox(const QPointF& pos) const;

    bool isNearbyBBoxTopLeft(const QPointF& p, double tolerance, const std::pair<double, double>& pixelScale) const;
    bool isNearbyBBoxTopRight(const QPointF& p, double tolerance, const std::pair<double, double>& pixelScale) const;
    bool isNearbyBBoxBtmLeft(const QPointF& p, double tolerance, const std::pair<double, double>& pixelScale) const;
    bool isNearbyBBoxBtmRight(const QPointF& p, double tolerance, const std::pair<double, double>& pixelScale) const;

    bool isNearbyBBoxMidTop(const QPointF& p, double tolerance, const std::pair<double, double>& pixelScale) const;
    bool isNearbyBBoxMidRight(const QPointF& p, double tolerance, const std::pair<double, double>& pixelScale) const;
    bool isNearbyBBoxMidBtm(const QPointF& p, double tolerance, const std::pair<double, double>& pixelScale) const;
    bool isNearbyBBoxMidLeft(const QPointF& p, double tolerance, const std::pair<double, double>& pixelScale) const;

    bool isNearbySelectedCpsBoundingBox(const QPointF& pos, double tolerance) const;

    EventStateEnum isMouseInteractingWithCPSBbox(const QPointF& pos, double tolerance, const std::pair<double, double>& pixelScale) const;

    bool isBboxClickAnywhereEnabled() const;

    void showMenuForControlPoint(const BezierCPPtr& cp);

    void showMenuForCurve(const BezierPtr& curve);

    void setCurrentTool(const KnobButtonPtr& tool);

    /**
     * @brief Set the selection to be the given beziers and the given control points.
     * This can only be called on the main-thread.
     **/
    void setSelection(const std::list<RotoDrawableItemPtr>& selectedBeziers,
                      const std::list<std::pair<BezierCPPtr, BezierCPPtr>>& selectedCps);
    void setSelection(const BezierPtr& curve,
                      const std::pair<BezierCPPtr, BezierCPPtr>& point);

    void getSelection(std::list<RotoDrawableItemPtr>* selectedBeziers,
                      std::list<std::pair<BezierCPPtr, BezierCPPtr>>* selectedCps);

    void setBuiltBezier(const BezierPtr& curve);

    /**
     * @brief What the point tool does on a click: makes a finished open curve of one control
     * point at (x, y), selects it, and pushes the undo command.
     **/
    BezierPtr makePointCurve(double x, double y, double time);

    BezierPtr getBezierBeingBuild() const;

    bool smoothSelectedCurve();
    bool cuspSelectedCurve();
    bool removeFeatherForSelectedCurve();
    bool lockSelectedCurves();

    /**
     *@brief Moves of the given pixel the selected control points.
     * This takes into account the zoom factor.
     **/
    bool moveSelectedCpsWithKeyArrows(int x, int y);

    void evaluate(bool redraw);
    void autoSaveAndRedraw();

    void redrawOverlays();

    /**
     * @brief Calls RotoContext::removeItem but also clears some pointers if they point to
     * this curve. For undo/redo purpose.
     **/
    virtual void removeCurve(const RotoDrawableItemPtr& curve);

    // The host effect's overlay actions forward to these.
    void drawOverlay(double time);
    bool onOverlayPenDown(double time, const QPointF& pos, double pressure, double timestamp, PenType pen);
    bool onOverlayPenMotion(double time, const QPointF& pos, double pressure, double timestamp);
    bool onOverlayPenUp();
    bool onOverlayPenDoubleClicked(const QPointF& pos);
    bool onOverlayKeyDown(Key key);
    bool onOverlayKeyUp(Key key);
    bool onOverlayFocusLost();
    void onViewportSelectionCleared();
    void onViewportSelectionUpdated(const RectD& rectangle, bool onRelease);

protected:
    // Paint hooks: no-ops here, RotoPaintInteract implements the paint roles and tools with them.

    // Called by onRoleChangedInternal() once role is the selected role.
    virtual void onRoleSelected(RotoRoleEnum /*role*/) { }

    // Called by onToolChangedInternal() once tool is the selected tool.
    virtual void onToolSelected(const KnobButtonPtr& /*actionButton*/, RotoToolEnum /*tool*/) { }

    // Draws the paint tool cursor, after the curves.
    virtual void drawPaintToolOverlay(double /*screenPixelRatio*/, const std::pair<double, double>& /*viewportSize*/) { }

    // Whether the overlay draws curve and lets the pointer pick it.
    virtual bool isCurveShown(const BezierPtr& /*curve*/) const
    {
        return true;
    }

    // The colour the overlay draws an unlocked curve in.
    virtual void getCurveOverlayColor(const RotoDrawableItemPtr& item, double color[4]) const;

    // Draws what the host adds over the curves, in the passive overlay too.
    virtual void drawOverlayExtras(double /*time*/, double /*screenPixelRatio*/, const std::pair<double, double>& /*pixelScale*/) { }

    // The nearest shown curve within tolerance of pos, as RotoCurveContext::isNearbyBezier() finds it.
    BezierPtr nearbyShownBezier(const QPointF& pos, double tolerance, int* index, double* t, bool* feather) const;

    // Called first thing on pen down.
    virtual void onPenDownStarted(PenType /*pen*/) { }

    // Pen down with a tool the curve interact doesn't handle. Returns true if it did something.
    virtual bool onPaintToolPenDown(double /*time*/, const QPointF& /*pos*/, double /*pressure*/, double /*timestamp*/)
    {
        return false;
    }

    // Sets the cursor for a paint role on pen motion. Returns true if it set one.
    virtual bool setPaintToolCursor(bool* /*redraw*/)
    {
        return false;
    }

    // Pen motion in a state the curve interact doesn't handle. Returns true if it did something.
    virtual bool onPaintStatePenMotion(const QPointF& /*pos*/, double /*pressure*/, double /*timestamp*/, double /*dx*/, double /*dy*/)
    {
        return false;
    }

    // Pen up in a state the curve interact doesn't handle, before the state is reset. Returns true if it did something.
    virtual bool onPaintStatePenUp()
    {
        return false;
    }

    // Pen up, after the state is reset. Returns true if it did something.
    virtual bool onPenUpFinished()
    {
        return false;
    }
};

NATRON_NAMESPACE_EXIT

#endif // Engine_RotoCurveInteract_h
