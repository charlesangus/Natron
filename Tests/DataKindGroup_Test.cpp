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
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "DataKindTestEffect.h"
#include "DeepRenderTestEffect.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/DeepImage.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

class DataKindGroupTest
    : public BaseTest {
protected:
    // A group with no Input or Output yet, so that each test wires its own.
    NodePtr createEmptyGroup(const NodeCollectionPtr& parent)
    {
        CreateNodeArgs args(PLUGINID_NATRON_GROUP, parent);
        args.setProperty<bool>(kCreateNodeArgsPropNodeGroupDisableCreateInitialNodes, true);
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);

        return getApp()->createNode(args);
    }

    NodePtr createRootGroup()
    {
        return createEmptyGroup(getApp()->getProject());
    }

    NodePtr createInside(const NodePtr& group,
                         const std::string& pluginID)
    {
        NodeCollectionPtr collection = std::dynamic_pointer_cast<NodeCollection>(group->getEffectInstance());
        if (!collection) {
            return NodePtr();
        }
        CreateNodeArgs args(pluginID, collection);
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);

        return getApp()->createNode(args);
    }

    NodePtr createRootNode(const char* pluginID)
    {
        return createNode(QString::fromUtf8(pluginID));
    }

    NodePtr createImageRenderSource(int seed)
    {
        NodePtr node = createRootNode(kTestPluginIDImageRenderSource);

        if (!node) {
            return node;
        }
        KnobInt* knob = dynamic_cast<KnobInt*>(node->getKnobByName("seed").get());
        if (!knob) {
            return NodePtr();
        }
        knob->setValue(seed);

        return node;
    }

    static bool hasDataKindMessage(const NodePtr& node)
    {
        if (!node->hasPersistentMessage()) {
            return false;
        }
        QString message;
        int type = 0;
        node->getPersistentMessage(&message, &type, false);

        return message.contains(QString::fromUtf8("kind of data"));
    }

    // Renders node's deep data the way the scheduler does, under frame args carrying an abort
    // flag for EffectInstance::aborted() to read.
    EffectInstance::RenderRoIRetCode renderDeepFrame(const NodePtr& node,
                                                     double time,
                                                     const RectI& roi,
                                                     DeepImagePtr* outputDeepImage)
    {
        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(true, 0);
        ParallelRenderArgsSetter frameRenderArgs(time,
                                                 ViewIdx(0),
                                                 true /*isRenderUserInteraction*/,
                                                 false /*isSequential*/,
                                                 abortInfo,
                                                 node,
                                                 0 /*textureIndex*/,
                                                 getApp()->getTimeLine().get(),
                                                 NodePtr(),
                                                 false /*isAnalysis*/,
                                                 false /*draftMode*/,
                                                 RenderStatsPtr());
        EffectInstance::RenderDeepRoIArgs args(time,
                                               RenderScale::identity,
                                               0 /*mipmapLevel*/,
                                               ViewIdx(0),
                                               false /*byPassCache*/,
                                               roi,
                                               RectD(),
                                               0 /*caller*/,
                                               time);

        return node->getEffectInstance()->renderDeepRoI(args, outputDeepImage);
    }

    // Renders node's float RGBA image through the ordinary image path, request pass included.
    EffectInstance::RenderRoIRetCode renderImageFrame(const NodePtr& node,
                                                      double time,
                                                      const RectI& roi,
                                                      ImagePtr* outputImage)
    {
        outputImage->reset();

        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(true, 0);
        ParallelRenderArgsSetter frameRenderArgs(time,
                                                 ViewIdx(0),
                                                 true /*isRenderUserInteraction*/,
                                                 false /*isSequential*/,
                                                 abortInfo,
                                                 node,
                                                 0 /*textureIndex*/,
                                                 getApp()->getTimeLine().get(),
                                                 NodePtr(),
                                                 false /*isAnalysis*/,
                                                 false /*draftMode*/,
                                                 RenderStatsPtr());
        EffectInstancePtr effect = node->getEffectInstance();

        RectD rod;
        bool isProjectFormat = false;
        if (effect->getRegionOfDefinition_public(node->getHashValue(), time, RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat) == eStatusFailed) {
            return EffectInstance::eRenderRoIRetCodeFailed;
        }

        FrameRequestMap request;
        if (EffectInstance::computeRequestPass(time, ViewIdx(0), 0 /*mipmapLevel*/, rod, node, request) == eStatusFailed) {
            return EffectInstance::eRenderRoIRetCodeFailed;
        }
        frameRenderArgs.updateNodesRequest(request);

        std::list<ImageLayerDesc> components;
        components.push_back(ImageLayerDesc::getRGBAComponents());
        EffectInstance::RenderRoIArgs args(time,
                                           RenderScale::identity,
                                           0 /*mipmapLevel*/,
                                           ViewIdx(0),
                                           false /*byPassCache*/,
                                           roi,
                                           rod,
                                           components,
                                           eImageBitDepthFloat,
                                           false /*calledFromGetImage*/,
                                           0 /*caller*/,
                                           eStorageModeRAM,
                                           time);
        std::map<ImageLayerDesc, ImagePtr> layers;
        const EffectInstance::RenderRoIRetCode code = effect->renderRoI(args, &layers);
        if ((code == EffectInstance::eRenderRoIRetCodeOk) && !layers.empty()) {
            *outputImage = layers.begin()->second;
        }

        return code;
    }
};

