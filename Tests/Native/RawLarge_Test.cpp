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
#include <map>
#include <optional>
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
#include "Engine/Nodes/IO/RawDngOpcodes.h"
#include "Engine/Nodes/Raw/RawDemosaic.h"
#include "Engine/Nodes/Raw/RawDevelop.h"
#include "Engine/Nodes/Raw/RawHighlights.h"
#include "Engine/Nodes/Raw/RawPrepare.h"
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

// The per-mode bounds below are counted in one-channel frames (kMosaicBytes, F) over what the
// bands cost with Clip. Every band and corner render starts from an empty node cache: a cached
// image asked for a second region grows to the bounding box of both, so two bands rendered
// against a warm cache would hold every stage over the whole frame.
//
// Reconstruct Color, Segmentation Based and Guided Laplacians ask for the whole source, so the
// source, RawPrepare and RawTemperature hold whole frames while they run (3 F), and Highlights
// reads the last in place. Reconstruct Color keeps one float per photosite (F).
constexpr std::size_t kMaxInpaintGrowthBytes = 3 * kMosaicBytes + kMosaicBytes + kMaxBandGrowthBytes;

// Segmentation Based keeps the same frame (F) and, while it computes it, works on planes of one
// value per 3x3 block: 8 float planes, 4 segment id planes and the combine scratch, 13 planes of
// F / 9, plus a flood-fill stack of F / 144 for each plane segmented. 14 F / 9
// covers both.
constexpr std::size_t kMaxSegmentsGrowthBytes = 3 * kMosaicBytes + kMosaicBytes + 14 * kMosaicBytes / 9 + kMaxBandGrowthBytes;

// Guided Laplacians keeps an RGBA image at a quarter of each side (F / 4) and builds it with five
// more of the same size (mask, temp, HF and two LF), F * 6 / 4 in all.
constexpr std::size_t kMaxLaplacianGrowthBytes = 3 * kMosaicBytes + 6 * kMosaicBytes / 4 + kMaxBandGrowthBytes;

// Flat Field, LMMSE, the dual methods, Frequency Domain Chroma and capture sharpening with a set
// radius read their window grown by a halo or a tile span of a few hundred photosites at most, so
// they take the Clip bound.
constexpr std::size_t kMaxWindowedGrowthBytes = kMaxBandGrowthBytes;

// Capture sharpening's automatic radius measures the centre 60 % x 60 % of the frame, and
// RawDemosaic merges that rectangle into the region it asks of its source, so a band asks for the
// bounding box of the band and the centre: 80 % of the frame. The source, RawPrepare,
// RawTemperature and RawHighlights each hold that (4 x 4 F / 5), RawHighlights copies its window
// into a top-down buffer (4 F / 5), and the measurement copies the centre twice (2 x 9 F / 25):
// 118 F / 25.
constexpr std::size_t kMaxAutoCaptureGrowthBytes = 118 * kMosaicBytes / 25 + kMaxBandGrowthBytes;

// The side of the corner windows rendered on their own and compared with the bands.
constexpr int kCornerSize = 64;

// Clipped squares every kBlobPeriod photosites, away from the corner windows, give the highlight
// modes something to rebuild; they are bright in every colour, so Segmentation Based also has
// areas where all of them clip.
constexpr int kBlobPeriod = 256;
constexpr int kBlobStart = 100;
constexpr int kBlobSize = 12;
static_assert((kWidth - kCornerSize) % kBlobPeriod >= kBlobStart + kBlobSize && (kHeight - kCornerSize) % kBlobPeriod >= kBlobStart + kBlobSize, "the corner windows must miss the clipped squares");

float
scene(int x,
      int y,
      int colour)
{
    return 0.1f + 0.05f * (float)colour + 0.2f * (float)((x % kPeriod) + (y % kPeriod)) / (float)(2 * (kPeriod - 1));
}

