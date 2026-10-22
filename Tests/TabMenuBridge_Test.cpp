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

#include <string>

#include <gtest/gtest.h>

#include <QAction>
#include <QThreadPool>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Project.h"

#include "Gui/TabMenuBridge.h"

NATRON_NAMESPACE_USING
NATRON_PYTHON_NAMESPACE_USING

namespace {
class NodeCreationContextTest
    : public ::testing::Test {
protected:
    virtual void SetUp() OVERRIDE
    {
        _app = appPTR->getTopLevelInstance();
        ASSERT_TRUE(bool(_app));
        _app->getProject()->reset(false, true);

        CreateNodeArgs args(PLUGINID_NATRON_GROUP, _app->getProject());
        args.setProperty<bool>(kCreateNodeArgsPropNodeGroupDisableCreateInitialNodes, true);
        NodePtr groupNode = _app->createNode(args);
        ASSERT_TRUE(bool(groupNode));
        _group = std::dynamic_pointer_cast<NodeGroup>(groupNode->getEffectInstance());
        ASSERT_TRUE(bool(_group));
    }

    virtual void TearDown() OVERRIDE
    {
        if (_app) {
            _app->clearNodeCreationContext();
            _group.reset();
            _app->getProject()->reset(false, true);
        }
        QThreadPool::globalInstance()->waitForDone();
    }

    NodePtr createDot(const NodeCollectionPtr& collection)
    {
        CreateNodeArgs args(PLUGINID_NATRON_DOT, collection);

        return _app->createNode(args);
    }

    NodeCollectionPtr group() const
    {
        return _group;
    }

    NodeCollectionPtr root() const
    {
        return _app->getProject();
    }

    AppInstancePtr _app;
    NodeGroupPtr _group;
};
} // anonymous namespace

TEST_F(NodeCreationContextTest, NodesWithNoCollectionGoIntoTheContextCollection)
{
    _app->setNodeCreationContext(group(), 10, 20);

    NodePtr first = createDot(NodeCollectionPtr());
    NodePtr second = createDot(NodeCollectionPtr());

    ASSERT_TRUE(first && second);
    EXPECT_EQ(group(), first->getGroup());
    EXPECT_EQ(group(), second->getGroup());
}

TEST_F(NodeCreationContextTest, AnExplicitCollectionIsKept)
{
    _app->setNodeCreationContext(group(), 10, 20);

    NodePtr dot = createDot(root());

    ASSERT_TRUE(bool(dot));
    EXPECT_EQ(root(), dot->getGroup());
}

TEST_F(NodeCreationContextTest, ClearingTheContextRestoresTheProjectRoot)
{
    _app->setNodeCreationContext(group(), 10, 20);
    EXPECT_EQ(group(), _app->getNodeCreationContextCollection());
    _app->clearNodeCreationContext();
    EXPECT_FALSE(_app->getNodeCreationContextCollection());

    NodePtr dot = createDot(NodeCollectionPtr());

    ASSERT_TRUE(bool(dot));
    EXPECT_EQ(root(), dot->getGroup());
}

TEST_F(NodeCreationContextTest, PythonCreateNodeWithNoGroupUsesTheContext)
{
    _app->setNodeCreationContext(group(), 10, 20);

    const std::string appVar = _app->getAppIDString();
    const std::string dot = std::string("\"") + PLUGINID_NATRON_DOT + "\"";
    const std::string script = "_tabMenuDot = " + appVar + ".createNode(" + dot + ")\n"
        + "_tabMenuRootDot = " + appVar + ".createNode(" + dot + ", -1, " + appVar + ")\n"
        + "del _tabMenuDot, _tabMenuRootDot\n";
    std::string error, output;
    ASSERT_TRUE(interpretPythonScript(script, &error, &output)) << error;

    EXPECT_EQ(1u, group()->getNodes().size());
    std::size_t rootDots = 0;
    NodesList rootNodes = root()->getNodes();
    for (NodesList::const_iterator it = rootNodes.begin(); it != rootNodes.end(); ++it) {
        if ((*it)->getPluginID() == PLUGINID_NATRON_DOT) {
            ++rootDots;
        }
    }
    EXPECT_EQ(1u, rootDots);
}

TEST(TabMenuBridge, TriggerWithoutAnOpenMenuTriggersTheActionAsIs)
{
    AppInstancePtr app = appPTR->getTopLevelInstance();
    ASSERT_TRUE(bool(app));
    QAction action(0);
    int triggered = 0;
    bool contextSetWhileTriggered = true;
    QObject::connect(&action, &QAction::triggered, [&]() {
        ++triggered;
        contextSetWhileTriggered = (bool)app->getNodeCreationContextCollection();
    });

    TabMenuBridge::triggerAction(&action);
    TabMenuBridge::triggerAction(0);

    EXPECT_EQ(1, triggered);
    EXPECT_FALSE(contextSetWhileTriggered);
    EXPECT_FALSE(app->getNodeCreationContextCollection());
}
