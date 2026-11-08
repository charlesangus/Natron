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

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Geo/GeoKernelBinding.h"
#include "Engine/GeoDetail.h"

NATRON_NAMESPACE_USING

namespace {

constexpr GeoOffset kPage = GeoOffset(kGeoPageSize);

GeoDetail
makePoints(GeoIndex count)
{
    GeoDetailBuilder builder;
    builder.appendPoints(count);
    std::vector<float> p(std::size_t(count) * 3);
    for (std::size_t i = 0; i < std::size_t(count); ++i) {
        p[3 * i] = float(i) * 0.25f;
        p[3 * i + 1] = float(i % 977) * 0.5f - 7.f;
        p[3 * i + 2] = float(i % 13);
    }
    builder.positionForWriting().scatterDense<float>(0, std::size_t(count), std::span<const float>(p));
    return builder.finish();
}

std::vector<float>
positions(const GeoDetail& detail)
{
    const GeoAttribute& p = detail.position();
    std::vector<float> out(p.size() * 3);
    p.copyDense<float>(0, p.size(), std::span<float>(out));
    return out;
}

GeoKernelRunResult
run(const GeoDetail& input, const char* source, int threads = 0, GeoAttribClassEnum runOver = eGeoAttribClassPoint)
{
    const GeoDetail* inputs[] = { &input };
    GeoKernelRunOptions options;
    options.threads = threads;
    return geoKernelRun(source, runOver, std::span<const GeoDetail* const>(inputs), options);
}

} // namespace

TEST(GeoKernelRunnerTest, AddOneToPYOverTenMillionPointsMatchesScalar)
{
    constexpr GeoIndex kCount = 10000000;
    const GeoDetail input = makePoints(kCount);
    const GeoKernelRunResult result = run(input, "@P.y += 1;");
    ASSERT_EQ(KernelRunStatus::eDone, result.status) << result.error;
    ASSERT_EQ(kCount, result.detail.pointCount());

    const std::vector<float> before = positions(input);
    const std::vector<float> after = positions(result.detail);
    ASSERT_EQ(before.size(), after.size());
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < std::size_t(kCount); ++i) {
        const float x = before[3 * i];
        const float y = before[3 * i + 1] + 1.f;
        const float z = before[3 * i + 2];
        mismatches += after[3 * i] != x || after[3 * i + 1] != y || after[3 * i + 2] != z;
    }
    EXPECT_EQ(0u, mismatches);
    EXPECT_EQ((std::uint64_t(kCount) + 255) / 256, result.batches);
}

TEST(GeoKernelRunnerTest, PagesNotWrittenStayShared)
{
    const GeoDetail input = makePoints(4 * kPage);
    const GeoKernelRunResult result = run(input, "if (@ptnum >= 1100 && @ptnum < 1200) @P.y += 1; i@id = 7;", 4);
    ASSERT_EQ(KernelRunStatus::eDone, result.status) << result.error;

    const GeoAttribute& before = input.position();
    const GeoAttribute& after = result.detail.position();
    ASSERT_EQ(before.values().pageCount(), after.values().pageCount());
    for (std::size_t i = 0; i < before.values().pageCount(); ++i) {
        EXPECT_EQ(i != 1, before.values().page(i).get() == after.values().page(i).get()) << "P page " << i;
    }
    const GeoAttribute* id = result.detail.findAttribute(eGeoAttribClassPoint, "id");
    ASSERT_TRUE(id);
    EXPECT_EQ(7, id->get<std::int32_t>(std::size_t(3 * kPage + 5)));
}