float
clippedScene(int x,
             int y,
             int colour)
{
    const int bx = x % kBlobPeriod;
    const int by = y % kBlobPeriod;

    if ((bx >= kBlobStart) && (bx < kBlobStart + kBlobSize) && (by >= kBlobStart) && (by < kBlobStart + kBlobSize)) {
        return 1.5f;
    }

    return scene(x, y, colour);
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

// Sets the peak resident set size back to the current one (Linux 4.0 and later). When it fails the
// peak keeps what earlier modes reached, so a mode's growth over it can only be smaller than the
// mode's own.
void
resetPeakResident()
{
    std::FILE* f = std::fopen("/proc/self/clear_refs", "w");

    if (f) {
        std::fputs("5", f);
        std::fclose(f);
    }
}

// Four maps of 5 x 4 points over the whole sensor, a different gain at every point and cell.
std::vector<DngGainMap>
largeGainMaps()
{
    std::vector<DngGainMap> maps;

    for (int cell = 0; cell < 4; ++cell) {
        DngGainMap g;
        g.top = (uint32_t)(cell >> 1);
        g.left = (uint32_t)(cell & 1);
        g.bottom = kHeight;
        g.right = kWidth;
        g.plane = 0;
        g.planes = 1;
        g.rowPitch = 2;
        g.colPitch = 2;
        g.mapPointsV = 4;
        g.mapPointsH = 5;
        g.mapSpacingV = 1. / 3.;
        g.mapSpacingH = 1. / 4.;
        g.mapOriginV = 0.;
        g.mapOriginH = 0.;
        g.mapPlanes = 1;
        for (uint32_t r = 0; r < g.mapPointsV; ++r) {
            for (uint32_t c = 0; c < g.mapPointsH; ++c) {
                g.gains.push_back(0.9f + 0.02f * (float)cell + 0.01f * (float)r + 0.005f * (float)c);
            }
        }
        maps.push_back(g);
    }

    return maps;
}

// Every pixel of window, which lies inside band, matches band; and the 8 x 8 pixels of window at
// the image's corner it touches are written.
void
expectWindowMatchesBand(const RenderedPlane& window,
                        const RenderedPlane& band,
                        int cornerY)
{
    ASSERT_GE(window.channels.size(), 3u);
    ASSERT_EQ(window.channels.size(), band.channels.size());
    ASSERT_TRUE(band.window.contains(window.window));

    std::size_t mismatches = 0;
    for (int y = window.window.y1; y < window.window.y2; ++y) {
        for (int x = window.window.x1; x < window.window.x2; ++x) {
            for (int c = 0; c < 3; ++c) {
                if (!(std::fabs(rawTestPlaneValue(window, x, y, c) - rawTestPlaneValue(band, x, y, c)) <= 1e-5f)) {
                    ++mismatches;
                }
            }
        }
    }
    EXPECT_EQ(0u, mismatches);

    for (int y = cornerY; y < cornerY + 8; ++y) {
        for (int x = kWidth - 8; x < kWidth; ++x) {
            for (int c = 0; c < 3; ++c) {
                const float v = rawTestPlaneValue(window, x, y, c);
                EXPECT_TRUE(std::isfinite(v) && (v > 0.f)) << "(" << x << ", " << y << ") channel " << c << " = " << v;
            }
        }
    }
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
        RawNodeSupport::setFrameComputedHookForTests(std::function<void(const char*, U64)>());
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

    // A 12288 x 8192 test source of pattern holding value(x, y, colour), with the black and white
    // levels, white balance and camera matrix of the first test, and maps as its GainMaps.
    NodePtr createLargeSource(const std::string& pattern,
                              float (*value)(int, int, int),
                              const std::vector<DngGainMap>& maps = std::vector<DngGainMap>())
    {
        NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));

        EXPECT_TRUE(bool(source));
        if (!source) {
            return source;
        }
        ImageMetadata metadata = rawTestMetadata(pattern, 0, 0, eRawStageRaw);
        RawMetadata::setBlackLevels(&metadata, { 0., 0., 0., 0. });
        RawMetadata::setWhiteLevel(&metadata, 1.);
        RawMetadata::setWbCoeffs(&metadata, { 1.2, 1., 1.1 });
        RawMetadata::setCamToXyz(&metadata, { 0.4124, 0.3576, 0.1805, 0.2126, 0.7152, 0.0722, 0.0193, 0.1192, 0.9505 });
        if (!maps.empty()) {
            RawMetadata::setGainMaps(&metadata, maps);
        }
        const std::optional<RawLayout> layout = RawLayout::fromMetadata(metadata);
        EXPECT_TRUE(bool(layout));
        if (!layout) {
            return NodePtr();
        }
        const RawLayout cfa = *layout;
        dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(RectI(0, 0, kWidth, kHeight), 1, [cfa, value](int x, int y, int) { return value(x, y, cfa.colourAt(x, y)); }, metadata);

        return source;
    }

    // Renders the two bands and the two far corners of output, each from an empty node cache but
    // for the second corner, which shares the first's, and checks that the peak resident set
    // grew by less than bound over the whole run, that each corner rendered on its own matches
    // the band holding it, and that the four renders of the one frame ran each whole-frame
    // computation named in frames exactly once and no other. Emptying the node cache leaves the
    // nodes' own whole-frame results, which only Clear Caches (purgeCaches()) drops.
    void developWithinBound(const NodePtr& output,
                            const char* mode,
                            std::size_t bound,
                            const char* boundText,
                            const std::vector<std::string>& frames = std::vector<std::string>())
    {
        SCOPED_TRACE(mode);
        const FrameComputedCounter computed;
        appPTR->clearNodeCache();
        resetPeakResident();
        const std::size_t before = peakResidentBytes();
        ASSERT_GT(before, 0u);

        std::string error;
        RenderedPlane bottom;
        ASSERT_TRUE(renderRawTestPlane(output, RectI(0, 0, kWidth, kBottomBandRows), &bottom, &error, false)) << error;
        appPTR->clearNodeCache();
        RenderedPlane top;
        ASSERT_TRUE(renderRawTestPlane(output, RectI(0, kHeight - kTopBandRows, kWidth, kHeight), &top, &error, false)) << error;
        appPTR->clearNodeCache();
        RenderedPlane low;
        ASSERT_TRUE(renderRawTestPlane(output, RectI(kWidth - kCornerSize, 0, kWidth, kCornerSize), &low, &error, false)) << error;
        RenderedPlane high;
        ASSERT_TRUE(renderRawTestPlane(output, RectI(kWidth - kCornerSize, kHeight - kCornerSize, kWidth, kHeight), &high, &error, false)) << error;
        appPTR->clearNodeCache();

        std::map<std::string, int> expected;
        for (const std::string& what : frames) {
            expected[what] = 1;
        }
        EXPECT_EQ(expected, computed.counts()) << "the whole-frame computations run by the two bands and the two corners";

        const std::size_t peak = peakResidentBytes();
        const std::size_t growth = (peak > before) ? (peak - before) : 0;
        EXPECT_LT(growth, bound) << "with " << mode << ", peak RSS grew by " << growth << " bytes, bound " << bound << " (" << boundText << ")";

        {
            SCOPED_TRACE("bottom right corner");
            expectWindowMatchesBand(low, bottom, 0);
        }
        {
            SCOPED_TRACE("top right corner");
            expectWindowMatchesBand(high, top, kHeight - 8);
        }
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

// The highlight modes that rebuild from the whole frame keep it per source and settings, so the
// four renders of each mode stay within the frames the mode holds.
TEST_F(RawLargeTest, HundredMegapixelWholeFrameHighlightModesStayWithinTheirBounds)
{
    NodePtr source = createLargeSource("RGGB", &clippedScene);
    ASSERT_TRUE(bool(source));
    const RawDevelop::Chain chain = RawDevelop::buildChain(source);
    ASSERT_TRUE(chain.complete());
    KnobChoicePtr highlightsMode = chain.highlights->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawHighlightsParamMode);
    ASSERT_TRUE(bool(highlightsMode));

    highlightsMode->setValue((int)eRawHighlightsModeInpaint);
    developWithinBound(chain.inputColor, "Reconstruct Color", kMaxInpaintGrowthBytes, "four one-channel frames and the Clip bound", { kRawHighlightsFrameReconstructColor });
    highlightsMode->setValue((int)eRawHighlightsModeSegments);
    developWithinBound(chain.inputColor, "Segmentation Based", kMaxSegmentsGrowthBytes, "four one-channel frames, 14/9 of one for the segmentation and the Clip bound", { kRawHighlightsFrameSegments, kRawHighlightsFrameOpposedChroma });
    highlightsMode->setValue((int)eRawHighlightsModeLaplacian);
    developWithinBound(chain.inputColor, "Guided Laplacians", kMaxLaplacianGrowthBytes, "4.5 one-channel frames and the Clip bound", { kRawHighlightsFrameLaplacian });
}

