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
#include <vector>

#include <gtest/gtest.h>

#include "BaseTest.h"
#include "InputChangedFetchTestEffect.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

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

void
buildConstantGradeChain(const AppInstancePtr& app,
                        const std::string& prefix,
                        std::vector<NodePtr>* chain)
{
    NodePtr constant = createNamedNode(app, PLUGINID_OFX_CONSTANT, prefix + "Constant");
    ASSERT_TRUE(bool(constant));
    chain->push_back(constant);
    for (int i = 1; i <= 3; ++i) {
        NodePtr grade = createNamedNode(app, PLUGINID_OFX_GRADE, prefix + "Grade" + std::to_string(i));
        ASSERT_TRUE(bool(grade));
        ASSERT_TRUE(grade->connectInput(chain->back(), 0));
        chain->push_back(grade);
    }
}

void
animateColorKnob(const NodePtr& node,
                 const char* knobName)
{
    KnobColor* color = dynamic_cast<KnobColor*>(node->getKnobByName(knobName).get());
    ASSERT_TRUE(color != NULL);
    color->setValueAtTime(1, 0.25, ViewSpec::all(), 0);
    color->setValueAtTime(10, 0.75, ViewSpec::all(), 0);
}

void
expectFrameVaryingFromSetterAndFallback(const AppInstancePtr& app,
                                        const std::vector<NodePtr>& chain,
                                        const std::vector<bool>& expected)
{
    ASSERT_EQ(chain.size(), expected.size());

    for (std::size_t i = 0; i < chain.size(); ++i) {
        EXPECT_FALSE(bool(chain[i]->getEffectInstance()->getParallelRenderArgsTLS()));
        EXPECT_EQ(expected[i], chain[i]->getEffectInstance()->isFrameVaryingOrAnimated_Recursive()) << chain[i]->getScriptName();
    }

    AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
    ParallelRenderArgsSetter setter(1.,
                                    ViewIdx(0),
                                    false,
                                    false,
                                    abortInfo,
                                    chain.back(),
                                    0,
                                    app->getTimeLine().get(),
                                    NodePtr(),
                                    false,
                                    false,
                                    RenderStatsPtr());

    for (std::size_t i = 0; i < chain.size(); ++i) {
        ParallelRenderArgsPtr frameArgs = chain[i]->getEffectInstance()->getParallelRenderArgsTLS();
        ASSERT_TRUE(bool(frameArgs)) << chain[i]->getScriptName();
        EXPECT_TRUE(frameArgs->frameVaryingComputed) << chain[i]->getScriptName();
        EXPECT_EQ(expected[i], bool(frameArgs->isFrameVaryingOrAnimated)) << chain[i]->getScriptName();
    }
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

TEST_F(GraphScalingTest, InputFetchedFromOnInputChangedLeavesNoFrameArgsUpstream)
{
    const AppInstancePtr app = getApp();

    NodePtr constant = createNamedNode(app, PLUGINID_OFX_CONSTANT, "FetchConstant");
    NodePtr gradeA = createNamedNode(app, PLUGINID_OFX_GRADE, "FetchGradeA");
    NodePtr gradeB = createNamedNode(app, PLUGINID_OFX_GRADE, "FetchGradeB");
    NodePtr fetcher = createNamedNode(app, kTestPluginIDInputChangedFetch, "Fetcher");
    ASSERT_TRUE(bool(constant));
    ASSERT_TRUE(bool(gradeA));
    ASSERT_TRUE(bool(gradeB));
    ASSERT_TRUE(bool(fetcher));

    InputChangedFetchTestEffect* fetchEffect = dynamic_cast<InputChangedFetchTestEffect*>(fetcher->getEffectInstance().get());
    ASSERT_TRUE(fetchEffect != NULL);

    ASSERT_TRUE(gradeA->connectInput(constant, 0));
    ASSERT_TRUE(gradeB->connectInput(gradeA, 0));
    ASSERT_TRUE(fetcher->connectInput(gradeB, 0));

    EXPECT_TRUE(bool(fetchEffect->getFetchedImage()));

    EXPECT_FALSE(bool(constant->getEffectInstance()->getParallelRenderArgsTLS()));
    EXPECT_FALSE(bool(gradeA->getEffectInstance()->getParallelRenderArgsTLS()));
    EXPECT_FALSE(bool(gradeB->getEffectInstance()->getParallelRenderArgsTLS()));
    EXPECT_FALSE(bool(fetcher->getEffectInstance()->getParallelRenderArgsTLS()));

    KnobColor* color = dynamic_cast<KnobColor*>(constant->getKnobByName("color").get());
    ASSERT_TRUE(color != NULL);
    const U64 hashBeforeEdit = gradeA->getEffectInstance()->getHash();
    color->setValues(0.5, 0.25, 0.125, 1., ViewSpec::all(), eValueChangedReasonNatronInternalEdited);

    EXPECT_NE(hashBeforeEdit, gradeA->getEffectInstance()->getHash());
    EXPECT_EQ(gradeA->getEffectInstance()->getHash(), gradeA->getEffectInstance()->getRenderHash());
}

TEST_F(GraphScalingTest, FrameVaryingIsPropagatedDownstreamOnly)
{
    const AppInstancePtr app = getApp();

    std::vector<NodePtr> animatedTop;
    ASSERT_NO_FATAL_FAILURE(buildConstantGradeChain(app, "Top", &animatedTop));
    ASSERT_NO_FATAL_FAILURE(animateColorKnob(animatedTop.front(), "color"));
    ASSERT_TRUE(animatedTop.front()->getEffectInstance()->getHasAnimation());
    expectFrameVaryingFromSetterAndFallback(app, animatedTop, std::vector<bool>(animatedTop.size(), true));

    std::vector<NodePtr> animatedBottom;
    ASSERT_NO_FATAL_FAILURE(buildConstantGradeChain(app, "Bottom", &animatedBottom));
    ASSERT_NO_FATAL_FAILURE(animateColorKnob(animatedBottom.back(), "multiply"));
    ASSERT_TRUE(animatedBottom.back()->getEffectInstance()->getHasAnimation());
    std::vector<bool> expected(animatedBottom.size(), false);
    expected.back() = true;
    expectFrameVaryingFromSetterAndFallback(app, animatedBottom, expected);
}

TEST_F(GraphScalingTest, FrameVaryingFallbackIsLinearOnDiamondLadder)
{
    const int kLevels = 30;
    const AppInstancePtr app = getApp();

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

    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    const bool varying = previous->getEffectInstance()->isFrameVaryingOrAnimated_Recursive();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    EXPECT_FALSE(varying);
    EXPECT_LT(seconds, 5.);
}
