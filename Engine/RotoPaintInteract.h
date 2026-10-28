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

#ifndef ROTOPAINTINTERACT_H
#define ROTOPAINTINTERACT_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <vector>

#include <QPointF>
#include <QRectF>

#include <ofxNatron.h>

#include "Engine/Bezier.h"
#include "Engine/BezierCP.h"
#include "Engine/EngineFwd.h"
#include "Engine/RotoCurveInteract.h"

NATRON_NAMESPACE_ENTER

#define kRotoUIParamPaintBrushToolButton "PaintBrushToolButton"
#define kRotoUIParamPaintBrushToolButtonLabel "Paint Brush Tool"

#define kRotoUIParamDrawBrushToolButtonAction "PaintSolidTool"
#define kRotoUIParamDrawBrushToolButtonActionLabel "Solid Paint Brush Tool"
#define kRotoUIParamDrawBrushToolButtonActionHint "Draw using the pen"

#define kRotoUIParamPencilToolButtonAction "PencilTool"
#define kRotoUIParamPencilToolButtonActionLabel "Pencil Tool"
#define kRotoUIParamPencilToolButtonActionHint "Draw open bezier"

#define kRotoUIParamEraserToolButtonAction "EraserTool"
#define kRotoUIParamEraserToolButtonActionLabel "Eraser Tool"
#define kRotoUIParamEraserToolButtonActionHint "Use the Eraser"

#define kRotoUIParamCloneBrushToolButton "CloneToolButton"
#define kRotoUIParamCloneBrushToolButtonLabel "Clone Tool"

#define kRotoUIParamCloneToolButtonAction "CloneTool"
#define kRotoUIParamCloneToolButtonActionLabel "Clone Tool"
#define kRotoUIParamCloneToolButtonActionHint "Draw with the pen to clone areas of the image. Hold CTRL to move the clone offset."

#define kRotoUIParamRevealToolButtonAction "RevealTool"
#define kRotoUIParamRevealToolButtonActionLabel "Reveal Tool"
#define kRotoUIParamRevealToolButtonActionHint "Draw with the pen to apply the effect from the selected source. Hold CTRL to move the offset"

#define kRotoUIParamEffectBrushToolButton "EffectToolButton"
#define kRotoUIParamEffectBrushToolButtonLabel "Effect Tool"

#define kRotoUIParamBlurToolButtonAction "BlurTool"
#define kRotoUIParamBlurToolButtonActionLabel "Blur Tool"
#define kRotoUIParamBlurToolButtonActionHint "Draw with the pen to apply a blur"

#define kRotoUIParamSmearToolButtonAction "SmearTool"
#define kRotoUIParamSmearToolButtonActionLabel "Smear Tool"
#define kRotoUIParamSmearToolButtonActionHint "Draw with the pen to blur and displace a portion of the source image along the direction of the pen"

#define kRotoUIParamMergeBrushToolButton "MergeToolButton"
#define kRotoUIParamMergeBrushToolButtonLabel "Merging Tool"

#define kRotoUIParamDodgeToolButtonAction "DodgeTool"
#define kRotoUIParamDodgeToolButtonActionLabel "Dodge Tool"
#define kRotoUIParamDodgeToolButtonActionHint "Make the source image brighter"

#define kRotoUIParamBurnToolButtonAction "BurnTool"
#define kRotoUIParamBurnToolButtonActionLabel "Burn Tool"
#define kRotoUIParamBurnToolButtonActionHint "Make the source image darker"

// RotoPaint
#define kRotoUIParamColorWheel "strokeColorButton"
#define kRotoUIParamColorWheelLabel "Stroke Color"
#define kRotoUIParamColorWheelHint "The color of the next paint brush stroke to be painted"

#define kRotoUIParamBlendingOp "blendingModeButton"
#define kRotoUIParamBlendingOpLabel "Blending Mode"
#define kRotoUIParamBlendingOpHint "The blending mode of the next brush stroke"

#define kRotoUIParamOpacity "opacitySpinbox"
#define kRotoUIParamOpacityLabel "Opacity"
#define kRotoUIParamOpacityHint "The opacity of the next brush stroke to be painted. Use CTRL + SHIFT + drag " \
    "with the mouse to change the opacity."

#define kRotoUIParamPressureOpacity "pressureOpacityButton"
#define kRotoUIParamPressureOpacityLabel "Pressure Affects Opacity"
#define kRotoUIParamPressureOpacityHint "If checked, the pressure of the pen will dynamically alter the opacity of the next " \
    "brush stroke."

