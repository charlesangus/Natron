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

    // Applies ops as one batch, runs check, then applies the reverse diff, as undo does, and
    // expects the project as it was.
    void applyAndUndo(const std::vector<Serialization::GraphOp>& ops,
                      const std::function<void()>& check)
    {
        const Serialization::ProjectSerialization before = state();
        bool restored = false;
        for (const Serialization::OpResult& result : ProjectMerge::applyAllOrNothing(project(), ops, &restored)) {
            EXPECT_TRUE(result.applied()) << result.message;
        }
        check();
        const Serialization::ProjectSerialization after = state();
        for (const Serialization::OpResult& result : applyGraphOps(project(), Serialization::diffSnapshots(after, before))) {
            EXPECT_TRUE(result.applied()) << result.message;
        }
        EXPECT_TRUE(Serialization::diffSnapshots(state(), before).empty());
    }

    // Blur1 -> Blur2 -> ... -> Blur<count>, 100 units apart down the graph.
    std::vector<NodePtr> createChain(int count)
    {
        std::vector<NodePtr> chain;
        for (int i = 1; i <= count; ++i) {
            NodePtr blur = createBlur("Blur" + std::to_string(i));
            EXPECT_NE(blur, nullptr);
            if (!blur) {
                return std::vector<NodePtr>();
            }
            blur->setPosition(0., 100. * (i - 1));
            if (!chain.empty()) {
                EXPECT_TRUE(NodeCollection::connectNodes(0, chain.back(), blur, false));
            }
            chain.push_back(blur);
        }

        return chain;
    }

    Serialization::ProjectSerialization snapshot() const
    {
        Serialization::ProjectSerialization saved;
        project()->toSerialization(&saved);

        return saved;
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

    NodePtr createIn(const char* pluginID,
                     const NodeCollectionPtr& parent,
                     const std::string& name)
    {
        CreateNodeArgs args(pluginID, parent);
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
        args.setProperty<bool>(kCreateNodeArgsPropNodeGroupDisableCreateInitialNodes, true);
        args.setProperty<std::string>(kCreateNodeArgsPropNodeInitialName, name);
        NodePtr node = getApp()->createNode(args);
        if (node && (node->getScriptName() != name)) {
            node->setScriptName(name);
        }

        return node;
    }

    // Writes the template Blur1 -> Blur2 -> Blur3, 100 units apart down the graph, as tpl.ntpz,
    // then edit's result as tpl_v2.ntpz.
    void saveChainTemplate(const QTemporaryDir& dir,
                           const std::function<void()>& edit,
                           QString* v1,
                           QString* v2)
    {
        NodePtr previous;
        for (int i = 1; i <= 3; ++i) {
            NodePtr blur = createBlur("Blur" + std::to_string(i));
            ASSERT_NE(blur, nullptr);
            blur->setPosition(0., 100. * (i - 1));
            if (previous) {
                ASSERT_TRUE(NodeCollection::connectNodes(0, previous, blur, false));
            }
            previous = blur;
        }
        ASSERT_TRUE(project()->saveProject(dir.path() + QLatin1Char('/'), QString::fromUtf8("tpl.ntpz"), v1));
        edit();
        ASSERT_TRUE(project()->saveProject(dir.path() + QLatin1Char('/'), QString::fromUtf8("tpl_v2.ntpz"), v2));
        project()->reset(false, true);
    }

    void importChain(const QString& v1)
    {
        QString error;
        ASSERT_EQ(project()->importTemplate(v1, NodeCollectionPtr(), &error).size(), 3u) << error.toStdString();
        ASSERT_EQ(project()->getTemplateImports().size(), 1u);
    }

    static void moveBy(const NodePtr& node,
                       double dx,
                       double dy)
    {
        double x = 0.;
        double y = 0.;
        node->getPosition(&x, &y);
        node->setPosition(x + dx, y + dy);
    }

    static void expectAt(const NodePtr& node,
                         double x,
                         double y)
    {
        double atX = 0.;
        double atY = 0.;
        node->getPosition(&atX, &atY);
        EXPECT_DOUBLE_EQ(atX, x);
        EXPECT_DOUBLE_EQ(atY, y);
    }

    ProjectMerge::ChangeSummary compareWithBase()
    {
        ProjectMerge::ChangeSummary summary;
        QString error;
        EXPECT_TRUE(ProjectMerge::compareWithTemplateBase(project(), project()->getTemplateImports().front().key, &summary, &error)) << error.toStdString();

        return summary;
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

    const Serialization::TemplateImportSerialization entry = project()->getTemplateImports().front();
    EXPECT_NE(entry.contentHash, oldHash);
    ASSERT_NE(entry.base, nullptr);

    const ProjectMerge::UpdateResult again = ProjectMerge::updateFromTemplate(project(), key, v2);
    ASSERT_TRUE(again.ok) << again.error.toStdString();
    EXPECT_TRUE(again.ops.empty());
    EXPECT_TRUE(again.conflicts.empty());
}

TEST_F(ProjectMergeTest, AnUpdateTagsTheNodeThatStandsInForADeletedTemplateNode)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString v1;
    QString v2;
    saveTemplateVersions(dir, &v1, &v2);
    QString error;
    ASSERT_EQ(project()->importTemplate(v1, NodeCollectionPtr(), &error).size(), 2u) << error.toStdString();
    const std::string key = project()->getTemplateImports().front().key;
    NodePtr original = nodeNamed("Blur1");
    NodePtr blur2 = nodeNamed("Blur2");
    ASSERT_NE(original, nullptr);
    ASSERT_NE(blur2, nullptr);
    const std::string templateNode = original->getOriginNode();
    ASSERT_FALSE(templateNode.empty());

    NodePtr copy = createBlur("Blur1_copy");
    ASSERT_NE(copy, nullptr);
    EXPECT_TRUE(copy->getOriginTemplate().empty());
    const std::vector<Serialization::GraphOp> deletion = { Serialization::DeleteNodeOp { original->getNodeId() } };
    for (const Serialization::OpResult& result : applyGraphOps(project(), deletion)) {
        ASSERT_TRUE(result.applied()) << result.message;
    }
    original.reset();
    ASSERT_TRUE(NodeCollection::connectNodes(0, copy, blur2, false));

    const Serialization::ProjectSerialization before = state();
    const ProjectMerge::UpdateResult result = ProjectMerge::updateFromTemplate(project(), key, v2);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    bool tagged = false;
    for (const Serialization::GraphOp& op : result.ops) {
        if (const Serialization::SetOriginOp* origin = std::get_if<Serialization::SetOriginOp>(&op)) {
            EXPECT_EQ(origin->nodeId, copy->getNodeId());
            tagged = true;
        }
    }
    EXPECT_TRUE(tagged);
    EXPECT_EQ(copy->getOriginTemplate(), key);
    EXPECT_EQ(copy->getOriginNode(), templateNode);
    EXPECT_DOUBLE_EQ(blurSize(copy)->getValue(0), 4.);

    const Serialization::ProjectSerialization after = state();
    for (const Serialization::OpResult& undone : applyGraphOps(project(), Serialization::diffSnapshots(after, before))) {
        EXPECT_TRUE(undone.applied()) << undone.message;
    }
    EXPECT_TRUE(copy->getOriginTemplate().empty());
    EXPECT_TRUE(copy->getOriginNode().empty());
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
    ASSERT_EQ(changed->knobChanges.size(), 1u);
    EXPECT_EQ(changed->knobChanges[0].name, kBlurParamSize);
    EXPECT_TRUE(changed->knobChanges[0].after.has_value());
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

TEST_F(ProjectMergeTest, ASameFrameCurveKeyConflictResolvesToEachSideWithoutKeepBothAndUndoes)
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
    EXPECT_TRUE(conflict->options.empty());
    const std::vector<ProjectMerge::ConflictChoice> choices = ProjectMerge::availableChoices(project(), *conflict);
    ASSERT_EQ(choices.size(), 3u);
    EXPECT_TRUE(choices[0].option.empty() && (choices[0].side == ProjectMerge::ConflictSide::eMine));
    EXPECT_TRUE(choices[1].option.empty() && (choices[1].side == ProjectMerge::ConflictSide::eTheirs));
    EXPECT_TRUE(choices[2].option.empty() && (choices[2].side == ProjectMerge::ConflictSide::eBase));

    const auto keys = [&] {
        return blurSize(blur1)->getKeyFramesCount(ViewSpec::all(), 0);
    };
    choose(*conflict, ProjectMerge::ConflictSide::eMine, [&] { EXPECT_DOUBLE_EQ(blurSize(blur1)->getValueAtTime(10, 0), 7.); });
    choose(*conflict, ProjectMerge::ConflictSide::eTheirs, [&] {
        EXPECT_EQ(keys(), 1);
        EXPECT_DOUBLE_EQ(blurSize(blur1)->getValueAtTime(10, 0), 4.);
    });
    choose(*conflict, ProjectMerge::ConflictSide::eBase, [&] { EXPECT_EQ(keys(), 0); });
    EXPECT_EQ(keys(), 1);
    EXPECT_FALSE(ProjectMerge::canChooseOption(project(), *conflict, std::string(Serialization::kMergeOptionKeepBothKeys)));
    EXPECT_FALSE(ProjectMerge::canChooseOption(project(), *conflict, "no-such-choice"));
}

