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
