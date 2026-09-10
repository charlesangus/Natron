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
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "DeepRenderTestEffect.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/DeepImage.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

// The naive equivalent of NativeEffectBase::renderDeepTwoPass(): the same two passes over the
// same per-pixel formulas, but one thread and one scanline at a time. What the parallel,
// chunked helper produces must be indistinguishable from this.
void
renderDeepSerialReference(DeepImage* image)
{
    const RectI& bounds = image->getBounds();
    const std::vector<std::string> channelNames = deepRenderTestChannelNames();

    SampleTable& table = image->getSampleTableForWriting();
    for (int y = bounds.y1; y < bounds.y2; ++y) {
        for (int x = bounds.x1; x < bounds.x2; ++x) {
            std::size_t index;
            ASSERT_TRUE(deepRenderTestPixelIndex(*image, x, y, &index));
            table.setCount(index, deepRenderTestSampleCount(x, y));
        }
    }
    table.recomputeOffsets();

    const std::size_t totalSampleCount = (std::size_t)table.getTotalSampleCount();
    float* z = image->getChannelForWriting("Z").dataForWriting();
    float* zBack = image->getChannelForWriting("ZBack").dataForWriting();
    std::vector<float*> channels(channelNames.size());
    for (std::size_t c = 0; c < channelNames.size(); ++c) {
        channels[c] = image->getChannelForWriting(channelNames[c]).dataForWriting();
    }
    ASSERT_EQ(totalSampleCount, image->getChannelForWriting("Z").size());

    for (int y = bounds.y1; y < bounds.y2; ++y) {
        for (int x = bounds.x1; x < bounds.x2; ++x) {
            std::size_t index;
            ASSERT_TRUE(deepRenderTestPixelIndex(*image, x, y, &index));
            const U32 count = table.getCount(index);
            const U64 offset = table.getOffset(index);
            for (U32 s = 0; s < count; ++s) {
                z[offset + s] = deepRenderTestZ(x, y, (int)s);
                zBack[offset + s] = z[offset + s];
                for (std::size_t c = 0; c < channels.size(); ++c) {
                    channels[c][offset + s] = deepRenderTestChannel(x, y, (int)s, (int)c);
                }
            }
        }
    }
    image->setTidy(true);
}

void
expectDeepImagesIdentical(const DeepImage& actual,
                          const DeepImage& expected)
{
    ASSERT_TRUE(actual.getBounds() == expected.getBounds());
    ASSERT_EQ(expected.getSampleTable().getTotalSampleCount(), actual.getSampleTable().getTotalSampleCount());
    ASSERT_EQ(expected.getPixelCount(), actual.getPixelCount());
    EXPECT_EQ(expected.isTidy(), actual.isTidy());

    for (std::size_t i = 0; i < expected.getPixelCount(); ++i) {
        ASSERT_EQ(expected.getSampleTable().getCount(i), actual.getSampleTable().getCount(i)) << "at pixel " << i;
        ASSERT_EQ(expected.getSampleTable().getOffset(i), actual.getSampleTable().getOffset(i)) << "at pixel " << i;
    }

    std::vector<std::string> channelNames = deepRenderTestChannelNames();
    channelNames.push_back("Z");
    channelNames.push_back("ZBack");

    for (std::size_t c = 0; c < channelNames.size(); ++c) {
        const DeepChannelBuffer* actualChannel = actual.getChannel(channelNames[c]);
        const DeepChannelBuffer* expectedChannel = expected.getChannel(channelNames[c]);
        ASSERT_TRUE(actualChannel != NULL) << "missing channel " << channelNames[c];
        ASSERT_TRUE(expectedChannel != NULL) << "missing channel " << channelNames[c];
        ASSERT_EQ(expectedChannel->size(), actualChannel->size()) << "channel " << channelNames[c];
        for (std::size_t s = 0; s < expectedChannel->size(); ++s) {
            ASSERT_EQ(expectedChannel->data()[s], actualChannel->data()[s]) << "channel " << channelNames[c] << " sample " << s;
        }
    }
}

} // namespace

class DeepRenderPipelineTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();

        deepRenderTestSourceRenderCount() = 0;
        deepRenderTestGainRenderCount() = 0;
        deepRenderTestSourceHook() = std::function<void()>();

        _source = createNode(QString::fromUtf8(kTestPluginIDDeepRenderSource));
        _gain = createNode(QString::fromUtf8(kTestPluginIDDeepRenderGain));
        ASSERT_TRUE(_source != NULL);
        ASSERT_TRUE(_gain != NULL);
        connectNodes(_source, _gain, 0, true);
    }

    virtual void TearDown() OVERRIDE
    {
        deepRenderTestSourceHook() = std::function<void()>();
        if (_gain) {
            _gain->destroyNode(false, false);
            _gain.reset();
        }
        if (_source) {
            _source->destroyNode(false, false);
            _source.reset();
        }
        BaseTest::TearDown();
    }

    // Renders node's deep data the way the scheduler does: under a ParallelRenderArgsSetter that
    // carries the abort info for this render, so that EffectInstance::aborted() has something to
    // read. abortInfo is handed back so a test can abort the render from inside it.
    EffectInstance::RenderRoIRetCode renderDeepFrame(const NodePtr& node,
                                                     double time,
                                                     const RectI& roi,
                                                     DeepImagePtr* outputDeepImage,
                                                     bool abortBeforeRendering = false,
                                                     AbortableRenderInfoPtr* abortInfoOut = 0)
    {
        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(true, 0);

        if (abortInfoOut) {
            *abortInfoOut = abortInfo;
        }

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

        if (abortBeforeRendering) {
            abortInfo->setAborted();
        }

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

    NodePtr _source;
    NodePtr _gain;
};

TEST_F(DeepRenderPipelineTest, SecondRenderOfTheSameFrameIsACacheHit)
{
    const RectI roi(0, 0, kDeepRenderTestWidth, kDeepRenderTestHeight);

    DeepImagePtr first;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(_gain, 1., roi, &first));
    ASSERT_TRUE(first != NULL);
    EXPECT_TRUE(roi == first->getBounds());
    EXPECT_EQ(1, deepRenderTestSourceRenderCount().load());
    EXPECT_EQ(1, deepRenderTestGainRenderCount().load());

    // The chain really ran: the gain node's output is the source's samples scaled.
    const DeepChannelBuffer* red = first->getChannel("R");
    ASSERT_TRUE(red != NULL);
    ASSERT_GT(first->getSampleTable().getTotalSampleCount(), (U64)0);
    for (int y = roi.y1; y < roi.y2; ++y) {
        for (int x = roi.x1; x < roi.x2; ++x) {
            std::size_t index;
            ASSERT_TRUE(deepRenderTestPixelIndex(*first, x, y, &index));
            const U32 count = first->getSampleTable().getCount(index);
            const U64 offset = first->getSampleTable().getOffset(index);
            ASSERT_EQ(deepRenderTestSampleCount(x, y), count);
            for (U32 s = 0; s < count; ++s) {
                ASSERT_FLOAT_EQ(deepRenderTestChannel(x, y, (int)s, 0) * kDeepRenderTestGain, red->data()[offset + s]);
            }
        }
    }

    DeepImagePtr second;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(_gain, 1., roi, &second));
    ASSERT_TRUE(second != NULL);

    // The very same payload came back out of the deep cache, and neither node's render action ran
    // a second time.
    EXPECT_EQ(first.get(), second.get());
    EXPECT_EQ(1, deepRenderTestSourceRenderCount().load());
    EXPECT_EQ(1, deepRenderTestGainRenderCount().load());

    // A different frame is a different cache identity, so it does render again.
    DeepImagePtr otherFrame;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(_gain, 2., roi, &otherFrame));
    ASSERT_TRUE(otherFrame != NULL);
    EXPECT_NE(first.get(), otherFrame.get());
    EXPECT_EQ(2, deepRenderTestSourceRenderCount().load());
    EXPECT_EQ(2, deepRenderTestGainRenderCount().load());
}

TEST_F(DeepRenderPipelineTest, BoundsGrowthReRendersOverTheUnionOfRequestedRoIs)
{
    const RectI fullFrame(0, 0, kDeepRenderTestWidth, kDeepRenderTestHeight);
    const RectI leftHalf(0, 0, kDeepRenderTestWidth / 2, kDeepRenderTestHeight);
    const RectI rightHalf(kDeepRenderTestWidth / 2, 0, kDeepRenderTestWidth, kDeepRenderTestHeight);

    DeepImagePtr left;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(_gain, 1., leftHalf, &left));
    ASSERT_TRUE(left != NULL);
    EXPECT_TRUE(leftHalf == left->getBounds());
    EXPECT_EQ(1, deepRenderTestSourceRenderCount().load());
    EXPECT_EQ(1, deepRenderTestGainRenderCount().load());

    DeepImagePtr right;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(_gain, 1., rightHalf, &right));
    ASSERT_TRUE(right != NULL);
    // Nothing cached covers the right half, so bounds growth re-renders over the union of every
    // RoI asked for so far, not just the right half.
    EXPECT_TRUE(fullFrame == right->getBounds());
    EXPECT_EQ(2, deepRenderTestSourceRenderCount().load());
    EXPECT_EQ(2, deepRenderTestGainRenderCount().load());

    DeepImagePtr leftAgain;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(_gain, 1., leftHalf, &leftAgain));
    ASSERT_TRUE(leftAgain != NULL);
    // Contained by an already-cached entry, so this is served from the cache: neither node's
    // render action ran a third time.
    EXPECT_EQ(2, deepRenderTestSourceRenderCount().load());
    EXPECT_EQ(2, deepRenderTestGainRenderCount().load());

    // The grown entry holds real data over the whole frame, not just wider bounds.
    const DeepChannelBuffer* red = right->getChannel("R");
    ASSERT_TRUE(red != NULL);
    ASSERT_GT(right->getSampleTable().getTotalSampleCount(), (U64)0);
    for (int y = fullFrame.y1; y < fullFrame.y2; ++y) {
        for (int x = fullFrame.x1; x < fullFrame.x2; ++x) {
            std::size_t index;
            ASSERT_TRUE(deepRenderTestPixelIndex(*right, x, y, &index));
            const U32 count = right->getSampleTable().getCount(index);
            const U64 offset = right->getSampleTable().getOffset(index);
            ASSERT_EQ(deepRenderTestSampleCount(x, y), count);
            for (U32 s = 0; s < count; ++s) {
                ASSERT_FLOAT_EQ(deepRenderTestChannel(x, y, (int)s, 0) * kDeepRenderTestGain, red->data()[offset + s]);
            }
        }
    }
}

