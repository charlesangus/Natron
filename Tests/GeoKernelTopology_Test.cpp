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

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Geo/GeoKernelBinding.h"
#include "Engine/Geo/GeoPointTree.h"
#include "Engine/GeoDetail.h"

NATRON_NAMESPACE_USING

namespace {

constexpr GeoOffset kPage = GeoOffset(kGeoPageSize);

GeoDetail
makeScatter(GeoIndex count, std::uint32_t seed)
{
    GeoDetailBuilder builder;
    const GeoOffset first = builder.appendPoints(count);
    std::uint32_t state = seed;
    const auto next = [&] {
        state = state * 1664525u + 1013904223u;
        return float(state >> 8) / float(1u << 24);
    };
    for (GeoOffset k = 0; k < count; ++k) {
        const float x = next() * 10.f;
        const float y = next() * 10.f;
        const float z = next() * 10.f;
        builder.setPointPosition(first + k, x, y, z);
    }
    return builder.finish();
}

GeoDetail
makeLine(GeoIndex count)
{
    GeoDetailBuilder builder;
    const GeoOffset first = builder.appendPoints(count);
    for (GeoOffset k = 0; k < count; ++k) {
        builder.setPointPosition(first + k, double(k), 0., 0.);
    }
    return builder.finish();
}

GeoKernelRunResult
run(std::span<const GeoDetail* const> inputs, const char* source, int threads = 0,
    GeoAttribClassEnum runOver = eGeoAttribClassPoint)
{
    GeoKernelRunOptions options;
    options.threads = threads;
    return geoKernelRun(source, runOver, inputs, options);
}

GeoKernelRunResult
run(const GeoDetail& input, const char* source, int threads = 0, GeoAttribClassEnum runOver = eGeoAttribClassPoint)
{
    const GeoDetail* inputs[] = { &input };
    return run(std::span<const GeoDetail* const>(inputs), source, threads, runOver);
}

std::vector<std::array<double, 3>>
livePositions(const GeoDetail& detail)
{
    std::vector<std::array<double, 3>> out;
    detail.pointMap().forEachLive([&](GeoIndex, GeoOffset offset) { out.push_back(detail.pointPosition(offset)); });
    return out;
}

} // namespace

TEST(GeoKernelTopologyTest, AddPointPerPointDoublesTheCountWhateverTheThreadCount)
{
    const GeoIndex count = 3 * kPage + 77;
    const GeoDetail input = makeLine(count);
    const char* source = "vector p = @P; p.y += 1; int h = addpoint(0, p);";
    const GeoKernelRunResult one = run(input, source, 1);
    const GeoKernelRunResult many = run(input, source, 16);
    ASSERT_EQ(KernelRunStatus::eDone, one.status) << one.error;
    ASSERT_EQ(KernelRunStatus::eDone, many.status) << many.error;
    ASSERT_EQ(2 * count, one.detail.pointCount());
    ASSERT_EQ(2 * count, many.detail.pointCount());

    const std::vector<std::array<double, 3>> a = livePositions(one.detail);
    EXPECT_EQ(a, livePositions(many.detail));
    for (GeoIndex i = 0; i < count; ++i) {
        const std::array<double, 3> expected { double(i), 1., 0. };
        ASSERT_EQ(expected, a[std::size_t(count + i)]) << "new point " << i;
    }
    EXPECT_EQ(one.detail.contentHash(), many.detail.contentHash());
}

TEST(GeoKernelTopologyTest, AddPointCopiesTheSourcePointsAttributes)
{
    GeoDetailBuilder builder(makeLine(5));
    GeoAttributeDesc d;
    d.name = "mass";
    d.storage = eGeoStorageFloat32;
    builder.addAttribute(eGeoAttribClassPoint, d);
    for (GeoOffset k = 0; k < 5; ++k) {
        builder.attributeForWriting(eGeoAttribClassPoint, "mass")->set<float>(std::size_t(k), float(k) * 2.f);
    }
    const GeoDetail input = builder.finish();
    const GeoKernelRunResult result = run(input, "if (@ptnum == 3) { int h = addpoint(0, @ptnum); }", 2);
    ASSERT_EQ(KernelRunStatus::eDone, result.status) << result.error;
    ASSERT_EQ(6, result.detail.pointCount());
    const GeoOffset added = result.detail.pointMap().offsetFromIndex(5);
    EXPECT_EQ(3., result.detail.pointPosition(added)[0]);
    EXPECT_EQ(6.f, result.detail.findAttribute(eGeoAttribClassPoint, "mass")->get<float>(std::size_t(added)));
}

