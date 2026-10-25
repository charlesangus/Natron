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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "GuiGLRequirements.h"

#include <iostream>

#include <QList>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QtGlobal>

NATRON_NAMESPACE_ENTER

namespace GuiGLRequirements {
QString
profileName(QSurfaceFormat::OpenGLContextProfile profile)
{
    switch (profile) {
    case QSurfaceFormat::CompatibilityProfile:
        return QString::fromUtf8("compatibility profile");
    case QSurfaceFormat::CoreProfile:
        return QString::fromUtf8("core profile");
    case QSurfaceFormat::NoProfile:
        break;
    }

    return QString::fromUtf8("no profile");
}

Verdict
judge(const ContextInfo& info)
{
    Verdict verdict;
    const QString required = QString::fromUtf8("%1 needs an OpenGL %2.%3 compatibility-profile context")
                                 .arg(QString::fromUtf8(NATRON_APPLICATION_NAME))
                                 .arg(kRequiredMajorVersion)
                                 .arg(kRequiredMinorVersion);

    if (!info.created) {
        verdict.message = required + QString::fromUtf8(", but the graphics driver could not create one.");

        return verdict;
    }

    const bool versionOk = info.majorVersion > kRequiredMajorVersion || (info.majorVersion == kRequiredMajorVersion && info.minorVersion >= kRequiredMinorVersion);
    const bool profileOk = info.profile == QSurfaceFormat::CompatibilityProfile;
    if (versionOk && profileOk) {
        verdict.accepted = true;

        return verdict;
    }

    QString got = QString::fromUtf8(", but the graphics driver gave OpenGL %1.%2 (%3).")
                      .arg(info.majorVersion)
                      .arg(info.minorVersion)
                      .arg(profileName(info.profile));
    verdict.message = required + got;
    if (!info.versionString.isEmpty()) {
        verdict.message += QString::fromUtf8("\nGL_VERSION: ") + info.versionString;
    }
    if (!info.renderer.isEmpty()) {
        verdict.message += QString::fromUtf8("\nGL_RENDERER: ") + info.renderer;
    }
    verdict.message += QString::fromUtf8("\nUpdate the graphics driver, or run on a GPU whose driver supports OpenGL %1.%2 with the compatibility profile.")
                           .arg(kRequiredMajorVersion)
                           .arg(kRequiredMinorVersion);

    return verdict;
}

QSurfaceFormat
requestedFormat(const QSurfaceFormat& base,
                const QByteArray& versionOverride,
                const QByteArray& profileOverride,
                QString* warning)
{
    QSurfaceFormat format = base;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(kRequiredMajorVersion, kRequiredMinorVersion);
    format.setProfile(QSurfaceFormat::CompatibilityProfile);

    const QByteArray version = versionOverride.trimmed();
    if (!version.isEmpty()) {
        const QList<QByteArray> parts = version.split('.');
        bool majorOk = false;
        bool minorOk = false;
        const int major = parts.size() == 2 ? parts[0].toInt(&majorOk) : 0;
        const int minor = parts.size() == 2 ? parts[1].toInt(&minorOk) : 0;
        if (majorOk && minorOk && major > 0 && minor >= 0) {
            format.setVersion(major, minor);
        } else if (warning) {
            *warning += QString::fromUtf8("%1=%2 is not <major>.<minor>; ignored.\n")
                            .arg(QString::fromUtf8(kRequestVersionEnv))
                            .arg(QString::fromUtf8(versionOverride));
        }
    }

    const QByteArray profile = profileOverride.trimmed().toLower();
    if (profile == "core") {
        format.setProfile(QSurfaceFormat::CoreProfile);
    } else if (!profile.isEmpty() && profile != "compatibility") {
        if (warning) {
            *warning += QString::fromUtf8("%1=%2 is neither core nor compatibility; ignored.\n")
                            .arg(QString::fromUtf8(kRequestProfileEnv))
                            .arg(QString::fromUtf8(profileOverride));
        }
    }

    // Without DeprecatedFunctions Qt asks for a forward-compatible context from 3.0 up, and Mesa
    // answers a forward-compatible compatibility-profile request with a core-profile context.
    format.setOption(QSurfaceFormat::DeprecatedFunctions, format.profile() != QSurfaceFormat::CoreProfile);

    return format;
}

bool
isRequestOverridden()
{
    return !qEnvironmentVariableIsEmpty(kRequestVersionEnv) || !qEnvironmentVariableIsEmpty(kRequestProfileEnv);
}

bool
isRefusalDialogSuppressed()
{
    return isRequestOverridden() || !qEnvironmentVariableIsEmpty(kNoDialogEnv);
}

void
installDefaultFormat()
{
    QString warning;
    const QSurfaceFormat format = requestedFormat(QSurfaceFormat::defaultFormat(), qgetenv(kRequestVersionEnv), qgetenv(kRequestProfileEnv), &warning);
    if (!warning.isEmpty()) {
        std::cerr << warning.toStdString();
    }
    QSurfaceFormat::setDefaultFormat(format);
}

namespace {
    QString
    glString(QOpenGLFunctions* functions,
             GLenum name)
    {
        const GLubyte* value = functions->glGetString(name);

        return value ? QString::fromUtf8(reinterpret_cast<const char*>(value)) : QString();
    }
} // namespace

ContextInfo
probe()
{
    ContextInfo info;
    QOpenGLContext context;
    context.setFormat(QSurfaceFormat::defaultFormat());
    if (!context.create()) {
        return info;
    }
    info.created = true;

    QOffscreenSurface surface;
    surface.setFormat(context.format());
    surface.create();
    if (surface.isValid() && context.makeCurrent(&surface)) {
        QOpenGLFunctions* functions = context.functions();
        info.versionString = glString(functions, GL_VERSION);
        info.renderer = glString(functions, GL_RENDERER);
        context.doneCurrent();
    }

    const QSurfaceFormat got = context.format();
    info.majorVersion = got.majorVersion();
    info.minorVersion = got.minorVersion();
    info.profile = got.profile();

    return info;
}
} // namespace GuiGLRequirements

NATRON_NAMESPACE_EXIT
