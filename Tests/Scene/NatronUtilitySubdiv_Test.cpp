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

#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/rect2i.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/vt/dictionary.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdx/tokens.h>
#include <pxr/imaging/pxOsd/meshTopology.h>
#include <pxr/imaging/pxOsd/subdivTags.h>
#include <pxr/imaging/pxOsd/tokens.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>

#include "Global/Macros.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/Utility/UtilityRenderDelegate.h"
#include "Engine/Nodes/Scene/Utility/UtilitySubdiv.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_USING

namespace {
const VtIntArray kCubeCounts = { 4, 4, 4, 4, 4, 4 };
const VtIntArray kCubeIndices = { 0, 1, 3, 2, 2, 3, 5, 4, 4, 5, 7, 6, 6, 7, 1, 0, 1, 7, 5, 3, 6, 0, 2, 4 };
const VtVec3fArray kCubePoints = { GfVec3f(-1, -1, 1), GfVec3f(1, -1, 1), GfVec3f(-1, 1, 1), GfVec3f(1, 1, 1),
                                   GfVec3f(-1, 1, -1), GfVec3f(1, 1, -1), GfVec3f(-1, -1, -1), GfVec3f(1, -1, -1) };

const char* const kCubeGeometry = R"(
    int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
    int[] faceVertexIndices = [0, 1, 3, 2, 2, 3, 5, 4, 4, 5, 7, 6, 6, 7, 1, 0, 1, 7, 5, 3, 6, 0, 2, 4]
    point3f[] points = [(-1, -1, 1), (1, -1, 1), (-1, 1, 1), (1, 1, 1), (-1, 1, -1), (1, 1, -1), (-1, -1, -1), (1, -1, -1)]
)";

// Catmull-Clark triangles of the cube: six quads, four per quad per level, two triangles each.
std::size_t
cubeTriangles(int level)
{
    return level == 0 ? 12 : 12 * static_cast<std::size_t>(std::pow(4., level));
}

PxOsdMeshTopology
cube(const TfToken& scheme,
     const PxOsdSubdivTags& tags = PxOsdSubdivTags(),
     const VtIntArray& holes = VtIntArray())
{
    return PxOsdMeshTopology(scheme, PxOsdOpenSubdivTokens->rightHanded, kCubeCounts, kCubeIndices, holes, tags);
}

UtilitySubdivRefinementPtr
refine(const PxOsdMeshTopology& topology,
       int level,
       const std::vector<VtIntArray>& faceVarying = std::vector<VtIntArray>())
{
    std::string error;
    UtilitySubdivRefinementPtr refinement = UtilitySubdivRefinement::get(topology, level, faceVarying, &error);

    EXPECT_TRUE(refinement != nullptr) << error;

    return refinement;
}

bool
near(const GfVec3f& a,
     const GfVec3f& b,
     float tolerance = 1e-5f)
{
    return (a - b).GetLength() < tolerance;
}

std::string
cubeAt(const char* name,
       const std::string& attributes)
{
    return std::string("def Mesh \"") + name + "\" {" + kCubeGeometry + attributes + "}\n";
}

const double kCameraZ = 30.;
const double kFieldOfView = 45.;

// The stage through the engine the node drives the delegate with, at a chosen image size.
class SubdivRender {
public:
    SubdivRender(const std::string& usda,
                 int size = 16)
        : _size(size)
    {
        RenderDelegates::list();
        _layer = SdfLayer::CreateAnonymous(".usda");
        EXPECT_TRUE(_layer->ImportFromString(usda));
        stage = UsdStage::Open(_layer);

        UsdImagingGLEngine::Parameters parameters;

        parameters.rendererPluginId = TfToken(kRenderDelegateUtilityId);
        parameters.gpuEnabled = false;
        _engine.reset(new UsdImagingGLEngine(parameters));
        _engine->SetEnablePresentation(false);
        EXPECT_TRUE(_engine->SetRendererAovs({ HdAovTokens->color }));
    }

    void setSetting(const char* key,
                    int value)
    {
        _engine->SetRendererSetting(TfToken(key), VtValue(value));
    }

    void render()
    {
        const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now() + std::chrono::seconds(60);

        renderOnce();
        while (!_engine->IsConverged() && std::chrono::steady_clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            renderOnce();
        }
        EXPECT_TRUE(_engine->IsConverged());
    }

