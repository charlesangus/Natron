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
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <QString>

#include <gtest/gtest.h>

#include "BaseTest.h"
#include "NativeMetadataTestEffect.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/Metadata/ModifyMetadata.h"
#include "Engine/Nodes/NativeEffectBase.h"
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

    void runPython(const std::string& script)
    {
        std::string error, output;

        ASSERT_TRUE(NATRON_NAMESPACE::NATRON_PYTHON_NAMESPACE::interpretPythonScript(script, &error, &output)) << error;
    }

    KnobStringPtr labelOf(const NodePtr& node)
    {
        return std::dynamic_pointer_cast<KnobString>(node->getKnobByName(kUserLabelKnobName));
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

TEST_F(MetadataExpressionDependencyTest, DefaultFrameIsTheEvaluationFrame)
{
    ASSERT_NO_FATAL_FAILURE(setReaderExpression(source->getScriptName() + ".metadata('ofx/frame')"));

    const int current = (int)reader->getEffectInstance()->getCurrentTime();

    EXPECT_EQ(current + 6, readerTag->getValueAtTime(current + 6, 0));
    EXPECT_EQ(current + 11, readerTag->getValueAtTime(current + 11, 0));
    EXPECT_EQ(current, readerTag->getValue());
}

TEST_F(MetadataExpressionDependencyTest, MetadataInvalidatedUnderTheSameHashReachesTheReader)
{
    MetadataSourceTestEffect* sourceEffect = dynamic_cast<MetadataSourceTestEffect*>(source->getEffectInstance().get());
    ASSERT_TRUE(sourceEffect != NULL);

    ImageMetadata extra;
    extra.setInt("exr/extra", 2);
    sourceEffect->setExtraMetadata(extra);
    ASSERT_NO_FATAL_FAILURE(setReaderExpression(source->getScriptName() + ".metadata('exr/extra', 1)"));
    ASSERT_EQ(2, readerTag->getValue());

    const U64 hash = source->getHashValue();
    extra.setInt("exr/extra", 5);
    sourceEffect->setExtraMetadata(extra);
    sourceEffect->changeMetadataUnderSameHash();

    EXPECT_EQ(hash, source->getHashValue());
    EXPECT_EQ(5, readerTag->getValue());
}

TEST_F(MetadataExpressionDependencyTest, ReaderNodeHashMovesWhenTheMetadataChangesUnderTheSameHash)
{
    MetadataSourceTestEffect* sourceEffect = dynamic_cast<MetadataSourceTestEffect*>(source->getEffectInstance().get());
    ASSERT_TRUE(sourceEffect != NULL);

    ImageMetadata extra;
    extra.setInt("exr/extra", 2);
    sourceEffect->setExtraMetadata(extra);
    ASSERT_NO_FATAL_FAILURE(setReaderExpression(source->getScriptName() + ".metadata('exr/extra', 1)"));
    ASSERT_EQ(2, readerTag->getValue());

    const U64 sourceHash = source->getHashValue();
    const U64 readerHash = reader->getHashValue();
    extra.setInt("exr/extra", 5);
    sourceEffect->setExtraMetadata(extra);
    sourceEffect->changeMetadataUnderSameHash();

    EXPECT_EQ(sourceHash, source->getHashValue());
    EXPECT_NE(readerHash, reader->getHashValue());
    EXPECT_EQ(5, readerTag->getValue());
}

TEST_F(MetadataExpressionDependencyTest, OwnNodeReadMovesItsNodeHashWhenTheMetadataChangesUnderTheSameHash)
{
    MetadataSourceTestEffect* sourceEffect = dynamic_cast<MetadataSourceTestEffect*>(source->getEffectInstance().get());
    ASSERT_TRUE(sourceEffect != NULL);
    NodePtr modify = createNode(QString::fromUtf8(PLUGINID_NATRON_MODIFYMETADATA));
    ASSERT_TRUE(bool(modify));
    connectNodes(source, modify, 0, true);

    ImageMetadata extra;
    extra.setInt("exr/extra", 2);
    sourceEffect->setExtraMetadata(extra);
    KnobIntPtr value = std::dynamic_pointer_cast<KnobInt>(modify->getKnobByName("intValue1"));
    ASSERT_TRUE(bool(value));
    ASSERT_NO_THROW(value->setExpression(0, "thisNode.metadata('exr/extra', 1)", false, true));
    ASSERT_EQ(2, value->getValue());

    const U64 modifyHash = modify->getHashValue();
    extra.setInt("exr/extra", 5);
    sourceEffect->setExtraMetadata(extra);
    sourceEffect->changeMetadataUnderSameHash();

    EXPECT_NE(modifyHash, modify->getHashValue());
    EXPECT_EQ(5, value->getValue());
}

TEST_F(MetadataExpressionDependencyTest, MetadataChangedUnderTheSameHashReachesTheReadersOfNodesDownstream)
{
    MetadataSourceTestEffect* sourceEffect = dynamic_cast<MetadataSourceTestEffect*>(source->getEffectInstance().get());
    ASSERT_TRUE(sourceEffect != NULL);
    NodePtr modify = createNode(QString::fromUtf8(PLUGINID_NATRON_MODIFYMETADATA));
    NodePtr downstream = createNode(QString::fromUtf8(PLUGINID_NATRON_MODIFYMETADATA));
    ASSERT_TRUE(modify && downstream);
    connectNodes(source, modify, 0, true);
    connectNodes(modify, downstream, 0, true);
    NativeEffectBase* downstreamEffect = dynamic_cast<NativeEffectBase*>(downstream->getEffectInstance().get());
    ASSERT_TRUE(downstreamEffect != NULL);

    ImageMetadata extra;
    extra.setInt("exr/extra", 2);
    sourceEffect->setExtraMetadata(extra);
    ASSERT_NO_FATAL_FAILURE(setReaderExpression(downstream->getScriptName() + ".metadata('exr/extra', 1)"));
    ASSERT_EQ(2, readerTag->getValue());
    ASSERT_EQ(std::optional<int>(2), downstreamEffect->getOutputMetadata(1., ViewIdx(0)).getInt("exr/extra"));

    const U64 downstreamHash = downstream->getHashValue();
    const U64 readerHash = reader->getHashValue();
    extra.setInt("exr/extra", 5);
    sourceEffect->setExtraMetadata(extra);
    sourceEffect->changeMetadataUnderSameHash();

    EXPECT_EQ(downstreamHash, downstream->getHashValue());
    EXPECT_EQ(std::optional<int>(5), downstreamEffect->getOutputMetadata(1., ViewIdx(0)).getInt("exr/extra"));
    EXPECT_NE(readerHash, reader->getHashValue());
    EXPECT_EQ(5, readerTag->getValue());
}

TEST_F(MetadataExpressionDependencyTest, ResultComputedAcrossAnInvalidationIsNotKept)
{
    // Edits the source between the expression's metadata read and the end of its evaluation, as
    // a concurrent edit would, and reads the source again so that its hash moves right then.
    ASSERT_NO_FATAL_FAILURE(runPython("pendingSourceTags = []\n"
                                      "def editSourceDuringEvaluation(node):\n"
                                      "    while pendingSourceTags:\n"
                                      "        node.getParam('tag').setValue(pendingSourceTags.pop())\n"
                                      "        node.metadata('exr/tag', 1)\n"
                                      "    return 0\n"));
    const std::string src = source->getScriptName();
    ASSERT_NO_FATAL_FAILURE(setReaderExpression(src + ".metadata('exr/tag', 1) + editSourceDuringEvaluation(" + src + ")"));

    const int time = (int)reader->getEffectInstance()->getCurrentTime() + 3;
    ASSERT_NO_FATAL_FAILURE(runPython("pendingSourceTags.append(2)\n"));

    EXPECT_EQ(1, readerTag->getValueAtTime(time, 0));
    EXPECT_EQ(2, readerTag->getValueAtTime(time, 0));
}

TEST_F(MetadataExpressionDependencyTest, ScalarReadResultComputedAcrossAnInvalidationIsNotKept)
{
    // As above, through the path reading the expression as a plain number, whose result cache is
    // dropped on this same thread, in the middle of the evaluation.
    ASSERT_NO_FATAL_FAILURE(runPython("pendingScalarSourceTags = []\n"
                                      "def editSourceDuringScalarEvaluation(node):\n"
                                      "    while pendingScalarSourceTags:\n"
                                      "        node.getParam('tag').setValue(pendingScalarSourceTags.pop())\n"
                                      "        node.metadata('exr/tag', 1)\n"
                                      "    return 0\n"));
    const std::string src = source->getScriptName();
    ASSERT_NO_FATAL_FAILURE(setReaderExpression(src + ".metadata('exr/tag', 1) + editSourceDuringScalarEvaluation(" + src + ")"));

    const double time = reader->getEffectInstance()->getCurrentTime() + 4;
    ASSERT_NO_FATAL_FAILURE(runPython("pendingScalarSourceTags.append(2)\n"));

    EXPECT_EQ(1., readerTag->getValueAtWithExpression(time, ViewSpec::all(), 0));
    EXPECT_EQ(2., readerTag->getValueAtWithExpression(time, ViewSpec::all(), 0));
}

TEST_F(MetadataExpressionDependencyTest, HashExemptOwnNodeReadFollowsTheNode)
{
    NodePtr modify = createNode(QString::fromUtf8(PLUGINID_NATRON_MODIFYMETADATA));
    ASSERT_TRUE(bool(modify));
    connectNodes(source, modify, 0, true);

    KnobStringPtr label = labelOf(modify);
    ASSERT_TRUE(bool(label));
    ASSERT_FALSE(label->isRenderHashed());
    ASSERT_NO_THROW(label->setExpression(0, "str(thisNode.metadata('exr/tag', 1))", false, true));

    EXPECT_TRUE(hasMetadataReader(modify, label));
    EXPECT_EQ("1", label->getValue());

    sourceTag->setValue(4, ViewSpec::all(), 0);
    modify->getHashValue();

    EXPECT_EQ("4", label->getValue());
}

TEST_F(MetadataExpressionDependencyTest, MetadataCycleIsRefused)
{
    ASSERT_NO_FATAL_FAILURE(setReaderExpression(source->getScriptName() + ".metadata('exr/tag', 1) * 10"));

    EXPECT_THROW(sourceTag->setExpression(0, reader->getScriptName() + ".metadata('exr/tag', 1)", false, true), std::exception);
    EXPECT_TRUE(sourceTag->getExpression(0).empty());
    EXPECT_FALSE(hasMetadataReader(reader, sourceTag));

    const U64 sourceHash = source->getHashValue();
    const U64 readerHash = reader->getHashValue();
    EXPECT_EQ(sourceHash, source->getHashValue());
    EXPECT_EQ(readerHash, reader->getHashValue());
    EXPECT_EQ(10, readerTag->getValue());
}

TEST_F(MetadataExpressionDependencyTest, MetadataCycleLoadedLeniently)
{
    ASSERT_NO_FATAL_FAILURE(setReaderExpression(source->getScriptName() + ".metadata('exr/tag', 1) * 10"));

    ASSERT_NO_THROW(sourceTag->setExpression(0, reader->getScriptName() + ".metadata('exr/tag', 1)", false, false));
    EXPECT_FALSE(sourceTag->isExpressionValid(0, NULL));
    EXPECT_FALSE(readsMetadataOf(sourceTag, 0, reader));
    EXPECT_FALSE(hasMetadataReader(reader, sourceTag));

    // The refused expression is not evaluated, so the knob keeps its own value.
    EXPECT_EQ(1, sourceTag->getValue());
    EXPECT_FALSE(sourceTag->isExpressionValid(0, NULL));

    for (int i = 0; i < 3; ++i) {
        const U64 sourceHash = source->getHashValue();
        const U64 readerHash = reader->getHashValue();
        EXPECT_EQ(sourceHash, source->getHashValue());
        EXPECT_EQ(readerHash, reader->getHashValue());
    }
}

TEST_F(MetadataExpressionDependencyTest, MetadataReadOfADownstreamNodeIsRefused)
{
    NodePtr modify = createNode(QString::fromUtf8(PLUGINID_NATRON_MODIFYMETADATA));
    ASSERT_TRUE(bool(modify));
    connectNodes(source, modify, 0, true);

    EXPECT_THROW(sourceTag->setExpression(0, modify->getScriptName() + ".metadata('exr/tag', 1)", false, true), std::exception);
    EXPECT_TRUE(sourceTag->getExpression(0).empty());
}

TEST_F(MetadataExpressionDependencyTest, ConnectionClosingAMetadataCycleIsRefused)
{
    NodePtr modify = createNode(QString::fromUtf8(PLUGINID_NATRON_MODIFYMETADATA));
    ASSERT_TRUE(bool(modify));
    ASSERT_NO_FATAL_FAILURE(setReaderExpression(modify->getScriptName() + ".metadata('exr/tag', 1)"));

    EXPECT_FALSE(modify->checkIfConnectingInputIsOk(reader.get()));
    EXPECT_TRUE(modify->checkIfConnectingInputIsOk(source.get()));
}

TEST_F(MetadataExpressionDependencyTest, MetadataReadClosingACycleThroughAnExpressionIsRefused)
{
    ASSERT_NO_THROW(sourceTag->setExpression(0, reader->getScriptName() + ".tag.getValue(0)", false, true));

    EXPECT_THROW(readerTag->setExpression(0, source->getScriptName() + ".metadata('exr/tag', 1)", false, true), std::exception);
    EXPECT_TRUE(readerTag->getExpression(0).empty());
    EXPECT_FALSE(hasMetadataReader(source, readerTag));
}

TEST_F(MetadataExpressionDependencyTest, ExpressionClosingAMetadataCycleIsRefused)
{
    ASSERT_NO_FATAL_FAILURE(setReaderExpression(source->getScriptName() + ".metadata('exr/tag', 1) * 10"));

    EXPECT_THROW(sourceTag->setExpression(0, reader->getScriptName() + ".tag.getValue(0)", false, true), std::exception);
    EXPECT_TRUE(sourceTag->getExpression(0).empty());

    const U64 sourceHash = source->getHashValue();
    const U64 readerHash = reader->getHashValue();
    EXPECT_EQ(sourceHash, source->getHashValue());
    EXPECT_EQ(readerHash, reader->getHashValue());
}

TEST_F(MetadataExpressionDependencyTest, ConnectionClosingACycleThroughAnExpressionIsRefused)
{
    NodePtr modify = createNode(QString::fromUtf8(PLUGINID_NATRON_MODIFYMETADATA));
    ASSERT_TRUE(bool(modify));
    ASSERT_NO_FATAL_FAILURE(setReaderExpression(modify->getScriptName() + ".metadata('exr/tag', 1)"));
    ASSERT_NO_THROW(sourceTag->setExpression(0, reader->getScriptName() + ".tag.getValue(0)", false, true));

    EXPECT_FALSE(modify->checkIfConnectingInputIsOk(source.get()));
}

TEST_F(MetadataExpressionDependencyTest, LinkClosingAMetadataCycleIsRefused)
{
    NodePtr other = createNode(QString::fromUtf8(kTestPluginIDMetadataSource));
    ASSERT_TRUE(bool(other));
    KnobIntPtr otherTag = tagKnobOf(other);
    ASSERT_TRUE(bool(otherTag));
    ASSERT_NO_THROW(sourceTag->setExpression(0, reader->getScriptName() + ".metadata('exr/tag', 1) * 10", false, true));
    ASSERT_TRUE(readsMetadataOf(sourceTag, 0, reader));

    // The reader's hash would fold the source's tag, which folds the reader's hash.
    EXPECT_FALSE(readerTag->slaveTo(0, sourceTag, 0));
    EXPECT_FALSE(bool(readerTag->getMaster(0).second));
    EXPECT_TRUE(otherTag->slaveTo(0, sourceTag, 0));

    for (int i = 0; i < 2; ++i) {
        const U64 sourceHash = source->getHashValue();
        const U64 readerHash = reader->getHashValue();
        EXPECT_EQ(sourceHash, source->getHashValue());
        EXPECT_EQ(readerHash, reader->getHashValue());
    }
    EXPECT_EQ(1, readerTag->getValue());
    EXPECT_EQ(10, otherTag->getValue());
}

TEST_F(MetadataExpressionDependencyTest, ConnectionCheckFollowsExpressionAndLinkEdits)
{
    NodePtr modify = createNode(QString::fromUtf8(PLUGINID_NATRON_MODIFYMETADATA));
    ASSERT_TRUE(bool(modify));
    ASSERT_NO_FATAL_FAILURE(setReaderExpression(modify->getScriptName() + ".metadata('exr/tag', 1)"));
    EXPECT_TRUE(modify->checkIfConnectingInputIsOk(source.get()));

    // Each edit below changes which nodes the source's hash folds after a check has gathered them.
    ASSERT_NO_THROW(sourceTag->setExpression(0, reader->getScriptName() + ".tag.getValue(0)", false, true));
    EXPECT_FALSE(modify->checkIfConnectingInputIsOk(source.get()));

    sourceTag->clearExpression(0, true);
    EXPECT_TRUE(modify->checkIfConnectingInputIsOk(source.get()));

    ASSERT_TRUE(sourceTag->slaveTo(0, readerTag, 0));
    EXPECT_FALSE(modify->checkIfConnectingInputIsOk(source.get()));

    sourceTag->unSlave(0, false);
    EXPECT_TRUE(modify->checkIfConnectingInputIsOk(source.get()));
}

TEST_F(MetadataExpressionDependencyTest, HashExemptReadOfADownstreamNodeIsAllowed)
{
    NodePtr modify = createNode(QString::fromUtf8(PLUGINID_NATRON_MODIFYMETADATA));
    NodePtr downstream = createNode(QString::fromUtf8(PLUGINID_NATRON_MODIFYMETADATA));
    ASSERT_TRUE(modify && downstream);
    connectNodes(source, modify, 0, true);
    connectNodes(modify, downstream, 0, true);

    KnobStringPtr label = labelOf(modify);
    ASSERT_TRUE(bool(label));
    ASSERT_FALSE(label->isRenderHashed());
    ASSERT_NO_THROW(label->setExpression(0, "str(" + downstream->getScriptName() + ".metadata('exr/tag', 1))", false, true));

    EXPECT_TRUE(readsMetadataOf(label, 0, downstream));
    EXPECT_TRUE(hasMetadataReader(downstream, label));
    EXPECT_EQ("1", label->getValue());

    sourceTag->setValue(5, ViewSpec::all(), 0);
    downstream->getHashValue();

    EXPECT_EQ("5", label->getValue());
}

TEST_F(MetadataExpressionDependencyTest, ScanAllowsBlanksAroundTheDot)
{
    ASSERT_NO_FATAL_FAILURE(setReaderExpression(source->getScriptName() + " . metadata ('exr/tag', 1) * 10"));

    EXPECT_TRUE(readsMetadataOf(readerTag, 0, source));
    EXPECT_TRUE(hasMetadataReader(source, readerTag));
    EXPECT_EQ(10, readerTag->getValue());
}

TEST_F(MetadataExpressionDependencyTest, ScanSkipsStringLiterals)
{
    ASSERT_NO_FATAL_FAILURE(setReaderExpression("len('Bogus1.metadata(1)') + len(\"" + source->getScriptName() + ".metadataKeys()\") * 0"));

    std::list<NodePtr> sources;
    readerTag->getMetadataDependencies(0, &sources);
    EXPECT_TRUE(sources.empty());
    EXPECT_EQ(18, readerTag->getValue());
}

TEST_F(MetadataExpressionDependencyTest, ScanSkipsComments)
{
    ASSERT_NO_THROW(readerTag->setExpression(0, "# Bogus1.metadata(1) and " + source->getScriptName() + ".metadata('exr/tag')\nret = 3", true, true));

    std::list<NodePtr> sources;
    readerTag->getMetadataDependencies(0, &sources);
    EXPECT_TRUE(sources.empty());
    EXPECT_EQ(3, readerTag->getValue());
}

TEST_F(MetadataExpressionDependencyTest, ScanReadsFStringFields)
{
    ASSERT_NO_FATAL_FAILURE(setReaderExpression("int(f\"{" + source->getScriptName() + ".metadata('exr/tag', 1)}\") * 10"));

    EXPECT_TRUE(readsMetadataOf(readerTag, 0, source));
    EXPECT_EQ(10, readerTag->getValue());
}

TEST_F(MetadataExpressionDependencyTest, ScanSkipsFStringLiteralText)
{
    const std::string src = source->getScriptName();

    ASSERT_NO_FATAL_FAILURE(setReaderExpression("len(f\"" + src + ".metadata('exr/tag') {{" + src + ".metadataKeys()}}\") * 0 + 3"));

    std::list<NodePtr> sources;
    readerTag->getMetadataDependencies(0, &sources);
    EXPECT_TRUE(sources.empty());
    EXPECT_EQ(3, readerTag->getValue());
}

TEST_F(MetadataExpressionDependencyTest, ScanSkipsFStringFormatSpecText)
{
    const std::string date = "__import__('datetime').date(2020, 1, 1)";

    ASSERT_NO_FATAL_FAILURE(setReaderExpression("len(f\"{" + date + ":%Y " + source->getScriptName() + ".metadata('exr/tag')}\") * 0 + 3"));

    std::list<NodePtr> sources;
    readerTag->getMetadataDependencies(0, &sources);
    EXPECT_TRUE(sources.empty());
    EXPECT_EQ(3, readerTag->getValue());
}

TEST_F(MetadataExpressionDependencyTest, ScanReadsFieldsNestedInAFormatSpec)
{
    ASSERT_NO_FATAL_FAILURE(setReaderExpression("int(f\"{3:>{" + source->getScriptName() + ".metadata('exr/tag', 1)}}\")"));

    EXPECT_TRUE(readsMetadataOf(readerTag, 0, source));
    EXPECT_EQ(3, readerTag->getValue());
}

TEST_F(MetadataExpressionDependencyTest, ScanResolvesACallReceiverInsideAnArgumentList)
{
    const std::string lookup = getApp()->getAppIDString() + ".getNode('" + source->getScriptName() + "')";

    ASSERT_NO_FATAL_FAILURE(setReaderExpression("max(0," + lookup + ".metadata('exr/tag', 1))*10"));

    EXPECT_TRUE(readsMetadataOf(readerTag, 0, source));
    EXPECT_EQ(10, readerTag->getValue());
}

TEST_F(MetadataExpressionDependencyTest, RenamingTheSourceRewritesTheExpression)
{
    ASSERT_NO_FATAL_FAILURE(setReaderExpression(source->getScriptName() + ".metadata('exr/tag', 1) * 10"));

    ASSERT_NO_THROW(source->setScriptName("RenamedMetadataSource"));

    EXPECT_NE(std::string::npos, readerTag->getExpression(0).find("RenamedMetadataSource.metadata"));
    EXPECT_TRUE(readerTag->isExpressionValid(0, NULL));
    EXPECT_TRUE(hasMetadataReader(source, readerTag));

    sourceTag->setValue(2, ViewSpec::all(), 0);
    source->getHashValue();
    EXPECT_EQ(20, readerTag->getValue());
}

TEST_F(MetadataExpressionDependencyTest, RenamingTheSourceLeavesKeysAndStringsAlone)
{
    const std::string oldName = source->getScriptName();
    const std::string key = "'exr/" + oldName + "/owner'";
    const std::string text = "'" + oldName + "'";

    ASSERT_NO_FATAL_FAILURE(setReaderExpression(oldName + ".metadata('exr/tag', 1) * 10 + (" + oldName + ".metadata(" + key + ", 1) or 0) + len(" + text + ") * 0"));
    ASSERT_EQ(10, readerTag->getValue());

    ASSERT_NO_THROW(source->setScriptName("RenamedMetadataSource"));

    const std::string renamed = readerTag->getExpression(0);
    EXPECT_NE(std::string::npos, renamed.find("RenamedMetadataSource.metadata('exr/tag', 1)"));
    EXPECT_NE(std::string::npos, renamed.find("RenamedMetadataSource.metadata(" + key));
    EXPECT_NE(std::string::npos, renamed.find("len(" + text + ")"));
    EXPECT_TRUE(readerTag->isExpressionValid(0, NULL));
    EXPECT_EQ(10, readerTag->getValue());
}

TEST_F(MetadataExpressionDependencyTest, RenamingASourceReadForKnobsAndMetadataLeavesKeysAlone)
{
    const std::string oldName = source->getScriptName();
    const std::string key = "'exr/" + oldName + "/owner'";

    ASSERT_NO_FATAL_FAILURE(setReaderExpression(oldName + ".tag.getValue(0) * 10 + (" + oldName + ".metadata(" + key + ", 1) or 0)"));
    ASSERT_EQ(10, readerTag->getValue());

    ASSERT_NO_THROW(source->setScriptName("RenamedMetadataSource"));

    const std::string renamed = readerTag->getExpression(0);
    EXPECT_NE(std::string::npos, renamed.find("RenamedMetadataSource.tag.getValue(0)"));
    EXPECT_NE(std::string::npos, renamed.find("RenamedMetadataSource.metadata(" + key));
    EXPECT_TRUE(readerTag->isExpressionValid(0, NULL));

    sourceTag->setValue(2, ViewSpec::all(), 0);
    source->getHashValue();
    EXPECT_EQ(20, readerTag->getValue());
}

TEST_F(MetadataExpressionDependencyTest, RenamingASourceLookedUpByPathRewritesThePath)
{
    const std::string oldName = source->getScriptName();
    const std::string key = "'exr/" + oldName + "/owner'";
    const std::string lookup = getApp()->getAppIDString() + ".getNode('" + oldName + "')";

    ASSERT_NO_FATAL_FAILURE(setReaderExpression(lookup + ".getParam('tag').getValue(0) * 10 + (" + lookup + ".metadata(" + key + ", 1) or 0)"));
    ASSERT_EQ(10, readerTag->getValue());

    ASSERT_NO_THROW(source->setScriptName("RenamedMetadataSource"));

    const std::string renamed = readerTag->getExpression(0);
    EXPECT_NE(std::string::npos, renamed.find(".getNode('RenamedMetadataSource').getParam('tag')"));
    EXPECT_NE(std::string::npos, renamed.find(".getNode('RenamedMetadataSource').metadata(" + key));
    EXPECT_TRUE(readerTag->isExpressionValid(0, NULL));
    EXPECT_TRUE(readsMetadataOf(readerTag, 0, source));

    sourceTag->setValue(2, ViewSpec::all(), 0);
    source->getHashValue();
    EXPECT_EQ(20, readerTag->getValue());
}

TEST_F(MetadataExpressionDependencyTest, RenamingANodeRewritesItsOwnKnobReadingItByName)
{
    NodePtr modify = createNode(QString::fromUtf8(PLUGINID_NATRON_MODIFYMETADATA));
    ASSERT_TRUE(bool(modify));
    connectNodes(source, modify, 0, true);

    KnobIntPtr value = std::dynamic_pointer_cast<KnobInt>(modify->getKnobByName("intValue1"));
    ASSERT_TRUE(bool(value));
    ASSERT_TRUE(value->isRenderHashed());
    ASSERT_NO_THROW(value->setExpression(0, modify->getScriptName() + ".metadata('exr/tag', 1)", false, true));
    ASSERT_FALSE(hasMetadataReader(modify, value));

    ASSERT_NO_THROW(modify->setScriptName("RenamedModify"));

    EXPECT_NE(std::string::npos, value->getExpression(0).find("RenamedModify.metadata"));
    EXPECT_TRUE(value->isExpressionValid(0, NULL));
    EXPECT_TRUE(readsMetadataOf(value, 0, modify));

    sourceTag->setValue(3, ViewSpec::all(), 0);
    modify->getHashValue();
    EXPECT_EQ(3, value->getValue());
}

TEST_F(MetadataExpressionDependencyTest, DeactivatingTheSourceInvalidatesTheReader)
{
    ASSERT_NO_FATAL_FAILURE(setReaderExpression(source->getScriptName() + ".metadata('exr/tag', 1) * 10"));
    ASSERT_EQ(10, readerTag->getValue());

    source->deactivate();
    EXPECT_FALSE(readerTag->isExpressionValid(0, NULL));

    source->activate();
    EXPECT_TRUE(readerTag->isExpressionValid(0, NULL));
    EXPECT_TRUE(hasMetadataReader(source, readerTag));
    EXPECT_EQ(10, readerTag->getValue());
}