TEST_F(ProjectMergeTest, MovingTheWholeTemplateIsNoChange)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString v1;
    QString v2;
    saveChainTemplate(dir, [] { }, &v1, &v2);
    importChain(v1);
    for (int i = 1; i <= 3; ++i) {
        moveBy(nodeNamed("Blur" + std::to_string(i)), 500., 300.);
    }

    const ProjectMerge::ChangeSummary summary = compareWithBase();
    for (const ProjectMerge::NodeChange& change : summary.nodes) {
        EXPECT_FALSE(change.moved) << change.scriptName;
        EXPECT_FALSE(change.appearance) << change.scriptName;
    }
    for (int i = 1; i <= 3; ++i) {
        EXPECT_EQ(summary.find(nodeNamed("Blur" + std::to_string(i))->getNodeId()), nullptr);
    }
}

TEST_F(ProjectMergeTest, ANodeMovedWithinTheTemplateIsOnlyMoved)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString v1;
    QString v2;
    saveChainTemplate(dir, [] { }, &v1, &v2);
    importChain(v1);
    for (int i = 1; i <= 3; ++i) {
        moveBy(nodeNamed("Blur" + std::to_string(i)), 500., 300.);
    }
    NodePtr blur2 = nodeNamed("Blur2");
    moveBy(blur2, 150., 0.);
    moveBy(nodeNamed("Blur3"), 4., -3.);

    const ProjectMerge::ChangeSummary summary = compareWithBase();
    ASSERT_EQ(summary.nodes.size(), 1u);
    const ProjectMerge::NodeChange& change = summary.nodes.front();
    EXPECT_EQ(change.nodeId, blur2->getNodeId());
    EXPECT_EQ(change.kind, ProjectMerge::NodeChange::eChanged);
    EXPECT_TRUE(change.moved);
    EXPECT_FALSE(change.appearance);
    EXPECT_FALSE(change.renamed);
    EXPECT_TRUE(change.knobs.empty());
    EXPECT_TRUE(change.inputs.empty());
}

