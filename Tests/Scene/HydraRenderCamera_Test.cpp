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

#include "Global/GlobalDefines.h"
#include "Global/Macros.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include "Engine/Nodes/Scene/CyclesAovs.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/MoonRayAovs.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/RenderSettingsPrim.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_USING

// A 4:3 camera looking down -z at a wall of quads of distinct colours, nearer and farther ones
// mixed, spread past what any fit of it shows: each fit and image aspect frames a different part.
namespace {
const char* const kCamera = "/World/Cam";
const char* const kSceneHead = "#usda 1.0\n"
                               "(\n"
                               "    defaultPrim = \"World\"\n"
                               "    upAxis = \"Y\"\n"
                               ")\n"
                               "def Xform \"World\"\n"
                               "{\n"
                               "    def Camera \"Cam\"\n"
                               "    {\n"
                               "        float focalLength = 35\n"
                               "        float horizontalAperture = 24\n"
                               "        float verticalAperture = 18\n"
                               "        float2 clippingRange = (0.5, 50)\n"
                               "        double3 xformOp:translate = (0.3, -0.2, 10)\n"
                               "        uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                               "    }\n";

// The wall, children of /World after the camera: a quad per cell of an 8 x 8 grid 1.25 units
// apart, each its own colour, every other one a unit nearer.
std::string
wall()
{
    std::string text;
    for (int row = 0; row < 8; ++row) {
        for (int column = 0; column < 8; ++column) {
            const double x = (column - 3.5) * 1.25;
            const double y = (row - 3.5) * 1.25;
            const double z = ((row + column) % 2) ? 1. : 0.;
            const double h = 0.55;
            char quad[1024];
            std::snprintf(quad, sizeof(quad),
                          "    def Mesh \"Q%d_%d\"\n"
                          "    {\n"
                          "        int[] faceVertexCounts = [4]\n"
                          "        int[] faceVertexIndices = [0, 1, 2, 3]\n"
                          "        point3f[] points = [(%g, %g, %g), (%g, %g, %g), (%g, %g, %g), (%g, %g, %g)]\n"
                          "        color3f[] primvars:displayColor = [(%g, %g, %g)]\n"
                          "    }\n",
                          row, column,
                          x - h, y - h, z, x + h, y - h, z, x + h, y + h, z, x - h, y + h, z,
                          (column + 1) / 8., (row + 1) / 8., ((row * 8 + column) % 5) / 4.);
            text += quad;
        }
    }

    return text;
}

// The dev image's /usr/local glvnd lists only NVIDIA; the system directory holds Mesa. Set before
// the first EGL use, as the render thread's context is made once.
void
pointEGLAtMesa()
{
    if (!std::getenv("__EGL_VENDOR_LIBRARY_FILENAMES") && !std::getenv("__EGL_VENDOR_LIBRARY_DIRS") && (::access("/usr/share/glvnd/egl_vendor.d", F_OK) == 0) && (::access("/usr/local/share/glvnd/egl_vendor.d", F_OK) == 0)) {
        ::setenv("__EGL_VENDOR_LIBRARY_DIRS", "/usr/share/glvnd/egl_vendor.d", 1);
    }
}

// True when the test must stop: no 4.5 context could be made, which fails the test only under
// NATRON_TEST_REQUIRE_GL=1. The bundled gtest has no GTEST_SKIP.
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
    std::printf("[  SKIPPED ] Storm: %s\n", error.c_str());

    return true;
}

class HydraRenderCameraTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        _layer = PXR_NS::SdfLayer::CreateAnonymous(".usda");
        ASSERT_TRUE(_layer->ImportFromString(std::string(kSceneHead) + wall() + "}\n"));
        _scene = SceneOps::appendLayer(ScenePayload(), _layer, 1);
        _stage = PXR_NS::UsdStage::Open(_layer);
        ASSERT_TRUE(_stage);
    }

    void TearDown() override
    {
        HydraRender::release(this);
    }

    // The request for width x height pixels pixelAspect wide through kCamera per fit, through its
    // path or its frustum.
    bool request(HydraRender::FitModeEnum fit,
                 int width,
                 int height,
                 bool throughPath,
                 HydraRender::Request* out,
                 double pixelAspect = 1.)
    {
        out->owner = this;
        out->scene = _scene;
        out->time = PXR_NS::UsdTimeCode(1.);
        out->width = width;
        out->height = height;
        out->headlight = true;
        std::string error;
        if (!HydraRender::cameraFrustum(_stage, PXR_NS::SdfPath(kCamera), out->time, width * pixelAspect / height, fit, &out->frustum, &error)) {
            ADD_FAILURE() << error;

            return false;
        }
        if (throughPath) {
            out->camera = PXR_NS::SdfPath(kCamera);
            out->fit = fit;
            out->pixelAspect = pixelAspect;
        }

        return true;
    }

    bool render(HydraRender::FitModeEnum fit,
                int width,
                int height,
                bool throughPath,
                HydraRender::Result* result,
                double pixelAspect = 1.)
    {
        HydraRender::Request r;
        if (!request(fit, width, height, throughPath, &r, pixelAspect)) {
            return false;
        }
        std::string error;
        if (!HydraRender::render(r, result, &error)) {
            ADD_FAILURE() << (throughPath ? "through the path: " : "through the frustum: ") << error;

            return false;
        }

        return true;
    }

    PXR_NS::SdfLayerRefPtr _layer;
    ScenePayload _scene;
    PXR_NS::UsdStageRefPtr _stage;
};

