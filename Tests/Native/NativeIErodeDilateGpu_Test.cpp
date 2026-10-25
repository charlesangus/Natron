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

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/IErodeDilate.h"
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

class NativeIErodeDilateGpuTest
    : public BaseTest {
protected:
    void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeIErodeDilateGpu64x48", 1.));
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

    // An IErode or IDilate of `source` at `size`, its Map `map` when there is one.
    NodePtr makeNode(bool dilate,
                     const NodePtr& source,
                     const NodePtr& map,
                     double sizeX,
                     double sizeY)
    {
        NodePtr node = dilate ? createNode(QString::fromUtf8(PLUGINID_NATRON_IDILATE), PLUGIN_MAJOR_NATRON_IDILATE)
                              : createNode(QString::fromUtf8(PLUGINID_NATRON_IERODE), PLUGIN_MAJOR_NATRON_IERODE);

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        if (source) {
            connectNodes(source, node, kIErodeDilateInputSource, true);
        }
        if (map) {
            connectNodes(map, node, kIErodeDilateInputMap, true);
        }
        EXPECT_TRUE(setKnobValues(node, kIErodeDilateParamSize, { sizeX, sizeY }));

        return node;
    }

    static StagedImageEffect* effectOf(const NodePtr& node)
    {
        return node ? dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get()) : NULL;
    }

    // Renders node on the GPU and on the CPU over the window, with the GPU frame holding `graph`,
    // and expects the two bit for bit with the node run on the device.
    void expectParity(const std::vector<NodePtr>& graph,
                      const NodePtr& node,
                      const std::string& name)
    {
        const EndToEndResult result = renderEndToEnd(graph, node, kWindow, kTime);
        if (result.skipped) {
            return;
        }
        ASSERT_TRUE(result.ok) << name << ": " << result.error;
        EXPECT_TRUE(result.ranInRegion(node)) << name;
        EXPECT_EQ(1u, result.gpuTasksPerNode.count(node->getScriptName_mt_safe())) << name << ": the region fell back to the CPU";

        ASSERT_EQ(result.cpu.size(), result.gpu.size()) << name;
        const ImageDiff diff = compareImages(result.cpu, result.gpu, ParityTolerance::exact());
        EXPECT_GT(diff.compared, 0u) << name;
        EXPECT_EQ(0u, diff.mismatches) << name << ": first at float " << diff.firstMismatchIndex << ", worst " << diff.worstAbsolute;
    }
};

TEST_F(NativeIErodeDilateGpuTest, EligibilityFollowsTheRadius)
{
#ifdef NATRON_HAVE_GPU
    const bool supported = true;
#else
    const bool supported = false;
#endif
    for (bool dilate : { false, true }) {
        NodePtr source = makeParitySource();
        NodePtr map = makeParitySource();
        NodePtr node = makeNode(dilate, source, map, 20., 20.);
        StagedImageEffect* effect = effectOf(node);
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
        // The GPU's cap is a radius of 255 pixels, which either size may reach.
        ASSERT_TRUE(setKnobValues(node, kMapInputParamSizeAtBlack, { 255., 3. }));
        EXPECT_EQ(supported, effect->canRenderOnGpu(eligibleQuery())) << "a radius at the cap";
        ASSERT_TRUE(setKnobValues(node, kMapInputParamSizeAtBlack, { 3., 256. }));
        EXPECT_FALSE(effect->canRenderOnGpu(eligibleQuery())) << "a radius beyond the cap";
    }
}

// The Map is read over the output alone, and only while its channel resolves.
TEST_F(NativeIErodeDilateGpuTest, MapIsReadOverTheOutput)
{
    NodePtr source = makeParitySource();
    NodePtr map = makeParitySource();
    NodePtr node = makeNode(true, source, NodePtr(), 7., 7.);
    StagedImageEffect* effect = effectOf(node);
    ASSERT_TRUE(bool(map) && (effect != NULL));

    const ImageLayerDesc rgba = ImageLayerDesc::getRGBAComponents();
    EXPECT_TRUE(effect->readsGpuInput(kIErodeDilateInputSource, rgba, kTime, ViewIdx(0)));
    EXPECT_FALSE(effect->readsGpuInput(kIErodeDilateInputMap, rgba, kTime, ViewIdx(0))) << "a disconnected Map";

    connectNodes(map, node, kIErodeDilateInputMap, true);
    EXPECT_TRUE(effect->readsGpuInput(kIErodeDilateInputMap, rgba, kTime, ViewIdx(0))) << "a connected Map";

    const RectI rect(10, 20, 31, 47);
    for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
        const RenderScale scale = RenderScale::fromMipmapLevel(mipmapLevel);
        const RectI sourceRoI = effect->getGpuInputRoI(kIErodeDilateInputSource, rect, scale);
        EXPECT_TRUE(sourceRoI.contains(rect)) << "mipmap " << mipmapLevel;
        EXPECT_NE(rect, sourceRoI) << "mipmap " << mipmapLevel << ": the source is read beyond the output";
        EXPECT_EQ(rect, effect->getGpuInputRoI(kIErodeDilateInputMap, rect, scale)) << "mipmap " << mipmapLevel;
    }
}

