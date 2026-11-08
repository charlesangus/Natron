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

#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "GraphOpTestHelpers.h"
#include "Serialization/Ops/ApplyOps.h"
#include "Serialization/Ops/SnapshotDiff.h"
#include "Serialization/Yaml/GraphOpYaml.h"

NATRON_NAMESPACE_USING
using namespace Serialization;
using GraphOpTest::nid;
using GraphOpTest::projectFromYaml;

namespace {

// Read1 -> Blur1 -> Merge1.B, Constant1 -> Merge1.A, Merge1 -> Group1 (Input1 -> Grade1 -> Output1) -> Viewer1
const char* const kGraph = R"yaml(knobs:
  frameRate: 25
nodes:
  Read1:
    id: "@1"
    plugin: "Read"
    version: [1, 0]
    pos: [0, 0]
    knobs:
      filename: "plate.####.exr"
  Blur1:
    id: "@2"
    plugin: "Blur"
    version: [1, 0]
    pos: [0, 100]
    knobs:
      size: 3.5
    inputs: {Source: "Read1"}
  Constant1:
    id: "@3"
    plugin: "Constant"
    version: [1, 0]
    pos: [200, 0]
  Merge1:
    id: "@4"
    plugin: "Merge"
    version: [1, 0]
    pos: [0, 200]
    inputs: {A: "Constant1", B: "Blur1"}
  Group1:
    id: "@5"
    plugin: "Group"
    version: [1, 0]
    pos: [0, 300]
    inputs: {Input1: "Merge1"}
    children:
      Input1:
        id: "@6"
        plugin: "Input"
        version: [1, 0]
      Grade1:
        id: "@7"
        plugin: "Grade"
        version: [1, 0]
        knobs:
          gamma: 1.2
        inputs: {Source: "Input1"}
      Output1:
        id: "@8"
        plugin: "Output"
        version: [1, 0]
        inputs: {Source: "Grade1"}
  Viewer1:
    id: "@9"
    plugin: "Viewer"
    version: [1, 0]
    inputs: {"1": "Group1"}
)yaml";

// kGraph with each (from, to) replacement made in turn, each on its first occurrence.
std::string
edited(const std::vector<std::pair<std::string, std::string>>& edits)
{
    std::string text = kGraph;
    for (const std::pair<std::string, std::string>& edit : edits) {
        const std::size_t at = text.find(edit.first);
        EXPECT_NE(at, std::string::npos) << edit.first;
        if (at != std::string::npos) {
            text.replace(at, edit.first.size(), edit.second);
        }
    }

    return text;
}

void
applyExpectingSuccess(ProjectSerialization* project,
                      const std::vector<GraphOp>& ops)
{
    const std::vector<OpResult> results = applyOps(project, ops);
    ASSERT_EQ(results.size(), ops.size());
    for (std::size_t i = 0; i < results.size(); ++i) {
        EXPECT_TRUE(results[i].applied()) << "operation " << i << " (" << graphOpName(ops[i]) << "): " << results[i].message;
    }
}

// The diff, after checking that it is stable and that applying it, directly or through YAML,
// turns before into after.
std::vector<GraphOp>
checkedDiff(const ProjectSerialization& before,
            const ProjectSerialization& after)
{
    const std::vector<GraphOp> ops = diffSnapshots(before, after);
    const std::string text = encodeGraphOpsYaml(ops);
    EXPECT_EQ(text, encodeGraphOpsYaml(diffSnapshots(before, after)));

    ProjectSerialization applied = before;
    applyExpectingSuccess(&applied, ops);
    EXPECT_SAME_PROJECT(after, applied) << text;
    EXPECT_TRUE(diffSnapshots(applied, after).empty());

    std::vector<GraphOp> decoded;
    for (const DecodedGraphOp& entry : decodeGraphOpsYaml(text)) {
        EXPECT_TRUE(entry.op.has_value()) << entry.error;
        if (entry.op) {
            decoded.push_back(*entry.op);
        }
    }
    ProjectSerialization appliedFromYaml = before;
    applyExpectingSuccess(&appliedFromYaml, decoded);
    EXPECT_SAME_PROJECT(after, appliedFromYaml);

    return ops;
}

std::vector<GraphOp>
checkedDiff(const std::string& before,
            const std::string& after)
{
    return checkedDiff(projectFromYaml(before), projectFromYaml(after));
}

