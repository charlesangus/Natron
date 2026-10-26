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

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

// Before Qt's OpenGL headers.
#include "Gui/SceneViewport.h"

#include <QColor>
#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QSurfaceFormat>

#include "Engine/AppManager.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneWriteBarrier.h"
#include "Gui/GuiGLRequirements.h"

NATRON_NAMESPACE_USING

// Draws Tests/fixtures/scene-reference.usda through SceneViewportRenderer, the code the viewer's
// scene view draws with, into a framebuffer from fixed cameras, and checks colours at pixels whose
// content the fixture fixes: /World/Box (red, a 2-unit cube at the origin), /World/Ball (blue,
// radius 0.5, at (3, 0, 0) at timecode 1 and (3, 2, 0) at 24) and /World/Cam (at (1, 1, 10),
// looking down -Z). Needs a 4.5 compatibility GLX context; run it strictly with
//   Tests/gui/run-gl-tests.sh "SceneViewportStorm.*" build/<preset>/Tests/GuiTests

namespace {

const int kSize = 256;

bool
skipWithoutGL(const std::string& reason)
{
    const char* require = std::getenv("NATRON_TEST_REQUIRE_GL");

    if (require && (std::strcmp(require, "1") == 0)) {
        ADD_FAILURE() << reason << " (NATRON_TEST_REQUIRE_GL=1)";

        return true;
    }
    std::cout << "[  SKIPPED ] SceneViewportStorm: " << reason
              << "; run with DISPLAY set to an X server with GLX" << std::endl;

    return true;
}

// The fixture as a USDRead node would hand it on: a node layer sublayering the file and carrying
// its stage metadata.
ScenePayload
referenceScene()
{
    const std::string path = NATRON_TESTS_FIXTURES_DIR "/scene-reference.usda";
    PXR_NS::SdfLayerRefPtr file = PXR_NS::SdfLayer::FindOrOpen(path);

    if (!file) {
        ADD_FAILURE() << "cannot open " << path;

        return ScenePayload();
    }
    PXR_NS::SdfLayerRefPtr layer = SceneOps::createNodeLayer("SceneViewportTest");
    const PXR_NS::SdfPath& root = PXR_NS::SdfPath::AbsoluteRootPath();
    PXR_NS::TfToken upAxis;
    if (file->HasField(root, PXR_NS::UsdGeomTokens->upAxis, &upAxis)) {
        layer->SetField(root, PXR_NS::UsdGeomTokens->upAxis, upAxis);
    }
    double metersPerUnit = 0.;
    if (file->HasField(root, PXR_NS::UsdGeomTokens->metersPerUnit, &metersPerUnit)) {
        layer->SetField(root, PXR_NS::UsdGeomTokens->metersPerUnit, metersPerUnit);
    }
    layer->SetTimeCodesPerSecond(file->GetTimeCodesPerSecond());
    layer->InsertSubLayerPath(file->GetIdentifier());

    return SceneOps::appendLayer(ScenePayload(), layer, 0x5ce7e5ULL);
}

struct Rgb {
    int r, g, b;
};

std::ostream&
operator<<(std::ostream& os,
           const Rgb& c)
{
    return os << "(" << c.r << ", " << c.g << ", " << c.b << ")";
}

bool
isReddish(const Rgb& c)
{
    return (c.r > 90) && (c.r > c.g + 40) && (c.r > c.b + 40);
}

bool
isBluish(const Rgb& c)
{
    return (c.b > 90) && (c.b > c.r + 40) && (c.b > c.g + 20);
}

bool
isBackground(const Rgb& c)
{
    float rgb[3];

    SceneViewportRenderer::getBackgroundColor(rgb);
    const int grey = int(std::lround(rgb[0] * 255.f));

    return (std::abs(c.r - grey) <= 20) && (std::abs(c.g - grey) <= 20) && (std::abs(c.b - grey) <= 20);
}

class SceneViewportStorm
    : public ::testing::Test {
protected:
    void SetUp() override
    {
        if (!appPTR->isOpenGLLoaded()) {
            _skip = skipWithoutGL("Natron did not load OpenGL");

            return;
        }
        std::string usdError;
        if (!SceneOps::checkUsdRuntime(&usdError)) {
            _skip = true;
            FAIL() << "OpenUSD is not usable: " << usdError;
        }
        const QSurfaceFormat format = GuiGLRequirements::requestedFormat(QSurfaceFormat::defaultFormat(), QByteArray(), QByteArray(), nullptr);
        _context.reset(new QOpenGLContext);
        _context->setFormat(format);
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
        const QSurfaceFormat got = _context->format();
        if ((got.version() < qMakePair(4, 5)) || (got.profile() == QSurfaceFormat::CoreProfile)) {
            _skip = skipWithoutGL("got OpenGL " + std::to_string(got.majorVersion()) + "." + std::to_string(got.minorVersion()) + ((got.profile() == QSurfaceFormat::CoreProfile) ? " core" : "") + ", Storm needs 4.5 compatibility");

            return;
        }
        _fbo.reset(new QOpenGLFramebufferObject(kSize, kSize, QOpenGLFramebufferObject::CombinedDepthStencil));
        ASSERT_TRUE(_fbo->isValid());

        std::string error;
        ASSERT_TRUE(_renderer.setScene(referenceScene(), 1., 24., &error)) << error;
        // Looks down -Z from 10 units out, the origin at the centre of the frame.
        SceneViewportRenderer::FreeCamera camera;
        camera.distance = 10.;
        camera.azimuth = 0.;
        camera.elevation = 0.;
        camera.fovY = 45.;
        _renderer.setFreeCamera(camera);
        _renderer.setDrawMode(SceneViewportRenderer::eDrawModeUnshaded);
        // Edge-on from this camera, the grid would be a line across the middle of the frame.
        _renderer.setShowGrid(false);
    }

    void TearDown() override
    {
        if (_context && _surface && _context->makeCurrent(_surface.get())) {
            _renderer.releaseGL();
            _fbo.reset();
            _context->doneCurrent();
        }
        _fbo.reset();
        _surface.reset();
        _context.reset();
    }

    QImage render()
    {
        bool converged = false;

        for (int i = 0; i < 4 && !converged; ++i) {
            converged = _renderer.render(_fbo->handle(), kSize, kSize);
        }
        EXPECT_TRUE(converged) << "Storm did not converge";

        return _fbo->toImage();
    }

    static Rgb pixel(const QImage& image,
                     int x,
                     int y)
    {
        const QColor c = image.pixelColor(x, y);

        return Rgb { c.red(), c.green(), c.blue() };
    }

    static int countNonBackground(const QImage& image)
    {
        int n = 0;

        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                if (!isBackground(pixel(image, x, y))) {
                    ++n;
                }
            }
        }

