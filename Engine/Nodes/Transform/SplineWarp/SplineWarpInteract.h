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

#ifndef Engine_Nodes_Transform_SplineWarp_SplineWarpInteract_h
#define Engine_Nodes_Transform_SplineWarp_SplineWarpInteract_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <memory>
#include <string>

#include <QCoreApplication>
#include <QPointF>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpContext.h"
#include "Engine/RotoCurveContext.h"
#include "Engine/RotoCurveInteract.h"

#define kSplineWarpUIParamCorrespondenceTools "CorrespondenceTools"
#define kSplineWarpUIParamCorrespondenceToolsLabel "Correspondence Tools"
#define kSplineWarpUIParamCorrespondenceTool "CorrespondenceTool"
#define kSplineWarpUIParamCorrespondenceToolLabel "Correspondence Tool"
#define kSplineWarpUIParamCorrespondenceToolHint "Click a linked curve to pin that point to its partner on the other curve of the link. " \
                                                 "Drag either end of a correspondence along its curve. Delete or Backspace removes the selected one"
#define kSplineWarpUIParamShow "show"
#define kSplineWarpUIParamShowLabel "Show"
#define kSplineWarpUIParamShowHint "Which curves of the links the viewer shows: the sources, the destinations, or both, joined by " \
                                   "lines between the points that pair up. Boundaries and unlinked curves are always shown"
#define kSplineWarpUIParamOutputWarped "outputWarped"
#define kSplineWarpUIParamOutputWarpedLabel "Output Warped"
#define kSplineWarpUIParamOutputWarpedHint "When off, the node outputs its Source unwarped, so you can compare. It is not saved, " \
                                           "and is on again when the project is loaded"

NATRON_NAMESPACE_ENTER

enum SplineWarpShowEnum {
    eSplineWarpShowSource = 0,
    eSplineWarpShowDestination,
    eSplineWarpShowBoth,
};

// The warp actions on the selected curves.
enum SplineWarpActionEnum {
    eSplineWarpActionDuplicateAndLink = 0,
    eSplineWarpActionLink,
    eSplineWarpActionUnlink,
    eSplineWarpActionSwap,
    eSplineWarpActionToggleHardBoundary,
    eSplineWarpActionToggleSoftBoundary,
};

class SplineWarpInteract;
typedef std::shared_ptr<SplineWarpInteract> SplineWarpInteractPtr;

/**
 * @brief SplineWarp's viewer interact: the curve tools, with the open Bezier and point tools, the
 * correspondence tool, and the warp actions on the selected curves.
 *
 * Each warp action returns an empty string when done, or why it refused, which it also shows in
 * the status bar; an action that refuses changes nothing. An action that edits pushes one undo
 * command, which it also returns in pushed when given.
 **/
