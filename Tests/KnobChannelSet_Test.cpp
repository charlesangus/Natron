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
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobSerialization.h"
#include "Engine/Node.h"
#include "Engine/ProjectSerialization.h"

NATRON_NAMESPACE_USING

namespace {

KnobChannelSetPtr
makeKnob()
{
    KnobChannelSetPtr knob = std::make_shared<KnobChannelSet>(static_cast<KnobHolder*>(NULL), std::string("channels"), 1, false);

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
    present.push_back(makeLayer("motion", channels("X", "Y")));
    present.push_back(makeLayer("depth", channels("Z")));
    present.push_back(makeLayer("specular", channels("R", "G", "B", "A")));

    return present;
}

} // namespace

TEST(KnobChannelSet, TypeAndColumns)
{
    KnobChannelSetPtr knob = makeKnob();
    KnobIPtr asKnobI = knob;

    EXPECT_EQ(std::string("ChannelSet"), asKnobI->typeName());
    EXPECT_EQ(3, knob->getColumnsCount());
    EXPECT_EQ(std::string("Mode"), knob->getColumnTag(0));
    EXPECT_EQ(std::string("Layer"), knob->getColumnTag(1));
    EXPECT_EQ(std::string("Channels"), knob->getColumnTag(2));
    EXPECT_FALSE(knob->isColumnEditable(0));
    EXPECT_FALSE(knob->useEditButton());
    EXPECT_FALSE(asKnobI->canAnimate());
}

TEST(KnobChannelSet, DefaultRowIsRgbaAllChannels)
{
    KnobChannelSetPtr knob = makeKnob();
    std::vector<ChannelSetRow> rows = knob->getRows();

    ASSERT_EQ(1u, rows.size());
    EXPECT_EQ(ChannelSetRow::eModeLayer, rows[0].mode);
    EXPECT_EQ(std::string(kNatronColorViewRGBA), rows[0].layerOrPattern);
    EXPECT_TRUE(rows[0].channels.empty());
}

TEST(KnobChannelSet, DefaultResolvesToTheFullColorPlane)
{
    KnobChannelSetPtr knob = makeKnob();

    std::vector<ResolvedLayer> resolved = knob->resolve(presentLayers());
    ASSERT_EQ(1u, resolved.size());
    EXPECT_EQ(std::string(kNatronColorLayerID), resolved[0].desc.getLayerID());
    EXPECT_EQ(4, resolved[0].desc.getNumComponents());
    EXPECT_EQ(std::bitset<4>(std::string("1111")), resolved[0].channels);
    EXPECT_TRUE(resolved[0].zeroChannels.none());
}

TEST(KnobChannelSet, DefaultOverRGBStorageReportsAlphaAsReadingZero)
{
    KnobChannelSetPtr knob = makeKnob();
    std::list<ImageLayerDesc> present;

    present.push_back(ImageLayerDesc::getRGBComponents());
    std::vector<ResolvedLayer> resolved = knob->resolve(present);
    ASSERT_EQ(1u, resolved.size());
    EXPECT_EQ(std::string(kNatronColorLayerID), resolved[0].desc.getLayerID());
    EXPECT_EQ(3, resolved[0].desc.getNumComponents());
    EXPECT_EQ(std::bitset<4>(std::string("1111")), resolved[0].channels);
    EXPECT_EQ(std::bitset<4>(std::string("1000")), resolved[0].zeroChannels);
}

TEST(KnobChannelSet, LegacyColorDefaultRowsNameTheStorageID)
{
    std::vector<ChannelSetRow> rows = KnobChannelSet::legacyColorDefaultRows();

    ASSERT_EQ(1u, rows.size());
    EXPECT_EQ(ChannelSetRow::eModeLayer, rows[0].mode);
    EXPECT_EQ(std::string(kNatronColorLayerID), rows[0].layerOrPattern);
    EXPECT_TRUE(rows[0].channels.empty());
}

TEST(KnobChannelSet, RgbViewResolvesToBitsZeroToTwoAndAlphaToBitThree)
{
    KnobChannelSetPtr knob = makeKnob();

    knob->setLayer(0, kNatronColorViewRGB, 0);
    std::vector<ResolvedLayer> resolved = knob->resolve(presentLayers());
    ASSERT_EQ(1u, resolved.size());
    EXPECT_EQ(std::string(kNatronColorLayerID), resolved[0].desc.getLayerID());
    EXPECT_EQ(std::bitset<4>(std::string("0111")), resolved[0].channels);
    EXPECT_TRUE(resolved[0].zeroChannels.none());

    knob->setLayer(0, kNatronColorViewAlpha, 0);
    resolved = knob->resolve(presentLayers());
    ASSERT_EQ(1u, resolved.size());
    EXPECT_EQ(std::string(kNatronColorLayerID), resolved[0].desc.getLayerID());
    EXPECT_EQ(std::bitset<4>(std::string("1000")), resolved[0].channels);
    EXPECT_TRUE(resolved[0].zeroChannels.none());
}

