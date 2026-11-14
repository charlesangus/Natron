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
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QTemporaryDir>

#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3i.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/vt/types.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hio/image.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/path.h>

#include "BaseTest.h"

#include "Engine/Nodes/Scene/LiveTextureImage.h"
#include "Engine/Nodes/Scene/LiveTextureStore.h"
#include "Engine/Nodes/Scene/SceneCaches.h"
#include "Engine/Nodes/Scene/Utility/UtilityKernels.h"
#include "Engine/Nodes/Scene/Utility/UtilityMaterial.h"
#include "Engine/Nodes/Scene/Utility/UtilityScene.h"
#include "Engine/Nodes/Scene/Utility/UtilityTextures.h"
#include "Engine/Nodes/Scene/Utility/Wavefront.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_USING

namespace {
const SdfPath kSurface("/Material/Surface");
const SdfPath kImage("/Material/Image");
const SdfPath kReader("/Material/Reader");

HdMaterialNetwork2
networkOf(const TfToken& surfaceId)
{
    HdMaterialNetwork2 network;

    network.nodes[kSurface].nodeTypeId = surfaceId;
    network.terminals[HdMaterialTerminalTokens->surface] = HdMaterialConnection2 { kSurface, TfToken("surface") };

    return network;
}

void
connect(HdMaterialNetwork2& network,
        const SdfPath& node,
        const char* input,
        const SdfPath& upstream,
        const char* output)
{
    network.nodes[node].inputConnections[TfToken(input)].push_back(HdMaterialConnection2 { upstream, TfToken(output) });
}

void
set(HdMaterialNetwork2& network,
    const SdfPath& node,
    const char* name,
    const VtValue& value)
{
    network.nodes[node].parameters[TfToken(name)] = value;
}

// A UsdUVTexture of file read through the uv primvar, driving the surface's diffuseColor.
HdMaterialNetwork2
texturedDiffuse(const std::string& file)
{
    HdMaterialNetwork2 network = networkOf(TfToken("UsdPreviewSurface"));

    network.nodes[kImage].nodeTypeId = TfToken("UsdUVTexture");
    network.nodes[kReader].nodeTypeId = TfToken("UsdPrimvarReader_float2");
    set(network, kImage, "file", VtValue(SdfAssetPath(file, file)));
    set(network, kImage, "wrapS", VtValue(TfToken("repeat")));
    set(network, kImage, "wrapT", VtValue(TfToken("mirror")));
    set(network, kImage, "scale", VtValue(GfVec4f(2.f, 2.f, 2.f, 1.f)));
    set(network, kImage, "bias", VtValue(GfVec4f(0.1f, 0.f, 0.f, 0.f)));
    set(network, kImage, "fallback", VtValue(GfVec4f(0.5f, 0.5f, 0.5f, 1.f)));
    set(network, kImage, "sourceColorSpace", VtValue(TfToken("raw")));
    set(network, kReader, "varname", VtValue(std::string("uv")));
    connect(network, kImage, "st", kReader, "result");
    connect(network, kSurface, "diffuseColor", kImage, "rgb");

    return network;
}

// A 4x4 texture whose texel in column x of row y, counted from the top of the file, is
// (80 x, 80 y, 20 (x + y) + 60, 255).
const int kTextureSize = 4;

GfVec4f
writtenTexel(int x,
             int rowFromTop)
{
    return GfVec4f(80.f * x / 255.f, 80.f * rowFromTop / 255.f, (20.f * (x + rowFromTop) + 60.f) / 255.f, 1.f);
}

bool
writeTexture(const std::string& path)
{
    std::vector<unsigned char> bytes;

    for (int y = 0; y < kTextureSize; ++y) {
        for (int x = 0; x < kTextureSize; ++x) {
            const GfVec4f texel = writtenTexel(x, y) * 255.f;

            for (int c = 0; c < 4; ++c) {
                bytes.push_back((unsigned char)std::lround(texel[c]));
            }
        }
    }

    HioImage::StorageSpec spec;

    spec.width = kTextureSize;
    spec.height = kTextureSize;
    spec.depth = 1;
    spec.format = HioFormatUNorm8Vec4;
    spec.flipped = false;
    spec.data = bytes.data();

    const HioImageSharedPtr image = HioImage::OpenForWriting(path);

    return image && image->Write(spec);
}

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

// A triangle whose texture coordinates equal its barycentrics, emitting the texture bound to it.
struct EmittedTexture {
    explicit EmittedTexture(const std::string& asset,
                            const GfVec4f& fallback = GfVec4f(0.f, 0.f, 0.f, 1.f))
    {
        UtilityTextureRef texture;

        texture.asset = asset;
        texture.colorSpace = UtilityTextureRef::eColorSpaceRaw;
        texture.wrapS = UtilityTextureRef::eWrapClamp;
        texture.wrapT = UtilityTextureRef::eWrapClamp;
        texture.fallback = fallback;

        UtilityMaterialDesc material;

        material.diffuseColor = GfVec3f(0.f);
        material.textureRefs.push_back(texture);
        material.textures[UtilityMaterialDesc::eSlotEmissiveColor] = 0;

        id = scene.addGeometry(SdfPath("/Triangle"), 3);
        scene.setTopology(id,
                          VtVec3fArray { GfVec3f(0.f, 0.f, 0.f), GfVec3f(1.f, 0.f, 0.f), GfVec3f(0.f, 1.f, 0.f) },
                          VtVec3iArray { GfVec3i(0, 1, 2) });
        scene.setInstances(id, { GfMatrix4d(1.) });
        scene.setSt(id, true, UtilityScene::eColorBindingVertex, VtVec2fArray { GfVec2f(0.f, 0.f), GfVec2f(1.f, 0.f), GfVec2f(0.f, 1.f) });
        scene.setMaterial(SdfPath("/Material"), std::make_shared<const UtilityMaterialDesc>(material));
        scene.setMaterialId(id, SdfPath("/Material"));
    }

