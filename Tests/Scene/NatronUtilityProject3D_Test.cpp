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

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include <QTemporaryDir>

#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/ray.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3i.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/vt/types.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hio/image.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdr/registry.h>
#include <pxr/usd/sdr/shaderNode.h>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/LiveTextureStore.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDCamera.h"
#include "Engine/Nodes/Scene/USDCard.h"
#include "Engine/Nodes/Scene/USDProject3D.h"
#include "Engine/Nodes/Scene/Utility/UtilityKernels.h"
#include "Engine/Nodes/Scene/Utility/UtilityMaterial.h"
#include "Engine/Nodes/Scene/Utility/UtilityScene.h"
#include "Engine/Nodes/Scene/Utility/Wavefront.h"
#include "Engine/Project.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_USING

namespace {
const int kPlateSize = 4;

// The plate's texel in column x of row y, counted from the top of the file.
GfVec4f
plateTexel(int x,
           int rowFromTop)
{
    return GfVec4f(60.f * x + 15.f, 60.f * rowFromTop + 15.f, 255.f - 30.f * (x + rowFromTop), 255.f) / 255.f;
}

bool
writePlate(const std::string& path)
{
    std::vector<unsigned char> bytes;

    for (int y = 0; y < kPlateSize; ++y) {
        for (int x = 0; x < kPlateSize; ++x) {
            const GfVec4f texel = plateTexel(x, y) * 255.f;

            for (int c = 0; c < 4; ++c) {
                bytes.push_back((unsigned char)std::lround(texel[c]));
            }
        }
    }

    HioImage::StorageSpec spec;

    spec.width = kPlateSize;
    spec.height = kPlateSize;
    spec.depth = 1;
    spec.format = HioFormatUNorm8Vec4;
    spec.flipped = false;
    spec.data = bytes.data();

    const HioImageSharedPtr image = HioImage::OpenForWriting(path);

    return image && image->Write(spec);
}

// The render camera, looking down -Z at a card on z = 0.
GfFrustum
renderCamera()
{
    GfFrustum camera;

    camera.SetPerspective(40., 1., 0.5, 100.);
    camera.SetPosition(GfVec3d(0.3, -0.2, 6.));

    return camera;
}

// The camera's frame as USDProject3D's worldToProjector puts it: (0, 0) at the bottom left, (1, 1) at
// the top right, through the camera's view and projection.
GfMatrix4d
worldToFrame(const GfFrustum& camera)
{
    GfMatrix4d toUnit(1.);

    toUnit.SetRow(0, GfVec4d(0.5, 0., 0., 0.));
    toUnit.SetRow(1, GfVec4d(0., 0.5, 0., 0.));
    toUnit.SetRow(3, GfVec4d(0.5, 0.5, 0., 1.));

    return camera.ComputeViewMatrix() * camera.ComputeProjectionMatrix() * toUnit;
}

// A card covering the camera's view at z = 0, with natronProject3D.glslfx's surface bound.
struct ProjectedCard {
    explicit ProjectedCard(const UtilityProjection& projection,
                           const std::string& plate)
    {
        UtilityTextureRef texture;

        texture.asset = plate;
        texture.colorSpace = UtilityTextureRef::eColorSpaceRaw;

        UtilityMaterialDesc material;

        material.kind = UtilityMaterialDesc::eKindProject3D;
        material.projection = projection;
        material.projection.plate = 0;
        material.textureRefs.push_back(texture);

        id = scene.addGeometry(SdfPath("/Card"), 7);
        scene.setTopology(id,
                          VtVec3fArray { GfVec3f(-20.f, -20.f, 0.f), GfVec3f(20.f, -20.f, 0.f), GfVec3f(20.f, 20.f, 0.f), GfVec3f(-20.f, 20.f, 0.f) },
                          VtVec3iArray { GfVec3i(0, 1, 2), GfVec3i(0, 2, 3) });
        scene.setInstances(id, { GfMatrix4d(1.) });
        scene.setMaterial(SdfPath("/Projection"), std::make_shared<const UtilityMaterialDesc>(material));
        scene.setMaterialId(id, SdfPath("/Projection"));
        tables.update(scene.snapshot());
    }

    struct Shaded {
        GfVec4f color;
        std::int32_t primId = 0;
    };

