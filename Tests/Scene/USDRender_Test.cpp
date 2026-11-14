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
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "CyclesTestUtils.h"
#include "Gpu/GpuTestSelector.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/GpuContext.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/Scene/CyclesAovs.h"
#include "Engine/Nodes/Scene/HeadlessGLContext.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/RenderSettingsPrim.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Nodes/Scene/USDRender.h"
#include "Engine/Nodes/Scene/USDRenderSettings.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"
#include "RenderBothWays.h"
#include "RenderSettingsTestUtils.h"
#include "Tests/Native/NativeGpuParity.h"
#include "ofxNatron.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/AbortableRenderInfo.h"
#include "Engine/FrameRenderContext.h"
#include "Engine/Gpu/GpuBufferPool.h"
#include "Engine/Gpu/GpuRuntime.h"
#include "Engine/GpuPlacement.h"
#include "Engine/GpuRegionStrips.h"
#include "Engine/Image.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Scene/RenderSettingsKnobs.h"
#include "Engine/Nodes/Scene/Utility/UtilityRenderBuffer.h"
#include "Engine/Nodes/Scene/Utility/UtilityRenderDelegate.h"
#include "Engine/Nodes/Scene/Utility/WavefrontGpu.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/RenderScheduler.h"
#include "Engine/RenderStats.h"
#include "Engine/TimeLine.h"
#include "Gpu/GpuTestDevice.h"
#endif

NATRON_NAMESPACE_USING

namespace {
const char* const kFixture = NATRON_TESTS_FIXTURES_DIR "/scene-reference.usda";
const char* const kInstancerFixture = NATRON_TESTS_FIXTURES_DIR "/scene-instancer-prototypes.usda";
const char* const kPurposeFixture = NATRON_TESTS_FIXTURES_DIR "/scene-purposes.usda";
const char* const kNestedInstancesFixture = NATRON_TESTS_FIXTURES_DIR "/scene-nested-instances.usda";
const int kWidth = 200;
const int kHeight = 100;

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
// NATRON_TEST_REQUIRE_GL=1.
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
    std::cout << "[  SKIPPED ] USDRender: " << error << std::endl;

    return true;
}

PXR_NS::GfVec2d
windowSize(const PXR_NS::UsdStageRefPtr& stage,
           double aspect,
           HydraRender::FitModeEnum fit)
{
    PXR_NS::GfFrustum frustum;
    std::string error;

    EXPECT_TRUE(HydraRender::cameraFrustum(stage, PXR_NS::SdfPath("/World/Cam"), PXR_NS::UsdTimeCode(1), aspect, fit, &frustum, &error)) << error;

    return frustum.GetWindow().GetSize();
}

// Where a prim landed in a render: its pixel count and centroid, rows counted from the bottom.
struct Coverage {
    int count = 0;
    double x = 0.;
    double y = 0.;
    float minDepth = std::numeric_limits<float>::infinity();
    float maxDepth = -std::numeric_limits<float>::infinity();
    double red = 0.;
    double blue = 0.;
};

Coverage
coverageOf(const std::string& path,
           int width,
           int height,
           const std::vector<float>& primId,
           const std::vector<float>& depth,
           const std::vector<float>& rgba)
{
    Coverage c;
    const float id = HydraRender::hashToFloat(HydraRender::primPathHash(path));

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t i = (std::size_t)y * (std::size_t)width + (std::size_t)x;
            if (std::memcmp(&primId[i], &id, sizeof(float)) != 0) {
                continue;
            }
            ++c.count;
            c.x += x;
            c.y += y;
            c.minDepth = std::min(c.minDepth, depth[i]);
            c.maxDepth = std::max(c.maxDepth, depth[i]);
            c.red += rgba[i * 4];
            c.blue += rgba[i * 4 + 2];
        }
    }
    if (c.count) {
        c.x /= c.count;
        c.y /= c.count;
    }

    return c;
}

HydraRender::Request
referenceRequest(const PXR_NS::UsdStageRefPtr& stage,
                 double frame,
                 const void* owner)
{
    HydraRender::Request request;
    std::string error;

    request.owner = owner;
    // The fixture file is the scene's one layer.
    request.scene = SceneOps::appendLayer(ScenePayload(), stage->GetRootLayer(), 1);
    request.time = PXR_NS::UsdTimeCode(frame);
    request.width = kWidth;
    request.height = kHeight;
    EXPECT_TRUE(HydraRender::cameraFrustum(stage, PXR_NS::SdfPath("/World/Cam"), request.time, double(kWidth) / kHeight, HydraRender::eFitModeFill, &request.frustum, &error)) << error;

    return request;
}
} // anonymous namespace

namespace {
// Storm's default resolve keeps one MSAA sample per pixel, off the centre by a fraction of a pixel, so
// its depth moves with the surface's slope and no tolerance on depth alone separates that from a wrong
// ray. USDRenderSingleSampleTest therefore runs Storm with one sample, at the pixel's centre as a ray
// is. Float window depth then leaves a worst difference of 1.2e-6 on the reference scene.
const double kCentreDepthTolerance = 1e-5;

// A centre within rounding of a silhouette can land either side of it, as Storm's fill rule and the
// ray's test need not agree there: 0 pixels on the reference scene and 4 on the nested instances. A
// sampling shift disagrees along whole edges, 26 pixels already for the offset of Storm's MSAA sample.
const std::size_t kSilhouetteTies = 8;

bool
sameBits(float a,
         float b)
{
    return std::memcmp(&a, &b, sizeof(float)) == 0;
}

// Holds a render's depth and primId to single-sample Storm's on every pixel, and returns how many
// pixels hit the same prim in both.
std::size_t
expectMatchesSingleSampleStorm(const std::vector<float>& stormDepth,
                               const std::vector<float>& stormPrimId,
                               const std::vector<float>& depth,
                               const std::vector<float>& primId)
{
    std::size_t compared = 0;
    std::size_t primIdFailures = 0;
    std::size_t depthFailures = 0;
    double worst = 0.;

    for (std::size_t i = 0; i < stormDepth.size(); ++i) {
        if (!sameBits(primId[i], stormPrimId[i])) {
            ++primIdFailures;
            continue;
        }
        if (!std::isfinite(stormDepth[i]) && !std::isfinite(depth[i])) {
            continue;
        }
        ++compared;

        const double relative = std::fabs((double)depth[i] - stormDepth[i]) / std::max(std::fabs((double)depth[i]), std::fabs((double)stormDepth[i]));

        worst = std::max(worst, relative);
        if (!(relative <= kCentreDepthTolerance)) {
            ++depthFailures;
        }
    }
    EXPECT_LE(primIdFailures, kSilhouetteTies);
    EXPECT_EQ(0u, depthFailures) << "worst relative depth difference " << worst;
    std::cout << "[ depth  ] " << compared << " pixels, worst relative difference from Storm " << worst << ", " << primIdFailures << " primId differences" << std::endl;

    return compared;
}

std::set<std::string>
primsIn(const std::vector<float>& primId,
        const std::vector<std::string>& candidates)
{
    std::set<std::string> shown;

    for (const std::string& path : candidates) {
        const float value = HydraRender::hashToFloat(HydraRender::primPathHash(path));
        for (const float pixel : primId) {
            if (sameBits(pixel, value)) {
                shown.insert(path);
                break;
            }
        }
    }

    return shown;
}

// Each instance of the nested fixture has its own side of the frame, so a flattened list in the wrong
// order names a wrong instance's path at these pixels.
void
expectNestedInstancesInPlace(const HydraRender::Result& result)
{
    const std::vector<std::string> paths = { "/World/Zed/Upper/Geo", "/World/Zed/Lower/Geo", "/World/Alpha/Upper/Geo", "/World/Alpha/Lower/Geo" };

    for (const std::string& path : paths) {
        EXPECT_EQ(1u, result.primPaths.count(HydraRender::primPathHash(path))) << path;
    }
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const float value = result.primId[(std::size_t)y * kWidth + x];
            if (sameBits(value, 0.f)) {
                continue;
            }
            std::uint32_t hash = 0;
            std::memcpy(&hash, &value, sizeof(hash));
            const std::map<std::uint32_t, std::string>::const_iterator found = result.primPaths.find(hash);
            ASSERT_TRUE(found != result.primPaths.end()) << "pixel " << x << ", " << y;
            const std::string& path = found->second;
            ASSERT_EQ(x < kWidth / 2, path.rfind("/World/Zed/", 0) == 0) << path << " at " << x << ", " << y;
            ASSERT_EQ(y >= kHeight / 2, path.find("/Upper/") != std::string::npos) << path << " at " << x << ", " << y;
        }
    }
}
} // anonymous namespace

TEST(USDRenderHeadless, AMissingEGLLibraryIsAClearError)
{
    std::string error;
    std::unique_ptr<HeadlessGLContext> context = HeadlessGLContext::create(&error, "libNatronNoSuchEGL.so.1");

    EXPECT_FALSE(context != nullptr);
    EXPECT_NE(std::string::npos, error.find("OpenGL 4.5")) << error;
    EXPECT_NE(std::string::npos, error.find("no EGL library")) << error;
}

TEST(USDRenderPrimId, HashesAreCryptomattesAndAlwaysFiniteNormalFloats)
{
    // MurmurHash3_x86_32 with seed 0, as Cryptomatte hashes names.
    EXPECT_EQ(0x248bfa47u, HydraRender::primPathHash("hello"));
    // The empty string hashes to 0, whose exponent is that of a denormal: Cryptomatte flips a bit.
    EXPECT_EQ(0x00800000u, HydraRender::primPathHash(""));
    for (int i = 0; i < 2000; ++i) {
        const std::uint32_t hash = HydraRender::primPathHash("/World/Prim" + std::to_string(i));
        const float value = HydraRender::hashToFloat(hash);
        EXPECT_TRUE(std::isnormal(value)) << i;
        std::uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        EXPECT_EQ(hash, bits);
    }
}

TEST(USDRenderCamera, AnUnknownCameraIsAnErrorNamingItsPath)
{
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(kFixture);
    ASSERT_TRUE(stage);
    PXR_NS::GfFrustum frustum;
    std::string error;

    EXPECT_FALSE(HydraRender::cameraFrustum(stage, PXR_NS::SdfPath("/World/Nope"), PXR_NS::UsdTimeCode(1), 2., HydraRender::eFitModeFill, &frustum, &error));
    EXPECT_NE(std::string::npos, error.find("/World/Nope")) << error;
    // A prim that is not a camera is no camera either.
    error.clear();
    EXPECT_FALSE(HydraRender::cameraFrustum(stage, PXR_NS::SdfPath("/World/Box"), PXR_NS::UsdTimeCode(1), 2., HydraRender::eFitModeFill, &frustum, &error));
    EXPECT_NE(std::string::npos, error.find("/World/Box")) << error;

    const std::vector<PXR_NS::SdfPath> cameras = HydraRender::cameraPaths(stage);
    ASSERT_EQ(1u, cameras.size());
    EXPECT_EQ("/World/Cam", cameras[0].GetString());
}