TEST_F(ProjectMergeTest, ARemovedNodeIsPlacedByItsNeighboursOffset)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString v1;
    QString v2;
    saveChainTemplate(dir, [] { }, &v1, &v2);
    importChain(v1);
    moveBy(nodeNamed("Blur1"), 500., 300.);
    moveBy(nodeNamed("Blur2"), 650., 300.);
    NodePtr blur3 = nodeNamed("Blur3");
    // A removed node has no live ID left to match, so the compare names it by its template ID.
    const std::string blur3TemplateId = blur3->getOriginNode();
    ASSERT_FALSE(blur3TemplateId.empty());
    blur3->destroyNode(true, false);
    blur3.reset();

    const ProjectMerge::ChangeSummary summary = compareWithBase();
    const ProjectMerge::NodeChange* removed = summary.find(blur3TemplateId);
    ASSERT_NE(removed, nullptr);
    EXPECT_EQ(removed->kind, ProjectMerge::NodeChange::eRemoved);
    EXPECT_DOUBLE_EQ(removed->x, 0.);
    EXPECT_DOUBLE_EQ(removed->y, 200.);
    EXPECT_DOUBLE_EQ(removed->placedX, 650.);
    EXPECT_DOUBLE_EQ(removed->placedY, 500.);
}

TEST_F(ProjectMergeTest, NodesInAGroupUseTheGroupsOwnOffset)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    NodePtr group = createIn(PLUGINID_NATRON_GROUP, project(), "Look");
    ASSERT_NE(group, nullptr);
    NodeGroupPtr collection = std::dynamic_pointer_cast<NodeGroup>(group->getEffectInstance());
    ASSERT_NE(collection, nullptr);
    group->setPosition(0., 0.);
    NodePtr outside = createBlur("Outside");
    ASSERT_NE(outside, nullptr);
    outside->setPosition(0., 100.);
    // Inner1 -> Inner2 -> Inner3, with Inner4 and Inner5 unwired.
    NodePtr previous;
    for (int i = 1; i <= 5; ++i) {
        NodePtr inner = createIn(PLUGINID_NATRON_BLUR, collection, "Inner" + std::to_string(i));
        ASSERT_NE(inner, nullptr);
        inner->setPosition(100. * i, 0.);
        if (previous && (i <= 3)) {
            ASSERT_TRUE(NodeCollection::connectNodes(0, previous, inner, false));
        }
        previous = inner;
    }
    QString path;
    ASSERT_TRUE(project()->saveProject(dir.path() + QLatin1Char('/'), QString::fromUtf8("tpl.ntpz"), &path));
    project()->reset(false, true);
    QString error;
    ASSERT_FALSE(project()->importTemplate(path, NodeCollectionPtr(), &error).empty()) << error.toStdString();

    moveBy(nodeNamed("Look"), 500., 300.);
    moveBy(nodeNamed("Outside"), 500., 300.);
    for (int i = 1; i <= 5; ++i) {
        moveBy(nodeNamed("Look.Inner" + std::to_string(i)), -50., 20.);
    }
    NodePtr inner2 = nodeNamed("Look.Inner2");
    ASSERT_NE(inner2, nullptr);
    moveBy(inner2, -30., 20.);
    const std::string inner2Id = inner2->getNodeId();
    std::string removedIds[2];
    const char* removedNames[2] = { "Look.Inner3", "Look.Inner5" };
    for (int i = 0; i < 2; ++i) {
        NodePtr inner = nodeNamed(removedNames[i]);
        ASSERT_NE(inner, nullptr);
        removedIds[i] = inner->getOriginNode();
        ASSERT_FALSE(removedIds[i].empty());
        inner->destroyNode(true, false);
    }

    const ProjectMerge::ChangeSummary summary = compareWithBase();
    for (const ProjectMerge::NodeChange& change : summary.nodes) {
        EXPECT_EQ(change.moved, change.nodeId == inner2Id) << change.scriptName;
        EXPECT_FALSE(change.appearance) << change.scriptName;
    }
    const ProjectMerge::NodeChange* wired = summary.find(removedIds[0]);
    ASSERT_NE(wired, nullptr);
    EXPECT_EQ(wired->kind, ProjectMerge::NodeChange::eRemoved);
    EXPECT_DOUBLE_EQ(wired->placedX, 300. - 80.);
    EXPECT_DOUBLE_EQ(wired->placedY, 40.);
    const ProjectMerge::NodeChange* unwired = summary.find(removedIds[1]);
    ASSERT_NE(unwired, nullptr);
    EXPECT_EQ(unwired->kind, ProjectMerge::NodeChange::eRemoved);
    EXPECT_DOUBLE_EQ(unwired->placedX, 500. - 50.);
    EXPECT_DOUBLE_EQ(unwired->placedY, 20.);
}