TEST(GeoKernelRunnerTest, ResultsAreIdenticalAcrossThreadCounts)
{
    const GeoDetail input = makePoints(9 * kPage + 333);
    const char* source = "vector p = @P;\n"
                         "float n = noise(p * 0.1);\n"
                         "@P.y += sin(p.x) * n + rand(@ptnum);\n"
                         "v@Cd = {p.z, n, float(@ptnum % 5)};\n"
                         "if (@ptnum % 3 == 0) i@tag = @ptnum * 2; else i@tag = -1;\n"
                         "for (int k = 0; k < @ptnum % 7; k++) f@acc += k * 0.5;\n";
    const GeoKernelRunResult one = run(input, source, 1);
    ASSERT_EQ(KernelRunStatus::eDone, one.status) << one.error;
    const std::vector<float> reference = positions(one.detail);
    for (int threads : { 2, 3, 8, 16 }) {
        const GeoKernelRunResult many = run(input, source, threads);
        ASSERT_EQ(KernelRunStatus::eDone, many.status) << many.error;
        EXPECT_EQ(reference, positions(many.detail)) << threads << " threads";
        for (const char* name : { "Cd", "tag", "acc" }) {
            const GeoAttribute* a = one.detail.findAttribute(eGeoAttribClassPoint, name);
            const GeoAttribute* b = many.detail.findAttribute(eGeoAttribClassPoint, name);
            ASSERT_TRUE(a && b) << name;
            EXPECT_EQ(a->contentHash(), b->contentHash()) << name << " at " << threads << " threads";
        }
    }
}

TEST(GeoKernelRunnerTest, HolesAreSkippedAndPtnumStaysDense)
{
    const GeoDetail full = makePoints(3 * kPage + 10);
    GeoDetailBuilder builder(full);
    std::vector<GeoOffset> dead;
    for (GeoOffset o = 0; o < full.pointMap().offsetSize(); o += 3) {
        dead.push_back(o);
    }
    for (GeoOffset o = 2 * kPage; o < 2 * kPage + 200; ++o) {
        if (o % 3 != 0) {
            dead.push_back(o);
        }
    }
    builder.deletePoints(dead, eGeoPointDeleteKeepReferenced);
    const GeoDetail input = builder.finish();
    ASSERT_GT(input.pointMap().tombstoneCount(), 0);

    const GeoKernelRunResult result = run(input, "i@n = @ptnum; i@count = @numpt; @P.x = -1;", 4);
    ASSERT_EQ(KernelRunStatus::eDone, result.status) << result.error;
    const GeoDetail& out = result.detail;
    ASSERT_EQ(input.pointCount(), out.pointCount());
    const GeoAttribute* n = out.findAttribute(eGeoAttribClassPoint, "n");
    const GeoAttribute* count = out.findAttribute(eGeoAttribClassPoint, "count");
    ASSERT_TRUE(n && count);
    for (GeoIndex i = 0; i < out.pointCount(); ++i) {
        const GeoOffset o = out.pointMap().offsetFromIndex(i);
        ASSERT_EQ(std::int32_t(i), n->get<std::int32_t>(std::size_t(o))) << "point " << i;
        ASSERT_EQ(std::int32_t(input.pointCount()), count->get<std::int32_t>(std::size_t(o)));
    }
    for (GeoOffset o : dead) {
        EXPECT_EQ(input.pointPosition(o)[0], out.pointPosition(o)[0]) << "dead offset " << o;
        EXPECT_EQ(0, n->get<std::int32_t>(std::size_t(o))) << "dead offset " << o;
    }
}

TEST(GeoKernelRunnerTest, AbortStopsWithinOneBatchPerThread)
{
    const GeoDetail input = makePoints(64 * kPage);
    for (int threads : { 1, 4, 16 }) {
        std::atomic<int> checks { 0 };
        const GeoDetail* inputs[] = { &input };
        GeoKernelRunOptions options;
        options.threads = threads;
        // The runner checks before every batch, so only the first eight checks can let one run.
        options.aborted = [&] { return checks.fetch_add(1) >= 8; };
        const GeoKernelRunResult result = geoKernelRun("@P.y += 1;", eGeoAttribClassPoint,
                                                       std::span<const GeoDetail* const>(inputs), options);
        EXPECT_EQ(KernelRunStatus::eAborted, result.status) << threads << " threads";
        EXPECT_LE(result.batches, 8u) << threads << " threads";
        EXPECT_EQ(input.contentHash(), result.detail.contentHash());
    }
}

