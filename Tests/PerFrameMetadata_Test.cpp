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

#include <atomic>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <QString>

#include <gtest/gtest.h>

#include <ofxImageEffect.h>

#include "BaseTest.h"
#include "NativeMetadataTestEffect.h"
#include "OfxTestPlugins/natronTest/NatronTestPluginIDs.h"
#include "RenderBothWays.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Merge/Dissolve.h"
#include "Engine/Nodes/Merge/Merge.h"
#include "Engine/Nodes/Merge/Switch.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

MetadataSourceTestEffect*
metadataSourceOf(const NodePtr& node)
{
    return dynamic_cast<MetadataSourceTestEffect*>(node->getEffectInstance().get());
}

// The channel count the output stream carries, as the timeless getters report it.
int
outputNComps(const NodeMetadataConstPtr& metadata)
{
    if ((metadata->getOutputStorageNComps() > 0) && (metadata->getComponentsType(-1) == kNatronColorLayerID)) {
        return metadata->getOutputStorageNComps();
    }

    return metadata->getNComps(-1);
}

int
outputNCompsAt(const NodePtr& node,
               double time)
{
    NodeMetadataConstPtr metadata = node->getEffectInstance()->getMetadata(time, ViewIdx(0));
    EXPECT_TRUE(bool(metadata)) << node->getScriptName() << " time=" << time;

    return metadata ? outputNComps(metadata) : -1;
}

} // namespace

class PerFrameMetadataTest
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

    NodePtr createSource(int nComps)
    {
        NodePtr source = createNode(QString::fromUtf8(kTestPluginIDMetadataSource));
        EXPECT_TRUE(bool(source));
        if (!source) {
            return source;
        }
        MetadataSourceTestEffect* effect = metadataSourceOf(source);
        EXPECT_TRUE(effect != NULL);
        if (effect) {
            effect->getNCompsKnob()->setValue(nComps, ViewSpec::all(), 0);
        }

        return source;
    }

    NodePtr createBlurBelow(const NodePtr& input)
    {
        NodePtr blur = createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR));
        EXPECT_TRUE(bool(blur));
        if (!blur) {
            return blur;
        }
        connectNodes(input, blur, 0, true);
        KnobDoublePtr size = std::dynamic_pointer_cast<KnobDouble>(blur->getKnobByName(kBlurParamSize));
        EXPECT_TRUE(bool(size));
        if (size) {
            size->setValue(3., ViewSpec::all(), 0);
            size->setValue(3., ViewSpec::all(), 1);
        }

        return blur;
    }

    // An RGB source on input 0 and an RGBA one on input 1, routed by `which` keyed 0 at frame 1
    // and 1 at frame 2.
    NodePtr createKeyedSwitch(NodePtr* sourceA,
                              NodePtr* sourceB)
    {
        *sourceA = createSource(3);
        *sourceB = createSource(4);
        NodePtr switchNode = createNode(QString::fromUtf8(PLUGINID_NATRON_SWITCH));
        EXPECT_TRUE(*sourceA && *sourceB && switchNode);
        if (!*sourceA || !*sourceB || !switchNode) {
            return NodePtr();
        }
        connectNodes(*sourceA, switchNode, 0, true);
        connectNodes(*sourceB, switchNode, 1, true);
        KnobIntPtr which = std::dynamic_pointer_cast<KnobInt>(switchNode->getKnobByName(kSwitchParamWhich));
        EXPECT_TRUE(bool(which));
        if (!which) {
            return NodePtr();
        }
        which->setValueAtTime(1, 0, ViewSpec::all(), 0);
        which->setValueAtTime(2, 1, ViewSpec::all(), 0);

        return switchNode;
    }
};

TEST_F(PerFrameMetadataTest, KeyedSwitchReportsRoutedInputPerFrame)
{
    NodePtr sourceA, sourceB;
    NodePtr switchNode = createKeyedSwitch(&sourceA, &sourceB);
    ASSERT_TRUE(bool(switchNode));
    NodePtr blur = createBlurBelow(switchNode);
    ASSERT_TRUE(bool(blur));

    EXPECT_EQ(3, outputNCompsAt(switchNode, 1));
    EXPECT_EQ(4, outputNCompsAt(switchNode, 2));
    EXPECT_EQ(3, outputNCompsAt(blur, 1));
    EXPECT_EQ(4, outputNCompsAt(blur, 2));
}

TEST_F(PerFrameMetadataTest, IntegerDissolveReportsRoutedInputPerFrame)
{
    NodePtr sourceA = createSource(3);
    NodePtr sourceB = createSource(4);
    NodePtr dissolve = createNode(QString::fromUtf8(PLUGINID_NATRON_DISSOLVE));
    ASSERT_TRUE(sourceA && sourceB && dissolve);
    connectNodes(sourceA, dissolve, dissolveSourceInput(0), true);
    connectNodes(sourceB, dissolve, dissolveSourceInput(1), true);
    KnobDoublePtr which = std::dynamic_pointer_cast<KnobDouble>(dissolve->getKnobByName(kDissolveParamWhich));
    ASSERT_TRUE(bool(which));
    which->setValueAtTime(1, 0., ViewSpec::all(), 0);
    which->setValueAtTime(2, 1., ViewSpec::all(), 0);
    NodePtr blur = createBlurBelow(dissolve);
    ASSERT_TRUE(bool(blur));

    EXPECT_EQ(3, outputNCompsAt(dissolve, 1));
    EXPECT_EQ(4, outputNCompsAt(dissolve, 2));
    EXPECT_EQ(3, outputNCompsAt(blur, 1));
    EXPECT_EQ(4, outputNCompsAt(blur, 2));
}

