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
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <thread>
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
        std::mutex mutex;
        int checks = 0;
        // Per thread, the checks made once the abort was raised. A thread polls before each batch,
        // so a thread that stops within the batch it is running polls at most once after it.
        std::map<std::thread::id, int> checksAfterAbort;
        const GeoDetail* inputs[] = { &input };
        GeoKernelRunOptions options;
        options.threads = threads;
        options.aborted = [&] {
            std::lock_guard<std::mutex> lock(mutex);
            const bool raised = ++checks > 8;
            if (raised) {
                ++checksAfterAbort[std::this_thread::get_id()];
            }
            return raised;
        };
        const GeoKernelRunResult result = geoKernelRun("@P.y += 1;", eGeoAttribClassPoint,
                                                       std::span<const GeoDetail* const>(inputs), options);
        EXPECT_EQ(KernelRunStatus::eAborted, result.status) << threads << " threads";
        EXPECT_LE(result.batches, 8u) << threads << " threads";
        EXPECT_FALSE(checksAfterAbort.empty()) << threads << " threads";
        for (const auto& entry : checksAfterAbort) {
            EXPECT_EQ(1, entry.second) << threads << " threads";
        }
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

namespace {

std::vector<float>
floatAttribute(const GeoDetail& detail, const char* name, int component = 0)
{
    std::vector<float> out;
    const GeoAttribute* a = detail.findAttribute(eGeoAttribClassPoint, name);
    if (!a) {
        ADD_FAILURE() << "no point attribute '" << name << "'";
        return out;
    }
    for (GeoIndex i = 0; i < detail.pointCount(); ++i) {
        out.push_back(a->get<float>(std::size_t(i), component));
    }
    return out;
}

} // namespace

