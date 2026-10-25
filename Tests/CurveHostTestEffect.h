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

#ifndef Tests_CurveHostTestEffect_h
#define Tests_CurveHostTestEffect_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <QPointF>

#include "Engine/EngineFwd.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/RectD.h"
#include "Engine/RotoCurveInteract.h"
#include "Engine/ViewIdx.h"

#define kTestPluginIDCurveHost "test.natron.built-in.CurveHost"

NATRON_NAMESPACE_ENTER

// The smallest geometry curve host: a node whose curve context holds shapes that no render tree
// draws, as SplineWarp's will. It hosts the curve tools in the viewer (no paint tools), has no
// input and renders nothing, so CurveHost_Test can check the curve machinery without RotoPaint.
class CurveHostTestEffect
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new CurveHostTestEffect(n);
    }

    explicit CurveHostTestEffect(NodePtr n)
        : NativeEffectBase(n)
        , _ui()
    {
    }

    virtual CurveContextKindEnum getCurveContextKind() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return eCurveContextKindGeometry;
    }

    const RotoCurveInteractPtr& getInteract() const
    {
        return _ui;
    }

private:
    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDCurveHost,
        .label = "Test Curve Host",
        .outputKind = eDataKindImage,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual void initializeKnobs() OVERRIDE FINAL
    {
        // The node's curve context exists by now (Node::createRotoContextConditionnally()).
        _ui = RotoCurveInteract::create(this);
        _ui->createToolbarKnobs(eRotoCurveToolSetAll);
    }

    virtual void onKnobsLoaded() OVERRIDE FINAL
    {
        KnobButtonPtr defaultAction = _ui->selectAllAction.lock();

        _ui->setCurrentTool(defaultAction);
        _ui->onRoleChangedInternal(_ui->selectToolGroup.lock());
        _ui->onToolChangedInternal(defaultAction);
    }

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum /*reason*/,
                             ViewSpec /*view*/,
                             double time,
                             bool /*originatedFromMainThread*/) OVERRIDE FINAL
    {
        bool ret = false;

        if (k && _ui->onKnobChanged(k, time, &ret)) {
            return ret;
        }

        return false;
    }

    virtual void drawOverlay(double time,
                             const RenderScale& /*renderScale*/,
                             ViewIdx /*view*/) OVERRIDE FINAL
    {
        _ui->drawOverlay(time);
    }

    virtual bool onOverlayPenDown(double time,
                                  const RenderScale& /*renderScale*/,
                                  ViewIdx /*view*/,
                                  const QPointF& /*viewportPos*/,
                                  const QPointF& pos,
                                  double pressure,
                                  double timestamp,
                                  PenType pen) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return _ui->onOverlayPenDown(time, pos, pressure, timestamp, pen);
    }

    virtual bool onOverlayPenMotion(double time,
                                    const RenderScale& /*renderScale*/,
                                    ViewIdx /*view*/,
                                    const QPointF& /*viewportPos*/,
                                    const QPointF& pos,
                                    double pressure,
                                    double timestamp) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return _ui->onOverlayPenMotion(time, pos, pressure, timestamp);
    }

    virtual bool onOverlayPenUp(double /*time*/,
                                const RenderScale& /*renderScale*/,
                                ViewIdx /*view*/,
                                const QPointF& /*viewportPos*/,
                                const QPointF& /*pos*/,
                                double /*pressure*/,
                                double /*timestamp*/) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return _ui->onOverlayPenUp();
    }

    virtual bool onOverlayPenDoubleClicked(double /*time*/,
                                           const RenderScale& /*renderScale*/,
                                           ViewIdx /*view*/,
                                           const QPointF& /*viewportPos*/,
                                           const QPointF& pos) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return _ui->onOverlayPenDoubleClicked(pos);
    }

    virtual bool onOverlayKeyDown(double /*time*/,
                                  const RenderScale& /*renderScale*/,
                                  ViewIdx /*view*/,
                                  Key key,
                                  KeyboardModifiers /*modifiers*/) OVERRIDE FINAL
    {
        return _ui->onOverlayKeyDown(key);
    }

    virtual bool onOverlayKeyUp(double /*time*/,
                                const RenderScale& /*renderScale*/,
                                ViewIdx /*view*/,
                                Key key,
                                KeyboardModifiers /*modifiers*/) OVERRIDE FINAL
    {
        return _ui->onOverlayKeyUp(key);
    }

    virtual bool onOverlayFocusLost(double /*time*/,
                                    const RenderScale& /*renderScale*/,
                                    ViewIdx /*view*/) OVERRIDE FINAL
    {
        return _ui->onOverlayFocusLost();
    }

    virtual void onInteractViewportSelectionCleared() OVERRIDE FINAL
    {
        _ui->onViewportSelectionCleared();
    }

    virtual void onInteractViewportSelectionUpdated(const RectD& rectangle,
                                                    bool onRelease) OVERRIDE FINAL
    {
        _ui->onViewportSelectionUpdated(rectangle, onRelease);
    }

    RotoCurveInteractPtr _ui;
};

NATRON_NAMESPACE_EXIT

#endif // Tests_CurveHostTestEffect_h
