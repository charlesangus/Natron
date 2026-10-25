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

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include "Global/Macros.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "BaseTest.h"
#include "SceneTestEffect.h"

#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneLayerMemo.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/StageCache.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_USING

namespace {
SceneLayerMemo::Key
makeKey(U64 digest)
{
    SceneLayerMemo::Key key;

    key.nodeDigest = digest;
    key.firstFrame = 1.;
    key.lastFrame = 100.;
    key.framesPerSecond = 24.;

    return key;
}

SceneLayerMemo::AuthorFunc
makeAuthor(int value)
{
    return [value](const SceneLayerEdit& edit, std::string* errorMessage) {
        return sceneTestAuthorSource(edit, value, errorMessage);
    };
}

PXR_NS::SdfLayerRefPtr
topLayer(const ScenePayload& payload)
{
    const std::vector<SceneLayerEntry> layers = SceneOps::getLayers(payload);

    return layers.empty() ? PXR_NS::SdfLayerRefPtr() : layers.back().layer;
}

ScenePayload
makePayload(int value)
{
    SceneLayerMemo memo;
    ScenePayload payload;
    std::string error;

    memo.produce(makeKey(value), ScenePayload(), "SceneLayerMemoTest", makeAuthor(value), &payload, &error);

    return payload;
}
} // anonymous namespace

TEST(SceneLayerMemoTest, UnchangedKeyReusesTheLayer)
{
    SceneLayerMemo memo;
    ScenePayload first, second;
    std::string error;

    ASSERT_TRUE(memo.produce(makeKey(1), ScenePayload(), "SceneLayerMemoTest", makeAuthor(1), &first, &error)) << error;
    ASSERT_TRUE(memo.produce(makeKey(1), ScenePayload(), "SceneLayerMemoTest", makeAuthor(1), &second, &error)) << error;

    EXPECT_EQ((U64)1, memo.getAuthorCount());
    EXPECT_TRUE(first.isSameStack(second));
    EXPECT_EQ(topLayer(first), topLayer(second));
    // A memoized layer is read-only, so a consumer cannot edit what other stacks share.
    EXPECT_FALSE(topLayer(first)->PermissionToEdit());
}

TEST(SceneLayerMemoTest, ChangedKeyAuthorsANewLayer)
{
    SceneLayerMemo memo;
    ScenePayload first, changedDigest, changedRange;
    std::string error;

    ASSERT_TRUE(memo.produce(makeKey(1), ScenePayload(), "SceneLayerMemoTest", makeAuthor(1), &first, &error)) << error;
    ASSERT_TRUE(memo.produce(makeKey(2), ScenePayload(), "SceneLayerMemoTest", makeAuthor(2), &changedDigest, &error)) << error;
    EXPECT_EQ((U64)2, memo.getAuthorCount());
    EXPECT_NE(topLayer(first), topLayer(changedDigest));
    EXPECT_NE(first.getStackHash(), changedDigest.getStackHash());

    SceneLayerMemo::Key key = makeKey(2);
    key.lastFrame = 200.;
    ASSERT_TRUE(memo.produce(key, ScenePayload(), "SceneLayerMemoTest", makeAuthor(2), &changedRange, &error)) << error;
    EXPECT_EQ((U64)3, memo.getAuthorCount());
    EXPECT_NE(topLayer(changedDigest), topLayer(changedRange));
}

// A new upstream with the same content re-authors nothing: the memoized layer is put on top of
// the new upstream.
TEST(SceneLayerMemoTest, ChangedUpstreamReusesTheLayerOnTheNewStack)
{
    const ScenePayload upstreamA = makePayload(10);
    const ScenePayload upstreamB = makePayload(10);

    ASSERT_FALSE(upstreamA.isSameStack(upstreamB));
    ASSERT_EQ(upstreamA.getStackHash(), upstreamB.getStackHash());

    SceneLayerMemo memo;
    ScenePayload onA, onB;
    std::string error;
    ASSERT_TRUE(memo.produce(makeKey(1), upstreamA, "SceneLayerMemoTest", makeAuthor(1), &onA, &error)) << error;
    ASSERT_TRUE(memo.produce(makeKey(1), upstreamB, "SceneLayerMemoTest", makeAuthor(1), &onB, &error)) << error;

    EXPECT_EQ((U64)1, memo.getAuthorCount());
    EXPECT_EQ(topLayer(onA), topLayer(onB));
    EXPECT_EQ((std::size_t)2, onB.getNumLayers());
    EXPECT_EQ(topLayer(upstreamB), SceneOps::getLayers(onB)[0].layer);
}

