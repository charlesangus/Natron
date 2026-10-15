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
#include "Engine/Nodes/Metadata/ModifyMetadata.h"
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

void
setOperation(const NodePtr& node,
             int slot,
             ModifyMetadata::OperationEnum operation,
             const std::string& key)
{
    const std::string suffix = std::to_string(slot);
    KnobChoice* op = knobOf<KnobChoice>(node, "operation" + suffix);
    KnobString* keyKnob = knobOf<KnobString>(node, "key" + suffix);

    ASSERT_TRUE(op != NULL);
    ASSERT_TRUE(keyKnob != NULL);
    op->setValue((int)operation, ViewSpec::all(), 0);
    keyKnob->setValue(key, ViewSpec::all(), 0);
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

class ModifyMetadataTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        source = createNode(QString::fromUtf8(kTestPluginIDMetadataSource));
        modify = createNode(QString::fromUtf8(PLUGINID_NATRON_MODIFYMETADATA));
        ASSERT_TRUE(source && modify);
        connectNodes(source, modify, 0, true);
    }

    NodePtr source;
    NodePtr modify;
};

TEST_F(ModifyMetadataTest, UntouchedPassesUpstreamThrough)
{
    const ImageMetadata result = metadataOf(modify, 1.);

    EXPECT_EQ(metadataOf(source, 1.), result);
    EXPECT_EQ(std::optional<int>(1), result.getInt("exr/tag"));
}

TEST_F(ModifyMetadataTest, SetsTypedValues)
{
    ASSERT_NO_FATAL_FAILURE(setOperation(modify, 1, ModifyMetadata::eOperationSet, "exr/added"));
    knobOf<KnobInt>(modify, "intValue1")->setValue(42, ViewSpec::all(), 0);

    ASSERT_NO_FATAL_FAILURE(setOperation(modify, 2, ModifyMetadata::eOperationSet, "exr/ratio"));
    knobOf<KnobChoice>(modify, "type2")->setValue((int)ModifyMetadata::eValueTypeDouble, ViewSpec::all(), 0);
    knobOf<KnobDouble>(modify, "doubleValue2")->setValue(2.5, ViewSpec::all(), 0);

    ASSERT_NO_FATAL_FAILURE(setOperation(modify, 3, ModifyMetadata::eOperationSet, "exr/origin"));
    knobOf<KnobChoice>(modify, "type3")->setValue((int)ModifyMetadata::eValueTypeString, ViewSpec::all(), 0);
    knobOf<KnobString>(modify, "stringValue3")->setValue("edited", ViewSpec::all(), 0);

    const ImageMetadata result = metadataOf(modify, 1.);

    EXPECT_EQ(std::optional<int>(42), result.getInt("exr/added"));
    EXPECT_EQ(std::optional<double>(2.5), result.getDouble("exr/ratio"));
    EXPECT_EQ(std::optional<std::string>("edited"), result.getString("exr/origin"));
    EXPECT_EQ(std::optional<int>(1), result.getInt("exr/tag"));
    EXPECT_EQ(std::optional<std::string>("source"), metadataOf(source, 1.).getString("exr/origin"));
}

TEST_F(ModifyMetadataTest, RemoveDropsOnlyTheKey)
{
    ASSERT_NO_FATAL_FAILURE(setOperation(modify, 1, ModifyMetadata::eOperationRemove, "exr/origin"));

    const ImageMetadata result = metadataOf(modify, 1.);

    EXPECT_FALSE(result.contains("exr/origin"));
    EXPECT_EQ(std::optional<int>(1), result.getInt("exr/tag"));
    EXPECT_EQ(std::optional<int>(1), result.getInt("ofx/frame"));
}

TEST_F(ModifyMetadataTest, RenameMovesTheValue)
{
    ASSERT_NO_FATAL_FAILURE(setOperation(modify, 1, ModifyMetadata::eOperationRename, "exr/origin"));
    knobOf<KnobString>(modify, "renameTo1")->setValue("exr/provenance", ViewSpec::all(), 0);

    const ImageMetadata result = metadataOf(modify, 1.);

    EXPECT_FALSE(result.contains("exr/origin"));
    EXPECT_EQ(std::optional<std::string>("source"), result.getString("exr/provenance"));
    EXPECT_EQ(std::optional<int>(1), result.getInt("exr/tag"));
}

