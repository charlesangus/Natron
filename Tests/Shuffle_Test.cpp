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

#include <algorithm>
#include <bitset>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobShuffleMap.h"
#include "Engine/KnobTypes.h"
#include "Engine/LayerRegistry.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Channel/Shuffle.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

#include <ofxImageEffect.h>

NATRON_NAMESPACE_USING

namespace {

std::vector<std::string>
layerIDs(const std::list<ImageLayerDesc>& layers)
{
    std::vector<std::string> ids;

    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        ids.push_back(it->getLayerID());
    }

    return ids;
}

bool
contains(const std::vector<std::string>& ids,
         const std::string& id)
{
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

std::vector<std::string>
colorDiffuseSpecular()
{
    std::vector<std::string> ids;

    ids.push_back(kNatronColorLayerID);
    ids.push_back("diffuse");
    ids.push_back("specular");

    return ids;
}

} // namespace

class ShuffleTest
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

    NodePtr createReader()
    {
        CreateNodeArgs readerArgs(_readOIIOPluginID.toStdString(), getApp()->getProject());

        readerArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/flat-three-layers.exr"));

        return getApp()->createNode(readerArgs);
    }

    NodePtr createShuffleOnReader()
    {
        NodePtr reader = createReader();
        if (!reader) {
            return NodePtr();
        }
        NodePtr shuffle = createNode(QString::fromUtf8(PLUGINID_NATRON_SHUFFLE));
        if (!shuffle) {
            return NodePtr();
        }
        connectNodes(reader, shuffle, Shuffle::eInputB, true);

        return shuffle;
    }

    static KnobLayerSelectPtr layerKnob(const NodePtr& node,
                                        const char* name)
    {
        return std::dynamic_pointer_cast<KnobLayerSelect>(node->getKnobByName(name));
    }

    static KnobChoicePtr choiceKnob(const NodePtr& node,
                                    const char* name)
    {
        return std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(name));
    }

    static std::shared_ptr<KnobShuffleMap> mappingKnob(const NodePtr& node)
    {
        return std::dynamic_pointer_cast<KnobShuffleMap>(node->getKnobByName(kShuffleParamMapping));
    }

    static std::vector<std::string> listed(const NodePtr& node,
                                           const KnobIPtr& knob)
    {
        std::list<ImageLayerDesc> layers;

        node->listLayersForKnob(knob, &layers);

        return layerIDs(layers);
    }

    static bool isIdentityOfB(const NodePtr& node)
    {
        EffectInstancePtr effect = node->getEffectInstance();
        const RectI window(0, 0, 8, 8);
        double inputTime = 0.;
        ViewIdx inputView(0);
        int inputNb = -1;
        const bool identity = effect->isIdentity_public(false, effect->getRenderHash(), 0, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb);

        return identity && inputNb == Shuffle::eInputB;
    }
};