    // Shades where the ray through the camera's frame at (x, y), each 0 to 1, meets the card.
    Shaded shade(const GfFrustum& camera,
                 double x,
                 double y)
    {
        const GfRay ray = camera.ComputeRay(GfVec2d(2. * x - 1., 2. * y - 1.));
        const GfVec3d direction = ray.GetDirection().GetNormalized();
        const GfVec3d origin = ray.GetStartPoint();
        const double t = -origin[2] / direction[2];
        const std::vector<float> rays = { (float)origin[0], (float)origin[1], (float)origin[2],
                                          (float)direction[0], (float)direction[1], (float)direction[2],
                                          0.f, (float)(2. * t) };
        const std::vector<float> hitSurface = { (float)t, 0.25f, 0.25f };
        const std::vector<std::uint32_t> hitIds = { 0u, 0u, 0u };
        float color[4] = { -1.f, -1.f, -1.f, -1.f };
        float depth = -1.f;
        Shaded shaded;
        std::int32_t instanceId = -1;
        KernelShadeBuffers buffers = tables.buffers();

        buffers.rays = rays.data();
        buffers.hitSurface = hitSurface.data();
        buffers.hitIds = hitIds.data();
        buffers.colorOut = color;
        buffers.depthOut = &depth;
        buffers.primIdOut = &shaded.primId;
        buffers.instanceIdOut = &instanceId;

        WavefrontFrame frame;

        frame.worldToView = camera.ComputeViewMatrix();
        frame.projection = camera.ComputeProjectionMatrix();
        // A pixel far smaller than a plate texel, so the lookups read the full-size level.
        frame.window.width = 1024;
        frame.window.height = 1024;
        runShadeSurfaceKernel(makeShadeParams(frame, 1), buffers);
        shaded.color = GfVec4f(color[0], color[1], color[2], color[3]);

        return shaded;
    }

    UtilityScene scene;
    UtilityScene::GeometryId id = 0;
    WavefrontShadeTables tables;
};

void
expectNear(const GfVec4f& expected,
           const GfVec4f& actual,
           float tolerance,
           const std::string& where)
{
    for (int c = 0; c < 4; ++c) {
        EXPECT_NEAR(expected[c], actual[c], tolerance) << where << " channel " << c;
    }
}

template <typename K>
K*
getKnob(const NodePtr& node,
        const char* name)
{
    return dynamic_cast<K*>(node->getKnobByName(name).get());
}

SdfPath
getNodePath(const NodePtr& node)
{
    return SdfPath("/" + node->getScriptName_mt_safe());
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
    std::cout << "[  SKIPPED ] NatronUtilityProject3D: " << error << std::endl;

    return true;
}
} // anon namespace

TEST(NatronUtilityProject3D, APlateProjectedFromTheRenderCameraReproducesThePlate)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("plate.png")).toStdString();
    ASSERT_TRUE(writePlate(path));

    const GfFrustum camera = renderCamera();
    UtilityProjection projection;

    projection.worldToProjector = worldToFrame(camera);
    projection.projectorOrigin = GfVec4f(0.3f, -0.2f, 6.f, 1.f);

    ProjectedCard card(projection, path);

    for (int y = 0; y < kPlateSize; ++y) {
        for (int x = 0; x < kPlateSize; ++x) {
            const ProjectedCard::Shaded shaded = card.shade(camera, (x + 0.5) / kPlateSize, (y + 0.5) / kPlateSize);

            expectNear(plateTexel(x, kPlateSize - 1 - y), shaded.color, 1e-4f, "pixel " + std::to_string(x) + ", " + std::to_string(y));
            EXPECT_EQ(7, shaded.primId);
        }
    }
    EXPECT_TRUE(card.scene.warnings().empty());
}

TEST(NatronUtilityProject3D, CropShowsNothingOutsideTheFrame)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("plate.png")).toStdString();
    ASSERT_TRUE(writePlate(path));

    const GfFrustum camera = renderCamera();
    GfFrustum wide = camera;

    wide.SetPerspective(80., 1., 0.5, 100.);

    UtilityProjection projection;

    projection.worldToProjector = worldToFrame(camera);
    projection.projectorOrigin = GfVec4f(0.3f, -0.2f, 6.f, 1.f);

    ProjectedCard card(projection, path);
    const ProjectedCard::Shaded outside = card.shade(wide, 0.02, 0.5);

    EXPECT_EQ(GfVec4f(0.f), outside.color);
    EXPECT_EQ(-1, outside.primId);
    EXPECT_EQ(7, card.shade(wide, 0.5, 0.5).primId);
}

