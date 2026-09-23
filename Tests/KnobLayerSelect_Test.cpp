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

#include <list>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobSerialization.h"
#include "Engine/Node.h"
#include "Engine/ProjectSerialization.h"

NATRON_NAMESPACE_USING

namespace {

KnobLayerSelectPtr
makeLayerSelectKnob(bool withChannelButtons = false)
{
    KnobLayerSelectPtr knob = std::make_shared<KnobLayerSelect>(static_cast<KnobHolder*>(NULL), std::string("layer"), 1, false);

    knob->populate();
    knob->setWithChannelButtons(withChannelButtons);

    return knob;
}

KnobChannelSelectPtr
makeChannelSelectKnob()
{
    KnobChannelSelectPtr knob = std::make_shared<KnobChannelSelect>(static_cast<KnobHolder*>(NULL), std::string("channel"), 1, false);

    knob->populate();

    return knob;
}

ImageLayerDesc
makeLayer(const std::string& name,
          const std::vector<std::string>& channels)
{
    return ImageLayerDesc(name, name, "", channels);
}

std::vector<std::string>
channels(const char* a,
         const char* b = 0,
         const char* c = 0,
         const char* d = 0)
{
    std::vector<std::string> out;

    out.push_back(a);
    if (b) {
        out.push_back(b);
    }
    if (c) {
        out.push_back(c);
    }
    if (d) {
        out.push_back(d);
    }

    return out;
}

std::list<ImageLayerDesc>
presentLayers()
{
    std::list<ImageLayerDesc> present;

    present.push_back(ImageLayerDesc::getRGBAComponents());
    present.push_back(makeLayer("diffuse", channels("R", "G", "B")));
    present.push_back(makeLayer("depth", channels("Z")));

    return present;
}

} // namespace

TEST(KnobLayerSelect, TypeAndColumns)
{
    KnobLayerSelectPtr knob = makeLayerSelectKnob();
    KnobIPtr asKnobI = knob;

    EXPECT_EQ(std::string("LayerSelect"), asKnobI->typeName());
    EXPECT_EQ(2, knob->getColumnsCount());
    EXPECT_EQ(std::string("Layer"), knob->getColumnTag(0));
    EXPECT_EQ(std::string("Channels"), knob->getColumnTag(1));
    EXPECT_FALSE(knob->isColumnEditable(0));
    EXPECT_FALSE(knob->useEditButton());
    EXPECT_FALSE(asKnobI->canAnimate());
}

TEST(KnobLayerSelect, DefaultIsColorAllChannels)
{
    KnobLayerSelectPtr knob = makeLayerSelectKnob();

    EXPECT_EQ(std::string(kNatronColorLayerID), knob->getLayer());
    EXPECT_TRUE(knob->getChannels().empty());
    EXPECT_EQ(std::string("Color"), knob->getSummary());

    ResolvedLayer resolved;
    ASSERT_TRUE(knob->resolve(presentLayers(), &resolved));
    EXPECT_EQ(std::string(kNatronColorLayerID), resolved.desc.getLayerID());
    EXPECT_EQ(std::bitset<4>(std::string("1111")), resolved.channels);
}

TEST(KnobLayerSelect, SettingChannelsThenChangingLayerResetsToAll)
{
    KnobLayerSelectPtr knob = makeLayerSelectKnob(true);

    knob->setChannels(channels("R"));
    EXPECT_EQ(channels("R"), knob->getChannels());

    knob->setLayer("diffuse");
    EXPECT_EQ(std::string("diffuse"), knob->getLayer());
    EXPECT_TRUE(knob->getChannels().empty());
}

TEST(KnobLayerSelect, WithoutButtonsSetChannelsThrowsAndAlwaysResolvesToAll)
{
    KnobLayerSelectPtr knob = makeLayerSelectKnob(false);

    EXPECT_FALSE(knob->getWithChannelButtons());
    EXPECT_THROW(knob->setChannels(channels("R")), std::invalid_argument);

    // setLayer is always legal (it is setChannels that requires buttons); this also
    // gives the knob a non-empty raw value so the Channels cell can be inspected.
    knob->setLayer(kNatronColorLayerID);

    const std::string raw = knob->getValue();
    EXPECT_NE(std::string::npos, raw.find("<Channels></Channels>"));

    ResolvedLayer resolved;
    ASSERT_TRUE(knob->resolve(presentLayers(), &resolved));
    EXPECT_EQ(std::bitset<4>(std::string("1111")), resolved.channels);
}

