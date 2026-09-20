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
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

#include <ofxImageEffect.h>

NATRON_NAMESPACE_USING

namespace {

struct NeededComponents {
    U64 hash;
    EffectInstance::ComponentsNeededMap comps;
    std::list<ImageLayerDesc> passThroughLayers;
    std::bitset<4> processChannels;
    EffectInstance::ProcessChannelsPerPlaneMap processChannelsPerPlane;
    int passThroughInputNb;

    NeededComponents()
        : hash(0)
        , comps()
        , passThroughLayers()
        , processChannels()
        , processChannelsPerPlane()
        , passThroughInputNb(-1)
    {
    }
};

NeededComponents
queryNeededComponents(const NodePtr& node)
{
    NeededComponents ret;
    EffectInstancePtr effect = node->getEffectInstance();
    double passThroughTime = 0.;
    int passThroughView = 0;

    ret.hash = effect->getRenderHash();
    effect->getComponentsNeededAndProduced_public(ret.hash, 0, ViewIdx(0), &ret.comps, &ret.passThroughLayers, &passThroughTime, &passThroughView, &ret.processChannels, &ret.processChannelsPerPlane, &ret.passThroughInputNb);

    return ret;
}

std::vector<std::string>
layerIDs(const std::list<ImageLayerDesc>& layers)
{
    std::vector<std::string> ids;

    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        ids.push_back(it->getLayerID());
    }

    return ids;
}

std::vector<std::string>
outputLayerIDs(const NeededComponents& needed)
{
    EffectInstance::ComponentsNeededMap::const_iterator found = needed.comps.find(-1);

    return found == needed.comps.end() ? std::vector<std::string>() : layerIDs(found->second);
}

std::bitset<4>
bits(bool r,
     bool g,
     bool b,
     bool a)
{
    std::bitset<4> ret;

    ret[0] = r;
    ret[1] = g;
    ret[2] = b;
    ret[3] = a;

    return ret;
}

std::bitset<4>
planeBits(const NeededComponents& needed,
          const std::string& layerID)
{
    for (EffectInstance::ProcessChannelsPerPlaneMap::const_iterator it = needed.processChannelsPerPlane.begin(); it != needed.processChannelsPerPlane.end(); ++it) {
        if (it->first.getLayerID() == layerID) {
            return it->second;
        }
    }

    return std::bitset<4>();
}

} // namespace

class LayerKnobsRenderTest
    : public BaseTest {
protected:
    NodePtr createReader()
    {
        CreateNodeArgs readerArgs(_readOIIOPluginID.toStdString(), getApp()->getProject());

        readerArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/flat-three-layers.exr"));

        return getApp()->createNode(readerArgs);
    }

    NodePtr createInvertOnReader(KnobChannelSetPtr* channels)
    {
        NodePtr reader = createReader();
        if (!reader) {
            return NodePtr();
        }
        NodePtr invert = createNode(QString::fromUtf8("net.sf.openfx.Invert"));
        if (!invert) {
            return NodePtr();
        }
        connectNodes(reader, invert, 0, true);
        *channels = std::dynamic_pointer_cast<KnobChannelSet>(invert->getKnobByName(kNodeParamChannelSet));

        return invert;
    }
};

