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

#include <memory>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Project.h"

#include "Serialization/NodeSerialization.h"
#include "Serialization/ProjectBundleTestHelpers.h"
#include "Serialization/ProjectSerialization.h"
#include "Serialization/Yaml/ClipboardYaml.h"

NATRON_NAMESPACE_USING
NATRON_PYTHON_NAMESPACE_USING

class NodeIdTest
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

    NodePtr createDot(const NodeCollectionPtr& parent)
    {
        CreateNodeArgs args(PLUGINID_NATRON_DOT, parent);
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);

        return getApp()->createNode(args);
    }

    NodePtr createEmptyGroup()
    {
        CreateNodeArgs args(PLUGINID_NATRON_GROUP, getApp()->getProject());
        args.setProperty<bool>(kCreateNodeArgsPropNodeGroupDisableCreateInitialNodes, true);
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);

        return getApp()->createNode(args);
    }

    // What the node graph's paste and duplicate do with a copied node.
    NodePtr pasteCopyOf(const NodePtr& source)
    {
        Serialization::NodeSerializationPtr serialization = std::make_shared<Serialization::NodeSerialization>();
        source->toSerialization(serialization.get());

        return pasteSerialization(serialization, source->getGroup());
    }

    NodePtr pasteSerialization(const Serialization::NodeSerializationPtr& serialization,
                               const NodeCollectionPtr& group)
    {
        CreateNodeArgs args(serialization->pluginID, group);
        args.setProperty<Serialization::NodeSerializationPtr>(kCreateNodeArgsPropNodeSerialization, serialization);
        args.setProperty<int>(kCreateNodeArgsPropPluginVersion, serialization->pluginMajorVersion, 0);
        args.setProperty<int>(kCreateNodeArgsPropPluginVersion, serialization->pluginMinorVersion, 1);
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
        args.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, false);
        args.setProperty<bool>(kCreateNodeArgsPropSettingsOpened, false);
        args.setProperty<bool>(kCreateNodeArgsPropAllowNonUserCreatablePlugins, true);

        return getApp()->createNode(args);
    }

    int loadWarningsMentioning(const std::string& text) const
    {
        int count = 0;
        const QStringList warnings = getApp()->getProject()->getLoadWarnings();
        for (const QString& warning : warnings) {
            if (warning.contains(QString::fromUtf8(text.c_str()))) {
                ++count;
            }
        }

        return count;
    }

    // The first key of the node map written under nodeKey ("Dot1:"), or empty if there is none.
    static std::string firstKeyUnder(const std::string& yaml,
                                     const std::string& nodeKey)
    {
        std::istringstream in(yaml);
        std::string line;
        bool found = false;
        while (std::getline(in, line)) {
            const std::size_t start = line.find_first_not_of(' ');
            if (start == std::string::npos) {
                continue;
            }
            if (found) {
                return line.substr(start, line.find(':', start) - start);
            }
            found = (line.compare(start, std::string::npos, nodeKey) == 0);
        }

        return std::string();
    }
};

TEST_F(NodeIdTest, ANewNodeHasA32HexIdAndTwoNodesDiffer)
{
    NodePtr a = createDot(getApp()->getProject());
    NodePtr b = createDot(getApp()->getProject());
    ASSERT_TRUE(bool(a) && bool(b));

    EXPECT_EQ(32u, a->getNodeId().size()) << a->getNodeId();
    EXPECT_TRUE(Node::isValidNodeId(a->getNodeId())) << a->getNodeId();
    EXPECT_TRUE(Node::isValidNodeId(b->getNodeId())) << b->getNodeId();
    EXPECT_NE(a->getNodeId(), b->getNodeId());

    EXPECT_FALSE(Node::isValidNodeId(""));
    EXPECT_FALSE(Node::isValidNodeId("0123456789ABCDEF0123456789abcdef"));
    EXPECT_FALSE(Node::isValidNodeId("0123456789abcdef0123456789abcde"));
    EXPECT_FALSE(Node::isValidNodeId("{0123456789abcdef0123456789abcd}"));
}

