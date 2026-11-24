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
#include <set>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/GraphOpApply.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Project.h"
#include "Engine/ProjectMerge.h"

#include "Serialization/Merge/MergeConflict.h"
#include "Serialization/Ops/GraphOp.h"
#include "Serialization/Ops/SnapshotDiff.h"
#include "Serialization/ProjectSerialization.h"

NATRON_NAMESPACE_USING

class ProjectMergeTest
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

    ProjectPtr project() const
    {
        return getApp()->getProject();
    }

    NodePtr createBlur(const std::string& name)
    {
        CreateNodeArgs args(PLUGINID_NATRON_BLUR, project());
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
        args.setProperty<std::string>(kCreateNodeArgsPropNodeInitialName, name);
        NodePtr node = getApp()->createNode(args);
        if (node && (node->getScriptName() != name)) {
            node->setScriptName(name);
        }

        return node;
    }

    static KnobDoublePtr blurSize(const NodePtr& blur)
    {
        return std::dynamic_pointer_cast<KnobDouble>(blur->getKnobByName(kBlurParamSize));
    }

    static void setSize(const NodePtr& blur,
                        double value)
    {
        KnobDoublePtr size = blurSize(blur);
        ASSERT_NE(size, nullptr);
        size->setValue(value, ViewSpec::all(), 0);
        size->setValue(value, ViewSpec::all(), 1);
    }

    NodePtr nodeNamed(const std::string& name) const
    {
        return project()->getNodeByFullySpecifiedName(name);
    }

    // Writes template version 1 (Blur1 -> Blur2) as tpl.ntpz and version 2, where the supervisor
    // set Blur1's size to 4, as tpl_v2.ntpz.
    void saveTemplateVersions(const QTemporaryDir& dir,
                              QString* v1,
                              QString* v2)
    {
        NodePtr first = createBlur("Blur1");
        NodePtr second = createBlur("Blur2");
        ASSERT_NE(first, nullptr);
        ASSERT_NE(second, nullptr);
        ASSERT_TRUE(NodeCollection::connectNodes(0, first, second, false));
        ASSERT_TRUE(project()->saveProject(dir.path() + QLatin1Char('/'), QString::fromUtf8("tpl.ntpz"), v1));
        setSize(first, 4.);
        ASSERT_TRUE(project()->saveProject(dir.path() + QLatin1Char('/'), QString::fromUtf8("tpl_v2.ntpz"), v2));
        project()->reset(false, true);
    }

    // Writes template version 1 (Blur1 -> Blur2) as tpl.ntpz, then edit's result as tpl_v2.ntpz.
    void saveTemplateVersions(const QTemporaryDir& dir,
                              const std::function<void()>& edit,
                              QString* v1,
                              QString* v2)
    {
        NodePtr first = createBlur("Blur1");
        NodePtr second = createBlur("Blur2");
        ASSERT_NE(first, nullptr);
        ASSERT_NE(second, nullptr);
        ASSERT_TRUE(NodeCollection::connectNodes(0, first, second, false));
        ASSERT_TRUE(project()->saveProject(dir.path() + QLatin1Char('/'), QString::fromUtf8("tpl.ntpz"), v1));
        edit();
        ASSERT_TRUE(project()->saveProject(dir.path() + QLatin1Char('/'), QString::fromUtf8("tpl_v2.ntpz"), v2));
        project()->reset(false, true);
    }

    Serialization::ProjectSerialization state() const
    {
        Serialization::ProjectSerialization live;
        project()->toSerialization(&live);

        return Serialization::editableState(live);
    }

    // Applies the side's (or, with option set, the option's) operations, runs check, then applies
    // the reverse diff, as undo does, and expects the project as it was.
    void choose(const Serialization::MergeConflict& conflict,
                ProjectMerge::ConflictSide side,
                const std::function<void()>& check,
                const std::string& option = std::string())
    {
        const Serialization::ProjectSerialization before = state();
        std::vector<Serialization::GraphOp> ops;
        QString error;
        if (option.empty()) {
            ASSERT_TRUE(ProjectMerge::canChooseSide(project(), conflict, side));
            ASSERT_TRUE(ProjectMerge::sideOperations(project(), conflict, side, &ops, &error)) << error.toStdString();
        } else {
            ASSERT_TRUE(ProjectMerge::canChooseOption(project(), conflict, option));
            ASSERT_TRUE(ProjectMerge::optionOperations(project(), conflict, option, &ops, &error)) << error.toStdString();
        }
        for (const Serialization::OpResult& result : applyGraphOps(project(), ops)) {
            EXPECT_TRUE(result.applied()) << result.message;
        }
        check();
        const Serialization::ProjectSerialization after = state();
        for (const Serialization::OpResult& result : applyGraphOps(project(), Serialization::diffSnapshots(after, before))) {
            EXPECT_TRUE(result.applied()) << result.message;
        }
        EXPECT_TRUE(Serialization::diffSnapshots(state(), before).empty());
    }

    const Serialization::MergeConflict* conflictOf(const ProjectMerge::UpdateResult& result,
                                                   Serialization::MergeConflictKind kind,
                                                   int dimension = -1) const
    {
        for (const Serialization::MergeConflict& conflict : result.conflicts) {
            if ((conflict.kind == kind) && ((dimension < 0) || (conflict.dimension == dimension))) {
                return &conflict;
            }
        }

        return nullptr;
    }

    // The shot: the artist's own Artist node and the imported template.
    void importShot(const QString& v1)
    {
        ASSERT_NE(createBlur("Artist"), nullptr);
        QString error;
        const NodesList imported = project()->importTemplate(v1, NodeCollectionPtr(), &error);
        ASSERT_EQ(imported.size(), 2u) << error.toStdString();
        ASSERT_EQ(project()->getTemplateImports().size(), 1u);
        ASSERT_NE(nodeNamed("Blur1"), nullptr);
        ASSERT_NE(nodeNamed("Blur2"), nullptr);
    }
};