namespace {

// Every pixel of roi holds the same samples, sample for sample and channel for channel, in both.
void
expectSameDeepSamples(const DeepImage& actual,
                      const DeepImage& expected,
                      const RectI& roi)
{
    const std::vector<std::string> channelNames = deepRenderTestChannelNames();
    std::vector<std::string> names = channelNames;

    names.push_back("Z");
    names.push_back("ZBack");

    for (int y = roi.y1; y < roi.y2; ++y) {
        for (int x = roi.x1; x < roi.x2; ++x) {
            std::size_t actualIndex;
            std::size_t expectedIndex;
            ASSERT_TRUE(deepRenderTestPixelIndex(actual, x, y, &actualIndex)) << "at pixel (" << x << ", " << y << ")";
            ASSERT_TRUE(deepRenderTestPixelIndex(expected, x, y, &expectedIndex)) << "at pixel (" << x << ", " << y << ")";
            const U32 count = actual.getSampleTable().getCount(actualIndex);
            ASSERT_EQ(expected.getSampleTable().getCount(expectedIndex), count) << "at pixel (" << x << ", " << y << ")";
            ASSERT_EQ(deepRenderTestSampleCount(x, y), count) << "at pixel (" << x << ", " << y << ")";
            const U64 actualOffset = actual.getSampleTable().getOffset(actualIndex);
            const U64 expectedOffset = expected.getSampleTable().getOffset(expectedIndex);
            for (std::size_t c = 0; c < names.size(); ++c) {
                const DeepChannelBuffer* actualChannel = actual.getChannel(names[c]);
                const DeepChannelBuffer* expectedChannel = expected.getChannel(names[c]);
                ASSERT_TRUE(actualChannel && expectedChannel) << "missing channel " << names[c];
                for (U32 s = 0; s < count; ++s) {
                    ASSERT_FLOAT_EQ(expectedChannel->data()[expectedOffset + s], actualChannel->data()[actualOffset + s])
                        << "at pixel (" << x << ", " << y << ") sample " << s << " channel " << names[c];
                }
            }
        }
    }
}

} // namespace