        return n;
    }

    bool _skip = false;
    std::unique_ptr<QOpenGLContext> _context;
    std::unique_ptr<QOffscreenSurface> _surface;
    std::unique_ptr<QOpenGLFramebufferObject> _fbo;
    SceneViewportRenderer _renderer;
};

} // namespace

// Pixel (x, y) counts from the top left. From the free camera, with a 45 degree vertical field of
// view over 256 pixels, a point (X, Y, Z) lands at x = 128 + 128 X / ((10 - Z) tan 22.5),
// y = 128 - 128 Y / ((10 - Z) tan 22.5): the box covers about 94..162 on both axes, the ball at
// timecode 1 is centred on (221, 128) with a radius of 15 pixels, and at 24 on (221, 66).
TEST_F(SceneViewportStorm, DrawsTheReferenceSceneFromAFixedCamera)
{
    if (_skip) {
        return;
    }
    QImage frame1 = render();
    EXPECT_TRUE(isReddish(pixel(frame1, 128, 128))) << "box at the centre: " << pixel(frame1, 128, 128);
    EXPECT_TRUE(isReddish(pixel(frame1, 110, 145))) << "box off centre: " << pixel(frame1, 110, 145);
    EXPECT_TRUE(isBluish(pixel(frame1, 221, 128))) << "ball at timecode 1: " << pixel(frame1, 221, 128);
    EXPECT_TRUE(isBackground(pixel(frame1, 221, 66))) << "above the ball at timecode 1: " << pixel(frame1, 221, 66);
    EXPECT_TRUE(isBackground(pixel(frame1, 6, 6))) << "corner: " << pixel(frame1, 6, 6);
    EXPECT_TRUE(isBackground(pixel(frame1, 40, 200))) << "left of the box: " << pixel(frame1, 40, 200);

    // Frame 24 of a 24 fps project is timecode 24 of the 24 tcps file.
    _renderer.setFrame(24., 24.);
    EXPECT_DOUBLE_EQ(24., _renderer.getTimeCode());
    QImage frame24 = render();
    EXPECT_TRUE(isBluish(pixel(frame24, 221, 66))) << "ball at timecode 24: " << pixel(frame24, 221, 66);
    EXPECT_TRUE(isBackground(pixel(frame24, 221, 128))) << "where the ball was: " << pixel(frame24, 221, 128);
    EXPECT_TRUE(isReddish(pixel(frame24, 128, 128))) << "box at timecode 24: " << pixel(frame24, 128, 128);

    // A 48 fps project reaches timecode 24 at frame 48.
    _renderer.setFrame(48., 48.);
    EXPECT_DOUBLE_EQ(24., _renderer.getTimeCode());
}

