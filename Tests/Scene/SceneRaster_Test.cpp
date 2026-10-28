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

#include "Engine/Nodes/Scene/SceneRaster.h"

NATRON_NAMESPACE_USING

namespace {
// A camera at (0, 0, 10) looking down -Z whose square aperture spans 10 units either side of the
// axis at 20 units away: at distance d it sees [-d / 2, d / 2] in x and y.
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

PXR_NS::UsdStageRefPtr
openStage(const std::string& body)
{
    PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::CreateAnonymous(".usda");

    if (!layer->ImportFromString("#usda 1.0\n" + body)) {
        return PXR_NS::UsdStageRefPtr();
    }

    return PXR_NS::UsdStage::Open(layer);
}

// A quad facing +Z with its corners at (x0, y0), (x1, y0), (x1, y1), (x0, y1), z per corner.
std::string
quad(const std::string& name,
     double x0,
     double y0,
     double x1,
     double y1,
     const double z[4],
     const std::string& extra = std::string())
{
    char points[256];

    std::snprintf(points, sizeof(points), "(%g, %g, %g), (%g, %g, %g), (%g, %g, %g), (%g, %g, %g)",
                  x0, y0, z[0], x1, y0, z[1], x1, y1, z[2], x0, y1, z[3]);

    return "def Mesh \"" + name + "\"\n{\n"
                                  "    int[] faceVertexCounts = [4]\n"
                                  "    int[] faceVertexIndices = [0, 1, 2, 3]\n"
                                  "    point3f[] points = ["
        + points + "]\n" + extra + "}\n";
}

const double kFlat[4] = { 0., 0., 0., 0. };

SceneRaster::CameraArgs
cameraArgs(int size)
{
    SceneRaster::CameraArgs args;

    args.cameraPath = PXR_NS::SdfPath("/cam");
    args.width = size;
    args.height = size;

    return args;
}

SceneRaster::BakeArgs
bakeArgs(const std::string& prim,
         int size)
{
    SceneRaster::BakeArgs args;

    args.primPath = PXR_NS::SdfPath(prim);
    args.width = size;
    args.height = size;

    return args;
}

const char* const kVertexSt = "    texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (\n"
                              "        interpolation = \"vertex\"\n"
                              "    )\n";
} // anonymous namespace

TEST(SceneRaster, ACameraFacingQuadFillsItsPixelRect)
{
    PXR_NS::UsdStageRefPtr stage = openStage(std::string(kCamera) + quad("card", -2., -2., 2., 2., kFlat));
    ASSERT_TRUE(bool(stage));

    SceneRaster::CameraBuffers buffers;
    std::string error;
    ASSERT_TRUE(SceneRaster::renderCamera(stage, cameraArgs(100), &buffers, &error)) << error;
    ASSERT_EQ((std::size_t)1, buffers.meshPaths.size());
    EXPECT_EQ(PXR_NS::SdfPath("/card"), buffers.meshPaths[0]);

    // At 10 units the camera sees [-5, 5], so the quad covers NDC [-0.4, 0.4]: pixels 30 to 69.
    for (int y = 0; y < 100; ++y) {
        for (int x = 0; x < 100; ++x) {
            const std::size_t i = x + y * 100;
            const bool inside = (x >= 30) && (x < 70) && (y >= 30) && (y < 70);
            ASSERT_EQ(inside ? 0 : -1, buffers.primIndex[i]) << x << ", " << y;
            if (inside) {
                EXPECT_NEAR(10., buffers.depth[i], 1e-4);
                EXPECT_NEAR(0., buffers.worldPosition[i][2], 1e-5);
                EXPECT_NEAR((x + 0.5 - 50.) / 10., buffers.worldPosition[i][0], 1e-4);
                EXPECT_NEAR((y + 0.5 - 50.) / 10., buffers.worldPosition[i][1], 1e-4);
            } else {
                EXPECT_TRUE(std::isinf(buffers.depth[i]));
            }
        }
    }
}

