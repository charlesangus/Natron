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
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <QString>
#include <QTemporaryDir>

#include "Engine/Nodes/IO/OiioReadSupport.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_USING

namespace {
struct Fixture {
    std::string name;
    OIIO::TypeDesc fileType;
    int channels;
    int width;
    int height;
    int x;
    int y;
    int fullX;
    int fullY;
    int fullWidth;
    int fullHeight;
    int tile;
};

float
pixelValue(int x,
           int y,
           int c)
{
    return (float)((x * 131 + y * 17 + c * 7) % 251) / 250.f;
}

std::string
writeFixture(const QTemporaryDir& dir,
             const Fixture& f)
{
    const std::string path = (dir.path() + QString::fromUtf8("/") + QString::fromStdString(f.name)).toStdString();
    OIIO::ImageSpec spec(f.width, f.height, f.channels, f.fileType);
    spec.x = f.x;
    spec.y = f.y;
    spec.full_x = f.fullX;
    spec.full_y = f.fullY;
    spec.full_width = f.fullWidth;
    spec.full_height = f.fullHeight;
    if (f.tile > 0) {
        spec.tile_width = f.tile;
        spec.tile_height = f.tile;
        spec.tile_depth = 1;
    }
    std::vector<float> pixels((std::size_t)f.width * (std::size_t)f.height * (std::size_t)f.channels);
    for (int j = 0; j < f.height; ++j) {
        for (int i = 0; i < f.width; ++i) {
            for (int c = 0; c < f.channels; ++c) {
                pixels[((std::size_t)j * (std::size_t)f.width + (std::size_t)i) * (std::size_t)f.channels + (std::size_t)c] = pixelValue(i, j, c);
            }
        }
    }
    OIIO::ImageOutput::unique_ptr out = OIIO::ImageOutput::create(path);
    EXPECT_TRUE(bool(out));
    if (!out) {
        return std::string();
    }
    EXPECT_TRUE(out->open(path, spec)) << out->geterror();
    EXPECT_TRUE(out->write_image(OIIO::TypeDesc::FLOAT, pixels.data())) << out->geterror();
    EXPECT_TRUE(out->close()) << out->geterror();

    return path;
}

// Decodes `window` and compares it with OIIO's own whole-image read, bit for bit. Row 0 of the
// window is its bottom, so it must equal the file row `height - 1 - (window.y1 - dataWindow.y1)`.
void
expectWindowMatchesOiio(const std::string& path,
                        int chbegin,
                        int chend,
                        const RectI& window)
{
    SCOPED_TRACE(path + " window " + std::to_string(window.x1) + "," + std::to_string(window.y1) + "," + std::to_string(window.x2) + "," + std::to_string(window.y2) + " channels " + std::to_string(chbegin) + ".." + std::to_string(chend));

    OIIO::ImageInput::unique_ptr input = OIIO::ImageInput::open(path);
    ASSERT_TRUE(bool(input));
    const OIIO::ImageSpec spec = input->spec();
    const std::size_t channels = (std::size_t)(chend - chbegin);
    std::vector<float> reference((std::size_t)spec.width * (std::size_t)spec.height * channels);
    ASSERT_TRUE(input->read_image(0, 0, chbegin, chend, OIIO::TypeDesc::FLOAT, reference.data())) << input->geterror();
    input->close();

    const RectI dataWindow = OiioReadSupport::dataWindowOf(spec);
    const std::size_t w = (std::size_t)window.width();
    const std::size_t h = (std::size_t)window.height();
    const std::size_t stride = w * channels + 3;
    const float sentinel = 12345.f;
    std::vector<float> decoded(h * stride, sentinel);

    std::string error;
    ASSERT_TRUE(OiioReadSupport::decode(path, 0, chbegin, chend, window, decoded.data(), stride, &error)) << error;

    std::size_t mismatches = 0;
    for (std::size_t row = 0; row < h; ++row) {
        const std::size_t fileRow = (std::size_t)spec.height - 1 - (std::size_t)(window.y1 - dataWindow.y1) - row;
        for (std::size_t col = 0; col < w; ++col) {
            const std::size_t fileCol = (std::size_t)(window.x1 - dataWindow.x1) + col;
            const float* want = &reference[(fileRow * (std::size_t)spec.width + fileCol) * channels];
            const float* got = &decoded[row * stride + col * channels];
            if (std::memcmp(want, got, channels * sizeof(float)) != 0) {
                ++mismatches;
            }
        }
        for (std::size_t pad = w * channels; pad < stride; ++pad) {
            EXPECT_EQ(sentinel, decoded[row * stride + pad]) << "row " << row << " padding was written";
        }
    }
    EXPECT_EQ(0u, mismatches);
}

void
expectAllWindowsMatch(const std::string& path)
{
    OIIO::ImageInput::unique_ptr input = OIIO::ImageInput::open(path);
    ASSERT_TRUE(bool(input)) << path;
    const OIIO::ImageSpec spec = input->spec();
    input->close();

    const RectI d = OiioReadSupport::dataWindowOf(spec);
    const int w = d.width();
    const int h = d.height();
    ASSERT_GE(w, 6);
    ASSERT_GE(h, 4);

    std::vector<RectI> windows;
    windows.push_back(d);
    windows.push_back(RectI(d.x1 + w / 4, d.y1 + h / 4, d.x2 - w / 4, d.y2 - h / 4));
    windows.push_back(RectI(d.x1, d.y1, d.x1 + w / 2 + 1, d.y1 + h / 2 + 1));
    windows.push_back(RectI(d.x2 - w / 2 - 1, d.y2 - h / 2 - 1, d.x2, d.y2));
    windows.push_back(RectI(d.x1 + 1, d.y1, d.x2 - 1, d.y1 + 1));
    windows.push_back(RectI(d.x2 - 1, d.y1, d.x2, d.y2));
    windows.push_back(RectI(d.x1, d.y2 - 1, d.x1 + 1, d.y2));
    windows.push_back(RectI(d.x1 + 5, d.y1 + 3, d.x1 + 6, d.y1 + 4));
    for (std::size_t i = 0; i < windows.size(); ++i) {
        expectWindowMatchesOiio(path, 0, spec.nchannels, windows[i]);
        if (spec.nchannels > 1) {
            expectWindowMatchesOiio(path, 1, spec.nchannels, windows[i]);
            expectWindowMatchesOiio(path, 0, 1, windows[i]);
        }
    }
}

void
expectFixturesMatch(const std::vector<Fixture>& fixtures)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    for (std::size_t i = 0; i < fixtures.size(); ++i) {
        const std::string path = writeFixture(dir, fixtures[i]);
        ASSERT_FALSE(path.empty());
        expectAllWindowsMatch(path);
    }
}

