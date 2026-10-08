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
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

#include <QFuture>
#include <QString>
#include <QtConcurrent/QtConcurrentRun>

#include "BaseTest.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/FrameRenderContext.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/RenderStats.h"
#include "Engine/TLSHolder.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kConstantPluginID = "net.sf.openfx.ConstantPlugin";
const char* const kGradePluginID = "net.sf.openfx.GradePlugin";

const double kTime = 1.;
const int kSize = 64;

int
renderRoICalls(const RenderStatsPtr& stats,
               const NodePtr& node)
{
    const std::string name = node->getScriptName_mt_safe();
    const std::map<std::tuple<std::string, double, int>, int> calls = stats->getRenderRoICalls();
    int total = 0;

    for (std::map<std::tuple<std::string, double, int>, int>::const_iterator it = calls.begin(); it != calls.end(); ++it) {
        if (std::get<0>(it->first) == name) {
            total += it->second;
        }
    }

    return total;
}

std::vector<float>
readWindow(const ImagePtr& image,
           const RectI& window)
{
    const std::size_t rowFloats = static_cast<std::size_t>(window.width()) * 4;
    std::vector<float> pixels(rowFloats * window.height());
    Image::ReadAccess access = image->getReadRights();

    for (int y = window.y1; y < window.y2; ++y) {
        const float* row = reinterpret_cast<const float*>(access.pixelAt(window.x1, y));
        std::memcpy(&pixels[static_cast<std::size_t>(y - window.y1) * rowFloats], row, rowFloats * sizeof(float));
    }

    return pixels;
}

EffectInstance::RenderRoIArgs
makeRootArgs(const RectI& window)
{
    std::list<ImageLayerDesc> components;
    components.push_back(ImageLayerDesc::getRGBAComponents());

    return EffectInstance::RenderRoIArgs(kTime,
                                         RenderScale::identity,
                                         0 /*mipmapLevel*/,
                                         ViewIdx(0),
                                         true /*byPassCache*/,
                                         window,
                                         RectD(),
                                         components,
                                         eImageBitDepthFloat,
                                         false /*calledFromGetImage*/,
                                         0 /*caller*/,
                                         eStorageModeRAM,
                                         kTime);
}

bool
isUsableResult(const std::map<ImageLayerDesc, ImagePtr>& layers,
               const RectI& window)
{
    if (layers.empty()) {
        return false;
    }
    const ImagePtr& image = layers.begin()->second;

    return image && (image->getComponentsCount() == 4) && (image->getBitDepth() == eImageBitDepthFloat) && image->getBounds().contains(window);
}

} // namespace