TEST_F(ProjectMergeTest, ARemovedNodeFollowsTheNearestMatchedNodeThroughRemovedNeighbours)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    for (int i = 1; i <= 2; ++i) {
        NodePtr other = createBlur("Other" + std::to_string(i));
        ASSERT_NE(other, nullptr);
        other->setPosition(300., 100. * i);
    }
    QString v1;
    QString v2;
    saveChainTemplate(dir, [] { }, &v1, &v2);
    QString error;
    ASSERT_EQ(project()->importTemplate(v1, NodeCollectionPtr(), &error).size(), 5u) << error.toStdString();
    ASSERT_EQ(project()->getTemplateImports().size(), 1u);
    moveBy(nodeNamed("Blur1"), 650., 300.);
    moveBy(nodeNamed("Other1"), 500., 300.);
    moveBy(nodeNamed("Other2"), 500., 300.);
    std::string removedIds[2];
    for (int i = 0; i < 2; ++i) {
        NodePtr blur = nodeNamed("Blur" + std::to_string(i + 2));
        ASSERT_NE(blur, nullptr);
        removedIds[i] = blur->getOriginNode();
        ASSERT_FALSE(removedIds[i].empty());
        blur->destroyNode(true, false);
    }

    const ProjectMerge::ChangeSummary summary = compareWithBase();
    for (int i = 0; i < 2; ++i) {
        const ProjectMerge::NodeChange* removed = summary.find(removedIds[i]);
        ASSERT_NE(removed, nullptr);
        EXPECT_EQ(removed->kind, ProjectMerge::NodeChange::eRemoved);
        EXPECT_DOUBLE_EQ(removed->placedX, 650.);
        EXPECT_DOUBLE_EQ(removed->placedY, 100. * (i + 1) + 300.);
    }
}

TEST_F(ProjectMergeTest, NodesRemovedFromAGroupWithNoMatchedNodesKeepTheirBasePosition)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    NodePtr group = createIn(PLUGINID_NATRON_GROUP, project(), "Look");
    ASSERT_NE(group, nullptr);
    NodeGroupPtr collection = std::dynamic_pointer_cast<NodeGroup>(group->getEffectInstance());
    ASSERT_NE(collection, nullptr);
    group->setPosition(0., 0.);
    NodePtr previous;
    for (int i = 1; i <= 2; ++i) {
        NodePtr inner = createIn(PLUGINID_NATRON_BLUR, collection, "Inner" + std::to_string(i));
        ASSERT_NE(inner, nullptr);
        inner->setPosition(100. * i, 50.);
        if (previous) {
            ASSERT_TRUE(NodeCollection::connectNodes(0, previous, inner, false));
        }
        previous = inner;
    }
    previous.reset();
    QString path;
    ASSERT_TRUE(project()->saveProject(dir.path() + QLatin1Char('/'), QString::fromUtf8("tpl.ntpz"), &path));
    project()->reset(false, true);
    QString error;
    ASSERT_FALSE(project()->importTemplate(path, NodeCollectionPtr(), &error).empty()) << error.toStdString();

    moveBy(nodeNamed("Look"), 500., 300.);
    std::string removedIds[2];
    for (int i = 0; i < 2; ++i) {
        NodePtr inner = nodeNamed("Look.Inner" + std::to_string(i + 1));
        ASSERT_NE(inner, nullptr);
        removedIds[i] = inner->getOriginNode();
        ASSERT_FALSE(removedIds[i].empty());
        inner->destroyNode(true, false);
    }

    const ProjectMerge::ChangeSummary summary = compareWithBase();
    for (int i = 0; i < 2; ++i) {
        const ProjectMerge::NodeChange* removed = summary.find(removedIds[i]);
        ASSERT_NE(removed, nullptr);
        EXPECT_EQ(removed->kind, ProjectMerge::NodeChange::eRemoved);
        EXPECT_DOUBLE_EQ(removed->x, 100. * (i + 1));
        EXPECT_DOUBLE_EQ(removed->y, 50.);
        EXPECT_DOUBLE_EQ(removed->placedX, removed->x);
        EXPECT_DOUBLE_EQ(removed->placedY, removed->y);
    }
}

