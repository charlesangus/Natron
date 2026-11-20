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

#include <optional>
#include <ostream>

#include <gtest/gtest.h>

#include "MergeScenarios.h"

#include "Serialization/Merge/MergeHandler.h"
#include "Serialization/Merge/ThreeWayMerge.h"
#include "Serialization/Yaml/BlobSink.h"
#include "Serialization/Yaml/ProjectYaml.h"

NATRON_NAMESPACE_USING
using namespace MergeScenarios;

namespace {

// A Double knob whose dimensions default to 1; an empty value list means the knob is absent.
std::optional<Serialization::KnobSerialization>
doubleKnob(const std::vector<std::optional<double>>& values)
{
    if (values.empty()) {
        return std::nullopt;
    }
    Serialization::KnobSerialization knob;
    knob.name = "size";
    knob.typeName = "Double";
    knob.dimension = int(values.size());
    knob.dimensions.resize(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        knob.dimensions[i].defaultValue = Serialization::KnobValueSerialization(1.);
        knob.dimensions[i].value = Serialization::KnobValueSerialization(values[i] ? *values[i] : 1.);
    }

    return knob;
}

const Serialization::KnobSerialization*
pointer(const std::optional<Serialization::KnobSerialization>& knob)
{
    return knob ? &*knob : nullptr;
}

struct ScalarCase {
    const char* name;
    std::vector<std::optional<double>> base;
    std::vector<std::optional<double>> mine;
    std::vector<std::optional<double>> theirs;
    // Empty when the merged knob is at its default.
    std::vector<double> merged;
    std::vector<int> conflictedDimensions;
};

std::ostream&
operator<<(std::ostream& stream,
           const ScalarCase& scalarCase)
{
    return stream << scalarCase.name;
}

class ScalarMerge
    : public ::testing::TestWithParam<ScalarCase> {
};

const ScalarCase kScalarCases[] = {
    { "unchanged", { 2. }, { 2. }, { 2. }, { 2. }, {} },
    { "mine changed", { 2. }, { 3. }, { 2. }, { 3. }, {} },
    { "theirs changed", { 2. }, { 2. }, { 4. }, { 4. }, {} },
    { "both changed the same way", { 2. }, { 5. }, { 5. }, { 5. }, {} },
    { "both changed differently", { 2. }, { 3. }, { 4. }, { 3. }, { 0 } },
    { "mine reset to default", { 2. }, {}, { 2. }, {}, {} },
    { "theirs reset to default", { 2. }, { 2. }, {}, {}, {} },
    { "mine reset while theirs changed", { 2. }, {}, { 4. }, {}, { 0 } },
    { "theirs reset while mine changed", { 2. }, { 3. }, {}, { 3. }, { 0 } },
    { "a default value written out is the default", { 2. }, { 1. }, {}, {}, {} },
    { "both set a knob absent from the base alike", {}, { 3. }, { 3. }, { 3. }, {} },
    { "both set a knob absent from the base differently", {}, { 3. }, { 4. }, { 3. }, { 0 } },
    { "theirs sets a knob absent from the base", {}, {}, { 4. }, { 4. }, {} },
    { "each side changed another dimension", { 2., 2. }, { 3., 2. }, { 2., 4. }, { 3., 4. }, {} },
    { "one dimension conflicts, the other merges", { 2., 2. }, { 3., 5. }, { 4., 2. }, { 3., 5. }, { 0 } },
    { "both dimensions conflict", { 2., 2. }, { 3., 3. }, { 6., 6. }, { 3., 3. }, { 0, 1 } },
};

TEST_P(ScalarMerge, TakesTheChangedSideAndKeepsMineOnAConflict)
{
    const ScalarCase& c = GetParam();
    const std::optional<Serialization::KnobSerialization> base = doubleKnob(c.base);
    const std::optional<Serialization::KnobSerialization> mine = doubleKnob(c.mine);
    const std::optional<Serialization::KnobSerialization> theirs = doubleKnob(c.theirs);
    std::optional<Serialization::KnobSerialization> merged;
    std::vector<Serialization::MergeConflict> conflicts;
    Serialization::ScalarKnobMergeHandler().merge(pointer(base), pointer(mine), pointer(theirs), &merged, &conflicts);

    std::vector<double> values;
    if (merged) {
        for (const Serialization::KnobDimensionSerialization& dimension : merged->dimensions) {
            values.push_back(dimension.value ? std::get<double>(*dimension.value) : 1.);
        }
        EXPECT_FALSE(Serialization::isDefaultKnob(*merged));
    }
    EXPECT_EQ(values, c.merged);
    std::vector<int> dimensions;
    for (const Serialization::MergeConflict& conflict : conflicts) {
        EXPECT_EQ(conflict.kind, Serialization::MergeConflictKind::eValue);
        EXPECT_EQ(conflict.knob, "size");
        dimensions.push_back(conflict.dimension);
    }
    EXPECT_EQ(dimensions, c.conflictedDimensions);
}

INSTANTIATE_TEST_CASE_P(Cases, ScalarMerge, ::testing::ValuesIn(kScalarCases));

TEST(ScalarMergeState, AnExpressionIsPartOfTheDimensionState)
{
    std::optional<Serialization::KnobSerialization> base = doubleKnob({ 2. });
    std::optional<Serialization::KnobSerialization> mine = base;
    mine->dimensions[0].expression = "Grade1.multiply.getValue(0)";
    std::optional<Serialization::KnobSerialization> theirs = doubleKnob({ 4. });
    std::optional<Serialization::KnobSerialization> merged;
    std::vector<Serialization::MergeConflict> conflicts;
    Serialization::ScalarKnobMergeHandler().merge(pointer(base), pointer(mine), pointer(theirs), &merged, &conflicts);
    ASSERT_TRUE(merged.has_value());
    EXPECT_EQ(merged->dimensions[0].expression, "Grade1.multiply.getValue(0)");
    EXPECT_EQ(conflicts.size(), 1u);

    theirs = base;
    conflicts.clear();
    Serialization::ScalarKnobMergeHandler().merge(pointer(base), pointer(mine), pointer(theirs), &merged, &conflicts);
    ASSERT_TRUE(merged.has_value());
    EXPECT_EQ(merged->dimensions[0].expression, "Grade1.multiply.getValue(0)");
    EXPECT_TRUE(conflicts.empty());
}

class TakeTheirsHandler
    : public Serialization::KnobMergeHandler {
public:
    void merge(const Serialization::KnobSerialization* /*base*/,
               const Serialization::KnobSerialization* /*mine*/,
               const Serialization::KnobSerialization* theirs,
               std::optional<Serialization::KnobSerialization>* merged,
               std::vector<Serialization::MergeConflict>* /*conflicts*/) const override
    {
        *merged = theirs ? std::optional<Serialization::KnobSerialization>(*theirs) : std::nullopt;
    }
};

TEST(MergeHandlerRegistry, DispatchesByPluginThenByKnobKind)
{
    std::shared_ptr<const Serialization::KnobMergeHandler> byPlugin = std::make_shared<TakeTheirsHandler>();
    std::shared_ptr<const Serialization::KnobMergeHandler> byKind = std::make_shared<TakeTheirsHandler>();
    Serialization::MergeHandlerRegistry registry;
    registry.setPluginHandler("Blur", byPlugin);
    registry.setKindHandler("String", byKind);
    EXPECT_EQ(&registry.handlerFor("Blur", "String"), byPlugin.get());
    EXPECT_EQ(&registry.handlerFor("Blur", "Double"), byPlugin.get());
    EXPECT_EQ(&registry.handlerFor("Grade", "String"), byKind.get());
    EXPECT_TRUE(dynamic_cast<const Serialization::ScalarKnobMergeHandler*>(&registry.handlerFor("Grade", "Double")) != nullptr);
    EXPECT_TRUE(dynamic_cast<const Serialization::ScalarKnobMergeHandler*>(&Serialization::MergeHandlerRegistry::defaults().handlerFor("Blur", "String")) != nullptr);
}

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

