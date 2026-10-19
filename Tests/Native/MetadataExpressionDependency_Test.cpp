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
#include <string>
#include <utility>

#include <QString>

#include <gtest/gtest.h>

#include "BaseTest.h"
#include "NativeMetadataTestEffect.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Metadata/ModifyMetadata.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

KnobIntPtr
tagKnobOf(const NodePtr& node)
{
    return std::dynamic_pointer_cast<KnobInt>(node->getKnobByName("tag"));
}

bool
readsMetadataOf(const KnobIPtr& knob,
                int dimension,
                const NodePtr& node)
{
    std::list<NodePtr> sources;

    knob->getMetadataDependencies(dimension, &sources);

    return std::find(sources.begin(), sources.end(), node) != sources.end();
}

bool
hasMetadataReader(const NodePtr& node,
                  const KnobIPtr& knob)
{
    std::list<std::pair<KnobIPtr, int>> readers;

    node->getMetadataReaders(&readers);
    for (std::list<std::pair<KnobIPtr, int>>::const_iterator it = readers.begin(); it != readers.end(); ++it) {
        if (it->first == knob) {
            return true;
        }
    }

    return false;
}

} // namespace

class MetadataExpressionDependencyTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        source = createNode(QString::fromUtf8(kTestPluginIDMetadataSource));
        reader = createNode(QString::fromUtf8(kTestPluginIDMetadataSource));
        ASSERT_TRUE(source && reader);
        sourceTag = tagKnobOf(source);
        readerTag = tagKnobOf(reader);
        ASSERT_TRUE(sourceTag && readerTag);
    }

    void setReaderExpression(const std::string& expression)
    {
        ASSERT_NO_THROW(readerTag->setExpression(0, expression, false, true));
    }

    NodePtr source;
    NodePtr reader;
    KnobIntPtr sourceTag;
    KnobIntPtr readerTag;
};

TEST_F(MetadataExpressionDependencyTest, ScanRecordsTheEdge)
{
    ASSERT_NO_FATAL_FAILURE(setReaderExpression(source->getScriptName() + ".metadata('exr/tag', 1) * 10"));

    EXPECT_TRUE(readsMetadataOf(readerTag, 0, source));
    EXPECT_TRUE(hasMetadataReader(source, readerTag));
    EXPECT_EQ(10, readerTag->getValue());
}

TEST_F(MetadataExpressionDependencyTest, ScanResolvesAppQualifiedMetadataKeys)
{
    const std::string sourceVar = getApp()->getAppIDString() + "." + source->getFullyQualifiedName();

    ASSERT_NO_FATAL_FAILURE(setReaderExpression("len(" + sourceVar + ".metadataKeys(1))"));

    EXPECT_TRUE(readsMetadataOf(readerTag, 0, source));
    EXPECT_TRUE(hasMetadataReader(source, readerTag));
}

TEST_F(MetadataExpressionDependencyTest, OwnNodeReadFollowsUpstreamWithoutRegisteringAReader)
{
    NodePtr modify = createNode(QString::fromUtf8(PLUGINID_NATRON_MODIFYMETADATA));

    ASSERT_TRUE(bool(modify));
    connectNodes(source, modify, 0, true);

    // The slot's operation is left at none, so its value does not feed the metadata it reads.
    KnobIntPtr value = std::dynamic_pointer_cast<KnobInt>(modify->getKnobByName("intValue1"));
    ASSERT_TRUE(bool(value));
    ASSERT_NO_THROW(value->setExpression(0, "thisNode.metadata('exr/tag', 1)", false, true));

    EXPECT_TRUE(readsMetadataOf(value, 0, modify));
    EXPECT_FALSE(hasMetadataReader(modify, value));
    EXPECT_EQ(1, value->getValue());

    sourceTag->setValue(6, ViewSpec::all(), 0);
    modify->getHashValue();

    EXPECT_EQ(6, value->getValue());
}

TEST_F(MetadataExpressionDependencyTest, ExpressionFollowsTheSourceWithoutReload)
{
    ASSERT_NO_FATAL_FAILURE(setReaderExpression(source->getScriptName() + ".metadata('exr/tag', 1) * 10"));
    ASSERT_EQ(10, readerTag->getValue());

    sourceTag->setValue(3, ViewSpec::all(), 0);
    // An edit's own metadata refresh reads the edited node's hash, which is what reaches the readers.
    source->getHashValue();

    EXPECT_EQ(30, readerTag->getValue());
}

