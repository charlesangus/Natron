/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
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

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "Native/NativeGpuParity.h"
#include "NativeParity.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Transform/LensCorrect.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

// LensCorrect's lenscorrect.slang against its CPU render. The kernel runs lensfun's arithmetic in
// float where the CPU normalises in double, so positions differ by about 1e-5 pixel.

namespace {

const double kTime = 1.;

ParityTolerance
lensCorrectTolerance()
{
    return ParityTolerance::make(2e-4, 1e-4);
}

StagedImageEffect*
effectOf(const NodePtr& node)
{
    return node ? dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get()) : NULL;
}

struct Setup {
    const char* what;
    const char* camera;
    const char* lens;
    double focal;
    const char* mode;
    const char* geometry;
};

const Setup kSetups[] = {
    { "ptlens poly3-tca vignetting", "Canon EOS 5D Mark III", "Canon EF 70-200mm f/2.8L IS II USM", 100., kLensCorrectModeCorrect, kLensCorrectGeometryLens },
    { "ptlens distort", "Canon EOS 5D Mark III", "Canon EF 70-200mm f/2.8L IS II USM", 100., kLensCorrectModeDistort, kLensCorrectGeometryLens },
    { "poly3", "X-Pro2", "XF50mmF2 R WR", 50., kLensCorrectModeCorrect, kLensCorrectGeometryLens },
    { "poly5 fixed lens", "Canon PowerShot G12", "", 0., kLensCorrectModeCorrect, kLensCorrectGeometryLens },
    { "fisheye to rectilinear", "Canon EOS 7D", "Zenitar MC 16mm f/2.8", 16., kLensCorrectModeCorrect, kLensCorrectGeometryRectilinear },
    { "fisheye to equisolid distort", "Canon EOS 7D", "Zenitar MC 16mm f/2.8", 16., kLensCorrectModeDistort, kLensCorrectGeometryEquisolid },
    { "fisheye to thoby", "Canon EOS 7D", "Zenitar MC 16mm f/2.8", 16., kLensCorrectModeCorrect, kLensCorrectGeometryThoby },
};
} // namespace

class NativeLensCorrectGpuTest
    : public BaseTest {
protected:
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeLensCorrectGpu64x48", 1.));
    }

    NodePtr makeSource()
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
        EXPECT_TRUE(bool(source));

        return source;
    }

    NodePtr makeLensCorrect(const NodePtr& source,
                            const Setup& setup)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_LENSCORRECT), PLUGIN_MAJOR_NATRON_LENSCORRECT);
        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        if (source) {
            connectNodes(source, node, kLensCorrectInputSource, true);
        }
        EXPECT_TRUE(setKnobValue(node, kLensCorrectParamCamera, std::string(setup.camera)));
        EXPECT_TRUE(setKnobValue(node, kLensCorrectParamLens, std::string(setup.lens)));
        EXPECT_TRUE(setKnobValues(node, kLensCorrectParamFocalLength, { setup.focal }));
        EXPECT_TRUE(setKnobValues(node, kLensCorrectParamAperture, { 2.8 }));
        EXPECT_TRUE(setKnobValue(node, kLensCorrectParamMode, std::string(setup.mode)));
        EXPECT_TRUE(setKnobValue(node, kLensCorrectParamTargetGeometry, std::string(setup.geometry)));

        return node;
    }

    void expectParity(const NodePtr& source,
                      const NodePtr& node,
                      const std::string& what)
    {
        const RectI window = paritySourceWindow(source, kTime, 0);
        const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ node }), node, window, kTime);

        if (result.skipped) {
            return;
        }
        ASSERT_TRUE(result.ok) << what << ": " << result.error;
        EXPECT_TRUE(result.ranInRegion(node)) << what;
        ASSERT_EQ(result.cpu.size(), result.gpu.size()) << what;
        const ImageDiff diff = compareImages(result.cpu, result.gpu, lensCorrectTolerance());
        EXPECT_EQ(0u, diff.mismatches) << what << ": worst " << diff.worstAbsolute << ", first at float " << diff.firstMismatchIndex;
    }
};

TEST_F(NativeLensCorrectGpuTest, EligibleForFloatImages)
{
#ifdef NATRON_HAVE_GPU
    const bool supported = true;
#else
    const bool supported = false;
#endif
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr node = makeLensCorrect(source, kSetups[0]);
    ASSERT_TRUE(bool(node));
    StagedImageEffect* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);

    for (int nComps : { 1, 2, 3, 4 }) {
        GpuQuery q;
        q.time = kTime;
        q.nComps = nComps;
        q.bitDepth = eImageBitDepthFloat;
        EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << nComps << " components";
    }
    GpuQuery q;
    q.time = kTime;
    q.nComps = 4;
    q.bitDepth = eImageBitDepthHalf;
    EXPECT_FALSE(effect->canRenderOnGpu(q));
}

TEST_F(NativeLensCorrectGpuTest, EveryModelMatchesTheCpu)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    for (const Setup& setup : kSetups) {
        NodePtr node = makeLensCorrect(source, setup);
        ASSERT_TRUE(bool(node)) << setup.what;
        expectParity(source, node, setup.what);
    }
}

TEST_F(NativeLensCorrectGpuTest, SettingsMatchTheCpu)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr node = makeLensCorrect(source, kSetups[0]);
    ASSERT_TRUE(bool(node));

    ASSERT_TRUE(setKnobValues(node, kLensCorrectParamVignetteStrength, { 0.6 }));
    expectParity(source, node, "manual vignette");
    ASSERT_TRUE(setKnobValues(node, kLensCorrectParamShowVignetteMask, { 1. }));
    expectParity(source, node, "vignette mask");
    ASSERT_TRUE(setKnobValues(node, kLensCorrectParamTCAOverride, { 1. }));
    ASSERT_TRUE(setKnobValues(node, kLensCorrectParamTCARed, { 1.004 }));
    ASSERT_TRUE(setKnobValues(node, kLensCorrectParamTCABlue, { 0.997 }));
    expectParity(source, node, "TCA override");
    ASSERT_TRUE(setKnobValues(node, kLensCorrectParamAutoScale, { 0. }));
    ASSERT_TRUE(setKnobValues(node, kLensCorrectParamScale, { 0.8 }));
    expectParity(source, node, "scale 0.8");
    for (const char* filter : { "impulse", "bilinear", "notch" }) {
        ASSERT_TRUE(setKnobValue(node, kResamplerParamFilterType, std::string(filter)));
        expectParity(source, node, std::string("filter ") + filter);
    }
    ASSERT_TRUE(setKnobValue(node, kLensCorrectParamMethod, std::string(kLensCorrectMethodVignette)));
    expectParity(source, node, "manual vignette only");
}
