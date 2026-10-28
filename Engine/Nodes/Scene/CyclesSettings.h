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

#ifndef Engine_Nodes_Scene_CyclesSettings_h
#define Engine_Nodes_Scene_CyclesSettings_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/Nodes/Scene/RenderSettingsKnobs.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The Cycles group of USDRender's settings: the controls a Cycles user reaches for first,
 * bound to the keys Cycles' Hydra delegate declares. Cycles declares no pixel filter: its filter
 * belongs to the film, which the delegate does not expose.
 **/
namespace CyclesSettings {
const RenderSettingsKnobs::Curated& curated();

// The key of the samples per pixel, which a viewer's own sample count stands in for.
const char* samplesKey();
} // namespace CyclesSettings

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_CyclesSettings_h