TEST(USDRenderCamera, FitModesConformTheApertureToTheFormat)
{
    // The fixture's camera has USD's default 20.955 x 15.2908 aperture, wider than square and
    // narrower than 2:1.
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(kFixture);
    ASSERT_TRUE(stage);
    const double tolerance = 1e-9;

    for (double aspect : { 1., 2. }) {
        for (HydraRender::FitModeEnum fit : { HydraRender::eFitModeFill, HydraRender::eFitModeHorizontal, HydraRender::eFitModeVertical }) {
            const PXR_NS::GfVec2d size = windowSize(stage, aspect, fit);
            EXPECT_NEAR(aspect, size[0] / size[1], tolerance) << aspect << " " << (int)fit;
        }
    }
    const PXR_NS::GfVec2d square[] = {
        windowSize(stage, 1., HydraRender::eFitModeFill), windowSize(stage, 1., HydraRender::eFitModeHorizontal), windowSize(stage, 1., HydraRender::eFitModeVertical)
    };
    // Square: Fill crops the width, as Vertical keeps the height; Horizontal extends the height.
    EXPECT_NEAR(square[0][0], square[2][0], tolerance);
    EXPECT_GT(square[1][0], square[0][0] * 1.3);
    const PXR_NS::GfVec2d wide[] = {
        windowSize(stage, 2., HydraRender::eFitModeFill), windowSize(stage, 2., HydraRender::eFitModeHorizontal), windowSize(stage, 2., HydraRender::eFitModeVertical)
    };
    // 2:1: Fill crops the height, as Horizontal keeps the width; Vertical extends the width.
    EXPECT_NEAR(wide[0][0], wide[1][0], tolerance);
    EXPECT_GT(wide[2][0], wide[0][0] * 1.4);
    // Horizontal keeps the aperture's width whatever the format.
    EXPECT_NEAR(square[1][0], wide[1][0], tolerance);
}

TEST(USDRenderStorm, PlanesHoldColourPlanarDepthAndPrimIdentity)
{
    if (skipWithoutGL()) {
        return;
    }
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(kFixture);
    ASSERT_TRUE(stage);
    HydraRender::Result result;
    std::string error;
    const int owner = 0;

    // The fixture has no light, and renders add none: a distant light shining down -Z, as the
    // camera looks, shows the display colours.
    HydraRender::Request request = referenceRequest(stage, 1., &owner);
    const PXR_NS::SdfLayerRefPtr lightLayer = SceneOps::createNodeLayer("light");
    ASSERT_TRUE(bool(lightLayer));
    {
        PXR_NS::UsdStageRefPtr lightStage = PXR_NS::UsdStage::Open(lightLayer);
        ASSERT_TRUE(bool(lightStage));
        ASSERT_TRUE(bool(PXR_NS::UsdLuxDistantLight::Define(lightStage, PXR_NS::SdfPath("/Key"))));
    }
    request.scene = SceneOps::appendLayer(request.scene, lightLayer, 2);
    ASSERT_TRUE(HydraRender::render(request, &result, &error)) << error;
    const std::size_t pixels = (std::size_t)kWidth * kHeight;
    ASSERT_EQ(pixels * 4, result.rgba.size());
    ASSERT_EQ(pixels, result.depth.size());
    ASSERT_EQ(pixels, result.primId.size());

    const std::uint32_t boxHash = HydraRender::primPathHash("/World/Box");
    const std::uint32_t ballHash = HydraRender::primPathHash("/World/Ball");
    ASSERT_EQ(1u, result.primPaths.count(boxHash));
    ASSERT_EQ(1u, result.primPaths.count(ballHash));
    EXPECT_EQ("/World/Box", result.primPaths[boxHash]);
    EXPECT_EQ("/World/Ball", result.primPaths[ballHash]);

    const Coverage box = coverageOf("/World/Box", kWidth, kHeight, result.primId, result.depth, result.rgba);
    const Coverage ball = coverageOf("/World/Ball", kWidth, kHeight, result.primId, result.depth, result.rgba);
    ASSERT_GT(box.count, 100);
    ASSERT_GT(ball.count, 20);

    // The camera at (1, 1, 10) looks down -Z: the box's front face at z = 1 is 9 away all over,
    // the ball (radius 0.5 at (3, 0, 0)) between 9.5 and 10.
    EXPECT_NEAR(9., box.minDepth, 0.01);
    EXPECT_NEAR(9., box.maxDepth, 0.01);
    EXPECT_GT(ball.minDepth, 9.45f);
    EXPECT_LT(ball.maxDepth, 10.05f);

    // Rows run from the bottom, as Natron's do: both prims lie below the camera, the box to its
    // left and the ball to its right.
    EXPECT_LT(box.y, kHeight / 2.);
    EXPECT_LT(ball.y, kHeight / 2.);
    EXPECT_LT(box.x, kWidth / 2.);
    EXPECT_GT(ball.x, kWidth / 2.);

    // Each is shaded in its display colour: the box red, the ball blue.
    EXPECT_GT(box.red, box.blue);
    EXPECT_GT(ball.blue, ball.red);

    // Nothing was hit in the top-left corner.
    const std::size_t corner = (std::size_t)(kHeight - 1) * kWidth;
    EXPECT_TRUE(std::isinf(result.depth[corner]) && (result.depth[corner] > 0.f));
    EXPECT_EQ(0.f, result.primId[corner]);
    for (int c = 0; c < 4; ++c) {
        EXPECT_EQ(0.f, result.rgba[corner * 4 + c]) << c;
    }
    HydraRender::release(&owner);
}

TEST(USDRenderStorm, TheBallMovesWithItsTimeSamples)
{
    if (skipWithoutGL()) {
        return;
    }
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(kFixture);
    ASSERT_TRUE(stage);
    HydraRender::Result result;
    std::string error;
    const int owner = 0;

    // At frame 24 the ball is at (3, 2, 0), above the camera.
    ASSERT_TRUE(HydraRender::render(referenceRequest(stage, 24., &owner), &result, &error)) << error;
    const Coverage ball = coverageOf("/World/Ball", kWidth, kHeight, result.primId, result.depth, result.rgba);
    ASSERT_GT(ball.count, 20);
    EXPECT_GT(ball.y, kHeight / 2.);
    HydraRender::release(&owner);
}

// Ten frames with one edit upstream between the fifth and the sixth: one engine renders them
// all, following the edit through its stage. Releasing the owner frees the engine, and the next
// render builds another.
TEST(USDRenderStorm, OneEngineFollowsAnEditAndAReleaseFreesIt)
{
    if (skipWithoutGL()) {
        return;
    }
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(kFixture);
    ASSERT_TRUE(stage);
    const int owner = 0;
    std::string error;
    HydraRender::Result result;

    // The edit: a layer above the fixture moving the box up by 2.
    PXR_NS::SdfLayerRefPtr editLayer = PXR_NS::SdfLayer::CreateAnonymous("edit.usda");
    {
        PXR_NS::UsdStageRefPtr editStage = PXR_NS::UsdStage::Open(editLayer);
        PXR_NS::UsdPrim box = editStage->OverridePrim(PXR_NS::SdfPath("/World/Box"));
        PXR_NS::UsdGeomXformable xformable(box);
        ASSERT_TRUE(xformable.AddTranslateOp(PXR_NS::UsdGeomXformOp::PrecisionDouble, PXR_NS::TfToken("edit")).Set(PXR_NS::GfVec3d(0., 2., 0.)));
    }

    const std::uint64_t builds = HydraRender::getEngineBuildCount();
    double boxYBefore = 0.;
    double boxYAfter = 0.;
    for (int frame = 1; frame <= 10; ++frame) {
        HydraRender::Request request = referenceRequest(stage, frame, &owner);
        if (frame > 5) {
            request.scene = SceneOps::appendLayer(request.scene, editLayer, 2);
        }
        ASSERT_TRUE(HydraRender::render(request, &result, &error)) << frame << ": " << error;
        const Coverage box = coverageOf("/World/Box", kWidth, kHeight, result.primId, result.depth, result.rgba);
        ASSERT_GT(box.count, 100) << frame;
        if (frame == 5) {
            boxYBefore = box.y;
        } else if (frame == 6) {
            boxYAfter = box.y;
        }
    }
    EXPECT_EQ(builds + 1, HydraRender::getEngineBuildCount());
    // The edit reached Storm: the box moved up the image.
    EXPECT_GT(boxYAfter, boxYBefore + 1.);
    EXPECT_EQ(1u, HydraRender::getEngineCount());

    HydraRender::release(&owner);
    EXPECT_EQ(0u, HydraRender::getEngineCount());
    ASSERT_TRUE(HydraRender::render(referenceRequest(stage, 1., &owner), &result, &error)) << error;
    EXPECT_EQ(builds + 2, HydraRender::getEngineBuildCount());

    HydraRender::releaseAll();
    EXPECT_EQ(0u, HydraRender::getEngineCount());
}

// Past the memory budget, the least recently used owner's engine goes; the rendering one stays.
TEST(USDRenderStorm, EnginesOverTheBudgetGoLeastRecentlyUsedFirst)
{
    if (skipWithoutGL()) {
        return;
    }
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(kFixture);
    ASSERT_TRUE(stage);
    const int first = 0;
    const int second = 1;
    std::string error;
    HydraRender::Result result;
    const std::size_t budget = HydraRender::getMemoryBudget();

    ASSERT_TRUE(HydraRender::render(referenceRequest(stage, 1., &first), &result, &error)) << error;
    ASSERT_TRUE(HydraRender::render(referenceRequest(stage, 1., &second), &result, &error)) << error;
    EXPECT_EQ(2u, HydraRender::getEngineCount());

    HydraRender::setMemoryBudget(1);
    const std::uint64_t builds = HydraRender::getEngineBuildCount();
    // The second owner renders again, reusing its engine; the first one's goes.
    ASSERT_TRUE(HydraRender::render(referenceRequest(stage, 2., &second), &result, &error)) << error;
    EXPECT_EQ(1u, HydraRender::getEngineCount());
    EXPECT_EQ(builds, HydraRender::getEngineBuildCount());
    // The first owner renders again with a new engine, and the second one's goes.
    ASSERT_TRUE(HydraRender::render(referenceRequest(stage, 2., &first), &result, &error)) << error;
    EXPECT_EQ(1u, HydraRender::getEngineCount());
    EXPECT_EQ(builds + 1, HydraRender::getEngineBuildCount());

    HydraRender::setMemoryBudget(budget);
    HydraRender::releaseAll();
    EXPECT_EQ(0u, HydraRender::getEngineCount());
}

class USDRenderNodeTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        SceneQueryPool::instance().releaseIdle();
        BaseTest::TearDown();
    }

    // A USDRead of fixture; null after a gtest failure.
    NodePtr createReader(const char* fixture)
    {
        NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
        KnobFile* file = reader ? dynamic_cast<KnobFile*>(reader->getKnobByName(kUSDReadParamFile).get()) : nullptr;
        if (!file) {
            ADD_FAILURE() << "USDRead is not registered or has no file knob";

            return NodePtr();
        }
        file->setValue(std::string(fixture));

        return reader;
    }

    /**
     * A USDRead of the fixture into a USDRenderSettings whose prim frames /World/Cam at kWidth x
     * kHeight, into a USDRender. *settingsNode gets the USDRenderSettings.
     **/
    NodePtr createRender(const char* fixture = kFixture,
                         NodePtr* settingsNode = nullptr)
    {
        NodePtr reader = createReader(fixture);
        NodePtr settings = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDERSETTINGS));
        NodePtr render = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDER));
        if (!reader || !settings || !render) {
            ADD_FAILURE() << "USDRead, USDRenderSettings or USDRender is not registered";

            return NodePtr();
        }
        if (!RenderSettingsTestUtils::configure(settings, "/World/Cam", kWidth, kHeight)) {
            return NodePtr();
        }
        connectNodes(reader, settings, 0, true);
        connectNodes(settings, render, 0, true);
        if (settingsNode) {
            *settingsNode = settings;
        }

        return render;
    }

    static USDRender* renderOf(const NodePtr& node)
    {
        return node ? dynamic_cast<USDRender*>(node->getEffectInstance().get()) : nullptr;
    }

    // The renderer node renders frame 1 with, empty with *error set if it cannot tell.
    static std::string activeRendererOf(const NodePtr& node,
                                        std::string* error)
    {
        std::string id;
        USDRender* render = renderOf(node);
        if (!render) {
            *error = "not a USDRender";

            return std::string();
        }

        return render->activeRenderer(1., ViewIdx(0), &id, error) ? id : std::string();
    }

    // The node's output format as its region of definition at frame 1.
    static RectD rodOf(const NodePtr& node)
    {
        RectD rod;
        USDRender* render = renderOf(node);
        EXPECT_TRUE(render != nullptr);
        if (render) {
            EXPECT_EQ(eStatusOK, render->getRegionOfDefinition(node->getHashValue(), 1., RenderScale::identity, ViewIdx(0), &rod));
        }

        return rod;
    }

    static std::list<ImageLayerDesc> allPlanes()
    {
        std::list<ImageLayerDesc> layers;
        layers.push_back(ImageLayerDesc::getRGBAComponents());
        layers.push_back(ImageLayerDesc("depth", "depth", "", std::vector<std::string>(1, "Z")));
        layers.push_back(ImageLayerDesc("primId", "primId", "", std::vector<std::string>(1, "id")));

        return layers;
    }
};

