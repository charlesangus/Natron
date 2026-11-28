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
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/GraphOpApply.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Project.h"
#include "Engine/ProjectTemplate.h"
#include "Engine/TemplateNode.h"

#include "Serialization/Bundle/ProjectBundle.h"
#include "Serialization/Merge/ThreeWayMerge.h"
#include "Serialization/NodeSerialization.h"
#include "Serialization/Ops/SnapshotDiff.h"
#include "Serialization/ProjectSerialization.h"
#include "Serialization/TemplateNodeSerialization.h"

NATRON_NAMESPACE_USING

class TemplatePublishTest
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

    Serialization::ProjectSerialization state() const
    {
        Serialization::ProjectSerialization live;
        project()->toSerialization(&live);

        return Serialization::editableState(live);
    }

    Serialization::TemplateImportSerialization entry(const std::string& key) const
    {
        for (const Serialization::TemplateImportSerialization& candidate : project()->getTemplateImports()) {
            if (candidate.key == key) {
                return candidate;
            }
        }

        return Serialization::TemplateImportSerialization();
    }

    // The IDs of the live nodes a merge of key would touch.
    std::set<std::string> scope(const std::string& key) const
    {
        const Serialization::TemplateImportSerialization found = entry(key);
        if (!found.base) {
            return std::set<std::string>();
        }
        Serialization::ProjectSerialization mine;
        project()->toSerialization(&mine);

        return Serialization::planMerge(*found.base, mine, *found.base, key).scope;
    }

    // Defines Blur1 -> Blur2 as the template tpl published to dir.
    std::string defineInto(const QTemporaryDir& dir)
    {
        NodePtr first = createBlur("Blur1");
        NodePtr second = createBlur("Blur2");
        EXPECT_TRUE(first != nullptr);
        EXPECT_TRUE(second != nullptr);
        if (!first || !second) {
            return std::string();
        }
        EXPECT_TRUE(NodeCollection::connectNodes(0, first, second, false));
        TemplateEdit edit;
        EXPECT_TRUE(project()->defineTemplate({ first, second }, "tpl", NodeCollectionPtr(), &edit)) << edit.error.toStdString();
        Serialization::TemplateImportSerialization defined = entry(edit.key);
        defined.publishDir = dir.path().toStdString();
        project()->setTemplateImport(defined);

        return edit.key;
    }

    static Serialization::ProjectSerialization readFile(const QString& path)
    {
        Serialization::ProjectBundleReader bundle(path.toStdString());

        return bundle.readProject();
    }

    static int templateNodesIn(const Serialization::ProjectSerialization& file)
    {
        int count = 0;
        for (const Serialization::NodeSerializationPtr& node : file.nodes.nodes) {
            if (node->pluginID == PLUGINID_NATRON_TEMPLATE) {
                ++count;
            }
        }

        return count;
    }

    int templateNodeCount() const
    {
        int count = 0;
        for (const NodePtr& node : project()->getNodes()) {
            if (node->getPluginID() == PLUGINID_NATRON_TEMPLATE) {
                ++count;
            }
        }

        return count;
    }
};

