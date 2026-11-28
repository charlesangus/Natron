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
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Scene/PaintMeshIndex.h"

NATRON_NAMESPACE_USING
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
// Appends the quad p0 p1 p2 p3, wound counter-clockwise seen from its front, as triangles
// (p0, p1, p2) and (p0, p2, p3), with st (0, 0), (1, 0), (1, 1), (0, 1) at its corners offset by
// stOffset.
void
addQuad(SceneRaster::TriangulatedMesh* mesh,
        const GfVec3f& p0,
        const GfVec3f& p1,
        const GfVec3f& p2,
        const GfVec3f& p3,
        const GfVec2f& stOffset = GfVec2f(0.f))
{
    const int base = (int)mesh->worldPoints.size();
    const GfVec2f corners[4] = { GfVec2f(0.f, 0.f), GfVec2f(1.f, 0.f), GfVec2f(1.f, 1.f), GfVec2f(0.f, 1.f) };
    const GfVec3f normal = GfGetNormalized(GfCross(p1 - p0, p2 - p0));
    const int face = mesh->faceIndices.empty() ? 0 : mesh->faceIndices.back() + 1;

    mesh->worldPoints.push_back(p0);
    mesh->worldPoints.push_back(p1);
    mesh->worldPoints.push_back(p2);
    mesh->worldPoints.push_back(p3);
    const int order[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
    for (int t = 0; t < 2; ++t) {
        mesh->triangles.push_back(GfVec3i(base + order[t][0], base + order[t][1], base + order[t][2]));
        mesh->faceIndices.push_back(face);
        mesh->normals.push_back(normal);
        mesh->hasSt.push_back(1);
        for (int k = 0; k < 3; ++k) {
            mesh->st.push_back(corners[order[t][k]] + stOffset);
        }
    }
}

// The square [-h, h]^2 at height z, facing +Z.
SceneRaster::TriangulatedMesh
squareAt(const std::string& path,
         float h,
         float z,
         const GfVec2f& stOffset = GfVec2f(0.f))
{
    SceneRaster::TriangulatedMesh mesh;

    mesh.path = SdfPath(path);
    addQuad(&mesh, GfVec3f(-h, -h, z), GfVec3f(h, -h, z), GfVec3f(h, h, z), GfVec3f(-h, h, z), stOffset);

    return mesh;
}

// The cube [-1, 1]^3 facing out, faces in the order +X, -X, +Y, -Y, +Z, -Z: face k is triangles 2k
// and 2k + 1.
SceneRaster::TriangulatedMesh
cube()
{
    SceneRaster::TriangulatedMesh mesh;

    mesh.path = SdfPath("/cube");
    addQuad(&mesh, GfVec3f(1, -1, -1), GfVec3f(1, 1, -1), GfVec3f(1, 1, 1), GfVec3f(1, -1, 1));
    addQuad(&mesh, GfVec3f(-1, -1, -1), GfVec3f(-1, -1, 1), GfVec3f(-1, 1, 1), GfVec3f(-1, 1, -1));
    addQuad(&mesh, GfVec3f(-1, 1, -1), GfVec3f(-1, 1, 1), GfVec3f(1, 1, 1), GfVec3f(1, 1, -1));
    addQuad(&mesh, GfVec3f(-1, -1, -1), GfVec3f(1, -1, -1), GfVec3f(1, -1, 1), GfVec3f(-1, -1, 1));
    addQuad(&mesh, GfVec3f(-1, -1, 1), GfVec3f(1, -1, 1), GfVec3f(1, 1, 1), GfVec3f(-1, 1, 1));
    addQuad(&mesh, GfVec3f(-1, -1, -1), GfVec3f(-1, 1, -1), GfVec3f(1, 1, -1), GfVec3f(1, -1, -1));

    return mesh;
}

// At (0, 0, 10) looking down -Z with a 90 degree view: at distance d it sees [-d, d] in x and y.
GfFrustum
perspectiveCamera()
{
    GfFrustum frustum;

    frustum.SetPosition(GfVec3d(0., 0., 10.));
    frustum.SetPerspective(90., 1., 1., 1000.);

    return frustum;
}

// The window coordinate in [0, 1] at which perspectiveCamera() sees x at distance 10.
double
windowAt(double x)
{
    return 0.5 * (x / 10. + 1.);
}

void
buildIndex(PaintMeshIndex* index,
           std::vector<SceneRaster::TriangulatedMesh> targets,
           std::vector<SceneRaster::TriangulatedMesh> occluders)
{
    std::string error;
    const bool built = index->build(std::move(targets), std::move(occluders), 1, &error);

    ASSERT_TRUE(built) << error;
}
} // anonymous namespace

