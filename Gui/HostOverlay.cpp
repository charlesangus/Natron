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

#include "HostOverlay.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <list>
#include <stdexcept>

#include "Gui/GuiAppInstance.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/NodeGui.h"

#include "Engine/Curve.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/OverlayDrawListPen.h"
#include "Engine/OverlaySupport.h"
#include "Engine/Project.h"
#include "Engine/PythonUndoCommands.h"
#include "Engine/Settings.h"
#include "Engine/Transform.h"
#include "Engine/Utils.h"
#include "Engine/ViewIdx.h"

#include "Global/KeySymbols.h"

#include "Global/GLIncludes.h" //!<must be included before QGlWidget because of gl.h and glew.h
#include <QDebug>
#include <QPointF>
#include <QThread>
#include <QFont>
#include <QColor>
#include <QApplication>

#include <QOpenGLWidget>

#ifndef M_PI
#define M_PI 3.14159265358979323846264338327950288419717
#endif

NATRON_NAMESPACE_ENTER

// IMPORTANT NOTE:
//
// Do not use setValues(x,y) in the internal interact code, because it blocks the instanceChanged call
// for the first dimension, which is then reset when the interact is closed (eg shape deselected in Roto).
// The reason seems to be that value changes are blocked when setting the first dimension in setValues(),
// but some of the blocked actions depend on the dimension.
//
// see:
// - call to it->knob->refreshListenersAfterValueChange() in KnobHolder::endChanges()
// - calls to dimensionChanged.insert() in Knob<T>::dequeueValuesSet()
// Functions and variables related to blocking value changes have various names in the code:
// - blockValueChanges valueChangeBlocked valueChangesBlocked valueChangedBlocked isValueChangedBlocked
//
// TODO: This is a bug, there is no reason to not use setValues().
// OpenFX plugins are not affected, because instanceChanged does not pass the dimension information.
namespace {

// Writes a dimension as an edit in the settings panel does: an animated dimension takes a key at
// the current frame, and the knob's prior state joins the undo step of the gesture (see
// EffectInstance::onOverlayPenDown_public). A dimension that keeps its value is not written, so it
// gains no key.
void
setOverlayKnobValue(const KnobDoublePtr& knob,
                    const double value,
                    const int dimension)
{
    if (!knob) {
        return;
    }
    if (!knob->isValueChangeAtCurrentTime(value, dimension)) {
        return;
    }
    recordKnobEdit(knob);
    KeyFrame k;
    knob->setValue(value, ViewSpec::all(), dimension, eValueChangedReasonNatronGuiEdited, &k);
}

void
setOverlayKnobValues(const KnobDoublePtr& knob,
                     const double x,
                     const double y)
{
    setOverlayKnobValue(knob, x, 0);
    setOverlayKnobValue(knob, y, 1);
}

inline void
setOverlayKnobValues(const KnobDoublePtr& knob,
                     const OfxPointD& point)
{
    setOverlayKnobValues(knob, point.x, point.y);
}

}  // namespace

DefaultInteractI::DefaultInteractI(HostOverlay* overlay)
    : _overlay(overlay)
{
}

ContentHasher
DefaultInteractI::beginOverlaySignature(const OfxPointD& pscale,
                                        const OfxRGBColourD& color) const
{
    ContentHasher h;
    h.appendU64((U64)(std::uintptr_t)_overlay->getLastCallingViewport());
    h.appendDouble(pscale.x);
    h.appendDouble(pscale.y);
    h.appendDouble(color.r);
    h.appendDouble(color.g);
    h.appendDouble(color.b);

    return h;
}

OverlayDrawList*
DefaultInteractI::beginOverlayList(const ContentHasher& signature)
{
    return _overlayCache.rebuildFor(signature.finalize());
}

void
DefaultInteractI::showOverlayList()
{
    OverlaySupport* viewport = _overlay->getLastCallingViewport();
    if (viewport && !_overlayCache.list().empty()) {
        viewport->drawOverlayDrawList(_overlayCache.list(), OverlayDrawState());
    }
}

DefaultInteractI::~DefaultInteractI()
{
}

void
DefaultInteractI::requestRedraw()
{
    _overlay->requestRedraw();
}

void
DefaultInteractI::getPixelScale(double& scaleX,
                                double& scaleY) const
{
    _overlay->n_getPixelScale(scaleX, scaleY);
}

#ifdef OFX_EXTENSIONS_NATRON
double
DefaultInteractI::getScreenPixelRatio() const
{
    return _overlay->n_getScreenPixelRatio();
}
#endif

void
DefaultInteractI::draw(double /*time*/,
                       const RenderScale& /*renderScale*/,
                       ViewIdx /*view*/,
                       const OfxPointD& /*pscale*/,
                       const QPointF& /*lastPenPos*/,
                       const OfxRGBColourD& /*color*/,
                       const QFontMetrics& /*fm*/)
{
}

bool
DefaultInteractI::penMotion(double /*time*/,
                            const RenderScale& /*renderScale*/,
                            ViewIdx /*view*/,
                            const OfxPointD& /*pscale*/,
                            const QPointF& /*lastPenPos*/,
                            const QPointF & /*penPos*/,
                            const QPoint & /*penPosViewport*/,
                            double /*pressure*/)
{
    return false;
}

bool
DefaultInteractI::penUp(double /*time*/,
                        const RenderScale& /*renderScale*/,
                        ViewIdx /*view*/,
                        const OfxPointD& /*pscale*/,
                        const QPointF& /*lastPenPos*/,
                        const QPointF & /*penPos*/,
                        const QPoint & /*penPosViewport*/,
                        double /*pressure*/)
{
    return false;
}

bool
DefaultInteractI::penDown(double /*time*/,
                          const RenderScale& /*renderScale*/,
                          ViewIdx /*view*/,
                          const OfxPointD& /*pscale*/,
                          const QPointF& /*lastPenPos*/,
                          const QPointF & /*penPos*/,
                          const QPoint & /*penPosViewport*/,
                          double /*pressure*/)
{
    return false;
}

bool
DefaultInteractI::penDoubleClicked(double /*time*/,
                                   const RenderScale& /*renderScale*/,
                                   ViewIdx /*view*/,
                                   const OfxPointD& /*pscale*/,
                                   const QPointF& /*lastPenPos*/,
                                   const QPointF & /*penPos*/,
                                   const QPoint & /*penPosViewport*/)
{
    return false;
}

bool
DefaultInteractI::keyDown(double /*time*/,
                          const RenderScale& /*renderScale*/,
                          ViewIdx /*view*/,
                          int /*key*/,
                          char*   /*keyString*/)
{
    return false;
}

bool
DefaultInteractI::keyUp(double /*time*/,
                        const RenderScale& /*renderScale*/,
                        ViewIdx /*view*/,
                        int /*key*/,
                        char*   /*keyString*/)
{
    return false;
}

bool
DefaultInteractI::keyRepeat(double /*time*/,
                            const RenderScale& /*renderScale*/,
                            ViewIdx /*view*/,
                            int /*key*/,
                            char*   /*keyString*/)
{
    return false;
}

bool
DefaultInteractI::gainFocus(double /*time*/,
                            const RenderScale& /*renderScale*/,
                            ViewIdx /*view*/)
{
    return false;
}

bool
DefaultInteractI::loseFocus(double /*time*/,
                            const RenderScale& /*renderScale*/,
                            ViewIdx /*view*/)
{
    return false;
}

NATRON_NAMESPACE_ANONYMOUS_ENTER

enum PositionInteractState
{
    ePositionInteractStateInactive,
    ePositionInteractStatePoised,
    ePositionInteractStatePicked
};


class PositionInteract
    : public DefaultInteractI
{
    KnobDoubleWPtr _param;
    KnobBoolWPtr _interactive;
    QPointF _dragPos;
    bool _interactiveDrag;
    PositionInteractState _state;

public:

    PositionInteract(const HostOverlayKnobsPosition* knobs,
                     HostOverlay* overlay)
        : DefaultInteractI(overlay)
        , _param()
        , _interactive()
        , _dragPos()
        , _interactiveDrag(false)
        , _state(ePositionInteractStateInactive)
    {
        _param = knobs->getKnob<KnobDouble>(HostOverlayKnobsPosition::eKnobsEnumerationPosition);
        _interactive = knobs->getKnob<KnobBool>(HostOverlayKnobsPosition::eKnobsEnumerationInteractive);
    }

    virtual ~PositionInteract()
    {
    }

    virtual bool isInteractForKnob(const KnobI* knob) const OVERRIDE FINAL
    {
        return _param.lock().get() == knob;
    }

    double pointSize() const
    {
        return TO_DPIX(5);
    }

    double pointTolerance() const
    {
        return TO_DPIX(12.);
    }

    bool getInteractive(double time) const
    {
        if ( _interactive.lock() ) {
            return _interactive.lock()->getValueAtTime(time);
        } else {
            return !appPTR->getCurrentSettings()->getRenderOnEditingFinishedOnly();
        }
    }

    virtual void draw(double time,
                      const RenderScale& renderScale,
                      ViewIdx view,
                      const OfxPointD& pscale,
                      const QPointF& lastPenPos,
                      const OfxRGBColourD& color,
                      const QFontMetrics& fm) OVERRIDE FINAL;
    virtual bool penMotion(double time,
                           const RenderScale& renderScale,
                           ViewIdx view,
                           const OfxPointD& pscale,
                           const QPointF& lastPenPos,
                           const QPointF &penPos,
                           const QPoint &penPosViewport,
                           double pressure) OVERRIDE FINAL;
    virtual bool penUp(double time,
                       const RenderScale& renderScale,
                       ViewIdx view,
                       const OfxPointD& pscale,
                       const QPointF& lastPenPos,
                       const QPointF &penPos,
                       const QPoint &penPosViewport,
                       double pressure) OVERRIDE FINAL;
    virtual bool penDown(double time,
                         const RenderScale& renderScale,
                         ViewIdx view,
                         const OfxPointD& pscale,
                         const QPointF& lastPenPos,
                         const QPointF &penPos,
                         const QPoint &penPosViewport,
                         double pressure) OVERRIDE FINAL;
    virtual bool loseFocus(double time,
                           const RenderScale& renderScale,
                           ViewIdx view) OVERRIDE FINAL;
};

typedef std::shared_ptr<PositionInteract> PositionInteractPtr;

class RectangleInteract
    : public DefaultInteractI {
public:
    enum MouseStateEnum {
        eMouseStateIdle = 0,
        eMouseStateDraggingTopLeft,
        eMouseStateDraggingTopRight,
        eMouseStateDraggingBtmLeft,
        eMouseStateDraggingBtmRight,
        eMouseStateDraggingCenter,
        eMouseStateDraggingTopMid,
        eMouseStateDraggingMidRight,
        eMouseStateDraggingBtmMid,
        eMouseStateDraggingMidLeft
    };

    enum DrawStateEnum {
        eDrawStateInactive = 0,
        eDrawStateHoveringTopLeft,
        eDrawStateHoveringTopRight,
        eDrawStateHoveringBtmLeft,
        eDrawStateHoveringBtmRight,
        eDrawStateHoveringCenter,
        eDrawStateHoveringTopMid,
        eDrawStateHoveringMidRight,
        eDrawStateHoveringBtmMid,
        eDrawStateHoveringMidLeft
    };

private:
    KnobDoubleWPtr _btmLeft;
    KnobDoubleWPtr _size;
    KnobBoolWPtr _enable;
    OfxPointD _lastMousePos;
    MouseStateEnum _mouseState;
    DrawStateEnum _drawState;
    int _modifierStateCtrl;
    int _modifierStateShift;
    OfxPointD _btmLeftDragPos;
    OfxPointD _sizeDrag;
    bool _interactiveDrag;

public:
    RectangleInteract(const HostOverlayKnobsRectangle* knobs,
                      HostOverlay* overlay)
        : DefaultInteractI(overlay)
        , _btmLeft()
        , _size()
        , _enable()
        , _lastMousePos()
        , _mouseState(eMouseStateIdle)
        , _drawState(eDrawStateInactive)
        , _modifierStateCtrl(0)
        , _modifierStateShift(0)
        , _btmLeftDragPos()
        , _sizeDrag()
        , _interactiveDrag(false)
    {
        _btmLeft = knobs->getKnob<KnobDouble>(HostOverlayKnobsRectangle::eKnobsEnumerationBottomLeft);
        _size = knobs->getKnob<KnobDouble>(HostOverlayKnobsRectangle::eKnobsEnumerationSize);
        _enable = knobs->getKnob<KnobBool>(HostOverlayKnobsRectangle::eKnobsEnumerationEnable);
        _lastMousePos.x = _lastMousePos.y = 0.;
        _btmLeftDragPos.x = _btmLeftDragPos.y = 0.;
        _sizeDrag.x = _sizeDrag.y = 0.;
    }

    virtual ~RectangleInteract()
    {
    }

    virtual bool isInteractForKnob(const KnobI* knob) const OVERRIDE FINAL
    {
        return _btmLeft.lock().get() == knob || _size.lock().get() == knob;
    }

    static double pointSize()
    {
        return 5.;
    }

    static double pointTolerance()
    {
        return 6.;
    }

    static double crossSize()
    {
        return 7.;
    }

    virtual void draw(double time,
                      const RenderScale& renderScale,
                      ViewIdx view,
                      const OfxPointD& pscale,
                      const QPointF& lastPenPos,
                      const OfxRGBColourD& color,
                      const QFontMetrics& fm) OVERRIDE FINAL;
    virtual bool penMotion(double time,
                           const RenderScale& renderScale,
                           ViewIdx view,
                           const OfxPointD& pscale,
                           const QPointF& lastPenPos,
                           const QPointF& penPos,
                           const QPoint& penPosViewport,
                           double pressure) OVERRIDE FINAL;
    virtual bool penUp(double time,
                       const RenderScale& renderScale,
                       ViewIdx view,
                       const OfxPointD& pscale,
                       const QPointF& lastPenPos,
                       const QPointF& penPos,
                       const QPoint& penPosViewport,
                       double pressure) OVERRIDE FINAL;
    virtual bool penDown(double time,
                         const RenderScale& renderScale,
                         ViewIdx view,
                         const OfxPointD& pscale,
                         const QPointF& lastPenPos,
                         const QPointF& penPos,
                         const QPoint& penPosViewport,
                         double pressure) OVERRIDE FINAL;
    virtual bool keyDown(double time,
                         const RenderScale& renderScale,
                         ViewIdx view,
                         int key,
                         char* keyString) OVERRIDE FINAL;
    virtual bool keyUp(double time,
                       const RenderScale& renderScale,
                       ViewIdx view,
                       int key,
                       char* keyString) OVERRIDE FINAL;
    virtual bool loseFocus(double time,
                           const RenderScale& renderScale,
                           ViewIdx view) OVERRIDE FINAL;

private:
    bool isActive(double time) const;

    void getRectangle(double time,
                      double* x1,
                      double* y1,
                      double* w,
                      double* h) const;

    void setValue(double time,
                  OfxPointD btmLeft,
                  OfxPointD size,
                  const OfxPointD& pscale);
};

typedef std::shared_ptr<RectangleInteract> RectangleInteractPtr;