// The card faces the projector, so projecting on back faces only leaves it to the outside mode.
TEST(NatronUtilityProject3D, ProjectingOnBackFacesLeavesAFacingCardToTheOutsideMode)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("plate.png")).toStdString();
    ASSERT_TRUE(writePlate(path));

    const GfFrustum camera = renderCamera();
    UtilityProjection projection;

    projection.worldToProjector = worldToFrame(camera);
    projection.projectorOrigin = GfVec4f(0.3f, -0.2f, 6.f, 1.f);
    projection.projectOn = 2;

    {
        ProjectedCard card(projection, path);
        const ProjectedCard::Shaded shaded = card.shade(camera, 0.5, 0.5);

        EXPECT_EQ(GfVec4f(0.f, 0.f, 0.f, 1.f), shaded.color);
        EXPECT_EQ(7, shaded.primId);
    }

    projection.outside = 1;

    ProjectedCard transparent(projection, path);

    EXPECT_EQ(-1, transparent.shade(camera, 0.5, 0.5).primId);

    projection.projectOn = 1;

    ProjectedCard front(projection, path);

    expectNear(plateTexel(1, 2), front.shade(camera, 0.375, 0.375).color, 1e-4f, "front");
}

class NatronUtilityProject3DTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        LiveTextureStore::instance().clear();
        SceneQueryPool::instance().releaseIdle();
        BaseTest::TearDown();
    }

    // A camera on +Z whose frame is 2 units wide at the origin, projecting a Constant onto a card.
    void createRig()
    {
        ProjectPtr project = getApp()->getProject();
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, 2000, 1000, "wide2to1", 1.));
        plate = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
        ASSERT_TRUE(bool(plate));
        plate->setScriptName("Plate1");
        KnobColor* color = getKnob<KnobColor>(plate, kConstantParamColor);
        ASSERT_TRUE(color);
        color->setValue(0.2, ViewSpec::all(), 0);
        color->setValue(0.4, ViewSpec::all(), 1);
        color->setValue(0.6, ViewSpec::all(), 2);
        color->setValue(1., ViewSpec::all(), 3);
        camera = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
        ASSERT_TRUE(bool(camera));
        getKnob<KnobDouble>(camera, kUSDCameraParamFocalLength)->setValue(50.);
        getKnob<KnobDouble>(camera, kUSDCameraParamHorizontalAperture)->setValue(36.);
        getKnob<KnobDouble>(camera, kUSDCameraParamVerticalAperture)->setValue(18.);
        getKnob<KnobDouble>(camera, kSceneParamTranslate)->setValue(50. / 18., ViewSpec::all(), 2);
        projector = createNode(QString::fromUtf8(PLUGINID_NATRON_USDPROJECT3D));
        ASSERT_TRUE(bool(projector));
        projector->setScriptName("Project1");
        ASSERT_TRUE(projector->connectInput(plate, kUSDProject3DInputImage));
        ASSERT_TRUE(projector->connectInput(camera, kUSDProject3DInputCamera));
        card = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
        ASSERT_TRUE(bool(card));
        ASSERT_TRUE(card->connectInput(projector, 0));
    }

    static bool evaluate(const NodePtr& node,
                         ScenePayload* payload)
    {
        std::string error;
        const bool ok = node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), payload, &error) == EffectInstance::eRenderRoIRetCodeOk;

        EXPECT_TRUE(ok) << error;

        return ok;
    }

    NodePtr plate;
    NodePtr camera;
    NodePtr projector;
    NodePtr card;
};

TEST_F(NatronUtilityProject3DTest, TheProjectionShaderIsRecognisedAndItsInputsRead)
{
    const SdrShaderNodeConstPtr shader = SdrRegistry::GetInstance().GetShaderNodeFromAsset(SdfAssetPath(USDProject3D::getShaderPath()));
    ASSERT_TRUE(shader) << USDProject3D::getShaderPath();
    EXPECT_TRUE(utilityIsProject3DShader(shader->GetIdentifier()));
    EXPECT_FALSE(utilityIsProject3DShader(TfToken("UsdPreviewSurface")));

    GfMatrix4d worldToProjector(1.);
    worldToProjector.SetTranslate(GfVec3d(1., 2., 3.));

    HdMaterialNetwork2 network;
    const SdfPath surface("/Projection/Surface");
    const SdfPath image("/Projection/Plate");

    network.nodes[surface].nodeTypeId = shader->GetIdentifier();
    network.nodes[surface].parameters[TfToken(kUSDProject3DInputWorldToProjector)] = VtValue(worldToProjector);
    network.nodes[surface].parameters[TfToken(kUSDProject3DInputProjectionSpace)] = VtValue(1);
    network.nodes[surface].parameters[TfToken("projectOn")] = VtValue(2);
    network.nodes[surface].parameters[TfToken("crop")] = VtValue(0);
    network.nodes[surface].parameters[TfToken("clip")] = VtValue(1);
    network.nodes[surface].parameters[TfToken("outside")] = VtValue(1);
    network.nodes[surface].parameters[TfToken(kUSDProject3DInputPlateTransform)] = VtValue(GfVec4f(0.5f, 0.25f, 0.1f, 0.2f));
    network.nodes[surface].inputConnections[TfToken("plate")].push_back(HdMaterialConnection2 { image, TfToken("rgba") });
    network.nodes[image].nodeTypeId = TfToken("UsdUVTexture");
    network.nodes[image].parameters[TfToken("file")] = VtValue(SdfAssetPath("natron://Plate1/rgba?v=1", "natron://Plate1/rgba?v=1"));
    network.terminals[HdMaterialTerminalTokens->surface] = HdMaterialConnection2 { surface, TfToken("surface") };

    const UtilityMaterialDesc desc = utilityMaterialFromNetwork(network);

    ASSERT_EQ(UtilityMaterialDesc::eKindProject3D, desc.kind) << desc.unsupportedReason;
    EXPECT_EQ(worldToProjector, desc.projection.worldToProjector);
    EXPECT_EQ(1, desc.projection.projectionSpace);
    EXPECT_EQ(2, desc.projection.projectOn);
    EXPECT_EQ(0, desc.projection.crop);
    EXPECT_EQ(1, desc.projection.clip);
    EXPECT_EQ(1, desc.projection.outside);
    EXPECT_EQ(GfVec4f(0.5f, 0.25f, 0.1f, 0.2f), desc.projection.plateTransform);
    ASSERT_EQ(0, desc.projection.plate);
    ASSERT_EQ(1u, desc.textureRefs.size());
    EXPECT_EQ("natron://Plate1/rgba?v=1", desc.textureRefs[0].asset);
}