TEST_F(PerFrameMetadataTest, KeyedDisablePassesInputThroughPerFrame)
{
    NodePtr sourceA = createSource(3);
    NodePtr sourceB = createSource(4);
    NodePtr switchNode = createNode(QString::fromUtf8(PLUGINID_NATRON_SWITCH));
    ASSERT_TRUE(sourceA && sourceB && switchNode);
    connectNodes(sourceA, switchNode, 0, true);
    connectNodes(sourceB, switchNode, 1, true);

    // A disabled node shows its preferred input; route the other one while enabled, so the two
    // frames differ.
    const int preferred = switchNode->getPreferredInput();
    ASSERT_TRUE((preferred == 0) || (preferred == 1));
    const int routed = 1 - preferred;
    KnobIntPtr which = std::dynamic_pointer_cast<KnobInt>(switchNode->getKnobByName(kSwitchParamWhich));
    ASSERT_TRUE(bool(which));
    which->setValue(routed, ViewSpec::all(), 0);

    KnobBoolPtr disable = std::dynamic_pointer_cast<KnobBool>(switchNode->getKnobByName(kDisableNodeKnobName));
    ASSERT_TRUE(bool(disable));
    disable->setValueAtTime(1, false, ViewSpec::all(), 0);
    disable->setValueAtTime(2, true, ViewSpec::all(), 0);

    const int nCompsOf[] = { 3, 4 };
    EXPECT_EQ(nCompsOf[routed], outputNCompsAt(switchNode, 1));
    EXPECT_EQ(nCompsOf[preferred], outputNCompsAt(switchNode, 2));
}

TEST_F(PerFrameMetadataTest, HeldFrameReportsHeldFrameMetadata)
{
    NodePtr source = createSource(4);
    ASSERT_TRUE(bool(source));
    KnobIntPtr nComps = metadataSourceOf(source)->getNCompsKnob();
    nComps->setValueAtTime(1, 3, ViewSpec::all(), 0);
    nComps->setValueAtTime(2, 4, ViewSpec::all(), 0);

    NodePtr hold = createNode(QString::fromUtf8(kNatronTestFrameWindowPluginID));
    ASSERT_TRUE(bool(hold));
    connectNodes(source, hold, 0, true);
    KnobIntPtr frameRange = std::dynamic_pointer_cast<KnobInt>(hold->getKnobByName(kNatronTestFrameWindowParamFrameRange));
    KnobBoolPtr absolute = std::dynamic_pointer_cast<KnobBool>(hold->getKnobByName(kNatronTestFrameWindowParamAbsolute));
    ASSERT_TRUE(frameRange && absolute);
    absolute->setValue(true);
    frameRange->setValue(1, ViewSpec::all(), 0);
    frameRange->setValue(1, ViewSpec::all(), 1);

    EXPECT_EQ(3, outputNCompsAt(source, 1));
    EXPECT_EQ(4, outputNCompsAt(source, 2));
    EXPECT_EQ(3, outputNCompsAt(hold, 1));
    EXPECT_EQ(3, outputNCompsAt(hold, 2));
}

TEST_F(PerFrameMetadataTest, SwitchTimeInvariantMetadataIsTheWidest)
{
    NodePtr sourceA, sourceB;
    NodePtr switchNode = createKeyedSwitch(&sourceA, &sourceB);
    ASSERT_TRUE(bool(switchNode));

    NodeMetadataConstPtr timeInvariant = switchNode->getEffectInstance()->getTimeInvariantMetadata();
    ASSERT_TRUE(bool(timeInvariant));
    EXPECT_EQ(4, outputNComps(timeInvariant));
    EXPECT_EQ(3, outputNCompsAt(switchNode, 1));
}

TEST_F(PerFrameMetadataTest, SecondQueryIsServedFromTheCache)
{
    NodePtr sourceA, sourceB;
    NodePtr switchNode = createKeyedSwitch(&sourceA, &sourceB);
    ASSERT_TRUE(bool(switchNode));
    MetadataSourceTestEffect* effectA = metadataSourceOf(sourceA);
    ASSERT_TRUE(effectA != NULL);

    NodeMetadataConstPtr first = switchNode->getEffectInstance()->getMetadata(1, ViewIdx(0));
    ASSERT_TRUE(bool(first));
    const int derivations = effectA->nodeMetadataDerivationCount();
    EXPECT_GT(derivations, 0);

    NodeMetadataConstPtr second = switchNode->getEffectInstance()->getMetadata(1, ViewIdx(0));
    NodeMetadataConstPtr sourceSecond = sourceA->getEffectInstance()->getMetadata(1, ViewIdx(0));
    EXPECT_EQ(first.get(), second.get());
    EXPECT_TRUE(bool(sourceSecond));
    EXPECT_EQ(derivations, effectA->nodeMetadataDerivationCount());
}

