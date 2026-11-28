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

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include "Global/Macros.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <QString>
#include <QTemporaryDir>

#include "Engine/Nodes/Scene/MaterialBake.h"
#include "Engine/Nodes/Scene/TexelPages.h"

NATRON_NAMESPACE_USING
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
typedef TexelPages::PageKey PageKey;
typedef TexelPages::PageData PageData;

PageKey
keyAt(int tile,
      int px,
      int py)
{
    PageKey key;
    key.tile = tile;
    key.px = px;
    key.py = py;

    return key;
}

// A base that is (tile / 10000, plane / 4, x / 1024, 1) at texel (x, y), counting its calls.
TexelPages::BaseFill
countingBase(std::atomic<int>* calls)
{
    return [calls](int tile, int plane, const RectI& texels, float* rgba, std::string*) {
        ++*calls;
        for (int y = texels.y1; y < texels.y2; ++y) {
            for (int x = texels.x1; x < texels.x2; ++x) {
                float* out = rgba + ((std::size_t)(x - texels.x1) + (std::size_t)(y - texels.y1) * texels.width()) * 4;
                out[0] = tile / 10000.f;
                out[1] = plane / 4.f;
                out[2] = x / 1024.f;
                out[3] = 1.f;
            }
        }

        return true;
    };
}

// A page holding value in every channel and touched, both exact in half floats.
PageData
filledPage(float value)
{
    PageData page;
    std::fill(page.rgba.begin(), page.rgba.end(), value);
    std::fill(page.touched.begin(), page.touched.end(), value);

    return page;
}

void
expectSamePage(const PageData& expected,
               const PageData& actual,
               const char* what)
{
    ASSERT_EQ(expected.rgba.size(), actual.rgba.size());
    int differences = 0;
    for (std::size_t i = 0; i < expected.rgba.size(); ++i) {
        differences += (expected.rgba[i] != actual.rgba[i]) ? 1 : 0;
    }
    for (std::size_t i = 0; i < expected.touched.size(); ++i) {
        differences += (expected.touched[i] != actual.touched[i]) ? 1 : 0;
    }
    EXPECT_EQ(0, differences) << what;
}

// An RGB half-float image of width x height whose pixel (x, row) is (x / 64, row / 64, 0.5).
void
writeRamp(const std::string& path,
          int width,
          int height)
{
    OIIO::ImageSpec spec(width, height, 3, OIIO::TypeDesc::HALF);
    std::vector<float> pixels;
    for (int row = 0; row < height; ++row) {
        for (int x = 0; x < width; ++x) {
            pixels.insert(pixels.end(), { x / 64.f, row / 64.f, 0.5f });
        }
    }
    OIIO::ImageOutput::unique_ptr out = OIIO::ImageOutput::create(path);
    ASSERT_TRUE(bool(out));
    ASSERT_TRUE(out->open(path, spec)) << out->geterror();
    ASSERT_TRUE(out->write_image(OIIO::TypeDesc::FLOAT, pixels.data())) << out->geterror();
    ASSERT_TRUE(out->close()) << out->geterror();
}

// The texels region of a size x size tile, as readFileRegion() reads them, against readBaseTile().
void
expectRegionMatchesTile(const std::string& path,
                        int size,
                        const RectI& texels)
{
    std::vector<float> tile;
    bool resampled = false;
    std::string error;
    ASSERT_TRUE(MaterialBake::readBaseTile(path, size, &tile, &resampled, &error)) << error;
    ASSERT_EQ((std::size_t)size * size * 4, tile.size());
    std::vector<float> region((std::size_t)texels.width() * texels.height() * 4, -1.f);
    ASSERT_TRUE(TexelPages::readFileRegion(path, size, texels, region.data(), &error)) << error;
    int differences = 0;
    for (int y = texels.y1; y < texels.y2; ++y) {
        for (int x = texels.x1; x < texels.x2; ++x) {
            for (int c = 0; c < 4; ++c) {
                const float expected = tile[((std::size_t)x + (std::size_t)y * size) * 4 + c];
                const float actual = region[((std::size_t)(x - texels.x1) + (std::size_t)(y - texels.y1) * texels.width()) * 4 + c];
                differences += (std::abs(expected - actual) > 1e-6f) ? 1 : 0;
            }
        }
    }
    EXPECT_EQ(0, differences) << path << " at " << size;
}
} // anonymous namespace

