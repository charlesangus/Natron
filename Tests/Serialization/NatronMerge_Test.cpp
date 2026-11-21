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

#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "MergeScenarios.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/Knob.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Project.h"

#include "Serialization/Bundle/ProjectBundle.h"
#include "Serialization/Merge/NodeMatcher.h"
#include "Serialization/Merge/ThreeWayMerge.h"
#include "Serialization/Yaml/BlobSink.h"
#include "Serialization/Yaml/ProjectYaml.h"

NATRON_NAMESPACE_USING
using namespace MergeScenarios;

namespace {

namespace fs = std::filesystem;

class ScratchDir {
public:
    ScratchDir()
    {
        static std::atomic<int> counter(0);
        _path = fs::temp_directory_path() / ("natron-merge-test-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        fs::create_directories(_path);
    }

    ~ScratchDir()
    {
        std::error_code ignored;
        fs::remove_all(_path, ignored);
    }

    std::string file(const std::string& name) const
    {
        return (_path / name).string();
    }

private:
    fs::path _path;
};

std::string
shellQuoted(const std::string& text)
{
    std::string quoted = "'";
    for (char c : text) {
        quoted += (c == '\'') ? std::string("'\\''") : std::string(1, c);
    }

    return quoted + "'";
}

struct RunResult {
    int status = -1;
    std::string out;
};

RunResult
runNatronMerge(const std::vector<std::string>& args)
{
    std::string command = shellQuoted(NATRON_TESTS_NATRON_MERGE);
    for (const std::string& arg : args) {
        command += " " + shellQuoted(arg);
    }
    command += " 2>/dev/null";
    RunResult result;
    FILE* pipe = ::popen(command.c_str(), "r");
    if (!pipe) {
        return result;
    }
    char buffer[4096];
    std::size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
        result.out.append(buffer, read);
    }
    const int status = ::pclose(pipe);
    result.status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    return result;
}

void
writeBundle(const Serialization::ProjectSerialization& project,
            const std::string& path)
{
    Serialization::ProjectBundleWriter writer;
    writer.setProject(project);
    writer.setRootFolder(fs::path(path).stem().string());
    writer.write(path);
}

Serialization::ProjectSerialization
readBundle(const std::string& path)
{
    return Serialization::ProjectBundleReader(path).readProject();
}

void
canonicalizeIds(std::list<Serialization::NodeSerializationPtr>* nodes,
                const std::string& prefix)
{
    for (Serialization::NodeSerializationPtr& node : *nodes) {
        node = std::make_shared<Serialization::NodeSerialization>(*node);
        const std::string path = prefix.empty() ? node->scriptName : prefix + "." + node->scriptName;
        node->id = hexId("merged:" + path);
        canonicalizeIds(&node->children, path);
    }
}

// The merged graph as text, with the IDs a merge makes up replaced by ones named after each node's
// path and the conflicts, compared on their own, left out.
std::string
graphText(Serialization::ProjectSerialization project)
{
    canonicalizeIds(&project.nodes.nodes, std::string());
    project.mergeConflicts.clear();
    Serialization::InlineBlobSink sink;

    return Serialization::encodeProjectYaml(project, sink);
}

std::set<ExpectedConflict>
conflictsByBasePath(const std::vector<Serialization::MergeConflict>& conflicts,
                    const Serialization::ProjectSerialization& base)
{
    const Serialization::MergeGraph graph(base);
    std::set<ExpectedConflict> got;
    for (const Serialization::MergeConflict& conflict : conflicts) {
        ExpectedConflict entry;
        const Serialization::MergeGraph::Node* node = graph.find(conflict.baseNodeId);
        entry.node = node ? node->path : conflict.baseNodeId;
        entry.kind = std::string(Serialization::mergeConflictKindName(conflict.kind));
        entry.knob = conflict.knob;
        entry.dimension = conflict.dimension;
        entry.time = conflict.time;
        entry.item = conflict.item;
        entry.input = conflict.input;
        entry.defaulted = conflict.defaultedKnobs;
        got.insert(entry);
    }

    return got;
}

// A project with the template's nodes as its own, edited by edit.
Serialization::ProjectSerialization
plainProject(const std::function<void(SimGraph*)>& edit = std::function<void(SimGraph*)>())
{
    SimGraph graph = templateGraph("b:");
    if (edit) {
        edit(&graph);
    }

    return graph.project();
}

bool
hasOrigin(const std::list<Serialization::NodeSerializationPtr>& nodes)
{
    for (const Serialization::NodeSerializationPtr& node : nodes) {
        if (node->origin.isSet() || hasOrigin(node->children)) {
            return true;
        }
    }

    return false;
}

TEST(NatronMerge, EveryScenarioMergesAsThroughTheApi)
{
    const std::string expectedDir = std::string(NATRON_TESTS_FIXTURES_DIR) + "/merge-scenarios/expected/";
    for (const std::pair<const std::string, Scenario>& scenario : scenarios()) {
        const Serialization::ProjectSerialization base = scenario.second.base.project();
        const Serialization::ProjectSerialization mine = scenario.second.mine.project();
        const Serialization::ProjectSerialization theirs = scenario.second.theirs.project();
        ScratchDir dir;
        writeBundle(base, dir.file("base.ntpz"));
        writeBundle(mine, dir.file("mine.ntpz"));
        writeBundle(theirs, dir.file("theirs.ntpz"));
        for (const ExpectedMerge& merge : readExpected(expectedDir + scenario.first + ".yaml")) {
            SCOPED_TRACE(scenario.first + " [" + merge.templateKey + "]");
            const std::string out = dir.file("out-" + merge.templateKey + ".ntpz");
            const RunResult run = runNatronMerge({ "merge", "--scope", "template:" + merge.templateKey, dir.file("base.ntpz"), dir.file("mine.ntpz"), dir.file("theirs.ntpz"), "-o", out });
            EXPECT_EQ(run.status, merge.conflicts.empty() ? 0 : 1);
            ASSERT_TRUE(fs::exists(out));
            const Serialization::ProjectSerialization merged = readBundle(out);
            EXPECT_TRUE(conflictsByBasePath(merged.mergeConflicts, base) == merge.conflicts);

            const Serialization::MergeResult api = Serialization::threeWayMerge(base, mine, theirs, Serialization::planMerge(base, mine, theirs, merge.templateKey));
            const std::string apiOut = dir.file("api-" + merge.templateKey + ".ntpz");
            writeBundle(api.merged, apiOut);
            EXPECT_EQ(graphText(merged), graphText(readBundle(apiOut)));
        }
    }
}

TEST(NatronMerge, AWholeProjectMergeTakesBothSidesEdits)
{
    ScratchDir dir;
    writeBundle(plainProject(), dir.file("base.ntpz"));
    writeBundle(plainProject([](SimGraph* g) {
                    g->rename("Grade1", "KeyGrade");
                    g->set("KeyGrade", "multiply", { 1.5 });
                }),
                dir.file("mine.ntpz"));
    writeBundle(plainProject([](SimGraph* g) {
                    g->set("Blur1", "size", { 4., 4. });
                    g->add("Extra", "Blur", "t:Extra", { { "Source", "ColorCorrect1" } });
                }),
                dir.file("theirs.ntpz"));
    const std::string out = dir.file("out.ntpz");
    const RunResult run = runNatronMerge({ "merge", dir.file("base.ntpz"), dir.file("mine.ntpz"), dir.file("theirs.ntpz"), "-o", out });
    EXPECT_EQ(run.status, 0);
    const Serialization::ProjectSerialization merged = readBundle(out);
    EXPECT_TRUE(merged.mergeConflicts.empty());
    EXPECT_FALSE(hasOrigin(merged.nodes.nodes));
    Serialization::NodeSerializationPtr grade = nodeAt(merged, "KeyGrade");
    ASSERT_TRUE(grade != nullptr);
    EXPECT_EQ(grade->id, hexId("b:Grade1"));
    EXPECT_EQ(valuesOf(*grade, "multiply"), std::vector<double>({ 1.5 }));
    Serialization::NodeSerializationPtr blur = nodeAt(merged, "Blur1");
    ASSERT_TRUE(blur != nullptr);
    EXPECT_EQ(valuesOf(*blur, "size"), std::vector<double>({ 4., 4. }));
    Serialization::NodeSerializationPtr extra = nodeAt(merged, "Extra");
    ASSERT_TRUE(extra != nullptr);
    EXPECT_EQ(extra->id, hexId("t:Extra"));
    EXPECT_EQ(extra->inputs["Source"], "ColorCorrect1");
}

TEST(NatronMerge, AWholeProjectConflictKeepsMineAndExitsOne)
{
    ScratchDir dir;
    writeBundle(plainProject(), dir.file("base.ntpz"));
    writeBundle(plainProject([](SimGraph* g) { g->set("Blur1", "size", { 3., 3. }); }), dir.file("mine.ntpz"));
    writeBundle(plainProject([](SimGraph* g) { g->set("Blur1", "size", { 4., 4. }); }), dir.file("theirs.ntpz"));
    const std::string out = dir.file("out.ntpz");
    const RunResult run = runNatronMerge({ "merge", "--scope", "all", dir.file("base.ntpz"), dir.file("mine.ntpz"), dir.file("theirs.ntpz"), "-o", out });
    EXPECT_EQ(run.status, 1);
    const Serialization::ProjectSerialization merged = readBundle(out);
    ASSERT_FALSE(merged.mergeConflicts.empty());
    for (const Serialization::MergeConflict& conflict : merged.mergeConflicts) {
        EXPECT_EQ(conflict.kind, Serialization::MergeConflictKind::eValue);
        EXPECT_EQ(conflict.knob, "size");
        EXPECT_EQ(conflict.baseNodeId, hexId("b:Blur1"));
    }
    Serialization::NodeSerializationPtr blur = nodeAt(merged, "Blur1");
    ASSERT_TRUE(blur != nullptr);
    EXPECT_EQ(valuesOf(*blur, "size"), std::vector<double>({ 3., 3. }));
}

TEST(NatronMerge, TheGitDriverMergesIntoTheCurrentFile)
{
    ScratchDir dir;
    writeBundle(plainProject(), dir.file("O"));
    writeBundle(plainProject([](SimGraph* g) { g->set("Grade1", "multiply", { 1.5 }); }), dir.file("A"));
    writeBundle(plainProject([](SimGraph* g) { g->set("Blur1", "size", { 4., 4. }); }), dir.file("B"));
    const RunResult run = runNatronMerge({ "git-driver", dir.file("O"), dir.file("A"), dir.file("B") });
    EXPECT_EQ(run.status, 0);
    const Serialization::ProjectSerialization merged = readBundle(dir.file("A"));
    Serialization::NodeSerializationPtr grade = nodeAt(merged, "Grade1");
    Serialization::NodeSerializationPtr blur = nodeAt(merged, "Blur1");
    ASSERT_TRUE(grade != nullptr);
    ASSERT_TRUE(blur != nullptr);
    EXPECT_EQ(valuesOf(*grade, "multiply"), std::vector<double>({ 1.5 }));
    EXPECT_EQ(valuesOf(*blur, "size"), std::vector<double>({ 4., 4. }));
}

TEST(NatronMerge, DiffPrintsChangesPerNode)
{
    ScratchDir dir;
    writeBundle(plainProject(), dir.file("before.ntpz"));
    writeBundle(plainProject([](SimGraph* g) {
                    g->rename("Grade1", "KeyGrade");
                    g->set("KeyGrade", "multiply", { 1.5 });
                    g->add("Blur2", "Blur", "a:Blur2", { { "Source", "KeyGrade" } });
                    g->get("Merge1").inputs["B"] = "Blur2";
                }),
                dir.file("after.ntpz"));
    const RunResult run = runNatronMerge({ "diff", dir.file("before.ntpz"), dir.file("after.ntpz") });
    EXPECT_EQ(run.status, 1);
    EXPECT_NE(run.out.find("~ KeyGrade (renamed from Grade1)\n"), std::string::npos) << run.out;
    EXPECT_NE(run.out.find("  multiply: 1 → 1.5\n"), std::string::npos) << run.out;
    EXPECT_NE(run.out.find("+ Blur2 (Blur)\n"), std::string::npos) << run.out;
    EXPECT_NE(run.out.find("  input B: Blur1 → Blur2\n"), std::string::npos) << run.out;

    const RunResult yaml = runNatronMerge({ "diff", "--yaml", dir.file("before.ntpz"), dir.file("after.ntpz") });
    EXPECT_EQ(yaml.status, 1);
    EXPECT_FALSE(yaml.out.empty());

    const RunResult same = runNatronMerge({ "diff", dir.file("before.ntpz"), dir.file("before.ntpz") });
    EXPECT_EQ(same.status, 0);
    EXPECT_EQ(same.out, "");
}

TEST(NatronMerge, BadArgumentsExitTwo)
{
    ScratchDir dir;
    writeBundle(plainProject(), dir.file("base.ntpz"));
    const std::string base = dir.file("base.ntpz");
    EXPECT_EQ(runNatronMerge({}).status, 2);
    EXPECT_EQ(runNatronMerge({ "frobnicate" }).status, 2);
    EXPECT_EQ(runNatronMerge({ "diff", base }).status, 2);
    EXPECT_EQ(runNatronMerge({ "diff", base, dir.file("missing.ntpz") }).status, 2);
    EXPECT_EQ(runNatronMerge({ "merge", base, base, base }).status, 2);
    EXPECT_EQ(runNatronMerge({ "merge", "--scope", "template:", base, base, base, "-o", dir.file("out.ntpz") }).status, 2);
    EXPECT_EQ(runNatronMerge({ "merge", "--scope", "everything", base, base, base, "-o", dir.file("out.ntpz") }).status, 2);
    std::ofstream(dir.file("junk.ntpz")) << "not a project";
    EXPECT_EQ(runNatronMerge({ "textconv", dir.file("junk.ntpz") }).status, 2);
    EXPECT_FALSE(fs::exists(dir.file("out.ntpz")));
}

class NatronMergeProjectTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        project()->reset(false, true);
    }

    virtual void TearDown() OVERRIDE
    {
        project()->reset(false, true);
        BaseTest::TearDown();
    }

    ProjectPtr project() const
    {
        return getApp()->getProject();
    }

    NodePtr create(const char* pluginID,
                   const std::string& name)
    {
        CreateNodeArgs args(pluginID, project());
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
        args.setProperty<std::string>(kCreateNodeArgsPropNodeInitialName, name);
        NodePtr node = getApp()->createNode(args);
        if (node && (node->getScriptName() != name)) {
            node->setScriptName(name);
        }

        return node;
    }
};

