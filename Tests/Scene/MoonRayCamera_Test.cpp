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
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <pxr/base/plug/registry.h>

#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/MoonRayAovs.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_USING

namespace {
const char* const kCamera = "/World/Cam";

// A 4:3 camera ten units up +z looking down -z, lit by a distant light shining down -z. The light
// is normalized so its intensity is the irradiance it gives; otherwise it is the radiance of a
// half-degree sun, which leaves the scene almost black.
const char* const kCameraAndLight = "#usda 1.0\n"
                                    "(\n"
                                    "    defaultPrim = \"World\"\n"
                                    "    upAxis = \"Y\"\n"
                                    "    timeCodesPerSecond = 24\n"
                                    ")\n"
                                    "def Xform \"World\"\n"
                                    "{\n"
                                    "    def Camera \"Cam\"\n"
                                    "    {\n"
                                    "        float focalLength = 35\n"
                                    "        float horizontalAperture = 24\n"
                                    "        float verticalAperture = 18\n"
                                    "        float2 clippingRange = (0.5, 50)\n"
                                    "        double3 xformOp:translate = (0, 0, 10)\n"
                                    "        uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                                    "    }\n"
                                    "    def DistantLight \"Sun\"\n"
                                    "    {\n"
                                    "        float inputs:intensity = 1\n"
                                    "        bool inputs:normalize = 1\n"
                                    "    }\n"
                                    "}\n";

// A white two-unit cube at the origin on frame 1, translating two units a frame along +x.
const char* const kMovingCube = "over \"World\"\n"
                                "{\n"
                                "    def Mesh \"Cube\"\n"
                                "    {\n"
                                "        int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]\n"
                                "        int[] faceVertexIndices = [0, 1, 3, 2, 2, 3, 5, 4, 4, 5, 7, 6, 6, 7, 1, 0, 1, 7, 5, 3, 6, 0, 2, 4]\n"
                                "        point3f[] points = [(-1, -1, 1), (1, -1, 1), (-1, 1, 1), (1, 1, 1), (-1, 1, -1), (1, 1, -1), (-1, -1, -1), (1, -1, -1)]\n"
                                "        color3f[] primvars:displayColor = [(1, 1, 1)]\n"
                                "        double3 xformOp:translate.timeSamples = {\n"
                                "            0: (-2, 0, 0),\n"
                                "            1: (0, 0, 0),\n"
                                "            2: (2, 0, 0),\n"
                                "        }\n"
                                "        uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                                "    }\n"
                                "}\n";

// Two side-by-side quads facing the camera; the right one is a hole.
const char* const kHoleQuads = "over \"World\"\n"
                               "{\n"
                               "    def Mesh \"Holed\"\n"
                               "    {\n"
                               "        int[] faceVertexCounts = [4, 4]\n"
                               "        int[] faceVertexIndices = [0, 1, 4, 3, 1, 2, 5, 4]\n"
                               "        int[] holeIndices = [1]\n"
                               "        point3f[] points = [(-3, -1, 0), (0, -1, 0), (3, -1, 0), (-3, 1, 0), (0, 1, 0), (3, 1, 0)]\n"
                               "        color3f[] primvars:displayColor = [(1, 1, 1)]\n"
                               "    }\n"
                               "}\n";

// The shutter open a quarter frame either side of the frame: the cube sweeps half a unit each way.
const char* const kOpenShutter = "over \"World\"\n"
                                 "{\n"
                                 "    over \"Cam\"\n"
                                 "    {\n"
                                 "        double shutter:open = -0.25\n"
                                 "        double shutter:close = 0.25\n"
                                 "    }\n"
                                 "}\n";

// A wall of quads of distinct colours, nearer and farther ones mixed, spread past what any fit
// of the camera shows, so each fit and image aspect frames a different part of it.
std::string
wall()
{
    std::string text = "over \"World\"\n{\n";
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
    text += "}\n";

    return text;
}

// The bundled gtest has no GTEST_SKIP, so the test returns early on false;
// NATRON_TEST_REQUIRE_MOONRAY=1 turns that skip into a failure.
bool
moonRayOrSkip()
{
    if (PXR_NS::PlugRegistry::GetInstance().GetPluginWithName("hd_moonray")) {
        return true;
    }
    const char* reason = "OpenUSD has no hd_moonray plug-in registered; this build image predates the "
                         "MoonRay render delegate (tools/ci/local/build-moonray.sh), or the CPU lacks AVX2";
    const char* require = std::getenv("NATRON_TEST_REQUIRE_MOONRAY");
    if (require && (std::strcmp(require, "1") == 0)) {
        ADD_FAILURE() << "NATRON_TEST_REQUIRE_MOONRAY=1 but " << reason;
    } else {
        std::printf("[  SKIPPED ] %s\n", reason);
    }

    return false;
}

