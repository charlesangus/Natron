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
#include <cstdio>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/AppManager.h"
#include "Engine/Nodes/Scene/TextureProjection.h"
#include "Engine/Plugin.h"

NATRON_NAMESPACE_USING

namespace {
const int kSize = 32;
const int kImageSize = 16;

// A camera at z = 10 looking down -z, seeing x and y in [-5, 5] at z = 0.
const char* const kCamera = R"(
def Camera "cam"
{
    float focalLength = 20
    float horizontalAperture = 20
    float verticalAperture = 20
    float2 clippingRange = (1, 1000)
    double3 xformOp:translate = (0, 0, 10)
    uniform token[] xformOpOrder = ["xformOp:translate"]
}
)";

// A 2 x 2 quad at z = 0 facing the camera (unless leftHanded), its st spanning [lo, hi] squared.
std::string
target(double lo = 0.,
       double hi = 1.,
       bool leftHanded = false)
{
    char st[160];

    std::snprintf(st, sizeof(st), "[(%g, %g), (%g, %g), (%g, %g), (%g, %g)]", lo, lo, hi, lo, hi, hi, lo, hi);

    return std::string("def Mesh \"Target\"\n{\n"
                       "    int[] faceVertexCounts = [4]\n"
                       "    int[] faceVertexIndices = [0, 1, 2, 3]\n"
                       "    point3f[] points = [(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)]\n")
        + (leftHanded ? "    uniform token orientation = \"leftHanded\"\n" : "") + "    texCoord2f[] primvars:st = " + st + " (\n        interpolation = \"vertex\"\n    )\n}\n";
}

// A quad at z = 2 hiding the half x < 0 of the target from the camera.
const char* const kOccluder = R"(
def Mesh "Occluder"
{
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(-2, -2, 2), (0, -2, 2), (0, 2, 2), (-2, 2, 2)]
}
)";

PXR_NS::UsdStageRefPtr
openStage(const std::string& body)
{
    PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::CreateAnonymous(".usda");

    if (!layer->ImportFromString("#usda 1.0\n" + std::string(kCamera) + body)) {
        return PXR_NS::UsdStageRefPtr();
    }

    return PXR_NS::UsdStage::Open(layer);
}

const std::vector<float>&
redImage()
{
    static const std::vector<float> pixels = [] {
        std::vector<float> p((std::size_t)kImageSize * kImageSize * 4, 0.f);
        for (std::size_t i = 0; i < (std::size_t)kImageSize * kImageSize; ++i) {
            p[i * 4] = 1.f;
            p[i * 4 + 3] = 1.f;
        }

        return p;
    }();

    return pixels;
}

TextureProjection::SourceImage
source()
{
    TextureProjection::SourceImage image;

    image.rgba = redImage().data();
    image.width = kImageSize;
    image.height = kImageSize;

    return image;
}

// The red image projected through /cam onto /Target at kSize squared, without dilation.
TextureProjection::Args
projectionArgs()
{
    TextureProjection::Args args;

    args.cameraPath = PXR_NS::SdfPath("/cam");
    args.meshPath = PXR_NS::SdfPath("/Target");
    args.width = kSize;
    args.height = kSize;

    return args;
}

// kCamera, given by its pose and lens.
SceneRaster::ExplicitCamera
explicitCamera()
{
    SceneRaster::ExplicitCamera cam;

    cam.cameraToWorld[14] = 10.;
    cam.viewHeight = 1.;
    cam.aspect = 1.;
    cam.nearClip = 1.;
    cam.farClip = 1000.;

    return cam;
}

bool
isRed(const TextureProjection::Result& r,
      std::size_t i)
{
    const float* t = &r.rgba[i * 4];

    return (std::abs(t[0] - 1.f) < 1e-4f) && (std::abs(t[1]) < 1e-4f) && (std::abs(t[2]) < 1e-4f) && (std::abs(t[3] - 1.f) < 1e-4f);
}

