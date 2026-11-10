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

#include <array>
#include <optional>
#include <string>

#include <QString>
#include <QStringList>

#include <gtest/gtest.h>

#include "BaseTest.h"
#include "NativeMetadataTestEffect.h"
#include "NativePluginIdentity.h"

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/Metadata/MetadataTimeCode.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Plugin.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

TEST(MetadataTimeCodeHelpersTest, RoundedRateIsNearestWholeAndAtLeastOne)
{
    EXPECT_EQ(24, MetadataTimeCode::roundedRate(24.));
    EXPECT_EQ(24, MetadataTimeCode::roundedRate(23.976));
    EXPECT_EQ(30, MetadataTimeCode::roundedRate(29.97));
    EXPECT_EQ(1, MetadataTimeCode::roundedRate(0.2));
    EXPECT_EQ(1, MetadataTimeCode::roundedRate(-5.));
}

TEST(MetadataTimeCodeHelpersTest, ParseTakesAnySeparator)
{
    EXPECT_EQ((std::array<int, 4> { 1, 2, 3, 4 }), MetadataTimeCode::parseTimecode("01:02:03:04"));
    EXPECT_EQ((std::array<int, 4> { 1, 2, 3, 4 }), MetadataTimeCode::parseTimecode(" 01;02.3 x04"));
    EXPECT_EQ((std::array<int, 4> { 1, 2, 3, 4 }), MetadataTimeCode::parseTimecode("1:2:3:4:5"));
}

TEST(MetadataTimeCodeHelpersTest, ShortInputReadsAsZero)
{
    EXPECT_EQ((std::array<int, 4> { 0, 0, 0, 0 }), MetadataTimeCode::parseTimecode("01:02:03"));
    EXPECT_EQ((std::array<int, 4> { 0, 0, 0, 0 }), MetadataTimeCode::parseTimecode(""));
    EXPECT_EQ(0, MetadataTimeCode::timecodeToFrames("12:34", 24));
}

TEST(MetadataTimeCodeHelpersTest, TimecodeToFramesCountsAtTheRate)
{
    EXPECT_EQ(86400, MetadataTimeCode::timecodeToFrames("01:00:00:00", 24));
    EXPECT_EQ(((60 + 2) * 60 + 3) * 25 + 4, MetadataTimeCode::timecodeToFrames("01:02:03:04", 25));
}

TEST(MetadataTimeCodeHelpersTest, FramesToTimecodeWrapsPastADay)
{
    const long long day = 24LL * 60 * 60 * 24;

    EXPECT_EQ("01:00:00:00", MetadataTimeCode::framesToTimecode(86400, 24));
    EXPECT_EQ("23:59:59:23", MetadataTimeCode::framesToTimecode(day - 1, 24));
    EXPECT_EQ("00:00:00:00", MetadataTimeCode::framesToTimecode(day, 24));
    EXPECT_EQ("00:00:00:05", MetadataTimeCode::framesToTimecode(2 * day + 5, 24));
}

TEST(MetadataTimeCodeHelpersTest, NegativeFramesWrapIntoThePreviousDay)
{
    EXPECT_EQ("23:59:59:23", MetadataTimeCode::framesToTimecode(-1, 24));
    EXPECT_EQ("23:59:58:29", MetadataTimeCode::framesToTimecode(-31, 30));
}

class MetadataTimeCodeTest
    : public BaseTest {
protected:
    // A Source -> MetadataTimeCode chain; an upstreamRate adds natron/framerate to the source.
    void build(std::optional<double> upstreamRate)
    {
        source = createNode(QString::fromUtf8(kTestPluginIDMetadataSource));
        timecode = createNode(QString::fromUtf8(PLUGINID_NATRON_METADATATIMECODE));
        ASSERT_TRUE(source && timecode);

        MetadataSourceTestEffect* sourceEffect = dynamic_cast<MetadataSourceTestEffect*>(source->getEffectInstance().get());
        ASSERT_TRUE(sourceEffect != NULL);
        if (upstreamRate) {
            ImageMetadata extra;
            extra.setDouble(kNatronMetadataKeyFrameRate, *upstreamRate);
            sourceEffect->setExtraMetadata(extra);
        }
        connectNodes(source, timecode, 0, true);
        effect = dynamic_cast<NativeEffectBase*>(timecode->getEffectInstance().get());
        ASSERT_TRUE(effect != NULL);
    }

    std::optional<std::string> timecodeAt(double time)
    {
        return effect->getOutputMetadata(time, ViewIdx(0)).getString(kNatronMetadataKeyTimecode);
    }

    std::optional<double> rateAt(double time)
    {
        return effect->getOutputMetadata(time, ViewIdx(0)).getDouble(kNatronMetadataKeyFrameRate);
    }

    template <typename KnobType, typename T>
    void set(const std::string& name,
             const T& value)
    {
        dynamic_cast<KnobType*>(timecode->getKnobByName(name).get())->setValue(value, ViewSpec::all(), 0);
    }

    NodePtr source;
    NodePtr timecode;
    NativeEffectBase* effect;
};

