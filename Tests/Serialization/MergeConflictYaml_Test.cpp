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
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "ProjectBundleTestHelpers.h"

#include "Engine/AppInstance.h"
#include "Engine/Project.h"

#include "Serialization/Bundle/ProjectBundle.h"
#include "Serialization/KnobSerialization.h"
#include "Serialization/Merge/MergeConflict.h"
#include "Serialization/ProjectSerialization.h"
#include "Serialization/Yaml/KnobYaml.h"
#include "Serialization/Yaml/MergeConflictYaml.h"

NATRON_NAMESPACE_USING

namespace {

Serialization::KnobSerialization
colorKnob(const std::string& yaml)
{
    std::list<Serialization::KnobSerialization> knobs = Serialization::decodeKnobsYaml(yaml);
    EXPECT_EQ(knobs.size(), 1u);
    if (knobs.empty()) {
        return Serialization::KnobSerialization();
    }
    Serialization::completeKnobSerialization(&knobs.front(), "Color", 4);

    return knobs.front();
}

Serialization::KeyFrameSerialization
key(double time,
    double value,
    int interpolation)
{
    Serialization::KeyFrameSerialization result;
    result.time = time;
    result.value = value;
    result.interpolation = interpolation;

    return result;
}

Serialization::KnobSerialization
doubleKnob(const std::string& name,
           double value)
{
    Serialization::KnobSerialization knob;
    knob.name = name;
    knob.typeName = "Double";
    knob.dimension = 1;
    knob.dimensions.resize(1);
    knob.dimensions[0].defaultValue = Serialization::KnobValueSerialization(1.);
    knob.dimensions[0].value = Serialization::KnobValueSerialization(value);

    return knob;
}

// A context holding one square shape, as a roto conflict records an item.
Serialization::RotoContextSerialization
shapeAlone(const std::string& name,
           double x,
           double opacity)
{
    Serialization::RotoLayerChildSerialization child;
    child.type = Serialization::eRotoLayerChildTypeBezier;
    child.bezier.name = name;
    child.bezier.label = name;
    child.bezier.activated = true;
    child.bezier.closed = true;
    for (const std::pair<double, double>& point : std::vector<std::pair<double, double>>({ { x, 0. }, { 10., 0. }, { 10., 10. }, { 0., 10. } })) {
        Serialization::BezierCPSerialization cp;
        cp.x = point.first;
        cp.y = point.second;
        child.bezier.controlPoints.push_back(cp);
        child.bezier.featherPoints.push_back(cp);
    }
    child.bezier.knobs.push_back(doubleKnob("opacity", opacity));
    Serialization::RotoContextSerialization context;
    context.baseLayer.name = "Layer1";
    context.baseLayer.children.push_back(child);

    return context;
}

Serialization::TrackerContextSerialization
trackAlone(const std::string& name,
           double centre)
{
    Serialization::TrackSerialization track;
    track.scriptName = name;
    track.label = name;
    track.userKeys = { 1, 5 };
    Serialization::KnobSerialization knob = doubleKnob("centerPoint", centre);
    knob.dimensions[0].defaultValue = Serialization::KnobValueSerialization(0.);
    Serialization::CurveSerialization curve;
    curve.keyframes.push_back(key(1., centre, 1));
    knob.dimensions[0].curve = curve;
    track.knobs.push_back(knob);
    Serialization::TrackerContextSerialization context;
    context.tracks.push_back(track);

    return context;
}

Serialization::MergeConflictConnection
connection(const std::string& node,
           const std::string& input,
           const std::string& source)
{
    Serialization::MergeConflictConnection result;
    result.nodeId = node;
    result.input = input;
    result.sourceId = source;

    return result;
}

std::vector<Serialization::MergeConflict>
everyKind()
{
    using Serialization::MergeConflict;
    using Serialization::MergeConflictKind;
    using Serialization::MergeSide;
    std::vector<MergeConflict> conflicts;

    MergeConflict value;
    value.kind = MergeConflictKind::eValue;
    value.templateKey = "grade_tpl";
    value.baseNodeId = "base-grade";
    value.nodeId = "grade";
    value.knob = "white";
    value.dimension = 1;
    value.base.knob = colorKnob("white: [1, 1, 1, 1]\n");
    value.mine.knob = colorKnob("white: [1, 1.25, 1, 1]\n");
    conflicts.push_back(value);

    MergeConflict curveKey;
    curveKey.kind = MergeConflictKind::eCurveKey;
    curveKey.templateKey = "grade_tpl";
    curveKey.baseNodeId = "base-grade";
    curveKey.nodeId = "grade";
    curveKey.knob = "gamma";
    curveKey.dimension = 0;
    curveKey.time = 10.5;
    curveKey.base.key = key(10.5, 2., 1);
    Serialization::KeyFrameSerialization broken = key(10.5, 2.5, 7);
    broken.leftDerivative = -0.25;
    broken.rightDerivative = 0.75;
    curveKey.mine.key = broken;
    curveKey.theirs.key = key(10.5, 1.8, 1);
    Serialization::MergeConflictOption keepBoth;
    keepBoth.name = std::string(Serialization::kMergeOptionKeepBothKeys);
    keepBoth.label = "Keep Both Keys";
    keepBoth.state.key = broken;
    keepBoth.state.otherKeys = { key(11.5, 1.8, 1) };
    curveKey.options.push_back(keepBoth);
    conflicts.push_back(curveKey);

    MergeConflict shape;
    shape.kind = MergeConflictKind::eRotoShape;
    shape.templateKey = "roto_tpl";
    shape.baseNodeId = "base-roto";
    shape.nodeId = "roto";
    shape.item = "Garbage";
    shape.base.roto = shapeAlone("Garbage", 0., 1.);
    shape.mine.roto = shapeAlone("Garbage", 5., 1.);
    shape.theirs.roto = shapeAlone("Garbage", 0., 0.5);
    conflicts.push_back(shape);

    MergeConflict deletedShape;
    deletedShape.kind = MergeConflictKind::eDeleteModify;
    deletedShape.templateKey = "roto_tpl";
    deletedShape.baseNodeId = "base-roto";
    deletedShape.nodeId = "roto";
    deletedShape.item = "Core";
    deletedShape.side = MergeSide::eMine;
    deletedShape.base.roto = shapeAlone("Core", 0., 1.);
    deletedShape.theirs.roto = shapeAlone("Core", 0., 0.5);
    conflicts.push_back(deletedShape);

    MergeConflict deletedTrack;
    deletedTrack.kind = MergeConflictKind::eDeleteModify;
    deletedTrack.templateKey = "track_tpl";
    deletedTrack.baseNodeId = "base-tracker";
    deletedTrack.nodeId = "tracker";
    deletedTrack.item = "track1";
    deletedTrack.side = MergeSide::eTheirs;
    deletedTrack.base.tracker = trackAlone("track1", 0.);
    deletedTrack.mine.tracker = trackAlone("track1", 4.);
    conflicts.push_back(deletedTrack);

    MergeConflict deletedKnob;
    deletedKnob.kind = MergeConflictKind::eDeleteModify;
    deletedKnob.templateKey = "grade_tpl";
    deletedKnob.baseNodeId = "base-grade";
    deletedKnob.nodeId = "grade";
    deletedKnob.knob = "boost";
    deletedKnob.side = MergeSide::eMine;
    Serialization::KnobSerialization boost = doubleKnob("boost", 2.);
    boost.isUserKnob = true;
    deletedKnob.base.knob = boost;
    boost.dimensions[0].value = Serialization::KnobValueSerialization(3.);
    deletedKnob.theirs.knob = boost;
    conflicts.push_back(deletedKnob);

    MergeConflict deleted;
    deleted.kind = MergeConflictKind::eDeleteModify;
    deleted.templateKey = "grade_tpl";
    deleted.baseNodeId = "base-blur";
    deleted.side = MergeSide::eTheirs;
    deleted.base.knobs = std::list<Serialization::KnobSerialization>({ doubleKnob("size", 2.) });
    conflicts.push_back(deleted);

    MergeConflict reparent;
    reparent.kind = MergeConflictKind::eReparent;
    reparent.templateKey = "grade_tpl";
    reparent.baseNodeId = "base-grade";
    reparent.nodeId = "grade";
    reparent.inputs = { "Source", "Mask" };
    reparent.base.parent = std::string();
    reparent.base.connections = { connection("grade", "Source", "read"), connection("grade", "Mask", "") };
    reparent.mine.parent = "look";
    reparent.mine.connections = { connection("grade", "Source", "look-in") };
    reparent.theirs.parent = "other-look";
    conflicts.push_back(reparent);

    MergeConflict input;
    input.kind = MergeConflictKind::eInput;
    input.templateKey = "grade_tpl";
    input.baseNodeId = "base-grade";
    input.nodeId = "grade";
    input.input = "Source";
    input.base.connections = { connection("grade", "Source", "read") };
    input.mine.connections = { connection("grade", "Source", "") };
    input.theirs.connections = { connection("grade", "Source", "plate") };
    conflicts.push_back(input);

    MergeConflict insert;
    insert.kind = MergeConflictKind::eStructuralInsert;
    insert.templateKey = "grade_tpl";
    insert.baseNodeId = "base-blur";
    insert.nodeId = "blur";
    insert.input = "Source";
    insert.base.connections = { connection("blur", "Source", "grade") };
    insert.mine.connections = { connection("blur", "Source", "artist-blur"), connection("artist-blur", "Source", "grade") };
    insert.theirs.connections = { connection("blur", "Source", "transform") };
    conflicts.push_back(insert);

    MergeConflict adopted;
    adopted.kind = MergeConflictKind::eAdopted;
    adopted.templateKey = "grade_tpl";
    adopted.baseNodeId = "base-grade";
    adopted.nodeId = "grade";
    adopted.side = MergeSide::eTheirs;
    adopted.defaultedKnobs = { "white", "gamma" };
    adopted.base.knobs = std::list<Serialization::KnobSerialization>({ colorKnob("white: [0.9, 0.9, 0.9, 1]\n") });
    adopted.mine.knobs = std::list<Serialization::KnobSerialization>();
    adopted.theirs.knobs = std::list<Serialization::KnobSerialization>({ colorKnob("white: [0.8, 0.9, 0.9, 1]\n"), doubleKnob("gamma", 1.2) });
    conflicts.push_back(adopted);

    return conflicts;
}

void
expectSameConflicts(const std::vector<Serialization::MergeConflict>& expected,
                    const std::vector<Serialization::MergeConflict>& actual)
{
    ASSERT_EQ(expected.size(), actual.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_TRUE(expected[i] == actual[i]) << "conflict " << i << " (" << Serialization::mergeConflictKindName(expected[i].kind) << ")";
    }
}

} // anonymous namespace

