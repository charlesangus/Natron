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

#ifndef NATRON_ENGINE_STARTUPGLDIAG_H
#define NATRON_ENGINE_STARTUPGLDIAG_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

NATRON_NAMESPACE_ENTER

/*
 * GL context diagnostics for startup, active only when NATRON_DIAG_GL=1.
 * Every entry point is a no-op otherwise. Output goes to stderr, one line per
 * event, each prefixed with "[natron-gl-diag]".
 */
namespace StartupGLDiag {

bool enabled();

struct GLXBinding {
    const void* context = nullptr;
    unsigned long drawable = 0;
};

enum class QtNativeKind {
    None,
    GLX,
    EGL,
    Unknown
};

struct QtGLState {
    const void* qtContext = nullptr;
    QtNativeKind nativeKind = QtNativeKind::None;
    const void* nativeContext = nullptr;
};

// Engine does not link QtGui, so the Gui layer supplies Qt's view of the
// calling thread's current context through this hook.
using QtGLStateQuery = QtGLState (*)();

void setQtGLStateQuery(QtGLStateQuery query);

// Chains to the previous handler, then dumps GL and cache state once on the
// first Qt shader-compile or pipeline-build failure.
void installMessageHandler();

GLXBinding currentGLXBinding();

void logMakeCurrent(const char* tag,
                    const void* caller,
                    const GLXBinding& target,
                    const GLXBinding& before,
                    bool ok);

// Compares Qt's current context with the GLX/EGL one on the calling thread
// and flags MISMATCH when they disagree.
void checkpoint(const char* name);

} // namespace StartupGLDiag

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_STARTUPGLDIAG_H
