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

#include "Global/Macros.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Geo/GeoUVPack.h"

NATRON_NAMESPACE_USING

namespace {
GeoUVPackIsland
square(double x,
       double y,
       double size)
{
    GeoUVPackIsland island;
    island.polygons = { { { x, y }, { x + size, y }, { x + size, y + size }, { x, y + size } } };

    return island;
}

GeoUVPackIsland
rect(double x0,
     double y0,
     double x1,
     double y1)
{
    GeoUVPackIsland island;
    island.polygons = { { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } } };

    return island;
}

struct Box {
    double x0, y0, x1, y1;
};

Box
placedBox(const GeoUVPackIsland& island,
          const GeoUVPackPlacement& placement)
{
    Box box = { 1e300, 1e300, -1e300, -1e300 };
    for (const std::vector<GeoUV>& poly : island.polygons) {
        for (const GeoUV& corner : poly) {
            const GeoUV p = geoUVPackApply(placement, corner);
            box.x0 = std::min(box.x0, p[0]);
            box.y0 = std::min(box.y0, p[1]);
            box.x1 = std::max(box.x1, p[0]);
            box.y1 = std::max(box.y1, p[1]);
        }
    }

    return box;
}

// The gap between two boxes along the axis that separates them most; negative when they overlap.
double
separation(const Box& a,
           const Box& b)
{
    return std::max(std::max(b.x0 - a.x1, a.x0 - b.x1), std::max(b.y0 - a.y1, a.y0 - b.y1));
}

bool
inside(const Box& box,
       double x0,
       double y0,
       double x1,
       double y1)
{
    const double eps = 1e-9;

    return box.x0 >= x0 - eps && box.y0 >= y0 - eps && box.x1 <= x1 + eps && box.y1 <= y1 + eps;
}

// A bar of three cells with one more on top at the left: no turn of it is its mirror image.
GeoUVPackIsland
lShape()
{
    GeoUVPackIsland island;
    island.polygons = {
        { { 0., 0. }, { 3., 0. }, { 3., 1. }, { 0., 1. } },
        { { 0., 1. }, { 1., 1. }, { 1., 2. }, { 0., 2. } },
    };

    return island;
}

GeoUVPackIsland
mapped(const GeoUVPackIsland& island,
       double m0,
       double m1,
       double m2,
       double m3,
       double tx,
       double ty)
{
    GeoUVPackIsland out = island;
    for (std::vector<GeoUV>& poly : out.polygons) {
        for (GeoUV& c : poly) {
            c = { m0 * c[0] + m1 * c[1] + tx, m2 * c[0] + m3 * c[1] + ty };
        }
    }

    return out;
}

GeoUVPackResult
pack(const std::vector<GeoUVPackIsland>& islands,
     const GeoUVPackParams& params)
{
    GeoUVPackResult result;
    std::string error;
    EXPECT_TRUE(geoPackUVIslands(islands, params, &result, &error)) << error;

    return result;
}
} // namespace

TEST(GeoUVPack, NoTwoIslandsComeWithinThePadding)
{
    const double sizes[] = { 0.3, 0.25, 0.2, 0.2, 0.15, 0.15, 0.1, 0.1, 0.1, 0.05, 0.3, 0.12 };
    std::vector<GeoUVPackIsland> islands;
    for (std::size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        islands.push_back(square(2. * i, 0.5 * i, sizes[i]));
    }
    GeoUVPackParams params;
    params.resolution = 128;
    params.padding = 3;
    params.iterations = 2;
    params.scaleTolerance = 0.01;
    const GeoUVPackResult result = pack(islands, params);
    ASSERT_EQ(result.placements.size(), islands.size());
    EXPECT_EQ(result.nonpacked, 0);
    for (std::size_t i = 0; i < islands.size(); ++i) {
        EXPECT_TRUE(result.placements[i].packed) << i;
        EXPECT_TRUE(inside(placedBox(islands[i], result.placements[i]), 0., 0., 1., 1.)) << i;
        for (std::size_t j = 0; j < i; ++j) {
            EXPECT_GE(separation(placedBox(islands[i], result.placements[i]), placedBox(islands[j], result.placements[j])),
                      3. / 128. - 1e-9)
                << i << " " << j;
        }
    }
}