TEST_F(ModifyMetadataTest, RenameOfAbsentKeyIsIgnored)
{
    ASSERT_NO_FATAL_FAILURE(setOperation(modify, 1, ModifyMetadata::eOperationRename, "exr/missing"));
    knobOf<KnobString>(modify, "renameTo1")->setValue("exr/other", ViewSpec::all(), 0);

    EXPECT_EQ(metadataOf(source, 1.), metadataOf(modify, 1.));
}

TEST_F(ModifyMetadataTest, SlotsApplyInOrder)
{
    ASSERT_NO_FATAL_FAILURE(setOperation(modify, 1, ModifyMetadata::eOperationSet, "exr/step"));
    knobOf<KnobInt>(modify, "intValue1")->setValue(1, ViewSpec::all(), 0);
    ASSERT_NO_FATAL_FAILURE(setOperation(modify, 2, ModifyMetadata::eOperationRename, "exr/step"));
    knobOf<KnobString>(modify, "renameTo2")->setValue("exr/renamed", ViewSpec::all(), 0);
    ASSERT_NO_FATAL_FAILURE(setOperation(modify, 3, ModifyMetadata::eOperationRemove, "exr/renamed"));

    const ImageMetadata result = metadataOf(modify, 1.);

    EXPECT_FALSE(result.contains("exr/step"));
    EXPECT_FALSE(result.contains("exr/renamed"));
}

TEST_F(ModifyMetadataTest, KeyframedValueDiffersBetweenFrames)
{
    ASSERT_NO_FATAL_FAILURE(setOperation(modify, 1, ModifyMetadata::eOperationSet, "exr/animated"));

    KnobInt* value = knobOf<KnobInt>(modify, "intValue1");

    ASSERT_TRUE(value != NULL);
    value->setValueAtTime(1., 10, ViewSpec::all(), 0);
    value->setValueAtTime(2., 20, ViewSpec::all(), 0);

    EXPECT_EQ(std::optional<int>(10), metadataOf(modify, 1.).getInt("exr/animated"));
    EXPECT_EQ(std::optional<int>(20), metadataOf(modify, 2.).getInt("exr/animated"));
}

TEST_F(ModifyMetadataTest, ExpressionValueDiffersBetweenFrames)
{
    ASSERT_NO_FATAL_FAILURE(setOperation(modify, 1, ModifyMetadata::eOperationSet, "exr/scripted"));

    KnobInt* value = knobOf<KnobInt>(modify, "intValue1");

    ASSERT_TRUE(value != NULL);
    ASSERT_NO_THROW(value->setExpression(0, "frame * 10", false, true));

    EXPECT_EQ(std::optional<int>(10), metadataOf(modify, 1.).getInt("exr/scripted"));
    EXPECT_EQ(std::optional<int>(20), metadataOf(modify, 2.).getInt("exr/scripted"));
}

TEST_F(ModifyMetadataTest, KnobEditRecomputesMetadata)
{
    ASSERT_NO_FATAL_FAILURE(setOperation(modify, 1, ModifyMetadata::eOperationSet, "exr/added"));
    knobOf<KnobInt>(modify, "intValue1")->setValue(5, ViewSpec::all(), 0);

    EXPECT_EQ(std::optional<int>(5), metadataOf(modify, 1.).getInt("exr/added"));

    knobOf<KnobInt>(modify, "intValue1")->setValue(6, ViewSpec::all(), 0);

    EXPECT_EQ(std::optional<int>(6), metadataOf(modify, 1.).getInt("exr/added"));

    ASSERT_NO_FATAL_FAILURE(setOperation(modify, 1, ModifyMetadata::eOperationRemove, "exr/added"));

    EXPECT_FALSE(metadataOf(modify, 1.).contains("exr/added"));
}

TEST_F(ModifyMetadataTest, UpstreamChangeReachesTheOutput)
{
    KnobInt* tag = knobOf<KnobInt>(source, "tag");

    ASSERT_TRUE(tag != NULL);
    EXPECT_EQ(std::optional<int>(1), metadataOf(modify, 1.).getInt("exr/tag"));
    tag->setValue(8, ViewSpec::all(), 0);
    EXPECT_EQ(std::optional<int>(8), metadataOf(modify, 1.).getInt("exr/tag"));
}
