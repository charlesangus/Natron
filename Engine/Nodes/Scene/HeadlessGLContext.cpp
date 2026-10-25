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

#include "HeadlessGLContext.h"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <type_traits>
#include <vector>

#include <dlfcn.h>

// Only EGL's own types: no X11 header, whose macros (Bool, None, Status) break other headers.
#ifndef EGL_NO_X11
#define EGL_NO_X11 1
#endif
#ifndef MESA_EGL_NO_X11_HEADERS
#define MESA_EGL_NO_X11_HEADERS 1
#endif
#include <EGL/egl.h>
#include <EGL/eglext.h>

NATRON_NAMESPACE_ENTER

namespace {
// GL enums used here, so that this file needs no GL header.
const unsigned int kGlVendor = 0x1F00;
const unsigned int kGlRenderer = 0x1F01;
const unsigned int kGlVersion = 0x1F02;
const unsigned int kGlMajorVersion = 0x821B;
const unsigned int kGlMinorVersion = 0x821C;
const unsigned int kGlContextProfileMask = 0x9126;
const int kGlCompatibilityProfileBit = 0x00000002;

typedef const unsigned char* (*GetStringFn)(unsigned int);
typedef void (*GetIntegervFn)(unsigned int, int*);

bool
hasExtension(const char* list,
             const char* name)
{
    if (!list) {
        return false;
    }
    const std::size_t length = std::strlen(name);
    for (const char* at = std::strstr(list, name); at; at = std::strstr(at + length, name)) {
        const bool startsWord = (at == list) || (at[-1] == ' ');
        const bool endsWord = (at[length] == '\0') || (at[length] == ' ');
        if (startsWord && endsWord) {
            return true;
        }
    }

    return false;
}

std::string
hexCode(EGLint code)
{
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%04x", (unsigned int)code);

    return buffer;
}
} // anon namespace

struct HeadlessGLContext::Implementation {
    // libEGL stays loaded for the life of the process: its drivers register exit handlers.
    void* library = nullptr;
    decltype(&::eglGetProcAddress) getProcAddress = nullptr;
    decltype(&::eglQueryString) queryString = nullptr;
    decltype(&::eglInitialize) initialize = nullptr;
    decltype(&::eglBindAPI) bindApi = nullptr;
    decltype(&::eglChooseConfig) chooseConfig = nullptr;
    decltype(&::eglCreateContext) createContext = nullptr;
    decltype(&::eglDestroyContext) destroyContext = nullptr;
    decltype(&::eglMakeCurrent) makeCurrent = nullptr;
    decltype(&::eglGetCurrentContext) getCurrentContext = nullptr;
    decltype(&::eglGetError) getError = nullptr;

    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    std::string description;

    bool loadEntryPoints()
    {
        auto load = [this](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(::dlsym(library, name));

            return fn != nullptr;
        };

        return load(getProcAddress, "eglGetProcAddress") && load(queryString, "eglQueryString") && load(initialize, "eglInitialize") && load(bindApi, "eglBindAPI") && load(chooseConfig, "eglChooseConfig") && load(createContext, "eglCreateContext") && load(destroyContext, "eglDestroyContext") && load(makeCurrent, "eglMakeCurrent") && load(getCurrentContext, "eglGetCurrentContext") && load(getError, "eglGetError");
    }

    // The GL version and profile of the context current on this thread, and its description.
    void queryGL(int* major,
                 int* minor,
                 bool* compatibility,
                 std::string* text) const
    {
        GetStringFn getString = reinterpret_cast<GetStringFn>(getProcAddress("glGetString"));
        GetIntegervFn getIntegerv = reinterpret_cast<GetIntegervFn>(getProcAddress("glGetIntegerv"));
        *major = 0;
        *minor = 0;
        *compatibility = false;
        if (!getString || !getIntegerv) {
            *text = "an OpenGL context without glGetString";

            return;
        }
        getIntegerv(kGlMajorVersion, major);
        getIntegerv(kGlMinorVersion, minor);
        int profile = 0;
        getIntegerv(kGlContextProfileMask, &profile);
        // Before 3.2 there are no profiles: everything is compatibility.
        *compatibility = ((*major < 3) || ((*major == 3) && (*minor < 2))) || (profile & kGlCompatibilityProfileBit);
        const unsigned char* version = getString(kGlVersion);
        const unsigned char* renderer = getString(kGlRenderer);
        const unsigned char* vendor = getString(kGlVendor);
        std::ostringstream os;
        os << "OpenGL " << (version ? (const char*)version : "?") << " on " << (renderer ? (const char*)renderer : "?") << " (" << (vendor ? (const char*)vendor : "?") << ")";
        *text = os.str();
    }

