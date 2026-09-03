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

#include <gtest/gtest.h>

#include "Engine/OSGLContext.h"
#include "Engine/GPUContextPool.h"
#include "Engine/Settings.h"

NATRON_NAMESPACE_USING

#if defined(__NATRON_WIN32__) && !defined(__NATRON_MINGW__)
namespace {

// Define nullptr_t operator since MSVC dosn't appear to have one by default.
std::ostream& operator<<(std::ostream& os, const std::nullptr_t p) {
    return os << "<nullptr>";
}

}  // namespace
#endif

// Disabled: under xvfb-run on aswf/ci-vfxall:2027-clang21.1, Xvfb yields no
// GLX visual (measured: "Error while loading OpenGL: GLX: No GLXFBConfigs
// returned" / "OpenGL rendering is disabled" with +extension GLX, +iglx, and
// LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe all tried), so the
// isOpenGLLoaded() guard below always takes the bare-return branch, which
// gtest scores as a pass -- reporting coverage that never ran. To run this
// for real: `--gtest_also_run_disabled_tests` inside devshell.sh on a host
// with /dev/dri (devshell.sh passes it through when present).
TEST(OSGLContext, DISABLED_Basic)
{
    if (!appPTR->isOpenGLLoaded()) {
        // TODO: Convert to GTEST_SKIP() when gtest updated.
        std::cerr << "Skipping test because OpenGL loading failed." << std::endl;
        return;
    }

    SettingsPtr settings =  appPTR->getCurrentSettings();
    ASSERT_NE(settings.get(), nullptr);
    GLRendererID rendererID = settings->getActiveOpenGLRendererID();

    // Verify that we start without a context being set.
    EXPECT_FALSE(OSGLContext::threadHasACurrentContext());

    auto newContext = std::make_shared<OSGLContext>( FramebufferConfig(), nullptr, GLVersion.major, GLVersion.minor, rendererID);

    // Verify that creating a context does not set the current context.
    EXPECT_FALSE(OSGLContext::threadHasACurrentContext());

    newContext->setContextCurrentNoRender();

    // Verify that the current context was actually set.
    EXPECT_TRUE(OSGLContext::threadHasACurrentContext());

    OSGLContext::unsetCurrentContextNoRender();

    // Verify that it is no longer set.
    EXPECT_FALSE(OSGLContext::threadHasACurrentContext());
}

// Disabled: same measured cause as OSGLContext.DISABLED_Basic above -- no
// GLX visual under xvfb-run on this image, so isOpenGLLoaded() is always
// false here and the bare return below was being scored as a pass. Run for
// real with `--gtest_also_run_disabled_tests` inside devshell.sh on a host
// with /dev/dri.
TEST(GPUContextPool, DISABLED_Basic) {
    if (!appPTR->isOpenGLLoaded()) {
        /// TODO: Convert to GTEST_SKIP() when gtest updated.
        std::cerr << "Skipping test because OpenGL loading failed." << std::endl;
        return;
    }

    // Verify that we start without a context being set.
    EXPECT_FALSE(OSGLContext::threadHasACurrentContext());

    GPUContextPool pool;

    // Verify that creating the pool does not set the current context.
    EXPECT_FALSE(OSGLContext::threadHasACurrentContext());
    OSGLContextPtr context = pool.attachGLContextToRender(/* checkIfGLLoaded */ false);

    // Verify that the attach returns a valid pointer to a context, but
    // this context has not been made current on this thread yet.
    EXPECT_NE(context.get(), nullptr);
    EXPECT_FALSE(OSGLContext::threadHasACurrentContext());

    context->setContextCurrentNoRender();

    // Verify that the current context is now set.
    EXPECT_TRUE(OSGLContext::threadHasACurrentContext());

    OSGLContext::unsetCurrentContextNoRender();

    // Verify that the current context is no longer set.
    EXPECT_FALSE(OSGLContext::threadHasACurrentContext());

    pool.releaseGLContextFromRender(context);

    // Verify that returing the context does not make it current.
    EXPECT_FALSE(OSGLContext::threadHasACurrentContext());

    context.reset();

    EXPECT_FALSE(OSGLContext::threadHasACurrentContext());
}
