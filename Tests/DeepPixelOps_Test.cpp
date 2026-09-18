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

#include <cmath>
#include <cstddef>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/DeepPixelOps.h"

NATRON_NAMESPACE_USING

namespace {

const int kAlpha = 0;
const int kColor = 1;
const int kNumChannels = 2;

// Builds one pixel's samples into a DeepPixelScratch: channels[kAlpha] = alpha,
// channels[kColor] = alpha * colorValue (premultiplied), matching the DeepPixelView convention.
// Any view handed out before a further addSample() call is invalidated by it.
class TestPixel {
public:
    TestPixel() = default;

    void addSample(float z,
                   float zback,
                   float alpha,
                   float colorValue)
    {
        _z.push_back(z);
        _zback.push_back(zback);
        _alpha.push_back(alpha);
        _color.push_back(alpha * colorValue);

        const int numSamples = (int)_z.size();
        _scratch.reset(kNumChannels, kAlpha);
        _scratch.setSampleCount(numSamples);
        for (int s = 0; s < numSamples; ++s) {
            _scratch.z()[s] = _z[s];
            _scratch.zback()[s] = _zback[s];
            _scratch.channel(kAlpha)[s] = _alpha[s];
            _scratch.channel(kColor)[s] = _color[s];
        }
    }

    void addPointSample(float z,
                        float alpha,
                        float colorValue)
    {
        addSample(z, z, alpha, colorValue);
    }

    DeepPixelView view() const
    {
        return _scratch.view();
    }

private:
    DeepPixelScratch _scratch;
    std::vector<float> _z;
    std::vector<float> _zback;
    std::vector<float> _alpha;
    std::vector<float> _color;
};

void
sizeScratch(DeepPixelScratch* scratch,
            int numSamples)
{
    scratch->reset(kNumChannels, kAlpha);
    scratch->setSampleCount(numSamples);
}

} // namespace

TEST(DeepPixelOpsTest, IsPointSample)
{
    TestPixel pixel;
    pixel.addPointSample(1.f, 0.5f, 0.2f);
    pixel.addSample(1.f, 2.f, 0.5f, 0.2f);

    EXPECT_TRUE(pixel.view().isPointSample(0));
    EXPECT_FALSE(pixel.view().isPointSample(1));
}

// Worked example: splitting a volumetric sample of alpha 0.5 exactly halfway through its depth
// extent gives alphaFront == alphaBack == 1 - sqrt(0.5) (OpenEXR "Interpreting Deep Pixels",
// splitting-a-volume-sample section: 1 - (1 - 0.5)^0.5).
TEST(DeepPixelOpsTest, SplitHalfwayMatchesWorkedExample)
{
    TestPixel pixel;
    pixel.addSample(0.f, 10.f, 0.5f, 1.f);
    DeepPixelScratch out;
    sizeScratch(&out, 2);

    DeepPixelOps::splitVolumeSample(pixel.view(), 0, 5.f, out.view(), 0, 1);

    const float expected = 1.f - std::sqrt(0.5f);
    EXPECT_NEAR(expected, out.channel(kAlpha)[0], 1e-6f);
    EXPECT_NEAR(expected, out.channel(kAlpha)[1], 1e-6f);
    EXPECT_FLOAT_EQ(0.f, out.z()[0]);
    EXPECT_FLOAT_EQ(5.f, out.zback()[0]);
    EXPECT_FLOAT_EQ(5.f, out.z()[1]);
    EXPECT_FLOAT_EQ(10.f, out.zback()[1]);
}

