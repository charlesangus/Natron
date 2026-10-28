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

#ifndef Engine_Nodes_Scene_RenderDelegates_h
#define Engine_Nodes_Scene_RenderDelegates_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <vector>

// Free of USD, so that node headers and test mains compiled under Py_LIMITED_API can include it.

// Storm's HdRendererPlugin type name, the renderer every consumer starts with.
#define kRenderDelegateStormId "HdStormRendererPlugin"

NATRON_NAMESPACE_ENTER

/**
 * @brief The Hydra render delegates the process can render with, as HdRendererPluginRegistry
 * discovers them from the plug-in paths.
 *
 * Storm draws through the GL render thread's context; every other delegate renders without GL
 * (UsdImagingGLEngine with gpuEnabled off) and its AOVs are read back from CPU buffers.
 **/
namespace RenderDelegates {
struct Delegate {
    // The plug-in's type name, which projects store.
    std::string id;
    // The name a menu shows: "Storm" for Storm, the plugInfo displayName otherwise.
    std::string displayName;
    bool available = false;
    // Why it is unavailable: the loader's error, or the delegate's own reason.
    std::string error;
};

bool isStorm(const std::string& id);

/**
 * @brief Every delegate the registry knows, Storm first and the others in the registry's
 * order. Each is loaded once, on the first call, to learn whether it can render here; later
 * calls return what that found, as the registry never discovers plug-ins again. MT-safe.
 **/
std::vector<Delegate> list();

// The delegate of id, false if the registry has none of that id.
bool find(const std::string& id,
          Delegate* delegate);

// The error a consumer shows when id cannot render: not installed, or unavailable and why.
std::string unavailableMessage(const std::string& id);

/**
 * @brief Adds the plug-ins of the plugInfo.json at path (a file, or a directory holding one) to
 * USD's plug-in registry. Only plug-ins registered before the first list() are seen.
 **/
void registerPluginDirectory(const std::string& path);
} // namespace RenderDelegates

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_RenderDelegates_h