    // The colour the kernel shades at texture coordinates (s, t), seen through a window of the given
    // width and height.
    GfVec4f shade(float s,
                  float t,
                  int window = 1024)
    {
        tables.update(scene.snapshot());

        GfFrustum frustum;

        frustum.SetPerspective(45., 1., 1., 100.);

        const std::vector<float> rays = { 0.f, 0.f, 1.f, 0.f, 0.f, -1.f, 0.f, 10.f };
        const std::vector<float> hitSurface = { 1.f, s, t };
        const std::vector<std::uint32_t> hitIds = { 0u, 0u, 0u };
        float color[4] = { -1.f, -1.f, -1.f, -1.f };
        float depth = -1.f;
        std::int32_t primId = -1;
        std::int32_t instanceId = -1;
        KernelShadeBuffers buffers = tables.buffers();

        buffers.rays = rays.data();
        buffers.hitSurface = hitSurface.data();
        buffers.hitIds = hitIds.data();
        buffers.colorOut = color;
        buffers.depthOut = &depth;
        buffers.primIdOut = &primId;
        buffers.instanceIdOut = &instanceId;

        WavefrontFrame frame;

        frame.worldToView = frustum.ComputeViewMatrix();
        frame.projection = frustum.ComputeProjectionMatrix();
        // By default a pixel is far smaller than a texel, so the lookups read the full-size level.
        frame.window.width = window;
        frame.window.height = window;
        runShadeSurfaceKernel(makeShadeParams(frame, 1), buffers);

        return GfVec4f(color[0], color[1], color[2], color[3]);
    }

    UtilityScene scene;
    UtilityScene::GeometryId id = 0;
    WavefrontShadeTables tables;
};

// x / (width - 1) in red and y / (height - 1) in green, y counted from the bottom as Natron does.
LiveTextureBufferConstPtr
makeGradient(int width,
             int height)
{
    std::shared_ptr<LiveTextureBuffer> buffer = LiveTextureBuffer::create(width, height);

    if (!buffer) {
        return buffer;
    }
    for (int y = 0; y < height; ++y) {
        float* row = buffer->getRow(y);
        for (int x = 0; x < width; ++x) {
            row[4 * x + 0] = (float)x / (float)(width - 1);
            row[4 * x + 1] = (float)y / (float)(height - 1);
            row[4 * x + 2] = 0.25f;
            row[4 * x + 3] = 1.f;
        }
    }

    return buffer;
}
} // anon namespace