TEST_F(ProjectMergeTest, ANodeAnUpdateAddsSitsBesideItsNeighboursLivePosition)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString v1;
    QString v2;
    const std::function<void()> addBesideBlur3 = [this] {
        NodePtr added = createBlur("Blur4");
        ASSERT_NE(added, nullptr);
        added->setPosition(200., 200.);
        ASSERT_TRUE(NodeCollection::connectNodes(0, nodeNamed("Blur3"), added, false));
    };
    saveChainTemplate(dir, addBesideBlur3, &v1, &v2);
    importChain(v1);
    for (int i = 1; i <= 3; ++i) {
        moveBy(nodeNamed("Blur" + std::to_string(i)), 500., 300.);
    }

    const ProjectMerge::UpdateResult result = ProjectMerge::updateFromTemplate(project(), project()->getTemplateImports().front().key, v2);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    NodePtr added = nodeNamed("Blur4");
    ASSERT_NE(added, nullptr);
    expectAt(added, 700., 500.);
    expectAt(nodeNamed("Blur3"), 500., 500.);
}

TEST_F(ProjectMergeTest, ANodeAnUpdateAddsBesideANodeTheUpdateMovedSitsBesideItsMergedPosition)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString v1;
    QString v2;
    const std::function<void()> moveBlur3AndAddBesideIt = [this] {
        moveBy(nodeNamed("Blur3"), 100., 0.);
        NodePtr added = createBlur("Blur4");
        ASSERT_NE(added, nullptr);
        added->setPosition(300., 200.);
        ASSERT_TRUE(NodeCollection::connectNodes(0, nodeNamed("Blur3"), added, false));
    };
    saveChainTemplate(dir, moveBlur3AndAddBesideIt, &v1, &v2);
    importChain(v1);

    const ProjectMerge::UpdateResult result = ProjectMerge::updateFromTemplate(project(), project()->getTemplateImports().front().key, v2);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    expectAt(nodeNamed("Blur3"), 100., 200.);
    NodePtr added = nodeNamed("Blur4");
    ASSERT_NE(added, nullptr);
    expectAt(added, 300., 200.);
}

TEST_F(ProjectMergeTest, ANodeAnUpdateAddsKeepsTheUpdatesLayoutWhenTheUpdateMovesEveryNode)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString v1;
    QString v2;
    const std::function<void()> relayOutAndAdd = [this] {
        for (int i = 1; i <= 3; ++i) {
            moveBy(nodeNamed("Blur" + std::to_string(i)), 400., 250.);
        }
        NodePtr added = createBlur("Blur4");
        ASSERT_NE(added, nullptr);
        added->setPosition(400., 550.);
        ASSERT_TRUE(NodeCollection::connectNodes(0, nodeNamed("Blur3"), added, false));
    };
    saveChainTemplate(dir, relayOutAndAdd, &v1, &v2);
    importChain(v1);

    const ProjectMerge::UpdateResult result = ProjectMerge::updateFromTemplate(project(), project()->getTemplateImports().front().key, v2);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    for (int i = 1; i <= 3; ++i) {
        expectAt(nodeNamed("Blur" + std::to_string(i)), 400., 250. + 100. * (i - 1));
    }
    NodePtr added = nodeNamed("Blur4");
    ASSERT_NE(added, nullptr);
    expectAt(added, 400., 550.);
}

TEST_F(ProjectMergeTest, ACompareKeepsTheOtherVersionsDeletedNodesWithWhatTheyHold)
{
    NodePtr blur = createBlur("Blur1");
    ASSERT_NE(blur, nullptr);
    NodePtr group = createIn(PLUGINID_NATRON_GROUP, project(), "Look");
    ASSERT_NE(group, nullptr);
    NodeGroupPtr collection = std::dynamic_pointer_cast<NodeGroup>(group->getEffectInstance());
    ASSERT_NE(collection, nullptr);
    NodePtr inner = createIn(PLUGINID_NATRON_BLUR, collection, "Inner1");
    ASSERT_NE(inner, nullptr);
    const std::string blurId = blur->getNodeId();
    const std::string groupId = group->getNodeId();
    const std::string innerId = inner->getNodeId();
    const Serialization::ProjectSerialization before = snapshot();
    blur->destroyNode(true, false);
    group->destroyNode(true, false);

    const ProjectMerge::ChangeSummary summary = ProjectMerge::compare(project(), before);
    ASSERT_TRUE(summary.other != nullptr);
    ASSERT_TRUE(summary.live != nullptr);
    const Serialization::NodeSerialization* deletedBlur = summary.otherNode(blurId);
    ASSERT_TRUE(deletedBlur != nullptr);
    EXPECT_EQ(deletedBlur->scriptName, "Blur1");
    const Serialization::NodeSerialization* deletedGroup = summary.otherNode(groupId);
    ASSERT_TRUE(deletedGroup != nullptr);
    ASSERT_EQ(deletedGroup->children.size(), 1u);
    EXPECT_EQ(deletedGroup->children.front()->scriptName, "Inner1");
    const Serialization::NodeSerialization* deletedInner = summary.otherNode(innerId);
    ASSERT_TRUE(deletedInner != nullptr);
    EXPECT_EQ(deletedInner->scriptName, "Inner1");
    EXPECT_TRUE(summary.otherNode("no-such-node") == nullptr);
}