TEST_F(MetadataTimeCodeTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_METADATATIMECODE, PLUGIN_MAJOR_NATRON_METADATATIMECODE, "MetadataTimeCode");

    const PluginsMap& plugins = appPTR->getPluginsList();
    const PluginsMap::const_iterator it = plugins.find(PLUGINID_NATRON_METADATATIMECODE);
    ASSERT_TRUE(it != plugins.end());
    const QStringList grouping = (*it->second.begin())->getGrouping();
    ASSERT_FALSE(grouping.isEmpty());
    EXPECT_EQ(QString::fromUtf8(PLUGIN_GROUP_METADATA), grouping.front());
}

TEST_F(MetadataTimeCodeTest, HasNoChannelSelector)
{
    build(std::nullopt);
    EXPECT_FALSE(bool(timecode->getLayerKnob()));
}

TEST_F(MetadataTimeCodeTest, AdvancesOneFramePerFrameAndRollsIntoSeconds)
{
    build(std::nullopt);

    EXPECT_EQ(std::optional<std::string>("01:00:00:00"), timecodeAt(1.));
    EXPECT_EQ(std::optional<std::string>("01:00:00:01"), timecodeAt(2.));
    EXPECT_EQ(std::optional<std::string>("01:00:00:23"), timecodeAt(24.));
    EXPECT_EQ(std::optional<std::string>("01:00:01:00"), timecodeAt(25.));
    EXPECT_EQ(std::optional<std::string>("00:59:59:23"), timecodeAt(0.));
    EXPECT_EQ(std::optional<double>(24.), rateAt(1.));
}

TEST_F(MetadataTimeCodeTest, OtherUpstreamKeysPassThrough)
{
    build(std::nullopt);
    const ImageMetadata m = effect->getOutputMetadata(3., ViewIdx(0));

    EXPECT_EQ(std::optional<std::string>("source"), m.getString("exr/origin"));
    EXPECT_EQ(std::optional<int>(3), m.getInt(kNatronMetadataKeySourceFrame));
}

TEST_F(MetadataTimeCodeTest, RateComesFromUpstreamWhenOn)
{
    build(30.);

    EXPECT_EQ(std::optional<double>(30.), rateAt(1.));
    EXPECT_EQ(std::optional<std::string>("01:00:00:29"), timecodeAt(30.));
    EXPECT_EQ(std::optional<std::string>("01:00:01:00"), timecodeAt(31.));
}

TEST_F(MetadataTimeCodeTest, RateComesFromTheKnobWhenOff)
{
    build(30.);
    set<KnobBool>("rateFromMetadata", false);
    set<KnobDouble>("rate", 25.);

    EXPECT_EQ(std::optional<double>(25.), rateAt(1.));
    EXPECT_EQ(std::optional<std::string>("01:00:01:00"), timecodeAt(26.));
}

TEST_F(MetadataTimeCodeTest, RateFallsBackToTheKnobWhenUpstreamHasNone)
{
    build(std::nullopt);
    set<KnobDouble>("rate", 25.);

    EXPECT_EQ(std::optional<double>(25.), rateAt(1.));
    EXPECT_EQ(std::optional<std::string>("01:00:01:00"), timecodeAt(26.));
}

TEST_F(MetadataTimeCodeTest, UseStartFrameMovesTheOrigin)
{
    build(std::nullopt);
    set<KnobInt>("startFrame", 101);

    EXPECT_EQ(std::optional<std::string>("01:00:04:04"), timecodeAt(101.));

    set<KnobBool>("useStartFrame", true);

    EXPECT_EQ(std::optional<std::string>("01:00:00:00"), timecodeAt(101.));
    EXPECT_EQ(std::optional<std::string>("00:59:59:23"), timecodeAt(100.));
}

TEST_F(MetadataTimeCodeTest, CountWrapsPastMidnight)
{
    build(std::nullopt);
    set<KnobString>("startTimecode", std::string("23:59:59:23"));

    EXPECT_EQ(std::optional<std::string>("23:59:59:23"), timecodeAt(1.));
    EXPECT_EQ(std::optional<std::string>("00:00:00:00"), timecodeAt(2.));
}

TEST_F(MetadataTimeCodeTest, ShortStartTimecodeReadsAsZero)
{
    build(std::nullopt);
    set<KnobString>("startTimecode", std::string("10:00"));

    EXPECT_EQ(std::optional<std::string>("00:00:00:00"), timecodeAt(1.));
}
