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
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "MergeScenarios.h"

#include "Serialization/Merge/ConflictResolution.h"
#include "Serialization/Merge/MergeHandler.h"
#include "Serialization/Merge/NodeMatcher.h"
#include "Serialization/Merge/ThreeWayMerge.h"
#include "Serialization/Ops/ApplyOps.h"
#include "Serialization/Ops/SnapshotDiff.h"

NATRON_NAMESPACE_USING
using namespace MergeScenarios;
using Serialization::MergeConflict;
using Serialization::MergeConflictKind;
using Serialization::ProjectSerialization;
using Serialization::ResolutionSide;

namespace {

struct Projects {
    ProjectSerialization base;
    ProjectSerialization mine;
    ProjectSerialization theirs;

    explicit Projects(const Scenario& scenario)
        : base(scenario.base.project())
        , mine(scenario.mine.project())
        , theirs(scenario.theirs.project())
    {
    }

    Serialization::MergeResult merge() const
    {
        return Serialization::threeWayMerge(base, mine, theirs, Serialization::planMerge(base, mine, theirs, "grade_tpl"));
    }
};

typedef std::function<bool(ProjectSerialization* snapshot, std::string* error)> Edit;

Edit
side(const MergeConflict& conflict,
     ResolutionSide chosen)
{
    return [&conflict, chosen](ProjectSerialization* snapshot, std::string* error) {
        return Serialization::applyConflictSide(conflict, chosen, snapshot, error);
    };
}

Edit
option(const MergeConflict& conflict,
       const std::string& name)
{
    return [&conflict, name](ProjectSerialization* snapshot, std::string* error) {
        return Serialization::applyConflictOption(conflict, name, snapshot, error);
    };
}

// The merged project after the choice, applied as the operations the app applies; undoing them,
// as the undo stack does with the reverse diff, gives the merged project back.
ProjectSerialization
resolved(const ProjectSerialization& merged,
         const Edit& edit)
{
    const ProjectSerialization before = Serialization::editableState(merged);
    ProjectSerialization edited = Serialization::editableState(merged);
    std::string error;
    EXPECT_TRUE(edit(&edited, &error)) << error;
    ProjectSerialization applied = Serialization::editableState(merged);
    for (const Serialization::OpResult& result : Serialization::applyOps(&applied, Serialization::diffSnapshots(before, edited))) {
        EXPECT_TRUE(result.applied()) << result.message;
    }
    EXPECT_TRUE(Serialization::diffSnapshots(Serialization::editableState(applied), edited).empty());

    ProjectSerialization undone = Serialization::editableState(applied);
    for (const Serialization::OpResult& result : Serialization::applyOps(&undone, Serialization::diffSnapshots(Serialization::editableState(applied), before))) {
        EXPECT_TRUE(result.applied()) << result.message;
    }
    EXPECT_TRUE(Serialization::diffSnapshots(Serialization::editableState(undone), before).empty());

    return applied;
}

const MergeConflict*
conflictOn(const Serialization::MergeResult& result,
           MergeConflictKind kind,
           const std::string& path,
           const std::function<bool(const MergeConflict&)>& also = nullptr)
{
    const Serialization::NodeSerializationPtr node = nodeAt(result.merged, path);
    if (!node) {
        ADD_FAILURE() << "no merged node " << path;

        return nullptr;
    }
    for (const MergeConflict& conflict : result.conflicts) {
        if ((conflict.kind == kind) && (conflict.nodeId == node->id) && (!also || also(conflict))) {
            return &conflict;
        }
    }
    ADD_FAILURE() << "no " << Serialization::mergeConflictKindName(kind) << " conflict on " << path;

    return nullptr;
}

std::string
inputOf(const ProjectSerialization& project,
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

std::map<double, double>
keysOf(const ProjectSerialization& project,
       const std::string& path,
       const std::string& knob)
{
    std::map<double, double> keys;
    Serialization::NodeSerializationPtr node = nodeAt(project, path);
    const Serialization::KnobSerialization* found = node ? knobOf(*node, knob) : nullptr;
    if (found && !found->dimensions.empty() && found->dimensions[0].curve) {
        for (const Serialization::KeyFrameSerialization& key : found->dimensions[0].curve->keyframes) {
            keys[key.time] = key.value;
        }
    }

    return keys;
}

const Serialization::BezierSerialization*
shapeIn(const ProjectSerialization& project,
        const std::string& path,
        const std::string& name)
{
    Serialization::NodeSerializationPtr node = nodeAt(project, path);
    if (!node) {
        return nullptr;
    }
    for (const Serialization::RotoLayerChildSerialization& child : node->rotoContext.baseLayer.children) {
        if ((child.type == Serialization::eRotoLayerChildTypeBezier) && (child.bezier.name == name)) {
            return &child.bezier;
        }
    }

    return nullptr;
}

std::vector<double>
opacityOf(const Serialization::BezierSerialization& shape)
{
    Serialization::NodeSerialization holder;
    holder.knobs = shape.knobs;

    return valuesOf(holder, "opacity");
}

void
expectSameShape(const Serialization::BezierSerialization* expected,
                const Serialization::BezierSerialization* actual)
{
    ASSERT_TRUE(expected != nullptr);
    ASSERT_TRUE(actual != nullptr);
    EXPECT_TRUE(expected->controlPoints == actual->controlPoints);
    EXPECT_EQ(opacityOf(*expected), opacityOf(*actual));
}

// The template with Blur1 in a group Look, imported, on every side.
Scenario
groupedScenario()
{
    Scenario scenario = templateScenario();
    wrapIntoGroup(&scenario.base, "Look", "b:", false);
    wrapIntoGroup(&scenario.theirs, "Look", "b:", false);
    wrapIntoGroup(&scenario.mine, "Look", "m:", false);
    for (const std::pair<std::string, std::string>& node : std::vector<std::pair<std::string, std::string>>({ { "Look", "" }, { "InB", "Look" }, { "LookOut", "Look" } })) {
        SimNode& imported = scenario.mine.get(node.first, node.second);
        imported.originKey = "grade_tpl";
        imported.originNode = hexId("b:" + (node.second.empty() ? node.first : node.second + "." + node.first));
    }

    return scenario;
}

void
removeGroup(SimGraph* graph,
            const std::string& group)
{
    graph->remove(group);
    graph->nodes.erase(std::remove_if(graph->nodes.begin(), graph->nodes.end(), [&](const SimNode& node) { return node.parent == group; }), graph->nodes.end());
}

Serialization::TrackSerialization
track(const std::string& name,
      double centre)
{
    Serialization::TrackSerialization result;
    result.scriptName = name;
    result.label = name;
    Serialization::KnobSerialization knob;
    knob.name = "centerPoint";
    knob.typeName = "Double";
    knob.dimension = 2;
    knob.dimensions.resize(2);
    for (Serialization::KnobDimensionSerialization& dimension : knob.dimensions) {
        dimension.defaultValue = Serialization::KnobValueSerialization(0.);
        dimension.value = Serialization::KnobValueSerialization(0.);
    }
    Serialization::CurveSerialization curve;
    Serialization::KeyFrameSerialization key;
    key.time = 1.;
    key.value = centre;
    key.interpolation = 1;
    curve.keyframes.push_back(key);
    knob.dimensions[0].curve = curve;
    result.knobs.push_back(knob);

    return result;
}

const Serialization::TrackSerialization*
trackIn(const ProjectSerialization& project,
        const std::string& name)
{
    for (const Serialization::TrackSerialization& entry : project.nodes.nodes.front()->trackerContext.tracks) {
        if (entry.scriptName == name) {
            return &entry;
        }
    }

    return nullptr;
}

double
centreOf(const Serialization::TrackSerialization& entry)
{
    for (const Serialization::KnobSerialization& knob : entry.knobs) {
        if ((knob.name == "centerPoint") && knob.dimensions[0].curve && !knob.dimensions[0].curve->keyframes.empty()) {
            return knob.dimensions[0].curve->keyframes.front().value;
        }
    }

    return -1.;
}

Serialization::KnobSerialization
userKnob(const std::string& name,
         double value)
{
    Serialization::KnobSerialization knob;
    knob.name = name;
    knob.typeName = "Double";
    knob.isUserKnob = true;
    knob.dimension = 1;
    knob.dimensions.resize(1);
    knob.dimensions[0].defaultValue = Serialization::KnobValueSerialization(1.);
    knob.dimensions[0].value = Serialization::KnobValueSerialization(value);

    return knob;
}

} // namespace

TEST(ConflictResolution, AValueConflictTakesTheChosenSidesDimension)
{
    const Serialization::MergeResult result = Projects(scenarios().at("supervisor-replaces-node")).merge();
    const MergeConflict* conflict = conflictOn(result, MergeConflictKind::eValue, "Blur2", [](const MergeConflict& c) { return c.knob == "size" && c.dimension == 0; });
    ASSERT_TRUE(conflict != nullptr);
    const struct {
        ResolutionSide side;
        double size;
    } cases[] = { { ResolutionSide::eMine, 3. }, { ResolutionSide::eTheirs, 6. }, { ResolutionSide::eBase, 2. } };
    for (const auto& c : cases) {
        SCOPED_TRACE(int(c.side));
        const ProjectSerialization project = resolved(result.merged, side(*conflict, c.side));
        Serialization::NodeSerializationPtr blur = nodeAt(project, "Blur2");
        ASSERT_TRUE(blur != nullptr);
        const std::vector<double> size = valuesOf(*blur, "size");
        ASSERT_EQ(size.size(), 2u);
        EXPECT_EQ(size[0], c.size);
        EXPECT_EQ(size[1], 3.);
    }
}

TEST(ConflictResolution, AnInputConflictReconnectsToTheChosenSidesSource)
{
    const Serialization::MergeResult result = Projects(scenarios().at("reconnect")).merge();
    const MergeConflict* conflict = conflictOn(result, MergeConflictKind::eInput, "Merge1");
    ASSERT_TRUE(conflict != nullptr);
    EXPECT_EQ(inputOf(resolved(result.merged, side(*conflict, ResolutionSide::eMine)), "Merge1", "A"), "ArtistMatte");
    EXPECT_EQ(inputOf(resolved(result.merged, side(*conflict, ResolutionSide::eTheirs)), "Merge1", "A"), "Constant2");
    EXPECT_EQ(inputOf(resolved(result.merged, side(*conflict, ResolutionSide::eBase)), "Merge1", "A"), "Constant1");
}

TEST(ConflictResolution, AStructuralInsertKeepsOneSidesChain)
{
    const Serialization::MergeResult result = Projects(scenarios().at("both-insert-input0")).merge();
    const MergeConflict* conflict = conflictOn(result, MergeConflictKind::eStructuralInsert, "Blur1");
    ASSERT_TRUE(conflict != nullptr);
    {
        const ProjectSerialization project = resolved(result.merged, side(*conflict, ResolutionSide::eMine));
        EXPECT_EQ(inputOf(project, "Blur1", "Source"), "ArtistBlur");
        EXPECT_EQ(inputOf(project, "ArtistBlur", "Source"), "Grade1");
    }
    {
        const ProjectSerialization project = resolved(result.merged, side(*conflict, ResolutionSide::eTheirs));
        EXPECT_EQ(inputOf(project, "Blur1", "Source"), "TplTransform");
        EXPECT_EQ(inputOf(project, "TplTransform", "Source"), "Grade1");
    }
    EXPECT_EQ(inputOf(resolved(result.merged, side(*conflict, ResolutionSide::eBase)), "Blur1", "Source"), "Grade1");
}

TEST(ConflictResolution, AReparentPutsTheNodeInTheChosenSidesGroup)
{
    const Serialization::MergeResult result = Projects(scenarios().at("both-sides-reparent")).merge();
    const MergeConflict* conflict = conflictOn(result, MergeConflictKind::eReparent, "ArtistLook.Blur1");
    ASSERT_TRUE(conflict != nullptr);
    const std::string id = nodeAt(result.merged, "ArtistLook.Blur1")->id;
    {
        const ProjectSerialization project = resolved(result.merged, side(*conflict, ResolutionSide::eMine));
        EXPECT_TRUE(nodeAt(project, "ArtistLook.Blur1") != nullptr);
        EXPECT_EQ(inputOf(project, "ArtistLook.Blur1", "Source"), "InB");
    }
    {
        const ProjectSerialization project = resolved(result.merged, side(*conflict, ResolutionSide::eTheirs));
        EXPECT_TRUE(nodeAt(project, "ArtistLook.Blur1") == nullptr);
        Serialization::NodeSerializationPtr blur = nodeAt(project, "Look.Blur1");
        ASSERT_TRUE(blur != nullptr);
        EXPECT_EQ(blur->id, id);
        EXPECT_EQ(inputOf(project, "Look.Blur1", "Source"), "Src");
    }
    {
        const ProjectSerialization project = resolved(result.merged, side(*conflict, ResolutionSide::eBase));
        Serialization::NodeSerializationPtr blur = nodeAt(project, "Blur1");
        ASSERT_TRUE(blur != nullptr);
        EXPECT_EQ(blur->id, id);
        EXPECT_EQ(inputOf(project, "Blur1", "Source"), "Grade1");
    }
}

TEST(ConflictResolution, ADeletedGroupGoesOnlyWhenTheDeletingSideIsChosen)
{
    Scenario scenario = groupedScenario();
    removeGroup(&scenario.mine, "Look");
    scenario.theirs.add("Sharpen", "Sharpen", "t:Sharpen", { { "Source", "Blur1" } }, "Look");
    scenario.theirs.get("LookOut", "Look").inputs = { { "Source", "Sharpen" } };
    const Serialization::MergeResult result = Projects(scenario).merge();
    const MergeConflict* conflict = conflictOn(result, MergeConflictKind::eDeleteModify, "Look");
    ASSERT_TRUE(conflict != nullptr);
    EXPECT_EQ(conflict->side, Serialization::MergeSide::eMine);
    EXPECT_TRUE(conflict->base.knobs.has_value());
    {
        const ProjectSerialization project = resolved(result.merged, side(*conflict, ResolutionSide::eMine));
        EXPECT_TRUE(nodeAt(project, "Look") == nullptr);
        EXPECT_NE(inputOf(project, "Merge1", "B"), "Look");
    }
    for (ResolutionSide kept : { ResolutionSide::eTheirs, ResolutionSide::eBase }) {
        SCOPED_TRACE(int(kept));
        const ProjectSerialization project = resolved(result.merged, side(*conflict, kept));
        EXPECT_TRUE(nodeAt(project, "Look") != nullptr);
        EXPECT_TRUE(nodeAt(project, "Look.Sharpen") != nullptr);
    }
}

TEST(ConflictResolution, AnAdoptedNodeTakesTheChosenSidesDefaultedKnobs)
{
    const Serialization::MergeResult result = Projects(scenarios().at("delete-recreate-grade")).merge();
    const MergeConflict* conflict = conflictOn(result, MergeConflictKind::eAdopted, "Grade2");
    ASSERT_TRUE(conflict != nullptr);
    const struct {
        ResolutionSide side;
        std::vector<double> white;
    } cases[] = { { ResolutionSide::eMine, {} }, { ResolutionSide::eTheirs, { 0.9 } }, { ResolutionSide::eBase, { 0.9 } } };
    for (const auto& c : cases) {
        SCOPED_TRACE(int(c.side));
        const ProjectSerialization project = resolved(result.merged, side(*conflict, c.side));
        Serialization::NodeSerializationPtr grade = nodeAt(project, "Grade2");
        ASSERT_TRUE(grade != nullptr);
        EXPECT_EQ(valuesOf(*grade, "white"), c.white);
        EXPECT_EQ(valuesOf(*grade, "multiply"), std::vector<double>({ 2. }));
    }
}

TEST(ConflictResolution, ACurveKeyConflictTakesTheChosenKeyOrKeepsBoth)
{
    const Serialization::MergeResult result = Projects(scenarios().at("keyframes-both-sides")).merge();
    const MergeConflict* conflict = conflictOn(result, MergeConflictKind::eCurveKey, "Grade1");
    ASSERT_TRUE(conflict != nullptr);
    EXPECT_EQ(keysOf(resolved(result.merged, side(*conflict, ResolutionSide::eMine)), "Grade1", "multiply").at(10.), 2.5);
    EXPECT_EQ(keysOf(resolved(result.merged, side(*conflict, ResolutionSide::eTheirs)), "Grade1", "multiply").at(10.), 1.8);
    EXPECT_EQ(keysOf(resolved(result.merged, side(*conflict, ResolutionSide::eBase)), "Grade1", "multiply").at(10.), 2.);

    ASSERT_EQ(conflict->options.size(), 1u);
    EXPECT_EQ(conflict->options.front().name, std::string(Serialization::kMergeOptionKeepBothKeys));
    const std::map<double, double> both = keysOf(resolved(result.merged, option(*conflict, std::string(Serialization::kMergeOptionKeepBothKeys))), "Grade1", "multiply");
    EXPECT_EQ(both.at(10.), 2.5);
    EXPECT_EQ(both.at(11.), 1.8);
    EXPECT_EQ(both.at(20.), 3.);
    EXPECT_EQ(both.at(30.), 0.5);

    std::string error;
    ProjectSerialization snapshot = Serialization::editableState(result.merged);
    EXPECT_FALSE(Serialization::applyConflictOption(*conflict, "no-such-choice", &snapshot, &error));
    EXPECT_FALSE(error.empty());
}

TEST(ConflictResolution, ARotoShapeConflictTakesTheChosenSidesShape)
{
    const Projects projects(scenarios().at("roto-both-sides"));
    const Serialization::MergeResult result = projects.merge();
    const MergeConflict* conflict = conflictOn(result, MergeConflictKind::eRotoShape, "Matte", [](const MergeConflict& c) { return c.item == "Garbage"; });
    ASSERT_TRUE(conflict != nullptr);
    expectSameShape(shapeIn(projects.mine, "Matte", "Garbage"), shapeIn(resolved(result.merged, side(*conflict, ResolutionSide::eMine)), "Matte", "Garbage"));
    expectSameShape(shapeIn(projects.theirs, "Matte", "Garbage"), shapeIn(resolved(result.merged, side(*conflict, ResolutionSide::eTheirs)), "Matte", "Garbage"));
    expectSameShape(shapeIn(projects.base, "Matte", "Garbage"), shapeIn(resolved(result.merged, side(*conflict, ResolutionSide::eBase)), "Matte", "Garbage"));
}

TEST(ConflictResolution, ADeletedRotoShapeGoesOnlyWhenTheDeletingSideIsChosen)
{
    Scenario scenario = templateScenario(true);
    std::vector<SimShape>& shapes = scenario.mine.get("Matte").shapes;
    shapes.erase(std::remove_if(shapes.begin(), shapes.end(), [](const SimShape& shape) { return shape.name == "Core"; }), shapes.end());
    scenario.theirs.shape("Matte", "Core").opacity = 0.5;
    const Projects projects(scenario);
    const Serialization::MergeResult result = projects.merge();
    const MergeConflict* conflict = conflictOn(result, MergeConflictKind::eDeleteModify, "Matte", [](const MergeConflict& c) { return c.item == "Core"; });
    ASSERT_TRUE(conflict != nullptr);
    EXPECT_EQ(conflict->side, Serialization::MergeSide::eMine);
    EXPECT_TRUE(shapeIn(resolved(result.merged, side(*conflict, ResolutionSide::eMine)), "Matte", "Core") == nullptr);
    expectSameShape(shapeIn(projects.theirs, "Matte", "Core"), shapeIn(resolved(result.merged, side(*conflict, ResolutionSide::eTheirs)), "Matte", "Core"));
    expectSameShape(shapeIn(projects.base, "Matte", "Core"), shapeIn(resolved(result.merged, side(*conflict, ResolutionSide::eBase)), "Matte", "Core"));
}

TEST(ConflictResolution, ADeletedTrackGoesOnlyWhenTheDeletingSideIsChosen)
{
    Serialization::TrackerContextSerialization base;
    base.tracks = { track("a", 0.), track("b", 0.) };
    Serialization::TrackerContextSerialization mine;
    mine.tracks = { track("a", 0.), track("b", 4.) };
    Serialization::TrackerContextSerialization theirs;
    theirs.tracks = { track("a", 0.) };
    std::vector<MergeConflict> conflicts;
    Serialization::NodeSerializationPtr node = std::make_shared<Serialization::NodeSerialization>();
    node->scriptName = "Tracker1";
    node->pluginID = "Tracker";
    node->id = hexId("m:Tracker1");
    node->hasTrackerContext = true;
    node->trackerContext = Serialization::mergeTrackerContexts(&base, mine, theirs, Serialization::MergeHandlerRegistry::defaults(), &conflicts);
    ProjectSerialization merged;
    merged.nodes.nodes.push_back(node);
    ASSERT_EQ(conflicts.size(), 1u);
    MergeConflict conflict = conflicts.front();
    conflict.nodeId = node->id;
    EXPECT_EQ(conflict.side, Serialization::MergeSide::eTheirs);

    EXPECT_TRUE(trackIn(resolved(merged, side(conflict, ResolutionSide::eTheirs)), "b") == nullptr);
    const ProjectSerialization kept = resolved(merged, side(conflict, ResolutionSide::eMine));
    ASSERT_TRUE(trackIn(kept, "b") != nullptr);
    EXPECT_EQ(centreOf(*trackIn(kept, "b")), 4.);
    const ProjectSerialization restored = resolved(merged, side(conflict, ResolutionSide::eBase));
    ASSERT_TRUE(trackIn(restored, "b") != nullptr);
    EXPECT_EQ(centreOf(*trackIn(restored, "b")), 0.);
}

TEST(ConflictResolution, ADeletedUserKnobGoesOnlyWhenTheDeletingSideIsChosen)
{
    Projects projects(templateScenario());
    nodeAt(projects.base, "Grade1")->knobs.push_back(userKnob("boost", 2.));
    nodeAt(projects.theirs, "Grade1")->knobs.push_back(userKnob("boost", 3.));
    const Serialization::MergeResult result = projects.merge();
    const MergeConflict* conflict = conflictOn(result, MergeConflictKind::eDeleteModify, "Grade1", [](const MergeConflict& c) { return c.knob == "boost"; });
    ASSERT_TRUE(conflict != nullptr);
    EXPECT_EQ(conflict->side, Serialization::MergeSide::eMine);
    {
        const ProjectSerialization project = resolved(result.merged, side(*conflict, ResolutionSide::eMine));
        EXPECT_TRUE(knobOf(*nodeAt(project, "Grade1"), "boost") == nullptr);
    }
    EXPECT_EQ(valuesOf(*nodeAt(resolved(result.merged, side(*conflict, ResolutionSide::eTheirs)), "Grade1"), "boost"), std::vector<double>({ 3. }));
    EXPECT_EQ(valuesOf(*nodeAt(resolved(result.merged, side(*conflict, ResolutionSide::eBase)), "Grade1"), "boost"), std::vector<double>({ 2. }));
}
