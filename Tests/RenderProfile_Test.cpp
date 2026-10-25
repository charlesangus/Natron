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

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/FrameRenderContext.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Merge/Merge.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RenderScheduler.h"
#include "Engine/RenderStats.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kCheckerBoardPluginID = "net.sf.openfx.CheckerBoardPlugin";
const int kWindowSize = 64;

struct Record {
    long long frame = -1;
    int task = -1;
    std::string node;
    std::string plugin;
    std::vector<int> deps;
    long long wallNs = 0;
    bool pointOp = false;
    std::string device;
    std::string regionId;
};

// The value after `key` up to the next ',' or '}' or ']'.
std::string
rawValue(const std::string& line,
         const std::string& key)
{
    const std::size_t at = line.find("\"" + key + "\":");
    if (at == std::string::npos) {
        return std::string();
    }
    const std::size_t begin = at + key.size() + 3;
    const std::size_t end = line.find_first_of(",}]", begin);

    return line.substr(begin, end - begin);
}

std::string
stringValue(const std::string& line,
            const std::string& key)
{
    const std::string raw = rawValue(line, key);

    return (raw.size() >= 2) ? raw.substr(1, raw.size() - 2) : std::string();
}

Record
parseRecord(const std::string& line)
{
    Record r;

    r.frame = std::atoll(rawValue(line, "frame").c_str());
    r.task = std::atoi(rawValue(line, "task").c_str());
    r.node = stringValue(line, "node");
    r.plugin = stringValue(line, "plugin");
    r.wallNs = std::atoll(rawValue(line, "wallNs").c_str());
    r.pointOp = rawValue(line, "pointOp") == "true";
    r.device = stringValue(line, "device");
    r.regionId = rawValue(line, "regionId");
    const std::size_t open = line.find("\"deps\":[");
    const std::size_t close = line.find(']', open);
    std::string list = line.substr(open + 8, close - open - 8);
    std::size_t pos = 0;
    while (pos < list.size()) {
        std::size_t comma = list.find(',', pos);
        if (comma == std::string::npos) {
            comma = list.size();
        }
        r.deps.push_back(std::atoi(list.substr(pos, comma - pos).c_str()));
        pos = comma + 1;
    }

    return r;
}

} // namespace

class RenderProfileTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        const char* tmp = std::getenv("TMPDIR");
        std::string pattern = std::string(tmp && *tmp ? tmp : "/tmp") + "/render_profile_test_XXXXXX";
        ASSERT_TRUE(mkdtemp(&pattern[0]) != NULL);
        _dir = pattern;
    }

    virtual void TearDown() OVERRIDE
    {
        RenderScheduler::setProfilePath(std::string());
        getApp()->getProject()->reset(false, true);
        if (!_dir.empty()) {
            std::remove(RenderScheduler::getProfileFilePath().c_str());
            rmdir(_dir.c_str());
        }
        BaseTest::TearDown();
    }

    // Read -> Grade -> Blur -> Merge, with a CheckerBoard source that needs no file on disk.
    void buildScene()
    {
        source = createNode(QString::fromUtf8(kCheckerBoardPluginID));
        grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE), PLUGIN_MAJOR_NATRON_GRADE);
        blur = createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR), PLUGIN_MAJOR_NATRON_BLUR);
        merge = createNode(QString::fromUtf8(PLUGINID_NATRON_MERGE), PLUGIN_MAJOR_NATRON_MERGE);
        ASSERT_TRUE(source && grade && blur && merge);
        connectNodes(source, grade, 0, true);
        connectNodes(grade, blur, 0, true);
        connectNodes(blur, merge, 0, true);
        connectNodes(source, merge, 1, true);
    }

    // Returns the graph that was rendered.
    FrameGraph renderScene()
    {
        const double time = 1.;
        const ViewIdx view(0);
        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
        RenderStatsPtr stats = std::make_shared<RenderStats>(false);
        ParallelRenderArgsSetter frameArgs(time, view, false, false, abortInfo, merge, 0, getApp()->getTimeLine().get(), NodePtr(), false, false, stats);
        std::shared_ptr<FrameRequestMap> request = std::make_shared<FrameRequestMap>();
        EXPECT_NE(EffectInstance::computeRequestPass(time, view, 0, RectD(0., 0., kWindowSize, kWindowSize), merge, *request), eStatusFailed);
        frameArgs.updateNodesRequest(*request);
        FrameRenderContextPtr context = FrameRenderContext::createFromSetter(frameArgs, abortInfo, stats, time, view);
        context->setRequest(request);
        FrameGraph graph = RenderScheduler::buildGraph(context, merge, time, view, 0);
        EXPECT_EQ(graph.tasks.size(), 4u);
        const FrameGraph expected = graph;
        FrameFuturePtr future = appPTR->getRenderScheduler()->submit(context, std::move(graph), RenderScheduler::Priority::Background);
        EXPECT_EQ(future->wait(), EffectInstance::eRenderRoIRetCodeOk);
        RenderScheduler::flushProfile();

        return expected;
    }

    std::string _dir;
    NodePtr source, grade, blur, merge;
};

