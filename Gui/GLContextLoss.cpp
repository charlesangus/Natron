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

#include "GLContextLoss.h"

#include <unordered_map>
#include <utility>

#include "Global/GLIncludes.h" //!<must be included before QOpenGLWidget because of gl.h and glew.h

#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLWidget>
#include <QPointer>
#include <QThread>
#include <QtCore/QCoreApplication>

NATRON_NAMESPACE_ENTER

NATRON_NAMESPACE_ANONYMOUS_ENTER

struct Watch {
    std::function<void()> release;
    QMetaObject::Connection contextConnection;
    QMetaObject::Connection destroyedConnection;
};

// GL widgets live on the main thread only, so the map needs no lock.
std::unordered_map<QOpenGLWidget*, Watch>&
watches()
{
    static std::unordered_map<QOpenGLWidget*, Watch> map;

    return map;
}

// Makes current again, on scope exit, the context and surface that were current on entry, or
// nothing if nothing was. Qt skips its own make-current calls while its record of the current
// context matches, so a release that leaves another context bound breaks the next composite.
class CurrentContextRestore {
public:
    CurrentContextRestore()
        : _context(QOpenGLContext::currentContext())
        , _surface(_context ? _context->surface() : nullptr)
    {
    }

    ~CurrentContextRestore()
    {
        QOpenGLContext* now = QOpenGLContext::currentContext();

        if (_context && (now == _context.data()) && (now->surface() == _surface)) {
            return;
        }
        if (_context && _surface) {
            _context->makeCurrent(_surface);
        } else if (now) {
            now->doneCurrent();
        }
    }

    CurrentContextRestore(const CurrentContextRestore&) = delete;
    CurrentContextRestore& operator=(const CurrentContextRestore&) = delete;

private:
    QPointer<QOpenGLContext> _context;
    QSurface* _surface;
};

void
forget(QOpenGLWidget* widget)
{
    std::unordered_map<QOpenGLWidget*, Watch>& map = watches();
    std::unordered_map<QOpenGLWidget*, Watch>::iterator found = map.find(widget);

    if (found == map.end()) {
        return;
    }
    QObject::disconnect(found->second.contextConnection);
    QObject::disconnect(found->second.destroyedConnection);
    map.erase(found);
}

// Takes the widget's release out of the map before running it, so a release that ends up in
// watch() or releaseNow() again finds a consistent map.
void
releaseWatched(QOpenGLWidget* widget)
{
    std::unordered_map<QOpenGLWidget*, Watch>& map = watches();
    std::unordered_map<QOpenGLWidget*, Watch>::iterator found = map.find(widget);

    if (found == map.end()) {
        return;
    }
    std::function<void()> release = std::move(found->second.release);
    forget(widget);

    QOpenGLContext* context = widget->context();
    if (!context || !release) {
        return;
    }
    CurrentContextRestore restore;
    // QOpenGLWidget keeps the dying context usable by makeCurrent() while it emits aboutToBeDestroyed.
    widget->makeCurrent();
    if (QOpenGLContext::currentContext() != context) {
        return;
    }
    release();
}

NATRON_NAMESPACE_ANONYMOUS_EXIT

namespace GLContextLoss {
void
watch(QOpenGLWidget* widget,
      std::function<void()> release)
{
    assert(qApp && qApp->thread() == QThread::currentThread());
    forget(widget);
    QOpenGLContext* context = widget->context();
    if (!context) {
        return;
    }
    Watch& entry = watches()[widget];
    entry.release = std::move(release);
    // The handler has to run before the context goes, so the connection must be direct.
    entry.contextConnection = QObject::connect(context, &QOpenGLContext::aboutToBeDestroyed, widget, [widget]() { releaseWatched(widget); }, Qt::DirectConnection);
    // A widget whose destructor forgot releaseNow() must not leave a dangling key behind for the
    // next widget allocated at the same address.
    entry.destroyedConnection = QObject::connect(widget, &QObject::destroyed, [widget]() {
        forget(widget);
    });
}

void
releaseNow(QOpenGLWidget* widget)
{
    assert(qApp && qApp->thread() == QThread::currentThread());
    releaseWatched(widget);
}

bool
runWithContextCurrent(QOpenGLContext* context,
                      const std::function<void()>& fn)
{
    if (!context) {
        return false;
    }
    if (QOpenGLContext::currentContext() == context) {
        fn();

        return true;
    }
    // The surface outlives the restore below, which has to switch away from it first.
    QOffscreenSurface surface(context->screen());
    surface.setFormat(context->format());
    surface.create();
    CurrentContextRestore restore;
    if (!surface.isValid() || !context->makeCurrent(&surface)) {
        return false;
    }
    fn();

    return true;
}
} // namespace GLContextLoss

NATRON_NAMESPACE_EXIT
