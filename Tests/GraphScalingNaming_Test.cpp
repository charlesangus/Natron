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
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/Node.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kGradePluginID = "net.sf.openfx.GradePlugin";
const int kNumNodes = 500;

} // namespace

class GraphScalingNaming
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

    NodePtr createDefaultNamedGrade()
    {
        CreateNodeArgs args(kGradePluginID, getApp()->getProject());

        return getApp()->createNode(args);
    }
};

TEST_F(GraphScalingNaming, DefaultNamesAreSequentialAndReused)
{
    std::vector<NodePtr> nodes;

    nodes.reserve(kNumNodes);
    for (int i = 1; i <= kNumNodes; ++i) {
        NodePtr node = createDefaultNamedGrade();
        ASSERT_TRUE(bool(node));
        nodes.push_back(node);
        EXPECT_EQ("Grade" + std::to_string(i), node->getScriptName_mt_safe());
    }

    // Non-blocking destruction defers deactivation to the event loop while the
    // node's preview thread is not flagged as quit, so the name would stay taken.
    nodes[6]->destroyNode(true, false);
    nodes[2]->destroyNode(true, false);
    EXPECT_FALSE(nodes[6]->isActivated());
    EXPECT_FALSE(nodes[2]->isActivated());

    NodePtr reusedLow = createDefaultNamedGrade();
    ASSERT_TRUE(bool(reusedLow));
    EXPECT_EQ(std::string("Grade3"), reusedLow->getScriptName_mt_safe());

    NodePtr reusedHigh = createDefaultNamedGrade();
    ASSERT_TRUE(bool(reusedHigh));
    EXPECT_EQ(std::string("Grade7"), reusedHigh->getScriptName_mt_safe());

    NodePtr next = createDefaultNamedGrade();
    ASSERT_TRUE(bool(next));
    EXPECT_EQ("Grade" + std::to_string(kNumNodes + 1), next->getScriptName_mt_safe());
}