TEST_F(RenderProfileTest, OneRecordPerTaskWithTheGraphsEdges)
{
    buildScene();
    RenderScheduler::setProfilePath(_dir + "/profile.jsonl");
    const std::string file = RenderScheduler::getProfileFilePath();
    ASSERT_FALSE(file.empty());

    const FrameGraph expected = renderScene();

    std::map<int, Record> records;
    std::ifstream in(file.c_str());
    std::string line;
    while (std::getline(in, line)) {
        const Record r = parseRecord(line);
        EXPECT_TRUE(records.insert(std::make_pair(r.task, r)).second) << "task " << r.task << " recorded twice";
        EXPECT_EQ(r.frame, records.begin()->second.frame);
    }
    ASSERT_EQ(records.size(), expected.tasks.size());
    int gradeTask = -1;
    int blurTask = -1;
    for (std::size_t i = 0; i < expected.tasks.size(); ++i) {
        std::map<int, Record>::const_iterator it = records.find(static_cast<int>(i));
        ASSERT_TRUE(it != records.end()) << "no record for task " << i;
        const FrameGraph::Task& task = expected.tasks[i];
        EXPECT_EQ(it->second.node, task.key.node->getScriptName_mt_safe());
        EXPECT_EQ(it->second.plugin, task.key.node->getPluginID());
        EXPECT_EQ(it->second.deps, task.dependencies);
        EXPECT_GT(it->second.wallNs, 0);
        EXPECT_EQ(it->second.device, "cpu");
        EXPECT_EQ(it->second.regionId, "-1");
        if (task.key.node == grade) {
            EXPECT_TRUE(it->second.pointOp);
            gradeTask = static_cast<int>(i);
        } else if (task.key.node == blur) {
            blurTask = static_cast<int>(i);
        }
    }
    ASSERT_GE(gradeTask, 0);
    ASSERT_GE(blurTask, 0);
    // Stated independently of the graph the profiler read.
    EXPECT_EQ(records[blurTask].deps, std::vector<int>(1, gradeTask));
}

TEST_F(RenderProfileTest, OffWritesNoFile)
{
    buildScene();
    RenderScheduler::setProfilePath(_dir + "/profile.jsonl");
    const std::string file = RenderScheduler::getProfileFilePath();
    RenderScheduler::setProfilePath(std::string());
    EXPECT_TRUE(RenderScheduler::getProfileFilePath().empty());

    renderScene();

    EXPECT_FALSE(std::ifstream(file.c_str()).good());
}

TEST_F(RenderProfileTest, UnwritablePathDoesNotAffectTheRender)
{
    buildScene();
    RenderScheduler::setProfilePath(_dir + "/missing/profile.jsonl");

    renderScene();

    EXPECT_FALSE(std::ifstream(RenderScheduler::getProfileFilePath().c_str()).good());
}

TEST(RenderStatsGpuCounters, AccumulateAndReport)
{
    RenderStats stats(false);

    stats.noteUpload(100);
    stats.noteUpload(50);
    stats.noteDownload(30);
    stats.noteEviction();
    stats.noteGpuRegionRun();
    stats.noteGpuTaskRun("Blur1");
    stats.noteGpuTaskRun("Blur1");
    stats.noteGpuTaskRun("Grade1");
    stats.noteCpuFallback("unsupported");
    stats.notePeakDeviceBytes(500);
    stats.notePeakDeviceBytes(200);

    EXPECT_EQ(stats.getUploadCount(), 2);
    EXPECT_EQ(stats.getUploadBytes(), 150u);
    EXPECT_EQ(stats.getDownloadCount(), 1);
    EXPECT_EQ(stats.getDownloadBytes(), 30u);
    EXPECT_EQ(stats.getEvictions(), 1);
    EXPECT_EQ(stats.getGpuRegionsRun(), 1);
    EXPECT_EQ(stats.getGpuTasksPerNode().at("Blur1"), 2);
    EXPECT_EQ(stats.getGpuTasksPerNode().at("Grade1"), 1);
    EXPECT_EQ(stats.getCpuFallbackReasons().at("unsupported"), 1);
    EXPECT_EQ(stats.getPeakDeviceBytes(), 500u);
}