TEST_F(ProjectMergeTest, ACleanUpdateChangesTheTemplateNodesAndKeepsTheArtistsOverrides)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString v1;
    QString v2;
    saveTemplateVersions(dir, &v1, &v2);
    importShot(v1);
    const std::string key = project()->getTemplateImports().front().key;
    const std::string oldHash = project()->getTemplateImports().front().contentHash;
    NodePtr blur1 = nodeNamed("Blur1");
    NodePtr blur2 = nodeNamed("Blur2");
    NodePtr artist = nodeNamed("Artist");
    const double blur1Before = blurSize(blur1)->getValue(0);
    setSize(blur2, 7.);
    setSize(artist, 9.);
    ASSERT_NE(blur1Before, 4.);

    std::vector<Serialization::GraphOp> applied;
    const ProjectMerge::UpdateResult result = ProjectMerge::updateFromTemplate(project(), key, v2, [&](const std::vector<Serialization::GraphOp>& ops) {
        applied = ops;

        return applyGraphOps(project(), ops);
    });
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_TRUE(result.conflicts.empty());
    EXPECT_TRUE(project()->getMergeConflicts().empty());

    EXPECT_DOUBLE_EQ(blurSize(blur1)->getValue(0), 4.);
    EXPECT_DOUBLE_EQ(blurSize(blur1)->getValue(1), 4.);
    EXPECT_DOUBLE_EQ(blurSize(blur2)->getValue(0), 7.);
    EXPECT_DOUBLE_EQ(blurSize(artist)->getValue(0), 9.);

    ASSERT_FALSE(applied.empty());
    for (const Serialization::GraphOp& op : applied) {
        const Serialization::SetKnobOp* knob = std::get_if<Serialization::SetKnobOp>(&op);
        ASSERT_NE(knob, nullptr);
        ASSERT_TRUE(knob->nodeId.has_value());
        EXPECT_EQ(*knob->nodeId, blur1->getNodeId());
        EXPECT_EQ(knob->knobName, kBlurParamSize);
    }

    const Serialization::TemplateImportSerialization& entry = project()->getTemplateImports().front();
    EXPECT_NE(entry.contentHash, oldHash);
    ASSERT_NE(entry.base, nullptr);

    const ProjectMerge::UpdateResult again = ProjectMerge::updateFromTemplate(project(), key, v2);
    ASSERT_TRUE(again.ok) << again.error.toStdString();
    EXPECT_TRUE(again.ops.empty());
    EXPECT_TRUE(again.conflicts.empty());
}