TEST(KnobChannelSet, RgbAndAlphaRowsResolveToOneColorEntry)
{
    KnobChannelSetPtr knob = makeKnob();

    knob->setLayer(0, kNatronColorViewRGB, 0);
    knob->addLayer(kNatronColorViewAlpha, 0);
    knob->addLayer("depth", 0);

    std::vector<ResolvedLayer> resolved = knob->resolve(presentLayers());
    ASSERT_EQ(2u, resolved.size());
    EXPECT_EQ(std::string(kNatronColorLayerID), resolved[0].desc.getLayerID());
    EXPECT_EQ(std::bitset<4>(std::string("1111")), resolved[0].channels);
    EXPECT_EQ(std::string("depth"), resolved[1].desc.getLayerID());
}

TEST(KnobChannelSet, StorageIDRowResolvesToNothing)
{
    KnobChannelSetPtr knob = makeKnob();

    knob->setLayer(0, kNatronColorLayerID, 0);
    EXPECT_TRUE(knob->resolve(presentLayers()).empty());

    std::vector<std::string> red = channels("R");
    knob->setLayer(0, kNatronColorLayerID, &red);
    EXPECT_TRUE(knob->resolve(presentLayers()).empty());
}

TEST(KnobChannelSet, ViewRowWithoutColorStorageResolvesToNothing)
{
    KnobChannelSetPtr knob = makeKnob();
    std::list<ImageLayerDesc> present;

    present.push_back(makeLayer("depth", channels("Z")));
    EXPECT_TRUE(knob->resolve(present).empty());
}

TEST(KnobChannelSet, ExplicitViewOverMissingChannelsReportsZeroBits)
{
    KnobChannelSetPtr knob = makeKnob();
    std::list<ImageLayerDesc> rgb;
    std::list<ImageLayerDesc> alpha;

    rgb.push_back(ImageLayerDesc::getRGBComponents());
    alpha.push_back(ImageLayerDesc::getAlphaComponents());

    knob->setLayer(0, kNatronColorViewAlpha, 0);
    std::vector<ResolvedLayer> resolved = knob->resolve(rgb);
    ASSERT_EQ(1u, resolved.size());
    EXPECT_EQ(std::bitset<4>(std::string("1000")), resolved[0].channels);
    EXPECT_EQ(std::bitset<4>(std::string("1000")), resolved[0].zeroChannels);

    knob->setLayer(0, kNatronColorViewRGB, 0);
    resolved = knob->resolve(alpha);
    ASSERT_EQ(1u, resolved.size());
    EXPECT_EQ(1, resolved[0].desc.getNumComponents());
    EXPECT_EQ(std::bitset<4>(std::string("0111")), resolved[0].channels);
    EXPECT_EQ(std::bitset<4>(std::string("0111")), resolved[0].zeroChannels);
    EXPECT_FALSE(resolved[0].isChannelSelected(0));

    std::vector<std::string> rg = channels("R", "G");
    knob->setLayer(0, kNatronColorViewRGBA, &rg);
    resolved = knob->resolve(rgb);
    ASSERT_EQ(1u, resolved.size());
    EXPECT_EQ(std::bitset<4>(std::string("0011")), resolved[0].channels);
    EXPECT_TRUE(resolved[0].zeroChannels.none());
}

TEST(KnobChannelSet, XYViewAliasesTheFirstTwoColorBits)
{
    KnobChannelSetPtr knob = makeKnob();
    std::list<ImageLayerDesc> xy;

    xy.push_back(ImageLayerDesc::getXYComponents());

    knob->setLayer(0, kNatronColorViewXY, 0);
    std::vector<ResolvedLayer> resolved = knob->resolve(xy);
    ASSERT_EQ(1u, resolved.size());
    EXPECT_EQ(std::string(kNatronColorLayerID), resolved[0].desc.getLayerID());
    EXPECT_EQ(std::bitset<4>(std::string("0011")), resolved[0].channels);
    EXPECT_TRUE(resolved[0].zeroChannels.none());

    resolved = knob->resolve(presentLayers());
    ASSERT_EQ(1u, resolved.size());
    EXPECT_EQ(std::bitset<4>(std::string("0011")), resolved[0].channels);
}