TEST_F(LayerKnobsRenderTest, ChannelSetRowsSelectOutputAndInputPlanesWithPerPlaneBits)
{
    KnobChannelSetPtr channels;
    NodePtr invert = createInvertOnReader(&channels);

    ASSERT_TRUE(bool(invert));
    ASSERT_TRUE(bool(channels));

    const U64 defaultHash = queryNeededComponents(invert).hash;

    channels->setLayer(0, kNatronColorLayerID, NULL);
    std::vector<std::string> rg;
    rg.push_back("R");
    rg.push_back("G");
    channels->addLayer("diffuse", &rg);

    NeededComponents needed = queryNeededComponents(invert);
    EXPECT_NE(defaultHash, needed.hash);

    std::vector<std::string> expected;
    expected.push_back(kNatronColorLayerID);
    expected.push_back("diffuse");
    EXPECT_EQ(expected, outputLayerIDs(needed));

    EffectInstance::ComponentsNeededMap::const_iterator output = needed.comps.find(-1);
    ASSERT_NE(needed.comps.end(), output);
    EXPECT_EQ(4, output->second.front().getNumComponents());

    EffectInstance::ComponentsNeededMap::const_iterator source = needed.comps.find(0);
    ASSERT_NE(needed.comps.end(), source);
    EXPECT_EQ(output->second, source->second);

    ASSERT_EQ(2u, needed.processChannelsPerPlane.size());
    EXPECT_EQ(bits(true, true, true, true), planeBits(needed, kNatronColorLayerID));
    EXPECT_EQ(bits(true, true, false, false), planeBits(needed, "diffuse"));
    EXPECT_EQ(bits(true, true, true, true), needed.processChannels);

    // Lookups go by layer ID, so a descriptor carrying another channel list still finds its entry.
    EffectInstancePtr effect = invert->getEffectInstance();
    ImageLayerDesc diffuse("diffuse", "diffuse", "RG", rg);
    EXPECT_EQ(bits(true, true, false, false), effect->getProcessChannelsForPlane(needed.hash, 0, ViewIdx(0), diffuse));
    ImageLayerDesc specular("specular", "specular", "RG", rg);
    EXPECT_EQ(bits(true, true, true, true), effect->getProcessChannelsForPlane(needed.hash, 0, ViewIdx(0), specular));
}

TEST_F(LayerKnobsRenderTest, ChannelSetAllSelectsEveryPresentPlane)
{
    KnobChannelSetPtr channels;
    NodePtr invert = createInvertOnReader(&channels);

    ASSERT_TRUE(bool(invert));
    ASSERT_TRUE(bool(channels));

    channels->setAll();

    NeededComponents needed = queryNeededComponents(invert);

    std::vector<std::string> expected;
    expected.push_back(kNatronColorLayerID);
    expected.push_back("diffuse");
    expected.push_back("specular");
    EXPECT_EQ(expected, outputLayerIDs(needed));

    EffectInstance::ComponentsNeededMap::const_iterator source = needed.comps.find(0);
    ASSERT_NE(needed.comps.end(), source);
    EXPECT_EQ(expected, layerIDs(source->second));

    ASSERT_EQ(3u, needed.processChannelsPerPlane.size());
    EXPECT_EQ(bits(true, true, true, true), planeBits(needed, kNatronColorLayerID));
    EXPECT_EQ(bits(true, true, true, false), planeBits(needed, "diffuse"));
    EXPECT_EQ(bits(true, true, true, false), planeBits(needed, "specular"));
    EXPECT_EQ(bits(true, true, true, true), needed.processChannels);
}

// A selection resolving to nothing falls back to the metadata Color plane with no per-plane
// entry, so the plane is processed on every channel until the node reports identity for it.
TEST_F(LayerKnobsRenderTest, EmptySelectionFallsBackToMetadataColor)
{
    KnobChannelSetPtr channels;
    NodePtr invert = createInvertOnReader(&channels);

    ASSERT_TRUE(bool(invert));
    ASSERT_TRUE(bool(channels));

    std::vector<std::string> colorOnly;
    colorOnly.push_back(kNatronColorLayerID);

    channels->setRegex(0, "nothing.*");
    {
        NeededComponents needed = queryNeededComponents(invert);
        EXPECT_EQ(colorOnly, outputLayerIDs(needed));
        EffectInstance::ComponentsNeededMap::const_iterator source = needed.comps.find(0);
        ASSERT_NE(needed.comps.end(), source);
        EXPECT_EQ(colorOnly, layerIDs(source->second));
        EXPECT_TRUE(needed.processChannelsPerPlane.empty());
        EXPECT_EQ(bits(true, true, true, true), needed.processChannels);
        EXPECT_EQ(bits(true, true, true, true), invert->getEffectInstance()->getProcessChannelsForPlane(needed.hash, 0, ViewIdx(0), ImageLayerDesc::getRGBAComponents()));
    }

    channels->setNone();
    {
        NeededComponents needed = queryNeededComponents(invert);
        EXPECT_EQ(colorOnly, outputLayerIDs(needed));
        EXPECT_TRUE(needed.processChannelsPerPlane.empty());
        EXPECT_EQ(bits(true, true, true, true), needed.processChannels);
    }
}

