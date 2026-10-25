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
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "RawGpuTestSupport.h"

#include "Engine/KnobTypes.h"
#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif
#include "Engine/Nodes/Raw/RawDemosaic.h"
#include "Engine/Nodes/Raw/RawDenoise.h"
#include "Engine/Nodes/Raw/RawHighlights.h"
#include "Engine/Nodes/Raw/RawHotPixels.h"
#include "Engine/Nodes/Raw/RawInputColor.h"
#include "Engine/Nodes/Raw/RawPrepare.h"
#include "Engine/Nodes/Raw/RawTemperature.h"

NATRON_NAMESPACE_USING

namespace {
// The chain's loosest bound is its demosaic's: the Markesteijn and AMaZE kernels branch on
// quotients Vulkan rounds within 2.5 ulp where the CPU rounds exactly, so a pixel on a threshold
// can take the other branch. Bounds are relative to the CPU render's mean colour level.
const double kChainMeanBound = 1e-4;
const double kChainMaxBound = 1e-4;

// AMaZE is ill-conditioned on denoised data, on the CPU as on the GPU: a one-ulp change to its
// input moves about a third of the CR2's pixels by more than 1e-4 and the worst by about 1e-2,
// where on the prepared mosaic it moves a few hundredths of a percent. Denoise's square roots
// round within an ulp of the CPU's, so its output differs from the CPU's by that much, and
// AMaZE's threshold decisions downstream flip in coherent patches. The bound on the pixels
// still off by more than this is the demosaic's one in a thousand.
const double kBayerChainMaxBound = 5e-3;

const RectI kWindow(200, 150, 264, 198);

// Large enough that Markesteijn's and AMaZE's tiles take several batches under kBatchingRange.
const RectI kLargeWindow(100, 100, 400, 400);

// The region's images and work buffers fit, while the tile scratch holds two Markesteijn 3-pass
// tiles and four AMaZE tiles, where the device's range holds every tile of kLargeWindow at once.
const std::uint64_t kBatchingRange = std::uint64_t(8) << 20;

// One pixel a thousand, rounded up.
int
outlierBudget(std::size_t nPixels)
{
    return (int)((nPixels + 999) / 1000);
}

#ifdef NATRON_HAVE_GPU
class StorageRangeCap {
public:
    explicit StorageRangeCap(std::uint64_t bytes)
    {
        gpu::GpuRecordContext::setMaxStorageBufferBytesForTests(bytes);
    }

    ~StorageRangeCap()
    {
        gpu::GpuRecordContext::setMaxStorageBufferBytesForTests(0);
    }

    StorageRangeCap(const StorageRangeCap&) = delete;
    StorageRangeCap& operator=(const StorageRangeCap&) = delete;
};
#endif
} // namespace

