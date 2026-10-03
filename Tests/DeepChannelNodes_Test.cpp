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

#include <cstddef>
#include <initializer_list>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "CacheMemoryPressureGuard.h"
#include "DeepRenderTestEffect.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/DeepImage.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Deep/DeepRemoveLayers.h"
#include "Engine/Nodes/Deep/DeepToImage.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Plugin.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;
const int kWidth = 4;
const int kHeight = 3;
const int kSamplesPerPixel = 2;

// The colour storage entry is named by its layout, e.g. "Color(1)", so a narrowed plane shows.
std::vector<std::string>
describe(const std::list<ImageLayerDesc>& layers)
{
    std::vector<std::string> ids;

    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        if (it->isColorLayer()) {
            std::ostringstream os;
            os << "Color(" << it->getNumComponents() << ")";
            ids.push_back(os.str());
        } else {
            ids.push_back(it->getLayerID());
        }
    }

    return ids;
}

std::vector<std::string>
ids(std::initializer_list<const char*> list)
{
    std::vector<std::string> result;

    for (const char* id : list) {
        result.push_back(id);
    }

    return result;
}

std::set<std::string>
nameSet(std::initializer_list<const char*> list)
{
    std::set<std::string> result;

    for (const char* name : list) {
        result.insert(name);
    }

    return result;
}

std::set<std::string>
channelNamesOf(const DeepImage& image)
{
    std::set<std::string> names;

    for (std::map<std::string, DeepChannelBuffer>::const_iterator it = image.getChannels().begin(); it != image.getChannels().end(); ++it) {
        names.insert(it->first);
    }

    return names;
}

const char* const kLayeredChannels[] = {
    "R", "G", "B", "A",
    "diffuse.R", "diffuse.G", "diffuse.B",
    "specular.R", "specular.G", "specular.B"
};

// Every pixel holds kSamplesPerPixel point samples at depths 1, 2, ..., each channel a value
// distinct per channel, pixel and sample.
DeepImagePtr
makeLayeredImage()
{
    const RectI bounds(0, 0, kWidth, kHeight);
    DeepImagePtr image = std::make_shared<DeepImage>(bounds, RenderScale::identity, ViewIdx(0));

    SampleTable& table = image->getSampleTableForWriting();
    for (std::size_t p = 0; p < (std::size_t)(kWidth * kHeight); ++p) {
        table.setCount(p, (U32)kSamplesPerPixel);
    }
    table.recomputeOffsets();

    const std::size_t total = (std::size_t)table.getTotalSampleCount();
    float* z = image->getChannelForWriting("Z").dataForWriting();
    float* zback = image->getChannelForWriting("ZBack").dataForWriting();
    for (std::size_t s = 0; s < total; ++s) {
        z[s] = 1.f + (float)(s % kSamplesPerPixel);
        zback[s] = z[s];
    }

    const std::size_t nChannels = sizeof(kLayeredChannels) / sizeof(kLayeredChannels[0]);
    for (std::size_t c = 0; c < nChannels; ++c) {
        float* data = image->getChannelForWriting(kLayeredChannels[c]).dataForWriting();
        for (std::size_t s = 0; s < total; ++s) {
            data[s] = 0.05f * (float)(c + 1) + 0.001f * (float)s;
        }
    }
    image->setTidy(true);

    return image;
}

} // namespace

