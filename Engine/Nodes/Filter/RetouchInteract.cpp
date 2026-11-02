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

#include "RetouchInteract.h"

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/OverlaySupport.h"
#include "Engine/PluginActionShortcut.h"
#include "Engine/RotoCurveContext.h"
#include "Engine/RotoLayer.h"
#include "Engine/RotoPoint.h"
#include "Engine/RotoStrokeItem.h"
#include "Engine/UndoCommand.h"
#include "Global/GLIncludes.h"

NATRON_NAMESPACE_ENTER

namespace {
class RetouchAddBrushCommand
    : public UndoCommand {
public:
    RetouchAddBrushCommand(const RotoCurveInteractPtr& interact,
                           const RotoStrokeItemPtr& stroke)
        : _interact(interact)
        , _stroke(stroke)
        , _layer(stroke->getParentLayer())
        , _index(_layer ? _layer->getChildIndex(stroke) : 0)
    {
        setText("Brush");
    }

    virtual void undo() OVERRIDE FINAL
    {
        RotoCurveInteractPtr interact = _interact.lock();
        if (interact) {
            interact->removeCurve(_stroke);
            interact->evaluate(true);
        }
    }

    virtual void redo() OVERRIDE FINAL
    {
        RotoCurveInteractPtr interact = _interact.lock();
        // The pen stroke itself is the first redo.
        if (interact && _redone) {
            interact->getCurveContext()->addItem(_layer, _index, _stroke, RotoItem::eSelectionReasonOverlayInteract);
            interact->evaluate(true);
        }
        _redone = true;
    }

private:
    std::weak_ptr<RotoCurveInteract> _interact;
    RotoStrokeItemPtr _stroke;
    RotoLayerPtr _layer;
    int _index;
    bool _redone = false;
};
} // anon namespace

struct RetouchInteract::MakeSharedEnabler
    : public RetouchInteract {
    MakeSharedEnabler(EffectInstance* host)
        : RetouchInteract(host)
    {
    }
};

RetouchInteract::RetouchInteract(EffectInstance* host)
    : RotoCurveInteract(host)
{
}

RetouchInteract::~RetouchInteract()
{
}

RetouchInteractPtr
RetouchInteract::create(EffectInstance* host)
{
    return std::make_shared<RetouchInteract::MakeSharedEnabler>(host);
}

void
RetouchInteract::createBrushToolbarKnobs(const KnobPagePtr& toolbar)
{
    EffectInstance* host = getHost();
    KnobGroupPtr group = AppManager::createKnob<KnobGroup>(host, tr(kRetouchUIParamBrushToolButtonLabel));
    group->setName(kRetouchUIParamBrushToolButton);
    group->setAsToolButton(true);
    group->setEvaluateOnChange(false);
    group->setSecretByDefault(true);
    group->setInViewerContextCanHaveShortcut(true);
    group->setIsPersistent(false);
    toolbar->addKnob(group);
    brushToolGroup = group;

    KnobButtonPtr tool = AppManager::createKnob<KnobButton>(host, tr(kRetouchUIParamDrawBrushActionLabel));
    tool->setName(kRetouchUIParamDrawBrushAction);
    tool->setHintToolTip(tr(kRetouchUIParamDrawBrushActionHint));
    tool->setCheckable(true);
    tool->setDefaultValue(true);
    tool->setSecretByDefault(true);
    tool->setEvaluateOnChange(false);
    tool->setIconLabel(NATRON_ICONS_PATH "lucide/brush.svg");
    tool->setIsPersistent(false);
    group->addKnob(tool);
    drawBrushAction = tool;
}

void
RetouchInteract::getBrushToolbarShortcuts(std::list<PluginActionShortcut>* shortcuts)
{
    shortcuts->push_back(PluginActionShortcut(kRetouchUIParamBrushToolButton, kRetouchUIParamBrushToolButtonLabel, Key_N));
}

bool
RetouchInteract::getRoleForGroup(const KnobGroupPtr& group,
                                 RotoRoleEnum* role) const
{
    if (group && (group == brushToolGroup.lock())) {
        *role = eRotoRolePaintBrush;

        return true;
    }

    return RotoCurveInteract::getRoleForGroup(group, role);
}

bool
RetouchInteract::getToolForAction(const KnobButtonPtr& action,
                                  RotoToolEnum* tool) const
{
    if (action && (action == drawBrushAction.lock())) {
        *tool = eRotoToolSolidBrush;

        return true;
    }

    return RotoCurveInteract::getToolForAction(action, tool);
}

void
RetouchInteract::removeCurve(const RotoDrawableItemPtr& curve)
{
    if (curve == _stroke) {
        _stroke.reset();
    }
    RotoCurveInteract::removeCurve(curve);
}

