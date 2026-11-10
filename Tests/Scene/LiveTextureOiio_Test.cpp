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

#include <OpenImageIO/imageio.h>
#include <OpenImageIO/texture.h>

#include "Global/Macros.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "BaseTest.h"

#include "Engine/Nodes/Scene/LiveTextureOiio/LiveTextureFile.h"
#include "Engine/Nodes/Scene/LiveTextureStore.h"
#include "Engine/Nodes/Scene/SceneCaches.h"

NATRON_NAMESPACE_USING

/*
 * The plug-in as Cycles and MoonRay meet it: through an OIIO TextureSystem set up as theirs are, so it
 * neither converts an untiled file nor builds mips for an unmipped one.
 */

namespace {
const int kWidth = 100;
const int kHeight = 70;

LiveTextureBufferConstPtr
makeImage(int width,
          int height)
{
    std::shared_ptr<LiveTextureBuffer> buffer = LiveTextureBuffer::create(width, height);

    if (!buffer) {
        return buffer;
    }
    for (int y = 0; y < height; ++y) {
        float* row = buffer->getRow(y);
        for (int x = 0; x < width; ++x) {
            row[4 * x + 0] = (float)x / (float)width;
            row[4 * x + 1] = (float)y / (float)height;
            row[4 * x + 2] = 0.5f;
            row[4 * x + 3] = 1.f;
        }
    }

    return buffer;
}

std::string
uriOf(const char* node)
{
    return LiveTextureSource::makeUri(node, "rgba", 0x5678abcdULL);
}

// The level as OIIO's row order has it, top first.
std::vector<float>
expectedLevel(const LiveTextureBufferConstPtr& exported,
              int width,
              int height,
              int index)
{
    const NatronLiveTexture::Level level = NatronLiveTexture::levelAt(width, height, index);
    const float* const stored = reinterpret_cast<const float*>(reinterpret_cast<const char*>(exported->getRow(0)) - NatronLiveTexture::kHeaderSize + level.offset);
    std::vector<float> flipped(std::size_t(level.width) * level.height * 4);

    for (int y = 0; y < level.height; ++y) {
        const float* const from = stored + 4 * std::size_t(level.height - 1 - y) * level.width;
        std::copy(from, from + 4 * std::size_t(level.width), flipped.data() + 4 * std::size_t(y) * level.width);
    }

    return flipped;
}
} // anonymous namespace

class LiveTextureOiioTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        std::string error;
        ASSERT_TRUE(SceneCaches::registerLiveTextures(&error)) << error;
        _system = OIIO::TextureSystem::create(false);
        _system->attribute("accept_untiled", 0);
        _system->attribute("accept_unmipped", 0);
        _system->attribute("automip", 0);
        _system->attribute("autotile", 0);
    }

    virtual void TearDown() OVERRIDE
    {
        OIIO::TextureSystem::destroy(_system);
        LiveTextureStore::instance().clear();
        BaseTest::TearDown();
    }

    LiveTextureBufferConstPtr exportImage(const char* node,
                                          int width,
                                          int height)
    {
        LiveTextureStore& store = LiveTextureStore::instance();
        const std::string uri = uriOf(node);

        store.insert(uri, makeImage(width, height));
        const LiveTextureBufferConstPtr exported = store.exportToSharedMemory(uri);
        EXPECT_TRUE(bool(exported)) << store.getFailure(uri);

        return exported;
    }

    bool readLevel(const std::string& path,
                   int width,
                   int height,
                   int index,
                   std::vector<float>* pixels)
    {
        const NatronLiveTexture::Level level = NatronLiveTexture::levelAt(width, height, index);
        OIIO::TextureOpt options;

        pixels->resize(std::size_t(level.width) * level.height * 4);

        return _system->get_texels(OIIO::ustring(path), options, index, 0, level.width, 0, level.height, 0, 1, 0, 4, OIIO::TypeDesc::FLOAT, pixels->data());
    }

    std::shared_ptr<OIIO::TextureSystem> _system;
};

TEST_F(LiveTextureOiioTest, RegisteringServesThePluginToDelegates)
{
    const char* const path = std::getenv("OPENIMAGEIO_PLUGIN_PATH");
    ASSERT_NE(nullptr, path);
    const std::string value(path);
    const std::string first = value.substr(0, value.find(':'));
    EXPECT_TRUE(std::filesystem::is_regular_file(std::filesystem::path(first) / "natronlive.imageio.so")) << value;
}

TEST_F(LiveTextureOiioTest, EveryLevelReadsBackThroughTheTextureSystem)
{
    const LiveTextureBufferConstPtr exported = exportImage("Levels", kWidth, kHeight);
    ASSERT_TRUE(bool(exported));

    const int count = NatronLiveTexture::levelCount(kWidth, kHeight);
    ASSERT_EQ(7, count);
    for (int index = 0; index < count; ++index) {
        std::vector<float> pixels;
        ASSERT_TRUE(readLevel(exported->getFilePath(), kWidth, kHeight, index, &pixels)) << _system->geterror();
        EXPECT_EQ(expectedLevel(exported, kWidth, kHeight, index), pixels) << "level " << index;
    }
}

