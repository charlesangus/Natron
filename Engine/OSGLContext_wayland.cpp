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

#include "OSGLContext_wayland.h"

#ifdef __NATRON_LINUX__

#ifdef __NATRON_WAYLAND__

#include <array>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream> // stringstream
#include <stdexcept>

#include <dlfcn.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include "Engine/AppManager.h"
#include "Engine/OSGLContext.h"
#include "Engine/StartupGLDiag.h"
#include "Global/GLIncludes.h"

NATRON_NAMESPACE_ENTER

// Wayland-specific global data
//
struct WaylandData
{
    wl_display* display;

    wl_compositor* compositor;
};

struct OSGLContext_egl_dataPrivate
{
    WaylandData wayland;
    EGLDisplay dpy;
    int major, minor;
    int eventBase;
    int errorBase;
    int err;

    // dlopen handle for libGL.so.1
    void* handle;

    // EGL
    PFNEGLGETDISPLAYPROC GetDisplay;
    PFNEGLGETCONFIGSPROC GetConfigs;
    PFNEGLGETCONFIGATTRIBPROC GetConfigAttrib;
    PFNEGLINITIALIZEPROC Initialize;
    PFNEGLBINDAPIPROC BindAPI;
    PFNEGLCHOOSECONFIGPROC ChooseConfig;
    PFNEGLCREATECONTEXTPROC CreateContext;
    PFNEGLGETCURRENTCONTEXTPROC GetCurrentContext;
    PFNEGLGETCURRENTDISPLAYPROC GetCurrentDisplay;
    PFNEGLGETCURRENTSURFACEPROC GetCurrentSurface;
    PFNEGLQUERYAPIPROC QueryAPI;
    PFNEGLDESTROYCONTEXTPROC DestroyContext;
    PFNEGLCREATEWINDOWSURFACEPROC CreateWindowSurface;
    PFNEGLDESTROYSURFACEPROC DestroySurface;
    PFNEGLMAKECURRENTPROC MakeCurrent;
    PFNEGLSWAPBUFFERSPROC SwapBuffers;
    PFNEGLSWAPINTERVALPROC SwapInterval;
    PFNEGLQUERYSTRINGPROC QueryString;
    PFNEGLGETPROCADDRESSPROC GetProcAddress;
    PFNEGLTERMINATEPROC Terminate;
    PFNEGLRELEASETHREADPROC ReleaseThread;

    PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC CreatePlatformWindowSurfaceEXT;

    EGLBoolean KHR_create_context;
    EGLBoolean EXT_platform_wayland;
    EGLBoolean KHR_gl_colorspace;
    EGLBoolean EXT_create_context_robustness;
    EGLBoolean KHR_context_flush_control;
};

OSGLContext_egl_data::OSGLContext_egl_data()
    : _imp(new OSGLContext_egl_dataPrivate())
{
}

OSGLContext_egl_data::~OSGLContext_egl_data()
{
}