TEST(KnobChannelSet, RegexMatchingSeveralViewsYieldsOneColorEntry)
{
    KnobChannelSetPtr knob = makeKnob();

    // Patterns are anchored to the whole label, so "rgb.*" is the form that matches both
    // rgba and rgb.
    knob->setRegex(0, "rgb.*");
    std::vector<ResolvedLayer> resolved = knob->resolve(presentLayers());
    ASSERT_EQ(1u, resolved.size());
    EXPECT_EQ(std::string(kNatronColorLayerID), resolved[0].desc.getLayerID());
    EXPECT_EQ(std::bitset<4>(std::string("1111")), resolved[0].channels);
    EXPECT_EQ(std::string("rgba"), knob->getSummary(presentLayers()));

    knob->setRegex(0, "rgb");
    resolved = knob->resolve(presentLayers());
    ASSERT_EQ(1u, resolved.size());
    EXPECT_EQ(std::bitset<4>(std::string("0111")), resolved[0].channels);
    EXPECT_EQ(std::string("rgb"), knob->getSummary(presentLayers()));

    knob->setRegex(0, "rgb|alpha");
    resolved = knob->resolve(presentLayers());
    ASSERT_EQ(1u, resolved.size());
    EXPECT_EQ(std::bitset<4>(std::string("1111")), resolved[0].channels);
    EXPECT_EQ(std::string("rgb"), knob->getSummary(presentLayers()));
}

TEST(KnobChannelSet, RegexMatchesViewLabelsNotTheStorageLabel)
{
    KnobChannelSetPtr knob = makeKnob();

    knob->setRegex(0, "Color");
    EXPECT_TRUE(knob->resolve(presentLayers()).empty());
    EXPECT_EQ(std::string("(no match)"), knob->getSummary(presentLayers()));

    knob->setRegex(0, ".*");
    std::vector<ResolvedLayer> resolved = knob->resolve(presentLayers());
    ASSERT_EQ(5u, resolved.size());
    EXPECT_EQ(std::string(kNatronColorLayerID), resolved[0].desc.getLayerID());
    EXPECT_EQ(std::bitset<4>(std::string("1111")), resolved[0].channels);
    EXPECT_EQ(std::string("rgba, diffuse, motion, depth, specular"), knob->getSummary(presentLayers()));
}

TEST(KnobChannelSet, RegexExcludedChannelsApplyToColorViews)
{
    KnobChannelSetPtr knob = makeKnob();

    knob->setRegex(0, "rgba");
    knob->setExcludedChannels(0, channels("G"));

    std::vector<ResolvedLayer> resolved = knob->resolve(presentLayers());
    ASSERT_EQ(1u, resolved.size());
    EXPECT_EQ(std::bitset<4>(std::string("1101")), resolved[0].channels);
    EXPECT_EQ(std::string("rgba.rba"), knob->getSummary(presentLayers()));
}

TEST(KnobChannelSet, AllAndRegexNeverWidenColor)
{
    KnobChannelSetPtr knob = makeKnob();
    std::list<ImageLayerDesc> rgb;

    rgb.push_back(ImageLayerDesc::getRGBComponents());

    knob->setAll();
    std::vector<ResolvedLayer> resolved = knob->resolve(rgb);
    ASSERT_EQ(1u, resolved.size());
    EXPECT_EQ(std::bitset<4>(std::string("0111")), resolved[0].channels);
    EXPECT_TRUE(resolved[0].zeroChannels.none());

    knob->setRegex(0, "rgba");
    resolved = knob->resolve(rgb);
    ASSERT_EQ(1u, resolved.size());
    EXPECT_EQ(std::bitset<4>(std::string("0111")), resolved[0].channels);
    EXPECT_TRUE(resolved[0].zeroChannels.none());

    knob->setRegex(0, "alpha");
    EXPECT_TRUE(knob->resolve(rgb).empty());
}

TEST(KnobChannelSet, NoneResolvesToNothing)
{
    KnobChannelSetPtr knob = makeKnob();

    knob->setNone();
    EXPECT_TRUE(knob->resolve(presentLayers()).empty());
    EXPECT_EQ(std::string("None"), knob->getSummary());
}

TEST(KnobChannelSet, AllResolvesEveryPresentLayerWithAllBits)
{
    KnobChannelSetPtr knob = makeKnob();

    knob->setAll();
    EXPECT_EQ(std::string("All"), knob->getSummary());

    std::vector<ResolvedLayer> resolved = knob->resolve(presentLayers());
    ASSERT_EQ(5u, resolved.size());
    EXPECT_EQ(std::string(kNatronColorLayerID), resolved[0].desc.getLayerID());
    EXPECT_EQ(std::bitset<4>(std::string("1111")), resolved[0].channels);
    EXPECT_EQ(std::string("diffuse"), resolved[1].desc.getLayerID());
    EXPECT_EQ(std::bitset<4>(std::string("0111")), resolved[1].channels);
    EXPECT_EQ(std::string("motion"), resolved[2].desc.getLayerID());
    EXPECT_EQ(std::bitset<4>(std::string("0011")), resolved[2].channels);
    EXPECT_EQ(std::string("depth"), resolved[3].desc.getLayerID());
    EXPECT_EQ(std::bitset<4>(std::string("1000")), resolved[3].channels);
    EXPECT_EQ(std::string("specular"), resolved[4].desc.getLayerID());
    EXPECT_EQ(std::bitset<4>(std::string("1111")), resolved[4].channels);
}

TEST(KnobChannelSet, AbsentLayerContributesNothing)
{
    KnobChannelSetPtr knob = makeKnob();

    knob->setLayer(0, "missing", 0);
    EXPECT_TRUE(knob->resolve(presentLayers()).empty());
}