TEST(MergeConflictYaml, EveryKindRoundTrips)
{
    const std::vector<Serialization::MergeConflict> conflicts = everyKind();
    const std::string text = Serialization::encodeMergeConflictsYaml(conflicts);
    Serialization::YamlWarnings warnings;
    const std::vector<Serialization::MergeConflict> decoded = Serialization::decodeMergeConflictsYaml(text, &warnings);
    EXPECT_TRUE(warnings.empty());
    expectSameConflicts(conflicts, decoded);
    EXPECT_EQ(text, Serialization::encodeMergeConflictsYaml(decoded));
}

TEST(MergeConflictYaml, AnUnknownKindIsRefused)
{
    const std::string text = "format: natron-merge-conflicts\n"
                             "formatVersion: 1\n"
                             "conflicts:\n"
                             "  - {kind: sideways, baseNode: a}\n";
    EXPECT_THROW(Serialization::decodeMergeConflictsYaml(text), Serialization::YamlError);
}

TEST(MergeConflictYaml, TheBundleHoldsTheMemberOnlyWhileConflictsRemain)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Serialization::ProjectSerialization project;
    project.mergeConflicts = everyKind();
    const QString withConflicts = dir.path() + QString::fromUtf8("/with.ntpz");
    ASSERT_TRUE(writeProjectBundle(project, withConflicts));
    {
        const Serialization::ProjectBundleReader reader(withConflicts.toStdString());
        EXPECT_TRUE(reader.findMember(Serialization::kMergeConflictsMember).has_value());
        Serialization::YamlWarnings warnings;
        const Serialization::ProjectSerialization read = reader.readProject(&warnings);
        EXPECT_TRUE(warnings.empty());
        expectSameConflicts(project.mergeConflicts, read.mergeConflicts);
    }

    project.mergeConflicts.clear();
    const QString resolved = dir.path() + QString::fromUtf8("/resolved.ntpz");
    ASSERT_TRUE(writeProjectBundle(project, resolved));
    const Serialization::ProjectBundleReader reader(resolved.toStdString());
    EXPECT_FALSE(reader.findMember(Serialization::kMergeConflictsMember).has_value());
    EXPECT_TRUE(reader.readProject().mergeConflicts.empty());
}

class MergeConflictLoadTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }
};

TEST_F(MergeConflictLoadTest, LoadingKeepsTheConflictsAndWarnsOnce)
{
    ProjectPtr project = getApp()->getProject();
    const std::vector<Serialization::MergeConflict> conflicts = everyKind();
    project->setMergeConflicts(conflicts);

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString folder = dir.path() + QLatin1Char('/');
    QString saved;
    ASSERT_TRUE(project->saveProject(folder, QString::fromUtf8("shot.ntpz"), &saved));
    project->reset(false, true);
    EXPECT_TRUE(project->getMergeConflicts().empty());

    ASSERT_TRUE(project->loadProject(folder, QString::fromUtf8("shot.ntpz")));
    expectSameConflicts(conflicts, project->getMergeConflicts());
    const QStringList warnings = project->getLoadWarnings();
    ASSERT_EQ(warnings.size(), 1) << warnings.join(QString::fromUtf8("; ")).toStdString();
    EXPECT_EQ(warnings.front().toStdString(), std::to_string(conflicts.size()) + " unresolved merge conflicts");
}