// The frames a node keeps are bounded, and the one asked about most recently stays: a frame
// read between every new one is never derived again, while the oldest is, with the same answer.
TEST_F(PerFrameMetadataTest, CachedFramesEvictTheLeastRecentlyUsed)
{
    NodePtr source = createSource(3);
    ASSERT_TRUE(bool(source));
    MetadataSourceTestEffect* effect = metadataSourceOf(source);
    ASSERT_TRUE(effect != NULL);
    EffectInstancePtr instance = source->getEffectInstance();

    NodeMetadataConstPtr held = instance->getMetadata(1, ViewIdx(0));
    NodeMetadataConstPtr oldest = instance->getMetadata(2, ViewIdx(0));
    ASSERT_TRUE(held && oldest);
    const int derivations = effect->nodeMetadataDerivationCount();

    const int nFrames = 300;
    for (int frame = 3; frame < 3 + nFrames; ++frame) {
        ASSERT_TRUE(bool(instance->getMetadata(frame, ViewIdx(0))));
        EXPECT_EQ(held.get(), instance->getMetadata(1, ViewIdx(0)).get()) << "frame=" << frame;
    }
    EXPECT_EQ(derivations + nFrames, effect->nodeMetadataDerivationCount());

    NodeMetadataConstPtr rederived = instance->getMetadata(2, ViewIdx(0));
    ASSERT_TRUE(bool(rederived));
    EXPECT_EQ(derivations + nFrames + 1, effect->nodeMetadataDerivationCount());
    EXPECT_TRUE(*rederived == *oldest);
    EXPECT_EQ(3, outputNComps(rederived));
    EXPECT_EQ(held.get(), instance->getMetadata(1, ViewIdx(0)).get());
}

// At a frame nothing asked about before the counts are taken, so the first pass derives it.
TEST_F(PerFrameMetadataTest, RepeatedQueriesAtOneFrameEvaluateEachNodeOnce)
{
    const double time = 7.;
    NodePtr source = createSource(4);
    ASSERT_TRUE(bool(source));
    NodePtr filter = createNode(QString::fromUtf8(kTestPluginIDMetadataFilter));
    ASSERT_TRUE(bool(filter));
    connectNodes(source, filter, 0, true);
    NodePtr blur = createBlurBelow(filter);
    ASSERT_TRUE(bool(blur));
    NodePtr blur2 = createBlurBelow(blur);
    ASSERT_TRUE(bool(blur2));
    MetadataSourceTestEffect* sourceEffect = metadataSourceOf(source);
    ASSERT_TRUE(sourceEffect != NULL);
    MetadataFilterTestEffect* filterEffect = dynamic_cast<MetadataFilterTestEffect*>(filter->getEffectInstance().get());
    ASSERT_TRUE(filterEffect != NULL);

    const int derivationsBefore = sourceEffect->nodeMetadataDerivationCount();
    const int identityBefore = sourceEffect->identityEvaluationCount();
    const int filterIdentityBefore = filterEffect->identityEvaluationCount();
    int filterIdentityAfterFirstPass = filterIdentityBefore;
    const NodePtr nodes[] = { source, filter, blur, blur2 };
    std::map<const EffectInstance*, EffectInstance::ComponentsNeededResultsPtr> firstResults;
    std::map<const EffectInstance*, std::shared_ptr<const std::list<ImageLayerDesc>>> firstPresent;
    for (int pass = 0; pass < 5; ++pass) {
        for (const NodePtr& node : nodes) {
            EffectInstancePtr effect = node->getEffectInstance();
            ASSERT_TRUE(bool(effect->getMetadata(time, ViewIdx(0))));
            EffectInstance::ComponentsNeededResultsPtr results = effect->getComponentsNeededAndProducedShared(node->getHashValue(), time, ViewIdx(0));
            std::shared_ptr<const std::list<ImageLayerDesc>> present = effect->getPresentLayersShared(time, ViewIdx(0), -1);
            ASSERT_TRUE(results && present);
            std::vector<ResolvedLayer> selected;
            (void)node->resolveLayerKnob(time, ViewIdx(0), &selected);
            (void)node->hasAtLeastOneChannelToProcess(time, ViewIdx(0));
            int passThroughInputNb = -1;
            double passThroughTime = 0.;
            ViewIdx passThroughView(0);
            effect->getLayersPassThroughInput(time, ViewIdx(0), &passThroughInputNb, &passThroughTime, &passThroughView);
            if (pass == 0) {
                firstResults[effect.get()] = results;
                firstPresent[effect.get()] = present;
            } else {
                // A hit hands out the stored answer rather than a copy of it.
                EXPECT_EQ(firstResults[effect.get()].get(), results.get());
                EXPECT_EQ(firstPresent[effect.get()].get(), present.get());
            }
        }
        if (pass == 0) {
            filterIdentityAfterFirstPass = filterEffect->identityEvaluationCount();
        }
    }

    EXPECT_EQ(derivationsBefore + 1, sourceEffect->nodeMetadataDerivationCount());
    EXPECT_EQ(identityBefore + 1, sourceEffect->identityEvaluationCount());
    // One for the identity rule its metadata went through, one for the choice of the input its
    // layers pass through from; the repeated choices are served from the cache.
    EXPECT_EQ(filterIdentityBefore + 2, filterIdentityAfterFirstPass);
    EXPECT_EQ(filterIdentityAfterFirstPass, filterEffect->identityEvaluationCount());
}