class TransformInteract
    : public DefaultInteractI
{
    enum DrawStateEnum
    {
        eInActive = 0, //< nothing happening
        eCircleHovered, //< the scale circle is hovered
        eLeftPointHovered, //< the left point of the circle is hovered
        eRightPointHovered, //< the right point of the circle is hovered
        eBottomPointHovered, //< the bottom point of the circle is hovered
        eTopPointHovered, //< the top point of the circle is hovered
        eCenterPointHovered, //< the center point of the circle is hovered
        eRotationBarHovered, //< the rotation bar is hovered
        eSkewXBarHoverered, //< the skew bar is hovered
        eSkewYBarHoverered //< the skew bar is hovered
    };

    enum MouseStateEnum
    {
        eReleased = 0,
        eDraggingCircle,
        eDraggingLeftPoint,
        eDraggingRightPoint,
        eDraggingTopPoint,
        eDraggingBottomPoint,
        eDraggingTranslation,
        eDraggingCenter,
        eDraggingRotationBar,
        eDraggingSkewXBar,
        eDraggingSkewYBar
    };

    enum OrientationEnum
    {
        eOrientationAllDirections = 0,
        eOrientationNotSet,
        eOrientationHorizontal,
        eOrientationVertical
    };

    KnobDoubleWPtr _translate;
    KnobDoubleWPtr _scale;
    KnobBoolWPtr _scaleUniform;
    KnobDoubleWPtr _rotate;
    KnobDoubleWPtr _center;
    KnobDoubleWPtr _skewX;
    KnobDoubleWPtr _skewY;
    KnobChoiceWPtr _skewOrder;
    KnobBoolWPtr _invert;
    DrawStateEnum _drawState;
    MouseStateEnum _mouseState;
    int _modifierStateCtrl;
    int _modifierStateShift;
    OrientationEnum _orientation;
    Point _centerDrag;
    Point _translateDrag;
    Point _scaleParamDrag;
    bool _scaleUniformDrag;
    double _rotateDrag;
    double _skewXDrag;
    double _skewYDrag;
    int _skewOrderDrag;
    bool _invertedDrag;
    bool _interactiveDrag;

public:
    TransformInteract(const HostOverlayKnobsTransform* knobs,
                      HostOverlay* overlay)
        : DefaultInteractI(overlay)
        , _translate()
        , _scale()
        , _scaleUniform()
        , _rotate()
        , _center()
        , _skewX()
        , _skewY()
        , _skewOrder()
        , _invert()
        , _drawState(eInActive)
        , _mouseState(eReleased)
        , _modifierStateCtrl(0)
        , _modifierStateShift(0)
        , _orientation(eOrientationAllDirections)
        , _centerDrag()
        , _translateDrag()
        , _scaleParamDrag()
        , _scaleUniformDrag(false)
        , _rotateDrag(0)
        , _skewXDrag(0)
        , _skewYDrag(0)
        , _skewOrderDrag(0)
        , _invertedDrag(false)
        , _interactiveDrag(false)
    {
        _translate = knobs->getKnob<KnobDouble>(HostOverlayKnobsTransform::eKnobsEnumerationTranslate);
        _scale = knobs->getKnob<KnobDouble>(HostOverlayKnobsTransform::eKnobsEnumerationScale);
        _scaleUniform = knobs->getKnob<KnobBool>(HostOverlayKnobsTransform::eKnobsEnumerationUniform);
        _rotate = knobs->getKnob<KnobDouble>(HostOverlayKnobsTransform::eKnobsEnumerationRotate);
        _center = knobs->getKnob<KnobDouble>(HostOverlayKnobsTransform::eKnobsEnumerationCenter);
        _skewX = knobs->getKnob<KnobDouble>(HostOverlayKnobsTransform::eKnobsEnumerationSkewx);
        _skewY = knobs->getKnob<KnobDouble>(HostOverlayKnobsTransform::eKnobsEnumerationSkewy);
        _skewOrder = knobs->getKnob<KnobChoice>(HostOverlayKnobsTransform::eKnobsEnumerationSkewOrder);
        _invert = knobs->getKnob<KnobBool>(HostOverlayKnobsTransform::eKnobsEnumerationInvert);
    }

    virtual ~TransformInteract()
    {
    }

    virtual bool isInteractForKnob(const KnobI* knob) const OVERRIDE FINAL
    {
        return knob == _translate.lock().get() || knob == _scale.lock().get() || knob == _scaleUniform.lock().get() || knob == _rotate.lock().get() || knob == _center.lock().get() || knob == _skewX.lock().get() || knob == _skewY.lock().get() || knob == _skewOrder.lock().get() || knob == _invert.lock().get();
    }

    static double circleRadiusBase() { return 30.; }

    static double circleRadiusMin() { return 15.; }

    static double circleRadiusMax() { return 300.; }

    static double pointSize() { return 7.; }

    static double ellipseNPoints() { return 50.; }


    void getTranslate(double time,
                      double* tx,
                      double* ty) const
    {
        KnobDoublePtr knob = _translate.lock();

        assert(knob);
        *tx = knob->getValueAtTime(time, 0);
        *ty = knob->getValueAtTime(time, 1);
    }

    void getCenter(double time,
                   double* cx,
                   double* cy) const
    {
        KnobDoublePtr knob = _center.lock();

        assert(knob);
        *cx = knob->getValueAtTime(time, 0);
        *cy = knob->getValueAtTime(time, 1);
    }

    void getScale(double time,
                  double* sx,
                  double* sy) const
    {
        KnobDoublePtr knob = _scale.lock();

        assert(knob);
        *sx = knob->getValueAtTime(time, 0);
        *sy = knob->getValueAtTime(time, 1);
    }

    void getRotate(double time,
                   double* rot) const
    {
        KnobDoublePtr knob = _rotate.lock();

        assert(knob);
        *rot = knob->getValueAtTime(time, 0);
    }

    void getSkewX(double time,
                  double* x) const
    {
        KnobDoublePtr knob = _skewX.lock();

        assert(knob);
        *x = knob->getValueAtTime(time, 0);
    }

    void getSkewY(double time,
                  double* y) const
    {
        KnobDoublePtr knob = _skewY.lock();

        assert(knob);
        *y = knob->getValueAtTime(time, 0);
    }

    void getSkewOrder(double time,
                      int* order) const
    {
        KnobChoicePtr knob = _skewOrder.lock();

        assert(knob);
        *order = knob->getValueAtTime(time, 0);
    }

    void getScaleUniform(double time,
                         bool* uniform) const
    {
        KnobBoolPtr knob = _scaleUniform.lock();

        *uniform = knob ? knob->getValueAtTime(time, 0) : false;
    }

    bool getInvert(double time) const
    {
        KnobBoolPtr knob = _invert.lock();

        if (knob) {
            return knob->getValueAtTime(time);
        } else {
            return false;
        }
    }

    bool getInteractive() const
    {
        return !appPTR->getCurrentSettings()->getRenderOnEditingFinishedOnly();
    }

    virtual void draw(double time,
                      const RenderScale& renderScale,
                      ViewIdx view,
                      const OfxPointD& pscale,
                      const QPointF& lastPenPos,
                      const OfxRGBColourD& color,
                      const QFontMetrics& fm) OVERRIDE FINAL;
    virtual bool penMotion(double time,
                           const RenderScale& renderScale,
                           ViewIdx view,
                           const OfxPointD& pscale,
                           const QPointF& lastPenPos,
                           const QPointF &penPos,
                           const QPoint &penPosViewport,
                           double pressure) OVERRIDE FINAL;
    virtual bool penUp(double time,
                       const RenderScale& renderScale,
                       ViewIdx view,
                       const OfxPointD& pscale,
                       const QPointF& lastPenPos,
                       const QPointF &penPos,
                       const QPoint &penPosViewport,
                       double pressure) OVERRIDE FINAL;
    virtual bool penDown(double time,
                         const RenderScale& renderScale,
                         ViewIdx view,
                         const OfxPointD& pscale,
                         const QPointF& lastPenPos,
                         const QPointF &penPos,
                         const QPoint &penPosViewport,
                         double pressure) OVERRIDE FINAL;
    virtual bool keyDown(double time,
                         const RenderScale& renderScale,
                         ViewIdx view,
                         int key,
                         char*   keyString) OVERRIDE FINAL;
    virtual bool keyUp(double time,
                       const RenderScale& renderScale,
                       ViewIdx view,
                       int key,
                       char*   keyString) OVERRIDE FINAL;
    virtual bool loseFocus(double time,
                           const RenderScale& renderScale,
                           ViewIdx view) OVERRIDE FINAL;
};

typedef std::shared_ptr<TransformInteract> TransformInteractPtr;

class CornerPinInteract
    : public DefaultInteractI
{
    std::vector<KnobDoubleWPtr> _from, _to;
    std::vector<KnobBoolWPtr> _enable;
    KnobBoolWPtr _invert;
    KnobChoiceWPtr _overlayPoints;
    int _dragging; // -1: idle, else dragging point number
    int _hovering; // -1: idle, else hovering point number
    Point _toDrag[4];
    Point _fromDrag[4];
    bool _enableDrag[4];
    bool _useFromDrag;
    bool _interactiveDrag;

public:
    CornerPinInteract(const HostOverlayKnobsCornerPin* knobs,
                      HostOverlay* overlay)
        : DefaultInteractI(overlay)
        , _invert()
        , _dragging(-1)
        , _hovering(-1)
        , _useFromDrag(false)
        , _interactiveDrag(false)
    {
        _from.resize(4);
        _to.resize(4);
        _enable.resize(4);

        _from[0] = knobs->getKnob<KnobDouble>(HostOverlayKnobsCornerPin::eKnobsEnumerationFrom1);
        _from[1] = knobs->getKnob<KnobDouble>(HostOverlayKnobsCornerPin::eKnobsEnumerationFrom2);
        _from[2] = knobs->getKnob<KnobDouble>(HostOverlayKnobsCornerPin::eKnobsEnumerationFrom3);
        _from[3] = knobs->getKnob<KnobDouble>(HostOverlayKnobsCornerPin::eKnobsEnumerationFrom4);

        _to[0] = knobs->getKnob<KnobDouble>(HostOverlayKnobsCornerPin::eKnobsEnumerationTo1);
        _to[1] = knobs->getKnob<KnobDouble>(HostOverlayKnobsCornerPin::eKnobsEnumerationTo2);
        _to[2] = knobs->getKnob<KnobDouble>(HostOverlayKnobsCornerPin::eKnobsEnumerationTo3);
        _to[3] = knobs->getKnob<KnobDouble>(HostOverlayKnobsCornerPin::eKnobsEnumerationTo4);


        _enable[0] = knobs->getKnob<KnobBool>(HostOverlayKnobsCornerPin::eKnobsEnumerationEnable1);
        _enable[1] = knobs->getKnob<KnobBool>(HostOverlayKnobsCornerPin::eKnobsEnumerationEnable2);
        _enable[2] = knobs->getKnob<KnobBool>(HostOverlayKnobsCornerPin::eKnobsEnumerationEnable3);
        _enable[3] = knobs->getKnob<KnobBool>(HostOverlayKnobsCornerPin::eKnobsEnumerationEnable4);

        _overlayPoints = knobs->getKnob<KnobChoice>(HostOverlayKnobsCornerPin::eKnobsEnumerationOverlayPoints);
        _invert = knobs->getKnob<KnobBool>(HostOverlayKnobsCornerPin::eKnobsEnumerationInvert);
        for (unsigned i = 0; i < 4; ++i) {
            _toDrag[i].x = _toDrag[i].y = _fromDrag[i].x = _fromDrag[i].y = 0.;
        }
    }

    virtual ~CornerPinInteract()
    {
    }

    virtual bool isInteractForKnob(const KnobI* knob) const OVERRIDE FINAL
    {
        for (int i = 0; i < 4; ++i) {
            if (_from[i].lock().get() == knob) {
                return true;
            } else if (_to[i].lock().get() == knob) {
                return true;
            } else if (_enable[i].lock().get() == knob) {
                return true;
            }
        }
        if (_invert.lock().get() == knob) {
            return true;
        }
        if (_overlayPoints.lock().get() == knob) {
            return true;
        }

        return false;
    }

    static double pointSize() { return TO_DPIX(5.); }

    static double pointTolerance() { return TO_DPIX(12.); }

    static bool isNearby(const QPointF & p,
                         double x,
                         double y,
                         double tolerance,
                         const OfxPointD & pscale)
    {
        return std::fabs(p.x() - x) <= tolerance * pscale.x &&  std::fabs(p.y() - y) <= tolerance * pscale.y;
    }

    void getFrom(double time,
                 int index,
                 double* tx,
                 double* ty) const
    {
        KnobDoublePtr knob = _from[index].lock();

        assert(knob);
        *tx = knob->getValueAtTime(time, 0);
        *ty = knob->getValueAtTime(time, 1);
    }

    void getTo(double time,
               int index,
               double* tx,
               double* ty) const
    {
        KnobDoublePtr knob = _to[index].lock();

        assert(knob);
        *tx = knob->getValueAtTime(time, 0);
        *ty = knob->getValueAtTime(time, 1);
    }

    bool getEnabled(double time,
                    int index) const
    {
        KnobBoolPtr knob = _enable[index].lock();

        assert(knob);

        return knob->getValueAtTime(time);
    }

    bool getInverted(double time) const
    {
        KnobBoolPtr knob = _invert.lock();

        assert(knob);

        return knob->getValueAtTime(time);
    }

    bool getUseFromPoints(double time) const
    {
        KnobChoicePtr knob = _overlayPoints.lock();

        assert(knob);

        return knob->getValueAtTime(time) == 1;
    }

    bool getInteractive() const
    {
        return !appPTR->getCurrentSettings()->getRenderOnEditingFinishedOnly();
    }

    virtual void draw(double time,
                      const RenderScale& renderScale,
                      ViewIdx view,
                      const OfxPointD& pscale,
                      const QPointF& lastPenPos,
                      const OfxRGBColourD& color,
                      const QFontMetrics& fm) OVERRIDE FINAL;
    virtual bool penMotion(double time,
                           const RenderScale& renderScale,
                           ViewIdx view,
                           const OfxPointD& pscale,
                           const QPointF& lastPenPos,
                           const QPointF &penPos,
                           const QPoint &penPosViewport,
                           double pressure) OVERRIDE FINAL;
    virtual bool penUp(double time,
                       const RenderScale& renderScale,
                       ViewIdx view,
                       const OfxPointD& pscale,
                       const QPointF& lastPenPos,
                       const QPointF &penPos,
                       const QPoint &penPosViewport,
                       double pressure) OVERRIDE FINAL;
    virtual bool penDown(double time,
                         const RenderScale& renderScale,
                         ViewIdx view,
                         const OfxPointD& pscale,
                         const QPointF& lastPenPos,
                         const QPointF &penPos,
                         const QPoint &penPosViewport,
                         double pressure) OVERRIDE FINAL;
    virtual bool loseFocus(double time,
                           const RenderScale& renderScale,
                           ViewIdx view) OVERRIDE FINAL;
};

typedef std::shared_ptr<CornerPinInteract> CornerPinInteractPtr;

// round to the closest int, 1/10 int, etc
// this make parameter editing easier
// pscale is args.pixelScale.x / args.renderScale.x;
// pscale10 is the power of 10 below pscale
inline double
fround(double val,
       double pscale)
{
    double pscale10 = ipow( 10., (int)std::floor( std::log10(pscale) ) );

    return pscale10 * std::floor(val / pscale10 + 0.5);
}

// The part of the line through p along v inside the rectangle, if any.
bool
clipLineToRect(const OfxPointD& p,
               const OfxPointD& v,
               const RectD& r,
               OfxPointD* a,
               OfxPointD* b)
{
    if ((v.x == 0.) && (v.y == 0.)) {
        return false;
    }
    const double pos[2] = { p.x, p.y };
    const double dir[2] = { v.x, v.y };
    const double lo[2] = { r.x1, r.y1 };
    const double hi[2] = { r.x2, r.y2 };
    double s0 = -std::numeric_limits<double>::infinity();
    double s1 = std::numeric_limits<double>::infinity();
    for (int k = 0; k < 2; ++k) {
        if (dir[k] == 0.) {
            if ((pos[k] < lo[k]) || (pos[k] > hi[k])) {
                return false;
            }
        } else {
            double ta = (lo[k] - pos[k]) / dir[k];
            double tb = (hi[k] - pos[k]) / dir[k];
            if (ta > tb) {
                std::swap(ta, tb);
            }
            s0 = (std::max)(s0, ta);
            s1 = (std::min)(s1, tb);
        }
    }
    if (s0 > s1) {
        return false;
    }
    a->x = p.x + s0 * v.x;
    a->y = p.y + s0 * v.y;
    b->x = p.x + s1 * v.x;
    b->y = p.y + s1 * v.y;

    return true;
}

