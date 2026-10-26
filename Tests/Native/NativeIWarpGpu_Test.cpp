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

#include <cstring>
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
#include "Engine/Nodes/Filter/MapInput.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Transform/CornerPin.h"
#include "Engine/Nodes/Transform/ICornerPin.h"
#include "Engine/Nodes/Transform/ITransform.h"
#include "Engine/Nodes/Transform/Transform.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

// The window every case renders: the project format, which is the parity source's.
const RectI kWindow(0, 0, kParitySourceWidth, kParitySourceHeight);

// The resampler's own CPU and GPU bound: the GPU evaluates the matrix curve and the sample point
// in two floats where the CPU uses doubles, and both round the point to float.
const ParityTolerance kWarpTolerance = ParityTolerance::resampling();

GpuQuery
eligibleQuery()
{
    GpuQuery q;
    q.time = kTime;
    q.nComps = 4;
    q.bitDepth = eImageBitDepthFloat;

    return q;
}

bool
setTransformKnobs(const NodePtr& node)
{
    return setKnobValues(node, kTransformNodeParamCenter, { 32., 24. }) && setKnobValues(node, kTransformNodeParamTranslate, { 6.5, -3.25 }) && setKnobValues(node, kTransformNodeParamRotate, { 25. }) && setKnobValues(node, kTransformNodeParamScale, { 0.7, 1.3 }) && setKnobValues(node, kTransformNodeParamSkewX, { 0.2 });
}

bool
setCornerPinKnobs(const NodePtr& node)
{
    const double from[4][2] = { { 0., 0. }, { 64., 0. }, { 64., 48. }, { 0., 48. } };
    const double to[4][2] = { { 6., 3. }, { 58., -4. }, { 70., 50. }, { -3., 44. } };
    const char* const fromNames[4] = { kCornerPinNodeParamFrom1, kCornerPinNodeParamFrom2, kCornerPinNodeParamFrom3, kCornerPinNodeParamFrom4 };
    const char* const toNames[4] = { kCornerPinNodeParamTo1, kCornerPinNodeParamTo2, kCornerPinNodeParamTo3, kCornerPinNodeParamTo4 };

    for (int i = 0; i < 4; ++i) {
        if (!setKnobValues(node, fromNames[i], { from[i][0], from[i][1] }) || !setKnobValues(node, toNames[i], { to[i][0], to[i][1] })) {
            return false;
        }
    }

    return true;
}

} // namespace

class NativeIWarpGpuTest
    : public BaseTest {
protected:
    void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeIWarpGpu64x48", 1.));
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

    // A node of id on source, and on map at the Map input when there is one; the knobs are set
    // after connecting, since connecting a source moves Transform's centre and CornerPin's points.
    NodePtr makeNode(const char* id,
                     int major,
                     const NodePtr& source,
                     const NodePtr& map)
    {
        NodePtr node = createNode(QString::fromUtf8(id), major);

        EXPECT_TRUE(bool(node)) << id;
        if (!node) {
            return node;
        }
        if (source) {
            connectNodes(source, node, kIWarpInputSource, true);
        }
        if (map) {
            connectNodes(map, node, kIWarpInputMap, true);
        }

        return node;
    }

    NodePtr makeITransform(const NodePtr& source,
                           const NodePtr& map)
    {
        NodePtr node = makeNode(PLUGINID_NATRON_ITRANSFORM, PLUGIN_MAJOR_NATRON_ITRANSFORM, source, map);

        EXPECT_TRUE(node && setTransformKnobs(node));

        return node;
    }

    NodePtr makeICornerPin(const NodePtr& source,
                           const NodePtr& map)
    {
        NodePtr node = makeNode(PLUGINID_NATRON_ICORNERPIN, PLUGIN_MAJOR_NATRON_ICORNERPIN, source, map);

        EXPECT_TRUE(node && setCornerPinKnobs(node));

        return node;
    }

    static NativeEffectBase* effectOf(const NodePtr& node)
    {
        return node ? dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get()) : NULL;
    }

    // Renders node on the GPU and on the CPU over the window, with the GPU frame holding `graph`,
    // and expects the two within the tolerance with node run on the device.
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
        const ImageDiff diff = compareImages(result.cpu, result.gpu, kWarpTolerance);
        EXPECT_GT(diff.compared, 0u) << name;
        EXPECT_EQ(0u, diff.mismatches) << name << ": first at float " << diff.firstMismatchIndex << ", worst " << diff.worstAbsolute;
    }
};

