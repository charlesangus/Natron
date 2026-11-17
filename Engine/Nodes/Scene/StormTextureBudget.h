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
#include <string>

NATRON_NAMESPACE_ENTER

class GpuTextureBudget;

/**
 * @brief Keeps Storm's textures within the GPU's memory: a texture whose upload would take the
 * textures of every Storm scene past the budget is not loaded, and the material reads the texture
 * node's fallback value in its place. Nothing on disk or in the scene changes.
 *
 * The automatic budget admits a texture only while the card keeps a headroom free with it loaded,
 * by the card's free memory as last sampled; drivers that do not report free memory get half the
 * card's memory instead. A configured budget is a fixed cap on the textures alone.
 *
 * Storm offers no hook between reading a texture and uploading it, so a material filter decides,
 * from the file's header, what the upload will take, by Storm's own sizing: mip chains for UV and
 * UDIM textures, the packed pages for Ptex. A texture left out has its node taken out of the
 * network, which keeps Storm from opening the file at all.
 *
 * Under the automatic budget, an engine that streams its textures has them come in after its
 * geometry: its first frame reads every material before any geometry is on the card, so free
 * memory sampled before it would admit textures the geometry then has no room beside. A texture
 * that engine's materials first ask for is held, drawn with its fallback, and decided over the
 * following frames, a batch a frame, by free memory sampled after the frame that held it. A fixed
 * budget does not depend on what else is on the card, and decides each texture as it comes.
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

/**
 * @brief Samples what the GPU behind the current GL context has free, for the automatic budget.
 *
 * Call it on Storm's thread, with its context current, before each render: textures a render
 * admits are uploaded only once its sync is done, so a sample taken in between would count them
 * as neither free nor taken. Cheap enough for every frame.
 **/
void sampleFreeVideoMemory();

/**
 * @brief Has the Storm engine whose render index is renderInstanceId stream its textures, and
 * decides a batch of those it holds: the materials using the textures admitted are read again by
 * its next render. Call it on the engine's render thread, with its context current, after
 * sampleFreeVideoMemory() and before the first render of each frame; the first call must come
 * before the engine's first render. An unknown or empty id does nothing.
 **/
void streamTextures(const std::string& renderInstanceId);

// Whether that engine holds textures still to decide or materials still to read again.
bool texturesPending(const std::string& renderInstanceId);

GpuTextureBudget& budget();
} // namespace StormTextureBudget

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_StormTextureBudget_h
