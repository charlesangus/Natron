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

#ifndef NATRON_GUI_GLCONTEXTLOSS_H
#define NATRON_GUI_GLCONTEXTLOSS_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>

class QOpenGLContext;
class QOpenGLWidget;

NATRON_NAMESPACE_ENTER

/**
 * @brief Releases the GL objects a QOpenGLWidget created in its context before that context dies.
 *
 * QOpenGLWidget destroys its context whenever the widget moves to another top-level window (a pane
 * floated out or docked back) and calls initializeGL() again in a new one. Names created in the old
 * context are meaningless in the new one: binding them samples a foreign object and deleting them
 * deletes someone else's.
 **/
namespace GLContextLoss {
/**
 * @brief Runs release with the widget's context current when that context is about to be destroyed,
 * then makes current again whatever context was current before. Call it from initializeGL(); a later
 * call replaces the earlier release and connection. Does nothing while the widget has no context.
 **/
void watch(QOpenGLWidget* widget, std::function<void()> release);

/**
 * @brief Runs the watched release now, with the widget's context current, and stops watching. Call it
 * from the widget's destructor, before the members release touches are gone. Does nothing if
 * initializeGL() never ran or the context was already released.
 **/
void releaseNow(QOpenGLWidget* widget);

/**
 * @brief Runs fn with context current, on an offscreen surface when the context is not already current,
 * and makes current again whatever was current before. Returns false without running fn when the
 * context cannot be made current.
 **/
bool runWithContextCurrent(QOpenGLContext* context, const std::function<void()>& fn);
} // namespace GLContextLoss

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_GLCONTEXTLOSS_H
