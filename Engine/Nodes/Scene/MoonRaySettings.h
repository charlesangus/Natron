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

#ifndef Engine_Nodes_Scene_MoonRaySettings_h
#define Engine_Nodes_Scene_MoonRaySettings_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/Nodes/Scene/RenderSettingsKnobs.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The MoonRay group of USDRender's settings. hdMoonray declares only its own options; the
 * sampling, depth and filter controls are MoonRay scene variables, which it takes as
 * "moonray:sceneVariable:<name>" settings without declaring them. MoonRay has no seed setting.
 **/
namespace MoonRaySettings {
const RenderSettingsKnobs::Curated& curated();

// The key of the samples per pixel, which a viewer's own sample count stands in for.
const char* samplesKey();
} // namespace MoonRaySettings

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_MoonRaySettings_h