TEST(NatronUtilityMaterial, PreviewSurfaceConstantsLandInTheRecord)
{
    HdMaterialNetwork2 network = networkOf(TfToken("UsdPreviewSurface"));

    set(network, kSurface, "diffuseColor", VtValue(GfVec3f(0.1f, 0.2f, 0.3f)));
    set(network, kSurface, "emissiveColor", VtValue(GfVec3f(1.f, 0.5f, 0.25f)));
    set(network, kSurface, "specularColor", VtValue(GfVec3f(0.4f, 0.5f, 0.6f)));
    set(network, kSurface, "useSpecularWorkflow", VtValue(1));
    set(network, kSurface, "metallic", VtValue(0.7f));
    set(network, kSurface, "roughness", VtValue(0.3f));
    set(network, kSurface, "clearcoat", VtValue(0.2f));
    set(network, kSurface, "clearcoatRoughness", VtValue(0.05f));
    set(network, kSurface, "opacity", VtValue(0.4f));
    set(network, kSurface, "opacityThreshold", VtValue(0.25f));
    set(network, kSurface, "ior", VtValue(1.3f));

    const UtilityMaterialDesc desc = utilityMaterialFromNetwork(network);

    ASSERT_EQ(UtilityMaterialDesc::eKindPreviewSurface, desc.kind) << desc.unsupportedReason;
    EXPECT_EQ(GfVec3f(0.1f, 0.2f, 0.3f), desc.diffuseColor);
    EXPECT_FALSE(desc.diffuseFromDisplayColor);
    EXPECT_TRUE(desc.textureRefs.empty());

    UtilityScene scene;
    const UtilityScene::GeometryId id = scene.addGeometry(SdfPath("/Mesh"), 1);

    scene.setTopology(id, VtVec3fArray { GfVec3f(0.f), GfVec3f(1.f, 0.f, 0.f), GfVec3f(0.f, 1.f, 0.f) }, VtVec3iArray { GfVec3i(0, 1, 2) });
    scene.setInstances(id, { GfMatrix4d(1.) });
    scene.setMaterial(SdfPath("/Material"), std::make_shared<const UtilityMaterialDesc>(desc));
    scene.setMaterialId(id, SdfPath("/Material"));

    WavefrontShadeTables tables;

    tables.update(scene.snapshot());

    const KernelShadeBuffers buffers = tables.buffers();

    // The first record is what geometry without a material shades with.
    ASSERT_EQ(2u, buffers.materialCount);
    EXPECT_EQ(1u, buffers.geometries[0].materialIndex);

    const KernelMaterialRecord& record = buffers.materials[1];

    EXPECT_FLOAT_EQ(0.1f, record.diffuseColor[0]);
    EXPECT_FLOAT_EQ(0.3f, record.diffuseColor[2]);
    EXPECT_FLOAT_EQ(1.f, record.emissiveColor[0]);
    EXPECT_FLOAT_EQ(0.25f, record.emissiveColor[2]);
    EXPECT_FLOAT_EQ(0.6f, record.specularColor[2]);
    EXPECT_FLOAT_EQ(1.f, record.specularColor[3]);
    EXPECT_FLOAT_EQ(0.7f, record.surface[0]);
    EXPECT_FLOAT_EQ(0.3f, record.surface[1]);
    EXPECT_FLOAT_EQ(0.2f, record.surface[2]);
    EXPECT_FLOAT_EQ(0.05f, record.surface[3]);
    EXPECT_FLOAT_EQ(0.4f, record.coverage[0]);
    EXPECT_FLOAT_EQ(0.25f, record.coverage[1]);
    EXPECT_FLOAT_EQ(1.3f, record.coverage[2]);
    EXPECT_EQ((std::uint32_t)UtilityMaterialDesc::eKindPreviewSurface, record.kind[0]);
    EXPECT_EQ(0u, record.kind[2]);
    for (int slot = 0; slot < UtilityMaterialDesc::eSlotCount; ++slot) {
        EXPECT_EQ(-1, record.textures[slot]) << slot;
    }
    EXPECT_EQ(kKernelMaterialDiffuseFromDisplayColor | kKernelMaterialOpacityFromDisplayOpacity, buffers.materials[0].kind[2]);
}

