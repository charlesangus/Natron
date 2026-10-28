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
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "Engine/Nodes/Scene/PrimitiveMeshes.h"

NATRON_NAMESPACE_USING

namespace {
const SceneMetadata::UpAxisEnum kUpAxes[] = { SceneMetadata::eUpAxisY, SceneMetadata::eUpAxisZ };

void
expectCounts(const PrimitiveMesh& mesh,
             std::size_t points,
             std::size_t faces)
{
    EXPECT_EQ(points, mesh.points.size());
    EXPECT_EQ(faces, mesh.faceVertexCounts.size());
}

// Every index in range, one normal and one st index per face vertex, and a valid topology.
void
expectWellFormed(const PrimitiveMesh& mesh)
{
    std::size_t corners = 0;
    for (int count : mesh.faceVertexCounts) {
        EXPECT_GE(count, 3);
        corners += (std::size_t)count;
    }
    ASSERT_EQ(corners, mesh.faceVertexIndices.size());
    ASSERT_EQ(corners, mesh.normals.size());
    ASSERT_EQ(corners, mesh.stIndices.size());
    for (int index : mesh.faceVertexIndices) {
        ASSERT_GE(index, 0);
        ASSERT_LT(index, (int)mesh.points.size());
    }
    for (int index : mesh.stIndices) {
        ASSERT_GE(index, 0);
        ASSERT_LT(index, (int)mesh.st.size());
    }
    std::string reason;
    EXPECT_TRUE(PXR_NS::UsdGeomMesh::ValidateTopology(mesh.faceVertexIndices, mesh.faceVertexCounts, mesh.points.size(), &reason)) << reason;
}

// Every edge in exactly two faces, run once each way, so the faces also wind consistently.
void
expectClosed(const PrimitiveMesh& mesh)
{
    std::map<std::pair<int, int>, int> directed;
    std::size_t start = 0;
    for (int count : mesh.faceVertexCounts) {
        for (int c = 0; c < count; ++c) {
            const int from = mesh.faceVertexIndices[start + c];
            const int to = mesh.faceVertexIndices[start + (c + 1) % count];
            ++directed[std::make_pair(from, to)];
        }
        start += (std::size_t)count;
    }
    for (const auto& edge : directed) {
        EXPECT_EQ(1, edge.second) << edge.first.first << "->" << edge.first.second;
        EXPECT_EQ((std::size_t)1, directed.count(std::make_pair(edge.first.second, edge.first.first))) << edge.first.first << "->" << edge.first.second << " has no twin";
    }
}

// The polygon's normal by Newell's method, unnormalized.
PXR_NS::GfVec3d
faceNormal(const PrimitiveMesh& mesh,
           std::size_t start,
           int count)
{
    PXR_NS::GfVec3d n(0.);
    for (int c = 0; c < count; ++c) {
        const PXR_NS::GfVec3d p(mesh.points[mesh.faceVertexIndices[start + c]]);
        const PXR_NS::GfVec3d q(mesh.points[mesh.faceVertexIndices[start + (c + 1) % count]]);
        n[0] += (p[1] - q[1]) * (p[2] + q[2]);
        n[1] += (p[2] - q[2]) * (p[0] + q[0]);
        n[2] += (p[0] - q[0]) * (p[1] + q[1]);
    }

    return n;
}

PXR_NS::GfVec3d
faceCentroid(const PrimitiveMesh& mesh,
             std::size_t start,
             int count)
{
    PXR_NS::GfVec3d c(0.);
    for (int i = 0; i < count; ++i) {
        c += PXR_NS::GfVec3d(mesh.points[mesh.faceVertexIndices[start + i]]);
    }

    return c / (double)count;
}

// Unit normals on the side the face winds counter-clockwise towards; for a mesh centred on the
// origin and convex, also pointing away from the origin.
void
expectNormals(const PrimitiveMesh& mesh,
              bool outwardFromOrigin)
{
    std::size_t start = 0;
    for (int count : mesh.faceVertexCounts) {
        const PXR_NS::GfVec3d geometric = faceNormal(mesh, start, count);
        ASSERT_GT(geometric.GetLength(), 0.) << "degenerate face at corner " << start;
        const PXR_NS::GfVec3d centroid = faceCentroid(mesh, start, count);
        for (int c = 0; c < count; ++c) {
            const PXR_NS::GfVec3d n(mesh.normals[start + c]);
            EXPECT_NEAR(1., n.GetLength(), 1e-5);
            EXPECT_GT(PXR_NS::GfDot(n, geometric), 0.) << "corner " << start + c;
            if (outwardFromOrigin) {
                EXPECT_GT(PXR_NS::GfDot(n, centroid), 0.) << "corner " << start + c;
            }
        }
        start += (std::size_t)count;
    }
}

void
expectStCoversUnitSquare(const PrimitiveMesh& mesh)
{
    float lo[2] = { 1.f, 1.f };
    float hi[2] = { 0.f, 0.f };
    for (int index : mesh.stIndices) {
        const PXR_NS::GfVec2f& st = mesh.st[index];
        for (int a = 0; a < 2; ++a) {
            EXPECT_GE(st[a], 0.f);
            EXPECT_LE(st[a], 1.f);
            lo[a] = std::min(lo[a], st[a]);
            hi[a] = std::max(hi[a], st[a]);
        }
    }
    for (int a = 0; a < 2; ++a) {
        EXPECT_FLOAT_EQ(0.f, lo[a]);
        EXPECT_FLOAT_EQ(1.f, hi[a]);
    }
}

PXR_NS::GfRange3d
bounds(const PrimitiveMesh& mesh)
{
    PXR_NS::GfRange3d range;
    for (const PXR_NS::GfVec3f& p : mesh.points) {
        range.UnionWith(PXR_NS::GfVec3d(p));
    }

    return range;
}

void
expectBounds(const PrimitiveMesh& mesh,
             const PXR_NS::GfVec3d& size)
{
    const PXR_NS::GfRange3d range = bounds(mesh);
    for (int a = 0; a < 3; ++a) {
        EXPECT_NEAR(-0.5 * size[a], range.GetMin()[a], 1e-5) << "axis " << a;
        EXPECT_NEAR(0.5 * size[a], range.GetMax()[a], 1e-5) << "axis " << a;
    }
}
} // anonymous namespace

