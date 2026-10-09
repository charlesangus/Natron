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

#include <QString>

#include <gtest/gtest.h>

#include "BaseTest.h"
#include "NativeMetadataTestEffect.h"

#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Channel/Shuffle.h"
#include "Engine/Nodes/Merge/Merge.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

NativeEffectBase*
nativeEffectOf(const NodePtr& node)
{
    return dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
}

MetadataSourceTestEffect*
metadataSourceOf(const NodePtr& node)
{
    return dynamic_cast<MetadataSourceTestEffect*>(node->getEffectInstance().get());
}

void
setTag(const NodePtr& source,
       int value)
{
    KnobInt* tag = dynamic_cast<KnobInt*>(source->getKnobByName("tag").get());

    ASSERT_TRUE(tag != NULL);
    tag->setValue(value, ViewSpec::all(), 0);
}

} // namespace

TEST_F(BaseTest, NativeMetadataSurvivesShuffleAndMerge)
{
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDMetadataSource));
    NodePtr shuffle = createNode(QString::fromUtf8(PLUGINID_NATRON_SHUFFLE));
    NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_MERGE));

    ASSERT_TRUE(source && shuffle && merge);

    connectNodes(source, shuffle, 0, true);
    connectNodes(shuffle, merge, 0, true);

    NativeEffectBase* mergeEffect = nativeEffectOf(merge);
    NativeEffectBase* shuffleEffect = nativeEffectOf(shuffle);
    MetadataSourceTestEffect* sourceEffect = metadataSourceOf(source);

    ASSERT_TRUE(mergeEffect != NULL);
    ASSERT_TRUE(shuffleEffect != NULL);
    ASSERT_TRUE(sourceEffect != NULL);

    const ImageMetadata expected = sourceEffect->getOutputMetadata(3., ViewIdx(0));

    ASSERT_EQ(std::optional<int>(1), expected.getInt("exr/tag"));

    EXPECT_EQ(expected, shuffleEffect->getOutputMetadata(3., ViewIdx(0)));
    EXPECT_EQ(expected, mergeEffect->getOutputMetadata(3., ViewIdx(0)));
    EXPECT_EQ(std::optional<int>(3), mergeEffect->getOutputMetadata(3., ViewIdx(0)).getInt("ofx/frame"));
    EXPECT_EQ(std::optional<int>(4), mergeEffect->getOutputMetadata(4., ViewIdx(0)).getInt("ofx/frame"));
}

TEST_F(BaseTest, NativeMetadataOfUnconnectedNodeIsEmpty)
{
    NodePtr shuffle = createNode(QString::fromUtf8(PLUGINID_NATRON_SHUFFLE));

    ASSERT_TRUE(bool(shuffle));

    NativeEffectBase* shuffleEffect = nativeEffectOf(shuffle);

    ASSERT_TRUE(shuffleEffect != NULL);
    EXPECT_TRUE(shuffleEffect->getOutputMetadata(1., ViewIdx(0)).empty());
}

TEST_F(BaseTest, NativeMetadataIsCachedPerFrame)
{
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDMetadataSource));
    NodePtr shuffle = createNode(QString::fromUtf8(PLUGINID_NATRON_SHUFFLE));

    ASSERT_TRUE(source && shuffle);

    connectNodes(source, shuffle, 0, true);

    NativeEffectBase* shuffleEffect = nativeEffectOf(shuffle);
    MetadataSourceTestEffect* sourceEffect = metadataSourceOf(source);

    ASSERT_TRUE(shuffleEffect != NULL);
    ASSERT_TRUE(sourceEffect != NULL);

    const int before = sourceEffect->derivationCount();

    ImageMetadata first = shuffleEffect->getOutputMetadata(1., ViewIdx(0));
    ImageMetadata second = shuffleEffect->getOutputMetadata(1., ViewIdx(0));

    EXPECT_EQ(first, second);
    EXPECT_EQ(before + 1, sourceEffect->derivationCount());

    ImageMetadata otherFrame = shuffleEffect->getOutputMetadata(2., ViewIdx(0));

    EXPECT_NE(first, otherFrame);
    EXPECT_EQ(before + 2, sourceEffect->derivationCount());
}

TEST_F(BaseTest, NativeMetadataCacheInvalidatedByUpstreamKnobChange)
{
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDMetadataSource));
    NodePtr shuffle = createNode(QString::fromUtf8(PLUGINID_NATRON_SHUFFLE));
    NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_MERGE));

    ASSERT_TRUE(source && shuffle && merge);

    connectNodes(source, shuffle, 0, true);
    connectNodes(shuffle, merge, 0, true);

    NativeEffectBase* mergeEffect = nativeEffectOf(merge);

    ASSERT_TRUE(mergeEffect != NULL);

    EXPECT_EQ(std::optional<int>(1), mergeEffect->getOutputMetadata(1., ViewIdx(0)).getInt("exr/tag"));

    setTag(source, 42);

    EXPECT_EQ(std::optional<int>(42), mergeEffect->getOutputMetadata(1., ViewIdx(0)).getInt("exr/tag"));
}

TEST_F(BaseTest, NativeMetadataCacheInvalidatedByConnectionChange)
{
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDMetadataSource));
    NodePtr shuffle = createNode(QString::fromUtf8(PLUGINID_NATRON_SHUFFLE));

    ASSERT_TRUE(source && shuffle);

    NativeEffectBase* shuffleEffect = nativeEffectOf(shuffle);

    ASSERT_TRUE(shuffleEffect != NULL);

    EXPECT_TRUE(shuffleEffect->getOutputMetadata(1., ViewIdx(0)).empty());

    connectNodes(source, shuffle, 0, true);

    EXPECT_EQ(std::optional<int>(1), shuffleEffect->getOutputMetadata(1., ViewIdx(0)).getInt("exr/tag"));

    disconnectNodes(source, shuffle, true);

    EXPECT_TRUE(shuffleEffect->getOutputMetadata(1., ViewIdx(0)).empty());
}