class RampInteract
    : public DefaultInteractI {
    enum StateEnum {
        eStateIdle = 0,
        eStateDraggingPoint0,
        eStateDraggingPoint1
    };

    KnobDoubleWPtr _point[2];
    KnobBoolWPtr _interactive;
    OfxPointD _dragPos[2];
    OfxPointD _lastMousePos;
    StateEnum _state;
    bool _interactiveDrag;

public:
    RampInteract(const HostOverlayKnobsRamp* knobs,
                 HostOverlay* overlay)
        : DefaultInteractI(overlay)
        , _point()
        , _interactive()
        , _dragPos()
        , _lastMousePos()
        , _state(eStateIdle)
        , _interactiveDrag(false)
    {
        _point[0] = knobs->getKnob<KnobDouble>(HostOverlayKnobsRamp::eKnobsEnumerationPoint0);
        _point[1] = knobs->getKnob<KnobDouble>(HostOverlayKnobsRamp::eKnobsEnumerationPoint1);
        _interactive = knobs->getKnob<KnobBool>(HostOverlayKnobsRamp::eKnobsEnumerationInteractive);
        for (int i = 0; i < 2; ++i) {
            _dragPos[i].x = _dragPos[i].y = 0.;
        }
        _lastMousePos.x = _lastMousePos.y = 0.;
    }

    virtual ~RampInteract()
    {
    }

    virtual bool isInteractForKnob(const KnobI* knob) const OVERRIDE FINAL
    {
        return _point[0].lock().get() == knob || _point[1].lock().get() == knob;
    }

    static double pointSize()
    {
        return 5.;
    }

    static double pointTolerance()
    {
        return 6.;
    }

    virtual void draw(double time,
                      const RenderScale& /*renderScale*/,
                      ViewIdx view,
                      const OfxPointD& pscale,
                      const QPointF& /*lastPenPos*/,
                      const OfxRGBColourD& color,
                      const QFontMetrics& /*fm*/) OVERRIDE FINAL
    {
        if (!isActive()) {
            return;
        }
        OfxPointD p[2];
        getPoints(time, p);

        // Each line runs through its point orthogonally to p0p1, across the node's output.
        RectD rod;
        getOutputRect(time, view, &rod);
        OfxPointD ortho;
        ortho.x = -(p[1].y - p[0].y);
        ortho.y = p[1].x - p[0].x;
        OfxPointD lineStart[2];
        OfxPointD lineEnd[2];
        bool hasLine[2];
        for (int i = 0; i < 2; ++i) {
            hasLine[i] = clipLineToRect(p[i], ortho, rod, &lineStart[i], &lineEnd[i]);
        }

        ContentHasher signature = beginOverlaySignature(pscale, color);
        signature.appendI64(_state);
        for (int i = 0; i < 2; ++i) {
            signature.appendDouble(p[i].x);
            signature.appendDouble(p[i].y);
            signature.appendBool(hasLine[i]);
            if (hasLine[i]) {
                signature.appendDouble(lineStart[i].x);
                signature.appendDouble(lineStart[i].y);
                signature.appendDouble(lineEnd[i].x);
                signature.appendDouble(lineEnd[i].y);
            }
            signature.appendString(_point[i].lock()->getLabel());
        }
        OverlayDrawList* list = beginOverlayList(signature);
        if (!list) {
            showOverlayList();

            return;
        }
        OverlayDrawListPen pen(list);
        pen.setShadow(true);
        pen.setSmoothLines(true);
        pen.setSmoothPoints(false);
        pen.lineWidth(1.5);
        pen.pointSize(pointSize());
        for (int i = 0; i < 2; ++i) {
            const bool dragging = _state == (i == 0 ? eStateDraggingPoint0 : eStateDraggingPoint1);
            float r = (float)color.r;
            float g = (float)color.g;
            float b = (float)color.b;
            if (dragging) {
                r = 0.f;
                g = 1.f;
                b = 0.f;
            }
            pen.color(r, g, b);
            pen.point(p[i].x, p[i].y);

            if (hasLine[i]) {
                pen.setDash(2., 2.);
                pen.color(color.r, color.g, color.b);
                pen.line(lineStart[i].x, lineStart[i].y, lineEnd[i].x, lineEnd[i].y);
                pen.setDash(0., 0.);
            }

            pen.color(r, g, b);
            KnobDoublePtr knob = _point[i].lock();
            pen.text(p[i].x, p[i].y, knob->getLabel(), 0., pointSize());
        }
        showOverlayList();
    }

    virtual bool penMotion(double time,
                           const RenderScale& /*renderScale*/,
                           ViewIdx /*view*/,
                           const OfxPointD& pscale,
                           const QPointF& /*lastPenPos*/,
                           const QPointF& penPos,
                           const QPoint& /*penPosViewport*/,
                           double /*pressure*/) OVERRIDE FINAL
    {
        if (!isActive()) {
            return false;
        }
        bool valuesChanged = false;
        const int dragged = draggedPoint();
        if (dragged >= 0) {
            _dragPos[dragged].x += penPos.x() - _lastMousePos.x;
            _dragPos[dragged].y += penPos.y() - _lastMousePos.y;
            valuesChanged = true;
            if (_interactiveDrag) {
                setPoint(time, dragged, pscale);
            }
            requestRedraw();
        }
        _lastMousePos.x = penPos.x();
        _lastMousePos.y = penPos.y();

        return valuesChanged;
    }

    virtual bool penDown(double time,
                         const RenderScale& /*renderScale*/,
                         ViewIdx /*view*/,
                         const OfxPointD& pscale,
                         const QPointF& /*lastPenPos*/,
                         const QPointF& penPos,
                         const QPoint& /*penPosViewport*/,
                         double /*pressure*/) OVERRIDE FINAL
    {
        if (!isActive()) {
            return false;
        }
        OfxPointD p[2];
        getPoints(time, p);
        if (_state == eStateIdle) {
            KnobBoolPtr interactive = _interactive.lock();
            if (interactive) {
                _interactiveDrag = interactive->getValueAtTime(time);
            } else {
                _interactiveDrag = !appPTR->getCurrentSettings()->getRenderOnEditingFinishedOnly();
            }
        }
        OfxPointD pen;
        pen.x = penPos.x();
        pen.y = penPos.y();
        if (isNearby(pen, p[0], pscale)) {
            _state = eStateDraggingPoint0;
        } else if (isNearby(pen, p[1], pscale)) {
            _state = eStateDraggingPoint1;
        } else {
            _state = eStateIdle;
        }
        _dragPos[0] = p[0];
        _dragPos[1] = p[1];
        _lastMousePos = pen;
        const bool didSomething = _state != eStateIdle;
        if (didSomething) {
            requestRedraw();
        }

        return didSomething;
    }

    virtual bool penUp(double time,
                       const RenderScale& /*renderScale*/,
                       ViewIdx /*view*/,
                       const OfxPointD& pscale,
                       const QPointF& /*lastPenPos*/,
                       const QPointF& /*penPos*/,
                       const QPoint& /*penPosViewport*/,
                       double /*pressure*/) OVERRIDE FINAL
    {
        if (!isActive()) {
            return false;
        }
        bool didSomething = false;
        const int dragged = draggedPoint();
        if (!_interactiveDrag && (dragged >= 0)) {
            setPoint(time, dragged, pscale);
            didSomething = true;
        } else if (dragged >= 0) {
            requestRedraw();
        }
        _state = eStateIdle;

        return didSomething;
    }

    virtual bool loseFocus(double /*time*/,
                           const RenderScale& /*renderScale*/,
                           ViewIdx /*view*/) OVERRIDE FINAL
    {
        _interactiveDrag = false;
        _state = eStateIdle;

        return false;
    }

private:
    bool isActive() const
    {
        for (int i = 0; i < 2; ++i) {
            KnobDoublePtr knob = _point[i].lock();
            if (!knob || !knob->shouldDrawOverlayInteract()) {
                return false;
            }
        }

        return true;
    }

    int draggedPoint() const
    {
        return _state == eStateDraggingPoint0 ? 0 : (_state == eStateDraggingPoint1 ? 1 : -1);
    }

    static bool isNearby(const OfxPointD& pen,
                         const OfxPointD& p,
                         const OfxPointD& pscale)
    {
        return std::fabs(pen.x - p.x) <= pointTolerance() * pscale.x && std::fabs(pen.y - p.y) <= pointTolerance() * pscale.y;
    }

    void getPoints(double time,
                   OfxPointD p[2]) const
    {
        if (_state != eStateIdle) {
            p[0] = _dragPos[0];
            p[1] = _dragPos[1];

            return;
        }
        for (int i = 0; i < 2; ++i) {
            KnobDoublePtr knob = _point[i].lock();
            double v[2];
            for (int d = 0; d < 2; ++d) {
                v[d] = knob->getValueAtTime(time, d);
                if (knob->getValueIsNormalized(d) != eValueIsNormalizedNone) {
                    v[d] = knob->denormalize(d, time, v[d]);
                }
            }
            p[i].x = v[0];
            p[i].y = v[1];
        }
    }

    // The node's output at the time, or the project format when it has none.
    void getOutputRect(double time,
                       ViewIdx view,
                       RectD* rect) const
    {
        EffectInstancePtr effect = _overlay->getNode()->getNode()->getEffectInstance();
        bool isProjectFormat = false;
        if ((effect->getRegionOfDefinition_public(effect->getRenderHash(), time, RenderScale::identity, view, rect, &isProjectFormat) != eStatusFailed) && !rect->isNull()) {
            return;
        }
        Format format;
        effect->getApp()->getProject()->getProjectDefaultFormat(&format);
        *rect = format.toCanonicalFormat();
    }

    void setPoint(double time,
                  int i,
                  const OfxPointD& pscale)
    {
        KnobDoublePtr knob = _point[i].lock();
        double v[2] = { fround(_dragPos[i].x, pscale.x), fround(_dragPos[i].y, pscale.y) };
        for (int d = 0; d < 2; ++d) {
            if (knob->getValueIsNormalized(d) != eValueIsNormalizedNone) {
                v[d] = knob->normalize(d, time, v[d]);
            }
        }
        setOverlayKnobValues(knob, v[0], v[1]);
    }
};

typedef std::shared_ptr<RampInteract> RampInteractPtr;

typedef std::list<DefaultInteractIPtr> DefaultInteractIPtrList;
typedef std::list<TransformInteract> TransformInteracts;

NATRON_NAMESPACE_ANONYMOUS_EXIT


struct HostOverlayPrivate
{
    HostOverlay* _publicInterface;
    DefaultInteractIPtrList interacts;
    NodeGuiWPtr node;
    QPointF lastPenPos;
    bool interactiveDrag;

    HostOverlayPrivate(HostOverlay* publicInterface,
                       const NodeGuiPtr& node)
        : _publicInterface(publicInterface)
        , interacts()
        , node(node)
        , lastPenPos()
        , interactiveDrag(false)
    {
    }

    void requestRedraw()
    {
        node.lock()->getNode()->getApp()->queueRedrawForAllViewers();
    }
};

void
HostOverlay::requestRedraw()
{
    _imp->requestRedraw();
}

HostOverlay::HostOverlay(const NodeGuiPtr& node)
    : _imp( new HostOverlayPrivate(this, node) )
{
}

HostOverlay::~HostOverlay()
{
}

NodeGuiPtr
HostOverlay::getNode() const
{
    return _imp->node.lock();
}

bool
HostOverlay::addInteract(const HostOverlayKnobsPtr& knobs)
{
    assert( QThread::currentThread() == qApp->thread() );

    KnobIPtr firstKnob = knobs->getFirstKnob();
    if (!firstKnob) {
        return false;
    }
    for (DefaultInteractIPtrList::iterator it = _imp->interacts.begin(); it != _imp->interacts.end(); ++it) {
        if ( (*it)->isInteractForKnob( firstKnob.get() ) ) {
            return false;
        }
    }

    if ( !knobs->checkHostOverlayValid() ) {
        return false;
    }

    HostOverlayKnobsPosition* isPosition = dynamic_cast<HostOverlayKnobsPosition*>( knobs.get() );
    HostOverlayKnobsTransform* isTransform = dynamic_cast<HostOverlayKnobsTransform*>( knobs.get() );
    HostOverlayKnobsCornerPin* isCornerPin = dynamic_cast<HostOverlayKnobsCornerPin*>( knobs.get() );
    HostOverlayKnobsRectangle* isRectangle = dynamic_cast<HostOverlayKnobsRectangle*>(knobs.get());
    HostOverlayKnobsRamp* isRamp = dynamic_cast<HostOverlayKnobsRamp*>(knobs.get());
    DefaultInteractIPtr overlay;
    if (isPosition) {
        PositionInteractPtr p( new PositionInteract(isPosition, this) );
        overlay = p;
    } else if (isTransform) {
        TransformInteractPtr p( new TransformInteract(isTransform, this) );
        overlay = p;
    } else if (isCornerPin) {
        CornerPinInteractPtr p( new CornerPinInteract(isCornerPin, this) );
        overlay = p;
    } else if (isRectangle) {
        RectangleInteractPtr p(new RectangleInteract(isRectangle, this));
        overlay = p;
    } else if (isRamp) {
        RampInteractPtr p(new RampInteract(isRamp, this));
        overlay = p;
    }

    _imp->interacts.push_back(overlay);

    return true;
}

void
PositionInteract::draw(double time,
                       const RenderScale& /*renderScale*/,
                       ViewIdx /*view*/,
                       const OfxPointD& pscale,
                       const QPointF& lastPenPos,
                       const OfxRGBColourD& color,
                       const QFontMetrics& fm)
{
    KnobDoublePtr knob = _param.lock();

    // do not show interact if knob is secret or not enabled
    // see https://github.com/MrKepzie/Natron/issues/932
    if ( !knob || !knob->shouldDrawOverlayInteract()) {
        return;
    }

    float pR = 1.f;
    float pG = 1.f;
    float pB = 1.f;
    switch (_state) {
    case ePositionInteractStateInactive:
        pR = (float)color.r; pG = (float)color.g; pB = (float)color.b; break;
    case ePositionInteractStatePoised:
        pR = 0.f; pG = 1.0f; pB = 0.0f; break;
    case ePositionInteractStatePicked:
        pR = 0.f; pG = 1.0f; pB = 0.0f; break;
    }

    QPointF pos;
    if (_state == ePositionInteractStatePicked) {
        pos = lastPenPos;
    } else {
        double p[2];
        for (int i = 0; i < 2; ++i) {
            p[i] = knob->getValueAtTime(time, i);
            if (knob->getValueIsNormalized(i) != eValueIsNormalizedNone) {
                p[i] = knob->denormalize(i, time, p[i]);
            }
        }
        pos.setX(p[0]);
        pos.setY(p[1]);
    }
    ContentHasher signature = beginOverlaySignature(pscale, color);
    signature.appendI64(_state);
    signature.appendDouble(pos.x());
    signature.appendDouble(pos.y());
    signature.appendString(knob->getOriginalName());
    signature.appendI64(fm.height());
    signature.appendDouble(pointSize());
    OverlayDrawList* list = beginOverlayList(signature);
    if (!list) {
        showOverlayList();

        return;
    }
    OverlayDrawListPen pen(list);
    pen.setShadow(true);
    pen.color(pR, pG, pB);
    pen.pointSize(pointSize());
    pen.point(pos.x(), pos.y());
    pen.text(pos.x(), pos.y(), knob->getOriginalName(), 0., -(fm.height() + pointSize()));
    showOverlayList();
} // PositionInteract::draw

bool
RectangleInteract::isActive(double time) const
{
    KnobDoublePtr btmLeft = _btmLeft.lock();
    KnobDoublePtr size = _size.lock();

    if (!btmLeft || !size || !btmLeft->shouldDrawOverlayInteract() || !size->shouldDrawOverlayInteract()) {
        return false;
    }
    KnobBoolPtr enable = _enable.lock();
    if (enable && !enable->getValueAtTime(time)) {
        return false;
    }

    return true;
}

void
RectangleInteract::getRectangle(double time,
                                double* x1,
                                double* y1,
                                double* w,
                                double* h) const
{
    if (_mouseState != eMouseStateIdle) {
        *x1 = _btmLeftDragPos.x;
        *y1 = _btmLeftDragPos.y;
        *w = _sizeDrag.x;
        *h = _sizeDrag.y;

        return;
    }
    KnobDoublePtr btmLeft = _btmLeft.lock();
    KnobDoublePtr size = _size.lock();
    double p[2];
    double s[2];
    for (int i = 0; i < 2; ++i) {
        p[i] = btmLeft->getValueAtTime(time, i);
        if (btmLeft->getValueIsNormalized(i) != eValueIsNormalizedNone) {
            p[i] = btmLeft->denormalize(i, time, p[i]);
        }
        s[i] = size->getValueAtTime(time, i);
        if (size->getValueIsNormalized(i) != eValueIsNormalizedNone) {
            s[i] = size->denormalize(i, time, s[i]);
        }
    }
    *x1 = p[0];
    *y1 = p[1];
    *w = s[0];
    *h = s[1];
}

static bool
rectangleHandleIsNearby(const OfxPointD& p,
                        double x,
                        double y,
                        double tolerance,
                        const OfxPointD& pscale)
{
    return std::fabs(p.x - x) <= tolerance * pscale.x && std::fabs(p.y - y) <= tolerance * pscale.y;
}

static void
drawRectangleHandle(OverlayDrawListPen& pen,
                    const OfxRGBColourD& color,
                    double x,
                    double y,
                    RectangleInteract::DrawStateEnum id,
                    RectangleInteract::DrawStateEnum ds,
                    bool keepAR)
{
    if (ds == id) {
        if (keepAR) {
            pen.color(1., 0., 0.);
        } else {
            pen.color(0., 1., 0.);
        }
    } else {
        pen.color(color.r, color.g, color.b);
    }
    pen.vertex(x, y);
}

void
RectangleInteract::draw(double time,
                        const RenderScale& /*renderScale*/,
                        ViewIdx /*view*/,
                        const OfxPointD& pscale,
                        const QPointF& /*lastPenPos*/,
                        const OfxRGBColourD& color,
                        const QFontMetrics& /*fm*/)
{
    if (!isActive(time)) {
        return;
    }

    double x1, y1, w, h;
    getRectangle(time, &x1, &y1, &w, &h);
    double x2 = x1 + w;
    double y2 = y1 + h;
    double xc = x1 + w / 2;
    double yc = y1 + h / 2;
    const bool keepAR = _modifierStateShift > 0;
    const bool centered = _modifierStateCtrl > 0;

    ContentHasher signature = beginOverlaySignature(pscale, color);
    signature.appendDouble(x1);
    signature.appendDouble(y1);
    signature.appendDouble(w);
    signature.appendDouble(h);
    signature.appendBool(keepAR);
    signature.appendBool(centered);
    signature.appendI64(_drawState);
    signature.appendDouble(pointSize());
    signature.appendDouble(crossSize());
    OverlayDrawList* list = beginOverlayList(signature);
    if (!list) {
        showOverlayList();

        return;
    }
    OverlayDrawListPen pen(list);
    pen.setShadow(true);
    pen.setSmoothLines(true);
    pen.setSmoothPoints(false);
    pen.lineWidth(1.5);
    pen.color(color.r, color.g, color.b);

    pen.begin(OverlayDrawListPen::eModeLineLoop);
    pen.vertex(x1, y1);
    pen.vertex(x1, y2);
    pen.vertex(x2, y2);
    pen.vertex(x2, y1);
    pen.end();

    pen.pointSize(pointSize());
    pen.begin(OverlayDrawListPen::eModePoints);
    drawRectangleHandle(pen, color, x1, y1, eDrawStateHoveringBtmLeft, _drawState, keepAR);
    drawRectangleHandle(pen, color, x1, yc, eDrawStateHoveringMidLeft, _drawState, false);
    drawRectangleHandle(pen, color, x1, y2, eDrawStateHoveringTopLeft, _drawState, keepAR);
    drawRectangleHandle(pen, color, xc, y1, eDrawStateHoveringBtmMid, _drawState, false);
    drawRectangleHandle(pen, color, xc, yc, eDrawStateHoveringCenter, _drawState, false);
    drawRectangleHandle(pen, color, xc, y2, eDrawStateHoveringTopMid, _drawState, false);
    drawRectangleHandle(pen, color, x2, y1, eDrawStateHoveringBtmRight, _drawState, keepAR);
    drawRectangleHandle(pen, color, x2, yc, eDrawStateHoveringMidRight, _drawState, false);
    drawRectangleHandle(pen, color, x2, y2, eDrawStateHoveringTopRight, _drawState, keepAR);
    pen.end();

    if ((_drawState == eDrawStateHoveringCenter) || (centered && (_drawState != eDrawStateInactive))) {
        pen.color(0., 1., 0.);
    } else {
        pen.color(color.r, color.g, color.b);
    }
    pen.anchoredShape(xc, yc, OverlayDrawList::AnchoredShape::cross(crossSize()));
    showOverlayList();
} // RectangleInteract::draw

