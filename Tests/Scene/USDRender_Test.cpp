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

#include <QCoreApplication>
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
#include "Engine/Nodes/Scene/AovLayers.h"
#include "Engine/Nodes/Scene/CyclesAovs.h"
#include "Engine/Nodes/Scene/HeadlessGLContext.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/MoonRayAovs.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/RenderSettingsKnobs.h"
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

// A box at the origin seen by /World/Cam from 10 units away, with no render settings prim.
const char* const kAovWorld = R"(#usda 1.0

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
)";

// The vars of the prim the USDRenderSettings node authors: depth, normal and a raw AOV no delegate
// has. A primvar no prim has would not do: Cycles and Storm render any primvar AOV, as zeros.
const char* const kDepthNormalNopeVars = R"(
def Scope "Render"
{
    def RenderSettings "rendersettings"
    {
        rel products = </Render/product>
    }

    def RenderProduct "product"
    {
        rel orderedVars = [</Render/product/depth>, </Render/product/normal>, </Render/product/nope>]

        def RenderVar "depth"
        {
            uniform string sourceName = "depth"
        }

        def RenderVar "normal"
        {
            uniform string sourceName = "normal"
        }

        def RenderVar "nope"
        {
            uniform string sourceName = "nope"
        }
    }
}
)";

const char* const kUnrelatedRenderer = "NatronTestAovRenderer";

// A Cycles pass and a primvar, for every delegate.
const char* const kDiffuseAndStVars = R"(
def Scope "Render"
{
    def RenderSettings "rendersettings"
    {
        rel products = </Render/product>
    }

    def RenderProduct "product"
    {
        rel orderedVars = [</Render/product/diffuse>, </Render/product/st>]

        def RenderVar "diffuse"
        {
            uniform string sourceName = "cycles:diffuse_direct"
            uniform token dataType = "color3f"
        }

        def RenderVar "st"
        {
            uniform string sourceName = "st"
            uniform token sourceType = "primvar"
            uniform token dataType = "texCoord2f"
        }
    }
}
)";

class USDRenderAovTest
    : public USDRenderRendererTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        HydraRender::releaseAllCpuEngines();
        RenderSettingsKnobs::setDescriptorsForTests(kUnrelatedRenderer, RenderSettingsKnobs::DescriptorList());
        USDRenderRendererTest::TearDown();
    }

    // Writes text to name in tmp and returns its path, empty after a gtest failure.
    static std::string writeScene(const QTemporaryDir& tmp,
                                  const char* name,
                                  const std::string& text)
    {
        const QString filePath = tmp.path() + QLatin1Char('/') + QString::fromUtf8(name);
        QFile file(filePath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || (file.write(text.c_str(), (qint64)text.size()) != (qint64)text.size())) {
            ADD_FAILURE() << "cannot write " << filePath.toStdString();

            return std::string();
        }

        return filePath.toStdString();
    }

    // The layers node gives at frame 1, after the main thread has followed its last render.
    static std::list<ImageLayerDesc> presentLayers(const NodePtr& node)
    {
        QCoreApplication::processEvents();
        std::list<ImageLayerDesc> layers;
        node->getEffectInstance()->getPresentLayers(1., ViewIdx(0), -1, &layers);

        return layers;
    }

    static const ImageLayerDesc* findLayer(const std::list<ImageLayerDesc>& layers,
                                           const std::string& id)
    {
        for (const ImageLayerDesc& layer : layers) {
            if (layer.getLayerID() == id) {
                return &layer;
            }
        }

        return nullptr;
    }

    static std::string persistentMessage(const NodePtr& node,
                                         int* type)
    {
        *type = -1;
        if (!node->hasPersistentMessage()) {
            return std::string();
        }
        QString message;
        node->getPersistentMessage(&message, type, false);

        return message.toStdString();
    }

    // Changes the first setting of rendererId the settings node has a bool, int or double knob for.
    static bool nudgeASetting(const NodePtr& settingsNode,
                              const std::string& rendererId)
    {
        USDRenderSettings* settings = dynamic_cast<USDRenderSettings*>(settingsNode->getEffectInstance().get());
        const RenderSettingsKnobs* knobs = settings ? settings->renderSettings() : nullptr;
        if (!knobs) {
            return false;
        }
        for (const std::pair<std::string, std::string>& entry : knobs->audit(rendererId)) {
            const KnobIPtr k = entry.second.empty() ? KnobIPtr() : knobs->knob(rendererId, entry.first);
            if (KnobBool* b = dynamic_cast<KnobBool*>(k.get())) {
                b->setValue(!b->getValue());

                return true;
            } else if (KnobInt* i = dynamic_cast<KnobInt*>(k.get())) {
                i->setValue(i->getValue() + 1);

                return true;
            } else if (KnobDouble* d = dynamic_cast<KnobDouble*>(k.get())) {
                d->setValue(d->getValue() + 0.5);

                return true;
            }
        }

        return false;
    }
};

