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
#include <random>
#include <thread>
#include <vector>

#include <QString>

#include <gtest/gtest.h>

#include "BaseTest.h"
#include "NativeMetadataTestEffect.h"
#include "OfxTestPlugins/natronTest/NatronTestPluginIDs.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Merge/Dissolve.h"
#include "Engine/Nodes/Merge/Switch.h"
#include "Engine/Project.h"
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
