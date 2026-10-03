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

#include <bitset>
#include <initializer_list>
#include <list>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "DeepRenderTestEffect.h"

#include "Engine/AppInstance.h"
#include "Engine/DeepImage.h"
#include "Engine/DeepLayers.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/TypedPassthrough.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
std::vector<std::string>
names(const char* const* list,
      std::size_t count)
{
    return std::vector<std::string>(list, list + count);
}

std::list<ImageLayerDesc>
group(const std::vector<std::string>& in)
{
    std::list<ImageLayerDesc> layers;

    DeepLayers::groupDeepChannels(in, &layers);

    return layers;
}

void
expectRoundTrip(const std::list<ImageLayerDesc>& layers)
{
    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        for (int c = 0; c < it->getNumComponents(); ++c) {
            const std::string name = DeepLayers::channelName(*it, c);
            ImageLayerDesc found;
            int index = -1;

            ASSERT_TRUE(DeepLayers::findChannel(name, layers, &found, &index)) << name;
            EXPECT_EQ(it->getLayerID(), found.getLayerID()) << name;
            EXPECT_EQ(c, index) << name;
        }
    }
}
} // namespace

TEST(DeepLayers, ColorAndDottedAov)
{
    const char* const in[] = { "R", "G", "B", "A", "Z", "ZBack", "diffuse.R", "diffuse.G", "diffuse.B" };
    const std::list<ImageLayerDesc> layers = group(names(in, 9));

    ASSERT_EQ(2u, layers.size());
    EXPECT_EQ(std::string(kNatronColorLayerID), layers.front().getLayerID());
    EXPECT_EQ(4, layers.front().getNumComponents());
    EXPECT_EQ("diffuse", layers.back().getLayerID());
    ASSERT_EQ(3, layers.back().getNumComponents());
    EXPECT_EQ("R", layers.back().getChannels()[0]);
    EXPECT_EQ("G", layers.back().getChannels()[1]);
    EXPECT_EQ("B", layers.back().getChannels()[2]);

    EXPECT_EQ("diffuse.G", DeepLayers::channelName(layers.back(), 1));
    EXPECT_EQ("B", DeepLayers::channelName(layers.front(), 2));
    expectRoundTrip(layers);
}

TEST(DeepLayers, NonCanonicalAlphaPlusBareAov)
{
    const char* const in[] = { "Z", "ZBack", "A", "AOV" };
    const std::list<ImageLayerDesc> layers = group(names(in, 4));

    ASSERT_EQ(2u, layers.size());
    EXPECT_EQ(1, layers.front().getNumComponents());
    EXPECT_EQ(std::string(kNatronColorLayerID), layers.front().getLayerID());
    EXPECT_EQ("AOV", layers.back().getLayerID());
    ASSERT_EQ(1, layers.back().getNumComponents());
    EXPECT_EQ("AOV", layers.back().getChannels()[0]);

    EXPECT_EQ("A", DeepLayers::channelName(layers.front(), 0));
    EXPECT_EQ("AOV", DeepLayers::channelName(layers.back(), 0));
    expectRoundTrip(layers);
}

TEST(DeepLayers, PartialColorWidensToRgba)
{
    const char* const in[] = { "R", "A" };
    const std::list<ImageLayerDesc> layers = group(names(in, 2));

    ASSERT_EQ(1u, layers.size());
    EXPECT_EQ(4, layers.front().getNumComponents());
    expectRoundTrip(layers);
}

TEST(DeepLayers, DepthOnlyStillHasAlphaStorage)
{
    const char* const in[] = { "Z" };
    const std::list<ImageLayerDesc> layers = group(names(in, 1));

    ASSERT_EQ(1u, layers.size());
    EXPECT_EQ(std::string(kNatronColorLayerID), layers.front().getLayerID());
    EXPECT_EQ(1, layers.front().getNumComponents());
    expectRoundTrip(layers);
}

TEST(DeepLayers, RgbWithoutAlphaIsNeverRgbStorage)
{
    const char* const in[] = { "R", "G", "B", "Z" };
    const std::list<ImageLayerDesc> layers = group(names(in, 4));

    ASSERT_EQ(1u, layers.size());
    EXPECT_EQ(4, layers.front().getNumComponents());
    expectRoundTrip(layers);
}

