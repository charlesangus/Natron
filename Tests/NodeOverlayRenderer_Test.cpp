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
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Global/GLIncludes.h" // must precede qopengl.h

#include <QOffscreenSurface>
#include <QOpenGLContext>

#include "Engine/AppManager.h"
#include "Engine/OverlayDrawList.h"
#include "Engine/OverlaySupport.h"
#include "Engine/RectD.h"
#include "Gui/NodeOverlayRenderer.h"

NATRON_NAMESPACE_USING

namespace {

bool
skipWithoutGL(const char* reason)
{
    const char* require = std::getenv("NATRON_TEST_REQUIRE_GL");
    if (require && (std::strcmp(require, "1") == 0)) {
        ADD_FAILURE() << reason << " (NATRON_TEST_REQUIRE_GL=1)";

        return true;
    }
    std::cout << "[  SKIPPED ] NodeOverlayRenderer: " << reason
              << "; run with DISPLAY set to an X server with GLX" << std::endl;

    return true;
}

class ScaledViewport
    : public OverlaySupport {
public:
    double scaleX = 1.;
    double scaleY = 1.;

    virtual void swapOpenGLBuffers() OVERRIDE FINAL { }

    virtual void redraw() OVERRIDE FINAL { }

    virtual void getViewportSize(double& width, double& height) const OVERRIDE FINAL
    {
        width = 256.;
        height = 256.;
    }

    virtual void getPixelScale(double& xScale, double& yScale) const OVERRIDE FINAL
    {
        xScale = scaleX;
        yScale = scaleY;
    }

#ifdef OFX_EXTENSIONS_NATRON
    virtual double getScreenPixelRatio() const OVERRIDE FINAL { return 1.; }
#endif

    virtual void getBackgroundColour(double& r, double& g, double& b) const OVERRIDE FINAL
    {
        r = g = b = 0.;
    }

    virtual void saveOpenGLContext() OVERRIDE FINAL { }

    virtual void restoreOpenGLContext() OVERRIDE FINAL { }

    virtual RectD getViewportRect() const OVERRIDE FINAL { return RectD(0., 0., 256., 256.); }

    virtual void getCursorPosition(double& x, double& y) const OVERRIDE FINAL
    {
        x = y = 0.;
    }

    virtual void toWidgetCoordinates(double* /*x*/, double* /*y*/) const OVERRIDE FINAL { }

    virtual void toCanonicalCoordinates(double* /*x*/, double* /*y*/) const OVERRIDE FINAL { }

    virtual int getWidgetFontHeight() const OVERRIDE FINAL { return 10; }

    virtual int getStringWidthForCurrentFont(const std::string& string) const OVERRIDE FINAL { return (int)string.size() * 6; }
};

class NodeOverlayRendererTest
    : public ::testing::Test {
protected:
    void SetUp() override
    {
        if (!appPTR->isOpenGLLoaded()) {
            _skip = skipWithoutGL("Natron did not load OpenGL");

            return;
        }
        _context.reset(new QOpenGLContext);
        if (!_context->create()) {
            _skip = skipWithoutGL("Qt could not create an OpenGL context");

            return;
        }
        _surface.reset(new QOffscreenSurface);
        _surface->setFormat(_context->format());
        _surface->create();
        if (!_surface->isValid() || !_context->makeCurrent(_surface.get())) {
            _skip = skipWithoutGL("Qt could not make its OpenGL context current");

            return;
        }
        glViewport(0, 0, 256, 256);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0., 256., 0., 256., -1., 1.);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
    }

    void TearDown() override
    {
        if (_context && (QOpenGLContext::currentContext() == _context.get())) {
            _renderer.releaseGLResources();
            _context->doneCurrent();
        }
        _context.reset();
        _surface.reset();
    }

    unsigned long long drawAt(const OverlayDrawList& list,
                              double scaleX,
                              double scaleY)
    {
        _viewport.scaleX = scaleX;
        _viewport.scaleY = scaleY;
        _renderer.draw(list, OverlayDrawState(), &_viewport);

        return _renderer.uploadCount();
    }

    bool _skip = false;
    std::unique_ptr<QOpenGLContext> _context;
    std::unique_ptr<QOffscreenSurface> _surface;
    ScaledViewport _viewport;
    NodeOverlayRenderer _renderer;
};

} // namespace

// 1.0, 1.03 and 1.05 are all within an eighth of an octave of each other, so a coarser zoom key
// would have treated them as one zoom.
TEST_F(NodeOverlayRendererTest, PixelSizedListIsRetessellatedOnAnyScaleChange)
{
    if (_skip) {
        return;
    }
    OverlayDrawList list;
    list.addAnchoredShape(128., 128., OverlayDrawList::AnchoredShape::cross(6.));
    ASSERT_TRUE(OverlayDrawList::dependsOnPixelScale(list.primitives().front()));

    const unsigned long long first = drawAt(list, 1., 1.);
    EXPECT_EQ(1u, first);
    EXPECT_EQ(first, drawAt(list, 1., 1.)) << "redrawing at the same scale uploads again";

    const unsigned long long zoomedX = drawAt(list, 1.03, 1.);
    EXPECT_EQ(first + 1, zoomedX) << "a small horizontal zoom change keeps the old cross size";

    const unsigned long long zoomedBoth = drawAt(list, 1.05, 1.05);
    EXPECT_EQ(zoomedX + 1, zoomedBoth) << "a small zoom change keeps the old cross size";
    EXPECT_EQ(zoomedBoth, drawAt(list, 1.05, 1.05)) << "redrawing at the same scale uploads again";
}

TEST_F(NodeOverlayRendererTest, ListWithoutPixelSizedGeometryKeepsItsBufferAcrossZoom)
{
    if (_skip) {
        return;
    }
    OverlayDrawList list;
    list.addPolyline({ OverlayDrawList::Point { 10., 10. }, OverlayDrawList::Point { 200., 40. }, OverlayDrawList::Point { 120., 220. } }, true);
    list.addPoint(64., 64.);
    for (const OverlayDrawList::Primitive& p : list.primitives()) {
        ASSERT_FALSE(OverlayDrawList::dependsOnPixelScale(p));
    }

    const unsigned long long first = drawAt(list, 1., 1.);
    EXPECT_EQ(1u, first);
    EXPECT_EQ(first, drawAt(list, 1.05, 1.05)) << "a small zoom change uploads a list with no pixel-sized geometry";
    EXPECT_EQ(first, drawAt(list, 4., 4.)) << "a large zoom change uploads a list with no pixel-sized geometry";
}