TEST(GeoKernelRunnerTest, AbortStopsAnEndlessLoop)
{
    const GeoDetail input = makePoints(4 * kPage);
    std::atomic<int> checks { 0 };
    const GeoDetail* inputs[] = { &input };
    GeoKernelRunOptions options;
    options.threads = 2;
    options.aborted = [&] { return checks.fetch_add(1) >= 4; };
    const GeoKernelRunResult result = geoKernelRun("int i = 0; while (i >= 0) { i = 1; } @P.y = i;", eGeoAttribClassPoint,
                                                   std::span<const GeoDetail* const>(inputs), options);
    EXPECT_EQ(KernelRunStatus::eAborted, result.status);
}

TEST(GeoKernelRunnerTest, VertexRunReadsPointAndPrimitiveAttributes)
{
    GeoDetailBuilder builder;
    const GeoOffset first = builder.appendPoints(4);
    for (GeoOffset k = 0; k < 4; ++k) {
        builder.setPointPosition(first + k, double(k), 0., 0.);
    }
    const std::vector<GeoOffset> quad { 0, 1, 2, 3 };
    const std::vector<GeoOffset> tri { 3, 2, 1 };
    builder.appendPrimitive(eGeoPrimitivePolygon, quad);
    builder.appendPrimitive(eGeoPrimitivePolygon, tri);
    GeoAttributeDesc w;
    w.name = "weight";
    w.storage = eGeoStorageFloat32;
    builder.addAttribute(eGeoAttribClassPrimitive, w);
    builder.attributeForWriting(eGeoAttribClassPrimitive, "weight")->set<float>(1, 10.f);
    const GeoDetail input = builder.finish();

    const GeoKernelRunResult result = run(input, "f@s = @P.x + @weight; i@pt = @ptnum; i@pr = @primnum;", 2, eGeoAttribClassVertex);
    ASSERT_EQ(KernelRunStatus::eDone, result.status) << result.error;
    const GeoAttribute* s = result.detail.findAttribute(eGeoAttribClassVertex, "s");
    const GeoAttribute* pt = result.detail.findAttribute(eGeoAttribClassVertex, "pt");
    const GeoAttribute* pr = result.detail.findAttribute(eGeoAttribClassVertex, "pr");
    ASSERT_TRUE(s && pt && pr);
    const std::vector<float> expectedS { 0.f, 1.f, 2.f, 3.f, 13.f, 12.f, 11.f };
    const std::vector<std::int32_t> expectedPt { 0, 1, 2, 3, 3, 2, 1 };
    const std::vector<std::int32_t> expectedPr { 0, 0, 0, 0, 1, 1, 1 };
    for (std::size_t v = 0; v < expectedS.size(); ++v) {
        EXPECT_EQ(expectedS[v], s->get<float>(v)) << "vertex " << v;
        EXPECT_EQ(expectedPt[v], pt->get<std::int32_t>(v)) << "vertex " << v;
        EXPECT_EQ(expectedPr[v], pr->get<std::int32_t>(v)) << "vertex " << v;
    }
}

TEST(GeoKernelRunnerTest, WritingAnotherClassAttributeFails)
{
    GeoDetailBuilder builder(makePoints(10));
    GeoAttributeDesc d;
    d.name = "global";
    d.storage = eGeoStorageFloat32;
    builder.addAttribute(eGeoAttribClassDetail, d);
    const GeoDetail input = builder.finish();
    const GeoKernelRunResult result = run(input, "f@global = 1;");
    EXPECT_EQ(KernelRunStatus::eFailed, result.status);
    EXPECT_NE(std::string::npos, result.error.find("detail attribute")) << result.error;
}
