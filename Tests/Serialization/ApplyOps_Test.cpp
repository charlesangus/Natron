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

#include <list>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "GraphOpTestHelpers.h"
#include "Serialization/Ops/ApplyOps.h"
#include "Serialization/Yaml/KnobYaml.h"

NATRON_NAMESPACE_USING
using namespace Serialization;
using GraphOpTest::nid;
using GraphOpTest::projectFromYaml;

namespace {

// Read1 -> Blur1 -> Group1 (Input1 -> Grade1 -> Output1) -> Viewer1
const char* const kBaseGraph = R"yaml(knobs:
  frameRate: 25
nodes:
  Read1:
    id: "@1"
    plugin: "Read"
    version: [1, 0]
    knobs:
      filename: "plate.####.exr"
  Blur1:
    id: "@2"
    plugin: "Blur"
    version: [1, 0]
    pos: [10, 20]
    knobs:
      size: 3.5
    inputs: {Source: "Read1"}
  Group1:
    id: "@3"
    plugin: "Group"
    version: [1, 0]
    inputs: {Input1: "Blur1"}
    children:
      Input1:
        id: "@4"
        plugin: "Input"
        version: [1, 0]
      Grade1:
        id: "@5"
        plugin: "Grade"
        version: [1, 0]
        inputs: {Source: "Input1"}
      Output1:
        id: "@6"
        plugin: "Output"
        version: [1, 0]
        inputs: {Source: "Grade1"}
  Viewer1:
    id: "@7"
    plugin: "Viewer"
    version: [1, 0]
    inputs: {"1": "Group1"}
)yaml";

ProjectSerialization
base()
{
    return projectFromYaml(kBaseGraph);
}

// base() with edit applied to its text: replace the first occurrence of from with to.
ProjectSerialization
baseWith(const std::string& from,
         const std::string& to)
{
    std::string text = kBaseGraph;
    const std::size_t at = text.find(from);
    EXPECT_NE(at, std::string::npos) << from;
    if (at != std::string::npos) {
        text.replace(at, from.size(), to);
    }

    return projectFromYaml(text);
}

std::vector<OpResult>
apply(ProjectSerialization* project,
      const std::vector<GraphOp>& ops)
{
    std::vector<OpResult> results = applyOps(project, ops);
    EXPECT_EQ(results.size(), ops.size());

    return results;
}

void
expectAllApplied(const std::vector<OpResult>& results)
{
    for (std::size_t i = 0; i < results.size(); ++i) {
        EXPECT_TRUE(results[i].applied()) << "operation " << i << ": " << results[i].message;
    }
}

KnobSerialization
knobFromYaml(const std::string& text)
{
    std::list<KnobSerialization> knobs = decodeKnobsYaml(text);
    EXPECT_EQ(knobs.size(), 1u);

    return knobs.front();
}

NodeSerializationPtr
plainNode(int id,
          const std::string& scriptName,
          const std::string& pluginID)
{
    NodeSerializationPtr node = std::make_shared<NodeSerialization>();
    node->id = nid(id);
    node->scriptName = scriptName;
    node->label = scriptName;
    node->pluginID = pluginID;
    node->pluginMajorVersion = 1;

    return node;
}

} // namespace

TEST(ApplyOps, NoOperationsChangeNothing)
{
    ProjectSerialization project = base();
    EXPECT_TRUE(apply(&project, {}).empty());
    EXPECT_SAME_PROJECT(base(), project);
}

