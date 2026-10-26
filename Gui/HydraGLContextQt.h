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

#ifndef Gui_HydraGLContextQt_h
#define Gui_HydraGLContextQt_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

NATRON_NAMESPACE_ENTER

namespace HydraGLContextQt {
/**
 * @brief Has USDRender's renders run on a Qt OpenGL context of the GUI's format, shared with Qt's
 * global share context when the application has one, instead of the headless EGL context they
 * make without a GUI. Call on the main thread once the GUI's OpenGL was accepted. Does nothing
 * where Qt cannot make a context current off the main thread, leaving the EGL context in place.
 **/
void install();
} // namespace HydraGLContextQt

NATRON_NAMESPACE_EXIT

#endif // Gui_HydraGLContextQt_h
