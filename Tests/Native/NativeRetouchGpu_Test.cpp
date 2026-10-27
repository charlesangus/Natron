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
#include "Native/RetouchTestSupport.h"
#include "NativeParity.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/Retouch.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

const RectI kWindow(0, 0, kParitySourceWidth, kParitySourceHeight);

// The heal is held to 1e-3 (EdgeAwareGpu_Test), the rest to the edge-aware library's 1e-4.
const ParityTolerance kRetouchTolerance = ParityTolerance::make(0., 1e-4);
const ParityTolerance kRetouchHealTolerance = ParityTolerance::make(0., 1e-3);

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

class NativeRetouchGpuTest
    : public BaseTest {
protected:
    void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeRetouchGpu64x48", 1.));
    }

    // A Retouch of the parity source with 3 scales, which the 64 pixel source allows.
    NodePtr makeRetouch()
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);

        EXPECT_TRUE(bool(source));
        if (!source) {
            return NodePtr();
        }
        setParitySourceOrigin(source, 0, 0);
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RETOUCH), PLUGIN_MAJOR_NATRON_RETOUCH);
        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        connectNodes(source, node, 0, true);
        EXPECT_TRUE(setKnobValues(node, kRetouchParamScales, { 3. }));

        return node;
    }

    static Retouch* effectOf(const NodePtr& node)
    {
        return node ? dynamic_cast<Retouch*>(node->getEffectInstance().get()) : NULL;
    }

    // Renders node on the GPU and on the CPU over the window and expects the two within tolerance,
    // with Retouch run on the device.
    void expectParity(const NodePtr& node,
                      const std::string& name,
                      const ParityTolerance& tolerance = kRetouchTolerance)
    {
        const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ node }), node, kWindow, kTime);

        if (result.skipped) {
            return;
        }
        ASSERT_TRUE(result.ok) << name << ": " << result.error;
        EXPECT_TRUE(result.ranInRegion(node)) << name;
        EXPECT_EQ(1u, result.gpuTasksPerNode.count(node->getScriptName_mt_safe())) << name << ": the region fell back to the CPU";
        ASSERT_EQ(result.cpu.size(), result.gpu.size()) << name;
        const ImageDiff diff = compareImages(result.cpu, result.gpu, tolerance);
        EXPECT_GT(diff.compared, 0u) << name;
        EXPECT_EQ(0u, diff.mismatches) << name << ": first at float " << diff.firstMismatchIndex << ", worst " << diff.worstAbsolute;
    }
};

TEST_F(NativeRetouchGpuTest, Eligibility)
{
#ifdef NATRON_HAVE_GPU
    const bool supported = true;
#else
    const bool supported = false;
#endif
    NodePtr node = makeRetouch();
    Retouch* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
    for (int nComps : { 1, 2, 3, 4 }) {
        GpuQuery q = eligibleQuery();
        q.nComps = nComps;
        EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << nComps << " components";
    }
    for (ImageBitDepthEnum depth : { eImageBitDepthByte, eImageBitDepthShort, eImageBitDepthHalf }) {
        GpuQuery q = eligibleQuery();
        q.bitDepth = depth;
        EXPECT_FALSE(effect->canRenderOnGpu(q)) << "bit depth " << (int)depth;
    }

    // The CPU renders the scale and residual previews, merged scales and the bilateral blur. Current
    // Scale on scale 0 is the result.
    ASSERT_TRUE(setKnobValue(node, kRetouchParamOutput, std::string(kRetouchOutputCurrentScale)));
    EXPECT_TRUE(effect->canRenderOnGpu(eligibleQuery()) == supported) << "current scale 0 output";
    ASSERT_TRUE(setKnobValues(node, kRetouchParamCurrentScale, { 2. }));
    EXPECT_FALSE(effect->canRenderOnGpu(eligibleQuery())) << "current scale output";
    ASSERT_TRUE(setKnobValues(node, kRetouchParamCurrentScale, { 0. }));
    ASSERT_TRUE(setKnobValue(node, kRetouchParamOutput, std::string(kRetouchOutputResidual)));
    EXPECT_FALSE(effect->canRenderOnGpu(eligibleQuery())) << "residual output";
    ASSERT_TRUE(setKnobValue(node, kRetouchParamOutput, std::string(kRetouchOutputResult)));
    ASSERT_TRUE(setKnobValues(node, kRetouchParamMergeFrom, { 2. }));
    EXPECT_FALSE(effect->canRenderOnGpu(eligibleQuery())) << "merge from 2";
    ASSERT_TRUE(setKnobValues(node, kRetouchParamMergeFrom, { 0. }));

    RetouchTest::ShapeSpec spec;
    spec.algorithm = RetouchCore::eAlgorithmBlur;
    spec.blurType = RetouchCore::eBlurTypeBilateral;
    ASSERT_TRUE(bool(RetouchTest::makeShape(node, spec)));
    EXPECT_FALSE(effect->canRenderOnGpu(eligibleQuery())) << "bilateral blur";
}

