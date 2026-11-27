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
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Geo/GeoGroupOps.h"
#include "Engine/Geo/GeoOpUtils.h"
#include "Engine/GeoDetail.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Geometry/GeoDelete.h"
#include "Engine/Nodes/Geometry/GeoEdit.h"
#include "Engine/Nodes/Geometry/GeoPrimitive.h"
#include "Engine/Nodes/Geometry/GeoToUSD.h"
#include "Engine/Nodes/Geometry/GeoTransform.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDWrite.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"
#include "Tests/Scene/GeoToUSDTestUtils.h"

NATRON_NAMESPACE_USING

/*
 * Scale bench of the geometry nodes, run by tools/bench/geo_scale.py. The suite is DISABLED_ so
 * that neither ctest nor a plain run of the binary executes it; the driver passes
 * --gtest_also_run_disabled_tests. Each stage prints one "GEOBENCH {json}" line.
 *
 * Environment:
 *   NATRON_GEO_BENCH_GRID       grid faces per side (default 3162, which is 10,004,569 points)
 *   NATRON_GEO_BENCH_REPS       timed runs per stage (default 5)
 *   NATRON_GEO_BENCH_SELECTION  elements in the encode and expand selections (default 1000000)
 */
namespace {

long
envLong(const char* name,
        long fallback)
{
    const char* value = std::getenv(name);

    return (value && value[0] != '\0') ? std::strtol(value, nullptr, 10) : fallback;
}

// VmRSS or VmHWM of this process in kB, 0 when /proc does not say.
std::int64_t
statusKb(const char* key)
{
    std::ifstream status("/proc/self/status");
    std::string line;
    const std::string prefix = std::string(key) + ":";
    while (std::getline(status, line)) {
        if (line.compare(0, prefix.size(), prefix) == 0) {
            return std::strtoll(line.c_str() + prefix.size(), nullptr, 10);
        }
    }

    return 0;
}

// Restarts the high-water mark at the current RSS, so that a stage's peak is its own.
void
resetHighWater()
{
    std::ofstream clear("/proc/self/clear_refs");
    clear << "5";
}

double
median(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();

    return n == 0 ? 0. : ((n % 2) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]));
}

struct StageResult {
    std::string name;
    std::vector<double> seconds;
    std::int64_t peakKb = 0;
    std::int64_t deltaKb = 0;
};

void
report(const StageResult& r,
       const std::string& detail)
{
    std::ostringstream times;
    for (std::size_t i = 0; i < r.seconds.size(); ++i) {
        times << (i ? "," : "") << r.seconds[i];
    }
    std::printf("GEOBENCH {\"stage\":\"%s\",\"median_s\":%.6f,\"min_s\":%.6f,\"runs_s\":[%s],"
                "\"peak_rss_kb\":%lld,\"delta_rss_kb\":%lld,\"detail\":\"%s\"}\n",
                r.name.c_str(),
                median(r.seconds),
                *std::min_element(r.seconds.begin(), r.seconds.end()),
                times.str().c_str(),
                (long long)r.peakKb,
                (long long)r.deltaKb,
                detail.c_str());
    std::fflush(stdout);
}

// prepare runs untimed before each repetition; body is the timed stage.
StageResult
runStage(const std::string& name,
         int reps,
         const std::function<void(int)>& prepare,
         const std::function<void()>& body)
{
    StageResult r;
    r.name = name;
    for (int i = 0; i < reps; ++i) {
        if (prepare) {
            prepare(i);
        }
        resetHighWater();
        const std::int64_t before = statusKb("VmRSS");
        const auto t0 = std::chrono::steady_clock::now();
        body();
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - t0;
        const std::int64_t peak = statusKb("VmHWM");
        r.seconds.push_back(elapsed.count());
        r.peakKb = std::max(r.peakKb, peak);
        r.deltaKb = std::max(r.deltaKb, peak - before);
    }

    return r;
}

// Cheap, stateless per-offset hash, so that the scattered selection is the same in every build.
std::uint64_t
mix(std::uint64_t x)
{
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;

    return x ^ (x >> 31);
}
} // anonymous namespace