TEST_F(USDRenderAovTest, RenderVarsRenderTheirLayersAndAWarningNamesTheVarNoneRendersFor)
{
    if (!CyclesTestUtils::cyclesOrSkip()) {
        return;
    }
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string plain = writeScene(tmp, "plain.usda", kAovWorld);
    const std::string withVars = writeScene(tmp, "vars.usda", std::string(kAovWorld) + kDepthNormalNopeVars);
    ASSERT_FALSE(plain.empty() || withVars.empty());
    const int width = kWidth;
    const int height = kHeight;
    const std::size_t pixels = (std::size_t)width * height;
    std::list<ImageLayerDesc> layers;
    layers.push_back(ImageLayerDesc::getRGBAComponents());
    layers.push_back(AovLayers::depthLayer());
    layers.push_back(AovLayers::normalLayer());

    // No vars: the layers every Cycles render has, and no warning.
    NodePtr node = createRender(plain.c_str());
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(selectRenderer(rendererKnob(node), kRenderDelegateCyclesId));
    std::vector<RenderedPlane> planes;
    std::string error;
    ASSERT_TRUE(renderNodePlanesDirect(node, 1., ViewIdx(0), 0, RectI(0, 0, width, height), layers, &planes, &error)) << error;
    int type = -1;
    EXPECT_EQ(std::string(), persistentMessage(node, &type));
    std::vector<std::string> ids;
    for (const ImageLayerDesc& layer : presentLayers(node)) {
        ids.push_back(layer.getLayerID());
    }
    std::vector<std::string> builtIn = { ImageLayerDesc::getRGBAComponents().getLayerID(), "depth", "primId", "normal" };
    std::sort(ids.begin(), ids.end());
    std::sort(builtIn.begin(), builtIn.end());
    EXPECT_EQ(builtIn, ids);

    KnobFile* file = dynamic_cast<KnobFile*>(node->getInput(0)->getInput(0)->getKnobByName(kUSDReadParamFile).get());
    ASSERT_TRUE(file);
    file->setValue(withVars);
    planes.clear();
    ASSERT_TRUE(renderNodePlanesDirect(node, 1., ViewIdx(0), 0, RectI(0, 0, width, height), layers, &planes, &error)) << error;
    ASSERT_EQ(3u, planes.size());
    ASSERT_EQ(pixels * 4, planes[0].pixels.size());
    ASSERT_EQ(pixels, planes[1].pixels.size());
    ASSERT_EQ(pixels * 3, planes[2].pixels.size());
    const std::size_t centre = (std::size_t)(height / 2) * width + (std::size_t)(width / 2);
    EXPECT_NEAR(1.f, planes[0].pixels[centre * 4 + 3], 1e-3f);
    EXPECT_NEAR(9.f, planes[1].pixels[centre], 0.05f);
    EXPECT_GT(std::fabs(planes[2].pixels[centre * 3 + 2]), 0.5f);
    const std::string message = persistentMessage(node, &type);
    EXPECT_EQ((int)eMessageTypeWarning, type) << message;
    EXPECT_NE(std::string::npos, message.find("/Render/product/nope")) << message;
    EXPECT_EQ(std::string::npos, message.find("/Render/product/depth")) << message;
    EXPECT_EQ(std::string::npos, message.find("/Render/product/normal")) << message;
    const std::list<ImageLayerDesc> present = presentLayers(node);
    EXPECT_TRUE(findLayer(present, "depth"));
    EXPECT_TRUE(findLayer(present, "normal"));
    EXPECT_FALSE(findLayer(present, "nope"));
}