class NativeRawChainGpuExtendedTest
    : public RawGpuTest {
protected:
    struct Chain {
        NodePtr read;
        NodePtr prepare;
        NodePtr hotPixels;
        NodePtr denoise;
        NodePtr temperature;
        NodePtr highlights;
        NodePtr demosaic;
        NodePtr inputColor;

        std::vector<NodePtr> develop() const
        {
            return { prepare, hotPixels, denoise, temperature, highlights, demosaic, inputColor };
        }
    };

    // Clip highlights, because the default Inpaint Opposed has no GPU path.
    Chain createChain(const RawFixture& fixture,
                      const char* demosaicParam,
                      int demosaicMethod)
    {
        Chain c;

        c.read = createRead(fixture);
        c.prepare = createAfter(PLUGINID_NATRON_RAWPREPARE, c.read);
        c.hotPixels = createAfter(PLUGINID_NATRON_RAWHOTPIXELS, c.prepare);
        c.denoise = createAfter(PLUGINID_NATRON_RAWDENOISE, c.hotPixels);
        c.temperature = createAfter(PLUGINID_NATRON_RAWTEMPERATURE, c.denoise);
        c.highlights = createAfter(PLUGINID_NATRON_RAWHIGHLIGHTS, c.temperature);
        c.demosaic = createAfter(PLUGINID_NATRON_RAWDEMOSAIC, c.highlights);
        c.inputColor = createAfter(PLUGINID_NATRON_RAWINPUTCOLOR, c.demosaic);
        if (c.highlights) {
            c.highlights->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawHighlightsParamMode)->setValue((int)eRawHighlightsModeClip);
        }
        if (c.demosaic) {
            c.demosaic->getEffectInstance()->getKnobByNameAndType<KnobChoice>(demosaicParam)->setValue(demosaicMethod);
        }

        return c;
    }

    // The demosaic bound over the chain's end-to-end renders: the mean colour error within
    // kChainMeanBound of the CPU's mean level, at most outlierBudget pixels off by more than
    // maxBound of it, and alpha and finiteness exact.
    static void expectWithinDemosaicBound(const EndToEndResult& result,
                                          double maxBound,
                                          const std::string& what)
    {
        ASSERT_EQ(result.cpu.size(), result.gpu.size()) << what;
        const std::size_t nComps = 4;
        const std::size_t nPixels = result.cpu.size() / nComps;
        ASSERT_GT(nPixels, 0u) << what;
        double sum = 0.;
        double level = 0.;
        std::size_t nonFinite = 0;
        std::size_t alphaMismatches = 0;
        std::vector<double> pixelErrors(nPixels, 0.);
        for (std::size_t p = 0; p < nPixels; ++p) {
            for (std::size_t c = 0; c < nComps; ++c) {
                const float a = result.cpu[p * nComps + c];
                const float b = result.gpu[p * nComps + c];
                const bool same = (a == b) || (std::isnan(a) && std::isnan(b));
                if (c == 3) {
                    alphaMismatches += same ? 0 : 1;
                    continue;
                }
                const double error = same ? 0. : std::fabs((double)a - (double)b);
                if (!std::isfinite(error)) {
                    ++nonFinite;
                    continue;
                }
                sum += error;
                level += std::isfinite(a) ? std::fabs((double)a) : 0.;
                pixelErrors[p] = std::max(pixelErrors[p], error);
            }
        }
        const double count = 3. * (double)nPixels;
        const double meanError = sum / count;
        level /= count;
        ASSERT_GT(level, 0.) << what << ": the CPU render is black";
        const int outliers = (int)std::count_if(pixelErrors.begin(), pixelErrors.end(), [&](double e) { return e > maxBound * level; });
        std::printf("[          ] %s: mean error %.2e of the mean level %g, %d of %zu pixels over %.0e\n", what.c_str(), meanError / level, level, outliers, nPixels, maxBound);
        EXPECT_EQ(0u, nonFinite) << what << ": colour floats finite on one side only";
        EXPECT_EQ(0u, alphaMismatches) << what << ": alpha differs";
        EXPECT_LE(meanError / level, kChainMeanBound) << what;
        EXPECT_LE(outliers, outlierBudget(nPixels)) << what;
    }

    static int gpuTasks(const EndToEndResult& result,
                        const NodePtr& node)
    {
        const std::map<std::string, int>::const_iterator found = result.gpuTasksPerNode.find(node->getScriptName_mt_safe());

        return (found == result.gpuTasksPerNode.end()) ? 0 : found->second;
    }

#ifdef NATRON_HAVE_GPU
    // Under a storage buffer range that splits the demosaic's tiles into more batches, the chain
    // still renders on the GPU, bit for bit as under the device's range, and within the
    // demosaic's bound of the CPU.
    void expectBatchedChain(const Chain& c,
                            double maxBound,
                            const std::string& what)
    {
        ASSERT_TRUE(c.read && c.prepare && c.hotPixels && c.denoise && c.temperature && c.highlights && c.demosaic && c.inputColor) << what;

        const EndToEndResult whole = renderEndToEnd(c.develop(), c.inputColor, kLargeWindow, kTime);
        if (whole.skipped) {
            return;
        }
        ASSERT_TRUE(whole.ok) << what << ": " << whole.error;
        ASSERT_GT(gpuTasks(whole, c.demosaic), 0) << what << ": the demosaic fell back to the CPU";
        EndToEndResult batched;
        {
            StorageRangeCap cap(kBatchingRange);
            batched = renderEndToEnd(c.develop(), c.inputColor, kLargeWindow, kTime);
        }
        ASSERT_TRUE(batched.ok) << what << ": " << batched.error;
        ASSERT_GT(gpuTasks(batched, c.demosaic), 0) << what << ": the demosaic fell back to the CPU under the capped range";
        ASSERT_EQ(whole.gpu.size(), batched.gpu.size()) << what;
        const ImageDiff diff = compareImages(whole.gpu, batched.gpu, ParityTolerance::exact());
        EXPECT_EQ(0u, diff.mismatches) << what << ": first at float " << diff.firstMismatchIndex << " (one batch " << whole.gpu[diff.firstMismatchIndex] << ", batched "
                                       << batched.gpu[diff.firstMismatchIndex] << ")";
        expectWithinDemosaicBound(batched, maxBound, what + ", batched");
    }

    // A demosaic whose single tile is over the storage buffer range renders on the CPU, the
    // whole region with it, while one just large enough for the tile keeps it on the GPU.
    void expectTileOverRangeFallsBack(const Chain& c,
                                      std::uint64_t tileFits,
                                      std::uint64_t tileOver,
                                      const std::string& what)
    {
        ASSERT_TRUE(c.read && c.prepare && c.hotPixels && c.denoise && c.temperature && c.highlights && c.demosaic && c.inputColor) << what;

        EndToEndResult fits;
        {
            StorageRangeCap cap(tileFits);
            fits = renderEndToEnd(c.develop(), c.inputColor, kWindow, kTime);
        }
        if (fits.skipped) {
            return;
        }
        ASSERT_TRUE(fits.ok) << what << ": " << fits.error;
        EXPECT_GT(gpuTasks(fits, c.demosaic), 0) << what << ": the demosaic fell back to the CPU with room for a tile";

        EndToEndResult over;
        {
            StorageRangeCap cap(tileOver);
            over = renderEndToEnd(c.develop(), c.inputColor, kWindow, kTime);
        }
        ASSERT_TRUE(over.ok) << what << ": " << over.error;
        EXPECT_EQ(0, gpuTasks(over, c.demosaic)) << what << ": the demosaic rendered on the GPU over the range";
        ASSERT_EQ(over.cpu.size(), over.gpu.size()) << what;
        const ImageDiff diff = compareImages(over.cpu, over.gpu, ParityTolerance::exact());
        EXPECT_EQ(0u, diff.mismatches) << what << ": first at float " << diff.firstMismatchIndex;
    }
