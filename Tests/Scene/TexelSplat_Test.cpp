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
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Scene/PaintMeshIndex.h"
#include "Engine/Nodes/Scene/SceneRaster.h"
#include "Engine/Nodes/Scene/TexelPages.h"
#include "Engine/Nodes/Scene/TexelSplat.h"
#include "Engine/Nodes/Scene/USDTexturePaintView.h"
#include "Engine/RotoStrokeDabs.h"

NATRON_NAMESPACE_USING
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
// The seam cube's tiles are this size, so that its faces' st edges fall on texel edges: a face is
// 64 texels wide and 2 world units, the front face spans columns and rows 80 to 143, the right face
// columns 176 to 239, and the top face (tile 1002) columns 80 to 143 and rows 80 to 143.
const int kCubeTile = 320;
const int kQuadTile = 256;

void
addQuad(SceneRaster::TriangulatedMesh* mesh,
        const GfVec3f (&p)[4],
        const GfVec2f (&st)[4])
{
    const int base = (int)mesh->worldPoints.size();
    const GfVec3f normal = GfGetNormalized(GfCross(p[1] - p[0], p[2] - p[0]));
    const int face = mesh->faceIndices.empty() ? 0 : mesh->faceIndices.back() + 1;

    for (int k = 0; k < 4; ++k) {
        mesh->worldPoints.push_back(p[k]);
    }
    const int order[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
    for (int t = 0; t < 2; ++t) {
        mesh->triangles.push_back(GfVec3i(base + order[t][0], base + order[t][1], base + order[t][2]));
        mesh->faceIndices.push_back(face);
        mesh->normals.push_back(normal);
        mesh->hasSt.push_back(1);
        for (int k = 0; k < 3; ++k) {
            mesh->st.push_back(st[order[t][k]]);
        }
    }
}

// The square [-1, 1]^2 at z = 0 facing +Z, its st [s0, s1]^2.
SceneRaster::TriangulatedMesh
frontQuad(float s0 = 0.f,
          float s1 = 1.f)
{
    SceneRaster::TriangulatedMesh mesh;

    mesh.path = SdfPath("/Quad");
    const GfVec3f p[4] = { GfVec3f(-1, -1, 0), GfVec3f(1, -1, 0), GfVec3f(1, 1, 0), GfVec3f(-1, 1, 0) };
    const GfVec2f st[4] = { GfVec2f(s0, s0), GfVec2f(s1, s0), GfVec2f(s1, s1), GfVec2f(s0, s1) };
    addQuad(&mesh, p, st);

    return mesh;
}

// frontQuad() turned by degrees about Y, its +X edge away from a camera on +Z; st [0, 1]^2.
SceneRaster::TriangulatedMesh
turnedQuad(double degrees)
{
    SceneRaster::TriangulatedMesh mesh;

    mesh.path = SdfPath("/Quad");
    const float c = (float)std::cos(degrees * M_PI / 180.);
    const float s = (float)std::sin(degrees * M_PI / 180.);
    const GfVec3f p[4] = { GfVec3f(-c, -1, s), GfVec3f(c, -1, -s), GfVec3f(c, 1, -s), GfVec3f(-c, 1, s) };
    const GfVec2f st[4] = { GfVec2f(0, 0), GfVec2f(1, 0), GfVec2f(1, 1), GfVec2f(0, 1) };
    addQuad(&mesh, p, st);

    return mesh;
}

void
buildIndex(PaintMeshIndex* index,
           std::vector<SceneRaster::TriangulatedMesh> targets)
{
    std::string error;
    const bool built = index->build(std::move(targets), std::vector<SceneRaster::TriangulatedMesh>(), 1, &error);

    ASSERT_TRUE(built) << error;
}

void
buildSeamCube(PaintMeshIndex* index)
{
    UsdStageRefPtr stage = UsdStage::Open(NATRON_TESTS_FIXTURES_DIR "/texture-paint-seam-cube.usda");
    ASSERT_TRUE(bool(stage));
    PaintMeshIndex::Key key;
    key.meshes.push_back(SdfPath("/Cube"));
    key.stName = TfToken("st");
    std::string error;
    ASSERT_TRUE(index->buildFromStage(stage, key, 1, &error)) << error;
    ASSERT_EQ(1u, index->targets().size());
}

// Orthographic at z = 5 looking down -Z, viewHeight high and wide: frontQuad() fills a view 2 high.
SceneRaster::ExplicitCamera
orthoCamera(double viewHeight = 2.)
{
    SceneRaster::ExplicitCamera camera;

    camera.cameraToWorld[14] = 5.;
    camera.viewHeight = viewHeight;
    camera.aspect = 1.;
    camera.orthographic = true;

    return camera;
}

// Perspective at eye looking at centre, Y up, 2 * tan(fovY / 2) = 1.
SceneRaster::ExplicitCamera
lookAtCamera(const GfVec3d& eye,
             const GfVec3d& centre)
{
    SceneRaster::ExplicitCamera camera;

    const GfMatrix4d cameraToWorld = GfMatrix4d().SetLookAt(eye, centre, GfVec3d(0., 1., 0.)).GetInverse();
    std::copy(cameraToWorld.data(), cameraToWorld.data() + 16, camera.cameraToWorld);
    camera.viewHeight = 1.;
    camera.aspect = 1.;
    camera.orthographic = false;

    return camera;
}

double
paintSpaceHeight(const SceneRaster::ExplicitCamera& camera)
{
    return USDTexturePaintView::paintSpaceBounds(camera.aspect).height();
}

// Where camera sees p, in paint space.
void
paintSpacePoint(const SceneRaster::ExplicitCamera& camera,
                const GfVec3d& p,
                double* x,
                double* y)
{
    const RectD bounds = USDTexturePaintView::paintSpaceBounds(camera.aspect);
    SceneRaster::CameraArgs args;
    args.explicitCamera = camera;
    args.width = (int)bounds.width();
    args.height = (int)bounds.height();
    GfFrustum frustum;
    std::string error;
    ASSERT_TRUE(SceneRaster::cameraFrustum(UsdStageRefPtr(), args, &frustum, &error)) << error;
    const GfVec4d clip = GfVec4d(p[0], p[1], p[2], 1.) * frustum.ComputeViewMatrix() * frustum.ComputeProjectionMatrix();
    *x = (clip[0] / clip[3] + 1.) * 0.5 * bounds.width();
    *y = (clip[1] / clip[3] + 1.) * 0.5 * bounds.height();
}

// The paint-space size of a brush whose disc is diameter world units across at p.
double
brushSizeAt(const SceneRaster::ExplicitCamera& camera,
            const GfVec3d& p,
            double diameter)
{
    double worldPerUnit = camera.viewHeight / paintSpaceHeight(camera);
    if (!camera.orthographic) {
        const GfVec3d eye(camera.cameraToWorld[12], camera.cameraToWorld[13], camera.cameraToWorld[14]);
        const GfVec3d forward = -GfVec3d(camera.cameraToWorld[8], camera.cameraToWorld[9], camera.cameraToWorld[10]).GetNormalized();
        worldPerUnit *= GfDot(p - eye, forward);
    }

    return diameter / worldPerUnit;
}

RotoStrokeDabs::Strokes
strokeThrough(const std::vector<std::pair<double, double>>& points)
{
    RotoStrokeDabs::StrokePoints line;
    for (const std::pair<double, double>& point : points) {
        Point p;
        p.x = point.first;
        p.y = point.second;
        line.push_back(std::make_pair(p, 1.));
    }

    return RotoStrokeDabs::Strokes(1, line);
}

// A one-dab stroke at world point p, diameter world units across there.
TexelSplat::Stroke
dabAt(RotoStrokeType type,
      const SceneRaster::ExplicitCamera& camera,
      const GfVec3d& p,
      double diameter,
      double hardness,
      double opacity)
{
    TexelSplat::Stroke stroke;
    stroke.type = type;
    double x = 0.;
    double y = 0.;
    paintSpacePoint(camera, p, &x, &y);
    stroke.points = strokeThrough({ { x, y } });
    stroke.brush.brushSizePixel = brushSizeAt(camera, p, diameter);
    stroke.brush.brushHardness = hardness;
    stroke.brush.brushSpacing = 0.1;
    stroke.opacity = opacity;

    return stroke;
}

TexelPages::BaseFill
constantBase(float r,
             float g,
             float b,
             float a)
{
    return [r, g, b, a](int, int, const RectI& texels, float* rgba, std::string*) {
        const std::size_t count = (std::size_t)texels.width() * texels.height();
        for (std::size_t i = 0; i < count; ++i) {
            rgba[i * 4 + 0] = r;
            rgba[i * 4 + 1] = g;
            rgba[i * 4 + 2] = b;
            rgba[i * 4 + 3] = a;
        }

        return true;
    };
}

// A base of inside where texel (x, y) of tile 1001 satisfies isInside, and outside elsewhere.
TexelPages::BaseFill
splitBase(const std::function<bool(int x, int y)>& isInside,
          const std::array<float, 4>& inside,
          const std::array<float, 4>& outside)
{
    return [isInside, inside, outside](int tile, int, const RectI& texels, float* rgba, std::string*) {
        for (int y = texels.y1; y < texels.y2; ++y) {
            for (int x = texels.x1; x < texels.x2; ++x) {
                const std::array<float, 4>& value = ((tile == 1001) && isInside(x, y)) ? inside : outside;
                std::copy(value.begin(), value.end(), rgba + ((std::size_t)(x - texels.x1) + (std::size_t)(y - texels.y1) * texels.width()) * 4);
            }
        }

        return true;
    };
}

TexelSplat::Context
contextFor(const PaintMeshIndex& index,
           const SceneRaster::ExplicitCamera& camera,
           TexelPages* pages)
{
    TexelSplat::Context context;

    context.index = &index;
    context.camera = camera;
    context.pages = pages;
    context.threads = 4;

    return context;
}

void
paintStroke(const TexelSplat::Context& context,
            const TexelSplat::Stroke& stroke)
{
    std::string error;
    const bool painted = TexelSplat::paint(context, stroke, &error);

    ASSERT_TRUE(painted) << error;
}

// The painted texels of one tile; zero where nothing was painted.
struct PaintedTile {
    int size = 0;
    std::vector<float> rgba;
    std::vector<float> touched;

    const float* at(int x,
                    int y) const
    {
        return rgba.data() + ((std::size_t)x + (std::size_t)y * size) * 4;
    }

    float touchedAt(int x,
                    int y) const
    {
        return touched[(std::size_t)x + (std::size_t)y * size];
    }
};

PaintedTile
exportOf(const TexelPages& pages,
         int tile)
{
    PaintedTile out;
    out.size = pages.tileSize();
    out.rgba.assign((std::size_t)out.size * out.size * 4, 0.f);
    out.touched.assign((std::size_t)out.size * out.size, 0.f);
    std::string error;
    EXPECT_TRUE(pages.exportTile(tile, 0, out.rgba.data(), out.touched.data(), &error)) << error;

    return out;
}

// The columns and rows holding touched texels: x1, y1, x2, y2 exclusive; null when none.
RectI
touchedBounds(const PaintedTile& tile)
{
    RectI bounds;
    for (int y = 0; y < tile.size; ++y) {
        for (int x = 0; x < tile.size; ++x) {
            if (tile.touchedAt(x, y) > 0.f) {
                bounds.merge(x, y, x + 1, y + 1);
            }
        }
    }

    return bounds;
}

int
touchedIn(const PaintedTile& tile,
          const RectI& rect)
{
    int count = 0;
    for (int y = rect.y1; y < rect.y2; ++y) {
        for (int x = rect.x1; x < rect.x2; ++x) {
            count += (tile.touchedAt(x, y) > 0.f || tile.at(x, y)[0] != 0.f || tile.at(x, y)[3] != 0.f) ? 1 : 0;
        }
    }

    return count;
}

const RectI kFrontFace(80, 80, 144, 144);
const RectI kRightFace(176, 80, 240, 144);
const RectI kBackFace(240, 80, 304, 144);
const RectI kLeftFace(16, 80, 80, 144);
const RectI kBottomFace(80, 16, 144, 80);
const RectI kTopFace(80, 80, 144, 144);
// Between the islands' facing edges, front's right and right's left, in tile 1001.
const RectI kIslandGap(144, 0, 176, kCubeTile);

// Seen from (5, 5, 5) the cube shows its front, right and top faces.
SceneRaster::ExplicitCamera
cubeCornerCamera()
{
    return lookAtCamera(GfVec3d(5., 5., 5.), GfVec3d(0.));
}
} // anonymous namespace