TEST(PaintMeshIndex, ARayThroughATrianglesCentroidHitsItThere)
{
    PaintMeshIndex index;
    std::vector<SceneRaster::TriangulatedMesh> targets;

    targets.push_back(squareAt("/quad", 1.f, 0.f));
    buildIndex(&index, targets, std::vector<SceneRaster::TriangulatedMesh>());

    // Triangle 0 is (-1, -1), (1, -1), (1, 1).
    const double cx = 1. / 3.;
    const double cy = -1. / 3.;
    PaintMeshIndex::Hit hit;
    ASSERT_TRUE(index.raycast(perspectiveCamera(), windowAt(cx), windowAt(cy), &hit));
    EXPECT_EQ(0, hit.mesh);
    EXPECT_EQ(0, hit.triangle);
    for (int k = 0; k < 3; ++k) {
        EXPECT_NEAR(1. / 3., hit.barycentrics[k], 1e-4);
    }
    EXPECT_NEAR(cx, hit.position[0], 1e-4);
    EXPECT_NEAR(cy, hit.position[1], 1e-4);
    EXPECT_NEAR(0., hit.position[2], 1e-4);
    EXPECT_NEAR(1., hit.normal[2], 1e-6);
    EXPECT_TRUE(hit.hasSt);
    EXPECT_NEAR(2. / 3., hit.st[0], 1e-4);
    EXPECT_NEAR(1. / 3., hit.st[1], 1e-4);
    EXPECT_EQ(1001, hit.udimTile);
    EXPECT_NEAR(std::sqrt(100. + cx * cx + cy * cy), hit.distance, 1e-3);
}

TEST(PaintMeshIndex, AHitNamesTheUDIMTileOfItsSt)
{
    PaintMeshIndex index;
    std::vector<SceneRaster::TriangulatedMesh> targets;

    targets.push_back(squareAt("/quad", 1.f, 0.f, GfVec2f(1.f, 1.f)));
    buildIndex(&index, targets, std::vector<SceneRaster::TriangulatedMesh>());

    PaintMeshIndex::Hit hit;
    ASSERT_TRUE(index.raycast(perspectiveCamera(), windowAt(0.5), windowAt(-0.5), &hit));
    EXPECT_EQ(1012, hit.udimTile);
}

TEST(PaintMeshIndex, ARayPastEveryMeshMisses)
{
    PaintMeshIndex index;
    std::vector<SceneRaster::TriangulatedMesh> targets;

    targets.push_back(squareAt("/quad", 1.f, 0.f));
    buildIndex(&index, targets, std::vector<SceneRaster::TriangulatedMesh>());

    PaintMeshIndex::Hit hit;
    EXPECT_FALSE(index.raycast(perspectiveCamera(), windowAt(5.), windowAt(5.), &hit));
}

TEST(PaintMeshIndex, AnOccluderInFrontHidesTheTargetFromARay)
{
    PaintMeshIndex index;
    std::vector<SceneRaster::TriangulatedMesh> targets;
    std::vector<SceneRaster::TriangulatedMesh> occluders;

    targets.push_back(squareAt("/target", 2.f, 0.f));
    occluders.push_back(squareAt("/occluder", 0.5f, 5.f));
    buildIndex(&index, targets, occluders);

    PaintMeshIndex::Hit hit;
    EXPECT_FALSE(index.raycast(perspectiveCamera(), windowAt(0.1), windowAt(0.1), &hit));
    ASSERT_TRUE(index.raycast(perspectiveCamera(), windowAt(1.5), windowAt(1.5), &hit));
    EXPECT_EQ(0, hit.mesh);
}