    Serialization::MergeResult merge(const Serialization::MergeOptions& options = Serialization::MergeOptions()) const
    {
        return Serialization::threeWayMerge(base, mine, theirs, Serialization::planMerge(base, mine, theirs, "grade_tpl"), options);
    }
};

TEST(ThreeWayMerge, ThePluginHandlerDecidesThatPluginsKnobs)
{
    Scenario scenario = scenarios().at("supervisor-replaces-node");
    const Projects projects(scenario);
    EXPECT_FALSE(projects.merge().conflicts.empty());

    Serialization::MergeHandlerRegistry registry;
    registry.setPluginHandler("Blur", std::make_shared<TakeTheirsHandler>());
    Serialization::MergeOptions options;
    options.handlers = &registry;
    const Serialization::MergeResult result = projects.merge(options);
    for (const Serialization::MergeConflict& conflict : result.conflicts) {
        EXPECT_NE(conflict.kind, Serialization::MergeConflictKind::eValue);
    }
    Serialization::NodeSerializationPtr blur = nodeAt(result.merged, "Blur2");
    ASSERT_TRUE(blur != nullptr);
    EXPECT_EQ(valuesOf(*blur, "size"), std::vector<double>({ 6., 6. }));
}

TEST(ThreeWayMerge, ExpressionsNamingNodesFollowTheArtistsRename)
{
    const Serialization::MergeResult result = Projects(scenarios().at("rename-plus-expression")).merge();
    EXPECT_TRUE(result.conflicts.empty());
    Serialization::NodeSerializationPtr colorCorrect = nodeAt(result.merged, "ColorCorrect1");
    Serialization::NodeSerializationPtr blur = nodeAt(result.merged, "Blur1");
    ASSERT_TRUE(colorCorrect != nullptr);
    ASSERT_TRUE(blur != nullptr);
    EXPECT_EQ(expressionOf(*colorCorrect, "saturation"), "KeyGrade.multiply.getValue(0)");
    EXPECT_EQ(expressionOf(*blur, "size"), "KeyGrade.multiply.getValue(0)");
    EXPECT_TRUE(nodeAt(result.merged, "Grade1") == nullptr);
}

