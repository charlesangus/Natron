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
#include <cstddef>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QThread>

#include "BaseTest.h"
#include "CacheMemoryPressureGuard.h"
#include "DeepRenderTestEffect.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/DeepImage.h"
#include "Engine/DeepImageCacheEntry.h"
#include "Engine/DeepImageKey.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageKey.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
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

// What flattening the source stub's samples must produce: they are point samples at strictly
// increasing depths, so the source declares them tidy and the flatten is a plain front-to-back
// composite over the stored order, with no splitting or merging in between.
std::vector<float>
expectedFlattenedPixel(int x,
                       int y)
{
    const std::vector<std::string> channelNames = deepRenderTestChannelNames();
    std::vector<float> out(channelNames.size(), 0.f);
    const U32 count = deepRenderTestSampleCount(x, y);
    float transmittance = 1.f;

    for (U32 s = 0; s < count; ++s) {
        for (std::size_t c = 0; c < channelNames.size(); ++c) {
            out[c] += transmittance * deepRenderTestChannel(x, y, (int)s, (int)c);
        }
        transmittance *= (1.f - deepRenderTestChannel(x, y, (int)s, 3));
    }

    return out;
}

void
expectFlattenedImageMatchesSource(const ImagePtr& image,
                                  const RectI& roi)
{
    const int numChannels = (int)deepRenderTestChannelNames().size();
    Image::ReadAccess access = image->getReadRights();

    for (int y = roi.y1; y < roi.y2; ++y) {
        for (int x = roi.x1; x < roi.x2; ++x) {
            const std::vector<float> expected = expectedFlattenedPixel(x, y);
            const float* actual = (const float*)access.pixelAt(x, y);
            ASSERT_TRUE(actual != NULL) << "at pixel (" << x << ", " << y << ")";
            for (int c = 0; c < numChannels; ++c) {
                ASSERT_FLOAT_EQ(expected[c], actual[c]) << "at pixel (" << x << ", " << y << ") channel " << c;
            }
        }
    }
}

DeepImageKey
deepEntryKey(const NodePtr& node,
             double time)
{
    return DeepImageKey(node.get(), node->getHashValue(), time, ViewIdx(0), RenderScale::identity);
}

// The bounds of every deep cache entry a node currently holds for that frame. Empty means the
// node has nothing cached, which is what a deliberate per-holder eviction must produce.
std::vector<RectI>
cachedDeepEntryBounds(const NodePtr& node,
                      double time)
{
    std::list<DeepImageCacheEntryPtr> found;
    std::vector<RectI> bounds;

    if (!appPTR->getDeepImage(deepEntryKey(node, time), &found)) {
        return bounds;
    }
    for (std::list<DeepImageCacheEntryPtr>::const_iterator it = found.begin(); it != found.end(); ++it) {
        if ((*it)->getDeepImage()) {
            bounds.push_back((*it)->getDeepImage()->getBounds());
        }
    }

    return bounds;
}

// Every pixel of roi holds exactly the samples the source formulas prescribe, scaled by the gain
// node. A total sample count would not catch an empty region, so this walks every pixel.
void
expectGainedSamplesOverWholeRegion(const DeepImagePtr& image,
                                   const RectI& roi)
{
    const std::vector<std::string> channelNames = deepRenderTestChannelNames();
    std::vector<const DeepChannelBuffer*> channels(channelNames.size());

    for (std::size_t c = 0; c < channelNames.size(); ++c) {
        channels[c] = image->getChannel(channelNames[c]);
        ASSERT_TRUE(channels[c] != NULL) << "missing channel " << channelNames[c];
    }

    for (int y = roi.y1; y < roi.y2; ++y) {
        for (int x = roi.x1; x < roi.x2; ++x) {
            std::size_t index;
            ASSERT_TRUE(deepRenderTestPixelIndex(*image, x, y, &index)) << "at pixel (" << x << ", " << y << ")";
            const U32 count = image->getSampleTable().getCount(index);
            const U64 offset = image->getSampleTable().getOffset(index);
            ASSERT_EQ(deepRenderTestSampleCount(x, y), count) << "at pixel (" << x << ", " << y << ")";
            for (U32 s = 0; s < count; ++s) {
                for (std::size_t c = 0; c < channels.size(); ++c) {
                    ASSERT_FLOAT_EQ(deepRenderTestChannel(x, y, (int)s, (int)c) * kDeepRenderTestGain, channels[c]->data()[offset + s])
                        << "at pixel (" << x << ", " << y << ") sample " << s << " channel " << channelNames[c];
                }
            }
        }
    }
}

} // namespace