// Readers on many threads race a knob toggled on the main thread. Each hash stands for one state,
// so whatever was cached under a hash must equal what that state derives from scratch.
TEST_F(PerFrameMetadataTest, ConcurrentQueriesCacheOnlyConsistentAnswers)
{
    const int kFrames = 8;
    const int kThreads = 16;
    const int kToggles = 1000;

    NodePtr sourceA = createSource(3);
    NodePtr sourceB = createSource(4);
    NodePtr switchNode = createNode(QString::fromUtf8(PLUGINID_NATRON_SWITCH));
    ASSERT_TRUE(sourceA && sourceB && switchNode);
    connectNodes(sourceA, switchNode, 0, true);
    connectNodes(sourceB, switchNode, 1, true);
    KnobIntPtr which = std::dynamic_pointer_cast<KnobInt>(switchNode->getKnobByName(kSwitchParamWhich));
    ASSERT_TRUE(bool(which));
    for (int frame = 1; frame <= kFrames; ++frame) {
        which->setValueAtTime(frame, frame % 2, ViewSpec::all(), 0);
    }
    NodePtr blur = createBlurBelow(switchNode);
    ASSERT_TRUE(bool(blur));
    KnobIntPtr toggled = metadataSourceOf(sourceB)->getNCompsKnob();

    const std::vector<NodePtr> nodes = { sourceA, sourceB, switchNode, blur };
    const std::vector<NodePtr> queried = { switchNode, blur };

    std::atomic<bool> stop(false);
    std::atomic<int> nullAnswers(0);
    std::vector<std::thread> readers;
    for (int t = 0; t < kThreads; ++t) {
        readers.emplace_back([&, t]() {
            std::mt19937 rng(t + 1);
            std::uniform_int_distribution<int> pickFrame(1, kFrames);
            while (!stop.load()) {
                const double frame = pickFrame(rng);
                for (const NodePtr& node : queried) {
                    if (!node->getEffectInstance()->getMetadata(frame, ViewIdx(0))) {
                        ++nullAnswers;
                    }
                }
            }
        });
    }
    for (int i = 0; i < kToggles; ++i) {
        toggled->setValue((i % 2) ? 4 : 3, ViewSpec::all(), 0);
    }
    stop.store(true);
    for (std::thread& reader : readers) {
        reader.join();
    }
    EXPECT_EQ(0, nullAnswers.load());

    // Revisiting a state returns to its hash, and so to what the readers cached under it.
    const int states[] = { 3, 4 };
    std::vector<std::vector<NodeMetadataConstPtr>> cached;
    for (int state : states) {
        toggled->setValue(state, ViewSpec::all(), 0);
        cached.emplace_back();
        for (int frame = 1; frame <= kFrames; ++frame) {
            for (const NodePtr& node : queried) {
                cached.back().push_back(node->getEffectInstance()->getMetadata(frame, ViewIdx(0)));
            }
        }
    }
    for (const NodePtr& node : nodes) {
        node->getEffectInstance()->dropCachedMetadata();
    }
    for (std::size_t s = 0; s < cached.size(); ++s) {
        toggled->setValue(states[s], ViewSpec::all(), 0);
        std::size_t i = 0;
        for (int frame = 1; frame <= kFrames; ++frame) {
            for (const NodePtr& node : queried) {
                NodeMetadataConstPtr fresh = node->getEffectInstance()->getMetadata(frame, ViewIdx(0));
                ASSERT_TRUE(cached[s][i] && fresh);
                EXPECT_TRUE(*cached[s][i] == *fresh) << node->getScriptName() << " state=" << states[s] << " frame=" << frame;
                ++i;
            }
        }
        for (const NodePtr& node : nodes) {
            node->getEffectInstance()->dropCachedMetadata();
        }
    }
}

// An edit landing while a frame is set up, after an upstream node was snapshotted but before the
// nodes reading it were, must not leave the frame with answers from two states of the graph.
TEST_F(PerFrameMetadataTest, FrameSnapshotHashesMatchTheArgs)
{
    NodePtr sourceA, sourceB;
    NodePtr switchNode = createKeyedSwitch(&sourceA, &sourceB);
    ASSERT_TRUE(bool(switchNode));
    NodePtr blur = createBlurBelow(switchNode);
    ASSERT_TRUE(bool(blur));
    KnobIntPtr nComps = metadataSourceOf(sourceB)->getNCompsKnob();

    int edits = 0;
    ParallelRenderArgsSetter::setMetadataSnapshotHookForTests([&](const NodePtr& node) {
        if ((node == sourceB) && (edits == 0)) {
            ++edits;
            nComps->setValue(3, ViewSpec::all(), 0);
        }
    });
    std::map<NodePtr, ParallelRenderArgsPtr> args;
    ParallelRenderArgsSetter::buildArgsMap(2, ViewIdx(0), false, false, AbortableRenderInfo::create(false, 0), blur, 0,
                                           getApp()->getTimeLine().get(), NodePtr(), false, false, RenderStatsPtr(),
                                           true, OSGLContextPtr(), &args);
    ParallelRenderArgsSetter::setMetadataSnapshotHookForTests(ParallelRenderArgsSetter::MetadataSnapshotHook());
    EXPECT_EQ(1, edits);

    const std::vector<NodePtr> nodes = { sourceA, sourceB, switchNode, blur };
    for (const NodePtr& node : nodes) {
        std::map<NodePtr, ParallelRenderArgsPtr>::const_iterator found = args.find(node);
        ASSERT_TRUE(found != args.end()) << node->getScriptName();
        ASSERT_TRUE(bool(found->second->metadata)) << node->getScriptName();
        EXPECT_EQ(node->getHashValue(), found->second->nodeHash) << node->getScriptName();
        U64 hash = 0;
        NodeMetadataConstPtr live = node->getEffectInstance()->getMetadata(2, ViewIdx(0), &hash);
        ASSERT_TRUE(bool(live));
        EXPECT_EQ(found->second->nodeHash, hash) << node->getScriptName();
        EXPECT_TRUE(*live == *found->second->metadata) << node->getScriptName();
    }
    // Frame 2 routes the edited source.
    EXPECT_EQ(3, outputNComps(args[blur]->metadata));
}

