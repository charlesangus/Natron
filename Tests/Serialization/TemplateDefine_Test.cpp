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

#include <QString>

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

#include "Serialization/Merge/ThreeWayMerge.h"
#include "Serialization/NodeSerialization.h"
#include "Serialization/Ops/SnapshotDiff.h"
#include "Serialization/ProjectSerialization.h"
#include "Serialization/TemplateNodeSerialization.h"

NATRON_NAMESPACE_USING

class TemplateDefineTest
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

TEST_F(TemplateDefineTest, ASelectionBecomesATemplateWithItsOwnNode)
{
    NodePtr first = createBlur("Blur1");
    NodePtr second = createBlur("Blur2");
    NodePtr artist = createBlur("Artist");
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(artist, nullptr);

    TemplateEdit edit;
    ASSERT_TRUE(project()->defineTemplate({ first, second }, "comp tpl", NodeCollectionPtr(), &edit)) << edit.error.toStdString();
    ASSERT_FALSE(edit.key.empty());
    ASSERT_NE(edit.templateNode, nullptr);
    EXPECT_EQ(edit.templateNode->getPluginID(), PLUGINID_NATRON_TEMPLATE);
    EXPECT_EQ(project()->findTemplateNode(edit.key), edit.templateNode);
    EXPECT_EQ(templateNodeCount(), 1);
    EXPECT_EQ(first->getOriginTemplate(), edit.key);
    EXPECT_EQ(first->getOriginNode(), first->getNodeId());
    EXPECT_EQ(second->getOriginTemplate(), edit.key);
    EXPECT_TRUE(artist->getOriginTemplate().empty());

    const Serialization::TemplateImportSerialization defined = entry(edit.key);
    EXPECT_EQ(defined.name, "comp tpl");
    EXPECT_FALSE(defined.templateId.empty());
    EXPECT_EQ(defined.version, 0);
    ASSERT_NE(defined.base, nullptr);
    EXPECT_EQ(defined.base->nodes.nodes.size(), 2u);
    const std::set<std::string> members = scope(edit.key);
    EXPECT_EQ(members, (std::set<std::string> { first->getNodeId(), second->getNodeId() }));

    TemplateEdit again;
    EXPECT_FALSE(project()->defineTemplate({ first }, "other", NodeCollectionPtr(), &again));
    EXPECT_FALSE(again.error.isEmpty());
    EXPECT_FALSE(project()->defineTemplate({ edit.templateNode }, "other", NodeCollectionPtr(), &again));
    EXPECT_EQ(project()->getTemplateImports().size(), 1u);
}

TEST_F(TemplateDefineTest, TheTemplateNodeShowsTheImportAndKeepsItsKeyHidden)
{
    NodePtr first = createBlur("Blur1");
    ASSERT_NE(first, nullptr);
    TemplateEdit edit;
    ASSERT_TRUE(project()->defineTemplate({ first }, "tpl", NodeCollectionPtr(), &edit)) << edit.error.toStdString();
    ASSERT_NE(edit.templateNode, nullptr);
    const NodePtr node = edit.templateNode;

    KnobStringPtr key = std::dynamic_pointer_cast<KnobString>(node->getKnobByName(Serialization::kTemplateParamKey));
    KnobStringPtr name = std::dynamic_pointer_cast<KnobString>(node->getKnobByName(Serialization::kTemplateParamName));
    KnobStringPtr id = std::dynamic_pointer_cast<KnobString>(node->getKnobByName(Serialization::kTemplateParamId));
    KnobIntPtr version = std::dynamic_pointer_cast<KnobInt>(node->getKnobByName(Serialization::kTemplateParamVersion));
    KnobIntPtr imported = std::dynamic_pointer_cast<KnobInt>(node->getKnobByName(Serialization::kTemplateParamImportedVersion));
    ASSERT_NE(key, nullptr);
    ASSERT_NE(name, nullptr);
    ASSERT_NE(id, nullptr);
    ASSERT_NE(version, nullptr);
    ASSERT_NE(imported, nullptr);
    for (const char* button : { "publish", "updateToLatest", "updateFromFile", "compareWithBase", "detach" }) {
        EXPECT_NE(node->getKnobByName(button), nullptr) << button;
    }

    EXPECT_EQ(key->getValue(), edit.key);
    EXPECT_TRUE(key->getIsSecret());
    EXPECT_EQ(name->getValue(), "tpl");
    EXPECT_EQ(id->getValue(), entry(edit.key).templateId);
    EXPECT_FALSE(id->isEnabled(0));
    EXPECT_FALSE(version->isEnabled(0));
    EXPECT_EQ(version->getValue(), 0);

    Serialization::TemplateImportSerialization changed = entry(edit.key);
    changed.version = 3;
    changed.name = "renamed";
    project()->setTemplateImport(changed);
    EXPECT_EQ(version->getValue(), 3);
    EXPECT_EQ(imported->getValue(), 3);
    EXPECT_EQ(name->getValue(), "renamed");

    name->setValue(std::string("by hand"));
    EXPECT_EQ(entry(edit.key).name, "by hand");
}

