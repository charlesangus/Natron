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
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/DeepFlatten.h"
#include "Engine/DeepImage.h"
#include "Engine/DeepPixelOps.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const int kFlattenTestWidth = 4;
const int kFlattenTestHeight = 2;
const int kFlattenTestNumChannels = 4;
const int kFlattenTestAlphaIndex = 3;

std::vector<std::string>
flattenTestChannelNames()
{
    std::vector<std::string> names;

    names.push_back("R");
    names.push_back("G");
    names.push_back("B");
    names.push_back("A");

    return names;
}

// One sample of the fixture, and the unit the serial reference below works in.
struct TestSample {
    float z;
    float zback;
    float channels[kFlattenTestNumChannels];

    TestSample(float z_,
               float zback_,
               float r,
               float g,
               float b,
               float a)
        : z(z_)
        , zback(zback_)
    {
        channels[0] = r;
        channels[1] = g;
        channels[2] = b;
        channels[3] = a;
    }

    bool isPointSample() const
    {
        return zback <= z;
    }
};

typedef std::vector<std::vector<TestSample>> TestPixels;

// The payload, one entry per pixel of a 4x2 rectangle in row-major order. Pixels 2 and 3 are the
// reason this fixture exists: without a pixel whose samples overlap, flattening tidy and
// flattening untidied give the same answer and a test comparing them proves nothing.
TestPixels
makeTestPixels()
{
    TestPixels pixels((std::size_t)(kFlattenTestWidth * kFlattenTestHeight));

    // Two point samples given back to front, so flattening has to sort them.
    pixels[1].push_back(TestSample(5.f, 5.f, 0.40f, 0.30f, 0.20f, 0.50f));
    pixels[1].push_back(TestSample(2.f, 2.f, 0.10f, 0.20f, 0.30f, 0.25f));

    // Two samples spanning the very same volume: neither is in front, so tidying merges them.
    pixels[2].push_back(TestSample(1.f, 3.f, 0.60f, 0.10f, 0.05f, 0.40f));
    pixels[2].push_back(TestSample(1.f, 3.f, 0.20f, 0.50f, 0.15f, 0.30f));

    // Partially overlapping volumes: tidying cuts both at the other's boundary and merges the
    // two pieces that end up aligned, turning two samples into three.
    pixels[3].push_back(TestSample(1.f, 3.f, 0.50f, 0.40f, 0.30f, 0.60f));
    pixels[3].push_back(TestSample(2.f, 4.f, 0.25f, 0.35f, 0.45f, 0.20f));

    pixels[4].push_back(TestSample(7.f, 7.f, 0.90f, 0.80f, 0.70f, 0.75f));

    pixels[5].push_back(TestSample(1.f, 1.f, 0.11f, 0.22f, 0.33f, 0.10f));
    pixels[5].push_back(TestSample(2.f, 2.f, 0.44f, 0.55f, 0.66f, 0.20f));
    pixels[5].push_back(TestSample(3.f, 3.f, 0.77f, 0.88f, 0.99f, 0.30f));

    // A volumetric sample nothing overlaps: tidying must leave it exactly as it is.
    pixels[7].push_back(TestSample(2.f, 6.f, 0.15f, 0.25f, 0.35f, 0.45f));

    return pixels;
}

DeepImagePtr
makeTestDeepImage(const TestPixels& pixels,
                  const RectI& bounds)
{
    DeepImagePtr image = std::make_shared<DeepImage>(bounds, RenderScale::identity, ViewIdx(0));
    const std::vector<std::string> channelNames = flattenTestChannelNames();

    SampleTable& table = image->getSampleTableForWriting();
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        table.setCount(i, (U32)pixels[i].size());
    }
    table.recomputeOffsets();

    float* z = image->getChannelForWriting("Z").dataForWriting();
    float* zback = image->getChannelForWriting("ZBack").dataForWriting();
    std::vector<float*> channels(channelNames.size());
    for (std::size_t c = 0; c < channelNames.size(); ++c) {
        channels[c] = image->getChannelForWriting(channelNames[c]).dataForWriting();
    }

    for (std::size_t i = 0; i < pixels.size(); ++i) {
        const U64 offset = table.getOffset(i);
        for (std::size_t s = 0; s < pixels[i].size(); ++s) {
            z[offset + s] = pixels[i][s].z;
            zback[offset + s] = pixels[i][s].zback;
            for (std::size_t c = 0; c < channels.size(); ++c) {
                channels[c][offset + s] = pixels[i][s].channels[c];
            }
        }
    }

    // Deliberately not tidy: pixels 2 and 3 are not, and the flatten is what has to notice.
    image->setTidy(false);

    return image;
}

