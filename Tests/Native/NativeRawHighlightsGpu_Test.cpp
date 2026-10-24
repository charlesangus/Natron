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

#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "RawGpuTestSupport.h"

#include "Engine/KnobTypes.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Raw/RawHighlights.h"

NATRON_NAMESPACE_USING

namespace {
// Clip is a comparison and a copy on both sides.
const ParityTolerance kClipGpuTolerance = ParityTolerance::exact();
// LCh takes a square root and divides, which Vulkan rounds within a few ulp.
const ParityTolerance kLChGpuTolerance = ParityTolerance::transcendental();

const RectI kRoD(0, 0, 64, 48);
// The whole source, and a window inside it whose LCh reads cross its edges.
const RectI kWindows[] = { kRoD, RectI(10, 7, 41, 30) };

const char* const kXTrans = "GGRGGBGGBGGRBRGRBGGGBGGRGGRGGBRBGBRG";

// A ramp under a bump that rises past the clipping threshold by a different amount per colour, so
// that some photosites of a block clip and others do not.
float
clippedPatch(int x,
             int y,
             int colour)
{
    const float gains[3] = { 1.1f, 1.45f, 0.85f };
    const float base = 0.2f + 0.006f * (float)x + 0.004f * (float)y;
    const float dx = (float)(x - 30);
    const float dy = (float)(y - 22);
    const float r2 = dx * dx + dy * dy;
    const float bump = (r2 < 160.f) ? (1.2f - r2 / 400.f) : 0.f;

    return base + bump * gains[(colour >= 0) && (colour < 3) ? colour : 1];
}
} // namespace

class NativeRawHighlightsGpuTest
    : public RawGpuTest {
protected:
    // A prepared source: a mosaic of pattern, or three-channel linear data when pattern is empty.
    NodePtr createPatch(const std::string& pattern)
    {
        NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));

        EXPECT_TRUE(bool(source));
        if (!source) {
            return source;
        }
        ImageMetadata metadata = rawTestMetadata(pattern.empty() ? std::string(kRawCfaPatternNone) : pattern, 0, 0, eRawStagePrepared);
        RawMetadata::setWhiteLevel(&metadata, 1.);
        RawMetadata::setWbApplied(&metadata, { 2.1, 1., 1.6 });
        RawMetadata::setProcessedMaximum(&metadata, { 2.1, 1., 1.6 });
        const std::optional<RawLayout> layout = RawLayout::fromPattern(pattern.empty() ? std::string(kRawCfaPatternNone) : pattern);
        const RawLayout cfa = layout.value_or(RawLayout());
        const bool mosaic = cfa.hasCfa();
        RawTestSource::ValueFn value = [cfa, mosaic](int x, int y, int c) {
            return clippedPatch(x, y, mosaic ? (int)cfa.colourAt(x, y) : c);
        };
        dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(kRoD, mosaic ? 1 : 3, value, metadata);

        return source;
    }

    void setMode(const NodePtr& node,
                 RawHighlightsModeEnum mode)
    {
        KnobChoicePtr knob = node->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawHighlightsParamMode);
        ASSERT_TRUE(bool(knob));
        knob->setValue((int)mode);
    }
};

#ifdef NATRON_HAVE_GPU

// Clip Highlights on Bayer and X-Trans mosaics and on linear data, at two thresholds.
TEST_F(NativeRawHighlightsGpuTest, ClipMatchesTheCpu)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    for (const std::string& pattern : { std::string("RGGB"), std::string(kXTrans), std::string() }) {
        NodePtr source = createPatch(pattern);
        NodePtr highlights = createAfter(PLUGINID_NATRON_RAWHIGHLIGHTS, source);
        ASSERT_TRUE(source && highlights);
        setMode(highlights, eRawHighlightsModeClip);
        ASSERT_FALSE(HasFatalFailure());
        for (double clip : { 1., 0.8 }) {
            highlights->getEffectInstance()->getKnobByNameAndType<KnobDouble>(kRawHighlightsParamClip)->setValue(clip);
            for (const RectI& window : kWindows) {
                expectGpuMatchesCpu(*runtime, highlights, source, window, kClipGpuTolerance,
                                    std::string("clip ") + std::to_string(clip) + ", " + (pattern.empty() ? std::string("linear") : pattern));
            }
        }
    }
}

// Reconstruct in LCh on Bayer and X-Trans mosaics, over the whole patch and over a window whose
// neighbourhood reads cross its edges.
TEST_F(NativeRawHighlightsGpuTest, LChMatchesTheCpu)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    for (const std::string& pattern : { std::string("RGGB"), std::string("GBRG"), std::string(kXTrans) }) {
        NodePtr source = createPatch(pattern);
        NodePtr highlights = createAfter(PLUGINID_NATRON_RAWHIGHLIGHTS, source);
        ASSERT_TRUE(source && highlights);
        setMode(highlights, eRawHighlightsModeLCh);
        ASSERT_FALSE(HasFatalFailure());
        for (const RectI& window : kWindows) {
            expectGpuMatchesCpu(*runtime, highlights, source, window, kLChGpuTolerance, std::string("lch, ") + pattern);
        }
    }
}

// Inpaint Opposed measures the whole frame on the CPU, and LCh without a CFA is Inpaint Opposed:
// neither offers a GPU path.
TEST_F(NativeRawHighlightsGpuTest, OpposedStaysOnTheCpu)
{
    NodePtr mosaic = createPatch("RGGB");
    NodePtr linear = createPatch(std::string());
    NodePtr onMosaic = createAfter(PLUGINID_NATRON_RAWHIGHLIGHTS, mosaic);
    NodePtr onLinear = createAfter(PLUGINID_NATRON_RAWHIGHLIGHTS, linear);
    ASSERT_TRUE(mosaic && linear && onMosaic && onLinear);

    setMode(onMosaic, eRawHighlightsModeOpposed);
    setMode(onLinear, eRawHighlightsModeLCh);
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_FALSE(stagedOf(onMosaic)->canRenderOnGpu(queryFor(1)));
    EXPECT_FALSE(stagedOf(onLinear)->canRenderOnGpu(queryFor(3)));
}

#endif // NATRON_HAVE_GPU