TEST_F(USDRenderNodeTest, TheSettingsPrimMenuListsTheScenesRenderSettingsPrims)
{
    NodePtr node = createRender();
    ASSERT_TRUE(node != nullptr);
    USDRender* effect = renderOf(node);
    ASSERT_TRUE(effect);
    effect->refreshSettingsPrimMenu();
    KnobChoice* settingsPrim = dynamic_cast<KnobChoice*>(node->getKnobByName(kUSDRenderParamSettingsPrim).get());
    ASSERT_TRUE(settingsPrim);
    const std::vector<ChoiceOption> entries = settingsPrim->getEntries_mt_safe();
    ASSERT_EQ(2u, entries.size());
    EXPECT_EQ(std::string(kUSDRenderSettingsPrimStageDefault), entries[0].id);
    EXPECT_EQ(std::string(kUSDRenderSettingsDefaultPrimPath), entries[1].id);
    EXPECT_EQ(std::string(kUSDRenderSettingsPrimStageDefault), settingsPrim->getActiveEntry().id);
}

// The prim's resolution and pixel aspect ratio are the node's format.
TEST_F(USDRenderNodeTest, ThePrimsResolutionIsTheFormat)
{
    NodePtr settings;
    NodePtr node = createRender(kFixture, &settings);
    ASSERT_TRUE(node != nullptr);
    RectD rod = rodOf(node);
    EXPECT_EQ(0., rod.x1);
    EXPECT_EQ(0., rod.y1);
    EXPECT_EQ((double)kWidth, rod.x2);
    EXPECT_EQ((double)kHeight, rod.y2);
    ASSERT_TRUE(RenderSettingsTestUtils::configure(settings, "/World/Cam", 64, 32, 2.));
    rod = rodOf(node);
    EXPECT_EQ(128., rod.x2);
    EXPECT_EQ(32., rod.y2);
}

TEST_F(USDRenderNodeTest, MetadataMapsEveryPrimIdToItsPath)
{
    NodePtr node = createRender();
    ASSERT_TRUE(node != nullptr);
    NativeEffectBase* effect = dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
    ASSERT_TRUE(effect);
    const ImageMetadata metadata = effect->getOutputMetadata(1., ViewIdx(0));
    for (const char* path : { "/World/Box", "/World/Ball" }) {
        char key[32];
        std::snprintf(key, sizeof(key), kUSDRenderMetadataPrimIdPrefix "%08x", HydraRender::primPathHash(path));
        const std::optional<std::string> value = metadata.getString(key);
        ASSERT_TRUE(value.has_value()) << key;
        EXPECT_EQ(path, *value);
    }
}

TEST_F(USDRenderNodeTest, MetadataNamesInstancerPrototypesAndSkipsGuides)
{
    NodePtr node = createRender(kInstancerFixture);
    ASSERT_TRUE(node != nullptr);
    NativeEffectBase* effect = dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
    ASSERT_TRUE(effect);
    const ImageMetadata metadata = effect->getOutputMetadata(1., ViewIdx(0));
    auto lookup = [&metadata](const char* path) {
        char key[32];
        std::snprintf(key, sizeof(key), kUSDRenderMetadataPrimIdPrefix "%08x", HydraRender::primPathHash(path));

        return metadata.getString(key);
    };
    for (const char* path : { "/Protos/Cube", "/Protos/Over/Ball", "/World/Plain" }) {
        const std::optional<std::string> value = lookup(path);
        ASSERT_TRUE(value.has_value()) << path;
        EXPECT_EQ(path, *value);
    }
    EXPECT_FALSE(lookup("/World/GuideBall").has_value());
}

TEST_F(USDRenderNodeTest, RendersEveryPlaneThroughTheNode)
{
    if (skipWithoutGL()) {
        return;
    }
    NodePtr node = createRender();
    ASSERT_TRUE(node != nullptr);
    std::vector<RenderedPlane> planes;
    std::string error;

    ASSERT_TRUE(renderNodePlanesDirect(node, 1., ViewIdx(0), 0, RectI(0, 0, kWidth, kHeight), allPlanes(), &planes, &error)) << error;
    ASSERT_EQ(3u, planes.size());
    const std::size_t pixels = (std::size_t)kWidth * kHeight;
    ASSERT_EQ(pixels * 4, planes[0].pixels.size());
    ASSERT_EQ(pixels, planes[1].pixels.size());
    ASSERT_EQ(pixels, planes[2].pixels.size());
    const Coverage box = coverageOf("/World/Box", kWidth, kHeight, planes[2].pixels, planes[1].pixels, planes[0].pixels);
    ASSERT_GT(box.count, 100);
    EXPECT_NEAR(9., box.minDepth, 0.01);
    EXPECT_LT(box.y, kHeight / 2.);
    const std::size_t corner = (std::size_t)(kHeight - 1) * kWidth;
    EXPECT_TRUE(std::isinf(planes[1].pixels[corner]));
}

TEST_F(USDRenderNodeTest, APrimCameraTheSceneLacksIsANodeErrorNamingIt)
{
    if (skipWithoutGL()) {
        return;
    }
    NodePtr settings;
    NodePtr node = createRender(kFixture, &settings);
    ASSERT_TRUE(node != nullptr);
    ASSERT_TRUE(RenderSettingsTestUtils::configure(settings, "/World/Gone", kWidth, kHeight));
    std::vector<RenderedPlane> planes;
    std::string error;

    EXPECT_FALSE(renderNodePlanesDirect(node, 1., ViewIdx(0), 0, RectI(0, 0, kWidth, kHeight), allPlanes(), &planes, &error));
    ASSERT_TRUE(node->hasPersistentMessage());
    QString message;
    int type = 0;
    node->getPersistentMessage(&message, &type, false);
    EXPECT_TRUE(message.contains(QString::fromUtf8("/World/Gone"))) << message.toStdString();
}

// A Settings Prim the scene has no RenderSettings prim at is the node's error, naming the path.
TEST_F(USDRenderNodeTest, ASettingsPrimTheSceneLacksIsANodeErrorNamingIt)
{
    NodePtr node = createRender();
    ASSERT_TRUE(node != nullptr);
    KnobChoice* settingsPrim = dynamic_cast<KnobChoice*>(node->getKnobByName(kUSDRenderParamSettingsPrim).get());
    ASSERT_TRUE(settingsPrim);
    settingsPrim->setActiveEntry(ChoiceOption("/Render/gone", "/Render/gone", ""));
    std::string error;
    EXPECT_TRUE(activeRendererOf(node, &error).empty());
    EXPECT_NE(std::string::npos, error.find("/Render/gone")) << error;
    std::vector<RenderedPlane> planes;
    std::string renderError;
    EXPECT_FALSE(renderNodePlanesDirect(node, 1., ViewIdx(0), 0, RectI(0, 0, kWidth, kHeight), allPlanes(), &planes, &renderError));
    ASSERT_TRUE(node->hasPersistentMessage());
    QString message;
    int type = 0;
    node->getPersistentMessage(&message, &type, false);
    EXPECT_TRUE(message.contains(QString::fromUtf8("/Render/gone"))) << message.toStdString();
}

// The stub's library is missing (Tests/fixtures/renderDelegates), so it is listed but unavailable.
const char* const kStubRendererId = "NatronTestStubRendererPlugin";

class USDRenderRendererTest
    : public USDRenderNodeTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        USDRenderNodeTest::TearDown();
    }

    // Saves the project into tmp, resets it and loads the file back.
    bool saveResetLoad(const QTemporaryDir& tmp)
    {
        ProjectPtr project = getApp()->getProject();
        const QString dirPath = tmp.path() + QLatin1Char('/');
        const QString fileName = QString::fromUtf8("usdrender-renderer.ntpz");
        QString savedFilePath;

        if (!project->saveProject(dirPath, fileName, &savedFilePath) || !QFile::exists(savedFilePath)) {
            return false;
        }
        project->reset(false, true);

        return project->loadProject(dirPath, fileName);
    }

    static KnobChoice* rendererKnob(const NodePtr& node)
    {
        return dynamic_cast<KnobChoice*>(node->getKnobByName(kUSDRenderParamRenderer).get());
    }

    static bool selectRenderer(KnobChoice* knob,
                               const std::string& id)
    {
        const std::vector<ChoiceOption> entries = knob->getEntries_mt_safe();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].id == id) {
                knob->setValue((int)i);

                return true;
            }
        }

        return false;
    }

    // Renders node, which must fail, and returns the node's persistent error.
    static std::string renderError(const NodePtr& node)
    {
        std::vector<RenderedPlane> planes;
        std::string error;
        EXPECT_FALSE(renderNodePlanesDirect(node, 1., ViewIdx(0), 0, RectI(0, 0, kWidth, kHeight), allPlanes(), &planes, &error));
        if (!node->hasPersistentMessage()) {
            return std::string();
        }
        QString message;
        int type = 0;
        node->getPersistentMessage(&message, &type, false);

        return message.toStdString();
    }
};

TEST_F(USDRenderRendererTest, TheRendererMenuListsSceneDefaultThenTheRegistry)
{
    NodePtr node = createRender();
    ASSERT_TRUE(bool(node));
    KnobChoice* renderer = rendererKnob(node);
    ASSERT_TRUE(renderer);
    EXPECT_EQ(std::string(kRenderDelegateSceneDefaultId), renderer->getActiveEntry().id);
    const std::vector<RenderDelegates::Delegate> delegates = RenderDelegates::list();
    const std::vector<ChoiceOption> entries = renderer->getEntries_mt_safe();
    ASSERT_EQ(delegates.size() + 1, entries.size());
    EXPECT_EQ(std::string(kRenderDelegateSceneDefaultId), entries[0].id);
    EXPECT_EQ(std::string("Scene Default"), entries[0].label);
    for (std::size_t i = 0; i < delegates.size(); ++i) {
        EXPECT_EQ(delegates[i].id, entries[i + 1].id);
    }
}

