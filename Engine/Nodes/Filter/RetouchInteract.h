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

#ifndef Engine_Nodes_Filter_RetouchInteract_h
#define Engine_Nodes_Filter_RetouchInteract_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <memory>

#include "Engine/EngineFwd.h"
#include "Engine/RotoCurveInteract.h"

NATRON_NAMESPACE_ENTER

#define kRetouchUIParamBrushToolButton "BrushToolButton"
#define kRetouchUIParamBrushToolButtonLabel "Brush Tool"
#define kRetouchUIParamDrawBrushAction "DrawBrushTool"
#define kRetouchUIParamDrawBrushActionLabel "Draw Brush Tool"
#define kRetouchUIParamDrawBrushActionHint "Paint a freehand brush shape: darktable's retouch brush. Pen pressure changes the stroke as the shape's Brush Pressure says."

#define kRetouchBrushBaseName "Brush"

class RetouchInteract;
typedef std::shared_ptr<RetouchInteract> RetouchInteractPtr;

/**
 * @brief The Retouch viewer interact: the curve tools plus darktable's brush shape, a freehand
 * stroke painted with the pen that is a retouch shape like the Beziers.
 **/
class RetouchInteract
    : public RotoCurveInteract {
    struct MakeSharedEnabler;

protected:
    explicit RetouchInteract(EffectInstance* host);

public:
    static RetouchInteractPtr create(EffectInstance* host);

    virtual ~RetouchInteract();

    KnobGroupWPtr brushToolGroup;
    KnobButtonWPtr drawBrushAction;
    // The diameter of the next stroke, canonical, for the brush cursor.
    KnobDoubleWPtr brushSizeKnob;

    /// Appends the brush tool group to toolbar, after createToolbarKnobs().
    void createBrushToolbarKnobs(const KnobPagePtr& toolbar);

    static void getBrushToolbarShortcuts(std::list<PluginActionShortcut>* shortcuts);

    /// The stroke being painted, between the pen down and the pen up.
    RotoStrokeItemPtr getStrokeBeingPainted() const
    {
        return _stroke;
    }

    virtual bool getRoleForGroup(const KnobGroupPtr& group, RotoRoleEnum* role) const OVERRIDE;
    virtual bool getToolForAction(const KnobButtonPtr& action, RotoToolEnum* tool) const OVERRIDE;
    virtual void removeCurve(const RotoDrawableItemPtr& curve) OVERRIDE;

protected:
    virtual void drawPaintToolOverlay(double screenPixelRatio, const std::pair<double, double>& viewportSize) OVERRIDE;
    virtual bool onPaintToolPenDown(double time, const QPointF& pos, double pressure, double timestamp) OVERRIDE;
    virtual bool setPaintToolCursor(bool* redraw) OVERRIDE;
    virtual bool onPaintStatePenMotion(const QPointF& pos, double pressure, double timestamp, double dx, double dy) OVERRIDE;
    virtual bool onPaintStatePenUp() OVERRIDE;

private:
    RotoStrokeItemPtr _stroke;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_RetouchInteract_h