// A group wrapping a deep op is deep at its output, so a deep-only consumer connects to it, and
// what that consumer renders through the group is exactly what the same chain renders without it.
TEST_F(DataKindGroupTest, DeepChainThroughGroupResolvesDeepAndRendersTheSameSamples)
{
    NodePtr source = createRootNode(kTestPluginIDDeepRenderSource);
    NodePtr group = createRootGroup();
    ASSERT_TRUE(source && group);
    NodePtr groupInput = createInside(group, PLUGINID_NATRON_INPUT);
    NodePtr innerGain = createInside(group, kTestPluginIDDeepRenderGain);
    NodePtr groupOutput = createInside(group, PLUGINID_NATRON_OUTPUT);
    ASSERT_TRUE(groupInput && innerGain && groupOutput);
    ASSERT_TRUE(innerGain->connectInput(groupInput, 0));
    ASSERT_TRUE(groupOutput->connectInput(innerGain, 0));
    ASSERT_GE(group->getNInputs(), 1);

    EXPECT_EQ(Node::eCanConnectInput_ok, group->canConnectInput(source, 0));
    ASSERT_TRUE(group->connectInput(source, 0));

    bool ambiguous = true;
    EXPECT_EQ(eDataKindDeep, group->getEffectiveOutputDataKind(&ambiguous));
    EXPECT_FALSE(ambiguous);
    EXPECT_TRUE(group->getEffectInstance()->producesDeepData());

    NodePtr outerGain = createRootNode(kTestPluginIDDeepRenderGain);
    ASSERT_TRUE(bool(outerGain));
    EXPECT_EQ(Node::eCanConnectInput_ok, outerGain->canConnectInput(group, 0));
    ASSERT_TRUE(outerGain->connectInput(group, 0));
    EXPECT_FALSE(hasDataKindMessage(outerGain));

    NodePtr referenceSource = createRootNode(kTestPluginIDDeepRenderSource);
    NodePtr referenceGain = createRootNode(kTestPluginIDDeepRenderGain);
    NodePtr referenceOuterGain = createRootNode(kTestPluginIDDeepRenderGain);
    ASSERT_TRUE(referenceSource && referenceGain && referenceOuterGain);
    ASSERT_TRUE(referenceGain->connectInput(referenceSource, 0));
    ASSERT_TRUE(referenceOuterGain->connectInput(referenceGain, 0));

    const RectI roi(0, 0, kDeepRenderTestWidth, kDeepRenderTestHeight);

    DeepImagePtr throughGroup;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(outerGain, 1., roi, &throughGroup));
    ASSERT_TRUE(throughGroup != NULL);

    DeepImagePtr withoutGroup;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(referenceOuterGain, 1., roi, &withoutGroup));
    ASSERT_TRUE(withoutGroup != NULL);

    expectSameDeepSamples(*throughGroup, *withoutGroup, roi);
}

// The image case is what groups did before they carried a kind of their own: image at the
// output, accepted by an image consumer, refused by a deep one, and rendered unchanged.
TEST_F(DataKindGroupTest, ImageChainThroughGroupStaysImage)
{
    const int seed = 3;
    NodePtr source = createImageRenderSource(seed);
    NodePtr group = createRootGroup();
    ASSERT_TRUE(source && group);
    NodePtr groupInput = createInside(group, PLUGINID_NATRON_INPUT);
    NodePtr innerDot = createInside(group, PLUGINID_NATRON_DOT);
    NodePtr groupOutput = createInside(group, PLUGINID_NATRON_OUTPUT);
    ASSERT_TRUE(groupInput && innerDot && groupOutput);
    ASSERT_TRUE(innerDot->connectInput(groupInput, 0));
    ASSERT_TRUE(groupOutput->connectInput(innerDot, 0));
    ASSERT_TRUE(group->connectInput(source, 0));

    bool ambiguous = true;
    EXPECT_EQ(eDataKindImage, group->getEffectiveOutputDataKind(&ambiguous));
    EXPECT_FALSE(ambiguous);
    EXPECT_FALSE(group->getEffectInstance()->producesDeepData());

    NodePtr imageSink = createRootNode(kTestPluginIDDataKindImageSink);
    NodePtr deepSink = createRootNode(kTestPluginIDDataKindDeepSink);
    ASSERT_TRUE(imageSink && deepSink);
    EXPECT_EQ(Node::eCanConnectInput_ok, imageSink->canConnectInput(group, 0));
    EXPECT_EQ(Node::eCanConnectInput_incompatibleDataKind, deepSink->canConnectInput(group, 0));

    NodePtr outerDot = createRootNode(PLUGINID_NATRON_DOT);
    ASSERT_TRUE(bool(outerDot));
    ASSERT_TRUE(outerDot->connectInput(group, 0));
    EXPECT_EQ(eDataKindImage, outerDot->getEffectiveOutputDataKind());

    const RectI roi(0, 0, kImageRenderTestWidth, kImageRenderTestHeight);
    ImagePtr image;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderImageFrame(outerDot, 1., roi, &image));
    ASSERT_TRUE(image != NULL);

    Image::ReadAccess access = image->getReadRights();
    for (int y = roi.y1; y < roi.y2; ++y) {
        for (int x = roi.x1; x < roi.x2; ++x) {
            const float* pixel = (const float*)access.pixelAt(x, y);
            ASSERT_TRUE(pixel != NULL) << "at pixel (" << x << ", " << y << ")";
            for (int c = 0; c < 4; ++c) {
                ASSERT_FLOAT_EQ(imageRenderTestValue(seed, x, y, c), pixel[c]) << "at pixel (" << x << ", " << y << ") channel " << c;
            }
        }
    }
}

