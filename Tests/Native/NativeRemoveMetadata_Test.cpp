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
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/Metadata/RemoveMetadata.h"
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

class RemoveMetadataTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        source = createNode(QString::fromUtf8(kTestPluginIDMetadataSource));
        remove = createNode(QString::fromUtf8(PLUGINID_NATRON_REMOVEMETADATA));
        ASSERT_TRUE(source && remove);
        connectNodes(source, remove, 0, true);
    }

    void setKeys(const std::string& keys)
    {
        knobOf<KnobString>(remove, "keys")->setValue(keys, ViewSpec::all(), 0);
    }

    NodePtr source;
    NodePtr remove;
};

TEST_F(RemoveMetadataTest, EmptyListKeepsEverything)
{
    EXPECT_EQ(metadataOf(source, 1.), metadataOf(remove, 1.));
}

TEST_F(RemoveMetadataTest, RemoveAllEmptiesTheMetadata)
{
    knobOf<KnobBool>(remove, "removeAll")->setValue(true, ViewSpec::all(), 0);

    EXPECT_TRUE(metadataOf(remove, 1.).empty());
}

TEST_F(RemoveMetadataTest, RemoveAllWinsOverTheList)
{
    setKeys("exr/tag");
    knobOf<KnobBool>(remove, "removeAll")->setValue(true, ViewSpec::all(), 0);

    EXPECT_TRUE(metadataOf(remove, 1.).empty());
}

TEST_F(RemoveMetadataTest, GlobRemovesMatchingKeysOnly)
{
    setKeys("exr/*");

    const ImageMetadata result = metadataOf(remove, 1.);

    EXPECT_FALSE(result.contains("exr/tag"));
    EXPECT_FALSE(result.contains("exr/origin"));
    EXPECT_EQ(std::optional<int>(1), result.getInt("ofx/frame"));
}

TEST_F(RemoveMetadataTest, ListRemovesEachEntryAndUnmatchedKeysSurvive)
{
    setKeys("exr/tag,\nofx/fra?e");

    const ImageMetadata result = metadataOf(remove, 1.);

    EXPECT_FALSE(result.contains("exr/tag"));
    EXPECT_FALSE(result.contains("ofx/frame"));
    EXPECT_EQ(std::optional<std::string>("source"), result.getString("exr/origin"));
}

TEST_F(RemoveMetadataTest, ListThatMatchesNothingChangesNothing)
{
    setKeys("dpx/*");

    EXPECT_EQ(metadataOf(source, 1.), metadataOf(remove, 1.));
}

TEST_F(RemoveMetadataTest, KnobEditRecomputesMetadata)
{
    setKeys("exr/tag");
    EXPECT_FALSE(metadataOf(remove, 1.).contains("exr/tag"));
    setKeys("exr/origin");

    const ImageMetadata result = metadataOf(remove, 1.);

    EXPECT_TRUE(result.contains("exr/tag"));
    EXPECT_FALSE(result.contains("exr/origin"));
}