// Fixed sampling while a test runs.
class MoonRaySampling {
public:
    explicit MoonRaySampling(int pixelSamples)
    {
        std::map<std::string, PXR_NS::VtValue> settings;
        settings["moonray:sceneVariable:pixel_samples"] = PXR_NS::VtValue(pixelSamples);
        HydraRender::setRendererSettings(kMoonRayRendererId, settings);
    }

    ~MoonRaySampling()
    {
        HydraRender::setRendererSettings(kMoonRayRendererId, std::map<std::string, PXR_NS::VtValue>());
        HydraRender::releaseAllCpuEngines();
    }
};

// kCameraAndLight with the given layers on top, rendered by MoonRay at frame 1 through kCamera,
// by its path or by the frustum cameraFrustum() gives for it. False after a gtest failure.
bool
renderMoonRay(const std::vector<std::string>& overrides,
              HydraRender::FitModeEnum fit,
              int width,
              int height,
              bool throughPath,
              std::vector<float>* rgba)
{
    PXR_NS::SdfLayerRefPtr root = PXR_NS::SdfLayer::CreateAnonymous(".usda");
    if (!root->ImportFromString(kCameraAndLight)) {
        ADD_FAILURE() << "the camera scene does not parse";

        return false;
    }
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(root);
    if (!stage) {
        ADD_FAILURE() << "the camera scene did not open";

        return false;
    }
    static const int owner = 0;
    HydraRender::Request request;
    request.owner = &owner;
    request.scene = SceneOps::appendLayer(ScenePayload(), root, 1);
    for (const std::string& text : overrides) {
        PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::CreateAnonymous(".usda");
        if (!layer->ImportFromString("#usda 1.0\n" + text)) {
            ADD_FAILURE() << "the overrides do not parse:\n"
                          << text;

            return false;
        }
        stage->GetRootLayer()->InsertSubLayerPath(layer->GetIdentifier(), 0);
        request.scene = SceneOps::appendLayer(request.scene, layer, (U64)std::hash<std::string>()(text) | 2);
    }
    request.time = PXR_NS::UsdTimeCode(1.);
    request.width = width;
    request.height = height;
    request.renderer = kMoonRayRendererId;
    std::string error;
    if (!HydraRender::cameraFrustum(stage, PXR_NS::SdfPath(kCamera), request.time, double(width) / height, fit, &request.frustum, &error)) {
        ADD_FAILURE() << error;

        return false;
    }
    if (throughPath) {
        request.camera = PXR_NS::SdfPath(kCamera);
        request.fit = fit;
        request.pixelAspect = 1.;
    }
    HydraRender::Result result;
    const bool rendered = HydraRender::render(request, &result, &error);
    HydraRender::release(&owner);
    if (!rendered) {
        ADD_FAILURE() << (throughPath ? "through the path: " : "through the frustum: ") << error;

        return false;
    }
    if (result.rgba.size() != (std::size_t)width * (std::size_t)height * 4) {
        ADD_FAILURE() << "MoonRay rendered " << result.rgba.size() << " values";

        return false;
    }
    *rgba = result.rgba;

    return true;
}

// The mean of the red, green and blue of columns x0 to x1 over rows y0 to y1.
float
meanOf(const std::vector<float>& rgba,
       int width,
       int x0,
       int x1,
       int y0,
       int y1)
{
    double sum = 0.;
    int count = 0;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const std::size_t i = ((std::size_t)y * (std::size_t)width + (std::size_t)x) * 4;
            sum += rgba[i] + rgba[i + 1] + rgba[i + 2];
            count += 3;
        }
    }

    return (float)(sum / count);
}

// Pixels whose colour or alpha differ by more than tolerance in any channel; path tracer noise
// along edges stays under it.
std::size_t
countDifferent(const std::vector<float>& a,
               const std::vector<float>& b,
               float tolerance)
{
    if (a.size() != b.size()) {
        return std::max(a.size(), b.size());
    }
    std::size_t different = 0;
    for (std::size_t i = 0; i + 3 < a.size(); i += 4) {
        for (std::size_t c = 0; c < 4; ++c) {
            if (std::fabs(a[i + c] - b[i + c]) > tolerance) {
                ++different;
                break;
            }
        }
    }

    return different;
}
} // anon namespace