class DeepRenderPipelineTest
    : public BaseTest {
protected:
    // Several tests in this fixture assert that a deep or flattened entry rendered earlier is
    // still servable from the app-wide cache; without this, that outcome depends on the host's
    // memory pressure rather than the pipeline under test. See CacheMemoryPressureGuard.h.
    DisableUnreachableRAMPurging _noPurging;

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

    // Same, but asking for the flattened Image the Viewer displays rather than the deep payload.
    EffectInstance::RenderRoIRetCode renderFlattenedFrame(const NodePtr& node,
                                                          double time,
                                                          const RectI& roi,
                                                          ImagePtr* outputImage,
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

        EffectInstance::RenderDeepRoIArgs args(time,
                                               RenderScale::identity,
                                               0 /*mipmapLevel*/,
                                               ViewIdx(0),
                                               false /*byPassCache*/,
                                               roi,
                                               RectD(),
                                               0 /*caller*/,
                                               time);

        return node->getEffectInstance()->renderDeepRoIFlattened(args, outputImage);
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

TEST_F(DeepRenderPipelineTest, BoundsGrowthRePullsItsInputOverTheGrownWindow)
{
    const RectI fullFrame(0, 0, kDeepRenderTestWidth, kDeepRenderTestHeight);
    const RectI leftHalf(0, 0, kDeepRenderTestWidth / 2, kDeepRenderTestHeight);
    const RectI rightHalf(kDeepRenderTestWidth / 2, 0, kDeepRenderTestWidth, kDeepRenderTestHeight);

    DeepImagePtr left;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(_gain, 1., leftHalf, &left));
    ASSERT_TRUE(left != NULL);
    ASSERT_TRUE(leftHalf == left->getBounds());
    ASSERT_EQ(1, deepRenderTestSourceRenderCount().load());
    ASSERT_EQ(1, deepRenderTestGainRenderCount().load());

    // Both links of the chain are cached at this point: the asymmetry below has to be a genuine
    // transition, not something that was already missing.
    const std::vector<RectI> sourceCachedBefore = cachedDeepEntryBounds(_source, 1.);
    ASSERT_EQ((std::size_t)1, sourceCachedBefore.size());
    ASSERT_TRUE(leftHalf == sourceCachedBefore[0]);
    ASSERT_EQ((std::size_t)1, cachedDeepEntryBounds(_gain, 1.).size());

    // Evict the upstream node's entry only. That is what forces the second render's grown window
    // to be satisfied by a fresh upstream render rather than by an entry that happens to be wide
    // enough already.
    appPTR->removeAllCacheEntriesForHolder(_source.get(), true);
    ASSERT_TRUE(cachedDeepEntryBounds(_source, 1.).empty());
    ASSERT_EQ((std::size_t)1, cachedDeepEntryBounds(_gain, 1.).size());

    DeepImagePtr right;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(_gain, 1., rightHalf, &right));
    ASSERT_TRUE(right != NULL);
    ASSERT_TRUE(fullFrame == right->getBounds());
    EXPECT_EQ(2, deepRenderTestSourceRenderCount().load());
    EXPECT_EQ(2, deepRenderTestGainRenderCount().load());

    // The upstream node was asked for the window the downstream node is rendering, not for the
    // narrower RoI the caller requested.
    const std::vector<RectI> sourceCachedAfter = cachedDeepEntryBounds(_source, 1.);
    ASSERT_EQ((std::size_t)1, sourceCachedAfter.size());
    EXPECT_TRUE(fullFrame == sourceCachedAfter[0]);

    expectGainedSamplesOverWholeRegion(right, fullFrame);
}

TEST_F(DeepRenderPipelineTest, BoundsGrowthRetractsTheSupersededDeepCacheEntry)
{
    const RectI fullFrame(0, 0, kDeepRenderTestWidth, kDeepRenderTestHeight);
    const RectI leftHalf(0, 0, kDeepRenderTestWidth / 2, kDeepRenderTestHeight);
    const RectI rightHalf(kDeepRenderTestWidth / 2, 0, kDeepRenderTestWidth, kDeepRenderTestHeight);

    DeepImagePtr left;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(_gain, 1., leftHalf, &left));
    ASSERT_TRUE(left != NULL);
    ASSERT_EQ((std::size_t)1, cachedDeepEntryBounds(_gain, 1.).size());

    DeepImagePtr right;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(_gain, 1., rightHalf, &right));
    ASSERT_TRUE(right != NULL);
    ASSERT_TRUE(fullFrame == right->getBounds());

    // The narrow entry growth superseded must not linger alongside the grown one: exactly one
    // entry should remain under this key, and it must be the grown entry.
    const std::vector<RectI> gainCached = cachedDeepEntryBounds(_gain, 1.);
    ASSERT_EQ((std::size_t)1, gainCached.size());
    EXPECT_TRUE(fullFrame == gainCached[0]);
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