TEST_F(LayerKnobsRenderTest, GradeDefaultRowMasksAlphaThroughTheColorBits)
{
    NodePtr reader = createReader();
    ASSERT_TRUE(bool(reader));
    NodePtr grade = createNode(QString::fromUtf8("net.sf.openfx.GradePlugin"));
    ASSERT_TRUE(bool(grade));
    connectNodes(reader, grade, 0, true);

    NeededComponents needed = queryNeededComponents(grade);

    std::vector<std::string> colorOnly;
    colorOnly.push_back(kNatronColorLayerID);
    EXPECT_EQ(colorOnly, outputLayerIDs(needed));
    EXPECT_EQ(bits(true, true, true, false), planeBits(needed, kNatronColorLayerID));
    EXPECT_EQ(bits(true, true, true, false), needed.processChannels);
}

TEST_F(LayerKnobsRenderTest, TargetLayerSelectProducesTheChosenPlane)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    NodePtr constant = createNode(QString::fromUtf8("net.sf.openfx.ConstantPlugin"));
    ASSERT_TRUE(bool(constant));
    KnobLayerSelectPtr layer = std::dynamic_pointer_cast<KnobLayerSelect>(constant->getKnobByName(kNodeParamLayerSelect));
    ASSERT_TRUE(bool(layer));

    {
        NeededComponents needed = queryNeededComponents(constant);
        std::vector<std::string> colorOnly;
        colorOnly.push_back(kNatronColorLayerID);
        EXPECT_EQ(colorOnly, outputLayerIDs(needed));
        EXPECT_EQ(bits(true, true, true, true), planeBits(needed, kNatronColorLayerID));
    }

    layer->setLayer("depth");
    {
        NeededComponents needed = queryNeededComponents(constant);
        std::vector<std::string> depthOnly;
        depthOnly.push_back("depth");
        EXPECT_EQ(depthOnly, outputLayerIDs(needed));
        ASSERT_EQ(1u, needed.processChannelsPerPlane.size());
        EXPECT_EQ(bits(false, false, false, true), planeBits(needed, "depth"));
        EXPECT_EQ(bits(false, false, false, true), needed.processChannels);
    }

    project->reset(false, true);
}

TEST_F(LayerKnobsRenderTest, NodesWithoutLayerKnobKeepMetadataPlanes)
{
    NodePtr reader = createReader();
    ASSERT_TRUE(bool(reader));
    NodePtr premult = createNode(QString::fromUtf8("net.sf.openfx.Premult"));
    ASSERT_TRUE(bool(premult));
    ASSERT_FALSE(bool(premult->getLayerKnob()));
    connectNodes(reader, premult, 0, true);

    NeededComponents needed = queryNeededComponents(premult);

    std::vector<std::string> colorOnly;
    colorOnly.push_back(kNatronColorLayerID);
    EXPECT_EQ(colorOnly, outputLayerIDs(needed));
    EffectInstance::ComponentsNeededMap::const_iterator source = needed.comps.find(0);
    ASSERT_NE(needed.comps.end(), source);
    EXPECT_EQ(colorOnly, layerIDs(source->second));
    EXPECT_TRUE(needed.processChannelsPerPlane.empty());
    EXPECT_EQ(bits(true, true, true, true), premult->getEffectInstance()->getProcessChannelsForPlane(needed.hash, 0, ViewIdx(0), ImageLayerDesc::getRGBAComponents()));
}