TEST(KnobLayerSelect, ResolveIntersectsPresentChannels)
{
    KnobLayerSelectPtr knob = makeLayerSelectKnob(true);

    knob->setLayer(kNatronColorLayerID);
    knob->setChannels(channels("R", "G", "Q"));

    ResolvedLayer resolved;
    ASSERT_TRUE(knob->resolve(presentLayers(), &resolved));
    EXPECT_EQ(std::string(kNatronColorLayerID), resolved.desc.getLayerID());
    EXPECT_EQ(std::bitset<4>(std::string("0011")), resolved.channels);
}

TEST(KnobLayerSelect, SummaryIsLabelDotInitialsForASubset)
{
    KnobLayerSelectPtr knob = makeLayerSelectKnob(true);

    knob->setLayer(kNatronColorLayerID);
    knob->setChannels(channels("R", "G"));
    EXPECT_EQ(std::string("Color.rg"), knob->getSummary());
}

TEST(KnobLayerSelect, ResolveAllChannelsOfALayer)
{
    KnobLayerSelectPtr knob = makeLayerSelectKnob(true);

    knob->setLayer("diffuse");

    ResolvedLayer resolved;
    ASSERT_TRUE(knob->resolve(presentLayers(), &resolved));
    EXPECT_EQ(std::bitset<4>(std::string("0111")), resolved.channels);
    EXPECT_EQ(std::string("diffuse"), knob->getSummary());
}

TEST(KnobLayerSelect, OneChannelLayerMapsToAlphaBit)
{
    KnobLayerSelectPtr knob = makeLayerSelectKnob(true);

    knob->setLayer("depth");

    ResolvedLayer resolved;
    ASSERT_TRUE(knob->resolve(presentLayers(), &resolved));
    EXPECT_EQ(std::bitset<4>(std::string("1000")), resolved.channels);
}

TEST(KnobLayerSelect, AbsentLayerResolvesToFalse)
{
    KnobLayerSelectPtr knob = makeLayerSelectKnob();

    knob->setLayer("missing");

    ResolvedLayer resolved;
    EXPECT_FALSE(knob->resolve(presentLayers(), &resolved));
}

TEST(KnobLayerSelect, GetReferencedLayerIDs)
{
    KnobLayerSelectPtr knob = makeLayerSelectKnob();

    knob->setLayer("diffuse");

    std::set<std::string> ids;
    knob->getReferencedLayerIDs(&ids);
    ASSERT_EQ(1u, ids.size());
    EXPECT_EQ(1u, ids.count("diffuse"));
}

TEST(KnobLayerSelect, AllowNoneFlag)
{
    KnobLayerSelectPtr knob = makeLayerSelectKnob();

    EXPECT_FALSE(knob->getAllowNone());
    knob->setAllowNone(true);
    EXPECT_TRUE(knob->getAllowNone());
}

TEST(KnobLayerSelect, SetLayerEmptyThrowsWhenNoneNotAllowed)
{
    KnobLayerSelectPtr knob = makeLayerSelectKnob();

    EXPECT_FALSE(knob->getAllowNone());
    EXPECT_THROW(knob->setLayer(""), std::invalid_argument);
}

TEST(KnobLayerSelect, SetLayerEmptyAllowedWhenNoneIsSet)
{
    KnobLayerSelectPtr knob = makeLayerSelectKnob();

    knob->setAllowNone(true);
    EXPECT_NO_THROW(knob->setLayer(""));
    EXPECT_EQ(std::string(""), knob->getLayer());
}

TEST(KnobLayerSelect, NoneResolvesToFalse)
{
    KnobLayerSelectPtr knob = makeLayerSelectKnob();

    knob->setAllowNone(true);
    knob->setLayer("");

    ResolvedLayer resolved;
    EXPECT_FALSE(knob->resolve(presentLayers(), &resolved));
}

TEST(KnobLayerSelect, NoneSummaryIsNone)
{
    KnobLayerSelectPtr knob = makeLayerSelectKnob();

    knob->setAllowNone(true);
    knob->setLayer("");

    EXPECT_EQ(std::string("None"), knob->getSummary());
}