// The renderer is the Renderer knob's, else the active prim's natron:renderer, else Storm.
TEST_F(USDRenderRendererTest, TheRendererKnobBeatsThePrimWhichBeatsStorm)
{
    // No render settings prim: Storm.
    NodePtr bare = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDER));
    NodePtr reader = createReader(kFixture);
    ASSERT_TRUE(bare && reader);
    connectNodes(reader, bare, 0, true);
    std::string error;
    EXPECT_EQ(std::string(kRenderDelegateStormId), activeRendererOf(bare, &error)) << error;

    // The prim names the stub.
    NodePtr settings;
    NodePtr node = createRender(kFixture, &settings);
    ASSERT_TRUE(bool(node));
    KnobChoice* defaultRenderer = dynamic_cast<KnobChoice*>(settings->getKnobByName(kUSDRenderSettingsParamDefaultRenderer).get());
    ASSERT_TRUE(defaultRenderer);
    (void)defaultRenderer->setValueFromID(kStubRendererId, 0);
    ASSERT_EQ(std::string(kStubRendererId), defaultRenderer->getActiveEntry().id);
    EXPECT_EQ(std::string(kStubRendererId), activeRendererOf(node, &error)) << error;

    // The Renderer knob beats the prim.
    KnobChoice* renderer = rendererKnob(node);
    ASSERT_TRUE(renderer);
    ASSERT_TRUE(selectRenderer(renderer, kRenderDelegateStormId));
    EXPECT_EQ(std::string(kRenderDelegateStormId), activeRendererOf(node, &error)) << error;
    ASSERT_TRUE(selectRenderer(renderer, kRenderDelegateSceneDefaultId));
    EXPECT_EQ(std::string(kStubRendererId), activeRendererOf(node, &error)) << error;

    // A prim that is not active is not used: Storm.
    KnobBool* makeActive = dynamic_cast<KnobBool*>(settings->getKnobByName(kUSDRenderSettingsParamMakeActive).get());
    ASSERT_TRUE(makeActive);
    makeActive->setValue(false);
    EXPECT_EQ(std::string(kRenderDelegateStormId), activeRendererOf(node, &error)) << error;
}

// The Settings Prim knob beats the stage's renderSettingsPrimPath metadata.
TEST_F(USDRenderRendererTest, TheSettingsPrimKnobBeatsTheStageMetadata)
{
    NodePtr first;
    NodePtr node = createRender(kFixture, &first);
    ASSERT_TRUE(bool(node));
    // A second prim, not active, at another path and size, naming the stub.
    NodePtr second = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDERSETTINGS));
    ASSERT_TRUE(bool(second));
    KnobString* path = dynamic_cast<KnobString*>(second->getKnobByName(kSceneParamPrimPath).get());
    KnobBool* makeActive = dynamic_cast<KnobBool*>(second->getKnobByName(kUSDRenderSettingsParamMakeActive).get());
    KnobChoice* defaultRenderer = dynamic_cast<KnobChoice*>(second->getKnobByName(kUSDRenderSettingsParamDefaultRenderer).get());
    ASSERT_TRUE(path && makeActive && defaultRenderer);
    path->setValue(std::string("/Render/second"));
    makeActive->setValue(false);
    (void)defaultRenderer->setValueFromID(kStubRendererId, 0);
    ASSERT_TRUE(RenderSettingsTestUtils::configure(second, "/World/Cam", 64, 32));
    disconnectNodes(first, node, true);
    connectNodes(first, second, 0, true);
    connectNodes(second, node, 0, true);

    std::string error;
    EXPECT_EQ(std::string(kRenderDelegateStormId), activeRendererOf(node, &error)) << error;
    EXPECT_EQ((double)kWidth, rodOf(node).x2);

    USDRender* effect = renderOf(node);
    ASSERT_TRUE(effect);
    effect->refreshSettingsPrimMenu();
    KnobChoice* settingsPrim = dynamic_cast<KnobChoice*>(node->getKnobByName(kUSDRenderParamSettingsPrim).get());
    ASSERT_TRUE(settingsPrim);
    const std::vector<ChoiceOption> entries = settingsPrim->getEntries_mt_safe();
    ASSERT_EQ(3u, entries.size());
    (void)settingsPrim->setValueFromID("/Render/second", 0);
    ASSERT_EQ(std::string("/Render/second"), settingsPrim->getActiveEntry().id);
    EXPECT_EQ(std::string(kStubRendererId), activeRendererOf(node, &error)) << error;
    const RectD rod = rodOf(node);
    EXPECT_EQ(64., rod.x2);
    EXPECT_EQ(32., rod.y2);
}

// An unavailable renderer stays chosen across a save and load, and every render is an error naming
// it rather than a Storm render.
TEST_F(USDRenderRendererTest, AnUnavailableRendererSurvivesAReloadAsANodeError)
{
    NodePtr node = createRender();
    ASSERT_TRUE(bool(node));
    KnobChoice* renderer = rendererKnob(node);
    ASSERT_TRUE(renderer);
    ASSERT_TRUE(selectRenderer(renderer, kStubRendererId));
    const std::size_t stormEngines = HydraRender::getEngineCount();
    std::string message = renderError(node);
    EXPECT_NE(std::string::npos, message.find(kStubRendererId)) << message;
    EXPECT_EQ(stormEngines, HydraRender::getEngineCount());
    const std::string name = node->getScriptName();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(saveResetLoad(tmp));
    NodePtr loaded = getApp()->getProject()->getNodeByName(name);
    ASSERT_TRUE(bool(loaded));
    KnobChoice* loadedRenderer = rendererKnob(loaded);
    ASSERT_TRUE(loadedRenderer);
    EXPECT_EQ(std::string(kStubRendererId), loadedRenderer->getActiveEntry().id);
    message = renderError(loaded);
    EXPECT_NE(std::string::npos, message.find(kStubRendererId)) << message;
}

// A saved renderer no plug-in provides is kept, as an entry of its own, and is the node's error.
TEST_F(USDRenderRendererTest, ARendererThatIsNotInstalledIsKeptOnLoad)
{
    const std::string missing = "NatronTestNoSuchRendererPlugin";
    NodePtr node = createRender();
    ASSERT_TRUE(bool(node));
    KnobChoice* renderer = rendererKnob(node);
    ASSERT_TRUE(renderer);
    renderer->appendChoice(ChoiceOption(missing, missing, ""));
    ASSERT_TRUE(selectRenderer(renderer, missing));
    const std::string name = node->getScriptName();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(saveResetLoad(tmp));
    NodePtr loaded = getApp()->getProject()->getNodeByName(name);
    ASSERT_TRUE(bool(loaded));
    KnobChoice* loadedRenderer = rendererKnob(loaded);
    ASSERT_TRUE(loadedRenderer);
    EXPECT_EQ(missing, loadedRenderer->getActiveEntry().id);
    const std::vector<ChoiceOption> entries = loadedRenderer->getEntries_mt_safe();
    const int index = loadedRenderer->getValue();
    ASSERT_TRUE((index >= 0) && (index < (int)entries.size()));
    EXPECT_EQ(missing, entries[index].id);
    const std::string message = renderError(loaded);
    EXPECT_NE(std::string::npos, message.find(missing)) << message;
}

// A RenderSettings prim written by hand into a .usda, not by a USDRenderSettings node, read by a
// USDRead: its camera, resolution and cycles:samples are what the render uses. The decoy camera
// comes first in the file, so it is the camera a render without the prim would use.
const char* const kHandWrittenSettings = R"(#usda 1.0
(
    renderSettingsPrimPath = "/Render/shot"
)

def Xform "Decoy"
{
    def Camera "Cam"
    {
        double3 xformOp:translate = (0, 1000, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
}

def Xform "World"
{
    def Cube "Box"
    {
        double size = 2
    }

    def Camera "Cam"
    {
        double3 xformOp:translate = (0, 0, 10)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
}

def Scope "Render"
{
    def RenderSettings "shot"
    {
        rel camera = </World/Cam>
        uniform int2 resolution = (64, 32)
        uniform token aspectRatioConformPolicy = "cropAperture"
        int cycles:samples = 3
    }
}
)";

TEST_F(USDRenderRendererTest, AHandWrittenRenderSettingsPrimIsHonoured)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString filePath = tmp.path() + QString::fromUtf8("/hand-written-settings.usda");
    {
        QFile file(filePath);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_GT(file.write(kHandWrittenSettings), 0);
    }
    const std::string path = filePath.toStdString();
    NodePtr reader = createReader(path.c_str());
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDER));
    ASSERT_TRUE(reader && node);
    connectNodes(reader, node, 0, true);

    USDRender* effect = renderOf(node);
    ASSERT_TRUE(effect);
    effect->refreshSettingsPrimMenu();
    KnobChoice* settingsPrim = dynamic_cast<KnobChoice*>(node->getKnobByName(kUSDRenderParamSettingsPrim).get());
    ASSERT_TRUE(settingsPrim);
    const std::vector<ChoiceOption> entries = settingsPrim->getEntries_mt_safe();
    ASSERT_EQ(2u, entries.size());
    EXPECT_EQ(std::string("/Render/shot"), entries[1].id);

    // The resolution.
    const RectD rod = rodOf(node);
    EXPECT_EQ(64., rod.x2);
    EXPECT_EQ(32., rod.y2);
    std::string error;
    EXPECT_EQ(std::string(kRenderDelegateStormId), activeRendererOf(node, &error)) << error;

    // The camera: /World/Cam sees the box, the decoy sees nothing.
    if (!skipWithoutGL()) {
        std::vector<RenderedPlane> planes;
        ASSERT_TRUE(renderNodePlanesDirect(node, 1., ViewIdx(0), 0, RectI(0, 0, 64, 32), allPlanes(), &planes, &error)) << error;
        ASSERT_EQ(3u, planes.size());
        ASSERT_EQ((std::size_t)64 * 32, planes[1].pixels.size());
        const Coverage box = coverageOf("/World/Box", 64, 32, planes[2].pixels, planes[1].pixels, planes[0].pixels);
        EXPECT_GT(box.count, 20);
        EXPECT_NEAR(9., box.minDepth, 0.01);
    }

    // cycles:samples reaches Cycles.
    if (!CyclesTestUtils::cyclesOrSkip()) {
        return;
    }
    KnobChoice* renderer = rendererKnob(node);
    ASSERT_TRUE(renderer);
    ASSERT_TRUE(selectRenderer(renderer, kRenderDelegateCyclesId));
    std::vector<RenderedPlane> planes;
    std::list<ImageLayerDesc> rgba;
    rgba.push_back(ImageLayerDesc::getRGBAComponents());
    // Bypassing the cache would empty the caches after the render, releasing the engine asked next.
    appPTR->clearAllCaches();
    ASSERT_TRUE(renderNodePlanesDirect(node, 1., ViewIdx(0), 0, RectI(0, 0, 64, 32), rgba, &planes, &error, false)) << error;
    PXR_NS::VtValue samples;
    ASSERT_TRUE(HydraRender::engineSetting(effect, "cycles:samples", &samples));
    EXPECT_EQ(PXR_NS::VtValue(3), samples);
    HydraRender::releaseAllCpuEngines();
}

