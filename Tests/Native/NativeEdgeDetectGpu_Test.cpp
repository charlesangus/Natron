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
#include "Engine/Nodes/Filter/EdgeDetect.h"
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

const char* const kFilters[] = {
    kEdgeDetectParamFilterSimple,
    kEdgeDetectParamFilterSobel,
    kEdgeDetectParamFilterRotationInvariant,
    kEdgeDetectParamFilterQuasiGaussian,
    kEdgeDetectParamFilterGaussian,
    kEdgeDetectParamFilterBox,
    kEdgeDetectParamFilterTriangle,
    kEdgeDetectParamFilterQuadratic,
};
const int kNFilters = 8;

const char* const kModes[] = {
    kEdgeDetectParamMultiChannelSeparate,
    kEdgeDetectParamMultiChannelRMS,
    kEdgeDetectParamMultiChannelMax,
    kEdgeDetectParamMultiChannelTensor,
};
const int kNModes = 4;

// No erosion, an erosion and a dilation, of radius 2.
const double kErodeSizes[] = { 0., 2., -2. };

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

class NativeEdgeDetectGpuTest
    : public BaseTest {
protected:
    // A 64x48 project format, the parity source's size.
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeEdgeDetectGpu64x48", 1.));
    }

    NodePtr makeEdgeDetect()
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_EDGEDETECT), PLUGIN_MAJOR_NATRON_EDGEDETECT);
        EXPECT_TRUE(bool(node));

        return node;
    }

    static StagedImageEffect* effectOf(const NodePtr& node)
    {
        return node ? dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get()) : NULL;
    }
};

