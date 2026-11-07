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

#ifndef Engine_Nodes_TypedPassthrough_h
#define Engine_Nodes_TypedPassthrough_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_TYPEDPASSTHROUGH "TypedPassthrough"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kTypedPassthroughInputs[] = {
    InputSpec { .label = "Source", .kind = eDataKindPolymorphic },
};

/**
 * @brief A single-input, single-output polymorphic pass-through: it forwards its
 * input unchanged, and its effective data kind is whatever feeds it. The forward
 * is a route() answer rather than a render() that copies pixels, so the
 * render path resolves straight to the input and the node costs nothing.
 **/
class TypedPassthrough
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new TypedPassthrough(node);
    }

    explicit TypedPassthrough(NodePtr node)
        : NativeEffectBase(node)
    {
    }

private:
    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_TYPEDPASSTHROUGH,
        .label = "TypedPassthrough",
        .description = "A trivial polymorphic pass-through node built on NativeEffectBase. "
                       "It is an identity of its single input, and its effective data kind "
                       "resolves structurally from whatever feeds it, exactly like Dot.",
        .grouping = PLUGIN_GROUP_OTHER,
        .majorVersion = 1,
        .minorVersion = 0,
        .outputKind = eDataKindPolymorphic,
        .inputs = kTypedPassthroughInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual Route route(double time,
                        ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void initializeKnobs() OVERRIDE FINAL;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_TypedPassthrough_h