TEST_F(ShuffleTest, OwnsItsLayerKnobsAndGetsNoHostLayerKnob)
{
    NodePtr shuffle = createNode(QString::fromUtf8(PLUGINID_NATRON_SHUFFLE));

    ASSERT_TRUE(bool(shuffle));
    EXPECT_EQ(std::string(PLUGINID_NATRON_SHUFFLE), shuffle->getPluginID());
    EXPECT_EQ(2, shuffle->getNInputs());

    EffectInstancePtr effect = shuffle->getEffectInstance();
    ASSERT_TRUE(bool(effect));
    EXPECT_TRUE(effect->isMultiPlanar());
    EXPECT_FALSE(effect->producesMetadataLayerImplicitly());
    EXPECT_EQ(std::string("B"), effect->getInputLabel(Shuffle::eInputB));
    EXPECT_EQ(std::string("A"), effect->getInputLabel(Shuffle::eInputA));

    EXPECT_FALSE(bool(shuffle->getLayerKnob()));
    EXPECT_FALSE(bool(shuffle->getKnobByName(kNodeParamChannelSet)));
    EXPECT_FALSE(bool(shuffle->getKnobByName(kNodeParamLayerSelect)));

    KnobChoicePtr in1Input = choiceKnob(shuffle, kShuffleParamIn1Input);
    KnobChoicePtr in2Input = choiceKnob(shuffle, kShuffleParamIn2Input);
    ASSERT_TRUE(bool(in1Input));
    ASSERT_TRUE(bool(in2Input));
    EXPECT_EQ((int)Shuffle::eInputB, in1Input->getValue());
    EXPECT_EQ((int)Shuffle::eInputA, in2Input->getValue());

    KnobLayerSelectPtr in1 = layerKnob(shuffle, kShuffleParamIn1);
    KnobLayerSelectPtr in2 = layerKnob(shuffle, kShuffleParamIn2);
    KnobLayerSelectPtr out1 = layerKnob(shuffle, kShuffleParamOut1);
    KnobLayerSelectPtr out2 = layerKnob(shuffle, kShuffleParamOut2);
    ASSERT_TRUE(bool(in1));
    ASSERT_TRUE(bool(in2));
    ASSERT_TRUE(bool(out1));
    ASSERT_TRUE(bool(out2));

    EXPECT_EQ(std::string(kNatronColorLayerID), in1->getLayer());
    EXPECT_EQ(std::string(), in2->getLayer());
    EXPECT_EQ(std::string(kNatronColorLayerID), out1->getLayer());
    EXPECT_EQ(std::string(), out2->getLayer());

    EXPECT_TRUE(in1->getAllowNone());
    EXPECT_TRUE(in2->getAllowNone());
    EXPECT_FALSE(out1->getAllowNone());
    EXPECT_TRUE(out2->getAllowNone());

    EXPECT_FALSE(in1->getWithChannelButtons());
    EXPECT_FALSE(in2->getWithChannelButtons());
    EXPECT_FALSE(out1->getWithChannelButtons());
    EXPECT_FALSE(out2->getWithChannelButtons());

    EXPECT_FALSE(shuffle->isTargetLayerKnob(in1));
    EXPECT_FALSE(shuffle->isTargetLayerKnob(in2));
    EXPECT_TRUE(shuffle->isTargetLayerKnob(out1));
    EXPECT_TRUE(shuffle->isTargetLayerKnob(out2));

    std::shared_ptr<KnobShuffleMap> mapping = mappingKnob(shuffle);
    ASSERT_TRUE(bool(mapping));
    EXPECT_TRUE(mapping->getRows().empty());
}

TEST_F(ShuffleTest, Out1ListsTheRegistryAndIn1ListsInputB)
{
    NodePtr shuffle = createShuffleOnReader();

    ASSERT_TRUE(bool(shuffle));

    KnobLayerSelectPtr out1 = layerKnob(shuffle, kShuffleParamOut1);
    ASSERT_TRUE(bool(out1));
    const std::vector<std::string> outIDs = listed(shuffle, out1);
    ASSERT_GE(outIDs.size(), 6u);
    EXPECT_EQ(std::string(kNatronColorLayerID), outIDs[0]);
    EXPECT_TRUE(contains(outIDs, "depth"));

    KnobLayerSelectPtr in1 = layerKnob(shuffle, kShuffleParamIn1);
    ASSERT_TRUE(bool(in1));
    EXPECT_EQ(colorDiffuseSpecular(), listed(shuffle, in1));
}

TEST_F(ShuffleTest, ProducesOnlyItsOutputLayerAndPassesTheRestThroughFromB)
{
    ProjectPtr project = getApp()->getProject();
    std::vector<std::string> rgb;

    rgb.push_back("R");
    rgb.push_back("G");
    rgb.push_back("B");
    std::string error;
    ASSERT_EQ(LayerRegistry::eAddResultAdded, project->addLayer(ImageLayerDesc("spec2", "spec2", "", rgb), LayerRegistryEntry::eOriginUser, &error)) << error;

    NodePtr shuffle = createShuffleOnReader();
    ASSERT_TRUE(bool(shuffle));

    KnobLayerSelectPtr out1 = layerKnob(shuffle, kShuffleParamOut1);
    ASSERT_TRUE(bool(out1));
    out1->setLayer("spec2");

    EffectInstancePtr effect = shuffle->getEffectInstance();
    EffectInstance::ComponentsNeededMap comps;
    std::list<ImageLayerDesc> passThroughLayers;
    double passThroughTime = 0.;
    int passThroughView = 0;
    std::bitset<4> processChannels;
    EffectInstance::ProcessChannelsPerPlaneMap processChannelsPerPlane;
    int passThroughInputNb = -1;
    effect->getComponentsNeededAndProduced_public(effect->getRenderHash(), 0, ViewIdx(0), &comps, &passThroughLayers, &passThroughTime, &passThroughView, &processChannels, &processChannelsPerPlane, &passThroughInputNb);

    std::vector<std::string> expectedProduced;
    expectedProduced.push_back("spec2");
    EXPECT_EQ(expectedProduced, layerIDs(comps[-1]));

    EXPECT_EQ((int)Shuffle::eInputB, passThroughInputNb);
    const std::vector<std::string> passThrough = layerIDs(passThroughLayers);
    EXPECT_TRUE(contains(passThrough, kNatronColorLayerID));
    EXPECT_TRUE(contains(passThrough, "diffuse"));
    EXPECT_TRUE(contains(passThrough, "specular"));
    EXPECT_FALSE(contains(passThrough, "spec2"));

    // B has no spec2 of its own to keep, so B is asked only for in1's Color.
    std::vector<std::string> expectedFromB;
    expectedFromB.push_back(kNatronColorLayerID);
    EXPECT_EQ(expectedFromB, layerIDs(comps[Shuffle::eInputB]));
    EXPECT_TRUE(comps[Shuffle::eInputA].empty());
}