TEST(SceneRaster, ATiltedQuadHasMonotonicDepth)
{
    // The left edge is a unit nearer the camera than the centre, the right edge a unit further.
    const double tilt[4] = { 1., -1., -1., 1. };
    PXR_NS::UsdStageRefPtr stage = openStage(std::string(kCamera) + quad("card", -2., -2., 2., 2., tilt));
    ASSERT_TRUE(bool(stage));

    SceneRaster::CameraBuffers buffers;
    std::string error;
    ASSERT_TRUE(SceneRaster::renderCamera(stage, cameraArgs(100), &buffers, &error)) << error;

    const int row = 50;
    float previous = -1.f;
    int covered = 0;
    for (int x = 0; x < 100; ++x) {
        const std::size_t i = x + row * 100;
        if (buffers.primIndex[i] < 0) {
            continue;
        }
        ++covered;
        EXPECT_GT(buffers.depth[i], previous) << x;
        previous = buffers.depth[i];
        // The camera looks down -Z from z = 10.
        EXPECT_NEAR(10. - buffers.worldPosition[i][2], buffers.depth[i], 1e-4);
        EXPECT_GE(buffers.depth[i], 9.f - 1e-4f);
        EXPECT_LE(buffers.depth[i], 11.f + 1e-4f);
    }
    EXPECT_GT(covered, 30);
}

TEST(SceneRaster, TheNearestSurfaceWins)
{
    // A card 5 units from the camera covers [0, 1] x [-1, 1], NDC [0, 0.4] x [-0.4, 0.4].
    PXR_NS::UsdStageRefPtr stage = openStage(std::string(kCamera) + quad("back", -2., -2., 2., 2., kFlat) + quad("front", 0., -1., 1., 1., std::vector<double>(4, 5.).data()));
    ASSERT_TRUE(bool(stage));

    SceneRaster::CameraBuffers buffers;
    std::string error;
    ASSERT_TRUE(SceneRaster::renderCamera(stage, cameraArgs(100), &buffers, &error)) << error;
    ASSERT_EQ((std::size_t)2, buffers.meshPaths.size());
    ASSERT_EQ(PXR_NS::SdfPath("/front"), buffers.meshPaths[1]);

    for (int y = 30; y < 70; ++y) {
        for (int x = 30; x < 70; ++x) {
            const std::size_t i = x + y * 100;
            const bool front = x >= 50;
            ASSERT_EQ(front ? 1 : 0, buffers.primIndex[i]) << x << ", " << y;
            EXPECT_NEAR(front ? 5. : 10., buffers.depth[i], 1e-4);
        }
    }

    // Restricted to the back card, the front one is not drawn.
    SceneRaster::CameraArgs args = cameraArgs(100);
    args.prims.push_back(PXR_NS::SdfPath("/back"));
    ASSERT_TRUE(SceneRaster::renderCamera(stage, args, &buffers, &error)) << error;
    ASSERT_EQ((std::size_t)1, buffers.meshPaths.size());
    EXPECT_EQ(0, buffers.primIndex[60 + 50 * 100]);
    EXPECT_NEAR(10., buffers.depth[60 + 50 * 100], 1e-4);
}

TEST(SceneRaster, AFloorThroughTheCameraIsClippedAtTheNearPlane)
{
    // A floor a unit below the camera's axis, running from far ahead to far behind it.
    PXR_NS::UsdStageRefPtr stage = openStage(std::string(kCamera) + "def Mesh \"floor\"\n{\n"
                                                                    "    int[] faceVertexCounts = [4]\n"
                                                                    "    int[] faceVertexIndices = [0, 1, 2, 3]\n"
                                                                    "    point3f[] points = [(-50, -1, -500), (50, -1, -500), (50, -1, 500), (-50, -1, 500)]\n"
                                                                    "}\n");
    ASSERT_TRUE(bool(stage));

    SceneRaster::CameraBuffers buffers;
    std::string error;
    ASSERT_TRUE(SceneRaster::renderCamera(stage, cameraArgs(64), &buffers, &error)) << error;

    int covered = 0;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const std::size_t i = x + y * 64;
            if (buffers.primIndex[i] < 0) {
                continue;
            }
            ++covered;
            // Below the horizon only, and never nearer than the near plane.
            EXPECT_LT(y, 32);
            EXPECT_GE(buffers.depth[i], 1.f - 1e-4f);
            EXPECT_NEAR(-1., buffers.worldPosition[i][1], 1e-3);
        }
    }
    EXPECT_GT(covered, 0);
}

TEST(SceneRaster, AMissingCameraFailsNamingItsPath)
{
    PXR_NS::UsdStageRefPtr stage = openStage(quad("card", -2., -2., 2., 2., kFlat));
    ASSERT_TRUE(bool(stage));

    SceneRaster::CameraBuffers buffers;
    std::string error;
    EXPECT_FALSE(SceneRaster::renderCamera(stage, cameraArgs(16), &buffers, &error));
    EXPECT_NE(std::string::npos, error.find("/cam"));
}