// An edit landing while the answer is derived: the hash given is the one the answer returned
// belongs to, here the edited state's, never the hash read before the derivation.
TEST_F(PerFrameMetadataTest, HashGivenWithAnAnswerIsTheOneItBelongsTo)
{
    NodePtr source = createSource(4);
    ASSERT_TRUE(bool(source));
    MetadataSourceTestEffect* effect = metadataSourceOf(source);
    ASSERT_TRUE(effect != NULL);
    KnobIntPtr nComps = effect->getNCompsKnob();
    const U64 before = source->getHashValue();

    int edits = 0;
    effect->setDeriveHook([&]() {
        if (edits == 0) {
            ++edits;
            nComps->setValue(3, ViewSpec::all(), 0);
        }
    });
    U64 hash = 0;
    NodeMetadataConstPtr metadata = source->getEffectInstance()->getMetadata(5, ViewIdx(0), &hash);
    effect->setDeriveHook(std::function<void()>());

    EXPECT_EQ(1, edits);
    ASSERT_TRUE(bool(metadata));
    EXPECT_NE(before, source->getHashValue());
    EXPECT_EQ(source->getHashValue(), hash);
    EXPECT_EQ(3, outputNComps(metadata));
}

// A purge of the node's cache while an answer is derived: the answer may predate whatever the
// purge stood for, so it is not stored and the next query derives again.
TEST_F(PerFrameMetadataTest, AnswerDerivedAcrossACachePurgeIsNotStored)
{
    NodePtr source = createSource(4);
    ASSERT_TRUE(bool(source));
    MetadataSourceTestEffect* effect = metadataSourceOf(source);
    ASSERT_TRUE(effect != NULL);
    EffectInstancePtr instance = source->getEffectInstance();

    int purges = 0;
    effect->setDeriveHook([&]() {
        if (purges == 0) {
            ++purges;
            instance->clearActionsCache();
        }
    });
    const int derivations = effect->nodeMetadataDerivationCount();
    ASSERT_TRUE(bool(instance->getMetadata(5, ViewIdx(0))));
    effect->setDeriveHook(std::function<void()>());
    EXPECT_EQ(1, purges);
    EXPECT_EQ(derivations + 1, effect->nodeMetadataDerivationCount());

    ASSERT_TRUE(bool(instance->getMetadata(5, ViewIdx(0))));
    EXPECT_EQ(derivations + 2, effect->nodeMetadataDerivationCount());
    ASSERT_TRUE(bool(instance->getMetadata(5, ViewIdx(0))));
    EXPECT_EQ(derivations + 2, effect->nodeMetadataDerivationCount());
}

// A graph edited on every snapshot attempt: the frame is aborted rather than rendered from
// answers of states it was not set up with; a frame that cannot be aborted reads the nodes the
// edits reached live, keeping none of those answers.
TEST_F(PerFrameMetadataTest, SnapshotThatNeverSettlesAbortsTheFrame)
{
    NodePtr sourceA, sourceB;
    NodePtr switchNode = createKeyedSwitch(&sourceA, &sourceB);
    ASSERT_TRUE(bool(switchNode));
    NodePtr blur = createBlurBelow(switchNode);
    ASSERT_TRUE(bool(blur));
    KnobIntPtr nComps = metadataSourceOf(sourceB)->getNCompsKnob();

    ParallelRenderArgsSetter::setMetadataSnapshotHookForTests([&](const NodePtr& node) {
        if (node == sourceB) {
            nComps->setValue((nComps->getValue() == 3) ? 4 : 3, ViewSpec::all(), 0);
        }
    });
    for (int abortable = 0; abortable < 2; ++abortable) {
        SCOPED_TRACE(abortable);
        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(abortable != 0, 0);
        std::map<NodePtr, ParallelRenderArgsPtr> args;
        ParallelRenderArgsSetter::buildArgsMap(2, ViewIdx(0), false, false, abortInfo, blur, 0,
                                               getApp()->getTimeLine().get(), NodePtr(), false, false, RenderStatsPtr(),
                                               true, OSGLContextPtr(), &args);
        EXPECT_EQ(abortable != 0, abortInfo->isAborted());
        ASSERT_TRUE(args.find(blur) != args.end());
        EXPECT_FALSE(bool(args[blur]->metadata));
        EXPECT_FALSE(bool(args[switchNode]->metadata));
        // Upstream of the edits, the snapshot stands.
        ASSERT_TRUE(args.find(sourceA) != args.end());
        EXPECT_TRUE(bool(args[sourceA]->metadata));
    }
    ParallelRenderArgsSetter::setMetadataSnapshotHookForTests(ParallelRenderArgsSetter::MetadataSnapshotHook());
}