bool
RectangleInteract::penMotion(double time,
                             const RenderScale& /*renderScale*/,
                             ViewIdx /*view*/,
                             const OfxPointD& pscale,
                             const QPointF& /*lastPenPos*/,
                             const QPointF& penPos,
                             const QPoint& /*penPosViewport*/,
                             double /*pressure*/)
{
    if (!isActive(time)) {
        return false;
    }

    OfxPointD pen;
    pen.x = penPos.x();
    pen.y = penPos.y();
    double x1, y1, w, h;
    getRectangle(time, &x1, &y1, &w, &h);
    double x2 = x1 + w;
    double y2 = y1 + h;
    double xc = x1 + w / 2;
    double yc = y1 + h / 2;
    bool valuesChanged = false;
    OfxPointD delta;
    delta.x = pen.x - _lastMousePos.x;
    delta.y = pen.y - _lastMousePos.y;

    DrawStateEnum newState;
    // The centre is tested first so that a small rectangle can still be moved.
    if (rectangleHandleIsNearby(pen, xc, yc, pointTolerance(), pscale)) {
        newState = eDrawStateHoveringCenter;
    } else if (rectangleHandleIsNearby(pen, x1, y1, pointTolerance(), pscale)) {
        newState = eDrawStateHoveringBtmLeft;
    } else if (rectangleHandleIsNearby(pen, x2, y1, pointTolerance(), pscale)) {
        newState = eDrawStateHoveringBtmRight;
    } else if (rectangleHandleIsNearby(pen, x1, y2, pointTolerance(), pscale)) {
        newState = eDrawStateHoveringTopLeft;
    } else if (rectangleHandleIsNearby(pen, x2, y2, pointTolerance(), pscale)) {
        newState = eDrawStateHoveringTopRight;
    } else if (rectangleHandleIsNearby(pen, xc, y1, pointTolerance(), pscale)) {
        newState = eDrawStateHoveringBtmMid;
    } else if (rectangleHandleIsNearby(pen, xc, y2, pointTolerance(), pscale)) {
        newState = eDrawStateHoveringTopMid;
    } else if (rectangleHandleIsNearby(pen, x1, yc, pointTolerance(), pscale)) {
        newState = eDrawStateHoveringMidLeft;
    } else if (rectangleHandleIsNearby(pen, x2, yc, pointTolerance(), pscale)) {
        newState = eDrawStateHoveringMidRight;
    } else {
        newState = eDrawStateInactive;
    }
    bool redraw = _drawState != newState;
    _drawState = newState;

    const bool keepAR = _modifierStateShift > 0;
    const bool centered = _modifierStateCtrl > 0;
    if (keepAR && (_sizeDrag.x > 0.) && (_sizeDrag.y > 0.) && ((_mouseState == eMouseStateDraggingTopLeft) || (_mouseState == eMouseStateDraggingTopRight) || (_mouseState == eMouseStateDraggingBtmLeft) || (_mouseState == eMouseStateDraggingBtmRight))) {
        // Project the motion onto the diagonal being dragged, so the aspect ratio is kept.
        double r2 = _sizeDrag.x * _sizeDrag.x + _sizeDrag.y * _sizeDrag.y;
        if ((_mouseState == eMouseStateDraggingTopRight) || (_mouseState == eMouseStateDraggingBtmLeft)) {
            double dotprod = (delta.x * _sizeDrag.y + delta.y * _sizeDrag.x) / r2;
            delta.x = _sizeDrag.x * dotprod;
            delta.y = _sizeDrag.y * dotprod;
        } else {
            double dotprod = (delta.x * _sizeDrag.y - delta.y * _sizeDrag.x) / r2;
            delta.x = _sizeDrag.x * dotprod;
            delta.y = -_sizeDrag.y * dotprod;
        }
    }
    if (_mouseState == eMouseStateDraggingBtmLeft) {
        _drawState = eDrawStateHoveringBtmLeft;
        OfxPointD topRight;
        topRight.x = _btmLeftDragPos.x + _sizeDrag.x;
        topRight.y = _btmLeftDragPos.y + _sizeDrag.y;
        _btmLeftDragPos.x += delta.x;
        _btmLeftDragPos.y += delta.y;
        _sizeDrag.x = topRight.x - _btmLeftDragPos.x;
        _sizeDrag.y = topRight.y - _btmLeftDragPos.y;
        if (centered) {
            _sizeDrag.x -= delta.x;
            _sizeDrag.y -= delta.y;
        }
        valuesChanged = true;
    } else if (_mouseState == eMouseStateDraggingTopLeft) {
        _drawState = eDrawStateHoveringTopLeft;
        OfxPointD btmRight;
        btmRight.x = _btmLeftDragPos.x + _sizeDrag.x;
        btmRight.y = _btmLeftDragPos.y;
        _btmLeftDragPos.x += delta.x;
        _sizeDrag.y += delta.y;
        _sizeDrag.x = btmRight.x - _btmLeftDragPos.x;
        if (centered) {
            _sizeDrag.x -= delta.x;
            _sizeDrag.y += delta.y;
            _btmLeftDragPos.y -= delta.y;
        }
        valuesChanged = true;
    } else if (_mouseState == eMouseStateDraggingTopRight) {
        _drawState = eDrawStateHoveringTopRight;
        _sizeDrag.x += delta.x;
        _sizeDrag.y += delta.y;
        if (centered) {
            _sizeDrag.x += delta.x;
            _btmLeftDragPos.x -= delta.x;
            _sizeDrag.y += delta.y;
            _btmLeftDragPos.y -= delta.y;
        }
        valuesChanged = true;
    } else if (_mouseState == eMouseStateDraggingBtmRight) {
        _drawState = eDrawStateHoveringBtmRight;
        OfxPointD topLeft;
        topLeft.x = _btmLeftDragPos.x;
        topLeft.y = _btmLeftDragPos.y + _sizeDrag.y;
        _sizeDrag.x += delta.x;
        _btmLeftDragPos.y += delta.y;
        _sizeDrag.y = topLeft.y - _btmLeftDragPos.y;
        if (centered) {
            _sizeDrag.x += delta.x;
            _btmLeftDragPos.x -= delta.x;
            _sizeDrag.y -= delta.y;
        }
        valuesChanged = true;
    } else if (_mouseState == eMouseStateDraggingTopMid) {
        _drawState = eDrawStateHoveringTopMid;
        _sizeDrag.y += delta.y;
        if (centered) {
            _sizeDrag.y += delta.y;
            _btmLeftDragPos.y -= delta.y;
        }
        valuesChanged = true;
    } else if (_mouseState == eMouseStateDraggingMidRight) {
        _drawState = eDrawStateHoveringMidRight;
        _sizeDrag.x += delta.x;
        if (centered) {
            _sizeDrag.x += delta.x;
            _btmLeftDragPos.x -= delta.x;
        }
        valuesChanged = true;
    } else if (_mouseState == eMouseStateDraggingBtmMid) {
        _drawState = eDrawStateHoveringBtmMid;
        double top = _btmLeftDragPos.y + _sizeDrag.y;
        _btmLeftDragPos.y += delta.y;
        _sizeDrag.y = top - _btmLeftDragPos.y;
        if (centered) {
            _sizeDrag.y -= delta.y;
        }
        valuesChanged = true;
    } else if (_mouseState == eMouseStateDraggingMidLeft) {
        _drawState = eDrawStateHoveringMidLeft;
        double right = _btmLeftDragPos.x + _sizeDrag.x;
        _btmLeftDragPos.x += delta.x;
        _sizeDrag.x = right - _btmLeftDragPos.x;
        if (centered) {
            _sizeDrag.x -= delta.x;
        }
        valuesChanged = true;
    } else if (_mouseState == eMouseStateDraggingCenter) {
        _drawState = eDrawStateHoveringCenter;
        _btmLeftDragPos.x += delta.x;
        _btmLeftDragPos.y += delta.y;
        valuesChanged = true;
    }

    // Dragging an edge past the opposite one flips the rectangle, and the dragged handle with it.
    if (_sizeDrag.x < 0) {
        if (_mouseState == eMouseStateDraggingBtmLeft) {
            _mouseState = eMouseStateDraggingBtmRight;
        } else if (_mouseState == eMouseStateDraggingMidLeft) {
            _mouseState = eMouseStateDraggingMidRight;
        } else if (_mouseState == eMouseStateDraggingTopLeft) {
            _mouseState = eMouseStateDraggingTopRight;
        } else if (_mouseState == eMouseStateDraggingBtmRight) {
            _mouseState = eMouseStateDraggingBtmLeft;
        } else if (_mouseState == eMouseStateDraggingMidRight) {
            _mouseState = eMouseStateDraggingMidLeft;
        } else if (_mouseState == eMouseStateDraggingTopRight) {
            _mouseState = eMouseStateDraggingTopLeft;
        }

        _btmLeftDragPos.x += _sizeDrag.x;
        _sizeDrag.x = -_sizeDrag.x;
        valuesChanged = true;
    }
    if (_sizeDrag.y < 0) {
        if (_mouseState == eMouseStateDraggingTopLeft) {
            _mouseState = eMouseStateDraggingBtmLeft;
        } else if (_mouseState == eMouseStateDraggingTopMid) {
            _mouseState = eMouseStateDraggingBtmMid;
        } else if (_mouseState == eMouseStateDraggingTopRight) {
            _mouseState = eMouseStateDraggingBtmRight;
        } else if (_mouseState == eMouseStateDraggingBtmLeft) {
            _mouseState = eMouseStateDraggingTopLeft;
        } else if (_mouseState == eMouseStateDraggingBtmMid) {
            _mouseState = eMouseStateDraggingTopMid;
        } else if (_mouseState == eMouseStateDraggingBtmRight) {
            _mouseState = eMouseStateDraggingTopRight;
        }

        _btmLeftDragPos.y += _sizeDrag.y;
        _sizeDrag.y = -_sizeDrag.y;
        valuesChanged = true;
    }

    if (_sizeDrag.x < 1) {
        _sizeDrag.x = 1;
        valuesChanged = true;
    }
    if (_sizeDrag.y < 1) {
        _sizeDrag.y = 1;
        valuesChanged = true;
    }

    if ((_mouseState != eMouseStateIdle) && _interactiveDrag && valuesChanged) {
        setValue(time, _btmLeftDragPos, _sizeDrag, pscale);
    } else if (redraw || valuesChanged) {
        requestRedraw();
    }

    _lastMousePos = pen;

    return valuesChanged;
} // RectangleInteract::penMotion

bool
RectangleInteract::penDown(double time,
                           const RenderScale& /*renderScale*/,
                           ViewIdx /*view*/,
                           const OfxPointD& pscale,
                           const QPointF& /*lastPenPos*/,
                           const QPointF& penPos,
                           const QPoint& /*penPosViewport*/,
                           double /*pressure*/)
{
    if (!isActive(time)) {
        return false;
    }

    if (_mouseState == eMouseStateIdle) {
        _interactiveDrag = !appPTR->getCurrentSettings()->getRenderOnEditingFinishedOnly();
    }
    OfxPointD pen;
    pen.x = penPos.x();
    pen.y = penPos.y();
    double x1, y1, w, h;
    getRectangle(time, &x1, &y1, &w, &h);
    double x2 = x1 + w;
    double y2 = y1 + h;
    double xc = x1 + w / 2;
    double yc = y1 + h / 2;

    MouseStateEnum newState;
    if (rectangleHandleIsNearby(pen, xc, yc, pointTolerance(), pscale)) {
        newState = eMouseStateDraggingCenter;
    } else if (rectangleHandleIsNearby(pen, x1, y1, pointTolerance(), pscale)) {
        newState = eMouseStateDraggingBtmLeft;
    } else if (rectangleHandleIsNearby(pen, x2, y1, pointTolerance(), pscale)) {
        newState = eMouseStateDraggingBtmRight;
    } else if (rectangleHandleIsNearby(pen, x1, y2, pointTolerance(), pscale)) {
        newState = eMouseStateDraggingTopLeft;
    } else if (rectangleHandleIsNearby(pen, x2, y2, pointTolerance(), pscale)) {
        newState = eMouseStateDraggingTopRight;
    } else if (rectangleHandleIsNearby(pen, xc, y1, pointTolerance(), pscale)) {
        newState = eMouseStateDraggingBtmMid;
    } else if (rectangleHandleIsNearby(pen, xc, y2, pointTolerance(), pscale)) {
        newState = eMouseStateDraggingTopMid;
    } else if (rectangleHandleIsNearby(pen, x1, yc, pointTolerance(), pscale)) {
        newState = eMouseStateDraggingMidLeft;
    } else if (rectangleHandleIsNearby(pen, x2, yc, pointTolerance(), pscale)) {
        newState = eMouseStateDraggingMidRight;
    } else {
        newState = eMouseStateIdle;
    }
    bool didSomething = _mouseState != newState;
    _mouseState = newState;

    _btmLeftDragPos.x = x1;
    _btmLeftDragPos.y = y1;
    _sizeDrag.x = w;
    _sizeDrag.y = h;
    _lastMousePos = pen;
    if (didSomething) {
        requestRedraw();
    }

    return didSomething;
} // RectangleInteract::penDown

bool
RectangleInteract::penUp(double time,
                         const RenderScale& /*renderScale*/,
                         ViewIdx /*view*/,
                         const OfxPointD& pscale,
                         const QPointF& /*lastPenPos*/,
                         const QPointF& /*penPos*/,
                         const QPoint& /*penPosViewport*/,
                         double /*pressure*/)
{
    if (!isActive(time)) {
        return false;
    }

    bool didSomething = false;
    if (!_interactiveDrag && (_mouseState != eMouseStateIdle)) {
        setValue(time, _btmLeftDragPos, _sizeDrag, pscale);
        didSomething = true;
    } else if (_mouseState != eMouseStateIdle) {
        requestRedraw();
    }
    _mouseState = eMouseStateIdle;

    return didSomething;
}

bool
RectangleInteract::keyDown(double time,
                           const RenderScale& /*renderScale*/,
                           ViewIdx /*view*/,
                           int key,
                           char* /*keyString*/)
{
    if (!isActive(time)) {
        return false;
    }

    // Note that on the Mac:
    // cmd/apple/cloverleaf is kOfxKey_Control_L
    // ctrl is kOfxKey_Meta_L
    // alt/option is kOfxKey_Alt_L
    bool mustRedraw = false;

    // the two control keys may be pressed consecutively, be aware about this
    if ((key == kOfxKey_Control_L) || (key == kOfxKey_Control_R)) {
        mustRedraw = _modifierStateCtrl == 0;
        ++_modifierStateCtrl;
    }
    if ((key == kOfxKey_Shift_L) || (key == kOfxKey_Shift_R)) {
        mustRedraw = _modifierStateShift == 0;
        ++_modifierStateShift;
    }
    if (mustRedraw) {
        requestRedraw();
    }

    // modifiers are not "caught"
    return false;
}

bool
RectangleInteract::keyUp(double time,
                         const RenderScale& /*renderScale*/,
                         ViewIdx /*view*/,
                         int key,
                         char* /*keyString*/)
{
    if (!isActive(time)) {
        return false;
    }

    bool mustRedraw = false;

    if ((key == kOfxKey_Control_L) || (key == kOfxKey_Control_R)) {
        // we may have missed a keypress
        if (_modifierStateCtrl > 0) {
            --_modifierStateCtrl;
            mustRedraw = _modifierStateCtrl == 0;
        }
    }
    if ((key == kOfxKey_Shift_L) || (key == kOfxKey_Shift_R)) {
        if (_modifierStateShift > 0) {
            --_modifierStateShift;
            mustRedraw = _modifierStateShift == 0;
        }
    }
    if (mustRedraw) {
        requestRedraw();
    }

    // modifiers are not "caught"
    return false;
}

bool
RectangleInteract::loseFocus(double /*time*/,
                             const RenderScale& /*renderScale*/,
                             ViewIdx /*view*/)
{
    _modifierStateCtrl = 0;
    _modifierStateShift = 0;
    _interactiveDrag = false;

    return false;
}

void
RectangleInteract::setValue(double time,
                            OfxPointD btmLeft,
                            OfxPointD size,
                            const OfxPointD& pscale)
{
    // Only the coordinates the dragged handle moves are rounded, so the others keep their exact values.
    switch (_mouseState) {
    case eMouseStateIdle:
        break;
    case eMouseStateDraggingTopLeft:
        btmLeft.x = fround(btmLeft.x, pscale.x);
        size.x = fround(size.x, pscale.x);
        size.y = fround(size.y, pscale.y);
        break;
    case eMouseStateDraggingTopRight:
        size.x = fround(size.x, pscale.x);
        size.y = fround(size.y, pscale.y);
        break;
    case eMouseStateDraggingBtmLeft:
        btmLeft.x = fround(btmLeft.x, pscale.x);
        btmLeft.y = fround(btmLeft.y, pscale.y);
        size.x = fround(size.x, pscale.x);
        size.y = fround(size.y, pscale.y);
        break;
    case eMouseStateDraggingBtmRight:
        size.x = fround(size.x, pscale.x);
        size.y = fround(size.y, pscale.y);
        btmLeft.y = fround(btmLeft.y, pscale.y);
        break;
    case eMouseStateDraggingCenter:
        btmLeft.x = fround(btmLeft.x, pscale.x);
        btmLeft.y = fround(btmLeft.y, pscale.y);
        break;
    case eMouseStateDraggingTopMid:
        size.y = fround(size.y, pscale.y);
        break;
    case eMouseStateDraggingMidRight:
        size.x = fround(size.x, pscale.x);
        break;
    case eMouseStateDraggingBtmMid:
        btmLeft.y = fround(btmLeft.y, pscale.y);
        break;
    case eMouseStateDraggingMidLeft:
        btmLeft.x = fround(btmLeft.x, pscale.x);
        break;
    }

    KnobDoublePtr btmLeftKnob = _btmLeft.lock();
    KnobDoublePtr sizeKnob = _size.lock();
    double p[2] = { btmLeft.x, btmLeft.y };
    double s[2] = { size.x, size.y };
    for (int i = 0; i < 2; ++i) {
        if (btmLeftKnob->getValueIsNormalized(i) != eValueIsNormalizedNone) {
            p[i] = btmLeftKnob->normalize(i, time, p[i]);
        }
        if (sizeKnob->getValueIsNormalized(i) != eValueIsNormalizedNone) {
            s[i] = sizeKnob->normalize(i, time, s[i]);
        }
    }

    setOverlayKnobValues(btmLeftKnob, p[0], p[1]);
    setOverlayKnobValues(sizeKnob, s[0], s[1]);
} // RectangleInteract::setValue

static void
getTargetCenter(const OfxPointD &center,
                const OfxPointD &translate,
                OfxPointD *targetCenter)
{
    targetCenter->x = center.x + translate.x;
    targetCenter->y = center.y + translate.y;
}