TEST(TexelSplat, ADabOnTheSeamPaintsBothIslandsAlike)
{
    PaintMeshIndex index;
    buildSeamCube(&index);
    const SceneRaster::ExplicitCamera camera = cubeCornerCamera();
    TexelPages pages(kCubeTile, TexelPages::BaseFill());
    TexelSplat::Stroke stroke = dabAt(eRotoStrokeTypeSolid, camera, GfVec3d(1., 0.6, 1.), 1., 0.5, 1.);
    stroke.color[0] = 1.;
    stroke.color[1] = 0.;
    stroke.color[2] = 0.;
    paintStroke(contextFor(index, camera, &pages), stroke);

    const PaintedTile tile = exportOf(pages, 1001);
    // Front texel (x, y) and right texel (319 - x, y) lie as far from the front/right edge.
    int painted = 0;
    for (int y = kFrontFace.y1; y < kFrontFace.y2; ++y) {
        for (int x = kFrontFace.x1; x < kFrontFace.x2; ++x) {
            const int mirror = kCubeTile - 1 - x;
            ASSERT_NEAR(tile.touchedAt(x, y), tile.touchedAt(mirror, y), 1e-3) << x << ", " << y;
            for (int c = 0; c < 4; ++c) {
                ASSERT_NEAR(tile.at(x, y)[c], tile.at(mirror, y)[c], 1e-3) << x << ", " << y << " channel " << c;
            }
            painted += (tile.touchedAt(x, y) > 0.f) ? 1 : 0;
        }
    }
    EXPECT_LT(100, painted);
    // Row 131 is at y = 0.61, beside the dab's centre.
    EXPECT_GT(tile.touchedAt(143, 131), 0.9f);
    EXPECT_GT(tile.touchedAt(176, 131), 0.9f);
    EXPECT_EQ(0, touchedIn(tile, kIslandGap));
}