    // Tries one display. On success the context is current; otherwise *error says why.
    bool tryDisplay(EGLDisplay dpy,
                    const std::string& platform,
                    std::string* error)
    {
        if ((dpy == EGL_NO_DISPLAY) || !initialize(dpy, nullptr, nullptr)) {
            *error = platform + ": eglInitialize failed (" + hexCode(getError()) + ")";

            return false;
        }
        const char* extensions = queryString(dpy, EGL_EXTENSIONS);
        if (!hasExtension(extensions, "EGL_KHR_surfaceless_context")) {
            *error = platform + ": no EGL_KHR_surfaceless_context";

            return false;
        }
        EGLConfig config = EGL_NO_CONFIG_KHR;
        if (!hasExtension(extensions, "EGL_KHR_no_config_context")) {
            const EGLint configAttributes[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE };
            EGLint count = 0;
            if (!chooseConfig(dpy, configAttributes, &config, 1, &count) || (count < 1)) {
                *error = platform + ": no EGL config renders desktop OpenGL";

                return false;
            }
        }
        if (!bindApi(EGL_OPENGL_API)) {
            *error = platform + ": no desktop OpenGL through EGL";

            return false;
        }
        const EGLint contextAttributes[] = {
            EGL_CONTEXT_MAJOR_VERSION, 4,
            EGL_CONTEXT_MINOR_VERSION, 5,
            EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT,
            EGL_NONE
        };
        EGLContext ctx = createContext(dpy, config, EGL_NO_CONTEXT, contextAttributes);
        if (ctx == EGL_NO_CONTEXT) {
            const EGLint code = getError();
            // What the driver gives instead, to name it.
            std::string got = "no OpenGL context at all";
            EGLContext fallback = createContext(dpy, config, EGL_NO_CONTEXT, nullptr);
            if (fallback != EGL_NO_CONTEXT) {
                if (makeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, fallback)) {
                    int major, minor;
                    bool compatibility;
                    queryGL(&major, &minor, &compatibility, &got);
                    makeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                }
                destroyContext(dpy, fallback);
            }
            *error = platform + ": no OpenGL 4.5 compatibility context (" + hexCode(code) + "); it gives " + got;

            return false;
        }
        if (!makeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
            *error = platform + ": eglMakeCurrent failed (" + hexCode(getError()) + ")";
            destroyContext(dpy, ctx);

            return false;
        }
        int major, minor;
        bool compatibility;
        std::string got;
        queryGL(&major, &minor, &compatibility, &got);
        if ((major < 4) || ((major == 4) && (minor < 5)) || !compatibility) {
            *error = platform + ": needs an OpenGL 4.5 compatibility context and got " + got + (compatibility ? "" : ", core profile");
            makeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            destroyContext(dpy, ctx);

            return false;
        }
        display = dpy;
        context = ctx;
        description = got + " through EGL " + platform;

        return true;
    }
};

HeadlessGLContext::HeadlessGLContext(std::unique_ptr<Implementation> imp)
    : _imp(std::move(imp))
{
}

HeadlessGLContext::~HeadlessGLContext()
{
    if (_imp->context == EGL_NO_CONTEXT) {
        return;
    }
    if (_imp->getCurrentContext() == _imp->context) {
        _imp->makeCurrent(_imp->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    _imp->destroyContext(_imp->display, _imp->context);
}

std::unique_ptr<HeadlessGLContext>
HeadlessGLContext::create(std::string* error,
                          const char* eglLibrary)
{
    const std::string prefix = "USDRender needs an OpenGL 4.5 compatibility context and could not make one without a window system: ";
    std::unique_ptr<Implementation> imp(new Implementation);

    imp->library = ::dlopen(eglLibrary, RTLD_NOW | RTLD_LOCAL);
    if (!imp->library) {
        const char* why = ::dlerror();
        *error = prefix + "no EGL library (" + (why ? why : eglLibrary) + ").";

        return std::unique_ptr<HeadlessGLContext>();
    }
    if (!imp->loadEntryPoints()) {
        *error = prefix + std::string(eglLibrary) + " lacks EGL 1.4's entry points.";

        return std::unique_ptr<HeadlessGLContext>();
    }

    const char* clientExtensions = imp->queryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(imp->getProcAddress("eglGetPlatformDisplayEXT"));
    std::vector<std::string> failures;
    if (!getPlatformDisplay || !hasExtension(clientExtensions, "EGL_EXT_platform_base")) {
        failures.push_back("EGL has no platform displays (EGL_EXT_platform_base)");
    } else {
        std::string failure;
        // Mesa's platform needs no device at all, and is where llvmpipe lives.
        if (hasExtension(clientExtensions, "EGL_MESA_platform_surfaceless")) {
            EGLDisplay dpy = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
            if (imp->tryDisplay(dpy, "surfaceless platform", &failure)) {
                return std::unique_ptr<HeadlessGLContext>(new HeadlessGLContext(std::move(imp)));
            }
            failures.push_back(failure);
        }
        // The device platform: what NVIDIA's driver offers instead.
        PFNEGLQUERYDEVICESEXTPROC queryDevices = reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(imp->getProcAddress("eglQueryDevicesEXT"));
        if (queryDevices && hasExtension(clientExtensions, "EGL_EXT_platform_device")) {
            EGLDeviceEXT devices[16];
            EGLint count = 0;
            if (queryDevices(16, devices, &count)) {
                for (EGLint i = 0; i < count; ++i) {
                    EGLDisplay dpy = getPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, devices[i], nullptr);
                    if (imp->tryDisplay(dpy, "device " + std::to_string(i), &failure)) {
                        return std::unique_ptr<HeadlessGLContext>(new HeadlessGLContext(std::move(imp)));
                    }
                    failures.push_back(failure);
                }
            }
        }
        if (failures.empty()) {
            failures.push_back("EGL offers neither the surfaceless platform (EGL_MESA_platform_surfaceless) nor devices (EGL_EXT_platform_device)");
        }
    }
    std::string joined;
    for (std::size_t i = 0; i < failures.size(); ++i) {
        joined += (i ? "; " : "") + failures[i];
    }
    *error = prefix + joined + ".";

    return std::unique_ptr<HeadlessGLContext>();
} // HeadlessGLContext::create

bool
HeadlessGLContext::makeCurrent()
{
    if (_imp->getCurrentContext() == _imp->context) {
        return true;
    }

    return _imp->makeCurrent(_imp->display, EGL_NO_SURFACE, EGL_NO_SURFACE, _imp->context) == EGL_TRUE;
}

std::string
HeadlessGLContext::description() const
{
    return _imp->description;
}

void*
HeadlessGLContext::getProcAddress(const char* name) const
{
    return reinterpret_cast<void*>(_imp->getProcAddress(name));
}

NATRON_NAMESPACE_EXIT