TEST(PaintMeshIndex, ASphereOnACubeEdgeFindsTheTrianglesOfBothFaces)
{
    PaintMeshIndex index;
    std::vector<SceneRaster::TriangulatedMesh> targets;
    std::vector<SceneRaster::TriangulatedMesh> occluders;

    targets.push_back(cube());
    // An occluder through the sphere is never reported.
    occluders.push_back(squareAt("/occluder", 3.f, 1.f));
    buildIndex(&index, targets, occluders);

    // The edge between +X and +Z, from (1, -1, 1) to (1, 1, 1), is corners 2 and 3 of +X, held by
    // its triangle 1, and corners 1 and 2 of +Z, held by its triangle 0, triangle 8 of the cube.
    // The faces' other triangles come no nearer than their diagonals, 0.35 away.
    const std::vector<PaintMeshIndex::TriangleRef> found = index.trianglesInSphere(GfVec3f(1.f, 0.5f, 1.f), 0.1f);
    ASSERT_EQ(2u, found.size());
    EXPECT_EQ(1, found[0].triangle);
    EXPECT_EQ(8, found[1].triangle);
    for (std::size_t i = 0; i < found.size(); ++i) {
        EXPECT_EQ(0, found[i].mesh);
    }
}

TEST(PaintMeshIndex, ASphereReachesOnlyTheTrianglesWithinItsRadius)
{
    PaintMeshIndex index;
    std::vector<SceneRaster::TriangulatedMesh> targets;

    targets.push_back(cube());
    buildIndex(&index, targets, std::vector<SceneRaster::TriangulatedMesh>());

    EXPECT_TRUE(index.trianglesInSphere(GfVec3f(0.f), 0.9f).empty());
    EXPECT_EQ(12u, index.trianglesInSphere(GfVec3f(0.f), 1.01f).size());
    // Just off +X over its triangle 0, whose diagonal and every other face are more than 0.3 away.
    const std::vector<PaintMeshIndex::TriangleRef> found = index.trianglesInSphere(GfVec3f(1.1f, 0.5f, -0.5f), 0.3f);
    ASSERT_EQ(1u, found.size());
    EXPECT_EQ(0, found[0].triangle);
}

TEST(PaintMeshIndex, APointBehindAnotherSurfaceIsNotVisible)
{
    PaintMeshIndex index;
    std::vector<SceneRaster::TriangulatedMesh> targets;
    std::vector<SceneRaster::TriangulatedMesh> occluders;

    targets.push_back(squareAt("/target", 2.f, 0.f));
    occluders.push_back(squareAt("/occluder", 0.5f, 5.f));
    buildIndex(&index, targets, occluders);

    const GfVec3f points[] = {
        GfVec3f(0.f, 0.f, 0.f), // behind the occluder
        GfVec3f(1.5f, 1.5f, 0.f), // beside it, on the target
        GfVec3f(0.f, 0.f, 5.f), // on the occluder itself
        GfVec3f(30.f, 0.f, 0.f), // outside the window
        GfVec3f(0.f, 0.f, 20.f), // behind the camera
        GfVec3f(0.f, 0.f, 9.5f), // nearer than the near plane
    };
    const std::size_t count = sizeof(points) / sizeof(points[0]);
    std::vector<std::uint8_t> seen(count, 2);
    index.visible(points, nullptr, count, perspectiveCamera(), 0.01, seen.data());
    EXPECT_EQ(0, seen[0]);
    EXPECT_EQ(1, seen[1]);
    EXPECT_EQ(1, seen[2]);
    EXPECT_EQ(0, seen[3]);
    EXPECT_EQ(0, seen[4]);
    EXPECT_EQ(1, seen[5]);
}

