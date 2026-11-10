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

#ifndef Gui_SceneStageEditor_h
#define Gui_SceneStageEditor_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_ENTER

class SceneStage;

/**
 * @brief What a viewer adds to the stage it composes a payload on: layers of its own on top of the
 * payload, and in-place edits of layers no other stage composes. SceneViewportRenderer calls both
 * from its sync, with its stage lock held exclusively, on the thread syncing.
 **/
class SceneStageEditor {
public:
    virtual ~SceneStageEditor()
    {
    }

    // The stack the stage composes for @p payload: the payload itself when the editor adds nothing.
    virtual ScenePayload stagedPayload(const ScenePayload& payload) = 0;

    // Edits the layers the editor owns in the stage synced to stagedPayload(). True if any changed.
    virtual bool editStage(SceneStage& stage) = 0;

    // Whether what the editor adds is guide-purpose geometry the view must draw. Any thread.
    virtual bool showsGuidePurpose() const = 0;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_SceneStageEditor_h