TEST(GeoUVPack, LargestScaleFillsTheUnitSquare)
{
    // Four unit squares fill the unit square at a scale just under a half: at exactly a half
    // each covers 129 of the 256 cells across.
    const std::vector<GeoUVPackIsland> islands = { square(0., 0., 1.), square(3., 0., 1.), square(6., 0., 1.), square(9., 0., 1.) };
    GeoUVPackParams params;
    params.resolution = 256;
    params.padding = 0;
    params.rotationDivisions = 1;
    params.scaleTolerance = 0.001;
    const GeoUVPackResult result = pack(islands, params);
    EXPECT_GT(result.scale, 0.49);
    EXPECT_LT(result.scale, 0.5);
    EXPECT_EQ(result.nonpacked, 0);
}

TEST(GeoUVPack, FixedScaleLinesUpTheIslandsThatDoNotFit)
{
    const std::vector<GeoUVPackIsland> islands = { square(0., 0., 0.6), square(2., 0., 0.6), square(4., 0., 0.6) };
    GeoUVPackParams params;
    params.scaleMode = eGeoUVPackScaleFixed;
    params.fixedScale = 1.;
    params.resolution = 64;
    const GeoUVPackResult result = pack(islands, params);
    EXPECT_EQ(result.nonpacked, 2);
    EXPECT_TRUE(result.placements[0].packed);
    EXPECT_TRUE(inside(placedBox(islands[0], result.placements[0]), 0., 0., 1., 1.));
    for (int i = 1; i < 3; ++i) {
        EXPECT_FALSE(result.placements[i].packed);
        EXPECT_EQ(result.placements[i].target, -1);
        EXPECT_GT(placedBox(islands[i], result.placements[i]).x0, 1.);
    }
    EXPECT_GT(separation(placedBox(islands[1], result.placements[1]), placedBox(islands[2], result.placements[2])), 0.);
}

TEST(GeoUVPack, UDIMFillsTilesFromTheStartingUDIM)
{
    // At 64 cells a tile, a 0.4 square covers 26 and its padding one more, so a tile holds four.
    std::vector<GeoUVPackIsland> islands;
    for (int i = 0; i < 10; ++i) {
        islands.push_back(square(i, 0., 0.4));
    }
    GeoUVPackParams params;
    params.targetType = eGeoUVPackTargetUDIM;
    params.scaleMode = eGeoUVPackScaleFixed;
    params.startingUdim = 1005;
    params.resolution = 64;
    params.rotationDivisions = 1;
    const GeoUVPackResult result = pack(islands, params);
    EXPECT_EQ(result.nonpacked, 0);
    std::map<int, int> perTile;
    for (std::size_t i = 0; i < islands.size(); ++i) {
        const GeoUVPackPlacement& placement = result.placements[i];
        ++perTile[placement.target];
        const int col = (placement.target - 1001) % 10;
        const int row = (placement.target - 1001) / 10;
        EXPECT_TRUE(inside(placedBox(islands[i], placement), col, row, col + 1, row + 1)) << i;
    }
    EXPECT_EQ(perTile, (std::map<int, int> { { 1005, 4 }, { 1006, 4 }, { 1007, 2 } }));
    EXPECT_EQ(result.maxUdim, 1007);
}

TEST(GeoUVPack, AnIslandTargetGoesToThatTile)
{
    std::vector<GeoUVPackIsland> islands = { square(0., 0., 1.), square(2., 0., 1.) };
    islands[0].target = 1012;
    GeoUVPackParams params;
    params.targetType = eGeoUVPackTargetUDIM;
    params.resolution = 32;
    params.scaleTolerance = 0.02;
    const GeoUVPackResult result = pack(islands, params);
    EXPECT_EQ(result.placements[0].target, 1012);
    EXPECT_TRUE(inside(placedBox(islands[0], result.placements[0]), 1., 1., 2., 2.));
    EXPECT_EQ(result.placements[1].target, 1001);
    EXPECT_TRUE(inside(placedBox(islands[1], result.placements[1]), 0., 0., 1., 1.));
    EXPECT_EQ(result.maxUdim, 1012);
}