// Float images of any width, with or without a mask, and never with motion blur.
TEST_F(NativeIWarpGpuTest, EligibilityFollowsTheSupportedSettings)
{
#ifdef NATRON_HAVE_GPU
    const bool supported = true;
#else
    const bool supported = false;
#endif
    NodePtr source = makeParitySource();
    NodePtr map = makeParitySource();
    NodePtr node = makeITransform(source, map);
    NativeEffectBase* effect = effectOf(node);
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

    ASSERT_TRUE(setKnobValues(node, kTransformNodeParamMotionBlur, { 1. }));
    ASSERT_TRUE(setKnobValues(node, kTransformNodeParamShutter, { 1. }));
    EXPECT_FALSE(effect->canRenderOnGpu(eligibleQuery())) << "motion blur over the shutter";
    ASSERT_TRUE(setKnobValues(node, kTransformNodeParamDirectionalBlur, { 1. }));
    EXPECT_FALSE(effect->canRenderOnGpu(eligibleQuery())) << "directional blur";
    ASSERT_TRUE(setKnobValues(node, kTransformNodeParamMotionBlur, { 0. }));
    ASSERT_TRUE(setKnobValues(node, kTransformNodeParamDirectionalBlur, { 0. }));
    EXPECT_EQ(supported, effect->canRenderOnGpu(eligibleQuery())) << "no motion blur";
}

// The Map is read only while it is connected, over at least the output grown by a pixel for its
// gradient.
TEST_F(NativeIWarpGpuTest, MapIsReadAroundTheOutput)
{
    NodePtr source = makeParitySource();
    NodePtr map = makeParitySource();
    NodePtr node = makeITransform(source, NodePtr());
    NativeEffectBase* effect = effectOf(node);
    ASSERT_TRUE(bool(map) && (effect != NULL));

    const ImageLayerDesc rgba = ImageLayerDesc::getRGBAComponents();
    EXPECT_TRUE(effect->readsGpuInput(kIWarpInputSource, rgba, kTime, ViewIdx(0)));
    EXPECT_FALSE(effect->readsGpuInput(kIWarpInputMap, rgba, kTime, ViewIdx(0))) << "a disconnected Map";

    connectNodes(map, node, kIWarpInputMap, true);
    EXPECT_TRUE(effect->readsGpuInput(kIWarpInputMap, rgba, kTime, ViewIdx(0))) << "a connected Map";

    const RectI rect(10, 20, 31, 47);
    for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
        const RenderScale scale = RenderScale::fromMipmapLevel(mipmapLevel);
        const RectI mapRoI = effect->getGpuInputRoI(kIWarpInputMap, rect, scale);
        EXPECT_TRUE(mapRoI.contains(RectI(rect.x1 - 1, rect.y1 - 1, rect.x2 + 1, rect.y2 + 1))) << "mipmap " << mipmapLevel;
    }
}

// A Map that is another parity source, whose alpha bands give exact 0, exact 1 and a ramp, under
// each of the Map's remapping knobs.
TEST_F(NativeIWarpGpuTest, ConnectedMapMatchesTheCpu)
{
    NodePtr source = makeParitySource();
    NodePtr map = makeParitySource();
    NodePtr node = makeITransform(source, map);
    ASSERT_TRUE(bool(source) && bool(map) && bool(node));

    expectParity(std::vector<NodePtr>({ node }), node, "default");

    ASSERT_TRUE(setKnobValues(node, kMapInputParamInvert, { 1. }));
    expectParity(std::vector<NodePtr>({ node }), node, "inverted");

    ASSERT_TRUE(setKnobValues(node, kMapInputParamInvert, { 0. }));
    ASSERT_TRUE(setKnobValues(node, kMapInputParamGamma, { 2. }));
    ASSERT_TRUE(setKnobValue(node, kMapInputParamResponse, std::string(kMapInputParamResponseSmooth)));
    expectParity(std::vector<NodePtr>({ node }), node, "gamma 2, smooth");

    ASSERT_TRUE(setKnobValues(node, kMapInputParamGamma, { 1. }));
    ASSERT_TRUE(setKnobValue(node, kMapInputParamResponse, std::string(kMapInputParamResponseLinear)));
    ASSERT_TRUE(setKnobValues(node, kTransformNodeParamInvert, { 1. }));
    expectParity(std::vector<NodePtr>({ node }), node, "inverted transform");

    ASSERT_TRUE(setKnobValues(node, kTransformNodeParamInvert, { 0. }));
    ASSERT_TRUE(setKnobValue(node, kResamplerParamFilterType, std::string("impulse")));
    expectParity(std::vector<NodePtr>({ node }), node, "impulse");
}

