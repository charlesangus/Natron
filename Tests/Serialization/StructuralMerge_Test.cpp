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

#include <functional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "MergeScenarios.h"

#include "Serialization/Merge/NodeMatcher.h"
#include "Serialization/Merge/ThreeWayMerge.h"

NATRON_NAMESPACE_USING
using namespace MergeScenarios;

namespace {

struct Projects {
    Serialization::ProjectSerialization base;
    Serialization::ProjectSerialization mine;
    Serialization::ProjectSerialization theirs;

    explicit Projects(const Scenario& scenario)
        : base(scenario.base.project())
        , mine(scenario.mine.project())
        , theirs(scenario.theirs.project())
    {
    }

    Serialization::MergeResult merge(const std::string& templateKey = "grade_tpl") const
    {
        return Serialization::threeWayMerge(base, mine, theirs, Serialization::planMerge(base, mine, theirs, templateKey));
    }
};

std::string
inputOf(const Serialization::ProjectSerialization& project,
        const std::string& path,
        const std::string& label)
{
    Serialization::NodeSerializationPtr node = nodeAt(project, path);
    if (!node) {
        return "<no node " + path + ">";
    }
    std::map<std::string, std::string>::const_iterator input = node->inputs.find(label);

    return (input == node->inputs.end()) ? std::string() : input->second;
}

int
countPlugin(const std::list<Serialization::NodeSerializationPtr>& nodes,
            const std::string& pluginID)
{
    int count = 0;
    for (const Serialization::NodeSerializationPtr& node : nodes) {
        count += (node->pluginID == pluginID) ? 1 : 0;
        count += countPlugin(node->children, pluginID);
    }

    return count;
}

struct ExistenceCase {
    const char* name;
    std::function<void(Scenario*)> editGraphs;
    std::function<void(Projects*)> editProjects;
    // The merged node the case is about.
    std::string node;
    bool present;
    bool hasOrigin;
    // Empty for no conflict.
    std::string conflict;
    Serialization::MergeSide side;
};

TEST(StructuralMerge, NodeExistence)
{
    const auto none = [](Projects*) { };
    const auto recolour = [](Projects* projects) {
        Serialization::NodeSerializationPtr node = nodeAt(projects->mine, "Constant1");
        node->layout.hasPosition = true;
        node->layout.x = 40.;
        node->layout.hasColor = true;
        node->layout.colorR = 1.;
    };
    const ExistenceCase cases[] = {
        { "theirs added a node", [](Scenario* s) { insertBefore(&s->theirs, "TplGrade", "Grade", "t:new", "TplOut", ""); }, none, "TplGrade", true, true, "", Serialization::MergeSide::eMine },
        { "mine added a node", [](Scenario* s) { insertBefore(&s->mine, "ShotGrade", "Grade", "m:new", "ShotOut", ""); }, none, "ShotGrade", true, false, "", Serialization::MergeSide::eMine },
        { "theirs deleted a node mine left alone", [](Scenario* s) { s->theirs.remove("Constant1"); }, none, "Constant1", false, false, "", Serialization::MergeSide::eMine },
        { "theirs deleted a node mine only moved and recoloured", [](Scenario* s) { s->theirs.remove("Constant1"); }, recolour, "Constant1", false, false, "", Serialization::MergeSide::eMine },
        { "mine deleted a node theirs left alone", [](Scenario* s) { s->mine.remove("Constant1"); }, none, "Constant1", false, false, "", Serialization::MergeSide::eMine },
        { "theirs deleted a node mine changed", [](Scenario* s) { s->theirs.remove("Constant1"); s->mine.set("Constant1", "color", {0.5}); }, none, "Constant1", true, false, "delete-modify", Serialization::MergeSide::eTheirs },
        { "theirs deleted a node mine rewired", [](Scenario* s) { s->theirs.remove("Constant1"); s->mine.add("Matte", "Constant", "m:matte"); s->mine.get("Constant1").inputs["Source"] = "Matte"; }, none, "Constant1", true, false, "delete-modify", Serialization::MergeSide::eTheirs },
        { "mine deleted a node theirs changed", [](Scenario* s) { s->mine.remove("Constant1"); s->theirs.set("Constant1", "color", {0.5}); }, none, "Constant1", true, true, "delete-modify", Serialization::MergeSide::eMine },
        { "both deleted a node", [](Scenario* s) { s->mine.remove("Constant1"); s->theirs.remove("Constant1"); }, none, "Constant1", false, false, "", Serialization::MergeSide::eMine },
    };
    for (const ExistenceCase& c : cases) {
        SCOPED_TRACE(c.name);
        Scenario scenario = templateScenario();
        c.editGraphs(&scenario);
        Projects projects(scenario);
        c.editProjects(&projects);
        const Serialization::MergeResult result = projects.merge();
        Serialization::NodeSerializationPtr node = nodeAt(result.merged, c.node);
        EXPECT_EQ(node != nullptr, c.present);
        if (node) {
            EXPECT_EQ(node->origin.isSet(), c.hasOrigin);
        }
        if (c.conflict.empty()) {
            EXPECT_TRUE(result.conflicts.empty());
        } else {
            ASSERT_EQ(result.conflicts.size(), 1u);
            EXPECT_EQ(Serialization::mergeConflictKindName(result.conflicts.front().kind), c.conflict);
            EXPECT_EQ(result.conflicts.front().side, c.side);
        }
    }
}

TEST(StructuralMerge, ARestoredNodeTakesTheirsStateAndWiring)
{
    Scenario scenario = templateScenario();
    scenario.mine.remove("Grade1");
    scenario.mine.get("Blur1").inputs["Source"] = "TplIn";
    scenario.theirs.set("Grade1", "gamma", { 1.2 });
    const Serialization::MergeResult result = Projects(scenario).merge();
    Serialization::NodeSerializationPtr grade = nodeAt(result.merged, "Grade1");
    ASSERT_TRUE(grade != nullptr);
    EXPECT_EQ(valuesOf(*grade, "gamma"), std::vector<double>({ 1.2 }));
    EXPECT_EQ(grade->origin.nodeId, hexId("b:Grade1"));
    EXPECT_EQ(inputOf(result.merged, "Grade1", "Source"), "TplIn");
    // Blur1's input is the artist's choice; restoring Grade1 does not rewire it.
    EXPECT_EQ(inputOf(result.merged, "Blur1", "Source"), "TplIn");
}

TEST(StructuralMerge, BothSidesInsertingOnOneEdgeChainTheirsThenMine)
{
    {
        const Serialization::MergeResult result = Projects(scenarios().at("both-insert-input0")).merge();
        EXPECT_EQ(inputOf(result.merged, "Blur1", "Source"), "ArtistBlur");
        EXPECT_EQ(inputOf(result.merged, "ArtistBlur", "Source"), "TplTransform");
        EXPECT_EQ(inputOf(result.merged, "TplTransform", "Source"), "Grade1");
        ASSERT_EQ(result.conflicts.size(), 1u);
        EXPECT_EQ(result.conflicts.front().kind, Serialization::MergeConflictKind::eStructuralInsert);
        EXPECT_EQ(result.conflicts.front().input, "Source");
    }
    {
        const Serialization::MergeResult result = Projects(scenarios().at("both-insert-side-input")).merge();
        EXPECT_EQ(inputOf(result.merged, "Merge1", "A"), "MatteGrade");
        EXPECT_EQ(inputOf(result.merged, "MatteGrade", "Source"), "MatteBlur");
        EXPECT_EQ(inputOf(result.merged, "MatteBlur", "Source"), "Constant1");
        EXPECT_EQ(inputOf(result.merged, "Merge1", "B"), "Blur1");
        ASSERT_EQ(result.conflicts.size(), 1u);
        EXPECT_EQ(result.conflicts.front().kind, Serialization::MergeConflictKind::eStructuralInsert);
        EXPECT_EQ(result.conflicts.front().input, "A");
    }
}

TEST(StructuralMerge, GroupsMergeTheirContents)
{
    {
        const Serialization::MergeResult result = Projects(scenarios().at("wrap-into-group")).merge();
        EXPECT_TRUE(result.conflicts.empty());
        EXPECT_TRUE(nodeAt(result.merged, "Blur1") == nullptr);
        Serialization::NodeSerializationPtr blur = nodeAt(result.merged, "Look.Blur1");
        ASSERT_TRUE(blur != nullptr);
        EXPECT_EQ(valuesOf(*blur, "size"), std::vector<double>({ 3., 3. }));
        EXPECT_EQ(inputOf(result.merged, "Look.Merge1", "B"), "Blur1");
        EXPECT_EQ(inputOf(result.merged, "Look", "InB"), "Grade1");
        EXPECT_EQ(inputOf(result.merged, "ColorCorrect1", "Source"), "Look");
    }
    {
        const Serialization::MergeResult result = Projects(scenarios().at("artist-groups-template-nodes")).merge();
        EXPECT_TRUE(result.conflicts.empty());
        Serialization::NodeSerializationPtr blur = nodeAt(result.merged, "ArtistLook.Blur1");
        ASSERT_TRUE(blur != nullptr);
        EXPECT_EQ(valuesOf(*blur, "size"), std::vector<double>({ 5., 5. }));
        EXPECT_EQ(inputOf(result.merged, "ArtistLook.Blur1", "Source"), "InB");
    }
}

TEST(StructuralMerge, MovesIntoDifferentGroupsConflictAndKeepMine)
{
    const Serialization::MergeResult result = Projects(scenarios().at("both-sides-reparent")).merge();
    EXPECT_TRUE(nodeAt(result.merged, "ArtistLook.Blur1") != nullptr);
    EXPECT_TRUE(nodeAt(result.merged, "Look.Blur1") == nullptr);
    EXPECT_TRUE(nodeAt(result.merged, "Look") != nullptr);
    EXPECT_EQ(inputOf(result.merged, "Merge1", "B"), "ArtistLook");
    EXPECT_TRUE(nodeAt(result.merged, "ArtistLook.LookOut") != nullptr);
    EXPECT_TRUE(nodeAt(result.merged, "Look.LookOut") != nullptr);
    EXPECT_TRUE(nodeAt(result.merged, "Look.LookOut2") == nullptr);
    Serialization::NodeSerializationPtr blur = nodeAt(result.merged, "ArtistLook.Blur1");
    ASSERT_TRUE(blur != nullptr);
    std::vector<Serialization::MergeConflict> onBlur;
    for (const Serialization::MergeConflict& conflict : result.conflicts) {
        if (conflict.nodeId == blur->id) {
            onBlur.push_back(conflict);
        }
    }
    ASSERT_EQ(onBlur.size(), 1u);
    EXPECT_EQ(onBlur.front().kind, Serialization::MergeConflictKind::eReparent);
    EXPECT_EQ(onBlur.front().inputs, std::vector<std::string>({ "Source" }));
}

TEST(StructuralMerge, NamesAreUniqueWithinTheirGroup)
{
    Scenario scenario = templateScenario();
    wrapIntoGroup(&scenario.mine, "ArtistLook", "m:", false);
    scenario.theirs.add("LookOut", "Output", "t:LookOut");
    const Serialization::MergeResult result = Projects(scenario).merge();
    EXPECT_TRUE(nodeAt(result.merged, "ArtistLook.LookOut") != nullptr);
    Serialization::NodeSerializationPtr added = nodeAt(result.merged, "LookOut");
    ASSERT_TRUE(added != nullptr);
    EXPECT_EQ(added->origin.nodeId, hexId("t:LookOut"));
    EXPECT_TRUE(nodeAt(result.merged, "LookOut2") == nullptr);

    Scenario clash = templateScenario();
    clash.mine.add("Blur2", "Blur", "m:Blur2");
    clash.theirs.add("Blur2", "Blur", "t:Blur2");
    const Serialization::MergeResult renamed = Projects(clash).merge();
    Serialization::NodeSerializationPtr mine = nodeAt(renamed.merged, "Blur2");
    ASSERT_TRUE(mine != nullptr);
    EXPECT_FALSE(mine->origin.isSet());
    Serialization::NodeSerializationPtr theirs = nodeAt(renamed.merged, "Blur3");
    ASSERT_TRUE(theirs != nullptr);
    EXPECT_EQ(theirs->origin.nodeId, hexId("t:Blur2"));
}

TEST(StructuralMerge, IdenticalAdditionsOnBothSidesGiveOneNode)
{
    {
        Scenario scenario = templateScenario();
        insertBefore(&scenario.mine, "ShotGrade", "Grade", "m:new", "TplOut", "");
        insertBefore(&scenario.theirs, "TplGrade", "Grade", "t:new", "TplOut", "");
        const Serialization::MergeResult result = Projects(scenario).merge();
        EXPECT_TRUE(result.conflicts.empty());
        EXPECT_EQ(countPlugin(result.merged.nodes.nodes, "Grade"), 2);
        Serialization::NodeSerializationPtr grade = nodeAt(result.merged, "ShotGrade");
        ASSERT_TRUE(grade != nullptr);
        EXPECT_EQ(grade->origin.nodeId, hexId("t:new"));
        EXPECT_EQ(inputOf(result.merged, "TplOut", ""), "ShotGrade");
    }
    {
        Scenario scenario = templateScenario();
        wrapIntoGroup(&scenario.mine, "ArtistLook", "m:", false);
        wrapIntoGroup(&scenario.theirs, "Look", "t:", false);
        const Serialization::MergeResult result = Projects(scenario).merge();
        EXPECT_TRUE(result.conflicts.empty());
        EXPECT_EQ(countPlugin(result.merged.nodes.nodes, "Group"), 1);
        EXPECT_EQ(countPlugin(result.merged.nodes.nodes, "Input"), 1);
        EXPECT_EQ(countPlugin(result.merged.nodes.nodes, "Output"), 1);
        EXPECT_EQ(inputOf(result.merged, "ArtistLook.Blur1", "Source"), "InB");
    }
}

TEST(StructuralMerge, NamesLabelsAndAppearanceNeverConflict)
{
    Scenario scenario = templateScenario();
    scenario.mine.rename("Grade1", "KeyGrade");
    scenario.theirs.rename("Grade1", "TplGrade");
    Projects projects(scenario);
    for (Serialization::ProjectSerialization* project : { &projects.mine, &projects.theirs }) {
        const bool mine = (project == &projects.mine);
        Serialization::NodeSerializationPtr node = nodeAt(*project, mine ? "KeyGrade" : "TplGrade");
        node->label = mine ? "Key" : "Template grade";
        node->layout.hasColor = true;
        node->layout.colorR = mine ? 1. : 0.;
        node->layout.hasSize = true;
        node->layout.width = mine ? 100. : 200.;
    }
    const Serialization::MergeResult result = projects.merge();
    EXPECT_TRUE(result.conflicts.empty());
    Serialization::NodeSerializationPtr grade = nodeAt(result.merged, "KeyGrade");
    ASSERT_TRUE(grade != nullptr);
    EXPECT_EQ(grade->label, "Key");
    EXPECT_EQ(grade->layout.colorR, 1.);
    EXPECT_EQ(grade->layout.width, 100.);
}

TEST(StructuralMerge, ASupervisorRenameFollowsTheMatchIntoInputsAndExpressions)
{
    Scenario scenario = templateScenario();
    for (SimGraph* graph : { &scenario.base, &scenario.mine, &scenario.theirs }) {
        graph->expression("ColorCorrect1", "saturation", "Grade1.multiply.getValue(0)");
    }
    scenario.theirs.rename("Grade1", "TplGrade");
    scenario.theirs.expression("ColorCorrect1", "saturation", "TplGrade.multiply.getValue(0)");
    const Serialization::MergeResult result = Projects(scenario).merge();
    EXPECT_TRUE(result.conflicts.empty());
    EXPECT_TRUE(nodeAt(result.merged, "Grade1") == nullptr);
    EXPECT_TRUE(nodeAt(result.merged, "TplGrade") != nullptr);
    EXPECT_EQ(inputOf(result.merged, "Blur1", "Source"), "TplGrade");
    Serialization::NodeSerializationPtr colorCorrect = nodeAt(result.merged, "ColorCorrect1");
    ASSERT_TRUE(colorCorrect != nullptr);
    EXPECT_EQ(expressionOf(*colorCorrect, "saturation"), "TplGrade.multiply.getValue(0)");
}

TEST(StructuralMerge, EveryScenarioListsTheExpectedConflicts)
{
    for (const std::pair<const std::string, Scenario>& scenario : scenarios()) {
        const Projects projects(scenario.second);
        const Serialization::MergeGraph base(projects.base);
        for (const ExpectedMerge& merge : readExpected(std::string(NATRON_TESTS_FIXTURES_DIR) + "/merge-scenarios/expected/" + scenario.first + ".yaml")) {
            SCOPED_TRACE(scenario.first + " [" + merge.templateKey + "]");
            const std::set<ExpectedConflict>& expected = merge.conflicts;
            std::set<ExpectedConflict> got;
            for (const Serialization::MergeConflict& conflict : projects.merge(merge.templateKey).conflicts) {
                ExpectedConflict entry;
                const Serialization::MergeGraph::Node* node = base.find(conflict.baseNodeId);
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
            EXPECT_TRUE(got == expected) << [&] {
                std::string text = "got:";
                for (const ExpectedConflict& c : got) {
                    text += " {" + c.node + " " + c.kind + " " + c.knob + " " + std::to_string(c.dimension) + " " + (c.time ? std::to_string(*c.time) : std::string()) + " " + c.item + " " + c.input;
                    for (const std::string& knob : c.defaulted) {
                        text += " " + knob;
                    }
                    text += "}";
                }

                return text;
            }();
        }
    }
}

} // namespace
