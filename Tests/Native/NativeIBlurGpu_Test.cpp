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
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Filter/IBlur.h"
#include "Engine/Nodes/Filter/MapInput.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

// The window every case renders: the project format, which is the parity source's.
const RectI kWindow(0, 0, kParitySourceWidth, kParitySourceHeight);

// The CPU/GPU parity bound. The GPU sums each plane exactly in 64-bit integers and weights the box
// sums in two-float arithmetic, so the output is within a few float roundings of the CPU's double
// sums; the radii, computed in float, add errors of the order of 1e-7.
const ParityTolerance kIBlurTolerance = ParityTolerance::make(0., 1e-4);

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

class NativeIBlurGpuTest
    : public BaseTest {
protected:
    void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeIBlurGpu64x48", 1.));
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

    // A Constant of `color` over the canonical rectangle `extent`.
    NodePtr makeConstant(const RectI& extent,
                         const std::vector<double>& color)
    {
        NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), PLUGIN_MAJOR_NATRON_CONSTANT);

        EXPECT_TRUE(bool(constant));
        if (constant) {
            EXPECT_TRUE(setKnobValue(constant, kNativeGeneratorParamExtent, std::string(kNativeGeneratorExtentSize)));
            EXPECT_TRUE(setKnobValues(constant, kNativeGeneratorParamBottomLeft, { (double)extent.x1, (double)extent.y1 }));
            EXPECT_TRUE(setKnobValues(constant, kNativeGeneratorParamSize, { (double)extent.width(), (double)extent.height() }));
            EXPECT_TRUE(setKnobValues(constant, kConstantParamColor, color));
        }

        return constant;
    }

    // An uncropped IBlur of `source` at `size`, its Map `map` when there is one.
    NodePtr makeIBlur(const NodePtr& source,
                      const NodePtr& map,
                      double size)
    {
        NodePtr iblur = createNode(QString::fromUtf8(PLUGINID_NATRON_IBLUR), PLUGIN_MAJOR_NATRON_IBLUR);

        EXPECT_TRUE(bool(iblur));
        if (!iblur) {
            return iblur;
        }
        if (source) {
            connectNodes(source, iblur, kIBlurInputSource, true);
        }
        if (map) {
            connectNodes(map, iblur, kIBlurInputMap, true);
        }
        EXPECT_TRUE(setKnobValues(iblur, kIBlurParamCropToFormat, { 0. }));
        EXPECT_TRUE(setKnobValues(iblur, kIBlurParamSize, { size, size }));

        return iblur;
    }

    static IBlur* effectOf(const NodePtr& node)
    {
        return node ? dynamic_cast<IBlur*>(node->getEffectInstance().get()) : NULL;
    }

    // Renders iblur on the GPU and on the CPU over the window, with the GPU frame holding `graph`,
    // and expects the two within the tolerance with IBlur run on the device.
    void expectParity(const std::vector<NodePtr>& graph,
                      const NodePtr& iblur,
                      const ParityTolerance& tolerance,
                      const std::string& name)
    {
        const EndToEndResult result = renderEndToEnd(graph, iblur, kWindow, kTime);
        if (result.skipped) {
            return;
        }
        ASSERT_TRUE(result.ok) << name << ": " << result.error;
        EXPECT_TRUE(result.ranInRegion(iblur)) << name;
        EXPECT_EQ(1u, result.gpuTasksPerNode.count(iblur->getScriptName_mt_safe())) << name << ": the region fell back to the CPU";

        ASSERT_EQ(result.cpu.size(), result.gpu.size()) << name;
        const ImageDiff diff = compareImages(result.cpu, result.gpu, tolerance);
        EXPECT_GT(diff.compared, 0u) << name;
        EXPECT_EQ(0u, diff.mismatches) << name << ": first at float " << diff.firstMismatchIndex << ", worst " << diff.worstAbsolute;
    }
};