TEST_F(ProjectMergeTest, RestoringADeletedMiddleNodeSplicesItBackIn)
{
    std::vector<NodePtr> chain = createChain(3);
    ASSERT_EQ(chain.size(), 3u);
    const std::string middleId = chain[1]->getNodeId();
    const Serialization::ProjectSerialization before = snapshot();
    chain[1]->destroyNode(true, false);
    ASSERT_TRUE(chain[2]->replaceInput(chain[0], 0));

    const ProjectMerge::ChangeSummary summary = ProjectMerge::compare(project(), before);
    std::vector<Serialization::GraphOp> ops;
    std::vector<ProjectMerge::PendingSplice> pending;
    QString error;
    ASSERT_TRUE(ProjectMerge::restoreOperations(project(), summary, std::vector<std::string>(1, middleId), ProjectMerge::SpliceMode::eAsk, &ops, &pending, &error)) << error.toStdString();
    EXPECT_TRUE(pending.empty());
    applyAndUndo(ops, [&]() {
        NodePtr restored = project()->findNodeById(middleId);
        ASSERT_NE(restored, nullptr);
        EXPECT_EQ(restored->getScriptName(), "Blur2");
        EXPECT_EQ(restored->getInput(0), chain[0]);
        EXPECT_EQ(chain[2]->getInput(0), restored);
        double x = 0.;
        double y = 0.;
        restored->getPosition(&x, &y);
        EXPECT_DOUBLE_EQ(x, 0.);
        EXPECT_DOUBLE_EQ(y, 100.);
        EXPECT_TRUE(ProjectMerge::compare(project(), before).nodes.empty());
    });
}

TEST_F(ProjectMergeTest, RestoringTwoAdjacentDeletedNodesRestoresTheChain)
{
    std::vector<NodePtr> chain = createChain(4);
    ASSERT_EQ(chain.size(), 4u);
    const std::vector<std::string> ids = { chain[1]->getNodeId(), chain[2]->getNodeId() };
    const Serialization::ProjectSerialization before = snapshot();
    chain[1]->destroyNode(true, false);
    chain[2]->destroyNode(true, false);
    ASSERT_TRUE(chain[3]->replaceInput(chain[0], 0));

    const ProjectMerge::ChangeSummary summary = ProjectMerge::compare(project(), before);
    std::vector<Serialization::GraphOp> ops;
    std::vector<ProjectMerge::PendingSplice> pending;
    QString error;
    ASSERT_TRUE(ProjectMerge::restoreOperations(project(), summary, ids, ProjectMerge::SpliceMode::eAsk, &ops, &pending, &error)) << error.toStdString();
    EXPECT_TRUE(pending.empty());
    applyAndUndo(ops, [&]() {
        NodePtr second = project()->findNodeById(ids[0]);
        NodePtr third = project()->findNodeById(ids[1]);
        ASSERT_NE(second, nullptr);
        ASSERT_NE(third, nullptr);
        EXPECT_EQ(second->getInput(0), chain[0]);
        EXPECT_EQ(third->getInput(0), second);
        EXPECT_EQ(chain[3]->getInput(0), third);
        EXPECT_TRUE(ProjectMerge::compare(project(), before).nodes.empty());
    });
}

TEST_F(ProjectMergeTest, RestoringANodeWhoseReaderWasRewiredFollowsTheSpliceMode)
{
    std::vector<NodePtr> chain = createChain(3);
    ASSERT_EQ(chain.size(), 3u);
    NodePtr other = createBlur("Other");
    ASSERT_NE(other, nullptr);
    const std::string middleId = chain[1]->getNodeId();
    const Serialization::ProjectSerialization before = snapshot();
    chain[1]->destroyNode(true, false);
    ASSERT_TRUE(chain[2]->replaceInput(other, 0));
    const ProjectMerge::ChangeSummary summary = ProjectMerge::compare(project(), before);
    const std::vector<std::string> ids(1, middleId);

    std::vector<Serialization::GraphOp> ops;
    std::vector<ProjectMerge::PendingSplice> pending;
    QString error;
    ASSERT_TRUE(ProjectMerge::restoreOperations(project(), summary, ids, ProjectMerge::SpliceMode::eAsk, &ops, &pending, &error)) << error.toStdString();
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_EQ(pending[0].readerId, chain[2]->getNodeId());
    EXPECT_EQ(pending[0].restoredId, middleId);
    EXPECT_EQ(pending[0].sourceId, other->getNodeId());
    EXPECT_FALSE(pending[0].input.empty());
    applyAndUndo(ops, [&]() {
        NodePtr restored = project()->findNodeById(middleId);
        ASSERT_NE(restored, nullptr);
        EXPECT_EQ(restored->getInput(0), chain[0]);
        EXPECT_EQ(chain[2]->getInput(0), other);
    });

    ASSERT_TRUE(ProjectMerge::restoreOperations(project(), summary, ids, ProjectMerge::SpliceMode::eKeepMine, &ops, &pending, &error)) << error.toStdString();
    EXPECT_TRUE(pending.empty());
    applyAndUndo(ops, [&]() {
        ASSERT_NE(project()->findNodeById(middleId), nullptr);
        EXPECT_EQ(chain[2]->getInput(0), other);
    });

    ASSERT_TRUE(ProjectMerge::restoreOperations(project(), summary, ids, ProjectMerge::SpliceMode::eEverywhere, &ops, &pending, &error)) << error.toStdString();
    EXPECT_TRUE(pending.empty());
    applyAndUndo(ops, [&]() {
        NodePtr restored = project()->findNodeById(middleId);
        ASSERT_NE(restored, nullptr);
        EXPECT_EQ(chain[2]->getInput(0), restored);
    });
}