TEST(DeepLayers, BareYAndDottedZAreOrdinaryLayers)
{
    const char* const in[] = { "Y", "depth.Z" };
    const std::list<ImageLayerDesc> layers = group(names(in, 2));

    ASSERT_EQ(3u, layers.size());
    std::list<ImageLayerDesc>::const_iterator it = layers.begin();
    ++it;
    EXPECT_EQ("Y", it->getLayerID());
    ASSERT_EQ(1, it->getNumComponents());
    EXPECT_EQ("Y", it->getChannels()[0]);
    EXPECT_EQ("Y", DeepLayers::channelName(*it, 0));
    ++it;
    EXPECT_EQ("depth", it->getLayerID());
    ASSERT_EQ(1, it->getNumComponents());
    EXPECT_EQ("Z", it->getChannels()[0]);

    EXPECT_EQ("depth.Z", DeepLayers::channelName(*it, 0));
    expectRoundTrip(layers);
}

TEST(DeepLayers, FindChannelMissesUnknownNames)
{
    const char* const in[] = { "R", "G", "B", "A", "diffuse.R" };
    const std::list<ImageLayerDesc> layers = group(names(in, 5));
    ImageLayerDesc found;
    int index = -1;

    EXPECT_FALSE(DeepLayers::findChannel("specular.R", layers, &found, &index));
    EXPECT_FALSE(DeepLayers::findChannel("Z", layers, &found, &index));
    EXPECT_EQ(-1, index);
}

TEST(DeepLayers, ExpandColorViewsListsRgbaRgbAlphaFirst)
{
    const char* const in[] = { "R", "G", "B", "A", "diffuse.R" };
    std::list<ImageLayerDesc> layers = group(names(in, 5));

    ImageLayerDesc::expandColorViews(&layers);

    std::list<ImageLayerDesc>::const_iterator it = layers.begin();
    ASSERT_GE(layers.size(), 4u);
    EXPECT_EQ("rgba", it->getLayerID());
    ++it;
    EXPECT_EQ("rgb", it->getLayerID());
    ++it;
    EXPECT_EQ("alpha", it->getLayerID());
    ++it;
    EXPECT_EQ("diffuse", it->getLayerID());
}

TEST(DeepLayers, ColorBitsAlwaysIncludeAlpha)
{
    const char* const in[] = { "R", "B", "diffuse.G" };
    const std::bitset<4> bits = DeepLayers::colorBits(names(in, 3));

    EXPECT_TRUE(bits[0]);
    EXPECT_FALSE(bits[1]);
    EXPECT_TRUE(bits[2]);
    EXPECT_TRUE(bits[3]);

    DeepImage image(RectI(0, 0, 1, 1), RenderScale::identity, ViewIdx(0));
    image.getChannelForWriting("G");
    const std::bitset<4> imageBits = DeepLayers::colorBits(image);
    EXPECT_FALSE(imageBits[0]);
    EXPECT_TRUE(imageBits[1]);
    EXPECT_FALSE(imageBits[2]);
    EXPECT_TRUE(imageBits[3]);
}

namespace {

const double kTime = 1.;

// The colour storage entry is named by its layout, e.g. "Color(1)", so a narrowed plane shows.
std::vector<std::string>
describe(const std::list<ImageLayerDesc>& layers)
{
    std::vector<std::string> ids;

    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        if (it->isColorLayer()) {
            std::ostringstream os;
            os << "Color(" << it->getNumComponents() << ")";
            ids.push_back(os.str());
        } else {
            ids.push_back(it->getLayerID());
        }
    }

    return ids;
}

std::vector<std::string>
layerIDs(const std::list<ImageLayerDesc>& layers)
{
    std::vector<std::string> result;

    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        result.push_back(it->getLayerID());
    }

    return result;
}

std::vector<std::string>
ids(std::initializer_list<const char*> list)
{
    std::vector<std::string> result;

    for (const char* id : list) {
        result.push_back(id);
    }

    return result;
}

std::list<ImageLayerDesc>
groupNames(std::initializer_list<const char*> list)
{
    std::vector<std::string> in;

    for (const char* name : list) {
        in.push_back(name);
    }

    return group(in);
}

DeepImagePtr
imageWithChannels(std::initializer_list<const char*> list)
{
    DeepImagePtr image = std::make_shared<DeepImage>(RectI(0, 0, 4, 4), RenderScale::identity, ViewIdx(0));

    for (const char* name : list) {
        image->getChannelForWriting(name);
    }

    return image;
}

} // namespace

class DeepLayersGraphTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        deepSyntheticSourceImages().clear();
        _nextSlot = 1;
    }

    // Destroyed nodes take their cached actions with them, so the next test's nodes, which may
    // get the same script names and so the same hashes, cannot be served this test's answers.
    virtual void TearDown() OVERRIDE
    {
        for (std::vector<NodePtr>::reverse_iterator it = _nodes.rbegin(); it != _nodes.rend(); ++it) {
            (*it)->destroyNode(false, false);
        }
        _nodes.clear();
        deepSyntheticSourceImages().clear();
        BaseTest::TearDown();
    }

    NodePtr createTrackedNode(const char* pluginID)
    {
        NodePtr node = createNode(QString::fromUtf8(pluginID));

        if (node) {
            _nodes.push_back(node);
        }

        return node;
    }

    NodePtr createSource(std::initializer_list<const char*> channels)
    {
        const int slot = _nextSlot++;

        deepSyntheticSourceImages()[slot] = imageWithChannels(channels);
        NodePtr node = createTrackedNode(kTestPluginIDDeepSyntheticSource);
        if (!node) {
            return node;
        }
        KnobInt* knob = dynamic_cast<KnobInt*>(node->getKnobByName("slot").get());
        if (!knob) {
            return NodePtr();
        }
        knob->setValue(slot);

        return node;
    }

    NodePtr createStub(const NodePtr& input,
                       const std::list<ImageLayerDesc>& layers)
    {
        NodePtr node = createTrackedNode(kTestPluginIDDeepLayersStub);

        if (!node) {
            return node;
        }
        DeepLayersStub* stub = dynamic_cast<DeepLayersStub*>(node->getEffectInstance().get());
        if (!stub) {
            return NodePtr();
        }
        stub->setDeepLayers(layers);
        connectNodes(input, node, 0, true);

        return node;
    }

    static std::vector<std::string> present(const NodePtr& node)
    {
        std::list<ImageLayerDesc> layers;

        node->getEffectInstance()->getPresentLayers(kTime, ViewIdx(0), -1, &layers);

        return describe(layers);
    }

    static std::vector<std::string> produced(const NodePtr& node)
    {
        EffectInstancePtr effect = node->getEffectInstance();
        EffectInstance::ComponentsNeededMap comps;
        std::list<ImageLayerDesc> passThroughLayers;
        double passThroughTime = 0.;
        int passThroughView = 0;
        std::bitset<4> processChannels;
        EffectInstance::ProcessChannelsPerPlaneMap processChannelsPerPlane;
        int passThroughInputNb = -1;

        effect->getComponentsNeededAndProduced_public(effect->getRenderHash(), kTime, ViewIdx(0), &comps, &passThroughLayers, &passThroughTime, &passThroughView, &processChannels, &processChannelsPerPlane, &passThroughInputNb);

        return describe(comps[-1]);
    }

    std::vector<NodePtr> _nodes;
    int _nextSlot;
};

TEST_F(DeepLayersGraphTest, SourcePresentsItsChannelsAsStorageLayers)
{
    NodePtr source = createSource({ "R", "G", "B", "A", "Z", "ZBack", "diffuse.R", "diffuse.G", "diffuse.B" });

    ASSERT_TRUE(bool(source));
    EXPECT_EQ(ids({ "Color(4)", "diffuse" }), present(source));
    EXPECT_EQ(ids({ "Color(4)", "diffuse" }), produced(source));
}

TEST_F(DeepLayersGraphTest, DotAndTypedPassthroughPresentTheirInputs)
{
    NodePtr source = createSource({ "R", "G", "B", "A", "Z", "diffuse.R", "diffuse.G", "diffuse.B" });
    NodePtr dot = createTrackedNode(PLUGINID_NATRON_DOT);
    NodePtr passthrough = createTrackedNode(PLUGINID_NATRON_TYPEDPASSTHROUGH);

    ASSERT_TRUE(source && dot && passthrough);
    connectNodes(source, dot, 0, true);
    connectNodes(dot, passthrough, 0, true);
    ASSERT_TRUE(dot->getEffectInstance()->producesDeepData());
    ASSERT_TRUE(passthrough->getEffectInstance()->producesDeepData());

    EXPECT_EQ(ids({ "Color(4)", "diffuse" }), present(dot));
    EXPECT_EQ(ids({ "Color(4)", "diffuse" }), present(passthrough));
    EXPECT_TRUE(produced(dot).empty());
    EXPECT_TRUE(produced(passthrough).empty());
}