TEST_F(ProjectMergeTest, AConflictedUpdateReturnsItsConflictsAndKeepsMine)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString v1;
    QString v2;
    saveTemplateVersions(dir, &v1, &v2);
    importShot(v1);
    const std::string key = project()->getTemplateImports().front().key;
    NodePtr blur1 = nodeNamed("Blur1");
    setSize(blur1, 7.);

    const ProjectMerge::UpdateResult result = ProjectMerge::updateFromTemplate(project(), key, v2);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    ASSERT_EQ(result.conflicts.size(), 1u);
    const Serialization::MergeConflict& conflict = result.conflicts.front();
    EXPECT_EQ(conflict.kind, Serialization::MergeConflictKind::eValue);
    EXPECT_EQ(conflict.knob, kBlurParamSize);
    EXPECT_EQ(conflict.dimension, Serialization::kMergeConflictAllDimensions);
    EXPECT_EQ(conflict.nodeId, blur1->getNodeId());
    EXPECT_EQ(conflict.templateKey, key);

    EXPECT_DOUBLE_EQ(blurSize(blur1)->getValue(0), 7.);
    EXPECT_DOUBLE_EQ(blurSize(blur1)->getValue(1), 7.);
    EXPECT_FALSE(project()->getMergeConflicts().empty());
    for (const Serialization::MergeConflict& saved : project()->getMergeConflicts()) {
        EXPECT_EQ(saved.nodeId, blur1->getNodeId());
    }
}

TEST_F(ProjectMergeTest, AnUnknownKeyOrMissingFileFailsWithoutChangingTheProject)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString v1;
    QString v2;
    saveTemplateVersions(dir, &v1, &v2);
    importShot(v1);
    const std::string key = project()->getTemplateImports().front().key;
    const std::string hash = project()->getTemplateImports().front().contentHash;

    EXPECT_FALSE(ProjectMerge::updateFromTemplate(project(), "no_such_template", v2).ok);
    const ProjectMerge::UpdateResult missing = ProjectMerge::updateFromTemplate(project(), key, dir.path() + QString::fromUtf8("/gone.ntpz"));
    EXPECT_FALSE(missing.ok);
    EXPECT_FALSE(missing.error.isEmpty());
    EXPECT_EQ(project()->getTemplateImports().front().contentHash, hash);
    EXPECT_DOUBLE_EQ(blurSize(nodeNamed("Blur1"))->getValue(0), blurSize(nodeNamed("Blur1"))->getValue(1));
}

TEST_F(ProjectMergeTest, AnUpdateWhoseOperationFailsLeavesTheGraphAsItWas)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString v1;
    QString v2;
    saveTemplateVersions(dir, &v1, &v2);
    importShot(v1);
    const std::string key = project()->getTemplateImports().front().key;
    const std::string hash = project()->getTemplateImports().front().contentHash;
    const Serialization::ProjectSerialization before = state();

    // Every operation applies, but the applier reports the last one as failed, as one that stops
    // partway would.
    bool appliedSome = false;
    const ProjectMerge::UpdateResult result = ProjectMerge::updateFromTemplate(project(), key, v2, [&](const std::vector<Serialization::GraphOp>& ops) {
        std::vector<Serialization::OpResult> results = applyGraphOps(project(), ops);
        appliedSome = !Serialization::diffSnapshots(before, state()).empty();
        if (!results.empty()) {
            results.back() = Serialization::OpResult::invalid("refused");
        }

        return results;
    });
    EXPECT_TRUE(appliedSome);
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.error.isEmpty());
    EXPECT_TRUE(Serialization::diffSnapshots(state(), before).empty());
    EXPECT_EQ(project()->getTemplateImports().front().contentHash, hash);
    EXPECT_TRUE(project()->getMergeConflicts().empty());
}