TEST_F(RawLargeTest, HundredMegapixelFlatFieldStaysWithinTheBandBound)
{
    NodePtr source = createLargeSource("RGGB", &scene, largeGainMaps());
    ASSERT_TRUE(bool(source));
    const RawDevelop::Chain chain = RawDevelop::buildChain(source);
    ASSERT_TRUE(chain.complete());
    KnobChoicePtr highlightsMode = chain.highlights->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawHighlightsParamMode);
    KnobChoicePtr flatField = chain.prepare->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawPrepareParamFlatField);
    ASSERT_TRUE(highlightsMode && flatField);

    highlightsMode->setValue((int)eRawHighlightsModeClip);
    flatField->setValue((int)eRawPrepareFlatFieldEmbedded);
    const std::optional<RawPrepare::Settings> settings = dynamic_cast<RawPrepare*>(chain.prepare->getEffectInstance().get())->resolveSettings(0., ViewIdx(0));
    ASSERT_TRUE(settings && settings->flatField);
    developWithinBound(chain.inputColor, "Flat Field", kMaxWindowedGrowthBytes, "the Clip bound");
}

TEST_F(RawLargeTest, HundredMegapixelBayerDemosaicModesStayWithinTheirBounds)
{
    NodePtr source = createLargeSource("RGGB", &scene);
    ASSERT_TRUE(bool(source));
    const RawDevelop::Chain chain = RawDevelop::buildChain(source);
    ASSERT_TRUE(chain.complete());
    const EffectInstancePtr demosaic = chain.demosaic->getEffectInstance();
    KnobChoicePtr highlightsMode = chain.highlights->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawHighlightsParamMode);
    KnobChoicePtr method = demosaic->getKnobByNameAndType<KnobChoice>(kRawDemosaicParamBayerMethod);
    KnobBoolPtr capture = demosaic->getKnobByNameAndType<KnobBool>(kRawDemosaicParamCaptureSharpen);
    KnobDoublePtr captureRadius = demosaic->getKnobByNameAndType<KnobDouble>(kRawDemosaicParamCaptureRadius);
    ASSERT_TRUE(highlightsMode && method && capture && captureRadius);
    highlightsMode->setValue((int)eRawHighlightsModeClip);

    method->setValue((int)eRawDemosaicBayerLMMSE);
    developWithinBound(chain.inputColor, "LMMSE", kMaxWindowedGrowthBytes, "the Clip bound");
    method->setValue((int)eRawDemosaicBayerRCDDual);
    developWithinBound(chain.inputColor, "RCD (dual)", kMaxWindowedGrowthBytes, "the Clip bound");
    method->setValue((int)eRawDemosaicBayerAMaZEDual);
    developWithinBound(chain.inputColor, "AMaZE (dual)", kMaxWindowedGrowthBytes, "the Clip bound");

    method->setValue((int)eRawDemosaicBayerRCD);
    capture->setValue(true);
    captureRadius->setValue(0.8);
    developWithinBound(chain.inputColor, "capture sharpening, radius 0.8", kMaxWindowedGrowthBytes, "the Clip bound");
    captureRadius->setValue(0.);
    developWithinBound(chain.inputColor, "capture sharpening, automatic radius", kMaxAutoCaptureGrowthBytes, "118/25 one-channel frames and the Clip bound", { kRawDemosaicFrameCaptureRadius });
    EXPECT_TRUE(bool(dynamic_cast<RawDemosaic*>(demosaic.get())->lastAutoCaptureRadius()));
}

TEST_F(RawLargeTest, HundredMegapixelXTransDemosaicModesStayWithinTheBandBound)
{
    const std::string xtrans = "GGRGGB"
                               "GGBGGR"
                               "BRGRBG"
                               "GGBGGR"
                               "GGRGGB"
                               "RBGBRG";
    NodePtr source = createLargeSource(xtrans, &scene);
    ASSERT_TRUE(bool(source));
    const RawDevelop::Chain chain = RawDevelop::buildChain(source);
    ASSERT_TRUE(chain.complete());
    KnobChoicePtr highlightsMode = chain.highlights->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawHighlightsParamMode);
    KnobChoicePtr method = chain.demosaic->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawDemosaicParamXTransMethod);
    ASSERT_TRUE(highlightsMode && method);
    highlightsMode->setValue((int)eRawHighlightsModeClip);

    method->setValue((int)eRawDemosaicXTransMarkesteijn3Dual);
    developWithinBound(chain.inputColor, "Markesteijn 3-pass (dual)", kMaxWindowedGrowthBytes, "the Clip bound");
    method->setValue((int)eRawDemosaicXTransFDC);
    developWithinBound(chain.inputColor, "Frequency Domain Chroma", kMaxWindowedGrowthBytes, "the Clip bound");
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
