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

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <list>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "Native/NativeGpuParity.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/EdgeExtend.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

// The blurs are FIR Gaussians running the same float taps in the same order on both sides; the
// merges and the division round alike.
const ParityTolerance kTolerance = ParityTolerance::resampling();

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

class NativeEdgeExtendGpuTest
    : public BaseTest {
protected:
    // A 64x48 project format, the parity source's size.
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeEdgeExtendGpu64x48", 1.));
    }

    NodePtr makeEdgeExtend()
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_EDGEEXTEND), PLUGIN_MAJOR_NATRON_EDGEEXTEND);
        EXPECT_TRUE(bool(node));

        return node;
    }

    static StagedImageEffect* effectOf(const NodePtr& node)
    {
        return node ? dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get()) : NULL;
    }
};

TEST_F(NativeEdgeExtendGpuTest, EligibilityFollowsTheSupportedSettings)
{
#ifdef NATRON_HAVE_GPU
    const bool supported = true;
#else
    const bool supported = false;
#endif
    resetProject();
    NodePtr node = makeEdgeExtend();
    StagedImageEffect* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);

    // Below four channels the node passes the source through, on the GPU too.
    for (int nComps : { 1, 2, 3, 4 }) {
        GpuQuery q = eligibleQuery();
        q.nComps = nComps;
        EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << nComps << " components";
    }
    {
        GpuQuery q = eligibleQuery();
        q.processChannels[3] = false;
        EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << "a channel subset";
    }
    {
        GpuQuery q = eligibleQuery();
        q.hasMask = true;
        q.maskInPlane = true;
        EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << "mask";
    }
    for (double mix : { 0., 0.5 }) {
        GpuQuery q = eligibleQuery();
        q.mix = mix;
        EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << "mix " << mix;
    }
    for (ImageBitDepthEnum depth : { eImageBitDepthByte, eImageBitDepthShort, eImageBitDepthHalf }) {
        GpuQuery q = eligibleQuery();
        q.bitDepth = depth;
        EXPECT_FALSE(effect->canRenderOnGpu(q)) << "bit depth " << (int)depth;
    }
    for (int slices : { 1, 5, 50 }) {
        ASSERT_TRUE(setKnobValues(node, kEdgeExtendParamSlices, { (double)slices }));
        for (double size : { 0., 10., 100. }) {
            ASSERT_TRUE(setKnobValues(node, kEdgeExtendParamSize, { size }));
            EXPECT_EQ(supported, effect->canRenderOnGpu(eligibleQuery())) << slices << " slices, size " << size;
        }
    }

    // The FIR blur's tile reaches radius 512. One slice of size 1000 is sigma 416.7, a radius of
    // about 1250 at full scale and 313 at a quarter.
    ASSERT_TRUE(setKnobValues(node, kEdgeExtendParamSlices, { 1. }));
    ASSERT_TRUE(setKnobValues(node, kEdgeExtendParamSize, { 1000. }));
    EXPECT_FALSE(effect->canRenderOnGpu(eligibleQuery())) << "radius beyond the blur's tile";
    {
        GpuQuery q = eligibleQuery();
        q.mappedScale = RenderScale::fromMipmapLevel(2);
        EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << "the same size at a quarter scale";
    }
}

TEST_F(NativeEdgeExtendGpuTest, InputRegionIsTheSourceRegionOfInterest)
{
    resetProject();
    NodePtr node = makeEdgeExtend();
    StagedImageEffect* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);

    const RectI rect(10, 20, 31, 47);
    for (int slices : { 1, 3, 5 }) {
        ASSERT_TRUE(setKnobValues(node, kEdgeExtendParamSlices, { (double)slices }));
        ASSERT_TRUE(setKnobValues(node, kEdgeExtendParamSize, { 30. }));
        EdgeExtendParams params;
        params.sizeX = params.sizeY = 30.;
        params.slices = slices;
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            const RenderScale scale = RenderScale::fromMipmapLevel(mipmapLevel);
            const RectI roi = EdgeExtend::getSourceRoI(rect, scale, params);
            EXPECT_EQ(roi, effect->getGpuInputRoI(0, rect, scale)) << slices << " slices, mipmap " << mipmapLevel;
            EXPECT_TRUE(roi.contains(rect));
            EXPECT_NE(roi, rect);
        }
    }
}

#ifdef NATRON_HAVE_GPU

namespace {

bool
renderRGBA(const NodePtr& node,
           const RectI& window,
           RenderedPlane* plane)
{
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error);
    EXPECT_TRUE(ok) << error;
    if (!ok || (planes.size() != 1) || (planes[0].channels.size() != 4)) {
        return false;
    }
    *plane = planes[0];