TEST_F(ProjectMergeTest, ABatchWithAFailingOperationAppliesNoneOfIt)
{
    NodePtr blur = createBlur("Blur1");
    ASSERT_NE(blur, nullptr);
    setSize(blur, 3.);
    const Serialization::ProjectSerialization before = state();

    bool restored = false;
    const std::vector<Serialization::GraphOp> ops = { Serialization::SetKnobOp { blur->getNodeId(), kBlurParamSize, std::nullopt },
                                                      Serialization::SetKnobOp { blur->getNodeId(), "noSuchKnob", std::nullopt } };
    const std::vector<Serialization::OpResult> results = ProjectMerge::applyAllOrNothing(project(), ops, &restored);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_TRUE(results[0].applied());
    EXPECT_FALSE(results[1].applied());
    EXPECT_TRUE(restored);
    EXPECT_DOUBLE_EQ(blurSize(blur)->getValue(0), 3.);
    EXPECT_TRUE(Serialization::diffSnapshots(state(), before).empty());
}

TEST_F(ProjectMergeTest, CompareSummarisesAddedChangedAndRemovedNodes)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString v1;
    QString v2;
    saveTemplateVersions(dir, &v1, &v2);
    importShot(v1);
    Serialization::ProjectSerialization before;
    project()->toSerialization(&before);

    NodePtr blur1 = nodeNamed("Blur1");
    NodePtr blur2 = nodeNamed("Blur2");
    const std::string blur1Id = blur1->getNodeId();
    const std::string blur2Id = blur2->getNodeId();
    const std::string artistId = nodeNamed("Artist")->getNodeId();
    setSize(blur1, 5.);
    blur2->destroyNode(true, false);
    NodePtr extra = createBlur("Extra");
    ASSERT_NE(extra, nullptr);

    const ProjectMerge::ChangeSummary summary = ProjectMerge::compare(project(), before);
    EXPECT_EQ(summary.find(artistId), nullptr);
    ASSERT_EQ(summary.nodes.size(), 3u);
    const ProjectMerge::NodeChange* changed = summary.find(blur1Id);
    ASSERT_NE(changed, nullptr);
    EXPECT_EQ(changed->kind, ProjectMerge::NodeChange::eChanged);
    EXPECT_EQ(changed->knobs, std::vector<std::string>(1, kBlurParamSize));
    EXPECT_TRUE(changed->inputs.empty());
    const ProjectMerge::NodeChange* removed = summary.find(blur2Id);
    ASSERT_NE(removed, nullptr);
    EXPECT_EQ(removed->kind, ProjectMerge::NodeChange::eRemoved);
    EXPECT_EQ(removed->scriptName, "Blur2");
    const ProjectMerge::NodeChange* added = summary.find(extra->getNodeId());
    ASSERT_NE(added, nullptr);
    EXPECT_EQ(added->kind, ProjectMerge::NodeChange::eAdded);
    EXPECT_EQ(added->scriptName, "Extra");
}

TEST_F(ProjectMergeTest, AValueConflictResolvesToEachSideAndUndoes)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString v1;
    QString v2;
    saveTemplateVersions(dir, &v1, &v2);
    importShot(v1);
    const std::string key = project()->getTemplateImports().front().key;
    NodePtr blur1 = nodeNamed("Blur1");
    const double base = blurSize(blur1)->getValue(0);
    setSize(blur1, 7.);
    const ProjectMerge::UpdateResult result = ProjectMerge::updateFromTemplate(project(), key, v2);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    ASSERT_EQ(result.conflicts.size(), 1u);
    const Serialization::MergeConflict* conflict = conflictOf(result, Serialization::MergeConflictKind::eValue);
    ASSERT_NE(conflict, nullptr);
    EXPECT_EQ(conflict->dimension, Serialization::kMergeConflictAllDimensions);

    choose(*conflict, ProjectMerge::ConflictSide::eMine, [&] {
        EXPECT_DOUBLE_EQ(blurSize(blur1)->getValue(0), 7.);
        EXPECT_DOUBLE_EQ(blurSize(blur1)->getValue(1), 7.);
    });
    choose(*conflict, ProjectMerge::ConflictSide::eTheirs, [&] {
        EXPECT_DOUBLE_EQ(blurSize(blur1)->getValue(0), 4.);
        EXPECT_DOUBLE_EQ(blurSize(blur1)->getValue(1), 4.);
    });
    choose(*conflict, ProjectMerge::ConflictSide::eBase, [&] {
        EXPECT_DOUBLE_EQ(blurSize(blur1)->getValue(0), base);
        EXPECT_DOUBLE_EQ(blurSize(blur1)->getValue(1), base);
    });
    EXPECT_DOUBLE_EQ(blurSize(blur1)->getValue(0), 7.);
    EXPECT_DOUBLE_EQ(blurSize(blur1)->getValue(1), 7.);
}