#endif // NATRON_HAVE_GPU

    void expectResidentChain(const Chain& c,
                             double maxBound,
                             const std::string& what)
    {
        ASSERT_TRUE(c.read && c.prepare && c.hotPixels && c.denoise && c.temperature && c.highlights && c.demosaic && c.inputColor) << what;

        const EndToEndResult result = renderEndToEnd(c.develop(), c.inputColor, kWindow, kTime);
        if (result.skipped) {
            return;
        }
        ASSERT_TRUE(result.ok) << what << ": " << result.error;
        ASSERT_EQ(1u, result.regions.size()) << what;
        for (const NodePtr& node : c.develop()) {
            EXPECT_TRUE(result.ranInRegion(node)) << what << ": " << node->getScriptName_mt_safe() << " is not in the region";
        }
        EXPECT_FALSE(result.ranInRegion(c.read)) << what;
        EXPECT_EQ(1, result.uploads) << what;
        EXPECT_EQ(1, result.downloads) << what;
        expectWithinDemosaicBound(result, maxBound, what);
    }
};

#ifdef NATRON_HAVE_GPU

// Hot pixels, denoise and Markesteijn 3-pass on the RAF run in the same single resident region as
// the rest of the Develop chain.
TEST_F(NativeRawChainGpuExtendedTest, XTransChainWithMarkesteijnIsOneResidentRegion)
{
    const Chain c = createChain(xtransFixture(), kRawDemosaicParamXTransMethod, (int)eRawDemosaicXTransMarkesteijn3);

    expectResidentChain(c, kChainMaxBound, "RAF Markesteijn 3-pass");
}

// The same chain on the CR2 with AMaZE.
TEST_F(NativeRawChainGpuExtendedTest, BayerChainWithAmazeIsOneResidentRegion)
{
    const Chain c = createChain(bayerFixture(), kRawDemosaicParamBayerMethod, (int)eRawDemosaicBayerAMaZE);

    expectResidentChain(c, kBayerChainMaxBound, "CR2 AMaZE");
}

// Markesteijn 3-pass and AMaZE in more batches of tiles than the device's range needs.
TEST_F(NativeRawChainGpuExtendedTest, ASmallStorageRangeBatchesTheTilesWithTheSameResult)
{
    expectBatchedChain(createChain(xtransFixture(), kRawDemosaicParamXTransMethod, (int)eRawDemosaicXTransMarkesteijn3), kChainMaxBound, "RAF Markesteijn 3-pass");
    getApp()->getProject()->reset(false, true);
    expectBatchedChain(createChain(bayerFixture(), kRawDemosaicParamBayerMethod, (int)eRawDemosaicBayerAMaZE), kBayerChainMaxBound, "CR2 AMaZE");
}

// A Markesteijn 3-pass tile takes 2.5 MB of scratch and an AMaZE tile 1.5 MB, so once the pool's
// rounding is allowed for, a 2 MB range holds no Markesteijn tile and a 1.5 MB one no AMaZE tile.
TEST_F(NativeRawChainGpuExtendedTest, ATileOverTheStorageRangeFallsBackToTheCpu)
{
    expectTileOverRangeFallsBack(createChain(xtransFixture(), kRawDemosaicParamXTransMethod, (int)eRawDemosaicXTransMarkesteijn3), std::uint64_t(4) << 20, std::uint64_t(2) << 20,
                                 "RAF Markesteijn 3-pass");
    getApp()->getProject()->reset(false, true);
    expectTileOverRangeFallsBack(createChain(bayerFixture(), kRawDemosaicParamBayerMethod, (int)eRawDemosaicBayerAMaZE), std::uint64_t(2) << 20, std::uint64_t(3) << 19, "CR2 AMaZE");
}

#endif // NATRON_HAVE_GPU
