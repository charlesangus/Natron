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
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

// Before Qt's OpenGL headers.
#include "Gui/SceneGizmo.h"
#include "Gui/SceneViewport.h"
#include "Tests/TestWatchdog.h"

#include <QColor>
#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QPoint>
#include <QSettings>
#include <QSurfaceFormat>

#include "Engine/AppManager.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Scene/CyclesAovs.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneWriteBarrier.h"
#include "Engine/Nodes/Scene/USDProject3D.h"
#include "Engine/ProjectColorManagement.h"
#include "Engine/Settings.h"
#include "Engine/ViewerInstance.h"
#include "Gui/GuiGLRequirements.h"
#include "Tests/Scene/CyclesTestUtils.h"

#include <pxr/base/gf/vec4f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/driver.h>
#include <pxr/imaging/hdSt/renderDelegate.h>
#include <pxr/imaging/hgi/tokens.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>

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

// base with a layer on top authoring /Render/rs, a RenderSettings prim the stage's
// renderSettingsPrimPath names: natron:renderer when renderer is not empty, and Cycles' and
// MoonRay's samples. hash tells the layers apart.
ScenePayload
withSettingsPrim(const ScenePayload& base,
                 const std::string& renderer,
                 int cyclesSamples,
                 int moonRaySamples,
                 U64 hash)
{
    std::string text = "#usda 1.0\n"
                       "(\n"
                       "    renderSettingsPrimPath = \"/Render/rs\"\n"
                       ")\n"
                       "def Scope \"Render\"\n"
                       "{\n"
                       "    def RenderSettings \"rs\"\n"
                       "    {\n";
    if (!renderer.empty()) {
        text += "        uniform token natron:renderer = \"" + renderer + "\"\n";
    }
    text += "        int cycles:samples = " + std::to_string(cyclesSamples) + "\n"
                                                                              "        bool cycles:integrator:use_adaptive_sampling = 0\n"
                                                                              "        int moonray:sceneVariable:pixel_samples = "
        + std::to_string(moonRaySamples) + "\n"
                                           "    }\n"
                                           "}\n";
    PXR_NS::SdfLayerRefPtr layer = SceneOps::createNodeLayer("SceneViewportSettings");
    if (!layer->ImportFromString(text)) {
        ADD_FAILURE() << "cannot read the render settings layer";

        return base;
    }

    return SceneOps::appendLayer(base, layer, hash);
}

PXR_NS::UsdGeomMesh
defineQuad(const PXR_NS::UsdStageRefPtr& stage,
           const char* path,
           float halfSize,
           float z)
{
    PXR_NS::UsdGeomMesh mesh = PXR_NS::UsdGeomMesh::Define(stage, PXR_NS::SdfPath(path));
    PXR_NS::VtVec3fArray points(4);

    points[0] = PXR_NS::GfVec3f(-halfSize, -halfSize, z);
    points[1] = PXR_NS::GfVec3f(halfSize, -halfSize, z);
    points[2] = PXR_NS::GfVec3f(halfSize, halfSize, z);
    points[3] = PXR_NS::GfVec3f(-halfSize, halfSize, z);
    mesh.CreatePointsAttr(PXR_NS::VtValue(points));
    mesh.CreateFaceVertexCountsAttr(PXR_NS::VtValue(PXR_NS::VtIntArray(1, 4)));
    PXR_NS::VtIntArray indices(4);
    for (int i = 0; i < 4; ++i) {
        indices[i] = i;
    }
    mesh.CreateFaceVertexIndicesAttr(PXR_NS::VtValue(indices));
    mesh.CreateSubdivisionSchemeAttr(PXR_NS::VtValue(PXR_NS::UsdGeomTokens->none));

    return mesh;
}

