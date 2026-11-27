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

#include "CyclesTestUtils.h"
#include "Engine/Nodes/Scene/CyclesAovs.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_USING

namespace {
const char* const kCamera = "/World/Cam";
const int kSize = 64;

// A camera ten units up +z looking down -z; the shutter is authored by each test. The world
// Cycles lights a stage with no light by leaves the background's alpha 0, so alpha is coverage.
const char* const kCameraPrim = "    def Camera \"Cam\"\n"
                                "    {\n"
                                "        float focalLength = 35\n"
                                "        float horizontalAperture = 24\n"
                                "        float verticalAperture = 18\n"
                                "        float2 clippingRange = (0.5, 50)\n"
                                "        double3 xformOp:translate = (0, 0, 10)\n"
                                "        uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                                "%s"
                                "    }\n";

// A 2-unit cube at the origin moving a unit along +x per frame, centred on frame 1.
const char* const kMovingCube = "    def Cube \"Box\"\n"
                                "    {\n"
                                "        double size = 2\n"
                                "        double3 xformOp:translate.timeSamples = { 0: (-1, 0, 0), 2: (1, 0, 0) }\n"
                                "        uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                                "    }\n";

// The same motion carried by the points of a 2-unit square facing the camera, its transform still.
const char* const kMovingPoints = "    def Mesh \"Card\"\n"
                                  "    {\n"
                                  "        int[] faceVertexCounts = [4]\n"
                                  "        int[] faceVertexIndices = [0, 1, 2, 3]\n"
                                  "        point3f[] points.timeSamples = {\n"
                                  "            0: [(-2, -1, 0), (0, -1, 0), (0, 1, 0), (-2, 1, 0)],\n"
                                  "            2: [(0, -1, 0), (2, -1, 0), (2, 1, 0), (0, 1, 0)],\n"
                                  "        }\n"
                                  "    }\n";

// Squares spread over the camera's 4:3 view, at its edges too, so that each fit and pixel aspect
// covers a different set of them.
const char* const kSquares = "    def Mesh \"Squares\"\n"
                             "    {\n"
                             "        int[] faceVertexCounts = [4, 4, 4, 4, 4]\n"
                             "        int[] faceVertexIndices = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19]\n"
                             "        point3f[] points = [\n"
                             "            (-0.5, -0.5, 0), (0.5, -0.5, 0), (0.5, 0.5, 0), (-0.5, 0.5, 0),\n"
                             "            (-3.4, 1.4, 0), (-2.6, 1.4, 0), (-2.6, 2.2, 0), (-3.4, 2.2, 0),\n"
                             "            (2.4, -2.2, 0), (3.2, -2.2, 0), (3.2, -1.4, 0), (2.4, -1.4, 0),\n"
                             "            (-0.4, 2.0, 0), (0.4, 2.0, 0), (0.4, 2.8, 0), (-0.4, 2.8, 0),\n"
                             "            (3.0, 0.6, 0), (3.8, 0.6, 0), (3.8, 1.4, 0), (3.0, 1.4, 0)]\n"
                             "    }\n";

const char* const kOpenShutter = "        double shutter:open = -0.25\n"
                                 "        double shutter:close = 0.25\n";

std::string
sceneOf(const char* shutter,
        const char* body)
{
    std::vector<char> camera(std::strlen(kCameraPrim) + std::strlen(shutter) + 1);
    std::snprintf(camera.data(), camera.size(), kCameraPrim, shutter);

    return std::string("#usda 1.0\n(\n    defaultPrim = \"World\"\n    upAxis = \"Y\"\n)\ndef Xform \"World\"\n{\n") + camera.data() + body + "}\n";
}

// Fixed sampling while a test runs.
class FixedSampling {
public:
    FixedSampling()
    {
        HydraRender::setRendererSettings(kRenderDelegateCyclesId, CyclesTestUtils::fixedSampling(16));
    }

    ~FixedSampling()
    {
        HydraRender::setRendererSettings(kRenderDelegateCyclesId, std::map<std::string, PXR_NS::VtValue>());
        HydraRender::releaseAllCpuEngines();
    }
};

// text at frame 1 rendered by Cycles at width x height through kCamera, by its path or by the
// frustum cameraFrustum() gives for it. False after a gtest failure.
bool
renderScene(const std::string& text,
            int width,
            int height,
            HydraRender::FitModeEnum fit,
            double pixelAspect,
            bool throughPath,
            std::vector<float>* rgba)
{
    PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::CreateAnonymous(".usda");
    if (!layer->ImportFromString(text)) {
        ADD_FAILURE() << "the scene does not parse:\n"
                      << text;

        return false;
    }
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(layer);
    if (!stage) {
        ADD_FAILURE() << "the scene did not open";

        return false;
    }
    static const int owner = 0;
    HydraRender::Request request;
    request.owner = &owner;
    request.scene = SceneOps::appendLayer(ScenePayload(), layer, (U64)std::hash<std::string>()(text) | 1);
    request.time = PXR_NS::UsdTimeCode(1.);
    request.width = width;
    request.height = height;
    request.renderer = kRenderDelegateCyclesId;
    std::string error;
    if (!HydraRender::cameraFrustum(stage, PXR_NS::SdfPath(kCamera), request.time, width * pixelAspect / height, fit, &request.frustum, &error)) {
        ADD_FAILURE() << error;

        return false;
    }
    if (throughPath) {
        request.camera = PXR_NS::SdfPath(kCamera);
        request.fit = fit;
        request.pixelAspect = pixelAspect;
    }
    HydraRender::Result result;
    const bool rendered = HydraRender::render(request, &result, &error);
    HydraRender::release(&owner);
    if (!rendered) {
        ADD_FAILURE() << error;

        return false;
    }
    if (result.rgba.size() != (std::size_t)width * (std::size_t)height * 4) {
        ADD_FAILURE() << "rendered " << result.rgba.size() << " values";

        return false;
    }
    *rgba = result.rgba;

    return true;
}

