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
#include <gtest/gtest.h>

#include "Engine/DeepImage.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

// Builds a DeepImage over the given bounds where every pixel has the same sample count, with
// R/G/B/A/Z/ZBack channels allocated and filled with a value derived from the sample index, so
// tests have something non-degenerate to check after a COW copy.
DeepImage
makeFilledDeepImage(const RectI& bounds, U32 samplesPerPixel)
{
    DeepImage img(bounds, RenderScale::identity, ViewIdx(0));
    SampleTable& table = img.getSampleTableForWriting();

    for (std::size_t i = 0; i < table.getPixelCount(); ++i) {
        table.setCount(i, samplesPerPixel);
    }
    table.recomputeOffsets();

    static const char* const kChannels[] = { "R", "G", "B", "A", "Z", "ZBack" };
    for (size_t c = 0; c < sizeof(kChannels) / sizeof(kChannels[0]); ++c) {
        DeepChannelBuffer& buf = img.getChannelForWriting(kChannels[c]);
        float* data = buf.dataForWriting();
        for (std::size_t i = 0; i < buf.size(); ++i) {
            data[i] = float(c * 1000 + i);
        }
    }

    return img;
}

} // namespace

TEST(DeepImageTest, ConstructionSizesTableToBoundsAndStartsEmpty)
{
    RectI bounds(0, 0, 4, 3);
    DeepImage img(bounds, RenderScale::identity, ViewIdx(0));

    EXPECT_TRUE(bounds == img.getBounds());
    EXPECT_EQ(ViewIdx(0), img.getView());
    EXPECT_EQ((std::size_t)12, img.getPixelCount());
    EXPECT_EQ((std::size_t)12, img.getSampleTable().getPixelCount());
    EXPECT_EQ((U64)0, img.getSampleTable().getTotalSampleCount());
    EXPECT_FALSE(img.hasChannel("R"));
    EXPECT_TRUE(img.getChannel("R") == NULL);
    EXPECT_FALSE(img.isTidy());
}

TEST(DeepImageTest, SampleTableOffsetsArePrefixSumOfCounts)
{
    RectI bounds(0, 0, 3, 1);
    DeepImage img(bounds, RenderScale::identity, ViewIdx(0));
    SampleTable& table = img.getSampleTableForWriting();

    table.setCount(0, 2);
    table.setCount(1, 0);
    table.setCount(2, 3);
    table.recomputeOffsets();

    EXPECT_EQ((U32)2, table.getCount(0));
    EXPECT_EQ((U32)0, table.getCount(1));
    EXPECT_EQ((U32)3, table.getCount(2));

    EXPECT_EQ((U64)0, table.getOffset(0));
    EXPECT_EQ((U64)2, table.getOffset(1));
    EXPECT_EQ((U64)2, table.getOffset(2));

    EXPECT_EQ((U64)5, table.getTotalSampleCount());

    // Every pixel's [offset, offset + count) range must land inside [0, totalSampleCount), and
    // a freshly-allocated channel buffer must be sized to exactly that total.
    DeepChannelBuffer& z = img.getChannelForWriting("Z");
    EXPECT_EQ((std::size_t)5, z.size());
    for (std::size_t i = 0; i < table.getPixelCount(); ++i) {
        EXPECT_LE(table.getOffset(i) + table.getCount(i), table.getTotalSampleCount());
    }
}

TEST(DeepImageTest, MutatingOneChannelLeavesOthersAndTableShared)
{
    RectI bounds(0, 0, 2, 2);
    DeepImage a = makeFilledDeepImage(bounds, 2);
    DeepImage b = a; // shallow COW copy: everything aliased so far.

    EXPECT_TRUE(a.sharesSampleTableWith(b));
    EXPECT_TRUE(a.sharesChannelStorageWith(b, "R"));
    EXPECT_TRUE(a.sharesChannelStorageWith(b, "Z"));
    EXPECT_TRUE(a.sharesChannelStorageWith(b, "ZBack"));
    EXPECT_TRUE(a.sharesChannelStorageWith(b, "A"));

    // Mutate only R on b.
    DeepChannelBuffer& bR = b.getChannelForWriting("R");
    float* bRData = bR.dataForWriting();
    for (std::size_t i = 0; i < bR.size(); ++i) {
        bRData[i] = -bRData[i];
    }

    // R has detached...
    EXPECT_FALSE(a.sharesChannelStorageWith(b, "R"));
    // ...but every other channel, and the sample table itself (untouched by a color-only
    // write), is still shared.
    EXPECT_TRUE(a.sharesSampleTableWith(b));
    EXPECT_TRUE(a.sharesChannelStorageWith(b, "G"));
    EXPECT_TRUE(a.sharesChannelStorageWith(b, "B"));
    EXPECT_TRUE(a.sharesChannelStorageWith(b, "A"));
    EXPECT_TRUE(a.sharesChannelStorageWith(b, "Z"));
    EXPECT_TRUE(a.sharesChannelStorageWith(b, "ZBack"));

    // a's own R data must be unaffected by the write through b's detached copy.
    const DeepChannelBuffer* aR = a.getChannel("R");
    ASSERT_TRUE(aR != NULL);
    for (std::size_t i = 0; i < aR->size(); ++i) {
        EXPECT_EQ(float(i), aR->data()[i]);
    }

    // Z (shared) must read identically through both handles.
    const DeepChannelBuffer* aZ = a.getChannel("Z");
    const DeepChannelBuffer* bZ = b.getChannel("Z");
    ASSERT_TRUE(aZ != NULL && bZ != NULL);
    EXPECT_EQ(aZ->data(), bZ->data());
}