TEST_F(NativeRetouchGpuTest, CloneMatchesTheCpu)
{
    NodePtr node = makeRetouch();
    ASSERT_TRUE(bool(node));
    RetouchTest::ShapeSpec spec;
    spec.algorithm = RetouchCore::eAlgorithmClone;
    spec.sourceX = 16.;
    spec.sourceY = 6.;
    ASSERT_TRUE(bool(RetouchTest::makeShape(node, spec)));
    expectParity(node, "clone on the image");
    spec.cx = 44.;
    spec.scale = 2;
    spec.sourceX = -20.;
    spec.sourceY = -4.;
    spec.opacity = 0.6;
    spec.feather = 3.;
    ASSERT_TRUE(bool(RetouchTest::makeShape(node, spec)));
    expectParity(node, "clone on scale 2, opacity 0.6, feather 3");
}

TEST_F(NativeRetouchGpuTest, HealMatchesTheCpu)
{
    NodePtr node = makeRetouch();
    ASSERT_TRUE(bool(node));
    // Within the iterations a device without push descriptors records.
    ASSERT_TRUE(setKnobValues(node, kRetouchParamMaxHealIterations, { 400. }));
    RetouchTest::ShapeSpec spec;
    spec.algorithm = RetouchCore::eAlgorithmHeal;
    spec.scale = 1;
    spec.sourceX = 14.;
    spec.sourceY = 8.;
    ASSERT_TRUE(bool(RetouchTest::makeShape(node, spec)));
    expectParity(node, "heal on scale 1", kRetouchHealTolerance);
    spec.cx = 42.;
    spec.scale = 0;
    spec.sourceX = -16.;
    spec.sourceY = 0.;
    spec.feather = 2.;
    ASSERT_TRUE(bool(RetouchTest::makeShape(node, spec)));
    expectParity(node, "heal on the image, feather 2", kRetouchHealTolerance);
}

TEST_F(NativeRetouchGpuTest, BlurMatchesTheCpu)
{
    NodePtr node = makeRetouch();
    ASSERT_TRUE(bool(node));
    RetouchTest::ShapeSpec spec;
    spec.algorithm = RetouchCore::eAlgorithmBlur;
    spec.blurRadius = 4.;
    spec.scale = 0;
    ASSERT_TRUE(bool(RetouchTest::makeShape(node, spec)));
    expectParity(node, "gaussian blur on the image");
    spec.cx = 44.;
    spec.scale = 2;
    spec.opacity = 0.5;
    ASSERT_TRUE(bool(RetouchTest::makeShape(node, spec)));
    expectParity(node, "gaussian blur on scale 2, opacity 0.5");
}

TEST_F(NativeRetouchGpuTest, FillMatchesTheCpu)
{
    NodePtr node = makeRetouch();
    ASSERT_TRUE(bool(node));
    RetouchTest::ShapeSpec spec;
    spec.algorithm = RetouchCore::eAlgorithmFill;
    spec.fillMode = RetouchCore::eFillModeColor;
    spec.fillColor[0] = 0.8;
    spec.fillColor[1] = 0.2;
    spec.fillColor[2] = 0.1;
    spec.fillBrightness = 0.1;
    ASSERT_TRUE(bool(RetouchTest::makeShape(node, spec)));
    expectParity(node, "colour fill on the image");
    spec.cx = 44.;
    spec.fillMode = RetouchCore::eFillModeErase;
    spec.scale = 4;
    spec.opacity = 0.7;
    ASSERT_TRUE(bool(RetouchTest::makeShape(node, spec)));
    expectParity(node, "erase on the residual, opacity 0.7");
    ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, { 0.5 }));
    expectParity(node, "mix 0.5");
}