TEST(PrimitiveMeshes, CardIsAGridFacingTheForwardAxis)
{
    for (SceneMetadata::UpAxisEnum upAxis : kUpAxes) {
        SCOPED_TRACE(upAxis == SceneMetadata::eUpAxisZ ? "Z up" : "Y up");
        const bool zUp = (upAxis == SceneMetadata::eUpAxisZ);
        const PrimitiveMesh mesh = PrimitiveMeshes::buildCard(2., 1., 8, 8, upAxis);
        expectCounts(mesh, 81, 64);
        expectWellFormed(mesh);
        expectNormals(mesh, false);
        expectStCoversUnitSquare(mesh);
        const PXR_NS::GfVec3f forward = zUp ? PXR_NS::GfVec3f(0.f, -1.f, 0.f) : PXR_NS::GfVec3f(0.f, 0.f, 1.f);
        for (const PXR_NS::GfVec3f& n : mesh.normals) {
            EXPECT_EQ(forward, n);
        }
        expectBounds(mesh, zUp ? PXR_NS::GfVec3d(2., 0., 1.) : PXR_NS::GfVec3d(2., 1., 0.));

        const PrimitiveMesh minimum = PrimitiveMeshes::buildCard(1., 1., 0, -3, upAxis);
        expectCounts(minimum, 4, 1);
        expectWellFormed(minimum);
        expectNormals(minimum, false);
        expectStCoversUnitSquare(minimum);
    }
}

TEST(PrimitiveMeshes, SphereIsClosedWithASeamInSt)
{
    for (SceneMetadata::UpAxisEnum upAxis : kUpAxes) {
        SCOPED_TRACE(upAxis == SceneMetadata::eUpAxisZ ? "Z up" : "Y up");
        struct Case {
            int rows, columns;
            std::size_t points, faces;
        };
        const Case cases[] = {
            { 30, 30, 2 + 29 * 30, 30 * 30 },
            { 2, 3, 5, 6 },
            { 1, 0, 5, 6 },
        };
        for (const Case& c : cases) {
            SCOPED_TRACE(testing::Message() << c.rows << "x" << c.columns);
            const PrimitiveMesh mesh = PrimitiveMeshes::buildSphere(2., c.rows, c.columns, upAxis);
            expectCounts(mesh, c.points, c.faces);
            expectWellFormed(mesh);
            expectClosed(mesh);
            expectNormals(mesh, true);
            expectStCoversUnitSquare(mesh);
            for (const PXR_NS::GfVec3f& p : mesh.points) {
                EXPECT_NEAR(2., PXR_NS::GfVec3d(p).GetLength(), 1e-5);
            }
            const int up = (upAxis == SceneMetadata::eUpAxisZ) ? 2 : 1;
            EXPECT_NEAR(-2., bounds(mesh).GetMin()[up], 1e-6);
            EXPECT_NEAR(2., bounds(mesh).GetMax()[up], 1e-6);
        }

        // A seam point is used with u = 0 by the faces on one side and u = 1 on the other.
        const PrimitiveMesh mesh = PrimitiveMeshes::buildSphere(1., 4, 8, upAxis);
        std::map<int, std::set<float>> uByPoint;
        for (std::size_t i = 0; i < mesh.faceVertexIndices.size(); ++i) {
            uByPoint[mesh.faceVertexIndices[i]].insert(mesh.st[mesh.stIndices[i]][0]);
        }
        int seamPoints = 0;
        for (const auto& point : uByPoint) {
            if (point.second.count(0.f) && point.second.count(1.f)) {
                ++seamPoints;
            }
        }
        EXPECT_EQ(3, seamPoints);
    }
}