class DISABLED_GeoBenchTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        KnobDouble* metersPerUnit = dynamic_cast<KnobDouble*>(getApp()->getProject()->getKnobByName("metersPerUnit").get());
        ASSERT_TRUE(metersPerUnit != NULL);
        metersPerUnit->setValue(0.01);
    }

    virtual void TearDown() OVERRIDE
    {
        for (auto it = _nodes.rbegin(); it != _nodes.rend(); ++it) {
            if (*it) {
                (*it)->destroyNode(false, false);
            }
        }
        _nodes.clear();
        SceneQueryPool::instance().releaseIdle();

        BaseTest::TearDown();
    }

    NodePtr make(const char* pluginID)
    {
        NodePtr node = createNode(QString::fromUtf8(pluginID));

        EXPECT_TRUE(bool(node)) << pluginID;
        if (node) {
            _nodes.push_back(node);
        }
        pinGeoToUSDPathPrefix(node, pluginID);

        return node;
    }

    NodePtr after(const NodePtr& input,
                  const char* pluginID)
    {
        NodePtr node = make(pluginID);
        if (node && input) {
            EXPECT_TRUE(node->connectInput(input, 0));
        }

        return node;
    }

    template <typename K>
    static K* knob(const NodePtr& node,
                   const char* name)
    {
        K* k = dynamic_cast<K*>(node->getKnobByName(name).get());

        EXPECT_TRUE(k != NULL) << name;

        return k;
    }

    GeoDetailConstPtr pull(const NodePtr& node,
                           double time = 1.)
    {
        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(true, 0);
        ParallelRenderArgsSetter frameRenderArgs(time,
                                                 ViewIdx(0),
                                                 true /*isRenderUserInteraction*/,
                                                 false /*isSequential*/,
                                                 abortInfo,
                                                 node,
                                                 0 /*textureIndex*/,
                                                 getApp()->getTimeLine().get(),
                                                 NodePtr(),
                                                 false /*isAnalysis*/,
                                                 false /*draftMode*/,
                                                 RenderStatsPtr());
        GeoDetailConstPtr detail;
        std::string error;

        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk,
                  node->getEffectInstance()->evaluateGeometry(EffectInstance::EvaluateGeometryArgs(time, ViewIdx(0)), &detail, &error))
            << error;

        return detail;
    }

    QTemporaryDir _dir;

private:
    std::vector<NodePtr> _nodes;
};

