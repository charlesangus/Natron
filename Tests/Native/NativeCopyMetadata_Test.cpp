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

#include <optional>
#include <string>

#include <QString>

#include <gtest/gtest.h>

#include "BaseTest.h"
#include "NativeMetadataTestEffect.h"

#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Metadata/CopyMetadata.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/Metadata/MetadataKeyFilter.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

template <typename KnobType>
KnobType*
knobOf(const NodePtr& node,
       const std::string& name)
{
    return dynamic_cast<KnobType*>(node->getKnobByName(name).get());
}

ImageMetadata
metadataOf(const NodePtr& node,
           double time)
{
    NativeEffectBase* effect = dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());

    EXPECT_TRUE(effect != NULL);

    return effect ? effect->getOutputMetadata(time, ViewIdx(0)) : ImageMetadata();
}

} // namespace

TEST(MetadataKeyFilterTest, EmptyListMatchesEverything)
{
    const MetadataKeyFilter filter("");

    EXPECT_TRUE(filter.matchesAll());
    EXPECT_TRUE(filter.matches("exr/tag"));
    EXPECT_TRUE(MetadataKeyFilter(" , \n ,").matchesAll());
}

TEST(MetadataKeyFilterTest, GlobsAreAnchored)
{
    const MetadataKeyFilter filter("exr/*");

    EXPECT_FALSE(filter.matchesAll());
    EXPECT_TRUE(filter.matches("exr/tag"));
    EXPECT_TRUE(filter.matches("exr/a/b"));
    EXPECT_FALSE(filter.matches("exif/Make"));
    EXPECT_FALSE(filter.matches("xexr/tag"));
    EXPECT_FALSE(MetadataKeyFilter("exr/tag").matches("exr/tag2"));
}

TEST(MetadataKeyFilterTest, StarCrossesSlashesAndQuestionMarkIsOneCharacter)
{
    EXPECT_TRUE(MetadataKeyFilter("*tag").matches("exr/tag"));
    EXPECT_TRUE(MetadataKeyFilter("exr/ta?").matches("exr/tag"));
    EXPECT_FALSE(MetadataKeyFilter("exr/ta?").matches("exr/ta"));
    EXPECT_FALSE(MetadataKeyFilter("exr/ta?").matches("exr/tags"));
}

TEST(MetadataKeyFilterTest, ListIsSplitOnCommasAndNewlinesAndTrimmed)
{
    const MetadataKeyFilter filter("  exr/tag ,exif/*\nofx/frame\r\n");

    EXPECT_TRUE(filter.matches("exr/tag"));
    EXPECT_TRUE(filter.matches("exif/Make"));
    EXPECT_TRUE(filter.matches("ofx/frame"));
    EXPECT_FALSE(filter.matches("exr/origin"));
}

TEST(MetadataKeyFilterTest, RegexCharactersAreLiteral)
{
    EXPECT_TRUE(MetadataKeyFilter("exr/a.b").matches("exr/a.b"));
    EXPECT_FALSE(MetadataKeyFilter("exr/a.b").matches("exr/axb"));
}

class CopyMetadataTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        a = createNode(QString::fromUtf8(kTestPluginIDMetadataSource));
        b = createNode(QString::fromUtf8(kTestPluginIDMetadataSource));
        copy = createNode(QString::fromUtf8(PLUGINID_NATRON_COPYMETADATA));
        ASSERT_TRUE(a && b && copy);

        MetadataSourceTestEffect* bEffect = dynamic_cast<MetadataSourceTestEffect*>(b->getEffectInstance().get());

        MetadataSourceTestEffect* aEffect = dynamic_cast<MetadataSourceTestEffect*>(a->getEffectInstance().get());

        ASSERT_TRUE(aEffect != NULL);
        ASSERT_TRUE(bEffect != NULL);
        ImageMetadata aExtra;
        aExtra.setInt("exr/aonly", 3);
        aEffect->setExtraMetadata(aExtra);
        ImageMetadata extra;
        extra.setString("exr/origin", "b");
        extra.setInt("exr/bonly", 7);
        extra.setString("exif/Make", "cam");
        bEffect->setExtraMetadata(extra);
        knobOf<KnobInt>(b, "tag")->setValue(2, ViewSpec::all(), 0);

        connectNodes(a, copy, 0, true);
    }

    void connectB()
    {
        connectNodes(b, copy, 1, true);
    }

    void setMode(CopyMetadata::ModeEnum mode)
    {
        knobOf<KnobChoice>(copy, "mode")->setValue((int)mode, ViewSpec::all(), 0);
    }

    void setKeys(const std::string& keys)
    {
        knobOf<KnobString>(copy, "keys")->setValue(keys, ViewSpec::all(), 0);
    }

    NodePtr a;
    NodePtr b;
    NodePtr copy;
};