TEST_F(NativeIBlurGpuTest, EligibilityFollowsTheBoxSize)
{
#ifdef NATRON_HAVE_GPU
    const bool supported = true;
#else
    const bool supported = false;
#endif
    NodePtr source = makeParitySource();
    NodePtr map = makeParitySource();
    NodePtr iblur = makeIBlur(source, map, 20.);
    IBlur* effect = effectOf(iblur);
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
    // Size tops out at 1000 (a box radius near 240), so the radius is pushed past the GPU's cap of
    // 512 pixels through Size At Black, which has no maximum: 1000 gives a radius near 240, and
    // 3000 one near 720 (a standard deviation of size / 2.4, a box radius of about 0.577 of it).
    ASSERT_TRUE(setKnobValues(iblur, kMapInputParamSizeAtBlack, { 1000., 1000. }));
    EXPECT_EQ(supported, effect->canRenderOnGpu(eligibleQuery())) << "a box radius below the cap";
    ASSERT_TRUE(setKnobValues(iblur, kMapInputParamSizeAtBlack, { 3000., 3000. }));
    EXPECT_FALSE(effect->canRenderOnGpu(eligibleQuery())) << "a box radius beyond the cap";
}

// The Map is read over the source's region of interest, and only while its channel resolves.
TEST_F(NativeIBlurGpuTest, MapIsReadOverTheSourceRegion)
{
    NodePtr source = makeParitySource();
    NodePtr map = makeParitySource();
    NodePtr iblur = makeIBlur(source, NodePtr(), 14.);
    IBlur* effect = effectOf(iblur);
    ASSERT_TRUE(bool(map) && (effect != NULL));

    const ImageLayerDesc rgba = ImageLayerDesc::getRGBAComponents();
    EXPECT_TRUE(effect->readsGpuInput(kIBlurInputSource, rgba, kTime, ViewIdx(0)));
    EXPECT_FALSE(effect->readsGpuInput(kIBlurInputMap, rgba, kTime, ViewIdx(0))) << "a disconnected Map";

    connectNodes(map, iblur, kIBlurInputMap, true);
    EXPECT_TRUE(effect->readsGpuInput(kIBlurInputMap, rgba, kTime, ViewIdx(0))) << "a connected Map";

    const RectI rect(10, 20, 31, 47);
    for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
        const RenderScale scale = RenderScale::fromMipmapLevel(mipmapLevel);
        const RectI sourceRoI = effect->getGpuInputRoI(kIBlurInputSource, kTime, ViewIdx(0), rect, scale);
        EXPECT_TRUE(sourceRoI.contains(rect)) << "mipmap " << mipmapLevel;
        EXPECT_NE(rect, sourceRoI) << "mipmap " << mipmapLevel << ": the source is read beyond the output";
        EXPECT_EQ(sourceRoI, effect->getGpuInputRoI(kIBlurInputMap, kTime, ViewIdx(0), rect, scale)) << "mipmap " << mipmapLevel;
    }
}

// A Map that is another parity source, whose alpha bands give exact 0, exact 1 and a ramp.
TEST_F(NativeIBlurGpuTest, ConnectedMapMatchesTheCpu)
{
    NodePtr source = makeParitySource();
    NodePtr map = makeParitySource();
    NodePtr iblur = makeIBlur(source, map, 9.);
    ASSERT_TRUE(bool(source) && bool(map) && bool(iblur));
    ASSERT_TRUE(setKnobValues(iblur, kIBlurParamSize, { 9., 5. }));

    expectParity(std::vector<NodePtr>({ iblur }), iblur, kIBlurTolerance, "default");

    ASSERT_TRUE(setKnobValues(iblur, kMapInputParamSizeAtBlack, { 3., 2. }));
    ASSERT_TRUE(setKnobValues(iblur, kMapInputParamInvert, { 1. }));
    expectParity(std::vector<NodePtr>({ iblur }), iblur, kIBlurTolerance, "size at black, inverted");

    ASSERT_TRUE(setKnobValues(iblur, kMapInputParamInvert, { 0. }));
    ASSERT_TRUE(setKnobValues(iblur, kMapInputParamGamma, { 2. }));
    ASSERT_TRUE(setKnobValue(iblur, kMapInputParamResponse, std::string(kMapInputParamResponseSmooth)));
    expectParity(std::vector<NodePtr>({ iblur }), iblur, kIBlurTolerance, "gamma 2, smooth");

    ASSERT_TRUE(setKnobValues(iblur, kMapInputParamGamma, { 0.5 }));
    ASSERT_TRUE(setKnobValue(iblur, kMapInputParamResponse, std::string(kMapInputParamResponseLinear)));
    expectParity(std::vector<NodePtr>({ iblur }), iblur, kIBlurTolerance, "gamma 0.5, linear");
}