TEST(ThreeWayMerge, APositionMergesAsOnePairAndNeverConflicts)
{
    struct Case {
        const char* name;
        std::pair<double, double> mine;
        std::pair<double, double> theirs;
        std::pair<double, double> merged;
    };
    const Case cases[] = {
        { "unchanged", { 0., 0. }, { 0., 0. }, { 0., 0. } },
        { "mine moved", { 10., 0. }, { 0., 0. }, { 10., 0. } },
        { "theirs moved", { 0., 0. }, { 0., 20. }, { 0., 20. } },
        { "both moved along different axes", { 10., 0. }, { 0., 20. }, { 10., 0. } },
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        Projects projects(scenarios().at("rename"));
        const auto place = [](const Serialization::ProjectSerialization& project, const std::string& name, std::pair<double, double> at) {
            Serialization::NodeSerializationPtr node = nodeAt(project, name);
            node->layout.hasPosition = true;
            node->layout.x = at.first;
            node->layout.y = at.second;
        };
        place(projects.base, "Blur1", { 0., 0. });
        place(projects.mine, "Blur1", c.mine);
        place(projects.theirs, "Blur1", c.theirs);
        const Serialization::MergeResult result = projects.merge();
        EXPECT_TRUE(result.conflicts.empty());
        Serialization::NodeSerializationPtr blur = nodeAt(result.merged, "Blur1");
        ASSERT_TRUE(blur != nullptr);
        EXPECT_EQ(std::make_pair(blur->layout.x, blur->layout.y), c.merged);
    }
}

TEST(ThreeWayMerge, AnAdoptedNodeListsTheKnobsItLeavesAtDefault)
{
    const Scenario scenario = scenarios().at("delete-recreate-grade");
    const Serialization::MergeResult result = Projects(scenario).merge();
    ASSERT_EQ(result.conflicts.size(), 1u);
    const Serialization::MergeConflict& conflict = result.conflicts.front();
    EXPECT_EQ(conflict.kind, Serialization::MergeConflictKind::eAdopted);
    EXPECT_EQ(conflict.side, Serialization::MergeSide::eMine);
    EXPECT_EQ(conflict.baseNodeId, hexId("b:Grade1"));
    EXPECT_EQ(conflict.nodeId, hexId("m:new"));
    EXPECT_EQ(conflict.defaultedKnobs, std::vector<std::string>({ "white" }));

    Serialization::NodeSerializationPtr grade = nodeAt(result.merged, "Grade2");
    ASSERT_TRUE(grade != nullptr);
    EXPECT_EQ(valuesOf(*grade, "multiply"), std::vector<double>({ 2. }));
    EXPECT_EQ(valuesOf(*grade, "gamma"), std::vector<double>({ 1.2 }));
    EXPECT_TRUE(valuesOf(*grade, "white").empty());
    EXPECT_EQ(grade->origin.templateKey, "grade_tpl");
    EXPECT_EQ(grade->origin.nodeId, hexId("b:Grade1"));
}

TEST(ThreeWayMerge, TheImportsBaseBecomesTheirsAndMergingItAgainChangesNothing)
{
    const Projects projects(scenarios().at("rename"));
    const Serialization::MergeResult first = projects.merge();
    std::shared_ptr<Serialization::ProjectSerialization> newBase;
    for (const Serialization::TemplateImportSerialization& entry : first.merged.templates) {
        if (entry.key == "grade_tpl") {
            newBase = entry.base;
        }
    }
    ASSERT_TRUE(newBase != nullptr);
    const auto encode = [](const Serialization::ProjectSerialization& project) {
        Serialization::MemoryBlobStore store;
        std::string text = Serialization::encodeProjectYaml(project, store);
        for (const auto& member : store.members()) {
            text += "\n" + member.first + "\n" + std::string(reinterpret_cast<const char*>(member.second.data()), member.second.size());
        }

        return text;
    };
    EXPECT_EQ(encode(*newBase), encode(projects.theirs));

    const Serialization::MergeResult again = Serialization::threeWayMerge(*newBase, first.merged, projects.theirs, Serialization::planMerge(*newBase, first.merged, projects.theirs, "grade_tpl"));
    EXPECT_TRUE(again.conflicts.empty());
    EXPECT_EQ(encode(again.merged), encode(first.merged));
}

TEST(ThreeWayMerge, NodesOutsideTheScopeKeepMine)
{
    Projects projects(scenarios().at("imported-twice"));
    const Serialization::MergeResult result = projects.merge();
    Serialization::NodeSerializationPtr second = nodeAt(result.merged, "Grade2");
    Serialization::NodeSerializationPtr first = nodeAt(result.merged, "Grade1");
    ASSERT_TRUE(second != nullptr);
    ASSERT_TRUE(first != nullptr);
    EXPECT_EQ(valuesOf(*first, "gamma"), std::vector<double>({ 1.2 }));
    EXPECT_TRUE(valuesOf(*second, "gamma").empty());
    EXPECT_EQ(valuesOf(*second, "multiply"), std::vector<double>({ 1.4 }));
    EXPECT_EQ(second->origin.templateKey, "grade_tpl_2");
}

} // namespace