TEST_F(DeepRenderPipelineTest, AbortDuringWideningRenderRetractsTheHalfBuiltWideEntry)
{
    const RectI fullFrame(0, 0, kDeepRenderTestWidth, kDeepRenderTestHeight);
    const RectI leftHalf(0, 0, kDeepRenderTestWidth / 2, kDeepRenderTestHeight);
    const RectI rightHalf(kDeepRenderTestWidth / 2, 0, kDeepRenderTestWidth, kDeepRenderTestHeight);

    DeepImagePtr left;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(_gain, 1., leftHalf, &left));
    ASSERT_TRUE(left != NULL);
    ASSERT_EQ(1, deepRenderTestSourceRenderCount().load());
    ASSERT_EQ(1, deepRenderTestGainRenderCount().load());

    const std::vector<RectI> sourceCachedBefore = cachedDeepEntryBounds(_source, 1.);
    ASSERT_EQ((std::size_t)1, sourceCachedBefore.size());
    ASSERT_TRUE(leftHalf == sourceCachedBefore[0]);

    // The right half is not covered, so the upstream render grows to the full frame and its
    // wide entry is created alongside the narrow one, both under the same key. Aborting from
    // inside the upstream render action leaves that wide entry empty.
    AbortableRenderInfoPtr abortInfo;
    deepRenderTestSourceHook() = [&abortInfo]() {
        if (abortInfo) {
            abortInfo->setAborted();
        }
    };

    DeepImagePtr aborted;
    EXPECT_EQ(EffectInstance::eRenderRoIRetCodeAborted, renderDeepFrame(_gain, 1., rightHalf, &aborted, false, &abortInfo));
    EXPECT_TRUE(aborted == NULL);
    EXPECT_EQ(2, deepRenderTestSourceRenderCount().load());
    EXPECT_EQ(1, deepRenderTestGainRenderCount().load());

    // It is the empty wide entry that has to go, not the narrow one it was meant to replace.
    const std::vector<RectI> sourceCachedAfterAbort = cachedDeepEntryBounds(_source, 1.);
    ASSERT_EQ((std::size_t)1, sourceCachedAfterAbort.size());
    EXPECT_TRUE(leftHalf == sourceCachedAfterAbort[0]);

    deepRenderTestSourceHook() = std::function<void()>();

    // Whatever the abort left behind, a request the wide bounds would have covered must be
    // rendered for real rather than served the entry the abort never filled.
    DeepImagePtr right;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(_gain, 1., rightHalf, &right));
    ASSERT_TRUE(right != NULL);
    ASSERT_TRUE(fullFrame == right->getBounds());
    EXPECT_EQ(3, deepRenderTestSourceRenderCount().load());
    EXPECT_EQ(2, deepRenderTestGainRenderCount().load());

    const std::vector<RectI> sourceCachedAfterRender = cachedDeepEntryBounds(_source, 1.);
    ASSERT_EQ((std::size_t)1, sourceCachedAfterRender.size());
    EXPECT_TRUE(fullFrame == sourceCachedAfterRender[0]);

    expectGainedSamplesOverWholeRegion(right, fullFrame);
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

class DeepReshapeTest
    : public DeepRenderPipelineTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        DeepRenderPipelineTest::SetUp();

        deepRenderTestReshapeConfig() = DeepRenderTestReshapeConfig();
        _reshape = createNode(QString::fromUtf8(kTestPluginIDDeepReshape));
        ASSERT_TRUE(_reshape != NULL);
        connectNodes(_source, _reshape, 0, true);
    }

    virtual void TearDown() OVERRIDE
    {
        if (_reshape) {
            _reshape->destroyNode(false, false);
            _reshape.reset();
        }
        deepRenderTestReshapeConfig() = DeepRenderTestReshapeConfig();
        DeepRenderPipelineTest::TearDown();
    }

    // The reshaped image holds exactly the source formulas over roi in the kept channels, and
    // the added channel is zero and as long as the sample table says.
    void expectReshapedSamples(const DeepImagePtr& image,
                               const RectI& roi)
    {
        EXPECT_FALSE(image->hasChannel("G"));
        const DeepChannelBuffer* added = image->getChannel("AOV");
        ASSERT_TRUE(added != NULL);
        ASSERT_EQ((std::size_t)image->getSampleTable().getTotalSampleCount(), added->size());
        for (std::size_t s = 0; s < added->size(); ++s) {
            ASSERT_EQ(0.f, added->data()[s]) << "sample " << s;
        }

        const int kept[] = { 0, 2, 3 };
        const char* const names[] = { "R", "B", "A" };
        for (int k = 0; k < 3; ++k) {
            const DeepChannelBuffer* channel = image->getChannel(names[k]);
            ASSERT_TRUE(channel != NULL) << "missing channel " << names[k];
            for (int y = roi.y1; y < roi.y2; ++y) {
                for (int x = roi.x1; x < roi.x2; ++x) {
                    std::size_t index;
                    ASSERT_TRUE(deepRenderTestPixelIndex(*image, x, y, &index));
                    const U32 count = image->getSampleTable().getCount(index);
                    const U64 offset = image->getSampleTable().getOffset(index);
                    ASSERT_EQ(deepRenderTestSampleCount(x, y), count) << "at pixel (" << x << ", " << y << ")";
                    for (U32 s = 0; s < count; ++s) {
                        ASSERT_FLOAT_EQ(deepRenderTestChannel(x, y, (int)s, kept[k]), channel->data()[offset + s])
                            << "at pixel (" << x << ", " << y << ") sample " << s << " channel " << names[k];
                    }
                }
            }
        }
        ASSERT_TRUE(image->hasChannel("Z"));
        ASSERT_TRUE(image->hasChannel("ZBack"));
    }

    NodePtr _reshape;
};

