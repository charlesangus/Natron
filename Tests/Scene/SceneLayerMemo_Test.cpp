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
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/Nodes/Scene/SceneWriteBarrier.h"
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

namespace {
int
sourceValue(const PXR_NS::UsdStageRefPtr& stage)
{
    int value = 0;
    PXR_NS::UsdPrim prim = stage ? stage->GetPrimAtPath(PXR_NS::SdfPath(kSceneTestPrimPath)) : PXR_NS::UsdPrim();

    if (!prim.IsValid() || !prim.GetAttribute(PXR_NS::TfToken(kSceneTestSourceAttribute)).Get(&value)) {
        return -1;
    }

    return value;
}
} // anonymous namespace

TEST(SceneQueryPoolTest, BorrowedStagesComposeThePayloadAndAreReused)
{
    SceneQueryPool pool(4);
    const ScenePayload a = makePayload(1);
    const ScenePayload b = makePayload(2);
    std::string error;
    const U64 opensBefore = SceneOps::getStageOpenCount();
    PXR_NS::UsdStage* stageA = nullptr;
    {
        SceneQueryPool::Lease lease = pool.borrow(a, &error);
        ASSERT_TRUE(lease.isValid()) << error;
        EXPECT_EQ(1, sourceValue(lease.getStage()));
        stageA = lease.getStage().operator->();
        EXPECT_EQ((std::size_t)0, pool.getNumIdle());
    }
    EXPECT_EQ((std::size_t)1, pool.getNumIdle());

    {
        // The idle stage is synced to the new stack rather than a new one opened.
        SceneQueryPool::Lease lease = pool.borrow(b, &error);
        ASSERT_TRUE(lease.isValid()) << error;
        EXPECT_EQ(stageA, lease.getStage().operator->());
        EXPECT_EQ(2, sourceValue(lease.getStage()));

        // A second borrower at the same time gets a stage of its own.
        SceneQueryPool::Lease other = pool.borrow(a, &error);
        ASSERT_TRUE(other.isValid()) << error;
        EXPECT_NE(lease.getStage(), other.getStage());
        EXPECT_EQ(1, sourceValue(other.getStage()));
    }
    EXPECT_EQ((std::size_t)2, pool.getNumIdle());
    EXPECT_EQ(opensBefore + 2, SceneOps::getStageOpenCount());

    {
        // The stage last synced to a stack is the one handed out for it again.
        SceneQueryPool::Lease lease = pool.borrow(b, &error);
        EXPECT_EQ(stageA, lease.getStage().operator->());
        EXPECT_EQ(2, sourceValue(lease.getStage()));
    }
    EXPECT_EQ(opensBefore + 2, SceneOps::getStageOpenCount());
}

TEST(SceneQueryPoolTest, KeepsAtMostItsCapacityAndReleasesIdleStages)
{
    SceneQueryPool pool(2);
    std::string error;
    {
        SceneQueryPool::Lease first = pool.borrow(makePayload(1), &error);
        SceneQueryPool::Lease second = pool.borrow(makePayload(2), &error);
        SceneQueryPool::Lease third = pool.borrow(makePayload(3), &error);
        ASSERT_TRUE(first.isValid() && second.isValid() && third.isValid()) << error;
        // Released early: the lease is invalid afterwards and gives nothing back twice.
        third.release();
        EXPECT_FALSE(third.isValid());
        EXPECT_EQ((std::size_t)1, pool.getNumIdle());
    }
    EXPECT_EQ((std::size_t)2, pool.getNumIdle());
    pool.releaseIdle();
    EXPECT_EQ((std::size_t)0, pool.getNumIdle());
}

TEST(SceneQueryPoolTest, EmptyPayloadGivesAnEmptyStage)
{
    SceneQueryPool pool(2);
    std::string error;
    SceneQueryPool::Lease lease = pool.borrow(ScenePayload(), &error);

    ASSERT_TRUE(lease.isValid()) << error;
    EXPECT_EQ(-1, sourceValue(lease.getStage()));
    EXPECT_TRUE(lease.getStage()->GetPseudoRoot().GetChildren().empty());
}

