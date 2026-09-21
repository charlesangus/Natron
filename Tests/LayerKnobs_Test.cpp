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
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/LayerRegistry.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/TrackerContext.h"

#include <ofxImageEffect.h>
#include <ofxNatron.h>

NATRON_NAMESPACE_USING

static bool
isFirstOnItsPage(const KnobIPtr& knob)
{
    KnobPagePtr page = std::dynamic_pointer_cast<KnobPage>(knob->getParentKnob());

    if (!page) {
        return false;
    }
    KnobsVec children = page->getChildren();

    return !children.empty() && children[0] == knob;
}

static std::vector<std::string>
layerIDs(const std::list<ImageLayerDesc>& layers)
{
    std::vector<std::string> ids;

    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        ids.push_back(it->getLayerID());
    }

    return ids;
}

TEST_F(BaseTest, GradeGetsChannelSetSeededByItsQuad)
{
    NodePtr grade = createNode(QString::fromUtf8("net.sf.openfx.GradePlugin"));

    ASSERT_TRUE(bool(grade));

    KnobChannelSetPtr channels = std::dynamic_pointer_cast<KnobChannelSet>(grade->getKnobByName(kNodeParamChannelSet));
    ASSERT_TRUE(bool(channels));
    EXPECT_EQ(channels, grade->getLayerKnob());
    EXPECT_TRUE(isFirstOnItsPage(channels));
    EXPECT_FALSE(channels->isAnimationEnabled());

    KnobBoolPtr processA = std::dynamic_pointer_cast<KnobBool>(grade->getKnobByName(kNatronOfxParamProcessA));
    ASSERT_TRUE(bool(processA));
    EXPECT_FALSE(processA->getDefaultValue(0));
    EXPECT_TRUE(processA->getIsSecret());
    EXPECT_TRUE(processA->getValue());
    EXPECT_FALSE(processA->getIsPersistent());

    std::vector<ChannelSetRow> rows = channels->getRows();
    ASSERT_EQ(1u, rows.size());
    EXPECT_EQ(ChannelSetRow::eModeLayer, rows[0].mode);
    EXPECT_EQ(std::string(kNatronColorLayerID), rows[0].layerOrPattern);
    std::vector<std::string> rgb;
    rgb.push_back("R");
    rgb.push_back("G");
    rgb.push_back("B");
    EXPECT_EQ(rgb, rows[0].channels);

    KnobIPtr legacy = grade->getKnobByName(kOutputChannelsKnobName);
    ASSERT_TRUE(bool(legacy));
    EXPECT_TRUE(legacy->getIsSecret());
    EXPECT_FALSE(legacy->getIsPersistent());
}

TEST_F(BaseTest, InvertGetsChannelSetWithEveryChannel)
{
    NodePtr invert = createNode(QString::fromUtf8("net.sf.openfx.Invert"));

    ASSERT_TRUE(bool(invert));

    KnobChannelSetPtr channels = std::dynamic_pointer_cast<KnobChannelSet>(invert->getKnobByName(kNodeParamChannelSet));
    ASSERT_TRUE(bool(channels));
    EXPECT_TRUE(isFirstOnItsPage(channels));

    std::vector<ChannelSetRow> rows = channels->getRows();
    ASSERT_EQ(1u, rows.size());
    EXPECT_EQ(std::string(kNatronColorLayerID), rows[0].layerOrPattern);

    std::list<ImageLayerDesc> present;
    invert->listLayersForKnob(channels, &present);
    std::vector<ResolvedLayer> resolved = channels->resolve(present);
    ASSERT_EQ(1u, resolved.size());
    EXPECT_TRUE(resolved[0].desc.isColorLayer());
    EXPECT_TRUE(resolved[0].channels[0] && resolved[0].channels[1] && resolved[0].channels[2] && resolved[0].channels[3]);
}