TEST(NatronUtilityMaterial, ATextureDrivenDiffuseGetsASlot)
{
    const UtilityMaterialDesc desc = utilityMaterialFromNetwork(texturedDiffuse("/textures/wood.png"));

    ASSERT_EQ(UtilityMaterialDesc::eKindPreviewSurface, desc.kind) << desc.unsupportedReason;
    ASSERT_EQ(1u, desc.textureRefs.size());
    EXPECT_EQ(0, desc.textures[UtilityMaterialDesc::eSlotDiffuseColor]);
    EXPECT_EQ(-1, desc.textures[UtilityMaterialDesc::eSlotEmissiveColor]);

    const UtilityTextureRef& texture = desc.textureRefs[0];

    EXPECT_EQ("/textures/wood.png", texture.asset);
    EXPECT_EQ(UtilityTextureRef::eWrapRepeat, texture.wrapS);
    EXPECT_EQ(UtilityTextureRef::eWrapMirror, texture.wrapT);
    EXPECT_EQ(GfVec4f(2.f, 2.f, 2.f, 1.f), texture.scale);
    EXPECT_EQ(GfVec4f(0.1f, 0.f, 0.f, 0.f), texture.bias);
    EXPECT_EQ(GfVec4f(0.5f, 0.5f, 0.5f, 1.f), texture.fallback);
    EXPECT_EQ(UtilityTextureRef::eColorSpaceRaw, texture.colorSpace);
    EXPECT_EQ(0, texture.firstChannel);
    EXPECT_EQ("uv", texture.primvar);
    EXPECT_EQ("uv", desc.stPrimvar());
    EXPECT_TRUE(desc.readsTextureCoordinates());
}

TEST(NatronUtilityMaterial, ATransform2dBeforeTheTextureIsFolded)
{
    HdMaterialNetwork2 network = texturedDiffuse("/textures/wood.png");
    const SdfPath transform("/Material/Transform");

    network.nodes[kImage].inputConnections.clear();
    network.nodes[transform].nodeTypeId = TfToken("UsdTransform2d");
    set(network, transform, "scale", VtValue(GfVec2f(2.f, 3.f)));
    set(network, transform, "translation", VtValue(GfVec2f(0.5f, 0.25f)));
    set(network, transform, "rotation", VtValue(90.f));
    connect(network, transform, "in", kReader, "result");
    connect(network, kImage, "st", transform, "result");

    const UtilityMaterialDesc desc = utilityMaterialFromNetwork(network);

    ASSERT_EQ(UtilityMaterialDesc::eKindPreviewSurface, desc.kind) << desc.unsupportedReason;
    ASSERT_EQ(1u, desc.textureRefs.size());
    EXPECT_EQ(GfVec2f(2.f, 3.f), desc.textureRefs[0].uvScale);
    EXPECT_EQ(GfVec2f(0.5f, 0.25f), desc.textureRefs[0].uvTranslation);
    EXPECT_FLOAT_EQ(90.f, desc.textureRefs[0].uvRotationDegrees);
    EXPECT_EQ("uv", desc.textureRefs[0].primvar);
}

TEST(NatronUtilityMaterial, AMaterialXNetworkIsUnsupported)
{
    const UtilityMaterialDesc desc = utilityMaterialFromNetwork(networkOf(TfToken("ND_standard_surface_surfaceshader")));

    EXPECT_EQ(UtilityMaterialDesc::eKindUnsupported, desc.kind);
    EXPECT_NE(std::string::npos, desc.unsupportedReason.find("ND_standard_surface_surfaceshader")) << desc.unsupportedReason;
}

TEST(NatronUtilityMaterial, AnUnknownNodeFeedingAPreviewSurfaceMakesItUnsupported)
{
    HdMaterialNetwork2 network = networkOf(TfToken("UsdPreviewSurface"));
    const SdfPath noise("/Material/Noise");

    network.nodes[noise].nodeTypeId = TfToken("ND_noise3d_color3");
    connect(network, kSurface, "diffuseColor", noise, "out");

    const UtilityMaterialDesc desc = utilityMaterialFromNetwork(network);

    EXPECT_EQ(UtilityMaterialDesc::eKindUnsupported, desc.kind);
    EXPECT_NE(std::string::npos, desc.unsupportedReason.find("ND_noise3d_color3")) << desc.unsupportedReason;
}