TEST_F(DeepReshapeTest, AliasingPathDropsAndZeroAddsWhileSharingTheRest)
{
    const RectI fullFrame(0, 0, kDeepRenderTestWidth, kDeepRenderTestHeight);

    deepRenderTestReshapeConfig().drop.push_back("G");
    deepRenderTestReshapeConfig().addZero.push_back("AOV");

    DeepImagePtr reshaped;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(_reshape, 1., fullFrame, &reshaped));
    ASSERT_TRUE(reshaped != NULL);
    ASSERT_EQ(1, deepRenderTestReshapeConfig().renderCount);
    ASSERT_TRUE(fullFrame == reshaped->getBounds());

    expectReshapedSamples(reshaped, fullFrame);

    EXPECT_TRUE(deepRenderTestReshapeConfig().sharedSampleTable);
    const std::vector<std::string>& shared = deepRenderTestReshapeConfig().sharedChannels;
    const char* const expectShared[] = { "R", "B", "A", "Z", "ZBack" };
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(std::find(shared.begin(), shared.end(), expectShared[i]) != shared.end()) << expectShared[i];
    }
    EXPECT_TRUE(std::find(shared.begin(), shared.end(), "AOV") == shared.end());
}

TEST_F(DeepReshapeTest, CopyPathOverAWiderCachedInputMatchesTheAliasingRender)
{
    const RectI fullFrame(0, 0, kDeepRenderTestWidth, kDeepRenderTestHeight);
    const RectI leftHalf(0, 0, kDeepRenderTestWidth / 2, kDeepRenderTestHeight);

    deepRenderTestReshapeConfig().drop.push_back("G");
    deepRenderTestReshapeConfig().addZero.push_back("AOV");

    DeepImagePtr wideSource;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(_source, 1., fullFrame, &wideSource));
    ASSERT_TRUE(wideSource != NULL);

    DeepImagePtr reshaped;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(_reshape, 1., leftHalf, &reshaped));
    ASSERT_TRUE(reshaped != NULL);
    ASSERT_EQ(1, deepRenderTestReshapeConfig().renderCount);
    ASSERT_TRUE(leftHalf == reshaped->getBounds());

    EXPECT_FALSE(deepRenderTestReshapeConfig().sharedSampleTable);
    expectReshapedSamples(reshaped, leftHalf);
}

TEST_F(DeepRenderPipelineTest, SecondFlattenOfTheSameFrameIsACacheHit)
{
    const RectI roi(0, 0, kDeepRenderTestWidth, kDeepRenderTestHeight);

    ImagePtr first;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderFlattenedFrame(_source, 1., roi, &first));
    ASSERT_TRUE(first != NULL);
    EXPECT_TRUE(roi == first->getBounds());
    EXPECT_EQ(1, deepRenderTestSourceRenderCount().load());
    expectFlattenedImageMatchesSource(first, roi);

    // A cache hit hands the entry back untouched; anything that re-ran the flatten would write
    // over this. Nothing else can tell the two apart -- the image object is the same either way,
    // because a second getImageOrCreate() under the same key finds the entry that already exists.
    const float sentinel = -1234.5f;
    {
        Image::WriteAccess access = first->getWriteRights();
        float* pixel = (float*)access.pixelAt(2, 3);
        ASSERT_TRUE(pixel != NULL);
        pixel[0] = sentinel;
    }

    ImagePtr second;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderFlattenedFrame(_source, 1., roi, &second));
    ASSERT_TRUE(second != NULL);
    EXPECT_EQ(first.get(), second.get());
    EXPECT_EQ(1, deepRenderTestSourceRenderCount().load());

    {
        Image::ReadAccess access = second->getReadRights();
        const float* pixel = (const float*)access.pixelAt(2, 3);
        ASSERT_TRUE(pixel != NULL);
        EXPECT_FLOAT_EQ(sentinel, pixel[0]);
    }

    // A different frame is a different cache identity, so that one does flatten.
    ImagePtr otherFrame;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderFlattenedFrame(_source, 2., roi, &otherFrame));
    ASSERT_TRUE(otherFrame != NULL);
    EXPECT_NE(first.get(), otherFrame.get());
    EXPECT_EQ(2, deepRenderTestSourceRenderCount().load());
}

TEST_F(DeepRenderPipelineTest, AbortDuringFlattenLeavesNothingCached)
{
    const RectI roi(0, 0, kDeepRenderTestWidth, kDeepRenderTestHeight);
    AbortableRenderInfoPtr abortInfo;

    // Abort from inside the deep render, i.e. after the flattened image's cache entry exists but
    // before anything has been composited into it.
    deepRenderTestSourceHook() = [&abortInfo]() {
        if (abortInfo) {
            abortInfo->setAborted();
        }
    };

    ImagePtr aborted;
    EXPECT_EQ(EffectInstance::eRenderRoIRetCodeAborted, renderFlattenedFrame(_source, 1., roi, &aborted, &abortInfo));
    EXPECT_TRUE(aborted == NULL);
    EXPECT_EQ(1, deepRenderTestSourceRenderCount().load());

    deepRenderTestSourceHook() = std::function<void()>();

    // The entry the aborted render created must not be servable: this has to flatten for real,
    // not hand back the empty image the first attempt left behind.
    ImagePtr complete;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderFlattenedFrame(_source, 1., roi, &complete));
    ASSERT_TRUE(complete != NULL);
    EXPECT_EQ(2, deepRenderTestSourceRenderCount().load());
    expectFlattenedImageMatchesSource(complete, roi);
}