TEST(TexelSplat, ADabOnATileEdgePaintsBothTilesAlike)
{
    PaintMeshIndex index;
    buildSeamCube(&index);
    const SceneRaster::ExplicitCamera camera = cubeCornerCamera();
    TexelPages pages(kCubeTile, TexelPages::BaseFill());
    TexelSplat::Stroke stroke = dabAt(eRotoStrokeTypeSolid, camera, GfVec3d(0.6, 1., 1.), 1., 0.5, 0.8);
    paintStroke(contextFor(index, camera, &pages), stroke);

    const PaintedTile front = exportOf(pages, 1001);
    const PaintedTile top = exportOf(pages, 1002);
    // Front texel (x, y) and top texel (x, 223 - y) lie as far from the front/top edge.
    int painted = 0;
    for (int y = kFrontFace.y1; y < kFrontFace.y2; ++y) {
        for (int x = kFrontFace.x1; x < kFrontFace.x2; ++x) {
            const int mirror = 223 - y;
            ASSERT_NEAR(front.touchedAt(x, y), top.touchedAt(x, mirror), 1e-3) << x << ", " << y;
            for (int c = 0; c < 4; ++c) {
                ASSERT_NEAR(front.at(x, y)[c], top.at(x, mirror)[c], 1e-3) << x << ", " << y << " channel " << c;
            }
            painted += (front.touchedAt(x, y) > 0.f) ? 1 : 0;
        }
    }
    EXPECT_LT(100, painted);
    EXPECT_GT(touchedIn(top, kTopFace), 100);
}