// Pixels of the middle row, or the middle column, that the geometry only partly covers.
int
partlyCovered(const std::vector<float>& rgba,
              bool row)
{
    int partly = 0;
    for (int i = 0; i < kSize; ++i) {
        const int x = row ? i : kSize / 2;
        const int y = row ? kSize / 2 : i;
        const float alpha = rgba[((std::size_t)y * kSize + x) * 4 + 3];
        if ((alpha > 0.05f) && (alpha < 0.95f)) {
            ++partly;
        }
    }

    return partly;
}

// An open shutter smears body's edges across the motion, along x, and leaves them sharp across y.
void
expectTheShutterBlursAlongTheMotion(const char* body)
{
    const FixedSampling sampling;
    std::vector<float> still;
    ASSERT_TRUE(renderScene(sceneOf("", body), kSize, kSize, HydraRender::eFitModeExpand, 1., true, &still));
    std::vector<float> blurred;
    ASSERT_TRUE(renderScene(sceneOf(kOpenShutter, body), kSize, kSize, HydraRender::eFitModeExpand, 1., true, &blurred));

    EXPECT_GE(partlyCovered(blurred, true), partlyCovered(still, true) + 4)
        << "row: blurred " << partlyCovered(blurred, true) << ", still " << partlyCovered(still, true);
    EXPECT_LE(partlyCovered(blurred, false), partlyCovered(still, false) + 1)
        << "column: blurred " << partlyCovered(blurred, false) << ", still " << partlyCovered(still, false);
}

// Pixels whose alpha differs by more than a quarter, which sampling noise along an edge stays under.
std::size_t
coverageDifferences(const std::vector<float>& a,
                    const std::vector<float>& b)
{
    if (a.size() != b.size()) {
        return std::max(a.size(), b.size());
    }
    std::size_t different = 0;
    for (std::size_t i = 3; i < a.size(); i += 4) {
        if (std::fabs(a[i] - b[i]) > 0.25f) {
            ++different;
        }
    }

    return different;
}
} // anon namespace

TEST(USDRenderCyclesMotion, TheShutterBlursAMovingTransform)
{
    if (!CyclesTestUtils::cyclesOrSkip()) {
        return;
    }
    expectTheShutterBlursAlongTheMotion(kMovingCube);
}

TEST(USDRenderCyclesMotion, TheShutterBlursMovingPoints)
{
    if (!CyclesTestUtils::cyclesOrSkip()) {
        return;
    }
    expectTheShutterBlursAlongTheMotion(kMovingPoints);
}

// Cycles frames the camera by its path as by the frustum cameraFrustum() gives for it, at the
// fill fit of an image wider than the aperture and at the expand fit of pixels twice as wide as
// tall, and the two framings differ. Where rendersThroughCamera() is false the path render goes
// through the frustum as well, which would match it whatever Cycles does.
TEST(USDRenderCyclesMotion, ThePathFramesAsTheFrustum)
{
    if (!CyclesTestUtils::cyclesOrSkip()) {
        return;
    }
    ASSERT_TRUE(HydraRender::rendersThroughCamera(kRenderDelegateCyclesId, HydraRender::eFitModeFill, 1.));
    ASSERT_TRUE(HydraRender::rendersThroughCamera(kRenderDelegateCyclesId, HydraRender::eFitModeExpand, 2.));
    const FixedSampling sampling;
    const std::string text = sceneOf("", kSquares);
    const int width = kSize;
    const int height = kSize / 2;
    const std::size_t pixels = (std::size_t)width * height;

    std::vector<float> fillFrustum;
    ASSERT_TRUE(renderScene(text, width, height, HydraRender::eFitModeFill, 1., false, &fillFrustum));
    std::vector<float> fillPath;
    ASSERT_TRUE(renderScene(text, width, height, HydraRender::eFitModeFill, 1., true, &fillPath));
    EXPECT_LE(coverageDifferences(fillFrustum, fillPath), pixels / 50) << "fill";

    std::vector<float> wideFrustum;
    ASSERT_TRUE(renderScene(text, width, height, HydraRender::eFitModeExpand, 2., false, &wideFrustum));
    std::vector<float> widePath;
    ASSERT_TRUE(renderScene(text, width, height, HydraRender::eFitModeExpand, 2., true, &widePath));
    EXPECT_LE(coverageDifferences(wideFrustum, widePath), pixels / 50) << "pixel aspect 2";

    EXPECT_GT(coverageDifferences(fillPath, widePath), pixels / 10) << "the framings do not differ";
}