TEST(KnobChannelSet, LayerRowIntersectsPresentChannels)
{
    KnobChannelSetPtr knob = makeKnob();
    std::vector<std::string> wanted = channels("R", "G", "Q");

    knob->setLayer(0, kNatronColorViewRGBA, &wanted);

    std::vector<ResolvedLayer> resolved = knob->resolve(presentLayers());
    ASSERT_EQ(1u, resolved.size());
    EXPECT_EQ(std::string(kNatronColorLayerID), resolved[0].desc.getLayerID());
    EXPECT_EQ(std::bitset<4>(std::string("0011")), resolved[0].channels);
}

TEST(KnobChannelSet, EmptyChannelListAfterSetChannelsContributesNothing)
{
    KnobChannelSetPtr knob = makeKnob();
    std::vector<std::string> wanted = channels("R");

    knob->setLayer(0, kNatronColorViewRGBA, &wanted);
    ASSERT_EQ(1u, knob->resolve(presentLayers()).size());

    std::vector<std::string> none;
    knob->setChannels(0, none);
    ASSERT_EQ(1u, knob->getRows().size());
    EXPECT_TRUE(knob->getRows()[0].channels.empty());
}

TEST(KnobChannelSet, SetChannelsOnAllRowZeroFallsBackToColor)
{
    KnobChannelSetPtr knob = makeKnob();

    knob->setAll();
    knob->setChannels(0, channels("R", "G", "B", "A"));

    std::vector<ChannelSetRow> rows = knob->getRows();
    ASSERT_EQ(1u, rows.size());
    EXPECT_EQ(ChannelSetRow::eModeLayer, rows[0].mode);
    EXPECT_EQ(std::string(kNatronColorViewRGBA), rows[0].layerOrPattern);
    EXPECT_EQ(channels("R", "G", "B", "A"), rows[0].channels);
}

TEST(KnobChannelSet, SetChannelsOnNoneRowZeroFallsBackToColor)
{
    KnobChannelSetPtr knob = makeKnob();

    knob->setNone();
    knob->setChannels(0, channels("A"));

    std::vector<ChannelSetRow> rows = knob->getRows();
    ASSERT_EQ(1u, rows.size());
    EXPECT_EQ(ChannelSetRow::eModeLayer, rows[0].mode);
    EXPECT_EQ(std::string(kNatronColorViewRGBA), rows[0].layerOrPattern);
    EXPECT_EQ(channels("A"), rows[0].channels);
}

TEST(KnobChannelSet, SetChannelsStillThrowsOnRegexRowZero)
{
    KnobChannelSetPtr knob = makeKnob();

    knob->setRegex(0, "spec.*");
    EXPECT_THROW(knob->setChannels(0, channels("R")), std::invalid_argument);
    EXPECT_EQ(ChannelSetRow::eModeRegex, knob->getRows()[0].mode);
}

TEST(KnobChannelSet, OneChannelLayerMapsToAlphaBit)
{
    KnobChannelSetPtr knob = makeKnob();
    std::vector<std::string> wanted = channels("Z");

    knob->setLayer(0, "depth", &wanted);

    std::vector<ResolvedLayer> resolved = knob->resolve(presentLayers());
    ASSERT_EQ(1u, resolved.size());
    EXPECT_EQ(std::string("depth"), resolved[0].desc.getLayerID());
    EXPECT_EQ(std::bitset<4>(std::string("1000")), resolved[0].channels);
}

TEST(KnobChannelSet, RegexIsAnchoredAndCaseSensitive)
{
    KnobChannelSetPtr knob = makeKnob();

    knob->setRegex(0, "spec.*");
    QString error;
    EXPECT_TRUE(knob->isPatternValid(0, &error));
    EXPECT_TRUE(error.isEmpty());

    std::list<ImageLayerDesc> present;
    present.push_back(makeLayer("specular", channels("R", "G", "B")));
    present.push_back(makeLayer("xspecular", channels("R", "G", "B")));
    present.push_back(makeLayer("Specular", channels("R", "G", "B")));
    present.push_back(makeLayer("spec", channels("A")));

    std::vector<ResolvedLayer> resolved = knob->resolve(present);
    ASSERT_EQ(2u, resolved.size());
    EXPECT_EQ(std::string("specular"), resolved[0].desc.getLayerID());
    EXPECT_EQ(std::bitset<4>(std::string("0111")), resolved[0].channels);
    EXPECT_EQ(std::string("spec"), resolved[1].desc.getLayerID());
    EXPECT_EQ(std::bitset<4>(std::string("1000")), resolved[1].channels);
    EXPECT_EQ(std::string("/spec.*/"), knob->getSummary());
}