class SplineWarpInteract
    : public RotoCurveInteract {
    Q_DECLARE_TR_FUNCTIONS(SplineWarpInteract)

public:
    KnobGroupWPtr correspondenceToolGroup;
    KnobButtonWPtr correspondenceAction;
    KnobChoiceWPtr showChoice;
    KnobButtonWPtr outputWarpedButton;

private:
    struct MakeSharedEnabler;

    // constructors should be privatized in any class that derives from std::enable_shared_from_this<>
    explicit SplineWarpInteract(EffectInstance* host);

public:
    static SplineWarpInteractPtr create(EffectInstance* host);

    virtual ~SplineWarpInteract();

    // The RotoCurveToolSetEnum mask of SplineWarp's curve tools.
    static int getCurveToolSet();

    /**
     * @brief The curve toolbar, then the correspondence tool group.
     **/
    KnobPagePtr createToolbar();

    static void getToolbarShortcuts(std::list<PluginActionShortcut>* shortcuts);

    /**
     * @brief The show choice and the output toggle, on page and in the viewer. Neither is saved;
     * the toggle drives the render.
     **/
    void createDisplayKnobs(const KnobPagePtr& page);

    SplineWarpShowEnum getShow() const;

    // The output toggle, read by renders on any thread.
    bool isOutputWarped() const;

    SplineWarpContextPtr getWarpContext() const;

    virtual bool getRoleForGroup(const KnobGroupPtr& group, RotoRoleEnum* role) const OVERRIDE FINAL;
    virtual bool getToolForAction(const KnobButtonPtr& action, RotoToolEnum* tool) const OVERRIDE FINAL;
    virtual bool onKnobChanged(KnobI* k, double time, bool* ret) OVERRIDE FINAL;

    /**
     * @brief Duplicate and Link on the one selected curve: the copy lands on it, just above it in
     * its layer, as the destination of a link from it, and becomes the selection.
     **/
    std::string duplicateAndLink(UndoCommandPtr* pushed = 0);

    /**
     * @brief Links the two selected curves, the first selected as the source, aligned
     * automatically, with no correspondences.
     **/
    std::string linkSelected(UndoCommandPtr* pushed = 0);

    std::string unlinkSelected(UndoCommandPtr* pushed = 0);

    /**
     * @brief Swaps the source and destination of the links of the selected curves, keeping how
     * their points pair at the current time.
     **/
    std::string swapSelected(UndoCommandPtr* pushed = 0);

    /**
     * @brief Makes every selected curve a boundary of the given kind, or, when they all are, none
     * of them. A curve made one kind of boundary stops being the other.
     **/
    std::string toggleBoundarySelected(bool soft,
                                       UndoCommandPtr* pushed = 0);

    // Why toggleBoundarySelected(soft) would refuse the current selection, empty when it would not.
    std::string canToggleBoundary(bool soft) const;

    // Why action would refuse items as the selection, in that order; empty when it would not.
    std::string getRefusal(SplineWarpActionEnum action, const std::list<RotoItemPtr>& items) const;

    // Runs action on the current selection, as its own function does.
    std::string runAction(SplineWarpActionEnum action, UndoCommandPtr* pushed = 0);

    /**
     * @brief What the correspondence tool does on a press at pos, tolerance away at most: picks the
     * nearest correspondence end, or else makes a correspondence on the nearest linked curve,
     * paired with the matching point of the other curve. Returns false, with the selection
     * cleared, when nothing linked is near.
     **/
    bool pressCorrespondence(const QPointF& pos, double time, double tolerance);

    /**
     * @brief Moves the end of the selected correspondence picked by the last press to the point of
     * its curve nearest pos, kept between the correspondences next to it.
     **/
    bool dragCorrespondence(const QPointF& pos, double time);

    // Ends the press: what it made or moved becomes one undo step.
    bool releaseCorrespondence(UndoCommandPtr* pushed = 0);

    bool deleteSelectedCorrespondence(UndoCommandPtr* pushed = 0);

    bool hasSelectedCorrespondence() const;

    // The selected correspondence: its link and index; false when there is none.
    bool getSelectedCorrespondence(SplineWarpLinkPtr* link, int* index) const;

    // Delete removes the selected correspondence; the base handles the rest.
    bool onKeyDown(Key key);

protected:
    virtual void onToolSelected(const KnobButtonPtr& actionButton, RotoToolEnum tool) OVERRIDE FINAL;
    virtual bool onPaintToolPenDown(double time, const QPointF& pos, double pressure, double timestamp) OVERRIDE FINAL;
    virtual bool onPaintStatePenMotion(const QPointF& pos, double pressure, double timestamp, double dx, double dy) OVERRIDE FINAL;
    virtual bool onPaintStatePenUp() OVERRIDE FINAL;
    virtual bool isCurveShown(const BezierPtr& curve) const OVERRIDE FINAL;
    virtual void getCurveOverlayColor(const RotoDrawableItemPtr& item, double color[4]) const OVERRIDE FINAL;
    virtual void drawOverlayExtras(double time, double screenPixelRatio, const std::pair<double, double>& pixelScale) OVERRIDE FINAL;

private:
    std::string refuse(const QString& message);

    void push(const UndoCommandPtr& command, UndoCommandPtr* pushed);

    std::list<RotoItemPtr> getSelectedRotoItems() const;

    // The selected Beziers in the order they were selected.
    std::list<BezierPtr> getSelectedBeziers() const;

    void setCorrespondenceParams(double sourceParam, double destinationParam, double time);

    // Drops the curves the show choice now hides from the selection.
    void deselectHiddenCurves();

    std::weak_ptr<SplineWarpLink> _selectedLink;
    int _selectedIndex;
    bool _dragOnSource;
    bool _gestureOpen;
    bool _gestureCreated;
    bool _gestureMoved;
    RotoCurveContextHostStatePtr _gestureBefore;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_SplineWarp_SplineWarpInteract_h