class DeepChannelNodesTest
    : public BaseTest {
protected:
    // Tests here compare a node's output storage with its input's cached render; see
    // CacheMemoryPressureGuard.h.
    DisableUnreachableRAMPurging _noPurging;

    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        deepSyntheticSourceImages().clear();
        _nextSlot = 1;
    }

    // Destroyed nodes take their cache entries with them, so the next test's nodes, which may get
    // the same script names and so the same hashes, cannot be served this test's renders.
    virtual void TearDown() OVERRIDE
    {
        for (std::vector<NodePtr>::reverse_iterator it = _nodes.rbegin(); it != _nodes.rend(); ++it) {
            (*it)->destroyNode(false, false);
        }
        _nodes.clear();
        deepSyntheticSourceImages().clear();
        BaseTest::TearDown();
    }

    NodePtr createTrackedNode(const char* pluginID)
    {
        NodePtr node = createNode(QString::fromUtf8(pluginID));

        if (node) {
            _nodes.push_back(node);
        }

        return node;
    }

    NodePtr createSyntheticSource(const DeepImagePtr& image)
    {
        const int slot = _nextSlot++;

        deepSyntheticSourceImages()[slot] = image;
        NodePtr node = createTrackedNode(kTestPluginIDDeepSyntheticSource);
        if (!node) {
            return node;
        }
        KnobInt* knob = dynamic_cast<KnobInt*>(node->getKnobByName("slot").get());
        if (!knob) {
            return NodePtr();
        }
        knob->setValue(slot);

        return node;
    }

    // A DeepRemoveLayers fed by a fresh layered source, returned in *source.
    NodePtr createRemoveOnLayeredSource(NodePtr* source)
    {
        *source = createSyntheticSource(makeLayeredImage());
        if (!*source) {
            return NodePtr();
        }
        NodePtr remove = createTrackedNode(PLUGINID_NATRON_DEEPREMOVELAYERS);
        if (!remove) {
            return NodePtr();
        }
        connectNodes(*source, remove, 0, true);

        return remove;
    }

    static KnobChannelSetPtr channelsKnob(const NodePtr& node)
    {
        return std::dynamic_pointer_cast<KnobChannelSet>(node->getKnobByName(kDeepRemoveLayersParamChannels));
    }

    static void setKeep(const NodePtr& node)
    {
        KnobChoicePtr operation = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kDeepRemoveLayersParamOperation));

        ASSERT_TRUE(bool(operation));
        operation->setValue((int)DeepRemoveLayers::eOperationKeep);
    }

    static std::vector<std::string> present(const NodePtr& node)
    {
        std::list<ImageLayerDesc> layers;

        node->getEffectInstance()->getPresentLayers(kTime, ViewIdx(0), -1, &layers);

        return describe(layers);
    }

    static bool isIdentityOfSource(const NodePtr& node)
    {
        EffectInstancePtr effect = node->getEffectInstance();
        const RectI window(0, 0, kWidth, kHeight);
        double inputTime = 0.;
        ViewIdx inputView(0);
        int inputNb = -1;
        const bool identity = effect->isIdentity_public(false, effect->getRenderHash(), kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb);

        return identity && (inputNb == 0);
    }

    // Renders node's deep data the way the scheduler does, under frame args carrying an abort
    // flag for EffectInstance::aborted() to read.
    EffectInstance::RenderRoIRetCode renderDeepFrame(const NodePtr& node,
                                                     DeepImagePtr* outputDeepImage)
    {
        const RectI roi(0, 0, kWidth, kHeight);
        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(true, 0);
        ParallelRenderArgsSetter frameRenderArgs(kTime,
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
        EffectInstance::RenderDeepRoIArgs args(kTime,
                                               RenderScale::identity,
                                               0 /*mipmapLevel*/,
                                               ViewIdx(0),
                                               false /*byPassCache*/,
                                               roi,
                                               RectD(),
                                               0 /*caller*/,
                                               kTime);

        return node->getEffectInstance()->renderDeepRoI(args, outputDeepImage);
    }

    // Renders node's float RGBA image through the ordinary image path, request pass included.
    EffectInstance::RenderRoIRetCode renderImageFrame(const NodePtr& node,
                                                      ImagePtr* outputImage)
    {
        outputImage->reset();

        const RectI roi(0, 0, kWidth, kHeight);
        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(true, 0);
        ParallelRenderArgsSetter frameRenderArgs(kTime,
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
        if (effect->getRegionOfDefinition_public(node->getHashValue(), kTime, RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat) == eStatusFailed) {
            return EffectInstance::eRenderRoIRetCodeFailed;
        }

        FrameRequestMap request;
        if (EffectInstance::computeRequestPass(kTime, ViewIdx(0), 0 /*mipmapLevel*/, rod, node, request) == eStatusFailed) {
            return EffectInstance::eRenderRoIRetCodeFailed;
        }
        frameRenderArgs.updateNodesRequest(request);

        std::list<ImageLayerDesc> components;
        components.push_back(ImageLayerDesc::getRGBAComponents());
        EffectInstance::RenderRoIArgs args(kTime,
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
                                           kTime);
        std::map<ImageLayerDesc, ImagePtr> layers;
        const EffectInstance::RenderRoIRetCode code = effect->renderRoI(args, &layers);
        if ((code == EffectInstance::eRenderRoIRetCodeOk) && !layers.empty()) {
            *outputImage = layers.begin()->second;
        }

        return code;
    }

    // Renders source and node, checks the output keeps exactly expected, and that the sample
    // table and every kept channel are the input's own storage.
    void expectRenderKeeps(const NodePtr& source,
                           const NodePtr& node,
                           const std::set<std::string>& expected)
    {
        DeepImagePtr input;
        ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(source, &input));
        ASSERT_TRUE(input != NULL);

        DeepImagePtr output;
        ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(node, &output));
        ASSERT_TRUE(output != NULL);
        EXPECT_FALSE(node->hasPersistentMessage());

        EXPECT_EQ(expected, channelNamesOf(*output));
        EXPECT_TRUE(output->hasChannel("A"));
        EXPECT_TRUE(output->sharesSampleTableWith(*input));
        EXPECT_EQ(input->getSampleTable().getTotalSampleCount(), output->getSampleTable().getTotalSampleCount());
        for (std::set<std::string>::const_iterator it = expected.begin(); it != expected.end(); ++it) {
            EXPECT_TRUE(output->sharesChannelStorageWith(*input, *it)) << *it;
        }
    }

    std::vector<NodePtr> _nodes;
    int _nextSlot;
};

