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

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include "Global/Macros.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include <pxr/base/gf/rect2i.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/garch/glApi.h>
#include <pxr/imaging/hgi/tokens.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>

#include "Engine/Nodes/Scene/HeadlessGLContext.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/StormGLSection.h"

NATRON_NAMESPACE_USING

namespace {
const char* const kFixture = NATRON_TESTS_FIXTURES_DIR "/scene-reference.usda";
const int kWidth = 160;
const int kHeight = 80;
const int kViewerRenders = 40;
const int kOffscreenRenders = 16;

void
pointEGLAtMesa()
{
    if (!std::getenv("__EGL_VENDOR_LIBRARY_FILENAMES") && !std::getenv("__EGL_VENDOR_LIBRARY_DIRS") && (::access("/usr/share/glvnd/egl_vendor.d", F_OK) == 0) && (::access("/usr/local/share/glvnd/egl_vendor.d", F_OK) == 0)) {
        ::setenv("__EGL_VENDOR_LIBRARY_DIRS", "/usr/share/glvnd/egl_vendor.d", 1);
    }
}

bool
skipWithoutGL()
{
    pointEGLAtMesa();
    std::string error;
    if (!HydraRender::contextDescription(&error).empty()) {
        return false;
    }
    const char* require = std::getenv("NATRON_TEST_REQUIRE_GL");
    if (require && (std::strcmp(require, "1") == 0)) {
        ADD_FAILURE() << error << " (NATRON_TEST_REQUIRE_GL=1)";

        return true;
    }
    std::cout << "[  SKIPPED ] StormGLSection: " << error << std::endl;

    return true;
}

// Lights over the fixture's cube, ball and camera: a key, a fill sphere light and a dome.
PXR_NS::SdfLayerRefPtr
makeLightsLayer()
{
    PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::CreateAnonymous("lights.usda");
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(layer);

    PXR_NS::UsdLuxDistantLight key = PXR_NS::UsdLuxDistantLight::Define(stage, PXR_NS::SdfPath("/World/Key"));
    key.CreateIntensityAttr().Set(2.f);
    PXR_NS::UsdGeomXformable(key.GetPrim()).AddRotateXYZOp().Set(PXR_NS::GfVec3f(-40.f, 30.f, 0.f));
    PXR_NS::UsdLuxSphereLight fill = PXR_NS::UsdLuxSphereLight::Define(stage, PXR_NS::SdfPath("/World/Fill"));
    fill.CreateIntensityAttr().Set(30.f);
    fill.CreateRadiusAttr().Set(0.5f);
    PXR_NS::UsdGeomXformable(fill.GetPrim()).AddTranslateOp().Set(PXR_NS::GfVec3d(-3., 3., 4.));
    PXR_NS::UsdLuxDomeLight dome = PXR_NS::UsdLuxDomeLight::Define(stage, PXR_NS::SdfPath("/World/Dome"));
    dome.CreateIntensityAttr().Set(0.3f);

    return layer;
}

PXR_NS::GfFrustum
cameraFrustum(const PXR_NS::UsdStageRefPtr& stage)
{
    PXR_NS::GfFrustum frustum;
    std::string error;

    EXPECT_TRUE(HydraRender::cameraFrustum(stage, PXR_NS::SdfPath("/World/Cam"), PXR_NS::UsdTimeCode(1), double(kWidth) / kHeight, HydraRender::eFitModeFill, &frustum, &error)) << error;

    return frustum;
}

/**
 * The scene view's way of drawing, on a thread and context of its own: a UsdImagingGLEngine
 * with its own HgiGL presenting into a framebuffer, read back as 8-bit RGBA.
 **/
class ViewerDraw {
public:
    explicit ViewerDraw(const PXR_NS::UsdStageRefPtr& stage)
        : _stage(stage)
        , _frustum(cameraFrustum(stage))
    {
    }

