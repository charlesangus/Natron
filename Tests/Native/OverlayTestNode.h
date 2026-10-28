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

#ifndef Tests_Native_OverlayTestNode_h
#define Tests_Native_OverlayTestNode_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define kTestPluginIDOverlay "test.natron.built-in.Overlay"
#define kOverlayTestParamCenter "center"

NATRON_NAMESPACE_ENTER

/**
 * @brief A pass-through node whose overlay draw list holds one of every primitive type around
 * its center knob, with a square handle on the center that drags the knob.
 **/
class OverlayTestNode
    : public NativeEffectBase {
public:
    enum HandleEnum {
        eHandleCenter = 0,
        eHandleMarker = 1,
    };

    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new OverlayTestNode(n);
    }

    explicit OverlayTestNode(NodePtr n);

    virtual ~OverlayTestNode();

    KnobDoublePtr getCenterKnob() const;

    virtual bool hasOverlayDrawList() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

protected:
    virtual void buildOverlay(double time, const RenderScale& renderScale, ViewIdx view, OverlayDrawList* list) OVERRIDE FINAL;
    virtual bool onOverlayHandlePressed(int id, double time, ViewIdx view, const QPointF& pos) OVERRIDE FINAL;
    virtual void onOverlayHandleDragged(int id, double time, ViewIdx view, const QPointF& pos, const QPointF& delta) OVERRIDE FINAL;

private:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindImage, .optional = true },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDOverlay,
        .label = "Test Overlay",
        .grouping = PLUGIN_GROUP_OTHER,
        .outputKind = eDataKindImage,
        .inputs = kInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual Route route(double time, ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;

    KnobDoubleWPtr _center;
};

NATRON_NAMESPACE_EXIT

#endif // Tests_Native_OverlayTestNode_h
