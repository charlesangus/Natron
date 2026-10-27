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

#include <cmath>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "RawGpuTestSupport.h"

#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Color/ToneEqualizer.h"
#include "Engine/Nodes/Color/ToneEqualizerKernels.h"
#include "Engine/Nodes/IO/RawMetadata.h"

NATRON_NAMESPACE_USING

using namespace ToneEqualizerKernels;

namespace {
// The mask's estimate and its edge-aware filter match the CPU's to the library's 1e-4 (EdgeAwareGpu
// tests); the correction is a smooth function of the mask's log, read from the same table, so it
// moves a little more than the mask: 1e-3 of the level covers both, and the mask display's sqrt.
const ParityTolerance kGpuTolerance = ParityTolerance::make(0., 1e-3);

// Zones round each pixel's exposure to a band: a mask value within rounding of a half-EV boundary
// lands in the neighbouring band on one side, a whole band step apart.
const double kZonesMean = 1e-3;
const double kZonesMax = 1e-3;
const int kZonesOutliers = 16;

const RectI kRoD(0, 0, 72, 56);
// The whole frame, and a window inside it: the mask is still filtered over the whole frame.
const RectI kWindows[] = { kRoD, RectI(11, 9, 50, 40) };

// Exposures from about -9 EV to +1 EV across the frame, with a few hard-edged patches for the
// edge-aware filters to keep, and coloured enough that the estimators disagree. Never zero, so
// that the power norm's ratio is defined.
float
scene(int x,
      int y,
      int c)
{
    const float ev = -9.f + 10.f * (float)x / (float)(kRoD.width() - 1) + 0.6f * std::sin(0.37f * (float)y);
    const bool patch = ((x / 12) + (y / 14)) % 3 == 0;
    const float tint[3] = { 1.15f, 1.f, 0.8f };
    const float level = std::exp2(ev + (patch ? 1.5f : 0.f));

    return level * tint[c] + 1e-4f;
}

struct DetailsCase {
    const char* name;
    DetailsEnum details;
};

const DetailsCase kDetails[] = {
    { "no", eDetailsNone },
    { "averaged guided", eDetailsAveragedGuided },
    { "guided", eDetailsGuided },
    { "averaged eigf", eDetailsAveragedEigf },
    { "eigf", eDetailsEigf },
};
} // namespace

class NativeToneEqualizerGpuTest
    : public RawGpuTest {
protected:
    NodePtr createScene()
    {
        NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));

        EXPECT_TRUE(bool(source));
        if (!source) {
            return source;
        }
        const ImageMetadata metadata = rawTestMetadata(kRawCfaPatternNone, 0, 0, eRawStagePrepared);
        dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(kRoD, 3, scene, metadata);

        return source;
    }

    static void setDouble(const NodePtr& node,
                          const char* name,
                          double value)
    {
        KnobDoublePtr knob = node->getEffectInstance()->getKnobByNameAndType<KnobDouble>(name);
        ASSERT_TRUE(bool(knob)) << name;
        knob->setValue(value);
    }

    static void setInt(const NodePtr& node,
                       const char* name,
                       int value)
    {
        KnobIntPtr knob = node->getEffectInstance()->getKnobByNameAndType<KnobInt>(name);
        ASSERT_TRUE(bool(knob)) << name;
        knob->setValue(value);
    }

    static void setChoice(const NodePtr& node,
                          const char* name,
                          int value)
    {
        KnobChoicePtr knob = node->getEffectInstance()->getKnobByNameAndType<KnobChoice>(name);
        ASSERT_TRUE(bool(knob)) << name;
        knob->setValue(value);
    }

    // A curve that lifts the shadows and pulls the highlights down, as an edit would.
    static void setBands(const NodePtr& node)
    {
        const char* const names[kBands] = {
            kToneEqualizerParamBlacks,
            kToneEqualizerParamDeepShadows,
            kToneEqualizerParamShadows,
            kToneEqualizerParamLightShadows,
            kToneEqualizerParamMidtones,
            kToneEqualizerParamDarkHighlights,
            kToneEqualizerParamHighlights,
            kToneEqualizerParamWhites,
            kToneEqualizerParamSpeculars,
        };
        const double gains[kBands] = { 1.2, 0.9, 0.6, 0.3, 0., -0.2, -0.5, -0.8, -1. };
        for (int b = 0; b < kBands; ++b) {
            setDouble(node, names[b], gains[b]);
        }
    }

    NodePtr createChain(NodePtr* source)
    {
        *source = createScene();
        NodePtr node = createAfter(PLUGINID_NATRON_TONEEQUALIZER, *source);
        if (node) {
            setBands(node);
        }

        return node;
    }
};