bool
isClear(const TextureProjection::Result& r,
        std::size_t i)
{
    const float* t = &r.rgba[i * 4];

    return (t[0] == 0.f) && (t[1] == 0.f) && (t[2] == 0.f) && (t[3] == 0.f);
}
} // anon namespace

TEST(TextureProjection, ProjectTextureIsNotAPlugin)
{
    ASSERT_TRUE(appPTR != nullptr);
    EXPECT_EQ(0u, appPTR->getPluginsList().count("ProjectTexture"));
}

TEST(TextureProjection, AConstantFillsTheWholeLayoutOfACameraFacingQuad)
{
    PXR_NS::UsdStageRefPtr stage = openStage(target());
    ASSERT_TRUE(stage);
    TextureProjection::Result result;
    std::string error;

    ASSERT_TRUE(TextureProjection::project(stage, source(), projectionArgs(), &result, &error)) << error;
    ASSERT_EQ(kSize, result.width);
    ASSERT_EQ(kSize, result.height);
    for (std::size_t i = 0; i < (std::size_t)kSize * kSize; ++i) {
        ASSERT_TRUE(isRed(result, i)) << "texel " << i % kSize << ", " << i / kSize << ": " << result.rgba[i * 4] << " " << result.rgba[i * 4 + 3];
        ASSERT_EQ(1, result.coverage[i]) << "texel " << i % kSize << ", " << i / kSize;
    }
}

TEST(TextureProjection, AMeshInFrontHidesWhatItCoversOnlyWithOcclusion)
{
    PXR_NS::UsdStageRefPtr stage = openStage(target() + kOccluder);
    ASSERT_TRUE(stage);
    TextureProjection::Args args = projectionArgs();
    TextureProjection::Result result;
    std::string error;

    // u runs with x on the target: the half u < 0.5 is behind the occluder.
    ASSERT_TRUE(TextureProjection::project(stage, source(), args, &result, &error)) << error;
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const double u = (x + 0.5) / kSize;
            const std::size_t i = (std::size_t)x + (std::size_t)y * kSize;
            if (u < 0.4) {
                EXPECT_TRUE(isClear(result, i)) << x << ", " << y << ": " << result.rgba[i * 4 + 3];
                EXPECT_EQ(0, result.coverage[i]) << x << ", " << y;
            } else if (u > 0.6) {
                EXPECT_TRUE(isRed(result, i)) << x << ", " << y << ": " << result.rgba[i * 4 + 3];
                EXPECT_EQ(1, result.coverage[i]) << x << ", " << y;
            }
        }
    }

    args.occlusion = false;
    ASSERT_TRUE(TextureProjection::project(stage, source(), args, &result, &error)) << error;
    for (std::size_t i = 0; i < (std::size_t)kSize * kSize; ++i) {
        ASSERT_TRUE(isRed(result, i)) << "texel " << i % kSize << ", " << i / kSize;
    }
}

TEST(TextureProjection, AnExplicitCameraProjectsAsTheSameCameraByPath)
{
    PXR_NS::UsdStageRefPtr stage = openStage(target() + kOccluder);
    ASSERT_TRUE(stage);
    TextureProjection::Args args = projectionArgs();
    TextureProjection::Result byPath;
    TextureProjection::Result byPose;
    std::string error;

    ASSERT_TRUE(TextureProjection::project(stage, source(), args, &byPath, &error)) << error;
    args.cameraPath = PXR_NS::SdfPath();
    args.explicitCamera = explicitCamera();
    ASSERT_TRUE(TextureProjection::project(stage, source(), args, &byPose, &error)) << error;
    ASSERT_EQ(byPath.rgba.size(), byPose.rgba.size());
    EXPECT_EQ(byPath.coverage, byPose.coverage);
    for (std::size_t i = 0; i < byPath.rgba.size(); ++i) {
        ASSERT_NEAR(byPath.rgba[i], byPose.rgba[i], 1e-5f) << "channel " << i;
    }
}

