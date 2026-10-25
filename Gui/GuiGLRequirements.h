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

#ifndef NATRON_GUI_GUIGLREQUIREMENTS_H
#define NATRON_GUI_GUIGLREQUIREMENTS_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <QByteArray>
#include <QString>
#include <QSurfaceFormat>

NATRON_NAMESPACE_ENTER

/**
 * @brief The GUI draws everything (viewer, overlays, OFX interacts, Hydra) on one OpenGL 4.5
 * compatibility-profile context.
 *
 * The requested version and profile can be forced for testing through the environment:
 * NATRON_GUI_GL_REQUEST_VERSION=<major>.<minor> and NATRON_GUI_GL_REQUEST_PROFILE=core|compatibility.
 * Mesa may still hand out a newer context than the one requested; MESA_GL_VERSION_OVERRIDE caps it.
 * When either variable is set, or NATRON_GUI_GL_NO_DIALOG is, a refused context is reported on
 * stderr only, without a dialog, so an unattended run cannot block on it.
 **/
namespace GuiGLRequirements {
const int kRequiredMajorVersion = 4;
const int kRequiredMinorVersion = 5;

const char* const kRequestVersionEnv = "NATRON_GUI_GL_REQUEST_VERSION";
const char* const kRequestProfileEnv = "NATRON_GUI_GL_REQUEST_PROFILE";
const char* const kNoDialogEnv = "NATRON_GUI_GL_NO_DIALOG";

struct ContextInfo {
    bool created = false;
    int majorVersion = 0;
    int minorVersion = 0;
    QSurfaceFormat::OpenGLContextProfile profile = QSurfaceFormat::NoProfile;
    QString versionString;
    QString renderer;
};

struct Verdict {
    bool accepted = false;
    QString message;
};

/**
 * @brief Accepts a context of version 4.5 or later with the compatibility profile; anything else is
 * refused with a message naming the version and profile that was obtained.
 **/
Verdict judge(const ContextInfo& info);

QString profileName(QSurfaceFormat::OpenGLContextProfile profile);

/**
 * @brief The format the GUI requests: @p base with an OpenGL 4.5 compatibility profile and
 * DeprecatedFunctions set (a compatibility profile is never forward-compatible), unless the
 * overrides (the values of the environment variables above, empty when unset) say otherwise. An
 * override that cannot be parsed is ignored and described in @p warning.
 **/
QSurfaceFormat requestedFormat(const QSurfaceFormat& base,
                               const QByteArray& versionOverride,
                               const QByteArray& profileOverride,
                               QString* warning);

bool isRequestOverridden();

/**
 * @brief True when a refused context must be reported on stderr only: the request is overridden
 * or NATRON_GUI_GL_NO_DIALOG is set.
 **/
bool isRefusalDialogSuppressed();

/**
 * @brief Makes requestedFormat() the default format of every QOpenGLWidget and QOpenGLContext.
 * Must run before the QApplication is constructed.
 **/
void installDefaultFormat();

/**
 * @brief Creates a context with the default format, makes it current on an offscreen surface and
 * reports what the platform gave. Needs the QApplication.
 **/
ContextInfo probe();
} // namespace GuiGLRequirements

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_GUIGLREQUIREMENTS_H