TEST_F(NodeIdTest, SaveAndLoadKeepIdsAndWriteThemFirst)
{
    ProjectPtr project = getApp()->getProject();
    NodePtr top = createDot(project);
    NodePtr group = createEmptyGroup();
    ASSERT_TRUE(bool(top) && bool(group));
    NodeCollectionPtr collection = std::dynamic_pointer_cast<NodeCollection>(group->getEffectInstance());
    ASSERT_TRUE(bool(collection));
    NodePtr child = createDot(collection);
    ASSERT_TRUE(bool(child));

    const std::string topName = top->getFullyQualifiedName();
    const std::string groupName = group->getFullyQualifiedName();
    const std::string childName = child->getFullyQualifiedName();
    const std::string topId = top->getNodeId();
    const std::string groupId = group->getNodeId();
    const std::string childId = child->getNodeId();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    std::string yaml;
    Serialization::ProjectSerialization saved;
    ASSERT_TRUE(saveProjectBundle(project, tmp, QString::fromUtf8("ids.ntpz"), &yaml, &saved));
    EXPECT_EQ(std::string("id"), firstKeyUnder(yaml, top->getScriptName() + ":")) << yaml;
    EXPECT_EQ(std::string("id"), firstKeyUnder(yaml, child->getScriptName() + ":")) << yaml;
    EXPECT_NE(std::string::npos, yaml.find("\"" + childId + "\"")) << yaml;

    top.reset();
    group.reset();
    child.reset();
    collection.reset();
    project->reset(false, true);
    EXPECT_FALSE(bool(project->findNodeById(topId)));

    ASSERT_TRUE(project->loadProject(tmp.path() + QLatin1Char('/'), QString::fromUtf8("ids.ntpz")));
    NodePtr top2 = getApp()->getNodeByFullySpecifiedName(topName);
    NodePtr group2 = getApp()->getNodeByFullySpecifiedName(groupName);
    NodePtr child2 = getApp()->getNodeByFullySpecifiedName(childName);
    ASSERT_TRUE(bool(top2) && bool(group2) && bool(child2));
    EXPECT_EQ(topId, top2->getNodeId());
    EXPECT_EQ(groupId, group2->getNodeId());
    EXPECT_EQ(childId, child2->getNodeId());
    EXPECT_EQ(top2, project->findNodeById(topId));
    EXPECT_EQ(child2, project->findNodeById(childId));
    EXPECT_EQ(0, loadWarningsMentioning(topId));
}

TEST_F(NodeIdTest, PasteAndDuplicateMakeFreshIds)
{
    ProjectPtr project = getApp()->getProject();
    NodePtr source = createDot(project);
    ASSERT_TRUE(bool(source));
    const std::string sourceId = source->getNodeId();

    NodePtr copy = pasteCopyOf(source);
    ASSERT_TRUE(bool(copy));
    EXPECT_TRUE(Node::isValidNodeId(copy->getNodeId()));
    EXPECT_NE(sourceId, copy->getNodeId());
    EXPECT_EQ(source, project->findNodeById(sourceId));
    EXPECT_EQ(copy, project->findNodeById(copy->getNodeId()));

    // A cut and paste: the source is gone by the time the copy is made, and it still gets a new ID.
    Serialization::NodeSerializationPtr cut = std::make_shared<Serialization::NodeSerialization>();
    source->toSerialization(cut.get());
    source->destroyNode(true, false);
    source.reset();
    NodePtr pasted = pasteSerialization(cut, project);
    ASSERT_TRUE(bool(pasted));
    EXPECT_NE(sourceId, pasted->getNodeId());
    EXPECT_FALSE(bool(project->findNodeById(sourceId)));

    // The clipboard leaves IDs out, so a paste in another Natron makes new ones too.
    Serialization::ClipboardSerialization clipboard;
    clipboard.nodes.push_back(cut);
    const std::string text = Serialization::encodeClipboardYaml(clipboard);
    EXPECT_EQ(std::string::npos, text.find(sourceId)) << text;
}

TEST_F(NodeIdTest, DeleteAndUndoKeepTheIdAndLookupFollowsActivation)
{
    ProjectPtr project = getApp()->getProject();
    NodePtr node = createDot(project);
    ASSERT_TRUE(bool(node));
    const std::string id = node->getNodeId();
    EXPECT_EQ(node, project->findNodeById(id));

    // The node graph deletes by deactivating, so that undo can reactivate the same node.
    node->deactivate(NodesList(), true, false, true, false);
    EXPECT_FALSE(bool(project->findNodeById(id)));
    EXPECT_EQ(node, project->findNodeHoldingId(id));

    node->activate(NodesList(), true, false);
    EXPECT_EQ(id, node->getNodeId());
    EXPECT_EQ(node, project->findNodeById(id));

    node->destroyNode(true, false);
    EXPECT_FALSE(bool(project->findNodeById(id)));
    EXPECT_FALSE(bool(project->findNodeHoldingId(id)));
}