TEST(DeepImageTest, SampleTableWriteDetachesOnlyWhenShared)
{
    RectI bounds(0, 0, 2, 1);
    DeepImage a(bounds, RenderScale::identity, ViewIdx(0));
    SampleTable& t0 = a.getSampleTableForWriting();
    t0.setCount(0, 1);
    t0.setCount(1, 1);
    t0.recomputeOffsets();

    // Not shared with anything yet: getSampleTableForWriting() must not reallocate.
    const SampleTable* beforeCopy = &a.getSampleTable();
    SampleTable& stillSame = a.getSampleTableForWriting();
    EXPECT_EQ(beforeCopy, &stillSame);

    DeepImage b = a;
    EXPECT_TRUE(a.sharesSampleTableWith(b));

    // Now shared with b: writing through a must detach, leaving b's table untouched.
    SampleTable& aTable = a.getSampleTableForWriting();
    aTable.setCount(0, 7);
    aTable.recomputeOffsets();

    EXPECT_FALSE(a.sharesSampleTableWith(b));
    EXPECT_EQ((U32)7, a.getSampleTable().getCount(0));
    EXPECT_EQ((U32)1, b.getSampleTable().getCount(0));
}

TEST(DeepImageTest, SizeInBytesIsStableAcrossSharingAndDetach)
{
    RectI bounds(0, 0, 4, 4);
    DeepImage a = makeFilledDeepImage(bounds, 3);

    const std::size_t tableBytes = a.getSampleTable().getSizeInBytes();
    const std::size_t oneChannelBytes = a.getChannel("R")->getSizeInBytes();
    const std::size_t sixChannelsBytes = oneChannelBytes * 6;
    const std::size_t fullSize = tableBytes + sixChannelsBytes;

    EXPECT_EQ(fullSize, a.getSizeInBytes());

    // getSizeInBytes() must depend only on a's own declared shape, not on what else aliases its
    // storage: a plain COW copy, a detach on the copy, and the copy going away must none of them
    // move a's reported size.
    {
        DeepImage b = a;
        EXPECT_EQ(fullSize, a.getSizeInBytes());
        EXPECT_EQ(fullSize, b.getSizeInBytes());

        b.getChannelForWriting("R").dataForWriting();
        EXPECT_EQ(fullSize, a.getSizeInBytes());
        EXPECT_EQ(fullSize, b.getSizeInBytes());
    }
    EXPECT_EQ(fullSize, a.getSizeInBytes());
}

TEST(DeepImageTest, UniquelyOwnedSizeInBytesCountsOwnedStorageOnlyOnceUnderSharing)
{
    RectI bounds(0, 0, 4, 4);
    DeepImage a = makeFilledDeepImage(bounds, 3);

    const std::size_t tableBytes = a.getSampleTable().getSizeInBytes();
    const std::size_t oneChannelBytes = a.getChannel("R")->getSizeInBytes();
    const std::size_t sixChannelsBytes = oneChannelBytes * 6;
    const std::size_t fullSize = tableBytes + sixChannelsBytes;

    // Sole owner of the table and every channel: full size.
    EXPECT_EQ(fullSize, a.getUniquelyOwnedSizeInBytes());

    // A plain COW copy aliases the table and every channel: neither image uniquely owns any
    // of it any more, so neither charges for it. This is the "not double-counted" guarantee --
    // summing both reports (0) never exceeds the true physical footprint (fullSize).
    DeepImage b = a;
    EXPECT_EQ((std::size_t)0, a.getUniquelyOwnedSizeInBytes());
    EXPECT_EQ((std::size_t)0, b.getUniquelyOwnedSizeInBytes());

    // A color-only op detaching just R (the DeepGrade scenario from the design doc): the
    // detach splits R into two independent buffers, one now uniquely owned by each of a and
    // b, while the table and the other 5 channels stay shared (and so still uncharged to
    // either). The two reports' sum now correctly equals the extra memory that really is used
    // twice (R), without ever double-counting the parts that are still physically shared once.
    b.getChannelForWriting("R").dataForWriting();

    EXPECT_EQ(oneChannelBytes, a.getUniquelyOwnedSizeInBytes());
    EXPECT_EQ(oneChannelBytes, b.getUniquelyOwnedSizeInBytes());
    EXPECT_LT(a.getUniquelyOwnedSizeInBytes() + b.getUniquelyOwnedSizeInBytes(), fullSize);
}

TEST(DeepImageTest, SetChannelAliasesAnotherImagesBufferDirectly)
{
    RectI bounds(0, 0, 2, 2);
    DeepImage a = makeFilledDeepImage(bounds, 1);
    DeepImage out(bounds, RenderScale::identity, ViewIdx(0));

    out.setChannel("Z", *a.getChannel("Z"));

    EXPECT_TRUE(a.sharesChannelStorageWith(out, "Z"));
    EXPECT_FALSE(out.hasChannel("R"));

    out.removeChannel("Z");
    EXPECT_FALSE(out.hasChannel("Z"));
}
