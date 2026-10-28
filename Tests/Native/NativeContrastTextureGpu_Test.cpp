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

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "Native/NativeGpuParity.h"
#include "NativeParity.h"
#include "RegionIndependenceSupport.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/ContrastTexture.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

const RectI kWindow(0, 0, kParitySourceWidth, kParitySourceHeight);

// 1e-4 relative to max(1, |CPU value|). The EIGF blurs hold it on their own (EdgeAwareGpu_Test);
// the logs and the final exp2 add float roundings only.
const ParityTolerance kContrastTextureTolerance = ParityTolerance::make(0., 1e-4);

GpuQuery
eligibleQuery()
{
    GpuQuery q;

    q.time = kTime;
    q.nComps = 4;
    q.bitDepth = eImageBitDepthFloat;

    return q;
}

} // namespace

class NativeContrastTextureGpuTest
    : public BaseTest {
protected:
    void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeContrastTextureGpu64x48", 1.));
    }

    NodePtr makeParitySource()
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);

        EXPECT_TRUE(bool(source));
        if (source) {
            setParitySourceOrigin(source, 0, 0);
        }

        return source;
    }

    // A ContrastTexture of source at base detail level 2: the 64 pixel parity source is too
    // small for the default 5, which leaves no level between the coarsest and the finest.
    NodePtr makeContrastTexture(const NodePtr& source)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_CONTRASTTEXTURE), PLUGIN_MAJOR_NATRON_CONTRASTTEXTURE);

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        if (source) {
            connectNodes(source, node, 0, true);
        }
        EXPECT_TRUE(setKnobValues(node, kContrastTextureParamDetailLevel, { 2. }));

        return node;
    }

    static ContrastTexture* effectOf(const NodePtr& node)
    {
        return node ? dynamic_cast<ContrastTexture*>(node->getEffectInstance().get()) : NULL;
    }

    // Renders node on the GPU and on the CPU over the window and expects the two within the
    // tolerance, with ContrastTexture run on the device.
    void expectParity(const NodePtr& node,
                      const std::string& name)
    {
        const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ node }), node, kWindow, kTime);

        if (result.skipped) {
            return;
        }
        ASSERT_TRUE(result.ok) << name << ": " << result.error;
        EXPECT_TRUE(result.ranInRegion(node)) << name;
        EXPECT_EQ(1u, result.gpuTasksPerNode.count(node->getScriptName_mt_safe())) << name << ": the region fell back to the CPU";
        ASSERT_EQ(result.cpu.size(), result.gpu.size()) << name;
        const ImageDiff diff = compareImages(result.cpu, result.gpu, kContrastTextureTolerance);
        EXPECT_GT(diff.compared, 0u) << name;
        EXPECT_EQ(0u, diff.mismatches) << name << ": first at float " << diff.firstMismatchIndex << ", worst " << diff.worstAbsolute;
    }
};

TEST_F(NativeContrastTextureGpuTest, Eligibility)
{
#ifdef NATRON_HAVE_GPU
    const bool supported = true;
#else
    const bool supported = false;
#endif
    NodePtr source = makeParitySource();
    NodePtr node = makeContrastTexture(source);
    ContrastTexture* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
    for (int nComps : { 1, 2, 3, 4 }) {
        GpuQuery q = eligibleQuery();
        q.nComps = nComps;
        EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << nComps << " components";
    }
    {
        GpuQuery q = eligibleQuery();
        q.hasMask = true;
        EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << "mask";
    }
    for (ImageBitDepthEnum depth : { eImageBitDepthByte, eImageBitDepthShort, eImageBitDepthHalf }) {
        GpuQuery q = eligibleQuery();
        q.bitDepth = depth;
        EXPECT_FALSE(effect->canRenderOnGpu(q)) << "bit depth " << (int)depth;
    }
}

// The three detail bands, alone and together, through the whole pyramid.
TEST_F(NativeContrastTextureGpuTest, LocalContrastMatchesTheCpu)
{
    NodePtr source = makeParitySource();
    NodePtr node = makeContrastTexture(source);
    ASSERT_TRUE(bool(source) && bool(node));
    ASSERT_TRUE(setKnobValues(node, kContrastTextureParamCoarse, { 1. }));
    expectParity(node, "coarse 1");
    ASSERT_TRUE(setKnobValues(node, kContrastTextureParamCoarse, { 0. }));
    ASSERT_TRUE(setKnobValues(node, kContrastTextureParamMedium, { -0.5 }));
    expectParity(node, "medium -0.5");
    ASSERT_TRUE(setKnobValues(node, kContrastTextureParamMedium, { 0. }));
    ASSERT_TRUE(setKnobValues(node, kContrastTextureParamFine, { 3. }));
    expectParity(node, "fine 3");
    ASSERT_TRUE(setKnobValues(node, kContrastTextureParamCoarse, { 0.4 }));
    ASSERT_TRUE(setKnobValues(node, kContrastTextureParamMedium, { 0.6 }));
    ASSERT_TRUE(setKnobValues(node, kContrastTextureParamFine, { 0.8 }));
    ASSERT_TRUE(setKnobValues(node, kContrastTextureParamDetailLevel, { 1. }));
    expectParity(node, "all bands, detail level 1");
}