TEST(TexelPages, PagesExistOnlyOnceReadOrWritten)
{
    std::atomic<int> calls(0);
    TexelPages pages(256, countingBase(&calls));
    EXPECT_EQ(4, pages.pagesPerSide());
    EXPECT_EQ(0u, pages.pageCount());

    std::vector<float> tile((std::size_t)256 * 256 * 4, -1.f);
    std::string error;
    ASSERT_TRUE(pages.exportTile(1001, 0, tile.data(), nullptr, &error)) << error;
    EXPECT_EQ(0u, pages.pageCount());
    EXPECT_EQ(0, calls.load());
    for (float value : tile) {
        ASSERT_EQ(-1.f, value);
    }

    PageData page;
    ASSERT_TRUE(pages.read(keyAt(1002, 1, 2), &page, &error)) << error;
    EXPECT_EQ(1u, pages.pageCount());
    EXPECT_EQ(1, calls.load());
    EXPECT_TRUE(pages.paintedPages().empty());
    // Texel (64 + 3, 128 + 5) of tile 1002.
    const float* texel = page.rgba.data() + (3 + 5 * TexelPages::kPageSize) * 4;
    EXPECT_NEAR(0.1002f, texel[0], 1e-4f);
    EXPECT_NEAR(67.f / 1024.f, texel[2], 1e-4f);
    EXPECT_EQ(1.f, texel[3]);
    EXPECT_EQ(0.f, page.touched[3 + 5 * TexelPages::kPageSize]);

    ASSERT_TRUE(pages.write(keyAt(1001, 3, 0), filledPage(0.25f), &error)) << error;
    EXPECT_EQ(2u, pages.pageCount());
    EXPECT_EQ(1, calls.load());
    ASSERT_EQ(1u, pages.paintedPages().size());
    EXPECT_TRUE(pages.paintedPages().front() == keyAt(1001, 3, 0));

    std::vector<float> touched((std::size_t)256 * 256, -1.f);
    ASSERT_TRUE(pages.exportTile(1001, 0, tile.data(), touched.data(), &error)) << error;
    for (int y = 0; y < 256; ++y) {
        for (int x = 0; x < 256; ++x) {
            const bool painted = (x >= 192) && (y < 64);
            const std::size_t i = (std::size_t)x + (std::size_t)y * 256;
            ASSERT_EQ(painted ? 0.25f : -1.f, tile[i * 4]) << x << ", " << y;
            ASSERT_EQ(painted ? 0.25f : -1.f, touched[i]) << x << ", " << y;
        }
    }
}

TEST(TexelPages, AnEdgePageCoversOnlyTheTile)
{
    TexelPages pages(100, TexelPages::BaseFill());
    EXPECT_EQ(2, pages.pagesPerSide());
    const RectI edge = pages.texelsOf(keyAt(1001, 1, 1));
    EXPECT_EQ(64, edge.x1);
    EXPECT_EQ(64, edge.y1);
    EXPECT_EQ(100, edge.x2);
    EXPECT_EQ(100, edge.y2);
    EXPECT_TRUE(pages.texelsOf(keyAt(1001, 2, 0)).isNull());
    PageKey corner = keyAt(1003, 1, 1);
    corner.plane = 2;
    EXPECT_TRUE(TexelPages::pageOf(1003, 2, 99, 64) == corner);
}

TEST(TexelPages, BaseFileRegionsMatchTheWholeTile)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.path().toStdString() + "/ramp.exr";
    writeRamp(path, 128, 128);

    // At the file's size, then resampled up and down, over regions at its corners and middle.
    for (int size : { 128, 200, 96 }) {
        for (const RectI& texels : { RectI(0, 0, 64, 64), RectI(64, 0, 128, 64), RectI(size - 37, size - 5, size, size), RectI(20, 30, 50, 33) }) {
            expectRegionMatchesTile(path, size, texels);
        }
    }

    TexelPages pages(128, [path](int, int, const RectI& texels, float* rgba, std::string* error) {
        return TexelPages::readFileRegion(path, 128, texels, rgba, error);
    });
    std::vector<float> tile;
    bool resampled = true;
    std::string error;
    ASSERT_TRUE(MaterialBake::readBaseTile(path, 128, &tile, &resampled, &error)) << error;
    EXPECT_FALSE(resampled);
    PageData page;
    ASSERT_TRUE(pages.read(keyAt(1001, 1, 1), &page, &error)) << error;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            for (int c = 0; c < 4; ++c) {
                ASSERT_EQ(tile[((std::size_t)(64 + x) + (std::size_t)(64 + y) * 128) * 4 + c], page.rgba[((std::size_t)x + (std::size_t)y * 64) * 4 + c]);
            }
        }
    }
}