TEST(TexelSplat, BackFacesTakeNoPaint)
{
    PaintMeshIndex index;
    buildSeamCube(&index);
    const SceneRaster::ExplicitCamera camera = cubeCornerCamera();
    TexelPages pages(kCubeTile, TexelPages::BaseFill());
    // Wide enough to reach every face of the cube.
    paintStroke(contextFor(index, camera, &pages), dabAt(eRotoStrokeTypeSolid, camera, GfVec3d(1., 0.6, 1.), 8., 1., 1.));

    const PaintedTile tile = exportOf(pages, 1001);
    EXPECT_EQ(kFrontFace.width() * kFrontFace.height(), touchedIn(tile, kFrontFace));
    EXPECT_EQ(kRightFace.width() * kRightFace.height(), touchedIn(tile, kRightFace));
    EXPECT_EQ(kTopFace.width() * kTopFace.height(), touchedIn(exportOf(pages, 1002), kTopFace));
    EXPECT_EQ(0, touchedIn(tile, kBackFace));
    EXPECT_EQ(0, touchedIn(tile, kLeftFace));
    EXPECT_EQ(0, touchedIn(tile, kBottomFace));
    EXPECT_EQ(0, touchedIn(tile, kIslandGap));
}

TEST(TexelSplat, OneThreadAndManyPaintTheSame)
{
    PaintMeshIndex index;
    buildSeamCube(&index);
    const SceneRaster::ExplicitCamera camera = cubeCornerCamera();
    std::vector<std::pair<double, double>> points;
    for (int i = 0; i <= 20; ++i) {
        const double t = i / 20.;
        double x = 0.;
        double y = 0.;
        paintSpacePoint(camera, GfVec3d(0.2 + 0.8 * t, 0.2 + 0.8 * t, 1.) * (1. - t) + GfVec3d(1., 0.5 + 0.5 * t, 0.2) * t, &x, &y);
        points.push_back(std::make_pair(x, y));
    }
    TexelSplat::Stroke stroke;
    stroke.points = strokeThrough(points);
    stroke.brush.brushSizePixel = brushSizeAt(camera, GfVec3d(1.), 0.6);
    stroke.brush.brushHardness = 0.3;
    stroke.brush.brushSpacing = 0.1;
    stroke.buildUp = true;
    stroke.opacity = 0.4;
    stroke.color[1] = 0.25;

    PaintedTile results[2][2];
    const int threads[2] = { 1, 8 };
    for (int run = 0; run < 2; ++run) {
        TexelPages pages(kCubeTile, constantBase(0.1f, 0.2f, 0.3f, 1.f));
        TexelSplat::Context context = contextFor(index, camera, &pages);
        context.threads = threads[run];
        paintStroke(context, stroke);
        results[run][0] = exportOf(pages, 1001);
        results[run][1] = exportOf(pages, 1002);
    }
    for (int t = 0; t < 2; ++t) {
        EXPECT_GT(touchedIn(results[0][t], RectI(0, 0, kCubeTile, kCubeTile)), 100) << "tile " << 1001 + t;
        EXPECT_EQ(0, std::memcmp(results[0][t].rgba.data(), results[1][t].rgba.data(), results[0][t].rgba.size() * sizeof(float))) << "tile " << 1001 + t;
        EXPECT_EQ(0, std::memcmp(results[0][t].touched.data(), results[1][t].touched.data(), results[0][t].touched.size() * sizeof(float))) << "tile " << 1001 + t;
    }
}

TEST(TexelSplat, SolidLaysItsColourAndEraserRestoresTheBase)
{
    PaintMeshIndex index;
    buildIndex(&index, { frontQuad() });
    const SceneRaster::ExplicitCamera camera = orthoCamera();
    TexelPages base(kQuadTile, constantBase(0.5f, 0.5f, 0.5f, 1.f));
    TexelPages pages(kQuadTile, constantBase(0.5f, 0.5f, 0.5f, 1.f));
    TexelSplat::Context context = contextFor(index, camera, &pages);
    context.base = &base;

    TexelSplat::Stroke solid = dabAt(eRotoStrokeTypeSolid, camera, GfVec3d(0.), 1., 1., 0.6);
    solid.color[0] = 1.;
    solid.color[1] = 0.;
    solid.color[2] = 0.;
    paintStroke(context, solid);
    PaintedTile tile = exportOf(pages, 1001);
    const float* texel = tile.at(128, 128);
    EXPECT_NEAR(0.8f, texel[0], 1e-3);
    EXPECT_NEAR(0.2f, texel[1], 1e-3);
    EXPECT_NEAR(0.2f, texel[2], 1e-3);
    EXPECT_NEAR(1.f, texel[3], 1e-3);
    EXPECT_NEAR(0.6f, tile.touchedAt(128, 128), 1e-3);
    // Past the dab's edge, a quarter of the quad from its centre.
    EXPECT_EQ(0.f, tile.touchedAt(128, 200));

    paintStroke(context, dabAt(eRotoStrokeTypeEraser, camera, GfVec3d(0.), 1., 1., 0.5));
    tile = exportOf(pages, 1001);
    texel = tile.at(128, 128);
    EXPECT_NEAR(0.65f, texel[0], 1e-3);
    EXPECT_NEAR(0.35f, texel[1], 1e-3);
    EXPECT_NEAR(0.3f, tile.touchedAt(128, 128), 1e-3);
}