TEST(KnobLayerSelect, NoneYieldsNoReferencedIDs)
{
    KnobLayerSelectPtr knob = makeLayerSelectKnob();

    knob->setAllowNone(true);
    knob->setLayer("");

    std::set<std::string> ids;
    knob->getReferencedLayerIDs(&ids);
    EXPECT_TRUE(ids.empty());
}

TEST_F(BaseTest, KnobLayerSelectNoneSerializationRoundTrip)
{
    NodePtr node = createNode(_generatorPluginID);

    ASSERT_TRUE(bool(node));

    KnobLayerSelectPtr source = AppManager::createKnob<KnobLayerSelect>(node->getEffectInstance().get(), std::string("layer"), 1, false);
    ASSERT_TRUE(bool(source));
    source->setAllowNone(true);
    source->setLayer("");

    std::stringstream buffer;
    {
        KnobSerialization serialization(source);
        boost::archive::xml_oarchive archive(buffer);
        archive << boost::serialization::make_nvp("Knob", serialization);
    }

    KnobSerialization loaded;
    {
        boost::archive::xml_iarchive archive(buffer);
        archive >> boost::serialization::make_nvp("Knob", loaded);
    }

    KnobIPtr loadedKnob = loaded.getKnob();
    ASSERT_TRUE(bool(loadedKnob));
    EXPECT_EQ(std::string("LayerSelect"), loadedKnob->typeName());

    KnobLayerSelectPtr loadedSelect = std::dynamic_pointer_cast<KnobLayerSelect>(loadedKnob);
    ASSERT_TRUE(bool(loadedSelect));
    EXPECT_EQ(std::string(""), loadedSelect->getLayer());
    EXPECT_EQ(source->getValue(), loadedSelect->getValue());
}

TEST_F(BaseTest, KnobLayerSelectSerializationRoundTrip)
{
    NodePtr node = createNode(_generatorPluginID);

    ASSERT_TRUE(bool(node));

    KnobLayerSelectPtr source = AppManager::createKnob<KnobLayerSelect>(node->getEffectInstance().get(), std::string("layer"), 1, false);
    ASSERT_TRUE(bool(source));
    source->setWithChannelButtons(true);
    source->setLayer("diffuse");
    source->setChannels(channels("R", "G"));

    std::stringstream buffer;
    {
        KnobSerialization serialization(source);
        boost::archive::xml_oarchive archive(buffer);
        archive << boost::serialization::make_nvp("Knob", serialization);
    }

    KnobSerialization loaded;
    {
        boost::archive::xml_iarchive archive(buffer);
        archive >> boost::serialization::make_nvp("Knob", loaded);
    }

    KnobIPtr loadedKnob = loaded.getKnob();
    ASSERT_TRUE(bool(loadedKnob));
    EXPECT_EQ(std::string("LayerSelect"), loadedKnob->typeName());

    KnobLayerSelectPtr loadedSelect = std::dynamic_pointer_cast<KnobLayerSelect>(loadedKnob);
    ASSERT_TRUE(bool(loadedSelect));
    EXPECT_EQ(std::string("diffuse"), loadedSelect->getLayer());
    EXPECT_EQ(channels("R", "G"), loadedSelect->getChannels());
    EXPECT_EQ(source->getValue(), loadedSelect->getValue());
}

TEST(KnobChannelSelect, TypeAndColumns)
{
    KnobChannelSelectPtr knob = makeChannelSelectKnob();
    KnobIPtr asKnobI = knob;

    EXPECT_EQ(std::string("ChannelSelect"), asKnobI->typeName());
    EXPECT_EQ(1, knob->getColumnsCount());
    EXPECT_EQ(std::string("Channel"), knob->getColumnTag(0));
    EXPECT_FALSE(knob->isColumnEditable(0));
    EXPECT_FALSE(knob->useEditButton());
    EXPECT_FALSE(asKnobI->canAnimate());
}