TEST(NatronUtilityMaterial, ANetworkWithoutASurfaceIsUnsupported)
{
    HdMaterialNetwork2 network = networkOf(TfToken("UsdPreviewSurface"));

    network.terminals.clear();
    EXPECT_EQ(UtilityMaterialDesc::eKindUnsupported, utilityMaterialFromNetwork(network).kind);
}

TEST(NatronUtilityMaterial, DisplayColorReadersFlagTheirInputs)
{
    HdMaterialNetwork2 network = networkOf(TfToken("UsdPreviewSurface"));
    const SdfPath color("/Material/Color");
    const SdfPath opacity("/Material/Opacity");

    network.nodes[color].nodeTypeId = TfToken("UsdPrimvarReader_float3");
    network.nodes[opacity].nodeTypeId = TfToken("UsdPrimvarReader_float");
    set(network, color, "varname", VtValue(TfToken("displayColor")));
    set(network, opacity, "varname", VtValue(std::string("displayOpacity")));
    connect(network, kSurface, "emissiveColor", color, "result");
    connect(network, kSurface, "opacity", opacity, "result");

    const UtilityMaterialDesc desc = utilityMaterialFromNetwork(network);

    ASSERT_EQ(UtilityMaterialDesc::eKindPreviewSurface, desc.kind) << desc.unsupportedReason;
    EXPECT_TRUE(desc.emissiveFromDisplayColor);
    EXPECT_FALSE(desc.diffuseFromDisplayColor);
    EXPECT_TRUE(desc.opacityFromDisplayOpacity);
}

TEST(NatronUtilityMaterial, APngLoadsBottomRowFirstWithBoxMips)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("grid.png")).toStdString();
    ASSERT_TRUE(writeTexture(path));

    std::string error;
    const UtilityTextureImageConstPtr image = UtilityTexturePool::loadImage(path, UtilityTextureRef::eColorSpaceRaw, &error);

    ASSERT_TRUE(bool(image)) << error;
    EXPECT_EQ(kTextureSize, image->width);
    EXPECT_EQ(kTextureSize, image->height);
    EXPECT_EQ(3, image->levels);
    ASSERT_EQ(16u + 4u + 1u, image->texels.size());
    for (int y = 0; y < kTextureSize; ++y) {
        for (int x = 0; x < kTextureSize; ++x) {
            expectNear(writtenTexel(x, kTextureSize - 1 - y), image->texels[y * kTextureSize + x], 1e-6f, "texel " + std::to_string(x) + ", " + std::to_string(y));
        }
    }

    GfVec4f mean(0.f);

    for (int i = 0; i < 16; ++i) {
        mean += image->texels[i] / 16.f;
    }
    expectNear(mean, image->texels[20], 1e-5f, "last mip");
    expectNear((image->texels[0] + image->texels[1] + image->texels[4] + image->texels[5]) / 4.f, image->texels[16], 1e-5f, "first mip");
}

TEST(NatronUtilityMaterial, TheCpuKernelSamplesTexelCentresExactly)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("grid.png")).toStdString();
    ASSERT_TRUE(writeTexture(path));

    EmittedTexture triangle(path);

    for (int y = 0; y < kTextureSize; ++y) {
        for (int x = 0; x < kTextureSize; ++x) {
            const GfVec4f color = triangle.shade((x + 0.5f) / kTextureSize, (y + 0.5f) / kTextureSize);

            expectNear(writtenTexel(x, kTextureSize - 1 - y), color, 1e-5f, "texel " + std::to_string(x) + ", " + std::to_string(y));
        }
    }

    // Halfway between two texels of a row is their mean.
    const GfVec4f between = triangle.shade(0.5f, 0.125f);

    expectNear((writtenTexel(1, 3) + writtenTexel(2, 3)) / 2.f, between, 1e-5f, "between texels");
    EXPECT_TRUE(triangle.scene.warnings().empty());
}

TEST(NatronUtilityMaterial, APixelWiderThanTheTextureReadsTheCoarsestMip)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("grid.png")).toStdString();
    ASSERT_TRUE(writeTexture(path));

    EmittedTexture triangle(path);
    GfVec4f mean(0.f);

    for (int y = 0; y < kTextureSize; ++y) {
        for (int x = 0; x < kTextureSize; ++x) {
            mean += writtenTexel(x, y) / float(kTextureSize * kTextureSize);
        }
    }
    expectNear(mean, triangle.shade(0.25f, 0.25f, 1), 1e-5f, "coarsest mip");
}