TEST(TexelSplat, DodgeAndBurnFollowRotoPaint)
{
    PaintMeshIndex index;
    buildIndex(&index, { frontQuad() });
    const SceneRaster::ExplicitCamera camera = orthoCamera();
    const float opacity = 0.6f;
    // RotoPaint's colour dodge B / (1 - B) and colour burn 1 - (1 - B) / B, of the texel over itself.
    const struct {
        RotoStrokeType type;
        float grey;
        float made;
    } cases[] = {
        { eRotoStrokeTypeDodge, 0.25f, 0.25f / 0.75f },
        { eRotoStrokeTypeBurn, 0.75f, 1.f - 0.25f / 0.75f },
    };
    for (const auto& brush : cases) {
        TexelPages pages(kQuadTile, constantBase(brush.grey, brush.grey, brush.grey, 1.f));
        paintStroke(contextFor(index, camera, &pages), dabAt(brush.type, camera, GfVec3d(0.), 1., 1., opacity));
        const PaintedTile tile = exportOf(pages, 1001);
        const float expected = brush.grey + (brush.made - brush.grey) * opacity;
        for (int c = 0; c < 3; ++c) {
            EXPECT_NEAR(expected, tile.at(128, 128)[c], 1e-3) << brush.type;
        }
        EXPECT_NEAR(1.f, tile.at(128, 128)[3], 1e-3) << brush.type;
        if (brush.type == eRotoStrokeTypeDodge) {
            EXPECT_GT(tile.at(128, 128)[0], brush.grey + 0.01f);
        } else {
            EXPECT_LT(tile.at(128, 128)[0], brush.grey - 0.01f);
        }
    }
}

TEST(TexelSplat, RevealOverPaintGivesBackTheBase)
{
    PaintMeshIndex index;
    buildIndex(&index, { frontQuad() });
    const SceneRaster::ExplicitCamera camera = orthoCamera();
    TexelPages base(kQuadTile, constantBase(0.f, 0.f, 1.f, 1.f));
    TexelPages pages(kQuadTile, constantBase(0.5f, 0.5f, 0.5f, 1.f));
    TexelSplat::Context context = contextFor(index, camera, &pages);
    context.base = &base;

    TexelSplat::Stroke red = dabAt(eRotoStrokeTypeSolid, camera, GfVec3d(0.), 1., 1., 1.);
    red.color[1] = 0.;
    red.color[2] = 0.;
    paintStroke(context, red);
    EXPECT_NEAR(1.f, exportOf(pages, 1001).at(128, 128)[0], 1e-3);

    paintStroke(context, dabAt(eRotoStrokeTypeReveal, camera, GfVec3d(0.), 0.6, 1., 1.));
    const PaintedTile tile = exportOf(pages, 1001);
    const float* texel = tile.at(128, 128);
    EXPECT_NEAR(0.f, texel[0], 1e-3);
    EXPECT_NEAR(0.f, texel[1], 1e-3);
    EXPECT_NEAR(1.f, texel[2], 1e-3);
    EXPECT_NEAR(1.f, texel[3], 1e-3);
    // Outside the reveal, inside the red dab.
    EXPECT_NEAR(1.f, tile.at(128, 175)[0], 1e-3);
    EXPECT_NEAR(0.f, tile.at(128, 175)[2], 1e-3);
}

TEST(TexelSplat, CloneCopiesASquareToItsOffset)
{
    PaintMeshIndex index;
    buildIndex(&index, { frontQuad() });
    const SceneRaster::ExplicitCamera camera = orthoCamera();
    const std::array<float, 4> red = { 1.f, 0.f, 0.f, 1.f };
    const std::array<float, 4> grey = { 0.5f, 0.5f, 0.5f, 1.f };
    auto inSquare = [](int x, int y) {
        return (x >= 40) && (x < 72) && (y >= 40) && (y < 72);
    };
    TexelPages pages(kQuadTile, splitBase(inSquare, red, grey));

    // 96 texels to the right of the square: each texel copies the one 96 texels to its left.
    const int offset = 96;
    const double unitsPerTexel = USDTexturePaintView::kPaintSpaceSize / (double)kQuadTile;
    TexelSplat::Stroke clone;
    clone.type = eRotoStrokeTypeClone;
    clone.points = strokeThrough({ { (56 + offset) * unitsPerTexel, 56 * unitsPerTexel } });
    clone.brush.brushSizePixel = 60 * unitsPerTexel;
    clone.brush.brushHardness = 1.;
    clone.brush.brushSpacing = 0.1;
    clone.sourceTransform[2] = -offset * unitsPerTexel;
    paintStroke(contextFor(index, camera, &pages), clone);

    const PaintedTile tile = exportOf(pages, 1001);
    int inside = 0;
    for (int y = 28; y < 84; ++y) {
        for (int x = 28 + offset; x < 84 + offset; ++x) {
            if (std::hypot(x + 0.5 - (56 + offset), y + 0.5 - 56) > 28.) {
                continue;
            }
            const std::array<float, 4>& expected = inSquare(x - offset, y) ? red : grey;
            for (int c = 0; c < 4; ++c) {
                ASSERT_NEAR(expected[c], tile.at(x, y)[c], 1e-2) << x << ", " << y << " channel " << c;
            }
            inside += inSquare(x - offset, y) ? 1 : 0;
        }
    }
    EXPECT_EQ(32 * 32, inside);
}

TEST(TexelSplat, CloneFromOffTheMeshLeavesTexelsAlone)
{
    PaintMeshIndex index;
    buildIndex(&index, { frontQuad() });
    const SceneRaster::ExplicitCamera camera = orthoCamera();
    TexelPages pages(kQuadTile, constantBase(0.5f, 0.5f, 0.5f, 1.f));
    TexelSplat::Stroke clone = dabAt(eRotoStrokeTypeClone, camera, GfVec3d(0.), 1., 1., 1.);
    clone.sourceTransform[2] = 10000.;
    paintStroke(contextFor(index, camera, &pages), clone);

    EXPECT_TRUE(pages.paintedPages().empty());
    TexelPages::PageData page;
    std::string error;
    ASSERT_TRUE(pages.read(TexelPages::pageOf(1001, 0, 128, 128), &page, &error)) << error;
    for (float value : page.rgba) {
        ASSERT_TRUE((value == 0.5f) || (value == 1.f));
    }
    for (float value : page.touched) {
        ASSERT_EQ(0.f, value);
    }
}