ImagePtr
makeFlattenDestination(const RectI& bounds)
{
    const RectD rod(bounds.x1, bounds.y1, bounds.x2, bounds.y2);

    return std::make_shared<Image>(ImageLayerDesc::getRGBAComponents(), rod, bounds, 0 /*mipmapLevel*/, 1. /*par*/,
                                   eImageBitDepthFloat, eImagePremultiplicationPremultiplied,
                                   eImageFieldingOrderNone, false /*useBitmap*/);
}

// The three formulas "Interpreting Deep Pixels" gives, written out here independently of
// DeepPixelOps so that the reference below is a second opinion rather than a rerun.

float
referenceVolumeAlpha(float alpha,
                     float fraction)
{
    if (fraction <= 0.f) {
        return 0.f;
    }
    if (fraction >= 1.f) {
        return alpha;
    }
    if (alpha <= 0.f) {
        return 0.f;
    }
    if (alpha >= 1.f) {
        return 1.f;
    }

    return -std::expm1(fraction * std::log1p(-(double)alpha));
}

void
referenceSplit(const TestSample& sample,
               float depth,
               TestSample* front,
               TestSample* back)
{
    const float extent = sample.zback - sample.z;
    const float fraction = (extent > 0.f) ? ((depth - sample.z) / extent) : 0.f;
    const float alpha = sample.channels[kFlattenTestAlphaIndex];
    const float alphaFront = referenceVolumeAlpha(alpha, fraction);
    const float alphaBack = referenceVolumeAlpha(alpha, 1.f - fraction);
    const float scaleFront = (alpha > 0.f) ? (alphaFront / alpha) : 0.f;
    const float scaleBack = (alpha > 0.f) ? (alphaBack / alpha) : 0.f;

    front->z = sample.z;
    front->zback = depth;
    back->z = depth;
    back->zback = sample.zback;
    for (int c = 0; c < kFlattenTestNumChannels; ++c) {
        front->channels[c] = sample.channels[c] * scaleFront;
        back->channels[c] = sample.channels[c] * scaleBack;
    }
}

TestSample
referenceMerge(const TestSample& a,
               const TestSample& b)
{
    TestSample out(a);
    const float alphaA = a.channels[kFlattenTestAlphaIndex];
    const float alphaB = b.channels[kFlattenTestAlphaIndex];

    for (int c = 0; c < kFlattenTestNumChannels; ++c) {
        out.channels[c] = a.channels[c] * (1.f - 0.5f * alphaB) + b.channels[c] * (1.f - 0.5f * alphaA);
    }

    return out;
}

void
referenceSortByDepth(std::vector<TestSample>* samples)
{
    std::stable_sort(samples->begin(), samples->end(), [](const TestSample& a, const TestSample& b) {
        if (a.z != b.z) {
            return a.z < b.z;
        }

        return a.isPointSample() && !b.isPointSample();
    });
}