std::string
repoFixture(const char* name)
{
    return std::string(NATRON_TESTS_FIXTURES_DIR "/") + name;
}
} // anonymous namespace

TEST(OiioReadSupport, ExrHalfAndFloatScanlineWithAnOffsetDataWindow)
{
    expectFixturesMatch({
        { "half-scan.exr", OIIO::TypeDesc::HALF, 4, 97, 61, 13, 7, 0, 0, 160, 100, 0 },
        { "float-scan.exr", OIIO::TypeDesc::FLOAT, 3, 70, 90, -9, -4, -20, -10, 128, 128, 0 },
        { "float-scan-1ch.exr", OIIO::TypeDesc::FLOAT, 1, 33, 40, 0, 0, 0, 0, 33, 40, 0 },
    });
}

TEST(OiioReadSupport, ExrHalfAndFloatTiledWithAnOffsetDataWindow)
{
    expectFixturesMatch({
        { "half-tiled.exr", OIIO::TypeDesc::HALF, 4, 97, 61, 13, 7, 0, 0, 160, 100, 16 },
        { "float-tiled.exr", OIIO::TypeDesc::FLOAT, 3, 70, 90, -9, -4, -20, -10, 128, 128, 32 },
        { "float-tiled-exact.exr", OIIO::TypeDesc::FLOAT, 2, 64, 64, 0, 0, 0, 0, 64, 64, 16 },
    });
}