// Splitting then recompositing (Porter-Duff "over") the two halves must reproduce the original
// sample's alpha and premultiplied color exactly, for every alpha and split fraction: this is the
// split identity the OpenEXR document derives the split formula from.
TEST(DeepPixelOpsTest, SplitThenOverCompositeReproducesOriginal)
{
    const float alphas[] = { 0.01f, 0.1f, 0.3f, 0.5f, 0.7f, 0.9f, 0.99f };
    const float fractions[] = { 0.05f, 0.25f, 0.5f, 0.75f, 0.95f };
    DeepPixelScratch out;
    sizeScratch(&out, 2);

    for (size_t ai = 0; ai < sizeof(alphas) / sizeof(alphas[0]); ++ai) {
        for (size_t fi = 0; fi < sizeof(fractions) / sizeof(fractions[0]); ++fi) {
            const float alpha = alphas[ai];
            const float colorValue = 0.6f;
            TestPixel pixel;
            pixel.addSample(0.f, 10.f, alpha, colorValue);
            const float depth = 10.f * fractions[fi];

            DeepPixelOps::splitVolumeSample(pixel.view(), 0, depth, out.view(), 0, 1);

            const float frontAlpha = out.channel(kAlpha)[0];
            const float recombinedAlpha = frontAlpha + (1.f - frontAlpha) * out.channel(kAlpha)[1];
            const float recombinedColor = out.channel(kColor)[0] + (1.f - frontAlpha) * out.channel(kColor)[1];

            EXPECT_NEAR(alpha, recombinedAlpha, 1e-5f);
            EXPECT_NEAR(pixel.view().channels[kColor][0], recombinedColor, 1e-5f);
        }
    }
}

TEST(DeepPixelOpsTest, SplitDegenerateAlphaZero)
{
    TestPixel pixel;
    pixel.addSample(0.f, 10.f, 0.f, 0.f);
    DeepPixelScratch out;
    sizeScratch(&out, 2);

    DeepPixelOps::splitVolumeSample(pixel.view(), 0, 3.f, out.view(), 0, 1);

    EXPECT_FLOAT_EQ(0.f, out.channel(kAlpha)[0]);
    EXPECT_FLOAT_EQ(0.f, out.channel(kAlpha)[1]);
}

// alpha == 1 is the degenerate "infinitely dense" case: at any interior split point both halves
// are already fully opaque in the limit, but splitting exactly at either endpoint (fraction 0 or
// 1) must still give one empty (alpha 0) side rather than computing 0 * -inf.
TEST(DeepPixelOpsTest, SplitDegenerateAlphaOne)
{
    TestPixel pixel;
    pixel.addSample(0.f, 10.f, 1.f, 1.f);
    DeepPixelScratch out;
    sizeScratch(&out, 2);

    DeepPixelOps::splitVolumeSample(pixel.view(), 0, 5.f, out.view(), 0, 1);
    EXPECT_FLOAT_EQ(1.f, out.channel(kAlpha)[0]);
    EXPECT_FLOAT_EQ(1.f, out.channel(kAlpha)[1]);

    DeepPixelOps::splitVolumeSample(pixel.view(), 0, 0.f, out.view(), 0, 1);
    EXPECT_FLOAT_EQ(0.f, out.channel(kAlpha)[0]);
    EXPECT_FLOAT_EQ(1.f, out.channel(kAlpha)[1]);

    DeepPixelOps::splitVolumeSample(pixel.view(), 0, 10.f, out.view(), 0, 1);
    EXPECT_FLOAT_EQ(1.f, out.channel(kAlpha)[0]);
    EXPECT_FLOAT_EQ(0.f, out.channel(kAlpha)[1]);
}

// Worked example: combining two coincident samples of alpha 0.5 each gives combined alpha
// 0.5 + 0.5 - 0.5*0.5 == 0.75.
TEST(DeepPixelOpsTest, MergeWorkedExample)
{
    TestPixel pixel;
    pixel.addSample(2.f, 4.f, 0.5f, 1.f);
    pixel.addSample(2.f, 4.f, 0.5f, 1.f);
    DeepPixelScratch out;
    sizeScratch(&out, 1);

    DeepPixelOps::mergeCoincidentSamples(pixel.view(), 0, 1, out.view(), 0);

    EXPECT_NEAR(0.75f, out.channel(kAlpha)[0], 1e-6f);
    EXPECT_FLOAT_EQ(2.f, out.z()[0]);
    EXPECT_FLOAT_EQ(4.f, out.zback()[0]);
}

