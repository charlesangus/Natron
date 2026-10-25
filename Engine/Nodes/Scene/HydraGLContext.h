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

#ifndef Engine_Nodes_Scene_HydraGLContext_h
#define Engine_Nodes_Scene_HydraGLContext_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <memory>
#include <string>

#include "Engine/EngineFwd.h"

// Free of USD and of any GL header: Gui includes it to install its context factory.

NATRON_NAMESPACE_ENTER

/**
 * @brief The OpenGL context USDRender's Storm renders run on. HydraRenderImpl creates one, on its
 * render thread, and only ever makes it current on that thread. It must be a 4.5 compatibility
 * context, which is what Storm on HgiGL needs.
 **/
class HydraGLContext {
public:
    virtual ~HydraGLContext() { }

    // Makes the context current on the calling thread. False if it could not be.
    virtual bool makeCurrent() = 0;

    // What the context is, for messages: its GL version and renderer and how it was made.
    virtual std::string description() const = 0;
};

/**
 * @brief Creates the context for USDRender's renders, called once on the render thread. Returns
 * null with *error set to a message naming what was missing, such as the GL version it got.
 **/
typedef std::function<std::unique_ptr<HydraGLContext>(std::string* error)> HydraGLContextFactory;

/**
 * @brief Replaces how USDRender's context is made. The GUI installs one making a context shared
 * with its own; without one, USDRender makes a headless EGL context (HeadlessGLContext). Must be
 * called before the first USDRender render: the context is made once, on the first render.
 **/
void setHydraGLContextFactory(const HydraGLContextFactory& factory);

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_HydraGLContext_h