TEST_F(ProjectMergeTest, ARestoredNodeWhoseNameIsTakenGetsAFreshName)
{
    std::vector<NodePtr> chain = createChain(2);
    ASSERT_EQ(chain.size(), 2u);
    const std::string deletedId = chain[1]->getNodeId();
    const Serialization::ProjectSerialization before = snapshot();
    chain[1]->destroyNode(true, false);
    NodePtr newcomer = createBlur("Blur2");
    ASSERT_NE(newcomer, nullptr);
    ASSERT_EQ(newcomer->getScriptName(), "Blur2");

    const ProjectMerge::ChangeSummary summary = ProjectMerge::compare(project(), before);
    std::vector<Serialization::GraphOp> ops;
    std::vector<ProjectMerge::PendingSplice> pending;
    QString error;
    ASSERT_TRUE(ProjectMerge::restoreOperations(project(), summary, std::vector<std::string>(1, deletedId), ProjectMerge::SpliceMode::eAsk, &ops, &pending, &error)) << error.toStdString();
    applyAndUndo(ops, [&]() {
        NodePtr restored = project()->findNodeById(deletedId);
        ASSERT_NE(restored, nullptr);
        EXPECT_EQ(restored->getScriptName(), "Blur3");
        EXPECT_EQ(restored->getInput(0), chain[0]);
        EXPECT_EQ(newcomer->getScriptName(), "Blur2");
    });
}

TEST_F(ProjectMergeTest, RestoringADeletedGroupBringsBackWhatItHeld)
{
    NodePtr group = createIn(PLUGINID_NATRON_GROUP, project(), "Look");
    ASSERT_NE(group, nullptr);
    NodeGroupPtr collection = std::dynamic_pointer_cast<NodeGroup>(group->getEffectInstance());
    ASSERT_NE(collection, nullptr);
    NodePtr inner = createIn(PLUGINID_NATRON_BLUR, collection, "Inner1");
    ASSERT_NE(inner, nullptr);
    const std::string groupId = group->getNodeId();
    const std::string innerId = inner->getNodeId();
    const Serialization::ProjectSerialization before = snapshot();
    group->destroyNode(true, false);

    const ProjectMerge::ChangeSummary summary = ProjectMerge::compare(project(), before);
    std::vector<Serialization::GraphOp> ops;
    std::vector<ProjectMerge::PendingSplice> pending;
    QString error;
    ASSERT_TRUE(ProjectMerge::restoreOperations(project(), summary, std::vector<std::string>(1, groupId), ProjectMerge::SpliceMode::eAsk, &ops, &pending, &error)) << error.toStdString();
    applyAndUndo(ops, [&]() {
        ASSERT_NE(project()->findNodeById(groupId), nullptr);
        ASSERT_NE(project()->findNodeById(innerId), nullptr);
        EXPECT_NE(nodeNamed("Look.Inner1"), nullptr);
        EXPECT_TRUE(ProjectMerge::compare(project(), before).nodes.empty());
    });
}

TEST_F(ProjectMergeTest, RevertingAChangedNodeGivesItTheOtherVersionsKnobsAndInputs)
{
    std::vector<NodePtr> chain = createChain(2);
    ASSERT_EQ(chain.size(), 2u);
    const Serialization::ProjectSerialization before = snapshot();
    setSize(chain[1], 5.);
    chain[1]->disconnectInput(0);

    const ProjectMerge::ChangeSummary summary = ProjectMerge::compare(project(), before);
    const std::string id = chain[1]->getNodeId();
    ASSERT_NE(summary.find(id), nullptr);
    applyAndUndo(ProjectMerge::revertOperations(project(), summary, std::vector<std::string>(1, id)), [&]() {
        EXPECT_DOUBLE_EQ(blurSize(chain[1])->getValue(0), blurSize(chain[0])->getValue(0));
        EXPECT_EQ(chain[1]->getInput(0), chain[0]);
        EXPECT_EQ(ProjectMerge::compare(project(), before).find(id), nullptr);
    });
}

