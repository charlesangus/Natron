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
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

#include <ofxImageEffect.h>
#include <ofxNatron.h>

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

    NodePtr createShuffleOnReader(const char* pluginID = PLUGINID_NATRON_SHUFFLE)
    {
        NodePtr reader = createReader();
        if (!reader) {
            return NodePtr();
        }
        NodePtr shuffle = createNode(QString::fromUtf8(pluginID));
        if (!shuffle) {
            return NodePtr();
        }
        connectNodes(reader, shuffle, Shuffle::eInputMain, true);

        return shuffle;
    }

    static Shuffle* shuffleEffect(const NodePtr& node)
    {
        return dynamic_cast<Shuffle*>(node->getEffectInstance().get());
    }

    static KnobLayerSelectPtr layerKnob(const NodePtr& node,
                                        const char* name)
    {
        return std::dynamic_pointer_cast<KnobLayerSelect>(node->getKnobByName(name));
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

    static bool isIdentityOfMain(const NodePtr& node)
    {
        EffectInstancePtr effect = node->getEffectInstance();
        const RectI window(0, 0, 8, 8);
        double inputTime = 0.;
        ViewIdx inputView(0);
        int inputNb = -1;
        const bool identity = effect->isIdentity_public(false, effect->getRenderHash(), 0, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb);

        return identity && inputNb == Shuffle::eInputMain;
    }

    static void expectLayerKnobsHidden(const NodePtr& node)
    {
        const char* names[] = { kShuffleParamIn1, kShuffleParamIn2, kShuffleParamOut1, kShuffleParamOut2 };

        for (std::size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
            KnobLayerSelectPtr knob = layerKnob(node, names[i]);
            ASSERT_TRUE(bool(knob)) << names[i];
            EXPECT_TRUE(knob->getDefaultIsSecret()) << names[i];
            EXPECT_TRUE(knob->getIsSecret()) << names[i];
        }
    }
};

TEST_F(ShuffleTest, OwnsItsLayerKnobsAndGetsNoHostLayerKnob)
{
    NodePtr shuffle = createNode(QString::fromUtf8(PLUGINID_NATRON_SHUFFLE));

    ASSERT_TRUE(bool(shuffle));
    EXPECT_EQ(std::string(PLUGINID_NATRON_SHUFFLE), shuffle->getPluginID());
    EXPECT_EQ(1, shuffle->getNInputs());

    EffectInstancePtr effect = shuffle->getEffectInstance();
    ASSERT_TRUE(bool(effect));
    EXPECT_TRUE(effect->isMultiPlanar());
    EXPECT_FALSE(effect->producesMetadataLayerImplicitly());
    EXPECT_EQ(std::string("Source"), effect->getInputLabel(Shuffle::eInputMain));

    Shuffle* shuffleFx = shuffleEffect(shuffle);
    ASSERT_TRUE(shuffleFx != NULL);
    EXPECT_FALSE(shuffleFx->isCopy());
    EXPECT_EQ((int)Shuffle::eInputMain, shuffleFx->getSlotInput(1));
    EXPECT_EQ((int)Shuffle::eInputMain, shuffleFx->getSlotInput(2));

    EXPECT_FALSE(bool(shuffle->getLayerKnob()));
    EXPECT_FALSE(bool(shuffle->getKnobByName(kNodeParamChannelSet)));
    EXPECT_FALSE(bool(shuffle->getKnobByName(kNodeParamLayerSelect)));
    EXPECT_FALSE(bool(shuffle->getKnobByName("in1Input")));
    EXPECT_FALSE(bool(shuffle->getKnobByName("in2Input")));

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

    expectLayerKnobsHidden(shuffle);

    std::shared_ptr<KnobShuffleMap> mapping = mappingKnob(shuffle);
    ASSERT_TRUE(bool(mapping));
    EXPECT_TRUE(mapping->getRows().empty());
}