TEST(KnobChannelSet, RegexExcludedChannelsAreRemovedFromMatchedLayers)
{
    KnobChannelSetPtr knob = makeKnob();

    knob->setRegex(0, "spec.*");
    knob->setExcludedChannels(0, channels("G"));

    std::list<ImageLayerDesc> present;
    present.push_back(makeLayer("specular", channels("R", "G", "B")));
    present.push_back(makeLayer("specularZ", channels("X", "Y", "Z")));

    std::vector<ResolvedLayer> resolved = knob->resolve(present);
    ASSERT_EQ(2u, resolved.size());
    EXPECT_EQ(std::string("specular"), resolved[0].desc.getLayerID());
    EXPECT_EQ(std::bitset<4>(std::string("0101")), resolved[0].channels);
    EXPECT_EQ(std::string("specularZ"), resolved[1].desc.getLayerID());
    EXPECT_EQ(std::bitset<4>(std::string("0111")), resolved[1].channels);

    EXPECT_EQ(channels("G"), knob->getExcludedChannels(0));
}

TEST(KnobChannelSet, SetExcludedChannelsOnlyValidOnRegexRow)
{
    KnobChannelSetPtr knob = makeKnob();

    EXPECT_THROW(knob->setExcludedChannels(0, channels("G")), std::invalid_argument);
    EXPECT_THROW(knob->getExcludedChannels(0), std::invalid_argument);

    knob->setRegex(0, "spec.*");
    EXPECT_NO_THROW(knob->setExcludedChannels(0, channels("G")));
    EXPECT_EQ(channels("G"), knob->getExcludedChannels(0));
}

TEST(KnobChannelSet, WithoutChannelButtonsALayerRowResolvesToEveryChannel)
{
    KnobChannelSetPtr knob = makeKnob();

    EXPECT_TRUE(knob->getWithChannelButtons());

    std::vector<std::string> r = channels("R");
    knob->setLayer(0, "diffuse", &r);

    std::vector<ResolvedLayer> resolved = knob->resolve(presentLayers());
    ASSERT_EQ(1u, resolved.size());
    EXPECT_EQ(std::bitset<4>(std::string("0001")), resolved[0].channels);

    knob->setWithChannelButtons(false);
    EXPECT_FALSE(knob->getWithChannelButtons());
    resolved = knob->resolve(presentLayers());
    ASSERT_EQ(1u, resolved.size());
    EXPECT_EQ(std::string("diffuse"), resolved[0].desc.getLayerID());
    EXPECT_EQ(std::bitset<4>(std::string("0111")), resolved[0].channels);
}

TEST(KnobChannelSet, WithoutChannelButtonsAColorViewRowIgnoresItsStoredChannels)
{
    KnobChannelSetPtr knob = makeKnob();
    std::list<ImageLayerDesc> rgba;
    std::list<ImageLayerDesc> rgb;

    rgba.push_back(ImageLayerDesc::getRGBAComponents());
    rgb.push_back(ImageLayerDesc::getRGBComponents());

    std::vector<std::string> rgbChannels = channels("R", "G", "B");
    knob->setLayer(0, kNatronColorViewRGBA, &rgbChannels);
    knob->setWithChannelButtons(false);

    std::vector<ResolvedLayer> resolved = knob->resolve(rgba);
    ASSERT_EQ(1u, resolved.size());
    EXPECT_EQ(std::bitset<4>(std::string("1111")), resolved[0].channels);
    EXPECT_TRUE(resolved[0].zeroChannels.none());

    resolved = knob->resolve(rgb);
    ASSERT_EQ(1u, resolved.size());
    EXPECT_EQ(std::bitset<4>(std::string("1111")), resolved[0].channels);
    EXPECT_EQ(std::bitset<4>(std::string("1000")), resolved[0].zeroChannels);
}

TEST(KnobChannelSet, WithoutChannelButtonsSettingChannelsThrows)
{
    KnobChannelSetPtr knob = makeKnob();

    knob->setWithChannelButtons(false);
    EXPECT_THROW(knob->setChannels(0, channels("R")), std::invalid_argument);

    knob->setNone();
    EXPECT_THROW(knob->setChannels(0, channels("R")), std::invalid_argument);
    EXPECT_EQ(ChannelSetRow::eModeNone, knob->getRows()[0].mode);

    knob->setRegex(0, "spec.*");
    EXPECT_THROW(knob->setExcludedChannels(0, channels("G")), std::invalid_argument);
}

TEST(KnobChannelSet, WithoutChannelButtonsARegexRowIgnoresItsExclusions)
{
    KnobChannelSetPtr knob = makeKnob();

    knob->setRegex(0, "spec.*");
    knob->setExcludedChannels(0, channels("G"));
    knob->setWithChannelButtons(false);

    std::list<ImageLayerDesc> present;
    present.push_back(makeLayer("specular", channels("R", "G", "B")));

    std::vector<ResolvedLayer> resolved = knob->resolve(present);
    ASSERT_EQ(1u, resolved.size());
    EXPECT_EQ(std::bitset<4>(std::string("0111")), resolved[0].channels);
}