// The document guarantees this merge is commutative for a pair of coincident samples (it has no
// such guarantee for chains of three or more merged pairwise in different orders, which this test
// does not exercise).
TEST(DeepPixelOpsTest, MergeIsCommutative)
{
    TestPixel pixel;
    pixel.addSample(0.f, 1.f, 0.37f, 0.8f);
    pixel.addSample(0.f, 1.f, 0.61f, 0.2f);
    DeepPixelScratch out;
    sizeScratch(&out, 2);

    DeepPixelOps::mergeCoincidentSamples(pixel.view(), 0, 1, out.view(), 0);
    DeepPixelOps::mergeCoincidentSamples(pixel.view(), 1, 0, out.view(), 1);

    EXPECT_EQ(out.channel(kAlpha)[0], out.channel(kAlpha)[1]);
    EXPECT_EQ(out.channel(kColor)[0], out.channel(kColor)[1]);
}

TEST(DeepPixelOpsTest, MergeWithZeroSampleIsIdentity)
{
    TestPixel pixel;
    pixel.addSample(0.f, 1.f, 0.42f, 0.3f);
    pixel.addSample(0.f, 1.f, 0.f, 0.f);
    DeepPixelScratch out;
    sizeScratch(&out, 1);

    DeepPixelOps::mergeCoincidentSamples(pixel.view(), 0, 1, out.view(), 0);

    EXPECT_FLOAT_EQ(pixel.view().channels[kAlpha][0], out.channel(kAlpha)[0]);
    EXPECT_FLOAT_EQ(pixel.view().channels[kColor][0], out.channel(kColor)[0]);
}

TEST(DeepPixelOpsTest, SortPutsPointSamplesBeforeVolumeAtSameDepthAndOrdersByDepth)
{
    TestPixel pixel;
    pixel.addSample(5.f, 8.f, 0.3f, 0.5f);
    pixel.addPointSample(0.f, 1.f, 0.1f);
    pixel.addSample(0.f, 2.f, 0.4f, 0.6f);
    std::vector<int> order;

    DeepPixelOps::sortSamplesByDepth(pixel.view(), order);

    const DeepPixelView view = pixel.view();
    ASSERT_EQ((size_t)3, order.size());
    EXPECT_TRUE(view.isPointSample(order[0]));
    EXPECT_FLOAT_EQ(0.f, view.z[order[0]]);
    EXPECT_FALSE(view.isPointSample(order[1]));
    EXPECT_FLOAT_EQ(0.f, view.z[order[1]]);
    EXPECT_FLOAT_EQ(5.f, view.z[order[2]]);
}

// Two samples that fully overlap the same depth range must tidy down to exactly the single
// sample mergeCoincidentSamples() would produce directly.
TEST(DeepPixelOpsTest, TidyFullyOverlappingSamplesReducesToSingleMerge)
{
    TestPixel pixel;
    pixel.addSample(0.f, 10.f, 0.5f, 0.9f);
    pixel.addSample(0.f, 10.f, 0.4f, 0.2f);
    DeepPixelScratch tidy;
    DeepTidyWorkspace work;
    DeepPixelScratch expected;
    sizeScratch(&expected, 1);

    DeepPixelOps::tidySamples(pixel.view(), &tidy, &work);
    DeepPixelOps::mergeCoincidentSamples(pixel.view(), 0, 1, expected.view(), 0);

    ASSERT_EQ(1, tidy.getSampleCount());
    EXPECT_FLOAT_EQ(expected.z()[0], tidy.z()[0]);
    EXPECT_FLOAT_EQ(expected.zback()[0], tidy.zback()[0]);
    EXPECT_FLOAT_EQ(expected.channel(kAlpha)[0], tidy.channel(kAlpha)[0]);
    EXPECT_FLOAT_EQ(expected.channel(kColor)[0], tidy.channel(kColor)[0]);
}