TEST_F(ShuffleTest, ShuffleCopyHasTwoInputsAndCopiesAlphaFromInput1ByDefault)
{
    NodePtr copy = createNode(QString::fromUtf8(PLUGINID_NATRON_SHUFFLECOPY));

    ASSERT_TRUE(bool(copy));
    EXPECT_EQ(std::string(PLUGINID_NATRON_SHUFFLECOPY), copy->getPluginID());
    EXPECT_EQ(2, copy->getNInputs());

    EffectInstancePtr effect = copy->getEffectInstance();
    ASSERT_TRUE(bool(effect));
    EXPECT_EQ(std::string("2"), effect->getInputLabel(Shuffle::eInputMain));
    EXPECT_EQ(std::string("1"), effect->getInputLabel(Shuffle::eInputCopy1));
    EXPECT_TRUE(effect->isInputOptional(Shuffle::eInputMain));
    EXPECT_TRUE(effect->isInputOptional(Shuffle::eInputCopy1));

    Shuffle* copyFx = shuffleEffect(copy);
    ASSERT_TRUE(copyFx != NULL);
    EXPECT_TRUE(copyFx->isCopy());
    EXPECT_EQ((int)Shuffle::eInputCopy1, copyFx->getSlotInput(1));
    EXPECT_EQ((int)Shuffle::eInputMain, copyFx->getSlotInput(2));

    KnobLayerSelectPtr in1 = layerKnob(copy, kShuffleParamIn1);
    KnobLayerSelectPtr in2 = layerKnob(copy, kShuffleParamIn2);
    KnobLayerSelectPtr out1 = layerKnob(copy, kShuffleParamOut1);
    KnobLayerSelectPtr out2 = layerKnob(copy, kShuffleParamOut2);
    ASSERT_TRUE(bool(in1));
    ASSERT_TRUE(bool(in2));
    ASSERT_TRUE(bool(out1));
    ASSERT_TRUE(bool(out2));
    EXPECT_EQ(std::string(kNatronColorLayerID), in1->getLayer());
    EXPECT_EQ(std::string(kNatronColorLayerID), in2->getLayer());
    EXPECT_EQ(std::string(kNatronColorLayerID), out1->getLayer());
    EXPECT_EQ(std::string(), out2->getLayer());

    expectLayerKnobsHidden(copy);

    const double time = getApp()->getTimeLine()->currentFrame();

    std::shared_ptr<KnobShuffleMap> mapping = mappingKnob(copy);
    ASSERT_TRUE(bool(mapping));
    EXPECT_EQ(3u, mapping->getRows().size());
    for (int c = 0; c < 3; ++c) {
        EXPECT_TRUE(mapping->hasExplicitSource(1, c)) << c;
        EXPECT_EQ(ShuffleSource::makeInput(2, c), copyFx->getEffectiveSource(1, c, time)) << c;
    }
    EXPECT_FALSE(mapping->hasExplicitSource(1, 3));
    EXPECT_EQ(ShuffleSource::makeInput(1, 3), copyFx->getEffectiveSource(1, 3, time));

    mapping->setSource(1, 0, ShuffleSource::makeOne());
    EXPECT_EQ(ShuffleSource::makeOne(), copyFx->getEffectiveSource(1, 0, time));
    mapping->reset();
    EXPECT_EQ(3u, mapping->getRows().size());
    EXPECT_EQ(ShuffleSource::makeInput(2, 0), copyFx->getEffectiveSource(1, 0, time));
}

TEST_F(ShuffleTest, ShuffleCopyPrefersInput2WhenBothAreConnected)
{
    NodePtr copy = createShuffleOnReader(PLUGINID_NATRON_SHUFFLECOPY);

    ASSERT_TRUE(bool(copy));
    NodePtr second = createReader();
    ASSERT_TRUE(bool(second));
    connectNodes(second, copy, Shuffle::eInputCopy1, true);

    ASSERT_TRUE(bool(copy->getInput(Shuffle::eInputMain)));
    ASSERT_TRUE(bool(copy->getInput(Shuffle::eInputCopy1)));
    EXPECT_EQ((int)Shuffle::eInputMain, copy->getPreferredInput());
}

TEST_F(ShuffleTest, ShuffleCopySlotsListTheirOwnInputs)
{
    NodePtr copy = createShuffleOnReader(PLUGINID_NATRON_SHUFFLECOPY);

    ASSERT_TRUE(bool(copy));
    KnobLayerSelectPtr in1 = layerKnob(copy, kShuffleParamIn1);
    KnobLayerSelectPtr in2 = layerKnob(copy, kShuffleParamIn2);
    ASSERT_TRUE(bool(in1));
    ASSERT_TRUE(bool(in2));

    std::vector<std::string> colorOnly;
    colorOnly.push_back(kNatronColorLayerID);
    EXPECT_EQ(colorOnly, listed(copy, in1));
    EXPECT_EQ(colorDiffuseSpecular(), listed(copy, in2));

    NodePtr second = createReader();
    ASSERT_TRUE(bool(second));
    connectNodes(second, copy, Shuffle::eInputCopy1, true);
    EXPECT_EQ(colorDiffuseSpecular(), listed(copy, in1));
}