    std::size_t stat(const char* key) const
    {
        return VtDictionaryGet<std::size_t>(_engine->GetRenderStats(), key, VtDefault = std::size_t(0));
    }

    // Stops the delegate, from any thread, as an abort does.
    bool stop()
    {
        return _engine->StopRenderer();
    }

    void restart()
    {
        EXPECT_TRUE(_engine->RestartRenderer());
    }

    bool isConverged() const
    {
        return _engine->IsConverged();
    }

    std::string warning() const
    {
        return VtDictionaryGet<std::string>(_engine->GetRenderStats(), kUtilityStatWarning, VtDefault = std::string());
    }

    float alphaAt(int x,
                  int y) const
    {
        HdRenderBuffer* buffer = _engine->GetAovRenderBuffer(HdAovTokens->color);

        if (!buffer) {
            ADD_FAILURE() << "no color buffer";

            return 0.f;
        }
        const GfVec4f* pixels = static_cast<const GfVec4f*>(buffer->Map());
        const float alpha = pixels[y * _size + x][3];

        buffer->Unmap();

        return alpha;
    }

    void movePoint(const char* path,
                   int point,
                   const GfVec3f& position)
    {
        const UsdGeomMesh mesh(stage->GetPrimAtPath(SdfPath(path)));
        VtVec3fArray points;

        ASSERT_TRUE(mesh.GetPointsAttr().Get(&points));
        points[point] = position;
        ASSERT_TRUE(mesh.GetPointsAttr().Set(points));
    }

    void renderOnce()
    {
        GfFrustum camera;

        camera.SetPerspective(kFieldOfView, 1., 0.1, 1000.);
        camera.SetPosition(GfVec3d(0., 0., kCameraZ));
        _engine->SetRenderBufferSize(GfVec2i(_size, _size));
        _engine->SetFraming(CameraUtilFraming(GfRect2i(GfVec2i(0, 0), _size, _size)));
        _engine->SetCameraState(camera.ComputeViewMatrix(), camera.ComputeProjectionMatrix());

        UsdImagingGLRenderParams params;

        params.drawMode = UsdImagingGLDrawMode::DRAW_SHADED_SMOOTH;
        params.clearColor = GfVec4f(0.f);
        params.colorCorrectionMode = HdxColorCorrectionTokens->disabled;
        params.highlight = false;
        _engine->Render(stage->GetPseudoRoot(), params);
    }

    UsdStageRefPtr stage;

private:
    int _size;
    SdfLayerRefPtr _layer;
    std::unique_ptr<UsdImagingGLEngine> _engine;
};

// A sphere cage of radius about the origin, its poles on the view axis so the silhouette runs along
// the equator.
void
defineSphereCage(const UsdStageRefPtr& stage,
                 double radius,
                 int segments,
                 int rings)
{
    VtVec3fArray points;
    VtIntArray counts;
    VtIntArray indices;
    const auto ringPoint = [segments](int ring, int segment) { return 1 + (ring - 1) * segments + segment % segments; };

    points.push_back(GfVec3f(0.f, 0.f, static_cast<float>(radius)));
    for (int ring = 1; ring < rings; ++ring) {
        const double theta = M_PI * ring / rings;

        for (int segment = 0; segment < segments; ++segment) {
            const double phi = 2. * M_PI * segment / segments;

            points.push_back(GfVec3f(static_cast<float>(radius * std::sin(theta) * std::cos(phi)),
                                     static_cast<float>(radius * std::sin(theta) * std::sin(phi)),
                                     static_cast<float>(radius * std::cos(theta))));
        }
    }
    points.push_back(GfVec3f(0.f, 0.f, static_cast<float>(-radius)));

    const int south = static_cast<int>(points.size()) - 1;

    for (int segment = 0; segment < segments; ++segment) {
        counts.push_back(3);
        indices.push_back(0);
        indices.push_back(ringPoint(1, segment));
        indices.push_back(ringPoint(1, segment + 1));
    }
    for (int ring = 1; ring + 1 < rings; ++ring) {
        for (int segment = 0; segment < segments; ++segment) {
            counts.push_back(4);
            indices.push_back(ringPoint(ring, segment));
            indices.push_back(ringPoint(ring + 1, segment));
            indices.push_back(ringPoint(ring + 1, segment + 1));
            indices.push_back(ringPoint(ring, segment + 1));
        }
    }
    for (int segment = 0; segment < segments; ++segment) {
        counts.push_back(3);
        indices.push_back(south);
        indices.push_back(ringPoint(rings - 1, segment + 1));
        indices.push_back(ringPoint(rings - 1, segment));
    }

    UsdGeomMesh mesh = UsdGeomMesh::Define(stage, SdfPath("/Sphere"));

    mesh.CreatePointsAttr(VtValue(points));
    mesh.CreateFaceVertexCountsAttr(VtValue(counts));
    mesh.CreateFaceVertexIndicesAttr(VtValue(indices));
    mesh.CreateSubdivisionSchemeAttr(VtValue(UsdGeomTokens->catmullClark));
}