namespace {
// A box seen by /World/Cam, whose shutter opens and closes at open and close; settings, attribute
// lines for the RenderSettings prim, override it.
std::string
shutterScene(double open,
             double close,
             const std::string& settings)
{
    return std::string("#usda 1.0\n(\n    renderSettingsPrimPath = \"/Render/shot\"\n)\n\n"
                       "def Xform \"World\"\n{\n    def Cube \"Box\"\n    {\n        double size = 2\n    }\n\n"
                       "    def Camera \"Cam\"\n    {\n        double3 xformOp:translate = (0, 0, 10)\n"
                       "        uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                       "        double shutter:open = ")
        + std::to_string(open) + "\n        double shutter:close = " + std::to_string(close) + "\n    }\n}\n\n"
        + "def Scope \"Render\"\n{\n    def RenderSettings \"shot\"\n    {\n        rel camera = </World/Cam>\n"
          "        uniform int2 resolution = (32, 16)\n"
        + settings + "    }\n}\n";
}
} // anon namespace

TEST_F(USDRenderRendererTest, TheShutterReachesNatronUtilityAndTheHash)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string id = kRenderDelegateUtilityId;
    const std::string overrides = std::string("        bool ") + RenderSettingsPrim::attributeName(id, kUtilitySettingOverrideShutter) + " = 1\n"
        + "        float " + RenderSettingsPrim::attributeName(id, kUtilitySettingShutter) + " = 1\n"
        + "        token " + RenderSettingsPrim::attributeName(id, kUtilitySettingShutterOffset) + " = \"" + kUtilityShutterStart + "\"\n";
    const std::string noBlur = std::string("        bool ") + RenderSettingsPrim::attributeName(id, kUtilitySettingCameraMotionBlur) + " = 0\n"
        + "        bool " + RenderSettingsPrim::attributeName(id, kUtilitySettingTransformMotionBlur) + " = 0\n"
        + "        bool " + RenderSettingsPrim::attributeName(id, kUtilitySettingDeformationMotionBlur) + " = 0\n";
    struct Case {
        const char* name;
        std::string text;
        PXR_NS::GfVec2d interval;
    };
    const std::vector<Case> cases = {
        { "centred", shutterScene(-0.25, 0.25, std::string()), PXR_NS::GfVec2d(-0.25, 0.25) },
        { "opening", shutterScene(0., 0.5, std::string()), PXR_NS::GfVec2d(0., 0.5) },
        { "overridden", shutterScene(0., 0.5, overrides), PXR_NS::GfVec2d(0., 1.) },
        { "unblurred", shutterScene(0., 0.5, noBlur), PXR_NS::GfVec2d(0., 0.) },
    };
    std::vector<std::string> paths;
    for (const Case& c : cases) {
        const QString filePath = tmp.path() + QString::fromUtf8("/shutter-") + QString::fromUtf8(c.name) + QString::fromUtf8(".usda");
        QFile file(filePath);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_GT(file.write(c.text.c_str()), 0);
        paths.push_back(filePath.toStdString());
    }
    NodePtr reader = createReader(paths[0].c_str());
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDER));
    ASSERT_TRUE(reader && node);
    connectNodes(reader, node, 0, true);
    USDRender* effect = renderOf(node);
    ASSERT_TRUE(effect);
    effect->refreshSettingsPrimMenu();
    ASSERT_TRUE(selectRenderer(rendererKnob(node), kRenderDelegateUtilityId));
    KnobFile* file = dynamic_cast<KnobFile*>(reader->getKnobByName(kUSDReadParamFile).get());
    ASSERT_TRUE(file);
    HydraRender::setRendererSettings(kRenderDelegateUtilityId, { { kUtilitySettingPixelSamples, PXR_NS::VtValue(1) } });

    std::set<U64> hashes;
    for (std::size_t i = 0; i < cases.size(); ++i) {
        file->setValue(paths[i]);
        EXPECT_TRUE(hashes.insert(node->getHashValue()).second) << cases[i].name << " left the hash of another shutter";
        std::vector<RenderedPlane> planes;
        std::list<ImageLayerDesc> rgba;
        rgba.push_back(ImageLayerDesc::getRGBAComponents());
        std::string error;
        appPTR->clearAllCaches();
        ASSERT_TRUE(renderNodePlanesDirect(node, 1., ViewIdx(0), 0, RectI(0, 0, 32, 16), rgba, &planes, &error, false)) << cases[i].name << ": " << error;
        PXR_NS::VtValue interval;
        ASSERT_TRUE(HydraRender::engineSetting(effect, kRenderDelegateUtilityShutterSetting, &interval)) << cases[i].name;
        ASSERT_TRUE(interval.IsHolding<PXR_NS::GfVec2d>()) << cases[i].name;
        EXPECT_EQ(cases[i].interval, interval.UncheckedGet<PXR_NS::GfVec2d>()) << cases[i].name;
    }
    HydraRender::setRendererSettings(kRenderDelegateUtilityId, {});
    HydraRender::releaseAllCpuEngines();
}

TEST_F(USDRenderRendererTest, ChangingTheRendererChangesTheHash)
{
    NodePtr node = createRender();
    ASSERT_TRUE(bool(node));
    KnobChoice* renderer = rendererKnob(node);
    ASSERT_TRUE(renderer);
    const U64 stormHash = node->getHashValue();
    ASSERT_TRUE(selectRenderer(renderer, kStubRendererId));
    const U64 stubHash = node->getHashValue();
    EXPECT_NE(stormHash, stubHash);
    ASSERT_TRUE(selectRenderer(renderer, kRenderDelegateSceneDefaultId));
    EXPECT_EQ(stormHash, node->getHashValue());
}

// HDX_MSAA_SAMPLE_COUNT is read once per process and would change the bits of every other Storm render
// in it, so Tests/SuiteProperties.cmake sets it for this suite's process alone.
class USDRenderSingleSampleTest
    : public USDRenderRendererTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        USDRenderRendererTest::SetUp();
        const char* samples = std::getenv("HDX_MSAA_SAMPLE_COUNT");
        ASSERT_TRUE(samples && (std::strcmp(samples, "1") == 0)) << "run this suite alone with HDX_MSAA_SAMPLE_COUNT=1, as ctest does";
    }
};

TEST_F(USDRenderSingleSampleTest, NatronUtilityMatchesStorm)
{
    if (skipWithoutGL()) {
        return;
    }
    NodePtr node = createRender();
    ASSERT_TRUE(bool(node));
    KnobChoice* renderer = rendererKnob(node);
    ASSERT_TRUE(renderer);
    const RectI window(0, 0, kWidth, kHeight);
    std::vector<RenderedPlane> storm;
    std::vector<RenderedPlane> utility;
    std::string error;

    ASSERT_TRUE(renderNodePlanesDirect(node, 1., ViewIdx(0), 0, window, allPlanes(), &storm, &error)) << error;
    ASSERT_TRUE(selectRenderer(renderer, kRenderDelegateUtilityId));
    ASSERT_TRUE(renderNodePlanesDirect(node, 1., ViewIdx(0), 0, window, allPlanes(), &utility, &error)) << error;
    ASSERT_EQ(3u, storm.size());
    ASSERT_EQ(3u, utility.size());

    const std::size_t pixels = (std::size_t)kWidth * kHeight;
    ASSERT_EQ(pixels * 4, utility[0].pixels.size());
    ASSERT_EQ(pixels, utility[1].pixels.size());
    ASSERT_EQ(pixels, utility[2].pixels.size());

    const std::size_t compared = expectMatchesSingleSampleStorm(storm[1].pixels, storm[2].pixels, utility[1].pixels, utility[2].pixels);
    EXPECT_GT(compared, 100u);

    const float boxId = HydraRender::hashToFloat(HydraRender::primPathHash("/World/Box"));
    const float ballId = HydraRender::hashToFloat(HydraRender::primPathHash("/World/Ball"));
    std::size_t boxPixels = 0;
    std::size_t ballPixels = 0;
    for (std::size_t i = 0; i < pixels; ++i) {
        if (sameBits(utility[2].pixels[i], ballId)) {
            ++ballPixels;
        } else if (sameBits(utility[2].pixels[i], boxId)) {
            ++boxPixels;
            EXPECT_NEAR(0.8f, utility[0].pixels[i * 4], 1e-5);
            EXPECT_NEAR(0.2f, utility[0].pixels[i * 4 + 1], 1e-5);
            EXPECT_NEAR(0.1f, utility[0].pixels[i * 4 + 2], 1e-5);
            EXPECT_EQ(1.f, utility[0].pixels[i * 4 + 3]);
        }
    }
    EXPECT_GT(boxPixels, 100u);
    EXPECT_GT(ballPixels, 0u);
}

TEST_F(USDRenderSingleSampleTest, NatronUtilityPicksNestedInstancesAsStormDoes)
{
    if (skipWithoutGL()) {
        return;
    }
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(kNestedInstancesFixture);
    ASSERT_TRUE(stage);
    const int stormOwner = 0;
    const int utilityOwner = 0;
    HydraRender::Request request = referenceRequest(stage, 1., &stormOwner);
    HydraRender::Result storm;
    HydraRender::Result utility;
    std::string error;

    ASSERT_TRUE(HydraRender::render(request, &storm, &error)) << error;
    expectNestedInstancesInPlace(storm);
    request.owner = &utilityOwner;
    request.renderer = kRenderDelegateUtilityId;
    ASSERT_TRUE(HydraRender::render(request, &utility, &error)) << error;
    EXPECT_GT(expectMatchesSingleSampleStorm(storm.depth, storm.primId, utility.depth, utility.primId), 100u);
    HydraRender::releaseAll();
}

TEST_F(USDRenderRendererTest, ThePurposeKnobsDefaultToRenderOnlyAndChooseWhatEachRendererDraws)
{
    NodePtr node = createRender(kPurposeFixture);
    ASSERT_TRUE(bool(node));
    KnobBool* render = dynamic_cast<KnobBool*>(node->getKnobByName(kUSDRenderParamRenderPurpose).get());
    KnobBool* proxy = dynamic_cast<KnobBool*>(node->getKnobByName(kUSDRenderParamProxyPurpose).get());
    KnobBool* guide = dynamic_cast<KnobBool*>(node->getKnobByName(kUSDRenderParamGuidePurpose).get());
    KnobChoice* renderer = rendererKnob(node);
    ASSERT_TRUE(render && proxy && guide && renderer);
    EXPECT_EQ(std::string("Render Purpose"), render->getLabel());
    EXPECT_EQ(std::string("Proxy Purpose"), proxy->getLabel());
    EXPECT_EQ(std::string("Guide Purpose"), guide->getLabel());
    EXPECT_TRUE(render->getValue());
    EXPECT_FALSE(proxy->getValue());
    EXPECT_FALSE(guide->getValue());

    const std::string defaultPrim = "/World/Default";
    const std::string renderPrim = "/World/Render";
    const std::string proxyPrim = "/World/Proxy";
    const std::string guidePrim = "/World/Guide";
    const std::vector<std::string> prims = { defaultPrim, renderPrim, proxyPrim, guidePrim };
    const auto shown = [&node, &prims]() {
        std::vector<RenderedPlane> planes;
        std::string error;

        EXPECT_TRUE(renderNodePlanesDirect(node, 1., ViewIdx(0), 0, RectI(0, 0, kWidth, kHeight), allPlanes(), &planes, &error)) << error;

        return planes.size() == 3 ? primsIn(planes[2].pixels, prims) : std::set<std::string>();
    };
    const auto expectToggles = [&]() {
        EXPECT_EQ(std::set<std::string>({ defaultPrim, renderPrim }), shown());
        const U64 defaultHash = node->getHashValue();
        proxy->setValue(true);
        EXPECT_NE(defaultHash, node->getHashValue());
        EXPECT_EQ(std::set<std::string>({ defaultPrim, renderPrim, proxyPrim }), shown());
        render->setValue(false);
        EXPECT_EQ(std::set<std::string>({ defaultPrim, proxyPrim }), shown());
        guide->setValue(true);
        EXPECT_EQ(std::set<std::string>({ defaultPrim, proxyPrim, guidePrim }), shown());
        proxy->setValue(false);
        guide->setValue(false);
        render->setValue(true);
        EXPECT_EQ(std::set<std::string>({ defaultPrim, renderPrim }), shown());
    };

    ASSERT_TRUE(selectRenderer(renderer, kRenderDelegateUtilityId));
    expectToggles();
    if (skipWithoutGL()) {
        return;
    }
    ASSERT_TRUE(selectRenderer(renderer, kRenderDelegateStormId));
    expectToggles();
}

