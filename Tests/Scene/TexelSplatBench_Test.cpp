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
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
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
typedef std::chrono::steady_clock Clock;

// 2 * 708 * 708 triangles: about a million.
const int kGridQuads = 708;
const int kStrokeDabs = 200;
// Live painting lays a stroke as it grows; this many updates lay it whole.
const int kUpdates = 20;

double
secondsSince(const Clock::time_point& start)
{
    return std::chrono::duration<double>(Clock::now() - start).count();
}

// [-1, 1]^2 rippled in z and facing +Z, its st [0, 2] x [0, 1], so that it spans tiles 1001 and 1002.
SceneRaster::TriangulatedMesh
rippledGrid(int quads)
{
    SceneRaster::TriangulatedMesh mesh;

    mesh.path = SdfPath("/Grid");
    const int side = quads + 1;
    mesh.worldPoints.reserve((std::size_t)side * side);
    for (int j = 0; j < side; ++j) {
        for (int i = 0; i < side; ++i) {
            const float x = -1.f + 2.f * i / quads;
            const float y = -1.f + 2.f * j / quads;
            mesh.worldPoints.push_back(GfVec3f(x, y, 0.05f * std::sin(9.f * x) * std::cos(7.f * y)));
        }
    }
    const std::size_t triangles = (std::size_t)quads * quads * 2;
    mesh.triangles.reserve(triangles);
    mesh.faceIndices.reserve(triangles);
    mesh.normals.reserve(triangles);
    mesh.hasSt.reserve(triangles);
    mesh.st.reserve(triangles * 3);
    for (int j = 0; j < quads; ++j) {
        for (int i = 0; i < quads; ++i) {
            const int corners[4] = { j * side + i, j * side + i + 1, (j + 1) * side + i + 1, (j + 1) * side + i };
            const int order[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
            for (int t = 0; t < 2; ++t) {
                const GfVec3i triangle(corners[order[t][0]], corners[order[t][1]], corners[order[t][2]]);
                const GfVec3f& a = mesh.worldPoints[(std::size_t)triangle[0]];
                const GfVec3f& b = mesh.worldPoints[(std::size_t)triangle[1]];
                const GfVec3f& c = mesh.worldPoints[(std::size_t)triangle[2]];
                mesh.triangles.push_back(triangle);
                mesh.faceIndices.push_back(j * quads + i);
                mesh.normals.push_back(GfGetNormalized(GfCross(b - a, c - a)));
                mesh.hasSt.push_back(1);
                for (int k = 0; k < 3; ++k) {
                    const GfVec3f& p = mesh.worldPoints[(std::size_t)triangle[k]];
                    mesh.st.push_back(GfVec2f(p[0] + 1.f, (p[1] + 1.f) * 0.5f));
                }
            }
        }
    }

    return mesh;
}

SceneRaster::ExplicitCamera
overheadCamera()
{
    SceneRaster::ExplicitCamera camera;

    camera.cameraToWorld[14] = 5.;
    camera.viewHeight = 2.2;
    camera.aspect = 1.;
    camera.orthographic = true;

    return camera;
}

// A wave across the grid in paint space, its spacing set for kStrokeDabs dabs a tenth of the grid across.
TexelSplat::Stroke
benchStroke(const SceneRaster::ExplicitCamera& camera)
{
    const RectD bounds = USDTexturePaintView::paintSpaceBounds(camera.aspect);
    const double unitsPerWorld = bounds.height() / camera.viewHeight;
    RotoStrokeDabs::StrokePoints line;
    double length = 0.;

    for (int i = 0; i <= 400; ++i) {
        const double t = i / 400.;
        const double wx = -0.85 + 1.7 * t;
        const double wy = 0.5 * std::sin(6. * t);
        Point p;
        p.x = bounds.x1 + bounds.width() * 0.5 + wx * unitsPerWorld;
        p.y = bounds.y1 + bounds.height() * 0.5 + wy * unitsPerWorld;
        if (!line.empty()) {
            length += std::hypot(p.x - line.back().first.x, p.y - line.back().first.y);
        }
        line.push_back(std::make_pair(p, 1.));
    }
    TexelSplat::Stroke stroke;
    stroke.type = eRotoStrokeTypeSolid;
    stroke.points = RotoStrokeDabs::Strokes(1, line);
    stroke.brush.brushSizePixel = 0.2 * unitsPerWorld;
    stroke.brush.brushHardness = 0.5;
    stroke.brush.brushSpacing = length / ((kStrokeDabs - 1) * stroke.brush.brushSizePixel);
    stroke.buildUp = true;
    stroke.opacity = 0.5;
    stroke.color[0] = 1.;

    return stroke;
}

// The first count points of stroke's line, as a pen has laid them so far.
TexelSplat::Stroke
strokePrefix(const TexelSplat::Stroke& stroke,
             std::size_t count)
{
    TexelSplat::Stroke prefix = stroke;
    RotoStrokeDabs::StrokePoints& line = prefix.points.front();

    line.resize(std::min(count, line.size()));

    return prefix;
}

TexelPages::BaseFill
greyBase()
{
    return [](int, int, const RectI& texels, float* rgba, std::string*) {
        std::fill_n(rgba, (std::size_t)texels.width() * (std::size_t)texels.height() * 4, 0.5f);

        return true;
    };
}

struct PaintRun {
    std::size_t dabs = 0;
    double seconds = 0.;
    std::size_t peakResidentPages = 0;
    std::size_t peakPages = 0;
    std::size_t spilledPages = 0;
};

void
paintLive(const PaintMeshIndex& index,
          const TexelSplat::Coverage& coverage,
          const SceneRaster::ExplicitCamera& camera,
          const TexelSplat::Stroke& stroke,
          int tileSize,
          int threads,
          PaintRun* run)
{
    TexelPages pages(tileSize, greyBase());
    TexelSplat::Context context;

    context.index = &index;
    context.coverage = &coverage;
    context.camera = camera;
    context.pages = &pages;
    context.threads = threads;
    TexelSplat::LiveStroke live;
    const std::size_t points = stroke.points.front().size();
    const Clock::time_point start = Clock::now();
    for (int update = 1; update <= kUpdates; ++update) {
        std::string error;
        const bool painted = live.paint(context, strokePrefix(stroke, (points * (std::size_t)update + kUpdates - 1) / kUpdates), &error);
        ASSERT_TRUE(painted) << error;
        run->peakResidentPages = std::max(run->peakResidentPages, pages.residentPageCount());
        run->peakPages = std::max(run->peakPages, pages.pageCount());
    }
    run->seconds = secondsSince(start);
    run->dabs = live.dabCount();
    run->spilledPages = pages.spilledPageCount();
    EXPECT_EQ(0, live.restarts());
}
} // anon namespace