TEST(PaintMeshIndex, APointFacingAwayIsNotVisible)
{
    PaintMeshIndex index;
    std::vector<SceneRaster::TriangulatedMesh> targets;

    targets.push_back(squareAt("/target", 2.f, 0.f));
    buildIndex(&index, targets, std::vector<SceneRaster::TriangulatedMesh>());

    const GfVec3f points[] = { GfVec3f(0.5f, 0.f, 0.f), GfVec3f(0.5f, 0.f, 0.f) };
    const GfVec3f normals[] = { GfVec3f(0.f, 0.f, 1.f), GfVec3f(0.f, 0.f, -1.f) };
    std::vector<std::uint8_t> seen(2, 2);
    index.visible(points, normals, 2, perspectiveCamera(), 0.01, seen.data());
    EXPECT_EQ(1, seen[0]);
    EXPECT_EQ(0, seen[1]);
}

TEST(PaintMeshIndex, MoreThanAPacketOfPointsAreEachDecided)
{
    PaintMeshIndex index;
    std::vector<SceneRaster::TriangulatedMesh> targets;
    std::vector<SceneRaster::TriangulatedMesh> occluders;

    targets.push_back(squareAt("/target", 4.f, 0.f));
    occluders.push_back(squareAt("/occluder", 1.f, 5.f));
    buildIndex(&index, targets, occluders);

    // Along y = 0 from x = -3.9 to 3.9: the occluder, half as far from the camera, hides |x| < 2.
    std::vector<GfVec3f> points;
    for (int i = 0; i < 40; ++i) {
        points.push_back(GfVec3f(-3.9f + 0.2f * (float)i, 0.f, 0.f));
    }
    std::vector<std::uint8_t> seen(points.size(), 2);
    index.visible(points.data(), nullptr, points.size(), perspectiveCamera(), 0.01, seen.data());
    for (std::size_t i = 0; i < points.size(); ++i) {
        const bool hidden = std::fabs(points[i][0]) < 2.f;
        EXPECT_EQ(hidden ? 0 : 1, seen[i]) << "x = " << points[i][0];
    }
}

TEST(PaintMeshIndex, AnOrthographicCameraLooksAlongItsViewDirection)
{
    PaintMeshIndex index;
    std::vector<SceneRaster::TriangulatedMesh> targets;
    std::vector<SceneRaster::TriangulatedMesh> occluders;

    targets.push_back(squareAt("/target", 4.f, 0.f));
    occluders.push_back(squareAt("/occluder", 1.f, 5.f));
    buildIndex(&index, targets, occluders);

    GfFrustum camera;
    camera.SetPosition(GfVec3d(0., 0., 10.));
    camera.SetOrthographic(-5., 5., -5., 5., 1., 1000.);

    // Unlike a perspective view, the occluder hides only what lies straight under it.
    const GfVec3f points[] = { GfVec3f(0.5f, 0.f, 0.f), GfVec3f(1.5f, 0.f, 0.f) };
    std::vector<std::uint8_t> seen(2, 2);
    index.visible(points, nullptr, 2, camera, 0.01, seen.data());
    EXPECT_EQ(0, seen[0]);
    EXPECT_EQ(1, seen[1]);

    PaintMeshIndex::Hit hit;
    EXPECT_FALSE(index.raycast(camera, 0.55, 0.5, &hit));
    ASSERT_TRUE(index.raycast(camera, 0.65, 0.5, &hit));
    EXPECT_NEAR(1.5, hit.position[0], 1e-4);
}

TEST(PaintMeshIndex, AnEmptyIndexHitsNothingAndHidesNothing)
{
    PaintMeshIndex index;

    PaintMeshIndex::Hit hit;
    EXPECT_FALSE(index.raycast(perspectiveCamera(), 0.5, 0.5, &hit));
    EXPECT_TRUE(index.trianglesInSphere(GfVec3f(0.f), 100.f).empty());
    const GfVec3f point(0.f);
    std::uint8_t seen = 2;
    index.visible(&point, nullptr, 1, perspectiveCamera(), 0.01, &seen);
    EXPECT_EQ(1, seen);
}
