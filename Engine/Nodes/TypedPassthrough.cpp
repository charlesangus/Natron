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

#include "TypedPassthrough.h"

#include "Engine/KnobTypes.h"

NATRON_NAMESPACE_ENTER

NativePluginDescription
TypedPassthrough::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_TYPEDPASSTHROUGH;
    desc.label = "TypedPassthrough";
    desc.description = tr("A trivial polymorphic pass-through node built on NativeEffectBase. "
                          "It is an identity of its single input, and its effective data kind "
                          "resolves structurally from whatever feeds it, exactly like Dot.")
                           .toStdString();
    desc.grouping = PLUGIN_GROUP_OTHER;
    desc.majorVersion = 1;
    desc.minorVersion = 0;
    desc.inputs.push_back(NativeInputDescription("Source", false, eDataKindPolymorphic));
    desc.outputKind = eDataKindPolymorphic;

    return desc;
}

void
TypedPassthrough::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));
    KnobStringPtr info = createKnob<KnobString>(tr("Info"));

    info->setAnimationEnabled(false);
    info->setAsLabel();
    info->setEvaluateOnChange(false);
    info->setValue(tr("Proof node for the native node framework. Has no effect on rendering.").toStdString());
    page->addKnob(info);
}

bool
TypedPassthrough::isIdentity(double time,
                             const RenderScale& /*scale*/,
                             const RectI& /*roi*/,
                             ViewIdx view,
                             double* inputTime,
                             ViewIdx* inputView,
                             int* inputNb)
{
    *inputTime = time;
    *inputNb = 0;
    *inputView = view;

    return true;
}

NATRON_NAMESPACE_EXIT