TEST_F(TemplateDefineTest, AWholeGraphDefinitionTakesEveryUntaggedNode)
{
    NodePtr first = createBlur("Blur1");
    NodePtr second = createBlur("Blur2");
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    TemplateEdit edit;
    ASSERT_TRUE(project()->defineTemplate(NodesList(), "whole", NodeCollectionPtr(), &edit)) << edit.error.toStdString();
    EXPECT_EQ(first->getOriginTemplate(), edit.key);
    EXPECT_EQ(second->getOriginTemplate(), edit.key);
    EXPECT_EQ(scope(edit.key).size(), 2u);

    TemplateEdit none;
    EXPECT_FALSE(project()->defineTemplate(NodesList(), "empty", NodeCollectionPtr(), &none));
    EXPECT_EQ(project()->getTemplateImports().size(), 1u);
}

TEST_F(TemplateDefineTest, RemovedNodesLeaveTheScopeAndAddedNodesJoinIt)
{
    NodePtr first = createBlur("Blur1");
    NodePtr second = createBlur("Blur2");
    NodePtr artist = createBlur("Artist");
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(artist, nullptr);
    TemplateEdit edit;
    ASSERT_TRUE(project()->defineTemplate({ first, second }, "tpl", NodeCollectionPtr(), &edit)) << edit.error.toStdString();

    TemplateEdit removal;
    ASSERT_TRUE(project()->removeFromTemplate(edit.key, second, &removal)) << removal.error.toStdString();
    EXPECT_TRUE(second->getOriginTemplate().empty());
    EXPECT_EQ(entry(edit.key).excluded.count(second->getNodeId()), 1u);
    EXPECT_EQ(scope(edit.key), (std::set<std::string> { first->getNodeId() }));

    TemplateEdit addition;
    ASSERT_TRUE(project()->addToTemplate(edit.key, artist, &addition)) << addition.error.toStdString();
    EXPECT_EQ(artist->getOriginTemplate(), edit.key);
    ASSERT_TRUE(project()->addToTemplate(edit.key, second, &addition)) << addition.error.toStdString();
    EXPECT_EQ(entry(edit.key).excluded.count(second->getNodeId()), 0u);
    EXPECT_EQ(second->getOriginTemplate(), edit.key);
    EXPECT_EQ(scope(edit.key).count(second->getNodeId()), 1u);

    TemplateEdit wrong;
    EXPECT_FALSE(project()->addToTemplate("no_such_template", artist, &wrong));
    EXPECT_FALSE(project()->addToTemplate(edit.key, edit.templateNode, &wrong));
}

TEST_F(TemplateDefineTest, DetachUntagsTheMembersDeletesTheTemplateNodeAndUndoes)
{
    NodePtr first = createBlur("Blur1");
    NodePtr second = createBlur("Blur2");
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    TemplateEdit edit;
    ASSERT_TRUE(project()->defineTemplate({ first, second }, "tpl", NodeCollectionPtr(), &edit)) << edit.error.toStdString();
    const std::string key = edit.key;
    const std::string templateNodeId = edit.templateNode->getNodeId();
    edit.templateNode.reset();
    const Serialization::ProjectSerialization before = state();

    TemplateEdit detach;
    ASSERT_TRUE(project()->detachTemplate(key, &detach)) << detach.error.toStdString();
    EXPECT_TRUE(first->getOriginTemplate().empty());
    EXPECT_TRUE(second->getOriginTemplate().empty());
    EXPECT_TRUE(project()->getTemplateImports().empty());
    EXPECT_EQ(project()->findNodeById(templateNodeId), nullptr);
    EXPECT_EQ(templateNodeCount(), 0);

    const Serialization::ProjectSerialization after = state();
    for (const Serialization::OpResult& undone : applyGraphOps(project(), Serialization::diffSnapshots(after, before))) {
        EXPECT_TRUE(undone.applied()) << undone.message;
    }
    EXPECT_EQ(first->getOriginTemplate(), key);
    EXPECT_NE(project()->findNodeById(templateNodeId), nullptr);

    TemplateEdit missing;
    EXPECT_FALSE(project()->detachTemplate("no_such_template", &missing));
}