// A disconnected Map is white: the Size everywhere.
TEST_F(NativeIBlurGpuTest, DisconnectedMapMatchesTheCpu)
{
    NodePtr source = makeParitySource();
    NodePtr iblur = makeIBlur(source, NodePtr(), 12.);
    ASSERT_TRUE(bool(source) && bool(iblur));

    expectParity(std::vector<NodePtr>({ iblur }), iblur, kIBlurTolerance, "disconnected Map");
}

// A Map white over the right half only, outside its image on the left, which reads as 0 there,
// inverted or not.
TEST_F(NativeIBlurGpuTest, MapOutsideItsImageMatchesTheCpu)
{
    const int half = kParitySourceWidth / 2;
    NodePtr source = makeParitySource();
    NodePtr map = makeConstant(RectI(half, 0, kParitySourceWidth, kParitySourceHeight), { 1., 1., 1., 1. });
    NodePtr iblur = makeIBlur(source, map, 10.);
    ASSERT_TRUE(bool(source) && bool(map) && bool(iblur));

    expectParity(std::vector<NodePtr>({ iblur }), iblur, kIBlurTolerance, "half Map");

    // Inverted, the Map is still 0 beyond its image: Size At Black there, not Size.
    ASSERT_TRUE(setKnobValues(iblur, kMapInputParamSizeAtBlack, { 3., 2. }));
    ASSERT_TRUE(setKnobValues(iblur, kMapInputParamInvert, { 1. }));
    expectParity(std::vector<NodePtr>({ iblur }), iblur, kIBlurTolerance, "half Map, inverted");
}

// NaN, +Inf and -Inf in the source count as 0 in both summed-area tables, so the CPU and the GPU
// agree, and the blur is finite throughout.
TEST_F(NativeIBlurGpuTest, NonFiniteSourceMatchesTheCpu)
{
    NodePtr source = makeParitySource();
    ASSERT_TRUE(bool(source));
    setParitySourceNonFinite(source, RectI(10, 8, 22, 18));
    NodePtr iblur = makeIBlur(source, NodePtr(), 6.);
    ASSERT_TRUE(bool(iblur));

    const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ iblur }), iblur, kWindow, kTime);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(1u, result.gpuTasksPerNode.count(iblur->getScriptName_mt_safe())) << "the region fell back to the CPU";
    ASSERT_EQ(result.cpu.size(), result.gpu.size());
    std::size_t nonFinite = 0;
    for (float v : result.cpu) {
        if (!std::isfinite(v)) {
            ++nonFinite;
        }
    }
    EXPECT_EQ(0u, nonFinite);
    const ImageDiff diff = compareImages(result.cpu, result.gpu, kIBlurTolerance);
    EXPECT_GT(diff.compared, 0u);
    EXPECT_EQ(0u, diff.mismatches) << "first at float " << diff.firstMismatchIndex << ", worst " << diff.worstAbsolute;
}

// Detail over a large offset, in one region with the Grade before it: a 32-bit float summed-area
// table would lose the detail to the offset by far more than the bound, which allows a few
// roundings of the output's floats.
TEST_F(NativeIBlurGpuTest, DetailOverALargeOffsetMatchesTheCpu)
{
    NodePtr source = makeParitySource();
    NodePtr grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    NodePtr map = makeParitySource();
    ASSERT_TRUE(bool(source) && bool(grade) && bool(map));
    connectNodes(source, grade, 0, true);
    ASSERT_TRUE(setKnobValues(grade, "offset", std::vector<double>(4, 1000.)));
    NodePtr iblur = makeIBlur(grade, map, 24.);
    ASSERT_TRUE(bool(iblur));

    expectParity(std::vector<NodePtr>({ grade, iblur }), iblur, ParityTolerance::make(5e-4, 1e-6), "offset 1000");
}