TEST_F(ProjectMergeTest, AnUnfoldedSideConflictsDimensionByDimensionWithAFoldedOne)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString v1;
    QString v2;
    saveTemplateVersions(dir, &v1, &v2);
    importShot(v1);
    const std::string key = project()->getTemplateImports().front().key;
    NodePtr blur1 = nodeNamed("Blur1");
    KnobDoublePtr size = blurSize(blur1);
    ASSERT_NE(size, nullptr);
    size->setValue(7., ViewSpec::all(), 0);
    size->setValue(8., ViewSpec::all(), 1);
    const ProjectMerge::UpdateResult result = ProjectMerge::updateFromTemplate(project(), key, v2);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    ASSERT_EQ(result.conflicts.size(), 2u);
    const Serialization::MergeConflict* first = conflictOf(result, Serialization::MergeConflictKind::eValue, 0);
    const Serialization::MergeConflict* second = conflictOf(result, Serialization::MergeConflictKind::eValue, 1);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_DOUBLE_EQ(size->getValue(0), 7.);
    EXPECT_DOUBLE_EQ(size->getValue(1), 8.);

    choose(*first, ProjectMerge::ConflictSide::eTheirs, [&] {
        EXPECT_DOUBLE_EQ(size->getValue(0), 4.);
        EXPECT_DOUBLE_EQ(size->getValue(1), 8.);
    });
    choose(*second, ProjectMerge::ConflictSide::eTheirs, [&] {
        EXPECT_DOUBLE_EQ(size->getValue(0), 7.);
        EXPECT_DOUBLE_EQ(size->getValue(1), 4.);
    });
    EXPECT_DOUBLE_EQ(size->getValue(0), 7.);
    EXPECT_DOUBLE_EQ(size->getValue(1), 8.);
}

TEST_F(ProjectMergeTest, AnInputConflictResolvesToEachSidesSourceAndUndoes)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString v1;
    QString v2;
    saveTemplateVersions(dir, [this] {
        NodePtr feed = createBlur("Blur3");
        NodePtr second = nodeNamed("Blur2");
        ASSERT_NE(feed, nullptr);
        ASSERT_NE(second, nullptr);
        second->disconnectInput(0);
        ASSERT_TRUE(NodeCollection::connectNodes(0, feed, second, false)); }, &v1, &v2);
    importShot(v1);
    const std::string key = project()->getTemplateImports().front().key;
    NodePtr blur2 = nodeNamed("Blur2");
    blur2->disconnectInput(0);
    ASSERT_TRUE(NodeCollection::connectNodes(0, nodeNamed("Artist"), blur2, false));
    const ProjectMerge::UpdateResult result = ProjectMerge::updateFromTemplate(project(), key, v2, [this](const std::vector<Serialization::GraphOp>& ops) {
        return applyGraphOps(project(), ops);
    });
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    const Serialization::MergeConflict* conflict = conflictOf(result, Serialization::MergeConflictKind::eInput);
    ASSERT_NE(conflict, nullptr);
    EXPECT_EQ(conflict->nodeId, blur2->getNodeId());
    ASSERT_NE(nodeNamed("Blur3"), nullptr);

    const auto sourceIs = [&](const std::string& name) {
        NodePtr source = blur2->getInput(0);
        ASSERT_NE(source, nullptr);
        EXPECT_EQ(source->getScriptName(), name);
    };
    choose(*conflict, ProjectMerge::ConflictSide::eMine, [&] { sourceIs("Artist"); });
    choose(*conflict, ProjectMerge::ConflictSide::eTheirs, [&] { sourceIs("Blur3"); });
    choose(*conflict, ProjectMerge::ConflictSide::eBase, [&] { sourceIs("Blur1"); });
    sourceIs("Artist");
}