TEST_F(SceneViewportStorm, PicksThePrimDrawnAtAPixel)
{
    if (_skip) {
        return;
    }
    const QImage unselected = render();
    EXPECT_EQ(std::string("/World/Box"), _renderer.pick(128, 128, kSize, kSize));
    EXPECT_EQ(std::string("/World/Ball"), _renderer.pick(221, 128, kSize, kSize));
    EXPECT_EQ(std::string(), _renderer.pick(6, 6, kSize, kSize));
    EXPECT_EQ(std::string(), _renderer.pick(221, 66, kSize, kSize));

    // The picked prim is highlighted.
    _renderer.setSelectedPrim("/World/Box");
    const QImage selected = render();
    const Rgb before = pixel(unselected, 128, 128);
    const Rgb after = pixel(selected, 128, 128);
    EXPECT_GT(std::abs(after.r - before.r) + std::abs(after.g - before.g) + std::abs(after.b - before.b), 30)
        << "highlight left the box unchanged: " << before << " -> " << after;
    EXPECT_LE(std::abs(pixel(unselected, 221, 128).b - pixel(selected, 221, 128).b), 3) << "the ball is not selected";

    _renderer.setSelectedPrim(std::string());
    const QImage cleared = render();
    const Rgb back = pixel(cleared, 128, 128);
    EXPECT_LE(std::abs(back.r - before.r) + std::abs(back.g - before.g) + std::abs(back.b - before.b), 6)
        << "clearing the selection left the highlight: " << before << " -> " << back;
}

// /World/Cam sits at (1, 1, 10) with the fallback aperture (vertical 15.2908 mm) and a 35 mm lens:
// the box, one unit down and to the left of its axis, is centred near (69, 187).
TEST_F(SceneViewportStorm, LooksThroughAStageCamera)
{
    if (_skip) {
        return;
    }
    const std::vector<std::string> cameras = _renderer.getStageCameraPaths();
    ASSERT_EQ(1u, cameras.size());
    EXPECT_EQ(std::string("/World/Cam"), cameras.front());

    const QImage free = render();
    EXPECT_TRUE(isBackground(pixel(free, 69, 187))) << pixel(free, 69, 187);

    _renderer.setLookThroughCamera("/World/Cam");
    const QImage through = render();
    EXPECT_TRUE(isReddish(pixel(through, 69, 187))) << "box through /World/Cam: " << pixel(through, 69, 187);
    EXPECT_TRUE(isBackground(pixel(through, 200, 40))) << pixel(through, 200, 40);
    EXPECT_EQ(std::string("/World/Box"), _renderer.pick(69, 187, kSize, kSize));

    // Moving the camera goes back to the free one.
    _renderer.tumble(0., 0.);
    EXPECT_EQ(std::string(), _renderer.getLookThroughCamera());
    _renderer.setLookThroughCamera("/World/NoSuchCamera");
    const QImage fallback = render();
    EXPECT_TRUE(isReddish(pixel(fallback, 128, 128))) << "an unknown camera looks through the free one";
}

TEST_F(SceneViewportStorm, DrawModesAndGridChangeTheImage)
{
    if (_skip) {
        return;
    }
    const QImage unshaded = render();
    const int unshadedCount = countNonBackground(unshaded);
    EXPECT_GT(unshadedCount, 2000);

    _renderer.setDrawMode(SceneViewportRenderer::eDrawModeShaded);
    const QImage shaded = render();
    // Lit head-on by the light at the camera.
    EXPECT_TRUE(isReddish(pixel(shaded, 128, 128))) << "shaded box: " << pixel(shaded, 128, 128);

    _renderer.setDrawMode(SceneViewportRenderer::eDrawModeWireframe);
    const QImage wireframe = render();
    const int wireCount = countNonBackground(wireframe);
    EXPECT_GT(wireCount, 50) << "wireframe drew nothing";
    EXPECT_LT(wireCount, unshadedCount / 2) << "wireframe filled the faces";

    _renderer.setDrawMode(SceneViewportRenderer::eDrawModeUnshaded);
    SceneViewportRenderer::FreeCamera above = _renderer.getFreeCamera();
    above.elevation = 30.;
    _renderer.setFreeCamera(above);
    const int withoutGrid = countNonBackground(render());
    _renderer.setShowGrid(true);
    const int withGrid = countNonBackground(render());
    EXPECT_GT(withGrid, withoutGrid + 100) << "the grid drew nothing";
}