// Nothing reaches the GroupOutput, so the group's output is fed by nothing whatever its input
// carries: like an unfed Dot it is unconstrained until a consumer constrains it.
TEST_F(DataKindGroupTest, UnconnectedGroupOutputFollowsTheUnfedPolymorphicRules)
{
    NodePtr deepSource = createRootNode(kTestPluginIDDataKindDeepSource);
    NodePtr group = createRootGroup();
    ASSERT_TRUE(deepSource && group);
    NodePtr groupInput = createInside(group, PLUGINID_NATRON_INPUT);
    NodePtr innerDot = createInside(group, PLUGINID_NATRON_DOT);
    NodePtr groupOutput = createInside(group, PLUGINID_NATRON_OUTPUT);
    ASSERT_TRUE(groupInput && innerDot && groupOutput);
    ASSERT_TRUE(innerDot->connectInput(groupInput, 0));

    bool ambiguous = true;
    EXPECT_EQ(eDataKindPolymorphic, group->getEffectiveOutputDataKind(&ambiguous));
    EXPECT_FALSE(ambiguous);

    ASSERT_TRUE(group->connectInput(deepSource, 0));
    EXPECT_EQ(eDataKindDeep, groupInput->getEffectiveOutputDataKind());
    EXPECT_EQ(eDataKindPolymorphic, group->getEffectiveOutputDataKind(&ambiguous));
    EXPECT_FALSE(ambiguous);

    NodePtr imageSink = createRootNode(kTestPluginIDDataKindImageSink);
    ASSERT_TRUE(bool(imageSink));
    EXPECT_EQ(Node::eCanConnectInput_ok, imageSink->canConnectInput(group, 0));
    ASSERT_TRUE(imageSink->connectInput(group, 0));
    EXPECT_EQ(eDataKindImage, group->getEffectiveOutputDataKind(&ambiguous));
    EXPECT_FALSE(ambiguous);
    EXPECT_FALSE(hasDataKindMessage(imageSink));
}

// An input nothing feeds from outside takes what the GroupInput's consumers inside require, and
// the requirement reaches anything upstream of that input, so an image source is refused there.
TEST_F(DataKindGroupTest, UnconnectedGroupInputTakesWhatItsInnerConsumersRequire)
{
    NodePtr group = createRootGroup();
    ASSERT_TRUE(bool(group));
    NodePtr groupInput = createInside(group, PLUGINID_NATRON_INPUT);
    NodePtr innerDeepSink = createInside(group, kTestPluginIDDataKindDeepSink);
    NodePtr groupOutput = createInside(group, PLUGINID_NATRON_OUTPUT);
    ASSERT_TRUE(groupInput && innerDeepSink && groupOutput);
    ASSERT_TRUE(innerDeepSink->connectInput(groupInput, 0));
    ASSERT_TRUE(groupOutput->connectInput(innerDeepSink, 0));

    EXPECT_EQ(eDataKindDeep, groupInput->getEffectiveOutputDataKind());
    EXPECT_EQ(eDataKindDeep, group->getEffectiveOutputDataKind());

    NodePtr imageSource = createImageRenderSource(0);
    NodePtr deepSource = createRootNode(kTestPluginIDDataKindDeepSource);
    ASSERT_TRUE(imageSource && deepSource);

    NodePtr conflictingNode;
    EXPECT_EQ(Node::eCanConnectInput_incompatibleDataKind, group->canConnectInput(imageSource, 0, &conflictingNode));
    EXPECT_EQ(innerDeepSink.get(), conflictingNode.get());
    EXPECT_EQ(Node::eCanConnectInput_ok, group->canConnectInput(deepSource, 0));

    NodePtr dot = createRootNode(PLUGINID_NATRON_DOT);
    ASSERT_TRUE(bool(dot));
    ASSERT_TRUE(group->connectInput(dot, 0));
    EXPECT_EQ(eDataKindDeep, dot->getEffectiveOutputDataKind());

    conflictingNode.reset();
    EXPECT_EQ(Node::eCanConnectInput_incompatibleDataKind, dot->canConnectInput(imageSource, 0, &conflictingNode));
    EXPECT_EQ(innerDeepSink.get(), conflictingNode.get());
    EXPECT_EQ(Node::eCanConnectInput_ok, dot->canConnectInput(deepSource, 0));
}