// A Catmull-Clark grid of n by n quads, its points row by row.
PxOsdMeshTopology
gridTopology(int n)
{
    VtIntArray counts(n * n, 4);
    VtIntArray indices;

    indices.reserve(n * n * 4);
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const int corner = y * (n + 1) + x;

            indices.push_back(corner);
            indices.push_back(corner + 1);
            indices.push_back(corner + n + 2);
            indices.push_back(corner + n + 1);
        }
    }

    return PxOsdMeshTopology(PxOsdOpenSubdivTokens->catmullClark, PxOsdOpenSubdivTokens->rightHanded, counts, indices);
}

// The grid topology of n, 20 units wide and facing the camera.
void
defineGridCage(const UsdStageRefPtr& stage,
               int n)
{
    const PxOsdMeshTopology topology = gridTopology(n);
    VtVec3fArray points;

    points.reserve((n + 1) * (n + 1));
    for (int y = 0; y <= n; ++y) {
        for (int x = 0; x <= n; ++x) {
            points.push_back(GfVec3f(-10.f + 20.f * x / n, -10.f + 20.f * y / n, 0.f));
        }
    }

    UsdGeomMesh mesh = UsdGeomMesh::Define(stage, SdfPath("/Grid"));

    mesh.CreatePointsAttr(VtValue(points));
    mesh.CreateFaceVertexCountsAttr(VtValue(topology.GetFaceVertexCounts()));
    mesh.CreateFaceVertexIndicesAttr(VtValue(topology.GetFaceVertexIndices()));
    mesh.CreateSubdivisionSchemeAttr(VtValue(UsdGeomTokens->catmullClark));
}
} // anon namespace

// The cube's level-1 points by the Catmull-Clark rules: each corner moves to 5/9 of itself, an edge
// point is the mean of its ends and the two face centres, and a face point is the face's centre.
TEST(NatronUtilitySubdiv, ACubeAtLevelOneIsTwentyFourQuadsOnTheCatmullClarkPoints)
{
    const UtilitySubdivRefinementPtr refinement = refine(cube(PxOsdOpenSubdivTokens->catmullClark), 1);

    ASSERT_TRUE(refinement != nullptr);
    EXPECT_EQ(48u, refinement->triangles().size());
    EXPECT_EQ(26u, refinement->refinedPointCount());

    VtVec3fArray refined;

    refinement->refine(kCubePoints, &refined);
    ASSERT_EQ(26u, refined.size());

    int corners = 0;
    int edges = 0;
    int faces = 0;

    for (const GfVec3f& p : refined) {
        int ones = 0;
        int threeQuarters = 0;
        int fiveNinths = 0;
        int zeros = 0;

        for (int axis = 0; axis < 3; ++axis) {
            const float a = std::fabs(p[axis]);

            ones += std::fabs(a - 1.f) < 1e-5f;
            threeQuarters += std::fabs(a - 0.75f) < 1e-5f;
            fiveNinths += std::fabs(a - 5.f / 9.f) < 1e-5f;
            zeros += a < 1e-5f;
        }
        corners += fiveNinths == 3;
        edges += threeQuarters == 2 && zeros == 1;
        faces += ones == 1 && zeros == 2;
    }
    EXPECT_EQ(8, corners);
    EXPECT_EQ(12, edges);
    EXPECT_EQ(6, faces);
}