// Partial overlap ([0,10) and [5,15)) must tidy into three boundary-aligned, non-overlapping
// segments: [0,5) from A alone, [5,10) the merge of A's and B's overlapping halves, [10,15) from
// B alone. Cross-checked against independently calling splitVolumeSample()/mergeCoincidentSamples
// by hand rather than against hardcoded numbers, so this exercises tidySamples()'s own boundary
// detection rather than just re-deriving the same arithmetic twice.
TEST(DeepPixelOpsTest, TidyPartialOverlapProducesThreeAlignedSegments)
{
    TestPixel pixel;
    pixel.addSample(0.f, 10.f, 0.5f, 0.8f);
    pixel.addSample(5.f, 15.f, 0.4f, 0.3f);
    DeepPixelScratch tidy;
    DeepTidyWorkspace work;

    DeepPixelOps::tidySamples(pixel.view(), &tidy, &work);

    ASSERT_EQ(3, tidy.getSampleCount());
    EXPECT_FLOAT_EQ(0.f, tidy.z()[0]);
    EXPECT_FLOAT_EQ(5.f, tidy.zback()[0]);
    EXPECT_FLOAT_EQ(5.f, tidy.z()[1]);
    EXPECT_FLOAT_EQ(10.f, tidy.zback()[1]);
    EXPECT_FLOAT_EQ(10.f, tidy.z()[2]);
    EXPECT_FLOAT_EQ(15.f, tidy.zback()[2]);

    // Slots 0/1 are A split at 10's start, slots 2/3 are B split at A's end.
    DeepPixelScratch pieces;
    sizeScratch(&pieces, 4);
    DeepPixelOps::splitVolumeSample(pixel.view(), 0, 5.f, pieces.view(), 0, 1);
    DeepPixelOps::splitVolumeSample(pixel.view(), 1, 10.f, pieces.view(), 2, 3);
    DeepPixelScratch expectedMiddle;
    sizeScratch(&expectedMiddle, 1);
    DeepPixelOps::mergeCoincidentSamples(pieces.view(), 1, 2, expectedMiddle.view(), 0);

    EXPECT_NEAR(pieces.channel(kAlpha)[0], tidy.channel(kAlpha)[0], 1e-6f);
    EXPECT_NEAR(expectedMiddle.channel(kAlpha)[0], tidy.channel(kAlpha)[1], 1e-6f);
    EXPECT_NEAR(expectedMiddle.channel(kColor)[0], tidy.channel(kColor)[1], 1e-6f);
    EXPECT_NEAR(pieces.channel(kAlpha)[3], tidy.channel(kAlpha)[2], 1e-6f);
}

// A point sample strictly inside a volumetric sample's range must split the volume sample so the
// point lands at the correct interleaved position, rather than being merged with either fragment
// (their depth ranges never coincide with a zero-length point sample's).
TEST(DeepPixelOpsTest, TidySplitsVolumeSampleAroundInteriorPointSample)
{
    TestPixel pixel;
    pixel.addSample(0.f, 10.f, 0.5f, 1.f);
    pixel.addPointSample(4.f, 0.9f, 0.1f);
    DeepPixelScratch tidy;
    DeepTidyWorkspace work;

    DeepPixelOps::tidySamples(pixel.view(), &tidy, &work);

    ASSERT_EQ(3, tidy.getSampleCount());
    EXPECT_FLOAT_EQ(0.f, tidy.z()[0]);
    EXPECT_FLOAT_EQ(4.f, tidy.zback()[0]);
    EXPECT_TRUE(DeepPixelOps::getSample(tidy.view(), 1).isPointSample());
    EXPECT_FLOAT_EQ(4.f, tidy.z()[1]);
    EXPECT_FLOAT_EQ(4.f, tidy.z()[2]);
    EXPECT_FLOAT_EQ(10.f, tidy.zback()[2]);
}

TEST(DeepPixelOpsTest, FlattenSequentialCompositeMatchesHandComputation)
{
    TestPixel tidy;
    tidy.addSample(0.f, 1.f, 0.5f, 1.f); // color premult = 0.5
    tidy.addSample(1.f, 2.f, 0.25f, 0.4f); // color premult = 0.1
    tidy.addPointSample(2.f, 1.f, 0.9f); // opaque, color premult = 0.9
    float result[kNumChannels];

    DeepPixelOps::flattenFrontToBack(tidy.view(), result);

    // transmittance starts at 1.
    // sample 0: color += 1 * 0.5 = 0.5, alpha += 1*0.5 = 0.5, transmittance *= 0.5 -> 0.5
    // sample 1: color += 0.5 * 0.1 = 0.05 -> 0.55, alpha += 0.5*0.25=0.125 -> 0.625, transmittance *= 0.75 -> 0.375
    // sample 2: color += 0.375 * 0.9 = 0.3375 -> 0.8875, alpha += 0.375*1 = 0.375 -> 1.0, transmittance *= 0 -> 0
    EXPECT_NEAR(1.0f, result[kAlpha], 1e-6f);
    EXPECT_NEAR(0.8875f, result[kColor], 1e-6f);
}

