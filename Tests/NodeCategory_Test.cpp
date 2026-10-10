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
#include <string>

#include <QString>

#include "BaseTest.h"

#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Deep/DeepMerge.h"
#include "Engine/Nodes/Deep/DeepRead.h"
#include "Engine/Nodes/Deep/DeepWrite.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/TypedPassthrough.h"

NATRON_NAMESPACE_USING

// A native Color-grouped node must read as the Color category.
TEST_F(BaseTest, NativeColorNodeIsColorCategory)
{
    NodePtr grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));

    ASSERT_TRUE(bool(grade));
    EXPECT_EQ(eNodeCategoryColor, grade->getNodeCategory());
}

// A native Deep node must read as the Deep category.
TEST_F(BaseTest, NativeDeepNodeIsDeepCategory)
{
    NodePtr deepMerge = createNode(QString::fromUtf8(PLUGINID_NATRON_DEEPMERGE));

    ASSERT_TRUE(bool(deepMerge));
    EXPECT_EQ(eNodeCategoryDeep, deepMerge->getNodeCategory());
}

// A generator with no domain grouping must read as the Generator category.
TEST_F(BaseTest, ConstantIsGeneratorCategory)
{
    NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));

    ASSERT_TRUE(bool(constant));
    EXPECT_EQ(eNodeCategoryGenerator, constant->getNodeCategory());
}

// DeepWrite sets isWriter() but is grouped Deep: the ladder's domain rung must beat the
// Reader/Writer/Generator rung, so this reads as Deep rather than falling through to Writer.
TEST_F(BaseTest, DeepWriteIsDeepCategoryDespiteBeingAWriter)
{
    NodePtr deepWrite = createNode(QString::fromUtf8(PLUGINID_NATRON_DEEPWRITE));

    ASSERT_TRUE(bool(deepWrite));
    EXPECT_EQ(eNodeCategoryDeep, deepWrite->getNodeCategory());
}

// DeepRead sets no isReader() flag at all, but must agree with DeepWrite above: both are Deep.
TEST_F(BaseTest, DeepReadIsDeepCategory)
{
    NodePtr deepRead = createNode(QString::fromUtf8(PLUGINID_NATRON_DEEPREAD));

    ASSERT_TRUE(bool(deepRead));
    EXPECT_EQ(eNodeCategoryDeep, deepRead->getNodeCategory());
}

// A node grouped PLUGIN_GROUP_OTHER, not a reader/writer/generator and matching no keyword,
// must fall all the way down the ladder to Other.
TEST_F(BaseTest, TypedPassthroughIsOtherCategory)
{
    NodePtr passthrough = createNode(QString::fromUtf8(PLUGINID_NATRON_TYPEDPASSTHROUGH));

    ASSERT_TRUE(bool(passthrough));
    EXPECT_EQ(eNodeCategoryOther, passthrough->getNodeCategory());
}

// The keyword heuristic is the last rung before Other, for a third-party OFX plugin whose
// grouping string is not one of Natron's own PLUGIN_GROUP_* literals. It is exercised directly
// since constructing a fake OFX plugin with an arbitrary grouping is impractical from a unit test.
TEST(NodeCategoryHeuristic, UnknownGroupingFallsBackToLabelKeyword)
{
    EXPECT_EQ(eNodeCategoryFilter, Node::categoryFromGroupingAndLabel("Foo", "MyBlur"));
    EXPECT_EQ(eNodeCategoryFilter, Node::categoryFromGroupingAndLabel("Foo", "ZBlur"));
    EXPECT_EQ(eNodeCategoryKeyer, Node::categoryFromGroupingAndLabel("Foo", "UltraKeyer"));
    EXPECT_EQ(eNodeCategoryColor, Node::categoryFromGroupingAndLabel("Foo", "ColorGrade"));
    EXPECT_EQ(eNodeCategoryMerge, Node::categoryFromGroupingAndLabel("Foo", "SuperMerge"));
    EXPECT_EQ(eNodeCategoryMerge, Node::categoryFromGroupingAndLabel("Foo", "Over"));
    EXPECT_EQ(eNodeCategoryTransform, Node::categoryFromGroupingAndLabel("Foo", "ReformatTool"));
    EXPECT_EQ(eNodeCategoryTime, Node::categoryFromGroupingAndLabel("Foo", "TimeOffset2"));
    EXPECT_EQ(eNodeCategoryDraw, Node::categoryFromGroupingAndLabel("Foo", "RotoPaint2"));
    EXPECT_EQ(eNodeCategoryChannel, Node::categoryFromGroupingAndLabel("Foo", "channel_shuffle"));
}