TEST(SceneLayerMemoTest, FailedAuthoringKeepsThePreviousLayer)
{
    SceneLayerMemo memo;
    ScenePayload first, failed, again;
    std::string error;

    ASSERT_TRUE(memo.produce(makeKey(1), ScenePayload(), "SceneLayerMemoTest", makeAuthor(1), &first, &error)) << error;

    const SceneLayerMemo::AuthorFunc failing = [](const SceneLayerEdit& /*edit*/, std::string* errorMessage) {
        *errorMessage = "authoring failed";

        return false;
    };
    error.clear();
    EXPECT_FALSE(memo.produce(makeKey(2), ScenePayload(), "SceneLayerMemoTest", failing, &failed, &error));
    EXPECT_EQ(std::string("authoring failed"), error);

    ASSERT_TRUE(memo.produce(makeKey(1), ScenePayload(), "SceneLayerMemoTest", makeAuthor(1), &again, &error)) << error;
    EXPECT_EQ(topLayer(first), topLayer(again));
}

TEST(SceneLayerMemoTest, InvalidateForcesAuthoring)
{
    SceneLayerMemo memo;
    ScenePayload first, second;
    std::string error;

    ASSERT_TRUE(memo.produce(makeKey(1), ScenePayload(), "SceneLayerMemoTest", makeAuthor(1), &first, &error)) << error;
    memo.invalidate();
    ASSERT_TRUE(memo.produce(makeKey(1), ScenePayload(), "SceneLayerMemoTest", makeAuthor(1), &second, &error)) << error;

    EXPECT_EQ((U64)2, memo.getAuthorCount());
    EXPECT_NE(topLayer(first), topLayer(second));
}

TEST(StageCacheTest, RepeatedStackHitsAndNewStackMisses)
{
    StageCache cache(4);
    const ScenePayload a = makePayload(1);
    const ScenePayload b = makePayload(2);

    PXR_NS::UsdStageRefPtr first = cache.getStage(a);
    ASSERT_TRUE(bool(first));
    EXPECT_EQ((U64)0, cache.getHitCount());
    EXPECT_EQ((U64)1, cache.getMissCount());

    PXR_NS::UsdStageRefPtr again = cache.getStage(a);
    EXPECT_EQ(first, again);
    EXPECT_EQ((U64)1, cache.getHitCount());

    PXR_NS::UsdStageRefPtr other = cache.getStage(b);
    ASSERT_TRUE(bool(other));
    EXPECT_NE(first, other);
    EXPECT_EQ((U64)2, cache.getMissCount());
    EXPECT_EQ((std::size_t)2, cache.getNumEntries());

    int value = 0;
    PXR_NS::UsdPrim prim = other->GetPrimAtPath(PXR_NS::SdfPath(kSceneTestPrimPath));
    ASSERT_TRUE(prim.IsValid());
    EXPECT_TRUE(prim.GetAttribute(PXR_NS::TfToken(kSceneTestSourceAttribute)).Get(&value));
    EXPECT_EQ(2, value);
}

TEST(StageCacheTest, EvictsTheLeastRecentlyUsedStack)
{
    StageCache cache(2);
    const ScenePayload a = makePayload(1);
    const ScenePayload b = makePayload(2);
    const ScenePayload c = makePayload(3);

    PXR_NS::UsdStageRefPtr stageA = cache.getStage(a);
    cache.getStage(b);
    // Touching a makes b the least recently used.
    EXPECT_EQ(stageA, cache.getStage(a));
    cache.getStage(c);
    EXPECT_EQ((std::size_t)2, cache.getNumEntries());

    const U64 missesBefore = cache.getMissCount();
    EXPECT_EQ(stageA, cache.getStage(a));
    EXPECT_EQ(missesBefore, cache.getMissCount());
    cache.getStage(b);
    EXPECT_EQ(missesBefore + 1, cache.getMissCount());

    cache.clear();
    EXPECT_EQ((std::size_t)0, cache.getNumEntries());
}

TEST(StageCacheTest, EmptyPayloadGivesNoStage)
{
    StageCache cache(2);

    EXPECT_FALSE(bool(cache.getStage(ScenePayload())));
}

