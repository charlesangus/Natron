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