TEST_F(LiveTextureOiioTest, TheSpecSaysTiledStraightAlphaFloat)
{
    const LiveTextureBufferConstPtr exported = exportImage("Spec", kWidth, kHeight);
    ASSERT_TRUE(bool(exported));

    OIIO::ImageSpec spec;
    ASSERT_TRUE(_system->get_imagespec(OIIO::ustring(exported->getFilePath()), spec)) << _system->geterror();
    EXPECT_EQ(kWidth, spec.width);
    EXPECT_EQ(kHeight, spec.height);
    EXPECT_EQ(4, spec.nchannels);
    EXPECT_EQ(3, spec.alpha_channel);
    EXPECT_GT(spec.tile_width, 0);
    EXPECT_TRUE(spec.format == OIIO::TypeDesc::FLOAT);
    EXPECT_EQ(1, spec.get_int_attribute("oiio:UnassociatedAlpha"));
    // The pixels are in the working space, which the file does not name.
    EXPECT_EQ(nullptr, spec.find_attribute("oiio:ColorSpace"));
}

TEST_F(LiveTextureOiioTest, AMissingFileFailsWithoutCrashing)
{
    std::vector<float> pixels;

    EXPECT_FALSE(readLevel("/dev/shm/natron-missing-texture.natron", 4, 4, 0, &pixels));
    EXPECT_FALSE(_system->geterror().empty());
}

TEST_F(LiveTextureOiioTest, ATruncatedFileIsRefused)
{
    const LiveTextureBufferConstPtr exported = exportImage("Truncated", 8, 8);
    ASSERT_TRUE(bool(exported));
    const std::string truncated = "/dev/shm/natron-truncated-texture-" + std::to_string(::getpid()) + ".natron";
    const int fd = ::open(truncated.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600);
    ASSERT_GE(fd, 0);
    const char* const bytes = reinterpret_cast<const char*>(exported->getRow(0)) - NatronLiveTexture::kHeaderSize;
    EXPECT_EQ(ssize_t(NatronLiveTexture::kHeaderSize + 32), ::write(fd, bytes, NatronLiveTexture::kHeaderSize + 32));
    ::close(fd);

    std::vector<float> pixels;
    EXPECT_FALSE(readLevel(truncated, 8, 8, 0, &pixels));
    EXPECT_FALSE(_system->geterror().empty());
    ::unlink(truncated.c_str());
}

// Both renderers can close a file and open it again in the middle of a render.
TEST_F(LiveTextureOiioTest, AHeldFileReopensAfterItsStoreEntryIsGone)
{
    const LiveTextureBufferConstPtr exported = exportImage("Reopened", kWidth, kHeight);
    ASSERT_TRUE(bool(exported));
    std::vector<float> pixels;
    ASSERT_TRUE(readLevel(exported->getFilePath(), kWidth, kHeight, 0, &pixels)) << _system->geterror();

    LiveTextureStore::instance().clear();
    _system->invalidate(OIIO::ustring(exported->getFilePath()));

    ASSERT_TRUE(readLevel(exported->getFilePath(), kWidth, kHeight, 1, &pixels)) << _system->geterror();
    EXPECT_EQ(expectedLevel(exported, kWidth, kHeight, 1), pixels);
}

// One open file and a tiny cache force the seeks and reopens a render makes from every thread.
TEST_F(LiveTextureOiioTest, ManyThreadsReadThroughOneInput)
{
    const int size = 256;
    const LiveTextureBufferConstPtr first = exportImage("ThreadsFirst", size, size);
    const LiveTextureBufferConstPtr second = exportImage("ThreadsSecond", size, size);
    ASSERT_TRUE(bool(first));
    ASSERT_TRUE(bool(second));
    _system->attribute("max_open_files", 1);
    _system->attribute("max_memory_MB", 1.f);

    const int count = NatronLiveTexture::levelCount(size, size);
    std::vector<std::vector<float>> expected;
    for (int index = 0; index < count; ++index) {
        expected.push_back(expectedLevel(first, size, size, index));
    }
    std::atomic<int> mismatches(0);
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t]() {
            for (int pass = 0; pass < 8; ++pass) {
                const int index = (t + pass) % count;
                std::vector<float> pixels;
                const bool useFirst = (t % 2 == 0);
                if (!readLevel((useFirst ? first : second)->getFilePath(), size, size, index, &pixels) || (pixels != expected[index])) {
                    mismatches.fetch_add(1);
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(0, mismatches.load());
}