// A time other than the frame's is answered under the hash the frame was set up with, from its
// first read on: an edit made after that read does not reach the frame, and one made before it
// aborts the frame as the edit does.
TEST_F(PerFrameMetadataTest, OtherTimesReadTheFramesHashes)
{
    NodePtr source = createSource(4);
    ASSERT_TRUE(bool(source));
    NodePtr blur = createBlurBelow(source);
    ASSERT_TRUE(bool(blur));
    KnobIntPtr nComps = metadataSourceOf(source)->getNCompsKnob();
    EffectInstancePtr instance = source->getEffectInstance();

    AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(true, 0);
    std::map<NodePtr, ParallelRenderArgsPtr> args;
    ParallelRenderArgsSetter::buildArgsMap(2, ViewIdx(0), false, false, abortInfo, blur, 0,
                                           getApp()->getTimeLine().get(), NodePtr(), false, false, RenderStatsPtr(),
                                           true, OSGLContextPtr(), &args);
    ASSERT_TRUE(args.find(source) != args.end());
    const ParallelRenderArgsPtr frameArgs = args[source];
    ASSERT_TRUE(bool(frameArgs->metadata));
    EXPECT_FALSE(bool(instance->findRenderMetadata(frameArgs, 7, ViewIdx(0))));

    NodeMetadataConstPtr other = instance->getRenderMetadata(frameArgs, 1, ViewIdx(0));
    ASSERT_TRUE(bool(other));
    EXPECT_EQ(4, outputNComps(other));
    EXPECT_FALSE(abortInfo->isAborted());

    nComps->setValue(3, ViewSpec::all(), 0);
    EXPECT_EQ(other.get(), instance->getRenderMetadata(frameArgs, 1, ViewIdx(0)).get());
    EXPECT_EQ(other.get(), instance->findRenderMetadata(frameArgs, 1, ViewIdx(0)).get());
    EXPECT_EQ(4, outputNComps(instance->getRenderMetadata(frameArgs, 2, ViewIdx(0))));
    EXPECT_FALSE(abortInfo->isAborted());

    NodeMetadataConstPtr afterEdit = instance->getRenderMetadata(frameArgs, 3, ViewIdx(0));
    ASSERT_TRUE(bool(afterEdit));
    EXPECT_EQ(3, outputNComps(afterEdit));
    EXPECT_TRUE(abortInfo->isAborted());
}

// The render reads the frame's own layout: a Switch keyed from an RGB reader at frame 1 to an RGBA
// reader at frame 2 renders a 3-channel colour plane at frame 1 and a 4-channel one at frame 2,
// whichever frame renders first.
TEST_F(PerFrameMetadataTest, KeyedSwitchRendersEachFramesLayout)
{
    for (int firstFrame = 1; firstFrame <= 2; ++firstFrame) {
        SCOPED_TRACE("frame " + std::to_string(firstFrame) + " rendered first");
        getApp()->getProject()->reset(false, true);

        CreateNodeArgs rgbArgs(_readPluginID.toStdString(), getApp()->getProject());
        rgbArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/flat-rgb-only.exr"));
        NodePtr rgbReader = getApp()->createNode(rgbArgs);
        CreateNodeArgs rgbaArgs(_readPluginID.toStdString(), getApp()->getProject());
        rgbaArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/flat-rgba-only.exr"));
        NodePtr rgbaReader = getApp()->createNode(rgbaArgs);
        NodePtr switchNode = createNode(QString::fromUtf8(PLUGINID_NATRON_SWITCH));
        ASSERT_TRUE(rgbReader && rgbaReader && switchNode);
        connectNodes(rgbReader, switchNode, 0, true);
        connectNodes(rgbaReader, switchNode, 1, true);
        KnobIntPtr which = std::dynamic_pointer_cast<KnobInt>(switchNode->getKnobByName(kSwitchParamWhich));
        ASSERT_TRUE(bool(which));
        which->setValueAtTime(1, 0, ViewSpec::all(), 0);
        which->setValueAtTime(2, 1, ViewSpec::all(), 0);

        for (int i = 0; i < 2; ++i) {
            const int frame = (i == 0) ? firstFrame : 3 - firstFrame;
            std::vector<RenderedPlane> planes;
            std::string error;
            ASSERT_TRUE(renderNodePlanesInMode(switchNode, frame, ViewIdx(0), 0, RectI(0, 0, 8, 8), std::list<ImageLayerDesc>(),
                                               eRenderSchedulerModeTaskGraph, true, &planes, &error))
                << "frame=" << frame << " " << error;
            const RenderedPlane* color = NULL;
            for (std::size_t p = 0; p < planes.size(); ++p) {
                if (planes[p].layer.isColorLayer()) {
                    color = &planes[p];
                }
            }
            ASSERT_TRUE(color != NULL) << "frame=" << frame;
            EXPECT_EQ((frame == 1) ? 3u : 4u, color->channels.size()) << "frame=" << frame;
        }
    }
}

