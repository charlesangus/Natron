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
#include "NativeMetadataTestEffect.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Transform/Crop.h"
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

    NodePtr createGroupInside(const NodePtr& group)
    {
        NodeCollectionPtr collection = std::dynamic_pointer_cast<NodeCollection>(group->getEffectInstance());
        if (!collection) {
            return NodePtr();
        }
        CreateNodeArgs args(PLUGINID_NATRON_GROUP, collection);
        args.setProperty<bool>(kCreateNodeArgsPropNodeGroupDisableCreateInitialNodes, true);
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);

        return getApp()->createNode(args);
    }

    static void setCropRectangle(const NodePtr& crop,
                                 double x,
                                 double y,
                                 double width,
                                 double height)
    {
        KnobDouble* bottomLeft = dynamic_cast<KnobDouble*>(crop->getKnobByName(kNativeGeneratorParamBottomLeft).get());
        KnobDouble* size = dynamic_cast<KnobDouble*>(crop->getKnobByName(kNativeGeneratorParamSize).get());
        ASSERT_TRUE(bottomLeft && size);
        bottomLeft->setValues(x, y, ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
        size->setValues(width, height, ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
    }

    static RectD regionOfDefinition(const NodePtr& node,
                                    const std::string& where)
    {
        EffectInstancePtr effect = node->getEffectInstance();
        RectD rod;
        bool isProjectFormat = false;
        EXPECT_EQ(eStatusOK, effect->getRegionOfDefinition_public(effect->getRenderHash(), 1., RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat)) << where << ": " << node->getScriptName_mt_safe();

        return rod;
    }

    static bool sameRegion(const RectD& a,
                           const RectD& b)
    {
        return (a.x1 == b.x1) && (a.y1 == b.y1) && (a.x2 == b.x2) && (a.y2 == b.y2);
    }

    static void expectSameRegion(const RectD& expected,
                                 const RectD& actual,
                                 const std::string& where)
    {
        EXPECT_TRUE(sameRegion(expected, actual)) << where << ": expected (" << expected.x1 << ", " << expected.y1 << ", " << expected.x2 << ", " << expected.y2
                                                  << "), got (" << actual.x1 << ", " << actual.y1 << ", " << actual.x2 << ", " << actual.y2 << ")";
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

    struct PassThroughBelowAGroup {
        NodePtr upper;
        NodePtr fed;
        NodePtr passThrough;
        NodePtr dot;
    };

    void buildPassThroughBelowAGroup(PassThroughBelowAGroup* g)
    {
        g->upper = createGroup();
        ASSERT_TRUE(g->upper != nullptr);
        g->fed = createSourceInside(g->upper, 80);
        NodePtr upperOutput = createInside(g->upper, PLUGINID_NATRON_OUTPUT);
        ASSERT_TRUE(g->fed && upperOutput);
        ASSERT_TRUE(upperOutput->connectInput(g->fed, 0));
        g->passThrough = createGroup();
        ASSERT_TRUE(g->passThrough != nullptr);
        NodePtr input = createInside(g->passThrough, PLUGINID_NATRON_INPUT);
        NodePtr output = createInside(g->passThrough, PLUGINID_NATRON_OUTPUT);
        ASSERT_TRUE(input && output);
        ASSERT_TRUE(output->connectInput(input, 0));
        ASSERT_TRUE(g->passThrough->connectInput(g->upper, 0));
        g->dot = createAtRoot(PLUGINID_NATRON_DOT);
        ASSERT_TRUE(g->dot != nullptr);
        ASSERT_TRUE(g->dot->connectInput(g->passThrough, 0));
    }

    // The caches are kept between the renders, so a pass-through group whose hash missed an edit
    // inside the group feeding it would serve the previous image.
    static void expectPassThroughBelowAGroupFollowsAnEditInside(const PassThroughBelowAGroup& g,
                                                                bool disabled,
                                                                RenderSchedulerModeEnum mode)
    {
        const std::string where = modeName(mode) + (disabled ? ", disabled" : "");
        appPTR->clearAllCaches();
        g.passThrough->setNodeDisabled(disabled);
        setSeed(g.fed, 80);
        std::vector<RenderedPlane> first;
        std::string error;
        ASSERT_TRUE(render(g.passThrough, mode, false, &first, &error)) << where << ": " << error;
        ASSERT_FALSE(first.empty()) << where;
        const U64 before = g.passThrough->getHashValue();

        setSeed(g.fed, 81);
        EXPECT_NE(before, g.passThrough->getHashValue()) << where << ": the edit did not reach the pass-through group's hash";
        std::vector<RenderedPlane> second;
        std::vector<RenderedPlane> throughDot;
        ASSERT_TRUE(render(g.passThrough, mode, false, &second, &error)) << where << ": " << error;
        ASSERT_TRUE(render(g.dot, mode, true, &throughDot, &error)) << where << ": " << error;
        ASSERT_FALSE(second.empty()) << where;
        expectSamePlanes(throughDot, second, where);
        EXPECT_FALSE(first.front().pixels == second.front().pixels) << where << ": the edit did not reach the pass-through group's render";
        g.passThrough->setNodeDisabled(false);
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

// The enabled group is rendered through the caches first, so a disabled render keyed like it
// would serve the interior's image.
TEST_F(GroupDirectRender, DisabledGroupRendersItsExternalInputInBothModes)
{
    NodePtr group = createGroup();
    ASSERT_TRUE(group != nullptr);
    NodePtr input = createInside(group, PLUGINID_NATRON_INPUT);
    NodePtr fed = createSourceInside(group, 50);
    NodePtr output = createInside(group, PLUGINID_NATRON_OUTPUT);
    ASSERT_TRUE(input && fed && output);
    ASSERT_TRUE(output->connectInput(fed, 0));
    NodePtr source = createAtRoot(kTestPluginIDImageRenderSource);
    ASSERT_TRUE(source != nullptr);
    setSeed(source, 51);
    ASSERT_TRUE(group->connectInput(source, 0));

    for (RenderSchedulerModeEnum mode : { eRenderSchedulerModeLegacy, eRenderSchedulerModeTaskGraph }) {
        appPTR->clearAllCaches();
        group->setNodeDisabled(false);
        std::vector<RenderedPlane> enabled;
        std::string error;
        ASSERT_TRUE(render(group, mode, false, &enabled, &error)) << modeName(mode) << ": " << error;
        ASSERT_FALSE(enabled.empty()) << modeName(mode);

        group->setNodeDisabled(true);
        std::vector<RenderedPlane> disabled;
        std::vector<RenderedPlane> expected;
        ASSERT_TRUE(render(group, mode, false, &disabled, &error)) << modeName(mode) << ": " << error;
        ASSERT_TRUE(render(source, mode, true, &expected, &error)) << modeName(mode) << ": " << error;
        ASSERT_FALSE(disabled.empty()) << modeName(mode);

        expectSamePlanes(expected, disabled, modeName(mode));
        EXPECT_FALSE(enabled.front().pixels == disabled.front().pixels) << modeName(mode) << ": the interior and the external input render alike";
    }
    group->setNodeDisabled(false);
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

namespace {
NodePtr
connectChain(const NodePtr& from,
             const std::vector<NodePtr>& chain)
{
    NodePtr previous = from;
    for (const NodePtr& node : chain) {
        if (!node || !node->connectInput(previous, 0)) {
            return NodePtr();
        }
        previous = node;
    }

    return previous;
}
}

// A Dot below the outer group reads through both groups to the innermost source, so only the
// direct renders take the groups' own path.
TEST_F(GroupDirectRender, NestedGroupsMatchADotBelowTheOuterGroupInBothModes)
{
    NodePtr outer = createGroup();
    ASSERT_TRUE(outer != nullptr);
    NodePtr inner = createGroupInside(outer);
    ASSERT_TRUE(inner != nullptr);
    NodePtr fed = createSourceInside(inner, 3);
    NodePtr innerOutput = createInside(inner, PLUGINID_NATRON_OUTPUT);
    ASSERT_TRUE(fed && innerOutput);
    ASSERT_TRUE(innerOutput->connectInput(fed, 0));
    NodePtr outerOutput = createInside(outer, PLUGINID_NATRON_OUTPUT);
    ASSERT_TRUE(outerOutput != nullptr);
    ASSERT_TRUE(outerOutput->connectInput(inner, 0));
    NodePtr dot = createAtRoot(PLUGINID_NATRON_DOT);
    ASSERT_TRUE(dot != nullptr);
    ASSERT_TRUE(dot->connectInput(outer, 0));

    for (RenderSchedulerModeEnum mode : { eRenderSchedulerModeLegacy, eRenderSchedulerModeTaskGraph }) {
        for (int run = 0; run < 2; ++run) {
            const std::string where = modeName(mode) + " run " + std::to_string(run);
            appPTR->clearAllCaches();
            setSeed(fed, 20 + run);
            std::vector<RenderedPlane> outerDirect;
            std::vector<RenderedPlane> innerDirect;
            std::vector<RenderedPlane> throughDot;
            std::string error;
            ASSERT_TRUE(render(outer, mode, false, &outerDirect, &error)) << where << ": " << error;
            ASSERT_TRUE(render(inner, mode, false, &innerDirect, &error)) << where << ": " << error;
            ASSERT_TRUE(render(dot, mode, true, &throughDot, &error)) << where << ": " << error;
            ASSERT_FALSE(outerDirect.empty()) << where;
            expectSamePlanes(throughDot, outerDirect, where + ", outer group");
            expectSamePlanes(throughDot, innerDirect, where + ", inner group");
        }
    }
}

// The caches are kept between the renders, so an outer group whose hash or region of definition
// missed an edit two groups down would serve the previous image or region.
TEST_F(GroupDirectRender, EditInsideANestedGroupReachesTheOuterGroupInBothModes)
{
    NodePtr outer = createGroup();
    ASSERT_TRUE(outer != nullptr);
    NodePtr inner = createGroupInside(outer);
    ASSERT_TRUE(inner != nullptr);
    NodePtr fed = createSourceInside(inner, 60);
    NodePtr crop = createInside(inner, PLUGINID_NATRON_CROP);
    NodePtr innerOutput = createInside(inner, PLUGINID_NATRON_OUTPUT);
    ASSERT_TRUE(fed && crop && innerOutput);
    ASSERT_TRUE(crop->connectInput(fed, 0));
    ASSERT_TRUE(innerOutput->connectInput(crop, 0));
    NodePtr outerOutput = createInside(outer, PLUGINID_NATRON_OUTPUT);
    ASSERT_TRUE(outerOutput != nullptr);
    ASSERT_TRUE(outerOutput->connectInput(inner, 0));
    NodePtr dot = createAtRoot(PLUGINID_NATRON_DOT);
    ASSERT_TRUE(dot != nullptr);
    ASSERT_TRUE(dot->connectInput(outer, 0));

    for (RenderSchedulerModeEnum mode : { eRenderSchedulerModeLegacy, eRenderSchedulerModeTaskGraph }) {
        const std::string where = modeName(mode);
        appPTR->clearAllCaches();
        setSeed(fed, 60);
        setCropRectangle(crop, 1., 1., 8., 5.);
        std::vector<RenderedPlane> first;
        std::string error;
        ASSERT_TRUE(render(outer, mode, false, &first, &error)) << where << ": " << error;
        ASSERT_FALSE(first.empty()) << where;
        const RectD firstRoD = regionOfDefinition(outer, where);

        const U64 outerBefore = outer->getHashValue();
        const U64 innerBefore = inner->getHashValue();
        setSeed(fed, 61);
        EXPECT_NE(outerBefore, outer->getHashValue()) << where << ": the seed edit did not reach the outer group's hash";
        EXPECT_NE(innerBefore, inner->getHashValue()) << where << ": the seed edit did not reach the inner group's hash";
        std::vector<RenderedPlane> second;
        std::vector<RenderedPlane> throughDot;
        ASSERT_TRUE(render(outer, mode, false, &second, &error)) << where << ": " << error;
        ASSERT_TRUE(render(dot, mode, true, &throughDot, &error)) << where << ": " << error;
        ASSERT_FALSE(second.empty()) << where;
        expectSamePlanes(throughDot, second, where + " after the seed edit");
        EXPECT_FALSE(first.front().pixels == second.front().pixels) << where << ": the seed edit did not reach the outer group's render";

        const U64 outerBeforeCrop = outer->getHashValue();
        setCropRectangle(crop, 2., 1., 4., 3.);
        EXPECT_NE(outerBeforeCrop, outer->getHashValue()) << where << ": the crop edit did not reach the outer group's hash";
        const RectD croppedRoD = regionOfDefinition(outer, where);
        expectSameRegion(regionOfDefinition(dot, where), croppedRoD, where + " after the crop edit");
        EXPECT_FALSE(sameRegion(firstRoD, croppedRoD)) << where << ": the crop edit did not move the outer group's region of definition";
        std::vector<RenderedPlane> third;
        ASSERT_TRUE(render(outer, mode, false, &third, &error)) << where << ": " << error;
        ASSERT_TRUE(render(dot, mode, true, &throughDot, &error)) << where << ": " << error;
        expectSamePlanes(throughDot, third, where + " after the crop edit");
    }
}

TEST_F(GroupDirectRender, LongChainInsideAGroupMatchesADotBelowTheGroupInBothModes)
{
    NodePtr group = createGroup();
    ASSERT_TRUE(group != nullptr);
    NodePtr fed = createSourceInside(group, 4);
    ASSERT_TRUE(fed != nullptr);
    std::vector<NodePtr> chain;
    for (int i = 0; i < 6; ++i) {
        chain.push_back(createInside(group, PLUGINID_NATRON_DOT));
    }
    NodePtr output = createInside(group, PLUGINID_NATRON_OUTPUT);
    ASSERT_TRUE(output != nullptr);
    NodePtr last = connectChain(fed, chain);
    ASSERT_TRUE(last != nullptr);
    ASSERT_TRUE(output->connectInput(last, 0));
    NodePtr dot = createAtRoot(PLUGINID_NATRON_DOT);
    ASSERT_TRUE(dot != nullptr);
    ASSERT_TRUE(dot->connectInput(group, 0));

    for (RenderSchedulerModeEnum mode : { eRenderSchedulerModeLegacy, eRenderSchedulerModeTaskGraph }) {
        appPTR->clearAllCaches();
        std::vector<RenderedPlane> direct;
        std::vector<RenderedPlane> throughDot;
        std::string error;
        ASSERT_TRUE(render(group, mode, false, &direct, &error)) << modeName(mode) << ": " << error;
        ASSERT_TRUE(render(dot, mode, true, &throughDot, &error)) << modeName(mode) << ": " << error;
        ASSERT_FALSE(direct.empty()) << modeName(mode);
        expectSamePlanes(throughDot, direct, modeName(mode));
    }
}

TEST_F(GroupDirectRender, PassThroughGroupMatchesItsExternalInputAndADotBelowInBothModes)
{
    NodePtr group = createGroup();
    ASSERT_TRUE(group != nullptr);
    NodePtr input = createInside(group, PLUGINID_NATRON_INPUT);
    NodePtr output = createInside(group, PLUGINID_NATRON_OUTPUT);
    ASSERT_TRUE(input && output);
    ASSERT_TRUE(output->connectInput(input, 0));
    NodePtr source = createAtRoot(kTestPluginIDImageRenderSource);
    ASSERT_TRUE(source != nullptr);
    ASSERT_TRUE(group->connectInput(source, 0));
    NodePtr dot = createAtRoot(PLUGINID_NATRON_DOT);
    ASSERT_TRUE(dot != nullptr);
    ASSERT_TRUE(dot->connectInput(group, 0));

    for (RenderSchedulerModeEnum mode : { eRenderSchedulerModeLegacy, eRenderSchedulerModeTaskGraph }) {
        for (int run = 0; run < 2; ++run) {
            const std::string where = modeName(mode) + " run " + std::to_string(run);
            appPTR->clearAllCaches();
            setSeed(source, 30 + run);
            std::vector<RenderedPlane> direct;
            std::vector<RenderedPlane> throughDot;
            std::string error;
            std::vector<RenderedPlane> external;
            ASSERT_TRUE(render(group, mode, false, &direct, &error)) << where << ": " << error;
            ASSERT_TRUE(render(dot, mode, true, &throughDot, &error)) << where << ": " << error;
            ASSERT_TRUE(render(source, mode, true, &external, &error)) << where << ": " << error;
            ASSERT_FALSE(direct.empty()) << where;
            expectSamePlanes(throughDot, direct, where);
            expectSamePlanes(external, direct, where + " against the external input");
        }
    }
}

// The caches are kept between the two renders so that a stale hash or region of definition
// would serve the first render's image.
TEST_F(GroupDirectRender, InnerEditAfterARenderIsReflectedInTheNextRenderInBothModes)
{
    NodePtr group = createGroup();
    ASSERT_TRUE(group != nullptr);
    NodePtr fed = createSourceInside(group, 1);
    ASSERT_TRUE(fed != nullptr);
    NodePtr crop = createInside(group, PLUGINID_NATRON_CROP);
    NodePtr output = createInside(group, PLUGINID_NATRON_OUTPUT);
    ASSERT_TRUE(crop && output);
    NodePtr last = connectChain(fed, { crop });
    ASSERT_TRUE(last != nullptr);
    ASSERT_TRUE(output->connectInput(last, 0));
    NodePtr dot = createAtRoot(PLUGINID_NATRON_DOT);
    ASSERT_TRUE(dot != nullptr);
    ASSERT_TRUE(dot->connectInput(group, 0));

    for (RenderSchedulerModeEnum mode : { eRenderSchedulerModeLegacy, eRenderSchedulerModeTaskGraph }) {
        appPTR->clearAllCaches();
        setSeed(fed, 40);
        setCropRectangle(crop, 1., 1., 8., 5.);
        std::vector<RenderedPlane> first;
        std::string error;
        ASSERT_TRUE(render(group, mode, false, &first, &error)) << modeName(mode) << ": " << error;
        ASSERT_FALSE(first.empty()) << modeName(mode);

        setSeed(fed, 41);
        std::vector<RenderedPlane> second;
        std::vector<RenderedPlane> throughDot;
        ASSERT_TRUE(render(group, mode, false, &second, &error)) << modeName(mode) << ": " << error;
        ASSERT_TRUE(render(dot, mode, true, &throughDot, &error)) << modeName(mode) << ": " << error;
        ASSERT_FALSE(second.empty()) << modeName(mode);

        expectSamePlanes(throughDot, second, modeName(mode));
        EXPECT_FALSE(first.front().pixels == second.front().pixels) << modeName(mode) << ": the edit did not reach the second render";

        const RectD before = regionOfDefinition(group, modeName(mode));
        setCropRectangle(crop, 2., 1., 4., 3.);
        const RectD after = regionOfDefinition(group, modeName(mode));
        expectSameRegion(regionOfDefinition(dot, modeName(mode)), after, modeName(mode) + " after the crop edit");
        EXPECT_FALSE(sameRegion(before, after)) << modeName(mode) << ": the crop edit did not move the group's region of definition";
        std::vector<RenderedPlane> third;
        ASSERT_TRUE(render(group, mode, false, &third, &error)) << modeName(mode) << ": " << error;
        ASSERT_TRUE(render(dot, mode, true, &throughDot, &error)) << modeName(mode) << ": " << error;
        expectSamePlanes(throughDot, third, modeName(mode) + " after the crop edit");
    }
}

// The caches are kept between the renders, so a group two or three levels up whose hash missed
// the edit would serve the previous image.
TEST_F(GroupDirectRender, EditThreeGroupsDownReachesEveryEnclosingGroupInBothModes)
{
    NodePtr outer = createGroup();
    ASSERT_TRUE(outer != nullptr);
    NodePtr middle = createGroupInside(outer);
    ASSERT_TRUE(middle != nullptr);
    NodePtr inner = createGroupInside(middle);
    ASSERT_TRUE(inner != nullptr);
    NodePtr fed = createSourceInside(inner, 70);
    NodePtr innerOutput = createInside(inner, PLUGINID_NATRON_OUTPUT);
    NodePtr middleOutput = createInside(middle, PLUGINID_NATRON_OUTPUT);
    NodePtr outerOutput = createInside(outer, PLUGINID_NATRON_OUTPUT);
    ASSERT_TRUE(fed && innerOutput && middleOutput && outerOutput);
    ASSERT_TRUE(innerOutput->connectInput(fed, 0));
    ASSERT_TRUE(middleOutput->connectInput(inner, 0));
    ASSERT_TRUE(outerOutput->connectInput(middle, 0));
    NodePtr dot = createAtRoot(PLUGINID_NATRON_DOT);
    ASSERT_TRUE(dot != nullptr);
    ASSERT_TRUE(dot->connectInput(outer, 0));
    const std::vector<NodePtr> groups = { outer, middle, inner };

    for (RenderSchedulerModeEnum mode : { eRenderSchedulerModeLegacy, eRenderSchedulerModeTaskGraph }) {
        const std::string where = modeName(mode);
        appPTR->clearAllCaches();
        setSeed(fed, 70);
        std::vector<std::vector<RenderedPlane>> first(groups.size());
        std::vector<U64> before(groups.size());
        std::string error;
        for (std::size_t i = 0; i < groups.size(); ++i) {
            ASSERT_TRUE(render(groups[i], mode, false, &first[i], &error)) << where << ": " << error;
            ASSERT_FALSE(first[i].empty()) << where;
            before[i] = groups[i]->getHashValue();
        }

        setSeed(fed, 71);
        std::vector<RenderedPlane> throughDot;
        ASSERT_TRUE(render(dot, mode, true, &throughDot, &error)) << where << ": " << error;
        for (std::size_t i = 0; i < groups.size(); ++i) {
            const std::string group = where + ", " + groups[i]->getScriptName_mt_safe();
            EXPECT_NE(before[i], groups[i]->getHashValue()) << group << ": the edit did not reach the group's hash";
            std::vector<RenderedPlane> second;
            ASSERT_TRUE(render(groups[i], mode, false, &second, &error)) << group << ": " << error;
            expectSamePlanes(throughDot, second, group);
            ASSERT_FALSE(second.empty()) << group;
            EXPECT_FALSE(first[i].front().pixels == second.front().pixels) << group << ": the edit did not reach the group's render";
        }
    }
}

TEST_F(GroupDirectRender, PassThroughGroupBelowAGroupFollowsAnEditInsideItInBothModes)
{
    PassThroughBelowAGroup g;
    ASSERT_NO_FATAL_FAILURE(buildPassThroughBelowAGroup(&g));

    for (RenderSchedulerModeEnum mode : { eRenderSchedulerModeLegacy, eRenderSchedulerModeTaskGraph }) {
        ASSERT_NO_FATAL_FAILURE(expectPassThroughBelowAGroupFollowsAnEditInside(g, false, mode));
    }
}

// A disabled group renders its external input, here the group above it.
TEST_F(GroupDirectRender, DisabledPassThroughGroupBelowAGroupFollowsAnEditInsideItInBothModes)
{
    PassThroughBelowAGroup g;
    ASSERT_NO_FATAL_FAILURE(buildPassThroughBelowAGroup(&g));

    for (RenderSchedulerModeEnum mode : { eRenderSchedulerModeLegacy, eRenderSchedulerModeTaskGraph }) {
        ASSERT_NO_FATAL_FAILURE(expectPassThroughBelowAGroupFollowsAnEditInside(g, true, mode));
    }
}

// The outer group's GroupInput feeds the nested pass-through group directly, so an edit of the
// external input reaches the nested group only through the outer one's redirections.
TEST_F(GroupDirectRender, NestedPassThroughGroupFedByAGroupInputFollowsAnExternalEditInBothModes)
{
    NodePtr outer = createGroup();
    ASSERT_TRUE(outer != nullptr);
    NodePtr outerInput = createInside(outer, PLUGINID_NATRON_INPUT);
    NodePtr outerOutput = createInside(outer, PLUGINID_NATRON_OUTPUT);
    NodePtr inner = createGroupInside(outer);
    ASSERT_TRUE(outerInput && outerOutput && inner);
    NodePtr innerInput = createInside(inner, PLUGINID_NATRON_INPUT);
    NodePtr innerOutput = createInside(inner, PLUGINID_NATRON_OUTPUT);
    ASSERT_TRUE(innerInput && innerOutput);
    ASSERT_TRUE(innerOutput->connectInput(innerInput, 0));
    ASSERT_TRUE(inner->connectInput(outerInput, 0));
    ASSERT_TRUE(outerOutput->connectInput(inner, 0));
    NodePtr source = createAtRoot(kTestPluginIDImageRenderSource);
    ASSERT_TRUE(source != nullptr);
    ASSERT_TRUE(outer->connectInput(source, 0));
    NodePtr dot = createAtRoot(PLUGINID_NATRON_DOT);
    ASSERT_TRUE(dot != nullptr);
    ASSERT_TRUE(dot->connectInput(outer, 0));

    for (RenderSchedulerModeEnum mode : { eRenderSchedulerModeLegacy, eRenderSchedulerModeTaskGraph }) {
        const std::string where = modeName(mode);
        appPTR->clearAllCaches();
        setSeed(source, 90);
        std::vector<RenderedPlane> first;
        std::string error;
        ASSERT_TRUE(render(inner, mode, false, &first, &error)) << where << ": " << error;
        ASSERT_FALSE(first.empty()) << where;
        const U64 outerBefore = outer->getHashValue();
        const U64 innerBefore = inner->getHashValue();

        setSeed(source, 91);
        EXPECT_NE(outerBefore, outer->getHashValue()) << where << ": the edit did not reach the outer group's hash";
        EXPECT_NE(innerBefore, inner->getHashValue()) << where << ": the edit did not reach the nested group's hash";
        std::vector<RenderedPlane> second;
        std::vector<RenderedPlane> throughDot;
        ASSERT_TRUE(render(inner, mode, false, &second, &error)) << where << ": " << error;
        ASSERT_TRUE(render(dot, mode, true, &throughDot, &error)) << where << ": " << error;
        ASSERT_FALSE(second.empty()) << where;
        expectSamePlanes(throughDot, second, where);
        EXPECT_FALSE(first.front().pixels == second.front().pixels) << where << ": the edit did not reach the nested group's render";
    }
}

// The group is reached as a leaf from the inner mark and through its input from the external
// one, in either order within one flush.
TEST_F(GroupDirectRender, InnerAndExternalMarksInOneFlushReachTheGroupsConsumersAndMetadataReader)
{
    NodePtr group = createGroup();
    ASSERT_TRUE(group != nullptr);
    NodePtr input = createInside(group, PLUGINID_NATRON_INPUT);
    NodePtr inputReader = createInside(group, PLUGINID_NATRON_DOT);
    NodePtr fed = createSourceInside(group, 100);
    NodePtr output = createInside(group, PLUGINID_NATRON_OUTPUT);
    ASSERT_TRUE(input && inputReader && fed && output);
    ASSERT_TRUE(inputReader->connectInput(input, 0));
    ASSERT_TRUE(output->connectInput(fed, 0));
    NodePtr external = createAtRoot(kTestPluginIDImageRenderSource);
    ASSERT_TRUE(external != nullptr);
    ASSERT_TRUE(group->connectInput(external, 0));
    NodePtr dot = createAtRoot(PLUGINID_NATRON_DOT);
    ASSERT_TRUE(dot != nullptr);
    ASSERT_TRUE(dot->connectInput(group, 0));
    NodePtr reader = createAtRoot(kTestPluginIDMetadataSource);
    ASSERT_TRUE(reader != nullptr);
    KnobIPtr readerTag = reader->getKnobByName("tag");
    ASSERT_TRUE(readerTag != nullptr);
    ASSERT_NO_THROW(readerTag->setExpression(0, "int(" + group->getScriptName() + ".metadata('exr/tag', 1) or 0)", false, true));
    std::list<std::pair<KnobIPtr, int>> metadataReaders;
    group->getMetadataReaders(&metadataReaders);
    bool registered = false;
    for (std::list<std::pair<KnobIPtr, int>>::const_iterator it = metadataReaders.begin(); it != metadataReaders.end(); ++it) {
        registered = registered || (it->first == readerTag);
    }
    ASSERT_TRUE(registered) << "the expression did not register the reader with the group";

    const std::vector<NodePtr> expectedDirty = { group, dot, reader, inputReader };
    for (bool innerFirst : { true, false }) {
        const std::string where = innerFirst ? "inner mark first" : "external mark first";
        std::vector<U64> before;
        for (const NodePtr& node : expectedDirty) {
            (void)node->getHashValue();
            before.push_back(node->getHashMarkCount());
        }

        // Marked directly rather than through knob edits, so that no read between the two
        // flushes the first mark on its own.
        if (innerFirst) {
            fed->markHashDirty();
            external->markHashDirty();
        } else {
            external->markHashDirty();
            fed->markHashDirty();
        }
        for (std::size_t i = 0; i < expectedDirty.size(); ++i) {
            EXPECT_NE(before[i], expectedDirty[i]->getHashMarkCount()) << where << ": " << expectedDirty[i]->getScriptName_mt_safe() << " was not marked";
        }
    }
}
