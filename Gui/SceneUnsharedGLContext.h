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

#ifndef NATRON_GUI_SCENEUNSHAREDGLCONTEXT_H
#define NATRON_GUI_SCENEUNSHAREDGLCONTEXT_H

#include "Global/Macros.h"

#include <memory>
#include <string>

NATRON_NAMESPACE_ENTER

/**
 * @brief A GLX context on an X connection of its own, sharing no objects with any other context.
 *
 * Qt's contexts all go through the application's one X connection, and a context made with Qt
 * either shares with the widget's or is created on that connection; this one is neither, so the
 * driver has no state in common between it and Qt's contexts. It draws into framebuffer objects
 * only: its own drawable is a 1x1 pbuffer.
 *
 * Created, made current, released and destroyed on one thread.
 **/
class SceneUnsharedGLContext {
public:
    struct Request {
        int majorVersion = 3;
        int minorVersion = 2;
        bool coreProfile = false;
        bool debug = false;
    };

    // Null, with the reason in error, when there is no X display or no context of the request.
    static std::unique_ptr<SceneUnsharedGLContext> create(const Request& request,
                                                          std::string* error);

    ~SceneUnsharedGLContext();

    SceneUnsharedGLContext(const SceneUnsharedGLContext&) = delete;
    SceneUnsharedGLContext& operator=(const SceneUnsharedGLContext&) = delete;

    bool makeCurrent();
    void doneCurrent();

private:
    struct Native;

    explicit SceneUnsharedGLContext(std::unique_ptr<Native> native);

    std::unique_ptr<Native> _native;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_SCENEUNSHAREDGLCONTEXT_H
