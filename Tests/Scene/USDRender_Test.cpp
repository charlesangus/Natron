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
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/GpuContext.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/Scene/HeadlessGLContext.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Nodes/Scene/USDRender.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"
#include "RenderBothWays.h"
#include "Tests/Native/NativeGpuParity.h"
#include "ofxNatron.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuBufferPool.h"
#include "Engine/Gpu/GpuRuntime.h"
#endif

NATRON_NAMESPACE_USING

namespace {
const char* const kFixture = NATRON_TESTS_FIXTURES_DIR "/scene-reference.usda";
const char* const kInstancerFixture = NATRON_TESTS_FIXTURES_DIR "/scene-instancer-prototypes.usda";
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

    ASSERT_TRUE(HydraRender::render(referenceRequest(stage, 1., &owner), &result, &error)) << error;
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

    // USDRead of the fixture into a USDRender at kWidth x kHeight.
    NodePtr createRender(const char* fixture = kFixture)
    {
        NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
        NodePtr render = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDER));
        if (!reader || !render) {
            ADD_FAILURE() << "USDRead or USDRender is not registered";

            return NodePtr();
        }
        KnobFile* file = dynamic_cast<KnobFile*>(reader->getKnobByName(kUSDReadParamFile).get());
        KnobInt* size = dynamic_cast<KnobInt*>(render->getKnobByName(kNatronParamFormatSize).get());
        KnobDouble* par = dynamic_cast<KnobDouble*>(render->getKnobByName(kNatronParamFormatPar).get());
        if (!file || !size || !par) {
            ADD_FAILURE() << "missing knobs";

            return NodePtr();
        }
        file->setValue(std::string(fixture));
        size->setValue(kWidth, ViewSpec::all(), 0);
        size->setValue(kHeight, ViewSpec::all(), 1);
        par->setValue(1.);
        connectNodes(reader, render, 0, true);

        return render;
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

TEST_F(USDRenderNodeTest, TheCameraMenuListsTheScenesCameras)
{
    NodePtr node = createRender();
    ASSERT_TRUE(node != nullptr);
    USDRender* effect = dynamic_cast<USDRender*>(node->getEffectInstance().get());
    ASSERT_TRUE(effect);
    effect->refreshCameraMenu();
    KnobChoice* camera = dynamic_cast<KnobChoice*>(node->getKnobByName(kUSDRenderParamCamera).get());
    ASSERT_TRUE(camera);
    const std::vector<ChoiceOption> entries = camera->getEntries_mt_safe();
    ASSERT_EQ(1u, entries.size());
    EXPECT_EQ("/World/Cam", entries[0].id);
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

TEST_F(USDRenderNodeTest, AChosenCameraTheSceneLacksIsANodeErrorNamingIt)
{
    if (skipWithoutGL()) {
        return;
    }
    NodePtr node = createRender();
    ASSERT_TRUE(node != nullptr);
    KnobChoice* camera = dynamic_cast<KnobChoice*>(node->getKnobByName(kUSDRenderParamCamera).get());
    ASSERT_TRUE(camera);
    camera->setActiveEntry(ChoiceOption("/World/Gone", "/World/Gone", ""));
    std::vector<RenderedPlane> planes;
    std::string error;

    EXPECT_FALSE(renderNodePlanesDirect(node, 1., ViewIdx(0), 0, RectI(0, 0, kWidth, kHeight), allPlanes(), &planes, &error));
    ASSERT_TRUE(node->hasPersistentMessage());
    QString message;
    int type = 0;
    node->getPersistentMessage(&message, &type, false);
    EXPECT_TRUE(message.contains(QString::fromUtf8("/World/Gone"))) << message.toStdString();
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

TEST_F(USDRenderRendererTest, TheRendererMenuListsTheRegistryWithStormChosen)
{
    NodePtr node = createRender();
    ASSERT_TRUE(bool(node));
    KnobChoice* renderer = rendererKnob(node);
    ASSERT_TRUE(renderer);
    EXPECT_EQ(std::string(kRenderDelegateStormId), renderer->getActiveEntry().id);
    const std::vector<RenderDelegates::Delegate> delegates = RenderDelegates::list();
    const std::vector<ChoiceOption> entries = renderer->getEntries_mt_safe();
    ASSERT_EQ(delegates.size(), entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        EXPECT_EQ(delegates[i].id, entries[i].id);
    }
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
    ASSERT_TRUE(selectRenderer(renderer, kRenderDelegateStormId));
    EXPECT_EQ(stormHash, node->getHashValue());
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
