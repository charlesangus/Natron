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
#include <vector>

#include <gtest/gtest.h>

#include "BaseTest.h"
#include "DeepRenderTestEffect.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_USING

class GroupDirectRender
    : public BaseTest {
protected:
    NodePtr createInside(const NodePtr& group,
                         const std::string& pluginID)
    {
        NodeCollectionPtr collection = std::dynamic_pointer_cast<NodeCollection>(group->getEffectInstance());
        if (!collection) {
            return NodePtr();
        }
        CreateNodeArgs args(pluginID, collection);
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);

        return getApp()->createNode(args);
    }

    NodePtr createSourceInside(const NodePtr& group,
                               int seed)
    {
        NodePtr node = createInside(group, kTestPluginIDImageRenderSource);
        if (!node) {
            return node;
        }
        KnobInt* knob = dynamic_cast<KnobInt*>(node->getKnobByName("seed").get());
        if (!knob) {
            return NodePtr();
        }
        knob->setValue(seed);

        return node;
    }

    static void setSeed(const NodePtr& node,
                        int seed)
    {
        KnobInt* knob = dynamic_cast<KnobInt*>(node->getKnobByName("seed").get());
        ASSERT_TRUE(knob != nullptr);
        knob->setValue(seed);
    }

    NodePtr createGroup()
    {
        CreateNodeArgs args(PLUGINID_NATRON_GROUP, getApp()->getProject());
        args.setProperty<bool>(kCreateNodeArgsPropNodeGroupDisableCreateInitialNodes, true);
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);

        return getApp()->createNode(args);
    }

    NodePtr createAtRoot(const std::string& pluginID)
    {
        CreateNodeArgs args(pluginID, getApp()->getProject());
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);

        return getApp()->createNode(args);
    }

    static RectI window()
    {
        return RectI(0, 0, kImageRenderTestWidth, kImageRenderTestHeight);
    }

    static bool render(const NodePtr& node,
                       RenderSchedulerModeEnum mode,
                       bool byPassCache,
                       std::vector<RenderedPlane>* planes,
                       std::string* error)
    {
        return renderNodePlanesInMode(node, 1., ViewIdx(0), 0, window(), std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), mode, byPassCache, planes, error);
    }

    static void expectSamePlanes(const std::vector<RenderedPlane>& expected,
                                 const std::vector<RenderedPlane>& actual,
                                 const std::string& where)
    {
        ASSERT_EQ(expected.size(), actual.size()) << where;
        for (std::size_t i = 0; i < expected.size(); ++i) {
            EXPECT_EQ(expected[i].channels, actual[i].channels) << where;
            EXPECT_TRUE(expected[i].pixels == actual[i].pixels) << where << ": the pixels of plane " << i << " differ";
        }
    }

    static std::string modeName(RenderSchedulerModeEnum mode)
    {
        return (mode == eRenderSchedulerModeLegacy) ? "legacy" : "task graph";
    }
};

TEST_F(GroupDirectRender, HashChangesWhenAnInnerKnobFeedingTheOutputChanges)
{
    NodePtr group = createGroup();
    ASSERT_TRUE(group != nullptr);
    NodePtr fed = createSourceInside(group, 1);
    NodePtr output = createInside(group, PLUGINID_NATRON_OUTPUT);
    ASSERT_TRUE(fed && output);
    ASSERT_TRUE(output->connectInput(fed, 0));

    const U64 before = group->getHashValue();
    setSeed(fed, 2);

    EXPECT_NE(before, group->getHashValue());
}

TEST_F(GroupDirectRender, HashChangesWhenTheOutputIsReconnected)
{
    NodePtr group = createGroup();
    ASSERT_TRUE(group != nullptr);
    NodePtr first = createSourceInside(group, 1);
    NodePtr second = createSourceInside(group, 2);
    NodePtr output = createInside(group, PLUGINID_NATRON_OUTPUT);
    ASSERT_TRUE(first && second && output);
    ASSERT_TRUE(output->connectInput(first, 0));

    const U64 before = group->getHashValue();
    output->disconnectInput(0);
    ASSERT_TRUE(output->connectInput(second, 0));

    EXPECT_NE(before, group->getHashValue());
}

TEST_F(GroupDirectRender, HashIgnoresAnInnerNodeNotFeedingTheOutput)
{
    NodePtr group = createGroup();
    ASSERT_TRUE(group != nullptr);
    NodePtr fed = createSourceInside(group, 1);
    NodePtr unrelated = createSourceInside(group, 5);
    NodePtr output = createInside(group, PLUGINID_NATRON_OUTPUT);
    ASSERT_TRUE(fed && unrelated && output);
    ASSERT_TRUE(output->connectInput(fed, 0));

    const U64 before = group->getHashValue();
    setSeed(unrelated, 6);

    EXPECT_EQ(before, group->getHashValue());
}