TEST(GeoKernelTopologyTest, RemovingOddPointsKeepsTheEvenOnesInOrder)
{
    const GeoIndex count = 2 * kPage + 300;
    const GeoDetail input = makeLine(count);
    for (int threads : { 1, 16 }) {
        const GeoKernelRunResult result = run(input, "if (@ptnum % 2 == 1) { int r = removepoint(0, @ptnum); }", threads);
        ASSERT_EQ(KernelRunStatus::eDone, result.status) << result.error;
        ASSERT_EQ(count / 2, result.detail.pointCount());
        const std::vector<std::array<double, 3>> kept = livePositions(result.detail);
        for (std::size_t i = 0; i < kept.size(); ++i) {
            ASSERT_EQ(double(2 * i), kept[i][0]) << "point " << i << " at " << threads << " threads";
        }
    }
}

TEST(GeoKernelTopologyTest, NearpointOnTheSecondInputMatchesBruteForce)
{
    const GeoDetail first = makeScatter(2 * kPage + 5, 7u);
    const GeoDetail second = makeScatter(3000, 99u);
    const GeoDetail* inputs[] = { &first, &second };
    const GeoKernelRunResult result = run(std::span<const GeoDetail* const>(inputs),
                                          "i@near = nearpoint(1, @P); i@close = nearpoint(1, @P, 0.05); i@n1 = npoints(1);", 8);
    ASSERT_EQ(KernelRunStatus::eDone, result.status) << result.error;
    const GeoAttribute* nearAttr = result.detail.findAttribute(eGeoAttribClassPoint, "near");
    const GeoAttribute* closeAttr = result.detail.findAttribute(eGeoAttribClassPoint, "close");
    const GeoAttribute* n1 = result.detail.findAttribute(eGeoAttribClassPoint, "n1");
    ASSERT_TRUE(nearAttr && closeAttr && n1);

    const std::vector<std::array<double, 3>> targets = livePositions(second);
    first.pointMap().forEachLive([&](GeoIndex index, GeoOffset offset) {
        const std::array<double, 3> q = first.pointPosition(offset);
        std::int32_t best = -1;
        double bestD2 = 0.;
        for (std::size_t k = 0; k < targets.size(); ++k) {
            const double dx = targets[k][0] - q[0];
            const double dy = targets[k][1] - q[1];
            const double dz = targets[k][2] - q[2];
            const double d2 = dx * dx + dy * dy + dz * dz;
            if (best < 0 || d2 < bestD2) {
                best = std::int32_t(k);
                bestD2 = d2;
            }
        }
        ASSERT_EQ(best, nearAttr->get<std::int32_t>(std::size_t(offset))) << "point " << index;
        // The kernel passes the radius as a float.
        const double radius = double(0.05f);
        const std::int32_t close = bestD2 <= radius * radius ? best : -1;
        ASSERT_EQ(close, closeAttr->get<std::int32_t>(std::size_t(offset))) << "point " << index;
        ASSERT_EQ(3000, n1->get<std::int32_t>(std::size_t(offset)));
    });

    std::vector<GeoIndex> within;
    GeoPointTree tree(second);
    tree.within(first.pointPosition(0), 1.5, 4, within);
    EXPECT_LE(within.size(), 4u);
}