// A mismatch between what reaches a group and what its inside requires is refused at the
// external edge that would introduce it, naming the inner node that cannot take it; and the
// group's output carries its kind on to the edges below it.
TEST_F(DataKindGroupTest, MismatchInsideAGroupIsReportedOnTheEdgeThatCausesIt)
{
    NodePtr deepSource = createRootNode(kTestPluginIDDataKindDeepSource);
    NodePtr group = createRootGroup();
    ASSERT_TRUE(deepSource && group);
    NodePtr groupInput = createInside(group, PLUGINID_NATRON_INPUT);
    NodePtr innerDot = createInside(group, PLUGINID_NATRON_DOT);
    NodePtr innerImageSink = createInside(group, kTestPluginIDDataKindImageSink);
    NodePtr groupOutput = createInside(group, PLUGINID_NATRON_OUTPUT);
    ASSERT_TRUE(groupInput && innerDot && innerImageSink && groupOutput);
    ASSERT_TRUE(innerDot->connectInput(groupInput, 0));
    ASSERT_TRUE(innerImageSink->connectInput(innerDot, 0));
    ASSERT_TRUE(groupOutput->connectInput(innerImageSink, 0));

    NodePtr conflictingNode;
    EXPECT_EQ(Node::eCanConnectInput_incompatibleDataKind, group->canConnectInput(deepSource, 0, &conflictingNode));
    EXPECT_EQ(innerImageSink.get(), conflictingNode.get());

    // The other way round: once the group's input carries deep, the inner edge is the one refused.
    NodePtr passGroup = createRootGroup();
    ASSERT_TRUE(bool(passGroup));
    NodePtr passInput = createInside(passGroup, PLUGINID_NATRON_INPUT);
    NodePtr passOutput = createInside(passGroup, PLUGINID_NATRON_OUTPUT);
    ASSERT_TRUE(passInput && passOutput);
    ASSERT_TRUE(passOutput->connectInput(passInput, 0));
    ASSERT_TRUE(passGroup->connectInput(deepSource, 0));
    EXPECT_EQ(eDataKindDeep, passGroup->getEffectiveOutputDataKind());

    NodePtr outerImageSink = createRootNode(kTestPluginIDDataKindImageSink);
    ASSERT_TRUE(bool(outerImageSink));
    conflictingNode.reset();
    EXPECT_EQ(Node::eCanConnectInput_incompatibleDataKind, outerImageSink->canConnectInput(passGroup, 0, &conflictingNode));
    EXPECT_EQ(passGroup.get(), conflictingNode.get());

    // Hand-wired past the check, the error lands on the node that cannot take what reaches it.
    ASSERT_TRUE(outerImageSink->connectInput(passGroup, 0));
    EXPECT_TRUE(hasDataKindMessage(outerImageSink));
    EXPECT_FALSE(hasDataKindMessage(passGroup));
}

// Rewiring what feeds the GroupOutput changes the group's kind, and the consumers below the group
// are judged again against the new one.
TEST_F(DataKindGroupTest, SwappingWhatFeedsTheGroupOutputFlipsTheGroupsKind)
{
    NodePtr group = createRootGroup();
    ASSERT_TRUE(bool(group));
    NodePtr imageSource = createInside(group, kTestPluginIDImageRenderSource);
    NodePtr deepSource = createInside(group, kTestPluginIDDataKindDeepSource);
    NodePtr groupOutput = createInside(group, PLUGINID_NATRON_OUTPUT);
    ASSERT_TRUE(imageSource && deepSource && groupOutput);
    ASSERT_TRUE(groupOutput->connectInput(imageSource, 0));

    NodePtr imageSink = createRootNode(kTestPluginIDDataKindImageSink);
    ASSERT_TRUE(bool(imageSink));
    EXPECT_EQ(eDataKindImage, group->getEffectiveOutputDataKind());
    EXPECT_EQ(Node::eCanConnectInput_ok, imageSink->canConnectInput(group, 0));
    ASSERT_TRUE(imageSink->connectInput(group, 0));
    EXPECT_FALSE(hasDataKindMessage(imageSink));

    ASSERT_TRUE(groupOutput->replaceInput(deepSource, 0));
    EXPECT_EQ(eDataKindDeep, group->getEffectiveOutputDataKind());
    EXPECT_TRUE(hasDataKindMessage(imageSink));

    ASSERT_TRUE(groupOutput->replaceInput(imageSource, 0));
    EXPECT_EQ(eDataKindImage, group->getEffectiveOutputDataKind());
    EXPECT_FALSE(hasDataKindMessage(imageSink));
}