TEST(GeoUVPack, TheSameSeedGivesTheSameLayout)
{
    std::vector<GeoUVPackIsland> islands;
    for (int i = 0; i < 6; ++i) {
        GeoUVPackIsland triangle;
        triangle.polygons = { { { 0., 0. }, { 1. + 0.3 * i, 0. }, { 0.2 * i, 0.5 + 0.1 * i } } };
        islands.push_back(triangle);
        islands.push_back(mapped(lShape(), 0.2 + 0.05 * i, 0., 0., 0.2 + 0.05 * i, 3. * i, 5.));
    }
    GeoUVPackParams params;
    params.iterations = 6;
    params.seed = 11;
    params.resolution = 64;
    params.scaleTolerance = 0.02;
    const GeoUVPackResult a = pack(islands, params);
    const GeoUVPackResult b = pack(islands, params);
    EXPECT_EQ(a.scale, b.scale);
    ASSERT_EQ(a.placements.size(), b.placements.size());
    for (std::size_t i = 0; i < a.placements.size(); ++i) {
        EXPECT_EQ(a.placements[i].m, b.placements[i].m) << i;
        EXPECT_EQ(a.placements[i].t, b.placements[i].t) << i;
        EXPECT_EQ(a.placements[i].target, b.placements[i].target) << i;
    }
}

TEST(GeoUVPack, AnotherSeedCanGiveAnotherLayout)
{
    std::vector<GeoUVPackIsland> islands;
    for (int i = 0; i < 6; ++i) {
        GeoUVPackIsland triangle;
        triangle.polygons = { { { 0., 0. }, { 1. + 0.3 * i, 0. }, { 0.2 * i, 0.5 + 0.1 * i } } };
        islands.push_back(triangle);
        islands.push_back(mapped(lShape(), 0.2 + 0.05 * i, 0., 0., 0.2 + 0.05 * i, 3. * i, 5.));
    }
    GeoUVPackParams params;
    params.iterations = 6;
    params.seed = 11;
    params.resolution = 64;
    params.scaleTolerance = 0.02;
    const GeoUVPackResult reference = pack(islands, params);
    bool differs = false;
    for (std::uint32_t seed = 1; (seed <= 32) && !differs; ++seed) {
        if (seed == 11) {
            continue;
        }
        params.seed = seed;
        const GeoUVPackResult other = pack(islands, params);
        differs = other.scale != reference.scale;
        for (std::size_t i = 0; (i < other.placements.size()) && !differs; ++i) {
            differs = (other.placements[i].m != reference.placements[i].m) || (other.placements[i].t != reference.placements[i].t);
        }
    }
    EXPECT_TRUE(differs);
}

TEST(GeoUVPack, StackingNeedsTheSameScale)
{
    std::vector<GeoUVPackIsland> islands = { lShape(), mapped(lShape(), 1., 0., 0., 1., 5., 0.), mapped(lShape(), 1., 0., 0., 1., 10., 0.) };
    islands[1].scale = 2.;
    GeoUVPackParams params;
    params.resolution = 64;
    params.scaleTolerance = 0.02;
    params.stackIdentical = true;
    const GeoUVPackResult result = pack(islands, params);
    EXPECT_EQ(result.placements[0].stackedOn, -1);
    EXPECT_EQ(result.placements[1].stackedOn, -1);
    EXPECT_EQ(result.placements[2].stackedOn, 0);
    const Box small = placedBox(islands[0], result.placements[0]);
    const Box large = placedBox(islands[1], result.placements[1]);
    EXPECT_NEAR((large.x1 - large.x0) * (large.y1 - large.y0), 4. * (small.x1 - small.x0) * (small.y1 - small.y0), 1e-9);
    EXPECT_GE(separation(small, large), 1. / 64. - 1e-9);
}

TEST(GeoUVPack, NoScaleFittingEverythingKeepsAUsableScale)
{
    std::vector<GeoUVPackIsland> islands = { square(0., 0., 1.), square(3., 3., 0.5) };
    islands[0].obstacle = true;
    GeoUVPackParams params;
    params.resolution = 32;
    params.avoidObstacles = true;
    const GeoUVPackResult result = pack(islands, params);
    EXPECT_FALSE(result.placements[1].packed);
    EXPECT_EQ(result.nonpacked, 1);
    EXPECT_GT(result.scale, 0.1);
}

