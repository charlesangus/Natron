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
#include <ostream>
#include <string>

#include <gtest/gtest.h>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Loop/GeoForEachBegin.h"
#include "Engine/Nodes/Loop/GeoForEachEnd.h"
#include "Engine/Nodes/Loop/GeoSolverBegin.h"
#include "Engine/Nodes/Loop/GeoSolverEnd.h"
#include "Engine/Nodes/Loop/LoopBegin.h"
#include "Engine/Nodes/Loop/LoopEnd.h"
#include "Engine/Nodes/Loop/LoopPair.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_USING

namespace {

struct ExpectedPair {
    std::string name;
    std::string beginPluginID;
    std::string endPluginID;
};

void
PrintTo(const ExpectedPair& pair, std::ostream* os)
{
    *os << pair.name;
}

bool
endIsPairedTo(const ExpectedPair& pair, const NodePtr& begin, const NodePtr& end)
{
    const EffectInstancePtr effect = end->getEffectInstance();
    if (pair.name == "GeoForEach") {
        return GeoForEachEnd::pairedEndOf(begin).get() == effect.get();
    }
    if (pair.name == "GeoSolver") {
        return GeoSolverEnd::pairedEndOf(begin).get() == effect.get();
    }

    return LoopEnd::pairedEndOf(begin).get() == effect.get();
}

class LoopPairTest
    : public BaseTest,
      public ::testing::WithParamInterface<ExpectedPair> {
protected:
    virtual void TearDown() OVERRIDE
    {
        for (auto it = _nodes.rbegin(); it != _nodes.rend(); ++it) {
            appPTR->removeAllCacheEntriesForHolder(it->get(), true);
            (*it)->destroyNode(false, false);
        }
        _nodes.clear();
        BaseTest::TearDown();
    }

    LoopPairNodes create(const LoopPairKind& kind)
    {
        NodeCollectionPtr project = getApp()->getProject();
        LoopPairNodes nodes = createLoopPair(getApp(), kind, [&project](CreateNodeArgs& args) {
            args.setProperty<NodeCollectionPtr>(kCreateNodeArgsPropGroupContainer, project);
            args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
            args.setProperty<bool>(kCreateNodeArgsPropSettingsOpened, false);
            args.setProperty<bool>(kCreateNodeArgsPropSilent, true);
        });
        if (nodes.end) {
            _nodes.push_back(nodes.end);
        }
        if (nodes.begin) {
            _nodes.push_back(nodes.begin);
        }

        return nodes;
    }

    NodesList _nodes;
};

TEST_P(LoopPairTest, TheKindIsListedWithItsBeginAndEnd)
{
    const LoopPairKind* kind = findLoopPairKind(GetParam().name);

    ASSERT_TRUE(kind != nullptr);
    EXPECT_EQ(GetParam().beginPluginID, std::string(kind->beginPluginID));
    EXPECT_EQ(GetParam().endPluginID, std::string(kind->endPluginID));
}

TEST_P(LoopPairTest, CreatesAPairedBeginAndEnd)
{
    const LoopPairKind* kind = findLoopPairKind(GetParam().name);
    ASSERT_TRUE(kind != nullptr);

    LoopPairNodes nodes = create(*kind);

    ASSERT_TRUE(nodes.begin != nullptr);
    ASSERT_TRUE(nodes.end != nullptr);
    EXPECT_EQ(GetParam().beginPluginID, nodes.begin->getPluginID());
    EXPECT_EQ(GetParam().endPluginID, nodes.end->getPluginID());
    EXPECT_TRUE(nodes.begin->getGroup() == getApp()->getProject());
    EXPECT_TRUE(nodes.end->getGroup() == getApp()->getProject());
    EXPECT_TRUE(nodes.end->getInput(0) == nodes.begin);
    EXPECT_TRUE(nodes.begin->getInput(0) == nullptr);
    EXPECT_TRUE(endIsPairedTo(GetParam(), nodes.begin, nodes.end));
}

TEST_P(LoopPairTest, TheEndIsPlacedBelowTheBegin)
{
    const LoopPairKind* kind = findLoopPairKind(GetParam().name);
    ASSERT_TRUE(kind != nullptr);

    LoopPairNodes nodes = create(*kind);
    ASSERT_TRUE(nodes.begin != nullptr);
    ASSERT_TRUE(nodes.end != nullptr);

    double beginX, beginY, beginWidth, beginHeight, endX, endY;
    nodes.begin->getPosition(&beginX, &beginY);
    nodes.begin->getSize(&beginWidth, &beginHeight);
    nodes.end->getPosition(&endX, &endY);
    EXPECT_GT(endY, beginY + beginHeight);
}

TEST_P(LoopPairTest, TwoPairsAreEachPairedToTheirOwnBegin)
{
    const LoopPairKind* kind = findLoopPairKind(GetParam().name);
    ASSERT_TRUE(kind != nullptr);

    LoopPairNodes first = create(*kind);
    LoopPairNodes second = create(*kind);
    ASSERT_TRUE(first.end != nullptr);
    ASSERT_TRUE(second.end != nullptr);

    EXPECT_TRUE(second.end->getInput(0) == second.begin);
    EXPECT_TRUE(endIsPairedTo(GetParam(), first.begin, first.end));
    EXPECT_TRUE(endIsPairedTo(GetParam(), second.begin, second.end));
    EXPECT_NE(first.begin->getScriptName(), second.begin->getScriptName());
}

INSTANTIATE_TEST_CASE_P(Kinds, LoopPairTest, ::testing::Values(ExpectedPair { "GeoForEach", PLUGINID_NATRON_GEOFOREACHBEGIN, PLUGINID_NATRON_GEOFOREACHEND }, ExpectedPair { "GeoSolver", PLUGINID_NATRON_GEOSOLVERBEGIN, PLUGINID_NATRON_GEOSOLVEREND }, ExpectedPair { "Loop", PLUGINID_NATRON_LOOPBEGIN, PLUGINID_NATRON_LOOPEND }));

TEST(LoopPairKinds, ListsEachKindOnce)
{
    EXPECT_EQ(3u, loopPairKinds().size());
    for (const LoopPairKind& kind : loopPairKinds()) {
        EXPECT_TRUE(findLoopPairKind(kind.name) == &kind) << kind.name;
    }
}

TEST(LoopPairKinds, AnUnknownNameIsNoKind)
{
    EXPECT_TRUE(findLoopPairKind("GeoForEachBegin") == nullptr);
    EXPECT_TRUE(findLoopPairKind("") == nullptr);
}

} // namespace