static void
getTargetRadius(const OfxPointD& scale,
                const OfxPointD& pixelScale,
                OfxPointD* targetRadius)
{
    targetRadius->x = scale.x * TransformInteract::circleRadiusBase();
    targetRadius->y = scale.y * TransformInteract::circleRadiusBase();
    // don't draw too small. 15 pixels is the limit
    if ( ( std::fabs(targetRadius->x) < TransformInteract::circleRadiusMin() ) && ( std::fabs(targetRadius->y) < TransformInteract::circleRadiusMin() ) ) {
        targetRadius->x = targetRadius->x >= 0 ? TransformInteract::circleRadiusMin() : -TransformInteract::circleRadiusMin();
        targetRadius->y = targetRadius->y >= 0 ? TransformInteract::circleRadiusMin() : -TransformInteract::circleRadiusMin();
    } else if ( ( std::fabs(targetRadius->x) > TransformInteract::circleRadiusMax() ) && ( std::fabs(targetRadius->y) > TransformInteract::circleRadiusMax() ) ) {
        targetRadius->x = targetRadius->x >= 0 ? TransformInteract::circleRadiusMax() : -TransformInteract::circleRadiusMax();
        targetRadius->y = targetRadius->y >= 0 ? TransformInteract::circleRadiusMax() : -TransformInteract::circleRadiusMax();
    } else {
        if ( std::fabs(targetRadius->x) < TransformInteract::circleRadiusMin() ) {
            if ( (targetRadius->x == 0.) && (targetRadius->y != 0.) ) {
                targetRadius->y = targetRadius->y > 0 ? TransformInteract::circleRadiusMax() : -TransformInteract::circleRadiusMax();
            } else {
                targetRadius->y *= std::fabs(TransformInteract::circleRadiusMin() / targetRadius->x);
            }
            targetRadius->x = targetRadius->x >= 0 ? TransformInteract::circleRadiusMin() : -TransformInteract::circleRadiusMin();
        }
        if ( std::fabs(targetRadius->x) > TransformInteract::circleRadiusMax() ) {
            targetRadius->y *= std::fabs(TransformInteract::circleRadiusMax() / targetRadius->x);
            targetRadius->x = targetRadius->x > 0 ? TransformInteract::circleRadiusMax() : -TransformInteract::circleRadiusMax();
        }
        if ( std::fabs(targetRadius->y) < TransformInteract::circleRadiusMin() ) {
            if ( (targetRadius->y == 0.) && (targetRadius->x != 0.) ) {
                targetRadius->x = targetRadius->x > 0 ? TransformInteract::circleRadiusMax() : -TransformInteract::circleRadiusMax();
            } else {
                targetRadius->x *= std::fabs(TransformInteract::circleRadiusMin() / targetRadius->y);
            }
            targetRadius->y = targetRadius->y >= 0 ? TransformInteract::circleRadiusMin() : -TransformInteract::circleRadiusMin();
        }
        if ( std::fabs(targetRadius->y) > TransformInteract::circleRadiusMax() ) {
            targetRadius->x *= std::fabs(TransformInteract::circleRadiusMax() / targetRadius->x);
            targetRadius->y = targetRadius->y > 0 ? TransformInteract::circleRadiusMax() : -TransformInteract::circleRadiusMax();
        }
    }
    // the circle axes are not aligned with the images axes, so we cannot use the x and y scales separately
    double meanPixelScale = (pixelScale.x + pixelScale.y) / 2.;
    targetRadius->x *= meanPixelScale;
    targetRadius->y *= meanPixelScale;
}

static void
getTargetPoints(const OfxPointD& targetCenter,
                const OfxPointD& targetRadius,
                OfxPointD *left,
                OfxPointD *bottom,
                OfxPointD *top,
                OfxPointD *right)
{
    left->x = targetCenter.x - targetRadius.x;
    left->y = targetCenter.y;
    right->x = targetCenter.x + targetRadius.x;
    right->y = targetCenter.y;
    top->x = targetCenter.x;
    top->y = targetCenter.y + targetRadius.y;
    bottom->x = targetCenter.x;
    bottom->y = targetCenter.y - targetRadius.y;
}

static void
ofxsTransformGetScale(const OfxPointD &scaleParam,
                      bool scaleUniform,
                      OfxPointD* scale)
{
    const double SCALE_MIN = 0.0001;

    scale->x = scaleParam.x;
    if (std::fabs(scale->x) < SCALE_MIN) {
        scale->x = (scale->x >= 0) ? SCALE_MIN : -SCALE_MIN;
    }
    if (scaleUniform) {
        scale->y = scaleParam.x;
    } else {
        scale->y = scaleParam.y;
    }
    if (std::fabs(scale->y) < SCALE_MIN) {
        scale->y = (scale->y >= 0) ? SCALE_MIN : -SCALE_MIN;
    }
}

namespace {

// The canonical frame of a GL modelview stack, for glyphs built about a fixed canonical anchor:
// a vertex maps through the frame, then to screen pixels from the anchor.
class GlyphFrame {
public:
    GlyphFrame(const OfxPointD& anchor,
               const OfxPointD& pixelScale)
        : _anchor(anchor)
        , _pixelScale(pixelScale)
        , _a(1.)
        , _b(0.)
        , _c(0.)
        , _d(1.)
        , _tx(0.)
        , _ty(0.)
    {
    }

    void translate(double x,
                   double y)
    {
        multiply(1., 0., 0., 1., x, y);
    }

    void rotate(double degrees)
    {
        const double r = degrees * M_PI / 180.;
        const double c = std::cos(r);
        const double s = std::sin(r);
        multiply(c, s, -s, c, 0., 0.);
    }

    // Right-multiplies by the matrix with columns (a, b), (c, d) and (tx, ty), as glMultMatrix does.
    void multiply(double a,
                  double b,
                  double c,
                  double d,
                  double tx,
                  double ty)
    {
        const double na = _a * a + _c * b;
        const double nb = _b * a + _d * b;
        const double nc = _a * c + _c * d;
        const double nd = _b * c + _d * d;
        const double ntx = _a * tx + _c * ty + _tx;
        const double nty = _b * tx + _d * ty + _ty;
        _a = na;
        _b = nb;
        _c = nc;
        _d = nd;
        _tx = ntx;
        _ty = nty;
    }

    void begin(OverlayDrawListPen& pen,
               OverlayDrawListPen::ModeEnum mode) const
    {
        pen.beginGlyph(mode, _anchor.x, _anchor.y);
    }

    void vertex(OverlayDrawListPen& pen,
                double x,
                double y) const
    {
        const double cx = _a * x + _c * y + _tx;
        const double cy = _b * x + _d * y + _ty;
        pen.vertex((cx - _anchor.x) / _pixelScale.x, (cy - _anchor.y) / _pixelScale.y);
    }

private:
    OfxPointD _anchor;
    OfxPointD _pixelScale;
    double _a;
    double _b;
    double _c;
    double _d;
    double _tx;
    double _ty;
};

void
setHoverColor(OverlayDrawListPen& pen,
              const OfxRGBColourD& color,
              bool hovered)
{
    if (hovered) {
        pen.color(1., 0., 0.);
    } else {
        pen.color(color.r, color.g, color.b);
    }
}

void
drawSquare(OverlayDrawListPen& pen,
           GlyphFrame frame,
           const OfxRGBColourD& color,
           const OfxPointD& center,
           const OfxPointD& pixelScale,
           bool hovered,
           bool althovered)
{
    // we are not axis-aligned
    double meanPixelScale = (pixelScale.x + pixelScale.y) / 2.;

    if (hovered) {
        if (althovered) {
            pen.color(0., 1., 0.);
        } else {
            pen.color(1., 0., 0.);
        }
    } else {
        pen.color(color.r, color.g, color.b);
    }
    double halfWidth = (TransformInteract::pointSize() / 2.) * meanPixelScale;
    double halfHeight = (TransformInteract::pointSize() / 2.) * meanPixelScale;
    frame.translate(center.x, center.y);
    frame.begin(pen, OverlayDrawListPen::eModePolygon);
    frame.vertex(pen, -halfWidth, -halfHeight); // bottom left
    frame.vertex(pen, -halfWidth, +halfHeight); // top left
    frame.vertex(pen, +halfWidth, +halfHeight); // bottom right
    frame.vertex(pen, +halfWidth, -halfHeight); // top right
    pen.end();
}

void
drawEllipse(OverlayDrawListPen& pen,
            GlyphFrame frame,
            const OfxRGBColourD& color,
            const OfxPointD& center,
            const OfxPointD& targetRadius,
            bool hovered)
{
    setHoverColor(pen, color, hovered);
    frame.translate(center.x, center.y);
    frame.begin(pen, OverlayDrawListPen::eModeLineLoop);
    // we don't need to be pixel-perfect here, it's just an interact!
    // 40 segments is enough.
    for (int i = 0; i < 40; ++i) {
        double theta = i * 2 * M_PI / 40.;
        frame.vertex(pen, targetRadius.x * std::cos(theta), targetRadius.y * std::sin(theta));
    }
    pen.end();
}

// A bar from -halfSize to +halfSize along x with arrowheads at both ends, about the frame origin.
void
drawDoubleArrow(OverlayDrawListPen& pen,
                const GlyphFrame& frame,
                double y,
                double halfSize,
                double headOffsetX,
                double headOffsetY)
{
    frame.vertex(pen, -halfSize, y);
    frame.vertex(pen, +halfSize, y);

    frame.vertex(pen, -halfSize, y);
    frame.vertex(pen, -halfSize + headOffsetX, y + headOffsetY);

    frame.vertex(pen, -halfSize, y);
    frame.vertex(pen, -halfSize + headOffsetX, y - headOffsetY);

    frame.vertex(pen, +halfSize, y);
    frame.vertex(pen, +halfSize - headOffsetX, y + headOffsetY);

    frame.vertex(pen, +halfSize, y);
    frame.vertex(pen, +halfSize - headOffsetX, y - headOffsetY);
}

void
drawSkewBar(OverlayDrawListPen& pen,
            GlyphFrame frame,
            const OfxRGBColourD& color,
            const OfxPointD& center,
            const OfxPointD& pixelScale,
            double targetRadiusY,
            bool hovered,
            double angle)
{
    setHoverColor(pen, color, hovered);

    // we are not axis-aligned: use the mean pixel scale
    double meanPixelScale = (pixelScale.x + pixelScale.y) / 2.;
    double barHalfSize = targetRadiusY + 20. * meanPixelScale;

    frame.translate(center.x, center.y);
    frame.rotate(angle);

    frame.begin(pen, OverlayDrawListPen::eModeLines);
    frame.vertex(pen, 0., -barHalfSize);
    frame.vertex(pen, 0., +barHalfSize);

    if (hovered) {
        double arrowYPosition = targetRadiusY + 10. * meanPixelScale;
        double arrowXHalfSize = 10 * meanPixelScale;
        double arrowHeadOffsetX = 3 * meanPixelScale;
        double arrowHeadOffsetY = 3 * meanPixelScale;

        drawDoubleArrow(pen, frame, -arrowYPosition, arrowXHalfSize, arrowHeadOffsetX, arrowHeadOffsetY);
    }
    pen.end();
}

void
drawRotationBar(OverlayDrawListPen& pen,
                const GlyphFrame& frame,
                const OfxRGBColourD& color,
                const OfxPointD& pixelScale,
                double targetRadiusX,
                bool hovered,
                bool inverted)
{
    // we are not axis-aligned
    double meanPixelScale = (pixelScale.x + pixelScale.y) / 2.;

    setHoverColor(pen, color, hovered);

    double barExtra = 30. * meanPixelScale;
    frame.begin(pen, OverlayDrawListPen::eModeLines);
    frame.vertex(pen, 0., 0.);
    frame.vertex(pen, 0. + targetRadiusX + barExtra, 0.);
    pen.end();

    if (hovered) {
        double arrowCenterX = targetRadiusX + barExtra / 2.;

        ///draw an arrow slightly bended. This is an arc of circle of radius 5 in X, and 10 in Y.
        OfxPointD arrowRadius;
        arrowRadius.x = 5. * meanPixelScale;
        arrowRadius.y = 10. * meanPixelScale;

        GlyphFrame arrow = frame;
        arrow.translate(arrowCenterX, 0.);
        arrow.begin(pen, OverlayDrawListPen::eModeLineStrip);
        arrow.vertex(pen, 0, arrowRadius.y);
        arrow.vertex(pen, arrowRadius.x, 0.);
        arrow.vertex(pen, 0, -arrowRadius.y);
        pen.end();

        arrow.begin(pen, OverlayDrawListPen::eModeLines);
        ///draw the top head
        arrow.vertex(pen, 0., arrowRadius.y);
        arrow.vertex(pen, 0., arrowRadius.y - 5. * meanPixelScale);

        arrow.vertex(pen, 0., arrowRadius.y);
        arrow.vertex(pen, 4. * meanPixelScale, arrowRadius.y - 3. * meanPixelScale); // 5^2 = 3^2+4^2

        ///draw the bottom head
        arrow.vertex(pen, 0., -arrowRadius.y);
        arrow.vertex(pen, 0., -arrowRadius.y + 5. * meanPixelScale);

        arrow.vertex(pen, 0., -arrowRadius.y);
        arrow.vertex(pen, 4. * meanPixelScale, -arrowRadius.y + 3. * meanPixelScale); // 5^2 = 3^2+4^2
        pen.end();
    }
    if (inverted) {
        double arrowXPosition = targetRadiusX + barExtra * 1.5;
        double arrowXHalfSize = 10 * meanPixelScale;
        double arrowHeadOffsetX = 3 * meanPixelScale;
        double arrowHeadOffsetY = 3 * meanPixelScale;

        GlyphFrame arrows = frame;
        arrows.translate(arrowXPosition, 0.);
        arrows.begin(pen, OverlayDrawListPen::eModeLines);
        drawDoubleArrow(pen, arrows, 0., arrowXHalfSize, arrowHeadOffsetX, arrowHeadOffsetY);
        pen.end();

        arrows.rotate(90.);
        arrows.begin(pen, OverlayDrawListPen::eModeLines);
        drawDoubleArrow(pen, arrows, 0., arrowXHalfSize, arrowHeadOffsetX, arrowHeadOffsetY);
        pen.end();
    }
} // drawRotationBar

} // anon namespace

void
TransformInteract::draw(double time,
                        const RenderScale& /*renderScale*/,
                        ViewIdx /*view*/,
                        const OfxPointD& pscale,
                        const QPointF& lastPenPos,
                        const OfxRGBColourD& color,
                        const QFontMetrics& /*fm*/)
{
    // do not show interact if knob is secret or not enabled
    // see https://github.com/MrKepzie/Natron/issues/932
    KnobDoublePtr translateKnob = _translate.lock();

    if ( !translateKnob || !translateKnob->shouldDrawOverlayInteract() ) {
        return;
    }

    OfxPointD center;
    OfxPointD translate;
    OfxPointD scaleParam;
    bool scaleUniform;
    double rotate;
    double skewX, skewY;
    int skewOrder;
    bool inverted = false;

    if (_mouseState == TransformInteract::eReleased) {
        getCenter(time, &center.x, &center.y);
        getTranslate(time, &translate.x, &translate.y);
        getScale(time, &scaleParam.x, &scaleParam.y);
        getScaleUniform(time, &scaleUniform);
        getRotate(time, &rotate);
        getSkewX(time, &skewX);
        getSkewY(time, &skewY);
        getSkewOrder(time, &skewOrder);
        inverted = getInvert(time);
    } else {
        center = _centerDrag;
        translate = _translateDrag;
        scaleParam = _scaleParamDrag;
        scaleUniform = _scaleUniformDrag;
        rotate = _rotateDrag;
        skewX = _skewXDrag;
        skewY = _skewYDrag;
        skewOrder = _skewOrderDrag;
        inverted = _invertedDrag;
    }

    OfxPointD targetCenter;
    getTargetCenter(center, translate, &targetCenter);

    OfxPointD scale;
    ofxsTransformGetScale(scaleParam, scaleUniform, &scale);

    OfxPointD targetRadius;
    getTargetRadius(scale, pscale, &targetRadius);

    OfxPointD left, right, bottom, top;
    getTargetPoints(targetCenter, targetRadius, &left, &bottom, &top, &right);

    // add 180 to the angle to draw the arrows on the other side. unfortunately, this requires knowing
    // the mouse position in the ellipse frame
    double flip = 0.;
    if ((_drawState == TransformInteract::eSkewXBarHoverered) || (_drawState == TransformInteract::eSkewYBarHoverered)) {
        double rot = Transform::toRadians(rotate);
        Transform::Matrix3x3 transformscale;
        transformscale = Transform::matInverseTransformCanonical(0., 0., scale.x, scale.y, skewX, skewY, (bool)skewOrder, rot, targetCenter.x, targetCenter.y);

        Transform::Point3D previousPos;
        previousPos.x = lastPenPos.x();
        previousPos.y = lastPenPos.y();
        previousPos.z = 1.;
        previousPos = Transform::matApply(transformscale, previousPos);
        if (previousPos.z != 0) {
            previousPos.x /= previousPos.z;
            previousPos.y /= previousPos.z;
        }
        if (((_drawState == TransformInteract::eSkewXBarHoverered) && (previousPos.y > targetCenter.y)) || ((_drawState == TransformInteract::eSkewYBarHoverered) && (previousPos.x > targetCenter.x))) {
            flip = 180.;
        }
    }
    ContentHasher signature = beginOverlaySignature(pscale, color);
    signature.appendDouble(center.x);
    signature.appendDouble(center.y);
    signature.appendDouble(translate.x);
    signature.appendDouble(translate.y);
    signature.appendDouble(scale.x);
    signature.appendDouble(scale.y);
    signature.appendDouble(rotate);
    signature.appendDouble(skewX);
    signature.appendDouble(skewY);
    signature.appendI64(skewOrder);
    signature.appendBool(inverted);
    signature.appendI64(_mouseState);
    signature.appendI64(_drawState);
    signature.appendI64(_modifierStateCtrl);
    signature.appendDouble(flip);
    OverlayDrawList* list = beginOverlayList(signature);
    if (!list) {
        showOverlayList();

        return;
    }
    OverlayDrawListPen pen(list);
    pen.setShadow(true);
    pen.setSmoothLines(true);
    pen.lineWidth(1.5);

    GlyphFrame frame(targetCenter, pscale);
    frame.translate(targetCenter.x, targetCenter.y);
    frame.rotate(rotate);
    drawRotationBar(pen, frame, color, pscale, targetRadius.x, _mouseState == TransformInteract::eDraggingRotationBar || _drawState == TransformInteract::eRotationBarHovered, inverted);
    frame.multiply(skewOrder ? 1. : (1. + skewX * skewY), skewY, skewX, skewOrder ? (1. + skewX * skewY) : 1., 0., 0.);
    frame.translate(-targetCenter.x, -targetCenter.y);

    drawEllipse(pen, frame, color, targetCenter, targetRadius, _mouseState == TransformInteract::eDraggingCircle || _drawState == TransformInteract::eCircleHovered);

    drawSkewBar(pen, frame, color, targetCenter, pscale, targetRadius.y, _mouseState == TransformInteract::eDraggingSkewXBar || _drawState == TransformInteract::eSkewXBarHoverered, flip);
    drawSkewBar(pen, frame, color, targetCenter, pscale, targetRadius.x, _mouseState == TransformInteract::eDraggingSkewYBar || _drawState == TransformInteract::eSkewYBarHoverered, flip - 90.);

    drawSquare(pen, frame, color, targetCenter, pscale, _mouseState == TransformInteract::eDraggingTranslation || _mouseState == TransformInteract::eDraggingCenter || _drawState == TransformInteract::eCenterPointHovered, _modifierStateCtrl);
    drawSquare(pen, frame, color, left, pscale, _mouseState == TransformInteract::eDraggingLeftPoint || _drawState == TransformInteract::eLeftPointHovered, false);
    drawSquare(pen, frame, color, right, pscale, _mouseState == TransformInteract::eDraggingRightPoint || _drawState == TransformInteract::eRightPointHovered, false);
    drawSquare(pen, frame, color, top, pscale, _mouseState == TransformInteract::eDraggingTopPoint || _drawState == TransformInteract::eTopPointHovered, false);
    drawSquare(pen, frame, color, bottom, pscale, _mouseState == TransformInteract::eDraggingBottomPoint || _drawState == TransformInteract::eBottomPointHovered, false);

    showOverlayList();
} // TransformInteract::draw