TEST_F(DeepRenderPipelineTest, FlattenCacheInvalidatesWhenTheDeepNodesHashChanges)
{
    const RectI roi(0, 0, kDeepRenderTestWidth, kDeepRenderTestHeight);
    const U64 firstHash = _source->getHashValue();

    ImagePtr first;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderFlattenedFrame(_source, 1., roi, &first));
    ASSERT_TRUE(first != NULL);
    EXPECT_EQ(1, deepRenderTestSourceRenderCount().load());

    // The entry is keyed on the deep node's own hash. That is what makes the purge
    // Node::computeHashInternal() fires on any hash change land on exactly these entries, so it
    // is the property to assert, not just the re-render it produces below.
    EXPECT_EQ(firstHash, first->getKey().getTreeVersion());

    _source->incrementKnobsAge();
    const U64 secondHash = _source->getHashValue();
    ASSERT_NE(firstHash, secondHash);

    ImagePtr second;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderFlattenedFrame(_source, 1., roi, &second));
    ASSERT_TRUE(second != NULL);
    EXPECT_NE(first.get(), second.get());
    EXPECT_EQ(secondHash, second->getKey().getTreeVersion());
    EXPECT_EQ(2, deepRenderTestSourceRenderCount().load());
    expectFlattenedImageMatchesSource(second, roi);
}

namespace {

const int kLayeredFlattenSize = 4;
const int kLayeredFlattenSamples = 2;

float
layeredFlattenAlpha(int sample)
{
    return (sample == 0) ? 0.5f : 0.25f;
}

float
layeredFlattenValue(const std::string& channel,
                    int pixel,
                    int sample)
{
    if (channel == "A") {
        return layeredFlattenAlpha(sample);
    }
    const char* const names[] = { "R", "G", "B", "diffuse.R", "diffuse.G", "diffuse.B", "diffuse.X", "diffuse.Y", "diffuse.Z" };
    int c = 0;
    while ((c < 9) && (channel != names[c])) {
        ++c;
    }

    return 0.1f * (float)(c + 1) + 0.01f * (float)sample + 0.001f * (float)pixel;
}

// Two point samples per pixel at depths 1 and 2, so the flatten is sample 0 plus sample 1 seen
// through sample 0's alpha, with no tidying in between.
DeepImagePtr
makeLayeredFlattenImage(const std::vector<std::string>& channels)
{
    const RectI bounds(0, 0, kLayeredFlattenSize, kLayeredFlattenSize);
    DeepImagePtr image = std::make_shared<DeepImage>(bounds, RenderScale::identity, ViewIdx(0));

    SampleTable& table = image->getSampleTableForWriting();
    for (std::size_t p = 0; p < (std::size_t)(kLayeredFlattenSize * kLayeredFlattenSize); ++p) {
        table.setCount(p, (U32)kLayeredFlattenSamples);
    }
    table.recomputeOffsets();

    const std::size_t total = (std::size_t)table.getTotalSampleCount();
    float* z = image->getChannelForWriting("Z").dataForWriting();
    float* zBack = image->getChannelForWriting("ZBack").dataForWriting();
    for (std::size_t s = 0; s < total; ++s) {
        z[s] = 1.f + (float)(s % kLayeredFlattenSamples);
        zBack[s] = z[s];
    }
    for (std::size_t c = 0; c < channels.size(); ++c) {
        float* data = image->getChannelForWriting(channels[c]).dataForWriting();
        for (std::size_t s = 0; s < total; ++s) {
            data[s] = layeredFlattenValue(channels[c], (int)(s / kLayeredFlattenSamples), (int)(s % kLayeredFlattenSamples));
        }
    }
    image->setTidy(true);

    return image;
}

ImageLayerDesc
diffuseLayer()
{
    std::vector<std::string> channels;

    channels.push_back("R");
    channels.push_back("G");
    channels.push_back("B");

    return ImageLayerDesc("diffuse", "diffuse", "", channels);
}

// The same layer ID and width as diffuseLayer(), over other channels.
ImageLayerDesc
diffuseXyzLayer()
{
    std::vector<std::string> channels;

    channels.push_back("X");
    channels.push_back("Y");
    channels.push_back("Z");

    return ImageLayerDesc("diffuse", "diffuse", "", channels);
}

// Every pixel of image holds the front-to-back flatten of the named deep channels, in order.
void
expectLayeredFlatten(const ImagePtr& image,
                     const std::vector<std::string>& channels)
{
    ASSERT_TRUE(image != NULL);
    ASSERT_EQ(channels.size(), (std::size_t)image->getComponentsCount());
    Image::ReadAccess access = image->getReadRights();

    for (int y = 0; y < kLayeredFlattenSize; ++y) {
        for (int x = 0; x < kLayeredFlattenSize; ++x) {
            const int pixel = y * kLayeredFlattenSize + x;
            const float* actual = (const float*)access.pixelAt(x, y);
            ASSERT_TRUE(actual != NULL) << "at pixel (" << x << ", " << y << ")";
            for (std::size_t c = 0; c < channels.size(); ++c) {
                const float expected = layeredFlattenValue(channels[c], pixel, 0) + (1.f - layeredFlattenAlpha(0)) * layeredFlattenValue(channels[c], pixel, 1);
                EXPECT_FLOAT_EQ(expected, actual[c]) << "at pixel (" << x << ", " << y << ") channel " << channels[c];
            }
        }
    }
}

void
writeSentinel(const ImagePtr& image,
              float value)
{
    Image::WriteAccess access = image->getWriteRights();
    float* pixel = (float*)access.pixelAt(1, 2);

    ASSERT_TRUE(pixel != NULL);
    pixel[0] = value;
}

float
readSentinel(const ImagePtr& image)
{
    Image::ReadAccess access = image->getReadRights();
    const float* pixel = (const float*)access.pixelAt(1, 2);

    return pixel ? pixel[0] : 0.f;
}

// The hash purge runs on the cache's cleaner thread, so poll for it, bounded, so the test fails
// rather than hangs if it never happens.
bool
waitUntilFlattenedImagesAbsent(const ImageKey& key,
                               int timeoutMs = 5000)
{
    for (int waited = 0; waited <= timeoutMs; waited += 5) {
        std::list<ImagePtr> found;
        if (!appPTR->getImage(key, &found)) {
            return true;
        }
        QThread::msleep(5);
    }

    return false;
}

} // namespace