    bool open(std::string* error)
    {
        _context = HeadlessGLContext::create(error);
        if (!_context) {
            return false;
        }
        if (!PXR_NS::GarchGLApiLoad()) {
            *error = "GL entry points could not be loaded in the viewer's context";

            return false;
        }
        glGenFramebuffers(1, &_framebuffer);
        glGenRenderbuffers(2, _renderbuffers);
        glBindFramebuffer(GL_FRAMEBUFFER, _framebuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, _renderbuffers[0]);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, kWidth, kHeight);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, _renderbuffers[0]);
        glBindRenderbuffer(GL_RENDERBUFFER, _renderbuffers[1]);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, kWidth, kHeight);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, _renderbuffers[1]);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            *error = "the viewer's framebuffer is incomplete";

            return false;
        }

        return true;
    }

    // Draws the stage through its camera into *pixels. False on a GL error or outside a section.
    bool draw(std::vector<unsigned char>* pixels,
              std::string* error)
    {
        StormGLSection section;
        if (!_engine) {
            _engine.reset(new PXR_NS::UsdImagingGLEngine(PXR_NS::UsdImagingGLEngine::Parameters()));
        }
        section.collectWith(_engine->GetHgi());
        if (!StormGLSection::isHeldOnThisThread()) {
            *error = "the viewer drew outside a Storm section";

            return false;
        }
        while (glGetError() != GL_NO_ERROR) {
        }
        glBindFramebuffer(GL_FRAMEBUFFER, _framebuffer);
        glViewport(0, 0, kWidth, kHeight);
        glClearColor(0.f, 0.f, 0.f, 0.f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        _engine->SetRenderBufferSize(PXR_NS::GfVec2i(kWidth, kHeight));
        _engine->SetFraming(PXR_NS::CameraUtilFraming(PXR_NS::GfRect2i(PXR_NS::GfVec2i(0, 0), kWidth, kHeight)));
        _engine->SetCameraState(_frustum.ComputeViewMatrix(), _frustum.ComputeProjectionMatrix());
        _engine->SetPresentationOutput(PXR_NS::HgiTokens->OpenGL, PXR_NS::VtValue(uint32_t(_framebuffer)));
        PXR_NS::UsdImagingGLRenderParams params;
        params.frame = PXR_NS::UsdTimeCode(1);
        params.enableLighting = true;
        params.enableSceneLights = true;
        params.enableSceneMaterials = true;
        params.clearColor = PXR_NS::GfVec4f(0.f, 0.f, 0.f, 0.f);
        for (int pass = 0; pass < 8; ++pass) {
            _engine->Render(_stage->GetPseudoRoot(), params);
            if (_engine->IsConverged()) {
                break;
            }
        }
        pixels->assign((std::size_t)kWidth * kHeight * 4, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, _framebuffer);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels->data());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        const GLenum glError = glGetError();
        if (glError != GL_NO_ERROR) {
            *error = "GL error " + std::to_string(glError) + " in the viewer's draw";

            return false;
        }

        return true;
    }

    // Lets the engine go in the context it was made in, as SceneViewportRenderer::releaseGL() does.
    void close()
    {
        if (_context && _context->makeCurrent()) {
            StormGLSection section;
            _engine.reset();
            glDeleteRenderbuffers(2, _renderbuffers);
            glDeleteFramebuffers(1, &_framebuffer);
        } else {
            (void)_engine.release();
        }
        _context.reset();
    }

private:
    PXR_NS::UsdStageRefPtr _stage;
    PXR_NS::GfFrustum _frustum;
    std::unique_ptr<HeadlessGLContext> _context;
    std::unique_ptr<PXR_NS::UsdImagingGLEngine> _engine;
    GLuint _framebuffer = 0;
    GLuint _renderbuffers[2] = { 0, 0 };
};

int
maxByteDifference(const std::vector<unsigned char>& a,
                  const std::vector<unsigned char>& b)
{
    if (a.size() != b.size()) {
        return 256;
    }
    int worst = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        worst = std::max(worst, std::abs(int(a[i]) - int(b[i])));
    }

    return worst;
}

float
maxFloatDifference(const std::vector<float>& a,
                   const std::vector<float>& b)
{
    if (a.size() != b.size()) {
        return std::numeric_limits<float>::infinity();
    }
    float worst = 0.f;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::isinf(a[i]) && std::isinf(b[i])) {
            continue;
        }
        worst = std::max(worst, std::abs(a[i] - b[i]));
    }

    return worst;
}

std::size_t
countLit(const std::vector<float>& rgba)
{
    std::size_t lit = 0;
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        if (rgba[i] + rgba[i + 1] + rgba[i + 2] > 0.05f) {
            ++lit;
        }
    }

    return lit;
}
} // anonymous namespace