TEST(ApplyOps, CreateNodeAtTopLevelAndInGroup)
{
    ProjectSerialization project = base();
    NodeSerializationPtr dot = plainNode(20, "Dot1", "Dot");
    dot->inputs["Source"] = "Read1";
    NodeSerializationPtr inner = plainNode(21, "Group2", "Group");
    NodeSerializationPtr innerInput = plainNode(22, "Input1", "Input");
    NodeSerializationPtr innerOutput = plainNode(23, "Output1", "Output");
    innerOutput->inputs["Source"] = "Input1";
    inner->children = { innerInput, innerOutput };
    expectAllApplied(apply(&project, { CreateNodeOp { std::string(), dot }, CreateNodeOp { nid(3), inner } }));
    EXPECT_EQ(dot->inputs.size(), 1u) << "the operation's node is copied, not taken";

    std::string expected = kBaseGraph;
    expected += R"yaml(  Dot1:
    id: "@20"
    plugin: "Dot"
    version: [1, 0]
)yaml";
    expected.replace(expected.find("      Output1:\n"), std::string("      Output1:\n").size(), R"yaml(      Group2:
        id: "@21"
        plugin: "Group"
        version: [1, 0]
        children:
          Input1:
            id: "@22"
            plugin: "Input"
            version: [1, 0]
          Output1:
            id: "@23"
            plugin: "Output"
            version: [1, 0]
      Output1:
)yaml");
    EXPECT_SAME_PROJECT(projectFromYaml(expected), project);
}

TEST(ApplyOps, CreateNodeRefusals)
{
    ProjectSerialization project = base();
    NodeSerializationPtr reusedId = plainNode(2, "Dot1", "Dot");
    NodeSerializationPtr takenName = plainNode(20, "Blur1", "Dot");
    NodeSerializationPtr malformed = plainNode(20, "Dot1", "Dot");
    malformed->id = "not-an-id";
    NodeSerializationPtr childReusesId = plainNode(20, "Group2", "Group");
    childReusesId->children.push_back(plainNode(5, "Input1", "Input"));
    NodeSerializationPtr twice = plainNode(20, "Group2", "Group");
    twice->children.push_back(plainNode(20, "Input1", "Input"));

    const std::vector<OpResult> results = apply(&project, {
                                                              CreateNodeOp { std::string(), reusedId },
                                                              CreateNodeOp { std::string(), takenName },
                                                              CreateNodeOp { std::string(), malformed },
                                                              CreateNodeOp { std::string(), childReusesId },
                                                              CreateNodeOp { std::string(), twice },
                                                              CreateNodeOp { nid(99), plainNode(20, "Dot1", "Dot") },
                                                              CreateNodeOp { std::string(), NodeSerializationPtr() },
                                                          });
    EXPECT_EQ(results[0].status, OpStatus::eInvalid);
    EXPECT_EQ(results[1].status, OpStatus::eInvalid);
    EXPECT_EQ(results[2].status, OpStatus::eInvalid);
    EXPECT_EQ(results[3].status, OpStatus::eInvalid);
    EXPECT_EQ(results[4].status, OpStatus::eInvalid);
    EXPECT_EQ(results[5].status, OpStatus::eTargetMissing);
    EXPECT_EQ(results[6].status, OpStatus::eInvalid);
    EXPECT_SAME_PROJECT(base(), project);
}

TEST(ApplyOps, DeleteNodeTakesItsChildrenAndConnections)
{
    ProjectSerialization project = base();
    const std::vector<OpResult> results = apply(&project, { DeleteNodeOp { nid(3) }, DeleteNodeOp { nid(5) }, DeleteNodeOp { nid(99) } });
    EXPECT_TRUE(results[0].applied());
    EXPECT_EQ(results[1].status, OpStatus::eTargetMissing) << "Grade1 went with Group1";
    EXPECT_EQ(results[2].status, OpStatus::eTargetMissing);

    std::string expected = kBaseGraph;
    expected.erase(expected.find("  Group1:\n"), expected.find("  Viewer1:\n") - expected.find("  Group1:\n"));
    expected.erase(expected.find("    inputs: {\"1\": \"Group1\"}\n"));
    EXPECT_SAME_PROJECT(projectFromYaml(expected), project);
}