// Flattening a single sample's own split halves must reproduce flattening the unsplit sample: the
// split identity restated through flattenFrontToBack() rather than by hand-checking the over
// formula (as SplitThenOverCompositeReproducesOriginal does).
TEST(DeepPixelOpsTest, FlattenOfSplitSampleMatchesFlattenOfOriginal)
{
    TestPixel pixel;
    pixel.addSample(0.f, 10.f, 0.6f, 0.7f);
    DeepPixelScratch split;
    sizeScratch(&split, 2);
    DeepPixelOps::splitVolumeSample(pixel.view(), 0, 3.f, split.view(), 0, 1);

    float originalResult[kNumChannels];
    float splitResult[kNumChannels];
    DeepPixelOps::flattenFrontToBack(pixel.view(), originalResult);
    DeepPixelOps::flattenFrontToBack(split.view(), splitResult);

    EXPECT_NEAR(originalResult[kAlpha], splitResult[kAlpha], 1e-5f);
    EXPECT_NEAR(originalResult[kColor], splitResult[kColor], 1e-5f);
}

// flattenFrontToBack() carries transmittance across kBlockSize == 64 sample boundaries; with 200
// samples this exercises that carry twice. Alphas are kept small (0.01) so transmittance decays
// slowly and stays well away from 0 through all 200 samples: with a larger alpha it would
// underflow toward 0 long before sample 200, and every later sample -- including everything past
// the block boundary the carry is meant to preserve -- would contribute nothing regardless of
// whether the carry is correct. The reference is an independent, unblocked front-to-back loop
// written here rather than a second call to flattenFrontToBack() or a hardcoded constant.
TEST(DeepPixelOpsTest, FlattenAcrossMultipleBlocksMatchesUnblockedReference)
{
    const int numSamples = 200;
    const float alpha = 0.01f;
    TestPixel pixel;
    std::vector<float> colors(numSamples);
    for (int i = 0; i < numSamples; ++i) {
        colors[i] = 0.2f + 0.6f * ((float)i / (float)(numSamples - 1));
        pixel.addSample((float)i, (float)(i + 1), alpha, colors[i]);
    }

    float expected[kNumChannels] = { 0.f, 0.f };
    float transmittance = 1.f;
    for (int i = 0; i < numSamples; ++i) {
        expected[kAlpha] += transmittance * alpha;
        expected[kColor] += transmittance * (alpha * colors[i]);
        transmittance *= (1.f - alpha);
    }

    float result[kNumChannels];
    DeepPixelOps::flattenFrontToBack(pixel.view(), result);

    EXPECT_NEAR(expected[kAlpha], result[kAlpha], 1e-4f);
    EXPECT_NEAR(expected[kColor], result[kColor], 1e-4f);
}

// A pixel that is already sorted and non-overlapping must survive tidySamples() byte for byte:
// no boundary falls strictly inside any of its samples, so nothing is split or merged.
TEST(DeepPixelOpsTest, TidyOfAlreadyTidyPixelIsIdentity)
{
    TestPixel pixel;
    pixel.addSample(0.f, 5.f, 0.5f, 0.8f);
    pixel.addSample(5.f, 10.f, 0.3f, 0.4f);
    pixel.addPointSample(12.f, 0.9f, 0.1f);
    DeepPixelScratch tidy;
    DeepTidyWorkspace work;

    DeepPixelOps::tidySamples(pixel.view(), &tidy, &work);

    const DeepPixelView view = pixel.view();
    ASSERT_EQ(3, tidy.getSampleCount());
    for (int s = 0; s < 3; ++s) {
        EXPECT_FLOAT_EQ(view.z[s], tidy.z()[s]);
        EXPECT_FLOAT_EQ(view.zbackAt(s), tidy.zback()[s]);
        EXPECT_FLOAT_EQ(view.channels[kAlpha][s], tidy.channel(kAlpha)[s]);
        EXPECT_FLOAT_EQ(view.channels[kColor][s], tidy.channel(kColor)[s]);
    }
}