TEST_F(DeepRenderPipelineTest, AbortBeforeRenderingIsHonoured)
{
    const RectI roi(0, 0, kDeepRenderTestWidth, kDeepRenderTestHeight);

    DeepImagePtr aborted;
    EXPECT_EQ(EffectInstance::eRenderRoIRetCodeAborted, renderDeepFrame(_gain, 1., roi, &aborted, true /*abortBeforeRendering*/));
    EXPECT_TRUE(aborted == NULL);
    EXPECT_EQ(0, deepRenderTestSourceRenderCount().load());
    EXPECT_EQ(0, deepRenderTestGainRenderCount().load());
}

TEST_F(DeepRenderPipelineTest, AbortDuringUpstreamRenderLeavesNothingCached)
{
    const RectI roi(0, 0, kDeepRenderTestWidth, kDeepRenderTestHeight);
    AbortableRenderInfoPtr abortInfo;

    // Abort from inside the upstream node's render action, i.e. after its cache entry has been
    // created but before it holds anything.
    deepRenderTestSourceHook() = [&abortInfo]() {
        if (abortInfo) {
            abortInfo->setAborted();
        }
    };

    DeepImagePtr aborted;
    EXPECT_EQ(EffectInstance::eRenderRoIRetCodeAborted, renderDeepFrame(_gain, 1., roi, &aborted, false, &abortInfo));
    EXPECT_TRUE(aborted == NULL);
    EXPECT_EQ(1, deepRenderTestSourceRenderCount().load());
    // The downstream node never got as far as its own render action.
    EXPECT_EQ(0, deepRenderTestGainRenderCount().load());

    // The half-built entry the aborted render created must not be servable: rendering the same
    // frame again has to run the source's render action a second time and produce real samples.
    deepRenderTestSourceHook() = std::function<void()>();

    DeepImagePtr complete;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(_gain, 1., roi, &complete));
    ASSERT_TRUE(complete != NULL);
    EXPECT_EQ(2, deepRenderTestSourceRenderCount().load());
    EXPECT_EQ(1, deepRenderTestGainRenderCount().load());

    const DeepChannelBuffer* red = complete->getChannel("R");
    ASSERT_TRUE(red != NULL);
    ASSERT_GT(complete->getSampleTable().getTotalSampleCount(), (U64)0);
    for (int y = roi.y1; y < roi.y2; ++y) {
        for (int x = roi.x1; x < roi.x2; ++x) {
            std::size_t index;
            ASSERT_TRUE(deepRenderTestPixelIndex(*complete, x, y, &index));
            const U32 count = complete->getSampleTable().getCount(index);
            const U64 offset = complete->getSampleTable().getOffset(index);
            for (U32 s = 0; s < count; ++s) {
                ASSERT_FLOAT_EQ(deepRenderTestChannel(x, y, (int)s, 0) * kDeepRenderTestGain, red->data()[offset + s]);
            }
        }
    }
}

TEST_F(DeepRenderPipelineTest, TwoPassHelperMatchesSerialReference)
{
    const RectI roi(0, 0, kDeepRenderTestWidth, kDeepRenderTestHeight);

    DeepImagePtr rendered;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(_source, 1., roi, &rendered));
    ASSERT_TRUE(rendered != NULL);
    EXPECT_TRUE(roi == rendered->getBounds());

    DeepImage reference(roi, RenderScale::identity, ViewIdx(0));
    renderDeepSerialReference(&reference);
    ASSERT_GT(reference.getSampleTable().getTotalSampleCount(), (U64)0);

    expectDeepImagesIdentical(*rendered, reference);
}