TEST(TexelSplat, SmearDragsColourAlongTheStroke)
{
    PaintMeshIndex index;
    buildIndex(&index, { frontQuad() });
    const SceneRaster::ExplicitCamera camera = orthoCamera();
    const std::array<float, 4> red = { 1.f, 0.f, 0.f, 1.f };
    const std::array<float, 4> grey = { 0.5f, 0.5f, 0.5f, 1.f };
    TexelPages pages(kQuadTile, splitBase([](int x, int) { return x < kQuadTile / 2; }, red, grey));

    const double size = USDTexturePaintView::kPaintSpaceSize;
    std::vector<std::pair<double, double>> points;
    for (int i = 0; i <= 30; ++i) {
        points.push_back(std::make_pair((0.45 + 0.01 * i) * size, 0.5 * size));
    }
    TexelSplat::Stroke smear;
    smear.type = eRotoStrokeTypeSmear;
    smear.points = strokeThrough(points);
    smear.brush.brushSizePixel = 600.;
    smear.brush.brushHardness = 0.5;
    smear.brush.brushSpacing = 0.1;
    paintStroke(contextFor(index, camera, &pages), smear);

    const PaintedTile tile = exportOf(pages, 1001);
    const int row = kQuadTile / 2;
    double redGain = 0.;
    double greenLoss = 0.;
    for (int x = kQuadTile / 2; x < kQuadTile * 3 / 4; ++x) {
        if (tile.touchedAt(x, row) > 0.f) {
            redGain += tile.at(x, row)[0] - 0.5;
            greenLoss += 0.5 - tile.at(x, row)[1];
        }
    }
    EXPECT_GT(redGain, 0.5);
    EXPECT_GT(greenLoss, 0.5);
    // Behind where the stroke starts, out of the brush's reach.
    EXPECT_EQ(0.f, tile.touchedAt(kQuadTile * 2 / 5, row));
}

namespace {
// The seam cube from (5, 0.5, 5), its front/right edge at (1, 0.5, 1) in the middle of the view; the
// base red on the front face and black on the right.
struct SeamEdge {
    PaintMeshIndex index;
    SceneRaster::ExplicitCamera camera = lookAtCamera(GfVec3d(5., 0.5, 5.), GfVec3d(0., 0.5, 0.));
    std::unique_ptr<TexelPages> pages;

    SeamEdge()
    {
        const std::array<float, 4> red = { 1.f, 0.f, 0.f, 1.f };
        const std::array<float, 4> black = { 0.f, 0.f, 0.f, 1.f };
        pages.reset(new TexelPages(kCubeTile, splitBase([](int x, int) { return x < 160; }, red, black)));
    }

    // A neighbourhood brush over the edge with a blur sigma of 0.1 world units.
    TexelSplat::Stroke edgeStroke(RotoStrokeType type) const
    {
        const GfVec3d edge(1., 0.5, 1.);
        TexelSplat::Stroke stroke = dabAt(type, camera, edge, 1.2, 1., 1.);
        stroke.effectStrength = brushSizeAt(camera, edge, 0.1 * 2.4);

        return stroke;
    }
};

// The row of texels at y = 0.48 on both the front and the right face.
const int kSeamRow = 127;
} // anonymous namespace

TEST(TexelSplat, BlurAcrossTheSeamIsSymmetric)
{
    SeamEdge scene;
    buildSeamCube(&scene.index);
    paintStroke(contextFor(scene.index, scene.camera, scene.pages.get()), scene.edgeStroke(eRotoStrokeTypeBlur));

    const PaintedTile tile = exportOf(*scene.pages, 1001);
    // Front texel x and right texel 319 - x lie as far from the edge, on its red and black sides.
    for (int x = kFrontFace.x2 - 16; x < kFrontFace.x2; ++x) {
        const float front = tile.at(x, kSeamRow)[0];
        const float right = tile.at(kCubeTile - 1 - x, kSeamRow)[0];
        EXPECT_NEAR(1.f, front + right, 2e-2) << x;
    }
    EXPECT_LT(tile.at(kFrontFace.x2 - 1, kSeamRow)[0], 0.9f);
    EXPECT_GT(tile.at(kRightFace.x1, kSeamRow)[0], 0.1f);
    EXPECT_GT(tile.at(kFrontFace.x2 - 1, kSeamRow)[0], tile.at(kRightFace.x1, kSeamRow)[0]);
    EXPECT_GT(tile.at(kFrontFace.x2 - 16, kSeamRow)[0], 0.98f);
}

TEST(TexelSplat, SharpenRaisesTheContrastAcrossTheSeam)
{
    SeamEdge scene;
    buildSeamCube(&scene.index);
    paintStroke(contextFor(scene.index, scene.camera, scene.pages.get()), scene.edgeStroke(eRotoStrokeTypeSharpen));

    const PaintedTile tile = exportOf(*scene.pages, 1001);
    EXPECT_GT(tile.at(kFrontFace.x2 - 1, kSeamRow)[0], 1.01f);
    EXPECT_LT(tile.at(kRightFace.x1, kSeamRow)[0], -0.01f);
}

