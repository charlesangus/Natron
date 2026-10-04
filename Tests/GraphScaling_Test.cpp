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

#include <chrono>
#include <string>

#include <gtest/gtest.h>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_USING

namespace {

NodePtr
createNamedNode(const AppInstancePtr& app,
                const char* pluginID,
                const std::string& name)
{
    CreateNodeArgs args(pluginID, app->getProject());

    args.setProperty<std::string>(kCreateNodeArgsPropNodeInitialName, name);

    return app->createNode(args);
}

} // namespace

class GraphScalingTest
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

TEST_F(GraphScalingTest, DiamondLadderConnectIsFast)
{
    const int kLevels = 40;
    const AppInstancePtr app = getApp();

    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

    NodePtr previous = createNamedNode(app, PLUGINID_OFX_CONSTANT, "Root");
    ASSERT_TRUE(bool(previous));

    for (int level = 1; level <= kLevels; ++level) {
        const std::string suffix = std::to_string(level);
        NodePtr left = createNamedNode(app, PLUGINID_OFX_GRADE, "Left" + suffix);
        NodePtr right = createNamedNode(app, PLUGINID_OFX_GRADE, "Right" + suffix);
        NodePtr join = createNamedNode(app, PLUGINID_OFX_MERGE, "Join" + suffix);
        ASSERT_TRUE(bool(left));
        ASSERT_TRUE(bool(right));
        ASSERT_TRUE(bool(join));

        ASSERT_TRUE(left->connectInput(previous, 0));
        ASSERT_TRUE(right->connectInput(previous, 0));
        ASSERT_TRUE(join->connectInput(left, 0));
        ASSERT_TRUE(join->connectInput(right, 1));
        previous = join;
    }

    NodePtr bottom = createNamedNode(app, PLUGINID_OFX_GRADE, "Bottom");
    ASSERT_TRUE(bool(bottom));
    ASSERT_TRUE(bottom->connectInput(previous, 0));

    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    EXPECT_LT(seconds, 5.);
}

TEST_F(GraphScalingTest, ConnectingIntoOwnUpstreamIsRefused)
{
    const AppInstancePtr app = getApp();

    NodePtr a = createNamedNode(app, PLUGINID_OFX_GRADE, "CycleA");
    NodePtr b = createNamedNode(app, PLUGINID_OFX_GRADE, "CycleB");
    NodePtr c = createNamedNode(app, PLUGINID_OFX_GRADE, "CycleC");
    ASSERT_TRUE(bool(a));
    ASSERT_TRUE(bool(b));
    ASSERT_TRUE(bool(c));

    ASSERT_TRUE(b->connectInput(a, 0));
    ASSERT_TRUE(c->connectInput(b, 0));

    EXPECT_FALSE(a->connectInput(c, 0));
    EXPECT_EQ((Node*)NULL, a->getInput(0).get());
    EXPECT_FALSE(a->isInputConnected(0));
    EXPECT_EQ(a, b->getInput(0));
    EXPECT_EQ(b, c->getInput(0));
}