// The viewer's own drawing relies on the GL state it set before the scene view drew.
TEST_F(SceneViewportStorm, RestoresTheGLStateItChanges)
{
    if (_skip) {
        return;
    }
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
    glViewport(3, 5, 70, 90);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_SCISSOR_TEST);
    glScissor(1, 2, 30, 40);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0., 10., 0., 20., -1., 1.);

    while (glGetError() != GL_NO_ERROR) {
    }
    (void)_renderer.render(_fbo->handle(), kSize, kSize);
    _renderer.setShowGrid(true);
    (void)_renderer.render(_fbo->handle(), kSize, kSize);
    (void)_renderer.pick(128, 128, kSize, kSize);

    GLint value = -1;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &value);
    EXPECT_EQ(0, value);
    glGetIntegerv(GL_CURRENT_PROGRAM, &value);
    EXPECT_EQ(0, value);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &value);
    EXPECT_EQ(GL_TEXTURE0, value);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &value);
    EXPECT_EQ((GLint)texture, value);
    GLint viewport[4] = { 0, 0, 0, 0 };
    glGetIntegerv(GL_VIEWPORT, viewport);
    EXPECT_EQ(3, viewport[0]);
    EXPECT_EQ(5, viewport[1]);
    EXPECT_EQ(70, viewport[2]);
    EXPECT_EQ(90, viewport[3]);
    EXPECT_TRUE(glIsEnabled(GL_BLEND));
    EXPECT_FALSE(glIsEnabled(GL_DEPTH_TEST));
    EXPECT_TRUE(glIsEnabled(GL_SCISSOR_TEST));
    EXPECT_FALSE(glIsEnabled(GL_FRAMEBUFFER_SRGB));
    glGetIntegerv(GL_BLEND_SRC_RGB, &value);
    EXPECT_EQ(GL_ONE, value);
    GLint scissor[4] = { 0, 0, 0, 0 };
    glGetIntegerv(GL_SCISSOR_BOX, scissor);
    EXPECT_EQ(30, scissor[2]);
    glGetIntegerv(GL_MATRIX_MODE, &value);
    EXPECT_EQ(GL_PROJECTION, value);
    GLdouble projection[16];
    glGetDoublev(GL_PROJECTION_MATRIX, projection);
    EXPECT_NEAR(2. / 10., projection[0], 1e-6);
    EXPECT_NEAR(2. / 20., projection[5], 1e-6);
    EXPECT_EQ(GLenum(GL_NO_ERROR), glGetError());

    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDeleteTextures(1, &texture);
}

namespace {

// The reference scene with an edit layer on top: a small green cube at (x, -3, 0), or nothing when
// x is NaN, so the stage gains, moves and loses a prim from edit to edit.
ScenePayload
editedScene(const ScenePayload& reference,
            double x,
            int edit)
{
    if (std::isnan(x)) {
        return reference;
    }
    PXR_NS::SdfLayerRefPtr layer = SceneOps::createNodeLayer("SceneViewportEdit");
    const std::string text = "#usda 1.0\n"
                             "over \"World\"\n{\n"
                             "    def Cube \"Edit\"\n    {\n"
                             "        double size = 1\n"
                             "        color3f[] primvars:displayColor = [(0.1, 0.8, 0.2)]\n"
                             "        double3 xformOp:translate = ("
        + std::to_string(x) + ", -3, 0)\n"
                              "        uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                              "    }\n}\n";
    if (!layer->ImportFromString(text)) {
        ADD_FAILURE() << "cannot author edit " << edit;
    }

    return SceneOps::appendLayer(reference, layer, 0xed170000ULL + U64(edit));
}

// Pixels whose channels differ by more than a few levels.
int
countDifferentPixels(const QImage& a,
                     const QImage& b)
{
    if (a.size() != b.size()) {
        return a.width() * a.height();
    }
    int n = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const QColor ca = a.pixelColor(x, y);
            const QColor cb = b.pixelColor(x, y);
            if ((std::abs(ca.red() - cb.red()) > 8) || (std::abs(ca.green() - cb.green()) > 8) || (std::abs(ca.blue() - cb.blue()) > 8)) {
                ++n;
            }
        }
    }

    return n;
}

} // namespace