TEST_F(NativeEdgeDetectGpuTest, EligibilityFollowsTheSupportedSettings)
{
#ifdef NATRON_HAVE_GPU
    const bool supported = true;
#else
    const bool supported = false;
#endif
    resetProject();
    NodePtr node = makeEdgeDetect();
    StagedImageEffect* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
    ASSERT_TRUE(setKnobValues(node, kEdgeDetectParamErodeSize, { 2. }));
    ASSERT_TRUE(setKnobValues(node, kEdgeDetectParamNMS, { 1. }));

    for (int nComps : { 1, 2, 3, 4 }) {
        GpuQuery q = eligibleQuery();
        q.nComps = nComps;
        EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << nComps << " components";
    }
    {
        GpuQuery q = eligibleQuery();
        q.processChannels[1] = false;
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
    for (const char* filter : kFilters) {
        ASSERT_TRUE(setKnobValue(node, kEdgeDetectParamFilter, std::string(filter)));
        for (double blurSize : { 0., 5., 1000. }) {
            ASSERT_TRUE(setKnobValues(node, kEdgeDetectParamBlurSize, { blurSize }));
            EXPECT_EQ(supported, effect->canRenderOnGpu(eligibleQuery())) << filter << " blur size " << blurSize;
        }
    }

    // An erosion radius past the morphology kernel's 1024, which the half scale brings back under it.
    for (double erodeSize : { 1500., -1500. }) {
        ASSERT_TRUE(setKnobValues(node, kEdgeDetectParamErodeSize, { erodeSize }));
        EXPECT_FALSE(effect->canRenderOnGpu(eligibleQuery())) << "erode size " << erodeSize;
        GpuQuery q = eligibleQuery();
        q.mappedScale = RenderScale::fromMipmapLevel(1);
        EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << "erode size " << erodeSize << " at half scale";
    }
}

TEST_F(NativeEdgeDetectGpuTest, InputRegionIsTheSourceRegionOfInterest)
{
    resetProject();
    NodePtr node = makeEdgeDetect();
    StagedImageEffect* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
    ASSERT_TRUE(setKnobValue(node, kEdgeDetectParamFilter, std::string(kEdgeDetectParamFilterSobel)));
    ASSERT_TRUE(setKnobValues(node, kEdgeDetectParamBlurSize, { 10. }));
    ASSERT_TRUE(setKnobValues(node, kEdgeDetectParamErodeSize, { -2. }));
    ASSERT_TRUE(setKnobValues(node, kEdgeDetectParamNMS, { 1. }));

    EdgeDetectParams params;
    params.filter = eEdgeDetectFilterSobel;
    params.sizeX = params.sizeY = 10.;
    params.erodeSize = -2.;
    params.nms = true;
    const RectI rect(10, 20, 31, 47);
    for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
        const RenderScale scale = RenderScale::fromMipmapLevel(mipmapLevel);
        EXPECT_EQ(EdgeDetect::getSourceRoI(rect, scale, params), effect->getGpuInputRoI(0, rect, scale)) << "mipmap " << mipmapLevel;
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
    int filter;
    int mode;
    bool nms;
    double erodeSize;
    double blurSize;
    int nComps;
    unsigned processed;
    bool expandRoD;
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

// The full product of filter, mode and suppression would be repeated for every component count
// and erosion; instead each axis takes every value and the combinations that differ in code are
// all present:
// - every filter, mode and suppression setting on RGBA at blur size 0, the erosion cycling so that
//   each filter meets each erosion with suppression on and off, each also rendered as a tile;
// - one, two and three components, every filter and mode, suppression alternating;
// - the default RGB channel set and an R+B subset on RGBA, which leave channels out of the
//   combination, for every mode with suppression on and off;
// - blur size 5, which runs the pre-blur and the smoothing derivative blurs: every filter and mode
//   on RGBA without suppression, and with it on RGB, whose noise leaves no ties for it to flip on;
// - an expanded region of definition, where the buffer reads the source's nearest pixel.
// The recursive (quasi-)Gaussian derivatives are held to the IIR tolerance, and suppression is a
// discontinuous decision on them, so they meet suppression only where the noise leaves no ties:
// never after an erosion or dilation, whose flat plateaus are ties, nor beside a nearest-pixel
// extension, whose repeated columns are. The finite and box derivatives match bit for bit and
// cover suppression after both.
std::vector<ParityCase>
parityCases()
{
    std::vector<ParityCase> cases;
    const auto erosion = [](int f, bool nms, int index) {
        const bool recursive = (f == 3) || (f == 4);

        return (nms && recursive) ? 0. : kErodeSizes[index % 3];
    };

    for (int f = 0; f < kNFilters; ++f) {
        for (int m = 0; m < kNModes; ++m) {
            for (bool nms : { false, true }) {
                cases.push_back(ParityCase { f, m, nms, erosion(f, nms, f + m), 0., 4, 0xFu, false, true });
            }
        }
    }
    for (int nComps = 1; nComps <= 3; ++nComps) {
        for (int f = 0; f < kNFilters; ++f) {
            for (int m = 0; m < kNModes; ++m) {
                const bool nms = ((f + m) % 2) != 0;
                cases.push_back(ParityCase { f, m, nms, erosion(f, nms, f + m + nComps), 0., nComps, channelsOfPlane(nComps), false, false });
            }
        }
    }
    for (unsigned processed : { 0x7u, 0x5u }) {
        for (int m = 0; m < kNModes; ++m) {
            for (bool nms : { false, true }) {
                const int f = (2 * m + (nms ? 1 : 0) + (int)processed) % kNFilters;
                cases.push_back(ParityCase { f, m, nms, erosion(f, nms, m + (nms ? 1 : 0)), 0., 4, processed, false, false });
            }
        }
    }
    for (int f = 0; f < kNFilters; ++f) {
        for (int m = 0; m < kNModes; ++m) {
            cases.push_back(ParityCase { f, m, false, kErodeSizes[(f + m) % 3], 5., 4, 0xFu, false, false });
        }
        for (int m : { 0, 3 }) {
            cases.push_back(ParityCase { f, m, true, 0., 5., 3, channelsOfPlane(3), false, false });
        }
    }
    for (int f : { 0, 4, 5 }) {
        cases.push_back(ParityCase { f, 3, f != 4, 2., 0., 4, 0xFu, true, true });
    }

    return cases;
}

std::string
describeCase(const ParityCase& c,
             const RectI& roi)
{
    std::ostringstream s;
    s << kFilters[c.filter] << " " << kModes[c.mode] << (c.nms ? " nms" : "") << " erode " << c.erodeSize << " blur " << c.blurSize
      << " components " << c.nComps << " channels 0x" << std::hex << c.processed << std::dec << (c.expandRoD ? " expandRoD" : "")
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
// origin so the buffer's edges fall inside the filtered extent.
TEST_F(NativeEdgeDetectGpuTest, MatchesTheCpuRender)
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

    const ParityTolerance tolerance = ParityTolerance::iir();
    std::size_t nCompared = 0;
    int nRenders = 0;
    for (const ParityCase& c : parityCases()) {
        // A fresh node per setting, so no region of definition cached under an earlier setting is
        // reused.
        NodePtr node = makeEdgeDetect();
        StagedImageEffect* effect = effectOf(node);
        ASSERT_TRUE(effect != NULL);
        connectNodes(source, node, 0, true);
        ASSERT_TRUE(setKnobValue(node, kEdgeDetectParamFilter, std::string(kFilters[c.filter])));
        ASSERT_TRUE(setKnobValue(node, kEdgeDetectParamMultiChannel, std::string(kModes[c.mode])));
        ASSERT_TRUE(setKnobValues(node, kEdgeDetectParamBlurSize, { c.blurSize }));
        ASSERT_TRUE(setKnobValues(node, kEdgeDetectParamErodeSize, { c.erodeSize }));
        ASSERT_TRUE(setKnobValues(node, kEdgeDetectParamNMS, { c.nms ? 1. : 0. }));
        ASSERT_TRUE(setKnobValues(node, kEdgeDetectParamExpandRoD, { c.expandRoD ? 1. : 0. }));
        ASSERT_TRUE(setKnobValues(node, kEdgeDetectParamCropToFormat, { 0. }));
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
            const ImageDiff diff = compareImages(want, run.pixels, tolerance);
            nCompared += diff.compared;
            const std::size_t at = diff.worstIndex / c.nComps;
            EXPECT_EQ(0u, diff.mismatches) << what << ": " << diff.mismatches << " mismatches, worst " << diff.worstAbsolute << " at (" << (roi.x1 + (int)(at % roi.width())) << ","
                                           << (roi.y1 + (int)(at / roi.width())) << ") channel " << (diff.worstIndex % c.nComps) << " gpu " << run.pixels[diff.worstIndex] << " cpu " << want[diff.worstIndex];
        }
    }
    EXPECT_GT(nCompared, 0u);
    std::printf("edgedetect gpu vs cpu: %d renders, %zu floats compared\n", nRenders, nCompared);
}

#endif // NATRON_HAVE_GPU