std::vector<std::string>
names(const std::vector<GraphOp>& ops)
{
    std::vector<std::string> result;
    for (const GraphOp& op : ops) {
        result.push_back(std::string(graphOpName(op)));
    }

    return result;
}

NodeSerializationPtr
findNode(const std::list<NodeSerializationPtr>& nodes,
         const std::string& scriptName)
{
    for (const NodeSerializationPtr& node : nodes) {
        if (node->scriptName == scriptName) {
            return node;
        }
    }

    return NodeSerializationPtr();
}

} // namespace

TEST(SnapshotDiff, IdenticalSnapshotsGiveNoOperation)
{
    EXPECT_TRUE(diffSnapshots(projectFromYaml(kGraph), projectFromYaml(kGraph)).empty());
    EXPECT_TRUE(diffSnapshots(ProjectSerialization(), ProjectSerialization()).empty());

    ProjectSerialization other = projectFromYaml(kGraph);
    other.projectId = "something else";
    other.timelineCurrentTime = 42;
    findNode(other.nodes.nodes, "Blur1")->cacheID = "cache";
    EXPECT_TRUE(diffSnapshots(projectFromYaml(kGraph), other).empty());
}

TEST(SnapshotDiff, KnobChanges)
{
    const std::vector<GraphOp> ops = checkedDiff(kGraph, edited({
                                                             { "      size: 3.5\n", "      size: {expr: \"frame / 10\", hasRet: false}\n      filter: \"Box\"\n" },
                                                             { "    knobs:\n      filename: \"plate.####.exr\"\n", "" },
                                                             { "  frameRate: 25\n", "  frameRate: 24\n" },
                                                         }));
    EXPECT_EQ(names(ops), (std::vector<std::string> { "setKnob", "setKnob", "setKnob", "setKnob" }));
    EXPECT_FALSE(std::get<SetKnobOp>(ops[0]).value.has_value()) << "filename goes back to its default";
}

TEST(SnapshotDiff, NodeAddedAndConnected)
{
    const std::vector<GraphOp> ops = checkedDiff(kGraph, edited({
                                                             { "    inputs: {Source: \"Read1\"}\n", "    inputs: {Source: \"Dot1\"}\n" },
                                                             { "  Viewer1:\n", "  Dot1:\n    id: \"@20\"\n    plugin: \"Dot\"\n    version: [1, 0]\n    pos: [0, 50]\n    inputs: {Source: \"Read1\"}\n  Viewer1:\n" },
                                                         }));
    ASSERT_EQ(names(ops), (std::vector<std::string> { "createNode", "setInput", "setInput" }));
    EXPECT_TRUE(std::get<CreateNodeOp>(ops[0]).node->inputs.empty()) << "connections come as setInput";
    EXPECT_EQ(std::get<SetInputOp>(ops[1]).nodeId, nid(2));
    EXPECT_EQ(std::get<SetInputOp>(ops[1]).sourceId, nid(20));
    EXPECT_EQ(std::get<SetInputOp>(ops[2]).nodeId, nid(20));
}

TEST(SnapshotDiff, NodeDeleted)
{
    const std::vector<GraphOp> ops = checkedDiff(kGraph, edited({
                                                             { "  Constant1:\n    id: \"@3\"\n    plugin: \"Constant\"\n    version: [1, 0]\n    pos: [200, 0]\n", "" },
                                                             { "{A: \"Constant1\", B: \"Blur1\"}", "{B: \"Blur1\"}" },
                                                         }));
    EXPECT_EQ(names(ops), (std::vector<std::string> { "deleteNode" })) << "deleting disconnects Merge1.A";
}

TEST(SnapshotDiff, GroupDeletedWithItsChildren)
{
    std::string after = kGraph;
    after.erase(after.find("  Group1:\n"), after.find("  Viewer1:\n") - after.find("  Group1:\n"));
    after.replace(after.find("{\"1\": \"Group1\"}"), std::string("{\"1\": \"Group1\"}").size(), "{\"1\": \"Merge1\"}");
    const std::vector<GraphOp> ops = checkedDiff(kGraph, after);
    ASSERT_EQ(names(ops), (std::vector<std::string> { "deleteNode", "setInput" }));
    EXPECT_EQ(std::get<DeleteNodeOp>(ops[0]).nodeId, nid(5));
}