TEST_F(DeepChannelNodesTest, DeepRemoveLayersIsRegisteredAsADeepNode)
{
    const PluginsMap& plugins = appPTR->getPluginsList();

    ASSERT_TRUE(plugins.find(PLUGINID_NATRON_DEEPREMOVELAYERS) != plugins.end());

    NodePtr remove = createTrackedNode(PLUGINID_NATRON_DEEPREMOVELAYERS);
    ASSERT_TRUE(bool(remove));
    EffectInstancePtr effect = remove->getEffectInstance();
    EXPECT_EQ(1, effect->getNInputs());
    EXPECT_EQ(eDataKindDeep, effect->getInputDataKind(0));
    EXPECT_EQ(eDataKindDeep, effect->getOutputDataKind());
    {
        std::list<std::string> grouping;
        effect->getPluginGrouping(&grouping);
        ASSERT_EQ((std::size_t)1, grouping.size());
        EXPECT_EQ(PLUGIN_GROUP_DEEP, grouping.front());
    }

    KnobChannelSetPtr channels = channelsKnob(remove);
    ASSERT_TRUE(bool(channels));
    EXPECT_FALSE(channels->getWithChannelButtons());
    const std::vector<ChannelSetRow> rows = channels->getRows();
    ASSERT_EQ((std::size_t)1, rows.size());
    EXPECT_EQ(ChannelSetRow::eModeNone, rows[0].mode);
}

TEST_F(DeepChannelNodesTest, NewNodeIsAnIdentity)
{
    NodePtr source;
    NodePtr remove = createRemoveOnLayeredSource(&source);

    ASSERT_TRUE(bool(remove));
    EXPECT_TRUE(isIdentityOfSource(remove));
    EXPECT_EQ(ids({ "Color(4)", "diffuse", "specular" }), present(remove));
}

TEST_F(DeepChannelNodesTest, RemovingDiffuseDropsOnlyItsChannelsAndSharesTheRest)
{
    NodePtr source;
    NodePtr remove = createRemoveOnLayeredSource(&source);

    ASSERT_TRUE(bool(remove));
    KnobChannelSetPtr channels = channelsKnob(remove);
    ASSERT_TRUE(bool(channels));
    channels->setLayer(0, "diffuse", NULL);

    EXPECT_FALSE(isIdentityOfSource(remove));
    EXPECT_EQ(ids({ "Color(4)", "specular" }), present(remove));
    expectRenderKeeps(source, remove, nameSet({ "R", "G", "B", "A", "Z", "ZBack", "specular.R", "specular.G", "specular.B" }));
}

TEST_F(DeepChannelNodesTest, RemovingAlphaAloneIsAnIdentity)
{
    NodePtr source;
    NodePtr remove = createRemoveOnLayeredSource(&source);

    ASSERT_TRUE(bool(remove));
    KnobChannelSetPtr channels = channelsKnob(remove);
    ASSERT_TRUE(bool(channels));
    channels->setLayer(0, kNatronColorViewAlpha, NULL);

    EXPECT_TRUE(isIdentityOfSource(remove));
    EXPECT_EQ(ids({ "Color(4)", "diffuse", "specular" }), present(remove));
}