// Shadows and highlights, and the filter settings.
TEST_F(NativeContrastTextureGpuTest, ToneAndFilterSettingsMatchTheCpu)
{
    NodePtr source = makeParitySource();
    NodePtr node = makeContrastTexture(source);
    ASSERT_TRUE(bool(source) && bool(node));
    ASSERT_TRUE(setKnobValues(node, kContrastTextureParamShadows, { 1. }));
    ASSERT_TRUE(setKnobValues(node, kContrastTextureParamHighlights, { -1. }));
    expectParity(node, "shadows 1, highlights -1");
    ASSERT_TRUE(setKnobValues(node, kContrastTextureParamFine, { 1. }));
    ASSERT_TRUE(setKnobValues(node, kContrastTextureParamHaloControl, { -3. }));
    ASSERT_TRUE(setKnobValues(node, kContrastTextureParamNoiseBias, { 0.05 }));
    expectParity(node, "fine 1, halo -3, noise bias 0.05");
    ASSERT_TRUE(setKnobValues(node, kContrastTextureParamHaloControl, { 3. }));
    ASSERT_TRUE(setKnobValues(node, kContrastTextureParamNoiseBias, { 0. }));
    ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, { 0.5 }));
    expectParity(node, "fine 1, halo 3, noise bias 0, mix 0.5");
}

// Every mask display preview.
TEST_F(NativeContrastTextureGpuTest, MaskDisplayMatchesTheCpu)
{
    NodePtr source = makeParitySource();
    NodePtr node = makeContrastTexture(source);
    ASSERT_TRUE(bool(source) && bool(node));
    for (const char* display : { kContrastTextureMaskDisplayCoarse, kContrastTextureMaskDisplayMedium, kContrastTextureMaskDisplayFine,
                                 kContrastTextureMaskDisplayLowpass }) {
        ASSERT_TRUE(setKnobValue(node, kContrastTextureParamMaskDisplay, std::string(display)));
        expectParity(node, std::string("mask display ") + display);
    }
}

namespace {

class ContrastTextureGpuRegionTest
    : public RegionIndependenceTest {
protected:
    NodePtr makeContrastTexture(const NodePtr& source,
                                const std::vector<std::pair<const char*, double>>& knobs)
    {
        NodePtr node = createAfter(PLUGINID_NATRON_CONTRASTTEXTURE, source);

        if (!node) {
            return node;
        }
        EXPECT_TRUE(setKnobValues(node, kContrastTextureParamCoarse, { 0.4 }));
        EXPECT_TRUE(setKnobValues(node, kContrastTextureParamMedium, { 0.6 }));
        EXPECT_TRUE(setKnobValues(node, kContrastTextureParamFine, { 0.8 }));
        for (const auto& knob : knobs) {
            EXPECT_TRUE(setKnobValues(node, knob.first, { knob.second })) << knob.first;
        }

        return node;
    }
};

// A frame too large for one device buffer is refused before anything is recorded.
TEST_F(ContrastTextureGpuRegionTest, OversizedFrameStaysOnTheCpu)
{
    NodePtr source = createRegionSource(RectI(0, 0, 70000, 70000), false);
    NodePtr node = makeContrastTexture(source, {});
    ContrastTexture* effect = node ? dynamic_cast<ContrastTexture*>(node->getEffectInstance().get()) : NULL;
    ASSERT_TRUE(effect != NULL);
    GpuQuery q = eligibleQuery();
    q.mappedScale = RenderScale::identity;
    EXPECT_FALSE(effect->canRenderOnGpu(q));
}

#ifdef NATRON_HAVE_GPU
TEST_F(ContrastTextureGpuRegionTest, RegionIndependentGpu)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    RegionIndependenceOptions options;
    options.gpu = runtime.get();
    options.gpuTolerance = kContrastTextureTolerance;
    const std::vector<std::vector<std::pair<const char*, double>>> settings = {
        {},
        { { kContrastTextureParamDetailLevel, 2. }, { kContrastTextureParamHaloControl, 1. }, { kContrastTextureParamNoiseBias, 0.01 }, { kContrastTextureParamShadows, 0.5 }, { kContrastTextureParamHighlights, -0.5 } },
        { { kContrastTextureParamDetailLevel, 1. }, { kContrastTextureParamHaloControl, -3. } },
    };
    for (const auto& knobs : settings) {
        expectRegionIndependent([this, knobs](const NodePtr& source) {
            return makeContrastTexture(source, knobs);
        },
                                options);
    }
}
#endif

} // namespace