std::size_t
countDifferent(const std::vector<float>& a,
               const std::vector<float>& b)
{
    if (a.size() != b.size()) {
        return std::max(a.size(), b.size());
    }
    std::size_t different = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) {
            ++different;
        }
    }

    return different;
}

bool
anyLit(const std::vector<float>& rgba)
{
    for (std::size_t i = 3; i < rgba.size(); i += 4) {
        if (rgba[i] > 0.f) {
            return true;
        }
    }

    return false;
}
} // anon namespace

// Storm frames the scene camera by its path exactly as by the frustum cameraFrustum() gives for
// it, at every fit, for an image wider and one narrower than the aperture; one engine goes from
// one to the other and back, the frustum render after the path's matching the first.
TEST_F(HydraRenderCameraTest, ThePathFramesAsTheFrustum)
{
    if (skipWithoutGL()) {
        return;
    }
    const HydraRender::FitModeEnum fits[] = { HydraRender::eFitModeFill, HydraRender::eFitModeHorizontal, HydraRender::eFitModeVertical, HydraRender::eFitModeExpand };
    const char* const fitNames[] = { "fill", "horizontal", "vertical", "expand" };
    const int sizes[][2] = { { 64, 32 }, { 32, 56 } };
    for (const int* size : sizes) {
        std::vector<float> expandRgba;
        std::vector<float> fillRgba;
        for (int f = 0; f < 4; ++f) {
            const std::string what = std::string(fitNames[f]) + " at " + std::to_string(size[0]) + "x" + std::to_string(size[1]);
            HydraRender::Result frustum;
            ASSERT_TRUE(render(fits[f], size[0], size[1], false, &frustum)) << what;
            HydraRender::Result path;
            ASSERT_TRUE(render(fits[f], size[0], size[1], true, &path)) << what;
            HydraRender::Result frustumAgain;
            ASSERT_TRUE(render(fits[f], size[0], size[1], false, &frustumAgain)) << what;

            EXPECT_TRUE(anyLit(frustum.rgba)) << what << ": nothing rendered";
            EXPECT_EQ(0u, countDifferent(frustum.rgba, path.rgba)) << what << ": colour";
            EXPECT_EQ(0u, countDifferent(frustum.depth, path.depth)) << what << ": depth";
            EXPECT_EQ(0u, countDifferent(frustum.rgba, frustumAgain.rgba)) << what << ": colour after the path";
            if (fits[f] == HydraRender::eFitModeExpand) {
                expandRgba = path.rgba;
            } else if (fits[f] == HydraRender::eFitModeFill) {
                fillRgba = path.rgba;
            }
        }
        EXPECT_NE(0u, countDifferent(expandRgba, fillRgba)) << "the fit does not reach the path render at " << size[0] << "x" << size[1];
    }
}

// Pixels twice as wide as they are tall: Storm frames the scene camera by its path as by the
// frustum cameraFrustum() gives for the image's aspect, pixels included, at every fit.
TEST_F(HydraRenderCameraTest, ThePathFramesAsTheFrustumWithWidePixels)
{
    if (skipWithoutGL()) {
        return;
    }
    const HydraRender::FitModeEnum fits[] = { HydraRender::eFitModeFill, HydraRender::eFitModeHorizontal, HydraRender::eFitModeVertical, HydraRender::eFitModeExpand };
    const char* const fitNames[] = { "fill", "horizontal", "vertical", "expand" };
    const double pixelAspect = 2.;
    std::vector<float> squareRgba;
    for (int f = 0; f < 4; ++f) {
        const std::string what = std::string(fitNames[f]) + " at 32x32, pixel aspect 2";
        HydraRender::Result frustum;
        ASSERT_TRUE(render(fits[f], 32, 32, false, &frustum, pixelAspect)) << what;
        HydraRender::Result path;
        ASSERT_TRUE(render(fits[f], 32, 32, true, &path, pixelAspect)) << what;

        EXPECT_TRUE(anyLit(frustum.rgba)) << what << ": nothing rendered";
        EXPECT_EQ(0u, countDifferent(frustum.rgba, path.rgba)) << what << ": colour";
        EXPECT_EQ(0u, countDifferent(frustum.depth, path.depth)) << what << ": depth";
        if (fits[f] == HydraRender::eFitModeExpand) {
            HydraRender::Result square;
            ASSERT_TRUE(render(fits[f], 32, 32, true, &square)) << what;
            EXPECT_NE(0u, countDifferent(square.rgba, path.rgba)) << what << ": the pixel aspect does not reach the path render";
        }
    }
}