TEST(TexelSplat, ViewAndSurfaceFootprintsAgreeFacingTheCamera)
{
    PaintMeshIndex index;
    buildIndex(&index, { frontQuad() });
    const SceneRaster::ExplicitCamera camera = orthoCamera();
    RectI bounds[2];
    const TexelSplat::FootprintEnum footprints[2] = { TexelSplat::eFootprintSurface, TexelSplat::eFootprintView };
    for (int f = 0; f < 2; ++f) {
        TexelPages pages(kQuadTile, TexelPages::BaseFill());
        TexelSplat::Context context = contextFor(index, camera, &pages);
        context.footprint = footprints[f];
        paintStroke(context, dabAt(eRotoStrokeTypeSolid, camera, GfVec3d(0.1, -0.2, 0.), 0.6, 1., 1.));
        bounds[f] = touchedBounds(exportOf(pages, 1001));
        ASSERT_FALSE(bounds[f].isNull());
    }
    EXPECT_NEAR(bounds[0].x1, bounds[1].x1, 1);
    EXPECT_NEAR(bounds[0].y1, bounds[1].y1, 1);
    EXPECT_NEAR(bounds[0].x2, bounds[1].x2, 1);
    EXPECT_NEAR(bounds[0].y2, bounds[1].y2, 1);
    // 0.6 world units across is 0.3 of the quad's st.
    EXPECT_NEAR(0.3 * kQuadTile, bounds[0].width(), 2);
}

TEST(TexelSplat, AtAGrazingAngleOnlyTheViewFootprintStretches)
{
    PaintMeshIndex index;
    buildIndex(&index, { turnedQuad(75.) });
    const SceneRaster::ExplicitCamera camera = orthoCamera();
    RectI bounds[2];
    const TexelSplat::FootprintEnum footprints[2] = { TexelSplat::eFootprintSurface, TexelSplat::eFootprintView };
    for (int f = 0; f < 2; ++f) {
        TexelPages pages(kQuadTile, TexelPages::BaseFill());
        TexelSplat::Context context = contextFor(index, camera, &pages);
        context.footprint = footprints[f];
        paintStroke(context, dabAt(eRotoStrokeTypeSolid, camera, GfVec3d(0.), 0.2, 1., 1.));
        bounds[f] = touchedBounds(exportOf(pages, 1001));
        ASSERT_FALSE(bounds[f].isNull());
    }
    const RectI& surface = bounds[0];
    const RectI& view = bounds[1];
    EXPECT_NEAR(surface.width(), surface.height(), 2);
    EXPECT_NEAR(surface.height(), view.height(), 2);
    EXPECT_GT(view.width(), 2 * surface.width());
}

TEST(TexelSplat, PaintAtAnIslandEdgeGrowsATwoTexelGutter)
{
    PaintMeshIndex index;
    // The island is texels 64 to 191 of the tile in each direction.
    buildIndex(&index, { frontQuad(0.25f, 0.75f) });
    const TexelSplat::Coverage coverage(index, kQuadTile);
    const SceneRaster::ExplicitCamera camera = orthoCamera();
    TexelPages pages(kQuadTile, TexelPages::BaseFill());
    TexelSplat::Context context = contextFor(index, camera, &pages);
    context.coverage = &coverage;
    TexelSplat::Stroke stroke = dabAt(eRotoStrokeTypeSolid, camera, GfVec3d(0.9, 0., 0.), 0.6, 1., 1.);
    stroke.color[1] = 0.;
    stroke.color[2] = 0.;
    paintStroke(context, stroke);

    const PaintedTile tile = exportOf(pages, 1001);
    const int row = kQuadTile / 2;
    const int edge = 191;
    EXPECT_NEAR(1.f, tile.touchedAt(edge, row), 1e-3);
    for (int g = 1; g <= TexelPages::kSeamGutter; ++g) {
        EXPECT_NEAR(1.f, tile.touchedAt(edge + g, row), 1e-3) << g;
        EXPECT_NEAR(1.f, tile.at(edge + g, row)[0], 1e-3) << g;
        EXPECT_NEAR(0.f, tile.at(edge + g, row)[1], 1e-3) << g;
    }
    for (int x = edge + TexelPages::kSeamGutter + 1; x < kQuadTile; ++x) {
        EXPECT_EQ(0.f, tile.touchedAt(x, row)) << x;
        EXPECT_EQ(0.f, tile.at(x, row)[3]) << x;
    }

    // Without coverage there is no gutter.
    TexelPages bare(kQuadTile, TexelPages::BaseFill());
    paintStroke(contextFor(index, camera, &bare), stroke);
    EXPECT_EQ(0.f, exportOf(bare, 1001).touchedAt(edge + 1, row));
}