TEST(PrimitiveMeshes, CubeIsClosedAndSizedAlongSceneAxes)
{
    for (SceneMetadata::UpAxisEnum upAxis : kUpAxes) {
        SCOPED_TRACE(upAxis == SceneMetadata::eUpAxisZ ? "Z up" : "Y up");
        const int unit[3] = { 1, 1, 1 };
        const PrimitiveMesh cube = PrimitiveMeshes::buildCube(PXR_NS::GfVec3d(1.), unit, upAxis);
        expectCounts(cube, 8, 6);
        expectWellFormed(cube);
        expectClosed(cube);
        expectNormals(cube, true);
        expectStCoversUnitSquare(cube);
        expectBounds(cube, PXR_NS::GfVec3d(1.));

        const int segments[3] = { 2, 3, 4 };
        const PrimitiveMesh box = PrimitiveMeshes::buildCube(PXR_NS::GfVec3d(1., 2., 3.), segments, upAxis);
        expectCounts(box, 3 * 4 * 5 - 1 * 2 * 3, 2 * (2 * 3 + 3 * 4 + 2 * 4));
        expectWellFormed(box);
        expectClosed(box);
        expectNormals(box, true);
        expectStCoversUnitSquare(box);
        expectBounds(box, PXR_NS::GfVec3d(1., 2., 3.));

        const int below[3] = { 0, -1, 0 };
        expectCounts(PrimitiveMeshes::buildCube(PXR_NS::GfVec3d(1.), below, upAxis), 8, 6);
    }
}

TEST(PrimitiveMeshes, CylinderRunsAlongTheUpAxisWithOptionalCaps)
{
    for (SceneMetadata::UpAxisEnum upAxis : kUpAxes) {
        SCOPED_TRACE(upAxis == SceneMetadata::eUpAxisZ ? "Z up" : "Y up");
        const bool zUp = (upAxis == SceneMetadata::eUpAxisZ);
        struct Case {
            int rows, columns;
            std::size_t points, faces;
        };
        const Case cases[] = {
            { 30, 30, 31 * 30 + 2, 30 * 30 + 2 * 30 },
            { 1, 3, 2 * 3 + 2, 3 + 2 * 3 },
            { 0, 2, 2 * 3 + 2, 3 + 2 * 3 },
        };
        for (const Case& c : cases) {
            SCOPED_TRACE(testing::Message() << c.rows << "x" << c.columns);
            const PrimitiveMesh mesh = PrimitiveMeshes::buildCylinder(1., 2., c.rows, c.columns, true, true, upAxis);
            expectCounts(mesh, c.points, c.faces);
            expectWellFormed(mesh);
            expectClosed(mesh);
            expectNormals(mesh, true);
            expectStCoversUnitSquare(mesh);
        }

        const PrimitiveMesh tall = PrimitiveMeshes::buildCylinder(0.5, 3., 2, 8, true, true, upAxis);
        expectBounds(tall, zUp ? PXR_NS::GfVec3d(1., 1., 3.) : PXR_NS::GfVec3d(1., 3., 1.));

        // Each cap alone, then an open tube: no longer closed, but well formed.
        const PrimitiveMesh topOnly = PrimitiveMeshes::buildCylinder(1., 2., 2, 6, true, false, upAxis);
        expectCounts(topOnly, 3 * 6 + 1, 2 * 6 + 6);
        expectWellFormed(topOnly);
        expectNormals(topOnly, true);
        const PrimitiveMesh tube = PrimitiveMeshes::buildCylinder(1., 2., 2, 6, false, false, upAxis);
        expectCounts(tube, 3 * 6, 2 * 6);
        expectWellFormed(tube);
        expectNormals(tube, true);
        expectStCoversUnitSquare(tube);
    }
}