class FrameStoreHandoff
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    enum ConsumerKindEnum {
        eConsumerKindGrading,
        eConsumerKindIdentity,
        eConsumerKindDisabled
    };

    // Constant -> Grade (input) -> Grade (consumer). A consumer with a zero mix is an identity on its input, while a
    // disabled one passes every layer of its input through without reaching the identity code.
    void buildChain(ConsumerKindEnum consumerKind,
                    NodePtr* constant,
                    NodePtr* input,
                    NodePtr* consumer)
    {
        Format format(0, 0, kSize, kSize, "frameStoreHandoffFormat", 1.);
        getApp()->getProject()->setOrAddProjectFormat(format);

        *constant = createNode(QString::fromUtf8(kConstantPluginID));
        ASSERT_TRUE(bool(*constant));
        KnobColor* color = dynamic_cast<KnobColor*>((*constant)->getKnobByName("color").get());
        ASSERT_TRUE(color != NULL);
        color->setValues(0.25, 0.5, 0.75, 1., ViewSpec::all(), eValueChangedReasonNatronInternalEdited);

        *input = createNode(QString::fromUtf8(kGradePluginID));
        ASSERT_TRUE(bool(*input));
        KnobColor* inputMultiply = dynamic_cast<KnobColor*>((*input)->getKnobByName("multiply").get());
        ASSERT_TRUE(inputMultiply != NULL);
        inputMultiply->setValues(0.5, 1.25, 0.9, 1., ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
        connectNodes(*constant, *input, 0, true);

        *consumer = createNode(QString::fromUtf8(kGradePluginID));
        ASSERT_TRUE(bool(*consumer));
        KnobColor* consumerMultiply = dynamic_cast<KnobColor*>((*consumer)->getKnobByName("multiply").get());
        ASSERT_TRUE(consumerMultiply != NULL);
        consumerMultiply->setValues(2., 2., 2., 1., ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
        if (consumerKind == eConsumerKindIdentity) {
            KnobDouble* mix = dynamic_cast<KnobDouble*>((*consumer)->getKnobByName("mix").get());
            ASSERT_TRUE(mix != NULL);
            mix->setValue(0.);
        }
        connectNodes(*input, *consumer, 0, true);
        if (consumerKind == eConsumerKindDisabled) {
            (*consumer)->setNodeDisabled(true);
        }
    }

    bool renderLegacy(const NodePtr& node,
                      const RenderStatsPtr& stats,
                      std::map<ImageLayerDesc, ImagePtr>* layers,
                      std::string* error)
    {
        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
        ParallelRenderArgsSetter frameRenderArgs(kTime,
                                                 ViewIdx(0),
                                                 false /*isRenderUserInteraction*/,
                                                 false /*isSequential*/,
                                                 abortInfo,
                                                 node,
                                                 0 /*textureIndex*/,
                                                 getApp()->getTimeLine().get(),
                                                 NodePtr(),
                                                 false /*isAnalysis*/,
                                                 false /*draftMode*/,
                                                 stats);
        const RectI window(0, 0, kSize, kSize);
        const RectD canonicalWindow(0., 0., kSize, kSize);

        FrameRequestMap request;
        if (EffectInstance::computeRequestPass(kTime, ViewIdx(0), 0 /*mipmapLevel*/, canonicalWindow, node, request) == eStatusFailed) {
            *error = "legacy request pass failed";

            return false;
        }
        frameRenderArgs.updateNodesRequest(request);

        if ((node->getEffectInstance()->renderRoI(makeRootArgs(window), layers) != EffectInstance::eRenderRoIRetCodeOk) || !isUsableResult(*layers, window)) {
            *error = "legacy render did not produce an RGBA float image covering the window";

            return false;
        }

        return true;
    }

    FrameRenderContextPtr makeContext(const NodePtr& treeRoot,
                                      const RenderStatsPtr& stats)
    {
        return FrameRenderContext::create(kTime,
                                          ViewIdx(0),
                                          false /*isRenderUserInteraction*/,
                                          false /*isSequential*/,
                                          AbortableRenderInfo::create(false, 0),
                                          treeRoot,
                                          0 /*textureIndex*/,
                                          getApp()->getTimeLine().get(),
                                          NodePtr(),
                                          false /*isAnalysis*/,
                                          false /*draftMode*/,
                                          stats);
    }

    // Renders node the way a scheduled task does: on a pool thread, with only the frame context installed.
    bool renderInContext(const FrameRenderContextPtr& context,
                         const NodePtr& node,
                         std::map<ImageLayerDesc, ImagePtr>* layers,
                         std::string* error)
    {
        const RectI window(0, 0, kSize, kSize);
        const RectD canonicalWindow(0., 0., kSize, kSize);
        bool ok = false;

        QFuture<void> future = QtConcurrent::run([&]() {
            AppTLS::FrameContextScope scope(context.get());

            std::shared_ptr<FrameRequestMap> request = std::make_shared<FrameRequestMap>();
            if (EffectInstance::computeRequestPass(kTime, ViewIdx(0), 0 /*mipmapLevel*/, canonicalWindow, node, *request) == eStatusFailed) {
                *error = "request pass failed";

                return;
            }
            context->setRequest(request);

            if ((node->getEffectInstance()->renderRoI(makeRootArgs(window), layers) != EffectInstance::eRenderRoIRetCodeOk) || !isUsableResult(*layers, window)) {
                *error = "render did not produce an RGBA float image covering the window";

                return;
            }
            ok = true;
        });
        future.waitForFinished();

        return ok;
    }

    void putInStore(const FrameRenderContextPtr& context,
                    const NodePtr& node,
                    const std::map<ImageLayerDesc, ImagePtr>& layers)
    {
        FrameStore::TaskKey key;
        key.node = node;
        key.time = kTime;
        key.view = ViewIdx(0);
        key.mipmapLevel = 0;
        context->getStore().put(key, layers, RectI(0, 0, kSize, kSize), 1);
    }

    // Stores the input's legacy render, renders the consumer in a frame context and checks that the input was taken
    // from the store rather than rendered. A consumer passing its input through returns the stored image itself.
    void expectConsumerUsesStoredInput(const NodePtr& constant,
                                       const NodePtr& input,
                                       const NodePtr& consumer,
                                       bool returnsStoredImage)
    {
        const RectI window(0, 0, kSize, kSize);
        std::string error;

        appPTR->clearAllCaches();
        std::map<ImageLayerDesc, ImagePtr> legacyConsumer;
        ASSERT_TRUE(renderLegacy(consumer, RenderStatsPtr(), &legacyConsumer, &error)) << error;
        const std::vector<float> expected = readWindow(legacyConsumer.begin()->second, window);

        appPTR->clearAllCaches();
        std::map<ImageLayerDesc, ImagePtr> inputLayers;
        ASSERT_TRUE(renderLegacy(input, RenderStatsPtr(), &inputLayers, &error)) << error;

        appPTR->clearAllCaches();
        RenderStatsPtr stats = std::make_shared<RenderStats>(false);
        stats->setTrackRenderRoICalls(true);
        FrameRenderContextPtr context = makeContext(consumer, stats);
        ASSERT_TRUE(bool(context));
        putInStore(context, input, inputLayers);

        std::map<ImageLayerDesc, ImagePtr> layers;
        ASSERT_TRUE(renderInContext(context, consumer, &layers, &error)) << error;

        EXPECT_EQ(0, renderRoICalls(stats, input)) << "the stored input must not be rendered again";
        EXPECT_EQ(0, renderRoICalls(stats, constant));
        EXPECT_GE(renderRoICalls(stats, consumer), 1);
        EXPECT_EQ(1, stats->getFrameStoreHits());
        EXPECT_EQ(0, stats->getUnplannedPulls());

        if (returnsStoredImage) {
            EXPECT_EQ(inputLayers.begin()->second.get(), layers.begin()->second.get());
        }

        const std::vector<float> actual = readWindow(layers.begin()->second, window);
        ASSERT_EQ(expected.size(), actual.size());
        EXPECT_EQ(0, std::memcmp(expected.data(), actual.data(), expected.size() * sizeof(float)));
    }
};

TEST_F(FrameStoreHandoff, StoredInputIsNotRenderedAgain)
{
    NodePtr constant, input, consumer;
    buildChain(eConsumerKindGrading, &constant, &input, &consumer);
    if (HasFatalFailure()) {
        return;
    }

    expectConsumerUsesStoredInput(constant, input, consumer, false);
}

TEST_F(FrameStoreHandoff, EmptyStorePullsInputLikeLegacy)
{
    NodePtr constant, input, consumer;
    buildChain(eConsumerKindGrading, &constant, &input, &consumer);
    if (HasFatalFailure()) {
        return;
    }
    const RectI window(0, 0, kSize, kSize);
    std::string error;

    appPTR->clearAllCaches();
    RenderStatsPtr legacyStats = std::make_shared<RenderStats>(false);
    legacyStats->setTrackRenderRoICalls(true);
    std::map<ImageLayerDesc, ImagePtr> legacyConsumer;
    ASSERT_TRUE(renderLegacy(consumer, legacyStats, &legacyConsumer, &error)) << error;
    const std::vector<float> expected = readWindow(legacyConsumer.begin()->second, window);

    appPTR->clearAllCaches();
    RenderStatsPtr stats = std::make_shared<RenderStats>(false);
    stats->setTrackRenderRoICalls(true);
    FrameRenderContextPtr context = makeContext(consumer, stats);
    ASSERT_TRUE(bool(context));

    std::map<ImageLayerDesc, ImagePtr> layers;
    ASSERT_TRUE(renderInContext(context, consumer, &layers, &error)) << error;

    EXPECT_GE(renderRoICalls(stats, input), 1);
    EXPECT_EQ(renderRoICalls(legacyStats, input), renderRoICalls(stats, input)) << "a pull must call renderRoI on the input as legacy does";
    EXPECT_EQ(0, stats->getFrameStoreHits());
    // The consumer pulls the input Grade and the input Grade pulls the Constant; getImage() re-fetching an input the
    // pre-render already pulled is not counted again.
    EXPECT_EQ(2, stats->getUnplannedPulls());
    EXPECT_EQ(0, legacyStats->getFrameStoreHits());
    EXPECT_EQ(0, legacyStats->getUnplannedPulls());

    const std::vector<float> actual = readWindow(layers.begin()->second, window);
    ASSERT_EQ(expected.size(), actual.size());
    EXPECT_EQ(0, std::memcmp(expected.data(), actual.data(), expected.size() * sizeof(float)));
}

TEST_F(FrameStoreHandoff, IdentityConsumerTakesStoredTarget)
{
    NodePtr constant, input, consumer;
    buildChain(eConsumerKindIdentity, &constant, &input, &consumer);
    if (HasFatalFailure()) {
        return;
    }

    expectConsumerUsesStoredInput(constant, input, consumer, true);
}

TEST_F(FrameStoreHandoff, DisabledConsumerTakesStoredPassThroughInput)
{
    NodePtr constant, input, consumer;
    buildChain(eConsumerKindDisabled, &constant, &input, &consumer);
    if (HasFatalFailure()) {
        return;
    }

    expectConsumerUsesStoredInput(constant, input, consumer, true);
}