TEST(SnapshotDiff, RenameAndRelabelKeepConnections)
{
    const std::vector<GraphOp> ops = checkedDiff(kGraph, edited({
                                                             { "  Blur1:\n", "  Soften:\n    label: \"Soft\"\n" },
                                                             { "{A: \"Constant1\", B: \"Blur1\"}", "{A: \"Constant1\", B: \"Soften\"}" },
                                                             { "      Grade1:\n", "      Look:\n" },
                                                             { "{Source: \"Grade1\"}", "{Source: \"Look\"}" },
                                                         }));
    ASSERT_EQ(names(ops), (std::vector<std::string> { "rename", "rename" }));
    EXPECT_EQ(std::get<RenameOp>(ops[0]).scriptName, "Soften");
    EXPECT_EQ(std::get<RenameOp>(ops[0]).label, std::optional<std::string>("Soft"));
}

TEST(SnapshotDiff, SwappedNamesGoThroughATemporaryName)
{
    const std::vector<GraphOp> ops = checkedDiff(kGraph, edited({
                                                             { "  Read1:\n", "  TEMP_A:\n" },
                                                             { "  Constant1:\n", "  Read1:\n" },
                                                             { "  TEMP_A:\n", "  Constant1:\n" },
                                                             { "{Source: \"Read1\"}", "{Source: \"Constant1\"}" },
                                                             { "{A: \"Constant1\", B: \"Blur1\"}", "{A: \"Read1\", B: \"Blur1\"}" },
                                                         }));
    EXPECT_EQ(names(ops), (std::vector<std::string> { "rename", "rename", "rename" }));
}

TEST(SnapshotDiff, PluginChangeRecreatesTheNodeWithItsId)
{
    const std::vector<GraphOp> ops = checkedDiff(kGraph, edited({
                                                             { "    plugin: \"Blur\"\n", "    plugin: \"Defocus\"\n" },
                                                         }));
    ASSERT_EQ(names(ops), (std::vector<std::string> { "deleteNode", "createNode", "setInput", "setInput" }));
    EXPECT_EQ(std::get<DeleteNodeOp>(ops[0]).nodeId, nid(2));
    EXPECT_EQ(std::get<CreateNodeOp>(ops[1]).node->id, nid(2));
}

TEST(SnapshotDiff, GroupCreatedWithChildrenAndNodeAddedInsideAGroup)
{
    const std::vector<GraphOp> ops = checkedDiff(kGraph, edited({
                                                             { "{Source: \"Grade1\"}", "{Source: \"Sharpen1\"}" },
                                                             { "      Output1:\n", "      Sharpen1:\n        id: \"@33\"\n        plugin: \"Sharpen\"\n        version: [1, 0]\n        inputs: {Source: \"Grade1\"}\n      Output1:\n" },
                                                             { "  Viewer1:\n", R"yaml(  Group2:
    id: "@30"
    plugin: "Group"
    version: [1, 0]
    inputs: {Input1: "Read1"}
    children:
      Input1:
        id: "@31"
        plugin: "Input"
        version: [1, 0]
      Output1:
        id: "@32"
        plugin: "Output"
        version: [1, 0]
        inputs: {Source: "Input1"}
  Viewer1:
)yaml" },
                                                         }));
    std::vector<std::string> expected { "createNode", "createNode" };
    expected.resize(2 + 4, "setInput");
    ASSERT_EQ(names(ops), expected);
    EXPECT_EQ(std::get<CreateNodeOp>(ops[0]).node->id, nid(30)) << "top level first";
    EXPECT_EQ(std::get<CreateNodeOp>(ops[1]).parentId, nid(5));
}

TEST(SnapshotDiff, NodeMovedIntoAGroup)
{
    std::string after = kGraph;
    const std::string constant = "  Constant1:\n    id: \"@3\"\n    plugin: \"Constant\"\n    version: [1, 0]\n    pos: [200, 0]\n";
    after.erase(after.find(constant), constant.size());
    after.replace(after.find("{A: \"Constant1\", B: \"Blur1\"}"), std::string("{A: \"Constant1\", B: \"Blur1\"}").size(), "{B: \"Blur1\"}");
    after.replace(after.find("      Output1:\n"), std::string("      Output1:\n").size(),
                  "      Constant1:\n        id: \"@3\"\n        plugin: \"Constant\"\n        version: [1, 0]\n      Output1:\n");
    const std::vector<GraphOp> ops = checkedDiff(kGraph, after);
    ASSERT_EQ(names(ops), (std::vector<std::string> { "deleteNode", "createNode" }));
    EXPECT_EQ(std::get<CreateNodeOp>(ops[1]).parentId, nid(5));
}