TEST_F(DeepLayersGraphTest, DisabledDeepNodePresentsItsInputs)
{
    NodePtr source = createSource({ "R", "G", "B", "A", "Z", "diffuse.R", "diffuse.G", "diffuse.B" });
    ASSERT_TRUE(bool(source));
    NodePtr stub = createStub(source, groupNames({ "A" }));
    ASSERT_TRUE(bool(stub));

    stub->setNodeDisabled(true);

    EXPECT_EQ(ids({ "Color(4)", "diffuse" }), present(stub));
    EXPECT_TRUE(produced(stub).empty());
}

TEST_F(DeepLayersGraphTest, NarrowingNodePresentsOnlyItsOwnLayers)
{
    NodePtr source = createSource({ "R", "G", "B", "A", "Z", "diffuse.R", "diffuse.G", "diffuse.B" });
    ASSERT_TRUE(bool(source));
    NodePtr stub = createStub(source, groupNames({ "A" }));
    ASSERT_TRUE(bool(stub));

    EXPECT_EQ(ids({ "Color(1)" }), present(stub));
    EXPECT_EQ(ids({ "Color(1)" }), produced(stub));
}

TEST_F(DeepLayersGraphTest, WideningNodeProducesTheLayersItsInputLacks)
{
    NodePtr source = createSource({ "A", "Z" });
    ASSERT_TRUE(bool(source));
    NodePtr stub = createStub(source, groupNames({ "R", "G", "B", "A", "diffuse.R", "diffuse.G", "diffuse.B" }));
    ASSERT_TRUE(bool(stub));

    EXPECT_EQ(ids({ "Color(4)", "diffuse" }), present(stub));
    EXPECT_EQ(ids({ "Color(4)", "diffuse" }), produced(stub));
}

TEST_F(DeepLayersGraphTest, UnchangedLayersPassThroughRatherThanBeingProduced)
{
    NodePtr source = createSource({ "R", "G", "B", "A", "Z", "diffuse.R", "diffuse.G", "diffuse.B" });
    ASSERT_TRUE(bool(source));
    NodePtr stub = createStub(source, groupNames({ "R", "G", "B", "A", "diffuse.B", "diffuse.G", "diffuse.R", "specular.R" }));
    ASSERT_TRUE(bool(stub));

    EXPECT_EQ(ids({ "Color(4)", "diffuse", "specular" }), present(stub));
    EXPECT_EQ(ids({ "specular" }), produced(stub));
}

TEST_F(DeepLayersGraphTest, InputBoundKnobListsDeepLayerViews)
{
    NodePtr source = createSource({ "R", "G", "B", "A", "Z", "diffuse.R", "diffuse.G", "diffuse.B" });
    ASSERT_TRUE(bool(source));
    NodePtr stub = createStub(source, groupNames({ "R", "G", "B", "A", "diffuse.R", "diffuse.G", "diffuse.B" }));
    ASSERT_TRUE(bool(stub));
    DeepLayersStub* effect = dynamic_cast<DeepLayersStub*>(stub->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL);

    std::list<ImageLayerDesc> views;
    stub->listLayerViewsForKnob(effect->getLayersKnob(), kTime, ViewIdx(0), &views);

    EXPECT_EQ(ids({ "rgba", "rgb", "alpha", "diffuse" }), layerIDs(views));
}

TEST_F(DeepLayersGraphTest, AlphaOnlySourcePresentsAlphaStorage)
{
    NodePtr source = createSource({ "A", "Z", "ZBack" });
    ASSERT_TRUE(bool(source));
    NodePtr stub = createStub(source, groupNames({ "A" }));
    ASSERT_TRUE(bool(stub));
    DeepLayersStub* effect = dynamic_cast<DeepLayersStub*>(stub->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL);

    EXPECT_EQ(ids({ "Color(1)" }), present(source));
    EXPECT_TRUE(produced(stub).empty());

    std::list<ImageLayerDesc> views;
    stub->listLayerViewsForKnob(effect->getLayersKnob(), kTime, ViewIdx(0), &views);
    EXPECT_EQ(ids({ "rgba", "rgb", "alpha" }), layerIDs(views));
}

TEST_F(DeepLayersGraphTest, SourceRegistersItsAovs)
{
    NodePtr source = createSource({ "R", "G", "B", "A", "Z", "diffuse.R", "diffuse.G", "diffuse.B" });
    ASSERT_TRUE(bool(source));

    source->registerProducedLayers();

    ImageLayerDesc diffuse;
    ASSERT_TRUE(getApp()->getProject()->findLayer("diffuse", &diffuse));
    EXPECT_EQ(3, diffuse.getNumComponents());
}