TEST(KnobChannelSet, InvalidRegexResolvesToNothing)
{
    KnobChannelSetPtr knob = makeKnob();

    knob->setRegex(0, "spec(");
    EXPECT_TRUE(knob->resolve(presentLayers()).empty());

    QString error;
    EXPECT_FALSE(knob->isPatternValid(0, &error));
    EXPECT_FALSE(error.isEmpty());

    EXPECT_EQ(ChannelSetRow::eModeRegex, knob->getRows()[0].mode);
    EXPECT_EQ(std::string("spec("), knob->getRows()[0].layerOrPattern);
}

TEST(KnobChannelSet, DuplicateLayerIDsAcrossRowsOrTheirBits)
{
    // setLayer()/addLayer() refuse a duplicate layer row, so the duplicate is built as a
    // loaded value would be, via setValue() directly, to check that resolve() ORs the bits.
    KnobChannelSetPtr knob = makeKnob();
    std::vector<ChannelSetRow> rows(2);

    rows[0].mode = ChannelSetRow::eModeLayer;
    rows[0].layerOrPattern = "specular";
    rows[0].channels = channels("R");
    rows[1].mode = ChannelSetRow::eModeLayer;
    rows[1].layerOrPattern = "specular";
    rows[1].channels = channels("A");
    knob->setValue(knob->encodeRows(rows));

    std::vector<ResolvedLayer> resolved = knob->resolve(presentLayers());
    ASSERT_EQ(1u, resolved.size());
    EXPECT_EQ(std::string("specular"), resolved[0].desc.getLayerID());
    EXPECT_EQ(std::bitset<4>(std::string("1001")), resolved[0].channels);
}

TEST(KnobChannelSet, ALayerCanBeChosenByOnlyOneLayerRow)
{
    KnobChannelSetPtr knob = makeKnob();

    knob->setLayer(0, "diffuse", 0);
    knob->addLayer("specular", 0);
    EXPECT_THROW(knob->setLayer(1, "diffuse", 0), std::invalid_argument);
    EXPECT_EQ(std::string("specular"), knob->getRows()[1].layerOrPattern);

    std::vector<ChannelSetRow> rows = knob->getRows();
    rows[1].layerOrPattern = "diffuse";
    EXPECT_THROW(knob->setRows(rows), std::invalid_argument);
}

TEST(KnobChannelSet, RegexRowsDoNotConsumeLayers)
{
    KnobChannelSetPtr knob = makeKnob();

    knob->setLayer(0, "diffuse", 0);
    EXPECT_NO_THROW(knob->addRegex("diff.*"));
    ASSERT_EQ(2u, knob->getRows().size());
    EXPECT_EQ(ChannelSetRow::eModeRegex, knob->getRows()[1].mode);
}

TEST(KnobChannelSet, DecodingDuplicateLayerRowsFromAValueDoesNotThrow)
{
    KnobChannelSetPtr knob = makeKnob();
    std::vector<ChannelSetRow> rows(2);

    rows[0].mode = ChannelSetRow::eModeLayer;
    rows[0].layerOrPattern = "diffuse";
    rows[1].mode = ChannelSetRow::eModeLayer;
    rows[1].layerOrPattern = "diffuse";

    EXPECT_NO_THROW(knob->setValue(knob->encodeRows(rows)));

    std::vector<ChannelSetRow> decoded = knob->getRows();
    ASSERT_EQ(2u, decoded.size());
    EXPECT_EQ(std::string("diffuse"), decoded[0].layerOrPattern);
    EXPECT_EQ(std::string("diffuse"), decoded[1].layerOrPattern);
}

TEST(KnobChannelSet, RegexAndLayerRowsMergeOnTheSameID)
{
    KnobChannelSetPtr knob = makeKnob();
    std::vector<std::string> red = channels("R");

    knob->setLayer(0, "diffuse", &red);
    knob->addRegex("diff.*");

    std::vector<ResolvedLayer> resolved = knob->resolve(presentLayers());
    ASSERT_EQ(1u, resolved.size());
    EXPECT_EQ(std::bitset<4>(std::string("0111")), resolved[0].channels);
}

TEST(KnobChannelSet, ColorIsSortedFirst)
{
    KnobChannelSetPtr knob = makeKnob();

    knob->setLayer(0, "diffuse", 0);
    knob->addLayer("depth", 0);
    knob->addLayer(kNatronColorViewRGBA, 0);

    std::vector<ResolvedLayer> resolved = knob->resolve(presentLayers());
    ASSERT_EQ(3u, resolved.size());
    EXPECT_EQ(std::string(kNatronColorLayerID), resolved[0].desc.getLayerID());
    EXPECT_EQ(std::string("diffuse"), resolved[1].desc.getLayerID());
    EXPECT_EQ(std::string("depth"), resolved[2].desc.getLayerID());
}

