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
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/VectorBlur.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

StagedImageEffect*
effectOf(const NodePtr& node)
{
    return node ? dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get()) : NULL;
}

GpuQuery
eligibleQuery()
{
    GpuQuery q;
    q.time = kTime;
    q.nComps = 4;
    q.bitDepth = eImageBitDepthFloat;

    return q;
}

// Both sides run the same float arithmetic; the GPU may fuse multiply-adds.
ParityTolerance
tolerance()
{
    return ParityTolerance::make(1e-5, 1e-4);
}

bool
setUVLayer(const NodePtr& node,
           const std::string& layerID,
           const std::vector<std::string>& channels)
{
    KnobLayerSelect* uv = node ? dynamic_cast<KnobLayerSelect*>(node->getKnobByName(kVectorBlurParamUV).get()) : NULL;

    EXPECT_TRUE(uv != NULL);
    if (!uv) {
        return false;
    }
    uv->setLayer(layerID);
    uv->setChannels(channels);

    return true;
}

bool
setDepthChannel(const NodePtr& node,
                const std::string& value)
{
    KnobChannelSelect* depth = node ? dynamic_cast<KnobChannelSelect*>(node->getKnobByName(kVectorBlurParamDepth).get()) : NULL;

    EXPECT_TRUE(depth != NULL);
    if (!depth) {
        return false;
    }
    depth->set(value);

    return true;
}

} // namespace

class NativeVectorBlurGpuTest
    : public BaseTest {
protected:
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeVectorBlurGpu64x48", 1.));
    }

    // A noisy parity source with its "spec" layer.
    NodePtr makeSource()
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
        EXPECT_TRUE(bool(source));
        if (source) {
            setParitySourceNoise(source, true);
            setParitySourceExtraLayer(source, true);
        }

        return source;
    }

    // A VectorBlur reading its vectors from R and G of the Source, scaled to a dozen pixels.
    NodePtr makeVectorBlur(const NodePtr& source)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_VECTORBLUR), PLUGIN_MAJOR_NATRON_VECTORBLUR);
        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        if (source) {
            connectNodes(source, node, kVectorBlurInputSource, true);
        }
        EXPECT_TRUE(setUVLayer(node, kNatronColorViewRGBA, { "R", "G" }));
        EXPECT_TRUE(setKnobValues(node, kVectorBlurParamScale, { 12. }));

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
        const ImageDiff diff = compareImages(result.cpu, result.gpu, tolerance());
        EXPECT_EQ(0u, diff.mismatches) << what << ": worst " << diff.worstAbsolute << ", first at float " << diff.firstMismatchIndex;
    }
};

TEST_F(NativeVectorBlurGpuTest, EligibilityFollowsTheSupportedSettings)
{
#ifdef NATRON_HAVE_GPU
    const bool supported = true;
#else
    const bool supported = false;
#endif
    resetProject();
    NodePtr source = makeSource();
    NodePtr node = makeVectorBlur(source);
    ASSERT_TRUE(source && node);
    StagedImageEffect* effect = effectOf(node);
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
    GpuQuery q = eligibleQuery();
    q.hasMask = true;
    q.mix = 0.5;
    EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << "mask and mix";
    ASSERT_TRUE(setKnobValue(node, kVectorBlurParamOcclusion, std::string(kVectorBlurOcclusionDepth)));
    ASSERT_TRUE(setDepthChannel(node, std::string(kParitySourceExtraLayerID) + ".B"));
    EXPECT_EQ(supported, effect->canRenderOnGpu(eligibleQuery())) << "depth-aware";
}

TEST_F(NativeVectorBlurGpuTest, FiltersAndOffsetsMatchTheCpuFrame)
{
    for (const char* filter : { kVectorBlurFilterBox, kVectorBlurFilterTriangle }) {
        for (double offset : { 0., 0.5, 1. }) {
            resetProject();
            NodePtr source = makeSource();
            NodePtr node = makeVectorBlur(source);
            ASSERT_TRUE(source && node);
            ASSERT_TRUE(setKnobValue(node, kVectorBlurParamFilter, std::string(filter)));
            ASSERT_TRUE(setKnobValues(node, kVectorBlurParamOffset, { offset }));
            expectParity(source, node, std::string(filter) + " offset " + std::to_string(offset));
        }
    }
}

// The vectors from a layer other than the one blurred reach the kernel as an extra input.
TEST_F(NativeVectorBlurGpuTest, VectorsFromAnotherLayerMatchTheCpuFrame)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr node = makeVectorBlur(source);
    ASSERT_TRUE(source && node);
    ASSERT_TRUE(setUVLayer(node, kParitySourceExtraLayerID, { "G", "B" }));
    expectParity(source, node, "spec vectors");
}

// Few samples on long vectors, clamped to a short Max Length, in normalised units.
TEST_F(NativeVectorBlurGpuTest, SamplesUnitsAndClampMatchTheCpuFrame)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr node = makeVectorBlur(source);
    ASSERT_TRUE(source && node);
    ASSERT_TRUE(setKnobValues(node, kVectorBlurParamScale, { 0.5 }));
    ASSERT_TRUE(setKnobValue(node, kVectorBlurParamVectorUnits, std::string(kVectorBlurUnitsNormalised)));
    ASSERT_TRUE(setKnobValues(node, kVectorBlurParamSamples, { 3. }));
    ASSERT_TRUE(setKnobValues(node, kVectorBlurParamMaxSamples, { 9. }));
    ASSERT_TRUE(setKnobValues(node, kVectorBlurParamMaxLength, { 10. }));
    expectParity(source, node, "samples, units and clamp");
}

TEST_F(NativeVectorBlurGpuTest, OcclusionMatchesTheCpuFrame)
{
    for (const char* units : { kVectorBlurDepthDistance, kVectorBlurDepthInverse }) {
        resetProject();
        NodePtr source = makeSource();
        NodePtr node = makeVectorBlur(source);
        ASSERT_TRUE(source && node);
        ASSERT_TRUE(setKnobValue(node, kVectorBlurParamOcclusion, std::string(kVectorBlurOcclusionDepth)));
        ASSERT_TRUE(setKnobValue(node, kVectorBlurParamDepthUnits, std::string(units)));
        ASSERT_TRUE(setDepthChannel(node, std::string(kParitySourceExtraLayerID) + ".B"));
        expectParity(source, node, std::string("occlusion ") + units);
    }
}

TEST_F(NativeVectorBlurGpuTest, MaskAndMixMatchTheCpuFrame)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr node = makeVectorBlur(source);
    ASSERT_TRUE(source && node);
    ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, { 0.5 }));
    expectParity(source, node, "mix");
}