// Where the Map is exactly 0 the GPU writes the Source pixel itself, bit for bit, with a filter
// whose footprint would otherwise soften it. The Map is a parity source whose alpha bands give
// exact 0, exact 1 and a ramp.
TEST_F(NativeIWarpGpuTest, ZeroMapIsTheSourcePixelUnfiltered)
{
    NodePtr source = makeParitySource();
    NodePtr map = makeParitySource();
    NodePtr itransform = makeITransform(source, map);
    NodePtr icornerpin = makeICornerPin(source, map);
    ASSERT_TRUE(bool(source) && bool(map) && bool(itransform) && bool(icornerpin));
    ASSERT_TRUE(setKnobValue(itransform, kResamplerParamFilterType, std::string("mitchell")));
    ASSERT_TRUE(setKnobValue(icornerpin, kResamplerParamFilterType, std::string("mitchell")));

    const NodePtr nodes[2] = { itransform, icornerpin };
    const char* const names[2] = { "ITransform", "ICornerPin" };
    for (int n = 0; n < 2; ++n) {
        const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ nodes[n] }), nodes[n], kWindow, kTime);
        if (result.skipped) {
            return;
        }
        ASSERT_TRUE(result.ok) << names[n] << ": " << result.error;
        EXPECT_EQ(1u, result.gpuTasksPerNode.count(nodes[n]->getScriptName_mt_safe())) << names[n] << ": the region fell back to the CPU";
        ASSERT_EQ((std::size_t)kWindow.width() * kWindow.height() * 4, result.gpu.size());
        std::size_t zero = 0;
        std::size_t wrong = 0;
        for (int y = kWindow.y1; y < kWindow.y2; ++y) {
            for (int x = kWindow.x1; x < kWindow.x2; ++x) {
                if (paritySourceColorValue(3, x, y, kTime) > 0.f) {
                    continue;
                }
                ++zero;
                const float* got = &result.gpu[((std::size_t)(y - kWindow.y1) * kWindow.width() + (x - kWindow.x1)) * 4];
                for (int c = 0; c < 4; ++c) {
                    const float want = paritySourceColorValue(c, x, y, kTime);
                    if (std::memcmp(&got[c], &want, sizeof(float)) != 0) {
                        ++wrong;
                        break;
                    }
                }
            }
        }
        EXPECT_GT(zero, 0u) << names[n];
        EXPECT_EQ(0u, wrong) << names[n] << ": Map 0 pixels that differ from the Source";
    }
}

// A disconnected Map is white: the full transform everywhere.
TEST_F(NativeIWarpGpuTest, DisconnectedMapMatchesTheCpu)
{
    NodePtr source = makeParitySource();
    NodePtr itransform = makeITransform(source, NodePtr());
    NodePtr icornerpin = makeICornerPin(source, NodePtr());
    ASSERT_TRUE(bool(source) && bool(itransform) && bool(icornerpin));

    expectParity(std::vector<NodePtr>({ itransform }), itransform, "ITransform");
    expectParity(std::vector<NodePtr>({ icornerpin }), icornerpin, "ICornerPin");
}

// A Map white over the right half only, outside its image on the left, which reads as black
// there: the warp tears along the edge.
TEST_F(NativeIWarpGpuTest, MapOutsideItsImageMatchesTheCpu)
{
    const int half = kParitySourceWidth / 2;
    NodePtr source = makeParitySource();
    NodePtr map = makeConstant(RectI(half, 0, kParitySourceWidth, kParitySourceHeight), { 1., 1., 1., 1. });
    NodePtr node = makeITransform(source, map);
    ASSERT_TRUE(bool(source) && bool(map) && bool(node));

    expectParity(std::vector<NodePtr>({ node }), node, "half Map");

    // Inverted, the Map is 0 on the right; on the left, beyond its image, it stays 0, uninverted.
    ASSERT_TRUE(setKnobValues(node, kMapInputParamInvert, { 1. }));
    expectParity(std::vector<NodePtr>({ node }), node, "half Map, inverted");
    NodePtr icornerpin = makeICornerPin(source, map);
    ASSERT_TRUE(bool(icornerpin));
    ASSERT_TRUE(setKnobValues(icornerpin, kMapInputParamInvert, { 1. }));
    expectParity(std::vector<NodePtr>({ icornerpin }), icornerpin, "ICornerPin, half Map, inverted");
}

// ICornerPin's homography blended by the Map, with and without the Black Outside border.
TEST_F(NativeIWarpGpuTest, CornerPinMatchesTheCpu)
{
    NodePtr source = makeParitySource();
    NodePtr map = makeParitySource();
    NodePtr node = makeICornerPin(source, map);
    ASSERT_TRUE(bool(source) && bool(map) && bool(node));

    expectParity(std::vector<NodePtr>({ node }), node, "default");

    ASSERT_TRUE(setKnobValues(node, kCornerPinNodeParamBlackOutside, { 0. }));
    expectParity(std::vector<NodePtr>({ node }), node, "no black outside");
}

// Masked and mixed, the mask another parity source.
TEST_F(NativeIWarpGpuTest, MaskedMatchesTheCpu)
{
    NodePtr source = makeParitySource();
    NodePtr map = makeParitySource();
    NodePtr mask = makeParitySource();
    NodePtr node = makeITransform(source, map);
    ASSERT_TRUE(bool(source) && bool(map) && bool(mask) && bool(node));
    connectNodes(mask, node, kIWarpInputMask, true);
    ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, { 0.5 }));

    expectParity(std::vector<NodePtr>({ node }), node, "mask, mix 0.5");
}