TEST(TexelPages, ARestoredSnapshotUndoesAWrite)
{
    std::atomic<int> calls(0);
    TexelPages pages(128, countingBase(&calls));
    std::string error;
    const PageKey painted = keyAt(1001, 0, 0);
    const PageKey based = keyAt(1001, 1, 0);
    ASSERT_TRUE(pages.write(painted, filledPage(0.5f), &error)) << error;
    PageData originalBase;
    ASSERT_TRUE(pages.read(based, &originalBase, &error)) << error;

    std::vector<TexelPages::Snapshot> snapshots;
    ASSERT_TRUE(pages.snapshot({ painted, based }, &snapshots, &error)) << error;
    ASSERT_EQ(2u, snapshots.size());

    ASSERT_TRUE(pages.write(painted, filledPage(0.75f), &error)) << error;
    ASSERT_TRUE(pages.write(based, filledPage(0.125f), &error)) << error;
    EXPECT_EQ(2u, pages.paintedPages().size());

    ASSERT_TRUE(pages.restore(snapshots, &error)) << error;
    PageData page;
    ASSERT_TRUE(pages.read(painted, &page, &error)) << error;
    expectSamePage(filledPage(0.5f), page, "the painted page");
    ASSERT_TRUE(pages.read(based, &page, &error)) << error;
    expectSamePage(originalBase, page, "the page that held its base");
    ASSERT_EQ(1u, pages.paintedPages().size());
    EXPECT_TRUE(pages.paintedPages().front() == painted);
}

TEST(TexelPages, PagesPastATinyBudgetComeBackTheSame)
{
    std::atomic<int> calls(0);
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    TexelPages pages(512, countingBase(&calls), TexelPages::kPageBytes, dir.path().toStdString());
    std::string error;

    PageData base;
    ASSERT_TRUE(pages.read(keyAt(1001, 7, 7), &base, &error)) << error;
    const float values[] = { 0.25f, 0.5f, 0.75f, 1.f, 2.f };
    for (int i = 0; i < 5; ++i) {
        PageData page = filledPage(values[i]);
        page.rgba[i] = -values[i];
        page.touched[TexelPages::kPageTexels - 1 - i] = 0.f;
        ASSERT_TRUE(pages.write(keyAt(1002, i, 0), page, &error)) << error;
    }
    // The base page, never painted, left memory rather than being spilled.
    EXPECT_EQ(5u, pages.pageCount());
    EXPECT_GE(1u, pages.residentPageCount());
    EXPECT_LE(4u, pages.spilledPageCount());

    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 4; i >= 0; --i) {
            PageData expected = filledPage(values[i]);
            expected.rgba[i] = -values[i];
            expected.touched[TexelPages::kPageTexels - 1 - i] = 0.f;
            PageData page;
            ASSERT_TRUE(pages.read(keyAt(1002, i, 0), &page, &error)) << error;
            expectSamePage(expected, page, "a spilled page");
        }
        const int before = calls.load();
        PageData again;
        ASSERT_TRUE(pages.read(keyAt(1001, 7, 7), &again, &error)) << error;
        expectSamePage(base, again, "a dropped base page");
        EXPECT_EQ(before + 1, calls.load());
    }

    std::vector<float> tile((std::size_t)512 * 512 * 4, 0.f);
    ASSERT_TRUE(pages.exportTile(1002, 0, tile.data(), nullptr, &error)) << error;
    EXPECT_EQ(-0.25f, tile[0]);
    EXPECT_EQ(2.f, tile[((std::size_t)(4 * 64 + 10) + 5 * 512) * 4 + 2]);
}
