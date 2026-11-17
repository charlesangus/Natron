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

#include "Gui/SceneUnsharedGLContext.h"

#include <atomic>
#include <vector>

// Last: Xlib defines None, Bool, Status and others as macros.
#include <GL/glx.h>
#include <X11/Xlib.h>

NATRON_NAMESPACE_ENTER

namespace {
using CreateContextAttribsFn = GLXContext (*)(Display*, GLXFBConfig, GLXContext, Bool, const int*);

std::atomic<bool> xErrorSeen { false };

int
noteXError(Display*,
           XErrorEvent*)
{
    xErrorSeen = true;

    return 0;
}
} // anonymous namespace

struct SceneUnsharedGLContext::Native {
    Display* display = nullptr;
    GLXPbuffer pbuffer = 0;
    GLXContext context = nullptr;

    ~Native()
    {
        if (!display) {
            return;
        }
        if (context) {
            if (glXGetCurrentContext() == context) {
                glXMakeContextCurrent(display, None, None, nullptr);
            }
            glXDestroyContext(display, context);
        }
        if (pbuffer) {
            glXDestroyPbuffer(display, pbuffer);
        }
        XCloseDisplay(display);
    }
};

SceneUnsharedGLContext::SceneUnsharedGLContext(std::unique_ptr<Native> native)
    : _native(std::move(native))
{
}

SceneUnsharedGLContext::~SceneUnsharedGLContext() = default;

std::unique_ptr<SceneUnsharedGLContext>
SceneUnsharedGLContext::create(const Request& request,
                               std::string* error)
{
    const auto fail = [error](const char* reason) {
        if (error) {
            *error = reason;
        }

        return std::unique_ptr<SceneUnsharedGLContext>();
    };
    std::unique_ptr<Native> native(new Native);

    // The display Qt connected to: Natron is never given another than $DISPLAY.
    native->display = XOpenDisplay(nullptr);
    if (!native->display) {
        return fail("cannot open the X display");
    }
    const int fbAttributes[] = { GLX_DRAWABLE_TYPE, GLX_PBUFFER_BIT, GLX_RENDER_TYPE, GLX_RGBA_BIT, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, GLX_ALPHA_SIZE, 8, None };
    int count = 0;
    GLXFBConfig* configs = glXChooseFBConfig(native->display, DefaultScreen(native->display), fbAttributes, &count);
    if (!configs || (count <= 0)) {
        if (configs) {
            XFree(configs);
        }

        return fail("no GLX framebuffer configuration with pbuffers");
    }
    const GLXFBConfig config = configs[0];
    XFree(configs);

    const CreateContextAttribsFn createContextAttribs = reinterpret_cast<CreateContextAttribsFn>(glXGetProcAddressARB(reinterpret_cast<const GLubyte*>("glXCreateContextAttribsARB")));
    if (!createContextAttribs) {
        return fail("no glXCreateContextAttribsARB");
    }
    std::vector<int> attributes { GLX_CONTEXT_MAJOR_VERSION_ARB, request.majorVersion, GLX_CONTEXT_MINOR_VERSION_ARB, request.minorVersion };
    if ((request.majorVersion > 3) || ((request.majorVersion == 3) && (request.minorVersion >= 2))) {
        attributes.insert(attributes.end(), { GLX_CONTEXT_PROFILE_MASK_ARB, request.coreProfile ? GLX_CONTEXT_CORE_PROFILE_BIT_ARB : GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB });
    }
    if (request.debug) {
        attributes.insert(attributes.end(), { GLX_CONTEXT_FLAGS_ARB, GLX_CONTEXT_DEBUG_BIT_ARB });
    }
    attributes.push_back(None);

    // A refused request is an X protocol error, which Xlib's default handler answers by exiting the
    // process. The handler is process-wide, but Qt's own X traffic goes through xcb, not Xlib's
    // error handlers.
    xErrorSeen = false;
    const XErrorHandler previous = XSetErrorHandler(noteXError);
    native->context = createContextAttribs(native->display, config, nullptr, True, attributes.data());
    XSync(native->display, False);
    XSetErrorHandler(previous);
    if (!native->context || xErrorSeen) {
        return fail("the driver refused a context of the version and profile asked for");
    }
    if (!glXIsDirect(native->display, native->context)) {
        return fail("only an indirect context is available");
    }
    const int pbufferAttributes[] = { GLX_PBUFFER_WIDTH, 1, GLX_PBUFFER_HEIGHT, 1, None };
    native->pbuffer = glXCreatePbuffer(native->display, config, pbufferAttributes);
    if (!native->pbuffer) {
        return fail("cannot create a pbuffer");
    }

    return std::unique_ptr<SceneUnsharedGLContext>(new SceneUnsharedGLContext(std::move(native)));
} // SceneUnsharedGLContext::create

bool
SceneUnsharedGLContext::makeCurrent()
{
    return glXMakeContextCurrent(_native->display, _native->pbuffer, _native->pbuffer, _native->context) == True;
}

void
SceneUnsharedGLContext::doneCurrent()
{
    glXMakeContextCurrent(_native->display, None, None, nullptr);
}

NATRON_NAMESPACE_EXIT