TEST_F(ShuffleTest, Out1ListsTheRegistryAndIn1ListsTheSource)
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
    KnobLayerSelectPtr in2 = layerKnob(shuffle, kShuffleParamIn2);
    ASSERT_TRUE(bool(in1));
    ASSERT_TRUE(bool(in2));
    EXPECT_EQ(colorDiffuseSpecular(), listed(shuffle, in1));
    EXPECT_EQ(colorDiffuseSpecular(), listed(shuffle, in2));
}

TEST_F(ShuffleTest, ProducesOnlyItsOutputLayerAndPassesTheRestThroughFromTheSource)
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

    EXPECT_EQ((int)Shuffle::eInputMain, passThroughInputNb);
    const std::vector<std::string> passThrough = layerIDs(passThroughLayers);
    EXPECT_TRUE(contains(passThrough, kNatronColorLayerID));
    EXPECT_TRUE(contains(passThrough, "diffuse"));
    EXPECT_TRUE(contains(passThrough, "specular"));
    EXPECT_FALSE(contains(passThrough, "spec2"));

    // spec2's channels read in1's Color by default, and nothing reads the None in2.
    std::vector<std::string> expectedFromMain;
    expectedFromMain.push_back(kNatronColorLayerID);
    EXPECT_EQ(expectedFromMain, layerIDs(comps[Shuffle::eInputMain]));
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

TEST_F(ShuffleTest, NewShuffleIsAnIdentityAndNewShuffleCopyIsNot)
{
    NodePtr lone = createNode(QString::fromUtf8(PLUGINID_NATRON_SHUFFLE));

    ASSERT_TRUE(bool(lone));
    EXPECT_TRUE(isIdentityOfMain(lone));

    NodePtr shuffle = createShuffleOnReader();
    ASSERT_TRUE(bool(shuffle));
    EXPECT_TRUE(isIdentityOfMain(shuffle));

    NodePtr copy = createShuffleOnReader(PLUGINID_NATRON_SHUFFLECOPY);
    ASSERT_TRUE(bool(copy));
    EXPECT_FALSE(isIdentityOfMain(copy));
}

TEST_F(ShuffleTest, IdentityOnlyWhileEveryOut1ChannelReadsItsOwnChannelOfTheSameLayer)
{
    NodePtr shuffle = createShuffleOnReader();

    ASSERT_TRUE(bool(shuffle));

    std::shared_ptr<KnobShuffleMap> mapping = mappingKnob(shuffle);
    KnobLayerSelectPtr in1 = layerKnob(shuffle, kShuffleParamIn1);
    KnobLayerSelectPtr in2 = layerKnob(shuffle, kShuffleParamIn2);
    KnobLayerSelectPtr out2 = layerKnob(shuffle, kShuffleParamOut2);
    ASSERT_TRUE(bool(mapping));
    ASSERT_TRUE(bool(in1));
    ASSERT_TRUE(bool(in2));
    ASSERT_TRUE(bool(out2));

    mapping->setSource(1, 0, ShuffleSource::makeInput(1, 1));
    EXPECT_FALSE(isIdentityOfMain(shuffle));

    mapping->setSource(1, 0, ShuffleSource::makeZero());
    EXPECT_FALSE(isIdentityOfMain(shuffle));

    mapping->setSource(1, 0, ShuffleSource::makeInput(1, 0));
    EXPECT_FALSE(mapping->hasExplicitSource(1, 0));
    EXPECT_TRUE(isIdentityOfMain(shuffle));

    in1->setLayer("diffuse");
    EXPECT_FALSE(isIdentityOfMain(shuffle));

    in1->setLayer(kNatronColorLayerID);
    mapping->setSource(1, 1, ShuffleSource::makeInput(2, 1));
    EXPECT_FALSE(isIdentityOfMain(shuffle));

    in2->setLayer(kNatronColorLayerID);
    EXPECT_TRUE(isIdentityOfMain(shuffle));

    out2->setLayer("diffuse");
    EXPECT_FALSE(isIdentityOfMain(shuffle));

    out2->setLayer(kNatronColorLayerID);
    EXPECT_TRUE(isIdentityOfMain(shuffle));
}