TEST(NatronUtilitySubdiv, LimitNormalsPointOutOfTheCubeAndAgreeWithTheWinding)
{
    const UtilitySubdivRefinementPtr refinement = refine(cube(PxOsdOpenSubdivTokens->catmullClark), 2);

    ASSERT_TRUE(refinement != nullptr);
    VtVec3fArray points;
    VtVec3fArray normals;

    refinement->evaluate(kCubePoints, &points, &normals);
    ASSERT_EQ(refinement->refinedPointCount(), points.size());
    ASSERT_EQ(points.size(), normals.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
        EXPECT_NEAR(1.f, normals[i].GetLength(), 1e-4f);
        EXPECT_GT(GfDot(points[i], normals[i]), 0.f) << "point " << i;
    }
    for (const GfVec3i& t : refinement->triangles()) {
        const GfVec3f facing = GfCross(points[t[1]] - points[t[0]], points[t[2]] - points[t[0]]);

        EXPECT_GT(GfDot(facing, normals[t[0]]), 0.f);
    }
}

// The four top edges creased at 10 stay sharp through every level, so each top corner's limit is
// the crease curve's, (p- + 4 p + p+) / 6 along the loop, and nothing rises above the top face.
TEST(NatronUtilitySubdiv, ACreaseOfSharpnessTenKeepsItsEdgeOnTheLimit)
{
    PxOsdSubdivTags tags;

    tags.SetCreaseIndices(VtIntArray { 2, 3, 5, 4, 2 });
    tags.SetCreaseLengths(VtIntArray { 5 });
    tags.SetCreaseWeights(VtFloatArray { 10.f });

    const UtilitySubdivRefinementPtr refinement = refine(cube(PxOsdOpenSubdivTokens->catmullClark, tags), 2);

    ASSERT_TRUE(refinement != nullptr);
    VtVec3fArray points;
    VtVec3fArray normals;

    refinement->evaluate(kCubePoints, &points, &normals);

    bool cornerOnCrease = false;
    float top = -1.f;

    for (const GfVec3f& p : points) {
        cornerOnCrease = cornerOnCrease || near(p, GfVec3f(2.f / 3.f, 1.f, 2.f / 3.f));
        top = std::max(top, p[1]);
    }
    EXPECT_TRUE(cornerOnCrease);
    EXPECT_NEAR(1.f, top, 1e-5f);

    // Uncreased, the same corner is pulled well inside.
    const UtilitySubdivRefinementPtr smooth = refine(cube(PxOsdOpenSubdivTokens->catmullClark), 2);

    ASSERT_TRUE(smooth != nullptr);
    smooth->evaluate(kCubePoints, &points, &normals);
    for (const GfVec3f& p : points) {
        EXPECT_LT(p[1], 0.9f);
    }
}

TEST(NatronUtilitySubdiv, AHoleFaceProducesNoTriangles)
{
    const UtilitySubdivRefinementPtr refinement = refine(cube(PxOsdOpenSubdivTokens->catmullClark, PxOsdSubdivTags(), VtIntArray { 0 }), 1);

    ASSERT_TRUE(refinement != nullptr);
    EXPECT_EQ(40u, refinement->triangles().size());
    ASSERT_EQ(refinement->triangles().size(), refinement->coarseFaces().size());
    for (const int face : refinement->coarseFaces()) {
        EXPECT_NE(0, face);
    }
}

TEST(NatronUtilitySubdiv, TheSameTopologyAndLevelComeFromTheCache)
{
    const UtilitySubdivRefinementPtr first = refine(cube(PxOsdOpenSubdivTokens->catmullClark), 3);
    const std::size_t builds = UtilitySubdivRefinement::stats().refinerBuilds;
    const UtilitySubdivRefinementPtr again = refine(cube(PxOsdOpenSubdivTokens->catmullClark), 3);

    EXPECT_EQ(first.get(), again.get());
    EXPECT_EQ(builds, UtilitySubdivRefinement::stats().refinerBuilds);

    const UtilitySubdivRefinementPtr otherLevel = refine(cube(PxOsdOpenSubdivTokens->catmullClark), 2);

    EXPECT_NE(first.get(), otherLevel.get());
    EXPECT_EQ(3, first->level());
    EXPECT_EQ(2, otherLevel->level());
}