TEST_F(ProjectMergeTest, TakingOneKnobLeavesTheNodesOtherChanges)
{
    std::vector<NodePtr> chain = createChain(2);
    ASSERT_EQ(chain.size(), 2u);
    const Serialization::ProjectSerialization before = snapshot();
    setSize(chain[1], 5.);
    chain[1]->disconnectInput(0);

    const ProjectMerge::ChangeSummary summary = ProjectMerge::compare(project(), before);
    const std::string id = chain[1]->getNodeId();
    std::vector<Serialization::GraphOp> ops;
    EXPECT_FALSE(ProjectMerge::takeKnobOperations(project(), summary, id, "noSuchKnob", &ops));
    ASSERT_TRUE(ProjectMerge::takeKnobOperations(project(), summary, id, kBlurParamSize, &ops));
    applyAndUndo(ops, [&]() {
        EXPECT_DOUBLE_EQ(blurSize(chain[1])->getValue(0), blurSize(chain[0])->getValue(0));
        EXPECT_EQ(chain[1]->getInput(0), nullptr);
        const ProjectMerge::NodeChange* change = ProjectMerge::compare(project(), before).find(id);
        ASSERT_NE(change, nullptr);
        EXPECT_TRUE(change->knobs.empty());
        EXPECT_EQ(change->inputs.size(), 1u);
    });
}

TEST_F(ProjectMergeTest, RemovingAnAddedNodeReconnectsItsReaders)
{
    std::vector<NodePtr> chain = createChain(2);
    ASSERT_EQ(chain.size(), 2u);
    const Serialization::ProjectSerialization before = snapshot();
    NodePtr extra = createBlur("Extra");
    ASSERT_NE(extra, nullptr);
    ASSERT_TRUE(NodeCollection::connectNodes(0, chain[0], extra, false));
    ASSERT_TRUE(chain[1]->replaceInput(extra, 0));

    const ProjectMerge::ChangeSummary summary = ProjectMerge::compare(project(), before);
    const std::string id = extra->getNodeId();
    extra.reset();
    applyAndUndo(ProjectMerge::removeAddedOperations(project(), summary, std::vector<std::string>(1, id)), [&]() {
        EXPECT_EQ(project()->findNodeById(id), nullptr);
        EXPECT_EQ(chain[1]->getInput(0), chain[0]);
        EXPECT_TRUE(ProjectMerge::compare(project(), before).nodes.empty());
    });
}

TEST_F(ProjectMergeTest, RestoringAPositionPutsAMovedNodeBack)
{
    std::vector<NodePtr> chain = createChain(3);
    ASSERT_EQ(chain.size(), 3u);
    const Serialization::ProjectSerialization before = snapshot();
    chain[1]->setPosition(300., 100.);

    const ProjectMerge::ChangeSummary summary = ProjectMerge::compare(project(), before);
    const std::string id = chain[1]->getNodeId();
    const ProjectMerge::NodeChange* change = summary.find(id);
    ASSERT_NE(change, nullptr);
    ASSERT_TRUE(change->moved);
    applyAndUndo(ProjectMerge::restorePositionOperations(project(), summary, std::vector<std::string>(1, id)), [&]() {
        double x = 0.;
        double y = 0.;
        chain[1]->getPosition(&x, &y);
        EXPECT_DOUBLE_EQ(x, 0.);
        EXPECT_DOUBLE_EQ(y, 100.);
        EXPECT_TRUE(ProjectMerge::compare(project(), before).nodes.empty());
    });
}

TEST_F(ProjectMergeTest, PendingSplicesBecomeSpliceChoicesThatReplaceEarlierOnes)
{
    std::vector<NodePtr> chain = createChain(2);
    ASSERT_EQ(chain.size(), 2u);
    const ProjectMerge::PendingSplice splice { chain[1]->getNodeId(), "Source", "restored-id", chain[0]->getNodeId() };
    ProjectMerge::addSpliceChoices(project(), std::vector<ProjectMerge::PendingSplice>(1, splice));
    ProjectMerge::addSpliceChoices(project(), std::vector<ProjectMerge::PendingSplice>(1, splice));
    const std::vector<Serialization::MergeConflict> conflicts = project()->getMergeConflicts();
    ASSERT_EQ(conflicts.size(), 1u);
    const Serialization::MergeConflict& conflict = conflicts[0];
    EXPECT_EQ(conflict.kind, Serialization::MergeConflictKind::eSpliceChoice);
    EXPECT_EQ(conflict.nodeId, splice.readerId);
    EXPECT_EQ(conflict.input, splice.input);
    ASSERT_EQ(conflict.mine.connections.size(), 1u);
    EXPECT_EQ(conflict.mine.connections[0].sourceId, splice.sourceId);
    ASSERT_EQ(conflict.theirs.connections.size(), 1u);
    EXPECT_EQ(conflict.theirs.connections[0].sourceId, splice.restoredId);
    project()->setMergeConflicts(std::vector<Serialization::MergeConflict>());
}