TEST_F(ShuffleTest, EffectiveSourceZeroesAnImplicitChannelTheSlotLacks)
{
    NodePtr shuffle = createShuffleOnReader();

    ASSERT_TRUE(bool(shuffle));
    Shuffle* shuffleFx = shuffleEffect(shuffle);
    std::shared_ptr<KnobShuffleMap> mapping = mappingKnob(shuffle);
    KnobLayerSelectPtr in1 = layerKnob(shuffle, kShuffleParamIn1);
    ASSERT_TRUE(shuffleFx != NULL);
    ASSERT_TRUE(bool(mapping));
    ASSERT_TRUE(bool(in1));

    const double time = getApp()->getTimeLine()->currentFrame();

    for (int c = 0; c < 4; ++c) {
        EXPECT_EQ(ShuffleSource::makeInput(1, c), shuffleFx->getEffectiveSource(1, c, time)) << c;
        EXPECT_EQ(ShuffleSource::makeZero(), shuffleFx->getEffectiveSource(2, c, time)) << c;
    }

    in1->setLayer("diffuse");
    EXPECT_EQ(ShuffleSource::makeInput(1, 2), shuffleFx->getEffectiveSource(1, 2, time));
    EXPECT_EQ(ShuffleSource::makeZero(), shuffleFx->getEffectiveSource(1, 3, time));

    // An explicit row is returned as stored, even beyond the slot's layer.
    mapping->setSource(1, 0, ShuffleSource::makeInput(1, 3));
    EXPECT_TRUE(mapping->hasExplicitSource(1, 0));
    EXPECT_EQ(ShuffleSource::makeInput(1, 3), shuffleFx->getEffectiveSource(1, 0, time));

    mapping->setSource(1, 1, ShuffleSource::makeInput(2, 1));
    EXPECT_EQ(ShuffleSource::makeInput(2, 1), shuffleFx->getEffectiveSource(1, 1, time));
}

TEST_F(ShuffleTest, SubLabelFollowsTheMapping)
{
    NodePtr shuffle = createShuffleOnReader();

    ASSERT_TRUE(bool(shuffle));
    std::shared_ptr<KnobShuffleMap> mapping = mappingKnob(shuffle);
    KnobLayerSelectPtr in1 = layerKnob(shuffle, kShuffleParamIn1);
    KnobLayerSelectPtr in2 = layerKnob(shuffle, kShuffleParamIn2);
    KnobLayerSelectPtr out1 = layerKnob(shuffle, kShuffleParamOut1);
    KnobLayerSelectPtr out2 = layerKnob(shuffle, kShuffleParamOut2);
    KnobStringPtr sublabel = std::dynamic_pointer_cast<KnobString>(shuffle->getKnobByName(kNatronOfxParamStringSublabelName));
    ASSERT_TRUE(bool(mapping));
    ASSERT_TRUE(bool(in1));
    ASSERT_TRUE(bool(in2));
    ASSERT_TRUE(bool(out1));
    ASSERT_TRUE(bool(out2));
    ASSERT_TRUE(bool(sublabel));

    in1->setLayer(kNatronColorLayerID);
    in2->setLayer("specular");
    out1->setLayer(kNatronColorLayerID);
    out2->setLayer("diffuse");

    // Out1 only passes Color through, so only out2 (fed by specular) is named.
    const std::string arrow = " \xE2\x86\x92 ";
    const std::string specularToDiffuse = sublabel->getValue();
    const std::size_t arrowPos = specularToDiffuse.find(arrow);
    ASSERT_NE(std::string::npos, arrowPos) << specularToDiffuse;
    const std::string specular = specularToDiffuse.substr(0, arrowPos);
    const std::string diffuse = specularToDiffuse.substr(arrowPos + arrow.size());
    const std::string color = ImageLayerDesc::getRGBAComponents().getLayerLabel();
    EXPECT_FALSE(specular.empty());
    EXPECT_FALSE(diffuse.empty());

    mapping->setSource(1, 0, ShuffleSource::makeInput(2, 0));
    EXPECT_EQ(color + ", " + specular + arrow + color + "\n" + specularToDiffuse, sublabel->getValue());

    mapping->setSource(2, 1, ShuffleSource::makeInput(1, 1));
    EXPECT_EQ(color + ", " + specular + arrow + color + "\n" + color + ", " + specular + arrow + diffuse, sublabel->getValue());

    mapping->setSource(1, 0, ShuffleSource::makeInput(1, 0));
    EXPECT_EQ(color + ", " + specular + arrow + diffuse, sublabel->getValue());
}