TEST(StormGLSection, NestsOnAThreadAndCountsOutermostSections)
{
    EXPECT_FALSE(StormGLSection::isHeldOnThisThread());
    const std::uint64_t sections = StormGLSection::getSectionCount();
    std::atomic<bool> entered(false);
    std::thread other;
    {
        StormGLSection outer;
        EXPECT_TRUE(StormGLSection::isHeldOnThisThread());
        {
            StormGLSection inner;
            EXPECT_TRUE(StormGLSection::isHeldOnThisThread());
        }
        EXPECT_TRUE(StormGLSection::isHeldOnThisThread());
        other = std::thread([&entered]() {
            EXPECT_FALSE(StormGLSection::isHeldOnThisThread());
            StormGLSection section;
            entered = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        EXPECT_FALSE(entered.load()) << "another thread entered while this one held a section";
    }
    EXPECT_FALSE(StormGLSection::isHeldOnThisThread());
    other.join();
    EXPECT_TRUE(entered.load());
    EXPECT_EQ(sections + 2, StormGLSection::getSectionCount());
}

/**
 * USDRender's render thread and a scene view on another thread both drawing through HgiGL, whose
 * garbage collector every HgiGL shares. The USDRender engines are released after each render so
 * that their buffers, textures and pipelines become garbage while the viewer is drawing. Every
 * render must match the one each side made alone, with no GL error.
 **/
TEST(StormGLSection, USDRenderAndASceneViewDrawAtOnceOnTwoThreads)
{
    if (skipWithoutGL()) {
        return;
    }
    const PXR_NS::SdfLayerRefPtr lights = makeLightsLayer();
    PXR_NS::SdfLayerRefPtr fixture = PXR_NS::SdfLayer::FindOrOpen(kFixture);
    ASSERT_TRUE(fixture);
    PXR_NS::UsdStageRefPtr viewerStage = PXR_NS::UsdStage::Open(fixture, lights);
    ASSERT_TRUE(viewerStage);

    HydraRender::Request request;
    request.scene = SceneOps::appendLayer(SceneOps::appendLayer(ScenePayload(), fixture, 1), lights, 2);
    request.time = PXR_NS::UsdTimeCode(1);
    request.width = kWidth;
    request.height = kHeight;
    request.frustum = cameraFrustum(viewerStage);
    const int owners[2] = { 0, 0 };

    std::string error;
    HydraRender::Result reference;
    request.owner = &owners[0];
    ASSERT_TRUE(HydraRender::render(request, &reference, &error)) << error;
    HydraRender::release(&owners[0]);
    ASSERT_GT(countLit(reference.rgba), 200u) << "the lights light the scene";

    std::vector<unsigned char> viewerReference;
    std::atomic<int> viewerDraws(0);
    std::atomic<int> viewerMismatches(0);
    std::atomic<bool> viewerFailed(false);
    std::atomic<bool> viewerReady(false);
    std::atomic<bool> offscreenDone(false);
    std::string viewerError;
    int worstViewer = 0;
    std::thread viewer([&]() {
        ViewerDraw draw(viewerStage);
        if (!draw.open(&viewerError) || !draw.draw(&viewerReference, &viewerError)) {
            viewerFailed = true;
            viewerReady = true;
            draw.close();

            return;
        }
        viewerReady = true;
        std::vector<unsigned char> pixels;
        for (int i = 0; (i < kViewerRenders) || !offscreenDone.load(); ++i) {
            if (!draw.draw(&pixels, &viewerError)) {
                viewerFailed = true;
                break;
            }
            ++viewerDraws;
            const int difference = maxByteDifference(viewerReference, pixels);
            worstViewer = std::max(worstViewer, difference);
            if (difference > 2) {
                ++viewerMismatches;
            }
            if (i > 10 * kViewerRenders) {
                break;
            }
            // A view repaints at a frame rate, leaving the render thread room between draws.
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        draw.close();
    });
    while (!viewerReady.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    const std::uint64_t sectionsBefore = StormGLSection::getSectionCount();
    const std::uint64_t collectionsBefore = StormGLSection::getCollectionCount();
    int offscreenMismatches = 0;
    float worstColour = 0.f;
    bool offscreenFailed = false;
    for (int i = 0; i < kOffscreenRenders; ++i) {
        HydraRender::Result result;
        request.owner = &owners[i % 2];
        if (!HydraRender::render(request, &result, &error)) {
            offscreenFailed = true;
            break;
        }
        const float colour = maxFloatDifference(reference.rgba, result.rgba);
        worstColour = std::max(worstColour, colour);
        if ((colour > 1e-3f) || (maxFloatDifference(reference.depth, result.depth) > 1e-3f) || (std::memcmp(reference.primId.data(), result.primId.data(), reference.primId.size() * sizeof(float)) != 0)) {
            ++offscreenMismatches;
        }
        HydraRender::release(request.owner);
    }
    offscreenDone = true;
    viewer.join();
    // Drains the releases.
    EXPECT_EQ(0u, HydraRender::getEngineCount());

    ASSERT_FALSE(viewerFailed.load()) << viewerError;
    ASSERT_FALSE(offscreenFailed) << error;
    EXPECT_FALSE(StormGLSection::isHeldOnThisThread());
    EXPECT_GE(viewerDraws.load(), kViewerRenders);
    EXPECT_EQ(0, viewerMismatches.load()) << "worst byte difference " << worstViewer;
    EXPECT_EQ(0, offscreenMismatches) << "worst colour difference " << worstColour;
    // Each USDRender render, read and release ran in sections that collected with its Hgi.
    EXPECT_GE(StormGLSection::getSectionCount() - sectionsBefore, (std::uint64_t)kOffscreenRenders * 3);
    EXPECT_GE(StormGLSection::getCollectionCount() - collectionsBefore, (std::uint64_t)kOffscreenRenders * 3);
}