TEST_F(BaseTest, ConstantGetsTargetLayerSelectListingTheRegistry)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    NodePtr constant = createNode(QString::fromUtf8("net.sf.openfx.ConstantPlugin"));
    ASSERT_TRUE(bool(constant));

    EXPECT_FALSE(bool(constant->getKnobByName(kNodeParamChannelSet)));
    KnobLayerSelectPtr layer = std::dynamic_pointer_cast<KnobLayerSelect>(constant->getKnobByName(kNodeParamLayerSelect));
    ASSERT_TRUE(bool(layer));
    EXPECT_EQ(layer, constant->getLayerKnob());
    EXPECT_TRUE(isFirstOnItsPage(layer));
    EXPECT_TRUE(layer->getWithChannelButtons());
    EXPECT_EQ(std::string(kNatronColorLayerID), layer->getLayer());

    std::list<ImageLayerDesc> listed;
    constant->listLayersForKnob(layer, &listed);
    std::vector<std::string> ids = layerIDs(listed);
    ASSERT_GE(ids.size(), 6u);
    EXPECT_EQ(std::string(kNatronColorLayerID), ids[0]);
    EXPECT_EQ(std::string(kNatronDisparityLeftLayerID), ids[1]);
    EXPECT_EQ(std::string(kNatronDisparityRightLayerID), ids[2]);
    EXPECT_EQ(std::string(kNatronBackwardMotionVectorsLayerID), ids[3]);
    EXPECT_EQ(std::string(kNatronForwardMotionVectorsLayerID), ids[4]);
    EXPECT_NE(ids.end(), std::find(ids.begin(), ids.end(), std::string("depth")));

    project->reset(false, true);
}

TEST_F(BaseTest, BlurMaskChannelSelectDefaultsToColorAlpha)
{
    NodePtr blur = createNode(QString::fromUtf8("net.sf.cimg.CImgBlur"));

    ASSERT_TRUE(bool(blur));

    KnobChannelSelectPtr maskChannel = std::dynamic_pointer_cast<KnobChannelSelect>(blur->getKnobByName(std::string(kMaskChannelKnobName) + "_Mask"));
    ASSERT_TRUE(bool(maskChannel));
    EXPECT_FALSE(maskChannel->isNone());

    std::list<ImageLayerDesc> present;
    blur->listLayersForKnob(maskChannel, &present);
    ASSERT_FALSE(present.empty());

    ImageLayerDesc layer;
    int channelIndex = -1;
    ASSERT_TRUE(maskChannel->resolve(present, &layer, &channelIndex));
    EXPECT_TRUE(layer.isColorLayer());
    EXPECT_EQ(3, channelIndex);

    ImageLayerDesc maskComps;
    EXPECT_EQ(3, blur->getMaskChannel(1, present, &maskComps));
    EXPECT_TRUE(maskComps.isColorLayer());
}

TEST_F(BaseTest, ChannelSetListsPresentLayersOfPreferredInput)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    NodePtr blur = createNode(QString::fromUtf8("net.sf.cimg.CImgBlur"));
    ASSERT_TRUE(bool(blur));
    KnobChannelSetPtr channels = std::dynamic_pointer_cast<KnobChannelSet>(blur->getKnobByName(kNodeParamChannelSet));
    ASSERT_TRUE(bool(channels));

    {
        std::list<ImageLayerDesc> listed;
        blur->listLayersForKnob(channels, &listed);
        std::vector<std::string> ids = layerIDs(listed);
        ASSERT_EQ(1u, ids.size());
        EXPECT_EQ(std::string(kNatronColorLayerID), ids[0]);
    }

    CreateNodeArgs readerArgs(_readOIIOPluginID.toStdString(), project);
    readerArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/flat-three-layers.exr"));
    NodePtr reader = getApp()->createNode(readerArgs);
    ASSERT_TRUE(bool(reader));

    connectNodes(reader, blur, 0, true);

    {
        std::list<ImageLayerDesc> listed;
        blur->listLayersForKnob(channels, &listed);
        std::vector<std::string> ids = layerIDs(listed);
        std::vector<std::string> expected;
        expected.push_back(kNatronColorLayerID);
        expected.push_back("diffuse");
        expected.push_back("specular");
        EXPECT_EQ(expected, ids);
    }

    channels->addLayer("diffuse", 0);

    std::list<NodePtr> users;
    project->getLayerUsers("diffuse", &users);
    EXPECT_NE(users.end(), std::find(users.begin(), users.end(), blur));

    project->reset(false, true);
}