// Sorts, splits every volumetric sample at every depth boundary any other sample introduces
// inside it, merges each resulting group of identically bounded samples, and composites the
// result front to back. One pixel at a time, one loop at a time, no views and no scratch.
std::vector<float>
referenceFlatten(const std::vector<TestSample>& raw)
{
    std::vector<float> out((std::size_t)kFlattenTestNumChannels, 0.f);

    if (raw.empty()) {
        return out;
    }

    std::vector<float> boundaries;
    for (std::size_t s = 0; s < raw.size(); ++s) {
        boundaries.push_back(raw[s].z);
        boundaries.push_back(raw[s].zback);
    }
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());

    std::vector<TestSample> sorted(raw);
    referenceSortByDepth(&sorted);

    std::vector<TestSample> fragments;
    for (std::size_t s = 0; s < sorted.size(); ++s) {
        TestSample remainder = sorted[s];
        if (!sorted[s].isPointSample()) {
            for (std::size_t b = 0; b < boundaries.size(); ++b) {
                if ((boundaries[b] <= sorted[s].z) || (boundaries[b] >= sorted[s].zback)) {
                    continue;
                }
                TestSample front(remainder);
                TestSample back(remainder);
                referenceSplit(remainder, boundaries[b], &front, &back);
                fragments.push_back(front);
                remainder = back;
            }
        }
        fragments.push_back(remainder);
    }

    referenceSortByDepth(&fragments);

    std::vector<TestSample> tidy;
    for (std::size_t i = 0; i < fragments.size();) {
        TestSample accumulated = fragments[i];
        std::size_t j = i + 1;
        while ((j < fragments.size()) && (fragments[j].z == accumulated.z) && (fragments[j].zback == accumulated.zback)) {
            accumulated = referenceMerge(accumulated, fragments[j]);
            ++j;
        }
        tidy.push_back(accumulated);
        i = j;
    }

    float transmittance = 1.f;
    for (std::size_t s = 0; s < tidy.size(); ++s) {
        for (int c = 0; c < kFlattenTestNumChannels; ++c) {
            out[c] += transmittance * tidy[s].channels[c];
        }
        transmittance *= (1.f - tidy[s].channels[kFlattenTestAlphaIndex]);
    }

    return out;
}

} // namespace

TEST(DeepFlattenTest, FlattenToImageMatchesSerialReference)
{
    const RectI bounds(0, 0, kFlattenTestWidth, kFlattenTestHeight);
    const TestPixels pixels = makeTestPixels();
    const DeepImagePtr src = makeTestDeepImage(pixels, bounds);
    const ImagePtr dst = makeFlattenDestination(bounds);

    DeepPixelScratch scratch;
    DeepTidyWorkspace work;
    ASSERT_EQ(eStatusOK, DeepFlatten::flattenToImage(*src, bounds, flattenTestChannelNames(), kFlattenTestAlphaIndex, &scratch, &work, dst));

    Image::ReadAccess access = dst->getReadRights();
    for (int y = bounds.y1; y < bounds.y2; ++y) {
        for (int x = bounds.x1; x < bounds.x2; ++x) {
            const std::size_t index = (std::size_t)(y * kFlattenTestWidth + x);
            const std::vector<float> expected = referenceFlatten(pixels[index]);
            const float* actual = (const float*)access.pixelAt(x, y);
            ASSERT_TRUE(actual != NULL);
            for (int c = 0; c < kFlattenTestNumChannels; ++c) {
                ASSERT_NEAR(expected[c], actual[c], 1e-5f) << "at pixel (" << x << ", " << y << ") channel " << c;
            }
        }
    }
}