TEST_F(NatronMergeProjectTest, ResavingAnUnchangedProjectGivesAnEmptyDiff)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    NodePtr grade = create(PLUGINID_NATRON_GRADE, "Grade1");
    NodePtr blur = create(PLUGINID_NATRON_BLUR, "Blur1");
    ASSERT_TRUE(grade != nullptr);
    ASSERT_TRUE(blur != nullptr);
    ASSERT_TRUE(NodeCollection::connectNodes(0, grade, blur, false));
    KnobIPtr multiply = grade->getKnobByName("multiply");
    ASSERT_TRUE(multiply != nullptr);
    multiply->setExpression(0, "Blur1.size.getValue(0)", false, false);
    const QString folder = dir.path() + QLatin1Char('/');
    QString first;
    ASSERT_TRUE(project()->saveProject(folder, QString::fromUtf8("shot.ntpz"), &first));
    ASSERT_TRUE(project()->loadProject(folder, QString::fromUtf8("shot.ntpz")));
    QString second;
    ASSERT_TRUE(project()->saveProject(folder, QString::fromUtf8("shot_resaved.ntpz"), &second));

    const RunResult diff = runNatronMerge({ "diff", first.toStdString(), second.toStdString() });
    EXPECT_EQ(diff.status, 0);
    EXPECT_EQ(diff.out, "");
    const RunResult before = runNatronMerge({ "textconv", first.toStdString() });
    const RunResult after = runNatronMerge({ "textconv", second.toStdString() });
    EXPECT_EQ(before.status, 0);
    EXPECT_EQ(after.status, 0);
    EXPECT_NE(before.out.find("Blur1"), std::string::npos);
}

} // namespace