TEST(KnobChannelSelect, DefaultIsColorAlpha)
{
    KnobChannelSelectPtr knob = makeChannelSelectKnob();

    EXPECT_EQ(std::string(kNatronColorLayerID) + ".A", knob->get());
    EXPECT_FALSE(knob->isNone());
    EXPECT_EQ(std::string("Color.A"), knob->getSummary());

    ImageLayerDesc layer;
    int channelIndex = -1;
    ASSERT_TRUE(knob->resolve(presentLayers(), &layer, &channelIndex));
    EXPECT_EQ(std::string(kNatronColorLayerID), layer.getLayerID());
    EXPECT_EQ(3, channelIndex);
}

TEST(KnobChannelSelect, ResolvesByLayerAndChannelName)
{
    KnobChannelSelectPtr knob = makeChannelSelectKnob();

    knob->set("diffuse.G");
    ImageLayerDesc layer;
    int channelIndex = -1;
    ASSERT_TRUE(knob->resolve(presentLayers(), &layer, &channelIndex));
    EXPECT_EQ(std::string("diffuse"), layer.getLayerID());
    EXPECT_EQ(1, channelIndex);
    EXPECT_EQ(std::string("diffuse.G"), knob->getSummary());

    knob->set("depth.Z");
    ASSERT_TRUE(knob->resolve(presentLayers(), &layer, &channelIndex));
    EXPECT_EQ(std::string("depth"), layer.getLayerID());
    EXPECT_EQ(0, channelIndex);
}

TEST(KnobChannelSelect, UnknownLayerOrChannelResolvesToFalse)
{
    KnobChannelSelectPtr knob = makeChannelSelectKnob();

    knob->set("missing.R");
    ImageLayerDesc layer;
    int channelIndex = -1;
    EXPECT_FALSE(knob->resolve(presentLayers(), &layer, &channelIndex));

    knob->set("diffuse.Q");
    EXPECT_FALSE(knob->resolve(presentLayers(), &layer, &channelIndex));
}

TEST(KnobChannelSelect, SetNoneAndIsNone)
{
    KnobChannelSelectPtr knob = makeChannelSelectKnob();

    knob->setNone();
    EXPECT_TRUE(knob->isNone());
    EXPECT_TRUE(knob->get().empty());
    EXPECT_EQ(std::string("None"), knob->getSummary());

    ImageLayerDesc layer;
    int channelIndex = -1;
    EXPECT_FALSE(knob->resolve(presentLayers(), &layer, &channelIndex));

    knob->set("diffuse.R");
    EXPECT_FALSE(knob->isNone());
}

TEST(KnobChannelSelect, GetReferencedLayerIDs)
{
    KnobChannelSelectPtr knob = makeChannelSelectKnob();

    knob->set("diffuse.R");

    std::set<std::string> ids;
    knob->getReferencedLayerIDs(&ids);
    ASSERT_EQ(1u, ids.size());
    EXPECT_EQ(1u, ids.count("diffuse"));

    knob->setNone();
    ids.clear();
    knob->getReferencedLayerIDs(&ids);
    EXPECT_TRUE(ids.empty());
}

TEST_F(BaseTest, KnobChannelSelectSerializationRoundTrip)
{
    NodePtr node = createNode(_generatorPluginID);

    ASSERT_TRUE(bool(node));

    KnobChannelSelectPtr source = AppManager::createKnob<KnobChannelSelect>(node->getEffectInstance().get(), std::string("channel"), 1, false);
    ASSERT_TRUE(bool(source));
    source->set("diffuse.G");

    std::stringstream buffer;
    {
        KnobSerialization serialization(source);
        boost::archive::xml_oarchive archive(buffer);
        archive << boost::serialization::make_nvp("Knob", serialization);
    }

    KnobSerialization loaded;
    {
        boost::archive::xml_iarchive archive(buffer);
        archive >> boost::serialization::make_nvp("Knob", loaded);
    }

    KnobIPtr loadedKnob = loaded.getKnob();
    ASSERT_TRUE(bool(loadedKnob));
    EXPECT_EQ(std::string("ChannelSelect"), loadedKnob->typeName());

    KnobChannelSelectPtr loadedSelect = std::dynamic_pointer_cast<KnobChannelSelect>(loadedKnob);
    ASSERT_TRUE(bool(loadedSelect));
    EXPECT_EQ(std::string("diffuse.G"), loadedSelect->get());
    EXPECT_EQ(source->getValue(), loadedSelect->getValue());
}