// A path that names no camera of the scene fails the render, naming the path.
TEST_F(HydraRenderCameraTest, APathToNoCameraFails)
{
    if (skipWithoutGL()) {
        return;
    }
    HydraRender::Request r;
    ASSERT_TRUE(request(HydraRender::eFitModeExpand, 32, 32, true, &r));
    r.camera = PXR_NS::SdfPath("/World/Nothing");
    HydraRender::Result result;
    std::string error;
    EXPECT_FALSE(HydraRender::render(r, &result, &error));
    EXPECT_NE(std::string::npos, error.find("/World/Nothing")) << error;
}

// A camera prim that is inactive fails the render, naming the path, rather than rendering with no
// camera.
TEST_F(HydraRenderCameraTest, AnInactiveCameraFails)
{
    if (skipWithoutGL()) {
        return;
    }
    HydraRender::Request r;
    ASSERT_TRUE(request(HydraRender::eFitModeExpand, 32, 32, true, &r));
    ASSERT_TRUE(_stage->GetPrimAtPath(PXR_NS::SdfPath(kCamera)).SetActive(false));
    HydraRender::Result result;
    std::string error;
    EXPECT_FALSE(HydraRender::render(r, &result, &error));
    EXPECT_NE(std::string::npos, error.find(kCamera)) << error;
    EXPECT_NE(std::string::npos, error.find("inactive")) << error;
}

// The render settings' pixel aspect decides, not one worked back from the image's aspect: a
// 2048x1556 format of square pixels, whose aspect times 1556 / 2048 is not exactly 1 in doubles,
// reaches Cycles and MoonRay by path.
TEST(HydraRenderCameraSelection, ASquarePixelFormatRendersThroughTheCamera)
{
    const PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::CreateAnonymous(".usda");
    ASSERT_TRUE(layer->ImportFromString("#usda 1.0\n"
                                        "def RenderSettings \"Settings\"\n"
                                        "{\n"
                                        "    uniform int2 resolution = (2048, 1556)\n"
                                        "    uniform float pixelAspectRatio = 1\n"
                                        "}\n"));
    const PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(layer);
    ASSERT_TRUE(stage);
    const RenderSettingsPrim::StandardAttributes standard = RenderSettingsPrim::standardAttributes(PXR_NS::UsdRenderSettings(stage->GetPrimAtPath(PXR_NS::SdfPath("/Settings"))));
    ASSERT_EQ(PXR_NS::GfVec2i(2048, 1556), standard.resolution);
    const char* const ids[] = { kRenderDelegateCyclesId, kMoonRayRendererId };
    for (int i = 0; i < 2; ++i) {
        EXPECT_TRUE(HydraRender::rendersThroughCamera(ids[i], HydraRender::eFitModeExpand, standard.pixelAspectRatio)) << ids[i];
    }
}

// Storm and MoonRay frame by the camera at any fit and pixel aspect; Cycles ignores the window
// policy and the pixels' aspect, so only the expand fit of square pixels may reach it by path.
TEST(HydraRenderCameraSelection, StormAndMoonRayHonourEveryFraming)
{
    const HydraRender::FitModeEnum fits[] = { HydraRender::eFitModeFill, HydraRender::eFitModeHorizontal, HydraRender::eFitModeVertical, HydraRender::eFitModeExpand };
    const char* const framing[] = { kRenderDelegateStormId, kMoonRayRendererId };
    for (int i = 0; i < 2; ++i) {
        for (int f = 0; f < 4; ++f) {
            EXPECT_TRUE(HydraRender::rendersThroughCamera(framing[i], fits[f], 1.)) << framing[i];
            EXPECT_TRUE(HydraRender::rendersThroughCamera(framing[i], fits[f], 2.)) << framing[i];
        }
    }
    EXPECT_TRUE(HydraRender::rendersThroughCamera(kRenderDelegateCyclesId, HydraRender::eFitModeExpand, 1.));
    EXPECT_FALSE(HydraRender::rendersThroughCamera(kRenderDelegateCyclesId, HydraRender::eFitModeExpand, 2.));
    EXPECT_FALSE(HydraRender::rendersThroughCamera(kRenderDelegateCyclesId, HydraRender::eFitModeFill, 1.));
    EXPECT_FALSE(HydraRender::rendersThroughCamera(kRenderDelegateCyclesId, HydraRender::eFitModeHorizontal, 1.));
    EXPECT_FALSE(HydraRender::rendersThroughCamera(kRenderDelegateCyclesId, HydraRender::eFitModeVertical, 1.));
}