// Keyword matching is case-insensitive.
TEST(NodeCategoryHeuristic, KeywordMatchIsCaseInsensitive)
{
    EXPECT_EQ(eNodeCategoryFilter, Node::categoryFromGroupingAndLabel("Foo", "DEFOCUS"));
    EXPECT_EQ(eNodeCategoryTransform, Node::categoryFromGroupingAndLabel("Foo", "warp tool"));
}

// A keyword only matches a whole word of the label, never a fragment of a longer word.
TEST(NodeCategoryHeuristic, KeywordMustMatchAWholeWord)
{
    EXPECT_EQ(eNodeCategoryOther, Node::categoryFromGroupingAndLabel("Foo", "Overlay"));
    EXPECT_EQ(eNodeCategoryOther, Node::categoryFromGroupingAndLabel("Foo", "Recover"));
    EXPECT_EQ(eNodeCategoryOther, Node::categoryFromGroupingAndLabel("Foo", "Overscan"));
    EXPECT_EQ(eNodeCategoryOther, Node::categoryFromGroupingAndLabel("Foo", "Compensate"));
}

// KeyMix blends two inputs through a mask, so its "Mix" must win over its "Key".
TEST(NodeCategoryHeuristic, KeyMixIsMerge)
{
    EXPECT_EQ(eNodeCategoryMerge, Node::categoryFromGroupingAndLabel("Foo", "KeyMix"));
}

// With no PLUGIN_GROUP_* table match and no keyword match, the heuristic falls back to Other.
TEST(NodeCategoryHeuristic, NoMatchFallsBackToOther)
{
    EXPECT_EQ(eNodeCategoryOther, Node::categoryFromGroupingAndLabel("Foo", "Widget"));
    EXPECT_EQ(eNodeCategoryOther, Node::categoryFromGroupingAndLabel("Foo", "Fx1"));
}

// The major-group table itself, reached directly (as it would be for a plugin that is not a
// Reader/Writer/Generator and whose grouping is one of Natron's own PLUGIN_GROUP_* literals).
TEST(NodeCategoryHeuristic, MajorGroupTableTakesPrecedenceOverKeywords)
{
    EXPECT_EQ(eNodeCategoryKeyer, Node::categoryFromGroupingAndLabel(PLUGIN_GROUP_KEYER, "ColorMerge"));
    EXPECT_EQ(eNodeCategoryViews, Node::categoryFromGroupingAndLabel(PLUGIN_GROUP_MULTIVIEW, "Anything"));
}

// getPluginGrouping() splits "3D/USD" into {"3D", "USD"}, so the USD category must be
// recognised from the first two components rather than from the unsplit literal.
TEST(NodeCategoryDomain, SplitGroupingResolvesDomainCategories)
{
    NodeCategoryEnum category = eNodeCategoryOther;
    std::list<std::string> grouping;

    grouping.push_back("3D");
    ASSERT_TRUE(Node::domainCategoryFromGrouping(grouping, &category));
    EXPECT_EQ(eNodeCategoryNative3D, category);

    grouping.push_back("USD");
    ASSERT_TRUE(Node::domainCategoryFromGrouping(grouping, &category));
    EXPECT_EQ(eNodeCategoryUsd3D, category);

    grouping.push_back("Lights");
    ASSERT_TRUE(Node::domainCategoryFromGrouping(grouping, &category));
    EXPECT_EQ(eNodeCategoryUsd3D, category);

    grouping.clear();
    grouping.push_back(PLUGIN_GROUP_DEEP);
    ASSERT_TRUE(Node::domainCategoryFromGrouping(grouping, &category));
    EXPECT_EQ(eNodeCategoryDeep, category);
}

TEST(NodeCategoryDomain, NonDomainGroupingIsNotADomainCategory)
{
    NodeCategoryEnum category = eNodeCategoryOther;
    std::list<std::string> grouping;

    EXPECT_FALSE(Node::domainCategoryFromGrouping(grouping, &category));

    grouping.push_back("USD");
    EXPECT_FALSE(Node::domainCategoryFromGrouping(grouping, &category));

    grouping.clear();
    grouping.push_back(PLUGIN_GROUP_FILTER);
    EXPECT_FALSE(Node::domainCategoryFromGrouping(grouping, &category));
}