#ifdef NATRON_HAVE_GPU

// The corrected image with each detail mode, at the default estimator and filter.
TEST_F(NativeToneEqualizerGpuTest, DetailModesMatchTheCpu)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr source;
    NodePtr node = createChain(&source);
    ASSERT_TRUE(source && node);
    ASSERT_FALSE(HasFatalFailure());
    for (const DetailsCase& d : kDetails) {
        setChoice(node, kToneEqualizerParamPreserveDetails, (int)d.details);
        setDouble(node, kToneEqualizerParamSmoothingDiameter, 10.);
        ASSERT_FALSE(HasFatalFailure());
        for (const RectI& window : kWindows) {
            expectGpuMatchesCpu(*runtime, node, source, window, kGpuTolerance, std::string(d.name) + " details");
        }
    }
}

// Every luminance estimator, with the mask compensations away from their defaults so that the
// contrast fulcrum is exercised.
TEST_F(NativeToneEqualizerGpuTest, EstimatorsMatchTheCpu)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr source;
    NodePtr node = createChain(&source);
    ASSERT_TRUE(source && node);
    setChoice(node, kToneEqualizerParamPreserveDetails, (int)eDetailsGuided);
    setDouble(node, kToneEqualizerParamMaskExposureCompensation, 0.75);
    setDouble(node, kToneEqualizerParamMaskContrastCompensation, -0.4);
    setInt(node, kToneEqualizerParamFilterDiffusion, 2);
    ASSERT_FALSE(HasFatalFailure());
    for (int e = 0; e < (int)eEstimatorCount; ++e) {
        setChoice(node, kToneEqualizerParamLuminanceEstimator, e);
        ASSERT_FALSE(HasFatalFailure());
        expectGpuMatchesCpu(*runtime, node, source, kWindows[1], kGpuTolerance, "estimator " + std::to_string(e));
    }
}

// The mask and zones outputs replace the colour with the mask's display grey.
TEST_F(NativeToneEqualizerGpuTest, MaskOutputsMatchTheCpu)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr source;
    NodePtr node = createChain(&source);
    ASSERT_TRUE(source && node);
    setChoice(node, kToneEqualizerParamOutput, (int)eOutputMask);
    ASSERT_FALSE(HasFatalFailure());
    for (const RectI& window : kWindows) {
        expectGpuMatchesCpu(*runtime, node, source, window, kGpuTolerance, "mask output");
    }
    setChoice(node, kToneEqualizerParamOutput, (int)eOutputZones);
    ASSERT_FALSE(HasFatalFailure());
    for (const RectI& window : kWindows) {
        expectGpuMatchesCpuWithin(*runtime, node, source, window, kZonesMean, kZonesMax, kZonesOutliers, "zones output");
    }
}

// Four-channel input: alpha passes through untouched.
TEST_F(NativeToneEqualizerGpuTest, AlphaPassesThrough)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));
    ASSERT_TRUE(bool(source));
    const ImageMetadata metadata = rawTestMetadata(kRawCfaPatternNone, 0, 0, eRawStagePrepared);
    dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(kRoD, 4, [](int x, int y, int c) { return (c == 3) ? 0.25f + 0.01f * (float)((x + y) % 50) : scene(x, y, c); }, metadata);
    NodePtr node = createAfter(PLUGINID_NATRON_TONEEQUALIZER, source);
    ASSERT_TRUE(bool(node));
    setBands(node);
    ASSERT_FALSE(HasFatalFailure());
    const RenderedPlane cpu = expectGpuMatchesCpu(*runtime, node, source, kWindows[1], kGpuTolerance, "rgba");
    ASSERT_EQ(4u, cpu.channels.size());
    const RectI& w = kWindows[1];
    for (int y = w.y1; y < w.y2; ++y) {
        for (int x = w.x1; x < w.x2; ++x) {
            const std::size_t i = ((std::size_t)(y - w.y1) * w.width() + (x - w.x1)) * 4 + 3;
            ASSERT_LT(i, cpu.pixels.size());
            EXPECT_EQ(0.25f + 0.01f * (float)((x + y) % 50), cpu.pixels[i]);
        }
    }
}

#endif // NATRON_HAVE_GPU