TEST(GeoUVPack, StackingPutsIdenticalAndMirroredIslandsOnTheFirst)
{
    const GeoUVPackIsland first = lShape();
    const std::vector<GeoUVPackIsland> islands = {
        first,
        mapped(first, 0., -1., 1., 0., 5., 0.), // turned a quarter
        mapped(first, -1., 0., 0., 1., 10., 0.), // mirrored
    };
    GeoUVPackParams params;
    params.resolution = 64;
    params.scaleTolerance = 0.02;
    params.stackIdentical = true;
    params.stackMirrored = true;
    const GeoUVPackResult result = pack(islands, params);
    EXPECT_EQ(result.placements[0].stackedOn, -1);
    EXPECT_EQ(result.placements[1].stackedOn, 0);
    EXPECT_EQ(result.placements[2].stackedOn, 0);
    for (int i = 1; i < 3; ++i) {
        for (std::size_t p = 0; p < first.polygons.size(); ++p) {
            for (std::size_t c = 0; c < first.polygons[p].size(); ++c) {
                const GeoUV want = geoUVPackApply(result.placements[0], first.polygons[p][c]);
                const GeoUV got = geoUVPackApply(result.placements[i], islands[i].polygons[p][c]);
                EXPECT_NEAR(got[0], want[0], 1e-9) << i;
                EXPECT_NEAR(got[1], want[1], 1e-9) << i;
            }
        }
    }
    const std::array<double, 4>& m = result.placements[2].m;
    EXPECT_LT(m[0] * m[3] - m[1] * m[2], 0.);

    params.stackMirrored = false;
    const GeoUVPackResult unmirrored = pack(islands, params);
    EXPECT_EQ(unmirrored.placements[1].stackedOn, 0);
    EXPECT_EQ(unmirrored.placements[2].stackedOn, -1);
}

TEST(GeoUVPack, AvoidedObstaclesStayPutAndAreKeptClearOf)
{
    std::vector<GeoUVPackIsland> islands = { square(0., 0., 0.5), square(3., 3., 0.4) };
    islands[0].obstacle = true;
    GeoUVPackParams params;
    params.scaleMode = eGeoUVPackScaleFixed;
    params.resolution = 64;
    params.avoidObstacles = true;
    const GeoUVPackResult result = pack(islands, params);
    EXPECT_EQ(result.placements[0].m, (std::array<double, 4> { 1., 0., 0., 1. }));
    EXPECT_EQ(result.placements[0].t, (GeoUV { 0., 0. }));
    EXPECT_TRUE(result.placements[1].packed);
    EXPECT_GE(separation(placedBox(islands[0], result.placements[0]), placedBox(islands[1], result.placements[1])), 1. / 64. - 1e-9);
}

TEST(GeoUVPack, RotationLetsALongIslandLieAlongTheTarget)
{
    const std::vector<GeoUVPackIsland> islands = { rect(0., 0., 0.1, 0.9) };
    GeoUVPackParams params;
    params.scaleMode = eGeoUVPackScaleFixed;
    params.resolution = 64;
    params.rectangles = { GeoUVPackRect { 0., 0., 1., 0.3 } };
    const GeoUVPackResult turned = pack(islands, params);
    EXPECT_TRUE(turned.placements[0].packed);
    EXPECT_EQ(std::abs(turned.placements[0].m[1]), 1.);
    EXPECT_TRUE(inside(placedBox(islands[0], turned.placements[0]), 0., 0., 1., 0.3));

    params.rotationDivisions = 1;
    EXPECT_EQ(pack(islands, params).nonpacked, 1);
}

TEST(GeoUVPack, CavitiesTakeIslandsInsideTheHoles)
{
    // A frame with a hole from 0.3 to 0.68, and a square that only fits in the hole.
    GeoUVPackIsland frame;
    frame.polygons = {
        { { 0., 0. }, { 0.98, 0. }, { 0.98, 0.3 }, { 0., 0.3 } },
        { { 0., 0.68 }, { 0.98, 0.68 }, { 0.98, 0.98 }, { 0., 0.98 } },
        { { 0., 0.3 }, { 0.3, 0.3 }, { 0.3, 0.68 }, { 0., 0.68 } },
        { { 0.68, 0.3 }, { 0.98, 0.3 }, { 0.98, 0.68 }, { 0.68, 0.68 } },
    };
    const std::vector<GeoUVPackIsland> islands = { frame, square(5., 5., 0.2) };
    GeoUVPackParams params;
    params.scaleMode = eGeoUVPackScaleFixed;
    params.resolution = 64;
    params.rotationDivisions = 1;
    const GeoUVPackResult result = pack(islands, params);
    EXPECT_EQ(result.nonpacked, 0);
    EXPECT_TRUE(inside(placedBox(islands[1], result.placements[1]), 0.3, 0.3, 0.68, 0.68));

    params.packInCavities = false;
    const GeoUVPackResult filled = pack(islands, params);
    EXPECT_TRUE(filled.placements[0].packed);
    EXPECT_FALSE(filled.placements[1].packed);
}