void
RetouchInteract::drawPaintToolOverlay(double screenPixelRatio,
                                      const std::pair<double, double>& viewportSize)
{
    if (selectedTool != eRotoToolSolidBrush) {
        return;
    }
    OverlaySupport* viewport = getHost()->getCurrentViewportForOverlays();
    KnobDoublePtr size = brushSizeKnob.lock();
    if (!viewport || !size) {
        return;
    }
    const bool building = (state == eEventStateBuildingStroke) && _stroke;
    // While painting the footprint follows the pen, as RotoPaint's does, since the viewer may not
    // have moved its cursor yet.
    QPointF centre;
    if (building || (state == eEventStateDraggingCloneOffset)) {
        centre = lastMousePos;
    } else {
        double x, y;
        viewport->getCursorPosition(x, y);
        if (!viewport->getViewportRect().contains(x, y)) {
            return;
        }
        centre = QPointF(x, y);
    }
    GLdouble projection[16];
    glGetDoublev(GL_PROJECTION_MATRIX, projection);
    // One screen pixel, for the shadow.
    const double shadowX = 2. / (projection[0] * viewportSize.first);
    const double shadowY = 2. / (projection[5] * viewportSize.second);
    const double radius = size->getValue() / 2.;
    std::list<std::list<std::pair<Point, double>>> strokes;
    if (building) {
        _stroke->evaluateStroke(0, getCurveContext()->getTimelineCurrentTime(), &strokes);
    }
    GLProtectAttrib a(GL_ENABLE_BIT | GL_LINE_BIT | GL_COLOR_BUFFER_BIT | GL_CURRENT_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_LINE_SMOOTH);
    for (int l = 0; l < 2; ++l) {
        glMatrixMode(GL_PROJECTION);
        const int direction = (l == 0) ? 1 : -1;
        glTranslated(direction * shadowX, -direction * shadowY, 0);
        glMatrixMode(GL_MODELVIEW);
        drawEllipse(centre.x(), centre.y(), radius, radius, l, 1.f, 1.f, 1.f, 1.f);
        glLineWidth(1.5 * screenPixelRatio);
        glColor4d(.5 * l, .5 * l, .5 * l, 1.);
        for (std::list<std::list<std::pair<Point, double>>>::const_iterator it = strokes.begin(); it != strokes.end(); ++it) {
            glBegin(GL_LINE_STRIP);
            for (std::list<std::pair<Point, double>>::const_iterator p = it->begin(); p != it->end(); ++p) {
                glVertex2d(p->first.x, p->first.y);
            }
            glEnd();
        }
    }
}

bool
RetouchInteract::onPaintToolPenDown(double /*time*/,
                                    const QPointF& pos,
                                    double pressure,
                                    double timestamp)
{
    if (selectedTool != eRotoToolSolidBrush) {
        return false;
    }
    RotoCurveContextPtr context = getCurveContext();
    if (!context) {
        return false;
    }
    const std::string name = context->generateUniqueName(kRetouchBrushBaseName);
    _stroke = std::make_shared<RotoStrokeItem>(eRotoStrokeTypeSolid, context, name, RotoLayerPtr());
    startLifeTimeAtCurrentFrame(_stroke);
    // The host gives the stroke its retouch knobs here, as for a loaded one.
    context->onItemCreated(_stroke, false);
    RotoLayerPtr layer = context->getDeepestSelectedLayer();
    if (!layer) {
        layer = context->getOrCreateBaseLayer();
    }
    context->addItem(layer, 0, _stroke, RotoItem::eSelectionReasonOther);
    _stroke->appendPoint(true, RotoPoint(pos.x(), pos.y(), pressure, timestamp));
    lastMousePos = pos;
    state = eEventStateBuildingStroke;
    getHost()->setCurrentCursor(eCursorBlank);
    context->evaluateChange_noIncrement();

    return true;
}

bool
RetouchInteract::setPaintToolCursor(bool* redraw)
{
    if (selectedTool != eRotoToolSolidBrush) {
        return false;
    }
    getHost()->setCurrentCursor((state == eEventStateBuildingStroke) ? eCursorBlank : eCursorCross);
    // The overlay draws the brush outline at the cursor.
    *redraw = true;

    return true;
}

bool
RetouchInteract::onPaintStatePenMotion(const QPointF& pos,
                                       double pressure,
                                       double timestamp,
                                       double /*dx*/,
                                       double /*dy*/)
{
    if ((state != eEventStateBuildingStroke) || !_stroke) {
        return false;
    }
    if (_stroke->appendPoint(false, RotoPoint(pos.x(), pos.y(), pressure, timestamp))) {
        lastMousePos = pos;
        getCurveContext()->evaluateChange_noIncrement();
    }

    return true;
}

bool
RetouchInteract::onPaintStatePenUp()
{
    if ((state != eEventStateBuildingStroke) || !_stroke) {
        return false;
    }
    RotoStrokeItemPtr stroke = _stroke;
    _stroke.reset();
    stroke->setStrokeFinished();
    if (stroke->getParentLayer()) {
        pushUndoCommand(new RetouchAddBrushCommand(shared_from_this(), stroke));
    }
    getHost()->setCurrentCursor(eCursorCross);
    getCurveContext()->evaluateChange();

    return true;
}

NATRON_NAMESPACE_EXIT