#define kRotoUIParamSize "sizeSpinbox"
#define kRotoUIParamSizeLabel "Brush Size"
#define kRotoUIParamSizeHint "The size of the next brush stroke to be painted. Use SHIFT + drag with the mouse " \
    "to change the size."

#define kRotoUIParamPressureSize "pressureSizeButton"
#define kRotoUIParamPressureSizeLabel "Pressure Affects Size"
#define kRotoUIParamPressureSizeHint "If checked, the pressure of the pen will dynamically alter the size of the next " \
    "brush stroke."

#define kRotoUIParamHardness "hardnessSpinbox"
#define kRotoUIParamHardnessLabel "Brush Hardness"
#define kRotoUIParamHardnessHint "The hardness of the next brush stroke to be painted"

#define kRotoUIParamPressureHardness "pressureHardnessButton"
#define kRotoUIParamPressureHardnessLabel "Pressure Affects Hardness"
#define kRotoUIParamPressureHardnessHint "If checked, the pressure of the pen will dynamically alter the hardness of the next " \
    "brush stroke"

#define kRotoUIParamBuildUp "buildUpButton"
#define kRotoUIParamBuildUpLabel "Build-Up"
#define kRotoUIParamBuildUpHint "When build-up is enabled, the next brush stroke will build up " \
    "when painted over itself"

#define kRotoUIParamAutoConnectViewer "autoConnectViewerButton"
#define kRotoUIParamAutoConnectViewerLabel "Auto-connect Viewer"
#define kRotoUIParamAutoConnectViewerHint "Auto-connect the viewer(s) to the RotoPaint node when using one of the paint tools."

#define kRotoUIParamEffect "effectSpinbox"
#define kRotoUIParamEffectLabel "Effect Strength"
#define kRotoUIParamEffectHint "The strength of the next paint brush effect"

#define kRotoUIParamTimeOffset "timeOffsetSpinbox"
#define kRotoUIParamTimeOffsetLabel "Time Offset"
#define kRotoUIParamTimeOffsetHint "When the Clone tool is used, this determines depending on the time offset " \
    "mode the source frame to clone. When in absolute mode, this is the frame " \
    "number of the source, when in relative mode, this is an offset relative " \
    "to the current frame"

#define kRotoUIParamTimeOffsetMode "timeOffsetModeChoice"
#define kRotoUIParamTimeOffsetModeLabel "Time Offset Mode"
#define kRotoUIParamTimeOffsetModeHint "When in absolute mode, this is the frame number of the source, " \
    "when in relative mode, this is an offset relative to " \
    "the current frame" \

#define kRotoUIParamSourceType "sourceTypeChoice"
#define kRotoUIParamSourceTypeLabel "Source"
#define kRotoUIParamSourceTypeHint "Source color used for painting the stroke when the Reveal/Clone tools are used:\n" \
    "- foreground: the painted result at this point in the hierarchy,\n" \
    "- background: the original image unpainted connected to bg,\n" \
    "- backgroundN: the original image unpainted connected to bgN"

#define kRotoUIParamResetCloneOffset "resetCloneOffsetButton"
#define kRotoUIParamResetCloneOffsetLabel "Reset Transform"
#define kRotoUIParamResetCloneOffsetHint "Reset the transform applied before cloning to identity"

#define kRotoUIParamMultiStrokeEnabled "multiStrokeEnabledButton"
#define kRotoUIParamMultiStrokeEnabledLabel "Multi-Stroke"
#define kRotoUIParamMultiStrokeEnabledHint "When checked, strokes will be appended to the same item " \
    "in the hierarchy as long as the same tool is selected.\n" \
    "Select another tool to make a new item."


// Shortcuts

#define kShortcutIDActionRotoDelete "delete"
#define kShortcutDescActionRotoDelete "Delete Element"

#define kShortcutIDActionRotoCloseBezier "closeBezier"
#define kShortcutDescActionRotoCloseBezier "Close Bezier"

#define kShortcutIDActionRotoSelectAll "selectAll"
#define kShortcutDescActionRotoSelectAll "Select All"

#define kShortcutIDActionRotoSelectionTool "selectionTool"
#define kShortcutDescActionRotoSelectionTool "Switch to Selection Mode"

#define kShortcutIDActionRotoAddTool "addTool"
#define kShortcutDescActionRotoAddTool "Switch to Add Mode"

#define kShortcutIDActionRotoEditTool "editTool"
#define kShortcutDescActionRotoEditTool "Switch to Edition Mode"

