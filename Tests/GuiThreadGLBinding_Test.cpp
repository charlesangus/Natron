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

#include "Global/Macros.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <list>
#include <memory>

#include <dlfcn.h>

#include <gtest/gtest.h>

#include "Global/GLIncludes.h" // must precede qopengl.h
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QtGui/qopenglcontext_platform.h>

#include "Engine/AppManager.h"
#include "Engine/OSGLContext.h"

NATRON_NAMESPACE_USING

// Qt records which context it made current on each thread and, while that
// record matches, never calls glXMakeCurrent again before compositing. So a
// Natron make-current or GPU probe on the GUI thread has to leave Qt's GLX
// context bound exactly as it found it; leaving nothing bound makes Qt's next
// shader compile run without a context. These tests bind a Qt context the way
// the widget compositor does and check the GLX binding after each Natron GL
// operation.
//
// They need GLX through Qt's xcb platform, so under the default offscreen
// platform they print a skip line and pass. Run them for real with
//   QT_QPA_PLATFORM=xcb Tests/gui/run-gl-tests.sh 'GuiThreadGLBinding.*' <build>/Tests/GuiTests
// NATRON_DIAG_GL_FORCE_PROBE_CONTEXT=1 makes the probe take the probe-context
// path NVIDIA drivers take; it is read once per process, during
// AppManager::load(), so it has to be in the environment at launch.

namespace {

using GetCurrentContextFn = void* (*)();
using GetCurrentDrawableFn = unsigned long (*)();

void*
resolveGLX(const char* symbol)
{
    if (void* p = dlsym(RTLD_DEFAULT, symbol)) {
        return p;
    }
    for (const char* lib : { "libGLX.so.0", "libGL.so.1", "libGL.so" }) {
        void* handle = dlopen(lib, RTLD_LAZY | RTLD_NOLOAD);
        if (!handle) {
            continue;
        }
        void* p = dlsym(handle, symbol);
        dlclose(handle);
        if (p) {
            return p;
        }
    }

    return nullptr;
}

struct GLXBinding {
    void* context = nullptr;
    unsigned long drawable = 0;
};

GLXBinding
currentGLXBinding()
{
    static const auto getContext = reinterpret_cast<GetCurrentContextFn>(resolveGLX("glXGetCurrentContext"));
    static const auto getDrawable = reinterpret_cast<GetCurrentDrawableFn>(resolveGLX("glXGetCurrentDrawable"));
    GLXBinding b;

    b.context = getContext ? getContext() : nullptr;
    b.drawable = getDrawable ? getDrawable() : 0;

    return b;
}

bool
skipWithoutGLX(const char* reason)
{
    const char* require = std::getenv("NATRON_TEST_REQUIRE_GL");

    if (require && (std::strcmp(require, "1") == 0)) {
        ADD_FAILURE() << reason << " (NATRON_TEST_REQUIRE_GL=1)";

        return true;
    }
    std::cout << "[  SKIPPED ] GuiThreadGLBinding: " << reason
              << "; run under QT_QPA_PLATFORM=xcb Tests/gui/run-gl-tests.sh with NATRON_TEST_REQUIRE_GL=1" << std::endl;

    return true;
}

class GuiThreadGLBinding
    : public ::testing::Test {
protected:
    void SetUp() override
    {
        if (appPTR->isOnWayland()) {
            _skip = skipWithoutGLX("Natron runs on EGL under Wayland");

            return;
        }
        if (!appPTR->isOpenGLLoaded() || !appPTR->getGLXData()) {
            _skip = skipWithoutGLX("Natron did not load OpenGL through GLX");

            return;
        }
        _qtContext.reset(new QOpenGLContext);
        if (!_qtContext->create()) {
            _skip = skipWithoutGLX("Qt could not create an OpenGL context");

            return;
        }
#if QT_CONFIG(xcb_glx_plugin)
        auto* glx = _qtContext->nativeInterface<QNativeInterface::QGLXContext>();
        _qtNative = glx ? static_cast<void*>(glx->nativeContext()) : nullptr;
#endif
        if (!_qtNative) {
            _skip = skipWithoutGLX("Qt's OpenGL context is not a GLX context");

            return;
        }
        _surface.reset(new QOffscreenSurface);
        _surface->setFormat(_qtContext->format());
        _surface->create();
        if (!_surface->isValid() || !_qtContext->makeCurrent(_surface.get())) {
            _skip = skipWithoutGLX("Qt could not make its OpenGL context current");

            return;
        }
        _qtBinding = currentGLXBinding();
        ASSERT_EQ(_qtNative, _qtBinding.context);
    }

    void TearDown() override
    {
        if (_qtContext && (QOpenGLContext::currentContext() == _qtContext.get())) {
            _qtContext->doneCurrent();
        }
        _surface.reset();
        _qtContext.reset();
    }

    void expectQtBindingKept(const char* after) const
    {
        const GLXBinding now = currentGLXBinding();

        EXPECT_EQ(_qtNative, now.context) << "GLX context after " << after << " is not Qt's";
        EXPECT_EQ(_qtBinding.drawable, now.drawable) << "GLX drawable after " << after << " is not Qt's";
        EXPECT_EQ(_qtContext.get(), QOpenGLContext::currentContext()) << "Qt's current context after " << after;
    }

    bool _skip = false;
    std::unique_ptr<QOpenGLContext> _qtContext;
    std::unique_ptr<QOffscreenSurface> _surface;
    void* _qtNative = nullptr;
    GLXBinding _qtBinding;
};

} // namespace

TEST_F(GuiThreadGLBinding, GPUProbeKeepsQtContextBound)
{
    if (_skip) {
        return;
    }
    std::list<OpenGLRendererInfo> renderers;

    OSGLContext::getGPUInfos(renderers);
    EXPECT_FALSE(renderers.empty()) << "the probe found no renderer, so it never bound a context of its own";
    expectQtBindingKept("OSGLContext::getGPUInfos");
}

TEST_F(GuiThreadGLBinding, MakeCurrentThenUnsetRestoresQtContext)
{
    if (_skip) {
        return;
    }
    std::unique_ptr<OSGLContext> context;
    try {
        context.reset(new OSGLContext(FramebufferConfig(), nullptr));
    } catch (const std::exception& e) {
        FAIL() << "creating an OSGLContext failed: " << e.what();
    }
    expectQtBindingKept("creating an OSGLContext");

    context->setContextCurrentNoRender();
    EXPECT_NE(_qtNative, currentGLXBinding().context) << "setContextCurrentNoRender left Qt's context bound";
    OSGLContext::unsetCurrentContextNoRender();
    expectQtBindingKept("setContextCurrentNoRender + unsetCurrentContextNoRender");

    context.reset();
    expectQtBindingKept("destroying the OSGLContext");
}

TEST_F(GuiThreadGLBinding, DestroyingContextKeepsQtContextBound)
{
    if (_skip) {
        return;
    }
    std::unique_ptr<OSGLContext> context;
    try {
        context.reset(new OSGLContext(FramebufferConfig(), nullptr));
    } catch (const std::exception& e) {
        FAIL() << "creating an OSGLContext failed: " << e.what();
    }

    context.reset();
    expectQtBindingKept("destroying an OSGLContext");
}