TEST_F(NodeIdTest, NodesInsideAGroupAreFoundById)
{
    ProjectPtr project = getApp()->getProject();
    NodePtr group = createEmptyGroup();
    ASSERT_TRUE(bool(group));
    NodeCollectionPtr collection = std::dynamic_pointer_cast<NodeCollection>(group->getEffectInstance());
    ASSERT_TRUE(bool(collection));
    NodePtr child = createDot(collection);
    ASSERT_TRUE(bool(child));
    NodeCollectionPtr inner;
    {
        CreateNodeArgs args(PLUGINID_NATRON_GROUP, collection);
        args.setProperty<bool>(kCreateNodeArgsPropNodeGroupDisableCreateInitialNodes, true);
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
        NodePtr innerGroup = getApp()->createNode(args);
        ASSERT_TRUE(bool(innerGroup));
        inner = std::dynamic_pointer_cast<NodeCollection>(innerGroup->getEffectInstance());
    }
    ASSERT_TRUE(bool(inner));
    NodePtr grandChild = createDot(inner);
    ASSERT_TRUE(bool(grandChild));

    EXPECT_EQ(group, project->findNodeById(group->getNodeId()));
    EXPECT_EQ(child, project->findNodeById(child->getNodeId()));
    EXPECT_EQ(grandChild, project->findNodeById(grandChild->getNodeId()));
    EXPECT_FALSE(bool(project->findNodeById("0123456789abcdef0123456789abcdef")));

    const std::string childId = child->getNodeId();
    child->destroyNode(true, false);
    EXPECT_FALSE(bool(project->findNodeById(childId)));
}

TEST_F(NodeIdTest, ADuplicateIdInAFileIsRemintedWithOneWarning)
{
    ProjectPtr project = getApp()->getProject();
    NodePtr a = createDot(project);
    NodePtr b = createDot(project);
    ASSERT_TRUE(bool(a) && bool(b));
    const std::string aName = a->getScriptName();
    const std::string bName = b->getScriptName();
    const std::string aId = a->getNodeId();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    std::string yaml;
    Serialization::ProjectSerialization saved;
    ASSERT_TRUE(saveProjectBundle(project, tmp, QString::fromUtf8("unique.ntpz"), &yaml, &saved));
    bool edited = false;
    for (const Serialization::NodeSerializationPtr& node : saved.nodes.nodes) {
        if (node->scriptName == bName) {
            node->id = aId;
            edited = true;
        }
    }
    ASSERT_TRUE(edited);

    a.reset();
    b.reset();
    ASSERT_TRUE(loadProjectBundle(project, tmp, QString::fromUtf8("duplicate.ntpz"), saved));
    NodePtr a2 = project->getNodeByName(aName);
    NodePtr b2 = project->getNodeByName(bName);
    ASSERT_TRUE(bool(a2) && bool(b2));

    EXPECT_TRUE((a2->getNodeId() == aId) != (b2->getNodeId() == aId)) << a2->getNodeId() << " " << b2->getNodeId();
    EXPECT_TRUE(Node::isValidNodeId(a2->getNodeId()));
    EXPECT_TRUE(Node::isValidNodeId(b2->getNodeId()));
    EXPECT_NE(a2->getNodeId(), b2->getNodeId());
    EXPECT_EQ(1, loadWarningsMentioning(aId)) << getApp()->getProject()->getLoadWarnings().join(QString::fromUtf8("\n")).toStdString();
}

TEST_F(NodeIdTest, PythonFindsANodeByItsId)
{
    NodePtr group = createEmptyGroup();
    ASSERT_TRUE(bool(group));
    NodeCollectionPtr collection = std::dynamic_pointer_cast<NodeCollection>(group->getEffectInstance());
    ASSERT_TRUE(bool(collection));
    NodePtr child = createDot(collection);
    ASSERT_TRUE(bool(child));

    const std::string app = getApp()->getAppIDString();
    const std::string script = "n = " + app + "." + child->getFullyQualifiedName() + "\n"
                                                                                     "i = n.getNodeId()\n"
                                                                                     "assert i == '"
        + child->getNodeId() + "', i\n"
                               "f = "
        + app + ".getNodeById(i)\n"
                "assert f is not None\n"
                "assert f.getScriptName() == n.getScriptName(), f.getScriptName()\n"
                "assert f.getNodeId() == i\n"
                "assert "
        + app + ".getNodeById('0123456789abcdef0123456789abcdef') is None\n";
    std::string error, output;
    EXPECT_TRUE(interpretPythonScript(script, &error, &output)) << script << "\n"
                                                                << error;
}