class DeepFlattenLayersTest
    : public DeepRenderPipelineTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        DeepRenderPipelineTest::SetUp();
        deepSyntheticSourceImages().clear();
        _nextSlot = 1;
    }

    virtual void TearDown() OVERRIDE
    {
        for (std::vector<NodePtr>::reverse_iterator it = _synthetic.rbegin(); it != _synthetic.rend(); ++it) {
            (*it)->destroyNode(false, false);
        }
        _synthetic.clear();
        deepSyntheticSourceImages().clear();
        DeepRenderPipelineTest::TearDown();
    }

    NodePtr createSyntheticSource(const std::vector<std::string>& channels)
    {
        const int slot = _nextSlot++;

        deepSyntheticSourceImages()[slot] = makeLayeredFlattenImage(channels);
        NodePtr node = createNode(QString::fromUtf8(kTestPluginIDDeepSyntheticSource));
        if (!node) {
            return node;
        }
        _synthetic.push_back(node);
        KnobInt* knob = dynamic_cast<KnobInt*>(node->getKnobByName("slot").get());
        if (!knob) {
            return NodePtr();
        }
        knob->setValue(slot);

        return node;
    }

    NodePtr createLayeredSource()
    {
        std::vector<std::string> channels;

        channels.push_back("R");
        channels.push_back("G");
        channels.push_back("B");
        channels.push_back("A");
        channels.push_back("diffuse.R");
        channels.push_back("diffuse.G");
        channels.push_back("diffuse.B");

        return createSyntheticSource(channels);
    }

    // The viewer's deep path: the selected layers flattened under the scheduler's frame args.
    EffectInstance::RenderRoIRetCode renderFlattenedLayers(const NodePtr& node,
                                                           const std::list<ImageLayerDesc>& layers,
                                                           std::list<ImagePtr>* outputImages)
    {
        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(true, 0);
        ParallelRenderArgsSetter frameRenderArgs(1.,
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
        EffectInstance::RenderDeepRoIArgs args(1.,
                                               RenderScale::identity,
                                               0 /*mipmapLevel*/,
                                               ViewIdx(0),
                                               false /*byPassCache*/,
                                               RectI(0, 0, kLayeredFlattenSize, kLayeredFlattenSize),
                                               RectD(),
                                               0 /*caller*/,
                                               1.);

        return node->getEffectInstance()->renderDeepRoIFlattened(args, layers, outputImages);
    }

    ImagePtr renderFlattenedLayer(const NodePtr& node,
                                  const ImageLayerDesc& layer)
    {
        std::list<ImagePtr> images;

        if (renderFlattenedLayers(node, std::list<ImageLayerDesc>(1, layer), &images) != EffectInstance::eRenderRoIRetCodeOk) {
            return ImagePtr();
        }

        return (images.size() == 1) ? images.front() : ImagePtr();
    }

    std::vector<NodePtr> _synthetic;
    int _nextSlot;
};

TEST_F(DeepFlattenLayersTest, TheSelectedLayerIsFlattenedWithTheDeepAlpha)
{
    NodePtr source = createLayeredSource();
    ASSERT_TRUE(source != NULL);

    std::vector<std::string> diffuseChannels;
    diffuseChannels.push_back("diffuse.R");
    diffuseChannels.push_back("diffuse.G");
    diffuseChannels.push_back("diffuse.B");

    const ImagePtr diffuse = renderFlattenedLayer(source, diffuseLayer());
    ASSERT_TRUE(diffuse != NULL);
    EXPECT_TRUE(diffuse->getComponents() == diffuseLayer());
    expectLayeredFlatten(diffuse, diffuseChannels);
}