TEST(OiioReadSupport, Png8And16)
{
    expectFixturesMatch({
        { "p8.png", OIIO::TypeDesc::UINT8, 4, 53, 41, 0, 0, 0, 0, 53, 41, 0 },
        { "p8rgb.png", OIIO::TypeDesc::UINT8, 3, 53, 41, 0, 0, 0, 0, 53, 41, 0 },
        { "p16.png", OIIO::TypeDesc::UINT16, 4, 53, 41, 0, 0, 0, 0, 53, 41, 0 },
    });
}

TEST(OiioReadSupport, Jpeg)
{
    expectFixturesMatch({
        { "j.jpg", OIIO::TypeDesc::UINT8, 3, 67, 45, 0, 0, 0, 0, 67, 45, 0 },
    });
}

TEST(OiioReadSupport, Tiff8And16AndFloat)
{
    expectFixturesMatch({
        { "t8.tif", OIIO::TypeDesc::UINT8, 4, 59, 37, 0, 0, 0, 0, 59, 37, 0 },
        { "t16.tif", OIIO::TypeDesc::UINT16, 3, 59, 37, 0, 0, 0, 0, 59, 37, 0 },
        { "tf.tif", OIIO::TypeDesc::FLOAT, 4, 59, 37, 0, 0, 0, 0, 59, 37, 0 },
        { "tf-tiled.tif", OIIO::TypeDesc::FLOAT, 4, 59, 37, 0, 0, 0, 0, 59, 37, 16 },
    });
}

TEST(OiioReadSupport, RepositoryFixtures)
{
    expectAllWindowsMatch(repoFixture("png-8bit.png"));
    expectAllWindowsMatch(repoFixture("png-16bit.png"));
    expectAllWindowsMatch(repoFixture("flat-rgba-pattern.exr"));
    expectAllWindowsMatch(repoFixture("flat-rgb-offset-window.exr"));
    expectAllWindowsMatch(repoFixture("flat-alpha-offset-window.exr"));
    expectAllWindowsMatch(repoFixture("flat-rgb-a-offset-window.exr"));
}

TEST(OiioReadSupport, DataWindowIsMirroredWithinTheDisplayWindow)
{
    OIIO::ImageSpec spec(10, 6, 1, OIIO::TypeDesc::FLOAT);
    spec.x = 3;
    spec.y = 2;
    spec.full_x = 0;
    spec.full_y = 0;
    spec.full_width = 20;
    spec.full_height = 12;

    const RectI r = OiioReadSupport::dataWindowOf(spec);
    EXPECT_EQ(3, r.x1);
    EXPECT_EQ(13, r.x2);
    EXPECT_EQ(4, r.y1);
    EXPECT_EQ(10, r.y2);
}

TEST(OiioReadSupport, HeaderCacheFollowsTheFileOnDisk)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const Fixture small = { "h.exr", OIIO::TypeDesc::FLOAT, 3, 16, 12, 0, 0, 0, 0, 16, 12, 0 };
    const std::string path = writeFixture(dir, small);
    ASSERT_FALSE(path.empty());

    OiioReadSupport::clearHeaderCache();
    std::string error;
    const std::shared_ptr<const OiioReadSupport::Header> first = OiioReadSupport::readHeader(path, &error);
    ASSERT_TRUE(bool(first)) << error;
    ASSERT_EQ(1u, first->subimages.size());
    EXPECT_EQ(16, first->subimages[0].width);
    EXPECT_EQ(first, OiioReadSupport::readHeader(path, &error));

    const Fixture big = { "h.exr", OIIO::TypeDesc::FLOAT, 3, 32, 24, 0, 0, 0, 0, 32, 24, 0 };
    ASSERT_FALSE(writeFixture(dir, big).empty());
    const std::shared_ptr<const OiioReadSupport::Header> second = OiioReadSupport::readHeader(path, &error);
    ASSERT_TRUE(bool(second)) << error;
    EXPECT_EQ(32, second->subimages[0].width);
}