// Both renderers draw the card through the projection, the plate's constant colour within it.
TEST_F(NatronUtilityProject3DTest, NatronUtilityMatchesStormOnInteriorPixels)
{
    if (skipWithoutGL()) {
        return;
    }
    createRig();
    ASSERT_TRUE(bool(card));

    ScenePayload payload;
    ASSERT_TRUE(evaluate(card, &payload));

    ScenePayload cameraPayload;
    ASSERT_TRUE(evaluate(camera, &cameraPayload));

    const int width = 64;
    const int height = 32;
    const int owner = 0;
    HydraRender::Request request;
    std::string error;

    request.owner = &owner;
    request.scene = payload;
    request.time = UsdTimeCode(1.);
    request.width = width;
    request.height = height;
    ASSERT_TRUE(HydraRender::cameraFrustum(SceneOps::compose(cameraPayload), getNodePath(camera), request.time, double(width) / height, HydraRender::eFitModeFill, &request.frustum, &error)) << error;

    int failed = -1;
    std::thread worker([&]() { failed = LiveTextureStore::instance().prepare(SceneOps::getLiveTextures(payload), 1.); });
    worker.join();
    ASSERT_EQ(0, failed);

    HydraRender::Result storm;
    HydraRender::Result utility;

    ASSERT_TRUE(HydraRender::render(request, &storm, &error)) << error;
    request.renderer = kRenderDelegateUtilityId;
    ASSERT_TRUE(HydraRender::render(request, &utility, &error)) << error;
    EXPECT_EQ(std::string(), utility.warning);

    const std::size_t pixels = (std::size_t)width * height;
    ASSERT_EQ(pixels * 4, storm.rgba.size());
    ASSERT_EQ(pixels * 4, utility.rgba.size());
    ASSERT_EQ(pixels, utility.primId.size());

    std::size_t compared = 0;

    for (int y = 1; y < height - 1; ++y) {
        for (int x = 1; x < width - 1; ++x) {
            const std::size_t centre = (std::size_t)y * width + x;
            bool interior = (storm.primId[centre] != 0.f) && (utility.primId[centre] != 0.f);

            for (int dy = -1; dy <= 1 && interior; ++dy) {
                for (int dx = -1; dx <= 1 && interior; ++dx) {
                    const std::size_t at = (std::size_t)(y + dy) * width + x + dx;

                    interior = (storm.primId[at] == storm.primId[centre]) && (utility.primId[at] == utility.primId[centre]) && (storm.rgba[at * 4 + 3] > 0.f) && (utility.rgba[at * 4 + 3] > 0.f);
                }
            }
            if (!interior) {
                continue;
            }
            ++compared;

            const std::size_t at = centre;

            for (int c = 0; c < 4; ++c) {
                EXPECT_NEAR(storm.rgba[at * 4 + c], utility.rgba[at * 4 + c], 2e-3) << "pixel " << x << ", " << y << " channel " << c;
            }
            EXPECT_NEAR(0.4f, utility.rgba[at * 4 + 1], 1e-3) << "pixel " << x << ", " << y;
        }
    }
    EXPECT_GT(compared, 20u);
    HydraRender::release(&owner);
}