TEST(TextureProjection, FacesTurnedAwayFromTheCameraStayClearUnlessProjectedOnto)
{
    PXR_NS::UsdStageRefPtr stage = openStage(target(0., 1., true));
    ASSERT_TRUE(stage);
    TextureProjection::Args args = projectionArgs();
    TextureProjection::Result result;
    std::string error;

    ASSERT_TRUE(TextureProjection::project(stage, source(), args, &result, &error)) << error;
    for (std::size_t i = 0; i < (std::size_t)kSize * kSize; ++i) {
        ASSERT_TRUE(isClear(result, i)) << "texel " << i % kSize << ", " << i / kSize;
        ASSERT_EQ(0, result.coverage[i]);
    }

    args.projectBackFaces = true;
    ASSERT_TRUE(TextureProjection::project(stage, source(), args, &result, &error)) << error;
    for (std::size_t i = 0; i < (std::size_t)kSize * kSize; ++i) {
        ASSERT_TRUE(isRed(result, i)) << "texel " << i % kSize << ", " << i / kSize;
    }
}

TEST(TextureProjection, DilationGrowsTheLayoutByItsWidthInTexels)
{
    // The layout covers texel columns and rows 8 to 23.
    PXR_NS::UsdStageRefPtr stage = openStage(target(0.25, 0.75));
    ASSERT_TRUE(stage);
    TextureProjection::Args args = projectionArgs();
    TextureProjection::Result result;
    std::string error;

    ASSERT_TRUE(TextureProjection::project(stage, source(), args, &result, &error)) << error;
    const std::size_t row = (std::size_t)(kSize / 2) * kSize;
    EXPECT_TRUE(isClear(result, row + 7));
    EXPECT_TRUE(isRed(result, row + 8));
    EXPECT_TRUE(isRed(result, row + 23));
    EXPECT_TRUE(isClear(result, row + 24));

    args.edgeDilation = 2;
    ASSERT_TRUE(TextureProjection::project(stage, source(), args, &result, &error)) << error;
    EXPECT_TRUE(isClear(result, row + 5));
    EXPECT_TRUE(isRed(result, row + 6));
    EXPECT_TRUE(isRed(result, row + 25));
    EXPECT_TRUE(isClear(result, row + 26));
    EXPECT_TRUE(isRed(result, 6 + 6 * kSize));
    EXPECT_TRUE(isClear(result, 5 + 5 * kSize));
    EXPECT_EQ(0, result.coverage[row + 6]);
    EXPECT_EQ(1, result.coverage[row + 8]);
}

TEST(TextureProjection, AMissingPrimvarFailsNamingTheMesh)
{
    PXR_NS::UsdStageRefPtr stage = openStage(target());
    ASSERT_TRUE(stage);
    TextureProjection::Args args = projectionArgs();
    args.stName = PXR_NS::TfToken("uv");
    TextureProjection::Result result;
    std::string error;

    EXPECT_FALSE(TextureProjection::project(stage, source(), args, &result, &error));
    EXPECT_NE(std::string::npos, error.find("/Target")) << error;
}

TEST(TextureProjection, APrimThatIsNotAMeshFailsNamingIt)
{
    PXR_NS::UsdStageRefPtr stage = openStage(target());
    ASSERT_TRUE(stage);
    TextureProjection::Args args = projectionArgs();
    args.meshPath = PXR_NS::SdfPath("/cam");
    TextureProjection::Result result;
    std::string error;

    EXPECT_FALSE(TextureProjection::project(stage, source(), args, &result, &error));
    EXPECT_NE(std::string::npos, error.find("/cam")) << error;
}

TEST(TextureProjection, AMissingCameraFailsNamingItsPath)
{
    PXR_NS::UsdStageRefPtr stage = openStage(target());
    ASSERT_TRUE(stage);
    TextureProjection::Args args = projectionArgs();
    args.cameraPath = PXR_NS::SdfPath("/nowhere");
    TextureProjection::Result result;
    std::string error;

    EXPECT_FALSE(TextureProjection::project(stage, source(), args, &result, &error));
    EXPECT_NE(std::string::npos, error.find("/nowhere")) << error;
}