TEST_F(DeepFlattenLayersTest, SeveralLayersInOneCallMatchTheirOwnFlattens)
{
    NodePtr source = createLayeredSource();
    ASSERT_TRUE(source != NULL);

    std::list<ImageLayerDesc> layers;
    layers.push_back(ImageLayerDesc::getRGBAComponents());
    layers.push_back(diffuseLayer());
    std::list<ImagePtr> images;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderFlattenedLayers(source, layers, &images));
    ASSERT_EQ((std::size_t)2, images.size());

    std::vector<std::string> rgba;
    rgba.push_back("R");
    rgba.push_back("G");
    rgba.push_back("B");
    rgba.push_back("A");
    std::vector<std::string> diffuseChannels;
    diffuseChannels.push_back("diffuse.R");
    diffuseChannels.push_back("diffuse.G");
    diffuseChannels.push_back("diffuse.B");
    expectLayeredFlatten(images.front(), rgba);
    expectLayeredFlatten(images.back(), diffuseChannels);
}

TEST_F(DeepFlattenLayersTest, ReRenderingALayerIsACacheHit)
{
    NodePtr source = createLayeredSource();
    ASSERT_TRUE(source != NULL);

    const ImagePtr first = renderFlattenedLayer(source, diffuseLayer());
    ASSERT_TRUE(first != NULL);
    const float sentinel = -1234.5f;
    writeSentinel(first, sentinel);

    const ImagePtr second = renderFlattenedLayer(source, diffuseLayer());
    ASSERT_TRUE(second != NULL);
    EXPECT_EQ(first.get(), second.get());
    EXPECT_FLOAT_EQ(sentinel, readSentinel(second));
}

TEST_F(DeepFlattenLayersTest, EachLayerKeepsItsOwnCacheEntry)
{
    NodePtr source = createLayeredSource();
    ASSERT_TRUE(source != NULL);

    const ImagePtr diffuse = renderFlattenedLayer(source, diffuseLayer());
    ASSERT_TRUE(diffuse != NULL);
    writeSentinel(diffuse, -1.f);

    const ImagePtr rgba = renderFlattenedLayer(source, ImageLayerDesc::getRGBAComponents());
    ASSERT_TRUE(rgba != NULL);
    EXPECT_NE(diffuse.get(), rgba.get());
    EXPECT_EQ(4, (int)rgba->getComponentsCount());
    writeSentinel(rgba, -2.f);

    // Both entries live under one key, the node's hash, which is what keeps the hash purge exact.
    EXPECT_TRUE(diffuse->getKey() == rgba->getKey());
    std::list<ImagePtr> cached;
    ASSERT_TRUE(appPTR->getImage(diffuse->getKey(), &cached));
    EXPECT_EQ((std::size_t)2, cached.size());

    const ImagePtr diffuseAgain = renderFlattenedLayer(source, diffuseLayer());
    const ImagePtr rgbaAgain = renderFlattenedLayer(source, ImageLayerDesc::getRGBAComponents());
    EXPECT_EQ(diffuse.get(), diffuseAgain.get());
    EXPECT_EQ(rgba.get(), rgbaAgain.get());
    EXPECT_FLOAT_EQ(-1.f, readSentinel(diffuseAgain));
    EXPECT_FLOAT_EQ(-2.f, readSentinel(rgbaAgain));
}

TEST_F(DeepFlattenLayersTest, LayersSharingAnIdAndWidthButNotChannelsAreFlattenedApart)
{
    std::vector<std::string> channels;
    channels.push_back("A");
    channels.push_back("diffuse.R");
    channels.push_back("diffuse.G");
    channels.push_back("diffuse.B");
    channels.push_back("diffuse.X");
    channels.push_back("diffuse.Y");
    channels.push_back("diffuse.Z");
    NodePtr source = createSyntheticSource(channels);
    ASSERT_TRUE(source != NULL);

    std::vector<std::string> rgbChannels;
    rgbChannels.push_back("diffuse.R");
    rgbChannels.push_back("diffuse.G");
    rgbChannels.push_back("diffuse.B");
    std::vector<std::string> xyzChannels;
    xyzChannels.push_back("diffuse.X");
    xyzChannels.push_back("diffuse.Y");
    xyzChannels.push_back("diffuse.Z");

    {
        std::list<ImageLayerDesc> layers;
        layers.push_back(diffuseLayer());
        layers.push_back(diffuseXyzLayer());
        std::list<ImagePtr> images;
        ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderFlattenedLayers(source, layers, &images));
        ASSERT_EQ((std::size_t)2, images.size());
        EXPECT_NE(images.front().get(), images.back().get());
        expectLayeredFlatten(images.front(), rgbChannels);
        expectLayeredFlatten(images.back(), xyzChannels);
    }

    const ImagePtr rgb = renderFlattenedLayer(source, diffuseLayer());
    const ImagePtr xyz = renderFlattenedLayer(source, diffuseXyzLayer());
    ASSERT_TRUE(rgb != NULL);
    ASSERT_TRUE(xyz != NULL);
    EXPECT_NE(rgb.get(), xyz.get());
    EXPECT_EQ(diffuseLayer().getChannels(), rgb->getComponents().getChannels());
    EXPECT_EQ(diffuseXyzLayer().getChannels(), xyz->getComponents().getChannels());
    expectLayeredFlatten(rgb, rgbChannels);
    expectLayeredFlatten(xyz, xyzChannels);

    // Both layouts keep their own cache entry, so each re-render is a hit on its own.
    writeSentinel(rgb, -1.f);
    writeSentinel(xyz, -2.f);
    const ImagePtr rgbAgain = renderFlattenedLayer(source, diffuseLayer());
    const ImagePtr xyzAgain = renderFlattenedLayer(source, diffuseXyzLayer());
    EXPECT_EQ(rgb.get(), rgbAgain.get());
    EXPECT_EQ(xyz.get(), xyzAgain.get());
    EXPECT_FLOAT_EQ(-1.f, readSentinel(rgbAgain));
    EXPECT_FLOAT_EQ(-2.f, readSentinel(xyzAgain));

    // The image cache's own match tells the two layouts apart, not just this lookup.
    ImagePtr fromCache;
    EXPECT_TRUE(appPTR->getImageOrCreate(rgb->getKey(), xyz->getParams(), &fromCache));
    EXPECT_EQ(xyz.get(), fromCache.get());
    EXPECT_TRUE(appPTR->getImageOrCreate(rgb->getKey(), rgb->getParams(), &fromCache));
    EXPECT_EQ(rgb.get(), fromCache.get());
}