TEST(GeoKernelRunnerTest, Noise4FunctionsStayInTheirRanges)
{
    const GeoDetail input = makePoints(4 * kPage);
    const GeoKernelRunResult result = run(input,
                                          "vector p = @P * 0.37 + set(0.1, 0.2, 0.3);\n"
                                          "f@p = pnoise(p); f@x = xnoise(p); f@v = valuenoise(p); f@w = wnoise(p);\n"
                                          "f@o = onoise(p, 6); f@a = anoise(p, 6);\n",
                                          4);
    ASSERT_EQ(KernelRunStatus::eDone, result.status) << result.error;
    const struct {
        const char* name;
        float lo;
        float hi;
    } ranges[] = {
        { "p", -1.f, 1.f },
        { "x", -1.f, 1.f },
        { "v", -1.f, 1.f },
        { "w", 0.f, 1.f },
        { "o", -1.f, 1.f },
        { "a", 0.f, 1.f },
    };
    for (const auto& r : ranges) {
        const std::vector<float> values = floatAttribute(result.detail, r.name);
        ASSERT_FALSE(values.empty()) << r.name;
        float lo = values[0];
        float hi = values[0];
        for (float v : values) {
            ASSERT_TRUE(std::isfinite(v)) << r.name;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        EXPECT_GE(lo, r.lo) << r.name;
        EXPECT_LE(hi, r.hi) << r.name;
        EXPECT_GT(hi - lo, 0.25f) << r.name << " barely varies";
    }
}

TEST(GeoKernelRunnerTest, Noise4FunctionsAreDeterministic)
{
    const GeoDetail input = makePoints(3 * kPage + 77);
    const char* source = "vector p = @P * 0.21;\n"
                         "f@p = pnoise(p); f@x = xnoise(p); f@v = valuenoise(p); f@w = wnoise(p);\n"
                         "f@o = onoise(p); f@a = anoise(p); v@c = curlnoise(p); v@pv = pnoise(p);\n";
    const GeoKernelRunResult one = run(input, source, 1);
    ASSERT_EQ(KernelRunStatus::eDone, one.status) << one.error;
    for (int threads : { 1, 3, 8 }) {
        const GeoKernelRunResult again = run(input, source, threads);
        ASSERT_EQ(KernelRunStatus::eDone, again.status) << again.error;
        for (const char* name : { "p", "x", "v", "w", "o", "a", "c", "pv" }) {
            const GeoAttribute* a = one.detail.findAttribute(eGeoAttribClassPoint, name);
            const GeoAttribute* b = again.detail.findAttribute(eGeoAttribClassPoint, name);
            ASSERT_TRUE(a && b) << name;
            EXPECT_EQ(a->contentHash(), b->contentHash()) << name << " at " << threads << " threads";
        }
    }
    EXPECT_EQ(floatAttribute(one.detail, "p"), floatAttribute(one.detail, "pv", 0));
}

TEST(GeoKernelRunnerTest, Noise4FourthComponentChangesTheResult)
{
    const GeoDetail input = makePoints(kPage);
    for (const char* fn : { "pnoise", "xnoise", "valuenoise", "wnoise", "onoise", "anoise", "curlnoise" }) {
        const std::string f(fn);
        const std::string source = "vector p = @P * 0.29 + set(0.1, 0.2, 0.3);\n"
                                   "v@three = "
            + f + "(p);\n"
                  "v@zero = "
            + f + "(set(p.x, p.y, p.z, 0));\n"
                  "v@moved = "
            + f + "(set(p.x, p.y, p.z, 0.45));\n";
        const GeoKernelRunResult result = run(input, source.c_str(), 2);
        ASSERT_EQ(KernelRunStatus::eDone, result.status) << fn << ": " << result.error;
        const std::vector<float> three = floatAttribute(result.detail, "three");
        const std::vector<float> zero = floatAttribute(result.detail, "zero");
        const std::vector<float> moved = floatAttribute(result.detail, "moved");
        EXPECT_EQ(three, zero) << fn << ": a vector position is w = 0";
        ASSERT_EQ(zero.size(), moved.size());
        std::size_t changed = 0;
        for (std::size_t i = 0; i < zero.size(); ++i) {
            changed += zero[i] != moved[i];
        }
        EXPECT_GT(changed, zero.size() * 9 / 10) << fn;
    }
}

TEST(GeoKernelRunnerTest, FractalNoiseWithOneOctaveIsTheBaseNoise)
{
    const GeoDetail input = makePoints(2 * kPage);
    const GeoKernelRunResult result = run(input,
                                          "vector4 p = set(@P.x * 0.33, @P.y * 0.33, @P.z * 0.33, @P.x * 0.05);\n"
                                          "f@base = pnoise(p); f@o1 = onoise(p, 1); f@a1 = anoise(p, 1, 3, 0.7);\n"
                                          "f@o4 = onoise(p);\n",
                                          2);
    ASSERT_EQ(KernelRunStatus::eDone, result.status) << result.error;
    const std::vector<float> base = floatAttribute(result.detail, "base");
    const std::vector<float> o1 = floatAttribute(result.detail, "o1");
    const std::vector<float> a1 = floatAttribute(result.detail, "a1");
    const std::vector<float> o4 = floatAttribute(result.detail, "o4");
    EXPECT_EQ(base, o1);
    ASSERT_EQ(base.size(), a1.size());
    std::size_t differs = 0;
    for (std::size_t i = 0; i < base.size(); ++i) {
        EXPECT_NEAR(std::abs(base[i]), a1[i], 1e-6f) << "point " << i;
        differs += o4[i] != base[i];
    }
    EXPECT_GT(differs, base.size() / 2) << "the default octaves add detail";
}

TEST(GeoKernelRunnerTest, CurlNoiseReturnsADivergenceFreeVector)
{
    const GeoDetail input = makePoints(kPage / 2);
    const GeoKernelRunResult result = run(input,
                                          "vector p = @P * 0.3 + set(0.1, 0.2, 0.3);\n"
                                          "float h = 0.01;\n"
                                          "v@c = curlnoise(p);\n"
                                          "vector xh = curlnoise(p + set(h, 0, 0)); vector xl = curlnoise(p - set(h, 0, 0));\n"
                                          "vector yh = curlnoise(p + set(0, h, 0)); vector yl = curlnoise(p - set(0, h, 0));\n"
                                          "vector zh = curlnoise(p + set(0, 0, h)); vector zl = curlnoise(p - set(0, 0, h));\n"
                                          "float dx = xh.x - xl.x; float dy = yh.y - yl.y; float dz = zh.z - zl.z;\n"
                                          "f@div = abs(dx + dy + dz) / (2 * h);\n"
                                          "f@dxx = abs(dx) / (2 * h);\n",
                                          2);
    ASSERT_EQ(KernelRunStatus::eDone, result.status) << result.error;
    const GeoAttribute* c = result.detail.findAttribute(eGeoAttribClassPoint, "c");
    ASSERT_TRUE(c);
    EXPECT_EQ(3, c->tupleSize());
    double magnitude = 0.;
    for (int k = 0; k < 3; ++k) {
        const std::vector<float> values = floatAttribute(result.detail, "c", k);
        float lo = values[0];
        float hi = values[0];
        for (float v : values) {
            ASSERT_TRUE(std::isfinite(v));
            lo = std::min(lo, v);
            hi = std::max(hi, v);
            magnitude += std::abs(v);
        }
        EXPECT_GT(hi - lo, 0.5f) << "component " << k << " barely varies";
    }
    EXPECT_GT(magnitude, 0.);
    const std::vector<float> div = floatAttribute(result.detail, "div");
    const std::vector<float> dxx = floatAttribute(result.detail, "dxx");
    double divSum = 0.;
    double dxxSum = 0.;
    for (std::size_t i = 0; i < div.size(); ++i) {
        divSum += div[i];
        dxxSum += dxx[i];
    }
    EXPECT_LT(divSum, 0.05 * dxxSum);
}