namespace {
// Paint-space points of a stroke across the seam cube's corner, from the front face over the right one.
std::vector<std::pair<double, double>>
cornerStrokePoints(const SceneRaster::ExplicitCamera& camera)
{
    std::vector<std::pair<double, double>> points;
    for (int i = 0; i <= 24; ++i) {
        const double t = i / 24.;
        double x = 0.;
        double y = 0.;
        paintSpacePoint(camera, GfVec3d(0.3 + 0.7 * t, 0.6, 1.) * (1. - t) + GfVec3d(1., 0.6, 1. - 0.7 * t) * t, &x, &y);
        points.push_back(std::make_pair(x, y));
    }

    return points;
}

TexelSplat::Stroke
cornerStroke(RotoStrokeType type,
             const SceneRaster::ExplicitCamera& camera,
             const std::vector<std::pair<double, double>>& points)
{
    TexelSplat::Stroke stroke;
    stroke.type = type;
    stroke.points = strokeThrough(points);
    stroke.brush.brushSizePixel = brushSizeAt(camera, GfVec3d(1.), 0.5);
    stroke.brush.brushHardness = 0.4;
    stroke.brush.brushSpacing = 0.1;
    stroke.buildUp = true;
    stroke.opacity = 0.6;
    stroke.color[1] = 0.2;
    stroke.effectStrength = 6.;
    stroke.sourceTransform[2] = -40.;

    return stroke;
}

TexelPages::BaseFill
stripedBase()
{
    return [](int tile, int, const RectI& texels, float* rgba, std::string*) {
        for (int y = texels.y1; y < texels.y2; ++y) {
            for (int x = texels.x1; x < texels.x2; ++x) {
                float* texel = rgba + ((std::size_t)(x - texels.x1) + (std::size_t)(y - texels.y1) * texels.width()) * 4;
                texel[0] = ((x / 6) % 2) ? 0.9f : 0.1f;
                texel[1] = (tile == 1001) ? 0.3f : 0.6f;
                texel[2] = ((y / 5) % 2) ? 0.7f : 0.2f;
                texel[3] = 1.f;
            }
        }

        return true;
    };
}

void
expectSameTiles(const TexelPages& a,
                const TexelPages& b,
                const std::string& what)
{
    for (int tile : { 1001, 1002 }) {
        const PaintedTile x = exportOf(a, tile);
        const PaintedTile y = exportOf(b, tile);
        EXPECT_EQ(0, std::memcmp(x.rgba.data(), y.rgba.data(), x.rgba.size() * sizeof(float))) << what << ", tile " << tile;
        EXPECT_EQ(0, std::memcmp(x.touched.data(), y.touched.data(), x.touched.size() * sizeof(float))) << what << ", tile " << tile;
    }
    EXPECT_EQ(a.paintedPages().size(), b.paintedPages().size()) << what;
}
} // anonymous namespace

// Each brush, its stroke growing a point at a time, ends as painting the whole stroke at once does.
TEST(TexelSplat, AStrokeLaidAsItGrowsEndsAsTheWholeStroke)
{
    PaintMeshIndex index;
    buildSeamCube(&index);
    const TexelSplat::Coverage coverage(index, kCubeTile);
    const SceneRaster::ExplicitCamera camera = cubeCornerCamera();
    const std::vector<std::pair<double, double>> points = cornerStrokePoints(camera);
    const RotoStrokeType types[] = { eRotoStrokeTypeSolid, eRotoStrokeTypeEraser, eRotoStrokeTypeClone, eRotoStrokeTypeBlur, eRotoStrokeTypeSmear };
    for (RotoStrokeType type : types) {
        const std::string what = "brush " + std::to_string((int)type);
        TexelPages under(kCubeTile, stripedBase());
        TexelPages whole(kCubeTile, stripedBase());
        TexelPages grown(kCubeTile, stripedBase());
        TexelSplat::Context context = contextFor(index, camera, &whole);
        context.coverage = &coverage;
        context.base = &under;
        paintStroke(context, cornerStroke(type, camera, points));

        context.pages = &grown;
        TexelSplat::LiveStroke live;
        for (std::size_t n = 1; n <= points.size(); n += 3) {
            const std::vector<std::pair<double, double>> part(points.begin(), points.begin() + (std::ptrdiff_t)n);
            std::string error;
            ASSERT_TRUE(live.paint(context, cornerStroke(type, camera, part), &error)) << what << ": " << error;
        }
        std::string error;
        ASSERT_TRUE(live.paint(context, cornerStroke(type, camera, points), &error)) << what << ": " << error;
        EXPECT_EQ(0, live.restarts()) << what;
        EXPECT_GT(live.dabCount(), (std::size_t)10) << what;
        EXPECT_FALSE(grown.paintedPages().empty()) << what;
        expectSameTiles(whole, grown, what);
    }
}

// A changed earlier point lays the stroke again from its start, and the journal puts the pages back.
TEST(TexelSplat, ALiveStrokeWhoseStartChangedIsLaidAgain)
{
    PaintMeshIndex index;
    buildSeamCube(&index);
    const TexelSplat::Coverage coverage(index, kCubeTile);
    const SceneRaster::ExplicitCamera camera = cubeCornerCamera();
    std::vector<std::pair<double, double>> points = cornerStrokePoints(camera);

    TexelPages pages(kCubeTile, stripedBase());
    TexelSplat::Context context = contextFor(index, camera, &pages);
    context.coverage = &coverage;
    TexelSplat::LiveStroke live;
    std::string error;
    ASSERT_TRUE(live.paint(context, cornerStroke(eRotoStrokeTypeSolid, camera, points), &error)) << error;
    points.front().first += 30.;
    ASSERT_TRUE(live.paint(context, cornerStroke(eRotoStrokeTypeSolid, camera, points), &error)) << error;
    EXPECT_EQ(1, live.restarts());

    TexelPages whole(kCubeTile, stripedBase());
    TexelSplat::Context wholeContext = context;
    wholeContext.pages = &whole;
    paintStroke(wholeContext, cornerStroke(eRotoStrokeTypeSolid, camera, points));
    expectSameTiles(whole, pages, "laid again");

    const std::vector<TexelPages::Snapshot> journal = live.takeJournal();
    EXPECT_FALSE(journal.empty());
    ASSERT_TRUE(pages.restore(journal, &error)) << error;
    EXPECT_TRUE(pages.paintedPages().empty());
}