TEST_F(DeepFlattenLayersTest, AHashChangePurgesEveryLayer)
{
    NodePtr source = createLayeredSource();
    ASSERT_TRUE(source != NULL);

    const ImagePtr diffuse = renderFlattenedLayer(source, diffuseLayer());
    const ImagePtr rgba = renderFlattenedLayer(source, ImageLayerDesc::getRGBAComponents());
    ASSERT_TRUE(diffuse != NULL);
    ASSERT_TRUE(rgba != NULL);
    const ImageKey oldKey = diffuse->getKey();
    EXPECT_EQ(source->getHashValue(), oldKey.getTreeVersion());

    source->incrementKnobsAge();
    ASSERT_NE(oldKey.getTreeVersion(), source->getHashValue());
    EXPECT_TRUE(waitUntilFlattenedImagesAbsent(oldKey));

    const ImagePtr diffuseAfter = renderFlattenedLayer(source, diffuseLayer());
    const ImagePtr rgbaAfter = renderFlattenedLayer(source, ImageLayerDesc::getRGBAComponents());
    ASSERT_TRUE(diffuseAfter != NULL);
    ASSERT_TRUE(rgbaAfter != NULL);
    EXPECT_NE(diffuse.get(), diffuseAfter.get());
    EXPECT_NE(rgba.get(), rgbaAfter.get());
    EXPECT_EQ(source->getHashValue(), diffuseAfter->getKey().getTreeVersion());
}

TEST_F(DeepFlattenLayersTest, AlphaDisplayOnAnAlphaOnlySourceShowsItsAlpha)
{
    NodePtr source = createSyntheticSource(std::vector<std::string>(1, "A"));
    ASSERT_TRUE(source != NULL);

    // The viewer's A display asks for the stream's colour storage, Alpha here, and reads "A".
    const ImagePtr alpha = renderFlattenedLayer(source, ImageLayerDesc::getAlphaComponents());
    ASSERT_TRUE(alpha != NULL);
    expectLayeredFlatten(alpha, std::vector<std::string>(1, "A"));

    // RGBA over Alpha storage is a different entry, with the colour it lacks read as zero.
    const ImagePtr rgba = renderFlattenedLayer(source, ImageLayerDesc::getRGBAComponents());
    ASSERT_TRUE(rgba != NULL);
    EXPECT_NE(alpha.get(), rgba.get());
    ASSERT_EQ(4, (int)rgba->getComponentsCount());
    Image::ReadAccess access = rgba->getReadRights();
    const float* pixel = (const float*)access.pixelAt(0, 0);
    ASSERT_TRUE(pixel != NULL);
    EXPECT_EQ(0.f, pixel[0]);
    EXPECT_EQ(0.f, pixel[1]);
    EXPECT_EQ(0.f, pixel[2]);
    EXPECT_FLOAT_EQ(layeredFlattenAlpha(0) + (1.f - layeredFlattenAlpha(0)) * layeredFlattenAlpha(1), pixel[3]);
}

TEST_F(DeepFlattenLayersTest, ASourceWithSamplesButNoAlphaFailsAndLeavesNoDeepCacheEntry)
{
    NodePtr source = createSyntheticSource(std::vector<std::string>({ "R", "G", "B" }));
    ASSERT_TRUE(source != NULL);

    const RectI roi(0, 0, kLayeredFlattenSize, kLayeredFlattenSize);
    DeepImagePtr out;
    EXPECT_EQ(EffectInstance::eRenderRoIRetCodeFailed, renderDeepFrame(source, 1., roi, &out));
    EXPECT_TRUE(source->hasPersistentMessage());
    EXPECT_TRUE(cachedDeepEntryBounds(source, 1.).empty());
}