// The viewer keeps one stage and one engine through every edit: 50 edits that add, move and
// remove a prim reuse the engine made by the first render, the free camera and the selection
// survive them, and every picture matches what a renderer seeing the scene for the first time
// draws.
TEST_F(SceneViewportStorm, FiftyEditsReuseOneEngineAndMatchFreshRenders)
{
    if (_skip) {
        return;
    }
    const ScenePayload reference = referenceScene();
    _renderer.setSelectedPrim("/World/Box");
    (void)render();
    ASSERT_EQ(U64(1), _renderer.getEngineCount());
    const SceneViewportRenderer::FreeCamera camera = _renderer.getFreeCamera();

    for (int edit = 0; edit < 50; ++edit) {
        // Every seventh edit drops the edit layer.
        const double x = ((edit % 7) == 6) ? std::nan("") : (-4. + 0.16 * edit);
        const ScenePayload scene = editedScene(reference, x, edit);
        std::string error;
        ASSERT_TRUE(_renderer.setScene(scene, 1., 24., &error)) << error;
        const QImage edited = render();

        SceneViewportRenderer fresh;
        ASSERT_TRUE(fresh.setScene(scene, 1., 24., &error)) << error;
        fresh.setFreeCamera(camera);
        fresh.setDrawMode(SceneViewportRenderer::eDrawModeUnshaded);
        fresh.setShowGrid(false);
        fresh.setSelectedPrim("/World/Box");
        bool converged = false;
        for (int i = 0; i < 4 && !converged; ++i) {
            converged = fresh.render(_fbo->handle(), kSize, kSize);
        }
        const QImage expected = _fbo->toImage();
        fresh.releaseGL();

        // A few edge pixels may round differently; a moved or stale prim covers hundreds.
        EXPECT_LE(countDifferentPixels(edited, expected), 40) << "edit " << edit;
        if (!std::isnan(x) && (x > -3.) && (x < 3.)) {
            // The cube's centre: x = 128 + 128 X / (10 tan 22.5), y = 128 + 128 * 3 / (10 tan 22.5).
            const int px = int(std::lround(128. + 128. * x / (10. * 0.41421356237 /* tan 22.5 degrees */)));
            EXPECT_GT(pixel(edited, px, 221).g, pixel(edited, px, 221).r + 40) << "the edit cube at edit " << edit << ": " << pixel(edited, px, 221);
        }
    }

    EXPECT_EQ(U64(1), _renderer.getEngineCount());
    EXPECT_EQ(std::string("/World/Box"), _renderer.getSelectedPrim());
    EXPECT_DOUBLE_EQ(camera.distance, _renderer.getFreeCamera().distance);
    EXPECT_DOUBLE_EQ(camera.azimuth, _renderer.getFreeCamera().azimuth);

    // Showing nothing and then a scene again keeps the engine too.
    _renderer.clearScene();
    EXPECT_FALSE(_renderer.hasScene());
    std::string error;
    ASSERT_TRUE(_renderer.setScene(reference, 1., 24., &error)) << error;
    (void)render();
    EXPECT_EQ(U64(1), _renderer.getEngineCount());
}

// A paint does not wait for a sync or a layer reload: while a reload holds the viewer lock through
// SceneWriteBarrier, renderIfIdle() draws nothing, and draws again once it is done.
TEST_F(SceneViewportStorm, PaintSkipsWhileTheViewerLockIsHeld)
{
    if (_skip) {
        return;
    }
    (void)render();
    std::mutex mutex;
    std::condition_variable changed;
    bool writing = false;
    bool painted = false;
    std::string error;
    std::thread writer([&]() {
        SceneWriteBarrier::runExclusive([&]() {
            std::unique_lock<std::mutex> lock(mutex);
            writing = true;
            changed.notify_all();
            changed.wait(lock, [&]() { return painted; });
        },
                                        &error);
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&]() { return writing; });
    }
    EXPECT_EQ(SceneViewportRenderer::eRenderStatusBusy, _renderer.renderIfIdle(_fbo->handle(), kSize, kSize));
    {
        std::lock_guard<std::mutex> lock(mutex);
        painted = true;
    }
    changed.notify_all();
    writer.join();
    EXPECT_TRUE(error.empty()) << error;
    EXPECT_NE(SceneViewportRenderer::eRenderStatusBusy, _renderer.renderIfIdle(_fbo->handle(), kSize, kSize));
}