// At 64 x 48 the still cube's front face spans about columns 22 to 42; with the shutter open it
// sweeps to about 17 and 47. The columns just past each still edge pick up the swept cube, the
// middle stays lit in both.
TEST(MoonRayCamera, TheShutterBlursAMovingCube)
{
    if (!moonRayOrSkip()) {
        return;
    }
    const int width = 64;
    const int height = 48;
    const MoonRaySampling sampling(16);
    std::vector<float> still;
    ASSERT_TRUE(renderMoonRay({ kMovingCube }, HydraRender::eFitModeExpand, width, height, true, &still));
    std::vector<float> blurred;
    ASSERT_TRUE(renderMoonRay({ kMovingCube, kOpenShutter }, HydraRender::eFitModeExpand, width, height, true, &blurred));

    const int y0 = 20;
    const int y1 = 28;
    const float stillMiddle = meanOf(still, width, 30, 34, y0, y1);
    const float blurredMiddle = meanOf(blurred, width, 30, 34, y0, y1);
    ASSERT_GT(stillMiddle, 0.1f) << "MoonRay does not draw the cube";
    EXPECT_GT(blurredMiddle, 0.1f) << "the blurred cube's middle is dark";

    const float stillRight = meanOf(still, width, 44, 46, y0, y1);
    const float blurredRight = meanOf(blurred, width, 44, 46, y0, y1);
    const float stillLeft = meanOf(still, width, 17, 19, y0, y1);
    const float blurredLeft = meanOf(blurred, width, 17, 19, y0, y1);
    EXPECT_GT(blurredRight, stillRight + 0.1f * stillMiddle) << "right trail: blurred " << blurredRight << ", still " << stillRight;
    EXPECT_GT(blurredLeft, stillLeft + 0.1f * stillMiddle) << "left trail: blurred " << blurredLeft << ", still " << stillLeft;
}

// MoonRay frames the scene camera by its path as by the frustum cameraFrustum() gives for it at
// the fill fit, for an image wider and one narrower than the aperture; fill and expand differ, so
// the fit reaches the render.
TEST(MoonRayCamera, TheFillFitFramesAsTheFrustum)
{
    if (!moonRayOrSkip()) {
        return;
    }
    ASSERT_TRUE(HydraRender::rendersThroughCamera(kMoonRayRendererId, HydraRender::eFitModeFill, 1.));
    const MoonRaySampling sampling(4);
    const int sizes[][2] = { { 64, 32 }, { 32, 56 } };
    for (const int* size : sizes) {
        const std::string what = std::to_string(size[0]) + "x" + std::to_string(size[1]);
        const std::size_t pixels = (std::size_t)size[0] * (std::size_t)size[1];
        std::vector<float> frustum;
        ASSERT_TRUE(renderMoonRay({ wall() }, HydraRender::eFitModeFill, size[0], size[1], false, &frustum)) << what;
        std::vector<float> path;
        ASSERT_TRUE(renderMoonRay({ wall() }, HydraRender::eFitModeFill, size[0], size[1], true, &path)) << what;
        std::vector<float> expand;
        ASSERT_TRUE(renderMoonRay({ wall() }, HydraRender::eFitModeExpand, size[0], size[1], true, &expand)) << what;

        EXPECT_GT(meanOf(path, size[0], 0, size[0] - 1, 0, size[1] - 1), 0.02f) << what << ": nothing rendered";
        EXPECT_LE(countDifferent(frustum, path, 0.1f), pixels / 50) << what << ": the path render is not framed as the frustum's";
        EXPECT_GT(countDifferent(expand, path, 0.1f), pixels / 10) << what << ": the fill fit does not reach the path render";
    }
}

// At 64 x 48 the left quad covers about columns 4 to 31 and the right, the hole, columns 32 to 59,
// over rows 6 to 41.
TEST(MoonRayCamera, AHoleFaceIsNotDrawn)
{
    if (!moonRayOrSkip()) {
        return;
    }
    const int width = 64;
    const int height = 48;
    const MoonRaySampling sampling(16);
    std::vector<float> image;
    ASSERT_TRUE(renderMoonRay({ kHoleQuads }, HydraRender::eFitModeExpand, width, height, true, &image));

    const float solid = meanOf(image, width, 10, 26, 14, 34);
    const float hole = meanOf(image, width, 38, 54, 14, 34);
    ASSERT_GT(solid, 0.1f) << "MoonRay does not draw the face beside the hole";
    EXPECT_LT(hole, 0.02f * solid) << "the hole face is drawn: " << hole << " against " << solid;
}