void
CornerPinInteract::draw(double time,
                        const RenderScale& /*renderScale*/,
                        ViewIdx /*view*/,
                        const OfxPointD& pscale,
                        const QPointF& /*lastPenPos*/,
                        const OfxRGBColourD& color,
                        const QFontMetrics& /*fm*/)
{
    // do not show interact if knob is secret or not enabled
    // see https://github.com/MrKepzie/Natron/issues/932
    KnobDoublePtr from1Knob = _from[0].lock();

    if ( !from1Knob || !from1Knob->shouldDrawOverlayInteract() ) {
        return;
    }

    OfxPointD to[4];
    OfxPointD from[4];
    bool enable[4];
    bool useFrom;

    if (_dragging == -1) {
        for (int i = 0; i < 4; ++i) {
            getFrom(time, i, &from[i].x, &from[i].y);
            getTo(time, i, &to[i].x, &to[i].y);
            enable[i] = getEnabled(time, i);
        }
        useFrom = getUseFromPoints(time);
    } else {
        for (int i = 0; i < 4; ++i) {
            to[i] = _toDrag[i];
            from[i] = _fromDrag[i];
            enable[i] = _enableDrag[i];
        }
        useFrom = _useFromDrag;
    }

    OfxPointD p[4];
    OfxPointD q[4];
    int enableBegin = 4;
    int enableEnd = 0;
    for (int i = 0; i < 4; ++i) {
        if (enable[i]) {
            if (useFrom) {
                p[i] = from[i];
                q[i] = to[i];
            } else {
                q[i] = from[i];
                p[i] = to[i];
            }
            if (i < enableBegin) {
                enableBegin = i;
            }
            if (i + 1 > enableEnd) {
                enableEnd = i + 1;
            }
        }
    }

    ContentHasher signature = beginOverlaySignature(pscale, color);
    signature.appendBool(useFrom);
    signature.appendI64(_hovering);
    signature.appendI64(_dragging);
    for (int i = 0; i < 4; ++i) {
        signature.appendBool(enable[i]);
        signature.appendDouble(from[i].x);
        signature.appendDouble(from[i].y);
        signature.appendDouble(to[i].x);
        signature.appendDouble(to[i].y);
        signature.appendString(useFrom ? _from[i].lock()->getName() : _to[i].lock()->getName());
    }
    OverlayDrawList* list = beginOverlayList(signature);
    if (!list) {
        showOverlayList();

        return;
    }
    OverlayDrawListPen pen(list);
    pen.setShadow(true);
    pen.setSmoothLines(true);
    pen.setSmoothPoints(false);
    pen.lineWidth(1.5);

    pen.color(color.r / 2, color.g / 2, color.b / 2);
    pen.begin(OverlayDrawListPen::eModeLines);
    for (int i = enableBegin; i < enableEnd; ++i) {
        if (enable[i]) {
            pen.vertex(p[i].x, p[i].y);
            pen.vertex(q[i].x, q[i].y);
        }
    }
    pen.end();
    pen.color(color.r, color.g, color.b);
    pen.begin(OverlayDrawListPen::eModeLineLoop);
    for (int i = enableBegin; i < enableEnd; ++i) {
        if (enable[i]) {
            pen.vertex(p[i].x, p[i].y);
        }
    }
    pen.end();

    pen.pointSize(CornerPinInteract::pointSize());
    pen.begin(OverlayDrawListPen::eModePoints);
    for (int i = enableBegin; i < enableEnd; ++i) {
        if (enable[i]) {
            if ((_hovering == i) || (_dragging == i)) {
                pen.color(0., 1., 0.);
            } else {
                pen.color(color.r, color.g, color.b);
            }
            pen.vertex(p[i].x, p[i].y);
        }
    }
    pen.end();
    pen.color(color.r, color.g, color.b);
    for (int i = enableBegin; i < enableEnd; ++i) {
        if (enable[i]) {
            pen.text(p[i].x, p[i].y, useFrom ? _from[i].lock()->getName() : _to[i].lock()->getName());
        }
    }
    showOverlayList();
} // CornerPinInteract::draw

void
HostOverlay::draw(double time,
                  const RenderScale& renderScale,
                  ViewIdx view)
{
    OfxRGBColourD color;

    if ( !getNode()->getOverlayColor(&color.r, &color.g, &color.b) ) {
        color.r = color.g = color.b = 0.8;
    }
    OfxPointD pscale;
    n_getPixelScale(pscale.x, pscale.y);
    QFontMetrics fm(QFont(appFont, appFontSize));
    // draw in reverse order
    for (DefaultInteractIPtrList::reverse_iterator it = _imp->interacts.rbegin(); it != _imp->interacts.rend(); ++it) {
        (*it)->draw(time, renderScale, view, pscale, _imp->lastPenPos, color, fm);
    }
}

bool
PositionInteract::penMotion(double time,
                            const RenderScale& /*renderScale*/,
                            ViewIdx /*view*/,
                            const OfxPointD& pscale,
                            const QPointF& /*lastPenPos*/,
                            const QPointF& penPos,
                            const QPoint& /*penPosViewport*/,
                            double /*pressure*/)
{
    KnobDoublePtr knob = _param.lock();

    // do not show interact if knob is secret or not enabled
    // see https://github.com/MrKepzie/Natron/issues/932
    if ( !knob || !knob->shouldDrawOverlayInteract() ) {
        return false;
    }

    QPointF pos;
    if (_state == ePositionInteractStatePicked) {
        pos = penPos;
    } else {
        double p[2];
        for (int i = 0; i < 2; ++i) {
            p[i] = knob->getValueAtTime(time, i);
            if (knob->getValueIsNormalized(i) != eValueIsNormalizedNone) {
                p[i] = knob->denormalize(i, time, p[i]);
            }
        }
        pos.setX(p[0]);
        pos.setY(p[1]);
    }

    bool didSomething = false;
    bool valuesChanged = false;

    switch (_state) {
    case ePositionInteractStateInactive:
    case ePositionInteractStatePoised: {
        // are we in the box, become 'poised'
        PositionInteractState newState;
        if ( ( std::fabs( penPos.x() - pos.x() ) <= pointTolerance() * pscale.x) &&
             ( std::fabs( penPos.y() - pos.y() ) <= pointTolerance() * pscale.y) ) {
            newState = ePositionInteractStatePoised;
        } else {
            newState = ePositionInteractStateInactive;
        }

        if (_state != newState) {
            // state changed, must redraw
            requestRedraw();
        }
        _state = newState;
        //}
        break;
    }

    case ePositionInteractStatePicked: {
        valuesChanged = true;
        break;
    }
    }
    didSomething = (_state == ePositionInteractStatePoised) || (_state == ePositionInteractStatePicked);

    if ( (_state != ePositionInteractStateInactive) && _interactiveDrag && valuesChanged ) {
        double p[2];
        p[0] = fround(penPos.x(), pscale.x);
        p[1] = fround(penPos.y(), pscale.y);
        for (int i = 0; i < 2; ++i) {
            if (knob->getValueIsNormalized(i) != eValueIsNormalizedNone) {
                p[i] = knob->normalize(i, time, p[i]);
            }
        }

        setOverlayKnobValues(knob, p[0], p[1]);
    }

    return (didSomething || valuesChanged);
} // PositionInteract::penMotion

static bool
squareContains(const Transform::Point3D& pos,
               const OfxRectD& rect,
               double toleranceX = 0.,
               double toleranceY = 0.)
{
    return ( pos.x >= (rect.x1 - toleranceX) && pos.x < (rect.x2 + toleranceX)
             && pos.y >= (rect.y1 - toleranceY) && pos.y < (rect.y2 + toleranceY) );
}

static bool
isOnEllipseBorder(const Transform::Point3D& pos,
                  const OfxPointD& targetRadius,
                  const OfxPointD& targetCenter,
                  double epsilon = 0.1)
{
    double v = ( (pos.x - targetCenter.x) * (pos.x - targetCenter.x) / (targetRadius.x * targetRadius.x) +
                 (pos.y - targetCenter.y) * (pos.y - targetCenter.y) / (targetRadius.y * targetRadius.y) );

    if ( ( v <= (1. + epsilon) ) && ( v >= (1. - epsilon) ) ) {
        return true;
    }

    return false;
}

static bool
isOnSkewXBar(const Transform::Point3D& pos,
             double targetRadiusY,
             const OfxPointD& center,
             const OfxPointD& pixelScale,
             double tolerance)
{
    // we are not axis-aligned
    double meanPixelScale = (pixelScale.x + pixelScale.y) / 2.;
    double barHalfSize = targetRadiusY + (20. * meanPixelScale);

    if ( ( pos.x >= (center.x - tolerance) ) && ( pos.x <= (center.x + tolerance) ) &&
         ( pos.y >= (center.y - barHalfSize - tolerance) ) && ( pos.y <= (center.y + barHalfSize + tolerance) ) ) {
        return true;
    }

    return false;
}

static bool
isOnSkewYBar(const Transform::Point3D& pos,
             double targetRadiusX,
             const OfxPointD& center,
             const OfxPointD& pixelScale,
             double tolerance)
{
    // we are not axis-aligned
    double meanPixelScale = (pixelScale.x + pixelScale.y) / 2.;
    double barHalfSize = targetRadiusX + (20. * meanPixelScale);

    if ( ( pos.y >= (center.y - tolerance) ) && ( pos.y <= (center.y + tolerance) ) &&
         ( pos.x >= (center.x - barHalfSize - tolerance) ) && ( pos.x <= (center.x + barHalfSize + tolerance) ) ) {
        return true;
    }

    return false;
}

static bool
isOnRotationBar(const Transform::Point3D& pos,
                double targetRadiusX,
                const OfxPointD& center,
                const OfxPointD& pixelScale,
                double tolerance)
{
    // we are not axis-aligned
    double meanPixelScale = (pixelScale.x + pixelScale.y) / 2.;
    double barExtra = 30. * meanPixelScale;

    if ( ( pos.x >= (center.x - tolerance) ) && ( pos.x <= (center.x + targetRadiusX + barExtra + tolerance) ) &&
         ( pos.y >= (center.y  - tolerance) ) && ( pos.y <= (center.y + tolerance) ) ) {
        return true;
    }

    return false;
}

static OfxRectD
rectFromCenterPoint(const OfxPointD& center,
                    const OfxPointD& pixelScale)
{
    // we are not axis-aligned
    double meanPixelScale = (pixelScale.x + pixelScale.y) / 2.;
    OfxRectD ret;

    ret.x1 = center.x - (TransformInteract::pointSize() / 2.) * meanPixelScale;
    ret.x2 = center.x + (TransformInteract::pointSize() / 2.) * meanPixelScale;
    ret.y1 = center.y - (TransformInteract::pointSize() / 2.) * meanPixelScale;
    ret.y2 = center.y + (TransformInteract::pointSize() / 2.) * meanPixelScale;

    return ret;
}