TEST_F(MetadataExpressionDependencyTest, ReaderContentHashFollowsTheSource)
{
    ASSERT_NO_FATAL_FAILURE(setReaderExpression(source->getScriptName() + ".metadata('exr/tag', 1) * 10"));

    const U64 before = reader->getHashValue();

    sourceTag->setValue(5, ViewSpec::all(), 0);
    const U64 edited = reader->getHashValue();

    EXPECT_NE(before, edited);
    EXPECT_EQ(50, readerTag->getValue());

    sourceTag->setValue(1, ViewSpec::all(), 0);
    EXPECT_EQ(before, reader->getHashValue());
    EXPECT_EQ(10, readerTag->getValue());
}

TEST_F(MetadataExpressionDependencyTest, UpstreamEditReachesTheReader)
{
    NodePtr modify = createNode(QString::fromUtf8(PLUGINID_NATRON_MODIFYMETADATA));

    ASSERT_TRUE(bool(modify));
    connectNodes(source, modify, 0, true);
    ASSERT_NO_FATAL_FAILURE(setReaderExpression(modify->getScriptName() + ".metadata('exr/tag', 1)"));
    ASSERT_EQ(1, readerTag->getValue());

    const U64 before = reader->getHashValue();

    sourceTag->setValue(7, ViewSpec::all(), 0);

    EXPECT_NE(before, reader->getHashValue());
    EXPECT_EQ(7, readerTag->getValue());
}

TEST_F(MetadataExpressionDependencyTest, ClearingTheExpressionRemovesTheEdge)
{
    ASSERT_NO_FATAL_FAILURE(setReaderExpression(source->getScriptName() + ".metadata('exr/tag', 1) * 10"));
    ASSERT_TRUE(hasMetadataReader(source, readerTag));

    readerTag->clearExpression(0, true);

    EXPECT_FALSE(readsMetadataOf(readerTag, 0, source));
    EXPECT_FALSE(hasMetadataReader(source, readerTag));

    const U64 before = reader->getHashValue();
    sourceTag->setValue(9, ViewSpec::all(), 0);
    EXPECT_EQ(before, reader->getHashValue());
}

TEST_F(MetadataExpressionDependencyTest, ReplacingTheExpressionRemovesTheEdge)
{
    ASSERT_NO_FATAL_FAILURE(setReaderExpression(source->getScriptName() + ".metadata('exr/tag', 1) * 10"));
    ASSERT_NO_FATAL_FAILURE(setReaderExpression("2 + 2"));

    EXPECT_FALSE(readsMetadataOf(readerTag, 0, source));
    EXPECT_FALSE(hasMetadataReader(source, readerTag));
    EXPECT_EQ(4, readerTag->getValue());
}

TEST_F(MetadataExpressionDependencyTest, NextFrameReadFollowsTheEvaluationFrame)
{
    ASSERT_NO_FATAL_FAILURE(setReaderExpression(source->getScriptName() + ".metadata('ofx/frame', frame + 1)"));

    EXPECT_EQ(4, readerTag->getValueAtTime(3, 0));
    EXPECT_EQ(9, readerTag->getValueAtTime(8, 0));
    EXPECT_EQ(4, readerTag->getValueAtTime(3, 0));
    EXPECT_EQ(6, readerTag->getValueAtTime(5, 0));
}

TEST_F(MetadataExpressionDependencyTest, NextFrameReadStaysCorrectAfterTheSourceChanges)
{
    ASSERT_NO_FATAL_FAILURE(setReaderExpression(source->getScriptName() + ".metadata('ofx/frame', frame + 1) + " + source->getScriptName() + ".metadata('exr/tag', frame)"));

    EXPECT_EQ(5, readerTag->getValueAtTime(3, 0));
    EXPECT_EQ(7, readerTag->getValueAtTime(5, 0));

    sourceTag->setValue(4, ViewSpec::all(), 0);
    source->getHashValue();

    EXPECT_EQ(8, readerTag->getValueAtTime(3, 0));
    EXPECT_EQ(10, readerTag->getValueAtTime(5, 0));
}