TEST(KnobChannelSet, CodecRoundTripEscapesXML)
{
    KnobChannelSetPtr knob = makeKnob();
    std::vector<ChannelSetRow> rows(3);

    rows[0].mode = ChannelSetRow::eModeLayer;
    rows[0].layerOrPattern = kNatronColorLayerID;
    rows[0].channels = channels("R", "G", "B");
    rows[1].mode = ChannelSetRow::eModeRegex;
    rows[1].layerOrPattern = "a<b & c;d e";
    rows[2].mode = ChannelSetRow::eModeLayer;
    rows[2].layerOrPattern = "depth";

    knob->setRows(rows);

    const std::string raw = knob->getValue();
    EXPECT_NE(std::string::npos, raw.find("<Mode>layer</Mode><Layer>" + std::string(kNatronColorLayerID) + "</Layer><Channels>R,G,B</Channels>"));
    EXPECT_NE(std::string::npos, raw.find("<Mode>regex</Mode><Layer>a&lt;b &amp; c;d e</Layer><Channels></Channels>"));
    EXPECT_NE(std::string::npos, raw.find("<Mode>layer</Mode><Layer>depth</Layer><Channels></Channels>"));
    EXPECT_EQ(std::string::npos, raw.find("a<b"));

    std::vector<ChannelSetRow> decoded = knob->getRows();
    ASSERT_EQ(rows.size(), decoded.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        EXPECT_EQ(rows[i], decoded[i]);
    }

    std::list<ImageLayerDesc> present;
    present.push_back(makeLayer("a<b & c;d e", channels("X", "Y")));
    std::vector<ResolvedLayer> resolved = knob->resolve(present);
    ASSERT_EQ(1u, resolved.size());
    EXPECT_EQ(std::bitset<4>(std::string("0011")), resolved[0].channels);
}

TEST(KnobChannelSet, CodecRoundTripPreservesExcludedChannels)
{
    KnobChannelSetPtr knob = makeKnob();
    std::vector<ChannelSetRow> rows(1);

    rows[0].mode = ChannelSetRow::eModeRegex;
    rows[0].layerOrPattern = "spec.*";
    rows[0].channels = channels("G", "B");

    knob->setRows(rows);

    const std::string raw = knob->getValue();
    EXPECT_NE(std::string::npos, raw.find("<Mode>regex</Mode><Layer>spec.*</Layer><Channels>G,B</Channels>"));

    std::vector<ChannelSetRow> decoded = knob->getRows();
    ASSERT_EQ(1u, decoded.size());
    EXPECT_EQ(rows[0], decoded[0]);
}

TEST(KnobChannelSet, NoneOnRowZeroKeepsButIgnoresLaterRows)
{
    KnobChannelSetPtr knob = makeKnob();

    knob->setLayer(0, "diffuse", 0);
    knob->addLayer("specular", 0);
    ASSERT_EQ(2u, knob->resolve(presentLayers()).size());

    knob->setNone();
    ASSERT_EQ(2u, knob->getRows().size());
    EXPECT_EQ(ChannelSetRow::eModeNone, knob->getRows()[0].mode);
    EXPECT_EQ(std::string("specular"), knob->getRows()[1].layerOrPattern);
    EXPECT_TRUE(knob->resolve(presentLayers()).empty());

    EXPECT_EQ(2, knob->addLayer("depth", 0));
    ASSERT_EQ(3u, knob->getRows().size());
    EXPECT_TRUE(knob->resolve(presentLayers()).empty());

    knob->setAll();
    EXPECT_EQ(5u, knob->resolve(presentLayers()).size());
    EXPECT_NO_THROW(knob->setRegex(1, "spec.*"));
    EXPECT_EQ(ChannelSetRow::eModeRegex, knob->getRows()[1].mode);

    knob->setLayer(0, "diffuse", 0);
    std::vector<ResolvedLayer> resolved = knob->resolve(presentLayers());
    ASSERT_EQ(3u, resolved.size());
    EXPECT_EQ(std::string("diffuse"), resolved[0].desc.getLayerID());
    EXPECT_EQ(std::string("specular"), resolved[1].desc.getLayerID());
    EXPECT_EQ(std::string("depth"), resolved[2].desc.getLayerID());
}

TEST(KnobChannelSet, InvariantsThrow)
{
    KnobChannelSetPtr knob = makeKnob();

    EXPECT_THROW(knob->removeRow(0), std::invalid_argument);
    EXPECT_THROW(knob->removeRow(1), std::invalid_argument);
    EXPECT_THROW(knob->setLayer(1, "diffuse", 0), std::invalid_argument);
    EXPECT_THROW(knob->setRegex(-1, "x"), std::invalid_argument);

    std::vector<ChannelSetRow> rows(2);
    rows[0].mode = ChannelSetRow::eModeLayer;
    rows[0].layerOrPattern = "diffuse";
    rows[1].mode = ChannelSetRow::eModeNone;
    EXPECT_THROW(knob->setRows(rows), std::invalid_argument);
    rows[1].mode = ChannelSetRow::eModeAll;
    EXPECT_THROW(knob->setRows(rows), std::invalid_argument);
    EXPECT_THROW(knob->setRows(std::vector<ChannelSetRow>()), std::invalid_argument);

    ASSERT_EQ(1u, knob->getRows().size());
    EXPECT_EQ(std::string(kNatronColorViewRGBA), knob->getRows()[0].layerOrPattern);

    knob->addRegex("spec.*");
    EXPECT_THROW(knob->setChannels(1, channels("R")), std::invalid_argument);
    EXPECT_NO_THROW(knob->removeRow(1));
    EXPECT_EQ(1u, knob->getRows().size());
}