TEST(ApplyOps, DeleteThenRecreateWithTheSameIdLeavesItUnconnected)
{
    ProjectSerialization project = base();
    expectAllApplied(apply(&project, { DeleteNodeOp { nid(2) }, CreateNodeOp { std::string(), plainNode(2, "Blur1", "Blur") } }));
    std::string expected = kBaseGraph;
    const std::string blurState = "    pos: [10, 20]\n    knobs:\n      size: 3.5\n    inputs: {Source: \"Read1\"}\n";
    expected.erase(expected.find(blurState), blurState.size());
    const std::string groupInput = "    inputs: {Input1: \"Blur1\"}\n";
    expected.erase(expected.find(groupInput), groupInput.size());
    EXPECT_SAME_PROJECT(projectFromYaml(expected), project);
}

TEST(ApplyOps, RenameKeepsConnectionsByName)
{
    ProjectSerialization project = base();
    const std::vector<OpResult> results = apply(&project, {
                                                              RenameOp { nid(2), "Soften", std::string("Soft blur") },
                                                              RenameOp { nid(4), "Grade1", std::nullopt },
                                                              RenameOp { nid(99), "Anything", std::nullopt },
                                                              RenameOp { nid(1), std::string(), std::nullopt },
                                                          });
    EXPECT_TRUE(results[0].applied());
    EXPECT_EQ(results[1].status, OpStatus::eInvalid) << "a sibling is called Grade1";
    EXPECT_EQ(results[2].status, OpStatus::eTargetMissing);
    EXPECT_EQ(results[3].status, OpStatus::eInvalid);

    std::string expected = kBaseGraph;
    expected.replace(expected.find("  Blur1:\n"), std::string("  Blur1:\n").size(), "  Soften:\n    label: \"Soft blur\"\n");
    expected.replace(expected.find("{Input1: \"Blur1\"}"), std::string("{Input1: \"Blur1\"}").size(), "{Input1: \"Soften\"}");
    EXPECT_SAME_PROJECT(projectFromYaml(expected), project);
}

TEST(ApplyOps, SetInputConnectsAndDisconnects)
{
    ProjectSerialization project = base();
    const std::vector<OpResult> results = apply(&project, {
                                                              SetInputOp { nid(2), "Mask", nid(1) },
                                                              SetInputOp { nid(7), "1", std::string() },
                                                              SetInputOp { nid(7), "2", nid(2) },
                                                              SetInputOp { nid(5), "Source", nid(1) },
                                                              SetInputOp { nid(2), "Source", nid(2) },
                                                              SetInputOp { nid(2), "Source", nid(99) },
                                                              SetInputOp { nid(99), "Source", nid(1) },
                                                              SetInputOp { nid(2), std::string(), nid(1) },
                                                          });
    EXPECT_TRUE(results[0].applied());
    EXPECT_TRUE(results[1].applied());
    EXPECT_TRUE(results[2].applied());
    EXPECT_EQ(results[3].status, OpStatus::eInvalid) << "Read1 is not in Group1";
    EXPECT_EQ(results[4].status, OpStatus::eInvalid) << "a node cannot feed itself";
    EXPECT_EQ(results[5].status, OpStatus::eTargetMissing);
    EXPECT_EQ(results[6].status, OpStatus::eTargetMissing);
    EXPECT_EQ(results[7].status, OpStatus::eInvalid);

    std::string expected = kBaseGraph;
    expected.replace(expected.find("{Source: \"Read1\"}"), std::string("{Source: \"Read1\"}").size(), "{Mask: \"Read1\", Source: \"Read1\"}");
    expected.replace(expected.find("{\"1\": \"Group1\"}"), std::string("{\"1\": \"Group1\"}").size(), "{\"2\": \"Blur1\"}");
    EXPECT_SAME_PROJECT(projectFromYaml(expected), project);
}