TEST(RenderStatsGpuCounters, RegionRecordFormat)
{
    std::vector<int> tasks;
    tasks.push_back(1);
    tasks.push_back(2);

    GpuRegionTimes times;
    times.uploadMs = 1.5;
    times.uploadBytes = 64;
    times.kernelMs = 2.25;
    times.downloadMs = 0.5;
    times.downloadBytes = 32;
    times.wallMs = 3.;
    times.strips = 4;
    times.stripsInFlight = 3;
    times.transferHiddenMs = 1.75;
    times.transferExposedMs = 0.25;
    times.planMs = 0.125;
    times.recordMs = 0.375;
    times.acquireMs = 0.0625;
    times.submitWaitMs = 0.875;
    times.poolMisses = 2;
    times.importCount = 5;
    times.newImportBytes = 4096;
    times.predictedTransferMs = 1.25;
    times.overlapStrips = true;
    times.prepareWaitMs = 0.75;
    times.uploadsImported = 6;
    times.uploadsStaged = 1;
    times.downloadsImported = 7;
    times.downloadsStaged = 2;
    times.importsAtUpload = 3;
    times.importsAtDownload = 4;
    GpuTransferRecord upload;
    upload.bytes = 8;
    upload.strip = 1;
    upload.deviceMs = 0.125;
    upload.queueFamily = 3;
    upload.comps = 1;
    times.uploads.push_back(upload);
    GpuTransferRecord first;
    first.bytes = 16;
    first.strip = 0;
    first.deviceMs = 0.25;
    first.queueFamily = 1;
    first.queueIndex = 2;
    first.comps = 4;
    first.staged = true;
    GpuTransferRecord second;
    second.bytes = 16;
    second.pinned = true;
    times.downloads.push_back(first);
    times.downloads.push_back(second);

    EXPECT_EQ(formatRegionRecord(7, 3, tasks, times),
              "{\"kind\":\"region\",\"frame\":7,\"regionId\":3,\"tasks\":[1,2],"
              "\"uploadMs\":1.500000,\"uploadBytes\":64,\"kernelMs\":2.250000,"
              "\"downloadMs\":0.500000,\"downloadBytes\":32,\"wallMs\":3.000000,\"strips\":4,"
              "\"stripsInFlight\":3,\"transferHiddenMs\":1.750000,\"transferExposedMs\":0.250000,"
              "\"planMs\":0.125000,\"recordMs\":0.375000,\"acquireMs\":0.062500,\"submitWaitMs\":0.875000,"
              "\"poolMisses\":2,\"importCount\":5,\"newImportBytes\":4096,\"predictedTransferMs\":1.250000,"
              "\"overlapStrips\":1,\"prepareWaitMs\":0.750000,\"uploadsImported\":6,\"uploadsStaged\":1,"
              "\"downloadsImported\":7,\"downloadsStaged\":2,\"importsAtUpload\":3,\"importsAtDownload\":4,"
              "\"uploads\":[{\"strip\":1,\"bytes\":8,\"deviceMs\":0.125000,\"queueFamily\":3,\"queueIndex\":0,"
              "\"comps\":1,\"format\":\"f32\",\"staged\":0}],"
              "\"downloads\":[{\"strip\":0,\"bytes\":16,\"deviceMs\":0.250000,\"queueFamily\":1,\"queueIndex\":2,"
              "\"comps\":4,\"format\":\"f32\",\"staged\":1,\"pinned\":0},{\"strip\":-1,\"bytes\":16,\"deviceMs\":null,"
              "\"queueFamily\":0,\"queueIndex\":0,\"comps\":0,\"format\":\"f32\",\"staged\":0,\"pinned\":1}],"
              "\"fallback\":\"\",\"retries\":0}\n");

    times.predictedTransferMs = std::numeric_limits<double>::infinity();
    const std::string unpriced = formatRegionRecord(7, 3, tasks, times);
    EXPECT_NE(unpriced.find("\"newImportBytes\":4096,\"predictedTransferMs\":null,\"overlapStrips\":1,\"prepareWaitMs\":"), std::string::npos) << unpriced;
}

TEST(RenderStatsGpuCounters, RegionRecordFormatCarriesTheFallbackAndRetries)
{
    std::vector<int> tasks;
    tasks.push_back(4);

    GpuRegionTimes times;
    times.fallback = "out of device memory";
    times.retries = 3;

    const std::string line = formatRegionRecord(2, 1, tasks, times);
    EXPECT_NE(line.find("\"fallback\":\"out of device memory\",\"retries\":3}\n"), std::string::npos) << line;

    times.fallback = "a \"quoted\" \\ reason";
    EXPECT_NE(formatRegionRecord(2, 1, tasks, times).find("\"fallback\":\"a \\\"quoted\\\" \\\\ reason\""), std::string::npos);
}