    return true;
}

RectI
pixelRegionOfDefinition(const NodePtr& node)
{
    EffectInstancePtr effect = node->getEffectInstance();
    RectD rod;
    const StatusEnum stat = effect->getRegionOfDefinition(effect->getRenderHash(), kTime, RenderScale::identity, ViewIdx(0), &rod);
    EXPECT_NE(eStatusFailed, stat);

    return rod.toPixelEnclosing(0, 1.);
}

GpuViewLayout
outputLayout()
{
    GpuViewLayout layout;

    layout.padFloats = 4;
    layout.rowPadFloats = 4;
    layout.marginLeft = 2;
    layout.marginBottom = 1;
    layout.marginRight = 3;
    layout.marginTop = 2;

    return layout;
}

// The RGBA channels a GPU render of nComps components holds: alpha alone, red and green, or RGB.
std::vector<int>
planeChannels(int nComps)
{
    switch (nComps) {
    case 1:
        return { 3 };
    case 2:
        return { 0, 1 };
    case 3:
        return { 0, 1, 2 };
    default:
        return { 0, 1, 2, 3 };
    }
}

ImageLayerDesc
planeOf(int nComps)
{
    switch (nComps) {
    case 1:
        return ImageLayerDesc::getAlphaComponents();
    case 2:
        return ImageLayerDesc::getXYComponents();
    case 3:
        return ImageLayerDesc::getRGBComponents();
    default:
        return ImageLayerDesc::getRGBAComponents();
    }
}

// The channels of window, inside bounds, of an RGBA image over bounds.
std::vector<float>
pickChannels(const std::vector<float>& rgba,
             const RectI& bounds,
             const RectI& window,
             const std::vector<int>& channels)
{
    std::vector<float> out;

    out.reserve((std::size_t)window.area() * channels.size());
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            const std::size_t at = ((std::size_t)(y - bounds.y1) * bounds.width() + (std::size_t)(x - bounds.x1)) * 4;
            for (int c : channels) {
                out.push_back(rgba[at + c]);
            }
        }
    }

    return out;
}

// One node setting. processed is the channel set in RGBA bits; below four components the CPU
// processes the plane's channels and only those are compared.
struct ParityCase {
    double size;
    int slices;
    bool premultiply;
    bool unpremultiply;
    int nComps;
    unsigned processed;
    bool tile;
};

unsigned
channelsOfPlane(int nComps)
{
    unsigned bits = 0;

    for (int c : planeChannels(nComps)) {
        bits |= 1u << c;
    }

    return bits;
}

// Every combination of the premultiplications on RGBA at the default size and slices, each also
// rendered as a tile; one slice, many slices, a size whose blurs are the identity and a large
// one; one, two and three components and an RGB subset on RGBA, all passed through.
std::vector<ParityCase>
parityCases()
{
    std::vector<ParityCase> cases;

    for (bool premultiply : { true, false }) {
        for (bool unpremultiply : { true, false }) {
            cases.push_back(ParityCase { 10., 5, premultiply, unpremultiply, 4, 0xFu, true });
        }
    }
    cases.push_back(ParityCase { 6., 1, true, true, 4, 0xFu, false });
    cases.push_back(ParityCase { 20., 12, true, true, 4, 0xFu, true });
    cases.push_back(ParityCase { 0.1, 5, true, true, 4, 0xFu, false });
    cases.push_back(ParityCase { 40., 3, false, true, 4, 0xFu, false });
    for (int nComps = 1; nComps <= 3; ++nComps) {
        cases.push_back(ParityCase { 10., 5, true, true, nComps, channelsOfPlane(nComps), false });
    }
    cases.push_back(ParityCase { 10., 5, true, true, 4, 0x7u, false });

    return cases;
}

std::string
describeCase(const ParityCase& c,
             const RectI& roi)
{
    std::ostringstream s;
    s << "size " << c.size << " slices " << c.slices << " premultiply " << c.premultiply << " unpremultiply " << c.unpremultiply
      << " components " << c.nComps << " channels 0x" << std::hex << c.processed << std::dec
      << " roi (" << roi.x1 << "," << roi.y1 << ")-(" << roi.x2 << "," << roi.y2 << ")";

    return s.str();
}

std::vector<std::string>
channelNames(unsigned processed)
{
    const char* const names[] = { "R", "G", "B", "A" };
    std::vector<std::string> out;

    for (int c = 0; c < 4; ++c) {
        if (processed & (1u << c)) {
            out.push_back(names[c]);
        }
    }

    return out;
}

} // namespace

