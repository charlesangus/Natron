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

#include "HydraGLContextQt.h"

#include <memory>
#include <string>

#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QSurfaceFormat>

#include "Engine/Nodes/Scene/HydraGLContext.h"

NATRON_NAMESPACE_ENTER

namespace {
class QtHydraGLContext
    : public HydraGLContext {
public:
    QtHydraGLContext(std::unique_ptr<QOpenGLContext> context,
                     QOffscreenSurface* surface)
        : _context(std::move(context))
        , _surface(surface)
    {
    }

    // The context lives on the render thread, which is never joined: it is not destroyed.
    virtual bool makeCurrent() OVERRIDE FINAL
    {
        return _context->makeCurrent(_surface);
    }

    virtual std::string description() const OVERRIDE FINAL
    {
        return _description;
    }

    std::string _description;

private:
    std::unique_ptr<QOpenGLContext> _context;
    QOffscreenSurface* _surface;
};

std::string
glString(QOpenGLFunctions* functions,
         GLenum name)
{
    const GLubyte* value = functions->glGetString(name);

    return value ? std::string(reinterpret_cast<const char*>(value)) : std::string();
}
} // namespace

namespace HydraGLContextQt {
void
install()
{
    if (!QOpenGLContext::supportsThreadedOpenGL()) {
        return;
    }
    // A surface is created on the main thread, and used from the render thread. It is never
    // destroyed, as the render thread may still hold it current while the application exits.
    QOffscreenSurface* surface = new QOffscreenSurface();
    surface->setFormat(QSurfaceFormat::defaultFormat());
    surface->create();
    if (!surface->isValid()) {
        delete surface;

        return;
    }

    setHydraGLContextFactory([surface](std::string* error) -> std::unique_ptr<HydraGLContext> {
        const std::string prefix = "USDRender needs an OpenGL 4.5 compatibility context and could not make one: ";
        std::unique_ptr<QOpenGLContext> context(new QOpenGLContext());
        context->setFormat(surface->requestedFormat());
        context->setShareContext(QOpenGLContext::globalShareContext());
        if (!context->create()) {
            *error = prefix + "Qt could not create an OpenGL context.";

            return std::unique_ptr<HydraGLContext>();
        }
        const QSurfaceFormat got = context->format();
        if ((got.majorVersion() < 4) || ((got.majorVersion() == 4) && (got.minorVersion() < 5)) || (got.profile() == QSurfaceFormat::CoreProfile)) {
            *error = prefix + "Qt gave OpenGL " + std::to_string(got.majorVersion()) + "." + std::to_string(got.minorVersion()) + ((got.profile() == QSurfaceFormat::CoreProfile) ? " core." : ".");

            return std::unique_ptr<HydraGLContext>();
        }
        if (!context->makeCurrent(surface)) {
            *error = prefix + "Qt could not make its OpenGL context current off the main thread.";

            return std::unique_ptr<HydraGLContext>();
        }
        QOpenGLFunctions* functions = context->functions();
        const std::string version = glString(functions, GL_VERSION);
        const std::string renderer = glString(functions, GL_RENDERER);
        std::unique_ptr<QtHydraGLContext> result(new QtHydraGLContext(std::move(context), surface));
        result->_description = "OpenGL " + version + " on " + renderer + " through Qt";

        return std::unique_ptr<HydraGLContext>(std::move(result));
    });
}
} // namespace HydraGLContextQt

NATRON_NAMESPACE_EXIT