#ifdef NATRON_HAVE_GPU
namespace {
// Puts a forced GPU context in the application's place while it lives, since the application's may
// refuse a CPU-type device. The Utility engines hold the context's runtime, so they go first. A non-zero
// poolBudget caps the runtime's buffer pool, so that a frame runs out of device memory.
class OwnGpuContext {
public:
    explicit OwnGpuContext(std::uint64_t poolBudget = 0)
    {
        GpuContextOptions options;
        std::shared_ptr<std::string> logged = std::make_shared<std::string>();

        options.mode = GpuResolvedMode::Force;
        options.device = gputest::cpuFallbackSelector();
        options.infoLog = [logged](const std::string& line) {
            *logged = line;
        };

        std::unique_ptr<GpuContext> own(new GpuContext(options));

        if (!own->waitForInit(std::chrono::seconds(60)) || !own->isAvailable()) {
            unavailable = logged->empty() ? std::string("the GPU context did not come up") : *logged;

            return;
        }
        if (poolBudget) {
            own->runtime()->pool().setUserBudget(poolBudget);
        }
        _saved = appPTR->swapGpuContextForTests(std::move(own));
        _swapped = true;
    }

    ~OwnGpuContext()
    {
        HydraRender::releaseAll();
        appPTR->clearAllCaches();
        if (_swapped) {
            appPTR->swapGpuContextForTests(std::move(_saved));
        }
    }

    gpu::GpuRuntime* runtime() const
    {
        return _swapped ? appPTR->getGpuContext()->runtime() : nullptr;
    }

    std::string unavailable;

private:
    std::unique_ptr<GpuContext> _saved;
    bool _swapped = false;
};

const std::uint64_t kStarvedPoolBytes = 1024;
} // anon namespace

TEST_F(USDRenderRendererTest, AnAutoUtilityRenderThatFallsBackToTheCpuWarnsOnTheNode)
{
    OwnGpuContext gpu(kStarvedPoolBytes);

    GPUTEST_DEVICE_OR_SKIP(gpu.unavailable.empty(), gpu.unavailable);

    const std::string missing = WavefrontGpu::missingSupport(*gpu.runtime());

    if (!missing.empty()) {
        GPUTEST_SKIP() << missing;
    }

    NodePtr node = createRender();
    ASSERT_TRUE(bool(node));
    KnobChoice* renderer = rendererKnob(node);
    ASSERT_TRUE(renderer);
    ASSERT_TRUE(selectRenderer(renderer, kRenderDelegateUtilityId));
    HydraRender::releaseAll();

    std::vector<RenderedPlane> planes;
    std::string error;

    ASSERT_TRUE(renderNodePlanesDirect(node, 1., ViewIdx(0), 0, RectI(0, 0, kWidth, kHeight), allPlanes(), &planes, &error)) << error;
    ASSERT_TRUE(node->hasPersistentMessage());

    QString message;
    int type = -1;

    node->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeWarning, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("out of memory"))) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8("Rendered on CPU"))) << message.toStdString();
    HydraRender::releaseAll();
}

namespace {
struct SchedulerFrame {
    AbortableRenderInfoPtr abortInfo;
    RenderStatsPtr stats;
    FrameRenderContextPtr context;
    FrameFuturePtr future;
};
} // anon namespace

class USDRenderUtilityGpuTest
    : public USDRenderRendererTest {
protected:
    // Sets the Device menu the settings node shows for Natron Utility to the entry device.
    static bool selectUtilityDevice(const NodePtr& settings,
                                    const std::string& device)
    {
        const USDRenderSettings* effect = dynamic_cast<const USDRenderSettings*>(settings->getEffectInstance().get());
        const RenderSettingsKnobs* knobs = effect ? effect->renderSettings() : nullptr;

        if (!knobs) {
            return false;
        }
        for (const std::pair<std::string, std::string>& setting : knobs->audit(kRenderDelegateUtilityId)) {
            if (setting.first == kUtilitySettingDevice) {
                KnobChoice* knob = dynamic_cast<KnobChoice*>(settings->getKnobByName(setting.second).get());

                return knob && selectRenderer(knob, device);
            }
        }

        return false;
    }

    // Renders root's colour over the format as a frame of the scheduler, with the tasks of gpuNodes
    // placed on the GPU as placeGraph() would if every region were accepted.
    bool renderFrame(const NodePtr& root,
                     const std::vector<NodePtr>& gpuNodes,
                     SchedulerFrame* run,
                     std::string* error)
    {
        const double time = 1.;

        run->abortInfo = AbortableRenderInfo::create(true, 0);
        run->stats = std::make_shared<RenderStats>(false);

        ParallelRenderArgsSetter frameArgs(time, ViewIdx(0), false, false, run->abortInfo, root, 0, getApp()->getTimeLine().get(), NodePtr(), false, false, run->stats);
        std::shared_ptr<FrameRequestMap> request = std::make_shared<FrameRequestMap>();

        if (EffectInstance::computeRequestPass(time, ViewIdx(0), 0, RectD(0, 0, kWidth, kHeight), root, *request) == eStatusFailed) {
            *error = "the request pass failed";

            return false;
        }
        frameArgs.updateNodesRequest(*request);
        run->context = FrameRenderContext::create(time, ViewIdx(0), false, false, run->abortInfo, root, 0, getApp()->getTimeLine().get(), NodePtr(), false, false, run->stats);
        run->context->setRequest(request);

        FrameGraph graph = RenderScheduler::buildGraph(run->context, root, time, ViewIdx(0), 0);

        if (graph.tasks.empty()) {
            *error = "the frame has no task";

            return false;
        }
        // A root nobody consumes asks for every plane it produces, and a task of several planes stays on the CPU;
        // the viewer narrows its root to the plane it shows the same way.
        graph.tasks.front().components = std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents());

        std::set<const Node*> onGpu;

        for (const NodePtr& node : gpuNodes) {
            onGpu.insert(node.get());
        }

        const FrameRenderContextPtr context = run->context;
        PlacementInputs inputs;

        inputs.canRenderOnGpu = [onGpu, context](const FrameGraph::Task& task, GpuRefusal*) {
            GpuQuery query;
            const NativeEffectBase* native = dynamic_cast<const NativeEffectBase*>(task.key.node->getEffectInstance().get());

            return onGpu.count(task.key.node.get()) && makeGpuQueryForTask(task, *context, &query) && native && native->canRenderOnGpu(query);
        };
        placeGraph(graph, inputs);
        run->future = appPTR->getRenderScheduler()->submit(run->context, std::move(graph), RenderScheduler::Priority::Interactive);
        if (run->future->wait() != EffectInstance::eRenderRoIRetCodeOk) {
            *error = "the frame failed";

            return false;
        }

        return true;
    }

    static int gpuRunsOf(const SchedulerFrame& run,
                         const NodePtr& node)
    {
        const std::map<std::string, int> runs = run.stats->getGpuTasksPerNode();
        const std::map<std::string, int>::const_iterator found = runs.find(node->getScriptName_mt_safe());

        return (found == runs.end()) ? 0 : found->second;
    }

    // The frame's root colour as float RGBA rows from the bottom one, empty if it has none.
    static std::vector<float> rootRgba(const SchedulerFrame& run)
    {
        const std::map<ImageLayerDesc, ImagePtr> planes = run.future->getRootPlanes();

        if (planes.empty() || !planes.begin()->second || (planes.begin()->second->getBitDepth() != eImageBitDepthFloat)) {
            return std::vector<float>();
        }

        const ImagePtr& image = planes.begin()->second;
        const int nComps = (int)image->getComponentsCount();
        std::vector<float> out((std::size_t)kWidth * kHeight * 4, 0.f);
        Image::ReadAccess access = image->getReadRights();

        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const float* pixel = (const float*)access.pixelAt(x, y);

                if (!pixel) {
                    continue;
                }
                for (int c = 0; c < std::min(nComps, 4); ++c) {
                    out[((std::size_t)y * kWidth + x) * 4 + c] = pixel[c];
                }
            }
        }

        return out;
    }
};

// A GPU frame's colour stays on the device: a GPU Grade reads it there, so the frame downloads only
// its root, and a CPU render downloads the same pixels from the device when it maps them.
TEST_F(USDRenderUtilityGpuTest, AGpuGradeReadsTheGpuFrameOnTheDeviceAndTheCpuMapsTheSamePixels)
{
    OwnGpuContext gpu;

    GPUTEST_DEVICE_OR_SKIP(gpu.unavailable.empty(), gpu.unavailable);

    const std::string missing = WavefrontGpu::missingSupport(*gpu.runtime());

    if (!missing.empty()) {
        GPUTEST_SKIP() << "Natron Utility cannot render on this GPU: " << missing;
    }

    NodePtr settings;
    NodePtr render = createRender(kFixture, &settings);

    ASSERT_TRUE(bool(render));
    ASSERT_TRUE(selectRenderer(rendererKnob(render), kRenderDelegateUtilityId));
    ASSERT_TRUE(selectUtilityDevice(settings, kUtilityDeviceGpu));

    NodePtr grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE), PLUGIN_MAJOR_NATRON_GRADE);

    ASSERT_TRUE(bool(grade));
    ASSERT_TRUE(setKnobValues(grade, "multiply", { 2., 2., 2., 2. }));
    ASSERT_TRUE(setKnobValues(grade, "offset", { 0.1, 0.1, 0.1, 0.1 }));
    connectNodes(render, grade, 0, true);
    HydraRender::releaseAll();

    const std::uint64_t mapped = UtilityRenderBuffer::deviceDownloadCount();
    SchedulerFrame graded;
    std::string error;

    ASSERT_TRUE(renderFrame(grade, { render, grade }, &graded, &error)) << error;
    EXPECT_LT(0, gpuRunsOf(graded, render));
    EXPECT_LT(0, gpuRunsOf(graded, grade));
    EXPECT_EQ(0, graded.stats->getUploadCount());
    EXPECT_EQ(1, graded.stats->getDownloadCount()) << "only the root comes back to the host";
    EXPECT_EQ(0, graded.context->getStore().downloadCount());
    EXPECT_EQ(mapped, UtilityRenderBuffer::deviceDownloadCount()) << "the film was mapped on the host";

    SchedulerFrame plain;

    ASSERT_TRUE(renderFrame(render, { render }, &plain, &error)) << error;
    EXPECT_LT(0, gpuRunsOf(plain, render));
    EXPECT_EQ(mapped, UtilityRenderBuffer::deviceDownloadCount()) << "the film was mapped on the host";

    const std::vector<float> onGpu = rootRgba(plain);
    const std::vector<float> gradedOnGpu = rootRgba(graded);
    std::vector<RenderedPlane> planes;

    ASSERT_TRUE(renderNodePlanesDirect(render, 1., ViewIdx(0), 0, RectI(0, 0, kWidth, kHeight), std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error)) << error;
    EXPECT_GT(UtilityRenderBuffer::deviceDownloadCount(), mapped) << "the CPU render did not map a GPU frame";
    ASSERT_EQ(1u, planes.size());

    const std::vector<float>& onCpu = planes.front().pixels;

    ASSERT_EQ((std::size_t)kWidth * kHeight * 4, onCpu.size());
    ASSERT_EQ(onCpu.size(), onGpu.size());
    ASSERT_EQ(onCpu.size(), gradedOnGpu.size());
    EXPECT_EQ(0, std::memcmp(onCpu.data(), onGpu.data(), onCpu.size() * sizeof(float)));

    std::size_t covered = 0;

    for (std::size_t i = 0; i < onCpu.size(); i += 4) {
        covered += (onCpu[i + 3] > 0.f) ? 1 : 0;
        for (std::size_t c = 0; c < 3; ++c) {
            const float want = 2.f * onCpu[i + c] + 0.1f;

            ASSERT_NEAR(want, gradedOnGpu[i + c], 1e-5f * std::max(1.f, std::fabs(want))) << "pixel " << (i / 4) << " channel " << c;
        }
    }
    EXPECT_GT(covered, 100u) << "the frame is empty, which proves nothing";
}