#define kShortcutIDActionRotoBrushTool "brushTool"
#define kShortcutDescActionRotoBrushTool "Switch to Brush Mode"

#define kShortcutIDActionRotoCloneTool "cloneTool"
#define kShortcutDescActionRotoCloneTool "Switch to Clone Mode"

#define kShortcutIDActionRotoEffectTool "EffectTool"
#define kShortcutDescActionRotoEffectTool "Switch to Effect Mode"

#define kShortcutIDActionRotoColorTool "colorTool"
#define kShortcutDescActionRotoColorTool "Switch to Color Mode"

#define kShortcutIDActionRotoNudgeLeft "nudgeLeft"
#define kShortcutDescActionRotoNudgeLeft "Move Bezier to the Left"

#define kShortcutIDActionRotoNudgeRight "nudgeRight"
#define kShortcutDescActionRotoNudgeRight "Move Bezier to the Right"

#define kShortcutIDActionRotoNudgeBottom "nudgeBottom"
#define kShortcutDescActionRotoNudgeBottom "Move Bezier to the Bottom"

#define kShortcutIDActionRotoNudgeTop "nudgeTop"
#define kShortcutDescActionRotoNudgeTop "Move Bezier to the Top"

#define kShortcutIDActionRotoSmooth "smooth"
#define kShortcutDescActionRotoSmooth "Smooth Bezier"

#define kShortcutIDActionRotoCuspBezier "cusp"
#define kShortcutDescActionRotoCuspBezier "Cusp Bezier"

#define kShortcutIDActionRotoRemoveFeather "rmvFeather"
#define kShortcutDescActionRotoRemoveFeather "Remove Feather"

#define kShortcutIDActionRotoLockCurve "lock"
#define kShortcutDescActionRotoLockCurve "Lock Shape"

class RotoPaintInteract;
struct RotoPaintPrivate
{
    RotoPaint* publicInterface;
    bool isPaintByDefault;
    RotoPaintInteractPtr ui;

    RotoPaintPrivate(RotoPaint* publicInterface,
                     bool isPaintByDefault);
};