TEST(SnapshotDiff, AppearanceChanges)
{
    const std::vector<GraphOp> ops = checkedDiff(kGraph, edited({
                                                             { "    pos: [0, 100]\n", "    pos: [40, 100]\n    size: [120, 40]\n    color: [0.5, 0.25, 1]\n" },
                                                             { "    pos: [200, 0]\n", "    pos: [200, 0]\n    overlay: [1, 0, 0]\n" },
                                                         }));
    ASSERT_EQ(names(ops), (std::vector<std::string> { "setNodeUi", "setNodeUi" }));

    const std::vector<GraphOp> back = checkedDiff(projectFromYaml(edited({ { "    pos: [0, 100]\n", "    pos: [0, 100]\n    color: [0.5, 0.25, 1]\n" } })), projectFromYaml(kGraph));
    ASSERT_EQ(back.size(), 1u);
    const SetNodeUiOp& cleared = std::get<SetNodeUiOp>(back[0]);
    ASSERT_TRUE(cleared.color.has_value());
    EXPECT_FALSE(cleared.color->has_value());
    EXPECT_FALSE(cleared.position.has_value());
}

TEST(SnapshotDiff, InputsRewired)
{
    const std::vector<GraphOp> ops = checkedDiff(kGraph, edited({
                                                             { "{A: \"Constant1\", B: \"Blur1\"}", "{A: \"Blur1\", B: \"Constant1\", Mask: \"Read1\"}" },
                                                             { "    inputs: {\"1\": \"Group1\"}\n", "    inputs: {\"2\": \"Group1\"}\n" },
                                                         }));
    EXPECT_EQ(names(ops), (std::vector<std::string> { "setInput", "setInput", "setInput", "setInput", "setInput" }));
}

TEST(SnapshotDiff, UserKnobsRotoTrackerAndProjectData)
{
    const std::string userPage = R"yaml(    userPages:
      Controls:
        group: {label: "Controls"}
        children:
          amount:
            value: 0.75
            user:
              type: "Double"
              dimension: 1
              label: "Amount"
              newLine: true
              evaluates: true
              animates: true
              persistent: true
              default: 0.5
              range: [0, 1, 0, 1]
    pages: ["Controls", "Node"]
)yaml";
    ProjectSerialization after = projectFromYaml(edited({
        { "    inputs: {Input1: \"Merge1\"}\n", "    inputs: {Input1: \"Merge1\"}\n" + userPage },
        { "  frameRate: 25\n", "  frameRate: 25\nformats:\n  - {name: \"Square\", rect: [0, 0, 512, 512], par: 1}\nlayers:\n  zdepth: {label: \"Depth\", channelsLabel: \"Z\", channels: [\"Z\"], origin: 1}\n" },
    }));
    NodeSerializationPtr blur = findNode(after.nodes.nodes, "Blur1");
    blur->hasRotoContext = true;
    blur->rotoContext.baseLayer.name = "Layer1";
    blur->rotoContext.baseLayer.label = "Layer1";
    blur->rotoContext.baseLayer.activated = true;
    NodeSerializationPtr read = findNode(after.nodes.nodes, "Read1");
    read->hasTrackerContext = true;
    TrackSerialization track;
    track.scriptName = "track1";
    track.label = "track1";
    read->trackerContext.tracks.push_back(track);

    const std::vector<GraphOp> ops = checkedDiff(projectFromYaml(kGraph), after);
    EXPECT_EQ(names(ops), (std::vector<std::string> { "setTrackerData", "setRotoData", "setUserKnobs", "setProjectData" }));

    const std::vector<GraphOp> back = checkedDiff(after, projectFromYaml(kGraph));
    EXPECT_EQ(names(back), (std::vector<std::string> { "setTrackerData", "setRotoData", "setUserKnobs", "setProjectData" }));
}