bool
TransformInteract::penMotion(double time,
                             const RenderScale& /*renderScale*/,
                             ViewIdx /*view*/,
                             const OfxPointD& pscale,
                             const QPointF& lastPenPos,
                             const QPointF &penPosParam,
                             const QPoint & /*penPosViewport*/,
                             double /*pressure*/)
{
    // do not show interact if knob is secret or not enabled
    // see https://github.com/MrKepzie/Natron/issues/932
    KnobDoublePtr translateKnob = _translate.lock();

    if ( !translateKnob || !translateKnob->shouldDrawOverlayInteract() ) {
        return false;
    }

    OfxPointD center;
    OfxPointD translate;
    OfxPointD scaleParam;
    bool scaleUniform;
    double rotate;
    double skewX, skewY;
    int skewOrder;
    bool inverted = false;

    if (_mouseState == TransformInteract::eReleased) {
        getCenter(time, &center.x, &center.y);
        getTranslate(time, &translate.x, &translate.y);
        getScale(time, &scaleParam.x, &scaleParam.y);
        getScaleUniform(time, &scaleUniform);
        getRotate(time, &rotate);
        getSkewX(time, &skewX);
        getSkewY(time, &skewY);
        getSkewOrder(time, &skewOrder);
        inverted = getInvert(time);
    } else {
        center = _centerDrag;
        translate = _translateDrag;
        scaleParam = _scaleParamDrag;
        scaleUniform = _scaleUniformDrag;
        rotate = _rotateDrag;
        skewX = _skewXDrag;
        skewY = _skewYDrag;
        skewOrder = _skewOrderDrag;
        inverted = _invertedDrag;
    }

    bool didSomething = false;
    bool centerChanged = false;
    bool translateChanged = false;
    bool scaleChanged = false;
    bool rotateChanged = false;
    bool skewXChanged = false;
    bool skewYChanged = false;
    OfxPointD targetCenter;
    getTargetCenter(center, translate, &targetCenter);

    OfxPointD scale;
    ofxsTransformGetScale(scaleParam, scaleUniform, &scale);

    OfxPointD targetRadius;
    getTargetRadius(scale, pscale, &targetRadius);

    OfxPointD left, right, bottom, top;
    getTargetPoints(targetCenter, targetRadius, &left, &bottom, &top, &right);

    OfxRectD centerPoint = rectFromCenterPoint(targetCenter, pscale);
    OfxRectD leftPoint = rectFromCenterPoint(left, pscale);
    OfxRectD rightPoint = rectFromCenterPoint(right, pscale);
    OfxRectD topPoint = rectFromCenterPoint(top, pscale);
    OfxRectD bottomPoint = rectFromCenterPoint(bottom, pscale);


    //double dx = args.penPosition.x - _lastMousePos.x;
    //double dy = args.penPosition.y - _lastMousePos.y;
    double rot = Transform::toRadians(rotate);
    Transform::Point3D penPos, prevPenPos, rotationPos, transformedPos, previousPos, currentPos;
    penPos.x = penPosParam.x();
    penPos.y = penPosParam.y();
    penPos.z = 1.;
    prevPenPos.x = lastPenPos.x();
    prevPenPos.y = lastPenPos.y();
    prevPenPos.z = 1.;

    Transform::Matrix3x3 rotation, transform, transformscale;
    ////for the rotation bar/translation/center dragging we don't use the same transform, we don't want to undo the rotation transform
    if ( (_mouseState != TransformInteract::eDraggingTranslation) && (_mouseState != TransformInteract::eDraggingCenter) ) {
        ///undo skew + rotation to the current position
        rotation = Transform::matInverseTransformCanonical(0., 0., 1., 1., 0., 0., false, rot, targetCenter.x, targetCenter.y);
        transform = Transform::matInverseTransformCanonical(0., 0., 1., 1., skewX, skewY, (bool)skewOrder, rot, targetCenter.x, targetCenter.y);
        transformscale = Transform::matInverseTransformCanonical(0., 0., scale.x, scale.y, skewX, skewY, (bool)skewOrder, rot, targetCenter.x, targetCenter.y);
    } else {
        rotation = Transform::matInverseTransformCanonical(0., 0., 1., 1., 0., 0., false, 0., targetCenter.x, targetCenter.y);
        transform = Transform::matInverseTransformCanonical(0., 0., 1., 1., skewX, skewY, (bool)skewOrder, 0., targetCenter.x, targetCenter.y);
        transformscale = Transform::matInverseTransformCanonical(0., 0., scale.x, scale.y, skewX, skewY, (bool)skewOrder, 0., targetCenter.x, targetCenter.y);
    }

    rotationPos = Transform::matApply(rotation, penPos);
    if (rotationPos.z != 0) {
        rotationPos.x /= rotationPos.z;
        rotationPos.y /= rotationPos.z;
    }

    transformedPos = Transform::matApply(transform, penPos);
    if (transformedPos.z != 0) {
        transformedPos.x /= transformedPos.z;
        transformedPos.y /= transformedPos.z;
    }

    previousPos = Transform::matApply(transformscale, prevPenPos);
    if (previousPos.z != 0) {
        previousPos.x /= previousPos.z;
        previousPos.y /= previousPos.z;
    }

    currentPos = Transform::matApply(transformscale, penPos);
    if (currentPos.z != 0) {
        currentPos.x /= currentPos.z;
        currentPos.y /= currentPos.z;
    }

    double minX, minY, maxX, maxY;
    KnobDoublePtr scaleKnob = _scale.lock();
    assert(scaleKnob);

    minX = scaleKnob->getMinimum(0);
    minY = scaleKnob->getMinimum(1);
    maxX = scaleKnob->getMaximum(0);
    maxY = scaleKnob->getMaximum(1);

    if (_mouseState == TransformInteract::eReleased) {
        // we are not axis-aligned
        double meanPixelScale = (pscale.x + pscale.y) / 2.;
        double hoverTolerance = (TransformInteract::pointSize() / 2.) * meanPixelScale;
        if ( squareContains(transformedPos, centerPoint) ) {
            _drawState = TransformInteract::eCenterPointHovered;
            didSomething = true;
        } else if ( squareContains(transformedPos, leftPoint) ) {
            _drawState = TransformInteract::eLeftPointHovered;
            didSomething = true;
        } else if ( squareContains(transformedPos, rightPoint) ) {
            _drawState = TransformInteract::eRightPointHovered;
            didSomething = true;
        } else if ( squareContains(transformedPos, topPoint) ) {
            _drawState = TransformInteract::eTopPointHovered;
            didSomething = true;
        } else if ( squareContains(transformedPos, bottomPoint) ) {
            _drawState = TransformInteract::eBottomPointHovered;
            didSomething = true;
        } else if ( isOnEllipseBorder(transformedPos, targetRadius, targetCenter) ) {
            _drawState = TransformInteract::eCircleHovered;
            didSomething = true;
        } else if ( isOnRotationBar(rotationPos, targetRadius.x, targetCenter, pscale, hoverTolerance) ) {
            _drawState = TransformInteract::eRotationBarHovered;
            didSomething = true;
        } else if ( isOnSkewXBar(transformedPos, targetRadius.y, targetCenter, pscale, hoverTolerance) ) {
            _drawState = TransformInteract::eSkewXBarHoverered;
            didSomething = true;
        } else if ( isOnSkewYBar(transformedPos, targetRadius.x, targetCenter, pscale, hoverTolerance) ) {
            _drawState = TransformInteract::eSkewYBarHoverered;
            didSomething = true;
        } else {
            _drawState = TransformInteract::eInActive;
        }
    } else if (_mouseState == TransformInteract::eDraggingCircle) {
        // we need to compute the backtransformed points with the scale

        // the scale ratio is the ratio of distances to the center
        double prevDistSq = (targetCenter.x - previousPos.x) * (targetCenter.x - previousPos.x) + (targetCenter.y - previousPos.y) * (targetCenter.y - previousPos.y);
        if (prevDistSq != 0.) {
            const double distSq = (targetCenter.x - currentPos.x) * (targetCenter.x - currentPos.x) + (targetCenter.y - currentPos.y) * (targetCenter.y - currentPos.y);
            const double distRatio = std::sqrt(distSq / prevDistSq);
            scale.x *= distRatio;
            scale.y *= distRatio;
            //_scale->setValue(scale.x, scale.y);
            scaleChanged = true;
        }
    } else if ( (_mouseState == TransformInteract::eDraggingLeftPoint) || (_mouseState == TransformInteract::eDraggingRightPoint) ) {
        // avoid division by zero
        if (targetCenter.x != previousPos.x) {
            const double scaleRatio = (targetCenter.x - currentPos.x) / (targetCenter.x - previousPos.x);
            OfxPointD newScale;
            newScale.x = scale.x * scaleRatio;
            newScale.x = std::max( minX, std::min(newScale.x, maxX) );
            newScale.y = scaleUniform ? newScale.x : scale.y;
            scale = newScale;
            //_scale->setValue(scale.x, scale.y);
            scaleChanged = true;
        }
    } else if ( (_mouseState == TransformInteract::eDraggingTopPoint) || (_mouseState == TransformInteract::eDraggingBottomPoint) ) {
        // avoid division by zero
        if (targetCenter.y != previousPos.y) {
            const double scaleRatio = (targetCenter.y - currentPos.y) / (targetCenter.y - previousPos.y);
            OfxPointD newScale;
            newScale.y = scale.y * scaleRatio;
            newScale.y = std::max( minY, std::min(newScale.y, maxY) );
            newScale.x = scaleUniform ? newScale.y : scale.x;
            scale = newScale;
            //_scale->setValue(scale.x, scale.y);
            scaleChanged = true;
        }
    } else if (_mouseState == TransformInteract::eDraggingTranslation) {
        double dx = penPosParam.x() - lastPenPos.x();
        double dy = penPosParam.y() - lastPenPos.y();

        if ( (_orientation == TransformInteract::eOrientationNotSet) && (_modifierStateShift > 0) ) {
            _orientation = std::abs(dx) > std::abs(dy) ? TransformInteract::eOrientationHorizontal : TransformInteract::eOrientationVertical;
        }

        dx = _orientation == TransformInteract::eOrientationVertical ? 0 : dx;
        dy = _orientation == TransformInteract::eOrientationHorizontal ? 0 : dy;
        double newx = translate.x + dx;
        double newy = translate.y + dy;
        // round newx/y to the closest int, 1/10 int, etc
        // this make parameter editing easier
        newx = fround(newx, pscale.x);
        newy = fround(newy, pscale.y);
        translate.x = newx;
        translate.y = newy;
        //_translate->setValue(translate.x, translate.y);
        translateChanged = true;
    } else if (_mouseState == TransformInteract::eDraggingCenter) {
        OfxPointD currentCenter = center;
        Transform::Matrix3x3 R = Transform::matMul( Transform::matMul( Transform::matScale(1. / scale.x, 1. / scale.y), Transform::matSkewXY(-skewX, -skewY, !skewOrder) ), Transform::matRotation(rot) );
        double dx = penPosParam.x() - lastPenPos.x();
        double dy = penPosParam.y() - lastPenPos.y();

        if ( (_orientation == TransformInteract::eOrientationNotSet) && (_modifierStateShift > 0) ) {
            _orientation = std::abs(dx) > std::abs(dy) ? TransformInteract::eOrientationHorizontal : TransformInteract::eOrientationVertical;
        }

        dx = _orientation == TransformInteract::eOrientationVertical ? 0 : dx;
        dy = _orientation == TransformInteract::eOrientationHorizontal ? 0 : dy;

        Transform::Point3D dRot;
        dRot.x = dx;
        dRot.y = dy;
        dRot.z = 1.;
        dRot = Transform::matApply(R, dRot);
        if (dRot.z != 0) {
            dRot.x /= dRot.z;
            dRot.y /= dRot.z;
        }
        double dxrot = dRot.x;
        double dyrot = dRot.y;
        double newx = currentCenter.x + dxrot;
        double newy = currentCenter.y + dyrot;
        // round newx/y to the closest int, 1/10 int, etc
        // this make parameter editing easier
        newx = fround(newx, pscale.x);
        newy = fround(newy, pscale.y);
        center.x = newx;
        center.y = newy;
        //_effect->beginEditBlock("setCenter");
        //_center->setValue(center.x, center.y);
        centerChanged = true;
        // recompute dxrot,dyrot after rounding
        double det = Transform::matDeterminant(R);
        if (det != 0.) {
            Transform::Matrix3x3 Rinv = Transform::matInverse(R, det);

            dxrot = newx - currentCenter.x;
            dyrot = newy - currentCenter.y;
            dRot.x = dxrot;
            dRot.y = dyrot;
            dRot.z = 1;
            dRot = Transform::matApply(Rinv, dRot);
            if (dRot.z != 0) {
                dRot.x /= dRot.z;
                dRot.y /= dRot.z;
            }
            dx = dRot.x;
            dy = dRot.y;
            OfxPointD newTranslation;
            newTranslation.x = translate.x + dx - dxrot;
            newTranslation.y = translate.y + dy - dyrot;
            translate = newTranslation;
            //_translate->setValue(translate.x, translate.y);
            translateChanged = true;
        }
        //_effect->endEditBlock();
    } else if (_mouseState == TransformInteract::eDraggingRotationBar) {
        OfxPointD diffToCenter;
        ///the current mouse position (untransformed) is doing has a certain angle relative to the X axis
        ///which can be computed by : angle = arctan(opposite / adjacent)
        diffToCenter.y = rotationPos.y - targetCenter.y;
        diffToCenter.x = rotationPos.x - targetCenter.x;
        double angle = std::atan2(diffToCenter.y, diffToCenter.x);
        double angledegrees = rotate + Transform::toDegrees(angle);
        double closest90 = 90. * std::floor( (angledegrees + 45.) / 90. );
        if (std::fabs(angledegrees - closest90) < 5.) {
            // snap to closest multiple of 90.
            angledegrees = closest90;
        }
        rotate = angledegrees;
        //_rotate->setValue(rotate);
        rotateChanged = true;
    } else if (_mouseState == TransformInteract::eDraggingSkewXBar) {
        // avoid division by zero
        if ( (scale.y != 0.) && (targetCenter.y != previousPos.y) ) {
            const double addSkew = (scale.x / scale.y) * (currentPos.x - previousPos.x) / (currentPos.y - targetCenter.y);
            skewX = skewX + addSkew;
            //_skewX->setValue(skewX);
            skewXChanged = true;
        }
    } else if (_mouseState == TransformInteract::eDraggingSkewYBar) {
        // avoid division by zero
        if ( (scale.x != 0.) && (targetCenter.x != previousPos.x) ) {
            const double addSkew = (scale.y / scale.x) * (currentPos.y - previousPos.y) / (currentPos.x - targetCenter.x);
            skewY = skewY + addSkew;
            //_skewY->setValue(skewY + addSkew);
            skewYChanged = true;
        }
    } else {
        assert(false);
    }

    _centerDrag = center;
    _translateDrag = translate;
    _scaleParamDrag = scale;
    _scaleUniformDrag = scaleUniform;
    _rotateDrag = rotate;
    _skewXDrag = skewX;
    _skewYDrag = skewY;
    _skewOrderDrag = skewOrder;
    _invertedDrag = inverted;

    bool valuesChanged = (centerChanged || translateChanged || scaleChanged || rotateChanged || skewXChanged || skewYChanged);

    if ( (_mouseState != TransformInteract::eReleased) && _interactiveDrag && valuesChanged ) {
        // no need to redraw overlay since it is slave to the parameters
        if (centerChanged) {
            KnobDoublePtr knob = _center.lock();
            setOverlayKnobValues(knob, center);
        }
        if (translateChanged) {
            KnobDoublePtr knob = _translate.lock();
            setOverlayKnobValues(knob, translate);
        }
        if (scaleChanged) {
            KnobDoublePtr knob = _scale.lock();
            setOverlayKnobValues(knob, scale);
        }
        if (rotateChanged) {
            KnobDoublePtr knob = _rotate.lock();
            setOverlayKnobValue(knob, rotate, 0);
        }
        if (skewXChanged) {
            KnobDoublePtr knob = _skewX.lock();
            setOverlayKnobValue(knob, skewX, 0);
        }
        if (skewYChanged) {
            KnobDoublePtr knob = _skewY.lock();
            setOverlayKnobValue(knob, skewY, 0);
        }
    } else if (didSomething || valuesChanged) {
        requestRedraw();
    }

    return didSomething || valuesChanged;
} // TransformInteract::penMotion

bool
CornerPinInteract::penMotion(double time,
                             const RenderScale& /*renderScale*/,
                             ViewIdx /*view*/,
                             const OfxPointD& pscale,
                             const QPointF& lastPenPos,
                             const QPointF &penPos,
                             const QPoint & /*penPosViewport*/,
                             double /*pressure*/)
{
    // do not show interact if knob is secret or not enabled
    // see https://github.com/MrKepzie/Natron/issues/932
    KnobDoublePtr from1Knob = _from[0].lock();

    if ( !from1Knob || !from1Knob->shouldDrawOverlayInteract() ) {
        return false;
    }

    OfxPointD to[4];
    OfxPointD from[4];
    bool enable[4];
    bool useFrom;

    if (_dragging == -1) {
        for (int i = 0; i < 4; ++i) {
            getFrom(time, i, &from[i].x, &from[i].y);
            getTo(time, i, &to[i].x, &to[i].y);
            enable[i] = getEnabled(time, i);
        }
        useFrom = getUseFromPoints(time);
    } else {
        for (int i = 0; i < 4; ++i) {
            to[i] = _toDrag[i];
            from[i] = _fromDrag[i];
            enable[i] = _enableDrag[i];
        }
        useFrom = _useFromDrag;
    }


    OfxPointD p[4];
    OfxPointD q[4];
    int enableBegin = 4;
    int enableEnd = 0;
    for (int i = 0; i < 4; ++i) {
        if (enable[i]) {
            if (useFrom) {
                p[i] = from[i];
                q[i] = to[i];
            } else {
                q[i] = from[i];
                p[i] = to[i];
            }
            if (i < enableBegin) {
                enableBegin = i;
            }
            if (i + 1 > enableEnd) {
                enableEnd = i + 1;
            }
        }
    }

    bool didSomething = false;
    bool valuesChanged = false;
    OfxPointD delta;
    delta.x = penPos.x() - lastPenPos.x();
    delta.y = penPos.y() - lastPenPos.y();

    _hovering = -1;

    for (int i = enableBegin; i < enableEnd; ++i) {
        if (enable[i]) {
            if (_dragging == i) {
                if (useFrom) {
                    from[i].x += delta.x;
                    from[i].y += delta.y;
                    _fromDrag[i] = from[i];
                } else {
                    to[i].x += delta.x;
                    to[i].y += delta.y;
                    _toDrag[i] = to[i];
                }
                valuesChanged = true;
            } else if ( CornerPinInteract::isNearby(penPos, p[i].x, p[i].y, CornerPinInteract::pointTolerance(), pscale) ) {
                _hovering = i;
                didSomething = true;
            }
        }
    }

    if ( (_dragging != -1) && _interactiveDrag && valuesChanged ) {
        // no need to redraw overlay since it is slave to the parameters

        if (_useFromDrag) {
            KnobDoublePtr knob = _from[_dragging].lock();
            assert(knob);
            setOverlayKnobValues(knob, from[_dragging]);
        } else {
            KnobDoublePtr knob = _to[_dragging].lock();
            assert(knob);
            setOverlayKnobValues(knob, to[_dragging]);
        }
    }

    return didSomething || valuesChanged;
} // CornerPinInteract::penMotion

bool
HostOverlay::penMotion(double time,
                       const RenderScale& renderScale,
                       ViewIdx view,
                       const QPointF &penPos,
                       const QPoint &penPosViewport,
                       double pressure)
{
    OfxPointD pscale;

    n_getPixelScale(pscale.x, pscale.y);
    for (DefaultInteractIPtrList::iterator it = _imp->interacts.begin(); it != _imp->interacts.end(); ++it) {
        if ( (*it)->penMotion(time, renderScale, view, pscale, _imp->lastPenPos, penPos, penPosViewport, pressure) ) {
            _imp->lastPenPos = penPos;

            return true;
        }
    }

    _imp->lastPenPos = penPos;

    return false;
}

bool
PositionInteract::penUp(double time,
                        const RenderScale& renderScale,
                        ViewIdx view,
                        const OfxPointD& pscale,
                        const QPointF& lastPenPos,
                        const QPointF &penPos,
                        const QPoint &penPosViewport,
                        double pressure)
{
    KnobDoublePtr knob = _param.lock();

    // do not show interact if knob is secret or not enabled
    // see https://github.com/MrKepzie/Natron/issues/932
    if ( !knob || !knob->shouldDrawOverlayInteract() ) {
        return false;
    }

    bool didSomething = false;
    if (_state == ePositionInteractStatePicked) {
        if (!_interactiveDrag) {
            KnobDoublePtr knob = _param.lock();
            double p[2];
            p[0] = fround(penPos.x(), pscale.x);
            p[1] = fround(penPos.y(), pscale.y);
            for (int i = 0; i < 2; ++i) {
                if (knob->getValueIsNormalized(i) != eValueIsNormalizedNone) {
                    p[i] = knob->normalize(i, time, p[i]);
                }
            }

            setOverlayKnobValues(knob, p[0], p[1]);
        }

        _state = ePositionInteractStateInactive;
        bool motion = penMotion(time, renderScale, view, pscale, lastPenPos, penPos, penPosViewport, pressure);
        Q_UNUSED(motion);
        didSomething = true;
    }

    return didSomething;
}

bool
TransformInteract::penUp(double /*time*/,
                         const RenderScale& /*renderScale*/,
                         ViewIdx /*view*/,
                         const OfxPointD& /*pscale*/,
                         const QPointF& /*lastPenPos*/,
                         const QPointF & /*penPos*/,
                         const QPoint & /*penPosViewport*/,
                         double /*pressure*/)
{
    bool ret = _mouseState != TransformInteract::eReleased;

    if ( !_interactiveDrag && (_mouseState != TransformInteract::eReleased) ) {
        // no need to redraw overlay since it is slave to the parameters
        {
            KnobDoublePtr knob = _center.lock();
            setOverlayKnobValues(knob, _centerDrag);
        }
        {
            KnobDoublePtr knob = _translate.lock();
            setOverlayKnobValues(knob, _translateDrag);
        }
        {
            KnobDoublePtr knob = _scale.lock();
            setOverlayKnobValues(knob, _scaleParamDrag);
        }
        {
            KnobDoublePtr knob = _rotate.lock();
            setOverlayKnobValue(knob, _rotateDrag, 0);
        }
        {
            KnobDoublePtr knob = _skewX.lock();
            setOverlayKnobValue(knob, _skewXDrag, 0);
        }
        {
            KnobDoublePtr knob = _skewY.lock();
            setOverlayKnobValue(knob, _skewYDrag, 0);
        }
    } else if (_mouseState != TransformInteract::eReleased) {
        requestRedraw();
    }

    _mouseState = TransformInteract::eReleased;

    return ret;
}

bool
CornerPinInteract::penUp(double /*time*/,
                         const RenderScale& /*renderScale*/,
                         ViewIdx /*view*/,
                         const OfxPointD& /*pscale*/,
                         const QPointF& /*lastPenPos*/,
                         const QPointF & /*penPos*/,
                         const QPoint & /*penPosViewport*/,
                         double /*pressure*/)
{
    // do not show interact if knob is secret or not enabled
    // see https://github.com/MrKepzie/Natron/issues/932
    KnobDoublePtr from1Knob = _from[0].lock();

    if ( !from1Knob || !from1Knob->shouldDrawOverlayInteract() ) {
        return false;
    }

    bool didSomething = _dragging != -1;

    if ( !_interactiveDrag && (_dragging != -1) ) {
        // no need to redraw overlay since it is slave to the parameters
        if (_useFromDrag) {
            KnobDoublePtr knob = _from[_dragging].lock();
            assert(knob);
            setOverlayKnobValues(knob, _fromDrag[_dragging]);
        } else {
            KnobDoublePtr knob = _to[_dragging].lock();
            assert(knob);
            setOverlayKnobValues(knob, _toDrag[_dragging]);
        }
    }
    _dragging = -1;

    return didSomething;
}