TEST(NatronUtilitySubdiv, AnEvictedRefinementLivesOnWhileHeld)
{
    const UtilitySubdivRefinementPtr held = refine(cube(PxOsdOpenSubdivTokens->catmullClark), 4);

    ASSERT_TRUE(held != nullptr);
    UtilitySubdivRefinement::setCacheBudget(0);
    EXPECT_LE(UtilitySubdivRefinement::stats().cachedEntries, 1u);

    const std::size_t builds = UtilitySubdivRefinement::stats().refinerBuilds;
    const UtilitySubdivRefinementPtr other = refine(cube(PxOsdOpenSubdivTokens->catmullClark), 1);

    EXPECT_EQ(builds + 1, UtilitySubdivRefinement::stats().refinerBuilds);
    EXPECT_EQ(1u, UtilitySubdivRefinement::stats().cachedEntries);
    UtilitySubdivRefinement::setCacheBudget(std::size_t(1) << 30);

    VtVec3fArray refined;

    held->refine(kCubePoints, &refined);
    EXPECT_EQ(held->refinedPointCount(), refined.size());
}

TEST(NatronUtilitySubdiv, TheEstimateMatchesTheRefinementAndTheBudgetLowersTheLevel)
{
    const PxOsdMeshTopology topology = cube(PxOsdOpenSubdivTokens->catmullClark);

    for (int level = 0; level <= 3; ++level) {
        EXPECT_EQ(cubeTriangles(level), UtilitySubdivRefinement::estimateTriangles(topology, level));
    }
    const UtilitySubdivRefinementPtr refinement = refine(topology, 2);

    ASSERT_TRUE(refinement != nullptr);
    EXPECT_EQ(cubeTriangles(2), refinement->triangles().size());
    EXPECT_EQ(1, UtilitySubdivRefinement::fitLevel(topology, 3, 100));
    EXPECT_EQ(3, UtilitySubdivRefinement::fitLevel(topology, 3, cubeTriangles(3)));
    EXPECT_EQ(0, UtilitySubdivRefinement::fitLevel(topology, 3, 10));
}

TEST(NatronUtilitySubdiv, NoneAndLoopOnQuadsAreNotRefined)
{
    std::string error;

    EXPECT_TRUE(UtilitySubdivRefinement::get(cube(PxOsdOpenSubdivTokens->none), 2, std::vector<VtIntArray>(), &error) == nullptr);
    EXPECT_TRUE(UtilitySubdivRefinement::get(cube(PxOsdOpenSubdivTokens->loop), 2, std::vector<VtIntArray>(), &error) == nullptr);
    EXPECT_FALSE(error.empty());
}

TEST(NatronUtilitySubdiv, BilinearKeepsTheCubeFlatWithoutNormals)
{
    const UtilitySubdivRefinementPtr refinement = refine(cube(PxOsdOpenSubdivTokens->bilinear), 2);

    ASSERT_TRUE(refinement != nullptr);
    VtVec3fArray points;
    VtVec3fArray normals;

    refinement->evaluate(kCubePoints, &points, &normals);
    EXPECT_TRUE(normals.empty());
    for (const GfVec3f& p : points) {
        EXPECT_NEAR(1.f, std::max({ std::fabs(p[0]), std::fabs(p[1]), std::fabs(p[2]) }), 1e-5f);
    }
}