TEST(KnobChannelSet, SummarySamples)
{
    KnobChannelSetPtr knob = makeKnob();

    EXPECT_EQ(std::string("rgba"), knob->getSummary());

    std::vector<std::string> rgb = channels("R", "G", "B");
    knob->setLayer(0, kNatronColorViewRGBA, &rgb);
    EXPECT_EQ(std::string("rgba.rgb"), knob->getSummary());

    std::vector<std::string> z = channels("Z");
    knob->addLayer("depth", &z);
    knob->addRegex("spec.*");
    knob->addLayer("diffuse", 0);
    EXPECT_EQ(std::string("rgba.rgb, depth.z, /spec.*/, diffuse"), knob->getSummary());

    knob->addLayer(kNatronColorViewAlpha, 0);
    EXPECT_EQ(std::string("rgba.rgb, depth.z, /spec.*/, diffuse, alpha"), knob->getSummary());

    std::set<std::string> ids;
    knob->getReferencedLayerIDs(&ids);
    ASSERT_EQ(4u, ids.size());
    EXPECT_EQ(1u, ids.count(kNatronColorViewRGBA));
    EXPECT_EQ(1u, ids.count(kNatronColorViewAlpha));
    EXPECT_EQ(1u, ids.count("depth"));
    EXPECT_EQ(1u, ids.count("diffuse"));
}

TEST(KnobChannelSet, SummaryWithPresentLayersNamesRegexMatchesInsteadOfPattern)
{
    KnobChannelSetPtr knob = makeKnob();

    knob->addRegex("spec.*");

    std::list<ImageLayerDesc> present;
    present.push_back(ImageLayerDesc::getRGBAComponents());
    present.push_back(makeLayer("diffuse", channels("R", "G", "B")));
    present.push_back(makeLayer("specular", channels("R", "G", "B")));

    EXPECT_EQ(std::string("rgba, specular"), knob->getSummary(present));

    knob->setExcludedChannels(1, channels("G"));
    EXPECT_EQ(std::string("rgba, specular.rb"), knob->getSummary(present));

    knob->setRegex(1, "nomatch.*");
    EXPECT_EQ(std::string("rgba, (no match)"), knob->getSummary(present));

    EXPECT_EQ(std::string("rgba, /nomatch.*/"), knob->getSummary());
}

TEST(KnobChannelSet, CacheFollowsRawValueChanges)
{
    KnobChannelSetPtr knob = makeKnob();

    knob->setRegex(0, "spec.*");
    ASSERT_EQ(1u, knob->resolve(presentLayers()).size());

    std::vector<ChannelSetRow> rows(1);
    rows[0].mode = ChannelSetRow::eModeRegex;
    rows[0].layerOrPattern = "diff.*";
    knob->setValue(knob->encodeRows(rows));

    std::vector<ResolvedLayer> resolved = knob->resolve(presentLayers());
    ASSERT_EQ(1u, resolved.size());
    EXPECT_EQ(std::string("diffuse"), resolved[0].desc.getLayerID());
}

TEST_F(BaseTest, KnobChannelSetSerializationRoundTrip)
{
    NodePtr node = createNode(_generatorPluginID);

    ASSERT_TRUE(bool(node));

    KnobChannelSetPtr source = AppManager::createKnob<KnobChannelSet>(node->getEffectInstance().get(), std::string("channels"), 1, false);
    ASSERT_TRUE(bool(source));

    std::vector<std::string> rg = channels("R", "G");
    source->setLayer(0, kNatronColorViewRGBA, &rg);
    source->addRegex("spec.*");
    source->addLayer("depth", 0);
    const std::vector<ChannelSetRow> expected = source->getRows();
    ASSERT_EQ(3u, expected.size());

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
    EXPECT_EQ(std::string("ChannelSet"), loadedKnob->typeName());
    EXPECT_EQ(source->getName(), loadedKnob->getName());

    KnobChannelSetPtr loadedSet = std::dynamic_pointer_cast<KnobChannelSet>(loadedKnob);
    ASSERT_TRUE(bool(loadedSet));
    std::vector<ChannelSetRow> actual = loadedSet->getRows();
    ASSERT_EQ(expected.size(), actual.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(expected[i], actual[i]);
    }
    EXPECT_EQ(source->getValue(), loadedSet->getValue());
}
