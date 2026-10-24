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
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include <ofxImageEffect.h>

#include "BaseTest.h"
#include "RawFixtures.h"
#include "RawTestSource.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/NativeRead.h"
#include "Engine/Nodes/IO/RawDecodeSupport.h"
#include "Engine/Nodes/Raw/RawDevelop.h"
#include "Engine/Nodes/Raw/RawHighlights.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_USING

namespace {
constexpr int kWidth = 12288;
constexpr int kHeight = 8192;

// The scene repeats every 64 photosites in both axes (a multiple of the Bayer period), so a band
// at the far corner of the frame must render what a band a whole number of periods nearer the
// origin renders.
constexpr int kPeriod = 64;

// Rows of the two bands rendered. The bottom band reaches past the first period so that rows
// well inside the image can be compared with the top band's.
constexpr int kBottomBandRows = 224;
constexpr int kTopBandRows = 160;
constexpr int kCompareFirstRow = kHeight - 128;
constexpr int kCompareRows = 96;
constexpr int kRowShift = kHeight - 256;
static_assert(kRowShift % kPeriod == 0, "the shift between the bands must be whole periods");
static_assert(kWidth % kPeriod == 0, "the frame must hold whole periods");
static_assert(kCompareFirstRow - kRowShift >= 128 && kCompareFirstRow + kCompareRows - kRowShift <= kBottomBandRows, "the compared rows must lie inside the bottom band, away from the image edge");

// With every stage asking only for what its window needs (Highlights set to Clip), the developed
// chain rendered over bands of rows must stay under half the bytes of the whole frame as float
// RGBA. Rendering the frame in one piece costs several times that (every stage holds a full-frame
// image), so the bound catches a path that materialises the frame while leaving room for the
// bands' own stages, the read-back planes and the demosaic scratch.
constexpr std::size_t kFrameRgbaBytes = (std::size_t)kWidth * (std::size_t)kHeight * 4 * sizeof(float);
constexpr std::size_t kMaxBandGrowthBytes = kFrameRgbaBytes / 2;

// Inpaint Opposed, the default, measures a chrominance over the whole mosaic, so it asks for its
// whole source. The test source never changes, so the node cache keeps whole the outputs of the
// source and of the two stages above Highlights: three one-channel frames. Highlights reads its
// whole source in place and keeps three floats, so beyond those frames the bands cost what they
// cost with Clip. A second whole-frame copy, or any RGBA frame, goes over.
constexpr std::size_t kMosaicBytes = (std::size_t)kWidth * (std::size_t)kHeight * sizeof(float);
constexpr std::size_t kMaxOpposedGrowthBytes = 3 * kMosaicBytes + kMaxBandGrowthBytes;

float
scene(int x,
      int y,
      int colour)
{
    return 0.1f + 0.05f * (float)colour + 0.2f * (float)((x % kPeriod) + (y % kPeriod)) / (float)(2 * (kPeriod - 1));
}

// Peak resident set size of this process in bytes, or 0 when /proc cannot be read.
std::size_t
peakResidentBytes()
{
    std::FILE* f = std::fopen("/proc/self/status", "r");

    if (!f) {
        return 0;
    }
    char line[256];
    std::size_t kb = 0;
    while (std::fgets(line, sizeof(line), f)) {
        if (std::sscanf(line, "VmHWM: %zu kB", &kb) == 1) {
            break;
        }
    }
    std::fclose(f);

    return kb * 1024;
}
} // namespace

class RawLargeTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerRawTestSourceOnce();
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        RawDecodeSupport::clearCaches();
    }

    virtual void TearDown() OVERRIDE
    {
        RawDecodeSupport::setDecodedHookForTests(std::function<void(const std::string&)>());
        getApp()->getProject()->reset(false, true);
        RawDecodeSupport::clearCaches();
        BaseTest::TearDown();
    }

    NodePtr createRead(const std::string& path)
    {
        NodePtr read = createNode(QString::fromUtf8(PLUGINID_NATRON_READ), PLUGIN_MAJOR_NATRON_READ);

        EXPECT_TRUE(bool(read));
        if (read) {
            KnobFilePtr file = read->getEffectInstance()->getKnobByNameAndType<KnobFile>(kOfxImageEffectFileParamName);
            EXPECT_TRUE(bool(file));
            if (file) {
                file->setValue(path);
            }
        }

        return read;
    }
};