// A Map that is another parity source, whose alpha bands give exact 0, exact 1 and a ramp, so
// that whole and fractional radii, and both shapes, are compared.
TEST_F(NativeIErodeDilateGpuTest, ConnectedMapMatchesTheCpu)
{
    for (bool dilate : { false, true }) {
        const std::string name = dilate ? "IDilate " : "IErode ";
        NodePtr source = makeParitySource();
        NodePtr map = makeParitySource();
        NodePtr node = makeNode(dilate, source, map, 6.5, 3.);
        ASSERT_TRUE(bool(source) && bool(map) && bool(node));

        expectParity(std::vector<NodePtr>({ node }), node, name + "square");

        ASSERT_TRUE(setKnobValue(node, kIErodeDilateParamShape, std::string(kIErodeDilateParamShapeDisc)));
        expectParity(std::vector<NodePtr>({ node }), node, name + "disc");

        ASSERT_TRUE(setKnobValues(node, kMapInputParamSizeAtBlack, { 2., 4.5 }));
        ASSERT_TRUE(setKnobValues(node, kMapInputParamInvert, { 1. }));
        expectParity(std::vector<NodePtr>({ node }), node, name + "size at black, inverted");

        ASSERT_TRUE(setKnobValues(node, kMapInputParamInvert, { 0. }));
        ASSERT_TRUE(setKnobValues(node, kMapInputParamGamma, { 2. }));
        ASSERT_TRUE(setKnobValue(node, kMapInputParamResponse, std::string(kMapInputParamResponseSmooth)));
        expectParity(std::vector<NodePtr>({ node }), node, name + "gamma 2, smooth");

        ASSERT_TRUE(setKnobValues(node, kMapInputParamGamma, { 0.5 }));
        ASSERT_TRUE(setKnobValue(node, kMapInputParamResponse, std::string(kMapInputParamResponseLinear)));
        expectParity(std::vector<NodePtr>({ node }), node, name + "gamma 0.5, linear");
    }
}

// A disconnected Map is white: the Size everywhere, here larger than the window on one axis.
TEST_F(NativeIErodeDilateGpuTest, DisconnectedMapMatchesTheCpu)
{
    for (bool dilate : { false, true }) {
        NodePtr source = makeParitySource();
        NodePtr node = makeNode(dilate, source, NodePtr(), 9., 60.);
        ASSERT_TRUE(bool(source) && bool(node));

        expectParity(std::vector<NodePtr>({ node }), node, dilate ? "IDilate, no Map" : "IErode, no Map");
    }
}

// A Map white over the right half only, outside its image on the left, which reads as 0 there,
// inverted or not.
TEST_F(NativeIErodeDilateGpuTest, MapOutsideItsImageMatchesTheCpu)
{
    const int half = kParitySourceWidth / 2;
    for (bool dilate : { false, true }) {
        NodePtr source = makeParitySource();
        NodePtr map = makeConstant(RectI(half, 0, kParitySourceWidth, kParitySourceHeight), { 1., 1., 1., 1. });
        NodePtr node = makeNode(dilate, source, map, 5., 5.);
        ASSERT_TRUE(bool(source) && bool(map) && bool(node));
        ASSERT_TRUE(setKnobValue(node, kIErodeDilateParamShape, std::string(kIErodeDilateParamShapeDisc)));

        expectParity(std::vector<NodePtr>({ node }), node, dilate ? "IDilate, half Map" : "IErode, half Map");

        // Inverted, the Map is still 0 beyond its image: Size At Black there, not Size.
        ASSERT_TRUE(setKnobValues(node, kMapInputParamSizeAtBlack, { 2., 1.5 }));
        ASSERT_TRUE(setKnobValues(node, kMapInputParamInvert, { 1. }));
        expectParity(std::vector<NodePtr>({ node }), node, dilate ? "IDilate, half Map, inverted" : "IErode, half Map, inverted");
    }
}