// Two quads side by side, x from 0 to 2, whose UV islands meet along x = 1: the left one's u runs
// 0 to 1 and the right one's 5 to 6, with v from 0 to 1 on both.
TEST(NatronUtilitySubdiv, AUvSeamStaysContinuousPerTheInterpolationRule)
{
    const VtIntArray counts = { 4, 4 };
    const VtIntArray indices = { 0, 1, 4, 3, 1, 2, 5, 4 };
    const VtVec3fArray points = { GfVec3f(0, 0, 0), GfVec3f(1, 0, 0), GfVec3f(2, 0, 0), GfVec3f(0, 1, 0), GfVec3f(1, 1, 0), GfVec3f(2, 1, 0) };
    const VtIntArray uvIndices = { 0, 1, 2, 3, 4, 5, 6, 7 };
    const VtVec2fArray uvs = { GfVec2f(0, 0), GfVec2f(1, 0), GfVec2f(1, 1), GfVec2f(0, 1), GfVec2f(5, 0), GfVec2f(6, 0), GfVec2f(6, 1), GfVec2f(5, 1) };

    for (const TfToken& rule : { PxOsdOpenSubdivTokens->all, PxOsdOpenSubdivTokens->cornersPlus1, PxOsdOpenSubdivTokens->none }) {
        SCOPED_TRACE(rule.GetString());
        PxOsdSubdivTags tags;

        tags.SetFaceVaryingInterpolationRule(rule);

        const PxOsdMeshTopology topology(PxOsdOpenSubdivTokens->catmullClark, PxOsdOpenSubdivTokens->rightHanded, counts, indices, VtIntArray(), tags);
        const UtilitySubdivRefinementPtr refinement = refine(topology, 2, { uvIndices });

        ASSERT_TRUE(refinement != nullptr);
        ASSERT_EQ(1, refinement->faceVaryingChannelCount());

        VtVec3fArray refined;
        const VtVec2fArray refinedUvs = refinement->interpolateFaceVarying(0, uvs);
        const VtVec3iArray& corners = refinement->faceVaryingTriangles(0);

        refinement->refine(points, &refined);
        ASSERT_EQ(refinement->refinedFaceVaryingCount(0), refinedUvs.size());
        ASSERT_EQ(refinement->triangles().size(), corners.size());

        // Per seam point, the UV each side gives it.
        std::map<int, GfVec2f> left;
        std::map<int, GfVec2f> right;

        for (std::size_t t = 0; t < corners.size(); ++t) {
            for (int corner = 0; corner < 3; ++corner) {
                const int point = refinement->triangles()[t][corner];

                if (std::fabs(refined[point][0] - 1.f) < 1e-6f) {
                    (refinement->coarseFaces()[t] == 0 ? left : right)[point] = refinedUvs[corners[t][corner]];
                }
            }
        }
        ASSERT_EQ(5u, left.size());
        ASSERT_EQ(left.size(), right.size());

        bool cornersMoved = false;

        for (const auto& entry : left) {
            const GfVec2f& a = entry.second;
            const GfVec2f& b = right[entry.first];

            EXPECT_NEAR(a[1], b[1], 1e-6f);
            EXPECT_NEAR(a[0] - 1.f, 5.f - b[0], 1e-6f);
            if (rule == PxOsdOpenSubdivTokens->none) {
                cornersMoved = cornersMoved || a[0] < 1.f - 1e-3f;
            } else {
                EXPECT_NEAR(1.f, a[0], 1e-6f);
                EXPECT_NEAR(5.f, b[0], 1e-6f);
            }
        }
        if (rule == PxOsdOpenSubdivTokens->none) {
            EXPECT_TRUE(cornersMoved) << "smoothing every UV boundary should pull the island corners in";
        }
    }
}