// An Auto frame that ran out of GPU memory keeps later Auto frames off the GPU, but not a frame the
// Device setting forces onto it.
TEST_F(USDRenderUtilityGpuTest, AnAutoFallbackDoesNotRefuseAForcedGpuRender)
{
    OwnGpuContext gpu(kStarvedPoolBytes);

    GPUTEST_DEVICE_OR_SKIP(gpu.unavailable.empty(), gpu.unavailable);

    const std::string missing = WavefrontGpu::missingSupport(*gpu.runtime());

    if (!missing.empty()) {
        GPUTEST_SKIP() << "Natron Utility cannot render on this GPU: " << missing;
    }

    NodePtr settings;
    NodePtr render = createRender(kFixture, &settings);

    ASSERT_TRUE(bool(render));
    ASSERT_TRUE(selectRenderer(rendererKnob(render), kRenderDelegateUtilityId));
    ASSERT_TRUE(selectUtilityDevice(settings, kUtilityDeviceAuto));
    HydraRender::releaseAll();

    const NativeEffectBase* native = dynamic_cast<const NativeEffectBase*>(render->getEffectInstance().get());

    ASSERT_TRUE(native);

    GpuQuery query;

    query.time = 1.;
    query.view = ViewIdx(0);
    query.plane = ImageLayerDesc::getRGBAComponents();
    query.nComps = 4;
    query.bitDepth = eImageBitDepthFloat;
    ASSERT_TRUE(native->canRenderOnGpu(query));

    SchedulerFrame run;
    std::string error;

    ASSERT_TRUE(renderFrame(render, { render }, &run, &error)) << error;
    ASSERT_FALSE(native->canRenderOnGpu(query)) << "the Auto frame did not fall back to the CPU, which proves nothing";

    ASSERT_TRUE(selectUtilityDevice(settings, kUtilityDeviceGpu));
    EXPECT_TRUE(native->canRenderOnGpu(query)) << "an Auto fallback refused a frame forced onto the GPU";

    ASSERT_TRUE(selectUtilityDevice(settings, kUtilityDeviceAuto));
    EXPECT_FALSE(native->canRenderOnGpu(query));
    HydraRender::releaseAll();
}

namespace {
// Forces the GPU regions into strips of rows rows while it lives. The row cap alone only bounds the
// height of strips a region already needs, so the buffer range is capped below a whole RGBA frame too.
class StripRows {
public:
    explicit StripRows(int rows)
    {
        const std::uint64_t wholeFrameBytes = (std::uint64_t)kWidth * kHeight * 4 * sizeof(float);

        setGpuStripOverridesForTests(wholeFrameBytes / 2, rows);
    }

    ~StripRows()
    {
        setGpuStripOverridesForTests(0, 0);
    }
};
} // anon namespace

// Each strip of a region copies its window out of one GPU frame, so the frame renders once however
// many strips there are, and gives the pixels the CPU maps.
TEST_F(USDRenderUtilityGpuTest, AStrippedRegionRendersTheUtilityFrameOnce)
{
    OwnGpuContext gpu;

    GPUTEST_DEVICE_OR_SKIP(gpu.unavailable.empty(), gpu.unavailable);

    const std::string missing = WavefrontGpu::missingSupport(*gpu.runtime());

    if (!missing.empty()) {
        GPUTEST_SKIP() << "Natron Utility cannot render on this GPU: " << missing;
    }

    NodePtr settings;
    NodePtr render = createRender(kFixture, &settings);

    ASSERT_TRUE(bool(render));
    ASSERT_TRUE(selectRenderer(rendererKnob(render), kRenderDelegateUtilityId));
    ASSERT_TRUE(selectUtilityDevice(settings, kUtilityDeviceGpu));
    HydraRender::releaseAll();

    const std::uint64_t renders = HydraRender::getEngineRenderCount();
    SchedulerFrame stripped;
    std::string error;

    {
        const StripRows strips(32);

        ASSERT_TRUE(renderFrame(render, { render }, &stripped, &error)) << error;
    }
    EXPECT_LT(0, gpuRunsOf(stripped, render));
    EXPECT_GT(stripped.stats->getDownloadCount(), 1) << "the region did not run as strips";
    EXPECT_EQ(renders + 1, HydraRender::getEngineRenderCount()) << "the strips did not share one frame";

    const std::vector<float> onGpu = rootRgba(stripped);
    std::vector<RenderedPlane> planes;

    ASSERT_TRUE(renderNodePlanesDirect(render, 1., ViewIdx(0), 0, RectI(0, 0, kWidth, kHeight), std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error)) << error;
    ASSERT_EQ(1u, planes.size());

    const std::vector<float>& onCpu = planes.front().pixels;

    ASSERT_EQ((std::size_t)kWidth * kHeight * 4, onCpu.size());
    ASSERT_EQ(onCpu.size(), onGpu.size());
    EXPECT_EQ(0, std::memcmp(onCpu.data(), onGpu.data(), onCpu.size() * sizeof(float)));
}
#endif // NATRON_HAVE_GPU

TEST_F(USDRenderRendererTest, TheMetadataFollowsThePurposeKnobs)
{
    NodePtr node = createRender(kPurposeFixture);
    ASSERT_TRUE(bool(node));
    NativeEffectBase* effect = dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
    KnobBool* render = dynamic_cast<KnobBool*>(node->getKnobByName(kUSDRenderParamRenderPurpose).get());
    KnobBool* proxy = dynamic_cast<KnobBool*>(node->getKnobByName(kUSDRenderParamProxyPurpose).get());
    KnobBool* guide = dynamic_cast<KnobBool*>(node->getKnobByName(kUSDRenderParamGuidePurpose).get());
    ASSERT_TRUE(effect && render && proxy && guide);
    const auto named = [effect](const char* path) {
        char key[32];
        std::snprintf(key, sizeof(key), kUSDRenderMetadataPrimIdPrefix "%08x", HydraRender::primPathHash(path));

        return effect->getOutputMetadata(1., ViewIdx(0)).getString(key).has_value();
    };

    EXPECT_TRUE(named("/World/Default"));
    EXPECT_TRUE(named("/World/Render"));
    EXPECT_FALSE(named("/World/Proxy"));
    EXPECT_FALSE(named("/World/Guide"));
    render->setValue(false);
    proxy->setValue(true);
    guide->setValue(true);
    EXPECT_TRUE(named("/World/Default"));
    EXPECT_FALSE(named("/World/Render"));
    EXPECT_TRUE(named("/World/Proxy"));
    EXPECT_TRUE(named("/World/Guide"));
}

// Each level of a nested instancer multiplies the flattened instance list, and the decoded path names
// the instance only if its index counts through the levels in the order Storm's does.
TEST(USDRenderRenderers, NatronUtilityDecodesNestedInstancesToTheirOwnPaths)
{
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(kNestedInstancesFixture);
    ASSERT_TRUE(stage);
    const int owner = 0;
    HydraRender::Request request = referenceRequest(stage, 1., &owner);
    request.renderer = kRenderDelegateUtilityId;
    HydraRender::Result utility;
    std::string error;

    ASSERT_TRUE(HydraRender::render(request, &utility, &error)) << error;
    expectNestedInstancesInPlace(utility);
    HydraRender::releaseAll();
}

// The abort is first polled 20 ms in, and the render is large enough to be drawing tiles by then and
// nowhere near done. Keeping the engine is the proof the delegate stopped: one that cannot stop has
// its engine dropped.
TEST(USDRenderRenderers, CancellingNatronUtilityStopsItKeepsItsEngineAndTheNextRenderWorks)
{
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(kFixture);
    ASSERT_TRUE(stage);
    const int owner = 0;
    HydraRender::Request request = referenceRequest(stage, 1., &owner);
    request.renderer = kRenderDelegateUtilityId;
    HydraRender::Result before;
    std::string error;

    ASSERT_TRUE(HydraRender::render(request, &before, &error)) << error;
    const std::uint64_t builds = HydraRender::getEngineBuildCount();

    HydraRender::Request large = request;
    large.width = 4096;
    large.height = 4096;
    large.aborted = []() { return true; };
    HydraRender::Result cancelled;
    EXPECT_FALSE(HydraRender::render(large, &cancelled, &error));
    EXPECT_NE(std::string::npos, error.find("cancelled")) << error;
    EXPECT_EQ(builds, HydraRender::getEngineBuildCount());

    HydraRender::Result after;
    error.clear();
    ASSERT_TRUE(HydraRender::render(request, &after, &error)) << error;
    EXPECT_EQ(builds, HydraRender::getEngineBuildCount());
    ASSERT_EQ(before.depth.size(), after.depth.size());
    EXPECT_EQ(0, std::memcmp(before.depth.data(), after.depth.data(), before.depth.size() * sizeof(float)));
    EXPECT_EQ(0, std::memcmp(before.primId.data(), after.primId.data(), before.primId.size() * sizeof(float)));
    HydraRender::releaseAll();
}

// Free GPU Memory drops USDRender's Storm engines and the device buffers the GPU runtime's pool
// keeps free, and the next render and allocation make what they need again.
TEST_F(USDRenderNodeTest, FreeGpuMemoryReleasesEnginesAndPooledBuffers)
{
    if (skipWithoutGL()) {
        return;
    }
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(kFixture);
    ASSERT_TRUE(stage);
    const int owner = 0;
    std::string error;
    HydraRender::Result result;

#ifdef NATRON_HAVE_GPU
    // A context of the test's own, since the application's may refuse a CPU-type device.
    GpuContextOptions options;
    options.mode = GpuResolvedMode::Force;
    options.device = gputest::cpuFallbackSelector();
    options.infoLog = [](const std::string&) { };
    std::unique_ptr<GpuContext> own(new GpuContext(options));
    const bool haveGpu = own->waitForInit(std::chrono::seconds(60)) && own->isAvailable();
    gpu::GpuRuntime* runtime = haveGpu ? own->runtime() : nullptr;
    // Puts the application's context back however the test ends.
    struct RestoreContext {
        std::unique_ptr<GpuContext> saved;
        bool swapped = false;
        ~RestoreContext()
        {
            if (swapped) {
                appPTR->swapGpuContextForTests(std::move(saved));
            }
        }
    } restore;
    if (haveGpu) {
        restore.saved = appPTR->swapGpuContextForTests(std::move(own));
        restore.swapped = true;
    } else {
        reportNoGpu("the GPU context did not come up; only the Storm engines are checked");
    }
    // A buffer acquired and dropped stays in the pool, free for the next acquire.
    const VkDeviceSize bytes = 1u << 20;
    if (runtime) {
        runtime->pool().trim();
        gpu::GpuBufferHandle handle;
        const gpu::GpuStatus status = runtime->acquire(bytes, handle);
        ASSERT_TRUE(status.ok()) << status.message;
    }
    if (runtime) {
        const gpu::GpuBufferPoolStats pooled = runtime->pool().stats();
        EXPECT_EQ(0u, pooled.liveBytes);
        EXPECT_GE(pooled.pooledBytes, (std::uint64_t)bytes);
    }
#endif

    ASSERT_TRUE(HydraRender::render(referenceRequest(stage, 1., &owner), &result, &error)) << error;
    EXPECT_EQ(1u, HydraRender::getEngineCount());

    appPTR->freeGpuMemory();
    EXPECT_EQ(0u, HydraRender::getEngineCount());
#ifdef NATRON_HAVE_GPU
    if (runtime) {
        const gpu::GpuBufferPoolStats freed = runtime->pool().stats();
        EXPECT_EQ(0u, freed.liveBytes);
        EXPECT_EQ(0u, freed.pooledBytes);
    }
#endif

    // Rendering goes on: the next render builds an engine, and the next acquire allocates a buffer.
    const std::uint64_t builds = HydraRender::getEngineBuildCount();
    ASSERT_TRUE(HydraRender::render(referenceRequest(stage, 2., &owner), &result, &error)) << error;
    EXPECT_EQ(1u, HydraRender::getEngineCount());
    EXPECT_EQ(builds + 1, HydraRender::getEngineBuildCount());
    const Coverage box = coverageOf("/World/Box", kWidth, kHeight, result.primId, result.depth, result.rgba);
    EXPECT_GT(box.count, 100);
#ifdef NATRON_HAVE_GPU
    if (runtime) {
        const std::uint64_t allocations = runtime->pool().allocationCount();
        gpu::GpuBufferHandle handle;
        const gpu::GpuStatus status = runtime->acquire(bytes, handle);
        EXPECT_TRUE(status.ok()) << status.message;
        EXPECT_EQ(allocations + 1, runtime->pool().allocationCount());
    }
#endif

    HydraRender::releaseAll();
    EXPECT_EQ(0u, HydraRender::getEngineCount());
}