TEST_F(ProjectMergeTest, ADeleteModifyConflictDeletesOnlyForTheDeletingSideAndUndoes)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString v1;
    QString v2;
    saveTemplateVersions(dir, [this] {
        NodePtr second = nodeNamed("Blur2");
        ASSERT_NE(second, nullptr);
        second->destroyNode(true, false); }, &v1, &v2);
    importShot(v1);
    const std::string key = project()->getTemplateImports().front().key;
    const double base = blurSize(nodeNamed("Blur2"))->getValue(0);
    setSize(nodeNamed("Blur2"), 7.);
    const ProjectMerge::UpdateResult result = ProjectMerge::updateFromTemplate(project(), key, v2, [this](const std::vector<Serialization::GraphOp>& ops) {
        return applyGraphOps(project(), ops);
    });
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    const Serialization::MergeConflict* conflict = conflictOf(result, Serialization::MergeConflictKind::eDeleteModify);
    ASSERT_NE(conflict, nullptr);
    EXPECT_EQ(conflict->side, Serialization::MergeSide::eTheirs);
    ASSERT_NE(nodeNamed("Blur2"), nullptr);

    choose(*conflict, ProjectMerge::ConflictSide::eMine, [&] { EXPECT_DOUBLE_EQ(blurSize(nodeNamed("Blur2"))->getValue(0), 7.); });
    choose(*conflict, ProjectMerge::ConflictSide::eTheirs, [&] { EXPECT_EQ(nodeNamed("Blur2"), nullptr); });
    choose(*conflict, ProjectMerge::ConflictSide::eBase, [&] {
        NodePtr blur2 = nodeNamed("Blur2");
        ASSERT_NE(blur2, nullptr);
        EXPECT_DOUBLE_EQ(blurSize(blur2)->getValue(0), base);
    });
    ASSERT_NE(nodeNamed("Blur2"), nullptr);
    EXPECT_DOUBLE_EQ(blurSize(nodeNamed("Blur2"))->getValue(0), 7.);
}

TEST_F(ProjectMergeTest, ACurveKeyConflictResolvesToEachSideOrKeepsBothKeysAndUndoes)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString v1;
    QString v2;
    saveTemplateVersions(dir, [this] {
        NodePtr first = nodeNamed("Blur1");
        ASSERT_NE(first, nullptr);
        blurSize(first)->setValueAtTime(10, 4., ViewSpec::all(), 0); }, &v1, &v2);
    importShot(v1);
    const std::string key = project()->getTemplateImports().front().key;
    NodePtr blur1 = nodeNamed("Blur1");
    blurSize(blur1)->setValueAtTime(10, 7., ViewSpec::all(), 0);
    const ProjectMerge::UpdateResult result = ProjectMerge::updateFromTemplate(project(), key, v2);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    const Serialization::MergeConflict* conflict = conflictOf(result, Serialization::MergeConflictKind::eCurveKey, 0);
    ASSERT_NE(conflict, nullptr);
    ASSERT_EQ(conflict->options.size(), 1u);
    const std::string keepBoth(Serialization::kMergeOptionKeepBothKeys);
    EXPECT_EQ(conflict->options.front().name, keepBoth);

    const auto keys = [&] {
        return blurSize(blur1)->getKeyFramesCount(ViewSpec::all(), 0);
    };
    choose(*conflict, ProjectMerge::ConflictSide::eMine, [&] { EXPECT_DOUBLE_EQ(blurSize(blur1)->getValueAtTime(10, 0), 7.); });
    choose(*conflict, ProjectMerge::ConflictSide::eTheirs, [&] {
        EXPECT_EQ(keys(), 1);
        EXPECT_DOUBLE_EQ(blurSize(blur1)->getValueAtTime(10, 0), 4.);
    });
    choose(*conflict, ProjectMerge::ConflictSide::eBase, [&] { EXPECT_EQ(keys(), 0); });
    choose(*conflict, ProjectMerge::ConflictSide::eMine, [&] {
        EXPECT_EQ(keys(), 2);
        EXPECT_DOUBLE_EQ(blurSize(blur1)->getValueAtTime(10, 0), 7.);
        EXPECT_DOUBLE_EQ(blurSize(blur1)->getValueAtTime(11, 0), 4.); }, keepBoth);
    EXPECT_EQ(keys(), 1);
    EXPECT_FALSE(ProjectMerge::canChooseOption(project(), *conflict, "no-such-choice"));
}