TEST_F(ShuffleTest, In2BoundToAnUnconnectedAListsColorOnly)
{
    NodePtr shuffle = createShuffleOnReader();

    ASSERT_TRUE(bool(shuffle));

    KnobChoicePtr in2Input = choiceKnob(shuffle, kShuffleParamIn2Input);
    KnobLayerSelectPtr in2 = layerKnob(shuffle, kShuffleParamIn2);
    ASSERT_TRUE(bool(in2Input));
    ASSERT_TRUE(bool(in2));

    in2Input->setValue((int)Shuffle::eInputA);
    std::vector<std::string> colorOnly;
    colorOnly.push_back(kNatronColorLayerID);
    EXPECT_EQ(colorOnly, listed(shuffle, in2));

    in2Input->setValue((int)Shuffle::eInputB);
    EXPECT_EQ(colorDiffuseSpecular(), listed(shuffle, in2));
}

TEST_F(ShuffleTest, RemovingALayerAnInputSlotReadsIsRefused)
{
    NodePtr shuffle = createShuffleOnReader();

    ASSERT_TRUE(bool(shuffle));

    KnobLayerSelectPtr in1 = layerKnob(shuffle, kShuffleParamIn1);
    ASSERT_TRUE(bool(in1));
    in1->setLayer("diffuse");

    std::string error;
    EXPECT_FALSE(getApp()->getProject()->removeLayer("diffuse", &error));
    EXPECT_NE(std::string::npos, error.find(shuffle->getScriptName_mt_safe())) << error;
}

TEST_F(ShuffleTest, NewNodeIsAnIdentityOfB)
{
    NodePtr lone = createNode(QString::fromUtf8(PLUGINID_NATRON_SHUFFLE));

    ASSERT_TRUE(bool(lone));
    EXPECT_TRUE(isIdentityOfB(lone));

    NodePtr shuffle = createShuffleOnReader();
    ASSERT_TRUE(bool(shuffle));
    EXPECT_TRUE(isIdentityOfB(shuffle));
}

TEST_F(ShuffleTest, IdentityOnlyWhileEveryOut1ChannelKeepsOrIsStraightFromIn1OnB)
{
    NodePtr shuffle = createShuffleOnReader();

    ASSERT_TRUE(bool(shuffle));

    std::shared_ptr<KnobShuffleMap> mapping = mappingKnob(shuffle);
    KnobLayerSelectPtr in1 = layerKnob(shuffle, kShuffleParamIn1);
    KnobLayerSelectPtr out2 = layerKnob(shuffle, kShuffleParamOut2);
    KnobChoicePtr in1Input = choiceKnob(shuffle, kShuffleParamIn1Input);
    ASSERT_TRUE(bool(mapping));
    ASSERT_TRUE(bool(in1));
    ASSERT_TRUE(bool(out2));
    ASSERT_TRUE(bool(in1Input));

    mapping->setSource(1, 0, ShuffleSource::makeInput(1, 0));
    EXPECT_TRUE(isIdentityOfB(shuffle));

    mapping->setSource(1, 0, ShuffleSource::makeInput(1, 1));
    EXPECT_FALSE(isIdentityOfB(shuffle));

    mapping->setSource(1, 0, ShuffleSource::makeZero());
    EXPECT_FALSE(isIdentityOfB(shuffle));

    mapping->setSource(1, 0, ShuffleSource::makeInput(1, 0));
    in1->setLayer("diffuse");
    EXPECT_FALSE(isIdentityOfB(shuffle));

    in1->setLayer(kNatronColorLayerID);
    in1Input->setValue((int)Shuffle::eInputA);
    EXPECT_FALSE(isIdentityOfB(shuffle));

    in1Input->setValue((int)Shuffle::eInputB);
    EXPECT_TRUE(isIdentityOfB(shuffle));

    out2->setLayer("diffuse");
    EXPECT_FALSE(isIdentityOfB(shuffle));

    out2->setLayer(kNatronColorLayerID);
    EXPECT_TRUE(isIdentityOfB(shuffle));
}