TEST_F(TemplatePublishTest, PublishingWritesNumberedVersionsWithTheTemplateIdentity)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string key = defineInto(dir);
    ASSERT_FALSE(key.empty());
    NodePtr first = project()->getNodeByFullySpecifiedName("Blur1");
    ASSERT_NE(first, nullptr);
    NodePtr artist = createBlur("Artist");
    ASSERT_NE(artist, nullptr);
    const std::string templateId = entry(key).templateId;

    QString error;
    TemplateEdit edit;
    ASSERT_TRUE(project()->publishTemplate(key, &error, &edit)) << error.toStdString();
    EXPECT_EQ(QFileInfo(edit.path).fileName(), QString::fromUtf8("tpl_v001.ntpz"));
    const Serialization::TemplateImportSerialization published = entry(key);
    EXPECT_EQ(published.version, 1);
    EXPECT_EQ(QString::fromStdString(published.path), edit.path);
    EXPECT_FALSE(published.contentHash.empty());
    ASSERT_NE(published.base, nullptr);
    EXPECT_EQ(templateNodesIn(*published.base), 0);
    EXPECT_EQ(published.base->nodes.nodes.size(), 2u);

    const Serialization::ProjectSerialization file = readFile(edit.path);
    EXPECT_TRUE(file.templates.empty());
    EXPECT_EQ(file.nodes.nodes.size(), 3u);
    ASSERT_EQ(templateNodesIn(file), 1);
    const Serialization::NodeSerialization* templateNode = Serialization::findTemplateNode(file.nodes.nodes);
    ASSERT_TRUE(templateNode != nullptr);
    const Serialization::TemplateNodeState state = Serialization::readTemplateNode(*templateNode);
    EXPECT_EQ(state.templateId, templateId);
    EXPECT_EQ(state.version, 1);
    EXPECT_EQ(state.name, "tpl");
    bool hasFirst = false;
    for (const Serialization::NodeSerializationPtr& node : file.nodes.nodes) {
        EXPECT_NE(node->scriptName, "Artist");
        EXPECT_TRUE(node->origin.templateKey.empty());
        hasFirst = hasFirst || (node->id == first->getNodeId());
    }
    EXPECT_TRUE(hasFirst);

    KnobDoublePtr size = std::dynamic_pointer_cast<KnobDouble>(first->getKnobByName(kBlurParamSize));
    ASSERT_NE(size, nullptr);
    size->setValue(4., ViewSpec::all(), 0);
    ASSERT_TRUE(project()->publishTemplate(key, &error, &edit)) << error.toStdString();
    EXPECT_EQ(QFileInfo(edit.path).fileName(), QString::fromUtf8("tpl_v002.ntpz"));
    EXPECT_EQ(entry(key).version, 2);
    EXPECT_EQ(Serialization::readTemplateNode(*Serialization::findTemplateNode(readFile(edit.path).nodes.nodes)).templateId, templateId);

    ASSERT_TRUE(project()->loadProject(dir.path() + QLatin1Char('/'), QString::fromUtf8("tpl_v002.ntpz")));
    EXPECT_NE(project()->getNodeByFullySpecifiedName("Blur1"), nullptr);
    EXPECT_EQ(templateNodeCount(), 1);
}

TEST_F(TemplatePublishTest, APublishIsRefusedWhenTheNextVersionExistsOrTheFolderIsMissing)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string key = defineInto(dir);
    ASSERT_FALSE(key.empty());
    QFile taken(dir.path() + QString::fromUtf8("/tpl_v001.ntpz"));
    ASSERT_TRUE(taken.open(QIODevice::WriteOnly));
    taken.write("taken");
    taken.close();

    QString error;
    EXPECT_FALSE(project()->publishTemplate(key, &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(entry(key).version, 0);
    EXPECT_EQ(QFileInfo(taken.fileName()).size(), 5);

    Serialization::TemplateImportSerialization moved = entry(key);
    moved.publishDir = (dir.path() + QString::fromUtf8("/missing")).toStdString();
    project()->setTemplateImport(moved);
    error.clear();
    EXPECT_FALSE(project()->publishTemplate(key, &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_FALSE(project()->publishTemplate("no_such_template", &error));
}

TEST_F(TemplatePublishTest, PublishingFromAnImportKeepsTheTemplateIds)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string definedKey = defineInto(dir);
    ASSERT_FALSE(definedKey.empty());
    QString error;
    TemplateEdit first;
    ASSERT_TRUE(project()->publishTemplate(definedKey, &error, &first)) << error.toStdString();
    const std::string templateId = entry(definedKey).templateId;
    std::set<std::string> templateIds;
    for (const Serialization::NodeSerializationPtr& node : readFile(first.path).nodes.nodes) {
        templateIds.insert(node->id);
    }
    project()->reset(false, true);

    ASSERT_NE(createBlur("Artist"), nullptr);
    const NodesList imported = project()->importTemplate(first.path, NodeCollectionPtr(), &error);
    ASSERT_EQ(imported.size(), 2u) << error.toStdString();
    ASSERT_EQ(project()->getTemplateImports().size(), 1u);
    const std::string key = project()->getTemplateImports().front().key;
    EXPECT_EQ(entry(key).templateId, templateId);
    EXPECT_EQ(entry(key).version, 1);
    for (const NodePtr& node : imported) {
        EXPECT_EQ(templateIds.count(node->getNodeId()), 0u);
    }

    TemplateEdit second;
    ASSERT_TRUE(project()->publishTemplate(key, &error, &second)) << error.toStdString();
    EXPECT_EQ(QFileInfo(second.path).fileName(), QString::fromUtf8("tpl_v002.ntpz"));
    for (const Serialization::NodeSerializationPtr& node : readFile(second.path).nodes.nodes) {
        if (node->pluginID == PLUGINID_NATRON_TEMPLATE) {
            continue;
        }
        EXPECT_EQ(templateIds.count(node->id), 1u) << node->scriptName;
        EXPECT_NE(node->scriptName, "Artist");
    }
    EXPECT_EQ(Serialization::readTemplateNode(*Serialization::findTemplateNode(readFile(second.path).nodes.nodes)).version, 2);
}