TEST_F(DISABLED_GeoBenchTest, ScalePipeline)
{
    const int side = (int)envLong("NATRON_GEO_BENCH_GRID", 3162);
    const int reps = std::max(1, (int)envLong("NATRON_GEO_BENCH_REPS", 5));
    const std::size_t selection = (std::size_t)envLong("NATRON_GEO_BENCH_SELECTION", 1000000);

    const NodePtr source = make(PLUGINID_NATRON_GEOPRIMITIVE);
    ASSERT_TRUE(source != nullptr);
    {
        KnobChoice* type = knob<KnobChoice>(source, kGeoPrimitiveParamType);
        KnobInt* rows = knob<KnobInt>(source, kGeoPrimitiveParamGridRows);
        KnobInt* columns = knob<KnobInt>(source, kGeoPrimitiveParamGridColumns);
        ASSERT_TRUE(type && rows && columns);
        type->setValueFromID("grid", 0);
        rows->setValue(side);
        columns->setValue(side);
    }
    const NodePtr transform = after(source, PLUGINID_NATRON_GEOTRANSFORM);
    const NodePtr edit = after(transform, PLUGINID_NATRON_GEOEDIT);
    const NodePtr remove = after(edit, PLUGINID_NATRON_GEODELETE);
    const NodePtr toUSD = after(remove, PLUGINID_NATRON_GEOTOUSD);
    const NodePtr writer = after(toUSD, PLUGINID_NATRON_USDWRITE);
    ASSERT_TRUE(transform && edit && remove && toUSD && writer);

    const GeoDetailConstPtr grid = pull(source);
    ASSERT_TRUE(grid != nullptr);
    const GeoIndex points = grid->pointCount();
    const GeoIndex primitives = grid->primitiveCount();
    char shape[160];
    std::snprintf(shape, sizeof(shape), "%lld points, %lld primitives", (long long)points, (long long)primitives);
    std::printf("GEOBENCH {\"stage\":\"grid\",\"detail\":\"%s\"}\n", shape);
    ASSERT_GT(points, GeoIndex(10));

    KnobDouble* translate = knob<KnobDouble>(transform, kGeoTransformParamTranslate);
    ASSERT_TRUE(translate != nullptr);
    // Each repetition moves the grid, which invalidates every stage downstream of the transform.
    auto invalidate = [&](int rep) {
        translate->setValue(0.001 * (rep + 1), ViewSpec::all(), 0);
    };

    GeoEditData edits;
    edits.pointCount = points;
    edits.primitiveCount = primitives;
    for (int i = 0; i < 5; ++i) {
        edits.pointOffsets[(points / 6) * i + i] = { 0.1 * (i + 1), -0.2, 0.3 };
    }
    knob<KnobString>(edit, kGeoEditParamEdits)->setValue(edits.encode());

    knob<KnobChoice>(remove, kGeoDeleteParamGroupType)->setValueFromID("points", 0);
    knob<KnobString>(remove, kGeoDeleteParamGroup)->setValue("0-" + std::to_string((long long)(points / 2) - 1));

    // The write flattens the whole stage to a file, so its time includes authoring and writing the mesh.
    USDWrite* writeEffect = dynamic_cast<USDWrite*>(writer->getEffectInstance().get());
    ASSERT_TRUE(writeEffect != NULL);
    knob<KnobOutputFile>(writer, kUSDWriteParamFile)->setValue(_dir.path().toStdString() + "/geo_bench.usdc");
    knob<KnobChoice>(writer, kUSDWriteParamExportMode)->setValue((int)USDWrite::eExportModeFlattened);
    knob<KnobChoice>(writer, kUSDWriteParamPathMode)->setValue((int)USDWrite::ePathModeRelative);
    knob<KnobChoice>(writer, kUSDWriteParamFrameRange)->setValue((int)USDWrite::eFrameRangeManual);
    knob<KnobInt>(writer, kUSDWriteParamFirstFrame)->setValue(1);
    knob<KnobInt>(writer, kUSDWriteParamLastFrame)->setValue(1);

    // Each stage is pulled after its inputs were, so the time is that node's own.
    auto stage = [&](const char* name,
                     const NodePtr& node,
                     const std::vector<NodePtr>& upstream) {
        StageResult r = runStage(
            name, reps,
            [&](int rep) {
                invalidate(rep);
                for (const NodePtr& n : upstream) {
                    EXPECT_TRUE(pull(n) != nullptr);
                }
            },
            [&] {
                EXPECT_TRUE(pull(node) != nullptr);
            });
        report(r, "");
    };
    stage("GeoTransform", transform, {});
    stage("GeoEdit", edit, { transform });
    stage("GeoDelete", remove, { transform, edit });

    StageResult usd = runStage(
        "GeoToUSD+USDWrite", reps,
        [&](int rep) {
            invalidate(rep);
            for (const NodePtr& n : { transform, edit, remove }) {
                EXPECT_TRUE(pull(n) != nullptr);
            }
        },
        [&] {
            EXPECT_EQ(eStatusOK, writeEffect->writeFrameRange(1, 1, ViewIdx(0)));
        });
    report(usd, "flattened export to a file");

    const std::size_t offsets = std::size_t(grid->pointMap().offsetSize());
    const std::size_t count = std::min(selection, offsets);

    GeoElementMask scattered(offsets, 0);
    const std::uint64_t modulus = std::max<std::uint64_t>(1, offsets / std::max<std::size_t>(count, 1));
    std::size_t scatteredCount = 0;
    for (std::size_t i = 0; i < offsets; ++i) {
        if (mix(i) % modulus == 0) {
            scattered[i] = 1;
            ++scatteredCount;
        }
    }
    std::string pattern;
    StageResult encode = runStage("geoEncodePattern", reps, nullptr, [&] {
        pattern = geoEncodePattern(*grid, eGeoAttribClassPoint, scattered);
    });
    std::snprintf(shape, sizeof(shape), "%zu scattered points of %zu, pattern %zu bytes", scatteredCount, offsets, pattern.size());
    report(encode, shape);

    GeoElementMask block(offsets, 0);
    std::fill(block.begin(), block.begin() + std::ptrdiff_t(count), char(1));
    std::size_t grown = 0;
    StageResult expand = runStage("geoExpandPoints", reps, nullptr, [&] {
        const GeoElementMask out = geoExpandPoints(*grid, block, 1);
        grown = std::count(out.begin(), out.end(), char(1));
    });
    std::snprintf(shape, sizeof(shape), "%zu contiguous points grown one ring to %zu", count, grown);
    report(expand, shape);
}