TEST_F(DeepChannelNodesTest, RemovingRgbaOrRgbDropsRgbAndKeepsAlpha)
{
    const char* const views[] = { kNatronColorViewRGBA, kNatronColorViewRGB };

    for (int v = 0; v < 2; ++v) {
        NodePtr source;
        NodePtr remove = createRemoveOnLayeredSource(&source);

        ASSERT_TRUE(bool(remove)) << views[v];
        KnobChannelSetPtr channels = channelsKnob(remove);
        ASSERT_TRUE(bool(channels));
        channels->setLayer(0, views[v], NULL);

        EXPECT_FALSE(isIdentityOfSource(remove)) << views[v];
        EXPECT_EQ(ids({ "Color(1)", "diffuse", "specular" }), present(remove)) << views[v];
        expectRenderKeeps(source, remove, nameSet({ "A", "Z", "ZBack", "diffuse.R", "diffuse.G", "diffuse.B", "specular.R", "specular.G", "specular.B" }));
    }
}

TEST_F(DeepChannelNodesTest, KeepingARegexKeepsTheLayersItMatchesAndAlpha)
{
    NodePtr source;
    NodePtr remove = createRemoveOnLayeredSource(&source);

    ASSERT_TRUE(bool(remove));
    setKeep(remove);
    KnobChannelSetPtr channels = channelsKnob(remove);
    ASSERT_TRUE(bool(channels));
    channels->setRegex(0, "spec.*");

    EXPECT_EQ(ids({ "Color(1)", "specular" }), present(remove));
    expectRenderKeeps(source, remove, nameSet({ "A", "Z", "ZBack", "specular.R", "specular.G", "specular.B" }));
}

TEST_F(DeepChannelNodesTest, KeepingAlphaKeepsOnlyAlpha)
{
    NodePtr source;
    NodePtr remove = createRemoveOnLayeredSource(&source);

    ASSERT_TRUE(bool(remove));
    setKeep(remove);
    KnobChannelSetPtr channels = channelsKnob(remove);
    ASSERT_TRUE(bool(channels));
    channels->setLayer(0, kNatronColorViewAlpha, NULL);

    EXPECT_EQ(ids({ "Color(1)" }), present(remove));
    expectRenderKeeps(source, remove, nameSet({ "A", "Z", "ZBack" }));
}

TEST_F(DeepChannelNodesTest, KeepingNoneStillKeepsAlphaAndDepths)
{
    NodePtr source;
    NodePtr remove = createRemoveOnLayeredSource(&source);

    ASSERT_TRUE(bool(remove));
    setKeep(remove);

    EXPECT_FALSE(isIdentityOfSource(remove));
    EXPECT_EQ(ids({ "Color(1)" }), present(remove));
    expectRenderKeeps(source, remove, nameSet({ "A", "Z", "ZBack" }));
}

TEST_F(DeepChannelNodesTest, RemovingEverythingByRegexStillKeepsAlphaAndDepths)
{
    NodePtr source;
    NodePtr remove = createRemoveOnLayeredSource(&source);

    ASSERT_TRUE(bool(remove));
    KnobChannelSetPtr channels = channelsKnob(remove);
    ASSERT_TRUE(bool(channels));
    channels->setRegex(0, ".*");

    EXPECT_FALSE(isIdentityOfSource(remove));
    EXPECT_EQ(ids({ "Color(1)" }), present(remove));
    expectRenderKeeps(source, remove, nameSet({ "A", "Z", "ZBack" }));
}

TEST_F(DeepChannelNodesTest, DeepToImageAfterRemovingDiffuseHasNoDiffuse)
{
    NodePtr source;
    NodePtr remove = createRemoveOnLayeredSource(&source);

    ASSERT_TRUE(bool(remove));
    KnobChannelSetPtr channels = channelsKnob(remove);
    ASSERT_TRUE(bool(channels));
    channels->setLayer(0, "diffuse", NULL);

    NodePtr toImage = createTrackedNode(PLUGINID_NATRON_DEEPTOIMAGE);
    ASSERT_TRUE(bool(toImage));
    connectNodes(remove, toImage, 0, true);

    const std::vector<std::string> layers = present(toImage);
    for (std::vector<std::string>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        EXPECT_NE(std::string("diffuse"), *it);
    }

    ImagePtr image;
    EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderImageFrame(toImage, &image));
    EXPECT_TRUE(image != NULL);
    EXPECT_FALSE(toImage->hasPersistentMessage());
}