TEST(ApplyOps, SetKnobReplacesAddsAndResets)
{
    ProjectSerialization project = base();
    expectAllApplied(apply(&project, {
                                         SetKnobOp { nid(2), "size", knobFromYaml("size: {expr: \"frame\", hasRet: false}") },
                                         SetKnobOp { nid(2), "filter", knobFromYaml("filter: \"Gaussian\"") },
                                         SetKnobOp { nid(1), "filename", std::nullopt },
                                         SetKnobOp { nid(1), "neverSet", std::nullopt },
                                         SetKnobOp { std::nullopt, "frameRate", std::nullopt },
                                         SetKnobOp { std::nullopt, "frameRange", knobFromYaml("frameRange: [1, 48]") },
                                     }));
    const std::vector<OpResult> refused = apply(&project, { SetKnobOp { nid(99), "size", std::nullopt }, SetKnobOp { nid(2), std::string(), std::nullopt } });
    EXPECT_EQ(refused[0].status, OpStatus::eTargetMissing);
    EXPECT_EQ(refused[1].status, OpStatus::eInvalid);

    std::string expected = kBaseGraph;
    expected.replace(expected.find("  frameRate: 25\n"), std::string("  frameRate: 25\n").size(), "  frameRange: [1, 48]\n");
    expected.replace(expected.find("      size: 3.5\n"), std::string("      size: 3.5\n").size(), "      size: {expr: \"frame\", hasRet: false}\n      filter: \"Gaussian\"\n");
    const std::string filename = "    knobs:\n      filename: \"plate.####.exr\"\n";
    expected.erase(expected.find(filename), filename.size());
    EXPECT_SAME_PROJECT(projectFromYaml(expected), project);
}

TEST(ApplyOps, SetKnobOnAUserKnobKeepsItsDefinition)
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
)yaml";
    ProjectSerialization project = baseWith("    inputs: {Input1: \"Blur1\"}\n", "    inputs: {Input1: \"Blur1\"}\n" + userPage);
    expectAllApplied(apply(&project, { SetKnobOp { nid(3), "amount", std::nullopt } }));
    std::string atDefault = userPage;
    atDefault.replace(atDefault.find("value: 0.75"), std::string("value: 0.75").size(), "value: 0.5");
    EXPECT_SAME_PROJECT(baseWith("    inputs: {Input1: \"Blur1\"}\n", "    inputs: {Input1: \"Blur1\"}\n" + atDefault), project);

    expectAllApplied(apply(&project, { SetKnobOp { nid(3), "amount", knobFromYaml("amount: 0.25") } }));
    std::string changed = userPage;
    changed.replace(changed.find("value: 0.75"), std::string("value: 0.75").size(), "value: 0.25");
    EXPECT_SAME_PROJECT(baseWith("    inputs: {Input1: \"Blur1\"}\n", "    inputs: {Input1: \"Blur1\"}\n" + changed), project);
}