TEST(DeepFlattenTest, ProbeReturnsTheSamplesStoredAtThePixel)
{
    const RectI bounds(0, 0, kFlattenTestWidth, kFlattenTestHeight);
    const TestPixels pixels = makeTestPixels();
    const DeepImagePtr src = makeTestDeepImage(pixels, bounds);

    std::vector<std::string> channelNames;
    std::vector<DeepSample> samples;

    // Channel names come back in the order a DeepImage stores its channels, which is why they
    // come back at all: a DeepSample's values are otherwise unlabelled.
    ASSERT_TRUE(DeepFlatten::getSamplesAtPixel(*src, 1, 1, &channelNames, &samples));
    ASSERT_EQ((std::size_t)kFlattenTestNumChannels, channelNames.size());
    EXPECT_EQ(std::string("A"), channelNames[0]);
    EXPECT_EQ(std::string("B"), channelNames[1]);
    EXPECT_EQ(std::string("G"), channelNames[2]);
    EXPECT_EQ(std::string("R"), channelNames[3]);

    const std::vector<TestSample>& expected = pixels[(std::size_t)(1 * kFlattenTestWidth + 1)];
    ASSERT_EQ(expected.size(), samples.size());
    for (std::size_t s = 0; s < expected.size(); ++s) {
        EXPECT_FLOAT_EQ(expected[s].z, samples[s].z) << "at sample " << s;
        EXPECT_FLOAT_EQ(expected[s].zback, samples[s].zback) << "at sample " << s;
        ASSERT_EQ((std::size_t)kFlattenTestNumChannels, samples[s].channels.size());
        // channels[3] of the payload is A, which getSamplesAtPixel reports first, and so on back.
        EXPECT_FLOAT_EQ(expected[s].channels[3], samples[s].channels[0]) << "at sample " << s;
        EXPECT_FLOAT_EQ(expected[s].channels[2], samples[s].channels[1]) << "at sample " << s;
        EXPECT_FLOAT_EQ(expected[s].channels[1], samples[s].channels[2]) << "at sample " << s;
        EXPECT_FLOAT_EQ(expected[s].channels[0], samples[s].channels[3]) << "at sample " << s;
    }

    EXPECT_TRUE(DeepFlatten::getSamplesAtPixel(*src, 0, 0, &channelNames, &samples));
    EXPECT_TRUE(samples.empty());

    EXPECT_FALSE(DeepFlatten::getSamplesAtPixel(*src, kFlattenTestWidth, 0, &channelNames, &samples));
}

TEST(DeepFlattenTest, ProbeReturnsRawSamplesRatherThanTidiedOnes)
{
    const RectI bounds(0, 0, kFlattenTestWidth, kFlattenTestHeight);
    const TestPixels pixels = makeTestPixels();
    const DeepImagePtr src = makeTestDeepImage(pixels, bounds);

    // Pixel 3 holds two partially overlapping volumetric samples, which tidy to three. The probe
    // must show the two the source actually contains, at their original depths and values.
    const std::vector<TestSample>& expected = pixels[3];
    ASSERT_EQ((std::size_t)2, expected.size());

    std::vector<std::string> channelNames;
    std::vector<DeepSample> samples;
    ASSERT_TRUE(DeepFlatten::getSamplesAtPixel(*src, 3, 0, &channelNames, &samples));

    ASSERT_EQ((std::size_t)2, samples.size());
    for (std::size_t s = 0; s < expected.size(); ++s) {
        EXPECT_FLOAT_EQ(expected[s].z, samples[s].z) << "at sample " << s;
        EXPECT_FLOAT_EQ(expected[s].zback, samples[s].zback) << "at sample " << s;
        EXPECT_FLOAT_EQ(expected[s].channels[kFlattenTestAlphaIndex], samples[s].channels[0]) << "at sample " << s;
    }

    // Tidying this pixel really would change what a probe showed, so the assertion above is not
    // vacuously true of any implementation.
    DeepPixelScratch tidied;
    DeepTidyWorkspace work;
    std::vector<const float*> channelPtrs((std::size_t)kFlattenTestNumChannels);
    const std::vector<std::string> names = flattenTestChannelNames();
    const U64 offset = src->getSampleTable().getOffset(3);
    for (int c = 0; c < kFlattenTestNumChannels; ++c) {
        channelPtrs[c] = src->getChannel(names[c])->data() + offset;
    }
    DeepPixelView pixel;
    pixel.z = src->getChannel("Z")->data() + offset;
    pixel.zback = src->getChannel("ZBack")->data() + offset;
    pixel.channels = channelPtrs.data();
    pixel.numChannels = kFlattenTestNumChannels;
    pixel.alphaChannelIndex = kFlattenTestAlphaIndex;
    pixel.numSamples = 2;
    DeepPixelOps::tidySamples(pixel, &tidied, &work);
    EXPECT_EQ(3, tidied.getSampleCount());
}