#ifdef NATRON_HAVE_GPU
// Free GPU Memory has a Natron Utility engine, which renders on a thread of its own, give back its
// tracer and its film, and empties the pool of them once it did; the engine itself stays.
TEST_F(USDRenderNodeTest, FreeGpuMemoryReleasesNatronUtilityGpuMemory)
{
    OwnGpuContext gpu;

    GPUTEST_DEVICE_OR_SKIP(gpu.unavailable.empty(), gpu.unavailable);

    const std::string missing = WavefrontGpu::missingSupport(*gpu.runtime());

    if (!missing.empty()) {
        GPUTEST_SKIP() << "Natron Utility cannot render on this GPU: " << missing;
    }

    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(kFixture);
    ASSERT_TRUE(stage);
    const int owner = 0;
    HydraRender::Request request = referenceRequest(stage, 1., &owner);
    request.renderer = kRenderDelegateUtilityId;
    request.settings[kUtilitySettingDevice] = PXR_NS::VtValue(PXR_NS::TfToken(kUtilityDeviceGpu));
    HydraRender::Result result;
    std::string error;

    HydraRender::releaseAll();
    gpu.runtime()->pool().trim();
    ASSERT_TRUE(HydraRender::render(request, &result, &error)) << error;
    ASSERT_EQ(0u, result.device.rfind("GPU", 0)) << result.device;
    const std::size_t engines = HydraRender::getCpuEngineCount();
    EXPECT_GT(gpu.runtime()->pool().stats().liveBytes, 0u);

    appPTR->freeGpuMemory();
    // Waits for the engine's thread to run what Free GPU Memory queued on it.
    EXPECT_EQ(engines, HydraRender::getCpuEngineCount());

    const gpu::GpuBufferPoolStats freed = gpu.runtime()->pool().stats();

    EXPECT_EQ(0u, freed.liveBytes);
    EXPECT_EQ(0u, freed.pooledBytes);

    const std::uint64_t builds = HydraRender::getEngineBuildCount();

    error.clear();
    request.time = PXR_NS::UsdTimeCode(2.);
    ASSERT_TRUE(HydraRender::render(request, &result, &error)) << error;
    EXPECT_EQ(0u, result.device.rfind("GPU", 0)) << result.device;
    EXPECT_EQ(builds, HydraRender::getEngineBuildCount());

    const Coverage box = coverageOf("/World/Box", kWidth, kHeight, result.primId, result.depth, result.rgba);

    EXPECT_GT(box.count, 100);
    HydraRender::release(&owner);
    // Waits for the engine, which holds the context's runtime, to go before the context does.
    EXPECT_EQ(engines - 1, HydraRender::getCpuEngineCount());
}

namespace {
typedef HydraRender::ProgressiveRender ProgressiveRender;

const std::chrono::seconds kProgressiveTimeout(60);

// Renders the reference scene through the 3D viewer's progressive engine with Natron Utility forced onto the GPU,
// its colour left on the device as the viewer asks for it.
void
startUtilityOnGpu(ProgressiveRender& render,
                  const PXR_NS::UsdStageRefPtr& stage,
                  const void* owner)
{
    ProgressiveRender::View view;

    view.frustum = referenceRequest(stage, 1., owner).frustum;
    view.width = kWidth;
    view.height = kHeight;
    view.time = PXR_NS::UsdTimeCode(1.);
    RenderDelegates::list();
    render.setDeviceColor(true);
    render.setSettings({ { kUtilitySettingDevice, PXR_NS::VtValue(PXR_NS::TfToken(kUtilityDeviceGpu)) } });
    render.setRenderer(kRenderDelegateUtilityId);
    render.setThreads([]() { return 0; });
    render.setView(view);
}

// The converged frame of a restart at least as late as minRestart, null with *error set when the render fails
// or does not converge in time.
std::shared_ptr<const ProgressiveRender::Frame>
convergedFrame(const ProgressiveRender& render,
               std::uint64_t minRestart,
               std::string* error)
{
    const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now() + kProgressiveTimeout;

    while (std::chrono::steady_clock::now() < end) {
        const ProgressiveRender::Status status = render.getStatus();
        const std::shared_ptr<const ProgressiveRender::Frame> frame = render.getFrame();

        if (!status.error.empty()) {
            *error = status.error;

            return std::shared_ptr<const ProgressiveRender::Frame>();
        }
        if (status.converged && frame && frame->converged && (frame->restart == status.restarts) && (status.restarts >= minRestart)) {
            return frame;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    *error = "the render did not converge in " + std::to_string(kProgressiveTimeout.count()) + " s";

    return std::shared_ptr<const ProgressiveRender::Frame>();
}
} // anon namespace

// The 3D viewer's Natron Utility engine renders nothing more once converged, so Free GPU Memory has its own
// worker give its GPU memory back; the frame shown is published again from host memory.
TEST_F(USDRenderNodeTest, FreeGpuMemoryReleasesAProgressiveNatronUtilityRendersGpuMemory)
{
    OwnGpuContext gpu;

    GPUTEST_DEVICE_OR_SKIP(gpu.unavailable.empty(), gpu.unavailable);

    const std::string missing = WavefrontGpu::missingSupport(*gpu.runtime());

    if (!missing.empty()) {
        GPUTEST_SKIP() << "Natron Utility cannot render on this GPU: " << missing;
    }

    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(kFixture);
    ASSERT_TRUE(stage);
    const int owner = 0;
    std::mutex stageLock;
    ProgressiveRender render(&stageLock, [stage]() { return stage; }, std::function<void()>(), 0.05f);
    std::string error;

    HydraRender::releaseAll();
    gpu.runtime()->pool().trim();
    startUtilityOnGpu(render, stage, &owner);

    const std::shared_ptr<const ProgressiveRender::Frame> first = convergedFrame(render, 1, &error);

    ASSERT_TRUE(bool(first)) << error;
    ASSERT_TRUE(bool(first->deviceColor)) << "the frame is not on the GPU";

    std::vector<unsigned char> before;

    ASSERT_TRUE(render.hostColor(*first, &before, &error)) << error;
    EXPECT_GT(gpu.runtime()->pool().stats().liveBytes, 0u);

    appPTR->freeGpuMemory();

    const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now() + kProgressiveTimeout;
    gpu::GpuBufferPoolStats freed = gpu.runtime()->pool().stats();
    std::shared_ptr<const ProgressiveRender::Frame> hosted = render.getFrame();

    while (((freed.liveBytes != 0) || (freed.pooledBytes != 0) || !hosted || hosted->deviceColor) && (std::chrono::steady_clock::now() < end)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        freed = gpu.runtime()->pool().stats();
        hosted = render.getFrame();
    }
    EXPECT_EQ(0u, freed.liveBytes) << "the frame held here keeps no film";
    EXPECT_EQ(0u, freed.pooledBytes);
    ASSERT_TRUE(bool(hosted));
    EXPECT_FALSE(bool(hosted->deviceColor));
    EXPECT_EQ(first->restart, hosted->restart);
    EXPECT_TRUE(hosted->rgba == before) << "the frame published again is not the one shown";

    render.invalidate();

    const std::shared_ptr<const ProgressiveRender::Frame> again = convergedFrame(render, first->restart + 1, &error);

    ASSERT_TRUE(bool(again)) << error;
    EXPECT_TRUE(bool(again->deviceColor)) << "the render after Free GPU Memory is not on the GPU";
}

// A converged progressive Natron Utility render issues no further frame that would notice a lost device, so the
// loss releaser has its worker drop the runtime it holds, and the runtime recovers.
TEST_F(USDRenderNodeTest, AConvergedProgressiveNatronUtilityRenderLetsALostGpuRecover)
{
    OwnGpuContext gpu;

    GPUTEST_DEVICE_OR_SKIP(gpu.unavailable.empty(), gpu.unavailable);

    const std::string missing = WavefrontGpu::missingSupport(*gpu.runtime());

    if (!missing.empty()) {
        GPUTEST_SKIP() << "Natron Utility cannot render on this GPU: " << missing;
    }

    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(kFixture);
    ASSERT_TRUE(stage);
    const int owner = 0;
    std::mutex stageLock;
    ProgressiveRender render(&stageLock, [stage]() { return stage; }, std::function<void()>(), 0.05f);
    std::string error;

    HydraRender::releaseAll();
    startUtilityOnGpu(render, stage, &owner);

    const std::shared_ptr<const ProgressiveRender::Frame> first = convergedFrame(render, 1, &error);

    ASSERT_TRUE(bool(first)) << error;
    ASSERT_TRUE(bool(first->deviceColor)) << "the frame is not on the GPU";

    GpuContext* context = appPTR->getGpuContext();
    gpu::GpuRuntime* runtime = gpu.runtime();
    const std::uint64_t generation = context->generation();

    runtime->device().injectFault(gpu::GpuFaultPlan::Kind::DeviceLost, 1);

    gpu::GpuBufferHandle handle;

    EXPECT_EQ(VK_ERROR_DEVICE_LOST, runtime->acquire(1 << 20, handle).result);
    ASSERT_TRUE(runtime->isLost());

    const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now() + kProgressiveTimeout;
    bool recovered = context->recoverIfLost();

    while (!recovered && (std::chrono::steady_clock::now() < end)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        recovered = context->recoverIfLost();
    }
    ASSERT_TRUE(recovered) << "the progressive render kept the lost runtime";
    EXPECT_NE(generation, context->generation());

    render.invalidate();

    const std::shared_ptr<const ProgressiveRender::Frame> again = convergedFrame(render, first->restart + 1, &error);

    ASSERT_TRUE(bool(again)) << error;
    EXPECT_TRUE(bool(again->deviceColor)) << "the render after the recovery is not on the GPU";
}
#endif