TEST_F(CopyMetadataTest, HasNoChannelSelector)
{
    EXPECT_FALSE(bool(copy->getLayerKnob()));
}

TEST_F(CopyMetadataTest, DisconnectedBPassesAThrough)
{
    setMode(CopyMetadata::eModeReplace);

    EXPECT_EQ(metadataOf(a, 1.), metadataOf(copy, 1.));
}

TEST_F(CopyMetadataTest, ReplaceWithoutFilterTakesB)
{
    connectB();
    setMode(CopyMetadata::eModeReplace);

    EXPECT_EQ(metadataOf(b, 1.), metadataOf(copy, 1.));
}

TEST_F(CopyMetadataTest, ReplaceWithGlobKeepsUnmatchedKeysOfA)
{
    connectB();
    setMode(CopyMetadata::eModeReplace);
    setKeys("exr/*");

    const ImageMetadata result = metadataOf(copy, 1.);

    EXPECT_EQ(std::optional<int>(2), result.getInt("exr/tag"));
    EXPECT_EQ(std::optional<std::string>("b"), result.getString("exr/origin"));
    EXPECT_EQ(std::optional<int>(7), result.getInt("exr/bonly"));
    EXPECT_EQ(std::optional<int>(1), result.getInt("ofx/frame"));
    EXPECT_FALSE(result.contains("exr/aonly"));
    EXPECT_FALSE(result.contains("exif/Make"));
}

TEST_F(CopyMetadataTest, ReplaceDropsMatchedKeysOfAThatBLacks)
{
    connectB();
    setMode(CopyMetadata::eModeReplace);
    setKeys("exr/aonly, exr/origin");

    const ImageMetadata result = metadataOf(copy, 1.);

    EXPECT_EQ(std::optional<std::string>("b"), result.getString("exr/origin"));
    EXPECT_EQ(std::optional<int>(1), result.getInt("exr/tag"));
    EXPECT_EQ(std::optional<int>(1), result.getInt("ofx/frame"));
    EXPECT_FALSE(result.contains("exr/aonly"));
    EXPECT_FALSE(result.contains("exr/bonly"));
}

TEST_F(CopyMetadataTest, MergeBWinsPrefersB)
{
    connectB();
    setMode(CopyMetadata::eModeMergeBWins);

    const ImageMetadata result = metadataOf(copy, 1.);

    EXPECT_EQ(std::optional<int>(2), result.getInt("exr/tag"));
    EXPECT_EQ(std::optional<std::string>("b"), result.getString("exr/origin"));
    EXPECT_EQ(std::optional<int>(7), result.getInt("exr/bonly"));
    EXPECT_EQ(std::optional<std::string>("cam"), result.getString("exif/Make"));
}

TEST_F(CopyMetadataTest, MergeAWinsPrefersA)
{
    connectB();
    setMode(CopyMetadata::eModeMergeAWins);

    const ImageMetadata result = metadataOf(copy, 1.);

    EXPECT_EQ(std::optional<int>(1), result.getInt("exr/tag"));
    EXPECT_EQ(std::optional<std::string>("source"), result.getString("exr/origin"));
    EXPECT_EQ(std::optional<int>(7), result.getInt("exr/bonly"));
    EXPECT_EQ(std::optional<std::string>("cam"), result.getString("exif/Make"));
}

TEST_F(CopyMetadataTest, MergeWithFilterOnlyAddsMatchedKeysOfB)
{
    connectB();
    setMode(CopyMetadata::eModeMergeBWins);
    setKeys("exr/tag");

    const ImageMetadata result = metadataOf(copy, 1.);

    EXPECT_EQ(std::optional<int>(2), result.getInt("exr/tag"));
    EXPECT_EQ(std::optional<std::string>("source"), result.getString("exr/origin"));
    EXPECT_FALSE(result.contains("exr/bonly"));
    EXPECT_FALSE(result.contains("exif/Make"));
}

TEST_F(CopyMetadataTest, ConnectingBRecomputesMetadata)
{
    setMode(CopyMetadata::eModeMergeBWins);
    EXPECT_FALSE(metadataOf(copy, 1.).contains("exr/bonly"));
    connectB();
    EXPECT_EQ(std::optional<int>(7), metadataOf(copy, 1.).getInt("exr/bonly"));
}