// The node graph's pixel aspect ratio warning follows the timeline: a Merge fed a PAR 1 source and
// a Switch keyed from a PAR 1 source at frame 1 to a PAR 2 one at frame 2 warns at frame 2 only,
// whichever way the frame moves.
TEST_F(PerFrameMetadataTest, PixelAspectRatioWarningFollowsTheFrame)
{
    NodePtr sourceA, sourceB;
    NodePtr switchNode = createKeyedSwitch(&sourceA, &sourceB);
    NodePtr other = createSource(4);
    NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_MERGE));
    ASSERT_TRUE(switchNode && other && merge);
    connectNodes(other, merge, kMergeInputB, true);
    connectNodes(switchNode, merge, kMergeInputA, true);
    // Set once connected: inputs whose ratios differ are refused a connection.
    KnobDoublePtr par = metadataSourceOf(sourceB)->getParKnob();
    ASSERT_TRUE(bool(par));
    par->setValue(2., ViewSpec::all(), 0);
    NodeMetadataConstPtr routed = switchNode->getEffectInstance()->getMetadata(2, ViewIdx(0));
    ASSERT_TRUE(bool(routed));
    ASSERT_EQ(2., routed->getPixelAspectRatio(-1));

    const int frames[] = { 1, 2, 1, 2 };
    for (int frame : frames) {
        getApp()->getTimeLine()->seekFrame(frame, false, NULL, eTimelineChangeReasonOtherSeek);
        merge->getEffectInstance()->refreshAfterTimeChange(false, frame);
        std::map<Node::StreamWarningEnum, QString> warnings;
        merge->getStreamWarnings(&warnings);
        EXPECT_EQ(frame == 2, !warnings[Node::eStreamWarningPixelAspectRatio].isEmpty()) << "frame=" << frame;
    }
}

namespace {

// Renders the red channel at `at` through renderRoI on this thread, after installing the frame's
// render args and request pass and then calling `beforeRender`. A failed or aborted render
// returns false.
bool
renderRedAt(const NodePtr& node,
            const RectI& roi,
            int x,
            int y,
            bool byPassCache,
            float* red,
            const std::function<void()>& beforeRender = std::function<void()>())
{
    const double time = 1.;
    const ViewIdx view(0);
    AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
    ParallelRenderArgsSetter frameRenderArgs(time,
                                             view,
                                             true /*isRenderUserInteraction*/,
                                             false /*isSequential*/,
                                             abortInfo,
                                             node,
                                             0 /*textureIndex*/,
                                             node->getApp()->getTimeLine().get(),
                                             NodePtr(),
                                             false /*isAnalysis*/,
                                             false /*draftMode*/,
                                             RenderStatsPtr());
    EffectInstancePtr effect = node->getEffectInstance();
    const RectD canonicalWindow = roi.toCanonical_noClipping(0, 1.);
    std::shared_ptr<FrameRequestMap> request = std::make_shared<FrameRequestMap>();
    if (EffectInstance::computeRequestPass(time, view, 0, canonicalWindow, node, *request) == eStatusFailed) {
        return false;
    }
    frameRenderArgs.updateNodesRequest(*request);
    if (beforeRender) {
        beforeRender();
    }

    std::list<ImageLayerDesc> components;
    components.push_back(ImageLayerDesc::getRGBAComponents());
    EffectInstance::RenderRoIArgs args(time,
                                       RenderScale::fromMipmapLevel(0),
                                       0,
                                       view,
                                       byPassCache,
                                       roi,
                                       RectD(),
                                       components,
                                       eImageBitDepthFloat,
                                       false /*calledFromGetImage*/,
                                       0 /*caller*/,
                                       eStorageModeRAM,
                                       time);
    std::map<ImageLayerDesc, ImagePtr> layers;
    try {
        if (effect->renderRoI(args, &layers) != EffectInstance::eRenderRoIRetCodeOk) {
            return false;
        }
    } catch (const std::exception&) {
        return false;
    }
    if (layers.empty() || !layers.begin()->second || (layers.begin()->second->getBitDepth() != eImageBitDepthFloat)) {
        return false;
    }
    Image::ReadAccess access = layers.begin()->second->getReadRights();
    *red = reinterpret_cast<const float*>(access.pixelAt(x, y))[0];

    return true;
}

// Renders on this thread's renderRoI path, and restores the scheduler mode and empties the caches
// on the way out, so no image cached here outlives the test.
class LegacyRenderScope {
public:
    LegacyRenderScope()
        : _saved(appPTR->getRenderSchedulerMode())
    {
        appPTR->setRenderSchedulerMode(eRenderSchedulerModeLegacy);
        appPTR->clearAllCaches();
    }

    ~LegacyRenderScope()
    {
        appPTR->setRenderSchedulerMode(_saved);
        appPTR->clearAllCaches();
    }

    LegacyRenderScope(const LegacyRenderScope&) = delete;
    LegacyRenderScope& operator=(const LegacyRenderScope&) = delete;

private:
    RenderSchedulerModeEnum _saved;
};

// A Constant whose colour is edited, below a Blur that caches its output. The Constant fills the
// format, so the blurred red at any pixel well inside it is the red of the colour.
struct CachedConstantBlur {
    NodePtr constant;
    NodePtr blur;
    KnobColorPtr color;
};
} // anon namespace

class CleanHashCacheTest
    : public PerFrameMetadataTest {
protected:
    bool createConstantBlur(CachedConstantBlur* graph)
    {
        graph->constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), PLUGIN_MAJOR_NATRON_CONSTANT);
        if (!graph->constant) {
            return false;
        }
        graph->color = std::dynamic_pointer_cast<KnobColor>(graph->constant->getKnobByName(kConstantParamColor));
        graph->blur = createBlurBelow(graph->constant);
        if (!graph->color || !graph->blur) {
            return false;
        }
        KnobBoolPtr forceCaching = std::dynamic_pointer_cast<KnobBool>(graph->blur->getKnobByName("forceCaching"));
        if (!forceCaching) {
            return false;
        }
        forceCaching->setValue(true);
        graph->color->setValue(0.25, ViewSpec::all(), 0);

        return true;
    }
};