// Throughput and memory across the hardware range; prints its numbers and never fails on timing.
TEST(TexelSplatBench, DISABLED_AStrokeOverAMillionTriangles)
{
    Clock::time_point start = Clock::now();
    std::vector<SceneRaster::TriangulatedMesh> targets(1, rippledGrid(kGridQuads));
    const std::size_t triangles = targets.front().triangles.size();
    const double meshSeconds = secondsSince(start);

    PaintMeshIndex index;
    start = Clock::now();
    std::string error;
    const bool built = index.build(std::move(targets), std::vector<SceneRaster::TriangulatedMesh>(), 0, &error);
    const double indexSeconds = secondsSince(start);
    ASSERT_TRUE(built) << error;
    std::printf("[ bench    ] %zu triangles: mesh %.3f s, index %.3f s\n", triangles, meshSeconds, indexSeconds);

    const SceneRaster::ExplicitCamera camera = overheadCamera();
    const TexelSplat::Stroke stroke = benchStroke(camera);
    const unsigned int cores = std::max(1u, std::thread::hardware_concurrency());
    const int resolutions[2] = { 4096, 8192 };
    for (int tileSize : resolutions) {
        start = Clock::now();
        const TexelSplat::Coverage coverage(index, tileSize);
        const double coverageSeconds = secondsSince(start);
        std::printf("[ bench    ] %dx%d tiles: coverage %.3f s\n", tileSize, tileSize, coverageSeconds);
        const int threads[2] = { 1, (int)cores };
        for (int threadCount : threads) {
            PaintRun run;
            paintLive(index, coverage, camera, stroke, tileSize, threadCount, &run);
            const double peakMiB = (double)(run.peakResidentPages * TexelPages::kPageBytes) / (1024. * 1024.);
            std::printf("[ bench    ] %dx%d, %s: %zu dabs in %.3f s, %.1f dabs/s; peak %zu pages (%zu resident, %.1f MiB), %zu spilled\n",
                        tileSize, tileSize, (std::to_string(threadCount) + ((threadCount == 1) ? " thread" : " threads")).c_str(),
                        run.dabs, run.seconds, (run.seconds > 0.) ? run.dabs / run.seconds : 0.,
                        run.peakPages, run.peakResidentPages, peakMiB, run.spilledPages);
            EXPECT_NEAR((double)kStrokeDabs, (double)run.dabs, 2.);
        }
    }
}