TEST_F(USDRenderAovTest, TheLayersAndHashFollowTheRenderVarsOfTheRenderer)
{
    if (!CyclesTestUtils::cyclesOrSkip()) {
        return;
    }
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string plain = writeScene(tmp, "plain.usda", kAovWorld);
    const std::string withVars = writeScene(tmp, "vars.usda", std::string(kAovWorld) + kDiffuseAndStVars);
    ASSERT_FALSE(plain.empty() || withVars.empty());
    // Storm's delegate declares no settings when it cannot be made here, so the unrelated delegate is a test one.
    RenderSettingsKnobs::Descriptor flag;
    flag.key = "flag";
    flag.name = "Flag";
    flag.defaultValue = PXR_NS::VtValue(false);
    RenderSettingsKnobs::setDescriptorsForTests(kUnrelatedRenderer, RenderSettingsKnobs::DescriptorList(1, flag));
    NodePtr settingsNode;
    NodePtr node = createRender(plain.c_str(), &settingsNode);
    ASSERT_TRUE(node && settingsNode);
    KnobChoice* renderer = rendererKnob(node);
    ASSERT_TRUE(renderer);
    ASSERT_TRUE(selectRenderer(renderer, kRenderDelegateCyclesId));
    EXPECT_FALSE(findLayer(presentLayers(node), "diffuse_direct"));
    const U64 plainHash = node->getHashValue();

    KnobFile* file = dynamic_cast<KnobFile*>(settingsNode->getInput(0)->getKnobByName(kUSDReadParamFile).get());
    ASSERT_TRUE(file);
    file->setValue(withVars);
    const U64 varsHash = node->getHashValue();
    EXPECT_NE(plainHash, varsHash);
    std::list<ImageLayerDesc> present = presentLayers(node);
    const ImageLayerDesc* diffuse = findLayer(present, "diffuse_direct");
    const ImageLayerDesc* st = findLayer(present, "st");
    ASSERT_TRUE(diffuse && st);
    EXPECT_EQ(std::vector<std::string>({ "R", "G", "B" }), diffuse->getChannels());
    // Nothing has rendered st yet, so the authored texCoord2f sets its channels, not the renderer's format.
    EXPECT_EQ(std::vector<std::string>({ "U", "V" }), st->getChannels());
    std::list<ImageLayerDesc> available;
    node->getEffectInstance()->getAvailableLayers(1., ViewIdx(0), -1, &available);
    EXPECT_TRUE(findLayer(available, "diffuse_direct"));
    EXPECT_TRUE(findLayer(available, "st"));

    // Another delegate's settings are not Cycles': the render, its hash and its layers stay.
    ASSERT_TRUE(nudgeASetting(settingsNode, kUnrelatedRenderer));
    EXPECT_EQ(varsHash, node->getHashValue());
    present = presentLayers(node);
    EXPECT_TRUE(findLayer(present, "diffuse_direct"));
    EXPECT_TRUE(findLayer(present, "st"));
    ASSERT_TRUE(nudgeASetting(settingsNode, kRenderDelegateCyclesId));
    EXPECT_NE(varsHash, node->getHashValue());

    if (!PXR_NS::PlugRegistry::GetInstance().GetPluginWithName("hd_moonray")) {
        std::printf("[  SKIPPED ] OpenUSD has no hd_moonray plug-in registered; the switch to MoonRay is not checked\n");

        return;
    }
    ASSERT_TRUE(selectRenderer(renderer, kMoonRayRendererId));
    present = presentLayers(node);
    EXPECT_FALSE(findLayer(present, "diffuse_direct"));
    EXPECT_TRUE(findLayer(present, "st"));
}

// A file's own product renders with no USDRenderSettings node: its camera and resolution beat the
// settings prim's, and each var is a layer under its authored name.
TEST_F(USDRenderAovTest, AFilesRenderVarsRenderWithoutASettingsNode)
{
    if (!CyclesTestUtils::cyclesOrSkip()) {
        return;
    }
    NodePtr reader = createReader(NATRON_TESTS_FIXTURES_DIR "/scene-rendervars.usda");
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDER));
    ASSERT_TRUE(reader && node);
    connectNodes(reader, node, 0, true);
    USDRender* effect = renderOf(node);
    ASSERT_TRUE(effect);
    effect->refreshSettingsPrimMenu();
    ASSERT_TRUE(selectRenderer(rendererKnob(node), kRenderDelegateCyclesId));
    const RectD rod = rodOf(node);
    EXPECT_EQ(64., rod.x2);
    EXPECT_EQ(48., rod.y2);

    std::list<ImageLayerDesc> present = presentLayers(node);
    const ImageLayerDesc* depth = findLayer(present, "depth");
    const ImageLayerDesc* normal = findLayer(present, "normal");
    const ImageLayerDesc* instance = findLayer(present, "instance");
    ASSERT_TRUE(depth && normal && instance);
    EXPECT_FALSE(findLayer(present, "instanceId"));
    EXPECT_EQ(1u, instance->getChannels().size());
    std::list<ImageLayerDesc> layers;
    layers.push_back(ImageLayerDesc::getRGBAComponents());
    layers.push_back(*depth);
    layers.push_back(*normal);
    layers.push_back(*instance);
    std::vector<RenderedPlane> planes;
    std::string error;
    ASSERT_TRUE(renderNodePlanesDirect(node, 1., ViewIdx(0), 0, RectI(0, 0, 64, 48), layers, &planes, &error)) << error;
    ASSERT_EQ(4u, planes.size());
    const std::size_t pixels = (std::size_t)64 * 48;
    ASSERT_EQ(pixels * 4, planes[0].pixels.size());
    ASSERT_EQ(pixels, planes[1].pixels.size());
    ASSERT_EQ(pixels * 3, planes[2].pixels.size());
    ASSERT_EQ(pixels, planes[3].pixels.size());
    // The settings prim's camera looks away from the box; the product's looks at it.
    const std::size_t centre = (std::size_t)24 * 64 + 32;
    EXPECT_NEAR(1.f, planes[0].pixels[centre * 4 + 3], 1e-3f);
    EXPECT_NEAR(9.f, planes[1].pixels[centre], 0.05f);
    present = presentLayers(node);
    EXPECT_TRUE(findLayer(present, "depth"));
    EXPECT_TRUE(findLayer(present, "normal"));
    EXPECT_TRUE(findLayer(present, "instance"));
    int type = -1;
    EXPECT_EQ(std::string(), persistentMessage(node, &type));
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