// A lease holds the write barrier's read side: a layer reload cannot start while one is out, and a
// thread holding one is refused rather than deadlocked.
TEST(SceneQueryPoolTest, ALeaseKeepsLayerWritesOut)
{
    SceneQueryPool pool(2);
    std::string error;
    SceneQueryPool::Lease lease = pool.borrow(makePayload(1), &error);

    ASSERT_TRUE(lease.isValid()) << error;
    EXPECT_TRUE(SceneWriteBarrier::isReadingOnThisThread());
    bool wrote = false;
    std::string writeError;
    EXPECT_FALSE(SceneWriteBarrier::runExclusive([&wrote]() { wrote = true; }, &writeError));
    EXPECT_FALSE(wrote);
    EXPECT_FALSE(writeError.empty());

    lease.release();
    EXPECT_FALSE(SceneWriteBarrier::isReadingOnThisThread());
    EXPECT_TRUE(SceneWriteBarrier::runExclusive([&wrote]() { wrote = true; }, &writeError)) << writeError;
    EXPECT_TRUE(wrote);
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

// Source -> 30 Ops. Each op's Upstream Read knob picks what its layer key covers.
class SceneLongChainTest
    : public BaseTest {
protected:
    static const int kChainLength = 30;

    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();

        _source = createNode(QString::fromUtf8(kTestPluginIDSceneSource));
        ASSERT_TRUE(_source != NULL);
        NodePtr previous = _source;
        for (int i = 0; i < kChainLength; ++i) {
            NodePtr op = createNode(QString::fromUtf8(kTestPluginIDSceneOp));
            ASSERT_TRUE(op != NULL);
            connectNodes(previous, op, 0, true);
            _ops.push_back(op);
            previous = op;
        }
    }

    virtual void TearDown() OVERRIDE
    {
        for (std::vector<NodePtr>::reverse_iterator it = _ops.rbegin(); it != _ops.rend(); ++it) {
            (*it)->destroyNode(false, false);
        }
        _ops.clear();
        if (_source) {
            _source->destroyNode(false, false);
        }
        _source.reset();
        SceneQueryPool::instance().releaseIdle();

        BaseTest::TearDown();
    }

    EffectInstance::RenderRoIRetCode evaluate(ScenePayload* payload, std::string* error)
    {
        return _ops.back()->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), payload, error);
    }

    static U64 authorCount(const NodePtr& node)
    {
        if (SceneTestSource* source = dynamic_cast<SceneTestSource*>(node->getEffectInstance().get())) {
            return source->getLayerAuthorCount();
        }
        SceneTestOp* op = dynamic_cast<SceneTestOp*>(node->getEffectInstance().get());

        return op ? op->getLayerAuthorCount() : 0;
    }

    U64 totalAuthorCount() const
    {
        U64 total = authorCount(_source);

        for (const NodePtr& op : _ops) {
            total += authorCount(op);
        }

        return total;
    }

    static void setInt(const NodePtr& node, const char* name, int value)
    {
        KnobInt* knob = dynamic_cast<KnobInt*>(node->getKnobByName(name).get());

        ASSERT_TRUE(knob != NULL) << name;
        knob->setValue(value);
    }

    void setUpstreamRead(int mode)
    {
        for (const NodePtr& op : _ops) {
            setInt(op, "upstreamRead", mode);
        }
    }

    NodePtr _source;
    std::vector<NodePtr> _ops;
};

