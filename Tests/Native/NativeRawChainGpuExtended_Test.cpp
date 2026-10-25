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
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "RawGpuTestSupport.h"

#include "Engine/KnobTypes.h"
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

const RectI kWindow(200, 150, 264, 198);

// One pixel a thousand, rounded up.
int
outlierBudget(const RectI& window)
{
    return (int)(((std::int64_t)window.width() * window.height() + 999) / 1000);
}
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
    // kChainMaxBound of it, and alpha and finiteness exact.
    static void expectWithinDemosaicBound(const EndToEndResult& result,
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
        const int outliers = (int)std::count_if(pixelErrors.begin(), pixelErrors.end(), [&](double e) { return e > kChainMaxBound * level; });
        std::printf("[          ] %s: mean error %.2e of the mean level %g, %d of %zu pixels over %.0e\n", what.c_str(), meanError / level, level, outliers, nPixels, kChainMaxBound);
        EXPECT_EQ(0u, nonFinite) << what << ": colour floats finite on one side only";
        EXPECT_EQ(0u, alphaMismatches) << what << ": alpha differs";
        EXPECT_LE(meanError / level, kChainMeanBound) << what;
        EXPECT_LE(outliers, outlierBudget(kWindow)) << what;
    }

    void expectResidentChain(const Chain& c,
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
        expectWithinDemosaicBound(result, what);
    }
};

#ifdef NATRON_HAVE_GPU

// Hot pixels, denoise and Markesteijn 3-pass on the RAF run in the same single resident region as
// the rest of the Develop chain.
TEST_F(NativeRawChainGpuExtendedTest, XTransChainWithMarkesteijnIsOneResidentRegion)
{
    const Chain c = createChain(xtransFixture(), kRawDemosaicParamXTransMethod, (int)eRawDemosaicXTransMarkesteijn3);

    expectResidentChain(c, "RAF Markesteijn 3-pass");
}

// The same chain on the CR2 with AMaZE.
TEST_F(NativeRawChainGpuExtendedTest, BayerChainWithAmazeIsOneResidentRegion)
{
    const Chain c = createChain(bayerFixture(), kRawDemosaicParamBayerMethod, (int)eRawDemosaicBayerAMaZE);

    expectResidentChain(c, "CR2 AMaZE");
}

#endif // NATRON_HAVE_GPU