TEST(NatronUtilityMaterial, AMissingFileReadsTheFallbackAndWarnsNamingIt)
{
    const std::string missing = "/nonexistent/m281/missing.png";
    EmittedTexture triangle(missing, GfVec4f(0.5f, 0.25f, 0.125f, 1.f));

    expectNear(GfVec4f(0.5f, 0.25f, 0.125f, 1.f), triangle.shade(0.25f, 0.25f), 0.f, "fallback");

    const std::vector<std::string> warnings = triangle.scene.warnings();

    ASSERT_EQ(1u, warnings.size());
    EXPECT_NE(std::string::npos, warnings[0].find(missing)) << warnings[0];
    EXPECT_EQ(1u, triangle.scene.textures()->failures().size());
}

class NatronUtilityLiveTextureTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        LiveTextureStore::instance().clear();
        BaseTest::TearDown();
    }
};

TEST_F(NatronUtilityLiveTextureTest, ANatronTextureFromTheLiveStoreLoads)
{
    std::string error;
    ASSERT_TRUE(SceneCaches::registerLiveTextures(&error)) << error;

    const LiveTextureBufferConstPtr buffer = makeGradient(4, 3);
    ASSERT_TRUE(bool(buffer));
    const std::string uri = LiveTextureSource::makeUri("Gradient1", "rgba", 0x1234abcdULL);
    LiveTextureStore::instance().insert(uri, buffer);

    UtilityTexturePool pool;
    const UtilityTextureAssetConstPtr asset = pool.acquire(uri, UtilityTextureRef::eColorSpaceRaw);

    ASSERT_TRUE(bool(asset)) << (pool.failures().empty() ? std::string() : pool.failures().front());
    ASSERT_EQ(1u, asset->tiles.size());

    const UtilityTextureImageConstPtr& image = asset->tiles[0];

    ASSERT_TRUE(bool(image));
    EXPECT_EQ(4, image->width);
    EXPECT_EQ(3, image->height);
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 4; ++x) {
            expectNear(GfVec4f(x / 3.f, y / 2.f, 0.25f, 1.f), image->texels[y * 4 + x], 1e-6f, "texel " + std::to_string(x) + ", " + std::to_string(y));
        }
    }
    EXPECT_TRUE(pool.failures().empty());
}

// A new version of a live texture is a new URI, so it loads afresh and the old one goes once unused.
TEST_F(NatronUtilityLiveTextureTest, ANewVersionLoadsAndTheOldOneIsDropped)
{
    std::string error;
    ASSERT_TRUE(SceneCaches::registerLiveTextures(&error)) << error;

    const std::string first = LiveTextureSource::makeUri("Gradient1", "rgba", 1ULL);
    const std::string second = LiveTextureSource::makeUri("Gradient1", "rgba", 2ULL);
    LiveTextureStore::instance().insert(first, makeGradient(4, 4));

    UtilityTexturePool pool;

    ASSERT_TRUE(bool(pool.acquire(first, UtilityTextureRef::eColorSpaceRaw)));
    LiveTextureStore::instance().insert(second, makeGradient(2, 2));

    const UtilityTextureAssetConstPtr reloaded = pool.acquire(second, UtilityTextureRef::eColorSpaceRaw);

    ASSERT_TRUE(bool(reloaded));
    EXPECT_EQ(2, reloaded->tiles[0]->width);
    EXPECT_EQ(2u, pool.size());
    pool.retainOnly({ UtilityTexturePool::Key(second, (int)UtilityTextureRef::eColorSpaceRaw) });
    EXPECT_EQ(1u, pool.size());
}

TEST_F(NatronUtilityLiveTextureTest, AnUnservedNatronTextureIsAFailure)
{
    std::string error;
    ASSERT_TRUE(SceneCaches::registerLiveTextures(&error)) << error;

    UtilityTexturePool pool;
    const std::string uri = LiveTextureSource::makeUri("Missing1", "rgba", 7ULL);

    EXPECT_FALSE(bool(pool.acquire(uri, UtilityTextureRef::eColorSpaceRaw)));
    ASSERT_EQ(1u, pool.failures().size());
    EXPECT_NE(std::string::npos, pool.failures()[0].find(uri)) << pool.failures()[0];
}