TEST(ApplyOps, NodeUiUserKnobsRotoTrackerAndProjectData)
{
    ProjectSerialization project = base();
    SetNodeUiOp ui;
    ui.nodeId = nid(2);
    ui.size = std::make_pair(80., 30.);
    ui.color.emplace(NodeUiColor { 0.5f, 0.25f, 1.f });
    SetNodeUiOp overlay;
    overlay.nodeId = nid(1);
    overlay.position = std::make_pair(-5., 7.5);
    overlay.overlayColor.emplace(NodeUiColor { 1.f, 0.f, 0.f });
    SetNodeUiOp clear;
    clear.nodeId = nid(1);
    clear.overlayColor.emplace(std::nullopt);

    SetUserKnobsOp pages;
    pages.nodeId = nid(5);
    pages.pagesOrder = { "Extra", "Node" };
    GroupKnobSerialization page;
    page.name = "Extra";
    page.label = "Extra";
    pages.userPages.push_back(page);

    RotoContextSerialization roto;
    roto.baseLayer.name = "Layer1";
    roto.baseLayer.label = "Layer1";
    roto.baseLayer.activated = true;
    TrackerContextSerialization tracker;
    TrackSerialization track;
    track.scriptName = "track1";
    track.label = "track1";
    tracker.tracks.push_back(track);

    SetProjectDataOp projectData;
    FormatSerialization format;
    format.name = "Square";
    format.x2 = 512;
    format.y2 = 512;
    format.pixelAspectRatio = 1.;
    projectData.formats = std::list<FormatSerialization> { format };

    expectAllApplied(apply(&project, {
                                         ui,
                                         overlay,
                                         clear,
                                         pages,
                                         SetRotoDataOp { nid(5), roto },
                                         SetTrackerDataOp { nid(6), tracker },
                                         SetTrackerDataOp { nid(6), std::nullopt },
                                         SetTrackerDataOp { nid(4), tracker },
                                         projectData,
                                     }));

    ProjectSerialization expected = base();
    for (const NodeSerializationPtr& node : expected.nodes.nodes) {
        if (node->scriptName == "Blur1") {
            node->layout.hasSize = true;
            node->layout.width = 80.;
            node->layout.height = 30.;
            node->layout.hasColor = true;
            node->layout.colorR = 0.5f;
            node->layout.colorG = 0.25f;
            node->layout.colorB = 1.f;
        } else if (node->scriptName == "Read1") {
            node->layout.hasPosition = true;
            node->layout.x = -5.;
            node->layout.y = 7.5;
        } else if (node->scriptName == "Group1") {
            for (const NodeSerializationPtr& child : node->children) {
                if (child->scriptName == "Grade1") {
                    child->userPages = pages.userPages;
                    child->pagesOrder = pages.pagesOrder;
                    child->hasRotoContext = true;
                    child->rotoContext = roto;
                } else if (child->scriptName == "Input1") {
                    child->hasTrackerContext = true;
                    child->trackerContext = tracker;
                }
            }
        }
    }
    expected.additionalFormats = *projectData.formats;
    EXPECT_SAME_PROJECT(expected, project);

    const std::vector<OpResult> missing = apply(&project, {
                                                              SetNodeUiOp { nid(99) },
                                                              SetUserKnobsOp { nid(99) },
                                                              SetRotoDataOp { nid(99) },
                                                              SetTrackerDataOp { nid(99) },
                                                          });
    for (const OpResult& result : missing) {
        EXPECT_EQ(result.status, OpStatus::eTargetMissing);
    }
    EXPECT_SAME_PROJECT(expected, project);
}

TEST(ApplyOps, AFailedOperationDoesNotStopTheOthers)
{
    ProjectSerialization project = base();
    const std::vector<OpResult> results = apply(&project, {
                                                              RenameOp { nid(99), "Ghost", std::nullopt },
                                                              RenameOp { nid(1), "Plate", std::nullopt },
                                                              SetInputOp { nid(2), "Source", nid(98) },
                                                              DeleteNodeOp { nid(7) },
                                                          });
    EXPECT_EQ(results[0].status, OpStatus::eTargetMissing);
    EXPECT_TRUE(results[1].applied());
    EXPECT_EQ(results[2].status, OpStatus::eTargetMissing);
    EXPECT_TRUE(results[3].applied());

    std::string expected = kBaseGraph;
    expected.replace(expected.find("  Read1:\n"), std::string("  Read1:\n").size(), "  Plate:\n");
    expected.replace(expected.find("{Source: \"Read1\"}"), std::string("{Source: \"Read1\"}").size(), "{Source: \"Plate\"}");
    expected.erase(expected.find("  Viewer1:\n"));
    EXPECT_SAME_PROJECT(projectFromYaml(expected), project);
}

TEST(ApplyOps, InputsNamingUnknownNodesAreKept)
{
    ProjectSerialization project = baseWith("{\"1\": \"Group1\"}", "{\"1\": \"Group1\", \"2\": \"Elsewhere\"}");
    expectAllApplied(apply(&project, { RenameOp { nid(3), "Comp", std::nullopt } }));
    std::string expected = kBaseGraph;
    expected.replace(expected.find("  Group1:\n"), std::string("  Group1:\n").size(), "  Comp:\n");
    expected.replace(expected.find("{\"1\": \"Group1\"}"), std::string("{\"1\": \"Group1\"}").size(), "{\"1\": \"Comp\", \"2\": \"Elsewhere\"}");
    EXPECT_SAME_PROJECT(projectFromYaml(expected), project);
}