TEST(SceneRaster, AUVMappedQuadBakesFullCoverageWithItsCornerPositions)
{
    PXR_NS::UsdStageRefPtr stage = openStage("def Xform \"geo\"\n{\n"
                                             "    double3 xformOp:translate = (1, 2, 3)\n"
                                             "    uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                                             + quad("card", -2., -2., 2., 2., kFlat, kVertexSt) + "}\n");
    ASSERT_TRUE(bool(stage));

    const int size = 16;
    SceneRaster::BakeBuffers buffers;
    std::string error;
    ASSERT_TRUE(SceneRaster::bakeUV(stage, bakeArgs("/geo/card", size), &buffers, &error)) << error;
    ASSERT_EQ((std::size_t)(size * size), buffers.coverage.size());

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const std::size_t i = x + y * size;
            ASSERT_EQ(1, buffers.coverage[i]) << x << ", " << y;
            // The texel centre (u, v) lies at (-2 + 4u, -2 + 4v, 0) on the card, moved by (1, 2, 3).
            const double u = (x + 0.5) / size;
            const double v = (y + 0.5) / size;
            EXPECT_NEAR(-1. + 4. * u, buffers.worldPosition[i][0], 1e-4);
            EXPECT_NEAR(4. * v, buffers.worldPosition[i][1], 1e-4);
            EXPECT_NEAR(3., buffers.worldPosition[i][2], 1e-5);
            EXPECT_NEAR(1., buffers.worldNormal[i][2], 1e-5);
        }
    }
    // The corner texels carry the card's corners, within half a texel (4 / 16 / 2 units).
    EXPECT_NEAR(-1., buffers.worldPosition[0][0], 0.125 + 1e-4);
    EXPECT_NEAR(0., buffers.worldPosition[0][1], 0.125 + 1e-4);
    EXPECT_NEAR(3., buffers.worldPosition[size * size - 1][0], 0.125 + 1e-4);
    EXPECT_NEAR(4., buffers.worldPosition[size * size - 1][1], 0.125 + 1e-4);
}

TEST(SceneRaster, NormalsFollowTheOrientationThroughAMirror)
{
    PXR_NS::UsdStageRefPtr stage = openStage("def Xform \"mirror\"\n{\n"
                                             "    double3 xformOp:scale = (-1, 1, 1)\n"
                                             "    uniform token[] xformOpOrder = [\"xformOp:scale\"]\n"
                                             + quad("card", -2., -2., 2., 2., kFlat, kVertexSt) + "}\n" + quad("flipped", -2., -2., 2., 2., kFlat, std::string(kVertexSt) + "    uniform token orientation = \"leftHanded\"\n"));
    ASSERT_TRUE(bool(stage));

    SceneRaster::BakeBuffers buffers;
    std::string error;
    // Mirroring the card does not turn its front away from +Z.
    ASSERT_TRUE(SceneRaster::bakeUV(stage, bakeArgs("/mirror/card", 8), &buffers, &error)) << error;
    EXPECT_NEAR(1., buffers.worldNormal[3 + 3 * 8][2], 1e-5);
    // A leftHanded card with the same points faces -Z.
    ASSERT_TRUE(SceneRaster::bakeUV(stage, bakeArgs("/flipped", 8), &buffers, &error)) << error;
    EXPECT_EQ(1, buffers.coverage[3 + 3 * 8]);
    EXPECT_NEAR(-1., buffers.worldNormal[3 + 3 * 8][2], 1e-5);
}