// The same one level deeper: the swap happens inside a group nested in the one the consumer sees.
TEST_F(DataKindGroupTest, SwappingInsideANestedGroupFlipsTheOuterGroupsKind)
{
    NodePtr outer = createRootGroup();
    ASSERT_TRUE(bool(outer));
    NodePtr nested = createEmptyGroup(std::dynamic_pointer_cast<NodeCollection>(outer->getEffectInstance()));
    NodePtr outerOutput = createInside(outer, PLUGINID_NATRON_OUTPUT);
    ASSERT_TRUE(nested && outerOutput);
    NodePtr imageSource = createInside(nested, kTestPluginIDImageRenderSource);
    NodePtr deepSource = createInside(nested, kTestPluginIDDataKindDeepSource);
    NodePtr nestedOutput = createInside(nested, PLUGINID_NATRON_OUTPUT);
    ASSERT_TRUE(imageSource && deepSource && nestedOutput);
    ASSERT_TRUE(nestedOutput->connectInput(imageSource, 0));
    ASSERT_TRUE(outerOutput->connectInput(nested, 0));

    NodePtr imageSink = createRootNode(kTestPluginIDDataKindImageSink);
    ASSERT_TRUE(bool(imageSink));
    EXPECT_EQ(eDataKindImage, outer->getEffectiveOutputDataKind());
    ASSERT_TRUE(imageSink->connectInput(outer, 0));
    EXPECT_FALSE(hasDataKindMessage(imageSink));

    ASSERT_TRUE(nestedOutput->replaceInput(deepSource, 0));
    EXPECT_EQ(eDataKindDeep, nested->getEffectiveOutputDataKind());
    EXPECT_EQ(eDataKindDeep, outer->getEffectiveOutputDataKind());
    EXPECT_TRUE(hasDataKindMessage(imageSink));

    ASSERT_TRUE(nestedOutput->replaceInput(imageSource, 0));
    EXPECT_EQ(eDataKindImage, outer->getEffectiveOutputDataKind());
    EXPECT_FALSE(hasDataKindMessage(imageSink));
}

// A nested group's unconnected input takes the requirement of what its GroupInput feeds, and
// that requirement climbs out through the outer group's GroupInput to whatever feeds it.
TEST_F(DataKindGroupTest, RequirementsCrossNestedGroupInputs)
{
    NodePtr outer = createRootGroup();
    ASSERT_TRUE(bool(outer));
    NodePtr outerInput = createInside(outer, PLUGINID_NATRON_INPUT);
    NodePtr nested = createEmptyGroup(std::dynamic_pointer_cast<NodeCollection>(outer->getEffectInstance()));
    ASSERT_TRUE(outerInput && nested);
    NodePtr nestedInput = createInside(nested, PLUGINID_NATRON_INPUT);
    NodePtr deepSink = createInside(nested, kTestPluginIDDataKindDeepSink);
    ASSERT_TRUE(nestedInput && deepSink);
    ASSERT_TRUE(deepSink->connectInput(nestedInput, 0));
    ASSERT_TRUE(nested->connectInput(outerInput, 0));

    EXPECT_EQ(eDataKindDeep, outerInput->getEffectiveOutputDataKind());

    NodePtr imageSource = createImageRenderSource(0);
    ASSERT_TRUE(bool(imageSource));
    NodePtr conflictingNode;
    EXPECT_EQ(Node::eCanConnectInput_incompatibleDataKind, outer->canConnectInput(imageSource, 0, &conflictingNode));
    EXPECT_EQ(deepSink.get(), conflictingNode.get());
}
