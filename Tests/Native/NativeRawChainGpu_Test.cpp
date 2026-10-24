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

#include <bitset>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "RawGpuTestSupport.h"

#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Raw/RawDemosaic.h"
#include "Engine/Nodes/Raw/RawHighlights.h"
#include "Engine/Nodes/Raw/RawInputColor.h"
#include "Engine/Nodes/Raw/RawPrepare.h"
#include "Engine/Nodes/Raw/RawTemperature.h"

NATRON_NAMESPACE_USING

namespace {
// Three products summed in the CPU's order, each rounded exactly on both sides.
const ParityTolerance kInputColorGpuTolerance = ParityTolerance::arithmetic();
// The chain's bound is its demosaic's; every other node in it stays within the arithmetic class.
const ParityTolerance kChainGpuTolerance = ParityTolerance::transcendental();

const RectI kWindow(200, 150, 264, 198);
} // namespace

class NativeRawChainGpuTest
    : public RawGpuTest {
protected:
    struct Chain {
        NodePtr read;
        NodePtr prepare;
        NodePtr temperature;
        NodePtr highlights;
        NodePtr demosaic;
        NodePtr inputColor;

        std::vector<NodePtr> develop() const
        {
            return { prepare, temperature, highlights, demosaic, inputColor };
        }
    };

    // The Develop chain on the Bayer fixture, with RCD and the highlights mode given.
    Chain createChain(RawHighlightsModeEnum highlightsMode)
    {
        Chain c;

        c.read = createRead(bayerFixture());
        c.prepare = createAfter(PLUGINID_NATRON_RAWPREPARE, c.read);
        c.temperature = createAfter(PLUGINID_NATRON_RAWTEMPERATURE, c.prepare);
        c.highlights = createAfter(PLUGINID_NATRON_RAWHIGHLIGHTS, c.temperature);
        c.demosaic = createAfter(PLUGINID_NATRON_RAWDEMOSAIC, c.highlights);
        c.inputColor = createAfter(PLUGINID_NATRON_RAWINPUTCOLOR, c.demosaic);
        if (c.highlights) {
            c.highlights->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawHighlightsParamMode)->setValue((int)highlightsMode);
        }
        if (c.demosaic) {
            c.demosaic->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawDemosaicParamBayerMethod)->setValue((int)eRawDemosaicBayerRCD);
        }

        return c;
    }

    // The query a render of node's RGBA plane makes, with the channels its layer knob processes.
    static GpuQuery rgbaQueryFor(const NodePtr& node)
    {
        GpuQuery query = queryFor(4);
        EffectInstancePtr effect = node->getEffectInstance();
        const std::bitset<4> channels = effect->getProcessChannelsForPlane(effect->getRenderHash(), kTime, ViewIdx(0), ImageLayerDesc::getRGBAComponents());

        for (int i = 0; i < 4; ++i) {
            query.processChannels[i] = channels[i];
        }

        return query;
    }
};

#ifdef NATRON_HAVE_GPU

// The matrix of every kind of input profile against the CPU, on demosaiced camera RGB.
TEST_F(NativeRawChainGpuTest, InputColorMatchesTheCpu)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const Chain c = createChain(eRawHighlightsModeClip);
    ASSERT_TRUE(c.read && c.prepare && c.temperature && c.highlights && c.demosaic && c.inputColor);
    KnobBoolPtr profileOverride = c.inputColor->getEffectInstance()->getKnobByNameAndType<KnobBool>(kRawInputColorParamProfileOverride);
    KnobChoicePtr profile = c.inputColor->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawInputColorParamProfile);
    ASSERT_TRUE(profileOverride && profile);
    KnobDoublePtr row1 = c.inputColor->getEffectInstance()->getKnobByNameAndType<KnobDouble>(kRawInputColorParamRow1);
    ASSERT_TRUE(bool(row1));
    profileOverride->setValue(true);

    for (const char* mode : { kRawInputColorProfileStandardMatrix, kRawInputColorProfileEnhancedMatrix, kRawInputColorProfileLinearRec709, kRawInputColorProfileCustom }) {
        profile->setValueFromID(mode, 0);
        if (std::string(mode) == kRawInputColorProfileCustom) {
            row1->setValue(-0.35, ViewSpec::all(), 0);
            row1->setValue(1.4, ViewSpec::all(), 1);
            row1->setValue(-0.05, ViewSpec::all(), 2);
        }
        const GpuQuery query = rgbaQueryFor(c.inputColor);
        expectGpuMatchesCpu(*runtime, c.inputColor, c.demosaic, kWindow, kInputColorGpuTolerance, std::string("RawInputColor profile ") + mode, &query);
    }
}

// The Develop chain from RawPrepare to RawInputColor, with RCD, runs as one region: the mosaic is
// uploaded once, every node renders on the device without a readback between them, and the image
// is downloaded once, within the demosaic's bound of the CPU chain.
TEST_F(NativeRawChainGpuTest, DevelopChainIsOneResidentRegion)
{
    for (RawHighlightsModeEnum highlights : { eRawHighlightsModeClip, eRawHighlightsModeLCh }) {
        const Chain c = createChain(highlights);
        ASSERT_TRUE(c.read && c.prepare && c.temperature && c.highlights && c.demosaic && c.inputColor);
        const std::string what = (highlights == eRawHighlightsModeClip) ? "clip" : "lch";

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
        ASSERT_EQ(result.cpu.size(), result.gpu.size()) << what;
        const ImageDiff diff = compareImages(result.cpu, result.gpu, kChainGpuTolerance);
        EXPECT_EQ(0u, diff.mismatches) << what << ": first at float " << diff.firstMismatchIndex << " (cpu " << result.cpu[diff.firstMismatchIndex] << ", gpu "
                                       << result.gpu[diff.firstMismatchIndex] << "), the worst by " << diff.worstAbsolute;
        std::printf("develop chain, %s: worst difference %g\n", what.c_str(), diff.worstAbsolute);
        getApp()->getProject()->reset(false, true);
    }
}

// With Inpaint Opposed, which has no GPU path, the raw run stays whole on the CPU: only
// RawInputColor, which reads the image, goes to the device.
TEST_F(NativeRawChainGpuTest, AChainWithACpuOpKeepsItsRawRunOnTheCpu)
{
    const Chain c = createChain(eRawHighlightsModeOpposed);
    ASSERT_TRUE(c.read && c.prepare && c.temperature && c.highlights && c.demosaic && c.inputColor);

    const EndToEndResult result = renderEndToEnd(c.develop(), c.inputColor, kWindow, kTime);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;
    for (const NodePtr& node : { c.prepare, c.temperature, c.highlights, c.demosaic }) {
        EXPECT_FALSE(result.ranInRegion(node)) << node->getScriptName_mt_safe();
    }
    EXPECT_TRUE(result.ranInRegion(c.inputColor));
    const ImageDiff diff = compareImages(result.cpu, result.gpu, kInputColorGpuTolerance);
    EXPECT_EQ(0u, diff.mismatches) << "first at float " << diff.firstMismatchIndex;
}

#endif // NATRON_HAVE_GPU