bool
HostOverlay::penUp(double time,
                   const RenderScale& renderScale,
                   ViewIdx view,
                   const QPointF &penPos,
                   const QPoint &penPosViewport,
                   double pressure)
{
    OfxPointD pscale;

    n_getPixelScale(pscale.x, pscale.y);

    bool didSomething = false;
    for (DefaultInteractIPtrList::iterator it = _imp->interacts.begin(); it != _imp->interacts.end(); ++it) {
        didSomething |= (*it)->penUp(time, renderScale, view, pscale, _imp->lastPenPos, penPos, penPosViewport, pressure);
    }

    return didSomething;
}

bool
PositionInteract::penDown(double time,
                          const RenderScale& renderScale,
                          ViewIdx view,
                          const OfxPointD& pscale,
                          const QPointF& lastPenPos,
                          const QPointF &penPos,
                          const QPoint &penPosViewport,
                          double pressure)
{
    KnobDoublePtr knob = _param.lock();

    // do not show interact if knob is secret or not enabled
    // see https://github.com/MrKepzie/Natron/issues/932
    if ( !knob || !knob->shouldDrawOverlayInteract() ) {
        return false;
    }

    bool motion = penMotion(time, renderScale, view, pscale, lastPenPos, penPos, penPosViewport, pressure);
    Q_UNUSED(motion);
    if (_state == ePositionInteractStatePoised) {
        _state = ePositionInteractStatePicked;
        _interactiveDrag = getInteractive(time);

        return true;
    }

    return false;
}

bool
TransformInteract::penDown(double time,
                           const RenderScale& /*renderScale*/,
                           ViewIdx /*view*/,
                           const OfxPointD& pscale,
                           const QPointF& /*lastPenPos*/,
                           const QPointF &penPosParam,
                           const QPoint & /*penPosViewport*/,
                           double /*pressure*/)
{
    // do not show interact if knob is secret or not enabled
    // see https://github.com/MrKepzie/Natron/issues/932
    KnobDoublePtr translateKnob = _translate.lock();

    if ( !translateKnob || !translateKnob->shouldDrawOverlayInteract() ) {
        return false;
    }

    OfxPointD center;
    OfxPointD translate;
    OfxPointD scaleParam;
    bool scaleUniform;
    double rotate;
    double skewX, skewY;
    int skewOrder;
    bool inverted = false;

    if (_mouseState == TransformInteract::eReleased) {
        getCenter(time, &center.x, &center.y);
        getTranslate(time, &translate.x, &translate.y);
        getScale(time, &scaleParam.x, &scaleParam.y);
        getScaleUniform(time, &scaleUniform);
        getRotate(time, &rotate);
        getSkewX(time, &skewX);
        getSkewY(time, &skewY);
        getSkewOrder(time, &skewOrder);
        inverted = getInvert(time);
        _interactiveDrag = getInteractive();
    } else {
        center = _centerDrag;
        translate = _translateDrag;
        scaleParam = _scaleParamDrag;
        scaleUniform = _scaleUniformDrag;
        rotate = _rotateDrag;
        skewX = _skewXDrag;
        skewY = _skewYDrag;
        skewOrder = _skewOrderDrag;
        inverted = _invertedDrag;
    }


    OfxPointD targetCenter;
    getTargetCenter(center, translate, &targetCenter);

    OfxPointD scale;
    ofxsTransformGetScale(scaleParam, scaleUniform, &scale);

    OfxPointD targetRadius;
    getTargetRadius(scale, pscale, &targetRadius);

    OfxPointD left, right, bottom, top;
    getTargetPoints(targetCenter, targetRadius, &left, &bottom, &top, &right);

    OfxRectD centerPoint = rectFromCenterPoint(targetCenter, pscale);
    OfxRectD leftPoint = rectFromCenterPoint(left, pscale);
    OfxRectD rightPoint = rectFromCenterPoint(right, pscale);
    OfxRectD topPoint = rectFromCenterPoint(top, pscale);
    OfxRectD bottomPoint = rectFromCenterPoint(bottom, pscale);
    Transform::Point3D transformedPos, rotationPos;
    transformedPos.x = penPosParam.x();
    transformedPos.y = penPosParam.y();
    transformedPos.z = 1.;

    double rot = Transform::toRadians(rotate);

    ///now undo skew + rotation to the current position
    Transform::Matrix3x3 rotation, transform;
    rotation = Transform::matInverseTransformCanonical(0., 0., 1., 1., 0., 0., false, rot, targetCenter.x, targetCenter.y);
    transform = Transform::matInverseTransformCanonical(0., 0., 1., 1., skewX, skewY, (bool)skewOrder, rot, targetCenter.x, targetCenter.y);

    rotationPos = Transform::matApply(rotation, transformedPos);
    if (rotationPos.z != 0) {
        rotationPos.x /= rotationPos.z;
        rotationPos.y /= rotationPos.z;
    }
    transformedPos = Transform::matApply(transform, transformedPos);
    if (transformedPos.z != 0) {
        transformedPos.x /= transformedPos.z;
        transformedPos.y /= transformedPos.z;
    }

    _orientation = TransformInteract::eOrientationAllDirections;

    double pressToleranceX = 5 * pscale.x;
    double pressToleranceY = 5 * pscale.y;
    bool didSomething = false;
    if ( squareContains(transformedPos, centerPoint, pressToleranceX, pressToleranceY) ) {
        _mouseState = _modifierStateCtrl ? TransformInteract::eDraggingCenter : TransformInteract::eDraggingTranslation;
        if (_modifierStateShift > 0) {
            _orientation = TransformInteract::eOrientationNotSet;
        }
        didSomething = true;
    } else if ( squareContains(transformedPos, leftPoint, pressToleranceX, pressToleranceY) ) {
        _mouseState = TransformInteract::eDraggingLeftPoint;
        didSomething = true;
    } else if ( squareContains(transformedPos, rightPoint, pressToleranceX, pressToleranceY) ) {
        _mouseState = TransformInteract::eDraggingRightPoint;
        didSomething = true;
    } else if ( squareContains(transformedPos, topPoint, pressToleranceX, pressToleranceY) ) {
        _mouseState = TransformInteract::eDraggingTopPoint;
        didSomething = true;
    } else if ( squareContains(transformedPos, bottomPoint, pressToleranceX, pressToleranceY) ) {
        _mouseState = TransformInteract::eDraggingBottomPoint;
        didSomething = true;
    } else if ( isOnEllipseBorder(transformedPos, targetRadius, targetCenter) ) {
        _mouseState = TransformInteract::eDraggingCircle;
        didSomething = true;
    } else if ( isOnRotationBar(rotationPos, targetRadius.x, targetCenter, pscale, pressToleranceY) ) {
        _mouseState = TransformInteract::eDraggingRotationBar;
        didSomething = true;
    } else if ( isOnSkewXBar(transformedPos, targetRadius.y, targetCenter, pscale, pressToleranceY) ) {
        _mouseState = TransformInteract::eDraggingSkewXBar;
        didSomething = true;
    } else if ( isOnSkewYBar(transformedPos, targetRadius.x, targetCenter, pscale, pressToleranceX) ) {
        _mouseState = TransformInteract::eDraggingSkewYBar;
        didSomething = true;
    } else {
        _mouseState = TransformInteract::eReleased;
    }

    _centerDrag = center;
    _translateDrag = translate;
    _scaleParamDrag = scaleParam;
    _scaleUniformDrag = scaleUniform;
    _rotateDrag = rotate;
    _skewXDrag = skewX;
    _skewYDrag = skewY;
    _skewOrderDrag = skewOrder;
    _invertedDrag = inverted;

    if (didSomething) {
        requestRedraw();
    }

    return didSomething;
} // TransformInteract::penDown

bool
CornerPinInteract::penDown(double time,
                           const RenderScale& /*renderScale*/,
                           ViewIdx /*view*/,
                           const OfxPointD& pscale,
                           const QPointF& /*lastPenPos*/,
                           const QPointF &penPos,
                           const QPoint & /*penPosViewport*/,
                           double /*pressure*/)
{
    // do not show interact if knob is secret or not enabled
    // see https://github.com/MrKepzie/Natron/issues/932
    KnobDoublePtr from1Knob = _from[0].lock();

    if ( !from1Knob || !from1Knob->shouldDrawOverlayInteract() ) {
        return false;
    }

    OfxPointD to[4];
    OfxPointD from[4];
    bool enable[4];
    bool useFrom;

    if (_dragging == -1) {
        for (int i = 0; i < 4; ++i) {
            getFrom(time, i, &from[i].x, &from[i].y);
            getTo(time, i, &to[i].x, &to[i].y);
            enable[i] = getEnabled(time, i);
        }
        useFrom = getUseFromPoints(time);
    } else {
        for (int i = 0; i < 4; ++i) {
            to[i] = _toDrag[i];
            from[i] = _fromDrag[i];
            enable[i] = _enableDrag[i];
        }
        _interactiveDrag = getInteractive();
        useFrom = _useFromDrag;
    }


    OfxPointD p[4];
    OfxPointD q[4];
    int enableBegin = 4;
    int enableEnd = 0;
    for (int i = 0; i < 4; ++i) {
        if (enable[i]) {
            if (useFrom) {
                p[i] = from[i];
                q[i] = to[i];
            } else {
                q[i] = from[i];
                p[i] = to[i];
            }
            if (i < enableBegin) {
                enableBegin = i;
            }
            if (i + 1 > enableEnd) {
                enableEnd = i + 1;
            }
        }
    }
    bool didSomething = false;

    for (int i = enableBegin; i < enableEnd; ++i) {
        if (enable[i]) {
            if ( CornerPinInteract::isNearby(penPos, p[i].x, p[i].y, CornerPinInteract::pointTolerance(), pscale) ) {
                _dragging = i;
                didSomething = true;
            }
            _toDrag[i] = to[i];
            _fromDrag[i] = from[i];
            _enableDrag[i] = enable[i];
        }
    }
    _useFromDrag = useFrom;


    return didSomething;
} // CornerPinInteract::penDown

bool
HostOverlay::penDown(double time,
                     const RenderScale& renderScale,
                     ViewIdx view,
                     const QPointF &penPos,
                     const QPoint &penPosViewport,
                     double pressure)
{
    OfxPointD pscale;

    n_getPixelScale(pscale.x, pscale.y);
    for (DefaultInteractIPtrList::iterator it = _imp->interacts.begin(); it != _imp->interacts.end(); ++it) {
        if ( (*it)->penDown(time, renderScale, view, pscale, _imp->lastPenPos, penPos, penPosViewport, pressure) ) {
            _imp->lastPenPos = penPos;

            return true;
        }
    }

    _imp->lastPenPos = penPos;

    return false;
}

bool
HostOverlay::penDoubleClicked(double time,
                              const RenderScale& renderScale,
                              ViewIdx view,
                              const QPointF &penPos,
                              const QPoint &penPosViewport)
{
    OfxPointD pscale;

    n_getPixelScale(pscale.x, pscale.y);
    for (DefaultInteractIPtrList::iterator it = _imp->interacts.begin(); it != _imp->interacts.end(); ++it) {
        if ( (*it)->penDoubleClicked(time, renderScale, view, pscale, _imp->lastPenPos, penPos, penPosViewport) ) {
            _imp->lastPenPos = penPos;

            return true;
        }
    }

    _imp->lastPenPos = penPos;

    return false;
}

bool
TransformInteract::keyDown(double /*time*/,
                           const RenderScale& /*renderScale*/,
                           ViewIdx /*view*/,
                           int key,
                           char*   /*keyString*/)
{
    // Always process, even if interact is not open, since this concerns modifiers
    //if (!_interactOpen->getValueAtTime(args.time)) {
    //    return false;
    //}

    // Note that on the Mac:
    // cmd/apple/cloverleaf is kOfxKey_Control_L
    // ctrl is kOfxKey_Meta_L
    // alt/option is kOfxKey_Alt_L
    bool mustRedraw = false;

    // the two control keys may be pressed consecutively, be aware about this
    if ( (key == kOfxKey_Control_L) || (key == kOfxKey_Control_R) ) {
        mustRedraw = _modifierStateCtrl == 0;
        ++_modifierStateCtrl;
    }
    if ( (key == kOfxKey_Shift_L) || (key == kOfxKey_Shift_R) ) {
        mustRedraw = _modifierStateShift == 0;
        ++_modifierStateShift;
        if (_modifierStateShift > 0) {
            _orientation = TransformInteract::eOrientationNotSet;
        }
    }
    if (mustRedraw) {
        requestRedraw();
    }
    //std::cout << std::hex << args.keySymbol << std::endl;

    // modifiers are not "caught"
    return false;
}

bool
HostOverlay::keyDown(double time,
                     const RenderScale& renderScale,
                     ViewIdx view,
                     int key,
                     char*   keyString)
{
    for (DefaultInteractIPtrList::iterator it = _imp->interacts.begin(); it != _imp->interacts.end(); ++it) {
        if ( (*it)->keyDown(time, renderScale, view, key, keyString) ) {
            return true;
        }
    }

    return false;
}

bool
TransformInteract::keyUp(double /*time*/,
                         const RenderScale& /*renderScale*/,
                         ViewIdx /*view*/,
                         int key,
                         char*   /*keyString*/)
{
    bool mustRedraw = false;

    if ( (key == kOfxKey_Control_L) || (key == kOfxKey_Control_R) ) {
        // we may have missed a keypress
        if (_modifierStateCtrl > 0) {
            --_modifierStateCtrl;
            mustRedraw = _modifierStateCtrl == 0;
        }
    }
    if ( (key == kOfxKey_Shift_L) || (key == kOfxKey_Shift_R) ) {
        if (_modifierStateShift > 0) {
            --_modifierStateShift;
            mustRedraw = _modifierStateShift == 0;
        }
        if (_modifierStateShift == 0) {
            _orientation = TransformInteract::eOrientationAllDirections;
        }
    }
    if (mustRedraw) {
        requestRedraw();
    }

    // modifiers are not "caught"
    return false;
}

bool
HostOverlay::keyUp(double time,
                   const RenderScale& renderScale,
                   ViewIdx view,
                   int key,
                   char*   keyString)
{
    bool didSomething = false;

    for (DefaultInteractIPtrList::iterator it = _imp->interacts.begin(); it != _imp->interacts.end(); ++it) {
        didSomething |= (*it)->keyUp(time, renderScale, view, key, keyString);
    }

    return didSomething;
}

bool
HostOverlay::keyRepeat(double time,
                       const RenderScale& renderScale,
                       ViewIdx view,
                       int key,
                       char*   keyString)
{
    bool didSomething = false;

    for (DefaultInteractIPtrList::iterator it = _imp->interacts.begin(); it != _imp->interacts.end(); ++it) {
        didSomething |= (*it)->keyRepeat(time, renderScale, view, key, keyString);
    }

    return didSomething;
}

bool
HostOverlay::gainFocus(double time,
                       const RenderScale& renderScale,
                       ViewIdx view)
{
    bool didSomething = false;

    for (DefaultInteractIPtrList::iterator it = _imp->interacts.begin(); it != _imp->interacts.end(); ++it) {
        didSomething |= (*it)->gainFocus(time, renderScale, view);
    }

    return didSomething;
}

bool
PositionInteract::loseFocus(double /*time*/,
                            const RenderScale & /*renderScale*/,
                            ViewIdx /*view*/)
{
    KnobDoublePtr knob = _param.lock();

    // do not show interact if knob is secret or not enabled
    // see https://github.com/MrKepzie/Natron/issues/932
    if ( !knob || !knob->shouldDrawOverlayInteract() ) {
        return false;
    }

    if (_state != ePositionInteractStateInactive) {
        _state = ePositionInteractStateInactive;
        // state changed, must redraw
        requestRedraw();
    }

    return false;
}

bool
TransformInteract::loseFocus(double /*time*/,
                             const RenderScale & /*renderScale*/,
                             ViewIdx /*view*/)
{
    // do not show interact if knob is secret or not enabled
    // see https://github.com/MrKepzie/Natron/issues/932
    KnobDoublePtr translateKnob = _translate.lock();

    if ( !translateKnob || !translateKnob->shouldDrawOverlayInteract() ) {
        return false;
    }
    // reset the modifiers state
    _modifierStateCtrl = 0;
    _modifierStateShift = 0;
    _interactiveDrag = false;
    _mouseState = TransformInteract::eReleased;
    _drawState = TransformInteract::eInActive;

    return false;
}

bool
CornerPinInteract::loseFocus(double /*time*/,
                             const RenderScale & /*renderScale*/,
                             ViewIdx /*view*/)
{
    _dragging = -1;
    _hovering = -1;
    _interactiveDrag = false;

    return false;
}

bool
HostOverlay::loseFocus(double time,
                       const RenderScale &renderScale,
                       ViewIdx view)
{
    bool didSomething = false;

    for (DefaultInteractIPtrList::iterator it = _imp->interacts.begin(); it != _imp->interacts.end(); ++it) {
        didSomething |= (*it)->loseFocus(time, renderScale, view);
    }

    return didSomething;
}

bool
HostOverlay::hasHostOverlayForParam(const KnobI* param)
{
    assert( QThread::currentThread() == qApp->thread() );
    for (DefaultInteractIPtrList::iterator it = _imp->interacts.begin(); it != _imp->interacts.end(); ++it) {
        if ( (*it)->isInteractForKnob(param) ) {
            return true;
        }
    }

    return false;
}

void
HostOverlay::removePositionHostOverlay(KnobI* knob)
{
    for (DefaultInteractIPtrList::iterator it = _imp->interacts.begin(); it != _imp->interacts.end(); ++it) {
        if ( (*it)->isInteractForKnob(knob) ) {
            _imp->interacts.erase(it);

            return;
        }
    }
}

bool
HostOverlay::isEmpty() const
{
    if ( _imp->interacts.empty() ) {
        return true;
    }

    return false;
}

NATRON_NAMESPACE_EXIT