TEST(GeoUVPack, TargetIslandsHoldWhatIsPackedIntoThem)
{
    const std::vector<GeoUVPackIsland> islands = { square(0., 0., 0.3) };
    GeoUVPackParams params;
    params.scaleMode = eGeoUVPackScaleFixed;
    params.resolution = 64;
    params.targetType = eGeoUVPackTargetIslands;
    params.targetIslands = { square(2., 0., 1.) };
    const GeoUVPackResult result = pack(islands, params);
    EXPECT_TRUE(result.placements[0].packed);
    EXPECT_EQ(result.placements[0].target, 0);
    EXPECT_TRUE(inside(placedBox(islands[0], result.placements[0]), 2., 0., 3., 1.));
}

TEST(GeoUVPack, CancelReturnsPromptly)
{
    std::mt19937 rng(3);
    std::uniform_real_distribution<double> size(0.01, 0.2);
    std::vector<GeoUVPackIsland> islands;
    for (int i = 0; i < 400; ++i) {
        GeoUVPackIsland triangle;
        const double a = size(rng);
        const double b = size(rng);
        triangle.polygons = { { { 0., 0. }, { a, 0. }, { 0.3 * a, b } } };
        islands.push_back(triangle);
    }
    GeoUVPackParams params;
    params.resolution = 2048;
    params.iterations = 4;
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    params.aborted = [start]() {
        return std::chrono::steady_clock::now() - start > std::chrono::milliseconds(50);
    };
    GeoUVPackResult result;
    std::string error;
    EXPECT_FALSE(geoPackUVIslands(islands, params, &result, &error));
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    EXPECT_TRUE(result.aborted);
    EXPECT_TRUE(error.empty()) << error;
    EXPECT_LT(seconds, 3.);
}

TEST(GeoUVPack, CancelDuringStackingReturnsPromptly)
{
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> size(0.01, 0.2);
    std::vector<GeoUVPackIsland> islands;
    for (int i = 0; i < 20000; ++i) {
        GeoUVPackIsland triangle;
        triangle.polygons = { { { 0., 0. }, { size(rng), 0. }, { 0., size(rng) } } };
        islands.push_back(triangle);
    }
    GeoUVPackParams params;
    params.stackIdentical = true;
    params.stackMirrored = true;
    std::atomic<int> polls { 0 };
    params.aborted = [&polls]() {
        return ++polls >= 2;
    };
    GeoUVPackResult result;
    std::string error;
    EXPECT_FALSE(geoPackUVIslands(islands, params, &result, &error));
    EXPECT_TRUE(result.aborted);
    EXPECT_TRUE(error.empty()) << error;
    // The set-up stops at its second poll, and the one check after it is the last.
    EXPECT_EQ(polls.load(), 3);
}

TEST(GeoUVPack, StackingFindsCopiesAmongIslandsOfOneTopology)
{
    // Each triangle is a tenth larger than the one before, far beyond the shape tolerance.
    std::vector<GeoUVPackIsland> islands;
    for (int i = 0; i < 60; ++i) {
        const double k = 0.05 * std::pow(1.1, i);
        GeoUVPackIsland triangle;
        triangle.polygons = { { { 0., 0. }, { k, 0. }, { 0.3 * k, 0.7 * k } } };
        islands.push_back(triangle);
    }
    for (int i = 0; i < 60; i += 7) {
        islands.push_back(mapped(islands[i], 0., -1., 1., 0., 3., 1.));
        islands.push_back(mapped(islands[i], -1., 0., 0., 1., 5., 2.));
    }
    GeoUVPackParams params;
    params.resolution = 64;
    params.stackIdentical = true;
    params.stackMirrored = true;
    const GeoUVPackResult result = pack(islands, params);
    for (int i = 0; i < 60; ++i) {
        EXPECT_EQ(result.placements[i].stackedOn, -1) << i;
    }
    std::size_t copy = 60;
    for (int i = 0; i < 60; i += 7) {
        EXPECT_EQ(result.placements[copy++].stackedOn, i);
        EXPECT_EQ(result.placements[copy++].stackedOn, i);
    }
}

TEST(GeoUVPack, BadParametersFail)
{
    const std::vector<GeoUVPackIsland> islands = { square(0., 0., 1.) };
    GeoUVPackResult result;
    std::string error;
    GeoUVPackParams params;
    params.resolution = 0;
    EXPECT_FALSE(geoPackUVIslands(islands, params, &result, &error));
    EXPECT_FALSE(error.empty());

    params = GeoUVPackParams();
    params.rectangles.clear();
    error.clear();
    EXPECT_FALSE(geoPackUVIslands(islands, params, &result, &error));
    EXPECT_FALSE(error.empty());
}