TEST(SceneRaster, IndexedFaceVaryingStBakesAsVertexSt)
{
    // Two quads sharing an edge, so the faceVarying values are indexed through shared entries.
    const std::string topology = "    int[] faceVertexCounts = [4, 4]\n"
                                 "    int[] faceVertexIndices = [0, 1, 4, 3, 1, 2, 5, 4]\n"
                                 "    point3f[] points = [(0, 0, 0), (1, 0, 0), (2, 0, 0), (0, 1, 0), (1, 1, 0), (2, 1, 0)]\n";
    PXR_NS::UsdStageRefPtr stage = openStage("def Mesh \"vertex\"\n{\n" + topology + "    texCoord2f[] primvars:st = [(0, 0), (0.5, 0), (1, 0), (0, 1), (0.5, 1), (1, 1)] (\n"
                                                                                     "        interpolation = \"vertex\"\n"
                                                                                     "    )\n"
                                                                                     "}\n"
                                                                                     "def Mesh \"faceVarying\"\n{\n"
                                             + topology + "    texCoord2f[] primvars:st = [(0, 0), (0.5, 0), (1, 0), (0, 1), (0.5, 1), (1, 1)] (\n"
                                                          "        interpolation = \"faceVarying\"\n"
                                                          "    )\n"
                                                          "    int[] primvars:st:indices = [0, 1, 4, 3, 1, 2, 5, 4]\n"
                                                          "}\n");
    ASSERT_TRUE(bool(stage));

    SceneRaster::BakeBuffers vertex;
    SceneRaster::BakeBuffers faceVarying;
    std::string error;
    ASSERT_TRUE(SceneRaster::bakeUV(stage, bakeArgs("/vertex", 32), &vertex, &error)) << error;
    ASSERT_TRUE(SceneRaster::bakeUV(stage, bakeArgs("/faceVarying", 32), &faceVarying, &error)) << error;

    int covered = 0;
    for (std::size_t i = 0; i < vertex.coverage.size(); ++i) {
        ASSERT_EQ(vertex.coverage[i], faceVarying.coverage[i]) << i;
        covered += vertex.coverage[i];
        for (int c = 0; c < 3; ++c) {
            EXPECT_NEAR(vertex.worldPosition[i][c], faceVarying.worldPosition[i][c], 1e-6);
            EXPECT_NEAR(vertex.worldNormal[i][c], faceVarying.worldNormal[i][c], 1e-6);
        }
    }
    EXPECT_EQ(32 * 32, covered);
}

TEST(SceneRaster, AUDIMTileSelectsTheUVsInIt)
{
    PXR_NS::UsdStageRefPtr stage = openStage(quad("card", -2., -2., 2., 2., kFlat,
                                                  "    texCoord2f[] primvars:st = [(1, 0), (2, 0), (2, 1), (1, 1)] (\n"
                                                  "        interpolation = \"vertex\"\n"
                                                  "    )\n"));
    ASSERT_TRUE(bool(stage));

    SceneRaster::BakeBuffers buffers;
    std::string error;
    SceneRaster::BakeArgs args = bakeArgs("/card", 16);
    ASSERT_TRUE(SceneRaster::bakeUV(stage, args, &buffers, &error)) << error;
    for (std::uint8_t c : buffers.coverage) {
        ASSERT_EQ(0, c);
    }
    args.udimTile = 1002;
    ASSERT_TRUE(SceneRaster::bakeUV(stage, args, &buffers, &error)) << error;
    for (std::uint8_t c : buffers.coverage) {
        ASSERT_EQ(1, c);
    }
    // The tile's bottom-left texel lies at the card's (u, v) = (1, 0) corner.
    EXPECT_NEAR(-2., buffers.worldPosition[0][0], 0.125 + 1e-4);
    EXPECT_NEAR(-2., buffers.worldPosition[0][1], 0.125 + 1e-4);
    args.udimTile = 1000;
    EXPECT_FALSE(SceneRaster::bakeUV(stage, args, &buffers, &error));
}

TEST(SceneRaster, AMissingStFailsNamingThePrim)
{
    PXR_NS::UsdStageRefPtr stage = openStage(quad("card", -2., -2., 2., 2., kFlat) + "def Xform \"group\"\n{\n}\n");
    ASSERT_TRUE(bool(stage));

    SceneRaster::BakeBuffers buffers;
    std::string error;
    EXPECT_FALSE(SceneRaster::bakeUV(stage, bakeArgs("/card", 8), &buffers, &error));
    EXPECT_NE(std::string::npos, error.find("/card")) << error;
    EXPECT_NE(std::string::npos, error.find("st")) << error;

    SceneRaster::BakeArgs args = bakeArgs("/card", 8);
    args.stName = PXR_NS::TfToken("uv");
    EXPECT_FALSE(SceneRaster::bakeUV(stage, args, &buffers, &error));
    EXPECT_NE(std::string::npos, error.find("uv")) << error;

    EXPECT_FALSE(SceneRaster::bakeUV(stage, bakeArgs("/group", 8), &buffers, &error));
    EXPECT_NE(std::string::npos, error.find("/group")) << error;
}