// A view aliasing buffers the caller already owns -- the zero-copy case DeepToImage and the
// viewer adapter take, including the no-ZBack-channel form where every sample is a point sample
// -- must flatten to exactly what the equivalent scratch-built pixel flattens to.
TEST(DeepPixelOpsTest, FlattenOverAliasedBufferMatchesScratchBuiltPixel)
{
    const std::vector<float> z = { 0.f, 1.f, 2.f };
    const std::vector<float> alpha = { 0.5f, 0.25f, 1.f };
    const std::vector<float> color = { 0.5f, 0.1f, 0.9f };
    const float* channels[kNumChannels] = { alpha.data(), color.data() };

    DeepPixelView aliased;
    aliased.z = z.data();
    aliased.zback = nullptr;
    aliased.channels = channels;
    aliased.numChannels = kNumChannels;
    aliased.alphaChannelIndex = kAlpha;
    aliased.numSamples = (int)z.size();

    EXPECT_TRUE(aliased.isPointSample(1));

    TestPixel pixel;
    pixel.addPointSample(0.f, 0.5f, 1.f);
    pixel.addPointSample(1.f, 0.25f, 0.4f);
    pixel.addPointSample(2.f, 1.f, 0.9f);

    float aliasedResult[kNumChannels];
    float scratchResult[kNumChannels];
    DeepPixelOps::flattenFrontToBack(aliased, aliasedResult);
    DeepPixelOps::flattenFrontToBack(pixel.view(), scratchResult);

    EXPECT_FLOAT_EQ(scratchResult[kAlpha], aliasedResult[kAlpha]);
    EXPECT_FLOAT_EQ(scratchResult[kColor], aliasedResult[kColor]);
    EXPECT_NEAR(0.8875f, aliasedResult[kColor], 1e-6f);
}

// The per-thread reuse contract: once a scratch and a workspace have served a pixel, a smaller
// pixel must reuse the same blocks rather than allocating again.
TEST(DeepPixelOpsTest, TidyReusesScratchAndWorkspaceWithoutReallocating)
{
    TestPixel wide;
    wide.addSample(0.f, 10.f, 0.5f, 0.8f);
    wide.addSample(5.f, 15.f, 0.4f, 0.3f);
    wide.addSample(2.f, 12.f, 0.3f, 0.2f);
    wide.addSample(7.f, 20.f, 0.2f, 0.7f);
    DeepPixelScratch out;
    DeepTidyWorkspace work;

    DeepPixelOps::tidySamples(wide.view(), &out, &work);

    ASSERT_EQ(7, out.getSampleCount());
    const int outCapacity = out.getCapacity();
    const float* outStorage = out.z();
    const int fragmentCapacity = work.fragments.getCapacity();
    const float* fragmentStorage = work.fragments.z();
    const std::size_t orderCapacity = work.order.capacity();
    const std::size_t boundaryCapacity = work.boundaries.capacity();

    TestPixel narrow;
    narrow.addSample(0.f, 1.f, 0.5f, 0.8f);
    narrow.addSample(1.f, 2.f, 0.4f, 0.3f);

    DeepPixelOps::tidySamples(narrow.view(), &out, &work);

    EXPECT_EQ(2, out.getSampleCount());
    EXPECT_EQ(outCapacity, out.getCapacity());
    EXPECT_EQ(outStorage, out.z());
    EXPECT_EQ(fragmentCapacity, work.fragments.getCapacity());
    EXPECT_EQ(fragmentStorage, work.fragments.z());
    EXPECT_EQ(orderCapacity, work.order.capacity());
    EXPECT_EQ(boundaryCapacity, work.boundaries.capacity());
}