namespace {

struct SavedEGLBinding {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface draw = EGL_NO_SURFACE;
    EGLSurface read = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    EGLenum api = EGL_OPENGL_API;
};

// Qt records which context it made current on each thread and skips its own
// eglMakeCurrent while that record matches, never asking EGL. So when Natron
// binds one of its contexts on the GUI thread, whatever it displaced is put
// back when Natron releases its context, rather than leaving nothing bound
// under a context Qt still believes current.
struct PendingRestore {
    SavedEGLBinding displaced;
    // Anything other than this bound at release time means Qt has rebound
    // its own context since, and that binding is left alone.
    EGLContext natronContext = EGL_NO_CONTEXT;
};

// Only the GUI thread fills it; a context destroyed on another thread may
// still have to clear it.
std::mutex g_pendingRestoreMutex;
std::optional<PendingRestore> g_pendingRestore;

SavedEGLBinding
captureEGLBinding(const OSGLContext_egl_data* eglInfo)
{
    SavedEGLBinding b;

    b.context = eglInfo->_imp->GetCurrentContext();
    if (eglInfo->_imp->GetCurrentDisplay) {
        b.display = eglInfo->_imp->GetCurrentDisplay();
    }
    if (eglInfo->_imp->GetCurrentSurface) {
        b.draw = eglInfo->_imp->GetCurrentSurface(EGL_DRAW);
        b.read = eglInfo->_imp->GetCurrentSurface(EGL_READ);
    }
    if (eglInfo->_imp->QueryAPI) {
        b.api = eglInfo->_imp->QueryAPI();
    }

    return b;
}

// Qt's EGL context may live on its own EGLDisplay, so it is restored with the
// display it was current on rather than Natron's.
bool
restoreEGLBinding(const OSGLContext_egl_data* eglInfo,
                  const SavedEGLBinding& saved)
{
    if (saved.context == EGL_NO_CONTEXT) {
        return eglInfo->_imp->MakeCurrent(eglInfo->_imp->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    eglInfo->_imp->BindAPI(saved.api);

    return eglInfo->_imp->MakeCurrent((saved.display != EGL_NO_DISPLAY) ? saved.display : eglInfo->_imp->dpy, saved.draw, saved.read, saved.context);
}

std::optional<PendingRestore>
takePendingRestore()
{
    std::lock_guard<std::mutex> k(g_pendingRestoreMutex);
    std::optional<PendingRestore> pending = g_pendingRestore;
    g_pendingRestore.reset();

    return pending;
}

} // anonymous namespace

struct ScopedEGLBindingRestore::Imp {
    const OSGLContext_egl_data* eglInfo = nullptr;
    bool guiThread = false;
    SavedEGLBinding saved;
    std::optional<PendingRestore> outerPending;
};

ScopedEGLBindingRestore::ScopedEGLBindingRestore()
    : _imp(new Imp)
{
    const OSGLContext_egl_data* eglInfo = appPTR ? appPTR->getEGLData() : nullptr;

    if (!eglInfo || !eglInfo->_imp->dpy) {
        return;
    }
    _imp->eglInfo = eglInfo;
    _imp->saved = captureEGLBinding(eglInfo);
    // A make-current inside the scope must not consume the restore owed to
    // an enclosing Natron make-current, so it is set aside until the scope
    // ends.
    _imp->guiThread = StartupGLDiag::onGuiThread();
    if (_imp->guiThread) {
        _imp->outerPending = takePendingRestore();
    }
}

ScopedEGLBindingRestore::~ScopedEGLBindingRestore()
{
    if (!_imp->eglInfo) {
        return;
    }
    const SavedEGLBinding now = captureEGLBinding(_imp->eglInfo);
    if ((now.context != _imp->saved.context) || (now.draw != _imp->saved.draw) || (now.read != _imp->saved.read) || (now.api != _imp->saved.api)) {
        restoreEGLBinding(_imp->eglInfo, _imp->saved);
    }
    if (_imp->guiThread) {
        std::lock_guard<std::mutex> k(g_pendingRestoreMutex);
        g_pendingRestore = _imp->outerPending;
    }
}

// Wayland-specific per-window data
//
struct WaylandWindow
{
    wl_surface* handle;
    wl_egl_window* native;
    int width;
    int height;

    WaylandWindow()
        : handle(nullptr)
        , native(nullptr)
        , width(0)
        , height(0)
    {
    }
};

struct OSGLContext_waylandPrivate
{
    EGLContext eglContextHandle;
    EGLDeviceEXT eglDeviceHandle;
    EGLSurface eglSurfaceHandle;
    WaylandWindow waylandWindow;

    OSGLContext_waylandPrivate()
        : eglContextHandle(EGL_NO_CONTEXT)
        , eglDeviceHandle(EGL_NO_DEVICE_EXT)
        , eglSurfaceHandle(EGL_NO_SURFACE)
        , waylandWindow()
    {
    }

    void createContextEGL(OSGLContext_egl_data* eglInfo, const FramebufferConfig& fbconfig, int major, int minor, bool coreProfile, int rendererID, const OSGLContext_wayland* shareContext);

    void createWindow(OSGLContext_egl_data* eglInfo,
                      const EGLConfig& native,
                      int depth);
};

static bool
extensionSupported(const OSGLContext_egl_data* data,
                   const char* extension)
{
    const char* extensions = data->_imp->QueryString(data->_imp->dpy, EGL_EXTENSIONS);

    if (extensions) {
        if (OSGLContext::stringInExtensionString(extension, extensions)) {
            return true;
        }
    }

    return false;
}

typedef void (*GLPFNglproc)(void);
static GLPFNglproc
getProcAddress(const OSGLContext_egl_data* data,
               const char* procname)
{
    if (data->_imp->GetProcAddress) {
        return data->_imp->GetProcAddress(procname);
    } else {
        return (GLPFNglproc)dlsym(data->_imp->handle, procname);
    }
}

static void
registry_handle_global(void* data, wl_registry* registry, uint32_t id, const char* interface, uint32_t version)
{
    auto state = reinterpret_cast<WaylandData*>(data);
    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
        state->compositor = reinterpret_cast<wl_compositor*>(wl_registry_bind(registry, id, &wl_compositor_interface, 4));
    }
}

static void
registry_handle_global_remove(void* data, wl_registry* registry, uint32_t id)
{
}

static const wl_registry_listener registry_listener = {
    registry_handle_global,
    registry_handle_global_remove,
};

void
OSGLContext_wayland::initEGLData(OSGLContext_egl_data* eglInfo)
{
    // Sets all bits to 0
    memset(eglInfo->_imp.get(), 0, sizeof(OSGLContext_egl_dataPrivate));

    eglInfo->_imp->wayland.display = wl_display_connect(nullptr);
    if (!eglInfo->_imp->wayland.display) {
        throw std::runtime_error("Wayland: Failed to open display");
    }

    wl_registry* registry = wl_display_get_registry(eglInfo->_imp->wayland.display);
    if (registry == nullptr) {
        throw std::runtime_error("Wayland: Failed to get registry from display");
    }

    wl_registry_add_listener(registry, &registry_listener, &eglInfo->_imp->wayland);

    wl_display_roundtrip(eglInfo->_imp->wayland.display);

    wl_registry_destroy(registry);

    const char* sonames[] = {
#if defined(__CYGWIN__)
        "libEGL-1.so",
#else
        "libEGL.so.1",
        "libEGL.so",
#endif
        nullptr
    };

    for (int i = 0; sonames[i]; ++i) {
        eglInfo->_imp->handle = dlopen(sonames[i], RTLD_LAZY | RTLD_GLOBAL);
        if (eglInfo->_imp->handle) {
            break;
        }
    }

    if (!eglInfo->_imp->handle) {
        throw std::runtime_error("EGL: Failed to load EGL");
    }

    // EGL
    eglInfo->_imp->GetDisplay = (PFNEGLGETDISPLAYPROC)dlsym(eglInfo->_imp->handle, "eglGetDisplay");
    eglInfo->_imp->GetConfigs = (PFNEGLGETCONFIGSPROC)dlsym(eglInfo->_imp->handle, "eglGetConfigs");
    eglInfo->_imp->GetConfigAttrib = (PFNEGLGETCONFIGATTRIBPROC)dlsym(eglInfo->_imp->handle, "eglGetConfigAttrib");
    eglInfo->_imp->Initialize = (PFNEGLINITIALIZEPROC)dlsym(eglInfo->_imp->handle, "eglInitialize");
    eglInfo->_imp->BindAPI = (PFNEGLBINDAPIPROC)dlsym(eglInfo->_imp->handle, "eglBindAPI");
    eglInfo->_imp->ChooseConfig = (PFNEGLCHOOSECONFIGPROC)dlsym(eglInfo->_imp->handle, "eglChooseConfig");
    eglInfo->_imp->CreateContext = (PFNEGLCREATECONTEXTPROC)dlsym(eglInfo->_imp->handle, "eglCreateContext");
    eglInfo->_imp->GetCurrentContext = (PFNEGLGETCURRENTCONTEXTPROC)dlsym(eglInfo->_imp->handle, "eglGetCurrentContext");
    eglInfo->_imp->GetCurrentDisplay = (PFNEGLGETCURRENTDISPLAYPROC)dlsym(eglInfo->_imp->handle, "eglGetCurrentDisplay");
    eglInfo->_imp->GetCurrentSurface = (PFNEGLGETCURRENTSURFACEPROC)dlsym(eglInfo->_imp->handle, "eglGetCurrentSurface");
    eglInfo->_imp->QueryAPI = (PFNEGLQUERYAPIPROC)dlsym(eglInfo->_imp->handle, "eglQueryAPI");
    eglInfo->_imp->DestroyContext = (PFNEGLDESTROYCONTEXTPROC)dlsym(eglInfo->_imp->handle, "eglDestroyContext");
    eglInfo->_imp->CreateWindowSurface = (PFNEGLCREATEWINDOWSURFACEPROC)dlsym(eglInfo->_imp->handle, "eglCreateWindowSurface");
    eglInfo->_imp->DestroySurface = (PFNEGLDESTROYSURFACEPROC)dlsym(eglInfo->_imp->handle, "eglDestroySurface");
    eglInfo->_imp->MakeCurrent = (PFNEGLMAKECURRENTPROC)dlsym(eglInfo->_imp->handle, "eglMakeCurrent");
    eglInfo->_imp->SwapBuffers = (PFNEGLSWAPBUFFERSPROC)dlsym(eglInfo->_imp->handle, "eglSwapBuffers");
    eglInfo->_imp->SwapInterval = (PFNEGLSWAPINTERVALPROC)getProcAddress(eglInfo, "eglSwapInterval");
    eglInfo->_imp->GetProcAddress = (PFNEGLGETPROCADDRESSPROC)dlsym(eglInfo->_imp->handle, "eglGetProcAddress");
    eglInfo->_imp->QueryString = (PFNEGLQUERYSTRINGPROC)dlsym(eglInfo->_imp->handle, "eglQueryString");
    eglInfo->_imp->Terminate = (PFNEGLTERMINATEPROC)dlsym(eglInfo->_imp->handle, "eglTerminate");
    eglInfo->_imp->ReleaseThread = (PFNEGLRELEASETHREADPROC)dlsym(eglInfo->_imp->handle, "eglReleaseThread");

    eglInfo->getProcAddress = (GLADloadproc)eglInfo->_imp->GetProcAddress;

    eglInfo->_imp->dpy = eglInfo->_imp->GetDisplay(eglInfo->_imp->wayland.display);
    if (eglInfo->_imp->dpy == EGL_NO_DISPLAY) {
        throw std::runtime_error("EGL: No available EGL display");
    }

    EGLBoolean ret = eglInfo->_imp->Initialize(eglInfo->_imp->dpy, &eglInfo->_imp->major, &eglInfo->_imp->minor);
    if (!ret) {
        throw std::runtime_error("EGL: Could not initialize EGL");
    }

    if ((eglInfo->_imp->major == 1) && (eglInfo->_imp->minor < 4)) {
        throw std::runtime_error("EGL: EGL version 1.4 or higher is required");
    }

    if ((eglInfo->_imp->major > 1) || (eglInfo->_imp->minor >= 5) || extensionSupported(eglInfo, "EGL_KHR_create_context")) {
        eglInfo->_imp->KHR_create_context = EGL_TRUE;
    }

    if (extensionSupported(eglInfo, "EGL_EXT_platform_wayland")) {
        eglInfo->_imp->CreatePlatformWindowSurfaceEXT = (PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC)getProcAddress(eglInfo, "eglCreatePlatformWindowSurfaceEXT");

        if (eglInfo->_imp->CreatePlatformWindowSurfaceEXT) {
            eglInfo->_imp->EXT_platform_wayland = EGL_TRUE;
        }
    }

    if (extensionSupported(eglInfo, "EGL_KHR_gl_colorspace")) {
        eglInfo->_imp->KHR_gl_colorspace = EGL_TRUE;
    }

    if (extensionSupported(eglInfo, "EGL_EXT_create_context_robustness")) {
        eglInfo->_imp->EXT_create_context_robustness = EGL_TRUE;
    }

    if (extensionSupported(eglInfo, "EGL_KHR_context_flush_control")) {
        eglInfo->_imp->KHR_context_flush_control = EGL_TRUE;
    }
} // initGLXData

void
OSGLContext_wayland::destroyEGLData(OSGLContext_egl_data* eglInfo)
{
    if (eglInfo->_imp->dpy != EGL_NO_DISPLAY) {
        eglInfo->_imp->Terminate(eglInfo->_imp->dpy);
        eglInfo->_imp->ReleaseThread();
    }
    if (eglInfo->_imp->handle != nullptr) {
        dlclose(eglInfo->_imp->handle);
        eglInfo->_imp->handle = nullptr;
    }
    if (eglInfo->_imp->wayland.compositor != nullptr) {
        wl_compositor_destroy(eglInfo->_imp->wayland.compositor);
        eglInfo->_imp->wayland.compositor = nullptr;
    }
    if (eglInfo->_imp->wayland.display != nullptr) {
        wl_display_flush(eglInfo->_imp->wayland.display);
        wl_display_disconnect(eglInfo->_imp->wayland.display);
        eglInfo->_imp->wayland.display = nullptr;
    }
}

// Returns the specified attribute of the specified EGLConfig
//
static int
getFBConfigAttrib(const OSGLContext_egl_data* eglInfo,
                  const EGLConfig& fbconfig,
                  int attrib)
{
    // Same out-parameter hazard as chooseFBConfig() below: eglGetConfigAttrib()
    // returns EGL_FALSE and leaves *value untouched when the attribute is not
    // supported, so an uninitialized `value` is an indeterminate read that then
    // feeds the config-scoring in OSGLContext::chooseFBConfig().
    int value = 0;

    eglInfo->_imp->GetConfigAttrib(eglInfo->_imp->dpy, fbconfig, attrib, &value);

    return value;
}

// Return a list of available and usable framebuffer configs
//
static void
chooseFBConfig(const OSGLContext_egl_data* eglInfo,
               const FramebufferConfig& desired,
               EGLConfig* result)
{
    if (!eglInfo->_imp->dpy) {
        throw std::runtime_error("EGL: No DISPLAY available");
    }

    // Same hazard as chooseFBConfig() in OSGLContext_x11.cpp: eglGetConfigs()
    // leaves its *num_config out-parameter untouched when it fails (returns
    // EGL_FALSE), so nativeCount must be initialized and the return value
    // checked. Otherwise the vector sizing below uses stack garbage.
    int nativeCount = 0;
    if (!eglInfo->_imp->GetConfigs(eglInfo->_imp->dpy, nullptr, 0, &nativeCount) || (nativeCount <= 0)) {
        throw std::runtime_error("EGL: No EGLConfigs returned");
    }
    std::vector<EGLConfig> nativeConfigs(nativeCount);
    // The second call needs the same treatment: on EGL_FALSE it writes neither
    // the configs nor the count, which would leave nativeConfigs full of the
    // vector's value-initialized null EGLConfigs while nativeCount still claims
    // they are populated.
    if (!eglInfo->_imp->GetConfigs(eglInfo->_imp->dpy, nativeConfigs.data(), nativeCount, &nativeCount) || (nativeCount <= 0)) {
        throw std::runtime_error("EGL: Failed to retrieve EGLConfigs");
    }

    std::vector<FramebufferConfig> usableConfigs(nativeCount);
    int usableCount = 0;

    for (int i = 0; i < nativeCount; ++i) {
        const EGLConfig& n = nativeConfigs[i];
        FramebufferConfig& u = usableConfigs[usableCount];

        // Only consider window EGLConfigs
        if (!(getFBConfigAttrib(eglInfo, n, EGL_SURFACE_TYPE) & EGL_WINDOW_BIT)) {
            continue;
        }

        u.redBits = getFBConfigAttrib(eglInfo, n, EGL_RED_SIZE);
        u.greenBits = getFBConfigAttrib(eglInfo, n, EGL_GREEN_SIZE);
        u.blueBits = getFBConfigAttrib(eglInfo, n, EGL_BLUE_SIZE);

        u.alphaBits = getFBConfigAttrib(eglInfo, n, EGL_ALPHA_SIZE);
        u.depthBits = getFBConfigAttrib(eglInfo, n, EGL_DEPTH_SIZE);
        u.stencilBits = getFBConfigAttrib(eglInfo, n, EGL_STENCIL_SIZE);

        u.accumRedBits = FramebufferConfig::ATTR_DONT_CARE;
        u.accumGreenBits = FramebufferConfig::ATTR_DONT_CARE;
        u.accumBlueBits = FramebufferConfig::ATTR_DONT_CARE;
        u.accumAlphaBits = FramebufferConfig::ATTR_DONT_CARE;

        u.auxBuffers = desired.auxBuffers;

        u.stereo = desired.stereo;

        u.doublebuffer = desired.doublebuffer;

        u.samples = getFBConfigAttrib(eglInfo, n, EGL_SAMPLES);

        u.sRGB = desired.sRGB;

        u.handle = (uintptr_t)n;
        usableCount++;
    }

    const FramebufferConfig& closest = OSGLContext::chooseFBConfig(desired, usableConfigs, usableCount);
    *result = (EGLConfig)closest.handle;
} // chooseFBConfig

// Create the Wayland window (and its colormap)
//
void
OSGLContext_waylandPrivate::createWindow(OSGLContext_egl_data* eglInfo, const EGLConfig& native, int depth)
{
    const int width = 32;
    const int height = 32;

    // Create the actual window
    waylandWindow.handle = wl_compositor_create_surface(eglInfo->_imp->wayland.compositor);

    if (!waylandWindow.handle) {
        throw std::runtime_error("Wayland: Failed to create surface");
    }

    waylandWindow.width = width;
    waylandWindow.height = height;

    waylandWindow.native = wl_egl_window_create(waylandWindow.handle, waylandWindow.width, waylandWindow.height);
    if (waylandWindow.native == nullptr) {
        throw std::runtime_error("EGL: Failed to create native window");
    }

    wl_surface_set_opaque_region(waylandWindow.handle, nullptr);

    wl_surface_commit(waylandWindow.handle);

    if (eglInfo->_imp->EXT_platform_wayland) {
        eglSurfaceHandle = eglInfo->_imp->CreatePlatformWindowSurfaceEXT(eglInfo->_imp->dpy, native, waylandWindow.native, nullptr);
    } else {
        eglSurfaceHandle = eglInfo->_imp->CreateWindowSurface(eglInfo->_imp->dpy, native, reinterpret_cast<EGLNativeWindowType>(waylandWindow.native), nullptr);
    }
    if (eglSurfaceHandle == EGL_NO_SURFACE) {
        throw std::runtime_error("EGL: Failed to create surface");
    }
} // createWindow

static inline void
setEGLattrib(std::vector<int>& attribs, int attribName, int attribValue)
{
    attribs.push_back(attribName);
    attribs.push_back(attribValue);
}

void
OSGLContext_waylandPrivate::createContextEGL(OSGLContext_egl_data* eglInfo,
                                             const FramebufferConfig& fbconfig,
                                             int major,
                                             int minor,
                                             bool coreProfile,
                                             int rendererID,
                                             const OSGLContext_wayland* shareContext)
{
    EGLConfig native;
    EGLContext share = shareContext ? shareContext->_imp->eglContextHandle : EGL_NO_CONTEXT;

    chooseFBConfig(eglInfo, fbconfig, &native);

    if (!eglInfo->_imp->BindAPI(EGL_OPENGL_API)) {
        throw std::runtime_error("EGL: Could not bind with OpenGL API");
    }

    std::vector<int> attribs;
    std::vector<int> ctxAttribs;
    int mask = 0, n;

    if (coreProfile) {
        mask |= EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR;
    } else {
        mask |= EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR;
    }

    // For EGL contexts we have to request for specific API versions, in our case is OpenGL 2.0
    if ((major < 2) || (minor < 0)) {
        major = 2;
        minor = 0;
    }

    if (eglInfo->_imp->KHR_create_context) {
        setEGLattrib(ctxAttribs, EGL_CONTEXT_MAJOR_VERSION_KHR, major);
        setEGLattrib(ctxAttribs, EGL_CONTEXT_MINOR_VERSION_KHR, minor);

        if (mask) {
            setEGLattrib(ctxAttribs, EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, mask);
        }
    } else {
        setEGLattrib(ctxAttribs, EGL_CONTEXT_CLIENT_VERSION, major);
    }

    if ((rendererID != -1) && (rendererID != 0)) {
        setEGLattrib(attribs, EGL_CONFIG_ID, rendererID);
    }

    setEGLattrib(attribs, EGL_SURFACE_TYPE, EGL_WINDOW_BIT);
    setEGLattrib(attribs, EGL_RED_SIZE, fbconfig.redBits);
    setEGLattrib(attribs, EGL_GREEN_SIZE, fbconfig.greenBits);
    setEGLattrib(attribs, EGL_BLUE_SIZE, fbconfig.blueBits);
    setEGLattrib(attribs, EGL_ALPHA_SIZE, fbconfig.alphaBits);
    setEGLattrib(attribs, EGL_DEPTH_SIZE, fbconfig.depthBits);
    setEGLattrib(attribs, EGL_STENCIL_SIZE, fbconfig.stencilBits);
    setEGLattrib(attribs, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT);

    setEGLattrib(attribs, EGL_NONE, EGL_NONE);
    setEGLattrib(ctxAttribs, EGL_NONE, EGL_NONE);

    eglInfo->_imp->ChooseConfig(eglInfo->_imp->dpy, attribs.data(), &native, 1, &n);
    if (n != 1) {
        throw std::runtime_error("EGL: Failed to choose config");
    }

    eglContextHandle = eglInfo->_imp->CreateContext(eglInfo->_imp->dpy, native, share, ctxAttribs.data());
    if (eglContextHandle == EGL_NO_CONTEXT) {
        throw std::runtime_error("EGL: Failed to create context");
    }
} // createContextEGL

OSGLContext_wayland::OSGLContext_wayland(const FramebufferConfig& pixelFormatAttrs,
                                         int major,
                                         int minor,
                                         bool coreProfile,
                                         const GLRendererID& rendererID,
                                         const OSGLContext_wayland* shareContext)
    : OSGLContext_xdg()
    , _imp(new OSGLContext_waylandPrivate())
{
    auto eglInfo = const_cast<OSGLContext_egl_data*>(appPTR->getEGLData());

    assert(eglInfo);

    EGLConfig native;

    int depth;

    chooseFBConfig(eglInfo, pixelFormatAttrs, &native);
    _imp->createWindow(eglInfo, native, depth);
    _imp->createContextEGL(eglInfo, pixelFormatAttrs, major, minor, coreProfile, rendererID.renderID, shareContext);
}

OSGLContext_wayland::~OSGLContext_wayland()
{
    const OSGLContext_egl_data* eglInfo = appPTR->getEGLData();

    assert(eglInfo);

    // Released before its surface goes away, so nothing is ever left bound
    // to a destroyed surface or context.
    if (_imp->eglContextHandle != EGL_NO_CONTEXT) {
        if (eglInfo->_imp->GetCurrentContext() == _imp->eglContextHandle) {
            std::optional<PendingRestore> pending;
            if (StartupGLDiag::onGuiThread()) {
                pending = takePendingRestore();
            }
            if (pending && (pending->displaced.context != _imp->eglContextHandle)) {
                restoreEGLBinding(eglInfo, pending->displaced);
            } else {
                eglInfo->_imp->MakeCurrent(eglInfo->_imp->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            }
        }
        std::lock_guard<std::mutex> k(g_pendingRestoreMutex);
        if (g_pendingRestore && (g_pendingRestore->displaced.context == _imp->eglContextHandle)) {
            g_pendingRestore.reset();
        }
    }
    if (_imp->eglSurfaceHandle != EGL_NO_SURFACE) {
        eglInfo->_imp->DestroySurface(eglInfo->_imp->dpy, _imp->eglSurfaceHandle);
        _imp->eglSurfaceHandle = EGL_NO_SURFACE;
    }
    if (_imp->waylandWindow.native != nullptr) {
        wl_egl_window_destroy(_imp->waylandWindow.native);
        _imp->waylandWindow.native = nullptr;
    }
    if (_imp->eglContextHandle != EGL_NO_CONTEXT) {
        eglInfo->_imp->DestroyContext(eglInfo->_imp->dpy, _imp->eglContextHandle);
        _imp->eglContextHandle = EGL_NO_CONTEXT;
    }
    if (_imp->waylandWindow.handle != nullptr) {
        wl_surface_destroy(_imp->waylandWindow.handle);
        _imp->waylandWindow.handle = nullptr;
    }
}

bool
OSGLContext_wayland::makeContextCurrent(const OSGLContext_wayland* context)
{
    const OSGLContext_egl_data* eglInfo = appPTR->getEGLData();

    assert(eglInfo);

    if (eglInfo->_imp->dpy == nullptr) {
        return false;
    }

    const bool guiThread = StartupGLDiag::onGuiThread();
    if (context != nullptr) {
        bool savedHere = false;
        if (guiThread) {
            std::lock_guard<std::mutex> k(g_pendingRestoreMutex);
            if (!g_pendingRestore) {
                g_pendingRestore = PendingRestore { captureEGLBinding(eglInfo), EGL_NO_CONTEXT };
                savedHere = true;
            }
        }
        if (!eglInfo->_imp->BindAPI(EGL_OPENGL_API)) {
            if (savedHere) {
                std::lock_guard<std::mutex> k(g_pendingRestoreMutex);
                g_pendingRestore.reset();
            }
            throw std::runtime_error("EGL: Could not bind with OpenGL API");
        }
        const bool ok = eglInfo->_imp->MakeCurrent(eglInfo->_imp->dpy, context->_imp->eglSurfaceHandle, context->_imp->eglSurfaceHandle, context->_imp->eglContextHandle);
        if (guiThread) {
            std::lock_guard<std::mutex> k(g_pendingRestoreMutex);
            if (ok && g_pendingRestore) {
                g_pendingRestore->natronContext = context->_imp->eglContextHandle;
            } else if (!ok && savedHere) {
                // A failed eglMakeCurrent leaves the previous binding in
                // place, so there is nothing to restore later.
                g_pendingRestore.reset();
            }
        }
        return ok;
    }

    std::optional<PendingRestore> pending;
    if (guiThread) {
        pending = takePendingRestore();
    }
    if (pending) {
        if (eglInfo->_imp->GetCurrentContext() != pending->natronContext) {
            return true;
        }

        return restoreEGLBinding(eglInfo, pending->displaced);
    }

    return eglInfo->_imp->MakeCurrent(eglInfo->_imp->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

bool
OSGLContext_wayland::threadHasACurrentContext() {
    const OSGLContext_egl_data* eglInfo = appPTR->getEGLData();

    assert(eglInfo);

    return eglInfo != nullptr && eglInfo->_imp->GetCurrentContext() != nullptr;
}

void
OSGLContext_wayland::swapBuffers()
{
    const OSGLContext_egl_data* eglInfo = appPTR->getEGLData();

    assert(eglInfo);
    eglInfo->_imp->SwapBuffers(eglInfo->_imp->dpy, _imp->eglSurfaceHandle);
}

void
OSGLContext_wayland::swapInterval(int interval)
{
    const OSGLContext_egl_data* eglInfo = appPTR->getEGLData();

    assert(eglInfo);

    eglInfo->_imp->SwapInterval(eglInfo->_imp->dpy, interval);
}

void
OSGLContext_wayland::getGPUInfos(std::list<OpenGLRendererInfo>& renderers)
{
    const OSGLContext_egl_data* eglInfo = appPTR->getEGLData();

    assert(eglInfo);
    if (!eglInfo) {
        return;
    }
    if (!eglInfo->_imp->dpy) {
        return;
    }

    // The probe binds a context of its own on whatever thread asks, possibly
    // over a context Qt believes current there.
    const ScopedEGLBindingRestore restoreOnExit;
    std::unique_ptr<OSGLContext_wayland> context;
    try {
        context.reset(new OSGLContext_wayland(FramebufferConfig(), GLVersion.major, GLVersion.minor, false, GLRendererID(), nullptr));
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;

        return;
    }

    if (!makeContextCurrent(context.get())) {
        return;
    }

    try {
        OSGLContext::checkOpenGLVersion();
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        makeContextCurrent(nullptr);

        return;
    }

    OpenGLRendererInfo info;
    info.vendorName = std::string(reinterpret_cast<const char*>(glGetString(GL_VENDOR)));
    info.rendererName = std::string(reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
    info.glVersionString = std::string(reinterpret_cast<const char*>(glGetString(GL_VERSION)));
    info.glslVersionString = std::string(reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION)));
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &info.maxTextureSize);
    // We don't have any way to get memory size, set it to 0
    info.maxMemBytes = 0;
    info.rendererID.renderID = -1;
    info.accelerated = !OSGLContext::isSoftwareRendererName(info.rendererName);
    info.hasUuid = OSGLContext::readCurrentDeviceUuid(getProcAddress(eglInfo, "glGetUnsignedBytei_vEXT"), &info.deviceUuid);
    renderers.push_back(info);

    makeContextCurrent(nullptr);
} // OSGLContext_wayland::getGPUInfos

NATRON_NAMESPACE_EXIT

#else // __NATRON_WAYLAND__

NATRON_NAMESPACE_ENTER

// Place stubs in case that Wayland wasn't enabled
struct OSGLContext_egl_dataPrivate
{
    void* moot;
};

OSGLContext_egl_data::OSGLContext_egl_data()
{
    throw std::runtime_error("EGL: Wayland contexts are not supported and need to be enabled at build time");
}

OSGLContext_egl_data::~OSGLContext_egl_data()
{
}

struct OSGLContext_waylandPrivate
{
    void* moot;
};

OSGLContext_wayland::OSGLContext_wayland(const FramebufferConfig&,
                                         int,
                                         int,
                                         bool,
                                         const GLRendererID&,
                                         const OSGLContext_wayland*)
    : OSGLContext_xdg()
{
}

OSGLContext_wayland::~OSGLContext_wayland()
{
}

void
OSGLContext_wayland::initEGLData(OSGLContext_egl_data*)
{
}

void
OSGLContext_wayland::destroyEGLData(OSGLContext_egl_data*)
{
}

void
OSGLContext_wayland::swapBuffers()
{
}

void
OSGLContext_wayland::swapInterval(int)
{
}

bool
OSGLContext_wayland::makeContextCurrent(const OSGLContext_wayland*)
{
    return false;
}

bool
OSGLContext_wayland::threadHasACurrentContext() {
    return false;
}

void
OSGLContext_wayland::getGPUInfos(std::list<OpenGLRendererInfo>&)
{
}

struct ScopedEGLBindingRestore::Imp {
};

ScopedEGLBindingRestore::ScopedEGLBindingRestore()
{
}

ScopedEGLBindingRestore::~ScopedEGLBindingRestore()
{
}

NATRON_NAMESPACE_EXIT

#endif // __NATRON_WAYLAND__

#endif // __NATRON_LINUX__