namespace {
const RectI kProbeRoI(100, 100, 132, 132);
const int kProbeX = 116;
const int kProbeY = 116;
}

// A render whose frame was set up before an edit renders the edited colour under the hash of the
// state before it. Going back to that state brings its hash back, and must not find that image.
TEST_F(CleanHashCacheTest, RenderOverlappingAnEditCachesNothingUnderTheOldHash)
{
    LegacyRenderScope scope;
    CachedConstantBlur graph;
    ASSERT_TRUE(createConstantBlur(&graph));

    float red = -1.f;
    // The outcome of the overlapped render does not matter: aborting it caches nothing either.
    (void)renderRedAt(graph.blur, kProbeRoI, kProbeX, kProbeY, false, &red, [&graph]() {
        graph.color->setValue(0.75, ViewSpec::all(), 0);
    });
    graph.color->setValue(0.25, ViewSpec::all(), 0);

    ASSERT_TRUE(renderRedAt(graph.blur, kProbeRoI, kProbeX, kProbeY, false, &red));
    EXPECT_NEAR(0.25f, red, 1e-5f);
    ASSERT_TRUE(renderRedAt(graph.constant, kProbeRoI, kProbeX, kProbeY, false, &red));
    EXPECT_NEAR(0.25f, red, 1e-5f);
}

// Renders on many threads race an upstream colour toggled on the main thread. Whatever any of them
// cached under a hash must be the image of the state that hash stands for.
TEST_F(CleanHashCacheTest, ConcurrentEditsCacheNoImageUnderAStaleHash)
{
    const int kThreads = 8;
    const int kToggles = 200;
    const float kStates[] = { 0.25f, 0.75f };

    LegacyRenderScope scope;
    CachedConstantBlur graph;
    ASSERT_TRUE(createConstantBlur(&graph));

    std::atomic<bool> stop(false);
    std::atomic<int> rendered(0);
    std::vector<std::thread> renderers;
    for (int t = 0; t < kThreads; ++t) {
        renderers.emplace_back([&]() {
            while (!stop.load()) {
                float red;
                if (renderRedAt(graph.blur, kProbeRoI, kProbeX, kProbeY, false, &red)) {
                    ++rendered;
                }
            }
        });
    }
    for (int i = 0; i < kToggles; ++i) {
        graph.color->setValue(kStates[i % 2], ViewSpec::all(), 0);
    }
    stop.store(true);
    for (std::thread& renderer : renderers) {
        renderer.join();
    }
    EXPECT_GT(rendered.load(), 0);

    // Each state's hash finds what the renderers cached under it, if anything.
    for (int pass = 0; pass < 2; ++pass) {
        for (float state : kStates) {
            graph.color->setValue(state, ViewSpec::all(), 0);
            float red = -1.f;
            ASSERT_TRUE(renderRedAt(graph.blur, kProbeRoI, kProbeX, kProbeY, false, &red));
            EXPECT_NEAR(state, red, 1e-5f) << "blur, pass=" << pass;
            ASSERT_TRUE(renderRedAt(graph.constant, kProbeRoI, kProbeX, kProbeY, false, &red));
            EXPECT_NEAR(state, red, 1e-5f) << "constant, pass=" << pass;
        }
    }
}

// A derive that reads a knob edited to B and back to A around the read answers for B under the
// hash A comes back to. Nothing may keep that answer under it, here or downstream.
TEST_F(PerFrameMetadataTest, EditAndRevertDuringDeriveCachesNoStaleMetadata)
{
    NodePtr source = createSource(3);
    ASSERT_TRUE(bool(source));
    NodePtr blur = createBlurBelow(source);
    ASSERT_TRUE(bool(blur));
    MetadataSourceTestEffect* sourceEffect = metadataSourceOf(source);
    NativeEffectBase* blurEffect = dynamic_cast<NativeEffectBase*>(blur->getEffectInstance().get());
    ASSERT_TRUE(sourceEffect && blurEffect);
    KnobIntPtr tag = sourceEffect->getTagKnob();
    ASSERT_TRUE(bool(tag));
    tag->setValue(1, ViewSpec::all(), 0);
    ASSERT_EQ(std::optional<int>(1), sourceEffect->getOutputMetadata(1., ViewIdx(0)).getInt("exr/tag"));

    const auto editAndRevert = [&tag](bool afterRead) {
        tag->setValue(afterRead ? 1 : 2, ViewSpec::all(), 0);
    };

    // Derived on the source itself.
    sourceEffect->setTagReadHook(editAndRevert);
    (void)sourceEffect->getOutputMetadata(2., ViewIdx(0));
    int derivations = sourceEffect->derivationCount();
    EXPECT_EQ(std::optional<int>(1), sourceEffect->getOutputMetadata(2., ViewIdx(0)).getInt("exr/tag"));
    EXPECT_EQ(derivations + 1, sourceEffect->derivationCount());

    // Derived on the source while the node below derives its own.
    sourceEffect->setTagReadHook(editAndRevert);
    (void)blurEffect->getOutputMetadata(3., ViewIdx(0));
    derivations = sourceEffect->derivationCount();
    EXPECT_EQ(std::optional<int>(1), blurEffect->getOutputMetadata(3., ViewIdx(0)).getInt("exr/tag"));
    EXPECT_EQ(std::optional<int>(1), sourceEffect->getOutputMetadata(3., ViewIdx(0)).getInt("exr/tag"));
    EXPECT_GT(sourceEffect->derivationCount(), derivations);
}