class RotoPaintInteract
    : public RotoCurveInteract {
public:
    RotoPaintPrivate* p;
    RotoStrokeItemPtr strokeBeingPaint;
    int strokeBeingPaintedTimelineFrame;// the frame at which we painted the last brush stroke
    std::pair<double, double> cloneOffset;
    KnobButtonWPtr lastPaintToolAction;
    bool lastTabletDownTriggeredEraser;
    QPointF mouseCenterOnSizeChange;

    //////// Toolbar
    KnobGroupWPtr paintBrushToolGroup;
    KnobButtonWPtr brushAction;
    KnobButtonWPtr pencilAction;
    KnobButtonWPtr eraserAction;
    KnobGroupWPtr cloneBrushToolGroup;
    KnobButtonWPtr cloneAction;
    KnobButtonWPtr revealAction;
    KnobGroupWPtr effectBrushToolGroup;
    KnobButtonWPtr blurAction;
    KnobButtonWPtr smearAction;
    KnobGroupWPtr mergeBrushToolGroup;
    KnobButtonWPtr dodgeAction;
    KnobButtonWPtr burnAction;

    // RotoPaint buttons
    KnobColorWPtr colorWheelButton;
    KnobChoiceWPtr compositingOperatorChoice;
    KnobDoubleWPtr opacitySpinbox;
    KnobButtonWPtr pressureOpacityButton;
    KnobDoubleWPtr sizeSpinbox;
    KnobButtonWPtr pressureSizeButton;
    KnobDoubleWPtr hardnessSpinbox;
    KnobButtonWPtr pressureHardnessButton;
    KnobButtonWPtr buildUpButton;
    KnobButtonWPtr autoConnectViewerButton;
    KnobDoubleWPtr effectSpinBox;
    KnobIntWPtr timeOffsetSpinBox;
    KnobChoiceWPtr timeOffsetModeChoice;
    KnobChoiceWPtr sourceTypeChoice;
    KnobButtonWPtr resetCloneOffsetButton;
    KnobBoolWPtr multiStrokeEnabled;


private:
    struct MakeSharedEnabler;

    // constructors should be privatized in any class that derives from std::enable_shared_from_this<>

    RotoPaintInteract(RotoPaintPrivate* p);

public:
    static RotoPaintInteractPtr create(RotoPaintPrivate* p);

    RotoContextPtr getContext();

    bool isMultiStrokeEnabled() const;

    virtual bool getRoleForGroup(const KnobGroupPtr& group, RotoRoleEnum* role) const OVERRIDE FINAL;
    virtual bool getToolForAction(const KnobButtonPtr& action, RotoToolEnum* tool) const OVERRIDE FINAL;

    virtual bool onKnobChanged(KnobI* k, double time, bool* ret) OVERRIDE FINAL;

    void makeStroke(bool prepareForLater, const RotoPoint& p);

    void checkViewersAreDirectlyConnected();

    void onBreakMultiStrokeTriggered();

    virtual void refreshAfterTimeChanged(double time) OVERRIDE FINAL;

    virtual void removeCurve(const RotoDrawableItemPtr& curve) OVERRIDE FINAL;

private:
    /**
     * @brief A stroke as the overlay draws it until its render catches up: dabs placed, sized and shaded by the stroke
     * renderer's rules, in canonical coordinates.
     **/
    struct StrokePreview {
        struct Dab {
            double x, y;
            double innerRadius, outerRadius;
            // Flat at opacities[0] when solid, else the renderer's nine ramp stops from innerRadius to outerRadius.
            bool solid;
            float opacities[9];
            // The point ending the segment the dab lies on.
            int pointIndex;
        };

        struct Vertex {
            float x, y;
            unsigned char rgba[4];
        };

        // Tools other than the solid brush show where they act, not what they do.
        bool footprintOnly;
        unsigned char colour[3];
        double opacity, size, hardness, spacing;
        bool pressureOpacity, pressureSize, pressureHardness, buildUp;

        int pointCount;
        double lastX, lastY, lastPressure, distToNext;
        std::vector<Dab> dabs;
        double bbox[4];

        // The stroke this preview stands in for, once the engine has it.
        bool engineStarted;
        RotoStrokeItemWPtr item;
        int multiStrokeIndex;
        bool neatRenderRequested;

        // Dab triangles, appended as dabs arrive and rebuilt when the zoom calls for another tessellation.
        std::vector<Vertex> vertices;
        std::vector<std::size_t> dabFirstVertex;
        double tessellationScale;
    };

    struct DeferredPenEvent {
        enum TypeEnum {
            eTypeDown,
            eTypeMotion,
            eTypeUp
        };

        TypeEnum type;
        QPointF pos;
        double pressure;
        double timestamp;
        RotoToolEnum tool;
    };

    std::list<StrokePreview> strokePreviews;

    // The preview of the stroke the engine is building, if any.
    StrokePreview* engineStrokePreview;

    // Pen events of strokes begun while the previous stroke's neat render runs, replayed once it lands.
    std::list<DeferredPenEvent> deferredPenEvents;
    bool neatRenderPending;

    bool mustDeferPenEvents() const
    {
        return neatRenderPending || !deferredPenEvents.empty();
    }

    void beginStrokePreview(const QPointF& pos, double pressure);
    static void addStrokePreviewPoint(StrokePreview& preview, double x, double y, double pressure);
    static void tessellateStrokePreview(StrokePreview& preview, std::size_t firstDab, double pixelScale);
    void drawStrokePreviews();

    void beginStroke(const QPointF& pos, double pressure, double timestamp);
    bool continueStroke(const QPointF& pos, double pressure, double timestamp);
    void finishStroke();
    void onNeatRenderLanded();
    void replayDeferredPenEvents();

    RotoPaintInteractPtr shared_from_this_paint()
    {
        return std::static_pointer_cast<RotoPaintInteract>(shared_from_this());
    }

    virtual void onRoleSelected(RotoRoleEnum role) OVERRIDE FINAL;
    virtual void onToolSelected(const KnobButtonPtr& actionButton, RotoToolEnum tool) OVERRIDE FINAL;
    virtual void drawPaintToolOverlay(double screenPixelRatio, const std::pair<double, double>& viewportSize) OVERRIDE FINAL;
    virtual void onPenDownStarted(PenType pen) OVERRIDE FINAL;
    virtual bool onPaintToolPenDown(double time, const QPointF& pos, double pressure, double timestamp) OVERRIDE FINAL;
    virtual bool setPaintToolCursor(bool* redraw) OVERRIDE FINAL;
    virtual bool onPaintStatePenMotion(const QPointF& pos, double pressure, double timestamp, double dx, double dy) OVERRIDE FINAL;
    virtual bool onPaintStatePenUp() OVERRIDE FINAL;
    virtual bool onPenUpFinished() OVERRIDE FINAL;
};

NATRON_NAMESPACE_EXIT

#endif // ROTOPAINTINTERACT_H