// The grid refines for seconds. A caller that stops must let go within the bound while the
// refinement goes on, unrepeated, for the caller still waiting on it.
TEST(NatronUtilitySubdiv, AStoppedCallerLeavesASharedRefinementToTheOthers)
{
    typedef std::chrono::steady_clock Clock;
    typedef UtilitySubdivRefinement::Outcome Outcome;
    const int grid = 96;
    const int level = 4;
    const PxOsdMeshTopology topology = gridTopology(grid);
    const std::chrono::milliseconds bound(500);

    UtilitySubdivRefinement::clearCache();

    const UtilitySubdivRefinement::Stats before = UtilitySubdivRefinement::stats();
    std::atomic<bool> stop { false };
    std::atomic<bool> stoppingReturned { false };
    Outcome stoppingOutcome = Outcome::Done;
    UtilitySubdivRefinementPtr stoppingGot;
    Clock::time_point stoppingReturnedAt;
    std::thread stopping([&]() {
        std::string error;

        stoppingGot = UtilitySubdivRefinement::get(topology, level, std::vector<VtIntArray>(), [&]() { return stop.load(); }, &stoppingOutcome, &error);
        stoppingReturnedAt = Clock::now();
        stoppingReturned.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    Outcome waitingOutcome = Outcome::Failed;
    UtilitySubdivRefinementPtr waited;
    std::thread waiting([&]() {
        std::string error;

        waited = UtilitySubdivRefinement::get(topology, level, std::vector<VtIntArray>(), UtilitySubdivRefinement::StopCheck(), &waitingOutcome, &error);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    const bool refining = !stoppingReturned.load();
    const Clock::time_point stoppedAt = Clock::now();

    stop.store(true);
    stopping.join();
    waiting.join();
    ASSERT_TRUE(refining) << "the grid refines too fast to test this";
    EXPECT_EQ(Outcome::Stopped, stoppingOutcome);
    EXPECT_TRUE(stoppingGot == nullptr);
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(stoppingReturnedAt - stoppedAt).count(), bound.count());

    const UtilitySubdivRefinement::Stats after = UtilitySubdivRefinement::stats();

    EXPECT_EQ(Outcome::Done, waitingOutcome);
    ASSERT_TRUE(waited != nullptr);
    EXPECT_EQ(static_cast<std::size_t>(grid) * grid * 2 * static_cast<std::size_t>(std::pow(4., level)), waited->triangles().size());
    EXPECT_EQ(before.refinementsStarted + 1, after.refinementsStarted);
    EXPECT_EQ(before.refinerBuilds + 1, after.refinerBuilds);
    EXPECT_EQ(1u, after.cachedEntries);
}

TEST(NatronUtilitySubdivRender, MeshesRefineToTheSettingsLevel)
{
    SubdivRender render(std::string("#usda 1.0\n") + cubeAt("Cube", ""));

    render.render();
    EXPECT_EQ(cubeTriangles(UtilitySubdivRefinement::kDefaultLevel), render.stat(kUtilityStatTriangleCount));
    render.setSetting(kUtilitySettingSubdivLevel, 0);
    render.render();
    EXPECT_EQ(cubeTriangles(0), render.stat(kUtilityStatTriangleCount));
    render.setSetting(kUtilitySettingSubdivLevel, 3);
    render.render();
    EXPECT_EQ(cubeTriangles(3), render.stat(kUtilityStatTriangleCount));
}

TEST(NatronUtilitySubdivRender, EditingACagePointReRunsTheStencilsWithoutANewRefiner)
{
    SubdivRender render(std::string("#usda 1.0\n") + cubeAt("Cube", ""));

    render.render();
    const UtilitySubdivRefinement::Stats before = UtilitySubdivRefinement::stats();
    const std::size_t topologyBuilds = render.stat(kUtilityStatTopologyBuilds);
    const std::size_t pointUpdates = render.stat(kUtilityStatPointUpdates);

    render.movePoint("/Cube", 0, GfVec3f(-1.5f, -1.f, 1.f));
    render.render();

    const UtilitySubdivRefinement::Stats after = UtilitySubdivRefinement::stats();

    EXPECT_EQ(before.refinerBuilds, after.refinerBuilds);
    EXPECT_GT(after.evaluations, before.evaluations);
    EXPECT_EQ(topologyBuilds, render.stat(kUtilityStatTopologyBuilds));
    EXPECT_EQ(pointUpdates + 1, render.stat(kUtilityStatPointUpdates));
}

TEST(NatronUtilitySubdivRender, ThePrimvarOverrideBeatsTheSetting)
{
    SubdivRender render(std::string("#usda 1.0\n")
                        + cubeAt("Override", "    int primvars:natron:subdivLevel = 1 (interpolation = \"constant\")\n")
                        + cubeAt("Setting", ""));

    render.setSetting(kUtilitySettingSubdivLevel, 3);
    render.render();
    EXPECT_EQ(cubeTriangles(1) + cubeTriangles(3), render.stat(kUtilityStatTriangleCount));
}

TEST(NatronUtilitySubdivRender, SchemeNoneIgnoresALevelOverride)
{
    SubdivRender render(std::string("#usda 1.0\n")
                        + cubeAt("Cube", "    uniform token subdivisionScheme = \"none\"\n"
                                         "    int primvars:natron:subdivLevel = 3 (interpolation = \"constant\")\n"));

    render.render();
    EXPECT_EQ(cubeTriangles(0), render.stat(kUtilityStatTriangleCount));
}

TEST(NatronUtilitySubdivRender, TheTriangleBudgetLowersTheLevelAndWarns)
{
    SubdivRender render(std::string("#usda 1.0\n") + cubeAt("Cube", ""));

    render.setSetting(kUtilitySettingSubdivLevel, 3);
    render.setSetting(kUtilitySettingMaxSubdivTriangles, 100);
    render.render();
    EXPECT_EQ(cubeTriangles(1), render.stat(kUtilityStatTriangleCount));

    const std::string warning = render.warning();

    EXPECT_NE(std::string::npos, warning.find("/Cube")) << warning;
    EXPECT_NE(std::string::npos, warning.find("lowered")) << warning;
}

TEST(NatronUtilitySubdivRender, TopologyOpenSubdivRejectsRendersTheCageAndWarns)
{
    SubdivRender render(std::string("#usda 1.0\n") + cubeAt("Cube", "    uniform token subdivisionScheme = \"loop\"\n"));

    render.render();
    EXPECT_EQ(cubeTriangles(0), render.stat(kUtilityStatTriangleCount));
    EXPECT_NE(std::string::npos, render.warning().find("/Cube")) << render.warning();
}

// Every pixel whose centre the render calls covered, or not, against the analytic sphere the cage
// samples must sit within half a pixel of that sphere's outline.
TEST(NatronUtilitySubdivRender, ACatmullClarkSphereCageMatchesTheSphereSilhouetteAtLevelThree)
{
    const int size = 128;
    const double radius = 8.;
    SubdivRender render("#usda 1.0\n", size);

    defineSphereCage(render.stage, radius, 64, 48);
    render.setSetting(kUtilitySettingSubdivLevel, 3);
    render.render();
    ASSERT_GT(render.stat(kUtilityStatTriangleCount), 100000u);

    const double halfHeight = std::tan(kFieldOfView * M_PI / 360.);
    const double outline = std::tan(std::asin(radius / kCameraZ)) / halfHeight * size / 2.;
    double worst = 0.;
    int covered = 0;

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const double dx = x + 0.5 - size / 2.;
            const double dy = y + 0.5 - size / 2.;
            const double fromCentre = std::sqrt(dx * dx + dy * dy);
            const bool inside = fromCentre < outline;
            const bool hit = render.alphaAt(x, y) > 0.5f;

            covered += hit;
            if (hit != inside) {
                worst = std::max(worst, std::fabs(fromCentre - outline));
            }
        }
    }
    EXPECT_GT(covered, 0);
    EXPECT_LT(worst, 0.5);
}

// The grid refines for seconds: a stop well into it must end the render within the bound, cache
// nothing, and leave the mesh to refine in full once the delegate restarts.
TEST(NatronUtilitySubdivRender, AStopDuringARefinementReturnsWithinTheBoundAndCachesNothing)
{
    typedef std::chrono::steady_clock Clock;
    const int grid = 96;
    const int level = 4;
    const std::chrono::milliseconds stopAfter(100);
    const std::chrono::milliseconds bound(500);

    UtilitySubdivRefinement::clearCache();

    SubdivRender render("#usda 1.0\n");

    defineGridCage(render.stage, grid);
    render.setSetting(kUtilitySettingSubdivLevel, level);

    const std::size_t buildsBefore = UtilitySubdivRefinement::stats().refinerBuilds;
    std::atomic<bool> returned { false };
    Clock::time_point returnedAt;
    std::thread thread([&]() {
        render.renderOnce();
        returnedAt = Clock::now();
        returned.store(true);
    });

    std::this_thread::sleep_for(stopAfter);

    const bool refining = !returned.load();
    const Clock::time_point stoppedAt = Clock::now();
    const bool stopped = render.stop();

    thread.join();
    ASSERT_TRUE(refining) << "the grid refines too fast to test this";
    EXPECT_TRUE(stopped);

    const long took = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(returnedAt - stoppedAt).count());

    std::cout << "[   NOTE   ] the render returned " << took << " ms after the stop" << std::endl;
    EXPECT_LT(took, static_cast<long>(bound.count()));
    EXPECT_FALSE(render.isConverged());

    const UtilitySubdivRefinement::Stats afterStop = UtilitySubdivRefinement::stats();

    EXPECT_EQ(0u, afterStop.cachedEntries);
    EXPECT_EQ(buildsBefore, afterStop.refinerBuilds);

    render.restart();
    render.render();
    EXPECT_EQ(1u, UtilitySubdivRefinement::stats().cachedEntries);
    EXPECT_EQ(static_cast<std::size_t>(grid) * grid * 2 * static_cast<std::size_t>(std::pow(4., level)), render.stat(kUtilityStatTriangleCount));
}
