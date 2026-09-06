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

#define PLUGINID_NATRON_TYPEDPASSTHROUGH "fr.natron.TypedPassthrough"

NATRON_NAMESPACE_ENTER

/**
 * @brief A deliberately trivial NativeEffectBase subclass: a single-input,
 * single-output polymorphic pass-through, proving declaration, registration,
 * typed-IO and the createKnob() helper before any feature pressure arrives.
 * Its effective data kind resolves structurally from whatever feeds it,
 * exactly like Dot, and it renders as an identity of its input, exactly like
 * NoOpBase -- it does not invent a new pass-through mechanism.
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
    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool isIdentity(double time,
                            const RenderScale& scale,
                            const RectI& roi,
                            ViewIdx view,
                            double* inputTime,
                            ViewIdx* inputView,
                            int* inputNb) OVERRIDE FINAL WARN_UNUSED_RETURN;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_TypedPassthrough_h