// Every run first renders the group in another colour through the caches, so a render of the
// group that reused that image rather than its source's would differ from the Dot's.
TEST_F(GroupDirectRender, DirectRenderMatchesADotBelowTheGroupInBothModes)
{
    NodePtr group = createGroup();
    ASSERT_TRUE(group != nullptr);
    NodePtr fed = createSourceInside(group, 1);
    NodePtr output = createInside(group, PLUGINID_NATRON_OUTPUT);
    ASSERT_TRUE(fed && output);
    ASSERT_TRUE(output->connectInput(fed, 0));
    NodePtr dot = createAtRoot(PLUGINID_NATRON_DOT);
    ASSERT_TRUE(dot != nullptr);
    ASSERT_TRUE(dot->connectInput(group, 0));

    for (RenderSchedulerModeEnum mode : { eRenderSchedulerModeLegacy, eRenderSchedulerModeTaskGraph }) {
        for (int run = 0; run < 3; ++run) {
            const std::string where = modeName(mode) + " run " + std::to_string(run);
            const int seed = 10 + run;
            std::vector<RenderedPlane> other;
            std::string error;
            setSeed(fed, seed + 100);
            ASSERT_TRUE(render(group, mode, false, &other, &error)) << where << ": " << error;
            appPTR->clearAllCaches();

            setSeed(fed, seed);
            std::vector<RenderedPlane> direct;
            ASSERT_TRUE(render(group, mode, false, &direct, &error)) << where << ": " << error;
            std::vector<RenderedPlane> throughDot;
            ASSERT_TRUE(render(dot, mode, true, &throughDot, &error)) << where << ": " << error;

            expectSamePlanes(throughDot, direct, where);
            ASSERT_FALSE(other.empty()) << where;
            ASSERT_FALSE(direct.empty()) << where;
            EXPECT_FALSE(other.front().pixels == direct.front().pixels) << where << ": the seed does not change the source's pixels";
            appPTR->clearAllCaches();
        }
    }
}

TEST_F(GroupDirectRender, GroupInputFeedingTheOutputRendersTheExternalInput)
{
    NodePtr group = createGroup();
    ASSERT_TRUE(group != nullptr);
    NodePtr input = createInside(group, PLUGINID_NATRON_INPUT);
    NodePtr output = createInside(group, PLUGINID_NATRON_OUTPUT);
    ASSERT_TRUE(input && output);
    ASSERT_TRUE(output->connectInput(input, 0));
    NodePtr source = createAtRoot(kTestPluginIDImageRenderSource);
    ASSERT_TRUE(source != nullptr);
    setSeed(source, 7);
    ASSERT_TRUE(group->connectInput(source, 0));

    for (RenderSchedulerModeEnum mode : { eRenderSchedulerModeLegacy, eRenderSchedulerModeTaskGraph }) {
        std::vector<RenderedPlane> expected;
        std::vector<RenderedPlane> direct;
        std::string error;
        ASSERT_TRUE(render(source, mode, true, &expected, &error)) << modeName(mode) << ": " << error;
        ASSERT_TRUE(render(group, mode, true, &direct, &error)) << modeName(mode) << ": " << error;
        expectSamePlanes(expected, direct, modeName(mode));
    }
}

TEST_F(GroupDirectRender, GeneratorOnlyGroupHasTheGeneratorsRegionOfDefinition)
{
    NodePtr group = createGroup();
    ASSERT_TRUE(group != nullptr);
    NodePtr fed = createSourceInside(group, 1);
    NodePtr output = createInside(group, PLUGINID_NATRON_OUTPUT);
    ASSERT_TRUE(fed && output);
    ASSERT_TRUE(output->connectInput(fed, 0));

    EffectInstancePtr generator = fed->getEffectInstance();
    RectD expected;
    bool isProjectFormat = false;
    ASSERT_EQ(eStatusOK, generator->getRegionOfDefinition_public(generator->getRenderHash(), 1., RenderScale::identity, ViewIdx(0), &expected, &isProjectFormat));
    ASSERT_FALSE(expected.isNull());

    EffectInstancePtr effect = group->getEffectInstance();
    RectD rod;
    ASSERT_EQ(eStatusOK, effect->getRegionOfDefinition_public(effect->getRenderHash(), 1., RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat));
    EXPECT_EQ(expected.x1, rod.x1);
    EXPECT_EQ(expected.y1, rod.y1);
    EXPECT_EQ(expected.x2, rod.x2);
    EXPECT_EQ(expected.y2, rod.y2);
}

TEST_F(GroupDirectRender, UnfedGroupRendersNoPlaneAndSucceeds)
{
    NodePtr group = createGroup();
    ASSERT_TRUE(group != nullptr);
    NodePtr fed = createSourceInside(group, 1);
    NodePtr output = createInside(group, PLUGINID_NATRON_OUTPUT);
    ASSERT_TRUE(fed && output);

    for (RenderSchedulerModeEnum mode : { eRenderSchedulerModeLegacy, eRenderSchedulerModeTaskGraph }) {
        std::vector<RenderedPlane> planes;
        std::string error;
        EXPECT_FALSE(render(group, mode, true, &planes, &error)) << modeName(mode);
        EXPECT_EQ(std::string(kRenderProducedNoPlane), error) << modeName(mode);
        EXPECT_TRUE(planes.empty()) << modeName(mode);
    }
}
