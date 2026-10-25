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

#ifndef Engine_Nodes_Scene_HeadlessGLContext_h
#define Engine_Nodes_Scene_HeadlessGLContext_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <string>

#include "Engine/Nodes/Scene/HydraGLContext.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief An OpenGL 4.5 compatibility context with no window system: EGL, loaded at run time so
 * that nothing links it, on Mesa's surfaceless platform or, failing that, on the first EGL device
 * that gives one. Works on any driver, llvmpipe included, with no X server. What USDRender renders
 * on when no GUI installed a context factory (NatronRenderer, the tests).
 **/
class HeadlessGLContext
    : public HydraGLContext {
public:
    /**
     * @brief Loads eglLibrary, creates the context and makes it current on the calling thread.
     * Null if any step fails, with *error naming it: no EGL library, no platform without a
     * window system, or a GL version or profile short of 4.5 compatibility (naming what EGL gave).
     **/
    static std::unique_ptr<HeadlessGLContext> create(std::string* error,
                                                     const char* eglLibrary = "libEGL.so.1");

    virtual ~HeadlessGLContext();

    virtual bool makeCurrent() OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual std::string description() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void* getProcAddress(const char* name) const OVERRIDE FINAL WARN_UNUSED_RETURN;

private:
    struct Implementation;

    explicit HeadlessGLContext(std::unique_ptr<Implementation> imp);

    std::unique_ptr<Implementation> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_HeadlessGLContext_h