TEST(OiioReadSupport, AHeaderParsedAcrossAnEvictionIsNotCached)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const Fixture wide = { "h.pfm", OIIO::TypeDesc::FLOAT, 3, 4, 2, 0, 0, 0, 0, 4, 2, 0 };
    const std::string path = writeFixture(dir, wide);
    ASSERT_FALSE(path.empty());
    std::error_code ec;
    const std::filesystem::file_time_type mtime = std::filesystem::last_write_time(path, ec);
    ASSERT_FALSE(ec) << ec.message();
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    ASSERT_FALSE(ec) << ec.message();

    // A rewrite with the same size and modification time, which the cache cannot tell from the
    // file it parsed.
    OiioReadSupport::clearHeaderCache();
    bool rewritten = false;
    OiioReadSupport::setHeaderParsedHookForTests([&](const std::string& parsed) {
        if (rewritten || parsed != path) {
            return;
        }
        rewritten = true;
        const Fixture tall = { "h.pfm", OIIO::TypeDesc::FLOAT, 3, 2, 4, 0, 0, 0, 0, 2, 4, 0 };
        EXPECT_FALSE(writeFixture(dir, tall).empty());
        std::error_code setError;
        std::filesystem::last_write_time(path, mtime, setError);
        EXPECT_FALSE(setError) << setError.message();
        OiioReadSupport::evictHeaders(std::vector<std::string>(1, path));
    });
    std::string error;
    const std::shared_ptr<const OiioReadSupport::Header> during = OiioReadSupport::readHeader(path, &error);
    OiioReadSupport::setHeaderParsedHookForTests(std::function<void(const std::string&)>());

    ASSERT_TRUE(rewritten);
    ASSERT_TRUE(bool(during)) << error;
    EXPECT_EQ(4, during->subimages[0].width);
    ASSERT_EQ(size, std::filesystem::file_size(path, ec));
    ASSERT_TRUE(mtime == std::filesystem::last_write_time(path, ec));

    const std::shared_ptr<const OiioReadSupport::Header> after = OiioReadSupport::readHeader(path, &error);
    ASSERT_TRUE(bool(after)) << error;
    EXPECT_EQ(2, after->subimages[0].width);
    EXPECT_EQ(4, after->subimages[0].height);
}

TEST(OiioReadSupport, ReportsErrors)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    std::string error;
    EXPECT_FALSE(bool(OiioReadSupport::readHeader((dir.path() + QString::fromUtf8("/missing.exr")).toStdString(), &error)));
    EXPECT_FALSE(error.empty());

    const Fixture f = { "e.exr", OIIO::TypeDesc::FLOAT, 3, 16, 12, 0, 0, 0, 0, 16, 12, 0 };
    const std::string path = writeFixture(dir, f);
    ASSERT_FALSE(path.empty());
    std::vector<float> buf(16 * 12 * 3);
    EXPECT_FALSE(OiioReadSupport::decode(path, 0, 0, 3, RectI(0, 0, 17, 12), buf.data(), 17 * 3, &error));
    EXPECT_FALSE(OiioReadSupport::decode(path, 0, 0, 4, RectI(0, 0, 16, 12), buf.data(), 16 * 3, &error));
    EXPECT_FALSE(OiioReadSupport::decode(path, 1, 0, 3, RectI(0, 0, 16, 12), buf.data(), 16 * 3, &error));
    EXPECT_FALSE(OiioReadSupport::decode(path, 0, 0, 3, RectI(0, 0, 16, 12), buf.data(), 16 * 3 - 1, &error));
    EXPECT_TRUE(OiioReadSupport::decode(path, 0, 0, 3, RectI(4, 4, 4, 8), buf.data(), 0, &error));
}