TEST_F(BaseTest, TrackerGetsInputBoundLayerSelectWithoutButtons)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    NodePtr tracker = createNode(QString::fromUtf8(PLUGINID_NATRON_TRACKER));
    ASSERT_TRUE(bool(tracker));

    EXPECT_FALSE(bool(tracker->getKnobByName(kNodeParamChannelSet)));
    EXPECT_FALSE(bool(tracker->getKnobByName("trackRed")));
    EXPECT_FALSE(bool(tracker->getKnobByName("trackGreen")));
    EXPECT_FALSE(bool(tracker->getKnobByName("trackBlue")));

    KnobLayerSelectPtr layer = std::dynamic_pointer_cast<KnobLayerSelect>(tracker->getKnobByName(kNodeParamLayerSelect));
    ASSERT_TRUE(bool(layer));
    EXPECT_EQ(layer, tracker->getLayerKnob());
    EXPECT_FALSE(tracker->isTargetLayerKnob(layer));
    EXPECT_FALSE(layer->getWithChannelButtons());
    EXPECT_EQ(std::string(kNatronColorLayerID), layer->getLayer());

    EXPECT_TRUE(isFirstOnItsPage(layer));
    KnobPagePtr page = std::dynamic_pointer_cast<KnobPage>(layer->getParentKnob());
    ASSERT_TRUE(bool(page));
    EXPECT_EQ(page, tracker->getTrackerContext()->getTrackingPageKnob());

    CreateNodeArgs readerArgs(_readOIIOPluginID.toStdString(), project);
    readerArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/flat-three-layers.exr"));
    NodePtr reader = getApp()->createNode(readerArgs);
    ASSERT_TRUE(bool(reader));
    connectNodes(reader, tracker, 0, true);

    std::list<ImageLayerDesc> listed;
    tracker->listLayersForKnob(layer, &listed);
    std::vector<std::string> expected;
    expected.push_back(kNatronColorLayerID);
    expected.push_back("diffuse");
    expected.push_back("specular");
    EXPECT_EQ(expected, layerIDs(listed));

    project->reset(false, true);
}

TEST_F(BaseTest, RotoAndRotoPaintGetTargetLayerSelectWithButtons)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    struct Case {
        const char* pluginID;
        std::vector<std::string> defaultChannels;
    };
    Case cases[2];
    cases[0].pluginID = PLUGINID_NATRON_ROTOPAINT;
    cases[1].pluginID = PLUGINID_NATRON_ROTO;
    cases[1].defaultChannels.push_back("A");

    for (std::size_t i = 0; i < 2; ++i) {
        NodePtr roto = createNode(QString::fromUtf8(cases[i].pluginID));
        ASSERT_TRUE(bool(roto)) << cases[i].pluginID;

        EXPECT_FALSE(bool(roto->getKnobByName(kNodeParamChannelSet))) << cases[i].pluginID;
        EXPECT_FALSE(bool(roto->getKnobByName(kNatronOfxParamProcessR))) << cases[i].pluginID;
        EXPECT_FALSE(bool(roto->getKnobByName(kNatronOfxParamProcessG))) << cases[i].pluginID;
        EXPECT_FALSE(bool(roto->getKnobByName(kNatronOfxParamProcessB))) << cases[i].pluginID;
        EXPECT_FALSE(bool(roto->getKnobByName(kNatronOfxParamProcessA))) << cases[i].pluginID;

        KnobLayerSelectPtr layer = std::dynamic_pointer_cast<KnobLayerSelect>(roto->getKnobByName(kNodeParamLayerSelect));
        ASSERT_TRUE(bool(layer)) << cases[i].pluginID;
        EXPECT_EQ(layer, roto->getLayerKnob());
        EXPECT_TRUE(roto->isTargetLayerKnob(layer)) << cases[i].pluginID;
        EXPECT_TRUE(layer->getWithChannelButtons()) << cases[i].pluginID;
        EXPECT_TRUE(isFirstOnItsPage(layer)) << cases[i].pluginID;
        EXPECT_EQ(std::string(kNatronColorLayerID), layer->getLayer()) << cases[i].pluginID;
        EXPECT_EQ(cases[i].defaultChannels, layer->getChannels()) << cases[i].pluginID;

        std::list<ImageLayerDesc> listed;
        roto->listLayersForKnob(layer, &listed);
        std::vector<std::string> ids = layerIDs(listed);
        ASSERT_GE(ids.size(), 6u) << cases[i].pluginID;
        EXPECT_EQ(std::string(kNatronColorLayerID), ids[0]);
        EXPECT_NE(ids.end(), std::find(ids.begin(), ids.end(), std::string("depth")));
    }

    project->reset(false, true);
}

TEST_F(BaseTest, NodesOwningTheirPlanesGetNoLayerKnob)
{
    const char* ids[] = {
        "fr.natron.DeepMerge",
        "net.sf.openfx.ShufflePlugin",
        "net.sf.openfx.Premult"
    };

    for (std::size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
        NodePtr node = createNode(QString::fromUtf8(ids[i]));
        ASSERT_TRUE(bool(node)) << ids[i];
        EXPECT_FALSE(bool(node->getLayerKnob())) << ids[i];
        EXPECT_FALSE(bool(node->getKnobByName(kNodeParamChannelSet))) << ids[i];
        EXPECT_FALSE(bool(node->getKnobByName(kNodeParamLayerSelect))) << ids[i];
    }
}