// The CPU render of the node over its region of definition against renderGpu() fed with the
// noisy parity source over getGpuInputRoI(), over that region and over a tile inside it, which is
// compared with the same pixels of the full-frame CPU render. The source sits off the format's
// origin so the buffer's edges fall inside the slices' reach and the nearest-pixel boundary is
// exercised on both sides.
TEST_F(NativeEdgeExtendGpuTest, MatchesTheCpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 3, 5);
    setParitySourceNoise(source, true);

    std::size_t nCompared = 0;
    int nRenders = 0;
    for (const ParityCase& c : parityCases()) {
        // A fresh node per setting, so no region of definition cached under an earlier setting is
        // reused.
        NodePtr node = makeEdgeExtend();
        StagedImageEffect* effect = effectOf(node);
        ASSERT_TRUE(effect != NULL);
        connectNodes(source, node, 0, true);
        ASSERT_TRUE(setKnobValues(node, kEdgeExtendParamSize, { c.size }));
        ASSERT_TRUE(setKnobValues(node, kEdgeExtendParamSlices, { (double)c.slices }));
        ASSERT_TRUE(setKnobValues(node, kEdgeExtendParamPremultiplyInput, { c.premultiply ? 1. : 0. }));
        ASSERT_TRUE(setKnobValues(node, kEdgeExtendParamUnpremultiplyOutput, { c.unpremultiply ? 1. : 0. }));
        KnobChannelSetPtr channelSet = std::dynamic_pointer_cast<KnobChannelSet>(node->getKnobByName(kNodeParamChannelSet));
        ASSERT_TRUE(bool(channelSet));
        channelSet->setChannels(0, channelNames(c.processed));

        const RectI rod = pixelRegionOfDefinition(node);
        ASSERT_FALSE(rod.isNull());
        RenderedPlane cpu;
        ASSERT_TRUE(renderRGBA(node, rod, &cpu)) << describeCase(c, rod);
        ASSERT_EQ(rod, cpu.window) << describeCase(c, rod);

        GpuQuery query = eligibleQuery();
        query.nComps = c.nComps;
        query.plane = planeOf(c.nComps);
        if (c.nComps == 4) {
            for (int i = 0; i < 4; ++i) {
                query.processChannels[i] = (c.processed & (1u << i)) != 0;
            }
        }
        ASSERT_TRUE(effect->canRenderOnGpu(query)) << describeCase(c, rod);

        std::vector<RectI> rois;
        rois.push_back(rod);
        if (c.tile) {
            rois.push_back(RectI(rod.x1 + 9, rod.y1 + 7, std::min(rod.x2 - 3, rod.x1 + 9 + 29), std::min(rod.y2 - 2, rod.y1 + 7 + 21)));
        }
        const std::vector<int> channels = planeChannels(c.nComps);
        for (const RectI& roi : rois) {
            const std::string what = describeCase(c, roi);
            const RectI inputRoI = effect->getGpuInputRoI(0, roi, RenderScale::identity);
            RenderedPlane in;
            ASSERT_TRUE(renderRGBA(source, inputRoI, &in)) << what;
            ASSERT_EQ(inputRoI, in.window) << what;
            GpuParityImage image;
            image.bounds = inputRoI;
            image.nComps = c.nComps;
            image.pixels = pickChannels(in.pixels, inputRoI, inputRoI, channels);
            image.layout.padFloats = 6;
            image.layout.rowPadFloats = 8;

            const GpuRunResult run = runRenderGpu(*runtime, *effect, query, std::vector<GpuParityImage>(1, image), nullptr, roi, outputLayout());
            ASSERT_TRUE(run.ok) << what;
            EXPECT_EQ(0u, run.writtenOutside) << what << ": floats outside the window were written";
            ++nRenders;

            const std::vector<float> want = pickChannels(cpu.pixels, rod, roi, channels);
            ASSERT_EQ(want.size(), run.pixels.size()) << what;
            const ImageDiff diff = compareImages(want, run.pixels, kTolerance);
            nCompared += diff.compared;
            const std::size_t at = diff.worstIndex / c.nComps;
            EXPECT_EQ(0u, diff.mismatches) << what << ": " << diff.mismatches << " mismatches, worst " << diff.worstAbsolute << " at (" << (roi.x1 + (int)(at % roi.width())) << ","
                                           << (roi.y1 + (int)(at / roi.width())) << ") channel " << (diff.worstIndex % c.nComps) << " gpu " << run.pixels[diff.worstIndex] << " cpu " << want[diff.worstIndex];
        }
    }
    EXPECT_GT(nCompared, 0u);
    std::printf("edgeextend gpu vs cpu: %d renders, %zu floats compared\n", nRenders, nCompared);
}

#endif // NATRON_HAVE_GPU
