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
#include <optional>
#include <string>
#include <vector>

#include <QString>

#include <gtest/gtest.h>

#include <ofxMetadata.h>

#include "BaseTest.h"
#include "NativeMetadataTestEffect.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/CheckerBoard.h"
#include "Engine/Nodes/Generator/Constant.h"
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

void
setTag(const NodePtr& source,
       int value)
{
    KnobInt* tag = dynamic_cast<KnobInt*>(source->getKnobByName("tag").get());

    ASSERT_TRUE(tag != NULL);
    tag->setValue(value, ViewSpec::all(), 0);
}

std::vector<std::string>
keysOf(const ImageMetadata& metadata)
{
    std::vector<std::string> keys;

    for (ImageMetadata::const_iterator it = metadata.begin(); it != metadata.end(); ++it) {
        keys.push_back(it->first);
    }

    return keys;
}

} // namespace

TEST_F(BaseTest, MergeMetadataComesFromA)
{
    NodePtr sourceA = createNode(QString::fromUtf8(kTestPluginIDMetadataSource));
    NodePtr sourceB = createNode(QString::fromUtf8(kTestPluginIDMetadataSource));
    NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_MERGE));

    ASSERT_TRUE(sourceA && sourceB && merge);

    setTag(sourceA, 7);
    setTag(sourceB, 9);
    connectNodes(sourceB, merge, kMergeInputB, true);
    connectNodes(sourceA, merge, kMergeInputA, true);

    NativeEffectBase* mergeEffect = nativeEffectOf(merge);
    NativeEffectBase* aEffect = nativeEffectOf(sourceA);

    ASSERT_TRUE(mergeEffect != NULL);
    ASSERT_TRUE(aEffect != NULL);

    const ImageMetadata result = mergeEffect->getOutputMetadata(2., ViewIdx(0));

    EXPECT_EQ(std::optional<int>(7), result.getInt("exr/tag"));
    EXPECT_EQ(aEffect->getOutputMetadata(2., ViewIdx(0)), result);
}

TEST_F(BaseTest, MergeMetadataFallsBackToBWithoutA)
{
    NodePtr sourceB = createNode(QString::fromUtf8(kTestPluginIDMetadataSource));
    NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_MERGE));

    ASSERT_TRUE(sourceB && merge);

    setTag(sourceB, 9);
    connectNodes(sourceB, merge, kMergeInputB, true);

    NativeEffectBase* mergeEffect = nativeEffectOf(merge);

    ASSERT_TRUE(mergeEffect != NULL);
    EXPECT_EQ(std::optional<int>(9), mergeEffect->getOutputMetadata(1., ViewIdx(0)).getInt("exr/tag"));
}

TEST_F(BaseTest, MergeMetadataFollowsAWhenItChanges)
{
    NodePtr sourceA = createNode(QString::fromUtf8(kTestPluginIDMetadataSource));
    NodePtr sourceB = createNode(QString::fromUtf8(kTestPluginIDMetadataSource));
    NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_MERGE));

    ASSERT_TRUE(sourceA && sourceB && merge);

    setTag(sourceB, 9);
    connectNodes(sourceB, merge, kMergeInputB, true);
    connectNodes(sourceA, merge, kMergeInputA, true);

    NativeEffectBase* mergeEffect = nativeEffectOf(merge);

    ASSERT_TRUE(mergeEffect != NULL);
    EXPECT_EQ(std::optional<int>(1), mergeEffect->getOutputMetadata(1., ViewIdx(0)).getInt("exr/tag"));

    setTag(sourceA, 5);

    EXPECT_EQ(std::optional<int>(5), mergeEffect->getOutputMetadata(1., ViewIdx(0)).getInt("exr/tag"));
}

TEST_F(BaseTest, MergeMetadataIsCachedPerFrame)
{
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDMetadataSource));
    NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_MERGE));

    ASSERT_TRUE(source && merge);

    connectNodes(source, merge, kMergeInputA, true);

    NativeEffectBase* mergeEffect = nativeEffectOf(merge);
    MetadataSourceTestEffect* sourceEffect = dynamic_cast<MetadataSourceTestEffect*>(source->getEffectInstance().get());

    ASSERT_TRUE(mergeEffect != NULL);
    ASSERT_TRUE(sourceEffect != NULL);

    const ImageMetadata first = mergeEffect->getOutputMetadata(1., ViewIdx(0));
    const int afterFirst = sourceEffect->derivationCount();

    // With the source's own cache gone, only Merge's cache can keep the source from being asked.
    sourceEffect->dropOwnMetadataCache();
    EXPECT_EQ(first, mergeEffect->getOutputMetadata(1., ViewIdx(0)));
    EXPECT_EQ(afterFirst, sourceEffect->derivationCount());

    const ImageMetadata otherFrame = mergeEffect->getOutputMetadata(2., ViewIdx(0));
    EXPECT_EQ(std::optional<int>(2), otherFrame.getInt(kOfxMetadataKeySourceFrame));
    EXPECT_EQ(afterFirst + 1, sourceEffect->derivationCount());

    setTag(source, 3);
    EXPECT_EQ(std::optional<int>(3), mergeEffect->getOutputMetadata(1., ViewIdx(0)).getInt("exr/tag"));
    EXPECT_EQ(afterFirst + 2, sourceEffect->derivationCount());
}

TEST_F(BaseTest, GeneratorMetadataIsTheMinimalKeySet)
{
    const char* const ids[] = { PLUGINID_NATRON_CONSTANT, PLUGINID_NATRON_CHECKERBOARD };

    for (const char* id : ids) {
        NodePtr generator = createNode(QString::fromUtf8(id));

        ASSERT_TRUE(bool(generator)) << id;

        NativeEffectBase* effect = nativeEffectOf(generator);

        ASSERT_TRUE(effect != NULL) << id;

        const ImageMetadata result = effect->getOutputMetadata(12., ViewIdx(0));
        const std::vector<std::string> expectedKeys = { kOfxMetadataKeyFrameRate, kOfxMetadataKeyPixelAspect };

        std::vector<std::string> sortedExpected = expectedKeys;
        std::sort(sortedExpected.begin(), sortedExpected.end());

        EXPECT_EQ(sortedExpected, keysOf(result)) << id;
        EXPECT_FALSE(result.contains(kOfxMetadataKeySourceFrame)) << id;
        EXPECT_EQ(std::optional<double>(getApp()->getProjectFrameRate()), result.getDouble(kOfxMetadataKeyFrameRate)) << id;
        EXPECT_EQ(std::optional<double>(effect->getAspectRatio(-1)), result.getDouble(kOfxMetadataKeyPixelAspect)) << id;
    }
}

TEST_F(BaseTest, GeneratorMetadataIgnoresItsSourceInput)
{
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDMetadataSource));
    NodePtr generator = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));

    ASSERT_TRUE(source && generator);

    connectNodes(source, generator, 0, true);

    NativeEffectBase* effect = nativeEffectOf(generator);

    ASSERT_TRUE(effect != NULL);

    const ImageMetadata result = effect->getOutputMetadata(3., ViewIdx(0));

    EXPECT_EQ(std::size_t(2), result.size());
    EXPECT_FALSE(result.contains("exr/tag"));
}