// Source -> Op -> Op, evaluated through the scene action: a knob change re-authors the changed
// node's layer and those downstream of it, never the ones upstream.
class SceneLayerChainTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();

        _source = createNode(QString::fromUtf8(kTestPluginIDSceneSource));
        _middle = createNode(QString::fromUtf8(kTestPluginIDSceneOp));
        _last = createNode(QString::fromUtf8(kTestPluginIDSceneOp));
        ASSERT_TRUE(_source != NULL);
        ASSERT_TRUE(_middle != NULL);
        ASSERT_TRUE(_last != NULL);
        connectNodes(_source, _middle, 0, true);
        connectNodes(_middle, _last, 0, true);
    }

    virtual void TearDown() OVERRIDE
    {
        NodePtr nodes[] = { _last, _middle, _source };

        for (NodePtr& node : nodes) {
            if (node) {
                node->destroyNode(false, false);
            }
        }
        _last.reset();
        _middle.reset();
        _source.reset();

        BaseTest::TearDown();
    }

    EffectInstance::RenderRoIRetCode evaluate(ScenePayload* payload, std::string* error)
    {
        return _last->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), payload, error);
    }

    static U64 authorCount(const NodePtr& node)
    {
        if (SceneTestSource* source = dynamic_cast<SceneTestSource*>(node->getEffectInstance().get())) {
            return source->getLayerAuthorCount();
        }
        SceneTestOp* op = dynamic_cast<SceneTestOp*>(node->getEffectInstance().get());

        return op ? op->getLayerAuthorCount() : 0;
    }

    static void setValue(const NodePtr& node, int value)
    {
        KnobInt* knob = dynamic_cast<KnobInt*>(node->getKnobByName("value").get());

        ASSERT_TRUE(knob != NULL);
        knob->setValue(value);
    }

    NodePtr _source;
    NodePtr _middle;
    NodePtr _last;
};

TEST_F(SceneLayerChainTest, ReEvaluationAuthorsNothing)
{
    ScenePayload first, second;
    std::string error;

    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(&first, &error)) << error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(&second, &error)) << error;

    EXPECT_EQ((std::size_t)3, first.getNumLayers());
    EXPECT_TRUE(first.isSameStack(second));
    EXPECT_EQ((U64)1, authorCount(_source));
    EXPECT_EQ((U64)1, authorCount(_middle));
    EXPECT_EQ((U64)1, authorCount(_last));
}

TEST_F(SceneLayerChainTest, ChangingTheLastNodeReAuthorsOnlyItsLayer)
{
    ScenePayload before, after;
    std::string error;

    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(&before, &error)) << error;
    setValue(_last, 7);
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(&after, &error)) << error;

    EXPECT_EQ((U64)1, authorCount(_source));
    EXPECT_EQ((U64)1, authorCount(_middle));
    EXPECT_EQ((U64)2, authorCount(_last));

    const std::vector<SceneLayerEntry> beforeLayers = SceneOps::getLayers(before);
    const std::vector<SceneLayerEntry> afterLayers = SceneOps::getLayers(after);
    ASSERT_EQ((std::size_t)3, afterLayers.size());
    EXPECT_EQ(beforeLayers[0].layer, afterLayers[0].layer);
    EXPECT_EQ(beforeLayers[1].layer, afterLayers[1].layer);
    EXPECT_NE(beforeLayers[2].layer, afterLayers[2].layer);

    PXR_NS::UsdStageRefPtr stage = SceneOps::compose(after);
    ASSERT_TRUE(bool(stage));
    int value = 0;
    EXPECT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath(kSceneTestPrimPath)).GetAttribute(PXR_NS::TfToken(kSceneTestOpAttribute)).Get(&value));
    EXPECT_EQ(7, value);
}

// The memo key holds the upstream stack's hash, so a change re-authors the changed node and the
// nodes downstream of it; the layers upstream of it stay the same objects.
TEST_F(SceneLayerChainTest, ChangingAMiddleNodeLeavesUpstreamUntouched)
{
    ScenePayload before, after;
    std::string error;

    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(&before, &error)) << error;
    setValue(_middle, 3);
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(&after, &error)) << error;

    EXPECT_EQ((U64)1, authorCount(_source));
    EXPECT_EQ((U64)2, authorCount(_middle));
    EXPECT_EQ((U64)2, authorCount(_last));

    const std::vector<SceneLayerEntry> beforeLayers = SceneOps::getLayers(before);
    const std::vector<SceneLayerEntry> afterLayers = SceneOps::getLayers(after);
    ASSERT_EQ((std::size_t)3, afterLayers.size());
    EXPECT_EQ(beforeLayers[0].layer, afterLayers[0].layer);
    EXPECT_NE(beforeLayers[1].layer, afterLayers[1].layer);
}
