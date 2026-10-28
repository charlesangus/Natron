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

#ifndef Engine_Nodes_Scene_MoonRayRuntime_h
#define Engine_Nodes_Scene_MoonRayRuntime_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>

// Free of USD, so that AppManager.cpp, compiled under Py_LIMITED_API, can include it.

NATRON_NAMESPACE_ENTER

namespace MoonRayRuntime {
/**
 * @brief Makes the MoonRay install the process ships with usable: registers its Hydra plug-ins
 * with USD and points MoonRay's run-time search paths at it (RDL2_DSO_PATH for the scene object
 * DSOs, ARRAS_SESSION_PATH for the session definitions, PATH for the execComp render process).
 *
 * The install is the bundle's moonray/ next to applicationDirPath's parent, else the one the build
 * was configured with. USD reads PXR_PLUGINPATH_NAME once, when libusd_plug.so is loaded, so the
 * plug-ins are registered directly instead; call this before anything lists Hydra renderers. Does
 * nothing when there is no install, or when the CPU lacks the AVX2 code MoonRay is compiled to.
 * Returns the install used, empty if none.
 **/
std::string setUp(const std::string& applicationDirPath);
} // namespace MoonRayRuntime

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_MoonRayRuntime_h