// /World/Card, a 4-unit square at z = 0, carries the projection material as USDProject3D authors
// it: the plate is a UsdUVTexture without a file, so it shows its fallback, half-transparent red
// in straight colour, as live textures hold it, which the shader premultiplies to (0.6, 0, 0, 0.5). The projector's frame covers the card's left half, x -2..0; Crop on and
// Outside transparent leave the right half undrawn. /World/Backdrop, an opaque 6-unit square at
// z = -1 with an emissive blue preview surface, so it shows without lights, lies behind the whole
// card. From the fixture's free camera the card spans pixels 66..190 and the backdrop 44..212 on
// both axes. Without the backdrop, the card's left half is all that is drawn, half-transparent.
ScenePayload
projectedCardScene(std::string* error,
                   bool withBackdrop = true)
{
    const std::string shaderPath = USDProject3D::getShaderPath();

    if (shaderPath.empty()) {
        *error = "the installation lacks " kUSDProject3DShaderFile;

        return ScenePayload();
    }
    PXR_NS::SdfLayerRefPtr layer = SceneOps::createNodeLayer("SceneViewportProjection");
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(layer);
    if (!stage) {
        *error = "USD could not open a stage on the layer";

        return ScenePayload();
    }

    if (withBackdrop) {
        PXR_NS::UsdGeomMesh backdrop = defineQuad(stage, "/World/Backdrop", 3.f, -1.f);
        const PXR_NS::SdfPath backdropMaterialPath("/World/BackdropMaterial");
        PXR_NS::UsdShadeMaterial backdropMaterial = PXR_NS::UsdShadeMaterial::Define(stage, backdropMaterialPath);
        PXR_NS::UsdShadeShader surface = PXR_NS::UsdShadeShader::Define(stage, backdropMaterialPath.AppendChild(PXR_NS::TfToken("Surface")));
        surface.CreateIdAttr(PXR_NS::VtValue(PXR_NS::TfToken("UsdPreviewSurface")));
        surface.CreateInput(PXR_NS::TfToken("emissiveColor"), PXR_NS::SdfValueTypeNames->Color3f).Set(PXR_NS::GfVec3f(0.f, 0.f, 1.f));
        surface.CreateInput(PXR_NS::TfToken("diffuseColor"), PXR_NS::SdfValueTypeNames->Color3f).Set(PXR_NS::GfVec3f(0.f, 0.f, 0.f));
        backdropMaterial.CreateSurfaceOutput().ConnectToSource(surface.CreateOutput(PXR_NS::TfToken("surface"), PXR_NS::SdfValueTypeNames->Token));
        PXR_NS::UsdShadeMaterialBindingAPI::Apply(backdrop.GetPrim()).Bind(backdropMaterial);
    }
    PXR_NS::UsdGeomMesh card = defineQuad(stage, "/World/Card", 2.f, 0.f);

    const PXR_NS::SdfPath materialPath("/World/Projection");
    PXR_NS::UsdShadeMaterial material = PXR_NS::UsdShadeMaterial::Define(stage, materialPath);
    PXR_NS::UsdShadeShader shader = PXR_NS::UsdShadeShader::Define(stage, materialPath.AppendChild(PXR_NS::TfToken("Project")));
    shader.SetSourceAsset(PXR_NS::SdfAssetPath(shaderPath), PXR_NS::TfToken("glslfx"));
    // Row vectors: u = (x + 2) / 2 and v = (y + 2) / 4, w = 1.
    PXR_NS::GfMatrix4d worldToProjector(0.5, 0., 0., 0.,
                                        0., 0.25, 0., 0.,
                                        0., 0., 1., 0.,
                                        1., 0.5, 0., 1.);
    shader.CreateInput(PXR_NS::TfToken(kUSDProject3DInputWorldToProjector), PXR_NS::SdfValueTypeNames->Matrix4d).Set(worldToProjector);
    shader.CreateInput(PXR_NS::TfToken("projectorOrigin"), PXR_NS::SdfValueTypeNames->Float4).Set(PXR_NS::GfVec4f(-1.f, 0.f, 5.f, 1.f));
    shader.CreateInput(PXR_NS::TfToken(kUSDProject3DInputPlateTransform), PXR_NS::SdfValueTypeNames->Float4).Set(PXR_NS::GfVec4f(1.f, 1.f, 0.f, 0.f));
    const std::pair<const char*, int> ints[] = {
        { kUSDProject3DInputProjectionSpace, 0 },
        { "projectOn", 0 },
        { "crop", 1 },
        { "clip", 0 },
        { "outside", 1 },
    };
    for (const std::pair<const char*, int>& value : ints) {
        shader.CreateInput(PXR_NS::TfToken(value.first), PXR_NS::SdfValueTypeNames->Int).Set(value.second);
    }
    material.CreateSurfaceOutput().ConnectToSource(shader.CreateOutput(PXR_NS::TfToken("surface"), PXR_NS::SdfValueTypeNames->Token));

    PXR_NS::UsdShadeShader reader = PXR_NS::UsdShadeShader::Define(stage, materialPath.AppendChild(PXR_NS::TfToken("StReader")));
    reader.CreateIdAttr(PXR_NS::VtValue(PXR_NS::TfToken("UsdPrimvarReader_float2")));
    reader.CreateInput(PXR_NS::TfToken("varname"), PXR_NS::SdfValueTypeNames->String).Set(std::string("st"));
    PXR_NS::UsdShadeShader texture = PXR_NS::UsdShadeShader::Define(stage, materialPath.AppendChild(PXR_NS::TfToken("Plate")));
    texture.CreateIdAttr(PXR_NS::VtValue(PXR_NS::TfToken("UsdUVTexture")));
    texture.CreateInput(PXR_NS::TfToken("st"), PXR_NS::SdfValueTypeNames->Float2)
        .ConnectToSource(reader.CreateOutput(PXR_NS::TfToken("result"), PXR_NS::SdfValueTypeNames->Float2));
    for (const char* name : { "wrapS", "wrapT" }) {
        texture.CreateInput(PXR_NS::TfToken(name), PXR_NS::SdfValueTypeNames->Token).Set(PXR_NS::TfToken("clamp"));
    }
    texture.CreateInput(PXR_NS::TfToken("fallback"), PXR_NS::SdfValueTypeNames->Float4).Set(PXR_NS::GfVec4f(1.2f, 0.f, 0.f, 0.5f));
    shader.CreateInput(PXR_NS::TfToken("plate"), PXR_NS::SdfValueTypeNames->Float4)
        .ConnectToSource(texture.CreateOutput(PXR_NS::TfToken("rgba"), PXR_NS::SdfValueTypeNames->Float4));

    PXR_NS::UsdShadeMaterialBindingAPI::Apply(card.GetPrim()).Bind(material);

    return SceneOps::appendLayer(ScenePayload(), layer, 0x9c0ec7ULL);
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

int
codeValue(float v)
{
    return int(std::lround(std::min(std::max(v, 0.f), 1.f) * 255.f));
}

float
srgbEncode(float linear)
{
    return (linear <= 0.0031308f) ? 12.92f * linear : 1.055f * std::pow(linear, 1.f / 2.4f) - 0.055f;
}

const char* const kStudioURI = "ocio://studio-config-v4.0.0_aces-v2.0_ocio-v2.5";
const char* const kWorkingSpace = "ACEScg";
const char* const kDisplay = "sRGB - Display";
const char* const kSDRView = "ACES 2.0 - SDR 100 nits (Rec.709)";

class SceneViewportStorm
    : public ::testing::Test {
protected:
    void SetUp() override
    {
        _watchdog.start();
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
        // /World/Cam's guide would draw lines across the frame and win picks near them.
        _renderer.setShowGuides(false);
    }

    void TearDown() override
    {
        _watchdog.setPhase("teardown");
        if (_context && _surface && _context->makeCurrent(_surface.get())) {
            _renderer.releaseGL();
            _fbo.reset();
            _context->doneCurrent();
        }
        _fbo.reset();
        _surface.reset();
        _context.reset();
        _watchdog.stop();
    }

    template <typename Id>
    void setRenderer(const Id& id)
    {
        _watchdog.setPhase("setRenderer");
        _renderer.setRendererId(id);
    }

    bool renderFrame()
    {
        _watchdog.setPhase("render");

        return _renderer.render(_fbo->handle(), kSize, kSize);
    }

    template <typename... Args>
    bool syncScene(Args&&... args)
    {
        _watchdog.setPhase("syncScene");

        return _renderer.syncScene(std::forward<Args>(args)...);
    }

    QImage render()
    {
        bool converged = false;

        for (int i = 0; i < 4 && !converged; ++i) {
            converged = renderFrame();
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

    bool isBackground(const Rgb& c) const
    {
        float rgb[3];

        _renderer.getBackgroundColor(rgb);
        const int grey = codeValue(rgb[0]);

        return (std::abs(c.r - grey) <= 20) && (std::abs(c.g - grey) <= 20) && (std::abs(c.b - grey) <= 20);
    }

    int countNonBackground(const QImage& image) const
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

    // Renders until done says the refinement is where it should be and the render drew its
    // converged frame, the delegate fails, or the time is up. True if both held.
    bool refineUntil(const std::function<bool(const SceneViewportRenderer::RefineStatus&)>& done)
    {
        _watchdog.setPhase("refineTo");
        const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now() + std::chrono::seconds(120);

        while (std::chrono::steady_clock::now() < end) {
            const bool drawn = renderFrame();
            if (!_renderer.getRendererError().empty()) {
                ADD_FAILURE() << _renderer.getRendererError();

                return false;
            }
            if (drawn && done(_renderer.getRefineStatus())) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ADD_FAILURE() << "the refinement did not get there in 120 s";

        return false;
    }

    // The refinement is done at cap samples, and its last frame is the one drawn.
    bool refineTo(int cap)
    {
        return refineUntil([cap](const SceneViewportRenderer::RefineStatus& status) {
            return status.active && status.converged && (status.maxSamples == cap) && (status.samples == cap) && (status.drawnSamples == cap);
        });
    }

    TestWatchdog _watchdog;
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
    _renderer.setSelectedPrims({ "/World/Box" });
    const QImage selected = render();
    const Rgb before = pixel(unselected, 128, 128);
    const Rgb after = pixel(selected, 128, 128);
    EXPECT_GT(std::abs(after.r - before.r) + std::abs(after.g - before.g) + std::abs(after.b - before.b), 30)
        << "highlight left the box unchanged: " << before << " -> " << after;
    EXPECT_LE(std::abs(pixel(unselected, 221, 128).b - pixel(selected, 221, 128).b), 3) << "the ball is not selected";

    _renderer.setSelectedPrims({});
    const QImage cleared = render();
    const Rgb back = pixel(cleared, 128, 128);
    EXPECT_LE(std::abs(back.r - before.r) + std::abs(back.g - before.g) + std::abs(back.b - before.b), 6)
        << "clearing the selection left the highlight: " << before << " -> " << back;
}

// The projection's material is translucent, drawn in Storm's blended pass, which writes no prim
// ids; the pick runs its own id pass over every material tag, which applies no alpha threshold
// to translucent materials, and the projection discards the fragments where it is transparent,
// so the projected card picks wherever it shows and not where it is transparent.
TEST_F(SceneViewportStorm, PicksAndHighlightsAProjectedCard)
{
    if (_skip) {
        return;
    }
    std::string error;
    const ScenePayload scene = projectedCardScene(&error);
    ASSERT_TRUE(error.empty()) << error;
    ASSERT_TRUE(_renderer.setScene(scene, 1., 24., &error)) << error;
    // Unshaded draws display colours and leaves every surface shader out.
    _renderer.setDrawMode(SceneViewportRenderer::eDrawModeShaded);

    const QImage unselected = render();
    const Rgb projected = pixel(unselected, 97, 128);
    const Rgb beside = pixel(unselected, 159, 128);
    EXPECT_TRUE(isBluish(beside)) << "the backdrop where the card is transparent: " << beside;
    // (0.6, 0, 0, 0.5) premultiplied over linear blue is (0.6, 0, 0.5), (203, 0, 188) once in sRGB.
    EXPECT_NEAR(codeValue(srgbEncode(0.6f)), projected.r, 8) << "the projected plate: " << projected;
    EXPECT_NEAR(codeValue(srgbEncode(0.5f)), projected.b, 8) << "the backdrop through the plate: " << projected;
    EXPECT_LT(projected.g, 10) << projected;
    EXPECT_TRUE(isBackground(pixel(unselected, 20, 128))) << pixel(unselected, 20, 128);

    EXPECT_EQ(std::string("/World/Card"), _renderer.pick(97, 128, kSize, kSize));
    EXPECT_EQ(std::string("/World/Card"), _renderer.pick(76, 76, kSize, kSize));
    EXPECT_EQ(std::string("/World/Backdrop"), _renderer.pick(159, 128, kSize, kSize));
    EXPECT_EQ(std::string("/World/Backdrop"), _renderer.pick(97, 200, kSize, kSize));
    EXPECT_EQ(std::string(), _renderer.pick(20, 128, kSize, kSize));

    // Storm's translucent pass draws no selection highlight, so only the backdrop is checked.
    _renderer.setSelectedPrims({ "/World/Card" });
    const QImage selected = render();
    const Rgb besideSelected = pixel(selected, 159, 128);
    EXPECT_LE(std::abs(besideSelected.r - beside.r) + std::abs(besideSelected.g - beside.g) + std::abs(besideSelected.b - beside.b), 6)
        << "the card's transparent half highlighted the backdrop: " << beside << " -> " << besideSelected;
}

// Through the studio config, the card and the background show as the 2D viewer shows images: from
// the working space through the display and view, each pixel as OpenColorIO's CPU processor
// computes it, through a view whose GPU shader is analytic and one that needs LUT textures (the
// ACES 2.0 output transforms). Without a config the sRGB encoding comes back.
TEST_F(SceneViewportStorm, AppliesTheViewersOpenColorIODisplayTransform)
{
    if (_skip) {
        return;
    }
    ProjectColorManagement cm;
    std::string error;
    ASSERT_EQ(ProjectColorManagement::eLoadErrorNone, cm.load(kStudioURI, std::string(), &error)) << error;
    const ScenePayload scene = projectedCardScene(&error);
    ASSERT_TRUE(error.empty()) << error;
    ASSERT_TRUE(_renderer.setScene(scene, 1., 24., &error)) << error;
    _renderer.setDrawMode(SceneViewportRenderer::eDrawModeShaded);

    const Rgb srgb { codeValue(srgbEncode(0.6f)), 0, codeValue(srgbEncode(0.5f)) };

    for (const char* view : { "Un-tone-mapped", kSDRView }) {
        SceneViewportRenderer::DisplayTransform transform;
        transform.config = cm.getConfig();
        transform.colorSpace = kWorkingSpace;
        transform.display = kDisplay;
        transform.view = view;

        OCIO_NAMESPACE::DisplayViewTransformRcPtr displayView = OCIO_NAMESPACE::DisplayViewTransform::Create();
        displayView->setSrc(kWorkingSpace);
        displayView->setDisplay(kDisplay);
        displayView->setView(view);
        OCIO_NAMESPACE::ConstProcessorRcPtr processor = cm.getConfig()->getProcessor(displayView);
        OCIO_NAMESPACE::GpuShaderDescRcPtr shader = OCIO_NAMESPACE::GpuShaderDesc::CreateShaderDesc();
        shader->setLanguage(OCIO_NAMESPACE::GPU_LANGUAGE_GLSL_4_0);
        processor->getDefaultGPUProcessor()->extractGpuShaderInfo(shader);
        // The SDR view must exercise the LUT textures, the un-tone-mapped one the analytic shader.
        ASSERT_EQ(std::string(view) == kSDRView, shader->getNumTextures() > 0) << view;

        _renderer.setDisplayTransform(transform);
        ASSERT_TRUE(_renderer.isUsingOpenColorIO()) << view;

        const OCIO_NAMESPACE::ConstCPUProcessorRcPtr cpu = processor->getDefaultCPUProcessor();
        float plate[3] = { 0.6f, 0.f, 0.5f };
        cpu->applyRGB(plate);
        float background[3] = { 0.05f, 0.05f, 0.05f };
        cpu->applyRGB(background);
        const Rgb expected { codeValue(plate[0]), codeValue(plate[1]), codeValue(plate[2]) };
        // Otherwise the checks below could not tell the view from sRGB.
        ASSERT_GT(std::abs(expected.r - srgb.r) + std::abs(expected.b - srgb.b), 20) << view << ": " << expected;

        float reported[3];
        _renderer.getBackgroundColor(reported);
        EXPECT_NEAR(background[0], reported[0], 1e-4f) << view;

        const QImage image = render();
        const Rgb projected = pixel(image, 97, 128);
        EXPECT_NEAR(expected.r, projected.r, 8) << view << ": " << projected << " expected " << expected;
        EXPECT_NEAR(expected.g, projected.g, 8) << view << ": " << projected << " expected " << expected;
        EXPECT_NEAR(expected.b, projected.b, 8) << view << ": " << projected << " expected " << expected;
        const Rgb outside = pixel(image, 20, 128);
        EXPECT_NEAR(codeValue(background[0]), outside.r, 2) << view << ": " << outside;
        EXPECT_NEAR(codeValue(background[2]), outside.b, 2) << view << ": " << outside;
    }

    _renderer.setDisplayTransform(SceneViewportRenderer::DisplayTransform());
    EXPECT_FALSE(_renderer.isUsingOpenColorIO());
    const Rgb projected = pixel(render(), 97, 128);
    EXPECT_NEAR(srgb.r, projected.r, 8) << projected;
    EXPECT_NEAR(srgb.b, projected.b, 8) << projected;
}

// The viewer's gain and gamma reach the card as applyViewerDisplayTransform(), the 2D viewer's own
// maths, computes them: gain before the display transform, gamma after, a gamma of 0 a threshold
// at 1. The background keeps the plain transform. Through an analytic view, one with LUT textures,
// and, without a config, around the sRGB encoding. Back at 1 they leave the plain transform.
TEST_F(SceneViewportStorm, AppliesTheViewersGainAndGamma)
{
    if (_skip) {
        return;
    }
    ProjectColorManagement cm;
    std::string error;
    ASSERT_EQ(ProjectColorManagement::eLoadErrorNone, cm.load(kStudioURI, std::string(), &error)) << error;
    const ScenePayload scene = projectedCardScene(&error);
    ASSERT_TRUE(error.empty()) << error;
    ASSERT_TRUE(_renderer.setScene(scene, 1., 24., &error)) << error;
    _renderer.setDrawMode(SceneViewportRenderer::eDrawModeShaded);

    struct Case {
        const char* view;
        double gain;
        double gamma;
    };
    const Case cases[] = {
        { "Un-tone-mapped", 0.5, 1.8 },
        { "Un-tone-mapped", 4., 0. },
        { kSDRView, 0.25, 1.8 },
        { kSDRView, 2., 0.6 },
    };

    for (const Case& c : cases) {
        const std::string label = std::string(c.view) + " gain " + std::to_string(c.gain) + " gamma " + std::to_string(c.gamma);
        const ProjectColorManagement::DisplayProcessorPtr processor = cm.getDisplayProcessor(kWorkingSpace, kDisplay, c.view, std::string(), &error);
        ASSERT_TRUE(processor != nullptr) << error;
        float plain[8] = { 0.6f, 0.f, 0.5f, 1.f, 0.05f, 0.05f, 0.05f, 1.f };
        applyViewerDisplayTransform(*processor, plain, 2, 1., 0., 1.);
        float graded[8] = { 0.6f, 0.f, 0.5f, 1.f, 0.05f, 0.05f, 0.05f, 1.f };
        applyViewerDisplayTransform(*processor, graded, 2, c.gain, 0., c.gamma);
        const Rgb plainPlate { codeValue(plain[0]), codeValue(plain[1]), codeValue(plain[2]) };
        const Rgb expected { codeValue(graded[0]), codeValue(graded[1]), codeValue(graded[2]) };
        // Otherwise the checks below could not tell gain and gamma from their absence.
        ASSERT_GT(std::abs(expected.r - plainPlate.r) + std::abs(expected.b - plainPlate.b), 20) << label << ": " << expected;

        SceneViewportRenderer::DisplayTransform transform;
        transform.config = cm.getConfig();
        transform.colorSpace = kWorkingSpace;
        transform.display = kDisplay;
        transform.view = c.view;
        transform.gain = c.gain;
        transform.gamma = c.gamma;
        _renderer.setDisplayTransform(transform);
        ASSERT_TRUE(_renderer.isUsingOpenColorIO()) << label;

        float reported[3];
        _renderer.getBackgroundColor(reported);
        EXPECT_NEAR(std::min(std::max(plain[4], 0.f), 1.f), reported[0], 1e-4f) << label;

        const QImage image = render();
        const Rgb projected = pixel(image, 97, 128);
        EXPECT_NEAR(expected.r, projected.r, 8) << label << ": " << projected << " expected " << expected;
        EXPECT_NEAR(expected.g, projected.g, 8) << label << ": " << projected << " expected " << expected;
        EXPECT_NEAR(expected.b, projected.b, 8) << label << ": " << projected << " expected " << expected;
        const Rgb outside = pixel(image, 20, 128);
        EXPECT_NEAR(codeValue(plain[4]), outside.r, 2) << label << ": " << outside;
        EXPECT_NEAR(codeValue(plain[6]), outside.b, 2) << label << ": " << outside;

        transform.gain = 1.;
        transform.gamma = 1.;
        _renderer.setDisplayTransform(transform);
        ASSERT_TRUE(_renderer.isUsingOpenColorIO()) << label;
        const Rgb restored = pixel(render(), 97, 128);
        EXPECT_NEAR(plainPlate.r, restored.r, 8) << label << ": " << restored << " expected " << plainPlate;
        EXPECT_NEAR(plainPlate.b, restored.b, 8) << label << ": " << restored << " expected " << plainPlate;
    }

    SceneViewportRenderer::DisplayTransform srgbGraded;
    srgbGraded.gain = 0.5;
    srgbGraded.gamma = 1.8;
    _renderer.setDisplayTransform(srgbGraded);
    EXPECT_FALSE(_renderer.isUsingOpenColorIO());
    const QImage srgbImage = render();
    const Rgb graded = pixel(srgbImage, 97, 128);
    EXPECT_NEAR(codeValue(std::pow(srgbEncode(0.3f), 1.f / 1.8f)), graded.r, 8) << graded;
    EXPECT_NEAR(codeValue(std::pow(srgbEncode(0.25f), 1.f / 1.8f)), graded.b, 8) << graded;
    const Rgb srgbOutside = pixel(srgbImage, 20, 128);
    EXPECT_NEAR(codeValue(srgbEncode(0.05f)), srgbOutside.r, 2) << srgbOutside;

    _renderer.setDisplayTransform(SceneViewportRenderer::DisplayTransform());
    EXPECT_FALSE(_renderer.isUsingOpenColorIO());
    const Rgb plain = pixel(render(), 97, 128);
    EXPECT_NEAR(codeValue(srgbEncode(0.6f)), plain.r, 8) << plain;
    EXPECT_NEAR(codeValue(srgbEncode(0.5f)), plain.b, 8) << plain;
}

namespace {

// A config with a view that adds 0.25 to everything, and a look that halves, in a linear space
// that is the reference.
OCIO_NAMESPACE::ConstConfigRcPtr
offsetConfig()
{
    OCIO_NAMESPACE::ConfigRcPtr config = OCIO_NAMESPACE::Config::CreateRaw()->createEditableCopy();
    OCIO_NAMESPACE::ColorSpaceRcPtr linear = OCIO_NAMESPACE::ColorSpace::Create();
    linear->setName("linear");
    config->addColorSpace(linear);

    OCIO_NAMESPACE::ColorSpaceRcPtr offset = OCIO_NAMESPACE::ColorSpace::Create();
    offset->setName("offset");
    OCIO_NAMESPACE::MatrixTransformRcPtr add = OCIO_NAMESPACE::MatrixTransform::Create();
    const double offsets[4] = { 0.25, 0.25, 0.25, 0. };
    add->setOffset(offsets);
    offset->setTransform(add, OCIO_NAMESPACE::COLORSPACE_DIR_FROM_REFERENCE);
    config->addColorSpace(offset);
    config->addDisplayView("Test", "Offset", "offset", "");

    OCIO_NAMESPACE::LookRcPtr half = OCIO_NAMESPACE::Look::Create();
    half->setName("half");
    half->setProcessSpace("linear");
    OCIO_NAMESPACE::MatrixTransformRcPtr scale = OCIO_NAMESPACE::MatrixTransform::Create();
    const double matrix[16] = { 0.5, 0., 0., 0., 0., 0.5, 0., 0., 0., 0., 0.5, 0., 0., 0., 0., 1. };
    scale->setMatrix(matrix);
    half->setTransform(scale);
    config->addLook(half);
    config->validate();

    return config;
}

SceneViewportRenderer::DisplayTransform
offsetTransform(const OCIO_NAMESPACE::ConstConfigRcPtr& config)
{
    SceneViewportRenderer::DisplayTransform transform;
    transform.config = config;
    transform.colorSpace = "linear";
    transform.display = "Test";
    transform.view = "Offset";

    return transform;
}

} // namespace

// A view that lifts black, and the gamma after it, change what was rendered and leave the
// background where nothing was as the plain view shows it, with nothing taken off it. A look goes
// through to both.
TEST_F(SceneViewportStorm, KeepsTheBackgroundUngradedThroughAnOffsetViewAndALook)
{
    if (_skip) {
        return;
    }
    std::string error;
    const ScenePayload scene = projectedCardScene(&error);
    ASSERT_TRUE(error.empty()) << error;
    ASSERT_TRUE(_renderer.setScene(scene, 1., 24., &error)) << error;
    _renderer.setDrawMode(SceneViewportRenderer::eDrawModeShaded);
    const OCIO_NAMESPACE::ConstConfigRcPtr config = offsetConfig();

    SceneViewportRenderer::DisplayTransform transform = offsetTransform(config);
    transform.gamma = 4.;
    _renderer.setDisplayTransform(transform);
    ASSERT_TRUE(_renderer.isUsingOpenColorIO());
    float reported[3];
    _renderer.getBackgroundColor(reported);
    EXPECT_NEAR(0.3f, reported[0], 1e-4f);
    QImage image = render();
    Rgb projected = pixel(image, 97, 128);
    EXPECT_NEAR(codeValue(std::pow(0.85f, 0.25f)), projected.r, 8) << projected;
    EXPECT_NEAR(codeValue(std::pow(0.25f, 0.25f)), projected.g, 8) << projected;
    EXPECT_NEAR(codeValue(std::pow(0.75f, 0.25f)), projected.b, 8) << projected;
    Rgb outside = pixel(image, 20, 128);
    EXPECT_NEAR(codeValue(0.3f), outside.r, 2) << outside;
    EXPECT_NEAR(codeValue(0.3f), outside.g, 2) << outside;

    transform.gamma = 1.;
    transform.look = "half";
    _renderer.setDisplayTransform(transform);
    ASSERT_TRUE(_renderer.isUsingOpenColorIO());
    _renderer.getBackgroundColor(reported);
    EXPECT_NEAR(0.275f, reported[0], 1e-4f);
    image = render();
    projected = pixel(image, 97, 128);
    EXPECT_NEAR(codeValue(0.55f), projected.r, 8) << projected;
    EXPECT_NEAR(codeValue(0.25f), projected.g, 8) << projected;
    EXPECT_NEAR(codeValue(0.5f), projected.b, 8) << projected;
    outside = pixel(image, 20, 128);
    EXPECT_NEAR(codeValue(0.275f), outside.r, 2) << outside;
}

// A half-transparent card over nothing but the background: its colour is graded unpremultiplied,
// as the 2D viewer grades an image, and lies over the ungraded background by its alpha, through
// the sRGB encoding and through a view that lifts black.
TEST_F(SceneViewportStorm, GradesHalfTransparentPixelsUnpremultipliedOverTheBackground)
{
    if (_skip) {
        return;
    }
    std::string error;
    const ScenePayload scene = projectedCardScene(&error, false);
    ASSERT_TRUE(error.empty()) << error;
    ASSERT_TRUE(_renderer.setScene(scene, 1., 24., &error)) << error;
    _renderer.setDrawMode(SceneViewportRenderer::eDrawModeShaded);
    // The plate's straight colour and alpha.
    const float red = 1.2f;
    const float alpha = 0.5f;
    const double gain = 0.5;
    const double gamma = 1.8;

    SceneViewportRenderer::DisplayTransform srgb;
    srgb.gain = gain;
    srgb.gamma = gamma;
    _renderer.setDisplayTransform(srgb);
    float background[3];
    _renderer.getBackgroundColor(background);
    QImage image = render();
    Rgb projected = pixel(image, 97, 128);
    const float srgbRed = std::pow(srgbEncode(red * (float)gain), 1.f / (float)gamma);
    EXPECT_NEAR(codeValue(alpha * srgbRed + (1.f - alpha) * background[0]), projected.r, 6) << projected;
    EXPECT_NEAR(codeValue((1.f - alpha) * background[1]), projected.g, 6) << projected;
    EXPECT_NEAR(codeValue((1.f - alpha) * background[2]), projected.b, 6) << projected;

    SceneViewportRenderer::DisplayTransform offset = offsetTransform(offsetConfig());
    offset.gain = gain;
    offset.gamma = gamma;
    _renderer.setDisplayTransform(offset);
    ASSERT_TRUE(_renderer.isUsingOpenColorIO());
    _renderer.getBackgroundColor(background);
    image = render();
    projected = pixel(image, 97, 128);
    const float offsetRed = std::pow(red * (float)gain + 0.25f, 1.f / (float)gamma);
    const float offsetBlack = std::pow(0.25f, 1.f / (float)gamma);
    EXPECT_NEAR(codeValue(alpha * offsetRed + (1.f - alpha) * background[0]), projected.r, 6) << projected;
    EXPECT_NEAR(codeValue(alpha * offsetBlack + (1.f - alpha) * background[1]), projected.g, 6) << projected;
    const Rgb outside = pixel(image, 20, 128);
    EXPECT_NEAR(codeValue(background[0]), outside.r, 2) << outside;
}

TEST_F(SceneViewportStorm, PicksThePrimsInARectangleAndHighlightsThemAll)
{
    if (_skip) {
        return;
    }
    const QImage unselected = render();
    EXPECT_EQ(std::vector<std::string>({ "/World/Ball", "/World/Box" }), _renderer.pickRect(0, 0, kSize - 1, kSize - 1, kSize, kSize));
    // Corners in any order.
    EXPECT_EQ(std::vector<std::string>({ "/World/Box" }), _renderer.pickRect(150, 150, 100, 100, kSize, kSize));
    EXPECT_EQ(std::vector<std::string>({ "/World/Ball" }), _renderer.pickRect(200, 110, 240, 140, kSize, kSize));
    EXPECT_TRUE(_renderer.pickRect(2, 2, 20, 20, kSize, kSize).empty());

    _renderer.setSelectedPrims({ "/World/Box", "/World/Ball" });
    const QImage selected = render();
    for (const QPoint& p : { QPoint(128, 128), QPoint(221, 128) }) {
        const Rgb before = pixel(unselected, p.x(), p.y());
        const Rgb after = pixel(selected, p.x(), p.y());
        EXPECT_GT(std::abs(after.r - before.r) + std::abs(after.g - before.g) + std::abs(after.b - before.b), 30)
            << "highlight left (" << p.x() << ", " << p.y() << ") unchanged: " << before << " -> " << after;
    }
}

TEST_F(SceneViewportStorm, FramesTheSelection)
{
    if (_skip) {
        return;
    }
    (void)render();

    // The ball, off to the right, ends up at the centre of the view.
    _renderer.setSelectedPrims({ "/World/Ball" });
    _renderer.frameSelection();
    (void)render();
    EXPECT_EQ(std::string("/World/Ball"), _renderer.pick(kSize / 2, kSize / 2, kSize, kSize));
    EXPECT_GT(_renderer.getFreeCamera().pivot[0], 1.);

    // A camera has no extent: its position is framed with a radius of one unit.
    _renderer.setSelectedPrims({ "/World/Cam" });
    _renderer.frameSelection();
    const SceneViewportRenderer::FreeCamera onCamera = _renderer.getFreeCamera();
    EXPECT_NEAR(1., onCamera.pivot[0], 1e-9);
    EXPECT_NEAR(1., onCamera.pivot[1], 1e-9);
    EXPECT_NEAR(10., onCamera.pivot[2], 1e-9);
    EXPECT_NEAR(1. / std::sin(onCamera.fovY * M_PI / 360.) * 1.05, onCamera.distance, 1e-9);

    // With nothing selected it frames everything.
    _renderer.frameScene();
    const SceneViewportRenderer::FreeCamera all = _renderer.getFreeCamera();
    _renderer.setSelectedPrims({ "/World/Ball" });
    _renderer.frameSelection();
    _renderer.setSelectedPrims({});
    _renderer.frameSelection();
    for (int i = 0; i < 3; ++i) {
        EXPECT_DOUBLE_EQ(all.pivot[i], _renderer.getFreeCamera().pivot[i]);
    }
    EXPECT_DOUBLE_EQ(all.distance, _renderer.getFreeCamera().distance);
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

// A locked camera's move is drawn from a transform the renderer is handed, with the stage
// untouched. Moved to (3, 0, 10), unturned, /World/Cam has the ball (at (3, 0, 0) at timecode 1)
// at the centre of the frame, and the box leaves (69, 187), where it is from (1, 1, 10).
TEST_F(SceneViewportStorm, DrawsALookedThroughCameraFromAGivenTransform)
{
    if (_skip) {
        return;
    }
    _renderer.setLookThroughCamera("/World/Cam");
    EXPECT_FALSE(_renderer.hasLookThroughTransform());
    const QImage atPrim = render();
    EXPECT_TRUE(isReddish(pixel(atPrim, 69, 187))) << "the box through /World/Cam: " << pixel(atPrim, 69, 187);
    SceneViewportRenderer::CameraPose primPose;
    ASSERT_TRUE(_renderer.getViewPose(kSize, kSize, &primPose));
    EXPECT_DOUBLE_EQ(1., primPose.cameraToWorld[12]);
    EXPECT_DOUBLE_EQ(1., primPose.cameraToWorld[13]);
    EXPECT_DOUBLE_EQ(10., primPose.cameraToWorld[14]);

    const PXR_NS::GfMatrix4d moved = PXR_NS::GfMatrix4d(1.).SetTranslate(PXR_NS::GfVec3d(3., 0., 10.));
    _renderer.setLookThroughTransform(moved.data());
    EXPECT_TRUE(_renderer.hasLookThroughTransform());
    EXPECT_EQ(std::string("/World/Cam"), _renderer.getLookThroughCamera());
    const QImage atMove = render();
    EXPECT_TRUE(isBluish(pixel(atMove, 128, 128))) << "the ball through the moved camera: " << pixel(atMove, 128, 128);
    EXPECT_TRUE(isBackground(pixel(atMove, 69, 187))) << "the box left the frame: " << pixel(atMove, 69, 187);
    EXPECT_EQ(std::string("/World/Ball"), _renderer.pick(128, 128, kSize, kSize));
    SceneViewportRenderer::CameraPose movedPose;
    ASSERT_TRUE(_renderer.getViewPose(kSize, kSize, &movedPose));
    EXPECT_DOUBLE_EQ(3., movedPose.cameraToWorld[12]);
    EXPECT_DOUBLE_EQ(0., movedPose.cameraToWorld[13]);
    EXPECT_DOUBLE_EQ(10., movedPose.cameraToWorld[14]);
    EXPECT_DOUBLE_EQ(primPose.viewHeight, movedPose.viewHeight) << "the lens is the prim's";

    _renderer.clearLookThroughTransform();
    EXPECT_FALSE(_renderer.hasLookThroughTransform());
    EXPECT_TRUE(isReddish(pixel(render(), 69, 187))) << "back at the prim's transform";

    // Looking through again, or through another camera, forgets the transform.
    _renderer.setLookThroughTransform(moved.data());
    _renderer.setLookThroughCamera("/World/Cam");
    EXPECT_FALSE(_renderer.hasLookThroughTransform());
    _renderer.setLookThroughTransform(moved.data());
    _renderer.tumble(0., 0.);
    _renderer.setLookThroughCamera("/World/Cam");
    EXPECT_FALSE(_renderer.hasLookThroughTransform());
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
    // The fixture has no light, and the view adds none: the shaded box is black against the grey.
    const Rgb unlit = pixel(shaded, 128, 128);
    EXPECT_TRUE((unlit.r < 40) && (unlit.g < 40) && (unlit.b < 40)) << "shaded box with no light: " << unlit;
    EXPECT_FALSE(isBackground(unlit)) << "shaded box with no light: " << unlit;

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

// The fixture has no light: the shaded box is black under Scene Lights, the default, lit under
// Headlight and Auto, and black again back under Scene Lights, all with the same engine.
TEST_F(SceneViewportStorm, TheHeadlightLightsTheShadedSceneOnlyWhenChosen)
{
    if (_skip) {
        return;
    }
    EXPECT_EQ(SceneViewportRenderer::eLightingModeScene, _renderer.getLightingMode()) << "the headlight is off by default";
    _renderer.setDrawMode(SceneViewportRenderer::eDrawModeShaded);

    const Rgb off = pixel(render(), 128, 128);
    EXPECT_TRUE((off.r < 40) && (off.g < 40) && (off.b < 40)) << "shaded box, scene lights: " << off;
    EXPECT_TRUE(_renderer.isSceneLit());

    _renderer.setLightingMode(SceneViewportRenderer::eLightingModeHeadlight);
    const Rgb on = pixel(render(), 128, 128);
    EXPECT_TRUE(isReddish(on)) << "shaded box, headlight: " << on;
    EXPECT_FALSE(_renderer.isSceneLit());

    _renderer.setLightingMode(SceneViewportRenderer::eLightingModeAuto);
    const Rgb automatic = pixel(render(), 128, 128);
    EXPECT_TRUE(isReddish(automatic)) << "shaded box, auto with no scene light: " << automatic;

    _renderer.setLightingMode(SceneViewportRenderer::eLightingModeScene);
    const Rgb offAgain = pixel(render(), 128, 128);
    EXPECT_TRUE((offAgain.r < 40) && (offAgain.g < 40) && (offAgain.b < 40)) << "shaded box, scene lights again: " << offAgain;
    EXPECT_EQ(U64(1), _renderer.getEngineCount()) << "changing the lighting keeps the engine";
}

// The 3D views' lighting is the Viewer 3D Lighting preference: Scene Lights (no headlight) by
// default, and saved and restored like any other.
TEST(SceneViewportLighting, ThePreferenceIsSceneLightsByDefaultAndSavesAndLoads)
{
    SettingsPtr settings = appPTR->getCurrentSettings();
    ASSERT_TRUE(bool(settings));
    KnobChoicePtr knob = settings->getKnobByNameAndType<KnobChoice>("viewer3DLighting");
    ASSERT_TRUE(bool(knob));
    EXPECT_EQ((int)Settings::eViewer3DLightingScene, knob->getDefaultValue(0));
    EXPECT_EQ(std::string("Viewer 3D Lighting"), knob->getLabel());

    const bool wasSaving = settings->getSaveSettings();
    const Settings::Viewer3DLightingEnum was = settings->getViewer3DLighting();
    settings->setSaveSettings(true);
    settings->setViewer3DLighting(Settings::eViewer3DLightingHeadlight);
    {
        QSettings saved(QString::fromUtf8(NATRON_ORGANIZATION_NAME), QString::fromUtf8(NATRON_APPLICATION_NAME));
        EXPECT_TRUE(saved.contains(QString::fromUtf8("viewer3DLighting")));
    }
    knob->setValue((int)Settings::eViewer3DLightingScene);
    ASSERT_EQ(Settings::eViewer3DLightingScene, settings->getViewer3DLighting());
    settings->restoreKnobsFromSettings(std::vector<KnobI*>(1, knob.get()));
    EXPECT_EQ(Settings::eViewer3DLightingHeadlight, settings->getViewer3DLighting()) << "the saved lighting is restored";

    {
        QSettings saved(QString::fromUtf8(NATRON_ORGANIZATION_NAME), QString::fromUtf8(NATRON_APPLICATION_NAME));
        saved.remove(QString::fromUtf8("viewer3DLighting"));
    }
    knob->setValue((int)was);
    settings->setSaveSettings(wasSaving);
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
    (void)renderFrame();
    _renderer.setShowGrid(true);
    (void)renderFrame();
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
bool
isGreenish(const Rgb& c)
{
    return (c.g > 90) && (c.g > c.r + 40) && (c.g > c.b + 40);
}

bool
isYellowish(const Rgb& c)
{
    return (c.r > 150) && (c.g > 120) && (c.b < c.g - 60);
}

// Whether any pixel of the 3x3 block centred on (x, y) passes test: lines are a few pixels wide
// and smoothed, so the exact pixel a line covers depends on rasterisation.
bool
anyNear(const QImage& image,
        int x,
        int y,
        bool (*test)(const Rgb&))
{
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const QColor c = image.pixelColor(x + dx, y + dy);
            if (test(Rgb { c.red(), c.green(), c.blue() })) {
                return true;
            }
        }
    }

    return false;
}

// The reference scene with an identity transform op named Gizmo appended to /World/Box.
ScenePayload
sceneWithOp()
{
    PXR_NS::SdfLayerRefPtr layer = SceneOps::createNodeLayer("SceneViewportGizmoTest");
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(layer);
    PXR_NS::UsdGeomXformable box(stage->OverridePrim(PXR_NS::SdfPath("/World/Box")));

    box.AddTransformOp(PXR_NS::UsdGeomXformOp::PrecisionDouble, PXR_NS::TfToken("Gizmo")).Set(PXR_NS::GfMatrix4d(1.));

    return SceneOps::appendLayer(referenceScene(), layer, 0x91220ULL);
}
} // namespace

// From the free camera the handles of an op at the origin are 90 pixels long: the X arrow runs
// right from the centre along row 128, the Y arrow up along column 128, and the ring about Z, of
// radius 63 pixels, faces the camera. The rings about X and Y are seen edge-on, as lines along
// column 128 and row 128 under the arrows.
TEST_F(SceneViewportStorm, DrawsTransformHandlesOverTheScene)
{
    if (_skip) {
        return;
    }
    std::string error;
    ASSERT_TRUE(_renderer.setScene(sceneWithOp(), 1., 24., &error)) << error;
    SceneViewportRenderer::TransformHandles handles;
    handles.primPath = "/World/Box";
    handles.opName = "xformOp:transform:Gizmo";
    _renderer.setTransformHandles(handles);
    ASSERT_TRUE(_renderer.hasTransformHandles());

    QImage image = render();
    EXPECT_TRUE(anyNear(image, 185, 128, isReddish)) << "X arrow: " << pixel(image, 185, 128);
    EXPECT_TRUE(anyNear(image, 128, 50, isGreenish)) << "Y arrow: " << pixel(image, 128, 50);
    EXPECT_TRUE(anyNear(image, 172, 83, isBluish)) << "Z ring: " << pixel(image, 172, 83);
    EXPECT_TRUE(isBackground(pixel(image, 40, 200))) << "away from the handles: " << pixel(image, 40, 200);
    // The box is still drawn under them.
    EXPECT_TRUE(isReddish(pixel(image, 110, 145))) << "box: " << pixel(image, 110, 145);

    // Picking agrees with the drawing.
    EXPECT_EQ(SceneGizmo::makeHandle(SceneGizmo::eHandleKindTranslate, 0), _renderer.hitTestTransformHandles(185.5, 128., kSize, kSize));
    EXPECT_EQ(SceneGizmo::makeHandle(SceneGizmo::eHandleKindTranslate, 1), _renderer.hitTestTransformHandles(128., 50.5, kSize, kSize));
    EXPECT_EQ(SceneGizmo::kNoHandle, _renderer.hitTestTransformHandles(40., 200., kSize, kSize));

    // A highlighted handle is drawn yellow.
    _renderer.setHighlightedTransformHandle(SceneGizmo::makeHandle(SceneGizmo::eHandleKindTranslate, 1));
    image = render();
    EXPECT_TRUE(anyNear(image, 128, 50, isYellowish)) << "highlighted Y arrow: " << pixel(image, 128, 50);
    EXPECT_FALSE(anyNear(image, 128, 50, isGreenish)) << "highlighted Y arrow: " << pixel(image, 128, 50);
    EXPECT_TRUE(anyNear(image, 185, 128, isReddish)) << "X arrow: " << pixel(image, 185, 128);
    _renderer.setHighlightedTransformHandle(SceneGizmo::kNoHandle);

    // No handles for an op the stage does not have.
    handles.opName = "xformOp:transform:Missing";
    _renderer.setTransformHandles(handles);
    EXPECT_FALSE(_renderer.hasTransformHandles());
    image = render();
    EXPECT_TRUE(isBackground(pixel(image, 128, 50))) << "no Y arrow: " << pixel(image, 128, 50);
    EXPECT_EQ(SceneGizmo::kNoHandle, _renderer.hitTestTransformHandles(185.5, 128., kSize, kSize));

    _renderer.clearTransformHandles();
    EXPECT_FALSE(_renderer.hasTransformHandles());
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
    _renderer.setSelectedPrims({ "/World/Box" });
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
        fresh.setShowGuides(false);
        fresh.setSelectedPrims({ "/World/Box" });
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
    EXPECT_EQ(std::vector<std::string>({ "/World/Box" }), _renderer.getSelectedPrims());
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

// Switching the viewer to a delegate that cannot load shows the loader's error instead of a
// frame and keeps the Storm engine; switching back draws with that engine.
TEST_F(SceneViewportStorm, SwitchesToAnUnavailableRendererAndBackToStorm)
{
    if (_skip) {
        return;
    }
    const std::string stub = "NatronTestStubRendererPlugin";
    EXPECT_GT(countNonBackground(render()), 0);
    ASSERT_TRUE(_renderer.hasEngine());
    EXPECT_EQ(std::string(kRenderDelegateStormId), _renderer.getEngineRendererId());
    const U64 engines = _renderer.getEngineCount();

    setRenderer(stub);
    EXPECT_TRUE(_renderer.hasEngine());
    EXPECT_TRUE(renderFrame());
    EXPECT_EQ(std::string(kRenderDelegateStormId), _renderer.getEngineRendererId());
    EXPECT_EQ(engines, _renderer.getEngineCount());
    EXPECT_NE(std::string::npos, _renderer.getRendererError().find(stub)) << _renderer.getRendererError();
    EXPECT_EQ(0, countNonBackground(_fbo->toImage()));

    setRenderer(kRenderDelegateStormId);
    EXPECT_TRUE(_renderer.getRendererError().empty());
    EXPECT_GT(countNonBackground(render()), 0);
    ASSERT_TRUE(_renderer.hasEngine());
    EXPECT_EQ(std::string(kRenderDelegateStormId), _renderer.getEngineRendererId());
    EXPECT_EQ(engines, _renderer.getEngineCount());
}

// The Storm engine kept while another delegate renders still hears the stage: edits made away
// from Storm show on the first frame back, drawn by the same engine, as a renderer seeing the
// edited scene for the first time draws it. Releasing the Storm engine alone leaves the other
// delegate chosen, and the next Storm frame makes a new engine.
TEST_F(SceneViewportStorm, TheStormEngineKeptBehindAnotherRendererFollowsItsEdits)
{
    if (_skip) {
        return;
    }
    const std::string stub = "NatronTestStubRendererPlugin";
    const ScenePayload reference = referenceScene();
    (void)render();
    ASSERT_TRUE(_renderer.hasEngine());
    const U64 engines = _renderer.getEngineCount();

    setRenderer(stub);
    (void)renderFrame();
    const ScenePayload scene = editedScene(reference, 1., 0);
    std::string error;
    ASSERT_TRUE(_renderer.setScene(scene, 1., 24., &error)) << error;
    (void)renderFrame();

    setRenderer(kRenderDelegateStormId);
    const QImage back = render();
    EXPECT_EQ(engines, _renderer.getEngineCount());

    SceneViewportRenderer fresh;
    ASSERT_TRUE(fresh.setScene(scene, 1., 24., &error)) << error;
    fresh.setFreeCamera(_renderer.getFreeCamera());
    fresh.setDrawMode(SceneViewportRenderer::eDrawModeUnshaded);
    fresh.setShowGrid(false);
    fresh.setShowGuides(false);
    bool converged = false;
    for (int i = 0; i < 4 && !converged; ++i) {
        converged = fresh.render(_fbo->handle(), kSize, kSize);
    }
    const QImage expected = _fbo->toImage();
    fresh.releaseGL();
    // A few edge pixels may round differently; a stale prim covers hundreds.
    EXPECT_LE(countDifferentPixels(back, expected), 40);
    // The edit cube's centre: x = 128 + 128 / (10 tan 22.5), y = 128 + 128 * 3 / (10 tan 22.5).
    const int px = int(std::lround(128. + 128. / (10. * 0.41421356237)));
    EXPECT_GT(pixel(back, px, 221).g, pixel(back, px, 221).r + 40) << pixel(back, px, 221);

    setRenderer(stub);
    _renderer.releaseStormEngine();
    EXPECT_FALSE(_renderer.hasEngine());
    EXPECT_EQ(stub, _renderer.getActiveRendererId());
    setRenderer(kRenderDelegateStormId);
    EXPECT_GT(countNonBackground(render()), 0);
    EXPECT_EQ(engines + 1, _renderer.getEngineCount());
}

// Scene Default renders with the renderer the stage's active render settings prim names
// (natron:renderer), Storm without one, and follows the prim as the scene changes.
TEST_F(SceneViewportStorm, SceneDefaultFollowsThePrimsRenderer)
{
    if (_skip) {
        return;
    }
    const std::string stub = "NatronTestStubRendererPlugin";
    const ScenePayload base = referenceScene();
    std::string error;
    ASSERT_TRUE(syncScene(base, &error)) << error;

    setRenderer(kRenderDelegateSceneDefaultId);
    EXPECT_EQ(std::string(kRenderDelegateSceneDefaultId), _renderer.getRendererId());
    EXPECT_GT(countNonBackground(render()), 0);
    EXPECT_EQ(std::string(kRenderDelegateStormId), _renderer.getActiveRendererId());

    // A prim without natron:renderer: still Storm.
    ASSERT_TRUE(syncScene(withSettingsPrim(base, std::string(), 4, 4, 0x5e771ULL), &error)) << error;
    EXPECT_GT(countNonBackground(render()), 0);
    EXPECT_EQ(std::string(kRenderDelegateStormId), _renderer.getActiveRendererId());

    // A prim naming a renderer that is not installed: the view says so instead of drawing.
    ASSERT_TRUE(syncScene(withSettingsPrim(base, stub, 4, 4, 0x5e772ULL), &error)) << error;
    EXPECT_TRUE(renderFrame());
    EXPECT_EQ(stub, _renderer.getActiveRendererId());
    EXPECT_EQ(std::string(kRenderDelegateSceneDefaultId), _renderer.getRendererId());
    EXPECT_EQ(std::string(kRenderDelegateStormId), _renderer.getEngineRendererId());
    EXPECT_NE(std::string::npos, _renderer.getRendererError().find(stub)) << _renderer.getRendererError();

    // The prim gone, Storm again.
    ASSERT_TRUE(syncScene(base, &error)) << error;
    EXPECT_GT(countNonBackground(render()), 0);
    EXPECT_EQ(std::string(kRenderDelegateStormId), _renderer.getActiveRendererId());
    EXPECT_TRUE(_renderer.getRendererError().empty()) << _renderer.getRendererError();

    // A renderer chosen beats the prim's.
    ASSERT_TRUE(syncScene(withSettingsPrim(base, stub, 4, 4, 0x5e773ULL), &error)) << error;
    setRenderer(kRenderDelegateStormId);
    EXPECT_GT(countNonBackground(render()), 0);
    EXPECT_EQ(std::string(kRenderDelegateStormId), _renderer.getActiveRendererId());
}

// The active prim's settings for the viewer's renderer reach its refinement: Cycles refines to
// the prim's cycles:samples, Max Samples applies over them, an edit of MoonRay's settings alone
// does not restart the refinement, and one of Cycles' makes it take the new samples.
TEST_F(SceneViewportStorm, ThePrimsSettingsReachTheRefinement)
{
    if (_skip || !CyclesTestUtils::cyclesOrSkip()) {
        return;
    }
    const ScenePayload base = referenceScene();
    std::string error;
    ASSERT_TRUE(syncScene(withSettingsPrim(base, std::string(), 6, 64, 0x5e781ULL), &error)) << error;
    setRenderer(kRenderDelegateCyclesId);
    ASSERT_TRUE(refineTo(6));

    _renderer.setMaxSamples(3);
    ASSERT_TRUE(refineTo(3));
    const U64 restarts = _renderer.getRefineStatus().restarts;
    const U64 frames = _renderer.getRefineStatus().frames;

    // The MoonRay tab's edit: only /Render changes, in a namespace Cycles does not read.
    ASSERT_TRUE(syncScene(withSettingsPrim(base, std::string(), 6, 128, 0x5e782ULL), &error)) << error;
    const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < end) {
        (void)renderFrame();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(restarts, _renderer.getRefineStatus().restarts);
    EXPECT_EQ(frames, _renderer.getRefineStatus().frames);
    EXPECT_TRUE(_renderer.getRefineStatus().converged);

    // The Cycles tab's edit, without the cap: the prim's new samples.
    _renderer.setMaxSamples(0);
    ASSERT_TRUE(syncScene(withSettingsPrim(base, std::string(), 9, 128, 0x5e783ULL), &error)) << error;
    ASSERT_TRUE(refineTo(9));
    EXPECT_GT(_renderer.getRefineStatus().restarts, restarts);
}

// The frame count starts over with each delegate, so a frame after a switch can have the serial of
// the one in the texture: it must be drawn all the same.
TEST_F(SceneViewportStorm, TheRefinementDrawsItsNewFrameAfterASwitchOfRenderer)
{
    if (_skip || !CyclesTestUtils::cyclesOrSkip()) {
        return;
    }
    _renderer.setMaxSamples(1);
    setRenderer(kRenderDelegateCyclesId);
    ASSERT_TRUE(refineTo(1));
    const QImage first = _fbo->toImage();

    setRenderer(kRenderDelegateStormId);
    (void)render();
    SceneViewportRenderer::FreeCamera camera = _renderer.getFreeCamera();
    camera.azimuth = 60.;
    _renderer.setFreeCamera(camera);
    setRenderer(kRenderDelegateCyclesId);
    ASSERT_TRUE(refineTo(1));
    EXPECT_NE(first, _fbo->toImage());
}

// A throw on the refinement's worker shows as the renderer's error only until the next edit.
TEST_F(SceneViewportStorm, ARefinementFailureClearsAtTheNextSceneEdit)
{
    if (_skip || !CyclesTestUtils::cyclesOrSkip()) {
        return;
    }
    const ScenePayload base = referenceScene();
    std::string error;
    ASSERT_TRUE(syncScene(withSettingsPrim(base, std::string(), 6, 64, 0x5e791ULL), &error)) << error;
    std::atomic<bool> armed(true);
    RenderDelegates::setRenderFaultHook([&armed]() {
        if (armed.exchange(false)) {
            throw std::runtime_error("texture commit failed");
        }
    });
    setRenderer(kRenderDelegateCyclesId);
    const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (_renderer.getRendererError().empty() && (std::chrono::steady_clock::now() < end)) {
        (void)renderFrame();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    RenderDelegates::setRenderFaultHook(nullptr);
    EXPECT_NE(std::string::npos, _renderer.getRendererError().find("texture commit failed")) << _renderer.getRendererError();

    ASSERT_TRUE(syncScene(withSettingsPrim(base, std::string(), 9, 64, 0x5e792ULL), &error)) << error;
    while (!_renderer.getRendererError().empty() && (std::chrono::steady_clock::now() < end)) {
        (void)renderFrame();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(refineTo(9));
}

// A guide Storm picks at a pixel is picked there on a delegate's frame too.
TEST_F(SceneViewportStorm, ARefinementPicksAGuide)
{
    if (_skip || !CyclesTestUtils::cyclesOrSkip()) {
        return;
    }
    // The fixture's camera sits in the plane of the default view's eye, so its guide would project
    // behind or outside the frame; pulled back, it is on screen and in front of the box.
    SceneViewportRenderer::FreeCamera camera = _renderer.getFreeCamera();
    camera.distance = 30.;
    _renderer.setFreeCamera(camera);
    _renderer.setGuideSize(4.);
    _renderer.setShowGuides(true);
    _renderer.setRecordDrawnGuides(true);
    (void)render();
    SceneViewportRenderer::DrawnGuide aimed;
    for (const SceneViewportRenderer::DrawnGuide& guide : _renderer.getDrawnGuides()) {
        if (_renderer.pick((int)guide.x, (int)guide.y, kSize, kSize) == guide.path) {
            aimed = guide;
            break;
        }
    }
    ASSERT_FALSE(aimed.path.empty()) << "no guide Storm can pick";

    _renderer.setMaxSamples(1);
    setRenderer(kRenderDelegateCyclesId);
    ASSERT_TRUE(refineTo(1));
    EXPECT_EQ(aimed.path, _renderer.pick((int)aimed.x, (int)aimed.y, kSize, kSize));
}

TEST_F(SceneViewportStorm, AnExceptionFromHydraDiscardsTheEngineAndTheNextRenderMakesAnother)
{
    if (_skip) {
        return;
    }
    EXPECT_GT(countNonBackground(render()), 0);
    ASSERT_TRUE(_renderer.hasEngine());
    const U64 engines = _renderer.getEngineCount();

    RenderDelegates::setRenderFaultHook([]() { throw std::runtime_error("texture commit failed"); });
    EXPECT_TRUE(renderFrame());
    RenderDelegates::setRenderFaultHook(nullptr);

    EXPECT_FALSE(_renderer.hasEngine());
    const std::string error = _renderer.takeRenderError();
    EXPECT_NE(std::string::npos, error.find("texture commit failed")) << error;
    EXPECT_TRUE(_renderer.takeRenderError().empty());

    EXPECT_GT(countNonBackground(render()), 0);
    ASSERT_TRUE(_renderer.hasEngine());
    EXPECT_EQ(engines + 1, _renderer.getEngineCount());
}

// applyStormPtexBudget reaches the engine's resource registry through a delegate of its own given
// the engine's Hgi, which only works while Storm shares one registry per Hgi. Neither the engine's
// delegate nor the registry's per-type memory request can be read back, so the sharing is all that
// can be checked.
TEST_F(SceneViewportStorm, DelegatesOnOneHgiShareAResourceRegistry)
{
    if (_skip) {
        return;
    }
    PXR_NS::UsdImagingGLEngine::Parameters parameters;
    parameters.rendererPluginId = PXR_NS::TfToken(kRenderDelegateStormId);
    std::unique_ptr<PXR_NS::UsdImagingGLEngine> engine(new PXR_NS::UsdImagingGLEngine(parameters));
    PXR_NS::Hgi* hgi = engine->GetHgi();
    ASSERT_TRUE(hgi != nullptr);

    {
        // The delegates release their registry in their destructors, which needs the Hgi the
        // engine owns, so they must be gone before the engine is.
        PXR_NS::HdDriver driver { PXR_NS::HgiTokens->renderDriver, PXR_NS::VtValue(hgi) };
        PXR_NS::HdStRenderDelegate first;
        PXR_NS::HdStRenderDelegate second;
        first.SetDrivers({ &driver });
        second.SetDrivers({ &driver });

        ASSERT_TRUE(first.GetResourceRegistry() != nullptr);
        EXPECT_EQ(first.GetResourceRegistry().get(), second.GetResourceRegistry().get());
    }

    engine.reset();
}
