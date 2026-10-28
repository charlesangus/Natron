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

#ifndef Engine_Nodes_Scene_SceneCaches_h
#define Engine_Nodes_Scene_SceneCaches_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>

NATRON_NAMESPACE_ENTER

/**
 * @brief Entry points into the scene caches for code that cannot include pxr headers: the
 * project and AppManager are compiled under Py_LIMITED_API. Implemented beside the caches they
 * reach (SceneQueryPool.cpp, HydraRenderImpl.cpp, LiveTextureStore.cpp, LiveTextureResolver.cpp).
 **/
namespace SceneCaches {
/**
 * @brief Drops the idle stages of SceneQueryPool, and with them the layers only those stages
 * kept open. Borrowed stages are dropped when they come back past the pool's capacity. MT-safe.
 **/
void releaseIdleQueryStages();

/**
 * @brief Drops every USDRender node's stage and Storm engine, and with them the GPU memory they
 * hold; the next render of a node makes them again. Does not wait. MT-safe.
 **/
void releaseRenderEngines();

/**
 * @brief Drops every rendered live texture; the consumers that need one render it again. MT-safe.
 **/
void releaseLiveTextures();

/**
 * @brief Registers the natron:// asset resolver and image reader with OpenUSD, from the plugInfo.json
 * in Resources/usd/natronLiveTexture beside the executable. OpenUSD reads which resolvers and image
 * readers exist once, so this must run before anything asks it for one. Runs once; returns false,
 * with errorMessage set if not null, when the registration failed. MT-safe.
 **/
bool registerLiveTextures(std::string* errorMessage);
} // namespace SceneCaches

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_SceneCaches_h