// A consumer following the chain, like the viewer or a node querying its upstream, keeps its
// stage: edits at the head sync it in place rather than opening a stage per edit, and the work
// per edit grows with the chain only through the layers that edit re-authors.
TEST_F(SceneLongChainTest, HeadEditsSyncOneStageWithoutOpeningAnother)
{
    ScenePayload payload;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(&payload, &error)) << error;
    EXPECT_EQ((std::size_t)(kChainLength + 1), payload.getNumLayers());

    SceneStage viewer;
    viewer.sync(payload);
    SceneQueryPool pool(1);
    {
        SceneQueryPool::Lease lease = pool.borrow(payload, &error);
        ASSERT_TRUE(lease.isValid()) << error;
    }
    const U64 opens = SceneOps::getStageOpenCount();

    for (int edit = 1; edit <= 5; ++edit) {
        const U64 authoredBefore = totalAuthorCount();
        setInt(_source, "value", edit);
        ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(&payload, &error)) << error;
        // Keyed on the whole stack: the head and every op below it, once each.
        EXPECT_EQ(authoredBefore + kChainLength + 1, totalAuthorCount()) << "edit " << edit;

        EXPECT_TRUE(viewer.sync(payload));
        SceneQueryPool::Lease lease = pool.borrow(payload, &error);
        ASSERT_TRUE(lease.isValid()) << error;
        int value = 0;
        PXR_NS::UsdPrim prim = viewer.getStage()->GetPrimAtPath(PXR_NS::SdfPath(kSceneTestPrimPath));
        EXPECT_TRUE(prim.GetAttribute(PXR_NS::TfToken(kSceneTestSourceAttribute)).Get(&value));
        EXPECT_EQ(edit, value);
        prim = lease.getStage()->GetPrimAtPath(PXR_NS::SdfPath(kSceneTestPrimPath));
        EXPECT_TRUE(prim.GetAttribute(PXR_NS::TfToken(kSceneTestSourceAttribute)).Get(&value));
        EXPECT_EQ(edit, value);
    }
    EXPECT_EQ(opens, SceneOps::getStageOpenCount());
}

// Ops that read nothing upstream: an edit at the head re-authors the head's layer alone, and the
// ops' memoized layers are re-stacked on the new stack, which still composes the edit.
TEST_F(SceneLongChainTest, AHeadEditReAuthorsOneLayerWhenNothingDownstreamReadsIt)
{
    setUpstreamRead(1);
    ScenePayload before, after;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(&before, &error)) << error;
    const U64 authoredBefore = totalAuthorCount();
    EXPECT_EQ((U64)(kChainLength + 1), authoredBefore);

    setInt(_source, "value", 42);
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(&after, &error)) << error;
    EXPECT_EQ(authoredBefore + 1, totalAuthorCount());
    EXPECT_EQ((U64)2, authorCount(_source));
    EXPECT_FALSE(before.isSameStack(after));

    const std::vector<SceneLayerEntry> beforeLayers = SceneOps::getLayers(before);
    const std::vector<SceneLayerEntry> afterLayers = SceneOps::getLayers(after);
    ASSERT_EQ(beforeLayers.size(), afterLayers.size());
    EXPECT_NE(beforeLayers[0].layer, afterLayers[0].layer);
    for (std::size_t i = 1; i < afterLayers.size(); ++i) {
        EXPECT_EQ(beforeLayers[i].layer, afterLayers[i].layer) << "layer " << i;
    }

    SceneStage stage;
    stage.sync(after);
    int value = 0;
    EXPECT_TRUE(stage.getStage()->GetPrimAtPath(PXR_NS::SdfPath(kSceneTestPrimPath)).GetAttribute(PXR_NS::TfToken(kSceneTestSourceAttribute)).Get(&value));
    EXPECT_EQ(42, value);
}

// An op reading one upstream fact (the source value, through SceneQueryPool) re-authors when that
// fact changes and not when an upstream edit leaves it as it was.
TEST_F(SceneLongChainTest, AFactsReadingOpReAuthorsWhenItsFactChanges)
{
    setUpstreamRead(1);
    const NodePtr reader = _ops.back();
    setInt(reader, "upstreamRead", 2);
    ScenePayload payload;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(&payload, &error)) << error;
    EXPECT_EQ((U64)1, authorCount(reader));

    // An op above it changes; the source value it reads does not.
    setInt(_ops[3], "value", 9);
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(&payload, &error)) << error;
    EXPECT_EQ((U64)2, authorCount(_ops[3]));
    EXPECT_EQ((U64)1, authorCount(reader));

    // The source value changes.
    setInt(_source, "value", 5);
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(&payload, &error)) << error;
    EXPECT_EQ((U64)2, authorCount(reader));
    PXR_NS::UsdStageRefPtr stage = SceneOps::compose(payload);
    ASSERT_TRUE(bool(stage));
    int read = 0;
    EXPECT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath(kSceneTestPrimPath)).GetAttribute(PXR_NS::TfToken(kSceneTestFactAttribute)).Get(&read));
    EXPECT_EQ(5, read);
}