TEST(GeoKernelTopologyTest, GroupWritesAppearInTheOutputGroups)
{
    GeoDetailBuilder builder(makeLine(2 * kPage + 9));
    const std::vector<GeoOffset> a { 0, 1, 2 };
    const std::vector<GeoOffset> b { 2, 3, 4 };
    const std::vector<GeoOffset> c { 4, 5, 6 };
    builder.appendPrimitive(eGeoPrimitivePolygon, a);
    builder.appendPrimitive(eGeoPrimitivePolygon, b);
    builder.appendPrimitive(eGeoPrimitivePolygon, c);
    const GeoDetail input = builder.finish();

    const GeoKernelRunResult points = run(input, "int r = setpointgroup(0, \"even\", @ptnum, @ptnum % 2 == 0);", 8);
    ASSERT_EQ(KernelRunStatus::eDone, points.status) << points.error;
    const GeoElementGroup* even = points.detail.findGroup(eGeoAttribClassPoint, "even");
    ASSERT_TRUE(even);
    points.detail.pointMap().forEachLive([&](GeoIndex index, GeoOffset offset) {
        ASSERT_EQ(index % 2 == 0, even->contains(offset)) << "point " << index;
    });

    const GeoKernelRunResult read = run(points.detail, "i@inEven = inpointgroup(0, \"even\", @ptnum);", 4);
    ASSERT_EQ(KernelRunStatus::eDone, read.status) << read.error;
    const GeoAttribute* inEven = read.detail.findAttribute(eGeoAttribClassPoint, "inEven");
    ASSERT_TRUE(inEven);
    for (GeoOffset o = 0; o < 20; ++o) {
        EXPECT_EQ(o % 2 == 0 ? 1 : 0, inEven->get<std::int32_t>(std::size_t(o)));
    }

    const GeoKernelRunResult prims = run(input, "int r = setprimgroup(0, \"late\", @primnum, @primnum >= 1);", 2,
                                         eGeoAttribClassPrimitive);
    ASSERT_EQ(KernelRunStatus::eDone, prims.status) << prims.error;
    const GeoElementGroup* late = prims.detail.findGroup(eGeoAttribClassPrimitive, "late");
    ASSERT_TRUE(late);
    EXPECT_FALSE(late->contains(0));
    EXPECT_TRUE(late->contains(1));
    EXPECT_TRUE(late->contains(2));

    const GeoKernelRunResult none = run(input, "if (@ptnum < 0) { int r = setpointgroup(0, \"empty\", @ptnum, 1); }");
    ASSERT_EQ(KernelRunStatus::eDone, none.status) << none.error;
    EXPECT_TRUE(none.detail.findGroup(eGeoAttribClassPoint, "empty"));
}

TEST(GeoKernelTopologyTest, DetailRunBuildsAPolygonFromNewPoints)
{
    const GeoDetail input = makeLine(4);
    const char* source = "int a = addpoint(0, {0, 0, 5});\n"
                         "int b = addpoint(0, {1, 0, 5});\n"
                         "int c = addpoint(0, {0, 1, 5});\n"
                         "int p = addprim(0, \"poly\", a, b);\n"
                         "int v = addvertex(0, p, c);\n"
                         "int q = addprim(0, \"polyline\", 0, 1);\n"
                         "int r = removeprim(0, q, 0);\n";
    const GeoKernelRunResult result = run(input, source, 0, eGeoAttribClassDetail);
    ASSERT_EQ(KernelRunStatus::eDone, result.status) << result.error;
    const GeoDetail& out = result.detail;
    ASSERT_EQ(7, out.pointCount());
    ASSERT_EQ(1, out.primitiveCount());
    const GeoOffset prim = out.primitiveMap().offsetFromIndex(0);
    const GeoVertexRange range = out.primitiveVertices(prim);
    ASSERT_EQ(3, range.count);
    EXPECT_EQ(5., out.pointPosition(out.vertexPoint(range.start))[2]);
    EXPECT_EQ(1., out.pointPosition(out.vertexPoint(range.start + 1))[0]);
    EXPECT_EQ(1., out.pointPosition(out.vertexPoint(range.start + 2))[1]);
}

TEST(GeoKernelTopologyTest, SecondInputAttributesReadByNumber)
{
    const GeoDetail first = makeLine(10);
    const GeoDetail second = makeScatter(6, 3u);
    const GeoDetail* inputs[] = { &first, &second };
    const GeoKernelRunResult result = run(std::span<const GeoDetail* const>(inputs),
                                          "v@other = @opinput1_P; vector q = point(1, \"P\", @ptnum); f@qx = q.x;", 3);
    ASSERT_EQ(KernelRunStatus::eDone, result.status) << result.error;
    const GeoAttribute* other = result.detail.findAttribute(eGeoAttribClassPoint, "other");
    const GeoAttribute* qx = result.detail.findAttribute(eGeoAttribClassPoint, "qx");
    ASSERT_TRUE(other && qx);
    for (GeoOffset o = 0; o < 10; ++o) {
        const float expected = o < 6 ? float(second.pointPosition(o)[0]) : 0.f;
        EXPECT_EQ(expected, other->get<float>(std::size_t(o), 0)) << "point " << o;
        EXPECT_EQ(expected, qx->get<float>(std::size_t(o))) << "point " << o;
    }
}

TEST(GeoKernelTopologyTest, WritingAnotherGeometryHandleFails)
{
    const GeoDetail input = makeLine(3);
    const GeoKernelRunResult result = run(input, "int h = addpoint(1, @P);");
    EXPECT_EQ(KernelRunStatus::eFailed, result.status);
}
