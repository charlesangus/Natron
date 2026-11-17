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

#ifndef Engine_Nodes_Scene_StormTextureBudget_h
#define Engine_Nodes_Scene_StormTextureBudget_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstdint>

NATRON_NAMESPACE_ENTER

class GpuTextureBudget;

/**
 * @brief Keeps Storm's textures within a share of the GPU's memory: a texture whose upload would
 * take the textures of every Storm scene past the budget is not loaded, and the material reads the
 * texture node's fallback value in its place. Nothing on disk or in the scene changes.
 *
 * Storm offers no hook between reading a texture and uploading it, so a material filter decides,
 * from the file's header, what the upload will take, by Storm's own sizing: mip chains for UV and
 * UDIM textures, the packed pages for Ptex. A texture left out has its node taken out of the
 * network, which keeps Storm from opening the file at all.
 *
 * Kept free of OpenUSD's headers so the settings can include it.
 **/
namespace StormTextureBudget {
// Puts the filter on every Storm render index made from now on. Once per process; MT-safe.
void registerForStorm();

// 0 chooses the budget from the memory of the GPU last seen by noteCurrentGLContext().
void setConfiguredBudgetMB(std::uint64_t megabytes);

/**
 * @brief Reads the memory of the GPU behind the current GL context, which must be the one Storm
 * draws with, and the budget the preferences ask for. Applies to textures claimed from then on.
 **/
void noteCurrentGLContext();

GpuTextureBudget& budget();
} // namespace StormTextureBudget

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_StormTextureBudget_h