// 12288 x 8192 photosites (100.7 MP). The whole frame is not rendered: in a debug build the
// per-photosite test source and the demosaic would take minutes, so two full-width bands are
// rendered instead, one on the first rows and one ending on the last row. Together they reach the
// far corner, where row and pixel offsets are the largest and a 32-bit index would first go wrong.
TEST_F(RawLargeTest, HundredMegapixelMosaicDevelopsAtTheFarCornerWithinTheMemoryBound)
{
    NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));
    ASSERT_TRUE(bool(source));

    ImageMetadata metadata = rawTestMetadata("RGGB", 0, 0, eRawStageRaw);
    RawMetadata::setBlackLevels(&metadata, { 0., 0., 0., 0. });
    RawMetadata::setWhiteLevel(&metadata, 1.);
    RawMetadata::setWbCoeffs(&metadata, { 1.2, 1., 1.1 });
    RawMetadata::setCamToXyz(&metadata, { 0.4124, 0.3576, 0.1805, 0.2126, 0.7152, 0.0722, 0.0193, 0.1192, 0.9505 });
    const RawLayout layout = *RawLayout::fromPattern("RGGB");
    dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(RectI(0, 0, kWidth, kHeight), 1, [layout](int x, int y, int) { return scene(x, y, layout.colourAt(x, y)); }, metadata);

    const RawDevelop::Chain chain = RawDevelop::buildChain(source);
    ASSERT_TRUE(chain.complete());
    KnobChoicePtr highlightsMode = chain.highlights->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawHighlightsParamMode);
    ASSERT_TRUE(bool(highlightsMode));

    const std::size_t peakBefore = peakResidentBytes();
    ASSERT_GT(peakBefore, 0u);

    // Renders the two bands and checks that the far corner matches the near one and is written.
    const auto developBands = [&](const char* method) {
        SCOPED_TRACE(method);
        std::string error;
        RenderedPlane bottom;
        ASSERT_TRUE(renderRawTestPlane(chain.inputColor, RectI(0, 0, kWidth, kBottomBandRows), &bottom, &error)) << error;
        RenderedPlane top;
        ASSERT_TRUE(renderRawTestPlane(chain.inputColor, RectI(0, kHeight - kTopBandRows, kWidth, kHeight), &top, &error)) << error;
        ASSERT_GE(bottom.channels.size(), 3u);
        ASSERT_EQ(bottom.channels.size(), top.channels.size());

        std::size_t mismatches = 0;
        for (int y = kCompareFirstRow; y < kCompareFirstRow + kCompareRows; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                for (int c = 0; c < 3; ++c) {
                    const float farValue = rawTestPlaneValue(top, x, y, c);
                    const float nearValue = rawTestPlaneValue(bottom, x, y - kRowShift, c);
                    if (!(std::fabs(farValue - nearValue) <= 1e-5f)) {
                        ++mismatches;
                    }
                }
            }
        }
        EXPECT_EQ(0u, mismatches);

        // Nothing else writes the corner, so a wrong offset leaves zeros (or garbage) there.
        for (int y = kHeight - 8; y < kHeight; ++y) {
            for (int x = kWidth - 8; x < kWidth; ++x) {
                for (int c = 0; c < 3; ++c) {
                    const float v = rawTestPlaneValue(top, x, y, c);
                    EXPECT_TRUE(std::isfinite(v) && (v > 0.f)) << "(" << x << ", " << y << ") channel " << c << " = " << v;
                }
            }
        }
    };
    const auto growthSinceStart = [peakBefore]() {
        const std::size_t peak = peakResidentBytes();

        return (peak > peakBefore) ? (peak - peakBefore) : 0;
    };

    // The peak only rises, so the band-limited chain goes first.
    highlightsMode->setValue((int)eRawHighlightsModeClip);
    developBands("Clip");
    const std::size_t bandGrowth = growthSinceStart();
    EXPECT_LT(bandGrowth, kMaxBandGrowthBytes) << "with Clip, peak RSS grew by " << bandGrowth << " bytes, bound " << kMaxBandGrowthBytes << " (half of one " << kWidth << "x" << kHeight << " float RGBA frame)";

    highlightsMode->setValue((int)eRawHighlightsModeOpposed);
    developBands("Inpaint Opposed");
    const std::size_t opposedGrowth = growthSinceStart();
    EXPECT_LT(opposedGrowth, kMaxOpposedGrowthBytes) << "with Inpaint Opposed, peak RSS grew by " << opposedGrowth << " bytes, bound " << kMaxOpposedGrowthBytes << " (three one-channel " << kWidth << "x" << kHeight << " frames and the Clip bound)";
}

// Two Reads of one file, and threads asking the decode cache for different parts of it at once,
// decode the file once.
TEST_F(RawLargeTest, TwoReadsAndConcurrentRequestsOfOneFileDecodeOnce)
{
    const std::string path = bayerFixture().path();
    std::atomic<int> decodes(0);
    RawDecodeSupport::setDecodedHookForTests([&decodes](const std::string&) { ++decodes; });

    NodePtr first = createRead(path);
    NodePtr second = createRead(path);
    ASSERT_TRUE(first && second);

    std::string error;
    const std::shared_ptr<const RawDecodeSupport::Header> header = RawDecodeSupport::readHeader(path, &error);
    ASSERT_TRUE(bool(header)) << error;

    const int nThreads = 4;
    const int bandHeight = std::min(64, header->window.height() / nThreads);
    std::atomic<int> ready(0);
    std::atomic<int> failures(0);
    std::vector<std::thread> threads;
    for (int i = 0; i < nThreads; ++i) {
        threads.emplace_back([&, i]() {
            const RectI window(header->window.x1, header->window.y1 + i * bandHeight, header->window.x2, header->window.y1 + (i + 1) * bandHeight);
            std::vector<float> pixels((std::size_t)window.area());
            ++ready;
            while (ready.load() < nThreads) {
                std::this_thread::yield();
            }
            std::string threadError;
            if (!RawDecodeSupport::decode(path, window, pixels.data(), (std::size_t)window.width(), &threadError)) {
                ++failures;
            }
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }
    EXPECT_EQ(0, failures.load());

    const RectI window(header->window.x1 + 37, header->window.y1 + 23, header->window.x1 + 37 + 48, header->window.y1 + 23 + 40);
    // A render that bypasses the cache empties every cache, which purges the Reads and drops the
    // decoded file with them, as Clear All Caches does. Only the image cache is emptied here, so
    // that the second Read renders instead of reusing the first one's image, and both go to the
    // decode cache.
    RenderedPlane a;
    RenderedPlane b;
    appPTR->clearNodeCache();
    ASSERT_TRUE(renderRawTestPlane(first, window, &a, &error, false)) << error;
    appPTR->clearNodeCache();
    ASSERT_TRUE(renderRawTestPlane(second, window, &b, &error, false)) << error;
    EXPECT_EQ(a.pixels, b.pixels);

    EXPECT_EQ(1, decodes.load());
}
